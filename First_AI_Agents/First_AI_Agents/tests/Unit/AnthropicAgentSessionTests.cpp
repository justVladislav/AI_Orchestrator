#include "ai/providers/AnthropicAgentSession.hpp"
#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/http/IHttpClient.hpp"
#include "ai/http/HttpResponse.hpp"

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
// ScriptedHttpClient
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

ProviderAgentSessionRequest makeRequest(
    const std::string& userContent = "Hello",
    std::vector<AgentToolDefinition> tools = {})
{
    ProviderAgentSessionRequest req;
    req.providerId = "prov-1";
    req.modelId = "claude-3-5-sonnet-20241022";
    req.apiModelId = "claude-3-5-sonnet-20241022";
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
    def.inputSchemaJson =
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})";
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

// ---------------------------------------------------------------------------
// Single turn → Stop (end_turn)
// ---------------------------------------------------------------------------

void testSingleTurnStop()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "id": "msg_1",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "I can assist."}],
        "usage": {"input_tokens": 12, "output_tokens": 4}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(resp.success(), "a-singleTurnStop: beginSession failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::Stop,
        "a-singleTurnStop: expected Stop");
    req(resp.value().assistantText == "I can assist.",
        "a-singleTurnStop: wrong text");
    req(resp.value().toolCalls.empty(), "a-singleTurnStop: no calls expected");
    req(resp.value().turnIndex == 0, "a-singleTurnStop: wrong turnIndex");
    req(resp.value().usage.inputTokens == 12, "a-singleTurnStop: wrong inputTokens");
    req(resp.value().usage.outputTokens == 4, "a-singleTurnStop: wrong outputTokens");
    req(client->empty(), "a-singleTurnStop: unused responses");
}

// ---------------------------------------------------------------------------
// Tool use turn (tool_use stop_reason)
// ---------------------------------------------------------------------------

void testToolUseTurn()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "tool_use",
        "content": [
            {
                "type": "tool_use",
                "id": "toolu_abc",
                "name": "workspace_read_file",
                "input": {"path": "README.md"}
            }
        ],
        "usage": {"input_tokens": 20, "output_tokens": 15}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("read README", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(resp.success(), "a-toolUse: beginSession failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::ToolUse,
        "a-toolUse: expected ToolUse");
    req(resp.value().toolCalls.size() == 1, "a-toolUse: expected 1 call");
    req(resp.value().toolCalls[0].callId == "toolu_abc",
        "a-toolUse: wrong callId (must be tool_use id)");
    req(resp.value().toolCalls[0].toolName == "workspace_read_file",
        "a-toolUse: wrong toolName");
    // argumentsJson must be the serialized input object
    req(!resp.value().toolCalls[0].argumentsJson.empty(),
        "a-toolUse: argumentsJson must not be empty");
}

// ---------------------------------------------------------------------------
// Mixed text + tool_use: order preserved
// ---------------------------------------------------------------------------

void testMixedTextAndToolCall()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "tool_use",
        "content": [
            {"type": "text", "text": "Let me check that file."},
            {
                "type": "tool_use",
                "id": "toolu_xyz",
                "name": "workspace_read_file",
                "input": {"path": "config.json"}
            }
        ],
        "usage": {"input_tokens": 10, "output_tokens": 10}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("check config", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(resp.success(), "a-mixed: failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::ToolUse,
        "a-mixed: expected ToolUse");
    req(resp.value().assistantText == "Let me check that file.",
        "a-mixed: wrong text");
    req(resp.value().toolCalls.size() == 1, "a-mixed: expected 1 call");
    req(resp.value().toolCalls[0].callId == "toolu_xyz", "a-mixed: wrong callId");
}

// ---------------------------------------------------------------------------
// Submit tool results → Stop (multi-turn, full history)
// ---------------------------------------------------------------------------

void testSubmitToolResults()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "tool_use",
        "content": [
            {
                "type": "tool_use",
                "id": "tid_1",
                "name": "workspace_list_directory",
                "input": {"path": "."}
            }
        ],
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "Found 3 files."}],
        "usage": {"input_tokens": 25, "output_tokens": 6}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("list files", {makeToolDef("workspace_list_directory")}),
        ss.get_token());

    req(r1.success(), "a-submit: first turn failed");
    req(r1.value().finishReason == ProviderAgentFinishReason::ToolUse,
        "a-submit: expected ToolUse on turn 0");

    AgentToolResult result;
    result.callId = "tid_1";
    result.toolName = "workspace_list_directory";
    result.success = true;
    result.contentJson = R"(["a.txt","b.txt","c.txt"])";

    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());
    req(r2.success(), "a-submit: second turn failed");
    req(r2.value().finishReason == ProviderAgentFinishReason::Stop,
        "a-submit: expected Stop on turn 1");
    req(r2.value().turnIndex == 1, "a-submit: wrong turnIndex");
    req(r2.value().assistantText == "Found 3 files.", "a-submit: wrong final text");
    req(client->empty(), "a-submit: unused responses");
}

// ---------------------------------------------------------------------------
// is_error: failed tool result
// ---------------------------------------------------------------------------

