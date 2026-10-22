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
    testGenerateWithoutApiKeyDoesNotCallNetwork();

    return 0;
}
