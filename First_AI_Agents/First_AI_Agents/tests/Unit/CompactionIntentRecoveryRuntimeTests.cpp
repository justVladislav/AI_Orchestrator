// B3-II-B: the compaction-intent recovery as the REAL BackendRuntime runs it, on a REAL temporary file database.
//  - the owner of a file database finishes its constructor with the open intent already Unresolved; a second startup is stable;
//  - a guest, an in-memory database, a refused lock (a second runtime while the owner lives) and an unavailable database do not take
//    part: no report, no error, and nobody else's intent is touched;
//  - the report and the error are their own channel: a Partial report does not appear in recoveryError() and does not stop the other
//    startup passes (an interrupted Run is still recovered).
// No provider, no client, no credential, no live database; the intents are seeded through a second connection with the real
// repositories. The checks use throwing require().
#include "ai/application/conversations/CompactionIntentRecoveryReport.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntent.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/backend/BackendRuntime.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using ai::backend::BackendRuntime;
using ai::backend::BackendRuntimeConfig;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionIntentRepository;
using ai::infrastructure::SqliteRunRepository;
using St = CompactionIntentRecoveryStatus;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Workspace {
    fs::path root;
    fs::path projectRoot;
    fs::path databasePath;
    Workspace()
    {
        root = fs::temp_directory_path()
            / ("aio-b3iib-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        projectRoot = root / "project";
        databasePath = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(projectRoot);
    }
    ~Workspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

struct Raw {
    sqlite3* handle {nullptr};
    explicit Raw(const fs::path& file, int flags)
    {
        require(sqlite3_open_v2(utf8(file).c_str(), &handle, flags, nullptr) == SQLITE_OK, "raw open failed");
        sqlite3_busy_timeout(handle, 5000);
    }
    ~Raw() { sqlite3_close_v2(handle); }
    Raw(const Raw&) = delete;
    Raw& operator=(const Raw&) = delete;

    std::string exec(const std::string& sql) const
    {
        char* message = nullptr;
        const int rc = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
        std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
        sqlite3_free(message);
        return text;
    }
    long long scalar(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed: " + sql);
        long long value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }
    std::string text(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed: " + sql);
        std::string value = "<none>";
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* raw = sqlite3_column_text(statement, 0);
            value = raw != nullptr ? reinterpret_cast<const char*>(raw) : "<null>";
        }
        sqlite3_finalize(statement);
        return value;
    }
};

long long readonlyScalar(const fs::path& file, const std::string& sql) { return Raw(file, SQLITE_OPEN_READONLY).scalar(sql); }

ProjectMemoryCompactionIntent intentFor(const std::string& id, const ProjectId& project)
{
    ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = project;
    value.providerId = "provider-1";
    value.modelId = "model-1";
    value.sourceSegmentIds = {"s1", "s2"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(2000));
    return value;
}

// The previous process: a short-lived second connection with the real repositories (no runtime, no lock).
void seedOpenIntent(const Workspace& workspace, const std::string& id, const ProjectId& project)
{
    auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "seed: open");
    SqliteProjectMemoryCompactionIntentRepository intents(opened.value());
    require(mayDispatch(intents.recordIntentObserved(intentFor(id, project))), "seed: the intent is admitted");
}

ProjectId createProjectAndClose(const Workspace& workspace)
{
    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "durable storage");
    const auto project = runtime.projectService().createProject({"Project", "intent recovery", workspace.projectRoot});
    require(project.success(), "project");
    return project.value().id;
}

std::string moneySnapshot(const fs::path& file)
{
    Raw db(file, SQLITE_OPEN_READONLY);
    std::string out;
    for (const char* table : {"project_memory_compaction_records", "model_executions", "project_memory_segments"}) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(db.handle, (std::string("SELECT * FROM ") + table + " ORDER BY rowid;").c_str(), -1, &statement, nullptr) == SQLITE_OK,
            "dump prepare");
        out += std::string(table) + "\n";
        while (sqlite3_step(statement) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                const unsigned char* raw = sqlite3_column_text(statement, i);
                out += raw != nullptr ? reinterpret_cast<const char*>(raw) : "<NULL>";
                out += '\x1f';
            }
            out += '\n';
        }
        sqlite3_finalize(statement);
    }
    return out;
}

void requireOtherChannelsClean(const BackendRuntime& runtime, const std::string& what)
{
    require(!runtime.recoveryError().has_value() && !runtime.modelExecutionRecoveryError().has_value()
                && !runtime.comparisonRecoveryError().has_value() && !runtime.agentAuditRecoveryError().has_value(),
        what + ": the other startup channels carry nothing from this pass");
}

