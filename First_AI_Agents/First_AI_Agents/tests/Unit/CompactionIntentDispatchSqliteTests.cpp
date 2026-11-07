// B3-II-A: the REAL compaction service over REAL SQLite (a temporary file) with a fake provider: the intent is admitted - durably, as a
// separate read-only connection sees it - BEFORE the provider request is entered, and the storage's own guarded triggers close it with
// the final record on every path. Nothing here is scripted: states are produced by real SQL (a deferred foreign key that fails the
// COMMIT, a trigger that refuses every record) and, for the one state the product never produces on its own, by a scoped
// sqlite3_wal_hook that reports an error AFTER the commit happened (armed only after the admission, on the final write). No network, no
// child process. The checks use throwing require().
#include "SqliteFaultForTesting.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
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
#include <stop_token>
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
using A = CompactionAccounting;
using O = CompactionIntentObservation;
using Src = CompactionIntentObservationSource;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
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
        root = fs::temp_directory_path() / ("aio-b3iia-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
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

// A SECOND, read-only connection to the file: what a restart would find. The service's own connection proves nothing about durability.
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

// The intent port of the product, with every call counted (a decorator: it changes nothing and adds no behaviour).
class CountingIntents final : public IProjectMemoryCompactionIntentRepository {
public:
    explicit CountingIntents(IProjectMemoryCompactionIntentRepository& inner) : inner_(inner) {}
    int recordCalls {0};
    mutable int observeCalls {0};
    mutable int otherCalls {0};
    // After the admission was decided (and committed by the real port), before it returns to the service.
    std::function<void(const ProjectMemoryCompactionIntent&)> afterRecord;

    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent& intent) override
    {
        ++recordCalls;
        CompactionIntentWriteOutcome outcome = inner_.recordIntentObserved(intent);
        if (afterRecord) afterRecord(intent);
        return outcome;
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId& id) const override
    {
        ++otherCalls;
        return inner_.findIntent(id);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId& projectId, std::optional<CompactionIntentState> state) const override
    {
        ++otherCalls;
        return inner_.listIntents(projectId, state);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState state) const override
    {
        ++otherCalls;
        return inner_.listIntentsByState(state);
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(
        const CompactionIntentIdentity& expected, std::chrono::system_clock::time_point markedAt) override
    {
        ++otherCalls;
        return inner_.markUnresolvedObserved(expected, markedAt);
    }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity& expected) override
    {
        ++otherCalls;
        return inner_.closeIntentObserved(expected);
    }
    [[nodiscard]] CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity& expected) const override
    {
        ++observeCalls;
        return inner_.observeOperation(expected);
    }

private:
    IProjectMemoryCompactionIntentRepository& inner_;
};

class Client final : public IProviderClient {
public:
    int observedCalls {0};
    std::string content {"CONSOLIDATED SUMMARY"};
    std::optional<common::Error> failure;
    std::stop_source* stopSource {nullptr};
    std::function<void()> onSend;

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++observedCalls;
        if (onSend) onSend();
        if (stopSource != nullptr) stopSource->request_stop();
        if (failure.has_value()) return {common::Result<ProviderChatResponse>::failure(*failure), std::nullopt};
        ProviderChatResponse response;
        response.content = content;
        response.usage.inputTokens = 40;
        response.usage.outputTokens = 15;
        return {common::Result<ProviderChatResponse>::ok(std::move(response)), std::nullopt};
    }
};

class Factory final : public IProviderClientFactory {
public:
    explicit Factory(IProviderClient& client) : client_(client) {}
    int resolutions {0};
    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        ++resolutions;
        return common::Result<std::shared_ptr<IProviderClient>>::ok(std::shared_ptr<IProviderClient>(&client_, [](IProviderClient*) {}));
    }

private:
    IProviderClient& client_;
};

// Arms a WAL hook on ONE connection that reports an error AFTER each commit (the commit has happened; no SQL runs inside it) and
// restores the default auto-checkpoint hook. Held only between the admission and the end of the call.
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

// One real database, the real repositories and the real service.
struct Rig {
    TempRoot temp;
    std::shared_ptr<SqliteDatabase> database;
    std::unique_ptr<SqliteProjectMemoryRepository> memory;
    std::unique_ptr<SqliteProjectMemoryCompactionRepository> records;
    std::unique_ptr<SqliteProjectMemoryCompactionIntentRepository> intentStore;
    std::unique_ptr<CountingIntents> intents;
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    Client client;
    Factory factory {client};
    std::unique_ptr<ProjectMemoryCompactionService> service;

