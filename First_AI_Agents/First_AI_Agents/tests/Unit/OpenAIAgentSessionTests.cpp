#include "ai/providers/OpenAIAgentSession.hpp"
#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/application/ProviderExecutionBudget.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"
#include "ai/http/HttpRequest.hpp"
#include "ai/http/IHttpClient.hpp"
#include "ai/http/HttpResponse.hpp"

#include <cassert>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace ai::providers;
using namespace ai::application;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

// ---------------------------------------------------------------------------
// ScriptedHttpClient — replays a queue of pre-built HttpResponse objects
// ---------------------------------------------------------------------------

class ScriptedHttpClient final : public ai::http::IHttpClient {
public:
    void enqueue(ai::http::HttpResponse resp)
    {
        responses_.push(std::move(resp));
    }

    ai::http::HttpResponse send(const ai::http::HttpRequest& req) override
    {
        capturedRequests_.push_back(req);
        if (responses_.empty())
            throw std::runtime_error("ScriptedHttpClient: no more scripted responses");
        auto resp = std::move(responses_.front());
        responses_.pop();
        return resp;
    }

    bool empty() const { return responses_.empty(); }
    const std::vector<ai::http::HttpRequest>& capturedRequests() const { return capturedRequests_; }

private:
    std::queue<ai::http::HttpResponse> responses_;
    std::vector<ai::http::HttpRequest> capturedRequests_;
};

ai::http::HttpResponse makeJsonResponse(int status, const std::string& body)
{
    ai::http::HttpResponse r;
    r.statusCode = status;
    r.body = body;
    return r;
}

ai::http::HttpResponse makeTransportError(const std::string& msg)
{
    ai::http::HttpResponse r;
    r.statusCode = 0;
    r.errorMessage = msg;
    return r;
}

// ---------------------------------------------------------------------------
// Minimal helpers to build ProviderAgentSessionRequest
// ---------------------------------------------------------------------------

ProviderAgentSessionRequest makeRequest(
    const std::string& userContent = "Hello",
    std::vector<AgentToolDefinition> tools = {})
{
    ProviderAgentSessionRequest req;
    req.providerId = "prov-1";
    req.modelId = "gpt-4o";
    req.apiModelId = "gpt-4o";
    req.userContent = userContent;
    req.tools = std::move(tools);
    req.maxTurns = 4;
    req.maxTotalToolCalls = 8;
    req.maxToolCallsPerTurn = 4;
    req.maxOutputTokens = 1024;
    req.temperature = 0.0;
    return req;
}

AgentToolDefinition makeToolDef(const std::string& name)
{
    AgentToolDefinition def;
    def.name = name;
    def.description = "Tool " + name;
    def.inputSchemaJson = R"({"type":"object","properties":{"path":{"type":"string"}}})";
    def.readOnly = true;
    return def;
}

AgentToolDefinition makeStrictToolDef(const std::string& name)
{
    AgentToolDefinition def;
    def.name = name;
    def.description = "Strict tool " + name;
    def.inputSchemaJson =
        R"({"type":"object","properties":{"path":{"type":"string"}},)"
        R"("required":["path"],"additionalProperties":false})";
    def.readOnly = true;
    def.strictInputSchema = true;
    return def;
}

std::shared_ptr<ScriptedHttpClient> makeSession(OpenAIAgentSession& session)
{
    (void)session;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Single-turn Stop
// ---------------------------------------------------------------------------

void testSingleTurnStop()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "resp_1",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "I can help."}]}
        ],
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(resp.success(), "singleTurnStop: beginSession failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::Stop,
        "singleTurnStop: expected Stop");
    req(resp.value().assistantText == "I can help.",
        "singleTurnStop: wrong text");
    req(resp.value().turnIndex == 0, "singleTurnStop: wrong turnIndex");
    req(resp.value().toolCalls.empty(), "singleTurnStop: should have no calls");
    req(resp.value().usage.inputTokens == 10, "singleTurnStop: wrong inputTokens");
    req(resp.value().usage.outputTokens == 5, "singleTurnStop: wrong outputTokens");
    req(client->empty(), "singleTurnStop: unused responses");
}

// ---------------------------------------------------------------------------
// Single tool call turn → ToolUse
// ---------------------------------------------------------------------------

void testToolUseTurn()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "resp_2",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "call_abc",
             "name": "workspace_read_file",
             "arguments": "{\"path\":\"README.md\"}"}
        ],
        "usage": {"input_tokens": 15, "output_tokens": 8}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("read README", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(resp.success(), "toolUseTurn: beginSession failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::ToolUse,
        "toolUseTurn: expected ToolUse");
    req(resp.value().toolCalls.size() == 1, "toolUseTurn: expected 1 call");
    req(resp.value().toolCalls[0].callId == "call_abc",
        "toolUseTurn: wrong callId");
    req(resp.value().toolCalls[0].toolName == "workspace_read_file",
        "toolUseTurn: wrong toolName");
    req(resp.value().toolCalls[0].argumentsJson == R"({"path":"README.md"})",
        "toolUseTurn: wrong arguments");
    req(resp.value().turnIndex == 0, "toolUseTurn: wrong turnIndex");
}

// ---------------------------------------------------------------------------
// Submit tool results → second turn Stop
// ---------------------------------------------------------------------------

