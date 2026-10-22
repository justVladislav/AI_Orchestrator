// Contract tests for the provider-neutral agent session protocol.
//
// Coverage:
//   - fail-closed default on AgentToolResult
//   - type defaults
//   - state machine happy paths (1, 3, 2+ turns)
//   - callId round-trip and correlation
//   - structured tool error flow
//   - illegal state transitions (submit before begin, double begin, submit after terminal)
//   - strict result correlation (missing, unknown, duplicate, name mismatch, empty)
//   - inline validators: validateTurnResponse and validateSessionRequest
//   - stop_token cancellation
//   - exhausted scripted session
//   - legacy IProviderClient::send() contract is unchanged and independent

#include "ai/application/IProviderClient.hpp"
#include "ai/providers/IProviderAgentSession.hpp"
#include "ai/providers/ProviderAgentSessionTypes.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

namespace common = ai::common;
using namespace ai::providers;
namespace app = ai::application;

// ---------------------------------------------------------------------------
// ScriptedProviderAgentSession — test-only fake
//
// Replays a pre-configured sequence of ProviderAgentTurnResponse objects.
// Does not make network calls.
// Enforces the full state machine and strict result-correlation rules.
// Never exposes argumentsJson, contentJson, or callIds in error messages.
// ---------------------------------------------------------------------------

class ScriptedProviderAgentSession final : public IProviderAgentSession {
public:
    explicit ScriptedProviderAgentSession(
        std::vector<ProviderAgentTurnResponse> script)
        : script_(std::move(script))
    {}

    [[nodiscard]] const std::vector<app::AgentToolCall>& outstandingCalls() const noexcept
    { return outstanding_; }

    [[nodiscard]] const std::vector<app::AgentToolResult>& lastSubmittedResults() const noexcept
    { return lastSubmitted_; }

    [[nodiscard]] common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& /*request*/,
        std::stop_token st) override
    {
        if (state_ != State::Idle)
            return sessionFail("agent_session.invalid_state",
                               "beginSession() called after session already started");
        return advance(st);
    }

    [[nodiscard]] common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<app::AgentToolResult> results,
        std::stop_token st,
        std::optional<NextTurnToolPolicy> = std::nullopt) override
    {
        if (state_ == State::Idle)
            return sessionFail("agent_session.not_started",
                               "submitToolResults() called before beginSession()");
        if (state_ == State::Terminal)
            return sessionFail("agent_session.already_terminal",
                               "session is finished; no further turns are possible");

        // Strict result-correlation validation (state_ == WaitingForToolResults)
        auto v = validateResultCorrelation(outstanding_, results);
        if (!v.success())
            return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

        lastSubmitted_ = std::move(results);
        outstanding_.clear();
        return advance(st);
    }

