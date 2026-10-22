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