void testSubmitToolResults()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "resp_3",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "c1",
             "name": "workspace_list_directory",
             "arguments": "{\"path\":\".\"}"}
        ],
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "resp_4",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Done."}]}
        ],
        "usage": {"input_tokens": 20, "output_tokens": 3}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("list files", {makeToolDef("workspace_list_directory")}),
        ss.get_token());

    req(r1.success(), "submitResults: first turn failed");
    req(r1.value().finishReason == ProviderAgentFinishReason::ToolUse,
        "submitResults: expected ToolUse on turn 0");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_list_directory";
    result.success = true;
    result.contentJson = R"(["src","README.md"])";

    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());
    req(r2.success(), "submitResults: second turn failed");
    req(r2.value().finishReason == ProviderAgentFinishReason::Stop,
        "submitResults: expected Stop on turn 1");
    req(r2.value().turnIndex == 1, "submitResults: wrong turnIndex on turn 1");
    req(r2.value().assistantText == "Done.", "submitResults: wrong final text");
    req(client->empty(), "submitResults: unused responses");
}

// ---------------------------------------------------------------------------
// Mixed text + function_call in one turn
// ---------------------------------------------------------------------------

void testMixedTextAndToolCall()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "resp_5",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Looking at "}]},
            {"type": "function_call",
             "call_id": "c2",
             "name": "workspace_read_file",
             "arguments": "{\"path\":\"x.txt\"}"},
            {"type": "message", "content": [{"type": "output_text", "text": "the file."}]}
        ],
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("read x.txt", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(resp.success(), "mixedText: failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::ToolUse,
        "mixedText: expected ToolUse");
    req(!resp.value().assistantText.empty(), "mixedText: expected some text");
    req(resp.value().toolCalls.size() == 1, "mixedText: expected 1 call");
}

// ---------------------------------------------------------------------------
// Multiple function_calls in one turn
// ---------------------------------------------------------------------------

void testMultipleToolCalls()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "resp_6",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "ca",
             "name": "workspace_read_file",
             "arguments": "{\"path\":\"a.txt\"}"},
            {"type": "function_call",
             "call_id": "cb",
             "name": "workspace_read_file",
             "arguments": "{\"path\":\"b.txt\"}"}
        ],
        "usage": {"input_tokens": 5, "output_tokens": 5}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("read both", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(resp.success(), "multiCalls: failed");
    req(resp.value().toolCalls.size() == 2, "multiCalls: expected 2 calls");
    req(resp.value().toolCalls[0].callId == "ca", "multiCalls: wrong callId[0]");
    req(resp.value().toolCalls[1].callId == "cb", "multiCalls: wrong callId[1]");
    req(resp.value().toolCalls[0].ordinal == 0, "multiCalls: wrong ordinal[0]");
    req(resp.value().toolCalls[1].ordinal == 1, "multiCalls: wrong ordinal[1]");
}

// ---------------------------------------------------------------------------
// HTTP 401 → provider.auth_failed (Result::failure)
// ---------------------------------------------------------------------------

void testHttp401AuthFailed()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(401, R"({"error":{"message":"Invalid API key"}})"));

    OpenAIAgentSession session("bad-key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "auth: expected failure");
    req(resp.error()->code == "provider.auth_failed", "auth: wrong error code");
}

// ---------------------------------------------------------------------------
// HTTP 429 → provider.rate_limited
// ---------------------------------------------------------------------------

void testHttp429RateLimited()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(429, R"({"error":{"message":"Rate limit exceeded"}})"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "rate: expected failure");
    req(resp.error()->code == "provider.rate_limited", "rate: wrong error code");
}

// ---------------------------------------------------------------------------
// Transport error → provider.network_error
// ---------------------------------------------------------------------------

void testTransportError()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeTransportError("Connection refused"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "transport: expected failure");
    req(resp.error()->code == "provider.network_error", "transport: wrong error code");
}

// ---------------------------------------------------------------------------
// Malformed JSON → provider.invalid_response
// ---------------------------------------------------------------------------

void testMalformedJson()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, "not json at all {{{{"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "malformed: expected failure");
    req(resp.error()->code == "provider.invalid_response", "malformed: wrong code");
}

// ---------------------------------------------------------------------------
// Missing call_id on function_call → provider.invalid_response
// ---------------------------------------------------------------------------

void testMissingCallId()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "name": "workspace_read_file",
             "arguments": "{}"}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(!resp.success(), "missingCallId: expected failure");
    req(resp.error()->code == "provider.invalid_response", "missingCallId: wrong code");
}

// ---------------------------------------------------------------------------
// status == "incomplete" + max_output_tokens → MaxTurns
// ---------------------------------------------------------------------------

void testIncompleteMaxTokens()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "incomplete",
        "incomplete_details": {"reason": "max_output_tokens"},
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "partial"}]}
        ],
        "usage": {"input_tokens": 5, "output_tokens": 1024}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(resp.success(), "maxTokens: expected success");
    req(resp.value().finishReason == ProviderAgentFinishReason::MaxOutputTokens,
        "maxTokens: expected MaxOutputTokens");
}

// ---------------------------------------------------------------------------
// State machine: submitToolResults before beginSession → not_started
// ---------------------------------------------------------------------------

void testSubmitBeforeBegin()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;

    AgentToolResult r;
    r.callId = "x";
    r.toolName = "workspace_read_file";
    r.success = true;
    auto resp = session.submitToolResults({std::move(r)}, ss.get_token());

    req(!resp.success(), "stateErr: expected failure");
    req(resp.error()->code == "agent_session.not_started", "stateErr: wrong code");
}

