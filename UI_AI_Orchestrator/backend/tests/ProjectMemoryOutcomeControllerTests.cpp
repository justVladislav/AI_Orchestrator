// C2-B/S2b-3: the REAL ProjectMemoryController over the REAL ProjectMemoryCompactionService, with scripted OBSERVED repositories and a
// controllable fake provider (no HTTP, no paid call). The repository replays what a storage reports - the production phases
// (Rollback, ProofRead, PrimaryWrite), never the service's fallback phases - so the checks prove the whole chain: service -> neutral
// outcome -> Qt projection -> ONE compactionOutcomeReported per accepted call, with the project it was accepted for. One scenario
// runs on a real SQLite database in a temporary directory (a real optimistic-concurrency conflict), never the application's database.
// Gates are condition variables, not sleeps. The checks throw, so they stay active under NDEBUG. The fakes are shared with the QML
// feature tests (test_support/ProjectMemoryOutcomeFixtures.hpp).
#include <ai/infrastructure/SqliteRepositories.hpp>
#include "../../test_support/ProjectMemoryOutcomeFixtures.hpp"

#include <QCoreApplication>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ai::ui::test_support::memory_outcome;
namespace fs = std::filesystem;

QString str(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toString(); }
QVariantMap sub(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toMap(); }
bool flag(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toBool(); }

// The rig plus what the signals of the controller said.
struct Harness : Rig {
    QObject listener;   // outlives the controller: signals are recorded here
    std::vector<std::pair<QString, QVariantMap>> outcomes;
    QStringList errors;
    QStringList segmentsChanged;
    QStringList historyChanged;
    int compactingChanged {0};

    explicit Harness(const std::string& projectId = "cp1") : Rig(projectId)
    {
        QObject::connect(controller.get(), &ProjectMemoryController::compactionOutcomeReported, &listener,
            [this](const QString& project, const QVariantMap& outcome) { outcomes.emplace_back(project, outcome); });
        QObject::connect(controller.get(), &ProjectMemoryController::errorOccurred, &listener,
            [this](const QString& code, const QString&) { errors << code; });
        QObject::connect(controller.get(), &ProjectMemoryController::segmentsChanged, &listener,
            [this](const QString& project) { segmentsChanged << project; });
        QObject::connect(controller.get(), &ProjectMemoryController::compactionHistoryChanged, &listener,
            [this](const QString& project) { historyChanged << project; });
        QObject::connect(controller.get(), &ProjectMemoryController::compactingChanged, &listener, [this] { ++compactingChanged; });
    }

    bool start(const QString& project = QStringLiteral("cp1")) { return controller->compact(project, QStringLiteral("prov1"), QStringLiteral("model1")); }
    void settle()
    {
        require(waitFor("the compaction settles", [&] { return !controller->compacting() && !outcomes.empty(); }), "the compaction did not settle");
        ai::ui::test_support::pumpOneEventCycle();
    }
    // The ONE outcome of the one accepted call.
    const QVariantMap& only() const
    {
        require(outcomes.size() == 1, "exactly one outcome per accepted call, saw " + std::to_string(outcomes.size()));
        return outcomes.front().second;
    }
};

int indexOfStage(const QVariantMap& outcome, const char* stage, int from = 0)
{
    const QVariantList list = outcome.value("diagnostics").toList();
    for (int index = from; index < list.size(); ++index) {
        if (list[index].toMap().value("stage").toString() == QString::fromLatin1(stage)) return index;
    }
    return -1;
}

// ---- the chain: service -> projection -> signal -----------------------------------------------------------------------------------------