// ---- 1. the owner finishes the constructor with the intent Unresolved; a second startup is stable -----------------------------------
void testTheOwnerRecoversInTheConstructorAndASecondStartupIsStable()
{
    Workspace workspace;
    const ProjectId project = createProjectAndClose(workspace);
    seedOpenIntent(workspace, "op-1", project);
    {   // an unfinished Run in the same database: the OTHER passes must keep working
        auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "seed run: open");
        SqliteRunRepository runs(opened.value());
        Run unfinished;
        unfinished.id = "run-unfinished";
        unfinished.projectId = project;
        unfinished.title = "Interrupted";
        unfinished.task = "Recovered by the next BackendRuntime";
        unfinished.workflowId = "mock-sequential-v1";
        unfinished.status = RunStatus::Running;
        unfinished.progress = 0.5;
        unfinished.createdAt = std::chrono::system_clock::now();
        unfinished.startedAt = unfinished.createdAt;
        require(runs.insertRun(unfinished).success(), "seed run");
    }
    const std::string moneyBefore = moneySnapshot(workspace.databasePath);
    require(readonlyScalar(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_intents WHERE state='open';") == 1, "setup: one open intent");

    long long markedAt = -1;
    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "the owner opened the database");
        require(runtime.compactionIntentRecoveryReport().has_value(), "the owner has a report");
        const auto& report = *runtime.compactionIntentRecoveryReport();
        require(report.status == St::Complete && report.inspected == 1 && report.markedUnresolved == 1 && report.unconfirmed == 0
                    && report.conflicts == 0 && !runtime.compactionIntentRecoveryError().has_value(),
            "Complete: the open intent is Unresolved before the constructor returned");
        require(readonlyScalar(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_intents WHERE state='unresolved';") == 1
                    && readonlyScalar(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_intents WHERE state='open';") == 0,
            "TRUTH from a separate connection: unresolved, not deleted");
        markedAt = readonlyScalar(workspace.databasePath, "SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents;");
        require(markedAt > 0, "markedAt is set");
        requireOtherChannelsClean(runtime, "owner");
        // The other startup passes still ran: the Run a crash left Running is Interrupted on the very first read.
        bool interrupted = false;
        const auto runs = runtime.orchestrationService().listRuns();
        require(runs.success(), "list runs");
        for (const auto& run : runs.value()) {
            if (run.id == "run-unfinished") interrupted = run.status == RunStatus::Interrupted;
        }
        require(interrupted, "the Run recovery pass ran as before");
        require(moneySnapshot(workspace.databasePath) == moneyBefore, "records, ledger and segments are byte-for-byte unchanged");
    }
    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.compactionIntentRecoveryReport().has_value(), "a second owner startup has a report");
        const auto& report = *runtime.compactionIntentRecoveryReport();
        require(report.status == St::Complete && report.alreadyUnresolved == 1 && report.markedUnresolved == 0 && report.closedConfirmed == 0,
            "stable: already unresolved, nothing marked again");
        require(readonlyScalar(workspace.databasePath, "SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents;") == markedAt,
            "markedAt did not move");
        require(moneySnapshot(workspace.databasePath) == moneyBefore, "...and the money tables are still the same");
    }
}

