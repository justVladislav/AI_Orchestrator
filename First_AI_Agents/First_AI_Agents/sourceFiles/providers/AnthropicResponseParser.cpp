#include "ai/providers/AnthropicResponseParser.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <limits>
#include <vector>

namespace ai::providers::anthropic_detail {

namespace {

std::string jsonStringOrEmpty(const nlohmann::json& value, const char* field)
{
    if (!value.is_object()) {
        return {};
    }

    const auto it = value.find(field);
    if (it == value.end() || !it->is_string()) {
        return {};
    }

    return it->get<std::string>();
}

std::string joinParts(const std::vector<std::string>& parts)
{
    std::ostringstream text;
    for (const auto& part : parts) {
        text << part;
    }
    return text.str();
}

bool isToolBlockType(const std::string& type)
{
    return type == "tool_use"
        || type == "server_tool_use"
        || type == "mcp_tool_use";
}

} // namespace

std::string parseAnthropicResponseText(const nlohmann::json& responseJson)
{
    if (!responseJson.is_object()) {
        throw std::runtime_error("Anthropic response JSON root is not an object.");
    }

    const std::string stopReason = jsonStringOrEmpty(responseJson, "stop_reason");
    if (stopReason == "refusal") {
        throw std::runtime_error("Anthropic refused to complete the request.");
    }

    const auto contentIt = responseJson.find("content");
    if (contentIt == responseJson.end() || !contentIt->is_array()) {
        throw std::runtime_error("Anthropic response did not contain a content array.");
    }

    std::vector<std::string> textParts;
    bool sawToolBlock = false;

    for (const auto& contentItem : *contentIt) {
        // Preserve provider order: one assistant response can be split across
        // several text blocks, especially as APIs add richer content blocks.
        const std::string type = jsonStringOrEmpty(contentItem, "type");
        if (type == "text") {
            const std::string text = jsonStringOrEmpty(contentItem, "text");
            if (!text.empty()) {
                textParts.push_back(text);
            }
        } else if (isToolBlockType(type)) {
            sawToolBlock = true;
        }
        // Unknown blocks are intentionally ignored. The provider only exposes
        // basic text generation; tools, thinking, and streaming are out of scope.
    }

    if (stopReason == "tool_use" || sawToolBlock) {
        throw std::runtime_error(
            "Anthropic returned a tool-use response, but tool execution is not implemented.");
    }

    if (stopReason == "max_tokens") {
        throw std::runtime_error("Anthropic response reached max_tokens before completing the response.");
    }

    std::string text = joinParts(textParts);
    if (!text.empty()) {        
        return text;
    }

    throw std::runtime_error("Anthropic response did not contain text content blocks.");
}

std::string extractAnthropicErrorMessage(const std::string& responseBody)
{
    if (responseBody.empty()) {
        return {};
    }

    try {
        const nlohmann::json body = nlohmann::json::parse(responseBody);
        const auto errorIt = body.find("error");
        if (errorIt == body.end() || !errorIt->is_object()) {
            return {};
        }

        const std::string type = jsonStringOrEmpty(*errorIt, "type");
        const std::string message = jsonStringOrEmpty(*errorIt, "message");
        const std::string requestId = jsonStringOrEmpty(body, "request_id");

        std::ostringstream result;
        if (!type.empty()) {
            result << '[' << type << ']';
        }
        if (!message.empty()) {
            if (!type.empty()) {
                result << ' ';
            }
            result << message;
        }
        if (!requestId.empty()) {
            if (!type.empty() || !message.empty()) {
                result << ' ';
            }
            result << "(request_id: " << requestId << ')';
        }

        return result.str();
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

models::TokenUsage parseAnthropicTokenUsage(
    const nlohmann::json& responseJson,
    bool oneHourCacheCheckpointUsed)
{
    models::TokenUsage usage;

    if (!responseJson.is_object()) {
        throw std::runtime_error("Anthropic response must be an object");
    }

    const auto usageIt = responseJson.find("usage");
    if (usageIt == responseJson.end()) {
        throw std::runtime_error("Anthropic response usage is missing");
    }
    if (!usageIt->is_object()) {
        throw std::runtime_error("Anthropic usage must be an object");
    }

    const auto counter = [&](const nlohmann::json& object, const char* name, bool required = false) {
        const auto found = object.find(name);
        if (found == object.end()) {
            if (required) {
                throw std::runtime_error(
                    std::string("Anthropic usage counter is missing: ") + name);
            }
            return std::uint64_t {0};
        }
        if (!found->is_number_unsigned()) {
            throw std::runtime_error(
                std::string("Anthropic usage counter is not an unsigned integer: ") + name);
        }
        return found->get<std::uint64_t>();
    };

    const std::uint64_t fresh = counter(*usageIt, "input_tokens", true);
    usage.outputTokens = counter(*usageIt, "output_tokens", true);
    usage.cacheCreationInputTokens = counter(*usageIt, "cache_creation_input_tokens");
    usage.cacheReadInputTokens = counter(*usageIt, "cache_read_input_tokens");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (usage.cacheCreationInputTokens > maximum - fresh
        || usage.cacheReadInputTokens
            > maximum - fresh - usage.cacheCreationInputTokens) {
        throw std::runtime_error("Anthropic input usage counters overflow");
    }
    usage.inputTokens = fresh + usage.cacheCreationInputTokens
        + usage.cacheReadInputTokens;

    const auto durationIt = usageIt->find("cache_creation");
    if (durationIt != usageIt->end()) {
        if (!durationIt->is_object()) {
            throw std::runtime_error("Anthropic cache_creation usage must be an object");
        }
        usage.cacheCreationDuration.known = true;
        usage.cacheCreationDuration.fiveMinuteTokens =
            counter(*durationIt, "ephemeral_5m_input_tokens");
        usage.cacheCreationDuration.oneHourTokens =
            counter(*durationIt, "ephemeral_1h_input_tokens");
    }
    if (oneHourCacheCheckpointUsed && usage.cacheCreationInputTokens > 0
        && durationIt == usageIt->end()) {
        usage.accountingTrusted = false;
    }
    const auto& duration = usage.cacheCreationDuration;
    if (!duration.known
        && (duration.fiveMinuteTokens != 0 || duration.oneHourTokens != 0)) {
        throw std::runtime_error(
            "Anthropic cache duration detail exists without a known partition");
    }
    if (duration.known
        && (duration.fiveMinuteTokens > usage.cacheCreationInputTokens
            || duration.oneHourTokens
                != usage.cacheCreationInputTokens - duration.fiveMinuteTokens)) {
        throw std::runtime_error(
            "Anthropic cache duration partition does not match cache creation input");
    }

    return usage;
}

} // namespace ai::providers::anthropic_detail
