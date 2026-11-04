#include "LoopbackHttpServer.hpp"
#include "ai/http/HttpRequest.hpp"
#include "ai/http/HttpResponse.hpp"
#include "ai/http/IHttpClient.hpp"
#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/providers/OpenAIProvider.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <functional>
#include <string>

namespace {

// The provider parser is intentionally tested with hand-written JSON fixtures.
// These tests verify response-shape handling without requiring a real API key,
// network connectivity, or billable OpenAI calls.
void assertThrowsContaining(const std::function<void()>& action, const std::string& expected)
{
    bool thrown = false;
    try {
        action();
    } catch (const std::exception& ex) {
        thrown = true;
        assert(std::string {ex.what()}.find(expected) != std::string::npos);
    }

    assert(thrown);
}

void testSingleOutputText()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "output": [
            {
              "type": "message",
              "role": "assistant",
              "content": [
                {"type": "output_text", "text": "Hello"}
              ]
            }
          ],
          "usage": {"input_tokens": 3, "output_tokens": 1}
        }
    )json");

    assert(ai::providers::openai_detail::parseOpenAIResponseText(response) == "Hello");
    const auto usage = ai::providers::openai_detail::parseOpenAITokenUsage(response);
    assert(usage.inputTokens == 3);
    assert(usage.outputTokens == 1);
}

// The shape OpenAI documents for Responses usage: input_tokens is the FULL
// input total and the two cache counters are subsets of it; reasoning tokens are
// already part of output_tokens.
void testTokenUsageSeparatesCachedAndCacheWriteTokens()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "usage": {
            "input_tokens": 1000,
            "input_tokens_details": {"cached_tokens": 600, "cache_write_tokens": 100},
            "output_tokens": 50,
            "output_tokens_details": {"reasoning_tokens": 20},
            "total_tokens": 1050
          }
        }
    )json");

    const auto usage = ai::providers::openai_detail::parseOpenAITokenUsage(response);
    assert(usage.inputTokens == 1000);          // the full total, never reduced
    assert(usage.cacheReadInputTokens == 600);  // cached_tokens
    assert(usage.cacheCreationInputTokens == 100); // cache_write_tokens
    assert(usage.outputTokens == 50);           // reasoning tokens are not added on top
}

// AG-17 gpt-6-sol checkpoint M2b: the long-context partition of a TokenUsage is BACKEND-derived.
// Whatever a provider response says - including keys that look like a partition - the parser never
// fills it and never marks the usage untrusted.
void testProviderJsonNeverFillsTheLongContextPartition()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "usage": {
            "input_tokens": 300000,
            "input_tokens_details": {"cached_tokens": 200000, "cache_write_tokens": 50000},
            "output_tokens": 1000,
            "longContext": {"inputTokens": 7, "outputTokens": 7},
            "long_context": {"input_tokens": 9, "output_tokens": 9},
            "accountingTrusted": false
          }
        }
    )json");
    const auto usage = ai::providers::openai_detail::parseOpenAITokenUsage(response);
    assert(usage.inputTokens == 300000 && usage.cacheReadInputTokens == 200000);
    assert(usage.longContext.empty());
    assert(usage.accountingTrusted);
}

// Models that predate cache-write reporting omit the field (or the whole
// details object): that is zero, never an error and never a guess.
void testTokenUsageWithoutCacheDetailsHasNoCacheSubsets()
{
    const auto noDetails = nlohmann::json::parse(
        R"json({"usage": {"input_tokens": 300, "output_tokens": 7}})json");
    auto usage = ai::providers::openai_detail::parseOpenAITokenUsage(noDetails);
    assert(usage.inputTokens == 300);
    assert(usage.outputTokens == 7);
    assert(usage.cacheReadInputTokens == 0);
    assert(usage.cacheCreationInputTokens == 0);

    const auto emptyDetails = nlohmann::json::parse(
        R"json({"usage": {"input_tokens": 300, "output_tokens": 7, "input_tokens_details": {}}})json");
    usage = ai::providers::openai_detail::parseOpenAITokenUsage(emptyDetails);
    assert(usage.inputTokens == 300);
    assert(usage.cacheReadInputTokens == 0);
    assert(usage.cacheCreationInputTokens == 0);

    const auto cachedOnly = nlohmann::json::parse(
        R"json({"usage": {"input_tokens": 300, "output_tokens": 7,
                          "input_tokens_details": {"cached_tokens": 128}}})json");
    usage = ai::providers::openai_detail::parseOpenAITokenUsage(cachedOnly);
    assert(usage.inputTokens == 300);
    assert(usage.cacheReadInputTokens == 128);
    assert(usage.cacheCreationInputTokens == 0);

    const auto writeOnly = nlohmann::json::parse(
        R"json({"usage": {"input_tokens": 300, "output_tokens": 7,
                          "input_tokens_details": {"cache_write_tokens": 250}}})json");
    usage = ai::providers::openai_detail::parseOpenAITokenUsage(writeOnly);
    assert(usage.cacheReadInputTokens == 0);
    assert(usage.cacheCreationInputTokens == 250);
}