private:
    enum class State { Idle, WaitingForToolResults, Terminal };

    // Validates that results exactly match the outstanding calls by callId and toolName.
    // Error messages do not include callIds, tool names, or JSON content.
    [[nodiscard]] static common::Result<void> validateResultCorrelation(
        const std::vector<app::AgentToolCall>&   outstanding,
        const std::vector<app::AgentToolResult>& results)
    {
        if (results.empty() && !outstanding.empty())
            return vFail("agent_session.empty_tool_results",
                         "no results submitted for outstanding calls");

        // Detect unknown callIds and toolName mismatches in a single pass.
        // Also collect the seen set for duplicate and missing checks.
        std::vector<std::string> seen;
        seen.reserve(results.size());
        for (const auto& r : results) {
            bool found = false;
            for (const auto& c : outstanding) {
                if (c.callId == r.callId) {
                    if (c.toolName != r.toolName)
                        return vFail("agent_session.tool_name_mismatch",
                                     "result toolName does not match the outstanding call");
                    found = true;
                    break;
                }
            }
            if (!found)
                return vFail("agent_session.unknown_tool_result",
                             "result references a callId not in the outstanding set");
            for (const auto& s : seen)
                if (s == r.callId)
                    return vFail("agent_session.duplicate_tool_result",
                                 "the same callId was submitted more than once");
            seen.push_back(r.callId);
        }

        // All outstanding calls must be covered.
        for (const auto& c : outstanding) {
            bool covered = false;
            for (const auto& s : seen)
                if (s == c.callId) { covered = true; break; }
            if (!covered)
                return vFail("agent_session.missing_tool_result",
                             "a required callId has no corresponding result");
        }
        return common::Result<void>::ok();
    }

    common::Result<ProviderAgentTurnResponse> advance(std::stop_token st)
    {
        if (st.stop_requested()) {
            state_ = State::Terminal;
            return sessionFail("agent_session.stop_requested",
                               "cancelled via stop_token");
        }
        if (idx_ >= script_.size()) {
            state_ = State::Terminal;
            return sessionFail("agent_session.script_exhausted",
                               "no more scripted turns available");
        }
        ProviderAgentTurnResponse resp = script_[idx_++];
        if (resp.finishReason == ProviderAgentFinishReason::ToolUse) {
            outstanding_ = resp.toolCalls;
            state_ = State::WaitingForToolResults;
        } else {
            outstanding_.clear();
            state_ = State::Terminal;
        }
        return common::Result<ProviderAgentTurnResponse>::ok(std::move(resp));
    }

    static common::Result<ProviderAgentTurnResponse> sessionFail(
        std::string code, std::string msg)
    { return common::Result<ProviderAgentTurnResponse>::failure({std::move(code), std::move(msg)}); }

    static common::Result<void> vFail(std::string code, std::string msg)
    { return common::Result<void>::failure({std::move(code), std::move(msg)}); }

    std::vector<ProviderAgentTurnResponse>   script_;
    std::size_t                              idx_ {0};
    State                                    state_ {State::Idle};
    std::vector<app::AgentToolCall>          outstanding_;
    std::vector<app::AgentToolResult>        lastSubmitted_;
};

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

app::AgentToolCall makeCall(std::string callId, std::string toolName,
                            std::size_t ordinal = 0)
{
    app::AgentToolCall c;
    c.callId   = std::move(callId);
    c.toolName = std::move(toolName);
    c.ordinal  = ordinal;
    return c;
}

// success defaults to true so tests that care about the happy path are concise;
// tests specifically verifying the fail-closed default construct directly.
app::AgentToolResult makeResult(std::string callId, std::string toolName,
                                bool success = true,
                                std::string contentJson = "{}",
                                std::string errorCode   = {})
{
    app::AgentToolResult r;
    r.callId       = std::move(callId);
    r.toolName     = std::move(toolName);
    r.success      = success;
    r.contentJson  = std::move(contentJson);
    r.errorCode    = std::move(errorCode);
    r.returnedBytes = r.contentJson.size();
    return r;
}

ProviderAgentTurnResponse toolUseTurn(
    std::size_t idx, std::vector<app::AgentToolCall> calls)
{
    ProviderAgentTurnResponse r;
    r.finishReason = ProviderAgentFinishReason::ToolUse;
    r.turnIndex    = idx;
    r.toolCalls    = std::move(calls);
    return r;
}

ProviderAgentTurnResponse stopTurn(std::size_t idx, std::string text = {})
{
    ProviderAgentTurnResponse r;
    r.finishReason  = ProviderAgentFinishReason::Stop;
    r.turnIndex     = idx;
    r.assistantText = std::move(text);
    return r;
}

ProviderAgentSessionRequest makeRequest()
{
    ProviderAgentSessionRequest req;
    req.providerId  = "test-provider";
    req.modelId     = "test-model-id";
    req.apiModelId  = "claude-sonnet-5";
    req.systemPrompt = "You are a test assistant.";
    req.userContent  = "Test query.";
    return req;
}

// ---------------------------------------------------------------------------
// Group 1: Fail-closed defaults and type sanity
// ---------------------------------------------------------------------------

void testDefaultAgentToolResultIsFailClosed()
{
    // The canonical Application type must default to success == false.
    // A default-constructed result must never be treated as a success.
    app::AgentToolResult result;
    assert(!result.success);
    assert(result.callId.empty());
    assert(result.toolName.empty());
    assert(result.errorCode.empty());
    assert(!result.truncated);
    assert(result.returnedBytes == 0);
}

