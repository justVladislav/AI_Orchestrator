#include "ai/backend/ProviderClientTranslation.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void testRequestTranslationPreservesTheTypedPolicy()
{
    ai::application::ProviderChatRequest request;
    request.apiModelId = "claude-test";
    request.systemPrompt = "stable";
    request.userContent = "moving";
    request.timeout = std::chrono::milliseconds {1234};
    request.maxOutputTokens = 321;
    request.temperature = 0.3;
    request.promptCache.mode = ai::models::PromptCacheMode::EphemeralOneHour;
    std::stop_source source;
    const auto translated = ai::backend::provider_client_detail::toModelRequest(
        request, source.get_token());
    require(translated.options.promptCache == request.promptCache,
        "request translation lost the typed cache policy");
    require(translated.messages.size() == 2
            && translated.messages.front().content == "stable"
            && translated.messages.back().content == "moving",
        "request translation changed the provider-facing prompt blocks");
    require(translated.options.model == "claude-test"
            && translated.options.maxOutputTokens == 321
            && translated.options.timeouts.totalTimeout
                == std::chrono::milliseconds {1234},
        "request translation changed generation controls");
}

void testResponseTranslationPreservesTheCompleteCacheAudit()
{
    ai::models::ModelResponse response;
    response.text = "done";
    response.usage.inputTokens = 70;
    response.usage.outputTokens = 3;
    response.usage.cacheCreationInputTokens = 20;
    response.usage.cacheReadInputTokens = 40;
    response.usage.cacheCreationDuration = {true, 5, 15};
    const auto translated =
        ai::backend::provider_client_detail::toChatResponse(response);
    require(translated.content == "done"
            && translated.usage.inputTokens == 70
            && translated.usage.cacheCreationInputTokens == 20
            && translated.usage.cacheReadInputTokens == 40
            && translated.usage.cacheCreationDuration.known
            && translated.usage.cacheCreationDuration.oneHourTokens == 15,
        "response translation lost cache accounting");
}

void testOnlyAnthropicAcceptsAnEnabledPolicy()
{
    using ai::backend::provider_client_detail::validatePromptCachePolicy;
    const ai::models::PromptCachePolicy oneHour {
        ai::models::PromptCacheMode::EphemeralOneHour};
    require(validatePromptCachePolicy("anthropic", oneHour).success(),
        "policy validation refused Anthropic one-hour caching");
    const auto openai = validatePromptCachePolicy("openai", oneHour);
    require(!openai.success()
            && openai.error()->code == "provider.prompt_cache_unsupported",
        "policy validation let OpenAI silently ignore caching");
    const auto unknown = validatePromptCachePolicy(
        "anthropic", {static_cast<ai::models::PromptCacheMode>(99)});
    require(!unknown.success()
            && unknown.error()->code == "provider.invalid_prompt_cache_policy",
        "policy validation accepted an unknown enum");
    require(validatePromptCachePolicy(
                "openai", {ai::models::PromptCacheMode::Disabled}).success(),
        "policy validation changed the disabled compatibility path");
}

} // namespace

int main()
{
    try {
        testRequestTranslationPreservesTheTypedPolicy();
        testResponseTranslationPreservesTheCompleteCacheAudit();
        testOnlyAnthropicAcceptsAnEnabledPolicy();
        std::cout << "ProviderClientTranslationTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProviderClientTranslationTests: " << error.what() << '\n';
        return 1;
    }
}
