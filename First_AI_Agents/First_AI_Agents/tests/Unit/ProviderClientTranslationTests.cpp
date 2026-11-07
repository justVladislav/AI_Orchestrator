#include "ai/backend/ProviderClientTranslation.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
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

// ---------------------------------------------------------------------------------------------------------------------
// C2-B/S2a: the common production client (GenerateProviderClient) and the observed-call contract, proven through the
// IProviderClient VIRTUAL calls with a fake Generate - no HTTP, no provider object, no network.
// ---------------------------------------------------------------------------------------------------------------------
namespace app = ai::application;
namespace det = ai::backend::provider_client_detail;

struct GenerateScript {
    int calls {0};
    std::stop_source* raiseStop {nullptr};     // raised INSIDE the call
    ai::models::ModelResponse response;
    std::string code;
    std::optional<ai::models::ModelRequest> seen;
};

det::GenerateFn generateOf(GenerateScript& script)
{
    return [&script](const ai::models::ModelRequest& request) {
        ++script.calls;
        script.seen = request;
        if (script.raiseStop != nullptr) script.raiseStop->request_stop();
        return std::make_pair(script.response, script.code);
    };
}

ai::models::ModelResponse successfulResponse()
{
    ai::models::ModelResponse response;
    response.success = true;
    response.text = "the paid answer";
    response.usage.inputTokens = 1000;
    response.usage.outputTokens = 20;
    response.usage.cacheCreationInputTokens = 300;
    response.usage.cacheReadInputTokens = 500;
    response.usage.cacheCreationDuration = {true, 100, 200};
    return response;
}

app::ProviderChatRequest plainRequest()
{
    app::ProviderChatRequest request;
    request.apiModelId = "model-x";
    request.systemPrompt = "system";
    request.userContent = "user";
    return request;
}

void requireTheOutcomeInvariant(const app::ProviderChatOutcome& outcome, const std::string& what)
{
    if (outcome.result.success()) {
        require(!outcome.discardedResponse.has_value(), what + ": a successful result never carries a discarded response");
    }
    if (outcome.discardedResponse.has_value()) {
        require(!outcome.result.success() && outcome.result.error()->code == "provider.cancelled",
            what + ": a discarded response exists only next to the provider.cancelled failure");
    }
}

void requireTheFullUsage(const ai::models::TokenUsage& usage, const std::string& what)
{
    require(usage.inputTokens == 1000 && usage.outputTokens == 20 && usage.cacheCreationInputTokens == 300
            && usage.cacheReadInputTokens == 500 && usage.cacheCreationDuration.known
            && usage.cacheCreationDuration.fiveMinuteTokens == 100 && usage.cacheCreationDuration.oneHourTokens == 200,
        what + ": the complete usage (cache details too) must be preserved");
}

// A client that implements ONLY send(), like the existing fakes of every other caller.
class SendOnlyClient final : public app::IProviderClient {
public:
    int sends {0};
    ai::common::Result<app::ProviderChatResponse> next {ai::common::Result<app::ProviderChatResponse>::failure({"x", "unset"})};

    ai::common::Result<app::ProviderChatResponse> send(const app::ProviderChatRequest&, std::stop_token) override
    {
        ++sends;
        return next;
    }
};

void testTheDefaultObservedCallIsExactlyOneSendAndCarriesNoDiscardedResponse()
{
    SendOnlyClient client;
    app::IProviderClient& virtualClient = client;
    app::ProviderChatResponse paid;
    paid.content = "paid";
    paid.usage = successfulResponse().usage;
    client.next = ai::common::Result<app::ProviderChatResponse>::ok(paid);
    const auto success = virtualClient.sendObserved(plainRequest(), std::stop_token {});
    require(client.sends == 1, "the default observed call performs exactly ONE send");
    require(success.result.success() && success.result.value().content == "paid", "...and returns its success unchanged");
    requireTheFullUsage(success.result.value().usage, "default observed success");
    requireTheOutcomeInvariant(success, "default observed success");

    client.next = ai::common::Result<app::ProviderChatResponse>::failure({"provider.rate_limited", "slow down"});
    const auto failure = virtualClient.sendObserved(plainRequest(), std::stop_token {});
    require(client.sends == 2 && !failure.result.success() && failure.result.error()->code == "provider.rate_limited"
            && failure.result.error()->message == "slow down" && !failure.discardedResponse.has_value(),
        "a failure is returned unchanged and without a payload, again with one send");

    // Even a cancellation reported by a send-only client has no payload: the default cannot know one.
    std::stop_source stopped;
    stopped.request_stop();
    client.next = ai::common::Result<app::ProviderChatResponse>::failure({"provider.cancelled", "stopped"});
    require(!virtualClient.sendObserved(plainRequest(), stopped.get_token()).discardedResponse.has_value(),
        "the default observed call never invents a discarded response");
}

