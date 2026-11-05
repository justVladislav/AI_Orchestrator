// C2-B/S2b-1: what happened to ONE compaction write, proven from what is really stored, and the health of the shared SQLite
// connection after a failed cleanup.
//
// Every database here is a temporary file (or ":memory:"). The fixtures are ordinary objects created in the MAIN schema of that
// temporary database through a SECOND connection (TEMP objects are connection-local and the tested connection would not see them);
// no production migration is involved. A real COMMIT failure is produced by a DEFERRABLE INITIALLY DEFERRED foreign key that a trigger
// violates: the INSERT passes, the COMMIT fails with SQLITE_CONSTRAINT_FOREIGNKEY and the transaction STAYS OPEN. The persistence-
// private fault seam (SqliteFaultForTesting.hpp) only prevents a ROLLBACK from running, so the transaction really stays open and every
// consequence (the quarantine, the refusal of other repositories) is the real one.
//
// The checks use throwing require(), so they stay active under NDEBUG.
#include "SqliteConnectionHealth.hpp"
#include "SqliteFaultForTesting.hpp"
#include "SqlitePrimitives.hpp"
#include "SqliteTransactionScope.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace detail = ai::infrastructure::sqlite_detail;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
using ai::infrastructure::SqliteProjectMemoryRepository;
using ai::infrastructure::SqliteProjectRepository;
using namespace ai::application;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point at(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

struct TempDb {
    fs::path root;
    fs::path file;

    TempDb()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path() / ("aio-s2b1-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                            + "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
        file = root / "state.sqlite3";
    }
    ~TempDb()
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

// SQL through a raw second connection; "" on success, the SQLite message otherwise.
std::string rawExec(const fs::path& file, const char* sql)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    char* message = nullptr;
    const int rc = sqlite3_exec(handle, sql, nullptr, nullptr, &message);
    std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
    sqlite3_free(message);
    sqlite3_close_v2(handle);
    return text;
}

long long rawCount(const fs::path& file, const char* sql)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed");
    long long value = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return value;
}

ProjectMemorySegment segment(const std::string& id, const std::string& projectId = "p1")
{
    ProjectMemorySegment value;
    value.id = id;
    value.projectId = projectId;
    value.runId = "run-" + id;
    value.summary = "Summary of " + id + " \xE6\xB8\xAC";
    value.sourceTokenCount = 11;
    value.summaryTokenCount = 7;
    value.createdAt = at(1000);
    return value;
}

ProjectMemoryCompactionRecord record(const std::string& id, ProjectMemoryCompactionStatus status,
    std::vector<std::string> sources = {"s1", "s2"}, const std::string& projectId = "p1")
{
    ProjectMemoryCompactionRecord value;
    value.id = id;
    value.projectId = projectId;
    value.providerId = "provider-1";
    value.modelId = "model-1";
    value.sourceSegmentIds = std::move(sources);
    value.sourceContentHash = std::string(64, 'a');
    value.inputTokens = 1234;
    value.outputTokens = 56;
    value.costMicros = 789;
    value.costPriceKnown = false;
    value.status = status;
    if (status == ProjectMemoryCompactionStatus::Completed) {
        value.resultSegmentId = "result-" + id;
    } else {
        value.errorCode = "project_memory.empty_summary";
        value.errorMessage = "Provider returned an empty compacted summary";
    }
    value.createdAt = at(2000);
    return value;
}

struct Fixture {
    TempDb temp;
    std::shared_ptr<SqliteDatabase> database;
    std::unique_ptr<SqliteProjectMemoryRepository> memory;
    std::unique_ptr<SqliteProjectMemoryCompactionRepository> compaction;

    Fixture()
    {
        open();
        require(rawExec(temp.file,
                    "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                    "VALUES('p1','One','d','C:/fixture/1','active',1,1),('p2','Two','d','C:/fixture/2','active',1,1);")
                    .empty(),
            "could not seed the projects");
        for (const char* id : {"s1", "s2", "keep"}) {
            require(memory->insertMemorySegment(segment(id)).success(), "could not seed a segment");
        }
    }

    void open()
    {
        memory.reset();
        compaction.reset();
        database.reset();
        auto opened = SqliteDatabase::open(temp.file);
        require(opened.success(), "could not open the temporary database");
        database = std::move(opened.value());
        memory = std::make_unique<SqliteProjectMemoryRepository>(database);
        compaction = std::make_unique<SqliteProjectMemoryCompactionRepository>(database);
    }

    long long segments() const { return rawCount(temp.file, "SELECT count(*) FROM project_memory_segments;"); }
    long long records() const { return rawCount(temp.file, "SELECT count(*) FROM project_memory_compaction_records;"); }
    long long recordsWithId(const char* id) const
    {
        return rawCount(temp.file, (std::string("SELECT count(*) FROM project_memory_compaction_records WHERE id='") + id + "';").c_str());
    }
    long long segmentsWithId(const char* id) const
    {
        return rawCount(temp.file, (std::string("SELECT count(*) FROM project_memory_segments WHERE id='") + id + "';").c_str());
    }

    bool inTransaction() const
    {
        const auto handles = detail::ConnectionAccess::of(*database);
        std::lock_guard lock(*handles.mutex);
        return sqlite3_get_autocommit(handles.handle) == 0;
    }
    int busyStatements() const
    {
        const auto handles = detail::ConnectionAccess::of(*database);
        std::lock_guard lock(*handles.mutex);
        return detail::busyStatementCount(handles.handle);
    }
    bool quarantined() const
    {
        const auto handles = detail::ConnectionAccess::of(*database);
        std::lock_guard lock(*handles.mutex);
        return handles.health->quarantined();
    }
};

// A real, deferred COMMIT failure: a Completed record makes a trigger insert a child row whose parent does not exist.
void installDeferredCommitFailure(const Fixture& f)
{
    require(rawExec(f.temp.file,
                "CREATE TABLE s2b_parent(id INTEGER PRIMARY KEY);"
                "CREATE TABLE s2b_child(parent_id INTEGER REFERENCES s2b_parent(id) DEFERRABLE INITIALLY DEFERRED);"
                "CREATE TRIGGER s2b_fail_commit AFTER INSERT ON project_memory_compaction_records WHEN NEW.status = 'completed' "
                "BEGIN INSERT INTO s2b_child VALUES (999); END;")
                .empty(),
        "could not install the deferred commit failure");
}

bool hasDiagnostic(const CompactionPersistOutcome& outcome, CompactionDiagnosticStage stage, const std::string& code)
{
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == stage && entry.error.code == code) return true;
    }
    return false;
}

int countDiagnostics(const CompactionPersistOutcome& outcome, CompactionDiagnosticStage stage)
{
    int count = 0;
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == stage) ++count;
    }
    return count;
}

void requireSameRecord(const ProjectMemoryCompactionRecord& a, const ProjectMemoryCompactionRecord& b, const std::string& what)
{
    require(a.id == b.id && a.projectId == b.projectId && a.providerId == b.providerId && a.modelId == b.modelId
            && a.sourceSegmentIds == b.sourceSegmentIds && a.sourceContentHash == b.sourceContentHash
            && a.resultSegmentId == b.resultSegmentId && a.inputTokens == b.inputTokens && a.outputTokens == b.outputTokens
            && a.costMicros == b.costMicros && a.costPriceKnown == b.costPriceKnown && a.status == b.status
            && a.errorCode == b.errorCode && a.errorMessage == b.errorMessage && a.createdAt == b.createdAt,
        what + ": every stored field must round-trip");
}

// ---- the legacy port default -----------------------------------------------------------------------------------------------------
class OldStyleRepository final : public IProjectMemoryCompactionRepository {
public:
    int inserts {0};
    int commits {0};
    bool fail {false};

