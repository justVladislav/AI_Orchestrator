// B3-II-B: the owner-startup recovery of the compaction intents on REAL SQLite (a temporary file, WAL, synchronous=NORMAL), with the real
// intent port. No process is killed here (that is AIOrchestrator_CompactionIntentCrash_tests) and no BackendRuntime is built (that is
// CompactionIntentRecoveryRuntimeTests.cpp); what is checked is the recovery itself against real guarded SQL:
//  - open -> unresolved is durable (seen from a SEPARATE read-only connection), a second pass changes nothing and keeps markedAt;
//  - an Unknown mark / close is produced by a REAL sqlite3_wal_hook that reports an error AFTER the commit, and is confirmed only by
//    ONE pair read of the real database;
//  - a pair of rows the product's triggers never allow (an intent AND its final record) exists only through a local fixture that drops the
//    step's own triggers, inserts the record, and puts back the very SQL of the original triggers BEFORE the recovery runs;
//  - a really quarantined connection leaves the row unconfirmed;
//  - the records, the ledger and the segments are byte-for-byte equal before and after every pass.
// The checks use throwing require().
#include "SqliteConnectionHealth.hpp"
#include "SqliteFaultForTesting.hpp"

#include "ai/application/conversations/CompactionIntentRecovery.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace detail = ai::infrastructure::sqlite_detail;
namespace common = ai::common;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionIntentRepository;
using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
using ai::infrastructure::SqliteProjectMemoryRepository;
using namespace ai::application;
using St = CompactionIntentRecoveryStatus;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point at(std::int64_t ms) { return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)); }

const auto kInstant = at(7'000'000) + std::chrono::microseconds(321);   // the pass instant; the storage keeps 7'000'000
const auto kLater = at(8'000'000);

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

struct TempRoot {
    fs::path root;
    fs::path file;
    TempRoot()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path() / ("aio-b3iib-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                            + "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
        file = root / "state.sqlite3";
    }
    ~TempRoot()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

// A SECOND, read-only connection: what a restart would find.
struct ReadOnly {
    sqlite3* handle {nullptr};
    explicit ReadOnly(const fs::path& file)
    {
        require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "read-only open failed");
        sqlite3_busy_timeout(handle, 5000);
    }
    ~ReadOnly() { sqlite3_close_v2(handle); }
    ReadOnly(const ReadOnly&) = delete;
    ReadOnly& operator=(const ReadOnly&) = delete;

    long long scalar(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "read-only prepare failed: " + sql);
        long long value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }
    std::string text(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "read-only prepare failed: " + sql);
        std::string value = "<none>";
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* raw = sqlite3_column_text(statement, 0);
            value = raw != nullptr ? reinterpret_cast<const char*>(raw) : "<null>";
        }
        sqlite3_finalize(statement);
        return value;
    }
    // Every column of every row, in rowid order: a byte-for-byte picture of a table.
    std::string dump(const std::string& table) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, ("SELECT * FROM " + table + " ORDER BY rowid;").c_str(), -1, &statement, nullptr) == SQLITE_OK,
            "dump prepare failed: " + table);
        std::string out = table + "\n";
        while (sqlite3_step(statement) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                const unsigned char* raw = sqlite3_column_text(statement, i);
                out += raw != nullptr ? reinterpret_cast<const char*>(raw) : "<NULL>";
                out += '\x1f';
            }
            out += '\n';
        }
        sqlite3_finalize(statement);
        return out;
    }
};

// Records, ledger and segments: everything money- or summary-related. Intents are NOT in it on purpose (they are what recovery changes).
std::string financialSnapshot(const fs::path& file)
{
    ReadOnly db(file);
    return db.dump("project_memory_compaction_records") + db.dump("model_executions") + db.dump("project_memory_segments");
}

int failAfterCommit(void*, sqlite3*, const char*, int) { return SQLITE_IOERR; }

class CommitThenErrorHook {
public:
    explicit CommitThenErrorHook(SqliteDatabase& database) : handles_(detail::ConnectionAccess::of(database))
    {
        std::lock_guard lock(*handles_.mutex);
        sqlite3_wal_hook(handles_.handle, &failAfterCommit, nullptr);
    }
    ~CommitThenErrorHook()
    {
        std::lock_guard lock(*handles_.mutex);
        sqlite3_wal_autocheckpoint(handles_.handle, 1000);
    }
    CommitThenErrorHook(const CommitThenErrorHook&) = delete;
    CommitThenErrorHook& operator=(const CommitThenErrorHook&) = delete;

private:
    detail::ConnectionHandles handles_;
};

