#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// Provider-neutral description exposed to a model. JSON schemas remain
// canonical strings so this public boundary does not depend on a JSON library.
struct AgentToolDefinition {
    std::string name;
    std::string version {"1"};
    std::string description;
    std::string inputSchemaJson;
    bool readOnly {true};
    std::size_t maxResultBytes {256ULL * 1024};

    // When true, the adapter must request provider-enforced schema
    // conformance (OpenAI Responses API top-level "strict": true / Anthropic
    // Messages API top-level "strict": true) instead of best-effort
    // generation, and must never silently fall back to non-strict on a
    // malformed or unsupported schema. Defaults to false so every existing
    // tool definition keeps today's best-effort behavior unchanged.
    bool strictInputSchema {false};
};

// A tool call after provider-specific response parsing. Arguments are not
// logged by generic infrastructure because they may contain sensitive search
// text even though the operation itself is read-only.
struct AgentToolCall {
    std::string callId;
    std::string toolName;
    std::string argumentsJson;
    std::size_t turnIndex {0};
    std::size_t ordinal {0};
};

struct AgentToolResult {
    std::string callId;
    std::string toolName;
    bool success {false};
    std::string errorCode;
    std::string contentJson;
    bool truncated {false};
    std::size_t returnedBytes {0};
};

struct AgentToolExecutionLimits {
    std::size_t maxTurns {16};
    std::size_t maxCalls {64};
    std::size_t maxCallsPerTurn {8};
    std::size_t maxSingleResultBytes {256ULL * 1024};
    std::size_t maxTotalResultBytes {2ULL * 1024 * 1024};
    std::size_t maxArgumentsBytes {64ULL * 1024};
};

enum class AgentToolCallState {
    Requested,
    Running,
    Completed,
    Failed,
    Interrupted
};

// Durable audit metadata deliberately stores hashes and optional artifact
// references instead of raw arguments/results. Sensitive payloads must not
// enter ordinary logs or SQLite text columns.
struct AgentToolAuditRecord {
    std::string auditId;
    std::string sessionId;
    std::string runId;
    std::string providerCallId;
    std::string toolName;
    AgentToolCallState state {AgentToolCallState::Requested};
    std::string argumentsSha256;
    std::string resultSha256;
    std::optional<std::string> resultArtifactId;
    std::string errorCode;
    std::size_t returnedBytes {0};
    bool truncated {false};
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
};

// ── Audit session and turn types ─────────────────────────────────────────────

enum class AgentToolSessionState {
    Active,       // at least one tool call is in progress or may start
    Completed,    // all calls finished successfully
    Interrupted   // crash-recovery or explicit abort
};

// One row per agent tool session: a model calling one or more tools within
// a single Run. Sessions are the outermost audit scope; individual tool calls
// are tracked via AgentToolAuditRecord rows linked by sessionId.
// raw prompts, raw arguments, raw results, and workspace paths are never stored.
struct AgentToolSession {
    std::string sessionId;
    std::string runId;
    std::string providerId;
    std::string modelId;
    AgentToolSessionState state {AgentToolSessionState::Active};
    std::size_t totalTurns {0};
    std::size_t totalCalls {0};
    std::size_t totalReturnedBytes {0};
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
};

// Bounded, provider-neutral facts about ONE provider response, kept so that a cut-off response
// (finishReason max_output_tokens) can be diagnosed afterwards instead of guessed at. Every
// optional distinguishes "the provider said 0" from "the provider did not say": nullopt always
// means "not reported / not recorded", never zero.
//
// Never holds prompts, tool arguments, response text, secrets or the raw HTTP body. The two text
// fields are short tokens (see kMaxDiagnosticTokenBytes) and are sanitised by the adapter that
// fills them. None of these fields is a billing figure: reasoningTokens is a SUBSET of the output
// tokens already counted in TokenUsage and must never be added to a cost or a usage total.
struct ProviderTurnDiagnostics {
    // The output-token limit this adapter put on the wire for the turn.
    std::optional<std::uint64_t> requestedMaxOutputTokens;
    // The output-token limit the provider's own response reported back (OpenAI Responses echoes
    // max_output_tokens). nullopt: the response carried no such field.
    std::optional<std::uint64_t> echoedMaxOutputTokens;
    // usage.output_tokens_details.reasoning_tokens, when reported.
    std::optional<std::uint64_t> reasoningTokens;
    // The provider's own status word for the response ("completed", "incomplete", ...).
    std::string providerStatus;
    // incomplete_details.reason (or the provider's stop reason) when the response is incomplete.
    std::string incompleteReason;

