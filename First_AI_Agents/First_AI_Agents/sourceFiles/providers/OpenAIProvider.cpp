#include "ai/providers/OpenAIProvider.hpp"

#include "ai/http/HttpClient.hpp"
#include "ai/http/HttpTimeout.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ai::providers {

namespace {

constexpr const char* defaultEndpoint = "https://api.openai.com/v1/responses";
constexpr const char* defaultModel = "gpt-5.4";

std::string envOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string {} : std::string {value};
}

bool isUsableOpenAIModel(const std::string& model)
{
    return !model.empty()
        && model != "mock-default"
        && model.rfind("mock-", 0) != 0;
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

std::string resolveModel(const models::GenerationOptions& options, ModelType modelType)
{
    // A concrete constructor ModelType should win over provider-neutral options.
    // Unknown keeps the older per-request/env/default fallback behavior.
    switch (modelType) {
    case ModelTypeOpenAi56Sol:
        return "gpt-5.6-sol";
    case ModelTypeOpenAi56Terra:
        return "gpt-5.6-terra";
    case ModelTypeOpenAi56Luna:
        return "gpt-5.6-luna";
    case ModelTypeOpenAi54:
        return "gpt-5.4";
    case ModelTypeOpenAi5:
        return "gpt-5";
    case ModelTypeOpenAi5Mini:
        return "gpt-5-mini";
    case ModelTypeOpenAi5Nano:
        return "gpt-5-nano";
    case ModelTypeUnknown:
    default:
        break;
    }

    // Mock agent configs use mock-* model names. If someone wires OpenAIProvider
    // to such a config, fall back to OPENAI_MODEL/default instead of sending an
    // invalid mock model name to OpenAI.
    if (isUsableOpenAIModel(options.model)) {
        return options.model;
    }

    const std::string envModel = envOrEmpty("OPENAI_MODEL");
    if (!envModel.empty()) {
        return envModel;
    }

    return defaultModel;
}

std::string resolveEndpoint(const models::GenerationOptions& options, const std::string& configuredEndpoint)
{
    // Metadata override keeps endpoint experimentation local to GenerationOptions
    // until the project has a full provider config loader.
    const std::string metadataEndpoint = metadataValue(options, "openai.endpoint", "endpoint");
    if (!metadataEndpoint.empty()) {
        return metadataEndpoint;
    }

    if (!configuredEndpoint.empty()) {
        return configuredEndpoint;
    }

    return defaultEndpoint;
}

std::string roleToOpenAIInputRole(models::MessageRole role)
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

std::string collectInstructions(const models::ModelRequest& request)
{
    std::ostringstream instructions;
    bool first = true;

    // Responses API supports top-level instructions. System messages belong
    // there instead of being mixed into user input, preserving role boundaries.
    for (const auto& message : request.messages) {
        if (message.role != models::MessageRole::System || message.content.empty()) {
            continue;
        }

        if (!first) {
            instructions << "\n\n";
        }
        instructions << message.content;
        first = false;
    }

    // Metadata instructions provide a narrow extension point for future provider
    // configuration without changing ModelRequest or Agent.
    const std::string metadataInstructions = metadataValue(
        request.options,
        "openai.instructions",
        "instructions");
    if (!metadataInstructions.empty()) {
        if (!first) {
            instructions << "\n\n";
        }
        instructions << metadataInstructions;
    }

    return instructions.str();
}

nlohmann::json buildInputItems(const models::ModelRequest& request)
{
    nlohmann::json input = nlohmann::json::array();

    // Only conversational input goes into the Responses API input array.
    // System-level guidance is handled separately by collectInstructions().
    for (const auto& message : request.messages) {
        if (message.role == models::MessageRole::System || message.content.empty()) {
            continue;
        }

        input.push_back({
            {"role", roleToOpenAIInputRole(message.role)},
            {"content", message.content}
        });
    }

    return input;
}

nlohmann::json buildRequestBody(const models::ModelRequest& request, const std::string& model)
{
    nlohmann::json body;
    body["model"] = model;

    // Keep request JSON construction in one place so OpenAI-specific field names
    // do not spread into Agent, Orchestrator, or tests that do not need them.
    const std::string instructions = collectInstructions(request);
    if (!instructions.empty()) {
        body["instructions"] = instructions;
    }

    nlohmann::json input = buildInputItems(request);
    if (input.empty()) {
        throw std::runtime_error("OpenAI request input is empty.");
    }

    body["input"] = std::move(input);

    if (request.options.maxOutputTokens > 0) {
        body["max_output_tokens"] = request.options.maxOutputTokens;
    }

    const std::string reasoningEffort = metadataValue(
        request.options,
        "openai.reasoning_effort",
        "reasoning_effort");
    if (!reasoningEffort.empty()) {
        body["reasoning"] = {{"effort", reasoningEffort}};
    }

    return body;
}

models::ModelResponse failureResponse(const std::string& model, std::string message)
{
    return models::ModelResponse {
        {},
        "openai",
        model,
        {},
        false,
        std::move(message)
    };
}

std::string httpFailureMessage(const http::HttpResponse& response)
{
    std::ostringstream message;
    message << "OpenAI request failed with HTTP " << response.statusCode;

    // OpenAI usually returns {"error": {"message": ...}} for non-2xx responses.
    // Extracting only that message gives useful diagnostics without echoing
    // request headers or secrets.
    const std::string apiMessage = openai_detail::extractOpenAIErrorMessage(response.body);
    if (!apiMessage.empty()) {
        message << ": " << apiMessage;
    }

    return message.str();
}

std::string transportFailureMessage(
    const std::string& model,
    const models::GenerationOptions& options,
    const http::HttpResponse& response)
{
    std::ostringstream message;
    message << "OpenAI transport error";

    const std::string stage = metadataValue(options, "workflow.stage", "stage");
    if (!stage.empty()) {
        message << " during " << stage;
    }

    message << " for model " << model << ": " << response.errorMessage;
    return message.str();
}

} // namespace
 