// ---------------------------------------------------------------------------
// State machine: beginSession twice → invalid_state
// ---------------------------------------------------------------------------

void testBeginTwice()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(makeRequest(), ss.get_token());
    req(r1.success(), "beginTwice: first call failed");

    auto r2 = session.beginSession(makeRequest(), ss.get_token());
    req(!r2.success(), "beginTwice: second call should fail");
    req(r2.error()->code == "agent_session.invalid_state",
        "beginTwice: wrong error code");
}

// ---------------------------------------------------------------------------
// Tool result correlation: unknown callId
// ---------------------------------------------------------------------------

void testUnknownCallId()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "real_id",
             "name": "workspace_read_file",
             "arguments": "{}"}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}),
        ss.get_token());
    req(r1.success(), "unknownId: first turn failed");

    AgentToolResult result;
    result.callId = "wrong_id";  // doesn't match "real_id"
    result.toolName = "workspace_read_file";
    result.success = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());

    req(!r2.success(), "unknownId: expected failure");
    req(r2.error()->code == "agent_session.unknown_tool_result",
        "unknownId: wrong error code");
}

// ---------------------------------------------------------------------------
// Tool result correlation: missing result
// ---------------------------------------------------------------------------

void testMissingResult()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "id1",
             "name": "workspace_read_file",
             "arguments": "{}"},
            {"type": "function_call",
             "call_id": "id2",
             "name": "workspace_read_file",
             "arguments": "{}"}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}),
        ss.get_token());
    req(r1.success(), "missingResult: first turn failed");

    // Only submit result for id1, not id2
    AgentToolResult result;
    result.callId = "id1";
    result.toolName = "workspace_read_file";
    result.success = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());

    req(!r2.success(), "missingResult: expected failure");
    req(r2.error()->code == "agent_session.missing_tool_result",
        "missingResult: wrong error code");
}

// ---------------------------------------------------------------------------
// status == "failed" → FinishReason::Error
// ---------------------------------------------------------------------------

void testStatusFailed()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "failed",
        "error": {"message": "Internal error"},
        "output": [],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(resp.success(), "statusFailed: expected ok(TurnResponse)");
    req(resp.value().finishReason == ProviderAgentFinishReason::Error,
        "statusFailed: expected Error finish reason");
    req(!resp.value().errorCode.empty(), "statusFailed: errorCode must be set");
}

// ---------------------------------------------------------------------------
// Regression: OpenAI's reasoning-family models reject an unrecognized
// "temperature" parameter with HTTP 400 ("Unsupported parameter"). The plain
// single-shot OpenAIProvider.cpp path never sends it; this session used to
// send it unconditionally on every turn (beginSession AND submitToolResults),
// so any workspaceAccess agent run against such a model failed outright while
// the identical model worked fine through Chat. Neither request body may
// contain the field at all.
// ---------------------------------------------------------------------------

void testRequestsNeverIncludeTemperature()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "c1",
             "name": "workspace_read_file",
             "arguments": "{}"}
        ],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r2",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Done."}]}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(r1.success(), "noTemperature: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());
    req(r2.success(), "noTemperature: second turn failed");

    req(client->capturedRequests().size() == 2, "noTemperature: expected 2 requests");
    for (const auto& sent : client->capturedRequests()) {
        req(sent.body.find("temperature") == std::string::npos,
            "noTemperature: request body must never contain a temperature field");
    }
}

// ---------------------------------------------------------------------------
// gpt-5.3-codex -> gpt-6-sol migration decision (2026-10-03): reasoning effort is
// deliberately left to the API default (documented `medium` for gpt-6-sol); an explicit,
// frozen effort is a deferred post-MVP hardening (CODEX_CONTEXT, "M3 deferred"). This pins
// the CURRENT contract: no request of the session - the first turn and every continuation -
// carries a `reasoning` field, so a model that does not accept one keeps working exactly as
// before and the migration never depends on a parameter this build does not send.
// ---------------------------------------------------------------------------

void testRequestsLeaveReasoningEffortToTheApiDefault()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "c1",
             "name": "workspace_read_file",
             "arguments": "{}"}
        ],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r2",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "Done."}]}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(r1.success(), "noReasoning: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());
    req(r2.success(), "noReasoning: second turn failed");

    req(client->capturedRequests().size() == 2, "noReasoning: expected 2 requests");
    for (const auto& sent : client->capturedRequests()) {
        req(sent.body.find("\"reasoning\"") == std::string::npos,
            "noReasoning: no request may carry a reasoning field - the effort is the API default");
        req(sent.body.find("\"effort\"") == std::string::npos,
            "noReasoning: no request may carry an effort value");
    }
}

// ---------------------------------------------------------------------------
// Regression: an HTTP 400 must surface OpenAI's actual error message (e.g.
// "Unsupported parameter: 'temperature' ..."), not just the bare status
// code - httpStatusError() previously discarded the response body entirely,
// making every failure indistinguishable as "Provider returned HTTP 400".
// ---------------------------------------------------------------------------