    Rig()
    {
        auto opened = SqliteDatabase::open(temp.file);
        require(opened.success(), "could not open the database");
        database = std::move(opened.value());
        memory = std::make_unique<SqliteProjectMemoryRepository>(database);
        records = std::make_unique<SqliteProjectMemoryCompactionRepository>(database);
        intentStore = std::make_unique<SqliteProjectMemoryCompactionIntentRepository>(database);
        intents = std::make_unique<CountingIntents>(*intentStore);
        service = std::make_unique<ProjectMemoryCompactionService>(*memory, *records, *intents, projects, providers, models, factory);

        require(exec("INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                     "VALUES('p1','One','d','C:/fixture/1','active',1,1);")
                    .empty(),
            "could not seed the project");
        Project project;
        project.id = "p1";
        project.name = "One";
        project.description = "d";
        project.rootPath = "C:/fixture/1";
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "setup: project");
        Provider provider;
        provider.id = "prov1";
        provider.name = "Fake";
        provider.kind = "openai_compatible";
        provider.enabled = true;
        provider.createdAt = project.createdAt;
        provider.updatedAt = project.createdAt;
        require(providers.insert(provider).success(), "setup: provider");
        Model model;
        model.id = "model1";
        model.providerId = "prov1";
        model.apiModelId = "fake-model-1";
        model.displayName = "Fake Model";
        model.contextWindowTokens = 128000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 5'000'000;
        model.outputPriceMicrosPerMillionTokens = 15'000'000;
        model.enabled = true;
        model.createdAt = project.createdAt;
        model.updatedAt = project.createdAt;
        require(models.insert(model).success(), "setup: model");
        for (const char* id : {"s1", "s2", "s3"}) {
            ProjectMemorySegment segment;
            segment.id = id;
            segment.projectId = "p1";
            segment.runId = std::string("run-") + id;
            segment.summary = std::string("Summary of ") + id;
            segment.sourceTokenCount = 11;
            segment.summaryTokenCount = 7;
            segment.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
            require(memory->insertMemorySegment(segment).success(), "could not seed a segment");
        }
    }

    ~Rig()
    {
        service.reset();
        intents.reset();
        intentStore.reset();
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

    // Truth from the SEPARATE connection.
    long long durable(const std::string& sql) const { return readonlyScalar(temp.file, sql); }
    long long intentRows() const { return durable("SELECT count(*) FROM project_memory_compaction_intents;"); }
    long long recordRows() const { return durable("SELECT count(*) FROM project_memory_compaction_records;"); }
    long long sourceRows() const { return durable("SELECT count(*) FROM project_memory_segments WHERE id IN ('s1','s2','s3');"); }

    ProjectMemoryCompactionOutcome run(std::stop_token stop = {}) { return service->compactObserved("p1", "prov1", "model1", stop); }
};

// ---- 1. the intent is durable before the request, and every final write closes it -----------------------------------------------------
void testTheIntentIsDurableBeforeTheRequestAndThePrimaryCommitClosesIt()
{
    Rig rig;
    long long intentRowsSeenBySend = -1;
    long long recordRowsSeenBySend = -1;
    std::optional<ProjectMemoryCompactionIntent> seenByPort;
    int openIntentsSeenByPort = -1;
    rig.client.onSend = [&] {
        intentRowsSeenBySend = rig.intentRows();   // a separate connection: the row is COMMITTED, not merely visible to the writer
        recordRowsSeenBySend = rig.recordRows();
        const auto listed = rig.intentStore->listIntents("p1", CompactionIntentState::Open);   // strict read through the real port
        require(listed.success(), "the strict listing during the request");
        openIntentsSeenByPort = static_cast<int>(listed.value().size());
        if (!listed.value().empty()) seenByPort = listed.value()[0];
    };
    const auto outcome = rig.run();
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Completed, "Completed");
    require(rig.client.observedCalls == 1 && rig.intents->recordCalls == 1, "one request after one admission");
    require(intentRowsSeenBySend == 1 && recordRowsSeenBySend == 0 && openIntentsSeenByPort == 1,
        "while the request was in flight the intent was durable (separate connection) and there was no final record yet");
    require(seenByPort.has_value() && seenByPort->state == CompactionIntentState::Open
                && !compactionIntentIdentityDifference(*seenByPort, outcome.result.value()).has_value(),
        "the stored intent is the EXACT identity the final record carries");
    require(rig.intentRows() == 0 && rig.recordRows() == 1 && rig.durable("SELECT count(*) FROM project_memory_segments;") == 1
                && rig.durable("SELECT count(*) FROM project_memory_segments WHERE id IN ('s1','s2','s3');") == 0,
        "the final commit stored the record AND the summary, replaced the sources and closed the intent - one transaction");
    require(rig.durable("SELECT input_tokens FROM project_memory_compaction_records;") == 40
                && rig.durable("SELECT output_tokens FROM project_memory_compaction_records;") == 15
                && rig.durable("SELECT cost_micros FROM project_memory_compaction_records;")
                    == static_cast<long long>(outcome.result.value().costMicros),
        "the usage and the amount are exactly what the response and the price gave (S2a/S2b unchanged)");
    require(outcome.intent.admission == CompactionIntentAdmission::Admitted && outcome.intent.observation == O::FinalRecordPresent
                && outcome.intent.source == Src::ConfirmedFinalWrite && rig.intents->observeCalls == 0,
        "the confirmed final write is the observation: no proof read");
    require(outcome.accounting == A::Saved, "Saved");
}