    ai::common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord&) override
    {
        ++inserts;
        return fail ? ai::common::Result<void>::failure({"x.failed", "no"}) : ai::common::Result<void>::ok();
    }
    ai::common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        ++commits;
        return fail ? ai::common::Result<void>::failure({"x.failed", "no"}) : ai::common::Result<void>::ok();
    }
    ai::common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId&) const override
    {
        return ai::common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok({});
    }
};

void testTheDefaultObservedMethodsCallTheOldMethodOnceAndNeverClaimARollback()
{
    OldStyleRepository old;
    const auto committed = old.commitSuccessfulCompactionObserved(record("r", ProjectMemoryCompactionStatus::Completed), segment("x"));
    require(old.commits == 1 && committed.result.success() && committed.state == CompactionPersistState::Committed,
        "default observed commit: exactly one old call, success = Committed");
    require(!committed.connection.has_value(), "default observed commit: the connection is NOT observed (no invented Healthy)");
    const auto inserted = old.insertCompactionRecordObserved(record("r2", ProjectMemoryCompactionStatus::Failed));
    require(old.inserts == 1 && inserted.state == CompactionPersistState::Committed, "default observed insert: one old call");
    old.fail = true;
    const auto refusedCommit = old.commitSuccessfulCompactionObserved(record("r3", ProjectMemoryCompactionStatus::Completed), segment("y"));
    require(old.commits == 2 && !refusedCommit.result.success() && refusedCommit.result.error()->code == "x.failed"
            && refusedCommit.state == CompactionPersistState::Unknown && !refusedCommit.connection.has_value(),
        "default observed commit: a failure is UNKNOWN (an implementation that cannot prove a rollback must not claim one), the "
        "primary error is unchanged");
    const auto refusedInsert = old.insertCompactionRecordObserved(record("r4", ProjectMemoryCompactionStatus::Failed));
    require(old.inserts == 2 && refusedInsert.state == CompactionPersistState::Unknown, "default observed insert: a failure is Unknown");
    const auto lookup = old.findCompactionRecord("r");
    require(!lookup.success() && lookup.error()->code == "project_memory.lookup_unsupported",
        "default lookup: 'not supported', never an empty success");
}

// ---- real SQLite: baseline and round trip -----------------------------------------------------------------------------------------
void testObservedWritesRoundTripAndTheOldMethodsAreAThinView()
{
    Fixture f;
    const auto completed = record("rec-ok", ProjectMemoryCompactionStatus::Completed);
    const auto committed = f.compaction->commitSuccessfulCompactionObserved(completed, segment("result-rec-ok"));
    require(committed.result.success() && committed.state == CompactionPersistState::Committed
            && committed.connection == CompactionConnectionState::Healthy && committed.diagnostics.empty(),
        "observed commit: Committed, Healthy, no secondary causes");
    require(f.segmentsWithId("s1") == 0 && f.segmentsWithId("s2") == 0 && f.segmentsWithId("result-rec-ok") == 1
            && f.segmentsWithId("keep") == 1,
        "observed commit: replaced exactly the sources by the result");
    require(f.busyStatements() == 0 && !f.inTransaction(), "observed commit: nothing executing, no transaction left");

    ProjectMemoryCompactionRecord failed = record("rec-failed", ProjectMemoryCompactionStatus::Failed, {"a", "b", "c"});
    failed.costPriceKnown = std::nullopt;
    const auto inserted = f.compaction->insertCompactionRecordObserved(failed);
    require(inserted.result.success() && inserted.state == CompactionPersistState::Committed
            && inserted.connection == CompactionConnectionState::Healthy,
        "observed insert: Committed, Healthy");
    ProjectMemoryCompactionRecord cancelled = record("rec-cancelled", ProjectMemoryCompactionStatus::Cancelled, {});
    cancelled.costPriceKnown = true;
    require(f.compaction->insertCompactionRecord(cancelled).success(), "the old insert still works (a view of the observed one)");

    const auto foundCompleted = f.compaction->findCompactionRecord("rec-ok");
    require(foundCompleted.success() && foundCompleted.value().has_value(), "lookup: the committed record is found");
    requireSameRecord(*foundCompleted.value(), completed, "lookup of the committed record");
    const auto foundFailed = f.compaction->findCompactionRecord("rec-failed");
    require(foundFailed.success() && foundFailed.value().has_value(), "lookup: the failed record is found");
    requireSameRecord(*foundFailed.value(), failed, "lookup of the failed record (sources in order, NULL flag kept)");
    const auto foundCancelled = f.compaction->findCompactionRecord("rec-cancelled");
    require(foundCancelled.success() && foundCancelled.value().has_value()
                && foundCancelled.value()->costPriceKnown == std::optional<bool> {true},
        "lookup: the cancelled record keeps its true flag");
    const auto missing = f.compaction->findCompactionRecord("no-such-record");
    require(missing.success() && !missing.value().has_value(), "lookup: an unknown id is an empty SUCCESS (distinct from 'unsupported')");

    // a GLOBAL id: the lookup does not take a project
    ProjectMemoryCompactionRecord otherProject = record("rec-p2", ProjectMemoryCompactionStatus::Failed, {}, "p2");
    require(f.compaction->insertCompactionRecordObserved(otherProject).result.success(), "a record of another project");
    const auto foundOther = f.compaction->findCompactionRecord("rec-p2");
    require(foundOther.success() && foundOther.value().has_value() && foundOther.value()->projectId == "p2",
        "lookup: found by its global id whatever the project");

    f.open();   // reopen: nothing is lost
    const auto reopened = f.compaction->findCompactionRecord("rec-ok");
    require(reopened.success() && reopened.value().has_value(), "lookup after a reopen finds the record");
    requireSameRecord(*reopened.value(), completed, "lookup of the committed record after a reopen");
}

// ---- real COMMIT failure, real rollback -------------------------------------------------------------------------------------------
void testARealDeferredCommitFailureIsRolledBackAndProvenNotStored()
{
    Fixture f;
    installDeferredCommitFailure(f);
    const auto rec = record("rec-dfk", ProjectMemoryCompactionStatus::Completed);
    const auto outcome = f.compaction->commitSuccessfulCompactionObserved(rec, segment("result-rec-dfk"));

    require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed",
        "the PRIMARY error of the failed COMMIT is kept");
    require(outcome.result.error()->message.find("FOREIGN KEY") != std::string::npos,
        "...with SQLite's own text (captured before the rollback overwrote the handle's last error)");
    require(outcome.state == CompactionPersistState::NotCommitted, "a closed transaction + no stored rows = proven NotCommitted");
    require(outcome.connection == CompactionConnectionState::Healthy, "the real ROLLBACK worked: the connection is Healthy");
    require(countDiagnostics(outcome, CompactionDiagnosticStage::Rollback) == 0 && outcome.diagnostics.empty(),
        "a rollback that worked adds no secondary cause");
    require(!f.inTransaction() && !f.quarantined() && f.busyStatements() == 0, "no transaction left, not isolated, nothing executing");
    require(f.segments() == 3 && f.segmentsWithId("s1") == 1 && f.segmentsWithId("s2") == 1 && f.segmentsWithId("result-rec-dfk") == 0,
        "the source segments are intact and the result segment is absent (checked outside the transaction)");
    require(f.recordsWithId("rec-dfk") == 0, "the record is absent");
    // the connection is usable: a Failed record (not 'completed') passes the same trigger
    const auto next = f.compaction->insertCompactionRecordObserved(record("rec-after", ProjectMemoryCompactionStatus::Failed));
    require(next.result.success() && next.state == CompactionPersistState::Committed, "the next write on the connection works");
}