void testHttp400SurfacesApiErrorMessage()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(400, R"({"error":{"message":)"
        R"("Unsupported parameter: 'temperature' is not supported with this model.",)"
        R"("type":"invalid_request_error","param":"temperature","code":"unsupported_parameter"}})"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "http400Message: expected failure");
    req(resp.error()->code == "provider.request_failed", "http400Message: wrong error code");
    req(resp.error()->message.find("Unsupported parameter") != std::string::npos,
        "http400Message: error message must include the API's actual explanation");
}

// ---------------------------------------------------------------------------
// Tool name mismatch in submitToolResults
// ---------------------------------------------------------------------------

void testToolNameMismatch()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "c1",
             "name": "workspace_read_file",
             "arguments": "{}"}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}),
        ss.get_token());
    req(r1.success(), "nameMismatch: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_list_directory";  // wrong name
    result.success = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());

    req(!r2.success(), "nameMismatch: expected failure");
    req(r2.error()->code == "agent_session.tool_name_mismatch",
        "nameMismatch: wrong error code");
}

// ---------------------------------------------------------------------------
// Failed tool result (is_error equivalent via success=false)
// ---------------------------------------------------------------------------

void testFailedToolResult()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1",
        "status": "completed",
        "output": [
            {"type": "function_call",
             "call_id": "c1",
             "name": "workspace_read_file",
             "arguments": "{\"path\":\"missing.txt\"}"}
        ],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r2",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "File not found."}]}
        ],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read missing", {makeToolDef("workspace_read_file")}),
        ss.get_token());
    req(r1.success(), "failedResult: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = false;
    result.errorCode = "workspace.not_found";
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());

    req(r2.success(), "failedResult: second turn failed");
    req(r2.value().finishReason == ProviderAgentFinishReason::Stop,
        "failedResult: expected Stop");
    req(client->empty(), "failedResult: unused responses");
}

// A failed tool result carries the Backend's own explanation (for example the allowed paths of a
// denied read) next to the code; a failure without one keeps the bare code exactly as before.
void testFailedToolResultCarriesTheBackendExplanation()
{
    const auto outputOfFailedResult = [](const std::string& errorCode, const std::string& json) {
        auto client = std::make_shared<ScriptedHttpClient>();
        client->enqueue(makeJsonResponse(200, R"({
            "id": "r1", "status": "completed",
            "output": [{"type": "function_call", "call_id": "cx", "name": "workspace_read_file",
                        "arguments": "{\"path\":\"x.txt\"}"}],
            "usage": {}
        })"));
        client->enqueue(makeJsonResponse(200, R"({
            "id": "r2", "status": "completed",
            "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
            "usage": {}
        })"));
        OpenAIAgentSession session("key", "https://fake", client);
        std::stop_source ss;
        const auto first = session.beginSession(
            makeRequest("read", {makeToolDef("workspace_read_file")}), ss.get_token());
        req(first.success(), "failedExplanation: first turn failed");
        AgentToolResult result;
        result.callId = "cx";
        result.toolName = "workspace_read_file";
        result.success = false;
        result.errorCode = errorCode;
        result.contentJson = json;
        const auto second = session.submitToolResults({std::move(result)}, ss.get_token());
        req(second.success(), "failedExplanation: second turn failed");
        const auto body = nlohmann::json::parse(client->capturedRequests()[1].body);
        const auto& item = body.at("input").front();
        req(item.at("type").get<std::string>() == "function_call_output",
            "failedExplanation: the failure must be a function_call_output");
        return item.at("output").get<std::string>();
    };
    req(outputOfFailedResult("workspace.not_found", "") == "error: workspace.not_found",
        "failedExplanation: a failure without an explanation keeps the bare code");
    req(outputOfFailedResult("workspace_path.outside_allowed_roots", R"({"allowedReadPaths":["a.md"]})")
            == "error: workspace_path.outside_allowed_roots\n{\"allowedReadPaths\":[\"a.md\"]}",
        "failedExplanation: the Backend explanation must follow the code");
}

// ---------------------------------------------------------------------------
// Package 2/3: strict tool schema wire semantics
// ---------------------------------------------------------------------------

void testStrictToolSendsStrictTrueWithSchema()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("hi", {makeStrictToolDef("submit_execution_manifest")}), ss.get_token());
    req(resp.success(), "strictTool: beginSession failed");
    req(client->capturedRequests().size() == 1, "strictTool: expected exactly one HTTP call");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(body.at("tools").size() == 1, "strictTool: expected exactly one tool");
    const auto& tool = body.at("tools").front();
    req(tool.at("strict").get<bool>() == true, "strictTool: strict:true was not sent");
    req(tool.at("parameters").at("additionalProperties").get<bool>() == false,
        "strictTool: schema was not forwarded intact");
}

void testNonStrictToolNeverGetsStrictFlag()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(resp.success(), "nonStrictTool: beginSession failed");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(!body.at("tools").front().contains("strict"),
        "nonStrictTool: a non-strict tool must never carry a false strict guarantee");
}

