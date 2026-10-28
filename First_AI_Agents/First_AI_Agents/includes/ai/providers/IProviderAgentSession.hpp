#pragma once

// Provider-neutral multi-turn agent session interface.
//
// State machine (per session object; one beginSession() per lifetime):
//
//   Idle ──beginSession()──► WaitingForToolResults  (finishReason == ToolUse)
//        └──────────────────► Terminal              (Stop | MaxTurns | Error |
//                                                    StopRequested)
//
//   WaitingForToolResults ──submitToolResults()──► WaitingForToolResults
//                          └──────────────────────► Terminal
//
//   Terminal ──any call──► Result::failure "agent_session.already_terminal"
//   Idle ──submitToolResults()──► Result::failure "agent_session.not_started"
//   WaitingForToolResults ──beginSession()──► Result::failure "agent_session.invalid_state"
//
// Failure semantics:
//   Result::failure  — the turn was not received (transport, auth, timeout,
//                      malformed response, or illegal state). No turn response.
//   FinishReason::Error — a valid provider response arrived but the provider
//                          signalled a terminal error inside it. errorCode is set.
//
// These two failure paths are mutually exclusive and must not be conflated.
//
// This interface does not replace IProviderClient::send(). Ordinary one-shot
// chat and Run workflows continue to use IProviderClient unchanged.