// The real intent port, every call counted (a decorator: it adds no behaviour), with one optional action before the first pair read.
class CountingPort final : public IProjectMemoryCompactionIntentRepository {
public:
    explicit CountingPort(IProjectMemoryCompactionIntentRepository& inner) : inner_(inner) {}
    int observeCalls {0};
    int markCalls {0};
    int closeCalls {0};
    // The returned mutation of the LAST mark / close, exactly as the real port gave it: the reference for "the native detail was kept".
    CompactionIntentMutationOutcome lastMark;
    CompactionIntentMutationOutcome lastClose;
    std::function<void()> beforeFirstObserve;

    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent& intent) override { return inner_.recordIntentObserved(intent); }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId& id) const override
    {
        return inner_.findIntent(id);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId& project, std::optional<CompactionIntentState> state) const override
    {
        return inner_.listIntents(project, state);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState state) const override
    {
        return inner_.listIntentsByState(state);
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(const CompactionIntentIdentity& e, std::chrono::system_clock::time_point t) override
    {
        ++markCalls;
        lastMark = inner_.markUnresolvedObserved(e, t);
        return lastMark;
    }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity& e) override
    {
        ++closeCalls;
        lastClose = inner_.closeIntentObserved(e);
        return lastClose;
    }
    [[nodiscard]] CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity& e) const override
    {
        if (observeCalls == 0 && beforeFirstObserve) beforeFirstObserve();
        ++const_cast<CountingPort*>(this)->observeCalls;
        return inner_.observeOperation(e);
    }

private:
    IProjectMemoryCompactionIntentRepository& inner_;
};

// R1a on REAL returned mutations: whatever native detail the real port attached to its Unknown write is in the report, bound to the same
// error. No fixed SQLite code is assumed; the reference is the mutation the decorator saw.
void requireWriteNativeKept(const CompactionIntentRecoveryReport& report, const CompactionIntentMutationOutcome& returned, const std::string& what)
{
    require(!returned.persist.result.success() && returned.persist.state != CompactionPersistState::Committed,
        what + ": the real write reported an error and no confirmed commit");
    require(returned.persist.primaryNative.has_value(),
        what + ": the real port attached a native detail to its Unknown write (so there is something to keep)");
    require(report.writeCauses.size() == 1, what + ": exactly one native cause");
    const auto& kept = report.writeCauses[0];
    require(kept.cause.code == returned.persist.result.error()->code && kept.cause.message == returned.persist.result.error()->message
                && kept.native.code == returned.persist.primaryNative->code
                && kept.native.extendedCode == returned.persist.primaryNative->extendedCode
                && kept.native.message == returned.persist.primaryNative->message,
        what + ": code, extendedCode and message of the native detail are those of the returned mutation, bound to its error");
    require(!report.firstError.has_value() && !report.firstNative.has_value(), what + ": a confirmed change is not the first error");
}

int anomalyCauses(const CompactionIntentRecoveryReport& report)
{
    int count = 0;
    for (const auto& entry : report.diagnostics) {
        if (entry.stage == CompactionDiagnosticStage::IntentRecovery && entry.error.code == kIntentObservationInconsistentCode
            && entry.error.message.rfind("The intent and a final record with the expected identity were observed together", 0) == 0) {
            ++count;
        }
    }
    return count;
}