    [[nodiscard]] bool empty() const noexcept
    {
        return !requestedMaxOutputTokens.has_value() && !echoedMaxOutputTokens.has_value()
            && !reasoningTokens.has_value() && providerStatus.empty() && incompleteReason.empty();
    }
};

inline constexpr std::size_t kMaxDiagnosticTokenBytes = 64;

// The provider words a diagnostic may carry. This is an ALLOW-LIST, not a character filter: provider-supplied
// text of any other shape (a free-text reason, something that looks like a key, a future status) is never
// stored or shown - it becomes kUnrecognizedDiagnosticWord. A character filter would keep a secret that merely
// consists of permitted characters.
inline constexpr const char* kUnrecognizedDiagnosticWord = "unrecognized";

[[nodiscard]] inline bool isKnownDiagnosticWord(const std::string& text) noexcept
{
    static constexpr const char* kKnown[] = {
        // OpenAI Responses status / incomplete_details.reason
        "completed", "incomplete", "failed", "in_progress", "queued", "cancelled", "canceled",
        "max_output_tokens", "content_filter",
        // Anthropic Messages stop_reason
        "end_turn", "max_tokens", "stop_sequence", "tool_use", "pause_turn", "refusal",
        "model_context_window_exceeded"};
    for (const char* word : kKnown)
        if (text == word) return true;
    return false;
}

// True for an absent value (empty), a known provider word, or the unrecognized marker - and nothing else.
[[nodiscard]] inline bool isBoundedDiagnosticToken(const std::string& text) noexcept
{
    return text.empty() || text == kUnrecognizedDiagnosticWord || isKnownDiagnosticWord(text);
}

// Maps provider text to what may be stored: empty stays empty, a known word is kept as is, everything else
// becomes the unrecognized marker (the original text is dropped, not truncated).
[[nodiscard]] inline std::string toBoundedDiagnosticToken(const std::string& text)
{
    if (text.empty()) return {};
    return isKnownDiagnosticWord(text) ? text : std::string(kUnrecognizedDiagnosticWord);
}

// One row per model-response turn inside a session. A turn may contain
// zero or more tool call requests. Token counts are informational only.
// Raw provider response text is never stored here.
struct AgentProviderTurn {
    std::string turnId;
    std::string sessionId;
    std::size_t turnIndex {0};
    std::string finishReason;   // "tool_use", "end_turn", "error", etc.
    int inputTokens {0};
    int outputTokens {0};
    std::size_t callCount {0};
    std::chrono::system_clock::time_point createdAt;
    // Bounded diagnostics of this response (see ProviderTurnDiagnostics). A row written before
    // schema step 72 has none of them, which reads back as an empty value, never as zeros.
    ProviderTurnDiagnostics diagnostics;
};

// The one Run-diagnostics sentence for a response cut at its output-token limit. Pure and
// bounded: it names what was asked for, what the provider reported back (or that it reported
// nothing), what was produced and, when reported, how much of that was reasoning.
[[nodiscard]] inline std::string describeOutputTokenCut(
    std::uint64_t producedOutputTokens, const ProviderTurnDiagnostics& diagnostics)
{
    const auto number = [](const std::optional<std::uint64_t>& value, const char* absent) {
        return value.has_value() ? std::to_string(*value) : std::string(absent);
    };
    std::string text = "requested limit " + number(diagnostics.requestedMaxOutputTokens, "unknown")
        + ", provider reported limit " + number(diagnostics.echoedMaxOutputTokens, "none")
        + ", produced " + std::to_string(producedOutputTokens);
    if (diagnostics.reasoningTokens.has_value())
        text += " (reasoning " + std::to_string(*diagnostics.reasoningTokens) + ")";
    text += " in that turn";
    if (!diagnostics.incompleteReason.empty()) text += ", incomplete reason " + diagnostics.incompleteReason;
    if (!diagnostics.providerStatus.empty()) text += ", provider status " + diagnostics.providerStatus;
    return text;
}

} // namespace ai::application
