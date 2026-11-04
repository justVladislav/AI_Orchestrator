#pragma once

#include "ai/models/TokenUsage.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace ai::providers::openai_detail {

/**
 * @brief Extracts assistant text from a Responses API JSON document.
 *
 * This parser is intentionally separated from OpenAIProvider so response-shape
 * edge cases can be tested without making a real network request. It throws
 * std::runtime_error when the response is structurally invalid, incomplete, or
 * contains a refusal instead of usable assistant text.
 */
[[nodiscard]] std::string parseOpenAIResponseText(const nlohmann::json& responseJson);

/**
 * @brief Reads the human-friendly error message from an OpenAI error body.
 *
 * Non-2xx API responses usually contain an "error.message" field. Returning
 * only that field keeps diagnostics useful without echoing headers, request
 * payloads, or secrets.
 */
[[nodiscard]] std::string extractOpenAIErrorMessage(const std::string& responseBody);

/**
 * @brief The Backend's verdict on one received non-2xx OpenAI HTTP response.
 *
 * `code` is ALWAYS one of the stable Backend codes, chosen by the HTTP status (even for an empty or malformed
 * body): 401 provider.auth_failed, 429 provider.rate_limited, 5xx provider.server_error, any other status
 * provider.request_failed. The one refinement is a 429 whose STRUCTURED error.code / error.type names an exhausted
 * credit balance, spend/usage limit or quota: that is provider.insufficient_quota and `durableMessage` is the fixed,
 * bounded text for it. The provider's own message and the raw error.code are never part of the result.
 */
struct OpenAIHttpFailure {
    std::string code;
    std::string durableMessage;   // non-empty only for provider.insufficient_quota
};

/// Shared by the single-shot OpenAIProvider and the agent session. Call it only for a non-2xx response.
[[nodiscard]] OpenAIHttpFailure classifyOpenAIHttpFailure(int statusCode, const std::string& responseBody);

/// True exactly when the shared classifier calls the response a durable credit/quota refusal. The transport uses it
/// to refuse a retry of such a response (it will not clear by waiting); any other status keeps the retry policy.
[[nodiscard]] bool isDurableOpenAIQuotaRefusal(int statusCode, const std::string& responseBody);

/**
 * @brief Converts OpenAI token counters into the project-wide TokenUsage shape.
 *
 * The one OpenAI usage parser: the single-shot provider and the agent session
 * both call it, so the two paths can never disagree about a response.
 *
 *   TokenUsage::inputTokens             = usage.input_tokens (the FULL total)
 *   TokenUsage::outputTokens            = usage.output_tokens (reasoning tokens
 *                                         are already inside it)
 *   TokenUsage::cacheReadInputTokens    = usage.input_tokens_details.cached_tokens
 *   TokenUsage::cacheCreationInputTokens= usage.input_tokens_details.cache_write_tokens
 *
 * Missing or malformed usage fields are treated as zero values because usage is
 * optional diagnostic data, not the primary generated result; in particular a
 * model that does not report cache writes yields cacheCreationInputTokens == 0.
 */
[[nodiscard]] models::TokenUsage parseOpenAITokenUsage(const nlohmann::json& responseJson);

} // namespace ai::providers::openai_detail