void testAFallbackThatSavedTheUsageIsReportedAsAFailedCompactionWithSavedAccounting()
{
    Harness h;
    h.repository.commitScript.push_back(failedWrite(S::NotCommitted, C::Healthy, "persistence.transaction_failed", "constraint failed",
        native(787, "FOREIGN KEY constraint failed")));
    h.repository.insertScript.push_back(committed(C::Healthy));
    require(h.start(), "the call is accepted");
    require(h.controller->compacting() && h.controller->compactingProjectId() == "cp1", "busy while the worker runs");
    h.settle();

    require(h.client.observedCalls == 1 && h.client.sendCalls == 0, "ONE provider call through sendObserved");
    require(h.repository.commits == 1 && h.repository.inserts == 1 && h.repository.legacyCalls == 0, "one commit, ONE fallback insert, no legacy path");
    const QVariantMap& outcome = h.only();
    require(h.outcomes.front().first == "cp1" && str(outcome, "projectId") == "cp1", "the project the call was accepted for");
    require(!flag(outcome, "resultSuccess") && str(outcome, "operationState") == "failed", "the compaction failed");
    require(str(outcome, "primaryCode") == "persistence.transaction_failed" && str(outcome, "primaryMessage") == "constraint failed",
        "the PRIMARY error is the commit's, not the fallback's");
    require(str(outcome, "accounting") == "saved" && flag(outcome, "responseObserved"), "the received usage is saved");
    require(str(outcome, "connectionState") == "healthy" && flag(outcome, "connectionUsable"), "healthy was observed");
    require(flag(sub(outcome, "primaryNative"), "present") && sub(outcome, "primaryNative").value("extendedCode").toInt() == 787
        && !flag(sub(outcome, "fallbackNative"), "present"), "the primary native code only");
    require(str(outcome, "observedRecordStatus") == "failed", "the attempted record is the fallback Failed row");
    require(h.repository.insertedRecords.size() == 1, "the fallback record was inserted");
    const auto& fallback = h.repository.insertedRecords.front();
    const QVariantMap cost = sub(outcome, "observedCost");
    require(flag(cost, "available") && str(cost, "priceState") == "known" && cost.value("costMicros").toULongLong() == fallback.costMicros
        && fallback.costMicros > 0, "the amount is the one the fallback row carries (nothing recomputed)");
    require(str(outcome, "costLine").endsWith("saved") && !str(outcome, "costLine").endsWith("not saved"), "saved next to the amount");
    require(h.errors.isEmpty(), "the operation failure does not go through errorOccurred");
    require(h.segmentsChanged == QStringList {"cp1"} && h.historyChanged == QStringList {"cp1"} && h.compactingChanged == 2,
        "the existing signals fire once, with the original project");
}

void testNotSavedKeepsBothNativeCodesAndTheProductionPhasesBecomeFallbackPhases()
{
    Harness h;
    h.repository.commitScript.push_back(failedWrite(S::NotCommitted, C::Healthy, "persistence.transaction_failed", "primary text",
        native(787, "FK failed"), {{Stage::Rollback, {"persistence.rollback_failed", "primary rollback"}}}));
    h.repository.insertScript.push_back(failedWrite(S::NotCommitted, C::Healthy, "persistence.write_failed", "fallback text",
        native(1555, "UNIQUE failed"), {{Stage::Rollback, {"persistence.rollback_failed", "fallback rollback"}}}));
    require(h.start(), "accepted");
    h.settle();
    const QVariantMap& outcome = h.only();
    require(str(outcome, "accounting") == "not_saved" && str(outcome, "accountingCode") == "project_memory.accounting_not_saved", "not saved");
    require(str(outcome, "primaryCode") == "persistence.transaction_failed" && str(outcome, "primaryMessage") == "primary text", "primary untouched");
    require(sub(outcome, "primaryNative").value("extendedCode").toInt() == 787 && sub(outcome, "fallbackNative").value("extendedCode").toInt() == 1555,
        "two native details, never merged");
    const int rollback = indexOfStage(outcome, "rollback");
    const int fallbackWrite = indexOfStage(outcome, "fallback_write");
    const int fallbackRollback = indexOfStage(outcome, "fallback_rollback");
    require(rollback >= 0 && fallbackWrite > rollback && fallbackRollback > fallbackWrite,
        "the production stages arrive as written, the fallback's own causes are renamed to the fallback phase, in order");
    const QVariantList list = outcome.value("diagnostics").toList();
    require(list[fallbackWrite].toMap().value("message").toString() == "fallback text", "the fallback insert's ORIGINAL error is its own cause");
    require(list[rollback].toMap().value("message").toString() == "primary rollback"
        && list[fallbackRollback].toMap().value("message").toString() == "fallback rollback", "each rollback keeps its own text");
    require(str(outcome, "costLine").endsWith("not saved"), "the amount says 'not saved'");
    require(h.client.observedCalls == 1 && h.repository.inserts == 1, "one send, one fallback attempt");
    // R1: the unconfirmed fallback row (its fixed text claims the usage is kept) is metadata of the attempt, never a recorded reason.
    require(str(outcome, "terminalOrigin") == "attempted" && str(outcome, "terminalText") == "The compaction attempt failed; its record is not confirmed saved."
        && str(outcome, "terminalDetailText").contains("not confirmed saved") && !str(outcome, "terminalText").toLower().contains("kept"),
        "NotSaved: the unconfirmed fallback candidate is a neutral attempt status, its words only in the details");
}