#include "ai/application/agents/AgentToolTypes.hpp"
#include "ai/common/Result.hpp"
#include "ai/providers/ProviderAgentSessionTypes.hpp"

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ai::providers {

// ---------------------------------------------------------------------------
// Session validation helpers (inline — no .cpp dependency)
// ---------------------------------------------------------------------------

// Returns an error if the session request violates any invariant that must
// hold before beginSession() is called.
//
// Checked invariants:
//   - providerId, modelId, apiModelId non-empty
//   - maxTurns, maxTotalToolCalls, maxToolCallsPerTurn > 0
//   - maxToolCallsPerTurn <= maxTotalToolCalls
//   - maxOutputTokens > 0
//   - all tools have readOnly == true
//   - tool names are unique
//
// Error messages do not contain user prompts, tool arguments, or JSON.
[[nodiscard]] inline common::Result<void> validateSessionRequest(
    const ProviderAgentSessionRequest& req)
{
    if (req.providerId.empty())
        return common::Result<void>::failure(
            {"agent_session.request.empty_provider_id",
             "providerId must not be empty"});
    if (req.modelId.empty())
        return common::Result<void>::failure(
            {"agent_session.request.empty_model_id",
             "modelId must not be empty"});
    if (req.apiModelId.empty())
        return common::Result<void>::failure(
            {"agent_session.request.empty_api_model_id",
             "apiModelId must not be empty"});
    if (req.maxTurns == 0)
        return common::Result<void>::failure(
            {"agent_session.request.invalid_max_turns",
             "maxTurns must be greater than zero"});
    if (req.maxTotalToolCalls == 0)
        return common::Result<void>::failure(
            {"agent_session.request.invalid_max_total_calls",
             "maxTotalToolCalls must be greater than zero"});
    if (req.maxToolCallsPerTurn == 0)
        return common::Result<void>::failure(
            {"agent_session.request.invalid_max_calls_per_turn",
             "maxToolCallsPerTurn must be greater than zero"});
    if (req.maxToolCallsPerTurn > req.maxTotalToolCalls)
        return common::Result<void>::failure(
            {"agent_session.request.per_turn_exceeds_total",
             "maxToolCallsPerTurn must not exceed maxTotalToolCalls"});
    if (req.maxOutputTokens == 0)
        return common::Result<void>::failure(
            {"agent_session.request.invalid_max_output_tokens",
             "maxOutputTokens must be greater than zero"});
    for (const auto& def : req.tools) {
        if (!def.readOnly)
            return common::Result<void>::failure(
                {"agent_session.request.write_tool",
                 "all tools must have readOnly == true in this session contract"});
    }
    for (std::size_t i = 0; i < req.tools.size(); ++i) {
        for (std::size_t j = i + 1; j < req.tools.size(); ++j) {
            if (req.tools[i].name == req.tools[j].name)
                return common::Result<void>::failure(
                    {"agent_session.request.duplicate_tool_name",
                     "tool names must be unique within a session"});
        }
    }
    return common::Result<void>::ok();
}

// Returns an error if a turn response is internally inconsistent.
// Adapters must call this before returning any turn to the coordinator.
//
// Checked invariants:
//   - turnIndex == expectedTurnIndex (must increase strictly by one)
//   - ToolUse response has at least one tool call
//   - non-ToolUse response has no tool calls
//   - Error response has a non-empty errorCode
//   - non-Error response has no errorCode or errorMessage
//   - tool call count does not exceed maxToolCallsPerTurn
//   - no tool call has an empty callId
//   - callIds within the same turn are unique
//
// Error messages do not contain provider-side content or JSON.
[[nodiscard]] inline common::Result<void> validateTurnResponse(
    const ProviderAgentTurnResponse& resp,
    std::size_t expectedTurnIndex,
    std::size_t maxToolCallsPerTurn)
{
    if (resp.turnIndex != expectedTurnIndex)
        return common::Result<void>::failure(
            {"agent_session.response.unexpected_turn_index",
             "turnIndex must increase strictly by one each turn"});
    if (resp.finishReason == ProviderAgentFinishReason::ToolUse
            && resp.toolCalls.empty())
        return common::Result<void>::failure(
            {"agent_session.response.tool_use_without_calls",
             "ToolUse response must contain at least one tool call"});
    if (resp.finishReason != ProviderAgentFinishReason::ToolUse
            && !resp.toolCalls.empty())
        return common::Result<void>::failure(
            {"agent_session.response.calls_after_stop",
             "non-ToolUse response must not contain tool calls"});
    if (resp.finishReason == ProviderAgentFinishReason::Error
            && resp.errorCode.empty())
        return common::Result<void>::failure(
            {"agent_session.response.error_without_code",
             "Error response must include a non-empty errorCode"});
    if (resp.finishReason != ProviderAgentFinishReason::Error
            && (!resp.errorCode.empty() || !resp.errorMessage.empty()))
        return common::Result<void>::failure(
            {"agent_session.response.error_fields_without_error",
             "errorCode and errorMessage are only valid for an Error response"});
    if (resp.toolCalls.size() > maxToolCallsPerTurn)
        return common::Result<void>::failure(
            {"agent_session.response.per_turn_limit_exceeded",
             "tool call count exceeds maxToolCallsPerTurn"});
    for (std::size_t i = 0; i < resp.toolCalls.size(); ++i) {
        if (resp.toolCalls[i].callId.empty())
            return common::Result<void>::failure(
                {"agent_session.response.empty_call_id",
                 "every tool call must have a non-empty callId"});
        for (std::size_t j = i + 1; j < resp.toolCalls.size(); ++j) {
            if (resp.toolCalls[i].callId == resp.toolCalls[j].callId)
                return common::Result<void>::failure(
                    {"agent_session.response.duplicate_call_id",
                     "callIds within a single turn must be unique"});
        }
    }
    return common::Result<void>::ok();
}

// ---------------------------------------------------------------------------
// IProviderAgentSession
// ---------------------------------------------------------------------------

// One live multi-turn session with a single provider endpoint.
// Implementations own transport, authentication, and the accumulated
// conversation history needed to build the next provider request.
// Callers never see raw HTTP or provider-specific JSON.
class IProviderAgentSession {
public:
    virtual ~IProviderAgentSession() = default;

    // Starts a new agent session and returns the provider's first turn.
    // MUST be called exactly once per session object.
    //
    // Result::failure codes:
    //   "agent_session.invalid_state"    — called after session already started
    //   "provider.auth_failed"           — credential rejected
    //   "provider.rate_limited"          — provider rate limit hit
    //   "provider.timeout"               — turn timed out
    //   "provider.network_error"         — transport failure
    //   "provider.invalid_response"      — malformed provider response
    //   "provider.request_failed"        — provider rejected the request
    //
    // Error::message MUST NOT contain the API key or any other secret.
    [[nodiscard]] virtual common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& request,
        std::stop_token stopToken) = 0;

    // Submits tool results and returns the provider's next turn.
    // May only be called when the previous turn returned finishReason == ToolUse.
    //
    // results must contain exactly one AgentToolResult per outstanding
    // AgentToolCall, correlated by callId, with a matching toolName.
    //
    // Result::failure codes:
    //   "agent_session.not_started"         — beginSession() was never called
    //   "agent_session.already_terminal"    — session has already finished
    //   "agent_session.invalid_state"       — not waiting for tool results
    //   "agent_session.empty_tool_results"  — empty submission with outstanding calls
    //   "agent_session.missing_tool_result" — a required call has no result
    //   "agent_session.unknown_tool_result" — result references an unknown callId
    //   "agent_session.duplicate_tool_result" — same callId submitted twice
    //   "agent_session.tool_name_mismatch"  — result.toolName != call.toolName
    //   plus the same transport codes as beginSession()
    //
    // nextTurnPolicy, when supplied, restricts only the upcoming turn (see
    // NextTurnToolPolicy). std::nullopt (the default) preserves this
    // session's normal, unrestricted turn behavior — every existing caller
    // that omits it is unaffected.
    //
    // Additional Result::failure code when nextTurnPolicy is supplied:
    //   "agent_session.invalid_next_turn_policy" — allowedToolNames names a
    //   tool that was never advertised to beginSession()
    [[nodiscard]] virtual common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<application::AgentToolResult> results,
        std::stop_token stopToken,
        std::optional<NextTurnToolPolicy> nextTurnPolicy = std::nullopt) = 0;
};

// ---------------------------------------------------------------------------
// IProviderAgentSessionFactory
// ---------------------------------------------------------------------------

// Factory that creates fresh, Idle session objects. Mirrors IProviderClientFactory:
// credential resolution and transport setup happen here so callers never see secrets.
class IProviderAgentSessionFactory {
public:
    virtual ~IProviderAgentSessionFactory() = default;

    // Returns a new Idle session ready for beginSession().
    //
    // Result::failure codes:
    //   "credential.missing"           — no credential configured or stored
    //   "credential.unavailable"       — credential store is unreachable
    //   "provider.unsupported_kind"    — no adapter for providerKind
    //   "provider_runtime.unavailable" — no adapter linked in this build
    [[nodiscard]] virtual common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const application::ProviderId& providerId,
        const std::string& providerKind) = 0;
};

} // namespace ai::providers
