#include "ai/providers/AnthropicProvider.hpp"

#include "ai/http/HttpClient.hpp"
#include "ai/http/HttpTimeout.hpp"
#include "ai/providers/AnthropicResponseParser.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstdint>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ai::providers {

namespace {

constexpr const char* defaultEndpoint = "https://api.anthropic.com/v1/messages";
constexpr const char* defaultModel = "claude-haiku-4-5-20251001";
constexpr const char* defaultApiVersion = "2023-06-01";

std::string envOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string {} : std::string {value};
}

std::string metadataValue(
    const models::GenerationOptions& options,
    const std::string& primaryKey,
    const std::string& fallbackKey = {})
{
    const auto primary = options.metadata.find(primaryKey);
    if (primary != options.metadata.end() && !primary->second.empty()) {
        return primary->second;
    }

    if (!fallbackKey.empty()) {
        const auto fallback = options.metadata.find(fallbackKey);
        if (fallback != options.metadata.end() && !fallback->second.empty()) {
            return fallback->second;
        }
    }

    return {};
}

std::string resolveModel(
    const models::GenerationOptions& options,
    ModelType modelType)
{
    // A concrete ModelType is an explicit constructor choice. Only Unknown
    // delegates to runtime configuration and finally the provider default.
    switch (modelType) {
    case ModelTypeClaudeFable:
        return "claude-fable-5";
    case ModelTypeClaudeOpus:
        return "claude-opus-4-8";
    case ModelTypeClaudeHaiku:
        return "claude-haiku-4-5-20251001";
    case ModelTypeClaudeSonnet:
        return "claude-sonnet-5";
    case ModelTypeUnknown:
    default:
        break;
    }

    // Runtime catalog selections are carried per request. The real provider
    // adapter intentionally constructs AnthropicProvider with ModelTypeUnknown,
    // so ignoring this field silently routed every Fable/Sonnet/Opus request
    // to the provider default (Haiku).
    if (!options.model.empty()) {
        return options.model;
    }

    const std::string envModel = envOrEmpty("ANTHROPIC_MODEL");
    if (!envModel.empty()) {
        return envModel;
    }

    return defaultModel;
}

std::string resolveEndpoint(const models::GenerationOptions& options, const std::string& configuredEndpoint)
{
    const std::string metadataEndpoint = metadataValue(options, "anthropic.endpoint", "endpoint");
    if (!metadataEndpoint.empty()) {
        return metadataEndpoint;
    }

    if (!configuredEndpoint.empty()) {
        return configuredEndpoint;
    }

    return defaultEndpoint;
}

std::string resolveApiVersion(const models::GenerationOptions& options, const std::string& configuredApiVersion)
{
    const std::string metadataVersion = metadataValue(options, "anthropic.version", "anthropic_version");
    if (!metadataVersion.empty()) {
        return metadataVersion;
    }

    if (!configuredApiVersion.empty()) {
        return configuredApiVersion;
    }

    return defaultApiVersion;
}

std::uint32_t resolveMaxTokens(const models::GenerationOptions& options)
{
    return options.maxOutputTokens > 0 ? options.maxOutputTokens : 1024;
}

std::string roleToAnthropicRole(models::MessageRole role)
{
    switch (role) {
    case models::MessageRole::Assistant:
        return "assistant";
    case models::MessageRole::User:
    case models::MessageRole::Tool:
    case models::MessageRole::System:
        return "user";
    }

    return "user";
}

std::string collectSystemPrompt(const models::ModelRequest& request)
{
    std::ostringstream system;
    bool first = true;

    for (const auto& message : request.messages) {
        if (message.role != models::MessageRole::System || message.content.empty()) {
            continue;
        }

        if (!first) {
            system << "\n\n";
        }
        system << message.content;
        first = false;
    }

    const std::string metadataSystem = metadataValue(request.options, "anthropic.system", "system");
    if (!metadataSystem.empty()) {
        if (!first) {
            system << "\n\n";
        }
        system << metadataSystem;
    }

    return system.str();
}

nlohmann::json buildMessages(const models::ModelRequest& request)
{
    nlohmann::json messages = nlohmann::json::array();

    // Anthropic uses a top-level "system" field for instructions that apply to
    // the whole request. Only conversational turns belong in messages[].
    for (const auto& message : request.messages) {
        if (message.role == models::MessageRole::System || message.content.empty()) {
            continue;
        }

        messages.push_back({
            {"role", roleToAnthropicRole(message.role)},
            {"content", message.content}
        });
    }

    return messages;
}

nlohmann::json buildRequestBody(const models::ModelRequest& request, const std::string& model)
{
    nlohmann::json body;
    body["model"] = model;
    // Anthropic's Messages API uses max_tokens, not OpenAI's max_output_tokens.
    body["max_tokens"] = resolveMaxTokens(request.options);

    const std::string systemPrompt = collectSystemPrompt(request);
    if (!systemPrompt.empty()) {
        if (!models::isKnown(request.options.promptCache.mode)) {
            throw std::runtime_error("Anthropic prompt-cache policy is unknown.");
        }
        if (!request.options.promptCache.enabled()) {
            // Compatibility path: preserve the exact pre-ACD-4 wire shape.
            body["system"] = systemPrompt;
        } else {
            nlohmann::json block = {{"type", "text"}, {"text", systemPrompt}};
            block["cache_control"] = {{"type", "ephemeral"}};
            if (request.options.promptCache.mode
                == models::PromptCacheMode::EphemeralOneHour) {
                block["cache_control"]["ttl"] = "1h";
            }
            body["system"] = nlohmann::json::array({std::move(block)});
        }
    } else if (request.options.promptCache.enabled()) {
        throw std::runtime_error(
            "Anthropic prompt caching requires a non-empty stable system prompt.");
    }

    nlohmann::json messages = buildMessages(request);
    if (messages.empty()) {
        throw std::runtime_error("Anthropic request messages are empty.");
    }

    body["messages"] = std::move(messages);
    return body;
}

