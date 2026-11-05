#pragma once

#include "ai/application/providers/IProviderClient.hpp"
#include "ai/common/Result.hpp"
#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/models/ModelResponse.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace ai::backend::provider_client_detail {

[[nodiscard]] inline common::Result<void> validatePromptCachePolicy(
    const std::string& providerKind,
    models::PromptCachePolicy policy)
{
    if (!models::isKnown(policy.mode)) {
        return common::Result<void>::failure(
            {"provider.invalid_prompt_cache_policy", "The prompt-cache policy is unknown"});
    }
    if (policy.enabled() && providerKind != "anthropic") {
        return common::Result<void>::failure(
            {"provider.prompt_cache_unsupported",
             "This provider client does not support the requested prompt-cache policy"});
    }
    return common::Result<void>::ok();
}

[[nodiscard]] inline application::ProviderChatResponse toChatResponse(
    const models::ModelResponse& response)
{
    application::ProviderChatResponse result;
    result.content = response.text;
    result.usage = response.usage;
    return result;
}

// The text fallback for failures that carry no structured code (Anthropic, transport errors, an unusable body, the
// missing key): a best-effort classification into the stable codes IProviderClient::send() promises. It is NOT used
// for any failure whose provider already supplied a code.
[[nodiscard]] inline std::string classifyLegacyProviderMessage(const std::string& message)
{
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
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

// Typed provider result -> IProviderClient result. A success carries no code. On failure a NON-EMPTY structured code
// wins over every word in the text; only an empty code falls back to the text classification above.
[[nodiscard]] inline common::Result<application::ProviderChatResponse> toChatResult(
    const models::ModelResponse& response, const std::string& structuredErrorCode)
{
    if (response.success) {
        return common::Result<application::ProviderChatResponse>::ok(toChatResponse(response));
    }
    return common::Result<application::ProviderChatResponse>::failure(
        {structuredErrorCode.empty() ? classifyLegacyProviderMessage(response.errorMessage) : structuredErrorCode,
         response.errorMessage});
}

[[nodiscard]] inline models::ModelRequest toModelRequest(
    const application::ProviderChatRequest& request,
    std::stop_token stopToken)
{
    models::ModelRequest modelRequest;
    if (!request.systemPrompt.empty()) {
        modelRequest.messages.push_back({models::MessageRole::System, request.systemPrompt});
    }
    modelRequest.messages.push_back({models::MessageRole::User, request.userContent});
    modelRequest.options.model = request.apiModelId;
    modelRequest.options.maxOutputTokens = request.maxOutputTokens;
    modelRequest.options.temperature = request.temperature;
    modelRequest.options.timeouts.disableTotalTimeout = request.disableTotalTimeout;
    if (!request.disableTotalTimeout) {
        modelRequest.options.timeouts.totalTimeout = request.timeout;
    }
    modelRequest.options.cancellationRequested = [stopToken] {
        return stopToken.stop_requested();
    };
    modelRequest.options.promptCache = request.promptCache;
    return modelRequest;
}

// The blocking legacy provider call as the common adapter sees it: the request it builds in, the response and the
// structured failure code (empty = none) out. The provider that performs it is owned by the callable.
using GenerateFn = std::function<std::pair<models::ModelResponse, std::string>(const models::ModelRequest&)>;

// ONE provider call, with the cancellation points IProviderClient::send() has always had: a stop before the request prevents
// it (the callable is not run); a stop seen after the call turns the outcome into `provider.cancelled`. NEW: when that
// call had produced a complete SUCCESSFUL response, it is returned as `discardedResponse` instead of being lost. A failed
// ModelResponse is never captured, so a stop during a failing or aborted call carries no payload.
[[nodiscard]] inline application::ProviderChatOutcome runObservedProviderCall(
    const GenerateFn& generate,
    const application::ProviderChatRequest& request,
    std::stop_token stopToken)
{
    using ChatResult = common::Result<application::ProviderChatResponse>;
    if (stopToken.stop_requested()) {
        return application::ProviderChatOutcome {
            ChatResult::failure({"provider.cancelled", "Cancelled before the request was sent"}), std::nullopt};
    }

    // ONE request: the callable performs it once and yields the response plus its structured failure code.
    const auto outcome = generate(toModelRequest(request, stopToken));

    if (stopToken.stop_requested()) {
        application::ProviderChatOutcome cancelled {
            ChatResult::failure({"provider.cancelled", "Cancelled while the request was in flight"}), std::nullopt};
        if (outcome.first.success) cancelled.discardedResponse = toChatResponse(outcome.first);
        return cancelled;
    }
    return application::ProviderChatOutcome {toChatResult(outcome.first, outcome.second), std::nullopt};
}

// The ONE production IProviderClient for every real provider kind (the factory builds it for both OpenAI and Anthropic;
// only the kind string and the callable differ). sendObserved() is the real implementation; send() is a view of the
// result of that same single call, so a legacy caller sees exactly the result it always saw and no second call is made.
// The prompt-cache policy is validated first: its typed error outranks a cancellation, as before.
class GenerateProviderClient final : public application::IProviderClient {
public:
    GenerateProviderClient(std::string providerKind, GenerateFn generate)
        : providerKind_(std::move(providerKind)), generate_(std::move(generate))
    {
    }

    common::Result<application::ProviderChatResponse> send(
        const application::ProviderChatRequest& request, std::stop_token stopToken) override
    {
        return sendObserved(request, stopToken).result;
    }

    application::ProviderChatOutcome sendObserved(
        const application::ProviderChatRequest& request, std::stop_token stopToken) override
    {
        const auto policy = validatePromptCachePolicy(providerKind_, request.promptCache);
        if (!policy.success()) {
            return application::ProviderChatOutcome {
                common::Result<application::ProviderChatResponse>::failure(*policy.error()), std::nullopt};
        }
        return runObservedProviderCall(generate_, request, stopToken);
    }

private:
    std::string providerKind_;
    GenerateFn generate_;
};

} // namespace ai::backend::provider_client_detail