void testTypeDefaults()
{
    // ProviderAgentSessionRequest defaults
    ProviderAgentSessionRequest req;
    assert(req.maxTurns            == 16);
    assert(req.maxTotalToolCalls   == 64);
    assert(req.maxToolCallsPerTurn == 8);
    assert(req.maxOutputTokens     == 4096u);

    // ProviderAgentTurnResponse defaults
    ProviderAgentTurnResponse turn;
    assert(turn.finishReason == ProviderAgentFinishReason::Stop);
    assert(turn.turnIndex    == 0);
    assert(turn.toolCalls.empty());
    assert(turn.assistantText.empty());
    assert(turn.errorCode.empty());

    // AgentToolDefinition defaults
    app::AgentToolDefinition def;
    assert(def.readOnly);
    assert(def.maxResultBytes == 256ULL * 1024);
    assert(def.version == "1");
}

// ---------------------------------------------------------------------------
// Group 2: Happy paths
// ---------------------------------------------------------------------------

void testSingleToolCallAndFinalAnswer()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("c1", "workspace_read_file")}));
    script.push_back(stopTurn(1, "Here is the file summary."));

    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    assert(t0.value().finishReason == ProviderAgentFinishReason::ToolUse);
    assert(t0.value().turnIndex    == 0);
    assert(t0.value().toolCalls.size() == 1);
    assert(t0.value().toolCalls[0].toolName == "workspace_read_file");

    auto t1 = session.submitToolResults(
        {makeResult("c1", "workspace_read_file", true, "{\"content\":\"hello\"}")},
        src.get_token());
    assert(t1.success());
    assert(t1.value().finishReason   == ProviderAgentFinishReason::Stop);
    assert(t1.value().turnIndex      == 1);
    assert(t1.value().assistantText  == "Here is the file summary.");
    assert(t1.value().toolCalls.empty());
}

void testMultipleToolCallsInOneTurn()
{
    std::vector<app::AgentToolCall> calls;
    calls.push_back(makeCall("cA", "workspace_list_directory", 0));
    calls.push_back(makeCall("cB", "workspace_stat_path",      1));
    calls.push_back(makeCall("cC", "workspace_hash_file",      2));
    for (auto& c : calls) c.turnIndex = 0;

    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, calls));
    script.push_back(stopTurn(1, "All three processed."));

    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    assert(t0.value().toolCalls.size() == 3);
    assert(t0.value().toolCalls[0].toolName == "workspace_list_directory");
    assert(t0.value().toolCalls[1].toolName == "workspace_stat_path");
    assert(t0.value().toolCalls[2].toolName == "workspace_hash_file");

    std::vector<app::AgentToolResult> results;
    results.push_back(makeResult("cA", "workspace_list_directory", true, "{\"entries\":[]}"));
    results.push_back(makeResult("cB", "workspace_stat_path",      true, "{\"size\":42}"));
    results.push_back(makeResult("cC", "workspace_hash_file",      true, "{\"sha256\":\"abc\"}"));

    auto t1 = session.submitToolResults(std::move(results), src.get_token());
    assert(t1.success());
    assert(t1.value().finishReason == ProviderAgentFinishReason::Stop);
}

void testTwoSequentialToolTurns()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("x0", "workspace_list_directory")}));
    script.push_back(toolUseTurn(1, {makeCall("x1", "workspace_read_file")}));
    script.push_back(stopTurn(2, "Analysis complete."));

    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    assert(t0.value().finishReason == ProviderAgentFinishReason::ToolUse);
    assert(t0.value().turnIndex    == 0);

    auto t1 = session.submitToolResults(
        {makeResult("x0", "workspace_list_directory", true, "{\"entries\":[\"main.cpp\"]}")},
        src.get_token());
    assert(t1.success());
    assert(t1.value().finishReason == ProviderAgentFinishReason::ToolUse);
    assert(t1.value().turnIndex    == 1);
    assert(t1.value().toolCalls[0].callId == "x1");

    auto t2 = session.submitToolResults(
        {makeResult("x1", "workspace_read_file", true, "{\"content\":\"int main(){}\"}")},
        src.get_token());
    assert(t2.success());
    assert(t2.value().finishReason  == ProviderAgentFinishReason::Stop);
    assert(t2.value().turnIndex     == 2);
    assert(t2.value().assistantText == "Analysis complete.");
}

