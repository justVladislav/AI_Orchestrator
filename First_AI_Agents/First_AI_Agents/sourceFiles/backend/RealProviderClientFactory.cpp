#include "ai/backend/RealProviderClientFactory.hpp"
#include "ai/backend/ProviderClientTranslation.hpp"

#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/models/ModelResponse.hpp"
#include "ai/providers/AnthropicProvider.hpp"
#include "ai/providers/OpenAIProvider.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

namespace ai::backend {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

// Both provider kinds are served by ONE client, provider_client_detail::GenerateProviderClient (ProviderClientTranslation.hpp):
// the policy gate, the cancellation points, the cancelled-response capture and the send() view are shared and tested there
// with a fake callable. Only the callable below differs; it OWNS its provider (shared, so the client can never hold a dangling
// reference to it) and performs the one blocking legacy call.
provider_client_detail::GenerateFn openAIGenerate(std::string apiKey)
{
    auto provider = std::make_shared<providers::OpenAIProvider>(std::move(apiKey), providers::ModelTypeUnknown);
    return [provider](const models::ModelRequest& modelRequest) {
        auto detailed = provider->generateDetailed(modelRequest);
        return std::make_pair(std::move(detailed.response), std::move(detailed.errorCode));
    };
}

provider_client_detail::GenerateFn anthropicGenerate(std::string apiKey)
{
    auto provider = std::make_shared<providers::AnthropicProvider>(std::move(apiKey), providers::ModelTypeUnknown);
    return [provider](const models::ModelRequest& modelRequest) {
        // Anthropic supplies no structured code: the empty code selects the text fallback.
        return std::make_pair(provider->generate(modelRequest), std::string {});
    };
}

} // namespace

RealProviderClientFactory::RealProviderClientFactory(
    application::ICredentialStore& credentials,
    application::IProviderRepository& providers)
    : credentials_(credentials)
    , providers_(providers)
{
}

common::Result<std::shared_ptr<application::IProviderClient>> RealProviderClientFactory::clientFor(
    const application::ProviderId& providerId,
    const std::string& providerKind)
{
    if (providerKind != "openai" && providerKind != "anthropic") {
        // "openai_compatible" and "local" are not yet implemented by this
        // factory - reported the same way an unlinked real-provider build
        // would be, rather than silently falling back to anything.
        return common::Result<std::shared_ptr<application::IProviderClient>>::failure(
            error("provider.unsupported_kind", "No real client adapter exists yet for this provider kind"));
    }

    const auto found = providers_.findById(providerId);
    if (!found.success()) {
        return common::Result<std::shared_ptr<application::IProviderClient>>::failure(*found.error());
    }
    if (!found.value().has_value() || !found.value()->credentialReference.has_value()) {
        return common::Result<std::shared_ptr<application::IProviderClient>>::failure(
            error("credential.missing", "No credential is configured for this provider"));
    }

    const auto loaded = credentials_.load(*found.value()->credentialReference);
    if (!loaded.success()) {
        return common::Result<std::shared_ptr<application::IProviderClient>>::failure(*loaded.error());
    }
    if (!loaded.value().has_value()) {
        return common::Result<std::shared_ptr<application::IProviderClient>>::failure(
            error("credential.missing", "No credential is configured for this provider"));
    }

    std::shared_ptr<application::IProviderClient> client;
    if (providerKind == "openai") {
        client = std::make_shared<provider_client_detail::GenerateProviderClient>(
            "openai", openAIGenerate(*loaded.value()));
    } else {
        client = std::make_shared<provider_client_detail::GenerateProviderClient>(
            "anthropic", anthropicGenerate(*loaded.value()));
    }

    return common::Result<std::shared_ptr<application::IProviderClient>>::ok(std::move(client));
}

} // namespace ai::backend
