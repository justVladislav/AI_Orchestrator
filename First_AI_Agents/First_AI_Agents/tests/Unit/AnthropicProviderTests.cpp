#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/providers/AnthropicProvider.hpp"
#include "ai/providers/AnthropicResponseParser.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void testRuntimeCatalogModelWinsForUnknownProviderType()
{
    // An empty key makes generate() stop before any network access while the
    // returned ModelResponse still exposes the resolved request model.
    ai::providers::AnthropicProvider provider(
        "", ai::providers::ModelTypeUnknown);

    constexpr std::array<const char*, 4> modelIds {
        "claude-fable-5",
        "claude-opus-4-8",
        "claude-sonnet-5",
        "claude-haiku-4-5-20251001"
    };

    for (const char* modelId : modelIds) {
        ai::models::ModelRequest request;
        request.options.model = modelId;
        request.messages.push_back({
            ai::models::MessageRole::User,
            "This request must not reach the network."
        });

        const auto response = provider.generate(request);
        assert(!response.success);
        assert(response.provider == "anthropic");
        assert(response.model == modelId);
        assert(response.errorMessage.find("ANTHROPIC_API_KEY") != std::string::npos);
    }
}

ai::models::ModelRequest cacheRequest(ai::models::PromptCacheMode mode)
{
    ai::models::ModelRequest request;
    request.options.model = "claude-test";
    request.options.maxOutputTokens = 123;
    request.options.promptCache.mode = mode;
    request.messages.push_back({ai::models::MessageRole::System, "stable prefix"});
    request.messages.push_back({ai::models::MessageRole::User, "moving request"});
    return request;
}

void testSingleShotCachePolicyHasAnExactWireShape()
{
    const auto disabled = nlohmann::json::parse(
        ai::providers::anthropic_detail::renderAnthropicRequestBody(
            cacheRequest(ai::models::PromptCacheMode::Disabled), "claude-test"));
    require(disabled.at("system").is_string()
            && disabled.at("system") == "stable prefix",
        "wire: disabled policy changed the legacy system shape");
    require(disabled.dump().find("cache_control") == std::string::npos,
        "wire: disabled policy emitted cache_control");

    const auto five = nlohmann::json::parse(
        ai::providers::anthropic_detail::renderAnthropicRequestBody(
            cacheRequest(ai::models::PromptCacheMode::EphemeralFiveMinutes),
            "claude-test"));
    const auto& fiveControl = five.at("system").at(0).at("cache_control");
    require(five.at("system").is_array()
            && five.at("system").at(0).at("text") == "stable prefix"
            && fiveControl.at("type") == "ephemeral"
            && !fiveControl.contains("ttl"),
        "wire: five-minute policy must omit ttl");

    const auto hour = nlohmann::json::parse(
        ai::providers::anthropic_detail::renderAnthropicRequestBody(
            cacheRequest(ai::models::PromptCacheMode::EphemeralOneHour),
            "claude-test"));
    require(hour.at("system").at(0).at("cache_control").at("ttl") == "1h",
        "wire: one-hour policy did not emit ttl=1h");
    require(hour.at("messages").size() == 1
            && hour.at("messages").at(0).at("content") == "moving request"
            && hour.at("messages").dump().find("cache_control") == std::string::npos,
        "wire: caching escaped the stable system prefix");
}

void testSingleShotCachePolicyRefusesAnImpossibleTarget()
{
    auto empty = cacheRequest(ai::models::PromptCacheMode::EphemeralOneHour);
    empty.messages.erase(empty.messages.begin());
    bool emptyRefused = false;
    try {
        (void)ai::providers::anthropic_detail::renderAnthropicRequestBody(
            empty, "claude-test");
    } catch (const std::runtime_error&) {
        emptyRefused = true;
    }
    require(emptyRefused, "wire: enabled cache accepted no stable system prompt");

    auto unknown = cacheRequest(static_cast<ai::models::PromptCacheMode>(99));
    bool unknownRefused = false;
    try {
        (void)ai::providers::anthropic_detail::renderAnthropicRequestBody(
            unknown, "claude-test");
    } catch (const std::runtime_error&) {
        unknownRefused = true;
    }
    require(unknownRefused, "wire: unknown cache policy was accepted");
}