void testCallIdPreserved()
{
    const std::string expectedId = "provider-call-xyz-9999";

    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall(expectedId, "workspace_search_text")}));
    script.push_back(stopTurn(1, "search done"));

    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    const std::string& receivedId = t0.value().toolCalls[0].callId;
    assert(receivedId == expectedId);

    // Submit using the exact received id.
    auto t1 = session.submitToolResults(
        {makeResult(receivedId, "workspace_search_text", true, "{\"matches\":[]}")},
        src.get_token());
    assert(t1.success());

    // Verify the result was stored with the unchanged id.
    assert(session.lastSubmittedResults().size() == 1);
    assert(session.lastSubmittedResults()[0].callId == expectedId);
}

void testToolErrorReturnedAsStructuredResult()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("ce", "workspace_read_file")}));
    script.push_back(stopTurn(1, "I could not read the file."));

    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());

    // Submit a structured error result (success == false, errorCode set).
    auto errResult = makeResult("ce", "workspace_read_file",
                                /*success=*/false,
                                /*contentJson=*/"{\"error\":\"denied\"}",
                                /*errorCode=*/"tool.path_traversal");
    assert(!errResult.success);
    assert(errResult.errorCode == "tool.path_traversal");

    auto t1 = session.submitToolResults({errResult}, src.get_token());
    assert(t1.success());
    assert(t1.value().finishReason == ProviderAgentFinishReason::Stop);

    // Verify the error result is stored intact.
    const auto& sub = session.lastSubmittedResults();
    assert(sub.size() == 1);
    assert(!sub[0].success);
    assert(sub[0].errorCode == "tool.path_traversal");
}

// ---------------------------------------------------------------------------
// Group 3: Illegal state transitions
// ---------------------------------------------------------------------------

void testSubmitBeforeBegin()
{
    ScriptedProviderAgentSession session({stopTurn(0)});
    std::stop_source src;
    auto r = session.submitToolResults({}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.not_started");
}

void testBeginTwice()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("c", "workspace_stat_path")}));
    script.push_back(stopTurn(1));
    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    auto first = session.beginSession(makeRequest(), src.get_token());
    assert(first.success());

    auto second = session.beginSession(makeRequest(), src.get_token());
    assert(!second.success());
    assert(second.error()->code == "agent_session.invalid_state");
}

void testSubmitAfterTerminal()
{
    ScriptedProviderAgentSession session({stopTurn(0, "immediate stop")});
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    assert(t0.value().finishReason == ProviderAgentFinishReason::Stop);

    auto r = session.submitToolResults({}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.already_terminal");
}

// ---------------------------------------------------------------------------
// Group 4: Strict result-correlation enforcement
// ---------------------------------------------------------------------------

void testMissingToolResult()
{
    std::vector<ProviderAgentTurnResponse> script;
    // Two outstanding calls.
    script.push_back(toolUseTurn(0, {
        makeCall("c1", "workspace_read_file"),
        makeCall("c2", "workspace_stat_path")}));
    script.push_back(stopTurn(1));
    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    { [[maybe_unused]] auto s = session.beginSession(makeRequest(), src.get_token()); assert(s.success()); }

    // Only submit one result — c2 is missing.
    auto r = session.submitToolResults(
        {makeResult("c1", "workspace_read_file")}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.missing_tool_result");
}

void testUnknownToolResult()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("known", "workspace_read_file")}));
    script.push_back(stopTurn(1));
    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    { [[maybe_unused]] auto s = session.beginSession(makeRequest(), src.get_token()); assert(s.success()); }

    auto r = session.submitToolResults(
        {makeResult("unknown-id", "workspace_read_file")}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.unknown_tool_result");
}

void testDuplicateToolResult()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("c1", "workspace_read_file")}));
    script.push_back(stopTurn(1));
    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    { [[maybe_unused]] auto s = session.beginSession(makeRequest(), src.get_token()); assert(s.success()); }

    // Same callId submitted twice.
    std::vector<app::AgentToolResult> results;
    results.push_back(makeResult("c1", "workspace_read_file"));
    results.push_back(makeResult("c1", "workspace_read_file"));
    auto r = session.submitToolResults(std::move(results), src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.duplicate_tool_result");
}