void testTerminalFailedAndCancelledRecordsCloseTheIntentToo()
{
    {
        Rig rig;
        rig.client.failure = common::Error {"provider.failed", "boom"};
        const auto outcome = rig.run();
        require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Failed, "Failed");
        require(rig.intentRows() == 0 && rig.recordRows() == 1 && rig.sourceRows() == 3, "a terminal Failed row closes the intent; the sources stay");
        require(outcome.intent.source == Src::ConfirmedFinalWrite && rig.intents->observeCalls == 0, "confirmed, not read");
    }
    {
        Rig rig;
        std::stop_source stop;
        rig.client.stopSource = &stop;   // the stop arrives while the request is in flight; the response was received
        const auto outcome = rig.run(stop.get_token());
        require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled
                    && outcome.result.value().inputTokens == 40 && outcome.accounting == A::Saved,
            "Cancelled, and the received usage is kept (S2a)");
        require(rig.intentRows() == 0 && rig.recordRows() == 1 && rig.sourceRows() == 3, "a terminal Cancelled row closes the intent; the sources stay");
        require(outcome.intent.source == Src::ConfirmedFinalWrite && rig.intents->observeCalls == 0, "confirmed, not read");
    }
}

void testAStopAfterTheAdmissionSendsNothingAndStillClosesTheIntent()
{
    Rig rig;
    std::stop_source stop;
    rig.intents->afterRecord = [&](const ProjectMemoryCompactionIntent&) { stop.request_stop(); };
    const auto outcome = rig.run(stop.get_token());
    require(rig.client.observedCalls == 0, "no request after a stop that followed the admission");
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled
                && outcome.result.value().inputTokens == 0 && outcome.result.value().costMicros == 0,
        "an ordinary Cancelled record with no usage");
    require(rig.intentRows() == 0 && rig.recordRows() == 1 && rig.sourceRows() == 3,
        "the terminal row (same id) closed the intent through the storage's own trigger; the sources are untouched");
    require(rig.intents->recordCalls == 1 && rig.intents->observeCalls == 0 && rig.intents->otherCalls == 0,
        "one admission, no best-effort close, no proof");
}

void testAStopBeforeAnythingWasSentWritesNoIntentAtAll()
{
    Rig rig;
    std::stop_source stop;
    stop.request_stop();
    const auto outcome = rig.run(stop.get_token());
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Cancelled, "Cancelled");
    require(rig.factory.resolutions == 0 && rig.client.observedCalls == 0 && rig.intents->recordCalls == 0 && rig.intentRows() == 0
                && rig.recordRows() == 1,
        "no client, no request, no intent row - and the S2a record as before");
}

// ---- 2. the accounting fallback closes it; two refused final writes leave it -----------------------------------------------------------
void testTheAccountingFallbackClosesTheIntentWithoutAProof()
{
    Rig rig;
    // A REAL commit failure: a deferred foreign key violated by a trigger on the Completed row. The Failed fallback row is not touched.
    require(rig.exec("CREATE TABLE b3_parent(id INTEGER PRIMARY KEY);"
                     "CREATE TABLE b3_child(parent_id INTEGER REFERENCES b3_parent(id) DEFERRABLE INITIALLY DEFERRED);"
                     "CREATE TRIGGER b3_fail_commit AFTER INSERT ON project_memory_compaction_records WHEN NEW.status = 'completed' "
                     "BEGIN INSERT INTO b3_child VALUES (999); END;")
                .empty(),
        "install the deferred commit failure");
    const auto outcome = rig.run();
    require(!outcome.result.success() && outcome.accounting == A::Saved && outcome.responseObserved,
        "the primary failure stays the result; the received usage is Saved by the fallback row");
    require(rig.durable("SELECT count(*) FROM project_memory_compaction_records WHERE status='failed' AND input_tokens=40;") == 1
                && rig.intentRows() == 0 && rig.sourceRows() == 3,
        "the Failed row (with the usage) closed the intent; the rolled-back commit left the sources in place");
    require(rig.client.observedCalls == 1, "no second provider call");
    require(outcome.intent.observation == O::FinalRecordPresent && outcome.intent.source == Src::ConfirmedFinalWrite
                && rig.intents->observeCalls == 0,
        "the fallback is the last final insert and it is confirmed: no proof");
}

