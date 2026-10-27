#pragma once

// Provider-neutral multi-turn agent session protocol types.
//
// These types form the session boundary between the Application Layer's
// AgentToolExecutionService (planned) and concrete OpenAI/Anthropic session
// adapters (planned). They deliberately mirror neither provider's native JSON
// structure so adapters can be swapped without changing the coordinator.
//
// Canonical tool types: AgentToolDefinition, AgentToolCall, and AgentToolResult
// from ai::application are used directly as the session boundary. No parallel
// provider-specific structs are defined here; adapters map to/from those
// Application types, not from a second independent set.
//
// JSON policy: all JSON-encoded fields (inputSchemaJson, argumentsJson,
// contentJson) are opaque UTF-8 std::string values. The interface has no
// dependency on a JSON library.
//
// Dependency policy: no Qt, no CURL, no OpenAI-specific or Anthropic-specific
// structures. Depends on AgentToolTypes (application layer), the provider
// catalog identifiers, and TokenUsage.

#include "ai/application/agents/AgentToolTypes.hpp"
#include "ai/application/catalog/ProviderCatalogTypes.hpp"
#include "ai/models/TokenUsage.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::providers {

// ---------------------------------------------------------------------------
// Finish reasons — provider-neutral session state classification
// ---------------------------------------------------------------------------

// Adapters map their native stop/finish signals onto these values.
// Callers branch on finishReason to decide whether to submit tool results
// or conclude the session.
//
// Semantic split — two distinct failure paths:
//
//   common::Result::failure  — the TURN was not received at all.
//     Transport errors, timeouts, malformed responses, authentication failures,
//     and illegal session-state transitions all produce Result::failure.
//     There is no ProviderAgentTurnResponse to inspect.
//
//   ProviderAgentFinishReason::Error — a valid provider response was received,
//     but the provider signalled a terminal error inside the response.
//     The ProviderAgentTurnResponse is populated and errorCode must be set.
//
// These two paths must never be conflated: do not return a FinishReason::Error
// response when the transport failed, and do not produce Result::failure when
// the provider returned a properly formed error turn.
enum class ProviderAgentFinishReason {
    // The model produced a final answer with no outstanding tool calls.
    Stop,

    // The model requested one or more tool calls; submitToolResults() must
    // be called to continue.
    ToolUse,

    // The session reached maxTurns and was halted before a natural stop.
    MaxTurns,

    // The provider truncated one response at its per-request output-token
    // limit. This is not a session turn-count limit.
    MaxOutputTokens,

    // The provider returned a valid response containing a terminal error.
    // ProviderAgentTurnResponse::errorCode must be non-empty in this case.
    Error,

    // Cooperative cancellation via std::stop_token was honoured.
    StopRequested,
};

// ---------------------------------------------------------------------------
// Per-turn response
// ---------------------------------------------------------------------------

struct ProviderAgentTurnResponse {
    // Accumulated assistant text from this turn. May be empty when the only
    // output is tool calls (provider behaviour varies).
    std::string assistantText;

    // Populated exactly when finishReason == ToolUse.
    // Uses the canonical Application type so the coordinator can pass these
    // directly to IWorkspaceToolExecutor and IAgentToolAuditRepository.
    std::vector<application::AgentToolCall> toolCalls;

    ProviderAgentFinishReason finishReason {ProviderAgentFinishReason::Stop};

    // Zero-based turn counter.
    //   0 = reply to beginSession()
    //   n = reply to nth submitToolResults()
    // Must increase strictly by one between consecutive turns.
    std::size_t turnIndex {0};

    // Token usage for this turn. Zero is valid when the provider omits usage.
    models::TokenUsage usage;

    // Stable provider-neutral error code and message.
    // Must be non-empty when finishReason == Error.
    // MUST NOT contain API keys, secrets, user prompts, or argument/result JSON.
    std::string errorCode;
    std::string errorMessage;
};

// ---------------------------------------------------------------------------
// Tool policy — an optional, provider-neutral restriction applied to a single
// turn: the upcoming turn of submitToolResults(), or (via
// ProviderAgentSessionRequest::firstTurnToolPolicy) the very first turn of
// beginSession()
// ---------------------------------------------------------------------------

