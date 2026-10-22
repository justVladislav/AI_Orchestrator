#include "ai/providers/RealProviderAgentSessionFactory.hpp"
#include "ai/providers/AnthropicAgentSession.hpp"
#include "ai/providers/OpenAIAgentSession.hpp"
#include "ai/http/HttpClient.hpp"

#include <memory>
#include <utility>

namespace ai::providers {

RealProviderAgentSessionFactory::RealProviderAgentSessionFactory(
    application::ICredentialStore& credentials,
    application::IProviderRepository& providers)
    : credentials_(credentials)
    , providers_(providers)
{
}

common::Result<std::unique_ptr<IProviderAgentSession>>
RealProviderAgentSessionFactory::createSession(
    const application::ProviderId& providerId,
    const std::string& providerKind)
{
    using Result = common::Result<std::unique_ptr<IProviderAgentSession>>;

    if (providerKind != "openai" && providerKind != "anthropic")
        return Result::failure(
            {"provider.unsupported_kind",
             "No agent session adapter exists for this provider kind"});

    const auto found = providers_.findById(providerId);
    if (!found.success())
        return Result::failure(*found.error());
    if (!found.value().has_value() || !found.value()->credentialReference.has_value())
        return Result::failure(
            {"credential.missing",
             "No credential is configured for this provider"});

    const auto loaded = credentials_.load(*found.value()->credentialReference);
    if (!loaded.success())
        return Result::failure(*loaded.error());
    if (!loaded.value().has_value())
        return Result::failure(
            {"credential.missing",
             "No credential is configured for this provider"});

    const std::string& apiKey = *loaded.value();
    auto httpClient = std::make_shared<http::HttpClient>();

    if (providerKind == "openai") {
        return Result::ok(std::make_unique<OpenAIAgentSession>(
            apiKey, OpenAIAgentSession::kDefaultEndpoint, std::move(httpClient)));
    }

    return Result::ok(std::make_unique<AnthropicAgentSession>(
        apiKey, AnthropicAgentSession::kDefaultEndpoint, std::move(httpClient)));
}

} // namespace ai::providers