ProjectMemoryCompactionIntent makeIntent(const std::string& id)
{
    ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = "p1";
    value.providerId = "provider-1";
    value.modelId = "model-1";
    value.sourceSegmentIds = {"s1", "s2"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = at(2000);
    return value;
}

ProjectMemoryCompactionRecord recordOf(const ProjectMemoryCompactionIntent& intent)
{
    ProjectMemoryCompactionRecord value;
    value.id = intent.id;
    value.projectId = intent.projectId;
    value.providerId = intent.providerId;
    value.modelId = intent.modelId;
    value.sourceSegmentIds = intent.sourceSegmentIds;
    value.sourceContentHash = intent.sourceContentHash;
    value.createdAt = intent.createdAt;
    value.inputTokens = 40;
    value.outputTokens = 15;
    value.costMicros = 425;
    value.costPriceKnown = true;
    value.status = ProjectMemoryCompactionStatus::Failed;
    value.errorCode = "provider.failed";
    value.errorMessage = "Compaction provider request failed";
    return value;
}

struct Rig {
    TempRoot temp;
    std::shared_ptr<SqliteDatabase> database;
    std::unique_ptr<SqliteProjectMemoryRepository> memory;
    std::unique_ptr<SqliteProjectMemoryCompactionRepository> records;
    std::unique_ptr<SqliteProjectMemoryCompactionIntentRepository> intents;

    Rig()
    {
        auto opened = SqliteDatabase::open(temp.file);
        require(opened.success(), "could not open the database");
        database = std::move(opened.value());
        memory = std::make_unique<SqliteProjectMemoryRepository>(database);
        records = std::make_unique<SqliteProjectMemoryCompactionRepository>(database);
        intents = std::make_unique<SqliteProjectMemoryCompactionIntentRepository>(database);
        require(exec("INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                     "VALUES('p1','One','d','C:/fixture/1','active',1,1);")
                    .empty(),
            "seed the project");
        for (const char* id : {"s1", "s2", "s3"}) {
            ProjectMemorySegment segment;
            segment.id = id;
            segment.projectId = "p1";
            segment.runId = std::string("run-") + id;
            segment.summary = std::string("Summary of ") + id;
            segment.sourceTokenCount = 11;
            segment.summaryTokenCount = 7;
            segment.createdAt = at(1000);
            require(memory->insertMemorySegment(segment).success(), "seed a segment");
        }
        // The process-crash guarantee this feature relies on: WAL with synchronous=NORMAL. Checked on every new database.
        ReadOnly probe(temp.file);
        require(probe.text("PRAGMA journal_mode;") == "wal", "journal_mode is wal");
        require(scalarOnShared("PRAGMA synchronous;") == 1, "synchronous is NORMAL (1)");
    }
    ~Rig()
    {
        intents.reset();
        records.reset();
        memory.reset();
        database.reset();
    }

    std::string exec(const std::string& sql) const
    {
        const auto h = detail::ConnectionAccess::of(*database);
        std::lock_guard lock(*h.mutex);
        char* message = nullptr;
        const int rc = sqlite3_exec(h.handle, sql.c_str(), nullptr, nullptr, &message);
        std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
        sqlite3_free(message);
        return text;
    }
    long long scalarOnShared(const std::string& sql) const
    {
        const auto h = detail::ConnectionAccess::of(*database);
        std::lock_guard lock(*h.mutex);
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(h.handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare failed: " + sql);
        long long value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    ProjectMemoryCompactionIntent admit(const std::string& id)
    {
        const ProjectMemoryCompactionIntent intent = makeIntent(id);
        const auto written = intents->recordIntentObserved(intent);
        require(mayDispatch(written), "setup: the intent is admitted");
        return intent;
    }

    // A pair the product's own triggers never allow: the intent AND a final record with the same id. The record goes in while the step's
    // three triggers are dropped; the ORIGINAL SQL of every trigger is then put back, so the recovery runs against the real guards.
    void makePair(const ProjectMemoryCompactionIntent& /*intent: already admitted*/, const ProjectMemoryCompactionRecord& record)
    {
        std::vector<std::string> definitions;
        for (const char* name : {"project_memory_compaction_intents_no_used_id", "project_memory_compaction_records_intent_guard",
                 "project_memory_compaction_records_intent_close"}) {
            ReadOnly db(temp.file);
            definitions.push_back(db.text(std::string("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='") + name + "';"));
            require(definitions.back() != "<none>" && definitions.back() != "<null>", "the original trigger exists");
        }
        require(exec("DROP TRIGGER project_memory_compaction_intents_no_used_id;"
                     "DROP TRIGGER project_memory_compaction_records_intent_guard;"
                     "DROP TRIGGER project_memory_compaction_records_intent_close;")
                    .empty(),
            "fixture: drop the guards");
        const auto inserted = records->insertCompactionRecordObserved(record);
        require(inserted.result.success() && inserted.state == CompactionPersistState::Committed, "fixture: the record is inserted");
        for (const auto& sql : definitions) require(exec(sql + ";").empty(), "fixture: the original trigger is restored");
        ReadOnly db(temp.file);
        require(db.scalar("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name LIKE 'project_memory_compaction_%';") == 3,
            "all three guards are back before the recovery runs");
        require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents;") == 1
                    && db.scalar("SELECT count(*) FROM project_memory_compaction_records;") == 1,
            "the pair exists: one intent and one record");
    }

    CompactionIntentRecoveryReport recover(IProjectMemoryCompactionIntentRepository& port, std::chrono::system_clock::time_point when = kInstant)
    {
        CompactionIntentRecovery recovery(port, CompactionIntentRecoveryPermit([] { return true; }), [when] { return when; });
        return recovery.run();
    }
};

// ---- 1. durable open -> unresolved, then stable -------------------------------------------------------------------------------------
void testAnOpenIntentBecomesUnresolvedDurablyAndASecondPassChangesNothing()
{
    Rig rig;
    rig.admit("op-1");
    const std::string moneyBefore = financialSnapshot(rig.temp.file);
    CountingPort port(*rig.intents);

    const auto first = rig.recover(port);
    require(first.status == St::Complete && first.inspected == 1 && first.markedUnresolved == 1 && first.alreadyUnresolved == 0
                && first.unconfirmed == 0 && first.conflicts == 0 && !first.firstError.has_value(),
        "the first pass marks the open intent and is Complete");
    {
        ReadOnly db(rig.temp.file);
        require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents WHERE id='op-1' AND state='unresolved' "
                          "AND unresolved_marked_at_ms=7000000;") == 1,
            "TRUTH from a separate connection: unresolved, marked at the pass instant cut to milliseconds");
        require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents;") == 1, "the intent was NOT deleted");
    }
    require(financialSnapshot(rig.temp.file) == moneyBefore, "records, ledger and segments are byte-for-byte unchanged");

    const auto second = rig.recover(port, kLater);
    require(second.status == St::Complete && second.inspected == 1 && second.markedUnresolved == 0 && second.alreadyUnresolved == 1,
        "the second pass finds it already unresolved");
    require(port.markCalls == 1, "...and does not even try to mark again");
    {
        ReadOnly db(rig.temp.file);
        require(db.scalar("SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents WHERE id='op-1';") == 7000000,
            "markedAt is stable");
    }
    require(financialSnapshot(rig.temp.file) == moneyBefore, "and the financial tables are still equal");
}

// ---- 2. a REAL Unknown: committed, but an error was reported -------------------------------------------------------------------------
void testAMarkThatCommittedButReportedAnErrorIsConfirmedByOneRealProof()
{
    Rig rig;
    rig.admit("op-1");
    const std::string moneyBefore = financialSnapshot(rig.temp.file);
    CountingPort port(*rig.intents);
    CompactionIntentRecoveryReport report;
    {
        CommitThenErrorHook hook(*rig.database);   // the commit happens; the error arrives after it; no SQL runs inside the hook
        report = rig.recover(port);
    }
    require(port.markCalls == 1 && port.observeCalls == 2, "ONE mark (no retry), the first read and ONE confirming read");
    {
        ReadOnly db(rig.temp.file);
        require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents WHERE state='unresolved' AND unresolved_marked_at_ms=7000000;") == 1,
            "TRUTH: the mark IS committed");
    }
    require(report.status == St::Complete && report.markedUnresolved == 1 && report.unconfirmed == 0,
        "the one proof read shows exactly the expected transition: counted, Complete");
    bool writeErrorKept = false;
    for (const auto& entry : report.diagnostics) {
        if (entry.stage == CompactionDiagnosticStage::IntentRecovery) writeErrorKept = true;
    }
    require(writeErrorKept && !report.firstError.has_value(), "the write's own error stays a cause; a confirmed change is not an error");
    requireWriteNativeKept(report, port.lastMark, "Unknown mark");
    require(financialSnapshot(rig.temp.file) == moneyBefore, "no financial row changed");
}