// Malformed counters are zero, and one bad field never poisons its neighbours.
void testMalformedTokenUsageDetailsAreZeroNotErrors()
{
    for (const char* details : {
             R"json("cached_tokens")json",
             R"json([1, 2])json",
             "null",
             "7"}) {
        const auto response = nlohmann::json::parse(
            std::string {R"json({"usage": {"input_tokens": 40, "output_tokens": 3,
                                  "input_tokens_details": )json"} + details + "}}");
        const auto usage = ai::providers::openai_detail::parseOpenAITokenUsage(response);
        assert(usage.inputTokens == 40);
        assert(usage.outputTokens == 3);
        assert(usage.cacheReadInputTokens == 0);
        assert(usage.cacheCreationInputTokens == 0);
    }

    const auto badFields = nlohmann::json::parse(R"json(
        {"usage": {"input_tokens": 40, "output_tokens": 3,
                   "input_tokens_details": {"cached_tokens": "12", "cache_write_tokens": 5}}}
    )json");
    auto usage = ai::providers::openai_detail::parseOpenAITokenUsage(badFields);
    assert(usage.cacheReadInputTokens == 0);       // a string is not a counter
    assert(usage.cacheCreationInputTokens == 5);   // its neighbour is unaffected
    assert(usage.inputTokens == 40);

    const auto negativeAndFractional = nlohmann::json::parse(R"json(
        {"usage": {"input_tokens": 40, "output_tokens": 3,
                   "input_tokens_details": {"cached_tokens": -9, "cache_write_tokens": 2.5}}}
    )json");
    usage = ai::providers::openai_detail::parseOpenAITokenUsage(negativeAndFractional);
    assert(usage.cacheReadInputTokens == 0);
    assert(usage.cacheCreationInputTokens == 0);

    // The parser keeps an inconsistent report verbatim (cache subsets larger
    // than the total): consumers saturate and flag it, it is never repaired here.
    const auto inconsistent = nlohmann::json::parse(R"json(
        {"usage": {"input_tokens": 10, "output_tokens": 1,
                   "input_tokens_details": {"cached_tokens": 8, "cache_write_tokens": 8}}}
    )json");
    usage = ai::providers::openai_detail::parseOpenAITokenUsage(inconsistent);
    assert(usage.inputTokens == 10);
    assert(usage.cacheReadInputTokens == 8);
    assert(usage.cacheCreationInputTokens == 8);
}

void testMultipleTextParts()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "output": [
            {
              "type": "message",
              "content": [
                {"type": "output_text", "text": "Hello, "},
                {"type": "output_text", "text": "world"}
              ]
            }
          ]
        }
    )json");

    assert(ai::providers::openai_detail::parseOpenAIResponseText(response) == "Hello, world");
}

void testAdditionalOutputItemsAreIgnored()
{
    // Reasoning items may appear beside final messages in Responses API output.
    // The orchestrator should receive only the final assistant text.
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "output": [
            {"type": "reasoning", "content": [], "summary": []},
            {
              "type": "message",
              "content": [
                {"type": "output_text", "text": "Final text"}
              ]
            }
          ]
        }
    )json");

    assert(ai::providers::openai_detail::parseOpenAIResponseText(response) == "Final text");
}

void testRefusalThrows()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "output": [
            {"type": "refusal", "refusal": "Cannot comply"}
          ]
        }
    )json");

    assertThrowsContaining([&response] {
        static_cast<void>(ai::providers::openai_detail::parseOpenAIResponseText(response));
    }, "Cannot comply");
}

void testEmptyOutputThrows()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "output": []
        }
    )json");

    assertThrowsContaining([&response] {
        static_cast<void>(ai::providers::openai_detail::parseOpenAIResponseText(response));
    }, "output text");
}