// ---- 2. who does NOT take part ---------------------------------------------------------------------------------------------------------
void testAGuestAnInMemoryDatabaseAndARefusedLockNeverTouchAnotherOwnersIntent()
{
    Workspace workspace;
    const ProjectId project = createProjectAndClose(workspace);
    {
        BackendRuntime owner({workspace.databasePath});
        require(owner.compactionIntentRecoveryReport().has_value() && owner.compactionIntentRecoveryReport()->status == St::Complete
                    && owner.compactionIntentRecoveryReport()->inspected == 0,
            "the owner started with nothing to recover (Complete, zero rows)");
        seedOpenIntent(workspace, "op-live", project);   // the owner's live operation, written after its startup

        {   // a GUEST of the owner's database
            BackendRuntimeConfig guestConfig {workspace.databasePath};
            guestConfig.exclusiveDatabaseOwnership = false;
            BackendRuntime guest(guestConfig);
            require(guest.storageAvailable(), "the guest can read");
            require(!guest.compactionIntentRecoveryReport().has_value() && !guest.compactionIntentRecoveryError().has_value(),
                "guest: no report and no error - the pass did not take part");
            requireOtherChannelsClean(guest, "guest");
        }
        {   // a second owner while the first lives: the lock is refused, storage is unavailable
            BackendRuntime second({workspace.databasePath});
            require(!second.storageAvailable() && second.storageError().has_value()
                        && second.storageError()->code == "persistence.owned_by_another_instance",
                "the second runtime is refused by the owner lock");
            require(!second.compactionIntentRecoveryReport().has_value() && !second.compactionIntentRecoveryError().has_value(),
                "lock refused: no report and no error");
        }
        require(readonlyScalar(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_intents WHERE id='op-live' AND state='open';") == 1,
            "the live owner's intent is still open: nobody else marked it");
    }
    {   // an in-memory database: no file, no other process, no lock to hold
        BackendRuntime memory({":memory:"});
        require(!memory.compactionIntentRecoveryReport().has_value() && !memory.compactionIntentRecoveryError().has_value(),
            ":memory: does not take part");
    }
    {   // the owner is gone: the next owner finishes the job
        BackendRuntime next({workspace.databasePath});
        require(next.compactionIntentRecoveryReport().has_value() && next.compactionIntentRecoveryReport()->markedUnresolved == 1,
            "the next owner marks it");
    }
}

// ---- 3. its own channel ----------------------------------------------------------------------------------------------------------------------
void testAConflictIsPartialInItsOwnChannelAndDoesNotStopTheOtherPasses()
{
    Workspace workspace;
    const ProjectId project = createProjectAndClose(workspace);
    seedOpenIntent(workspace, "op-1", project);
    {   // a record under the same id that is a DIFFERENT operation, made while the step's triggers are away; the original SQL goes back
        Raw db(workspace.databasePath, SQLITE_OPEN_READWRITE);
        std::vector<std::string> definitions;
        for (const char* name : {"project_memory_compaction_intents_no_used_id", "project_memory_compaction_records_intent_guard",
                 "project_memory_compaction_records_intent_close"}) {
            definitions.push_back(db.text(std::string("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='") + name + "';"));
            require(definitions.back() != "<none>", "the original trigger exists");
        }
        require(db.exec("DROP TRIGGER project_memory_compaction_intents_no_used_id;"
                        "DROP TRIGGER project_memory_compaction_records_intent_guard;"
                        "DROP TRIGGER project_memory_compaction_records_intent_close;")
                    .empty(),
            "fixture: drop the guards");
        require(db.exec("INSERT INTO project_memory_compaction_records(id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,"
                        "input_tokens,output_tokens,cost_micros,status,error_code,error_message,created_at_ms) VALUES('op-1','" + project
                        + "','provider-1','model-1','[\"s1\",\"s2\"]','" + std::string(64, 'b') + "',40,15,425,'failed','provider.failed','x',2000);")
                    .empty(),
            "fixture: the conflicting record");
        for (const auto& sql : definitions) require(db.exec(sql + ";").empty(), "fixture: the original trigger is back");
        require(db.scalar("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name LIKE 'project_memory_compaction_%';") == 3,
            "all three guards are back before the runtime starts");
    }
    {   // and an unfinished Run, to show that the other passes go on
        auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "seed run: open");
        SqliteRunRepository runs(opened.value());
        Run unfinished;
        unfinished.id = "run-unfinished";
        unfinished.projectId = project;
        unfinished.title = "Interrupted";
        unfinished.task = "Recovered by the next BackendRuntime";
        unfinished.workflowId = "mock-sequential-v1";
        unfinished.status = RunStatus::Running;
        unfinished.createdAt = std::chrono::system_clock::now();
        unfinished.startedAt = unfinished.createdAt;
        require(runs.insertRun(unfinished).success(), "seed run");
    }
    const std::string moneyBefore = moneySnapshot(workspace.databasePath);

    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "the runtime starts normally");
    require(runtime.compactionIntentRecoveryReport().has_value() && runtime.compactionIntentRecoveryReport()->status == St::Partial
                && runtime.compactionIntentRecoveryReport()->conflicts == 1,
        "Partial with one conflict");
    require(runtime.compactionIntentRecoveryError().has_value() && runtime.compactionIntentRecoveryError()->code == kIntentRecordConflictCode,
        "its own error channel carries the first cause, unchanged");
    requireOtherChannelsClean(runtime, "conflict");
    bool interrupted = false;
    const auto runs = runtime.orchestrationService().listRuns();
    require(runs.success(), "list runs");
    for (const auto& run : runs.value()) {
        if (run.id == "run-unfinished") interrupted = run.status == RunStatus::Interrupted;
    }
    require(interrupted, "the other startup passes were not stopped by the Partial report");
    require(readonlyScalar(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_intents WHERE state='open';") == 1
                && moneySnapshot(workspace.databasePath) == moneyBefore,
        "the conflicting intent is untouched and so is every financial row");
}

} // namespace

int runCompactionIntentRecoveryRuntimeTests()
{
    try {
        testTheOwnerRecoversInTheConstructorAndASecondStartupIsStable();
        testAGuestAnInMemoryDatabaseAndARefusedLockNeverTouchAnotherOwnersIntent();
        testAConflictIsPartialInItsOwnChannelAndDoesNotStopTheOtherPasses();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentRecoveryRuntimeTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentRecoveryRuntimeTests: OK\n";
    return 0;
}