void testFailedToolResult()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "tool_use",
        "content": [
            {"type": "tool_use", "id": "tid_e", "name": "workspace_read_file",
             "input": {"path": "missing.txt"}}
        ],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "File not found, sorry."}],
        "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read missing", {makeToolDef("workspace_read_file")}),
        ss.get_token());
    req(r1.success(), "a-failedResult: first turn failed");

    AgentToolResult result;
    result.callId = "tid_e";
    result.toolName = "workspace_read_file";
    result.success = false;
    result.errorCode = "workspace.not_found";
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());

    req(r2.success(), "a-failedResult: second turn failed");
    req(r2.value().finishReason == ProviderAgentFinishReason::Stop,
        "a-failedResult: expected Stop");
    req(client->empty(), "a-failedResult: unused responses");
}

// A failed tool result carries the Backend's own explanation (for example the allowed paths of a
// denied read) next to the code; a failure without one keeps the bare code exactly as before.
void testFailedToolResultCarriesTheBackendExplanation()
{
    const auto contentOfFailedResult = [](const std::string& errorCode, const std::string& json) {
        auto client = std::make_shared<ScriptedHttpClient>();
        client->enqueue(makeJsonResponse(200, R"({
            "type": "message", "stop_reason": "tool_use",
            "content": [{"type": "tool_use", "id": "tid_x", "name": "workspace_read_file",
                         "input": {"path": "x.txt"}}],
            "usage": {}
        })"));
        client->enqueue(makeJsonResponse(200, R"({
            "type": "message", "stop_reason": "end_turn",
            "content": [{"type": "text", "text": "ok"}], "usage": {}
        })"));
        AnthropicAgentSession session("key", "https://fake", client);
        std::stop_source ss;
        const auto first = session.beginSession(
            makeRequest("read", {makeToolDef("workspace_read_file")}), ss.get_token());
        req(first.success(), "a-failedExplanation: first turn failed");
        AgentToolResult result;
        result.callId = "tid_x";
        result.toolName = "workspace_read_file";
        result.success = false;
        result.errorCode = errorCode;
        result.contentJson = json;
        const auto second = session.submitToolResults({std::move(result)}, ss.get_token());
        req(second.success(), "a-failedExplanation: second turn failed");
        const auto body = nlohmann::json::parse(client->capturedRequests()[1].body);
        const auto& block = body.at("messages").back().at("content").front();
        req(block.at("type").get<std::string>() == "tool_result"
                && block.at("is_error").get<bool>(),
            "a-failedExplanation: the failure must stay an is_error tool_result");
        return block.at("content").get<std::string>();
    };
    req(contentOfFailedResult("workspace.not_found", "")
            == "error: workspace.not_found",
        "a-failedExplanation: a failure without an explanation keeps the bare code");
    req(contentOfFailedResult("workspace_path.outside_allowed_roots", R"({"allowedReadPaths":["a.md"]})")
            == "error: workspace_path.outside_allowed_roots\n{\"allowedReadPaths\":[\"a.md\"]}",
        "a-failedExplanation: the Backend explanation must follow the code");
}

// ---------------------------------------------------------------------------
// max_tokens stop_reason → MaxTurns
// ---------------------------------------------------------------------------

void testMaxTokens()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "max_tokens",
        "content": [{"type": "text", "text": "partial answer"}],
        "usage": {"input_tokens": 5, "output_tokens": 1024}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(resp.success(), "a-maxTokens: expected ok");
    req(resp.value().finishReason == ProviderAgentFinishReason::MaxOutputTokens,
        "a-maxTokens: expected MaxOutputTokens");
}

// ---------------------------------------------------------------------------
// HTTP 401 → provider.auth_failed
// ---------------------------------------------------------------------------

void testHttp401()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(401,
        R"({"type":"error","error":{"type":"authentication_error","message":"Invalid key"}})"));

    AnthropicAgentSession session("bad-key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "a-401: expected failure");
    req(resp.error()->code == "provider.auth_failed", "a-401: wrong error code");
    req(resp.error()->message.find("Invalid key") != std::string::npos,
        "a-401: error message must include Anthropic's actual explanation");
}

// ---------------------------------------------------------------------------
// Regression (real Runs 7e44ab7e-.../3650caed-..., "Stage 2A-canary 12",
// 2026-09-11): an HTTP 400 must surface Anthropic's actual error message
// (e.g. naming the unsupported schema keyword), not just the bare status
// code - httpStatusError() previously discarded the response body entirely,
// making every failure indistinguishable as "Provider returned HTTP 400"
// and forcing this exact incident's root cause to be confirmed against
// Anthropic's own external documentation instead of the app's own
// diagnostics.
// ---------------------------------------------------------------------------

void testHttp400SurfacesApiErrorMessage()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(400,
        R"({"type":"error","error":{"type":"invalid_request_error",)"
        R"("message":"tools.0.input_schema: Extra inputs are not permitted: minLength"},)"
        R"("request_id":"req_012abc"})"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "http400Message: expected failure");
    req(resp.error()->code == "provider.request_failed", "http400Message: wrong error code");
    req(resp.error()->message.find("minLength") != std::string::npos,
        "http400Message: error message must include Anthropic's actual explanation");
    req(resp.error()->message.find("req_012abc") != std::string::npos,
        "http400Message: error message must include the request_id for support follow-up");
}

// ---------------------------------------------------------------------------
// Transport error → provider.network_error
// ---------------------------------------------------------------------------

void testTransportError()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeTransportError("CURL error: connection refused"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "a-transport: expected failure");
    req(resp.error()->code == "provider.network_error", "a-transport: wrong code");
}

// ---------------------------------------------------------------------------
// Top-level type == "error" (Anthropic API error body with HTTP 200)
// This is unusual but tested for robustness
// ---------------------------------------------------------------------------