void testSingleShotUsageCarriesTheAuthoritativeDurationAudit()
{
    const auto response = nlohmann::json::parse(R"({
        "usage": {"input_tokens": 10, "output_tokens": 3,
          "cache_creation_input_tokens": 20, "cache_read_input_tokens": 40,
          "cache_creation": {"ephemeral_5m_input_tokens": 5,
                             "ephemeral_1h_input_tokens": 15}}
    })");
    const auto usage = ai::providers::anthropic_detail::parseAnthropicTokenUsage(
        response, true);
    require(usage.inputTokens == 70 && usage.outputTokens == 3
            && usage.cacheCreationInputTokens == 20
            && usage.cacheReadInputTokens == 40,
        "usage: cache categories were not included in the conservative total");
    require(usage.cacheCreationDuration.known
            && usage.cacheCreationDuration.fiveMinuteTokens == 5
            && usage.cacheCreationDuration.oneHourTokens == 15
            && usage.accountingTrusted,
        "usage: exact duration audit was lost");

    const auto missingDetail = nlohmann::json::parse(R"({
        "usage": {"input_tokens": 10, "output_tokens": 1,
          "cache_creation_input_tokens": 2}
    })");
    require(!ai::providers::anthropic_detail::parseAnthropicTokenUsage(
                missingDetail, true).accountingTrusted,
        "usage: one-hour write without duration detail looked exact");
    require(ai::providers::anthropic_detail::parseAnthropicTokenUsage(
                missingDetail, false).accountingTrusted,
        "usage: compatible legacy/five-minute accounting was rejected");
}

void testSingleShotUsageRejectsMalformedCountersAndPartitions()
{
    const auto rejects = [](const std::string& json) {
        try {
            (void)ai::providers::anthropic_detail::parseAnthropicTokenUsage(
                nlohmann::json::parse(json), true);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    require(rejects(R"({"usage":{"input_tokens":-1}})"),
        "usage: negative counter was accepted");
    require(rejects(R"([])"),
        "usage: a non-object response was accepted as a zero-cost success");
    require(rejects(R"({})"),
        "usage: a response without usage was accepted as a zero-cost success");
    require(rejects(R"({"usage":[]})"),
        "usage: a non-object usage value was accepted");
    require(rejects(R"({"usage":{"output_tokens":1}})"),
        "usage: missing input_tokens was accepted");
    require(rejects(R"({"usage":{"input_tokens":1}})"),
        "usage: missing output_tokens was accepted");
    require(rejects(R"({"usage":{"input_tokens":"10"}})"),
        "usage: textual counter was accepted");
    require(rejects(R"({"usage":{"cache_creation_input_tokens":4,
        "cache_creation":{"ephemeral_5m_input_tokens":1,
                          "ephemeral_1h_input_tokens":2}}})"),
        "usage: incomplete duration partition was accepted");
    require(rejects(std::string("{\"usage\":{\"input_tokens\":")
            + std::to_string(std::numeric_limits<std::uint64_t>::max())
            + ",\"cache_creation_input_tokens\":1}}"),
        "usage: overflowing input total was accepted");
}

void testExplicitConstructorModelStillWins()
{
    ai::providers::AnthropicProvider provider(
        "", ai::providers::ModelTypeClaudeSonnet);

    ai::models::ModelRequest request;
    request.options.model = "claude-haiku-4-5-20251001";
    request.messages.push_back({
        ai::models::MessageRole::User,
        "This request must not reach the network."
    });

    const auto response = provider.generate(request);
    assert(!response.success);
    assert(response.model == "claude-sonnet-5");
}

} // namespace

int runAnthropicProviderTests()
{
    testRuntimeCatalogModelWinsForUnknownProviderType();
    testExplicitConstructorModelStillWins();
    testSingleShotCachePolicyHasAnExactWireShape();
    testSingleShotCachePolicyRefusesAnImpossibleTarget();
    testSingleShotUsageCarriesTheAuthoritativeDurationAudit();
    testSingleShotUsageRejectsMalformedCountersAndPartitions();
    return 0;
}
