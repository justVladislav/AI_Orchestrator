#include "ai/providers/OpenAIAgentSession.hpp"
#include "ai/http/HttpRequest.hpp"
#include "ai/http/HttpResponse.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"
#include "ai/providers/StrictSchemaValidation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace ai::providers {

namespace {

template <typename T>
common::Result<T> httpStatusError(int statusCode, const std::string& responseBody)
{
    // OpenAI returns {"error": {"message": ...}} for non-2xx responses; without
    // this, every failure collapsed into an undiagnosable "Provider returned
    // HTTP 400" with no way to tell a rejected parameter from a bad model id
    // from anything else. Mirrors OpenAIProvider.cpp's httpFailureMessage().
    const std::string apiMessage = openai_detail::extractOpenAIErrorMessage(responseBody);
    auto withDetail = [&](std::string message) {
        if (!apiMessage.empty()) message += ": " + apiMessage;
        return message;
    };
    if (statusCode == 401)
        return common::Result<T>::failure(
            {"provider.auth_failed", withDetail("Authentication failed")});
    if (statusCode == 429)
        return common::Result<T>::failure(
            {"provider.rate_limited", withDetail("Rate limit exceeded")});
    if (statusCode >= 500 && statusCode < 600)
        return common::Result<T>::failure(
            {"provider.server_error",
             withDetail("Provider returned HTTP " + std::to_string(statusCode))});
    return common::Result<T>::failure(
        {"provider.request_failed",
         withDetail("Provider returned HTTP " + std::to_string(statusCode))});
}

common::Result<void> validateToolResultCorrelation(
    const std::vector<application::AgentToolCall>& pendingCalls,
    const std::vector<application::AgentToolResult>& results)
{
    if (results.empty())
        return common::Result<void>::failure(
            {"agent_session.empty_tool_results", "no tool results were submitted"});

    for (const auto& result : results) {
        const auto it = std::find_if(pendingCalls.begin(), pendingCalls.end(),
            [&](const application::AgentToolCall& c) { return c.callId == result.callId; });
        if (it == pendingCalls.end())
            return common::Result<void>::failure(
                {"agent_session.unknown_tool_result", "result references an unknown callId"});
        if (it->toolName != result.toolName)
            return common::Result<void>::failure(
                {"agent_session.tool_name_mismatch",
                 "result toolName does not match the call"});
    }

    for (std::size_t i = 0; i < results.size(); ++i)
        for (std::size_t j = i + 1; j < results.size(); ++j)
            if (results[i].callId == results[j].callId)
                return common::Result<void>::failure(
                    {"agent_session.duplicate_tool_result", "same callId submitted twice"});

    for (const auto& call : pendingCalls) {
        const auto it = std::find_if(results.begin(), results.end(),
            [&](const application::AgentToolResult& r) { return r.callId == call.callId; });
        if (it == results.end())
            return common::Result<void>::failure(
                {"agent_session.missing_tool_result", "a required call has no result"});
    }

    return common::Result<void>::ok();
}

} // namespace

OpenAIAgentSession::OpenAIAgentSession(
    std::string apiKey,
    std::string endpoint,
    std::shared_ptr<http::IHttpClient> httpClient)
    : apiKey_(std::move(apiKey))
    , endpoint_(std::move(endpoint))
    , httpClient_(std::move(httpClient))
{
}

// static
common::Result<std::string> OpenAIAgentSession::buildToolsJson(
    const std::vector<application::AgentToolDefinition>& tools,
    const NextTurnToolPolicy* policy)
{
    auto arr = nlohmann::json::array();
    for (const auto& def : tools) {
        if (policy && !policy->allowedToolNames.empty()) {
            const auto& allowed = policy->allowedToolNames;
            if (std::find(allowed.begin(), allowed.end(), def.name) == allowed.end())
                continue;
        }
        nlohmann::json tool = {
            {"type", "function"},
            {"name", def.name},
            {"description", def.description}
        };
        nlohmann::json parsedSchema;
        bool parsed = false;
        if (!def.inputSchemaJson.empty()) {
            try {
                parsedSchema = nlohmann::json::parse(def.inputSchemaJson);
                parsed = true;
            } catch (...) {
                parsed = false;
            }
        }
        if (def.strictInputSchema) {
            // A strict tool must never silently degrade to a best-effort
            // empty schema: a malformed or unsupported schema fails this
            // session before any HTTP call, so the provider is never asked
            // to honor a guarantee we cannot actually make.
            if (!parsed) {
                return common::Result<std::string>::failure(
                    {"agent_session.malformed_strict_schema",
                     "Strict tool schema is not valid JSON: " + def.name});
            }
            const auto valid = validateStrictJsonSchema(parsedSchema, def.name);
            if (!valid.success()) {
                return common::Result<std::string>::failure(*valid.error());
            }
            tool["parameters"] = std::move(parsedSchema);
            tool["strict"] = true;
        } else {
            tool["parameters"] = parsed ? std::move(parsedSchema) : nlohmann::json::object();
        }
        arr.push_back(std::move(tool));
    }
    return common::Result<std::string>::ok(arr.dump());
}

