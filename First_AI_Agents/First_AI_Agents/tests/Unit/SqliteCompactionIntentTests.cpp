// B3 phase I: the SQLite storage of the compaction intent - admission, the guarded closure by the final record, the guarded state
// changes, the one proof operation and the strict decoding - on REAL SQLite.
//
// Every database is a temporary file or ":memory:" (the logic is the same; ":memory:" promises no restart durability and its restart
// behaviour is not tested). Fixtures that need a state the product never produces (both an intent and a record under one id) are made
// by dropping this step's own triggers in the temporary database first. Faults are the closed, per-connection seams of
// SqliteFaultForTesting.hpp plus a scoped sqlite3_wal_hook (which reports an error AFTER the commit happened, without running SQL).
//
// The checks use throwing require(), so they stay active under NDEBUG.
#include "SqliteCompactionIntentMigration.hpp"
#include "SqliteConnectionHealth.hpp"
#include "SqliteFaultForTesting.hpp"
#include "SqlitePrimitives.hpp"
#include "conversations/SqliteCompactionIdentityCodec.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace detail = ai::infrastructure::sqlite_detail;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionIntentRepository;
using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
using ai::infrastructure::SqliteProjectMemoryRepository;
using namespace ai::application;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point at(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

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
        root = fs::temp_directory_path() / ("aio-b3i-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
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

// A SECOND, read-only connection to the file: what a restart would find. The shared connection's own view proves nothing about durability.
long long readonlyScalar(const fs::path& file, const std::string& sql)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "read-only open failed");
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "read-only prepare failed: " + sql);
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
    value.summary = "Summary of " + id;
    value.sourceTokenCount = 11;
    value.summaryTokenCount = 7;
    value.createdAt = at(1000);
    return value;
}

ProjectMemoryCompactionIntent makeIntent(const std::string& id = "op-1")
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

ProjectMemoryCompactionRecord recordOf(const ProjectMemoryCompactionIntent& intent, ProjectMemoryCompactionStatus status)
{
    ProjectMemoryCompactionRecord value;
    value.id = intent.id;
    value.projectId = intent.projectId;
    value.providerId = intent.providerId;
    value.modelId = intent.modelId;
    value.sourceSegmentIds = intent.sourceSegmentIds;
    value.sourceContentHash = intent.sourceContentHash;
    value.createdAt = intent.createdAt;
    value.inputTokens = 1234;
    value.outputTokens = 56;
    value.costMicros = 789;
    value.costPriceKnown = false;
    value.status = status;
    if (status == ProjectMemoryCompactionStatus::Completed) {
        value.resultSegmentId = "result-" + intent.id;
    } else {
        value.errorCode = "project_memory.empty_summary";
        value.errorMessage = "Provider returned an empty compacted summary";
    }
    return value;
}

ProjectMemorySegment resultSegmentOf(const ProjectMemoryCompactionRecord& record)
{
    ProjectMemorySegment value = segment(*record.resultSegmentId, record.projectId);
    value.createdAt = record.createdAt;
    return value;
}

enum class WritePath { Completed, Failed, Cancelled, FallbackFailed };

constexpr WritePath kAllPaths[] = {WritePath::Completed, WritePath::Failed, WritePath::Cancelled, WritePath::FallbackFailed};

const char* pathName(WritePath path)
{
    switch (path) {
    case WritePath::Completed: return "primary Completed";
    case WritePath::Failed: return "terminal Failed";
    case WritePath::Cancelled: return "terminal Cancelled";
    case WritePath::FallbackFailed: return "fallback Failed";
    }
    return "?";
}

struct Db {
    TempRoot temp;
    bool memory;
    std::shared_ptr<SqliteDatabase> database;
    std::unique_ptr<SqliteProjectMemoryRepository> memoryRepo;
    std::unique_ptr<SqliteProjectMemoryCompactionRepository> records;
    std::unique_ptr<SqliteProjectMemoryCompactionIntentRepository> intents;