void testInvalidStructureThrows()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "completed",
          "not_output": []
        }
    )json");

    assertThrowsContaining([&response] {
        static_cast<void>(ai::providers::openai_detail::parseOpenAIResponseText(response));
    }, "output text");
}

void testIncompleteStatusThrows()
{
    const auto response = nlohmann::json::parse(R"json(
        {
          "status": "incomplete",
          "incomplete_details": {"reason": "max_output_tokens"}
        }
    )json");

    assertThrowsContaining([&response] {
        static_cast<void>(ai::providers::openai_detail::parseOpenAIResponseText(response));
    }, "max_output_tokens");
}

void testErrorMessageExtraction()
{
    const std::string body = R"json(
        {
          "error": {
            "message": "Incorrect API key provided",
            "type": "invalid_request_error",
            "code": "invalid_api_key"
          }
        }
    )json";

    assert(ai::providers::openai_detail::extractOpenAIErrorMessage(body)
        == "Incorrect API key provided");
}

// ---------------------------------------------------------------------------
// Legacy single-shot path: structured failure code from a received HTTP response
// ---------------------------------------------------------------------------

class RecordingTransport final : public ai::http::IHttpClient {
public:
    explicit RecordingTransport(ai::http::HttpResponse response) : response_(std::move(response)) {}
    ai::http::HttpResponse send(const ai::http::HttpRequest& request) override
    {
        ++calls;
        last = request;
        return response_;
    }
    int calls {0};
    ai::http::HttpRequest last;

private:
    ai::http::HttpResponse response_;
};

ai::http::HttpResponse httpReply(int status, std::string body)
{
    ai::http::HttpResponse reply;
    reply.statusCode = status;
    reply.body = std::move(body);
    return reply;
}

ai::models::ModelRequest legacyRequest()
{
    ai::models::ModelRequest request;
    request.options.model = "gpt-5.4";
    request.messages.push_back({ai::models::MessageRole::User, "Answer with one word."});
    return request;
}

