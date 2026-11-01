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

// Typed provider result -> IProviderClient result: a structured code beats every word of the text; an empty code uses
// the previous text table; a success carries no error.
void testStructuredCodeBeatsTheTextAndAnEmptyCodeKeepsTheTextFallback()
{
    using ai::backend::provider_client_detail::toChatResult;
    ai::models::ModelResponse failure;
    failure.success = false;
    failure.errorMessage = "authentication connect json 429 rate limit timeout";
    const auto structured = toChatResult(failure, "provider.insufficient_quota");
    require(!structured.success() && structured.error()->code == "provider.insufficient_quota"
            && structured.error()->message == failure.errorMessage,
        "a structured code must win over any word in the message, and the message must travel unchanged");

    const struct { const char* text; const char* code; } fallbacks[] = {
        {"HTTP 401 Unauthorized", "provider.auth_failed"},
        {"Rate limit exceeded (429)", "provider.rate_limited"},
        {"request timed out", "provider.timeout"},
        {"curl: could not resolve host", "provider.network_error"},
        {"JSON parse failed", "provider.invalid_response"},
        {"something else", "provider.request_failed"},
    };
    for (const auto& entry : fallbacks) {
        ai::models::ModelResponse plain;
        plain.success = false;
        plain.errorMessage = entry.text;
        const auto result = toChatResult(plain, "");
        require(!result.success() && result.error()->code == entry.code,
            std::string("the text fallback changed for: ") + entry.text);
    }

    ai::models::ModelResponse success;
    success.success = true;
    success.text = "ok";
    success.usage.inputTokens = 3;
    const auto ok = toChatResult(success, "provider.insufficient_quota");   // a code on a success is ignored
    require(ok.success() && ok.value().content == "ok" && ok.value().usage.inputTokens == 3,
        "a success must translate to the content and usage, with no error");
}

} // namespace

int main()
{
    try {
        testRequestTranslationPreservesTheTypedPolicy();
        testResponseTranslationPreservesTheCompleteCacheAudit();
        testOnlyAnthropicAcceptsAnEnabledPolicy();
        testStructuredCodeBeatsTheTextAndAnEmptyCodeKeepsTheTextFallback();
        std::cout << "ProviderClientTranslationTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProviderClientTranslationTests: " << error.what() << '\n';
        return 1;
    }
}
