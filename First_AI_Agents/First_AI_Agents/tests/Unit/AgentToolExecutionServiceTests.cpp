#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/backend/BackendRuntime.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using ai::backend::BackendRuntime;
using ai::backend::BackendRuntimeConfig;

void req(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// ── Stub executor ────────────────────────────────────────────────────────────

class AlwaysSucceedExecutor final : public IWorkspaceToolExecutor {
public:
    ai::common::Result<AgentToolResult> execute(
        const AgentToolCall& call,
        const WorkspaceReadPolicy& /*policy*/) override
    {
        AgentToolResult r;
        r.callId = call.callId;
        r.toolName = call.toolName;
        r.success = true;
        r.contentJson = R"({"lines":["hello"]})";
        r.returnedBytes = r.contentJson.size();
        return ai::common::Result<AgentToolResult>::ok(std::move(r));
    }
};

class AlwaysFailExecutor final : public IWorkspaceToolExecutor {
public:
    ai::common::Result<AgentToolResult> execute(
        const AgentToolCall& /*call*/,
        const WorkspaceReadPolicy& /*policy*/) override
    {
        return ai::common::Result<AgentToolResult>::failure(
            {"workspace.internal_error", "stub executor always fails"});
    }
};

// ── Helper: build a minimal service with in-memory repos ─────────────────────

struct ServiceFixture {
    InMemoryAgentToolSessionRepository sessions;
    InMemoryAgentToolAuditRepository   audits;
    AlwaysSucceedExecutor              executor;

    AgentToolExecutionService make(bool available = true)
    {
        return AgentToolExecutionService(
            ToolRegistry{},
            executor,
            sessions,
            audits,
            available);
    }
};

// ── Tests: storageAvailable flag ──────────────────────────────────────────────

void testUnavailableServiceRejectsAll()
{
    ServiceFixture fix;
    auto svc = fix.make(false);

    req(!svc.storageAvailable(), "unavail: storageAvailable should be false");

    const auto session = AgentToolExecutionService::makeSession("run-1", "openai", "gpt-4o");
    req(!svc.beginSession(session).success(), "unavail: beginSession should fail");
    req(svc.beginSession(session).error()->code == "agent_tool.storage_unavailable",
        "unavail: wrong error code from beginSession");

    AgentProviderTurn turn;
    turn.turnId = "t-1";
    turn.sessionId = session.sessionId;
    turn.createdAt = std::chrono::system_clock::now();
    req(!svc.recordTurn(turn).success(), "unavail: recordTurn should fail");

    AgentToolCall call;
    call.callId = "c-1";
    call.toolName = "workspace_read_file";
    call.argumentsJson = R"({"path":"README.md"})";

    const auto policy = AgentToolExecutionService::buildPolicy(fs::temp_directory_path());
    const auto result = svc.executeToolCall("a-1", session.sessionId, "run-1", call, policy);
    req(result.success(), "unavail: executeToolCall should return ok(AgentToolResult)");
    req(!result.value().success, "unavail: tool result should be failed");
    req(result.value().errorCode == "agent_tool.storage_unavailable",
        "unavail: tool result error code wrong");
}

// ── Tests: makeSession / buildPolicy ─────────────────────────────────────────

void testMakeSession()
{
    const auto s = AgentToolExecutionService::makeSession("run-A", "anthropic", "claude-3");
    req(!s.sessionId.empty(), "makeSession: sessionId must not be empty");
    req(s.runId == "run-A", "makeSession: runId wrong");
    req(s.providerId == "anthropic", "makeSession: providerId wrong");
    req(s.modelId == "claude-3", "makeSession: modelId wrong");
    req(s.state == AgentToolSessionState::Active, "makeSession: state must be Active");

    // Two consecutive sessions must have different IDs.
    const auto s2 = AgentToolExecutionService::makeSession("run-A", "anthropic", "claude-3");
    req(s.sessionId != s2.sessionId, "makeSession: IDs must be unique");
}

void testBuildPolicy()
{
    const auto root = fs::temp_directory_path() / "ai-orchestrator-policy-test";
    const auto policy = AgentToolExecutionService::buildPolicy(root);
    req(policy.workspaceRoot == root, "buildPolicy: workspaceRoot mismatch");
    req(policy.maxFileBytes > 0, "buildPolicy: maxFileBytes must be > 0");
    req(policy.maxDirectoryEntries > 0, "buildPolicy: maxDirectoryEntries must be > 0");
    // allowedRoots empty by design: workspace root alone constrains access.
    // The validator enforces any entries that do exist are safe relative paths.
    req(policy.allowedRoots.empty(), "buildPolicy: allowedRoots should be empty in secureDefaults");
    req(!policy.deniedDirectoryNames.empty(), "buildPolicy: deniedDirectoryNames must be populated");
}

// ── Tests: session lifecycle through in-memory repos ─────────────────────────

void testSessionLifecycle()
{
    ServiceFixture fix;
    auto svc = fix.make(true);

    auto session = AgentToolExecutionService::makeSession("run-lc", "openai", "gpt-4o");
    const std::string sid = session.sessionId;

    const auto inserted = svc.beginSession(session);
    req(inserted.success(), "lifecycle: beginSession failed");
    req(inserted.value().sessionId == sid, "lifecycle: sessionId mismatch after insert");
    req(inserted.value().state == AgentToolSessionState::Active, "lifecycle: state not Active");

    auto updated = inserted.value();
    updated.state = AgentToolSessionState::Completed;
    updated.totalCalls = 3;
    updated.finishedAt = std::chrono::system_clock::now();
    const auto updRes = svc.updateSession(updated);
    req(updRes.success(), "lifecycle: updateSession failed");
    req(updRes.value().state == AgentToolSessionState::Completed,
        "lifecycle: state not Completed after update");

    AgentProviderTurn turn;
    turn.turnId    = "t-lc-1";
    turn.sessionId = sid;
    turn.turnIndex = 0;
    turn.finishReason = "stop";
    turn.inputTokens  = 10;
    turn.outputTokens = 20;
    turn.createdAt    = std::chrono::system_clock::now();
    req(svc.recordTurn(turn).success(), "lifecycle: recordTurn failed");
}

// ── Tests: executeToolCall audit trail ───────────────────────────────────────

void testExecuteToolCallAuditTrail()
{
    ServiceFixture fix;
    auto svc = fix.make(true);

    auto session = AgentToolExecutionService::makeSession("run-audit", "openai", "gpt-4o");
    req(svc.beginSession(session).success(), "audit: beginSession failed");

    AgentToolCall call;
    call.callId       = "call-1";
    call.toolName     = "workspace_read_file";
    call.argumentsJson = R"({"path":"src/main.cpp"})";

    const auto policy = AgentToolExecutionService::buildPolicy(fs::temp_directory_path());
    const auto result = svc.executeToolCall(
        "audit-1", session.sessionId, "run-audit", call, policy);

    req(result.success(), "audit: executeToolCall returned failure");
    req(result.value().success, "audit: tool result should be successful");
    req(result.value().callId == "call-1", "audit: callId mismatch");
    req(result.value().returnedBytes > 0, "audit: returnedBytes should be > 0");

    // Verify the audit record was persisted.
    const auto records = fix.audits.listForSession(session.sessionId);
    req(records.success(), "audit: listForSession failed");
    req(records.value().size() == 1, "audit: expected 1 audit record");
    const auto& rec = records.value().front();
    req(rec.toolName == "workspace_read_file", "audit: toolName mismatch");
    req(rec.state == AgentToolCallState::Completed, "audit: state not Completed");
    req(!rec.argumentsSha256.empty(), "audit: argumentsSha256 must not be empty");
    req(!rec.resultSha256.empty(), "audit: resultSha256 must not be empty");
    req(rec.returnedBytes > 0, "audit: returnedBytes not persisted");
    req(rec.finishedAt.has_value(), "audit: finishedAt not set");
}

void testExecuteToolCallExecutorFailure()
{
    InMemoryAgentToolSessionRepository sessions;
    InMemoryAgentToolAuditRepository   audits;
    AlwaysFailExecutor                 executor;

    AgentToolExecutionService svc(ToolRegistry{}, executor, sessions, audits, true);

    auto session = AgentToolExecutionService::makeSession("run-fail", "openai", "gpt-4o");
    req(svc.beginSession(session).success(), "execfail: beginSession failed");

    AgentToolCall call;
    call.callId       = "call-f1";
    call.toolName     = "workspace_read_file";
    call.argumentsJson = R"({"path":"x.txt"})";

    const auto policy = AgentToolExecutionService::buildPolicy(fs::temp_directory_path());
    const auto result = svc.executeToolCall(
        "audit-f1", session.sessionId, "run-fail", call, policy);

    // executeToolCall returns ok(AgentToolResult{success:false}) when executor fails.
    req(result.success(), "execfail: executeToolCall returned Result::failure");
    req(!result.value().success, "execfail: tool result should not be successful");
    req(!result.value().errorCode.empty(), "execfail: errorCode should be set");

    // Audit record should be persisted in Failed state.
    const auto records = audits.listForSession(session.sessionId);
    req(records.success(), "execfail: listForSession failed");
    req(!records.value().empty(), "execfail: expected at least 1 audit record");
    req(records.value().front().state == AgentToolCallState::Failed,
        "execfail: audit record state should be Failed");
}

// ── Tests: BackendRuntime feature flag ───────────────────────────────────────

struct TempDb {
    fs::path root {
        fs::temp_directory_path()
        / ("ai-orch-s4-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path db {root / "state" / "orchestrator.sqlite3"};

    TempDb() { fs::create_directories(root / "project"); }
    ~TempDb() { std::error_code e; fs::remove_all(root, e); }
};

void testFeatureFlagOffMeansUnavailable()
{
    TempDb tmp;
    BackendRuntimeConfig config;
    config.databasePath = tmp.db;
    config.enableAgentToolExecution = false;

    BackendRuntime runtime(config);
    req(!runtime.agentToolExecutionAvailable(),
        "flag-off: agentToolExecutionAvailable must be false");
    req(!runtime.agentToolExecutionService().storageAvailable(),
        "flag-off: service.storageAvailable must be false");
}

void testFeatureFlagOnMeansAvailable()
{
    TempDb tmp;
    BackendRuntimeConfig config;
    config.databasePath = tmp.db;
    config.enableAgentToolExecution = true;

    BackendRuntime runtime(config);
    req(runtime.storageAvailable(), "flag-on: storageAvailable must be true");
    req(runtime.agentToolExecutionAvailable(),
        "flag-on: agentToolExecutionAvailable must be true");
    req(runtime.agentToolExecutionService().storageAvailable(),
        "flag-on: service.storageAvailable must be true");
}

void testAgentSessionFactoryUnavailableByDefault()
{
    TempDb tmp;
    BackendRuntimeConfig config;
    config.databasePath = tmp.db;
    // Default: enableAgentToolExecution = false

    BackendRuntime runtime(config);
    const auto session = runtime.agentSessionFactory().createSession("provider-1", "openai");
    req(!session.success(), "session-factory: createSession must fail when flag off");
    req(session.error()->code == "agent_session.unavailable",
        "session-factory: wrong error code");
}

void testToolDefinitionsAvailableWhenEnabled()
{
    TempDb tmp;
    BackendRuntimeConfig config;
    config.databasePath = tmp.db;
    config.enableAgentToolExecution = true;

    BackendRuntime runtime(config);
    // createDefaultReadOnly registry registers the 6 read-only workspace tools.
    req(!runtime.agentToolExecutionService().toolDefinitions().empty(),
        "tool-defs: expected at least one tool definition");
}

void testWorkspaceRootFromProject()
{
    TempDb tmp;
    // buildPolicy is static — workspace root must come from the project, never
    // from model output. Verify the policy reflects the trusted root.
    const auto root = tmp.root / "project";
    const auto policy = AgentToolExecutionService::buildPolicy(root);
    req(policy.workspaceRoot == root, "workspace-root: policy root must match project root");
}

} // namespace

int runAgentToolExecutionServiceTests()
{
    testUnavailableServiceRejectsAll();
    testMakeSession();
    testBuildPolicy();
    testSessionLifecycle();
    testExecuteToolCallAuditTrail();
    testExecuteToolCallExecutorFailure();
    testFeatureFlagOffMeansUnavailable();
    testFeatureFlagOnMeansAvailable();
    testAgentSessionFactoryUnavailableByDefault();
    testToolDefinitionsAvailableWhenEnabled();
    testWorkspaceRootFromProject();
    return 0;
}
