#pragma once

#include "ai/models/TokenUsage.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace ai::providers::anthropic_detail {

/**
 * @brief Extracts assistant text from a native Anthropic Messages API response.
 *
 * The parser collects only content blocks whose type is "text". Unknown blocks
 * are skipped because Anthropic responses may contain tool, thinking, or future
 * block types that this basic text provider does not implement yet.
 */
[[nodiscard]] std::string parseAnthropicResponseText(const nlohmann::json& responseJson);

/**
 * @brief Extracts a safe, human-readable message from an Anthropic error body.
 *
 * Anthropic error responses commonly include error.type, error.message, and
 * request_id. This helper returns those diagnostics without exposing request
 * headers or credentials.
 */
[[nodiscard]] std::string extractAnthropicErrorMessage(const std::string& responseBody);

/**
 * @brief Converts Anthropic usage counters into the project-wide TokenUsage shape.
 */
[[nodiscard]] models::TokenUsage parseAnthropicTokenUsage(
    const nlohmann::json& responseJson,
    bool oneHourCacheCheckpointUsed = false);

} // namespace ai::providers::anthropic_detail