void testToolNameMismatch()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("c1", "workspace_read_file")}));
    script.push_back(stopTurn(1));
    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    { [[maybe_unused]] auto s = session.beginSession(makeRequest(), src.get_token()); assert(s.success()); }

    // Correct callId but wrong toolName.
    auto r = session.submitToolResults(
        {makeResult("c1", "workspace_list_directory")}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.tool_name_mismatch");
}

void testEmptyResultsWithOutstandingCalls()
{
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("c1", "workspace_read_file")}));
    script.push_back(stopTurn(1));
    ScriptedProviderAgentSession session(std::move(script));
    std::stop_source src;

    { [[maybe_unused]] auto s = session.beginSession(makeRequest(), src.get_token()); assert(s.success()); }

    auto r = session.submitToolResults({}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.empty_tool_results");
}

// ---------------------------------------------------------------------------
// Group 5: validateTurnResponse — standalone inline validator
// ---------------------------------------------------------------------------

void testValidateTurnResponse_ToolUseWithoutCalls()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    resp.turnIndex    = 0;
    // toolCalls deliberately left empty.

    auto r = validateTurnResponse(resp, 0, 8);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.tool_use_without_calls");
}

void testValidateTurnResponse_StopWithCalls()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::Stop;
    resp.turnIndex    = 0;
    resp.toolCalls.push_back(makeCall("c1", "workspace_read_file"));

    auto r = validateTurnResponse(resp, 0, 8);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.calls_after_stop");
}

void testValidateTurnResponse_ErrorWithoutCode()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::Error;
    resp.turnIndex    = 0;
    // errorCode deliberately left empty.

    auto r = validateTurnResponse(resp, 0, 8);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.error_without_code");
}

void testValidateTurnResponse_StopWithErrorFields()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::Stop;
    resp.turnIndex = 0;
    resp.errorCode = "provider.unexpected";
    resp.errorMessage = "must not accompany Stop";

    auto r = validateTurnResponse(resp, 0, 8);
    assert(!r.success());
    assert(r.error()->code
        == "agent_session.response.error_fields_without_error");
}

void testValidateTurnResponse_DuplicateCallIds()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    resp.turnIndex    = 0;
    resp.toolCalls.push_back(makeCall("dup", "workspace_read_file",  0));
    resp.toolCalls.push_back(makeCall("dup", "workspace_stat_path",  1));

    auto r = validateTurnResponse(resp, 0, 8);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.duplicate_call_id");
}

void testValidateTurnResponse_PerTurnLimitExceeded()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    resp.turnIndex    = 0;
    for (int i = 0; i < 3; ++i)
        resp.toolCalls.push_back(makeCall("c" + std::to_string(i), "workspace_stat_path"));

    // maxToolCallsPerTurn = 2 → 3 calls is over the limit.
    auto r = validateTurnResponse(resp, 0, /*maxToolCallsPerTurn=*/2);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.per_turn_limit_exceeded");
}

void testValidateTurnResponse_EmptyCallId()
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    resp.turnIndex    = 0;
    app::AgentToolCall c;
    c.callId   = "";   // empty
    c.toolName = "workspace_read_file";
    resp.toolCalls.push_back(std::move(c));

    auto r = validateTurnResponse(resp, 0, 8);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.empty_call_id");
}

void testValidateTurnResponse_WrongTurnIndex()
{
    ProviderAgentTurnResponse resp = stopTurn(5); // first turn should be 0

    auto r = validateTurnResponse(resp, /*expectedTurnIndex=*/0, 8);
    assert(!r.success());
    assert(r.error()->code == "agent_session.response.unexpected_turn_index");
}

void testValidateTurnResponse_HappyPath()
{
    // Well-formed ToolUse response at turn 0 with distinct calls should pass.
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    resp.turnIndex    = 0;
    resp.toolCalls.push_back(makeCall("a", "workspace_read_file",  0));
    resp.toolCalls.push_back(makeCall("b", "workspace_stat_path",  1));

    auto r = validateTurnResponse(resp, 0, 8);
    assert(r.success());
}