OpenAIProvider::OpenAIProvider()
    : OpenAIProvider(ModelTypeUnknown)
{
}

OpenAIProvider::OpenAIProvider(const ModelType& modelType)
    : apiKey_(envOrEmpty("OPENAI_API_KEY"))
    , endpoint_(defaultEndpoint)
    , modelType_(modelType)
{
}

OpenAIProvider::OpenAIProvider(std::string apiKey, const ModelType& modelType)
    : apiKey_(std::move(apiKey))
    , endpoint_(defaultEndpoint)
    , modelType_(modelType)
{
}

std::string OpenAIProvider::name() const
{
    return "openai";
}

models::ModelResponse OpenAIProvider::generate(const models::ModelRequest& request)
{
    const std::string model = resolveModel(request.options, modelType_);

    if (apiKey_.empty()) {
        return failureResponse(
            model,
            "OpenAI API key is missing. Set the OPENAI_API_KEY environment variable.");
    }

    try {
        const nlohmann::json requestBody = buildRequestBody(request, model);

        http::HttpRequest httpRequest;
        httpRequest.url = resolveEndpoint(request.options, endpoint_);
        httpRequest.method = "POST";
        http::applyTimeoutOptions(httpRequest, request.options, "openai");
        // Authorization is attached at the last possible layer. Higher-level
        // code only passes neutral model requests and never sees the raw header.
        httpRequest.headers = {
            {"Authorization", "Bearer " + apiKey_},
            {"Content-Type", "application/json"}
        };
        httpRequest.body = requestBody.dump();

        http::HttpClient client;
        const http::HttpResponse httpResponse = client.send(httpRequest);

        if (httpResponse.hasTransportError()) {
            return failureResponse(model, transportFailureMessage(model, request.options, httpResponse));
        }

        if (!httpResponse.isSuccess()) {
            return failureResponse(model, httpFailureMessage(httpResponse));
        }

        if (httpResponse.body.empty()) {
            return failureResponse(model, "OpenAI response body was empty.");
        }

        // Parse after status validation so API error bodies are handled by
        // httpFailureMessage() instead of being reported as malformed success JSON.
        nlohmann::json responseJson;
        try {
            responseJson = nlohmann::json::parse(httpResponse.body);
        } catch (const nlohmann::json::exception& ex) {
            return failureResponse(model, std::string {"OpenAI response JSON parse failed: "} + ex.what());
        }

        const std::string text = openai_detail::parseOpenAIResponseText(responseJson);
        return models::ModelResponse {
            text,
            "openai",
            model,
            openai_detail::parseOpenAITokenUsage(responseJson),
            true,
            {}
        };
    } catch (const std::exception& ex) {
        return failureResponse(model, ex.what());
    }
}

} // namespace ai::providers