// A fallback whose own write is UNKNOWN: the same production Failed candidate, accounting Unknown, no claim either way.
void testAFallbackWhoseWriteIsUnknownIsUnknownAndNotWordedAsRecorded()
{
    Harness h;
    h.repository.commitScript.push_back(failedWrite(S::NotCommitted, C::Healthy, "persistence.transaction_failed", "primary text", native(787, "FK failed")));
    h.repository.insertScript.push_back(failedWrite(S::Unknown, C::Healthy, "persistence.commit_unknown", "fallback outcome unknown"));
    require(h.start(), "accepted");
    h.settle();
    const QVariantMap& outcome = h.only();
    require(h.repository.inserts == 1 && h.client.observedCalls == 1, "one send, one fallback attempt");
    require(str(outcome, "observedRecordStatus") == "failed" && str(outcome, "terminalMessage").contains("kept as a failed attempt"),
        "the candidate really is the production fallback row");
    require(str(outcome, "accounting") == "unknown" && str(outcome, "primaryCode") == "persistence.transaction_failed", "unknown; primary kept");
    require(str(outcome, "terminalOrigin") == "attempted" && str(outcome, "terminalText") == "The compaction attempt failed; its record is not confirmed saved.", "not worded as recorded on the main screen");
    for (const char* key : {"headline", "primaryMessage", "accountingText", "costLine", "terminalText"}) {
        const QString shown = str(outcome, key).toLower();
        require(!shown.contains("kept") && !shown.contains("failed attempt") && !shown.contains("recorded"), "no main text claims a record");
    }
    require(str(outcome, "costLine").endsWith("saving not confirmed"), "the amount is qualified");
}

void testAnIsolatedConnectionStopsTheFallbackAndIsShownApartFromTheVerdict()
{
    Harness h;
    h.repository.commitScript.push_back(failedWrite(S::NotCommitted, C::Quarantined));
    require(h.start(), "accepted");
    h.settle();
    const QVariantMap& outcome = h.only();
    require(h.repository.inserts == 0, "no fallback on a connection that is not explicitly healthy");
    require(str(outcome, "accounting") == "not_saved" && str(outcome, "connectionState") == "quarantined"
        && flag(outcome, "connectionNeedsAttention") && !flag(outcome, "connectionUsable"), "not saved, and the isolation is visible");
}

void testAnUnknownCommitIsUnknownAndNeverNotSaved()
{
    Harness h;
    h.repository.commitScript.push_back(failedWrite(S::Unknown, C::Healthy, "persistence.commit_unknown", "outcome unknown"));
    require(h.start(), "accepted");
    h.settle();
    const QVariantMap& outcome = h.only();
    require(h.repository.inserts == 0, "no fallback when it is not proven that nothing was stored");
    require(str(outcome, "accounting") == "unknown" && str(outcome, "accountingCode") == "project_memory.accounting_unknown"
        && str(outcome, "primaryCode") == "persistence.commit_unknown", "unknown, with the primary code intact");
    require(str(outcome, "costLine").endsWith("saving not confirmed"), "the amount is next to 'saving not confirmed'");
    require(str(outcome, "observedRecordStatus") == "completed", "the attempted record is the Completed one, not rewritten into a Failed one");
}