// ---- 3. a pair, by a local fixture --------------------------------------------------------------------------------------------------------
void testAMatchingPairIsClosedAndAnUnknownCloseIsConfirmedByOneRealProof()
{
    for (int withHook = 0; withHook < 2; ++withHook) {
        Rig rig;
        const auto intent = rig.admit("op-1");
        rig.makePair(intent, recordOf(intent));
        const std::string moneyBefore = financialSnapshot(rig.temp.file);
        CountingPort port(*rig.intents);
        CompactionIntentRecoveryReport report;
        if (withHook == 0) {
            report = rig.recover(port);
        } else {
            CommitThenErrorHook hook(*rig.database);
            report = rig.recover(port);
        }
        const std::string what = withHook == 0 ? "plain close" : "close that committed but reported an error";
        require(report.status == St::Complete && report.closedConfirmed == 1 && report.markedUnresolved == 0 && report.conflicts == 0,
            what + ": the matching pair is closed and confirmed");
        require(anomalyCauses(report) == 1, what + ": the 'intent and matching record observed together' fact is kept exactly once");
        if (withHook == 1) {
            requireWriteNativeKept(report, port.lastClose, what);
        } else {
            require(report.writeCauses.empty(), what + ": a plain close has no write error");
        }
        require(port.closeCalls == 1 && port.markCalls == 0 && port.observeCalls == (withHook == 0 ? 1 : 2),
            what + ": one guarded close; the Unknown one has exactly one confirming read");
        ReadOnly db(rig.temp.file);
        require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents;") == 0 && db.scalar("SELECT count(*) FROM project_memory_compaction_records;") == 1,
            what + ": TRUTH: the intent is gone, the record is untouched");
        require(financialSnapshot(rig.temp.file) == moneyBefore, what + ": byte-for-byte the same financial tables");
    }
}