void testStepFailuresAndAStaleSourceGiveTheSameGuarantees()
{
    Fixture f;
    // (a) a failing INSERT of the result segment
    require(rawExec(f.temp.file,
                "CREATE TRIGGER s2b_segment_boom BEFORE INSERT ON project_memory_segments WHEN NEW.id = 'result-boom' "
                "BEGIN SELECT RAISE(ABORT, 'segment boom'); END;")
                .empty(),
        "install the segment failure");
    const auto segmentFailure = f.compaction->commitSuccessfulCompactionObserved(
        record("boom", ProjectMemoryCompactionStatus::Completed), segment("result-boom"));
    require(!segmentFailure.result.success() && segmentFailure.result.error()->code == "persistence.write_failed"
            && segmentFailure.result.error()->message.find("segment boom") != std::string::npos,
        "segment INSERT failure: the primary error and SQLite's text are kept");
    require(segmentFailure.state == CompactionPersistState::NotCommitted
            && segmentFailure.connection == CompactionConnectionState::Healthy && f.segments() == 3 && f.recordsWithId("boom") == 0,
        "segment INSERT failure: rolled back, the sources are restored, nothing stored");
    // (b) a failing INSERT of the record (after the sources were deleted and the segment inserted)
    require(rawExec(f.temp.file,
                "CREATE TRIGGER s2b_record_boom BEFORE INSERT ON project_memory_compaction_records WHEN NEW.id = 'rec-boom' "
                "BEGIN SELECT RAISE(ABORT, 'record boom'); END;")
                .empty(),
        "install the record failure");
    const auto recordFailure = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-boom", ProjectMemoryCompactionStatus::Completed), segment("result-rec-boom"));
    require(!recordFailure.result.success() && recordFailure.result.error()->message.find("record boom") != std::string::npos
            && recordFailure.state == CompactionPersistState::NotCommitted && f.segments() == 3
            && f.segmentsWithId("result-rec-boom") == 0 && f.recordsWithId("rec-boom") == 0,
        "record INSERT failure: the segment inserted earlier in the transaction is gone, the sources are back");
    // (c) a stale source
    const auto stale = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-stale", ProjectMemoryCompactionStatus::Completed, {"s1", "ghost"}), segment("result-rec-stale"));
    require(!stale.result.success() && stale.result.error()->code == "project_memory.compaction_conflict"
            && stale.state == CompactionPersistState::NotCommitted && f.segments() == 3 && f.segmentsWithId("s1") == 1
            && f.segmentsWithId("result-rec-stale") == 0 && f.recordsWithId("rec-stale") == 0,
        "stale source: the optimistic conflict keeps its code, everything already deleted is restored");
    require(!f.inTransaction() && f.busyStatements() == 0, "no transaction left, nothing executing");
}

void testAnExistingIdIsNeverTakenForANewSuccessfulSave()
{
    Fixture f;
    const auto first = record("rec-dup", ProjectMemoryCompactionStatus::Failed);
    require(f.compaction->insertCompactionRecord(first).success(), "the first record");
    ProjectMemoryCompactionRecord second = first;
    second.errorMessage = "a DIFFERENT message";
    const auto duplicate = f.compaction->insertCompactionRecordObserved(second);
    require(!duplicate.result.success() && duplicate.result.error()->code == "project_memory.duplicate_id",
        "the duplicate id keeps its primary error");
    require(duplicate.state == CompactionPersistState::NotCommitted, "our statement was refused by the constraint: this write is NotCommitted");
    require(hasDiagnostic(duplicate, CompactionDiagnosticStage::ProofRead, "project_memory.record_id_present"),
        "...and the proof says that a row with this id exists and was not compared");
    const auto stored = f.compaction->findCompactionRecord("rec-dup");
    require(stored.success() && stored.value().has_value() && stored.value()->errorMessage == first.errorMessage,
        "the stored row is exactly the first one (no overwrite)");

    // the same collision inside the atomic commit: rolled back, the segment inserted before it is gone
    const auto inCommit = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-dup", ProjectMemoryCompactionStatus::Completed), segment("result-rec-dup"));
    require(!inCommit.result.success() && inCommit.result.error()->code == "project_memory.duplicate_id"
            && inCommit.state == CompactionPersistState::NotCommitted && f.segments() == 3 && f.segmentsWithId("result-rec-dup") == 0
            && hasDiagnostic(inCommit, CompactionDiagnosticStage::ProofRead, "project_memory.record_id_present"),
        "duplicate record id inside the commit: rolled back, sources restored, the existing row reported");
    // a colliding SEGMENT id
    ProjectMemoryCompactionRecord collidingSegment = record("rec-seg", ProjectMemoryCompactionStatus::Completed);
    collidingSegment.resultSegmentId = "keep";   // the record names the result segment; here that id is already taken
    const auto segmentDuplicate = f.compaction->commitSuccessfulCompactionObserved(collidingSegment, segment("keep"));
    require(!segmentDuplicate.result.success() && segmentDuplicate.result.error()->code == "project_memory.duplicate_id"
            && segmentDuplicate.state == CompactionPersistState::NotCommitted && f.segments() == 3
            && hasDiagnostic(segmentDuplicate, CompactionDiagnosticStage::ProofRead, "project_memory.segment_id_present"),
        "duplicate result-segment id: rolled back, the existing segment is reported and is not ours");
}