common::Result<ProviderAgentTurnResponse> OpenAIAgentSession::beginSession(
    const ProviderAgentSessionRequest& request,
    std::stop_token stopToken)
{
    if (state_ != State::Idle)
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"agent_session.invalid_state",
             "beginSession called after session was already started"});

    if (auto v = validateSessionRequest(request); !v.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

    model_ = request.apiModelId;
    systemPrompt_ = request.systemPrompt;
    tools_ = request.tools;
    auto toolsJsonResult = buildToolsJson(tools_);
    if (!toolsJsonResult.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*toolsJsonResult.error());
    toolsJson_ = std::move(toolsJsonResult.value());
    maxOutputTokens_ = request.maxOutputTokens;
    maxToolCallsPerTurn_ = request.maxToolCallsPerTurn;
    turnTimeout_ = request.turnTimeout;
    disallowParallelToolCalls_ = request.disallowParallelToolCalls;

    // An optional first-turn policy is validated against the advertised
    // tools before any request is built, exactly like a NextTurnToolPolicy.
    const NextTurnToolPolicy* firstTurnPolicy = request.firstTurnToolPolicy.has_value()
        ? &*request.firstTurnToolPolicy : nullptr;
    if (firstTurnPolicy != nullptr) {
        for (const auto& name : firstTurnPolicy->allowedToolNames) {
            const bool known = std::any_of(request.tools.begin(), request.tools.end(),
                [&](const application::AgentToolDefinition& def) { return def.name == name; });
            if (!known)
                return common::Result<ProviderAgentTurnResponse>::failure(
                    {"agent_session.invalid_first_turn_policy",
                     "firstTurnToolPolicy names a tool never advertised to beginSession"});
        }
    }

    nlohmann::json body;
    body["model"] = model_;
    if (!systemPrompt_.empty())
        body["instructions"] = systemPrompt_;
    body["input"] = nlohmann::json::array({
        nlohmann::json{{"role", "user"}, {"content", request.userContent}}
    });
    bool disableParallel = disallowParallelToolCalls_;
    if (!request.tools.empty()) {
        std::string turnToolsJson = toolsJson_;
        // This session contract never permits a plain-text final answer: every
        // caller advertises only read-only inspect tools plus one or more
        // terminal declarative tools and requires exactly one terminal call.
        // Without this, the model is free to stop with prose instead of
        // calling a tool, which surfaced as a real-Run planner.missing_submission
        // failure even after two independent inspection/repair turns.
        nlohmann::json toolChoice = "required";
        if (firstTurnPolicy != nullptr) {
            auto filtered = buildToolsJson(tools_, firstTurnPolicy);
            if (!filtered.success())
                return common::Result<ProviderAgentTurnResponse>::failure(*filtered.error());
            if (!filtered.value().empty() && filtered.value() != "[]") {
                turnToolsJson = std::move(filtered.value());
                // Same wire contract as submitToolResults(): exactly one
                // allowed tool is forced by name, several use `required`
                // within the filtered catalog.
                if (firstTurnPolicy->allowedToolNames.size() == 1) {
                    toolChoice = nlohmann::json {
                        {"type", "function"},
                        {"name", firstTurnPolicy->allowedToolNames.front()}};
                }
                if (firstTurnPolicy->disallowParallelToolCalls) disableParallel = true;
            }
        }
        body["tools"] = nlohmann::json::parse(turnToolsJson);
        body["tool_choice"] = std::move(toolChoice);
    }
    if (disableParallel) {
        body["parallel_tool_calls"] = false;
    }
    body["max_output_tokens"] = maxOutputTokens_;

    return doTurn(body.dump(), std::move(stopToken));
}