void testTheProductionClientDoesNotSendAfterAStopBeforeTheRequest()
{
    for (const char* kind : {"openai", "anthropic"}) {
        GenerateScript script;
        script.response = successfulResponse();
        std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>(kind, generateOf(script));
        std::stop_source stopped;
        stopped.request_stop();
        const auto observed = client->sendObserved(plainRequest(), stopped.get_token());
        require(!observed.result.success() && observed.result.error()->code == "provider.cancelled"
                && observed.result.error()->message == "Cancelled before the request was sent"
                && !observed.discardedResponse.has_value(),
            std::string(kind) + ": a stop before the request is a payload-free cancellation");
        const auto legacy = client->send(plainRequest(), stopped.get_token());
        require(!legacy.success() && legacy.error()->code == "provider.cancelled" && script.calls == 0,
            std::string(kind) + ": legacy send() agrees and neither call ran the provider (0 Generate)");
    }
}

void testTheProductionClientKeepsAResponseThatACancellationDiscarded()
{
    for (const char* kind : {"openai", "anthropic"}) {
        // observed: the received answer is reported, not lost
        {
            GenerateScript script;
            script.response = successfulResponse();
            std::stop_source stop;
            script.raiseStop = &stop;
            std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>(kind, generateOf(script));
            const auto observed = client->sendObserved(plainRequest(), stop.get_token());
            require(script.calls == 1, std::string(kind) + ": exactly ONE provider call");
            require(!observed.result.success() && observed.result.error()->code == "provider.cancelled"
                    && observed.result.error()->message == "Cancelled while the request was in flight",
                std::string(kind) + ": the result is still the cancellation");
            require(observed.discardedResponse.has_value() && observed.discardedResponse->content == "the paid answer",
                std::string(kind) + ": the received answer is returned as the discarded response");
            requireTheFullUsage(observed.discardedResponse->usage, std::string(kind) + " discarded response");
            requireTheOutcomeInvariant(observed, kind);
        }
        // legacy send() on the SAME situation: the old cancelled failure, ONE provider call, no second request
        {
            GenerateScript script;
            script.response = successfulResponse();
            std::stop_source stop;
            script.raiseStop = &stop;
            std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>(kind, generateOf(script));
            const auto legacy = client->send(plainRequest(), stop.get_token());
            require(script.calls == 1, std::string(kind) + ": legacy send() makes exactly ONE provider call");
            require(!legacy.success() && legacy.error()->code == "provider.cancelled"
                    && legacy.error()->message == "Cancelled while the request was in flight",
                std::string(kind) + ": legacy send() keeps its exact cancelled failure");
        }
    }
}

void testTheProductionClientNeverCapturesAFailedCall()
{
    for (const char* kind : {"openai", "anthropic"}) {
        GenerateScript script;
        script.response.success = false;
        script.response.errorMessage = "transport aborted";
        script.response.text = "partial text must not travel";
        script.response.usage = successfulResponse().usage;   // even a populated usage on a FAILED response is not evidence
        std::stop_source stop;
        script.raiseStop = &stop;
        std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>(kind, generateOf(script));
        const auto observed = client->sendObserved(plainRequest(), stop.get_token());
        require(!observed.result.success() && observed.result.error()->code == "provider.cancelled"
                && !observed.discardedResponse.has_value() && script.calls == 1,
            std::string(kind) + ": a stop during a failing call is a cancellation without a payload");
    }
}

void testTheProductionClientWithoutAStopKeepsTheOldResults()
{
    for (const char* kind : {"openai", "anthropic"}) {
        {
            GenerateScript script;
            script.response = successfulResponse();
            std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>(kind, generateOf(script));
            const auto observed = client->sendObserved(plainRequest(), std::stop_token {});
            require(observed.result.success() && observed.result.value().content == "the paid answer", kind);
            requireTheFullUsage(observed.result.value().usage, std::string(kind) + " success");
            requireTheOutcomeInvariant(observed, kind);
            const auto legacy = client->send(plainRequest(), std::stop_token {});
            require(legacy.success() && legacy.value().content == "the paid answer" && script.calls == 2,
                std::string(kind) + ": legacy send() returns the same success (one call each)");
        }
        {
            GenerateScript script;
            script.response.success = false;
            script.response.errorMessage = "quota exhausted";
            script.code = "provider.insufficient_quota";
            std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>(kind, generateOf(script));
            const auto observed = client->sendObserved(plainRequest(), std::stop_token {});
            require(!observed.result.success() && observed.result.error()->code == "provider.insufficient_quota"
                    && observed.result.error()->message == "quota exhausted" && !observed.discardedResponse.has_value(),
                std::string(kind) + ": the structured failure code and message travel unchanged");
            script.code.clear();
            script.response.errorMessage = "HTTP 401 Unauthorized";
            const auto fallback = client->send(plainRequest(), std::stop_token {});
            require(!fallback.success() && fallback.error()->code == "provider.auth_failed",
                std::string(kind) + ": an empty code keeps the text fallback");
        }
    }
}

