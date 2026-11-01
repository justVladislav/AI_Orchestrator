#include "ai/providers/OpenAIResponseParser.hpp"
#include "ai/providers/ProviderAgentSessionTypes.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ai::providers::openai_detail {

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

// A non-negative integer counter, or zero when the field is absent, not an
// object member, or not an unsigned integer (usage is optional diagnostic data).
std::uint64_t unsignedCounterOrZero(const nlohmann::json& value, const char* field)
{
    if (!value.is_object()) {
        return 0;
    }

    const auto it = value.find(field);
    if (it == value.end() || !it->is_number_unsigned()) {
        return 0;
    }

    return it->get<std::uint64_t>();
}

std::string incompleteReason(const nlohmann::json& responseJson)
{
    const auto details = responseJson.find("incomplete_details");
    if (details == responseJson.end() || !details->is_object()) {
        return {};
    }

    return jsonStringOrEmpty(*details, "reason");
}

[[noreturn]] void throwRefusal(const std::string& refusal)
{
    if (refusal.empty()) {
        throw std::runtime_error("OpenAI response contained a refusal.");
    }

    throw std::runtime_error("OpenAI response contained a refusal: " + refusal);
}

void appendMessageTextParts(const nlohmann::json& message, std::vector<std::string>& parts)
{
    const auto contentIt = message.find("content");
    if (contentIt == message.end() || !contentIt->is_array()) {
        return;
    }

    for (const auto& contentItem : *contentIt) {
        const std::string type = jsonStringOrEmpty(contentItem, "type");
        if (type == "output_text") {
            const std::string text = jsonStringOrEmpty(contentItem, "text");
            if (!text.empty()) {
                parts.push_back(text);
            }
        } else if (type == "refusal") {
            throwRefusal(jsonStringOrEmpty(contentItem, "refusal"));
        }
    }
}

std::string joinParts(const std::vector<std::string>& parts)
{
    std::ostringstream text;
    for (const auto& part : parts) {
        text << part;
    }
    return text.str();
}

} // namespace

std::string parseOpenAIResponseText(const nlohmann::json& responseJson)
{
    if (!responseJson.is_object()) {
        throw std::runtime_error("OpenAI response JSON root is not an object.");
    }

    const std::string status = jsonStringOrEmpty(responseJson, "status");
    if (status == "incomplete") {
        const std::string reason = incompleteReason(responseJson);
        if (reason.empty()) {
            throw std::runtime_error("OpenAI response is incomplete.");
        }
        throw std::runtime_error("OpenAI response is incomplete: " + reason);
    }

    const auto outputIt = responseJson.find("output");
    if (outputIt == responseJson.end() || !outputIt->is_array() || outputIt->empty()) {
        throw std::runtime_error("OpenAI response did not contain output text.");
    }

    std::vector<std::string> textParts;
    for (const auto& item : *outputIt) {
        const std::string type = jsonStringOrEmpty(item, "type");
        if (type == "message") {
            appendMessageTextParts(item, textParts);
        } else if (type == "refusal") {
            throwRefusal(jsonStringOrEmpty(item, "refusal"));
        }
    }

    std::string text = joinParts(textParts);
    if (text.empty()) {
        throw std::runtime_error("OpenAI response did not contain output text.");
    }

    return text;
}

std::string extractOpenAIErrorMessage(const std::string& responseBody)
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

        return jsonStringOrEmpty(*errorIt, "message");
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