std::string structuredErrorBody(const std::string& code, const std::string& type, const std::string& message)
{
    return std::string(R"({"error":{"message":")") + message + R"(","type":")" + type + R"(","param":null,"code":)"
        + (code.empty() ? std::string("null") : "\"" + code + "\"") + "}}";
}

// One classifier for both paths: the status decides, even for an empty or malformed body; only a 429 is refined by
// the STRUCTURED quota fields, and the verdict never contains the provider's own text.
void testHttpFailureClassifierDecidesByStatusAndStructuredQuotaFields()
{
    using ai::providers::openai_detail::classifyOpenAIHttpFailure;
    for (const char* durable : {"credit_balance_exhausted", "organization_spend_limit_exceeded",
             "project_spend_limit_exceeded", "organization_usage_limit_exceeded", "insufficient_quota"}) {
        const auto failure = classifyOpenAIHttpFailure(429, structuredErrorBody(durable, "billing", "Try again in 20s sk-secret"));
        assert(failure.code == "provider.insufficient_quota");
        assert(!failure.durableMessage.empty() && failure.durableMessage.find("sk-secret") == std::string::npos
            && failure.durableMessage.find("Try again") == std::string::npos);
    }
    assert(classifyOpenAIHttpFailure(429, structuredErrorBody("", "insufficient_quota", "m")).code
        == "provider.insufficient_quota");
    // Temporary / unstructured 429 and every body shape that carries no recognized field.
    for (const std::string& body : {structuredErrorBody("rate_limit_exceeded", "requests", "m"),
             structuredErrorBody("slow_down", "requests", "m"), std::string(), std::string("not json"),
             std::string(R"({"error":"insufficient_quota"})"), std::string(R"({"error":{"code":429}})"),
             std::string(R"({"error":{"message":"You exceeded your current quota, check your plan and billing details."}})")}) {
        const auto failure = classifyOpenAIHttpFailure(429, body);
        assert(failure.code == "provider.rate_limited" && failure.durableMessage.empty());
    }
    // The status decides for every other non-2xx response, whatever the body says.
    const auto misleading = structuredErrorBody("insufficient_quota", "insufficient_quota", "authentication connect json 429");
    for (const std::string& body : {std::string(), std::string("<html>"), misleading}) {
        assert(classifyOpenAIHttpFailure(401, body).code == "provider.auth_failed");
        assert(classifyOpenAIHttpFailure(500, body).code == "provider.server_error");
        assert(classifyOpenAIHttpFailure(503, body).code == "provider.server_error");
        assert(classifyOpenAIHttpFailure(400, body).code == "provider.request_failed");
        assert(classifyOpenAIHttpFailure(403, body).code == "provider.request_failed");
        assert(classifyOpenAIHttpFailure(404, body).code == "provider.request_failed");
        assert(classifyOpenAIHttpFailure(401, body).durableMessage.empty());
    }
}

// The real detailed provider over a fake transport: ONE send, a typed code, a fixed message without provider text.
void testDetailedProviderReportsADurableQuotaRefusalWithOneRequest()
{
    ai::providers::OpenAIProvider provider("test-key");
    RecordingTransport transport(httpReply(
        429, structuredErrorBody("credit_balance_exhausted", "billing", "Try again in 20s. sk-secret-key-0123456789")));
    const auto result = provider.generateDetailed(legacyRequest(), transport);
    assert(transport.calls == 1);
    assert(!result.response.success && result.response.provider == "openai");
    assert(result.errorCode == "provider.insufficient_quota");
    assert(result.response.errorMessage.find("no remaining credit balance") != std::string::npos);
    assert(result.response.errorMessage.find("sk-secret") == std::string::npos
        && result.response.errorMessage.find("Try again") == std::string::npos);
    assert(transport.last.headers.at("Authorization") == "Bearer test-key");   // the real request was built
}

// Other HTTP failures keep their previous human messages byte for byte and now also carry the status code.
void testDetailedProviderKeepsTheHumanMessageOfEveryOtherHttpFailure()
{
    struct Case {
        int status;
        std::string body;
        std::string code;
        std::string message;
    };
    const std::vector<Case> cases {
        {429, structuredErrorBody("rate_limit_exceeded", "requests", "Rate limit reached"),
            "provider.rate_limited", "OpenAI request failed with HTTP 429: Rate limit reached"},
        {429, "", "provider.rate_limited", "OpenAI request failed with HTTP 429"},
        {401, "not json", "provider.auth_failed", "OpenAI request failed with HTTP 401"},
        {500, "", "provider.server_error", "OpenAI request failed with HTTP 500"},
        {400, R"({"error":{"message":"authentication connect json 429"}})", "provider.request_failed",
            "OpenAI request failed with HTTP 400: authentication connect json 429"},
        {403, "<html>forbidden</html>", "provider.request_failed", "OpenAI request failed with HTTP 403"},
    };
    for (const auto& c : cases) {
        ai::providers::OpenAIProvider provider("test-key");
        RecordingTransport transport(httpReply(c.status, c.body));
        const auto result = provider.generateDetailed(legacyRequest(), transport);
        assert(transport.calls == 1 && !result.response.success);
        assert(result.errorCode == c.code);
        assert(result.response.errorMessage == c.message);
    }
}

void testDetailedProviderSuccessKeepsContentAndUsageAndHasNoCode()
{
    ai::providers::OpenAIProvider provider("test-key");
    RecordingTransport transport(httpReply(200, R"({
        "status": "completed",
        "output": [{"type": "message", "role": "assistant",
                    "content": [{"type": "output_text", "text": "Hello"}]}],
        "usage": {"input_tokens": 30, "output_tokens": 4,
                  "input_tokens_details": {"cached_tokens": 12, "cache_write_tokens": 5}}
    })"));
    const auto result = provider.generateDetailed(legacyRequest(), transport);
    assert(transport.calls == 1);
    assert(result.response.success && result.response.text == "Hello");
    assert(result.response.usage.inputTokens == 30 && result.response.usage.outputTokens == 4
        && result.response.usage.cacheReadInputTokens == 12 && result.response.usage.cacheCreationInputTokens == 5);
    assert(result.errorCode.empty());
}

// A transport failure, an unusable success body and a missing key have no received HTTP error: no structured code,
// the caller's explicit text fallback stays responsible for them.
void testDetailedProviderLeavesNonHttpFailuresWithoutACode()
{
    {
        ai::providers::OpenAIProvider provider("test-key");
        ai::http::HttpResponse down;
        down.errorMessage = "could not resolve host";
        RecordingTransport transport(down);
        const auto result = provider.generateDetailed(legacyRequest(), transport);
        assert(!result.response.success && result.errorCode.empty() && transport.calls == 1);
        assert(result.response.errorMessage.find("transport error") != std::string::npos);
    }
    {
        ai::providers::OpenAIProvider provider("test-key");
        RecordingTransport transport(httpReply(200, "{ not json"));
        const auto result = provider.generateDetailed(legacyRequest(), transport);
        assert(!result.response.success && result.errorCode.empty());
    }
    {
        // generate() is the thin wrapper over generateDetailed(): the missing-key failure is identical, no network.
        ai::providers::OpenAIProvider provider("");
        RecordingTransport transport(httpReply(200, "{}"));
        const auto detailed = provider.generateDetailed(legacyRequest(), transport);
        const auto wrapped = provider.generate(legacyRequest());
        assert(transport.calls == 0 && detailed.errorCode.empty());
        assert(wrapped.success == detailed.response.success && wrapped.errorMessage == detailed.response.errorMessage);
    }
}

#ifdef _WIN32
// The REAL default transport of the single-shot provider (HttpClient + libcurl + the retry policy) against a loopback
// server that counts requests: a durable quota refusal is sent once (through generateDetailed AND through the
// generate() wrapper); a temporary rate limit keeps its three attempts.
void testProductionTransportSendsADurableQuotaRefusalOnceAndRetriesATemporaryOne()
{
    const auto request = [](const std::string& url) {
        auto modelRequest = legacyRequest();
        modelRequest.options.metadata["openai.endpoint"] = url;
        return modelRequest;
    };
    {
        ai::testing::LoopbackHttpServer server(429, structuredErrorBody("credit_balance_exhausted", "billing", "m"));
        assert(server.ready());
        ai::providers::OpenAIProvider provider("test-key");
        const auto result = provider.generateDetailed(request(server.url()));
        assert(server.requestsServed() == 1 && "a durable quota refusal must be sent exactly once");
        assert(!result.response.success && result.errorCode == "provider.insufficient_quota");
        assert(result.response.errorMessage.find("no remaining credit balance") != std::string::npos);
    }
    {
        ai::testing::LoopbackHttpServer server(429, structuredErrorBody("insufficient_quota", "insufficient_quota", "m"));
        assert(server.ready());
        ai::providers::OpenAIProvider provider("test-key");
        const auto wrapped = provider.generate(request(server.url()));   // the thin wrapper uses the same transport
        assert(server.requestsServed() == 1 && !wrapped.success);
    }
    {
        ai::testing::LoopbackHttpServer server(429, structuredErrorBody("rate_limit_exceeded", "requests", "m"));
        assert(server.ready());
        ai::providers::OpenAIProvider provider("test-key");
        const auto result = provider.generateDetailed(request(server.url()));
        assert(server.requestsServed() == 3 && "a temporary rate limit keeps its bounded retries");
        assert(result.errorCode == "provider.rate_limited");
    }
}
#endif

void testGenerateWithoutApiKeyDoesNotCallNetwork()
{
    // Missing credentials should be reported before HttpClient is constructed,
    // making this test deterministic and safe in local/CI environments.
    ai::providers::OpenAIProvider provider("");

    ai::models::ModelRequest request;
    request.options.model = "gpt-5.4";
    request.messages.push_back({
        ai::models::MessageRole::User,
        "Answer with one word."
    });

    const auto response = provider.generate(request);
    assert(!response.success);
    assert(response.provider == "openai");
    assert(response.errorMessage.find("OPENAI_API_KEY") != std::string::npos);
}

} // namespace

