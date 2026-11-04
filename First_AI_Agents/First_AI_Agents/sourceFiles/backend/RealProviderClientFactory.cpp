#include "ai/backend/RealProviderClientFactory.hpp"
#include "ai/backend/ProviderClientTranslation.hpp"

#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/models/ModelResponse.hpp"
#include "ai/providers/AnthropicProvider.hpp"
#include "ai/providers/OpenAIProvider.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ai::backend {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

// Shared by both provider adapters below: run the blocking legacy generate()
// call (as a callable returning the response and its structured code), then translate its result/failure into the
// ProviderChatResponse/Result<...> shape IProviderClient::send() promises. stopToken is checked
// before and after the (currently non-interruptible, since the legacy
// providers do not accept one) blocking call so a cancellation requested
// just before or immediately after the call is still observed promptly
// rather than only on the next stage.
template <typename Generate>
common::Result<application::ProviderChatResponse> runProviderCall(
    Generate&& generate,
    const application::ProviderChatRequest& request,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return common::Result<application::ProviderChatResponse>::failure(
            error("provider.cancelled", "Cancelled before the request was sent"));
    }

    // ONE request: the callable performs it once and yields the response plus its structured failure code.
    const auto outcome = generate(provider_client_detail::toModelRequest(request, stopToken));

    if (stopToken.stop_requested()) {
        return common::Result<application::ProviderChatResponse>::failure(
            error("provider.cancelled", "Cancelled while the request was in flight"));
    }
    return provider_client_detail::toChatResult(outcome.first, outcome.second);
}

class OpenAIProviderClientAdapter final : public application::IProviderClient {
public:
    explicit OpenAIProviderClientAdapter(std::string apiKey)
        : provider_(std::move(apiKey), providers::ModelTypeUnknown)
    {
    }

    common::Result<application::ProviderChatResponse> send(
        const application::ProviderChatRequest& request, std::stop_token stopToken) override
    {
        const auto policy = provider_client_detail::validatePromptCachePolicy(
            "openai", request.promptCache);
        if (!policy.success())
            return common::Result<application::ProviderChatResponse>::failure(*policy.error());
        return runProviderCall([this](const models::ModelRequest& modelRequest) {
            auto detailed = provider_.generateDetailed(modelRequest);
            return std::make_pair(std::move(detailed.response), std::move(detailed.errorCode));
        }, request, stopToken);
    }

private:
    providers::OpenAIProvider provider_;
};

class AnthropicProviderClientAdapter final : public application::IProviderClient {
public:
    explicit AnthropicProviderClientAdapter(std::string apiKey)
        : provider_(std::move(apiKey), providers::ModelTypeUnknown)
    {
    }

    common::Result<application::ProviderChatResponse> send(
        const application::ProviderChatRequest& request, std::stop_token stopToken) override
    {
        const auto policy = provider_client_detail::validatePromptCachePolicy(
            "anthropic", request.promptCache);
        if (!policy.success())
            return common::Result<application::ProviderChatResponse>::failure(*policy.error());
        return runProviderCall([this](const models::ModelRequest& modelRequest) {
            // Anthropic supplies no structured code: the empty code selects the text fallback.
            return std::make_pair(provider_.generate(modelRequest), std::string {});
        }, request, stopToken);
    }

private:
    providers::AnthropicProvider provider_;
};

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
        client = std::make_shared<OpenAIProviderClientAdapter>(*loaded.value());
    } else {
        client = std::make_shared<AnthropicProviderClientAdapter>(*loaded.value());
    }

    return common::Result<std::shared_ptr<application::IProviderClient>>::ok(std::move(client));
}

} // namespace ai::backend
