#pragma once

#include "ai/application/agents/AgentToolTypes.hpp"
#include "ai/application/agents/IAgentToolAuditRepository.hpp"
#include "ai/application/agents/IAgentToolSessionRepository.hpp"
#include "ai/application/workspace/IWorkspaceToolExecutor.hpp"
#include "ai/application/workspace/ToolRegistry.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ai::application {

// Audited, fail-closed agent tool execution service.
//
// Ties together:
//   - ToolRegistry (owned) — the set of tools the model is allowed to call.
//   - IWorkspaceToolExecutor& — filesystem dispatcher, always read-only.
//   - IAgentToolSessionRepository& — session + turn persistence.
//   - IAgentToolAuditRepository& — per-call audit record persistence.
//
// Fail-closed guarantee: every public method checks storageAvailable() first.
// When false, beginSession/recordTurn/executeToolCall all return the typed
// error "agent_tool.storage_unavailable" without touching the filesystem or
// any repository. Callers must check storageAvailable() before starting an
// agent tool session.
//
// Workspace-root rule: the trusted workspace root is always derived from the
// Run's project.rootPath via buildPolicy(). It is never accepted from model
// arguments or UI input.
class AgentToolExecutionService {
public:
    AgentToolExecutionService(
        ToolRegistry registry,
        IWorkspaceToolExecutor& executor,
        IAgentToolSessionRepository& sessions,
        IAgentToolAuditRepository& audits,
        bool storageAvailable);

    // ── Availability ──────────────────────────────────────────────────────────

    // False when either the feature flag is off or the database could not be
    // opened. Tool executors must not start a session when this returns false.
    [[nodiscard]] bool storageAvailable() const noexcept;

    // ── Tool catalog ──────────────────────────────────────────────────────────

    // All read-only tool definitions registered at construction.
    // Pass this vector directly into ProviderAgentSessionRequest::tools.
    [[nodiscard]] const std::vector<AgentToolDefinition>& toolDefinitions() const noexcept;

    // ── Policy derivation ─────────────────────────────────────────────────────

    // Build a secure workspace policy from a trusted project root path.
    // Must be called with Project::rootPath, never with a path from model output.
    [[nodiscard]] static WorkspaceReadPolicy buildPolicy(
        const std::filesystem::path& workspaceRoot);

    // ── Session factory helper ────────────────────────────────────────────────

    // Creates a new AgentToolSession with a generated UUID sessionId and the
    // current wall-clock createdAt. Pass the returned struct to beginSession()
    // before submitting any tool calls.
    [[nodiscard]] static AgentToolSession makeSession(
        const std::string& runId,
        const std::string& providerId,
        const std::string& modelId);

    // ── Session lifecycle ─────────────────────────────────────────────────────

    // Inserts a new Active session row. The session must have been created via
    // makeSession() or constructed with a unique UUID sessionId.
    [[nodiscard]] common::Result<AgentToolSession> beginSession(
        AgentToolSession session);

    // Updates an existing session row (e.g. to Completed/Interrupted with stats).
    [[nodiscard]] common::Result<AgentToolSession> updateSession(
        AgentToolSession session);

    // Inserts a provider turn row for a completed provider interaction.
    [[nodiscard]] common::Result<AgentProviderTurn> recordTurn(
        AgentProviderTurn turn);

    // ── Audited tool execution ────────────────────────────────────────────────

    // Executes one tool call with a complete audit trail:
    //   1. Inserts AgentToolAuditRecord in Requested state.
    //      Fails closed with "agent_tool.audit_failed" if insert fails.
    //   2. Updates to Running before dispatching to IWorkspaceToolExecutor.
    //   3. Calls executor_.execute(call, policy).
    //   4. Updates to Completed (any executor result) or Failed (executor
    //      returned Result::failure) with sha256 hashes and byte sizes.
    //      Terminal update is best-effort (does not abort the return value).
    //   5. Returns the AgentToolResult as Result::ok in all cases except
    //      audit persistence failure (step 1/2), which returns Result::failure.
    //
    // Raw argumentsJson and result contentJson are never stored; only their
    // sha256 hashes are persisted in the audit record.
    [[nodiscard]] common::Result<AgentToolResult> executeToolCall(
        const std::string& auditId,
        const std::string& sessionId,
        const std::string& runId,
        const AgentToolCall& call,
        const WorkspaceReadPolicy& policy);

private:
    [[nodiscard]] common::Result<void> checkAvailable() const;

    ToolRegistry registry_;
    IWorkspaceToolExecutor& executor_;
    IAgentToolSessionRepository& sessions_;
    IAgentToolAuditRepository& audits_;
    bool storageAvailable_;
};

} // namespace ai::application