void testAMismatchingPairIsAConflictAndNothingIsChanged()
{
    Rig rig;
    const auto intent = rig.admit("op-1");
    ProjectMemoryCompactionRecord other = recordOf(intent);
    other.sourceContentHash = std::string(64, 'b');   // the record under the id is a DIFFERENT operation
    rig.makePair(intent, other);
    const std::string moneyBefore = financialSnapshot(rig.temp.file);
    std::string intentsBefore;
    {
        ReadOnly db(rig.temp.file);
        intentsBefore = db.dump("project_memory_compaction_intents");
    }
    CountingPort port(*rig.intents);
    const auto report = rig.recover(port);
    require(report.status == St::Partial && report.conflicts == 1 && report.closedConfirmed == 0 && report.markedUnresolved == 0,
        "a pair with a differing identity is a conflict, Partial");
    require(report.firstError.has_value() && report.firstError->code == kIntentRecordConflictCode
                && report.firstError->message.find("source_content_hash") != std::string::npos,
        "intent_record_conflict naming the field");
    require(port.markCalls == 0 && port.closeCalls == 0, "nothing was asked to change");
    ReadOnly db(rig.temp.file);
    require(db.dump("project_memory_compaction_intents") == intentsBefore && financialSnapshot(rig.temp.file) == moneyBefore,
        "the intent and the record are byte-for-byte unchanged");
}

// ---- 4. a really quarantined connection ----------------------------------------------------------------------------------------------------
void testAQuarantinedConnectionLeavesTheRowUnconfirmedAndUnchanged()
{
    Rig rig;
    rig.admit("op-1");
    const std::string moneyBefore = financialSnapshot(rig.temp.file);
    CountingPort port(*rig.intents);
    const auto h = detail::ConnectionAccess::of(*rig.database);
    // The lists are read on a healthy connection; right before the first pair read the connection becomes REALLY unusable.
    port.beforeFirstObserve = [&] {
        std::lock_guard lock(*h.mutex);
        require(h.health->enterQuarantine(h.handle, detail::QuarantineReason::ForeignOpenTransaction).success(), "quarantine");
    };
    const auto report = rig.recover(port);
    {
        std::lock_guard lock(*h.mutex);
        require(h.health->release(h.handle).success(), "release");
    }
    require(report.status == St::Partial && report.unconfirmed == 1 && report.markedUnresolved == 0 && report.conflicts == 0,
        "unconfirmed, Partial");
    require(report.firstError.has_value() && report.firstError->code == "persistence.connection_unusable",
        "the original refusal of the port is the first error");
    require(port.markCalls == 0 && port.closeCalls == 0 && port.observeCalls == 1, "nothing was written and nothing was retried");
    ReadOnly db(rig.temp.file);
    require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents WHERE state='open';") == 1, "the intent is still open");
    require(financialSnapshot(rig.temp.file) == moneyBefore, "financial tables unchanged");

    // After the ordinary restoration the next pass marks it.
    CountingPort healthy(*rig.intents);
    const auto retry = rig.recover(healthy);
    require(retry.status == St::Complete && retry.markedUnresolved == 1, "the next start finishes the job");
}

} // namespace

int runCompactionIntentRecoverySqliteTests()
{
    try {
        testAnOpenIntentBecomesUnresolvedDurablyAndASecondPassChangesNothing();
        testAMarkThatCommittedButReportedAnErrorIsConfirmedByOneRealProof();
        testAMatchingPairIsClosedAndAnUnknownCloseIsConfirmedByOneRealProof();
        testAMismatchingPairIsAConflictAndNothingIsChanged();
        testAQuarantinedConnectionLeavesTheRowUnconfirmedAndUnchanged();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentRecoverySqliteTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentRecoverySqliteTests: OK\n";
    return 0;
}