// Restricts what the provider may do on the one turn it is applied to;
// every later turn reverts to the session's normal (unrestricted) behavior
// unless another policy is supplied again. Both adapters implement this
// identically at the wire level:
//   - allowedToolNames: OpenAI/Anthropic both accept a fresh "tools" array
//     on every request, so restricting is just sending a filtered subset of
//     the tool definitions originally passed to beginSession(). Every name
//     here must already be one of those tool names. If the subset contains
//     exactly one name, both adapters additionally use their provider's
//     named-tool `tool_choice` form; a multi-name subset uses required/any
//     choice within that filtered catalog.
//   - disallowParallelToolCalls: OpenAI Responses API's request-level
//     "parallel_tool_calls": false; Anthropic Messages API's
//     "tool_choice": {..., "disable_parallel_tool_use": true}. Both really
//     mean "at most one tool call in this turn's response" — combined with
//     this session contract's existing "some tool call is required" rule,
//     the result is exactly one call.
// A malformed policy (allowedToolNames names a tool never advertised to
// beginSession()) is a caller programming error, not a provider condition;
// adapters reject it with a typed error before sending any request.
struct NextTurnToolPolicy {
    // Empty means "no restriction — advertise the full original tool set".
    std::vector<std::string> allowedToolNames;
    bool disallowParallelToolCalls {false};
};

// ---------------------------------------------------------------------------
// Session request — sent once at session start via beginSession()
// ---------------------------------------------------------------------------

struct ProviderAgentSessionRequest {
    // Catalog identifiers: resolved from the execution profile by the caller.
    // Adapters use these for diagnostics; the actual provider routing key is
    // apiModelId.
    application::ProviderId providerId;
    application::ModelId    modelId;

    // The provider's own model string (Model::apiModelId). Passed explicitly so
    // the adapter never needs a catalog lookup.
    std::string apiModelId;

    std::string systemPrompt;
    std::string userContent;

    // Tool definitions advertised to the model. All definitions must have
    // readOnly == true; write tools are not permitted in this session contract.
    // Tool names must be unique within the vector.
    std::vector<application::AgentToolDefinition> tools;

    // Per-turn HTTP timeout. Does not bound total session wall time.
    std::chrono::milliseconds turnTimeout {60'000};
    std::uint32_t maxOutputTokens {4096u};
    double temperature {0.2};

    // Hard limit on provider turns (beginSession counts as turn 0).
    // Must be > 0.
    std::size_t maxTurns {16};

    // Hard limit on total tool calls across all turns.
    // Must be > 0.
    std::size_t maxTotalToolCalls {64};

    // Hard limit on tool calls within a single provider turn.
    // Must be > 0 and <= maxTotalToolCalls.
    std::size_t maxToolCallsPerTurn {8};

    // Real-Run incident (Run a739e790-db80-4efd-a98f-5d91af2e5e86,
    // 2026-09-14): a worker whose real job touches many files (reading
    // several newly-created plus several pre-existing files before editing)
    // can legitimately have the provider batch more parallel tool calls into
    // one turn than maxToolCallsPerTurn allows - validateTurnResponse then
    // hard-fails the WHOLE session on turn 1, before a single successful
    // inspection, with no graceful degradation. Session-wide (unlike
    // NextTurnToolPolicy::disallowParallelToolCalls, which only restricts
    // one upcoming turn from submitToolResults()): when true, every turn of
    // this session - including the very first, via beginSession(), which
    // NextTurnToolPolicy cannot reach at all - asks the provider for at most
    // one tool call per turn, which can never exceed any positive
    // maxToolCallsPerTurn. A caller that retries a task after this exact
    // failure sets this true on the retry attempt so the same wall can never
    // be hit again, regardless of how many files a future task's real work
    // requires - trading turn-count economy for a structural guarantee on
    // the bounded retry, never imposed on every attempt by default.
    bool disallowParallelToolCalls {false};

    // Optional tool policy for the very first turn only - the one turn a
    // NextTurnToolPolicy passed to submitToolResults() can never reach. Used
    // by the Manifest worker loop when the FIRST provider turn of a session
    // is already the last turn the task may spend (its reserved allowance has
    // no room for a second one), so that turn must be terminal. Every name
    // must be one of `tools`; a malformed policy is rejected by the adapter
    // with agent_session.invalid_first_turn_policy before any request is
    // sent. std::nullopt (the default) leaves the first turn unrestricted.
    std::optional<NextTurnToolPolicy> firstTurnToolPolicy;
};

} // namespace ai::providers
