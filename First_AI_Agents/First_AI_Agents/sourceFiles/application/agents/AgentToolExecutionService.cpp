#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/Sha256.hpp"

#include <chrono>
#include <utility>

namespace ai::application {

AgentToolExecutionService::AgentToolExecutionService(
    ToolRegistry registry,
    IWorkspaceToolExecutor& executor,
    IAgentToolSessionRepository& sessions,
    IAgentToolAuditRepository& audits,
    bool storageAvailable)
    : registry_(std::move(registry))
    , executor_(executor)
    , sessions_(sessions)
    , audits_(audits)
    , storageAvailable_(storageAvailable)
{
}

bool AgentToolExecutionService::storageAvailable() const noexcept
{
    return storageAvailable_;
}

const std::vector<AgentToolDefinition>& AgentToolExecutionService::toolDefinitions() const noexcept
{
    return registry_.definitions();
}

WorkspaceReadPolicy AgentToolExecutionService::buildPolicy(
    const std::filesystem::path& workspaceRoot)
{
    return WorkspaceReadPolicy::secureDefaults(workspaceRoot);
}

AgentToolSession AgentToolExecutionService::makeSession(
    const std::string& runId,
    const std::string& providerId,
    const std::string& modelId)
{
    AgentToolSession s;
    s.sessionId = generateUuid();
    s.runId = runId;
    s.providerId = providerId;
    s.modelId = modelId;
    s.state = AgentToolSessionState::Active;
    s.createdAt = std::chrono::system_clock::now();
    return s;
}

common::Result<void> AgentToolExecutionService::checkAvailable() const
{
    if (!storageAvailable_)
        return common::Result<void>::failure(
            {"agent_tool.storage_unavailable",
             "agent tool execution service is unavailable"});
    return common::Result<void>::ok();
}

common::Result<AgentToolSession> AgentToolExecutionService::beginSession(
    AgentToolSession session)
{
    if (const auto check = checkAvailable(); !check.success())
        return common::Result<AgentToolSession>::failure(*check.error());
    return sessions_.insertSession(std::move(session));
}

common::Result<AgentToolSession> AgentToolExecutionService::updateSession(
    AgentToolSession session)
{
    if (const auto check = checkAvailable(); !check.success())
        return common::Result<AgentToolSession>::failure(*check.error());
    return sessions_.updateSession(std::move(session));
}

common::Result<AgentProviderTurn> AgentToolExecutionService::recordTurn(
    AgentProviderTurn turn)
{
    if (const auto check = checkAvailable(); !check.success())
        return common::Result<AgentProviderTurn>::failure(*check.error());
    return sessions_.insertTurn(std::move(turn));
}

common::Result<AgentToolResult> AgentToolExecutionService::executeToolCall(
    const std::string& auditId,
    const std::string& sessionId,
    const std::string& runId,
    const AgentToolCall& call,
    const WorkspaceReadPolicy& policy)
{
    // Feature-flag / storage gate — return a typed error result, not failure.
    if (!storageAvailable_) {
        AgentToolResult err;
        err.callId   = call.callId;
        err.toolName = call.toolName;
        err.success  = false;
        err.errorCode = "agent_tool.storage_unavailable";
        return common::Result<AgentToolResult>::ok(std::move(err));
    }

    // 1. Insert Requested audit record (fail closed: abort if insert fails).
    AgentToolAuditRecord record;
    record.auditId         = auditId;
    record.sessionId       = sessionId;
    record.runId           = runId;
    record.providerCallId  = call.callId;
    record.toolName        = call.toolName;
    record.state           = AgentToolCallState::Requested;
    record.argumentsSha256 = sha256Hex(call.argumentsJson);
    record.createdAt       = std::chrono::system_clock::now();

    if (!audits_.insert(record).success())
        return common::Result<AgentToolResult>::failure(
            {"agent_tool.audit_failed",
             "could not persist requested audit record"});

    // 2. Update to Running before dispatching.
    record.state = AgentToolCallState::Running;
    if (!audits_.update(record).success())
        return common::Result<AgentToolResult>::failure(
            {"agent_tool.audit_failed",
             "could not update audit record to running state"});

    // 3. Dispatch to executor.
    auto execResult = executor_.execute(call, policy);

    // 4. Update to terminal state with hash + sizes (best-effort).
    record.finishedAt = std::chrono::system_clock::now();

    if (!execResult.success()) {
        // Executor itself failed (internal error, not a tool-level error).
        record.state     = AgentToolCallState::Failed;
        record.errorCode = execResult.error()->code;
        (void)audits_.update(record); // best-effort terminal update

        AgentToolResult err;
        err.callId    = call.callId;
        err.toolName  = call.toolName;
        err.success   = false;
        err.errorCode = execResult.error()->code;
        return common::Result<AgentToolResult>::ok(std::move(err));
    }

    // Executor returned a result (either success or tool-level error).
    const auto& toolResult = execResult.value();
    record.state         = AgentToolCallState::Completed;
    record.resultSha256  = sha256Hex(toolResult.contentJson);
    record.returnedBytes = toolResult.returnedBytes;
    record.truncated     = toolResult.truncated;
    record.errorCode     = toolResult.errorCode;
    (void)audits_.update(record); // best-effort terminal update

    return common::Result<AgentToolResult>::ok(std::move(execResult.value()));
}

} // namespace ai::application