models::ModelResponse failureResponse(const std::string& model, std::string message)
{
    return models::ModelResponse {
        {},
        "anthropic",
        model,
        {},
        false,
        std::move(message)
    };
}

std::string safeBodySnippet(const std::string& body)
{
    constexpr std::size_t maxSnippetSize = 300;
    if (body.empty()) {
        return {};
    }

    if (body.size() <= maxSnippetSize) {
        return body;
    }

    return body.substr(0, maxSnippetSize) + "...";
}

std::string httpFailureMessage(const http::HttpResponse& response)
{
    std::ostringstream message;
    message << "Anthropic request failed with HTTP " << response.statusCode;

    const std::string apiMessage = anthropic_detail::extractAnthropicErrorMessage(response.body);
    if (!apiMessage.empty()) {
        message << ": " << apiMessage;
        return message.str();
    }

    const std::string snippet = safeBodySnippet(response.body);
    if (!snippet.empty()) {
        message << ": " << snippet;
    }

    return message.str();
}

std::string transportFailureMessage(
    const std::string& model,
    const models::GenerationOptions& options,
    const http::HttpResponse& response)
{
    std::ostringstream message;
    message << "Anthropic transport error";

    const std::string stage = metadataValue(options, "workflow.stage", "stage");
    if (!stage.empty()) {
        message << " during " << stage;
    }

    message << " for model " << model << ": " << response.errorMessage;
    return message.str();
}

} // namespace

namespace anthropic_detail {

std::string renderAnthropicRequestBody(
    const models::ModelRequest& request,
    const std::string& model)
{
    return buildRequestBody(request, model).dump();
}

} // namespace anthropic_detail

AnthropicProvider::AnthropicProvider()
    : AnthropicProvider(ModelTypeUnknown)
{
}

AnthropicProvider::AnthropicProvider(ModelType modelType)
    : apiKey_(envOrEmpty("ANTHROPIC_API_KEY"))
    , endpoint_(defaultEndpoint)
    , apiVersion_(defaultApiVersion)
    , modelType_(modelType)
{
}

AnthropicProvider::AnthropicProvider(std::string apiKey, ModelType modelType)
    : apiKey_(std::move(apiKey))
    , endpoint_(defaultEndpoint)
    , apiVersion_(defaultApiVersion)
    , modelType_(modelType)
{
}

std::string AnthropicProvider::name() const
{
    return "anthropic";
}

models::ModelResponse AnthropicProvider::generate(const models::ModelRequest& request)
{
    const std::string model = resolveModel(request.options, modelType_);

    if (apiKey_.empty()) {
        return failureResponse(
            model,
            "Anthropic API key is missing. Set the ANTHROPIC_API_KEY environment variable.");
    }

    try {
        const std::string requestBody = anthropic_detail::renderAnthropicRequestBody(request, model);

        http::HttpRequest httpRequest;
        httpRequest.url = resolveEndpoint(request.options, endpoint_);
        httpRequest.method = "POST";
        http::applyTimeoutOptions(httpRequest, request.options, "anthropic");
        // Native Anthropic authentication uses x-api-key plus an API version
        // header; do not use an Authorization bearer header for this API.
        httpRequest.headers = {
            {"x-api-key", apiKey_},
            {"anthropic-version", resolveApiVersion(request.options, apiVersion_)},
            {"content-type", "application/json"}
        };
        httpRequest.body = requestBody;

        http::HttpClient client;
        const http::HttpResponse httpResponse = client.send(httpRequest);

        if (httpResponse.hasTransportError()) {
            return failureResponse(model, transportFailureMessage(model, request.options, httpResponse));
        }

        if (!httpResponse.isSuccess()) {
            return failureResponse(model, httpFailureMessage(httpResponse));
        }

        if (httpResponse.body.empty()) {
            return failureResponse(model, "Anthropic response body was empty.");
        }

        // Parse after status validation so Anthropic error bodies are reported
        // through httpFailureMessage() instead of as malformed success JSON.
        nlohmann::json responseJson;
        try {
            responseJson = nlohmann::json::parse(httpResponse.body);
        } catch (const nlohmann::json::exception& ex) {
            return failureResponse(model, std::string {"Anthropic response JSON parse failed: "} + ex.what());
        }

        const std::string text = anthropic_detail::parseAnthropicResponseText(responseJson);
        models::TokenUsage usage;
        try {
            usage = anthropic_detail::parseAnthropicTokenUsage(
                responseJson,
                request.options.promptCache.mode
                    == models::PromptCacheMode::EphemeralOneHour);
        } catch (const std::exception& ex) {
            return failureResponse(
                model,
                std::string {"Anthropic response usage parse failed: "} + ex.what());
        }
        return models::ModelResponse {
            text,
            "anthropic",
            model,
            std::move(usage),
            true,
            {}
        };
    } catch (const std::exception& ex) {
        return failureResponse(model, ex.what());
    }
}

} // namespace ai::providers
