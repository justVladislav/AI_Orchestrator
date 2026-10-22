#include "ai/application/AgentToolTypes.hpp"
#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void req(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct AuditWorkspace {
    fs::path root {
        fs::temp_directory_path()
        / ("ai-orchestrator-audit-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "audit.sqlite3"};

    AuditWorkspace() { fs::create_directories(root); }
    ~AuditWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

// ── helpers ──────────────────────────────────────────────────────────────────

AgentToolSession makeSession(const std::string& sessionId, const std::string& runId = "run-1")
{
    AgentToolSession s;
    s.sessionId = sessionId;
    s.runId = runId;
    s.providerId = "openai";
    s.modelId = "gpt-4o";
    s.state = AgentToolSessionState::Active;
    s.createdAt = std::chrono::system_clock::now();
    return s;
}

AgentProviderTurn makeTurn(const std::string& turnId, const std::string& sessionId,
    std::size_t index = 0)
{
    AgentProviderTurn t;
    t.turnId = turnId;
    t.sessionId = sessionId;
    t.turnIndex = index;
    t.finishReason = "stop";
    t.inputTokens = 10;
    t.outputTokens = 20;
    t.callCount = 1;
    t.createdAt = std::chrono::system_clock::now();
    return t;
}

AgentToolAuditRecord makeAuditRecord(const std::string& id, const std::string& sessionId,
    const std::string& toolName = "read_file")
{
    AgentToolAuditRecord r;
    r.auditId = id;
    r.sessionId = sessionId;
    r.runId = "run-1";
    r.toolName = toolName;
    r.state = AgentToolCallState::Requested;
    r.argumentsSha256 = "aabbcc";
    r.createdAt = std::chrono::system_clock::now();
    return r;
}

// ── SQLite v21 migration ──────────────────────────────────────────────────────

void testV21TablesCreated()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "v21: Could not open fresh database");

    SqliteAgentToolSessionRepository sessions(opened.value());
    SqliteAgentToolAuditRepository audits(opened.value());

    // Basic insert into each new table proves all 3 were created by migration
    const auto s = sessions.insertSession(makeSession("s-migrate-1"));
    req(s.success(), "v21: agent_tool_sessions table not created by migration");

    const auto t = sessions.insertTurn(makeTurn("t-migrate-1", "s-migrate-1"));
    req(t.success(), "v21: agent_provider_turns table not created by migration");

    const auto a = audits.insert(makeAuditRecord("a-migrate-1", "s-migrate-1"));
    req(a.success(), "v21: agent_tool_calls table not created by migration");
}

// ── session lifecycle ─────────────────────────────────────────────────────────

void testSessionActiveToCompleted()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "session-lifecycle: Could not open database");

    SqliteAgentToolSessionRepository repo(opened.value());

    auto session = makeSession("s-lifecycle-1");
    const auto inserted = repo.insertSession(session);
    req(inserted.success(), "session-lifecycle: Insert failed");
    req(inserted.value().state == AgentToolSessionState::Active, "session-lifecycle: Wrong initial state");

    const auto found = repo.findSession("s-lifecycle-1");
    req(found.success() && found.value().has_value(), "session-lifecycle: findSession returned nothing");
    req(found.value()->state == AgentToolSessionState::Active, "session-lifecycle: Persisted state wrong");

    auto toComplete = inserted.value();
    toComplete.state = AgentToolSessionState::Completed;
    toComplete.totalTurns = 3;
    toComplete.totalCalls = 7;
    toComplete.totalReturnedBytes = 1024;
    toComplete.finishedAt = std::chrono::system_clock::now();

    const auto updated = repo.updateSession(toComplete);
    req(updated.success(), "session-lifecycle: Update to Completed failed");
    req(updated.value().state == AgentToolSessionState::Completed, "session-lifecycle: Updated state wrong");

    const auto again = repo.findSession("s-lifecycle-1");
    req(again.success() && again.value().has_value(), "session-lifecycle: findSession after update returned nothing");
    req(again.value()->state == AgentToolSessionState::Completed, "session-lifecycle: Persisted Completed state wrong");
    req(again.value()->totalCalls == 7, "session-lifecycle: totalCalls not persisted");
    req(again.value()->finishedAt.has_value(), "session-lifecycle: finishedAt not persisted");
}