void testTopLevelError()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200,
        R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "a-topLevelError: expected failure");
    req(resp.error()->code == "provider.request_failed",
        "a-topLevelError: wrong code");
    req(resp.error()->message.find("Overloaded") != std::string::npos,
        "a-topLevelError: error message must include Anthropic's actual explanation");
}

// ---------------------------------------------------------------------------
// input_schema in tools request: verify format accepted in roundtrip
// ---------------------------------------------------------------------------

void testToolsInputSchema()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "Done."}],
        "usage": {}
    })"));

    AgentToolDefinition def;
    def.name = "workspace_read_file";
    def.description = "Read a file";
    def.inputSchemaJson = R"({
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "File path"},
            "encoding": {"type": "string", "enum": ["utf-8", "binary"]}
        },
        "required": ["path"]
    })";
    def.readOnly = true;

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("read file", {std::move(def)}), ss.get_token());

    req(resp.success(), "a-inputSchema: beginSession failed");
    req(resp.value().finishReason == ProviderAgentFinishReason::Stop,
        "a-inputSchema: expected Stop");
}

// ---------------------------------------------------------------------------
// Regression (real Run "Stage 2B - Anthropic", 1865d44f-ac23-4444-8053-
// d24f4ba030a4, 2026-09-11): a strictInputSchema=true tool must NEVER put
// "strict":true on the actual Anthropic wire request, no matter how simple
// its schema is. An earlier fix stripped Anthropic-unsupported keywords
// per schema but still requested strict:true; that still failed for real
// with "The compiled grammar is too large..." because Anthropic compiles
// one combined grammar over every strict tool in a request and this
// codebase's terminal schemas reliably exceed its undocumented size
// budget regardless of per-keyword compatibility. The canonical schema
// (including minLength/maxItems - Anthropic never rejects these outside
// strict/grammar-compiled mode) is still sent as ordinary documentation
// for the model; only the provider-enforced grammar constraint is gone.
// The Backend parser/validator remains the actual authority regardless.
// ---------------------------------------------------------------------------

void testStrictToolNeverRequestsAnthropicGrammarCompilation()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "Done."}],
        "usage": {}
    })"));

    AgentToolDefinition def;
    def.name = "submit_thing";
    def.description = "Submit a thing";
    def.inputSchemaJson =
        R"({"type":"object","properties":{)"
        R"("id":{"type":"string","minLength":1,"maxLength":128},)"
        R"("items":{"type":"array","maxItems":32,"items":{"type":"string"}}},)"
        R"("required":["id","items"],"additionalProperties":false})";
    def.readOnly = true;
    def.strictInputSchema = true;

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("go", {std::move(def)}), ss.get_token());

    req(resp.success(), "a-noStrictGrammar: beginSession failed");
    req(client->capturedRequests().size() == 1, "a-noStrictGrammar: expected one HTTP request");
    const std::string& sentBody = client->capturedRequests().front().body;
    req(sentBody.find("\"strict\"") == std::string::npos,
        "a-noStrictGrammar: Anthropic must never be asked to compile a strict grammar "
        "for this codebase's terminal tool schemas");
    req(sentBody.find("minLength") != std::string::npos
            && sentBody.find("maxItems") != std::string::npos,
        "a-noStrictGrammar: the canonical schema's own constraints must still reach "
        "Anthropic as ordinary (non-grammar-compiled) documentation");
}

// ---------------------------------------------------------------------------
// State machine: beginSession twice → invalid_state
// ---------------------------------------------------------------------------

void testBeginTwice()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(makeRequest(), ss.get_token());
    req(r1.success(), "a-beginTwice: first call failed");

    auto r2 = session.beginSession(makeRequest(), ss.get_token());
    req(!r2.success(), "a-beginTwice: second call should fail");
    req(r2.error()->code == "agent_session.invalid_state",
        "a-beginTwice: wrong error code");
}

// ---------------------------------------------------------------------------
// State machine: submitToolResults before beginSession → not_started
// ---------------------------------------------------------------------------

void testSubmitBeforeBegin()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;

    AgentToolResult r;
    r.callId = "x";
    r.toolName = "workspace_read_file";
    r.success = true;
    auto resp = session.submitToolResults({std::move(r)}, ss.get_token());

    req(!resp.success(), "a-submitBeforeBegin: expected failure");
    req(resp.error()->code == "agent_session.not_started",
        "a-submitBeforeBegin: wrong code");
}

// ---------------------------------------------------------------------------
// Tool result correlation: unknown callId
// ---------------------------------------------------------------------------

void testUnknownCallId()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "tool_use",
        "content": [
            {"type": "tool_use", "id": "real_id",
             "name": "workspace_read_file", "input": {}}
        ],
        "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("read", {makeToolDef("workspace_read_file")}),
        ss.get_token());
    req(r1.success(), "a-unknownId: first turn failed");

    AgentToolResult result;
    result.callId = "wrong_id";
    result.toolName = "workspace_read_file";
    result.success = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());

    req(!r2.success(), "a-unknownId: expected failure");
    req(r2.error()->code == "agent_session.unknown_tool_result",
        "a-unknownId: wrong code");
}

// ---------------------------------------------------------------------------
// Malformed JSON → provider.invalid_response
// ---------------------------------------------------------------------------

void testMalformedJson()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, "{{not valid json"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());

    req(!resp.success(), "a-malformed: expected failure");
    req(resp.error()->code == "provider.invalid_response",
        "a-malformed: wrong code");
}

// ---------------------------------------------------------------------------
// Multiple tool_use blocks in one turn → multiple calls, preserved order
// ---------------------------------------------------------------------------

