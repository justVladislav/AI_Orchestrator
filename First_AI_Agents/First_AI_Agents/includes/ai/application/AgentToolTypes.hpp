#pragma once

#include <cstddef>
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
};

} // namespace ai::application