void testSessionActiveToInterrupted()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "session-interrupted: Could not open database");

    SqliteAgentToolSessionRepository repo(opened.value());

    req(repo.insertSession(makeSession("s-int-1")).success(), "session-interrupted: Insert 1 failed");
    req(repo.insertSession(makeSession("s-int-2")).success(), "session-interrupted: Insert 2 failed");

    // Mark one completed — it must survive interruptActiveSessions
    auto complete = makeSession("s-int-3");
    complete.state = AgentToolSessionState::Completed;
    complete.finishedAt = std::chrono::system_clock::now();
    req(repo.insertSession(complete).success(), "session-interrupted: Insert completed session failed");

    const auto count = repo.interruptActiveSessions();
    req(count.success(), "session-interrupted: interruptActiveSessions failed");
    req(count.value() == 2, "session-interrupted: Wrong interrupted count (expected 2)");

    const auto found1 = repo.findSession("s-int-1");
    req(found1.success() && found1.value().has_value(), "session-interrupted: s-int-1 not found");
    req(found1.value()->state == AgentToolSessionState::Interrupted, "session-interrupted: s-int-1 not Interrupted");

    const auto found3 = repo.findSession("s-int-3");
    req(found3.success() && found3.value().has_value(), "session-interrupted: s-int-3 not found");
    req(found3.value()->state == AgentToolSessionState::Completed, "session-interrupted: Completed session state changed");
}

// ── turn insert / list ────────────────────────────────────────────────────────

void testTurnInsertAndList()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "turns: Could not open database");

    SqliteAgentToolSessionRepository repo(opened.value());

    req(repo.insertSession(makeSession("s-turn-1")).success(), "turns: Insert session failed");

    req(repo.insertTurn(makeTurn("t-turn-1", "s-turn-1", 0)).success(), "turns: Insert turn 0 failed");
    req(repo.insertTurn(makeTurn("t-turn-2", "s-turn-1", 1)).success(), "turns: Insert turn 1 failed");
    req(repo.insertTurn(makeTurn("t-turn-3", "s-turn-1", 2)).success(), "turns: Insert turn 2 failed");

    const auto turns = repo.listTurns("s-turn-1");
    req(turns.success(), "turns: listTurns failed");
    req(turns.value().size() == 3, "turns: Expected 3 turns");

    const auto empty = repo.listTurns("s-nonexistent");
    req(empty.success() && empty.value().empty(), "turns: listTurns for nonexistent session should be empty");
}

// ── tool call lifecycle ───────────────────────────────────────────────────────

void testToolCallRequestedToCompleted()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "toolcall: Could not open database");

    SqliteAgentToolSessionRepository sessions(opened.value());
    SqliteAgentToolAuditRepository audits(opened.value());

    req(sessions.insertSession(makeSession("s-tc-1")).success(), "toolcall: Insert session failed");

    auto record = makeAuditRecord("a-tc-1", "s-tc-1");
    req(audits.insert(record).success(), "toolcall: Insert Requested failed");

    const auto found = audits.find("a-tc-1");
    req(found.success() && found.value().has_value(), "toolcall: find Requested returned nothing");
    req(found.value()->state == AgentToolCallState::Requested, "toolcall: Requested state wrong");

    // Advance to Running
    auto running = record;
    running.state = AgentToolCallState::Running;
    req(audits.update(running).success(), "toolcall: Update to Running failed");

    // Advance to Completed
    auto completed = running;
    completed.state = AgentToolCallState::Completed;
    completed.resultSha256 = "ddeeff";
    completed.returnedBytes = 512;
    completed.finishedAt = std::chrono::system_clock::now();
    req(audits.update(completed).success(), "toolcall: Update to Completed failed");

    const auto final = audits.find("a-tc-1");
    req(final.success() && final.value().has_value(), "toolcall: find Completed returned nothing");
    req(final.value()->state == AgentToolCallState::Completed, "toolcall: Completed state wrong");
    req(final.value()->returnedBytes == 512, "toolcall: returnedBytes not persisted");
    req(final.value()->resultSha256 == "ddeeff", "toolcall: resultSha256 not persisted");
    req(final.value()->finishedAt.has_value(), "toolcall: finishedAt not persisted");
}

void testToolCallInterruptRunning()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "toolcall-interrupt: Could not open database");

    SqliteAgentToolSessionRepository sessions(opened.value());
    SqliteAgentToolAuditRepository audits(opened.value());

    req(sessions.insertSession(makeSession("s-tci-1")).success(), "toolcall-interrupt: Insert session failed");

    auto req1 = makeAuditRecord("a-tci-1", "s-tci-1", "read_file");
    req(audits.insert(req1).success(), "toolcall-interrupt: Insert call 1 failed");

    auto run1 = makeAuditRecord("a-tci-2", "s-tci-1", "write_file");
    run1.state = AgentToolCallState::Running;
    req(audits.insert(run1).success(), "toolcall-interrupt: Insert call 2 (Running) failed");

    auto done = makeAuditRecord("a-tci-3", "s-tci-1", "list_dir");
    done.state = AgentToolCallState::Completed;
    done.finishedAt = std::chrono::system_clock::now();
    req(audits.insert(done).success(), "toolcall-interrupt: Insert call 3 (Completed) failed");

    const auto interrupted = audits.interruptRunning("s-tci-1");
    req(interrupted.success(), "toolcall-interrupt: interruptRunning failed");
    req(interrupted.value() == 2, "toolcall-interrupt: Wrong interrupted count (expected 2)");

    const auto stillDone = audits.find("a-tci-3");
    req(stillDone.success() && stillDone.value().has_value(), "toolcall-interrupt: Completed call disappeared");
    req(stillDone.value()->state == AgentToolCallState::Completed, "toolcall-interrupt: Completed call changed state");
}