void testTwoRefusedFinalWritesLeaveTheIntentOpenAndTheSourcesAlone()
{
    Rig rig;
    require(rig.exec("CREATE TRIGGER b3_refuse_every_record BEFORE INSERT ON project_memory_compaction_records "
                     "BEGIN SELECT RAISE(ABORT, 'b3 refuses every record'); END;")
                .empty(),
        "install the refusing trigger");
    const auto outcome = rig.run();
    require(rig.client.observedCalls == 1, "ONE provider request: the refusal of both final writes never triggers another");
    require(!outcome.result.success() && outcome.responseObserved && outcome.accounting == A::NotSaved,
        "the primary failure; the received usage is proven not stored");
    require(rig.recordRows() == 0 && rig.intentRows() == 1 && rig.sourceRows() == 3 && rig.durable("SELECT count(*) FROM project_memory_segments;") == 3,
        "no record, no summary; the intent is still open; the sources are in place");
    require(rig.intents->observeCalls == 1 && outcome.intent.observation == O::IntentPresent && outcome.intent.source == Src::AtomicPairRead,
        "ONE pair proof, on the real database, finds the intent");
    require(rig.intents->otherCalls == 0, "nothing was deleted or marked by the service");
    require(outcome.primaryNative.has_value() && outcome.fallbackNative.has_value(), "both record writes keep their own native detail");
}

// ---- 3. a final write that DID commit but reported an error --------------------------------------------------------------------------
void testACommittedFinalWriteThatReportedAnErrorIsObservedAndTheMoneyOutcomeIsNotRewritten()
{
    Rig rig;
    std::unique_ptr<CommitThenErrorHook> hook;
    // Armed only AFTER the admission, inside the request: the intent insert is an ordinary commit, the FINAL write is the one that
    // commits and reports an error. (No SQL runs inside the hook; it is removed when the call ends.)
    rig.client.onSend = [&] { hook = std::make_unique<CommitThenErrorHook>(*rig.database); };
    const auto outcome = rig.run();
    hook.reset();

    require(rig.client.observedCalls == 1, "one request");
    require(rig.recordRows() == 1 && rig.intentRows() == 0 && rig.durable("SELECT count(*) FROM project_memory_segments;") == 1,
        "TRUTH from a separate connection: the record and the summary ARE committed and the intent IS closed");
    require(!outcome.result.success(), "the call reports the error the storage gave");
    require(outcome.accounting == A::Unknown && outcome.responseObserved && outcome.observedRecord.has_value()
                && outcome.observedRecord->status == ProjectMemoryCompactionStatus::Completed,
        "the financial outcome stays what the record write proved - Unknown - and is NOT rewritten by what the intent proof found");
    require(rig.durable("SELECT count(*) FROM project_memory_compaction_records WHERE status='failed';") == 0,
        "and no accounting fallback row was invented on top of a committed record");
    require(rig.intents->observeCalls == 1 && outcome.intent.observation == O::FinalRecordPresent && outcome.intent.source == Src::AtomicPairRead,
        "ONE pair proof found the record without the intent (read, not derived)");
    require(outcome.intent.writeState == CompactionPersistState::Committed && outcome.intent.admission == CompactionIntentAdmission::Admitted,
        "and the admission was not rewritten");
}

} // namespace

int runCompactionIntentDispatchSqliteTests()
{
    try {
        testTheIntentIsDurableBeforeTheRequestAndThePrimaryCommitClosesIt();
        testTerminalFailedAndCancelledRecordsCloseTheIntentToo();
        testAStopAfterTheAdmissionSendsNothingAndStillClosesTheIntent();
        testAStopBeforeAnythingWasSentWritesNoIntentAtAll();
        testTheAccountingFallbackClosesTheIntentWithoutAProof();
        testTwoRefusedFinalWritesLeaveTheIntentOpenAndTheSourcesAlone();
        testACommittedFinalWriteThatReportedAnErrorIsObservedAndTheMoneyOutcomeIsNotRewritten();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentDispatchSqliteTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentDispatchSqliteTests: OK\n";
    return 0;
}