namespace {

// The structured fields of an OpenAI error body: {"error": {"code": "...", "type": "..."}}. Only string members of an
// object under "error" are read; anything else (not JSON, no error object, a number where a string is expected)
// yields empty values. The human message is never consulted.
struct StructuredOpenAIError {
    std::string code;
    std::string type;
};

StructuredOpenAIError parseStructuredOpenAIError(const std::string& responseBody)
{
    StructuredOpenAIError parsed;
    try {
        const auto json = nlohmann::json::parse(responseBody);
        if (!json.is_object()) return parsed;
        const auto error = json.find("error");
        if (error == json.end() || !error->is_object()) return parsed;
        if (const auto code = error->find("code"); code != error->end() && code->is_string())
            parsed.code = code->get<std::string>();
        if (const auto type = error->find("type"); type != error->end() && type->is_string())
            parsed.type = type->get<std::string>();
    } catch (const std::exception&) {
        return {};
    }
    return parsed;
}

// Fixed text keyed by the RECOGNIZED code - never the provider's own text - saying only what the code proves:
// credit_balance_exhausted names the balance; the other codes name a limit or quota, not a zero balance.
std::string durableQuotaRefusalMessage(const StructuredOpenAIError& error)
{
    if (error.code == "credit_balance_exhausted")
        return "The provider account has no remaining credit balance (credit_balance_exhausted)";
    for (const char* spendOrUsage : {"organization_spend_limit_exceeded", "project_spend_limit_exceeded",
             "organization_usage_limit_exceeded"}) {
        if (error.code == spendOrUsage)
            return std::string("The provider reports that a spend or usage limit was reached (") + spendOrUsage + ")";
    }
    if (error.code == "insufficient_quota" || error.type == "insufficient_quota") {
        return "The provider reports insufficient quota or a billing limit (insufficient_quota); "
               "this alone does not prove a zero credit balance";
    }
    return {};
}

} // namespace

OpenAIHttpFailure classifyOpenAIHttpFailure(int statusCode, const std::string& responseBody)
{
    if (statusCode == 401) return {"provider.auth_failed", {}};
    if (statusCode == 429) {
        // HTTP 429 alone is not enough: billing/quota exhaustion shares the status with the temporary rate limit.
        // Only the structured code/type separates them; rate_limit_exceeded, slow_down and anything unstructured
        // stay a temporary rate limit.
        std::string durable = durableQuotaRefusalMessage(parseStructuredOpenAIError(responseBody));
        if (!durable.empty()) return {kProviderInsufficientQuotaCode, std::move(durable)};
        return {"provider.rate_limited", {}};
    }
    if (statusCode >= 500 && statusCode < 600) return {"provider.server_error", {}};
    return {"provider.request_failed", {}};
}

bool isDurableOpenAIQuotaRefusal(int statusCode, const std::string& responseBody)
{
    return statusCode >= 400 && classifyOpenAIHttpFailure(statusCode, responseBody).code == kProviderInsufficientQuotaCode;
}

models::TokenUsage parseOpenAITokenUsage(const nlohmann::json& responseJson)
{
    models::TokenUsage usage;

    if (!responseJson.is_object()) {
        return usage;
    }

    const auto usageIt = responseJson.find("usage");
    if (usageIt == responseJson.end() || !usageIt->is_object()) {
        return usage;
    }

    usage.inputTokens = unsignedCounterOrZero(*usageIt, "input_tokens");
    usage.outputTokens = unsignedCounterOrZero(*usageIt, "output_tokens");

    // The two cache counters are SUBSETS of input_tokens (OpenAI prompt
    // caching: ordinary input = input_tokens - cached_tokens -
    // cache_write_tokens), so inputTokens above stays the full total and is
    // never reduced here. Models that predate cache-write reporting simply
    // omit cache_write_tokens (or the whole details object) - that is zero,
    // not an error. They are copied verbatim even when they do not fit into
    // input_tokens; consumers saturate and flag such a report instead of this
    // parser silently repairing it.
    const auto detailsIt = usageIt->find("input_tokens_details");
    if (detailsIt != usageIt->end()) {
        usage.cacheReadInputTokens = unsignedCounterOrZero(*detailsIt, "cached_tokens");
        usage.cacheCreationInputTokens = unsignedCounterOrZero(*detailsIt, "cache_write_tokens");
    }

    return usage;
}

} // namespace ai::providers::openai_detail