void testInterruptAllRunning()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "interruptAll: Could not open database");

    SqliteAgentToolSessionRepository sessions(opened.value());
    SqliteAgentToolAuditRepository audits(opened.value());

    req(sessions.insertSession(makeSession("s-ia-1", "run-1")).success(), "interruptAll: Insert session 1 failed");
    req(sessions.insertSession(makeSession("s-ia-2", "run-2")).success(), "interruptAll: Insert session 2 failed");

    // 2 Requested, 1 Running, 1 Completed — across 2 sessions
    auto r1 = makeAuditRecord("a-ia-1", "s-ia-1");
    req(audits.insert(r1).success(), "interruptAll: Insert r1 failed");

    auto r2 = makeAuditRecord("a-ia-2", "s-ia-2");
    req(audits.insert(r2).success(), "interruptAll: Insert r2 failed");

    auto run1 = makeAuditRecord("a-ia-3", "s-ia-1");
    run1.state = AgentToolCallState::Running;
    req(audits.insert(run1).success(), "interruptAll: Insert run1 failed");

    auto done = makeAuditRecord("a-ia-4", "s-ia-2");
    done.state = AgentToolCallState::Completed;
    done.finishedAt = std::chrono::system_clock::now();
    req(audits.insert(done).success(), "interruptAll: Insert done failed");

    const auto count = audits.interruptAllRunning();
    req(count.success(), "interruptAll: interruptAllRunning failed");
    req(count.value() == 3, "interruptAll: Expected 3 interrupted (2 Requested + 1 Running)");

    // Completed record must be untouched
    const auto remaining = audits.find("a-ia-4");
    req(remaining.success() && remaining.value().has_value(), "interruptAll: Completed record disappeared");
    req(remaining.value()->state == AgentToolCallState::Completed, "interruptAll: Completed record state changed");
}

// ── startup recovery (cross-repo) ─────────────────────────────────────────────

void testStartupRecovery()
{
    AuditWorkspace workspace;

    // First open: create dangling Active sessions and Requested/Running calls
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        req(opened.success(), "recovery: Could not open database (first)");

        SqliteAgentToolSessionRepository sessions(opened.value());
        SqliteAgentToolAuditRepository audits(opened.value());

        req(sessions.insertSession(makeSession("s-rec-1")).success(), "recovery: Insert active session failed");
        req(sessions.insertSession(makeSession("s-rec-2")).success(), "recovery: Insert active session 2 failed");

        req(audits.insert(makeAuditRecord("a-rec-1", "s-rec-1")).success(), "recovery: Insert Requested call failed");

        auto running = makeAuditRecord("a-rec-2", "s-rec-1");
        running.state = AgentToolCallState::Running;
        req(audits.insert(running).success(), "recovery: Insert Running call failed");
    }

    // Second open: startup recovery must mark everything Interrupted
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        req(opened.success(), "recovery: Could not reopen database");

        SqliteAgentToolSessionRepository sessions(opened.value());
        SqliteAgentToolAuditRepository audits(opened.value());

        const auto sessionCount = sessions.interruptActiveSessions();
        req(sessionCount.success(), "recovery: interruptActiveSessions failed");
        req(sessionCount.value() == 2, "recovery: Expected 2 sessions interrupted");

        const auto callCount = audits.interruptAllRunning();
        req(callCount.success(), "recovery: interruptAllRunning failed");
        req(callCount.value() == 2, "recovery: Expected 2 calls interrupted");

        // Verify persistence
        const auto s1 = sessions.findSession("s-rec-1");
        req(s1.success() && s1.value().has_value(), "recovery: s-rec-1 not found");
        req(s1.value()->state == AgentToolSessionState::Interrupted, "recovery: s-rec-1 not Interrupted");

        const auto a1 = audits.find("a-rec-1");
        req(a1.success() && a1.value().has_value(), "recovery: a-rec-1 not found");
        req(a1.value()->state == AgentToolCallState::Interrupted, "recovery: a-rec-1 not Interrupted");

        const auto a2 = audits.find("a-rec-2");
        req(a2.success() && a2.value().has_value(), "recovery: a-rec-2 not found");
        req(a2.value()->state == AgentToolCallState::Interrupted, "recovery: a-rec-2 not Interrupted");
    }
}