void testMultipleToolUse()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message",
        "stop_reason": "tool_use",
        "content": [
            {"type": "tool_use", "id": "t1",
             "name": "workspace_read_file", "input": {"path": "a.txt"}},
            {"type": "tool_use", "id": "t2",
             "name": "workspace_read_file", "input": {"path": "b.txt"}}
        ],
        "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("read both", {makeToolDef("workspace_read_file")}),
        ss.get_token());

    req(resp.success(), "a-multiUse: failed");
    req(resp.value().toolCalls.size() == 2, "a-multiUse: expected 2 calls");
    req(resp.value().toolCalls[0].callId == "t1", "a-multiUse: wrong callId[0]");
    req(resp.value().toolCalls[1].callId == "t2", "a-multiUse: wrong callId[1]");
    req(resp.value().toolCalls[0].ordinal == 0, "a-multiUse: wrong ordinal[0]");
    req(resp.value().toolCalls[1].ordinal == 1, "a-multiUse: wrong ordinal[1]");
}

// ---------------------------------------------------------------------------
// Package 2/3: strict tool schema wire semantics
// ---------------------------------------------------------------------------

// Updated 2026-09-11 (real Run "Stage 2B - Anthropic",
// 1865d44f-ac23-4444-8053-d24f4ba030a4): a strictInputSchema=true tool no
// longer requests strict:true from Anthropic at all (see
// testStrictToolNeverRequestsAnthropicGrammarCompilation below for why) -
// this test now confirms the schema itself still reaches Anthropic intact
// as ordinary documentation, with no strict guarantee attached.
void testStrictToolSendsSchemaWithoutStrictFlag()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("hi", {makeStrictToolDef("submit_execution_manifest")}), ss.get_token());
    req(resp.success(), "a-strictTool: beginSession failed");
    req(client->capturedRequests().size() == 1, "a-strictTool: expected exactly one HTTP call");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(body.at("tools").size() == 1, "a-strictTool: expected exactly one tool");
    const auto& tool = body.at("tools").front();
    req(!tool.contains("strict"), "a-strictTool: strict must never be sent to Anthropic");
    req(tool.at("input_schema").at("additionalProperties").get<bool>() == false,
        "a-strictTool: schema was not forwarded intact");
}

void testNonStrictToolNeverGetsStrictFlag()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(resp.success(), "a-nonStrictTool: beginSession failed");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(!body.at("tools").front().contains("strict"),
        "a-nonStrictTool: a non-strict tool must never carry a false strict guarantee");
}

// The real propose_change_set contract, not a synthetic strict tool: proves
// AgentToolTurnLoopRunner::proposeChangeSetToolDefinition() itself reaches
// the Anthropic wire without strict:true (see
// testStrictToolNeverRequestsAnthropicGrammarCompilation for why) but with
// its exact schema (schemaVersion/summary/changes, no
// expected_original_hash).
void testRealProposeChangeSetToolSendsExactSchemaWithoutStrictFlag()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));

    const auto toolDef = ai::application::proposeChangeSetToolDefinition();
    req(toolDef.strictInputSchema,
        "a-realProposeChangeSet: proposeChangeSetToolDefinition() must set strictInputSchema");

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("hi", {toolDef}), ss.get_token());
    req(resp.success(), "a-realProposeChangeSet: beginSession failed");
    req(client->capturedRequests().size() == 1,
        "a-realProposeChangeSet: expected exactly one HTTP call");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    const auto& tool = body.at("tools").front();
    req(!tool.contains("strict"),
        "a-realProposeChangeSet: strict must never be sent to Anthropic for this tool");
    const auto& schema = tool.at("input_schema");
    req(schema.at("additionalProperties").get<bool>() == false,
        "a-realProposeChangeSet: root additionalProperties:false was not forwarded");
    const auto& props = schema.at("properties");
    req(props.contains("schemaVersion") && props.contains("summary") && props.contains("changes"),
        "a-realProposeChangeSet: expected top-level fields missing from the wire schema");
    const auto& changeItemProps = props.at("changes").at("items").at("properties");
    req(!changeItemProps.contains("expected_original_hash"),
        "a-realProposeChangeSet: the wire schema must never carry a hash field");
    req(changeItemProps.contains("change_id") && changeItemProps.contains("operation")
            && changeItemProps.contains("relative_path") && changeItemProps.contains("source_path")
            && changeItemProps.contains("proposed_content"),
        "a-realProposeChangeSet: expected per-change fields missing from the wire schema");
}

void testRealScopeExpansionToolSendsSchemaWithoutStrictFlag()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type":"message","stop_reason":"end_turn",
        "content":[{"type":"text","text":"ok"}],"usage":{}
    })"));
    const auto def = ai::application::requestScopeExpansionToolDefinition();
    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    const auto response = session.beginSession(makeRequest("hi", {def}), ss.get_token());
    req(response.success(), "a-scopeExpansion: beginSession failed");
    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    const auto& tool = body.at("tools").front();
    req(!tool.contains("strict"), "a-scopeExpansion: strict must never be sent to Anthropic");
    const auto& schema = tool.at("input_schema");
    req(schema.at("additionalProperties").get<bool>() == false,
        "a-scopeExpansion: schema was not forwarded intact");
    req(schema.at("properties").at("role").at("type") == "string",
        "a-scopeExpansion: enum field lost its explicit type");
}