// ---------------------------------------------------------------------------
// Group 6: validateSessionRequest — standalone inline validator
// ---------------------------------------------------------------------------

void testValidateSessionRequest_EmptyProviderId()
{
    auto req = makeRequest();
    req.providerId = "";
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.empty_provider_id");
}

void testValidateSessionRequest_ZeroMaxTurns()
{
    auto req    = makeRequest();
    req.maxTurns = 0;
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.invalid_max_turns");
}

void testValidateSessionRequest_EmptyModelId()
{
    auto req = makeRequest();
    req.modelId = "";
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.empty_model_id");
}

void testValidateSessionRequest_EmptyApiModelId()
{
    auto req = makeRequest();
    req.apiModelId = "";
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.empty_api_model_id");
}

void testValidateSessionRequest_ZeroMaxTotalToolCalls()
{
    auto req = makeRequest();
    req.maxTotalToolCalls = 0;
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code
        == "agent_session.request.invalid_max_total_calls");
}

void testValidateSessionRequest_ZeroMaxToolCallsPerTurn()
{
    auto req = makeRequest();
    req.maxToolCallsPerTurn = 0;
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code
        == "agent_session.request.invalid_max_calls_per_turn");
}

void testValidateSessionRequest_ZeroMaxOutputTokens()
{
    auto req = makeRequest();
    req.maxOutputTokens = 0;
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code
        == "agent_session.request.invalid_max_output_tokens");
}

void testValidateSessionRequest_PerTurnExceedsTotal()
{
    auto req = makeRequest();
    req.maxTotalToolCalls   = 4;
    req.maxToolCallsPerTurn = 8;   // > total
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.per_turn_exceeds_total");
}

void testValidateSessionRequest_WriteTool()
{
    auto req = makeRequest();
    app::AgentToolDefinition writeTool;
    writeTool.name     = "workspace_write_file";
    writeTool.readOnly = false;   // not allowed
    req.tools.push_back(writeTool);
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.write_tool");
}

void testValidateSessionRequest_DuplicateToolNames()
{
    auto req = makeRequest();
    app::AgentToolDefinition t;
    t.name     = "workspace_read_file";
    t.readOnly = true;
    req.tools.push_back(t);
    req.tools.push_back(t); // duplicate
    auto r = validateSessionRequest(req);
    assert(!r.success());
    assert(r.error()->code == "agent_session.request.duplicate_tool_name");
}

void testValidateSessionRequest_Valid()
{
    auto req = makeRequest();
    app::AgentToolDefinition def;
    def.name     = "workspace_read_file";
    def.readOnly = true;
    req.tools.push_back(def);
    auto r = validateSessionRequest(req);
    assert(r.success());
}

// ---------------------------------------------------------------------------
// Group 7: Lifecycle — stop_token and exhausted script
// ---------------------------------------------------------------------------

void testStopTokenCancellation()
{
    // Session begins normally; then we cancel before the second turn.
    std::vector<ProviderAgentTurnResponse> script;
    script.push_back(toolUseTurn(0, {makeCall("c1", "workspace_read_file")}));
    script.push_back(stopTurn(1, "never reached"));
    ScriptedProviderAgentSession session(std::move(script));

    std::stop_source src;
    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());

    // Cancel before submitToolResults.
    src.request_stop();
    auto r = session.submitToolResults(
        {makeResult("c1", "workspace_read_file")}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.stop_requested");

    // Session is now terminal.
    auto r2 = session.submitToolResults({}, src.get_token());
    assert(!r2.success());
    assert(r2.error()->code == "agent_session.already_terminal");
}