// ── listForRun / listForSession cross-repo ─────────────────────────────────────

void testListForRunAndSession()
{
    AuditWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    req(opened.success(), "list: Could not open database");

    SqliteAgentToolSessionRepository sessions(opened.value());
    SqliteAgentToolAuditRepository audits(opened.value());

    req(sessions.insertSession(makeSession("s-list-1", "run-A")).success(), "list: Insert s-list-1 failed");
    req(sessions.insertSession(makeSession("s-list-2", "run-A")).success(), "list: Insert s-list-2 failed");
    req(sessions.insertSession(makeSession("s-list-3", "run-B")).success(), "list: Insert s-list-3 failed");

    req(audits.insert(makeAuditRecord("a-list-1", "s-list-1", "read_file")).success(), "list: Insert a1 failed");
    req(audits.insert(makeAuditRecord("a-list-2", "s-list-1", "write_file")).success(), "list: Insert a2 failed");
    req(audits.insert(makeAuditRecord("a-list-3", "s-list-2", "list_dir")).success(), "list: Insert a3 failed");

    const auto forRunA = sessions.listForRun("run-A");
    req(forRunA.success(), "list: listForRun run-A failed");
    req(forRunA.value().size() == 2, "list: run-A should have 2 sessions");

    const auto forRunB = sessions.listForRun("run-B");
    req(forRunB.success(), "list: listForRun run-B failed");
    req(forRunB.value().size() == 1, "list: run-B should have 1 session");

    const auto callsForS1 = audits.listForSession("s-list-1");
    req(callsForS1.success(), "list: listForSession s-list-1 failed");
    req(callsForS1.value().size() == 2, "list: s-list-1 should have 2 calls");

    const auto callsForS2 = audits.listForSession("s-list-2");
    req(callsForS2.success(), "list: listForSession s-list-2 failed");
    req(callsForS2.value().size() == 1, "list: s-list-2 should have 1 call");
}

// ── unavailable-storage returns storage.unavailable ───────────────────────────

void testUnavailableSessionRepository()
{
    InMemoryAgentToolSessionRepository repo;

    const auto s = repo.insertSession(makeSession("s-mem-1"));
    req(s.success(), "in-memory: insertSession failed");

    const auto found = repo.findSession("s-mem-1");
    req(found.success() && found.value().has_value(), "in-memory: findSession returned nothing");
    req(found.value()->sessionId == "s-mem-1", "in-memory: sessionId mismatch");

    const auto t = repo.insertTurn(makeTurn("t-mem-1", "s-mem-1"));
    req(t.success(), "in-memory: insertTurn failed");

    const auto turns = repo.listTurns("s-mem-1");
    req(turns.success() && turns.value().size() == 1, "in-memory: listTurns wrong count");

    const auto forRun = repo.listForRun("run-1");
    req(forRun.success() && forRun.value().size() == 1, "in-memory: listForRun wrong count");

    const auto intCount = repo.interruptActiveSessions();
    req(intCount.success() && intCount.value() == 1, "in-memory: interruptActiveSessions wrong count");
}

void testUnavailableAuditRepository()
{
    InMemoryAgentToolAuditRepository repo;

    auto record = makeAuditRecord("a-mem-1", "s-mem-1");
    req(repo.insert(record).success(), "in-memory-audit: insert failed");

    const auto found = repo.find("a-mem-1");
    req(found.success() && found.value().has_value(), "in-memory-audit: find returned nothing");

    record.state = AgentToolCallState::Running;
    req(repo.update(record).success(), "in-memory-audit: update failed");

    const auto listed = repo.listForSession("s-mem-1");
    req(listed.success() && listed.value().size() == 1, "in-memory-audit: listForSession wrong count");

    const auto intRunning = repo.interruptRunning("s-mem-1");
    req(intRunning.success() && intRunning.value() == 1, "in-memory-audit: interruptRunning wrong count");

    // interruptAllRunning on empty after above
    const auto intAll = repo.interruptAllRunning();
    req(intAll.success() && intAll.value() == 0, "in-memory-audit: interruptAllRunning expected 0 after empty");
}

} // namespace

int runAgentToolAuditTests()
{
    testV21TablesCreated();
    testSessionActiveToCompleted();
    testSessionActiveToInterrupted();
    testTurnInsertAndList();
    testToolCallRequestedToCompleted();
    testToolCallInterruptRunning();
    testInterruptAllRunning();
    testStartupRecovery();
    testListForRunAndSession();
    testUnavailableSessionRepository();
    testUnavailableAuditRepository();
    return 0;
}