void testMalformedStrictSchemaJsonFailsFastBeforeHttpSend()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    // Deliberately no scripted response: a real HTTP call here would throw
    // and fail this test, proving fail-fast happened before any send().
    auto badDef = makeStrictToolDef("submit_execution_manifest");
    badDef.inputSchemaJson = "{not valid json";

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("hi", {badDef}), ss.get_token());
    req(!resp.success(), "a-malformedStrictJson: expected failure");
    req(resp.error()->code == "agent_session.malformed_strict_schema",
        "a-malformedStrictJson: wrong error code");
    req(client->capturedRequests().empty(), "a-malformedStrictJson: no HTTP call must be made");
}

void testStructurallyInvalidStrictSchemaFailsFastBeforeHttpSend()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    auto badDef = makeStrictToolDef("submit_execution_manifest");
    // Valid JSON, but missing additionalProperties:false - not strict-compatible.
    badDef.inputSchemaJson = R"({"type":"object","properties":{"path":{"type":"string"}},)"
                              R"("required":["path"]})";

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest("hi", {badDef}), ss.get_token());
    req(!resp.success(), "a-structurallyInvalid: expected failure");
    req(resp.error()->code == "agent_session.malformed_strict_schema",
        "a-structurallyInvalid: wrong error code");
    req(client->capturedRequests().empty(), "a-structurallyInvalid: no HTTP call must be made");
}

// ---------------------------------------------------------------------------
// Package 4: next-turn tool policy wire semantics
// ---------------------------------------------------------------------------

void testNextTurnPolicyRestrictsToolsAndDisablesParallel()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1",
                      "name": "workspace_read_file", "input": {"path": "a.txt"}}],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "done"}], "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto request = makeRequest("hi", {makeToolDef("workspace_read_file"),
                                       makeStrictToolDef("submit_execution_manifest")});
    request.systemPrompt = "stable system";
    auto r1 = session.beginSession(request, ss.get_token());
    req(r1.success(), "a-nextTurnPolicy: first turn failed");

    AgentToolResult result;
    result.callId = "t1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";

    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"submit_execution_manifest"};
    policy.disallowParallelToolCalls = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token(), policy);
    req(r2.success(), "a-nextTurnPolicy: second turn failed");
    req(client->capturedRequests().size() == 2,
        "a-nextTurnPolicy: expected exactly two HTTP calls");

    const auto body = nlohmann::json::parse(client->capturedRequests()[1].body);
    req(body.at("tools").size() == 1,
        "a-nextTurnPolicy: restricted turn must advertise only the allowed tool");
    req(body.at("tools").front().at("name").get<std::string>() == "submit_execution_manifest",
        "a-nextTurnPolicy: wrong tool survived the restriction");
    req(!body.at("tools").front().contains("cache_control")
            && !body.at("system").front().contains("cache_control"),
        "a-nextTurnPolicy: a dynamic tool prefix must not receive a one-hour breakpoint "
        "directly or through the following system block");
    req(body.at("tool_choice").at("type").get<std::string>() == "tool",
        "a-nextTurnPolicy: a one-tool policy must use Anthropic's named-tool forcing");
    req(body.at("tool_choice").at("name").get<std::string>()
            == "submit_execution_manifest",
        "a-nextTurnPolicy: named-tool forcing selected the wrong terminal tool");
    req(body.at("tool_choice").at("disable_parallel_tool_use").get<bool>() == true,
        "a-nextTurnPolicy: disallowParallelToolCalls did not serialize to disable_parallel_tool_use");
}

void testNextTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1",
                      "name": "workspace_read_file", "input": {"path": "a.txt"}}],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "done"}], "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file"),
                           makeStrictToolDef("submit_execution_manifest"),
                           makeStrictToolDef("submit_no_changes_needed")}),
        ss.get_token());
    req(r1.success(), "a-multiNextTurnPolicy: first turn failed");

    AgentToolResult result;
    result.callId = "t1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";

    NextTurnToolPolicy policy;
    policy.allowedToolNames = {
        "submit_execution_manifest", "submit_no_changes_needed"};
    policy.disallowParallelToolCalls = true;
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token(), policy);
    req(r2.success(), "a-multiNextTurnPolicy: second turn failed");

    const auto body = nlohmann::json::parse(client->capturedRequests()[1].body);
    req(body.at("tools").size() == 2,
        "a-multiNextTurnPolicy: both allowed terminal tools must remain advertised");
    req(body.at("tool_choice").at("type").get<std::string>() == "any",
        "a-multiNextTurnPolicy: several allowed tools must retain required choice within subset");
    req(!body.at("tool_choice").contains("name"),
        "a-multiNextTurnPolicy: a multi-tool policy must not force its first entry");
    req(body.at("tool_choice").at("disable_parallel_tool_use").get<bool>(),
        "a-multiNextTurnPolicy: the required choice must remain single-call");
}

void testNextTurnPolicyUnknownToolNameIsRejected()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1",
                      "name": "workspace_read_file", "input": {"path": "a.txt"}}],
        "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto r1 = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file")}), ss.get_token());
    req(r1.success(), "a-unknownToolPolicy: first turn failed");

    AgentToolResult result;
    result.callId = "t1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";

    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"never_advertised_tool"};
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token(), policy);
    req(!r2.success(), "a-unknownToolPolicy: expected failure");
    req(r2.error()->code == "agent_session.invalid_next_turn_policy",
        "a-unknownToolPolicy: wrong error code");
    req(client->capturedRequests().size() == 1,
        "a-unknownToolPolicy: no second HTTP call must be made for a rejected policy");
}