// The real propose_change_set contract, not a synthetic strict tool: proves
// AgentToolTurnLoopRunner::proposeChangeSetToolDefinition() itself - the
// production terminal tool the strict propose_change_set contract package
// exists to lock down - actually reaches the wire with strict:true and its
// exact schema (schemaVersion/summary/changes, no expected_original_hash).
void testRealProposeChangeSetToolSendsStrictTrueWithExactSchema()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {}
    })"));

    const auto toolDef = ai::application::proposeChangeSetToolDefinition();
    req(toolDef.strictInputSchema,
        "realProposeChangeSet: proposeChangeSetToolDefinition() must set strictInputSchema");

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("hi", {toolDef}), ss.get_token());
    req(resp.success(), "realProposeChangeSet: beginSession failed");
    req(client->capturedRequests().size() == 1, "realProposeChangeSet: expected exactly one HTTP call");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    const auto& tool = body.at("tools").front();
    req(tool.at("strict").get<bool>() == true,
        "realProposeChangeSet: strict:true was not sent for the real tool");
    const auto& params = tool.at("parameters");
    req(params.at("additionalProperties").get<bool>() == false,
        "realProposeChangeSet: root additionalProperties:false was not forwarded");
    const auto& props = params.at("properties");
    req(props.contains("schemaVersion") && props.contains("summary") && props.contains("changes"),
        "realProposeChangeSet: expected top-level fields missing from the wire schema");
    const auto& changeItemProps = props.at("changes").at("items").at("properties");
    req(!changeItemProps.contains("expected_original_hash"),
        "realProposeChangeSet: the wire schema must never carry a hash field");
    req(changeItemProps.contains("change_id") && changeItemProps.contains("operation")
            && changeItemProps.contains("relative_path") && changeItemProps.contains("source_path")
            && changeItemProps.contains("proposed_content"),
        "realProposeChangeSet: expected per-change fields missing from the wire schema");
}

void testRealScopeExpansionToolSendsStrictSchema()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id":"r-scope","status":"completed",
        "output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}],
        "usage":{}
    })"));
    const auto def = ai::application::requestScopeExpansionToolDefinition();
    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    const auto response = session.beginSession(makeRequest("hi", {def}), ss.get_token());
    req(response.success(), "scopeExpansion: beginSession failed");
    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    const auto& tool = body.at("tools").front();
    req(tool.at("strict").get<bool>(), "scopeExpansion: strict:true missing");
    const auto& parameters = tool.at("parameters");
    req(parameters.at("additionalProperties").get<bool>() == false,
        "scopeExpansion: schema was not forwarded intact");
    req(parameters.at("properties").at("role").at("type") == "string",
        "scopeExpansion: enum field lost its explicit type");
}

void testMalformedStrictSchemaJsonFailsFastBeforeHttpSend()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    // Deliberately no scripted response: a real HTTP call here would throw
    // and fail this test, proving fail-fast happened before any send().
    auto badDef = makeStrictToolDef("submit_execution_manifest");
    badDef.inputSchemaJson = "{not valid json";

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("hi", {badDef}), ss.get_token());
    req(!resp.success(), "malformedStrictJson: expected failure");
    req(resp.error()->code == "agent_session.malformed_strict_schema",
        "malformedStrictJson: wrong error code");
    req(client->capturedRequests().empty(), "malformedStrictJson: no HTTP call must be made");
}

void testStructurallyInvalidStrictSchemaFailsFastBeforeHttpSend()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    auto badDef = makeStrictToolDef("submit_execution_manifest");
    // Valid JSON, but missing additionalProperties:false - not strict-compatible.
    badDef.inputSchemaJson = R"({"type":"object","properties":{"path":{"type":"string"}},)"
                              R"("required":["path"]})";

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("hi", {badDef}), ss.get_token());
    req(!resp.success(), "structurallyInvalid: expected failure");
    req(resp.error()->code == "agent_session.malformed_strict_schema",
        "structurallyInvalid: wrong error code");
    req(client->capturedRequests().empty(), "structurallyInvalid: no HTTP call must be made");
}

// ---------------------------------------------------------------------------
// Package 4: next-turn tool policy wire semantics
// ---------------------------------------------------------------------------

void testNextTurnPolicyRestrictsToolsAndDisablesParallel()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "function_call", "call_id": "c1",
                     "name": "workspace_read_file", "arguments": "{}"}],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r2", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "done"}]}],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file"),
                           makeStrictToolDef("submit_execution_manifest")}),
        ss.get_token());
    req(r1.success(), "nextTurnPolicy: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";

    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"submit_execution_manifest"};
    policy.disallowParallelToolCalls = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token(), policy);
    req(r2.success(), "nextTurnPolicy: second turn failed");
    req(client->capturedRequests().size() == 2, "nextTurnPolicy: expected exactly two HTTP calls");

    const auto body = nlohmann::json::parse(client->capturedRequests()[1].body);
    req(body.at("tools").size() == 1,
        "nextTurnPolicy: restricted turn must advertise only the allowed tool");
    req(body.at("tools").front().at("name").get<std::string>() == "submit_execution_manifest",
        "nextTurnPolicy: wrong tool survived the restriction");
    req(body.at("tool_choice").at("type").get<std::string>() == "function",
        "nextTurnPolicy: a one-tool policy must use OpenAI's named-function forcing");
    req(body.at("tool_choice").at("name").get<std::string>()
            == "submit_execution_manifest",
        "nextTurnPolicy: named-function forcing selected the wrong terminal tool");
    req(body.at("parallel_tool_calls").get<bool>() == false,
        "nextTurnPolicy: disallowParallelToolCalls did not serialize to parallel_tool_calls:false");
}

void testNextTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "function_call", "call_id": "c1",
                     "name": "workspace_read_file", "arguments": "{}"}],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r2", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "done"}]}],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file"),
                           makeStrictToolDef("submit_execution_manifest"),
                           makeStrictToolDef("submit_no_changes_needed")}),
        ss.get_token());
    req(r1.success(), "multiNextTurnPolicy: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";

    NextTurnToolPolicy policy;
    policy.allowedToolNames = {
        "submit_execution_manifest", "submit_no_changes_needed"};
    policy.disallowParallelToolCalls = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token(), policy);
    req(r2.success(), "multiNextTurnPolicy: second turn failed");

    const auto body = nlohmann::json::parse(client->capturedRequests()[1].body);
    req(body.at("tools").size() == 2,
        "multiNextTurnPolicy: both allowed terminal tools must remain advertised");
    req(body.at("tool_choice").get<std::string>() == "required",
        "multiNextTurnPolicy: several allowed tools must retain required choice within subset");
    req(body.at("parallel_tool_calls").get<bool>() == false,
        "multiNextTurnPolicy: the required choice must remain single-call");
}

void testNextTurnPolicyUnknownToolNameIsRejected()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "function_call", "call_id": "c1",
                     "name": "workspace_read_file", "arguments": "{}"}],
        "usage": {}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(r1.success(), "unknownToolPolicy: first turn failed");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";

    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"never_advertised_tool"};
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token(), policy);
    req(!r2.success(), "unknownToolPolicy: expected failure");
    req(r2.error()->code == "agent_session.invalid_next_turn_policy",
        "unknownToolPolicy: wrong error code");
    req(client->capturedRequests().size() == 1,
        "unknownToolPolicy: no second HTTP call must be made for a rejected policy");
}

// Worker capacity fairness: the same one-tool/several-tools wire contract for
// the FIRST turn (ProviderAgentSessionRequest::firstTurnToolPolicy), which a
// NextTurnToolPolicy passed to submitToolResults() can never reach.
void testFirstTurnPolicyForcesTheSingleAllowedFunctionByName()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "function_call", "call_id": "c1",
                     "name": "propose_change_set", "arguments": "{}"}],
        "usage": {}
    })"));

    auto request = makeRequest("hi", {makeToolDef("workspace_read_file"),
                                       makeToolDef("propose_change_set")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"propose_change_set"};
    policy.disallowParallelToolCalls = true;
    request.firstTurnToolPolicy = policy;

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    req(session.beginSession(request, ss.get_token()).success(),
        "firstTurnPolicy: first turn failed");
    const auto body = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(body.at("tools").size() == 1
            && body.at("tools").front().at("name").get<std::string>() == "propose_change_set",
        "firstTurnPolicy: the first turn must advertise only the allowed function");
    req(body.at("tool_choice").at("type").get<std::string>() == "function"
            && body.at("tool_choice").at("name").get<std::string>() == "propose_change_set",
        "firstTurnPolicy: a one-tool first-turn policy must force the function by name");
    req(body.at("parallel_tool_calls").get<bool>() == false,
        "firstTurnPolicy: disallowParallelToolCalls must serialize to parallel_tool_calls:false");
}

void testFirstTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "function_call", "call_id": "c1",
                     "name": "propose_change_set", "arguments": "{}"}],
        "usage": {}
    })"));

    auto request = makeRequest("hi", {makeToolDef("workspace_read_file"),
                                       makeToolDef("propose_change_set"),
                                       makeToolDef("submit_task_no_changes_needed")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"propose_change_set", "submit_task_no_changes_needed"};
    policy.disallowParallelToolCalls = true;
    request.firstTurnToolPolicy = policy;

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    req(session.beginSession(request, ss.get_token()).success(),
        "firstTurnPolicyMulti: first turn failed");
    const auto body = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(body.at("tools").size() == 2,
        "firstTurnPolicyMulti: both terminal tools must remain advertised");
    req(body.at("tool_choice").get<std::string>() == "required",
        "firstTurnPolicyMulti: several allowed tools must keep required choice within the subset");
    req(body.at("parallel_tool_calls").get<bool>() == false,
        "firstTurnPolicyMulti: the required choice must stay single-call");
}

void testFirstTurnPolicyUnknownToolNameIsRejectedBeforeAnyRequest()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    auto request = makeRequest("hi", {makeToolDef("workspace_read_file")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"never_advertised_tool"};
    request.firstTurnToolPolicy = policy;

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    const auto result = session.beginSession(request, ss.get_token());
    req(!result.success()
            && result.error()->code == "agent_session.invalid_first_turn_policy",
        "firstTurnPolicyUnknown: a policy naming an unadvertised tool must be rejected");
    req(client->capturedRequests().empty(),
        "firstTurnPolicyUnknown: no HTTP call may be made for a rejected policy");
}

void testWithoutFirstTurnPolicyTheFirstTurnStaysUnrestricted()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "ok"}]}],
        "usage": {}
    })"));
    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    req(session.beginSession(
            makeRequest("hi", {makeToolDef("workspace_read_file"),
                               makeToolDef("propose_change_set")}),
            ss.get_token()).success(),
        "noFirstTurnPolicy: first turn failed");
    const auto body = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(body.at("tools").size() == 2
            && body.at("tool_choice").get<std::string>() == "required",
        "noFirstTurnPolicy: an absent policy must leave the first turn exactly as before");
}

// ---------------------------------------------------------------------------
// AG-17 gpt-6-sol checkpoint M1: usage accounting. input_tokens is the FULL
// input total; usage.input_tokens_details.cached_tokens (cache read) and
// .cache_write_tokens are subsets of it.
// ---------------------------------------------------------------------------