void testTheProductionClientKeepsThePolicyGateAndItsPriority()
{
    app::ProviderChatRequest oneHour = plainRequest();
    oneHour.promptCache.mode = ai::models::PromptCacheMode::EphemeralOneHour;
    app::ProviderChatRequest unknown = plainRequest();
    unknown.promptCache.mode = static_cast<ai::models::PromptCacheMode>(99);
    std::stop_source stopped;
    stopped.request_stop();

    {   // OpenAI refuses caching: the typed error outranks even a stop, and the provider is not called
        GenerateScript script;
        std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>("openai", generateOf(script));
        const auto withStop = client->sendObserved(oneHour, stopped.get_token());
        const auto withoutStop = client->send(oneHour, std::stop_token {});
        require(!withStop.result.success() && withStop.result.error()->code == "provider.prompt_cache_unsupported"
                && !withoutStop.success() && withoutStop.error()->code == "provider.prompt_cache_unsupported" && script.calls == 0,
            "openai: an unsupported cache policy is refused first, with 0 Generate");
    }
    {   // Anthropic: an unknown enum is refused first; the supported one-hour policy is admitted and reaches the provider request
        GenerateScript script;
        script.response = successfulResponse();
        std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>("anthropic", generateOf(script));
        const auto refused = client->sendObserved(unknown, stopped.get_token());
        require(!refused.result.success() && refused.result.error()->code == "provider.invalid_prompt_cache_policy" && script.calls == 0,
            "anthropic: an unknown cache policy is refused before any call");
        const auto admitted = client->sendObserved(oneHour, std::stop_token {});
        require(admitted.result.success() && script.calls == 1 && script.seen.has_value()
                && script.seen->options.promptCache == oneHour.promptCache,
            "anthropic: the supported one-hour policy is admitted and reaches the provider request");
        const auto cancelled = client->sendObserved(oneHour, stopped.get_token());
        require(!cancelled.result.success() && cancelled.result.error()->code == "provider.cancelled" && script.calls == 1,
            "anthropic: an admitted policy followed by a stop before the request cancels with 0 further Generate");
    }
    {   // The request the provider callable receives is the translated one, and its cancellation probe follows the token
        GenerateScript script;
        script.response = successfulResponse();
        std::stop_source stop;
        std::shared_ptr<app::IProviderClient> client = std::make_shared<det::GenerateProviderClient>("openai", generateOf(script));
        const auto outcome = client->sendObserved(plainRequest(), stop.get_token());
        require(outcome.result.success() && script.seen.has_value() && script.seen->options.model == "model-x"
                && script.seen->messages.size() == 2 && script.seen->options.cancellationRequested
                && !script.seen->options.cancellationRequested(),
            "the provider request is translated and its cancellation probe is wired to the token");
        stop.request_stop();
        require(script.seen->options.cancellationRequested(), "...and follows a later stop");
    }
}

} // namespace

int main()
{
    try {
        testRequestTranslationPreservesTheTypedPolicy();
        testResponseTranslationPreservesTheCompleteCacheAudit();
        testOnlyAnthropicAcceptsAnEnabledPolicy();
        testStructuredCodeBeatsTheTextAndAnEmptyCodeKeepsTheTextFallback();
        testTheDefaultObservedCallIsExactlyOneSendAndCarriesNoDiscardedResponse();
        testTheProductionClientDoesNotSendAfterAStopBeforeTheRequest();
        testTheProductionClientKeepsAResponseThatACancellationDiscarded();
        testTheProductionClientNeverCapturesAFailedCall();
        testTheProductionClientWithoutAStopKeepsTheOldResults();
        testTheProductionClientKeepsThePolicyGateAndItsPriority();
        std::cout << "ProviderClientTranslationTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProviderClientTranslationTests: " << error.what() << '\n';
        return 1;
    }
}