// ---------------------------------------------------------------------------
// Worker capacity fairness: the same one-tool/several-tools wire contract for
// the FIRST turn (ProviderAgentSessionRequest::firstTurnToolPolicy), which a
// NextTurnToolPolicy passed to submitToolResults() can never reach.
// ---------------------------------------------------------------------------

void testFirstTurnPolicyForcesTheSingleAllowedToolByName()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1",
                      "name": "propose_change_set", "input": {}}],
        "usage": {}
    })"));

    auto request = makeRequest("hi", {makeToolDef("workspace_read_file"),
                                       makeToolDef("propose_change_set")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"propose_change_set"};
    policy.disallowParallelToolCalls = true;
    request.firstTurnToolPolicy = policy;

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto first = session.beginSession(request, ss.get_token());
    req(first.success(), "a-firstTurnPolicy: first turn failed");

    const auto body = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(body.at("tools").size() == 1
            && body.at("tools").front().at("name").get<std::string>() == "propose_change_set",
        "a-firstTurnPolicy: the first turn must advertise only the allowed tool");
    req(body.at("tool_choice").at("type").get<std::string>() == "tool"
            && body.at("tool_choice").at("name").get<std::string>() == "propose_change_set",
        "a-firstTurnPolicy: a one-tool first-turn policy must force the tool by name");
    req(body.at("tool_choice").at("disable_parallel_tool_use").get<bool>(),
        "a-firstTurnPolicy: disallowParallelToolCalls must serialize to disable_parallel_tool_use");
}

void testFirstTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1",
                      "name": "propose_change_set", "input": {}}],
        "usage": {}
    })"));

    auto request = makeRequest("hi", {makeToolDef("workspace_read_file"),
                                       makeToolDef("propose_change_set"),
                                       makeToolDef("submit_task_no_changes_needed")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"propose_change_set", "submit_task_no_changes_needed"};
    policy.disallowParallelToolCalls = true;
    request.firstTurnToolPolicy = policy;

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    req(session.beginSession(request, ss.get_token()).success(),
        "a-firstTurnPolicyMulti: first turn failed");
    const auto body = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(body.at("tools").size() == 2,
        "a-firstTurnPolicyMulti: both terminal tools must remain advertised");
    req(body.at("tool_choice").at("type").get<std::string>() == "any"
            && !body.at("tool_choice").contains("name"),
        "a-firstTurnPolicyMulti: several allowed tools must keep required choice within the subset");
    req(body.at("tool_choice").at("disable_parallel_tool_use").get<bool>(),
        "a-firstTurnPolicyMulti: the required choice must stay single-call");
}

void testFirstTurnPolicyUnknownToolNameIsRejectedBeforeAnyRequest()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    auto request = makeRequest("hi", {makeToolDef("workspace_read_file")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"never_advertised_tool"};
    request.firstTurnToolPolicy = policy;

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    const auto result = session.beginSession(request, ss.get_token());
    req(!result.success()
            && result.error()->code == "agent_session.invalid_first_turn_policy",
        "a-firstTurnPolicyUnknown: a policy naming an unadvertised tool must be rejected");
    req(client->capturedRequests().empty(),
        "a-firstTurnPolicyUnknown: no HTTP call may be made for a rejected policy");
}

void testWithoutFirstTurnPolicyTheFirstTurnStaysUnrestricted()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));
    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    req(session.beginSession(
            makeRequest("hi", {makeToolDef("workspace_read_file"),
                               makeToolDef("propose_change_set")}),
            ss.get_token()).success(),
        "a-noFirstTurnPolicy: first turn failed");
    const auto body = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(body.at("tools").size() == 2
            && body.at("tool_choice").at("type").get<std::string>() == "any",
        "a-noFirstTurnPolicy: an absent policy must leave the first turn exactly as before");
}

// ---------------------------------------------------------------------------
// MW-P2: prompt caching wire protocol and cache-aware usage accounting
// ---------------------------------------------------------------------------

void testSystemPromptCarriesCacheControlBreakpoint()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));

    auto request = makeRequest();
    request.systemPrompt = "You are a helpful agent.";

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(request, ss.get_token());
    req(resp.success(), "a-systemCache: beginSession failed");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(body.at("system").is_array() && body.at("system").size() == 1,
        "a-systemCache: system prompt must be sent as one block, not a plain string");
    req(body.at("system").front().at("text").get<std::string>() == request.systemPrompt,
        "a-systemCache: system block lost the original prompt text");
    req(body.at("system").front().at("cache_control").at("type").get<std::string>()
            == "ephemeral",
        "a-systemCache: system prompt must carry an ephemeral cache checkpoint");
    req(body.at("system").front().at("cache_control").at("ttl").get<std::string>() == "1h",
        "a-systemCache: the stable system prefix must use the one-hour duration");
}

void testLastToolCarriesCacheControlBreakpointOnly()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(
        makeRequest("hi", {makeToolDef("workspace_read_file"),
                           makeToolDef("workspace_list_directory")}),
        ss.get_token());
    req(resp.success(), "a-toolCache: beginSession failed");

    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(body.at("tools").size() == 2, "a-toolCache: expected both tools");
    req(!body.at("tools")[0].contains("cache_control"),
        "a-toolCache: only the LAST tool should carry the cache checkpoint - marking every "
        "tool wastes one of Anthropic's 4-breakpoint budget for no benefit");
    req(body.at("tools")[1].at("cache_control").at("type").get<std::string>() == "ephemeral",
        "a-toolCache: the last tool must carry the cache checkpoint");
    req(body.at("tools")[1].at("cache_control").at("ttl").get<std::string>() == "1h",
        "a-toolCache: the stable tool roster must use the one-hour duration");
}