void testAnUnobservedConnectionIsNotHealthy()
{
    Harness h;
    h.repository.commitScript.push_back(failedWrite(S::NotCommitted, std::nullopt));
    require(h.start(), "accepted");
    h.settle();
    const QVariantMap& outcome = h.only();
    require(h.repository.inserts == 0, "an unobserved connection is not explicitly healthy: no fallback");
    require(str(outcome, "connectionState") == "unobserved" && outcome.value("connectionUsable").isNull(), "unobserved, usable null");
}

void testAValidationFailureInsideTheWorkerIsAStructuredOutcomeWithNoSend()
{
    {
        Harness h;
        require(h.start(QStringLiteral("ghost")), "the call is accepted: the project is only checked inside the worker");
        h.settle();
        const QVariantMap& outcome = h.only();
        require(h.outcomes.front().first == "ghost" && str(outcome, "projectId") == "ghost", "the project of the call, though no record exists");
        require(str(outcome, "operationState") == "failed" && !str(outcome, "primaryCode").isEmpty() && !str(outcome, "primaryMessage").isEmpty(),
            "the validation error is the primary error");
        require(str(outcome, "accounting") == "not_applicable" && !flag(outcome, "responseObserved") && !flag(sub(outcome, "observedCost"), "available"),
            "no response, nothing to account for");
        require(h.client.observedCalls == 0 && h.repository.commits == 0 && h.repository.inserts == 0, "no provider call, no record");
        require(h.errors.isEmpty(), "not sent through errorOccurred");
        require(h.segmentsChanged == QStringList {"ghost"} && h.historyChanged == QStringList {"ghost"}, "the existing signals carry the original project");
    }
    {
        Harness h;
        h.memory.segments.resize(1);   // fewer than two segments
        require(h.start(), "accepted");
        h.settle();
        require(str(h.only(), "primaryCode") == "project_memory.insufficient_segments_to_compact" && h.client.observedCalls == 0, "insufficient segments");
    }
}

void testAProviderFailureIsAReturnedFailedRecordNotASuccessfulCompaction()
{
    Harness h;
    h.client.failure = common::Error {"provider.http_error", "upstream 500"};
    h.repository.insertScript.push_back(committed(C::Healthy));
    require(h.start(), "accepted");
    h.settle();
    const QVariantMap& outcome = h.only();
    require(flag(outcome, "resultSuccess") && str(outcome, "operationState") == "failed" && str(outcome, "recordStatus") == "failed",
        "the call returned a Failed record: success of the call, failure of the compaction");
    require(str(outcome, "primaryCode").isEmpty() && !str(outcome, "terminalCode").isEmpty(), "the reason is the record's terminal one");
    require(str(outcome, "accounting") == "not_applicable" && !flag(outcome, "responseObserved"), "no response was received");
    require(h.client.observedCalls == 1 && h.repository.inserts == 1 && h.repository.commits == 0, "one send, one terminal insert");
}

// ---- lifecycle: stop, a late success, busy, destruction ---------------------------------------------------------------------------------

void testAnAcceptedCancellationIsReportedAsCancelledNotRenamed()
{
    Harness h;
    h.client.hold = true;
    ProviderChatResponse discarded;
    discarded.content = "discarded";
    discarded.usage.inputTokens = 40;
    discarded.usage.outputTokens = 15;
    h.client.discarded = discarded;
    h.repository.insertScript.push_back(committed(C::Healthy));
    require(h.start(), "accepted");
    require(waitFor("the provider call is in flight", [&] { return h.client.entered.load(); }), "provider entered");
    require(h.outcomes.empty(), "nothing is reported before the end, and Stop clears nothing in advance");
    h.controller->stopCompaction();
    h.settle();
    const QVariantMap& outcome = h.only();
    require(flag(outcome, "resultSuccess") && str(outcome, "operationState") == "cancelled" && str(outcome, "recordStatus") == "cancelled",
        "cancelled stays cancelled");
    require(flag(outcome, "responseObserved") && str(outcome, "accounting") == "saved", "the discarded response's usage is saved on the Cancelled record");
    require(flag(sub(outcome, "observedCost"), "available") && str(outcome, "costLine").endsWith("saved"), "its amount is shown");
    require(h.client.observedCalls == 1, "one send");
}