// ---- the entry protocol and an unreadable proof ----------------------------------------------------------------------------------
void testABusyBeginIsNotCommittedAndTouchesNothingOfAnother()
{
    Fixture f;
    sqlite3* other = nullptr;
    require(sqlite3_open_v2(utf8(f.temp.file).c_str(), &other, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "second connection");
    require(sqlite3_exec(other, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "another writer holds the write lock");
    {
        detail::ScopedConnectionFault fault(*f.database, detail::FaultPlan {}, 0);   // busy_timeout 0: do not wait five seconds
        const auto outcome = f.compaction->commitSuccessfulCompactionObserved(
            record("rec-busy", ProjectMemoryCompactionStatus::Completed), segment("result-rec-busy"));
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed",
            "BEGIN refused by another writer: the primary error is kept");
        require(outcome.state == CompactionPersistState::NotCommitted && outcome.connection == CompactionConnectionState::Healthy,
            "...nothing of the write was started and the connection is healthy");
        require(!f.quarantined() && !f.inTransaction(), "no transaction was opened or closed by us");
    }
    require(sqlite3_get_autocommit(other) == 0, "the OTHER writer's transaction was not touched (still open)");
    require(sqlite3_exec(other, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK, "the other writer ends its own transaction");
    sqlite3_close_v2(other);
    const auto afterwards = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-busy", ProjectMemoryCompactionStatus::Completed), segment("result-rec-busy"));
    require(afterwards.result.success(), "once the lock is free the same write succeeds");
}

void testAnUnreadableProofIsUnknown()
{
    Fixture f;
    installDeferredCommitFailure(f);
    detail::ScopedConnectionFault fault(*f.database, [] { detail::FaultPlan plan; plan.failProofRead = true; return plan; }());
    const auto outcome = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-proof", ProjectMemoryCompactionStatus::Completed), segment("result-rec-proof"));
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed",
        "the primary error is kept");
    require(outcome.state == CompactionPersistState::Unknown, "the proof could not be read: Unknown, not NotCommitted");
    require(outcome.connection == CompactionConnectionState::Healthy, "the connection is a separate axis: Healthy");
    require(hasDiagnostic(outcome, CompactionDiagnosticStage::ProofRead, "persistence.read_failed"), "the proof failure is a separate cause");
}

// ---- a rollback that cannot be done: the real transaction stays open ---------------------------------------------------------------
void testARefusedRollbackIsolatesTheConnectionForEveryRepositoryAndOnlyTheOwnerRecovers()
{
    Fixture f;
    installDeferredCommitFailure(f);
    SqliteProjectRepository projects(f.database);
    {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 3;   // one more than the permitted two: the third must NOT be consumed
        detail::ScopedConnectionFault fault(*f.database, plan);
        const auto outcome = f.compaction->commitSuccessfulCompactionObserved(
            record("rec-stuck", ProjectMemoryCompactionStatus::Completed), segment("result-rec-stuck"));

        require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed"
                && outcome.result.error()->message.find("FOREIGN KEY") != std::string::npos,
            "the PRIMARY COMMIT error is kept and is not replaced by the cleanup failures");
        require(outcome.state == CompactionPersistState::Unknown, "the transaction could not be closed: the outcome is Unknown");
        require(outcome.connection == CompactionConnectionState::Quarantined, "the connection is isolated");
        require(countDiagnostics(outcome, CompactionDiagnosticStage::Rollback) == 2
                    && hasDiagnostic(outcome, CompactionDiagnosticStage::Rollback, "persistence.rollback_failed"),
            "exactly TWO rollback attempts, each reported separately");
        require(hasDiagnostic(outcome, CompactionDiagnosticStage::ProofRead, "persistence.proof_unavailable"),
            "the proof refuses to read while a transaction is open (its own rows are not proof)");
        const auto handles = detail::ConnectionAccess::of(*f.database);
        require(handles.fault->skipRollbackAttempts == 1, "no third rollback round was started");
        require(f.inTransaction(), "the REAL transaction is still open (autocommit == 0)");
        require(f.quarantined(), "the health state says quarantined");
    }

    // The fault is gone, the transaction is real, the connection is isolated: a DIFFERENT repository gets a typed refusal and
    // writes/commits nothing.
    ai::application::Project other;
    other.id = "p-other";
    other.name = "Other";
    other.description = "d";
    other.rootPath = f.temp.root / "other";
    other.createdAt = at(5);
    other.updatedAt = at(5);
    const auto refusedProject = projects.insert(other);
    require(!refusedProject.success() && refusedProject.error()->code == "persistence.connection_unusable",
        "another repository's write is refused with persistence.connection_unusable");
    const auto refusedRead = f.memory->listMemorySegments("p1");
    require(!refusedRead.success() && refusedRead.error()->code == "persistence.connection_unusable",
        "another repository's READ is refused too (nothing is read inside the open transaction)");
    {
        const auto handles = detail::ConnectionAccess::of(*f.database);
        std::lock_guard lock(*handles.mutex);
        require(sqlite3_exec(handles.handle, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_AUTH,
            "an ordinary caller cannot roll the open transaction back");
        require(sqlite3_get_autocommit(handles.handle) == 0, "...it is still open");
    }

    // Lazy recovery: the NEXT entry of the owner closes ITS transaction (real ROLLBACK, bounded), lifts the isolation and goes on.
    const auto next = f.compaction->insertCompactionRecordObserved(record("rec-next", ProjectMemoryCompactionStatus::Failed));
    require(next.result.success() && next.state == CompactionPersistState::Committed
            && next.connection == CompactionConnectionState::Healthy,
        "the owner's next call recovers its own transaction and succeeds");
    require(!f.inTransaction() && !f.quarantined() && f.busyStatements() == 0, "no transaction, no isolation, nothing executing");
    require(f.recordsWithId("rec-stuck") == 0 && f.segmentsWithId("result-rec-stuck") == 0 && f.segments() == 3,
        "what the stuck write had done was rolled back, not committed: sources intact, no result, no record");
    require(f.recordsWithId("rec-next") == 1, "the owner's next write is committed");
    require(projects.insert(other).success(), "after the recovery the other repository works again");
    require(rawCount(f.temp.file, "SELECT count(*) FROM projects WHERE id='p-other';") == 1, "...and its row is committed");
}

void testATransactionThatIsNotOursIsNeverRolledBackOrAdopted()
{
    Fixture f;
    SqliteProjectRepository projects(f.database);
    const auto handles = detail::ConnectionAccess::of(*f.database);
    {   // somebody else's transaction, opened directly on the shared connection
        std::lock_guard lock(*handles.mutex);
        require(sqlite3_exec(handles.handle, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "a foreign transaction");
        require(sqlite3_exec(handles.handle, "INSERT INTO project_memory_segments VALUES('marker','p1','r','m',0,0,1);",
                    nullptr, nullptr, nullptr) == SQLITE_OK,
            "with an uncommitted marker row");
    }
    const auto outcome = f.compaction->insertCompactionRecordObserved(record("rec-foreign", ProjectMemoryCompactionStatus::Failed));
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.connection_unusable",
        "the write is refused at the entry with a typed code");
    require(outcome.state == CompactionPersistState::NotCommitted, "nothing of it was issued: proven NotCommitted");
    require(outcome.connection == CompactionConnectionState::Quarantined, "the connection is isolated");
    {
        std::lock_guard lock(*handles.mutex);
        require(sqlite3_get_autocommit(handles.handle) == 0, "the foreign transaction was NOT rolled back");
        require(handles.health->transactionOwner() == nullptr, "...and was not adopted (no owner tag)");
        require(handles.health->reason() == detail::QuarantineReason::ForeignOpenTransaction, "the reason is recorded");
    }
    ai::application::Project other;
    other.id = "p-other";
    other.name = "Other";
    other.description = "d";
    other.rootPath = f.temp.root / "other";
    other.createdAt = at(5);
    other.updatedAt = at(5);
    require(!projects.insert(other).success(), "no repository can write into the foreign transaction");
    // A second compaction call does not roll it back either (it is not ours): still refused.
    const auto again = f.compaction->insertCompactionRecordObserved(record("rec-foreign2", ProjectMemoryCompactionStatus::Failed));
    require(!again.result.success() && again.state == CompactionPersistState::NotCommitted, "still refused, still nothing issued");
    {
        std::lock_guard lock(*handles.mutex);
        require(sqlite3_get_autocommit(handles.handle) == 0, "the foreign transaction is still open");
        // Its owner ends it (inside the narrow window the quarantine leaves for the owner), then the isolation is lifted.
        {
            detail::ConnectionHealth::RecoveryWindow window(*handles.health);
            require(sqlite3_exec(handles.handle, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK, "the owner's own rollback");
        }
        require(handles.health->release(handles.handle).success(), "the isolation is lifted by an explicit release");
    }
    require(f.segmentsWithId("marker") == 0, "the marker row was never committed");
    require(f.compaction->insertCompactionRecordObserved(record("rec-after-foreign", ProjectMemoryCompactionStatus::Failed)).result.success()
                && projects.insert(other).success(),
        "everything works again after the owner closed its own transaction");
}

void testAnExceptionAfterBeginCleansUpWithoutReplacingItAndWithoutATerminate()
{
    Fixture f;
    {
        detail::FaultPlan plan;
        plan.throwAfterBegin = true;
        detail::ScopedConnectionFault fault(*f.database, plan);
        bool caught = false;
        try {
            static_cast<void>(f.compaction->commitSuccessfulCompactionObserved(
                record("rec-throw", ProjectMemoryCompactionStatus::Completed), segment("result-rec-throw")));
        } catch (const std::runtime_error& error) {
            caught = std::string(error.what()).find("exception after BEGIN") != std::string::npos;
        }
        require(caught, "the original exception reaches the caller");
    }
    require(!f.inTransaction() && !f.quarantined() && f.busyStatements() == 0, "the scope's destructor closed the transaction");
    require(f.segments() == 3 && f.recordsWithId("rec-throw") == 0, "nothing was changed");

    // A cleanup that FAILS while the exception is unwinding must neither terminate nor replace the exception.
    {
        detail::FaultPlan plan;
        plan.throwAfterBegin = true;
        plan.skipRollbackAttempts = 3;
        detail::ScopedConnectionFault fault(*f.database, plan);
        bool original = false;
        try {
            static_cast<void>(f.compaction->commitSuccessfulCompactionObserved(
                record("rec-throw2", ProjectMemoryCompactionStatus::Completed), segment("result-rec-throw2")));
        } catch (const std::runtime_error& error) {
            original = std::string(error.what()).find("exception after BEGIN") != std::string::npos;
        } catch (...) {
        }
        require(original, "the original exception is not replaced by the failing cleanup");
        const auto handles = detail::ConnectionAccess::of(*f.database);
        require(handles.fault->skipRollbackAttempts == 1, "the destructor used exactly two attempts, no third round");
        require(f.inTransaction() && f.quarantined(), "the transaction really stayed open and the connection is isolated");
    }
    const auto recovered = f.compaction->insertCompactionRecordObserved(record("rec-recovered", ProjectMemoryCompactionStatus::Failed));
    require(recovered.result.success() && !f.quarantined() && !f.inTransaction(), "the owner's next call recovers");
    require(f.segments() == 3 && f.segmentsWithId("result-rec-throw2") == 0, "the interrupted write left nothing");
}

// ---- the authorizer lifecycle itself -----------------------------------------------------------------------------------------------
void testTheQuarantineBlocksStatementsPreparedBeforeItAndEachCycleRepeats()
{
    TempDb temp;
    auto opened = SqliteDatabase::open(temp.file);
    require(opened.success(), "open");
    auto database = std::move(opened.value());
    const auto handles = detail::ConnectionAccess::of(*database);
    require(rawExec(temp.file, "CREATE TABLE t(x TEXT);").empty(), "fixture table");

    for (int cycle = 0; cycle < 2; ++cycle) {
        std::lock_guard lock(*handles.mutex);
        sqlite3* h = handles.handle;
        sqlite3_stmt* select = nullptr;
        sqlite3_stmt* insertNever = nullptr;
        sqlite3_stmt* insertOnce = nullptr;
        require(sqlite3_prepare_v2(h, "SELECT count(*) FROM t;", -1, &select, nullptr) == SQLITE_OK, "prepare SELECT before");
        require(sqlite3_prepare_v2(h, "INSERT INTO t VALUES ('never-run');", -1, &insertNever, nullptr) == SQLITE_OK, "prepare INSERT before");
        require(sqlite3_prepare_v2(h, "INSERT INTO t VALUES ('ran-once');", -1, &insertOnce, nullptr) == SQLITE_OK, "prepare INSERT 2 before");
        require(sqlite3_step(insertOnce) == SQLITE_DONE, "run it once, healthy");
        sqlite3_reset(insertOnce);
        require(detail::busyStatementCount(h) == 0, "nothing is executing at the transition (the production invariant)");
        const long long before = rawCount(temp.file, "SELECT count(*) FROM t;");

        require(handles.health->enterQuarantine(h, detail::QuarantineReason::ForeignOpenTransaction).success(), "enter the quarantine");
        // Statements prepared BEFORE the quarantine - never run, and run once then reset - are refused at their next step.
        const int selectRc = sqlite3_step(select);
        const int neverRc = sqlite3_step(insertNever);
        const int onceRc = sqlite3_step(insertOnce);
        require(selectRc == SQLITE_AUTH && neverRc == SQLITE_AUTH && onceRc == SQLITE_AUTH,
            "statements prepared before the quarantine are refused (not just fresh ones): flag-only switching would let them run");
        require(rawCount(temp.file, "SELECT count(*) FROM t;") == before, "and they wrote nothing");
        // The shared error text names the quarantine for the refusal at step AND at prepare / exec
        const auto stepError = detail::databaseError(h, "persistence.write_failed", "x");
        require(stepError.code == "persistence.connection_unusable", "a refusal at step is translated by the shared databaseError");
        const auto execError = detail::execute(h, "SELECT 1;", "persistence.anything");
        require(!execError.success() && execError.error()->code == "persistence.connection_unusable",
            "a fresh exec is translated by the shared execute");
        sqlite3_stmt* fresh = nullptr;
        require(sqlite3_prepare_v2(h, "SELECT 1;", -1, &fresh, nullptr) == SQLITE_AUTH, "a fresh prepare is refused");
        sqlite3_finalize(fresh);

        sqlite3_reset(select);
        sqlite3_reset(insertNever);
        sqlite3_reset(insertOnce);
        require(handles.health->release(h).success(), "release");
        require(sqlite3_step(select) == SQLITE_ROW, "after the release the earlier SELECT works again (re-prepared)");
        sqlite3_reset(select);
        require(sqlite3_step(insertNever) == SQLITE_DONE, "...and the earlier INSERT that never ran works");
        sqlite3_reset(insertNever);
        require(sqlite3_step(insertOnce) == SQLITE_DONE, "...and the one that ran once works");
        sqlite3_finalize(select);
        sqlite3_finalize(insertNever);
        sqlite3_finalize(insertOnce);
        require(rawCount(temp.file, "SELECT count(*) FROM t;") == before + 2, "exactly the two released inserts were committed");
    }

    // An authorization error of a connection that is NOT quarantined is some other error and is not renamed.
    {
        std::lock_guard lock(*handles.mutex);
        sqlite3* h = handles.handle;
        sqlite3_set_authorizer(h, [](void*, int, const char*, const char*, const char*, const char*) { return SQLITE_DENY; }, nullptr);
        const auto foreign = detail::execute(h, "SELECT 1;", "persistence.anything");
        sqlite3_set_authorizer(h, nullptr, nullptr);
        require(!foreign.success() && foreign.error()->code == "persistence.anything",
            "SQLITE_AUTH without a known quarantine keeps the caller's own code");
    }
}

void testTheDatabaseClosesSafelyAfterTheAuthorizerWasUsed()
{
    TempDb temp;
    {
        auto opened = SqliteDatabase::open(temp.file);
        require(opened.success(), "open");
        auto database = std::move(opened.value());
        const auto handles = detail::ConnectionAccess::of(*database);
        std::lock_guard lock(*handles.mutex);
        require(rawExec(temp.file, "CREATE TABLE t(x TEXT);").empty(), "fixture");
        require(sqlite3_exec(handles.handle, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "an open transaction");
        require(sqlite3_exec(handles.handle, "INSERT INTO t VALUES ('uncommitted');", nullptr, nullptr, nullptr) == SQLITE_OK, "a write");
        require(handles.health->enterQuarantine(handles.handle, detail::QuarantineReason::RollbackFailed).success(), "isolated");
        // the database is destroyed here, isolated, with the transaction open
    }
    require(rawCount(temp.file, "SELECT count(*) FROM t;") == 0, "closing rolled the open transaction back; nothing was committed");
    auto reopened = SqliteDatabase::open(temp.file);
    require(reopened.success(), "reopening succeeds");

    auto memory = SqliteDatabase::open(":memory:");
    require(memory.success(), "an in-memory database opens");
    {
        const auto handles = detail::ConnectionAccess::of(*memory.value());
        std::lock_guard lock(*handles.mutex);
        require(handles.health->enterQuarantine(handles.handle, detail::QuarantineReason::BeginStateUnknown).success(), "isolate");
        require(handles.health->release(handles.handle).success(), "release");
    }
    memory.value().reset();
}

void testTwoRepositoriesShareTheConnectionSeriallyThroughTheSameMutex()
{
    Fixture f;
    SqliteProjectRepository projects(f.database);
    std::atomic<int> failures {0};
    std::thread writer([&] {
        for (int i = 0; i < 25; ++i) {
            const auto outcome = f.compaction->insertCompactionRecordObserved(
                record("rec-thread-" + std::to_string(i), ProjectMemoryCompactionStatus::Failed));
            if (!outcome.result.success() || outcome.state != CompactionPersistState::Committed) ++failures;
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 25; ++i) {
            ai::application::Project project;
            project.id = "p-thread-" + std::to_string(i);
            project.name = "T";
            project.description = "d";
            project.rootPath = f.temp.root / ("t" + std::to_string(i));
            project.createdAt = at(5);
            project.updatedAt = at(5);
            if (!projects.insert(project).success()) ++failures;
        }
    });
    writer.join();
    reader.join();
    require(failures.load() == 0, "two repositories on the one connection ran serially without a failure");
    require(rawCount(f.temp.file, "SELECT count(*) FROM project_memory_compaction_records WHERE id LIKE 'rec-thread-%';") == 25,
        "every compaction record was committed");
    require(!f.inTransaction() && !f.quarantined(), "the connection is healthy afterwards");
}

// ---- S2b-1 follow-up: R1 (a cleanup whose own diagnostics fail), R2 (causes kept), the failed lifting, R3 (strict lookup) ----------
// A DEFERRED-FK failure only for record ids that start with "bad-": other Completed records commit normally, so one fixture can
// leave a transaction stuck and then do an ordinary write.
void installSelectiveCommitFailure(const Fixture& f)
{
    require(rawExec(f.temp.file,
                "CREATE TABLE s2b_parent(id INTEGER PRIMARY KEY);"
                "CREATE TABLE s2b_child(parent_id INTEGER REFERENCES s2b_parent(id) DEFERRABLE INITIALLY DEFERRED);"
                "CREATE TRIGGER s2b_fail_bad AFTER INSERT ON project_memory_compaction_records "
                "WHEN NEW.status = 'completed' AND NEW.id GLOB 'bad-*' BEGIN INSERT INTO s2b_child VALUES (999); END;")
                .empty(),
        "could not install the selective deferred commit failure");
}

ai::application::Project anotherProject(const Fixture& f)
{
    ai::application::Project other;
    other.id = "p-other";
    other.name = "Other";
    other.description = "d";
    other.rootPath = f.temp.root / "other";
    other.createdAt = at(5);
    other.updatedAt = at(5);
    return other;
}

void requireOtherRepositoryRefused(Fixture& f, const std::string& what)
{
    SqliteProjectRepository projects(f.database);
    const auto refused = projects.insert(anotherProject(f));
    require(!refused.success() && refused.error()->code == "persistence.connection_unusable",
        what + ": a DIFFERENT repository's write is refused with persistence.connection_unusable");
    require(rawCount(f.temp.file, "SELECT count(*) FROM projects WHERE id='p-other';") == 0, what + ": ...and nothing of it was written");
}

int skipsLeft(Fixture& f)
{
    const auto handles = detail::ConnectionAccess::of(*f.database);
    std::lock_guard lock(*handles.mutex);
    return handles.fault->skipRollbackAttempts;
}

// The owner's next ordinary write closes ITS transaction and lifts the isolation.
void requireOwnerRecovers(Fixture& f, const std::string& id, const std::string& what)
{
    const auto next = f.compaction->insertCompactionRecordObserved(record(id, ProjectMemoryCompactionStatus::Failed));
    require(next.result.success() && next.state == CompactionPersistState::Committed, what + ": the owner's next call recovers and writes");
    require(!f.inTransaction() && !f.quarantined() && f.busyStatements() == 0, what + ": no transaction, no isolation, nothing executing");
}

// R1. The diagnostics of a failed ROLLBACK cannot be built (the allocation of one fails inside the cleanup). The attempts are still
// counted, the connection is still isolated, nothing throws out of the cleanup, and the destructor does not start another round.
void testACleanupWhoseDiagnosticsFailStillIsolatesTheConnectionAndKeepsTheBudget()
{
    Fixture f;
    installDeferredCommitFailure(f);

    // (1) the explicit cleanup after a failed COMMIT
    {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 3;
        plan.throwWhileRecordingRollbackFailure = true;
        detail::ScopedConnectionFault fault(*f.database, plan);
        CompactionPersistOutcome outcome;
        bool threw = false;
        try {
            outcome = f.compaction->commitSuccessfulCompactionObserved(
                record("rec-r1a", ProjectMemoryCompactionStatus::Completed), segment("result-rec-r1a"));
        } catch (...) {
            threw = true;
        }
        require(!threw, "R1(1): nothing throws out of the cleanup");
        require(!outcome.result.success() && outcome.result.error()->code == "persistence.transaction_failed"
                && outcome.result.error()->message.find("FOREIGN KEY") != std::string::npos,
            "R1(1): the primary COMMIT error is kept");
        require(outcome.state == CompactionPersistState::Unknown && outcome.connection == CompactionConnectionState::Quarantined,
            "R1(1): the transaction is open: Unknown, and the connection is ISOLATED");
        require(outcome.diagnosticsIncomplete && countDiagnostics(outcome, CompactionDiagnosticStage::Rollback) == 0,
            "R1(1): the causes that could not be recorded are flagged, not silently dropped");
        require(skipsLeft(f) == 1, "R1(1): exactly two attempts were made (they were counted although their causes were lost); no third round");
        require(f.inTransaction() && f.quarantined(), "R1(1): the real transaction is open and the health state is quarantined");
        requireOtherRepositoryRefused(f, "R1(1)");
    }
    requireOwnerRecovers(f, "rec-r1a-next", "R1(1)");
    require(f.segments() == 3 && f.segmentsWithId("result-rec-r1a") == 0 && f.recordsWithId("rec-r1a") == 0, "R1(1): the stuck write was rolled back");

    // (2) an exception in flight, and the destructor has to do the cleanup whose diagnostics also fail
    {
        detail::FaultPlan plan;
        plan.throwAfterBegin = true;
        plan.skipRollbackAttempts = 3;
        plan.throwWhileRecordingRollbackFailure = true;
        detail::ScopedConnectionFault fault(*f.database, plan);
        bool original = false;
        try {
            static_cast<void>(f.compaction->commitSuccessfulCompactionObserved(
                record("rec-r1b", ProjectMemoryCompactionStatus::Completed), segment("result-rec-r1b")));
        } catch (const std::runtime_error& error) {
            original = std::string(error.what()).find("exception after BEGIN") != std::string::npos;
        } catch (...) {
        }
        require(original, "R1(2): the ORIGINAL exception reaches the caller (it is neither replaced nor swallowed)");
        require(skipsLeft(f) == 1, "R1(2): the destructor used exactly two attempts");
        require(f.inTransaction() && f.quarantined(), "R1(2): the destructor did not release an open transaction unisolated");
        requireOtherRepositoryRefused(f, "R1(2)");
    }
    requireOwnerRecovers(f, "rec-r1b-next", "R1(2)");
    require(f.segments() == 3 && f.segmentsWithId("result-rec-r1b") == 0, "R1(2): the interrupted write left nothing");

    // (3) the explicit cleanup after a failed STEP (a stale source), not after COMMIT
    {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 3;
        plan.throwWhileRecordingRollbackFailure = true;
        detail::ScopedConnectionFault fault(*f.database, plan);
        CompactionPersistOutcome outcome;
        bool threw = false;
        try {
            outcome = f.compaction->commitSuccessfulCompactionObserved(
                record("rec-r1c", ProjectMemoryCompactionStatus::Completed, {"s1", "ghost"}), segment("result-rec-r1c"));
        } catch (...) {
            threw = true;
        }
        require(!threw, "R1(3): nothing throws out of the cleanup after a failed step");
        require(!outcome.result.success() && outcome.result.error()->code == "project_memory.compaction_conflict",
            "R1(3): the primary error (the stale source) is kept");
        require(outcome.state == CompactionPersistState::Unknown && outcome.connection == CompactionConnectionState::Quarantined
                && outcome.diagnosticsIncomplete,
            "R1(3): Unknown, isolated, diagnostics flagged incomplete");
        require(skipsLeft(f) == 1 && f.inTransaction() && f.quarantined(), "R1(3): two attempts, the transaction is open, the connection isolated");
        requireOtherRepositoryRefused(f, "R1(3)");
    }
    requireOwnerRecovers(f, "rec-r1c-next", "R1(3)");
    require(f.segments() == 3 && f.segmentsWithId("s1") == 1, "R1(3): the sources are back");
}

// R2(a). The native codes of the PRIMARY failure are kept in the outcome, captured before the rollback and the proof read.
void testThePrimaryNativeCodesSurviveTheCleanup()
{
    Fixture f;
    installDeferredCommitFailure(f);
    const auto committedOk = f.compaction->insertCompactionRecordObserved(record("rec-native-ok", ProjectMemoryCompactionStatus::Failed));
    require(committedOk.result.success() && !committedOk.primaryNative.has_value() && !committedOk.diagnosticsIncomplete,
        "a success carries no native failure");

    // a real COMMIT failure, the rollback works
    const auto commitFailure = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-native-dfk", ProjectMemoryCompactionStatus::Completed), segment("result-rec-native-dfk"));
    require(commitFailure.primaryNative.has_value() && commitFailure.primaryNative->code == SQLITE_CONSTRAINT
            && commitFailure.primaryNative->extendedCode == SQLITE_CONSTRAINT_FOREIGNKEY
            && commitFailure.primaryNative->message.find("FOREIGN KEY") != std::string::npos,
        "COMMIT: the numeric primary code is 19 / extended 787 (not what the ROLLBACK or the proof read left in the handle)");
    require(commitFailure.state == CompactionPersistState::NotCommitted && commitFailure.diagnostics.empty(),
        "COMMIT: and the rest of the outcome is the same as before");

    // the same failure, the rollback does NOT work: the later errors are different, the primary codes still are 787
    {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 3;
        detail::ScopedConnectionFault fault(*f.database, plan);
        const auto stuck = f.compaction->commitSuccessfulCompactionObserved(
            record("rec-native-stuck", ProjectMemoryCompactionStatus::Completed), segment("result-rec-native-stuck"));
        require(stuck.primaryNative.has_value() && stuck.primaryNative->extendedCode == SQLITE_CONSTRAINT_FOREIGNKEY
                && stuck.result.error()->code == "persistence.transaction_failed",
            "COMMIT with a failed rollback: the primary 787 stays apart from the cleanup causes");
        require(countDiagnostics(stuck, CompactionDiagnosticStage::Rollback) == 2, "...which are two separate rollback causes");
    }
    requireOwnerRecovers(f, "rec-native-recovered", "native codes");

    // other failed SQL steps
    require(rawExec(f.temp.file,
                "CREATE TRIGGER s2b_native_boom BEFORE INSERT ON project_memory_segments WHEN NEW.id = 'result-native-boom' "
                "BEGIN SELECT RAISE(ABORT, 'native boom'); END;")
                .empty(),
        "install the segment failure");
    const auto segmentFailure = f.compaction->commitSuccessfulCompactionObserved(
        record("native-boom", ProjectMemoryCompactionStatus::Completed), segment("result-native-boom"));
    require(segmentFailure.primaryNative.has_value() && segmentFailure.primaryNative->code == SQLITE_CONSTRAINT
            && segmentFailure.primaryNative->extendedCode == SQLITE_CONSTRAINT_TRIGGER
            && segmentFailure.primaryNative->message.find("native boom") != std::string::npos,
        "a failed INSERT STEP inside the commit: its codes and text are kept");
    const auto duplicate = f.compaction->insertCompactionRecordObserved(record("rec-native-ok", ProjectMemoryCompactionStatus::Failed));
    require(duplicate.primaryNative.has_value() && duplicate.primaryNative->extendedCode == SQLITE_CONSTRAINT_PRIMARYKEY
            && duplicate.result.error()->code == "project_memory.duplicate_id",
        "a duplicate id: the mapped error keeps the native primary-key code (1555)");
    const auto stale = f.compaction->commitSuccessfulCompactionObserved(
        record("rec-native-stale", ProjectMemoryCompactionStatus::Completed, {"s1", "ghost"}), segment("result-rec-native-stale"));
    require(!stale.primaryNative.has_value() && stale.result.error()->code == "project_memory.compaction_conflict",
        "a failure that did not come from SQLite has no native codes (none are invented)");

    // a refused BEGIN
    sqlite3* other = nullptr;
    require(sqlite3_open_v2(utf8(f.temp.file).c_str(), &other, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "second connection");
    require(sqlite3_exec(other, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "another writer holds the write lock");
    {
        detail::ScopedConnectionFault fault(*f.database, detail::FaultPlan {}, 0);
        const auto busy = f.compaction->commitSuccessfulCompactionObserved(
            record("rec-native-busy", ProjectMemoryCompactionStatus::Completed), segment("result-rec-native-busy"));
        require(busy.primaryNative.has_value() && busy.primaryNative->code == SQLITE_BUSY && busy.state == CompactionPersistState::NotCommitted,
            "a refused BEGIN: the BUSY code is kept");
    }
    sqlite3_exec(other, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close_v2(other);
}

// R2(b). The causes of a lazy recovery at the entry are kept on EVERY path, also when the new write works.
void testTheCausesOfAnEntryRecoveryAreKeptOnEveryPath()
{
    Fixture f;
    installSelectiveCommitFailure(f);
    const auto leaveStuck = [&](const char* id) {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 2;
        detail::ScopedConnectionFault fault(*f.database, plan);
        const auto stuck = f.compaction->commitSuccessfulCompactionObserved(
            record(id, ProjectMemoryCompactionStatus::Completed), segment(std::string("result-") + id));
        require(stuck.state == CompactionPersistState::Unknown && stuck.connection == CompactionConnectionState::Quarantined
                && f.inTransaction() && f.quarantined(),
            "setup: the owner's transaction is left open and the connection isolated");
    };
    const auto firstRollbackFailsThenWorks = [] {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 1;
        return plan;
    };
    const auto requireOneRecoveryNote = [&](const CompactionPersistOutcome& outcome, const std::string& what) {
        require(countDiagnostics(outcome, CompactionDiagnosticStage::Rollback) == 1
                    && hasDiagnostic(outcome, CompactionDiagnosticStage::Rollback, "persistence.rollback_failed"),
            what + ": the failed first rollback of the entry recovery is reported, once");
        require(!f.inTransaction() && !f.quarantined() && f.busyStatements() == 0, what + ": the recovery worked: no transaction, no isolation");
    };

    // (A) a new INSERT that works
    leaveStuck("bad-1");
    {
        detail::ScopedConnectionFault fault(*f.database, firstRollbackFailsThenWorks());
        const auto inserted = f.compaction->insertCompactionRecordObserved(record("rec-after-a", ProjectMemoryCompactionStatus::Failed));
        require(inserted.result.success() && inserted.state == CompactionPersistState::Committed
                && inserted.connection == CompactionConnectionState::Healthy && !inserted.primaryNative.has_value(),
            "(A) the new insert is committed");
        requireOneRecoveryNote(inserted, "(A) insert");
    }
    // (C) a new COMMIT that fails for its own reason: the entry's note AND the primary 787 both survive
    leaveStuck("bad-2");
    {
        detail::ScopedConnectionFault fault(*f.database, firstRollbackFailsThenWorks());
        const auto failed = f.compaction->commitSuccessfulCompactionObserved(
            record("bad-3", ProjectMemoryCompactionStatus::Completed), segment("result-bad-3"));
        require(!failed.result.success() && failed.result.error()->code == "persistence.transaction_failed"
                && failed.primaryNative.has_value() && failed.primaryNative->extendedCode == SQLITE_CONSTRAINT_FOREIGNKEY
                && failed.state == CompactionPersistState::NotCommitted && failed.connection == CompactionConnectionState::Healthy,
            "(C) the new commit fails with its own primary error and codes");
        requireOneRecoveryNote(failed, "(C) commit failure");
        require(f.segments() == 3 && f.recordsWithId("bad-2") == 0 && f.recordsWithId("bad-3") == 0, "(C) nothing of either write is stored");
    }
    // (D) a new INSERT that fails (a duplicate id): the entry's note comes BEFORE the proof's finding
    leaveStuck("bad-4");
    {
        detail::ScopedConnectionFault fault(*f.database, firstRollbackFailsThenWorks());
        const auto duplicate = f.compaction->insertCompactionRecordObserved(record("rec-after-a", ProjectMemoryCompactionStatus::Failed));
        require(!duplicate.result.success() && duplicate.result.error()->code == "project_memory.duplicate_id"
                && duplicate.state == CompactionPersistState::NotCommitted
                && hasDiagnostic(duplicate, CompactionDiagnosticStage::ProofRead, "project_memory.record_id_present"),
            "(D) the duplicate keeps its error and its proof finding");
        requireOneRecoveryNote(duplicate, "(D) insert failure");
        require(!duplicate.diagnostics.empty() && duplicate.diagnostics.front().stage == CompactionDiagnosticStage::Rollback,
            "(D) the causes are in the order they happened (the recovery first)");
    }
    // (B) a new COMMIT that works
    leaveStuck("bad-5");
    {
        detail::ScopedConnectionFault fault(*f.database, firstRollbackFailsThenWorks());
        const auto committed = f.compaction->commitSuccessfulCompactionObserved(
            record("good-1", ProjectMemoryCompactionStatus::Completed), segment("result-good-1"));
        require(committed.result.success() && committed.state == CompactionPersistState::Committed
                && committed.connection == CompactionConnectionState::Healthy,
            "(B) the new commit is committed");
        requireOneRecoveryNote(committed, "(B) commit");
        require(f.segmentsWithId("result-good-1") == 1 && f.segmentsWithId("s1") == 0 && f.recordsWithId("bad-5") == 0
                    && f.segmentsWithId("result-bad-5") == 0,
            "(B) only the new write is stored; the stuck one was rolled back");
    }
}

// The lifting of the isolation: a setter that does not work never turns into a healthy connection.
void testAFailedAuthorizerRemovalNeverDeclaresTheConnectionHealthy()
{
    Fixture f;
    installSelectiveCommitFailure(f);
    {
        detail::FaultPlan plan;
        plan.skipRollbackAttempts = 2;
        detail::ScopedConnectionFault fault(*f.database, plan);
        static_cast<void>(f.compaction->commitSuccessfulCompactionObserved(
            record("bad-lift", ProjectMemoryCompactionStatus::Completed), segment("result-bad-lift")));
        require(f.inTransaction() && f.quarantined(), "setup: stuck and isolated");
    }
    {
        detail::FaultPlan plan;
        plan.failAuthorizerRemovals = 1;
        detail::ScopedConnectionFault fault(*f.database, plan);
        // The owner's rollback works (the transaction is closed), but the authorizer cannot be removed.
        const auto refused = f.compaction->insertCompactionRecordObserved(record("rec-lift", ProjectMemoryCompactionStatus::Failed));
        require(!refused.result.success() && refused.result.error()->code == "persistence.connection_unusable",
            "the write is refused: the connection was not proven healthy");
        require(refused.state == CompactionPersistState::NotCommitted && refused.connection == CompactionConnectionState::Quarantined,
            "nothing of it was issued; the connection is still reported as isolated, never Healthy");
        require(hasDiagnostic(refused, CompactionDiagnosticStage::Rollback, "persistence.isolation_failed"),
            "the failed removal is a separate cause");
        require(f.quarantined() && !f.inTransaction(), "the health state still says isolated although the transaction was closed");
        require(skipsLeft(f) == 0, "(the plan's rollback skips are not involved)");
        require(f.recordsWithId("rec-lift") == 0, "no row was written");
        requireOtherRepositoryRefused(f, "after the failed removal");
    }
    // The transaction is closed already; only the lifting remains, and the next entry does it.
    requireOwnerRecovers(f, "rec-lift", "the next entry");
    SqliteProjectRepository projects(f.database);
    require(projects.insert(anotherProject(f)).success(), "after the lifting the other repository works again");
    require(f.recordsWithId("rec-lift") == 1 && f.segments() == 3, "only the later write is stored");
}

// R3. The identity lookup never repairs a damaged provenance list.
void testALookupRefusesDamagedSourceListsAndKeepsTheOrder()
{
    Fixture f;
    const auto original = record("rec-damaged", ProjectMemoryCompactionStatus::Failed, {"s2", "s1", "s3"});
    require(f.compaction->insertCompactionRecordObserved(original).result.success(), "insert");
    const auto exact = f.compaction->findCompactionRecord("rec-damaged");
    require(exact.success() && exact.value().has_value(), "a healthy row is found");
    requireSameRecord(*exact.value(), original, "the exact round trip");
    require(exact.value()->sourceSegmentIds == std::vector<std::string> {"s2", "s1", "s3"}, "the order of the sources is kept");

    const struct { const char* stored; const char* what; } damages[] = {
        {"[\"s1\",\"s2\",17]", "a non-string element (it used to be dropped: [s1,s2])"},
        {"not json", "invalid JSON (it used to read as an empty list)"},
        {"{\"a\":\"b\"}", "not an array"},
        {"[\"a\",null]", "a null element"},
        {"", "an empty text"},
        {"[\"a\"", "truncated JSON"},
        {"[[\"a\"]]", "a nested array"},
    };
    for (const auto& damage : damages) {
        require(rawExec(f.temp.file, (std::string("UPDATE project_memory_compaction_records SET source_segment_ids='")
                                      + damage.stored + "' WHERE id='rec-damaged';").c_str())
                    .empty(),
            "damage the row");
        const auto found = f.compaction->findCompactionRecord("rec-damaged");
        require(!found.success() && found.error()->code == "persistence.read_failed",
            std::string("lookup refuses ") + damage.what + " with persistence.read_failed (no repaired record, no empty success)");
    }
    require(rawExec(f.temp.file, "UPDATE project_memory_compaction_records SET source_segment_ids='[]' WHERE id='rec-damaged';").empty(), "restore");
    const auto emptyList = f.compaction->findCompactionRecord("rec-damaged");
    require(emptyList.success() && emptyList.value().has_value() && emptyList.value()->sourceSegmentIds.empty(),
        "a well-formed EMPTY list is a valid row");
}

// The version boundary: the client-data API exists, is wired to this connection, and the minimum is the documented one.
void testTheSqliteVersionBoundaryIsExplicit()
{
    require(detail::kMinimumSqliteVersionNumber == 3044000, "the documented minimum is SQLite 3.44.0");
    require(sqlite3_libversion_number() >= detail::kMinimumSqliteVersionNumber, "the linked SQLite is not older than the minimum");
    Fixture f;
    const auto handles = detail::ConnectionAccess::of(*f.database);
    std::lock_guard lock(*handles.mutex);
    require(detail::connectionHealthOf(handles.handle) == handles.health, "the health object is associated with its connection through client data");
}

} // namespace

int runSqliteCompactionWriteCertaintyTests()
{
    testTheDefaultObservedMethodsCallTheOldMethodOnceAndNeverClaimARollback();
    testObservedWritesRoundTripAndTheOldMethodsAreAThinView();
    testARealDeferredCommitFailureIsRolledBackAndProvenNotStored();
    testStepFailuresAndAStaleSourceGiveTheSameGuarantees();
    testAnExistingIdIsNeverTakenForANewSuccessfulSave();
    testABusyBeginIsNotCommittedAndTouchesNothingOfAnother();
    testAnUnreadableProofIsUnknown();
    testARefusedRollbackIsolatesTheConnectionForEveryRepositoryAndOnlyTheOwnerRecovers();
    testATransactionThatIsNotOursIsNeverRolledBackOrAdopted();
    testAnExceptionAfterBeginCleansUpWithoutReplacingItAndWithoutATerminate();
    testTheQuarantineBlocksStatementsPreparedBeforeItAndEachCycleRepeats();
    testTheDatabaseClosesSafelyAfterTheAuthorizerWasUsed();
    testTwoRepositoriesShareTheConnectionSeriallyThroughTheSameMutex();
    testACleanupWhoseDiagnosticsFailStillIsolatesTheConnectionAndKeepsTheBudget();
    testThePrimaryNativeCodesSurviveTheCleanup();
    testTheCausesOfAnEntryRecoveryAreKeptOnEveryPath();
    testAFailedAuthorizerRemovalNeverDeclaresTheConnectionHealthy();
    testALookupRefusesDamagedSourceListsAndKeepsTheOrder();
    testTheSqliteVersionBoundaryIsExplicit();
    std::cout << "SqliteCompactionWriteCertaintyTests: OK\n";
    return 0;
}