// The heart of MW-P2's cost fix: the growing message history must repeat an
// identical, cacheable prefix every turn. Only the newest message may carry
// the checkpoint - if an older message kept one, or the newest lacked one,
// Anthropic would either waste breakpoints or never find a matching cached
// prefix, and the quadratic-cost incident this package closes would recur.
void testCacheCheckpointMovesToTheNewestMessageEachTurn()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1",
                      "name": "workspace_read_file", "input": {"path": "a.txt"}}],
        "usage": {}
    })"));
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "done"}], "usage": {}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto request = makeRequest("hi", {makeToolDef("workspace_read_file")});
    request.systemPrompt = "stable system";
    auto r1 = session.beginSession(request, ss.get_token());
    req(r1.success(), "a-movingCheckpoint: first turn failed");

    const auto firstBody = nlohmann::json::parse(client->capturedRequests()[0].body);
    req(firstBody.at("messages").size() == 1,
        "a-movingCheckpoint: turn 1 must send exactly the seed user message");
    req(firstBody.at("messages")[0].at("content").back().contains("cache_control"),
        "a-movingCheckpoint: turn 1's only message must carry the checkpoint");
    req(!firstBody.at("messages")[0].at("content").back().at("cache_control").contains("ttl"),
        "a-movingCheckpoint: the moving message must keep the compatible default five-minute TTL");

    AgentToolResult result;
    result.callId = "t1";
    result.toolName = "workspace_read_file";
    result.success = true;
    result.contentJson = "{}";
    auto r2 = session.submitToolResults({std::move(result)}, ss.get_token());
    req(r2.success(), "a-movingCheckpoint: second turn failed");

    const auto secondBody = nlohmann::json::parse(client->capturedRequests()[1].body);
    const auto& messages = secondBody.at("messages");
    req(messages.size() == 3,
        "a-movingCheckpoint: turn 2 must resend user+assistant+new tool_result");
    req(!messages[0].at("content").back().contains("cache_control"),
        "a-movingCheckpoint: the original user message must NOT carry a checkpoint once it is "
        "no longer the newest message");
    req(!messages[1].at("content").back().contains("cache_control"),
        "a-movingCheckpoint: the assistant turn must NOT carry a checkpoint once it is no "
        "longer the newest message");
    req(messages[2].at("content").back().contains("cache_control"),
        "a-movingCheckpoint: the new tool_result message must carry this turn's checkpoint");
    req(!messages[2].at("content").back().at("cache_control").contains("ttl"),
        "a-movingCheckpoint: the newest moving message must omit ttl (five-minute compatibility)");
    req(secondBody.at("tools").back().at("cache_control").at("ttl") == "1h"
            && secondBody.at("system").front().at("cache_control").at("ttl") == "1h",
        "a-movingCheckpoint: a later turn with the complete stable prefix must retain both "
        "one-hour breakpoints");
}

void testFilteredToolCatalogIsNotGivenAOneHourCheckpoint()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}], "usage": {}
    })"));
    auto request = makeRequest("hi", {makeToolDef("read"), makeToolDef("finish")});
    NextTurnToolPolicy policy;
    policy.allowedToolNames = {"finish"};
    request.firstTurnToolPolicy = policy;
    request.systemPrompt = "stable";

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    const auto response = session.beginSession(request, ss.get_token());
    req(response.success(), "a-filteredCache: request failed");
    const auto body = nlohmann::json::parse(client->capturedRequests().front().body);
    req(body.at("tools").size() == 1 && !body.at("tools").back().contains("cache_control"),
        "a-filteredCache: a changing filtered roster must not incur a one-hour cache write");
    req(!body.at("system").front().contains("cache_control"),
        "a-filteredCache: the system breakpoint would include the preceding dynamic tools and "
        "must therefore be suppressed too");
    req(!body.at("messages").back().at("content").back().at("cache_control").contains("ttl"),
        "a-filteredCache: the shorter moving breakpoint must remain after the stable prefix");
}

void testCacheUsageIsParsedAndSummedIntoConservativeInputTokens()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {"input_tokens": 50, "output_tokens": 10,
                   "cache_creation_input_tokens": 200, "cache_read_input_tokens": 4000,
                   "cache_creation": {"ephemeral_5m_input_tokens": 75,
                                      "ephemeral_1h_input_tokens": 125}}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());
    req(resp.success(), "a-cacheUsage: beginSession failed");

    req(resp.value().usage.cacheCreationInputTokens == 200,
        "a-cacheUsage: cache_creation_input_tokens was not parsed");
    req(resp.value().usage.cacheReadInputTokens == 4000,
        "a-cacheUsage: cache_read_input_tokens was not parsed");
    // inputTokens must remain the conservative TOTAL (fresh + cache-write +
    // cache-read) so MW-P0's budget guard, which only reads inputTokens,
    // keeps seeing the true amount of input-side volume this turn cost -
    // caching must never make a turn look cheaper to the safety barrier.
    req(resp.value().usage.inputTokens == 50 + 200 + 4000,
        "a-cacheUsage: inputTokens must be the sum of fresh+cache-creation+cache-read, "
        "never just the fresh input_tokens field");
    req(resp.value().usage.outputTokens == 10, "a-cacheUsage: outputTokens was not parsed");
    req(resp.value().usage.cacheCreationDuration.known
            && resp.value().usage.cacheCreationDuration.fiveMinuteTokens == 75
            && resp.value().usage.cacheCreationDuration.oneHourTokens == 125,
        "a-cacheUsage: detailed duration usage was not preserved exactly");
    // AG-17 gpt-6-sol checkpoint M2b: the long-context partition is Backend-derived; a provider
    // adapter never fills it and never marks its usage untrusted.
    req(resp.value().usage.longContext.empty() && resp.value().usage.accountingTrusted,
        "a-cacheUsage: a session adapter must never fill the long-context partition");
}