    explicit Db(bool inMemory) : memory(inMemory)
    {
        open();
        require(exec("INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                     "VALUES('p1','One','d','C:/fixture/1','active',1,1),('p2','Two','d','C:/fixture/2','active',1,1);")
                    .empty(),
            "could not seed the projects");
        for (const char* id : {"s1", "s2", "s3"}) require(memoryRepo->insertMemorySegment(segment(id)).success(), "could not seed a segment");
    }

    void open()
    {
        intents.reset();
        records.reset();
        memoryRepo.reset();
        database.reset();
        auto opened = SqliteDatabase::open(memory ? fs::path(":memory:") : temp.file);
        require(opened.success(), "could not open the database");
        database = std::move(opened.value());
        memoryRepo = std::make_unique<SqliteProjectMemoryRepository>(database);
        records = std::make_unique<SqliteProjectMemoryCompactionRepository>(database);
        intents = std::make_unique<SqliteProjectMemoryCompactionIntentRepository>(database);
    }

    detail::ConnectionHandles handles() const { return detail::ConnectionAccess::of(*database); }

    // SQL on the SHARED connection (the only one that sees a ":memory:" database); "" on success.
    std::string exec(const std::string& sql) const
    {
        const auto h = handles();
        std::lock_guard lock(*h.mutex);
        char* message = nullptr;
        const int rc = sqlite3_exec(h.handle, sql.c_str(), nullptr, nullptr, &message);
        std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
        sqlite3_free(message);
        return text;
    }

    long long scalar(const std::string& sql) const
    {
        const auto h = handles();
        std::lock_guard lock(*h.mutex);
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(h.handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare failed: " + sql);
        long long value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    std::string text(const std::string& sql) const
    {
        const auto h = handles();
        std::lock_guard lock(*h.mutex);
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(h.handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare failed: " + sql);
        std::string value = "<none>";
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* raw = sqlite3_column_text(statement, 0);
            value = raw != nullptr ? reinterpret_cast<const char*>(raw) : "<null>";
        }
        sqlite3_finalize(statement);
        return value;
    }

    long long intentCount() const { return scalar("SELECT count(*) FROM project_memory_compaction_intents;"); }
    long long recordCount() const { return scalar("SELECT count(*) FROM project_memory_compaction_records;"); }
    long long segmentCount() const { return scalar("SELECT count(*) FROM project_memory_segments;"); }
    bool inTransaction() const
    {
        const auto h = handles();
        std::lock_guard lock(*h.mutex);
        return sqlite3_get_autocommit(h.handle) == 0;
    }

    // Both rows may coexist only in a database whose guards were removed: this makes that (never produced) state for a test.
    void dropGuards() const
    {
        require(exec("DROP TRIGGER project_memory_compaction_intents_no_used_id;"
                     "DROP TRIGGER project_memory_compaction_records_intent_guard;"
                     "DROP TRIGGER project_memory_compaction_records_intent_close;")
                    .empty(),
            "could not drop the guards");
    }

    CompactionPersistOutcome write(const ProjectMemoryCompactionRecord& record, WritePath path) const
    {
        if (path == WritePath::Completed) return records->commitSuccessfulCompactionObserved(record, resultSegmentOf(record));
        return records->insertCompactionRecordObserved(record);
    }

    ProjectMemoryCompactionRecord recordFor(const ProjectMemoryCompactionIntent& intent, WritePath path) const
    {
        switch (path) {
        case WritePath::Completed: return recordOf(intent, ProjectMemoryCompactionStatus::Completed);
        case WritePath::Failed: return recordOf(intent, ProjectMemoryCompactionStatus::Failed);
        case WritePath::Cancelled: return recordOf(intent, ProjectMemoryCompactionStatus::Cancelled);
        case WritePath::FallbackFailed:
            return compaction_accounting::makeFailedAccountingRecord(
                recordOf(intent, ProjectMemoryCompactionStatus::Completed), ai::common::Error {"persistence.transaction_failed", "x"});
        }
        return {};
    }
};

void requireAdmitted(const CompactionIntentWriteOutcome& outcome, const std::string& what)
{
    require(outcome.persist.result.success() && outcome.persist.state == CompactionPersistState::Committed && outcome.newIntentInserted &&
            mayDispatch(outcome) && outcome.persist.connection == CompactionConnectionState::Healthy,
        what + ": a new intent must be admitted (Committed, new row, Healthy)");
}

void requireRefused(const CompactionIntentWriteOutcome& outcome, const std::string& code, const std::string& what)
{
    require(!mayDispatch(outcome) && !outcome.newIntentInserted && !outcome.persist.result.success() &&
            outcome.persist.result.error()->code == code,
        what + ": must be refused with " + code + (outcome.persist.result.success() ? "" : " (got " + outcome.persist.result.error()->code + ")"));
}

// The seven fields, one at a time, applied to an INTENT (the record keeps the baseline).
struct FieldVariant {
    CompactionIntentIdentityField field;
    std::function<void(ProjectMemoryCompactionIntent&)> apply;
};

std::vector<FieldVariant> mismatchingVariants()
{
    return {
        {CompactionIntentIdentityField::ProjectId, [](ProjectMemoryCompactionIntent& i) { i.projectId = "p2"; }},
        {CompactionIntentIdentityField::ProviderId, [](ProjectMemoryCompactionIntent& i) { i.providerId = "provider-2"; }},
        {CompactionIntentIdentityField::ModelId, [](ProjectMemoryCompactionIntent& i) { i.modelId = "model-2"; }},
        {CompactionIntentIdentityField::SourceSegmentIds, [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {"s2", "s1"}; }},
        {CompactionIntentIdentityField::SourceContentHash, [](ProjectMemoryCompactionIntent& i) { i.sourceContentHash = std::string(64, 'b'); }},
        {CompactionIntentIdentityField::CreatedAt, [](ProjectMemoryCompactionIntent& i) { i.createdAt = at(2001); }},
    };
}

// ---- schema and decoding -------------------------------------------------------------------------------------------------------

void testANewDatabaseHasExactlyTheStep75Objects(bool memory)
{
    Db db(memory);
    require(db.scalar("PRAGMA user_version;") == 75, "a new database is at version 75");
    const detail::CompactionIntentSchemaObject* objects = detail::compactionIntentSchemaObjects();
    for (int i = 0; i < detail::kCompactionIntentSchemaObjectCount; ++i) {
        const auto& object = objects[i];
        require(db.scalar(std::string("SELECT count(*) FROM sqlite_master WHERE type='") + object.type + "' AND name='" + object.name
                    + "' AND tbl_name='" + object.table + "';") == 1,
            std::string("the object ") + object.name + " exists on its table");
        const auto stored = detail::normalizeSchemaSqlForComparison(
            db.text(std::string("SELECT sql FROM sqlite_master WHERE name='") + object.name + "';"));
        require(stored.has_value() && stored == detail::normalizeSchemaSqlForComparison(object.sql),
            std::string("the stored SQL of ") + object.name + " is exactly the step's own");
    }
    require(db.intentCount() == 0, "a new database holds no intent");
    // The other columns' semantics are the table's own CHECKs.
    require(!db.exec("INSERT INTO project_memory_compaction_intents VALUES('x','p1','a','b','[]','h',1,'open',5);").empty(),
        "an open intent cannot carry a mark");
    require(!db.exec("INSERT INTO project_memory_compaction_intents VALUES('x','p1','a','b','[]','h',1,'unresolved',NULL);").empty(),
        "an unresolved intent must carry a mark");
    require(!db.exec("INSERT INTO project_memory_compaction_intents VALUES('x','p1','a','b','[]','h',1,'closed',NULL);").empty(),
        "only open and unresolved exist");
    require(!db.exec("INSERT INTO project_memory_compaction_intents VALUES('x','p1','a','b','[]',NULL,1,'open',NULL);").empty(),
        "no column of the identity may be NULL");
    if (!memory) {
        require(db.text("PRAGMA journal_mode;") == "wal", "the real journal mode of a new file database is WAL");
        require(db.scalar("PRAGMA synchronous;") == 1, "the real synchronous level is NORMAL (1)");
    }
}

void testARoundTripKeepsEveryFieldInOrder(bool memory)
{
    Db db(memory);
    ProjectMemoryCompactionIntent first = makeIntent("op-\xE6\xB8\xAC");   // a UTF-8 id
    first.sourceSegmentIds = {"s2", "s1", "s2", "\xD0\xA1"};               // order and a duplicate
    first.createdAt = at(3000);
    ProjectMemoryCompactionIntent second = makeIntent("op-b");
    second.createdAt = at(1000);
    ProjectMemoryCompactionIntent sameTime = makeIntent("op-a");
    sameTime.createdAt = at(3000);
    ProjectMemoryCompactionIntent other = makeIntent("op-other");
    other.projectId = "p2";
    other.providerId = "no-such-provider";   // provider and model are plain text: no foreign key
    other.modelId = "no-such-model";
    for (const auto* intent : {&first, &second, &sameTime, &other}) requireAdmitted(db.intents->recordIntentObserved(*intent), "admit " + intent->id);

    const auto found = db.intents->findIntent(first.id);
    require(found.success() && found.value().has_value(), "the intent is found by its id");
    const ProjectMemoryCompactionIntent& stored = *found.value();
    require(stored.id == first.id && stored.projectId == first.projectId && stored.providerId == first.providerId &&
            stored.modelId == first.modelId && stored.sourceSegmentIds == first.sourceSegmentIds &&
            stored.sourceContentHash == first.sourceContentHash && stored.createdAt == first.createdAt &&
            stored.state == CompactionIntentState::Open && !stored.unresolvedMarkedAt.has_value(),
        "every field round-trips, the sources in order with their duplicate");
    const auto missing = db.intents->findIntent("nope");
    require(missing.success() && !missing.value().has_value(), "an unknown id is success without a value");

    const auto listed = db.intents->listIntents("p1", std::nullopt);
    require(listed.success() && listed.value().size() == 3 && listed.value()[0].id == "op-b" && listed.value()[1].id == "op-a" &&
            listed.value()[2].id == first.id,
        "the intents of a project are ordered by createdAt then id");
    const auto openOnly = db.intents->listIntents("p1", CompactionIntentState::Open);
    const auto unresolvedOnly = db.intents->listIntents("p1", CompactionIntentState::Unresolved);
    require(openOnly.success() && openOnly.value().size() == 3 && unresolvedOnly.success() && unresolvedOnly.value().empty(),
        "the state filter works");
    const auto byState = db.intents->listIntentsByState(CompactionIntentState::Open);
    require(byState.success() && byState.value().size() == 4, "listing by state spans projects");
}

void testDamagedRowsAreAReadFailureNeverRepaired(bool memory)
{
    Db db(memory);
    // Noncanonical or malformed lists, and a non-integer time: each is refused by find, list and the proof; none is skipped or fixed.
    const char* damagedLists[] = {"[ \"s1\", \"s2\" ]", "{\"a\":1}", "[\"s1\",2]", "not json", "[\"s1\",\"s2\"] ", "[\"\\u0073\"]"};
    int n = 0;
    for (const char* list : damagedLists) {
        const std::string id = "damaged-" + std::to_string(n++);
        require(db.exec("INSERT INTO project_memory_compaction_intents VALUES('" + id + "','p1','a','b','" + std::string(list)
                        + "','h',1,'open',NULL);").empty(), "plant a damaged row");
        const auto one = db.intents->findIntent(id);
        require(!one.success() && one.error()->code == "persistence.read_failed", std::string("find refuses: ") + list);
        CompactionIntentIdentity expected;
        expected.id = id;
        const auto observed = db.intents->observeOperation(expected);
        require(observed.observation == CompactionIntentObservation::Unreadable && observed.error.has_value() &&
                observed.error->code == "persistence.read_failed" && observed.source == CompactionIntentObservationSource::None,
            std::string("the proof is Unreadable: ") + list);
    }
    const auto listed = db.intents->listIntentsByState(CompactionIntentState::Open);
    require(!listed.success() && listed.error()->code == "persistence.read_failed", "a listing with one damaged row fails as a whole");
    require(db.exec("DELETE FROM project_memory_compaction_intents;").empty(), "clean");
    require(db.exec("INSERT INTO project_memory_compaction_intents VALUES('t','p1','a','b','[]','h','not-a-time','open',NULL);").empty(),
        "plant a text time");
    const auto text = db.intents->findIntent("t");
    require(!text.success() && text.error()->code == "persistence.read_failed", "a non-integer createdAt is a damaged row");
}

// ---- admission --------------------------------------------------------------------------------------------------------------------

void testAdmissionIsOnlyAConfirmedNewInsert(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    requireAdmitted(db.intents->recordIntentObserved(intent), "the first insert");
    require(db.intentCount() == 1, "one row");

    // The very same intent again: a duplicate, never a second admission - also for identical content.
    const auto again = db.intents->recordIntentObserved(intent);
    requireRefused(again, kIntentDuplicateIdCode, "an identical repeat");
    require(again.persist.state == CompactionPersistState::NotCommitted && again.persist.primaryNative.has_value() &&
            again.persist.primaryNative->extendedCode == SQLITE_CONSTRAINT_PRIMARYKEY,
        "...proven not stored, with the native primary-key code kept");
    require(db.intentCount() == 1, "...and no second row");
    ProjectMemoryCompactionIntent changed = intent;
    changed.modelId = "other-model";
    requireRefused(db.intents->recordIntentObserved(changed), kIntentDuplicateIdCode, "a repeat with another model");
    require(db.text("SELECT model_id FROM project_memory_compaction_intents WHERE id='op-1';") == "model-1", "...which does not replace the stored row");

    // An unknown project.
    ProjectMemoryCompactionIntent orphan = makeIntent("op-orphan");
    orphan.projectId = "no-such-project";
    const auto refused = db.intents->recordIntentObserved(orphan);
    requireRefused(refused, "project_memory.project_not_found", "an unknown project");
    require(refused.persist.state == CompactionPersistState::NotCommitted && refused.persist.primaryNative.has_value() &&
            refused.persist.primaryNative->extendedCode == SQLITE_CONSTRAINT_FOREIGNKEY,
        "...proven not stored, with the foreign-key code kept");

    // An invalid new intent never reaches the connection.
    ProjectMemoryCompactionIntent unresolved = makeIntent("op-u");
    unresolved.state = CompactionIntentState::Unresolved;
    unresolved.unresolvedMarkedAt = at(5);
    const auto invalid = db.intents->recordIntentObserved(unresolved);
    requireRefused(invalid, kIntentInvalidCode, "an unresolved new intent");
    require(invalid.persist.state == CompactionPersistState::NotCommitted && !invalid.persist.connection.has_value(),
        "...refused before the connection is touched (nothing observed)");
    require(db.intentCount() == 1, "none of the refusals added a row");
}

void testAFinalRecordUnderTheIdBlocksAdmissionEvenWhenTheIntentIsGone(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    for (const WritePath path : kAllPaths) {
        Db each(memory);
        requireAdmitted(each.intents->recordIntentObserved(intent), "the intent");
        const auto written = each.write(each.recordFor(intent, path), path);
        require(written.result.success() && written.state == CompactionPersistState::Committed, std::string("the final record is stored: ") + pathName(path));
        require(each.intentCount() == 0 && each.recordCount() == 1, std::string("the final record closed the intent: ") + pathName(path));

        // Same identity: the operation is already finished; it must not be admitted again.
        const auto identical = each.intents->recordIntentObserved(intent);
        requireRefused(identical, kIntentDuplicateIdCode, std::string("an identical new intent after ") + pathName(path));
        require(identical.persist.state == CompactionPersistState::NotCommitted && identical.persist.primaryNative.has_value() &&
                identical.persist.primaryNative->extendedCode == SQLITE_CONSTRAINT_TRIGGER &&
                identical.persist.primaryNative->message == detail::kCompactionIntentIdOccupiedToken,
            "...by the atomic check of the insert itself, with the trigger's code and token kept");
        ProjectMemoryCompactionIntent different = intent;
        different.providerId = "another-provider";
        requireRefused(each.intents->recordIntentObserved(different), kIntentDuplicateIdCode, "a different new intent after the record");
        require(each.intentCount() == 0, "no intent appeared");
    }
}

// ---- the guard and the closure ---------------------------------------------------------------------------------------------------

void testAMatchingFinalRecordClosesTheIntentInTheSameTransaction(bool memory)
{
    for (const WritePath path : kAllPaths) {
        Db db(memory);
        const ProjectMemoryCompactionIntent intent = makeIntent();
        requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
        const ProjectMemoryCompactionRecord record = db.recordFor(intent, path);
        const auto written = db.write(record, path);
        require(written.result.success() && written.state == CompactionPersistState::Committed &&
                written.connection == CompactionConnectionState::Healthy,
            std::string("the matching record is stored: ") + pathName(path));
        require(db.recordCount() == 1 && db.intentCount() == 0, std::string("record stored AND intent closed: ") + pathName(path));
        require(!db.inTransaction(), "no transaction is left open");
        if (path == WritePath::Completed) {
            require(db.scalar("SELECT count(*) FROM project_memory_segments WHERE id IN ('s1','s2');") == 0 &&
                    db.scalar("SELECT count(*) FROM project_memory_segments WHERE id='result-op-1';") == 1,
                "the sources were replaced by the result in that same commit");
        }
        const auto observed = db.intents->observeOperation(compactionIntentIdentityOf(intent));
        require(observed.observation == CompactionIntentObservation::FinalRecordPresent && !observed.intent.has_value() &&
                observed.recordIdentity.has_value() && observed.source == CompactionIntentObservationSource::AtomicPairRead,
            std::string("the proof sees the record and no intent: ") + pathName(path));
    }
}

void testAMismatchingFinalRecordIsRefusedAndNothingIsLost(bool memory)
{
    for (const WritePath path : kAllPaths) {
        for (const FieldVariant& variant : mismatchingVariants()) {
            Db db(memory);
            const ProjectMemoryCompactionIntent baseline = makeIntent();
            ProjectMemoryCompactionIntent stored = baseline;
            variant.apply(stored);
            requireAdmitted(db.intents->recordIntentObserved(stored), "the (different) intent");
            const std::string what = std::string(pathName(path)) + " / " + toString(variant.field);

            const ProjectMemoryCompactionRecord record = db.recordFor(baseline, path);
            const auto written = db.write(record, path);
            require(!written.result.success() && written.result.error()->code == kIntentIdentityConflictCode, what + ": refused as an identity conflict");
            require(written.state == CompactionPersistState::NotCommitted, what + ": proven not stored");
            require(written.connection == CompactionConnectionState::Healthy && !db.inTransaction(), what + ": the connection is healthy, no transaction");
            require(written.primaryNative.has_value() && written.primaryNative->extendedCode == SQLITE_CONSTRAINT_TRIGGER &&
                    written.primaryNative->message == detail::kCompactionIntentConflictToken,
                what + ": the native code 1811 and the exact token are kept");
            require(db.recordCount() == 0, what + ": no record");
            require(db.intentCount() == 1 && db.text("SELECT model_id FROM project_memory_compaction_intents WHERE id='op-1';") == stored.modelId,
                what + ": the intent is untouched");
            require(db.scalar("SELECT count(*) FROM project_memory_segments WHERE id IN ('s1','s2','s3');") == 3 &&
                    db.scalar("SELECT count(*) FROM project_memory_segments WHERE id='result-op-1';") == 0,
                what + ": the sources are in place and no result segment exists (the commit rolled back)");
        }
    }
}

void testAnotherIdIsNotAMismatchBecauseThereIsNoIntentForIt(bool memory)
{
    Db db(memory);
    requireAdmitted(db.intents->recordIntentObserved(makeIntent("op-1")), "the intent");
    ProjectMemoryCompactionIntent other = makeIntent("op-2");   // an unrelated operation: no intent exists for this id
    other.providerId = "whatever";
    const auto written = db.records->insertCompactionRecordObserved(recordOf(other, ProjectMemoryCompactionStatus::Failed));
    require(written.result.success() && written.state == CompactionPersistState::Committed, "a record without an intent of ITS id is stored as before");
    require(db.recordCount() == 1 && db.intentCount() == 1 && db.text("SELECT id FROM project_memory_compaction_intents;") == "op-1",
        "...and the intent of the other operation stays");
}

void testNoIntentLeavesEveryOldPathUnchanged(bool memory)
{
    for (const WritePath path : kAllPaths) {
        Db db(memory);
        const ProjectMemoryCompactionIntent shape = makeIntent("legacy");
        const auto written = db.write(db.recordFor(shape, path), path);
        require(written.result.success() && written.state == CompactionPersistState::Committed, std::string("no intent, no guard: ") + pathName(path));
        require(db.recordCount() == 1 && db.intentCount() == 0, "one record, no intent");
        if (path == WritePath::Completed) continue;   // its sources are gone after the first commit: that duplicate is a compaction_conflict
        const auto duplicate = db.write(db.recordFor(shape, path), path);
        require(!duplicate.result.success() && duplicate.result.error()->code == "project_memory.duplicate_id",
            std::string("the duplicate-id mapping of the old writer is unchanged: ") + pathName(path));
    }
}

void testOnlyTheIntentTriggerIsMappedToAConflict(bool memory)
{
    Db db(memory);
    // A foreign ABORT with another text, an ABORT whose text merely CONTAINS the token, and a CHECK failure are NOT conflicts.
    require(db.exec("CREATE TRIGGER foreign_abort BEFORE INSERT ON project_memory_compaction_records WHEN NEW.id='foreign' "
                    "BEGIN SELECT RAISE(ABORT, 'SOME_OTHER_ABORT'); END;"
                    "CREATE TRIGGER foreign_substring BEFORE INSERT ON project_memory_compaction_records WHEN NEW.id='substring' "
                    "BEGIN SELECT RAISE(ABORT, 'x AIO_COMPACTION_INTENT_IDENTITY_CONFLICT y'); END;")
                .empty(),
        "plant the foreign triggers");
    for (const char* id : {"foreign", "substring"}) {
        const auto written = db.records->insertCompactionRecordObserved(recordOf(makeIntent(id), ProjectMemoryCompactionStatus::Failed));
        require(!written.result.success() && written.result.error()->code == "persistence.write_failed",
            std::string("a foreign trigger abort stays a persistence failure: ") + id);
        require(written.primaryNative.has_value() && written.primaryNative->extendedCode == SQLITE_CONSTRAINT_TRIGGER,
            "...with the native code kept");
    }
    require(!db.exec("INSERT INTO project_memory_compaction_records(id,project_id,provider_id,model_id,source_segment_ids,"
                     "source_content_hash,status,created_at_ms,cost_price_known) VALUES('c','p1','a','b','[]','h','failed',1,7);").empty(),
        "(a CHECK failure exists in this schema)");
}

// ---- comparator <-> guard equivalence ------------------------------------------------------------------------------------------

void testTheApplicationComparatorAndTheStorageGuardAgree(bool memory)
{
    struct Case {
        const char* name;
        std::function<void(ProjectMemoryCompactionIntent&)> apply;
    };
    const Case cases[] = {
        {"identical", [](ProjectMemoryCompactionIntent&) {}},
        {"project", [](ProjectMemoryCompactionIntent& i) { i.projectId = "p2"; }},
        {"provider", [](ProjectMemoryCompactionIntent& i) { i.providerId += "x"; }},
        {"provider case", [](ProjectMemoryCompactionIntent& i) { i.providerId = "PROVIDER-1"; }},
        {"model", [](ProjectMemoryCompactionIntent& i) { i.modelId = ""; }},
        {"sources reordered", [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {"s2", "s1"}; }},
        {"sources with a duplicate", [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {"s1", "s2", "s2"}; }},
        {"sources shortened", [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {"s1"}; }},
        {"sources unicode", [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {"s1", "s\xC3\xA9"}; }},
        {"sources empty", [](ProjectMemoryCompactionIntent& i) { i.sourceSegmentIds = {}; }},
        {"hash", [](ProjectMemoryCompactionIntent& i) { i.sourceContentHash.front() = 'f'; }},
        {"hash case", [](ProjectMemoryCompactionIntent& i) { i.sourceContentHash = std::string(64, 'A'); }},
        {"time +499us (same millisecond)", [](ProjectMemoryCompactionIntent& i) { i.createdAt += std::chrono::microseconds(499); }},
        {"time +999us (same millisecond)", [](ProjectMemoryCompactionIntent& i) { i.createdAt += std::chrono::microseconds(999); }},
        {"time +1ms", [](ProjectMemoryCompactionIntent& i) { i.createdAt += std::chrono::milliseconds(1); }},
        {"time -1ms", [](ProjectMemoryCompactionIntent& i) { i.createdAt -= std::chrono::milliseconds(1); }},
    };
    for (const Case& c : cases) {
        Db db(memory);
        const ProjectMemoryCompactionIntent baseline = makeIntent();
        ProjectMemoryCompactionIntent variant = baseline;
        c.apply(variant);
        requireAdmitted(db.intents->recordIntentObserved(variant), std::string("admit ") + c.name);
        ProjectMemoryCompactionRecord record = recordOf(baseline, ProjectMemoryCompactionStatus::Failed);
        record.sourceSegmentIds = baseline.sourceSegmentIds;
        const bool comparatorSaysDifferent = compactionIntentIdentityDifference(variant, record).has_value();
        const auto written = db.records->insertCompactionRecordObserved(record);
        const bool guardRefused = !written.result.success() && written.result.error()->code == kIntentIdentityConflictCode;
        require(written.result.success() || guardRefused, std::string(c.name) + ": the only possible refusal is the conflict");
        require(comparatorSaysDifferent == guardRefused,
            std::string(c.name) + ": the pure comparator and the storage guard must give the same decision (comparator says "
                + (comparatorSaysDifferent ? "different" : "same") + ", guard " + (guardRefused ? "refused" : "accepted") + ")");
        require((db.intentCount() == 0) == !guardRefused, std::string(c.name) + ": the intent is closed exactly when the record was accepted");
    }
}

void testANoncanonicalStoredIntentIsRefusedByTheDecoderAndFailsClosedAtTheGuard(bool memory)
{
    Db db(memory);
    // Another writer's spelling of the same list. The decoder refuses it before any comparison; the guard (a byte comparison) would
    // say "different" - so a record can never silently close an intent that the application cannot read.
    require(db.exec("INSERT INTO project_memory_compaction_intents VALUES('op-1','p1','provider-1','model-1','[ \"s1\", \"s2\" ]','"
                    + std::string(64, 'a') + "',2000,'open',NULL);").empty(), "plant a spaced list");
    const auto read = db.intents->findIntent("op-1");
    require(!read.success() && read.error()->code == "persistence.read_failed", "the decoder refuses the noncanonical text");
    const auto written = db.records->insertCompactionRecordObserved(recordOf(makeIntent(), ProjectMemoryCompactionStatus::Failed));
    require(!written.result.success() && written.result.error()->code == kIntentIdentityConflictCode && db.recordCount() == 0 && db.intentCount() == 1,
        "the guard fails closed on it: nothing is stored and the intent stays");
    // The old record writer's text IS the canonical text.
    ProjectMemoryCompactionIntent canonical = makeIntent("op-2");
    requireAdmitted(db.intents->recordIntentObserved(canonical), "a canonical intent");
    require(db.text("SELECT source_segment_ids FROM project_memory_compaction_intents WHERE id='op-2';") == "[\"s1\",\"s2\"]",
        "the stored text is the compact JSON array");
    require(db.records->insertCompactionRecordObserved(recordOf(canonical, ProjectMemoryCompactionStatus::Failed)).result.success() &&
            db.text("SELECT source_segment_ids FROM project_memory_compaction_records WHERE id='op-2';") == "[\"s1\",\"s2\"]",
        "...and it is byte for byte the text of the record");
    require(detail::serializeSegmentIdList({"a", "b", "a"}) == "[\"a\",\"b\",\"a\"]" && detail::serializeSegmentIdList({}) == "[]" &&
            detail::serializeSegmentIdList({"\xE6\xB8\xAC"}) == "[\"\xE6\xB8\xAC\"]",
        "the shared serializer keeps its old output (compact, ordered, duplicates, raw UTF-8)");
    require(detail::deserializeCanonicalSegmentIdList("[\"a\",\"b\"]").success() && !detail::deserializeCanonicalSegmentIdList("[ \"a\" ]").success() &&
            !detail::deserializeCanonicalSegmentIdList("[\"\\u0061\"]").success() && detail::deserializeSegmentIdListStrict("[ \"a\" ]").success(),
        "the canonical decoder is stricter than the lenient record lookup, as designed");
}

// ---- the one proof operation ------------------------------------------------------------------------------------------------

void testTheObservationCoversEveryState(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    {
        const auto none = db.intents->observeOperation(expected);
        require(none.observation == CompactionIntentObservation::NeitherPresent && none.source == CompactionIntentObservationSource::AtomicPairRead &&
                !none.error.has_value() && none.connection == CompactionConnectionState::Healthy,
            "neither row: NeitherPresent (no verdict about delivery)");
    }
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    {
        const auto open = db.intents->observeOperation(expected);
        require(open.observation == CompactionIntentObservation::IntentPresent && open.intent.has_value() &&
                open.intent->state == CompactionIntentState::Open && !open.recordIdentity.has_value(),
            "intent only: IntentPresent");
        CompactionIntentIdentity foreign = expected;
        foreign.providerId = "someone-else";
        const auto mismatch = db.intents->observeOperation(foreign);
        require(mismatch.observation == CompactionIntentObservation::Inconsistent && mismatch.error.has_value() &&
                mismatch.error->code == kIntentObservationInconsistentCode,
            "a row under the id that is not the expected operation: Inconsistent");
    }
    db.dropGuards();
    require(db.exec("INSERT INTO project_memory_compaction_records(id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,"
                    "status,created_at_ms) VALUES('op-1','p1','provider-1','model-1','[\"s1\",\"s2\"]','" + std::string(64, 'a') + "','failed',2000);")
                .empty(),
        "plant a record next to the intent");
    {
        const auto both = db.intents->observeOperation(expected);
        require(both.observation == CompactionIntentObservation::Inconsistent && both.intent.has_value() && both.recordIdentity.has_value() &&
                both.error.has_value() && both.error->code == kIntentObservationInconsistentCode,
            "both present: Inconsistent, both rows reported");
    }
    require(db.exec("DELETE FROM project_memory_compaction_intents;").empty(), "remove the intent");
    {
        const auto record = db.intents->observeOperation(expected);
        require(record.observation == CompactionIntentObservation::FinalRecordPresent && record.recordIdentity.has_value() && !record.intent.has_value(),
            "record only: FinalRecordPresent");
        CompactionIntentIdentity foreign = expected;
        foreign.createdAt = at(2001);
        require(db.intents->observeOperation(foreign).observation == CompactionIntentObservation::Inconsistent,
            "a foreign record under the id: Inconsistent");
    }
}

void testTheObservationIsUnreadableNotInventedWhenItCannotRead(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");

    {   // a failing proof read (the closed fault seam)
        detail::FaultPlan plan;
        plan.failProofRead = true;
        detail::ScopedConnectionFault fault(*db.database, plan);
        const auto outcome = db.intents->observeOperation(expected);
        require(outcome.observation == CompactionIntentObservation::Unreadable && outcome.source == CompactionIntentObservationSource::None &&
                outcome.error.has_value() && outcome.error->code == "persistence.read_failed" && !outcome.intent.has_value(),
            "a failed proof read: Unreadable with the original code, no row invented");
    }
    {   // an open transaction on the connection: its own rows are not proof
        require(db.exec("BEGIN;").empty(), "begin");
        const auto outcome = db.intents->observeOperation(expected);
        const auto found = db.intents->findIntent(intent.id);
        require(db.exec("ROLLBACK;").empty(), "rollback");
        require(outcome.observation == CompactionIntentObservation::Unreadable && outcome.error.has_value() &&
                outcome.error->code == "persistence.proof_unavailable",
            "inside an open transaction the proof refuses");
        require(!found.success() && found.error()->code == "persistence.lookup_unavailable", "...and so does the lookup");
    }
    {   // a REAL quarantined connection
        const auto h = db.handles();
        {
            std::lock_guard lock(*h.mutex);
            require(h.health->enterQuarantine(h.handle, detail::QuarantineReason::ForeignOpenTransaction).success(), "quarantine");
        }
        const auto outcome = db.intents->observeOperation(expected);
        const auto write = db.intents->recordIntentObserved(makeIntent("op-q"));
        const auto mutation = db.intents->markUnresolvedObserved(expected, at(9));
        {
            std::lock_guard lock(*h.mutex);
            require(h.health->release(h.handle).success(), "release");
        }
        require(outcome.observation == CompactionIntentObservation::Unreadable && outcome.error.has_value() &&
                outcome.error->code == "persistence.connection_unusable" && outcome.connection == CompactionConnectionState::Quarantined,
            "a quarantined connection: Unreadable, connection reported Quarantined, original refusal kept");
        require(!mayDispatch(write) && !write.persist.result.success() && write.persist.connection == CompactionConnectionState::Quarantined,
            "a quarantined connection never admits");
        require(mutation.mutation == CompactionIntentMutation::Unconfirmed && !mutation.persist.result.success() &&
                mutation.persist.state != CompactionPersistState::Committed,
            "...and never confirms a mark");
        require(db.intentCount() == 1, "nothing was written meanwhile");
        require(db.intents->observeOperation(expected).observation == CompactionIntentObservation::IntentPresent, "after the release it reads again");
    }
}

// R1: the reads are forbidden by the connection's HEALTH, not by luck of an installed SQL filter. The state is modelled on a real
// connection: health says "unusable" while the authorizer is taken away again, so a raw SELECT demonstrably works. (This models a
// connection whose isolation filter is not in force; it does not claim to reproduce a failing sqlite3_set_authorizer.)
void testEveryReadRefusesAnUnusableConnectionEvenWithoutAWorkingSqlFilter(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    require(db.intents->findIntent(intent.id).value().has_value(), "healthy control: the lookup works");

    const auto h = db.handles();
    {
        std::lock_guard lock(*h.mutex);
        require(h.health->enterQuarantine(h.handle, detail::QuarantineReason::ForeignOpenTransaction).success(), "unusable health");
        sqlite3_set_authorizer(h.handle, nullptr, nullptr);   // the filter is NOT in force
        require(h.health->quarantined(), "health still says unusable");
        require(sqlite3_get_autocommit(h.handle) != 0, "no transaction is open");
        sqlite3_stmt* probe = nullptr;
        require(sqlite3_prepare_v2(h.handle, "SELECT count(*) FROM project_memory_compaction_intents;", -1, &probe, nullptr) == SQLITE_OK &&
                sqlite3_step(probe) == SQLITE_ROW && sqlite3_column_int(probe, 0) == 1,
            "a raw SELECT technically works on this connection");
        sqlite3_finalize(probe);
    }
    const auto found = db.intents->findIntent(intent.id);
    const auto listed = db.intents->listIntents("p1", std::nullopt);
    const auto byState = db.intents->listIntentsByState(CompactionIntentState::Open);
    const auto observed = db.intents->observeOperation(expected);
    {
        std::lock_guard lock(*h.mutex);
        require(h.health->release(h.handle).success(), "restore");
    }
    require(!found.success() && found.error()->code == "persistence.connection_unusable", "findIntent refuses and returns no row");
    require(!listed.success() && listed.error()->code == "persistence.connection_unusable", "listIntents refuses");
    require(!byState.success() && byState.error()->code == "persistence.connection_unusable", "listIntentsByState refuses");
    require(observed.observation == CompactionIntentObservation::Unreadable && observed.source == CompactionIntentObservationSource::None &&
            !observed.intent.has_value() && !observed.recordIdentity.has_value() && observed.error.has_value() &&
            observed.error->code == "persistence.connection_unusable" && !observed.primaryNative.has_value() &&
            observed.connection == CompactionConnectionState::Quarantined,
        "observeOperation: Unreadable, no source, no rows, no native code of a statement that never ran, connection state kept");
    // After the ordinary restoration the healthy control passes again.
    require(db.intents->findIntent(intent.id).value().has_value() && db.intents->listIntentsByState(CompactionIntentState::Open).value().size() == 1 &&
            db.intents->observeOperation(expected).observation == CompactionIntentObservation::IntentPresent,
        "healthy control after the restoration");
}

// ---- guarded state changes -----------------------------------------------------------------------------------------------------

void testMarkUnresolvedIsGuardedAndIdempotent(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");

    // A mismatch in ANY identity field changes nothing, whatever its state.
    for (const FieldVariant& variant : mismatchingVariants()) {
        ProjectMemoryCompactionIntent changed = intent;
        variant.apply(changed);
        const auto refused = db.intents->markUnresolvedObserved(compactionIntentIdentityOf(changed), at(7000));
        require(refused.mutation == CompactionIntentMutation::IdentityConflict && !refused.persist.result.success() &&
                refused.persist.result.error()->code == kIntentIdentityConflictCode && refused.persist.state == CompactionPersistState::NotCommitted,
            std::string("a mismatching ") + toString(variant.field) + " is an identity conflict");
        require(db.text("SELECT state FROM project_memory_compaction_intents;") == "open", "...and the row stays open");
    }
    CompactionIntentIdentity otherId = expected;
    otherId.id = "nope";
    const auto none = db.intents->markUnresolvedObserved(otherId, at(7000));
    require(none.mutation == CompactionIntentMutation::NotFound && none.persist.result.success() &&
            none.persist.state == CompactionPersistState::NotCommitted,
        "an unknown id is NotFound, an answer and not a change");

    const auto marked = db.intents->markUnresolvedObserved(expected, at(7000));
    require(marked.mutation == CompactionIntentMutation::Applied && marked.persist.state == CompactionPersistState::Committed &&
            marked.persist.result.success() && marked.persist.connection == CompactionConnectionState::Healthy,
        "a matching open intent is marked (Committed)");
    require(db.text("SELECT state FROM project_memory_compaction_intents;") == "unresolved" &&
            db.scalar("SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents;") == 7000,
        "...with its mark");

    const auto repeated = db.intents->markUnresolvedObserved(expected, at(9000));
    require(repeated.mutation == CompactionIntentMutation::AlreadyApplied && repeated.persist.result.success() &&
            repeated.persist.state == CompactionPersistState::NotCommitted,
        "marking again is AlreadyApplied and not a change");
    require(db.scalar("SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents;") == 7000 && db.intentCount() == 1,
        "...the first mark is never rewritten and no row is added");
    const auto mismatchOnUnresolved = [&] {
        ProjectMemoryCompactionIntent changed = intent;
        changed.modelId = "other";
        return db.intents->markUnresolvedObserved(compactionIntentIdentityOf(changed), at(9000));
    }();
    require(mismatchOnUnresolved.mutation == CompactionIntentMutation::IdentityConflict, "an unresolved row with another identity is still a conflict");

    const auto stored = db.intents->findIntent(intent.id);
    require(stored.success() && stored.value()->state == CompactionIntentState::Unresolved && stored.value()->unresolvedMarkedAt == at(7000),
        "the strict read returns the mark");
}

void testAnUnexplainedZeroRowResultIsNeverDeclaredDone(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    detail::FaultPlan plan;
    plan.failProofRead = true;
    detail::ScopedConnectionFault fault(*db.database, plan);
    CompactionIntentIdentity otherId = expected;
    otherId.id = "nope";   // zero rows changed; the read that would explain it cannot run
    const auto mark = db.intents->markUnresolvedObserved(otherId, at(1));
    const auto close = db.intents->closeIntentObserved(otherId);
    require(mark.mutation == CompactionIntentMutation::Unconfirmed && !mark.persist.result.success() &&
            close.mutation == CompactionIntentMutation::Unconfirmed && !close.persist.result.success(),
        "when the explaining read fails the result is Unconfirmed: neither NotFound nor AlreadyApplied is claimed");
}

void testGuardedCloseDeletesOnlyTheExactIntent(bool memory)
{
    Db db(memory);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    for (const FieldVariant& variant : mismatchingVariants()) {
        ProjectMemoryCompactionIntent changed = intent;
        variant.apply(changed);
        const auto refused = db.intents->closeIntentObserved(compactionIntentIdentityOf(changed));
        require(refused.mutation == CompactionIntentMutation::IdentityConflict && refused.persist.result.error()->code == kIntentIdentityConflictCode,
            std::string("a mismatching ") + toString(variant.field) + " does not close");
        require(db.intentCount() == 1, "...and nothing is deleted");
    }
    CompactionIntentIdentity otherId = expected;
    otherId.id = "nope";
    require(db.intents->closeIntentObserved(otherId).mutation == CompactionIntentMutation::NotFound, "an unknown id is NotFound");

    // The state is not compared: an unresolved intent can be closed by its exact identity.
    require(db.intents->markUnresolvedObserved(expected, at(5)).mutation == CompactionIntentMutation::Applied, "mark");
    const auto closed = db.intents->closeIntentObserved(expected);
    require(closed.mutation == CompactionIntentMutation::Applied && closed.persist.state == CompactionPersistState::Committed && db.intentCount() == 0,
        "the exact identity closes the intent (Committed)");
    require(db.intents->closeIntentObserved(expected).mutation == CompactionIntentMutation::NotFound, "closing again is NotFound, not a second success");
}

// ---- Unknown outcomes (real states, not a fabricated result) -------------------------------------------------------------------

int failAfterCommit(void*, sqlite3*, const char*, int) { return SQLITE_IOERR; }

// Arms a WAL hook on ONE connection that reports an error AFTER each commit (the commit has happened; no SQL runs inside it) and
// restores the default auto-checkpoint hook.
class CommitThenErrorHook {
public:
    explicit CommitThenErrorHook(const Db& db) : handles_(db.handles())
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

void testAnInsertThatCommittedButReportedAnErrorIsUnknownAndNotAdmission()
{
    Db db(false);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    CompactionIntentWriteOutcome outcome;
    {
        CommitThenErrorHook hook(db);
        outcome = db.intents->recordIntentObserved(intent);
    }
    require(!outcome.persist.result.success() && !mayDispatch(outcome) && !outcome.newIntentInserted &&
            outcome.persist.state == CompactionPersistState::Unknown,
        "an insert whose error arrived after the commit is Unknown and is not an admission");
    // The truth, from a SEPARATE read-only connection: it WAS committed.
    require(readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_intents WHERE id='op-1';") == 1,
        "the row really is committed (a restart would find it)");
    require(db.intents->findIntent(intent.id).value().has_value(), "and it is readable");
    // A hook that is gone leaves the connection as it was.
    requireRefused(db.intents->recordIntentObserved(intent), kIntentDuplicateIdCode, "a retry is a duplicate, never a second admission");
}

void testAnUnprovenNotCommittedInsertIsUnknownToo()
{
    Db db(false);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    requireAdmitted(db.intents->recordIntentObserved(intent), "the first intent");
    // A genuine failure (a duplicate id: nothing is written) whose proof read cannot run: the state is Unknown although nothing was stored.
    CompactionIntentWriteOutcome outcome;
    {
        detail::FaultPlan plan;
        plan.failProofRead = true;
        detail::ScopedConnectionFault fault(*db.database, plan);
        outcome = db.intents->recordIntentObserved(intent);
    }
    require(!mayDispatch(outcome) && !outcome.persist.result.success() && outcome.persist.result.error()->code == kIntentDuplicateIdCode &&
            outcome.persist.state == CompactionPersistState::Unknown,
        "the primary error is kept and an unprovable write is Unknown, never NotCommitted");
    require(outcome.persist.diagnostics.size() == 1 && outcome.persist.diagnostics[0].stage == CompactionDiagnosticStage::ProofRead,
        "the proof failure is a secondary diagnostic");
    require(readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_intents;") == 1, "...and in fact nothing was added");
}

void testAMutationThatCommittedButReportedAnErrorIsUnconfirmed()
{
    Db db(false);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(intent);
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    CompactionIntentMutationOutcome mark;
    {
        CommitThenErrorHook hook(db);
        mark = db.intents->markUnresolvedObserved(expected, at(4000));
    }
    require(mark.mutation == CompactionIntentMutation::Unconfirmed && !mark.persist.result.success() &&
            mark.persist.state == CompactionPersistState::Unknown,
        "a mark whose error came after the commit is Unconfirmed / Unknown, never Applied and never 'nothing happened'");
    require(readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_intents WHERE state='unresolved' AND unresolved_marked_at_ms=4000;") == 1,
        "the mark really is committed");

    CompactionIntentMutationOutcome close;
    {
        CommitThenErrorHook hook(db);
        close = db.intents->closeIntentObserved(expected);
    }
    require(close.mutation == CompactionIntentMutation::Unconfirmed && close.persist.state == CompactionPersistState::Unknown,
        "a close whose error came after the commit is Unconfirmed / Unknown");
    require(readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_intents;") == 0, "the delete really is committed");
}

void testAFinalWriteThatCommittedButReportedAnErrorClosedTheIntentAtomically()
{
    Db db(false);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    CompactionPersistOutcome written;
    {
        CommitThenErrorHook hook(db);
        written = db.records->commitSuccessfulCompactionObserved(recordOf(intent, ProjectMemoryCompactionStatus::Completed),
            resultSegmentOf(recordOf(intent, ProjectMemoryCompactionStatus::Completed)));
    }
    require(!written.result.success() && written.state == CompactionPersistState::Unknown, "the final write is Unknown (the error came after the commit)");
    // Both halves, from a separate connection: the record and the closure are one commit.
    require(readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_records WHERE id='op-1';") == 1 &&
            readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_intents;") == 0,
        "the record is stored AND the intent is gone: the commit was atomic");
    const auto observed = db.intents->observeOperation(compactionIntentIdentityOf(intent));
    require(observed.observation == CompactionIntentObservation::FinalRecordPresent, "the proof read says so");
}

void testARolledBackFinalWriteLeavesTheIntentOpen()
{
    Db db(false);
    const ProjectMemoryCompactionIntent intent = makeIntent();
    requireAdmitted(db.intents->recordIntentObserved(intent), "the intent");
    // A REAL commit failure after the closing delete ran: a deferred foreign key violated by a trigger.
    require(db.exec("CREATE TABLE b3_parent(id INTEGER PRIMARY KEY);"
                    "CREATE TABLE b3_child(parent_id INTEGER REFERENCES b3_parent(id) DEFERRABLE INITIALLY DEFERRED);"
                    "CREATE TRIGGER b3_fail_commit AFTER INSERT ON project_memory_compaction_records WHEN NEW.status = 'completed' "
                    "BEGIN INSERT INTO b3_child VALUES (999); END;")
                .empty(),
        "install the deferred commit failure");
    const ProjectMemoryCompactionRecord record = recordOf(intent, ProjectMemoryCompactionStatus::Completed);
    const auto written = db.records->commitSuccessfulCompactionObserved(record, resultSegmentOf(record));
    require(!written.result.success() && written.state == CompactionPersistState::NotCommitted && written.connection == CompactionConnectionState::Healthy,
        "the failed COMMIT is proven not stored and the connection is healthy");
    require(readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_intents;") == 1 &&
            readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_compaction_records;") == 0 &&
            readonlyScalar(db.temp.file, "SELECT count(*) FROM project_memory_segments WHERE id IN ('s1','s2');") == 2,
        "a rolled-back commit keeps the intent open and the sources in place (the closing delete was rolled back with it)");
    require(db.intents->observeOperation(compactionIntentIdentityOf(intent)).observation == CompactionIntentObservation::IntentPresent,
        "the proof says IntentPresent");
}

// ---- deletion --------------------------------------------------------------------------------------------------------------------

void testADeletedProjectTakesItsIntentsAndAModelIdIsPlainText(bool memory)
{
    Db db(memory);
    requireAdmitted(db.intents->recordIntentObserved(makeIntent("op-1")), "p1 intent");
    ProjectMemoryCompactionIntent p2 = makeIntent("op-2");
    p2.projectId = "p2";
    requireAdmitted(db.intents->recordIntentObserved(p2), "p2 intent");
    require(db.intents->markUnresolvedObserved(compactionIntentIdentityOf(p2), at(1)).mutation == CompactionIntentMutation::Applied, "mark p2");
    require(db.exec("DELETE FROM projects WHERE id='p2';").empty(), "delete the project");
    require(db.intentCount() == 1 && db.text("SELECT id FROM project_memory_compaction_intents;") == "op-1",
        "the intent of the deleted project is gone, the other stays (no orphan)");
    // The provider and model rows are not referenced: removing them (or never having them) changes nothing.
    const auto found = db.intents->findIntent("op-1");
    require(found.success() && found.value().has_value() && found.value()->modelId == "model-1", "an intent reads without any catalog row");
}

void testTwoOperationsAreIndependent(bool memory)
{
    Db db(memory);
    ProjectMemoryCompactionIntent a = makeIntent("op-a");
    ProjectMemoryCompactionIntent b = makeIntent("op-b");
    b.projectId = "p2";
    b.sourceSegmentIds = {"x1"};
    requireAdmitted(db.intents->recordIntentObserved(a), "a");
    requireAdmitted(db.intents->recordIntentObserved(b), "b");
    require(db.records->insertCompactionRecordObserved(recordOf(a, ProjectMemoryCompactionStatus::Cancelled)).result.success(), "record a");
    require(db.intentCount() == 1 && db.text("SELECT id FROM project_memory_compaction_intents;") == "op-b", "a closed, b untouched");
}

} // namespace

int runSqliteCompactionIntentTests()
{
    for (const bool memory : {false, true}) {
        testANewDatabaseHasExactlyTheStep75Objects(memory);
        testARoundTripKeepsEveryFieldInOrder(memory);
        testDamagedRowsAreAReadFailureNeverRepaired(memory);
        testAdmissionIsOnlyAConfirmedNewInsert(memory);
        testAFinalRecordUnderTheIdBlocksAdmissionEvenWhenTheIntentIsGone(memory);
        testAMatchingFinalRecordClosesTheIntentInTheSameTransaction(memory);
        testAMismatchingFinalRecordIsRefusedAndNothingIsLost(memory);
        testAnotherIdIsNotAMismatchBecauseThereIsNoIntentForIt(memory);
        testNoIntentLeavesEveryOldPathUnchanged(memory);
        testOnlyTheIntentTriggerIsMappedToAConflict(memory);
        testTheApplicationComparatorAndTheStorageGuardAgree(memory);
        testANoncanonicalStoredIntentIsRefusedByTheDecoderAndFailsClosedAtTheGuard(memory);
        testTheObservationCoversEveryState(memory);
        testTheObservationIsUnreadableNotInventedWhenItCannotRead(memory);
        testEveryReadRefusesAnUnusableConnectionEvenWithoutAWorkingSqlFilter(memory);
        testMarkUnresolvedIsGuardedAndIdempotent(memory);
        testAnUnexplainedZeroRowResultIsNeverDeclaredDone(memory);
        testGuardedCloseDeletesOnlyTheExactIntent(memory);
        testADeletedProjectTakesItsIntentsAndAModelIdIsPlainText(memory);
        testTwoOperationsAreIndependent(memory);
    }
    // States that need a file (a second connection sees what a restart would find; the WAL hook needs WAL).
    testAnInsertThatCommittedButReportedAnErrorIsUnknownAndNotAdmission();
    testAnUnprovenNotCommittedInsertIsUnknownToo();
    testAMutationThatCommittedButReportedAnErrorIsUnconfirmed();
    testAFinalWriteThatCommittedButReportedAnErrorClosedTheIntentAtomically();
    testARolledBackFinalWriteLeavesTheIntentOpen();
    std::cout << "SqliteCompactionIntentTests: OK\n";
    return 0;
}