std::string completedMessageBody(const std::string& usageJson)
{
    return R"({"id": "r", "status": "completed",
               "output": [{"type": "message",
                           "content": [{"type": "output_text", "text": "ok"}]}],
               "usage": )" + usageJson + "}";
}

// What one scripted first turn reports as usage.
ai::models::TokenUsage sessionUsageFor(const std::string& usageJson)
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, completedMessageBody(usageJson)));
    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());
    req(resp.success(), "usageFor: beginSession failed");
    return resp.value().usage;
}

void testSessionUsageCarriesTheCacheBreakdownOnEveryTurn()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r1", "status": "completed",
        "output": [{"type": "function_call", "call_id": "c1",
                    "name": "workspace_read_file", "arguments": "{}"}],
        "usage": {"input_tokens": 1000, "output_tokens": 50,
                  "input_tokens_details": {"cached_tokens": 600, "cache_write_tokens": 100},
                  "output_tokens_details": {"reasoning_tokens": 20}}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "id": "r2", "status": "completed",
        "output": [{"type": "message", "content": [{"type": "output_text", "text": "Done."}]}],
        "usage": {"input_tokens": 1200, "output_tokens": 9,
                  "input_tokens_details": {"cached_tokens": 1100}}
    })"));

    OpenAIAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto first = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(first.success(), "cacheUsage: first turn failed");
    const auto& u1 = first.value().usage;
    req(u1.inputTokens == 1000, "cacheUsage: inputTokens must be the full input_tokens");
    req(u1.cacheReadInputTokens == 600, "cacheUsage: cached_tokens was not parsed");
    req(u1.cacheCreationInputTokens == 100, "cacheUsage: cache_write_tokens was not parsed");
    req(u1.outputTokens == 50, "cacheUsage: reasoning tokens are already in output_tokens");

    AgentToolResult result;
    result.callId = "c1";
    result.toolName = "workspace_read_file";
    result.success = true;
    auto second = session.submitToolResults({std::move(result)}, ss.get_token());
    req(second.success(), "cacheUsage: second turn failed");
    const auto& u2 = second.value().usage;
    req(u2.inputTokens == 1200, "cacheUsage: second turn inputTokens");
    req(u2.cacheReadInputTokens == 1100, "cacheUsage: second turn cached_tokens");
    req(u2.cacheCreationInputTokens == 0,
        "cacheUsage: a missing cache_write_tokens must be zero, not an error");
}

// The single-shot OpenAIProvider returns parseOpenAITokenUsage() of the raw
// response (it has no injectable HTTP client), and this session must report
// exactly the same value for the same response - field by field - whatever the
// shape, including malformed ones.
void testSessionUsageEqualsTheSharedParserForEveryShape()
{
    const std::vector<std::string> shapes {
        R"({"input_tokens": 1000, "output_tokens": 50,
            "input_tokens_details": {"cached_tokens": 600, "cache_write_tokens": 100}})",
        R"({"input_tokens": 300, "output_tokens": 7})",
        R"({})",
        R"({"input_tokens": 300, "output_tokens": 7, "input_tokens_details": {}})",
        R"({"input_tokens": 300, "output_tokens": 7,
            "input_tokens_details": {"cached_tokens": 128}})",
        R"({"input_tokens": 300, "output_tokens": 7,
            "input_tokens_details": {"cache_write_tokens": 250}})",
        R"({"input_tokens": 40, "output_tokens": 3, "input_tokens_details": "x"})",
        R"({"input_tokens": 40, "output_tokens": 3,
            "input_tokens_details": {"cached_tokens": "12", "cache_write_tokens": 5}})",
        R"({"input_tokens": 10, "output_tokens": 1,
            "input_tokens_details": {"cached_tokens": 8, "cache_write_tokens": 8}})",
    };
    for (const auto& usageJson : shapes) {
        const auto shared = ai::providers::openai_detail::parseOpenAITokenUsage(
            nlohmann::json::parse(completedMessageBody(usageJson)));
        const auto fromSession = sessionUsageFor(usageJson);
        req(fromSession.inputTokens == shared.inputTokens, "sameParser: inputTokens differ");
        req(fromSession.outputTokens == shared.outputTokens, "sameParser: outputTokens differ");
        req(fromSession.cacheReadInputTokens == shared.cacheReadInputTokens,
            "sameParser: cacheRead differs");
        req(fromSession.cacheCreationInputTokens == shared.cacheCreationInputTokens,
            "sameParser: cacheWrite differs");
    }

    // The fully-populated shape is pinned to literal values too, so "both are
    // equally wrong" cannot pass.
    const auto full = sessionUsageFor(shapes.front());
    req(full.inputTokens == 1000 && full.outputTokens == 50
            && full.cacheReadInputTokens == 600 && full.cacheCreationInputTokens == 100,
        "sameParser: the documented shape must map to exact literals");
}