int runOpenAIProviderTests()
{
    // Keep OpenAI-specific tests in this function so the aggregate test binary
    // can run all provider and orchestration checks from one executable.
    testSingleOutputText();
    testTokenUsageSeparatesCachedAndCacheWriteTokens();
    testProviderJsonNeverFillsTheLongContextPartition();
    testTokenUsageWithoutCacheDetailsHasNoCacheSubsets();
    testMalformedTokenUsageDetailsAreZeroNotErrors();
    testMultipleTextParts();
    testAdditionalOutputItemsAreIgnored();
    testRefusalThrows();
    testEmptyOutputThrows();
    testInvalidStructureThrows();
    testIncompleteStatusThrows();
    testErrorMessageExtraction();
    testHttpFailureClassifierDecidesByStatusAndStructuredQuotaFields();
    testDetailedProviderReportsADurableQuotaRefusalWithOneRequest();
    testDetailedProviderKeepsTheHumanMessageOfEveryOtherHttpFailure();
    testDetailedProviderSuccessKeepsContentAndUsageAndHasNoCode();
    testDetailedProviderLeavesNonHttpFailuresWithoutACode();
#ifdef _WIN32
    testProductionTransportSendsADurableQuotaRefusalOnceAndRetriesATemporaryOne();
#endif
    testGenerateWithoutApiKeyDoesNotCallNetwork();

    return 0;
}
