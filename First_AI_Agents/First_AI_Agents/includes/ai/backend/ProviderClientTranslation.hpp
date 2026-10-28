#pragma once

#include "ai/application/providers/IProviderClient.hpp"
#include "ai/common/Result.hpp"
#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/models/ModelResponse.hpp"

#include <stop_token>
#include <string>

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

} // namespace ai::backend::provider_client_detail