void testMissingCacheUsageFieldsDefaultToZeroNotError()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {"input_tokens": 12, "output_tokens": 4}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    auto resp = session.beginSession(makeRequest(), ss.get_token());
    req(resp.success(), "a-noCacheFields: beginSession failed");
    req(resp.value().usage.cacheCreationInputTokens == 0,
        "a-noCacheFields: missing cache_creation_input_tokens must default to zero");
    req(resp.value().usage.cacheReadInputTokens == 0,
        "a-noCacheFields: missing cache_read_input_tokens must default to zero");
    req(resp.value().usage.inputTokens == 12,
        "a-noCacheFields: inputTokens must equal the plain input_tokens when no cache "
        "categories are reported");
    req(!resp.value().usage.cacheCreationDuration.known,
        "a-noCacheFields: an omitted detailed partition must remain explicitly unknown");
}

void testOneHourWriteWithoutDurationDetailIsNeverExactlyPriced()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "ok"}],
        "usage": {"input_tokens": 10, "output_tokens": 2,
                  "cache_creation_input_tokens": 7}
    })"));

    AnthropicAgentSession session("key", "https://fake", client);
    auto request = makeRequest();
    request.systemPrompt = "stable system prefix";
    std::stop_source ss;
    const auto response = session.beginSession(std::move(request), ss.get_token());
    req(response.success(), "a-missingDuration: the paid response must remain available");
    req(response.value().usage.cacheCreationInputTokens == 7
            && !response.value().usage.cacheCreationDuration.known
            && !response.value().usage.accountingTrusted,
        "a-missingDuration: a one-hour write without its TTL split looked exact");
}

void testMalformedCacheDurationUsageFailsClosed()
{
    auto client = std::make_shared<ScriptedHttpClient>();
    client->enqueue(makeJsonResponse(200, R"({
        "type": "message", "stop_reason": "tool_use",
        "content": [{"type": "tool_use", "id": "t1", "name": "read", "input": {}}],
        "usage": {"input_tokens": 10, "output_tokens": 2,
                  "cache_creation_input_tokens": 7,
                  "cache_creation": {"ephemeral_5m_input_tokens": 3,
                                     "ephemeral_1h_input_tokens": 3}}
    })"));
    AnthropicAgentSession session("key", "https://fake", client);
    std::stop_source ss;
    const auto response = session.beginSession(
        makeRequest("hi", {makeToolDef("read")}), ss.get_token());
    req(!response.success(), "a-badDuration: mismatched duration detail must fail");
    req(response.error()->code == "cache_duration.partition_total_mismatch",
        "a-badDuration: the shared ACD-1 error taxonomy must cross the provider boundary");
}

} // namespace

int runAnthropicAgentSessionTests()
{
    testSingleTurnStop();
    testToolUseTurn();
    testMixedTextAndToolCall();
    testSubmitToolResults();
    testFailedToolResult();
    testFailedToolResultCarriesTheBackendExplanation();
    testMaxTokens();
    testHttp401();
    testHttp400SurfacesApiErrorMessage();
    testTransportError();
    testTopLevelError();
    testToolsInputSchema();
    testStrictToolNeverRequestsAnthropicGrammarCompilation();
    testBeginTwice();
    testSubmitBeforeBegin();
    testUnknownCallId();
    testMalformedJson();
    testMultipleToolUse();
    testStrictToolSendsSchemaWithoutStrictFlag();
    testRealProposeChangeSetToolSendsExactSchemaWithoutStrictFlag();
    testRealScopeExpansionToolSendsSchemaWithoutStrictFlag();
    testNonStrictToolNeverGetsStrictFlag();
    testMalformedStrictSchemaJsonFailsFastBeforeHttpSend();
    testStructurallyInvalidStrictSchemaFailsFastBeforeHttpSend();
    testNextTurnPolicyRestrictsToolsAndDisablesParallel();
    testNextTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset();
    testNextTurnPolicyUnknownToolNameIsRejected();
    testFirstTurnPolicyForcesTheSingleAllowedToolByName();
    testFirstTurnPolicyWithSeveralAllowedToolsKeepsRequiredChoiceWithinSubset();
    testFirstTurnPolicyUnknownToolNameIsRejectedBeforeAnyRequest();
    testWithoutFirstTurnPolicyTheFirstTurnStaysUnrestricted();
    testSystemPromptCarriesCacheControlBreakpoint();
    testLastToolCarriesCacheControlBreakpointOnly();
    testCacheCheckpointMovesToTheNewestMessageEachTurn();
    testFilteredToolCatalogIsNotGivenAOneHourCheckpoint();
    testCacheUsageIsParsedAndSummedIntoConservativeInputTokens();
    testMissingCacheUsageFieldsDefaultToZeroNotError();
    testOneHourWriteWithoutDurationDetailIsNeverExactlyPriced();
    testMalformedCacheDurationUsageFailsClosed();
    return 0;
}