// The budget guard is unchanged; this proves the OpenAI mapping lands in the
// guard's existing semantics: cache-read is NOT spent against maxInputTokens,
// cache-write IS (it stays inside the countable input).
void testOpenAIUsageSpendsTheInputBudgetWithoutCacheReadButWithCacheWrite()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 100;
    policy.maxTurns = 20;

    {   // A turn that is entirely served from the cache costs no input budget.
        ProviderSessionBudgetGuard guard(policy);
        req(guard.beginProviderTurn().success(), "budget: begin 1");
        const auto allCached = sessionUsageFor(
            R"({"input_tokens": 100, "output_tokens": 1,
                "input_tokens_details": {"cached_tokens": 100}})");
        req(guard.completeProviderTurn(allCached).success(), "budget: all-cached turn");
        req(guard.usage().inputTokens == 0, "budget: cache-read must not spend maxInputTokens");
        req(guard.usage().cacheReadInputTokens == 100, "budget: cache-read is still reported");
        req(guard.beginProviderTurn().success(),
            "budget: a Run that only read from the cache must still be admitted");
    }
    {   // Mixed turn: 100 total = 70 cache-read + 10 cache-write + 20 plain -> 30 spent.
        ProviderSessionBudgetGuard guard(policy);
        req(guard.beginProviderTurn().success(), "budget: begin 2");
        const auto mixed = sessionUsageFor(
            R"({"input_tokens": 100, "output_tokens": 1,
                "input_tokens_details": {"cached_tokens": 70, "cache_write_tokens": 10}})");
        req(guard.completeProviderTurn(mixed).success(), "budget: mixed turn");
        req(guard.usage().inputTokens == 30,
            "budget: countable input is input_tokens minus cache-read only (cache-write counts)");
        req(guard.usage().cacheReadInputTokens == 70
                && guard.usage().cacheCreationInputTokens == 10,
            "budget: the breakdown must survive into the Run usage");
    }
    {   // A turn that is entirely cache-write spends the whole input and trips the cap.
        ProviderSessionBudgetGuard guard(policy);
        req(guard.beginProviderTurn().success(), "budget: begin 3");
        const auto allWritten = sessionUsageFor(
            R"({"input_tokens": 101, "output_tokens": 1,
                "input_tokens_details": {"cache_write_tokens": 101}})");
        const auto tripped = guard.completeProviderTurn(allWritten);
        req(!tripped.success()
                && tripped.error()->code == "provider_budget.input_token_limit_reached",
            "budget: cache-write must spend maxInputTokens");
        req(guard.usage().inputTokens == 101, "budget: cache-write counted in full");
    }
    // A malformed OpenAI report - a cache breakdown larger than the whole input -
    // earns no discount and stops the Run (stable usage_invalid), for both forms.
    for (const char* malformedUsage : {
             R"({"input_tokens": 10, "output_tokens": 1,
                 "input_tokens_details": {"cached_tokens": 50}})",
             R"({"input_tokens": 100, "output_tokens": 1,
                 "input_tokens_details": {"cached_tokens": 60, "cache_write_tokens": 60}})"}) {
        ProviderSessionBudgetGuard guard(policy);
        req(guard.beginProviderTurn().success(), "budget: begin malformed");
        const auto reported = sessionUsageFor(malformedUsage);
        const auto stopped = guard.completeProviderTurn(reported);
        req(!stopped.success() && stopped.error()->code == "provider_budget.usage_invalid",
            "budget: an inconsistent OpenAI breakdown must stop the Run as usage_invalid");
        req(guard.usage().inputTokens == reported.inputTokens,
            "budget: an inconsistent report earns no cache discount");
        req(guard.usage().cacheReadInputTokens == 0
                && guard.usage().cacheCreationInputTokens == 0
                && !guard.usage().usageAuthoritative,
            "budget: the untrusted breakdown must not be recorded as authoritative usage");
    }
}

} // namespace

int runOpenAIAgentSessionTests()
{
    testSingleTurnStop();
    testToolUseTurn();
    testSubmitToolResults();
    testMixedTextAndToolCall();
    testMultipleToolCalls();
    testHttp401AuthFailed();
    testHttp429RateLimited();
    testTransportError();
    testMalformedJson();
    testMissingCallId();
    testIncompleteMaxTokens();
    testSubmitBeforeBegin();
    testBeginTwice();
    testUnknownCallId();
    testMissingResult();
    testStatusFailed();
    testToolNameMismatch();
    testFailedToolResult();
    testFailedToolResultCarriesTheBackendExplanation();
    testRequestsNeverIncludeTemperature();
    testRequestsLeaveReasoningEffortToTheApiDefault();
    testHttp400SurfacesApiErrorMessage();
    testStrictToolSendsStrictTrueWithSchema();
    testRealProposeChangeSetToolSendsStrictTrueWithExactSchema();
    testRealScopeExpansionToolSendsStrictSchema();
    testNonStrictToolNeverGetsStrictFlag();
    testMalformedStrictSchemaJsonFailsFastBeforeHttpSend();
    testStructurallyInvalidStrictSchemaFailsFastBeforeHttpSend();
    testNextTurnPolicyRestrictsToolsAndDisablesParallel();
    testNextTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset();
    testNextTurnPolicyUnknownToolNameIsRejected();
    testFirstTurnPolicyForcesTheSingleAllowedFunctionByName();
    testFirstTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset();
    testFirstTurnPolicyUnknownToolNameIsRejectedBeforeAnyRequest();
    testWithoutFirstTurnPolicyTheFirstTurnStaysUnrestricted();
    testSessionUsageCarriesTheCacheBreakdownOnEveryTurn();
    testSessionUsageEqualsTheSharedParserForEveryShape();
    testOpenAIUsageSpendsTheInputBudgetWithoutCacheReadButWithCacheWrite();
    return 0;
}
