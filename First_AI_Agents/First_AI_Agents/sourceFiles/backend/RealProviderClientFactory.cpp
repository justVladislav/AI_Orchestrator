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

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// The legacy providers only give a human-readable errorMessage, never a
// structured code, and never include the API key in it (see
// OpenAIProvider.cpp/AnthropicProvider.cpp's own secret-handling
// guarantees) - this is a best-effort text classification into the stable
// codes IProviderClient::send() promises, not a guarantee every provider
// failure mode is covered.
std::string classifyProviderErrorCode(const std::string& message)
{
    const std::string lower = toLower(message);
    if (lower.find("401") != std::string::npos || lower.find("unauthorized") != std::string::npos
            || lower.find("invalid api key") != std::string::npos
            || lower.find("authentication") != std::string::npos) {
        return "provider.auth_failed";
    }
    if (lower.find("429") != std::string::npos || lower.find("rate limit") != std::string::npos) {
        return "provider.rate_limited";
    }
    if (lower.find("timed out") != std::string::npos || lower.find("timeout") != std::string::npos) {
        return "provider.timeout";
    }
    if (lower.find("curl") != std::string::npos || lower.find("could not resolve") != std::string::npos
            || lower.find("connect") != std::string::npos || lower.find("network") != std::string::npos) {
        return "provider.network_error";
    }
    if (lower.find("json") != std::string::npos || lower.find("parse") != std::string::npos
            || lower.find("unexpected response") != std::string::npos) {
        return "provider.invalid_response";
    }
    return "provider.request_failed";
}

// Shared by both provider adapters below: run the blocking legacy generate()
// call, then translate its result/failure into the ProviderChatResponse/
// Result<...> shape IProviderClient::send() promises. stopToken is checked
// before and after the (currently non-interruptible, since the legacy
// providers do not accept one) blocking call so a cancellation requested
// just before or immediately after the call is still observed promptly
// rather than only on the next stage.
common::Result<application::ProviderChatResponse> runProviderCall(
    providers::IModelProvider& provider,
    const application::ProviderChatRequest& request,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return common::Result<application::ProviderChatResponse>::failure(
            error("provider.cancelled", "Cancelled before the request was sent"));
    }

    const models::ModelResponse response = provider.generate(
        provider_client_detail::toModelRequest(request, stopToken));

    if (stopToken.stop_requested()) {
        return common::Result<application::ProviderChatResponse>::failure(
            error("provider.cancelled", "Cancelled while the request was in flight"));
    }
    if (!response.success) {
        return common::Result<application::ProviderChatResponse>::failure(
            error(classifyProviderErrorCode(response.errorMessage), response.errorMessage));
    }
    return common::Result<application::ProviderChatResponse>::ok(
        provider_client_detail::toChatResponse(response));
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
        return runProviderCall(provider_, request, stopToken);
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
        return runProviderCall(provider_, request, stopToken);
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