common::Result<ProviderAgentTurnResponse> OpenAIAgentSession::submitToolResults(
    std::vector<application::AgentToolResult> results,
    std::stop_token stopToken,
    std::optional<NextTurnToolPolicy> nextTurnPolicy)
{
    if (state_ == State::Idle)
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"agent_session.not_started", "beginSession was never called"});
    if (state_ == State::Terminal)
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"agent_session.already_terminal", "session has already finished"});

    if (auto v = validateToolResultCorrelation(pendingCalls_, results); !v.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

    if (nextTurnPolicy.has_value()) {
        for (const auto& name : nextTurnPolicy->allowedToolNames) {
            const bool known = std::any_of(tools_.begin(), tools_.end(),
                [&](const application::AgentToolDefinition& def) { return def.name == name; });
            if (!known)
                return common::Result<ProviderAgentTurnResponse>::failure(
                    {"agent_session.invalid_next_turn_policy",
                     "nextTurnPolicy names a tool never advertised to beginSession"});
        }
    }

    auto inputArr = nlohmann::json::array();
    for (const auto& result : results) {
        nlohmann::json item;
        item["type"] = "function_call_output";
        item["call_id"] = result.callId;
        if (result.success) {
            item["output"] = result.contentJson.empty() ? "{}" : result.contentJson;
        } else {
            // Responses API has no is_error field; embed error code as output. A
            // Backend-authored explanation of the failure (for example the allowed paths of a
            // denied read) travels with the code; a failure without one keeps the bare code.
            std::string detail = "error: " + (result.errorCode.empty()
                ? std::string("tool execution failed")
                : result.errorCode);
            if (!result.contentJson.empty()) detail += "\n" + result.contentJson;
            item["output"] = std::move(detail);
        }
        inputArr.push_back(std::move(item));
    }

    nlohmann::json body;
    body["model"] = model_;
    body["previous_response_id"] = previousResponseId_;
    body["input"] = std::move(inputArr);
    if (nextTurnPolicy.has_value()) {
        auto turnToolsJson = buildToolsJson(tools_, &(*nextTurnPolicy));
        if (!turnToolsJson.success())
            return common::Result<ProviderAgentTurnResponse>::failure(*turnToolsJson.error());
        if (!turnToolsJson.value().empty() && turnToolsJson.value() != "[]") {
            body["tools"] = nlohmann::json::parse(turnToolsJson.value());
            // Keep the provider-neutral NextTurnToolPolicy structurally
            // equivalent across adapters: when the Backend allows exactly
            // one tool, force that named function rather than merely asking
            // for some tool from a filtered catalog. With several allowed
            // tools, `required` preserves the intentional choice among them.
            body["tool_choice"] = nextTurnPolicy->allowedToolNames.size() == 1
                ? nlohmann::json {
                    {"type", "function"},
                    {"name", nextTurnPolicy->allowedToolNames.front()}}
                : nlohmann::json("required");
        }
    } else if (!toolsJson_.empty()) {
        body["tools"] = nlohmann::json::parse(toolsJson_);
        body["tool_choice"] = "required";
    }
    if (disallowParallelToolCalls_
            || (nextTurnPolicy.has_value() && nextTurnPolicy->disallowParallelToolCalls)) {
        body["parallel_tool_calls"] = false;
    }
    body["max_output_tokens"] = maxOutputTokens_;

    return doTurn(body.dump(), std::move(stopToken));
}