void testALateRealSaveIsReportedAsSavedEvenIfStopArrivedBeforeIt()
{
    Harness h;
    h.repository.holdCommit = true;
    h.repository.commitScript.push_back(committed(C::Healthy));
    require(h.start(), "accepted");
    require(waitFor("the commit is in flight", [&] { return h.repository.commitEntered.load(); }), "commit entered");
    h.controller->stopCompaction();   // the stop arrives while the commit is in flight
    h.repository.releaseCommit();
    h.settle();
    const QVariantMap& outcome = h.only();
    require(flag(outcome, "resultSuccess") && str(outcome, "operationState") == "completed" && str(outcome, "accounting") == "saved",
        "what was really saved is shown as saved");
    require(h.client.observedCalls == 1 && h.repository.commits == 1 && h.repository.inserts == 0, "one send, one commit, no fallback");
}

void testABusyCallIsRefusedSynchronouslyAndProducesNoOutcome()
{
    Harness h;
    h.client.hold = true;
    h.repository.commitScript.push_back(committed(C::Healthy));
    require(h.start(), "the first call is accepted");
    require(waitFor("the provider call is in flight", [&] { return h.client.entered.load(); }), "provider entered");
    require(!h.start(), "the second call is refused");
    require(h.errors == QStringList {"project_memory.busy"}, "the synchronous refusal keeps errorOccurred");
    require(h.outcomes.empty(), "a refusal is not an accepted call: no outcome");
    h.client.release();
    h.settle();
    require(!h.only().isEmpty() && h.client.observedCalls == 1 && h.errors.size() == 1,
        "exactly one outcome and one send for the one accepted call; the refusal added no second error");
}

void testTheControllerCanBeDestroyedWhileTheProviderCallIsInFlight()
{
    Harness h;
    h.client.hold = true;
    h.repository.insertScript.push_back(committed(C::Healthy));
    require(h.start(), "accepted");
    require(waitFor("the provider call is in flight", [&] { return h.client.entered.load(); }), "provider entered");
    h.controller.reset();   // request_stop + join: the call ends on the stop request, not on a sleep
    for (int cycle = 0; cycle < 50; ++cycle) ai::ui::test_support::pumpOneEventCycle();
    require(h.outcomes.empty() && h.errors.empty() && h.segmentsChanged.isEmpty(),
        "a queued completion never reaches a destroyed controller");
    require(h.client.observedCalls == 1 && h.repository.commits == 0, "one send, no commit after the stop");
}

void testEachAcceptedCallReportsItsOwnProjectEvenWhenAnotherIsSelectedLater()
{
    // Two sequential accepted calls for two projects: each signal carries the project it was accepted for.
    Harness h;
    require(h.projects.insert(projectNamed("cp2")).success(), "setup: second project");
    h.repository.commitScript.push_back(committed(C::Healthy));
    h.repository.commitScript.push_back(failedWrite(S::Unknown, C::Healthy, "persistence.commit_unknown", "unknown"));
    require(h.start(QStringLiteral("cp1")), "first accepted");
    h.settle();
    require(h.outcomes.size() == 1 && h.outcomes[0].first == "cp1" && str(h.outcomes[0].second, "projectId") == "cp1", "first outcome: cp1");
    require(h.start(QStringLiteral("cp2")), "second accepted");
    require(waitFor("the second outcome", [&] { return h.outcomes.size() == 2 && !h.controller->compacting(); }), "second settled");
    require(h.outcomes[1].first == "cp2" && str(h.outcomes[1].second, "projectId") == "cp2"
        && str(h.outcomes[1].second, "accounting") == "unknown", "second outcome: cp2, with its own verdict");
    require(str(h.outcomes[0].second, "accounting") == "saved", "the first outcome is not changed by the second");
}