void testExhaustedScript()
{
    // Script has only one Stop turn.
    ScriptedProviderAgentSession session({stopTurn(0, "done")});
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    assert(t0.value().finishReason == ProviderAgentFinishReason::Stop);

    // Try to re-enter after terminal — must get already_terminal.
    auto r = session.submitToolResults({}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.already_terminal");
}

void testScriptedSessionExhaustsAfterToolUse()
{
    // Script ends mid-way: one ToolUse turn but no follow-up.
    ScriptedProviderAgentSession session(
        {toolUseTurn(0, {makeCall("c1", "workspace_read_file")})});
    std::stop_source src;

    auto t0 = session.beginSession(makeRequest(), src.get_token());
    assert(t0.success());
    assert(t0.value().finishReason == ProviderAgentFinishReason::ToolUse);

    // Submit the correct result — but no scripted turn follows.
    auto r = session.submitToolResults(
        {makeResult("c1", "workspace_read_file")}, src.get_token());
    assert(!r.success());
    assert(r.error()->code == "agent_session.script_exhausted");
}

// ---------------------------------------------------------------------------
// Group 8: Legacy IProviderClient::send() is unchanged and independent
// ---------------------------------------------------------------------------

class FakeLegacyProviderClient final : public ai::application::IProviderClient {
public:
    common::Result<ai::application::ProviderChatResponse> send(
        const ai::application::ProviderChatRequest& request,
        std::stop_token /*stopToken*/) override
    {
        ai::application::ProviderChatResponse resp;
        resp.content = "legacy-reply-to:" + request.userContent;
        return common::Result<ai::application::ProviderChatResponse>::ok(std::move(resp));
    }
};

void testLegacyIProviderClientContractUnchanged()
{
    FakeLegacyProviderClient client;
    std::stop_source src;

    ai::application::ProviderChatRequest req;
    req.providerId   = "p1";
    req.modelId      = "m1";
    req.apiModelId   = "claude-sonnet-5";
    req.systemPrompt = "sys";
    req.userContent  = "hello";

    auto r = client.send(req, src.get_token());
    assert(r.success());
    assert(r.value().content == "legacy-reply-to:hello");

    // IProviderClient must remain a completely separate hierarchy.
    static_assert(
        !std::is_base_of_v<IProviderAgentSession, FakeLegacyProviderClient>,
        "FakeLegacyProviderClient must not be an IProviderAgentSession");
}

} // namespace

int runProviderAgentSessionContractTests()
{
    // Group 1: fail-closed defaults
    testDefaultAgentToolResultIsFailClosed();
    testTypeDefaults();

    // Group 2: happy paths
    testSingleToolCallAndFinalAnswer();
    testMultipleToolCallsInOneTurn();
    testTwoSequentialToolTurns();
    testCallIdPreserved();
    testToolErrorReturnedAsStructuredResult();

    // Group 3: illegal transitions
    testSubmitBeforeBegin();
    testBeginTwice();
    testSubmitAfterTerminal();

    // Group 4: strict result-correlation
    testMissingToolResult();
    testUnknownToolResult();
    testDuplicateToolResult();
    testToolNameMismatch();
    testEmptyResultsWithOutstandingCalls();

    // Group 5: validateTurnResponse
    testValidateTurnResponse_ToolUseWithoutCalls();
    testValidateTurnResponse_StopWithCalls();
    testValidateTurnResponse_ErrorWithoutCode();
    testValidateTurnResponse_StopWithErrorFields();
    testValidateTurnResponse_DuplicateCallIds();
    testValidateTurnResponse_PerTurnLimitExceeded();
    testValidateTurnResponse_EmptyCallId();
    testValidateTurnResponse_WrongTurnIndex();
    testValidateTurnResponse_HappyPath();

    // Group 6: validateSessionRequest
    testValidateSessionRequest_EmptyProviderId();
    testValidateSessionRequest_EmptyModelId();
    testValidateSessionRequest_EmptyApiModelId();
    testValidateSessionRequest_ZeroMaxTurns();
    testValidateSessionRequest_ZeroMaxTotalToolCalls();
    testValidateSessionRequest_ZeroMaxToolCallsPerTurn();
    testValidateSessionRequest_ZeroMaxOutputTokens();
    testValidateSessionRequest_PerTurnExceedsTotal();
    testValidateSessionRequest_WriteTool();
    testValidateSessionRequest_DuplicateToolNames();
    testValidateSessionRequest_Valid();

    // Group 7: lifecycle
    testStopTokenCancellation();
    testExhaustedScript();
    testScriptedSessionExhaustsAfterToolUse();

    // Group 8: legacy contract
    testLegacyIProviderClientContractUnchanged();

    return 0;
}