common::Result<ProviderAgentTurnResponse> OpenAIAgentSession::doTurn(
    const std::string& requestBody,
    std::stop_token stopToken)
{
    // Check cancellation before making a network call
    auto makeStopResp = [this]() -> common::Result<ProviderAgentTurnResponse> {
        ProviderAgentTurnResponse resp;
        resp.turnIndex = turnIndex_;
        resp.finishReason = ProviderAgentFinishReason::StopRequested;
        if (auto v = validateTurnResponse(resp, turnIndex_, maxToolCallsPerTurn_);
                !v.success())
            return common::Result<ProviderAgentTurnResponse>::failure(*v.error());
        state_ = State::Terminal;
        ++turnIndex_;
        return common::Result<ProviderAgentTurnResponse>::ok(std::move(resp));
    };

    if (stopToken.stop_requested())
        return makeStopResp();

    http::HttpRequest req;
    req.url = endpoint_;
    req.method = "POST";
    req.headers["Authorization"] = "Bearer " + apiKey_;
    req.headers["Content-Type"] = "application/json";
    req.body = requestBody;
    req.totalTimeout = turnTimeout_;
    req.cancellationRequested = [st = stopToken] { return st.stop_requested(); };

    const http::HttpResponse httpResp = httpClient_->send(req);

    if (httpResp.hasTransportError()) {
        if (stopToken.stop_requested())
            return makeStopResp();
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.network_error",
             "Transport error during provider request: " + httpResp.errorMessage});
    }

    if (!httpResp.isSuccess())
        return httpStatusError<ProviderAgentTurnResponse>(httpResp.statusCode, httpResp.body);

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(httpResp.body);
    } catch (const std::exception&) {
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.invalid_response", "Failed to parse provider JSON response"});
    }

    const std::string responseId =
        (json.contains("id") && json["id"].is_string())
        ? json["id"].get<std::string>()
        : std::string{};

    const std::string status =
        (json.contains("status") && json["status"].is_string())
        ? json["status"].get<std::string>()
        : std::string{};

    // Parse output items
    std::string assistantText;
    std::vector<application::AgentToolCall> toolCalls;
    std::size_t callOrdinal = 0;

    if (json.contains("output") && json["output"].is_array()) {
        for (const auto& item : json["output"]) {
            if (!item.is_object()) continue;
            const auto type = item.value("type", std::string{});

            if (type == "message") {
                if (item.contains("content") && item["content"].is_array()) {
                    for (const auto& block : item["content"]) {
                        if (block.is_object()
                                && block.value("type", "") == "output_text"
                                && block.contains("text")
                                && block["text"].is_string()) {
                            assistantText += block["text"].get<std::string>();
                        }
                    }
                }
            } else if (type == "function_call") {
                if (!item.contains("call_id") || !item["call_id"].is_string())
                    return common::Result<ProviderAgentTurnResponse>::failure(
                        {"provider.invalid_response",
                         "function_call item missing call_id field"});
                if (!item.contains("name") || !item["name"].is_string())
                    return common::Result<ProviderAgentTurnResponse>::failure(
                        {"provider.invalid_response",
                         "function_call item missing name field"});

                application::AgentToolCall call;
                call.callId = item["call_id"].get<std::string>();
                call.toolName = item["name"].get<std::string>();
                call.argumentsJson = item.value("arguments", std::string("{}"));
                call.turnIndex = turnIndex_;
                call.ordinal = callOrdinal++;
                toolCalls.push_back(std::move(call));
            }
            // Other output types (e.g. "reasoning") are silently ignored
        }
    }

    ProviderAgentTurnResponse resp;
    resp.turnIndex = turnIndex_;
    resp.assistantText = std::move(assistantText);

    if (status == "completed") {
        if (!toolCalls.empty()) {
            resp.finishReason = ProviderAgentFinishReason::ToolUse;
            resp.toolCalls = std::move(toolCalls);
        } else {
            resp.finishReason = ProviderAgentFinishReason::Stop;
        }
    } else if (status == "incomplete") {
        const std::string reason =
            (json.contains("incomplete_details") && json["incomplete_details"].is_object())
            ? json["incomplete_details"].value("reason", std::string{})
            : std::string{};
        if (reason == "max_output_tokens") {
            resp.finishReason = ProviderAgentFinishReason::MaxOutputTokens;
        } else {
            resp.finishReason = ProviderAgentFinishReason::Error;
            resp.errorCode = "provider.incomplete";
            resp.errorMessage = "Response incomplete";
        }
    } else if (status == "failed") {
        resp.finishReason = ProviderAgentFinishReason::Error;
        resp.errorCode = "provider.request_failed";
        resp.errorMessage = "Provider request failed";
    } else {
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.invalid_response", "Unknown or missing response status field"});
    }

    // The shared parser also reports the cached/cache-write subsets of
    // input_tokens, exactly as the single-shot OpenAIProvider sees them.
    resp.usage = openai_detail::parseOpenAITokenUsage(json);

    if (auto v = validateTurnResponse(resp, turnIndex_, maxToolCallsPerTurn_); !v.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

    previousResponseId_ = responseId;
    ++turnIndex_;

    if (resp.finishReason == ProviderAgentFinishReason::ToolUse) {
        pendingCalls_ = resp.toolCalls;
        state_ = State::WaitingForToolResults;
    } else {
        pendingCalls_.clear();
        state_ = State::Terminal;
    }

    return common::Result<ProviderAgentTurnResponse>::ok(std::move(resp));
}

} // namespace ai::providers