// ---- the real database: a real optimistic-concurrency conflict ---------------------------------------------------------------------------

struct TempDir {
    fs::path root;
    TempDir()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path() / ("aio-s2b3-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                            + "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void testARealConflictSavesOneFailedAccountingRowAndTheSignalAndReadbackAgree()
{
    using ai::infrastructure::SqliteDatabase;
    using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
    using ai::infrastructure::SqliteProjectMemoryRepository;
    using ai::infrastructure::SqliteProjectRepository;

    TempDir temp;
    auto opened = SqliteDatabase::open(temp.root / "state.sqlite3");
    require(opened.success(), "open the temporary database");
    std::shared_ptr<SqliteDatabase> database = std::move(opened.value());
    SqliteProjectRepository projects(database);
    SqliteProjectMemoryRepository memory(database);
    SqliteProjectMemoryCompactionRepository compaction(database);
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    seedCatalog(providers, models);
    require(projects.insert(projectNamed("cp1")).success(), "seed the project");
    for (const char* id : {"s1", "s2", "keep"}) require(memory.insertMemorySegment(segmentFor("cp1", id)).success(), "seed a segment");

    ControlledClient client;
    ClientFactory factory {client};
    // While the provider thinks, somebody clears the memory and writes a newer segment: the commit's expectation is stale.
    client.whileInFlight = [&] {
        require(memory.removeMemorySegments("cp1").success(), "the concurrent clear");
        require(memory.insertMemorySegment(segmentFor("cp1", "newer")).success(), "the concurrent newer segment");
    };
    ProjectMemoryService memoryService(memory);
    ProviderCatalogService catalog(providers, models);
    ai::application::test_support::ScriptedCompactionIntentRepository intents;
    ProjectMemoryCompactionService service(memory, compaction, intents, projects, providers, models, factory);
    ai::application::CompactionIntentReadModelService intentReader(intents);
    ProjectMemoryController controller(memoryService, service, catalog, intentReader);
    QObject listener;
    std::vector<std::pair<QString, QVariantMap>> outcomes;
    QStringList errors;
    QObject::connect(&controller, &ProjectMemoryController::compactionOutcomeReported, &listener,
        [&](const QString& project, const QVariantMap& outcome) { outcomes.emplace_back(project, outcome); });
    QObject::connect(&controller, &ProjectMemoryController::errorOccurred, &listener, [&](const QString& code, const QString&) { errors << code; });

    require(controller.compact(QStringLiteral("cp1"), QStringLiteral("prov1"), QStringLiteral("model1")), "accepted");
    require(waitFor("the real compaction settles", [&] { return !controller.compacting() && !outcomes.empty(); }), "settled");
    require(outcomes.size() == 1 && client.observedCalls == 1 && client.sendCalls == 0, "one outcome, one provider call");
    const QVariantMap& outcome = outcomes.front().second;
    require(!flag(outcome, "resultSuccess") && str(outcome, "primaryCode") == "project_memory.compaction_conflict", "the primary failure is the conflict");
    require(str(outcome, "accounting") == "saved" && str(outcome, "connectionState") == "healthy" && flag(outcome, "responseObserved"), "saved, healthy");
    require(!flag(sub(outcome, "primaryNative"), "present") && !flag(sub(outcome, "fallbackNative"), "present"),
        "a conflict is not a SQLite failure: no native code is invented");
    const QVariantMap cost = sub(outcome, "observedCost");
    require(flag(cost, "available") && str(cost, "priceState") == "known" && cost.value("costMicros").toULongLong() > 0, "the received estimate");
    require(errors.isEmpty(), "no errorOccurred for the operation");

    // The read-back of the history shows the SAME stored estimate, in exactly one row.
    const QVariantMap snapshot = controller.compactionHistorySnapshot(QStringLiteral("cp1"));
    const QVariantList rows = snapshot.value("rows").toList();
    require(str(snapshot, "state") == "available" && rows.size() == 1, "exactly ONE Failed accounting row is in the history");
    const QVariantMap row = rows.front().toMap();
    require(str(row, "status") == "failed" && str(row, "resultSegmentId").isEmpty(), "a Failed row without a result segment");
    require(str(row, "priceState") == str(cost, "priceState") && str(row, "costText") == str(cost, "costText")
        && row.value("costMicros").toULongLong() == cost.value("costMicros").toULongLong(),
        "the signal and the read-back carry the same stored amount");
    require(row.value("inputTokens").toULongLong() == sub(outcome, "observedTokens").value("inputTokens").toULongLong()
        && row.value("outputTokens").toULongLong() == sub(outcome, "observedTokens").value("outputTokens").toULongLong(), "and the same usage");
}

} // namespace

int runProjectMemoryOutcomeControllerTests()
{
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"testAFallbackThatSavedTheUsageIsReportedAsAFailedCompactionWithSavedAccounting",
            testAFallbackThatSavedTheUsageIsReportedAsAFailedCompactionWithSavedAccounting},
        {"testNotSavedKeepsBothNativeCodesAndTheProductionPhasesBecomeFallbackPhases",
            testNotSavedKeepsBothNativeCodesAndTheProductionPhasesBecomeFallbackPhases},
        {"testAFallbackWhoseWriteIsUnknownIsUnknownAndNotWordedAsRecorded", testAFallbackWhoseWriteIsUnknownIsUnknownAndNotWordedAsRecorded},
        {"testAnIsolatedConnectionStopsTheFallbackAndIsShownApartFromTheVerdict", testAnIsolatedConnectionStopsTheFallbackAndIsShownApartFromTheVerdict},
        {"testAnUnknownCommitIsUnknownAndNeverNotSaved", testAnUnknownCommitIsUnknownAndNeverNotSaved},
        {"testAnUnobservedConnectionIsNotHealthy", testAnUnobservedConnectionIsNotHealthy},
        {"testAValidationFailureInsideTheWorkerIsAStructuredOutcomeWithNoSend", testAValidationFailureInsideTheWorkerIsAStructuredOutcomeWithNoSend},
        {"testAProviderFailureIsAReturnedFailedRecordNotASuccessfulCompaction", testAProviderFailureIsAReturnedFailedRecordNotASuccessfulCompaction},
        {"testAnAcceptedCancellationIsReportedAsCancelledNotRenamed", testAnAcceptedCancellationIsReportedAsCancelledNotRenamed},
        {"testALateRealSaveIsReportedAsSavedEvenIfStopArrivedBeforeIt", testALateRealSaveIsReportedAsSavedEvenIfStopArrivedBeforeIt},
        {"testABusyCallIsRefusedSynchronouslyAndProducesNoOutcome", testABusyCallIsRefusedSynchronouslyAndProducesNoOutcome},
        {"testTheControllerCanBeDestroyedWhileTheProviderCallIsInFlight", testTheControllerCanBeDestroyedWhileTheProviderCallIsInFlight},
        {"testEachAcceptedCallReportsItsOwnProjectEvenWhenAnotherIsSelectedLater",
            testEachAcceptedCallReportsItsOwnProjectEvenWhenAnotherIsSelectedLater},
        {"testARealConflictSavesOneFailedAccountingRowAndTheSignalAndReadbackAgree",
            testARealConflictSavesOneFailedAccountingRowAndTheSignalAndReadbackAgree},
    };
    int failures = 0;
    for (const auto& item : cases) {
        try {
            item.run();
        } catch (const std::exception& exception) {
            std::cerr << "FAILED " << item.name << ": " << exception.what() << std::endl;
            ++failures;
        }
    }
    if (failures == 0) std::cerr << "[project-memory-outcome-controller] " << std::size(cases) << " cases passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
