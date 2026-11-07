// B3-III: the Qt boundary of the compaction recording coverage.
//  - the projections: an absent count stays absent (never 0), a real zero stays a zero, the requested period survives a failed read, an
//    unfinished-protocol row has no amount at all;
//  - ProjectCostController: the coverage comes from the SAME read as the money (one intent read per refresh), is cleared WITH the summary,
//    and an intent read failure leaves the money exactly as it was;
//  - ProjectMemoryController: the history snapshot has a separate intent section whose counts and rows come from ONE read, whose error
//    travels in the snapshot and never through errorOccurred, and which is independent of the record read;
//  - the startup recovery report: a read-only database-scope map that keeps every cause bound to its native detail, and the wiring on a
//    real BackendBridge (a read-only controller with no invokable method).
// The controllers run over the REAL services and scripted ports (labelled); no network, no paid call. The checks throw (active under NDEBUG).
#include "../../test_support/ProjectMemoryOutcomeFixtures.hpp"

#include <ProjectCostTestSupport.hpp>

#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/CompactionCoverageProjection.hpp>
#include <ai/ui/CompactionIntentRecoveryController.hpp>
#include <ai/ui/CompactionIntentRecoveryProjection.hpp>
#include <ai/ui/ProjectCostController.hpp>
#include <ai/ui/ProjectMemoryController.hpp>

#include <QCoreApplication>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace app = ai::application;
namespace fs = std::filesystem;
namespace cov = ai::ui::compaction_coverage;
namespace rec = ai::ui::compaction_recovery;
using Cov = app::CompactionCoverageState;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point ms(std::int64_t value) { return std::chrono::system_clock::time_point(std::chrono::milliseconds(value)); }

app::ProjectMemoryCompactionIntent intentRow(const std::string& id, const std::string& project, app::CompactionIntentState state, std::int64_t created)
{
    app::ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = project;
    value.providerId = "prov";
    value.modelId = "model";
    value.sourceSegmentIds = {"s1", "s2", "s3"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = ms(created);
    value.state = state;
    if (state == app::CompactionIntentState::Unresolved) value.unresolvedMarkedAt = ms(created + 10);
    return value;
}

bool isNull(const QVariant& value) { return !value.isValid() || value.isNull(); }

// ---- 1. projections ---------------------------------------------------------------------------------------------------------------------
void testTheCoverageMapNeverTurnsAnAbsenceIntoAZero()
{
    const QVariantMap none = cov::coverageMap(std::nullopt);
    require(none.value("state").toString() == "none" && isNull(none.value("openOperations")) && isNull(none.value("unresolvedOperations"))
                && !none.value("hasCounts").toBool(),
        "nothing measured: state none and no counts");

    app::CompactionCoverageObservation zero;
    zero.state = Cov::Available;
    zero.openOperations = 0;
    zero.unresolvedOperations = 0;
    const QVariantMap zeros = cov::coverageMap(zero);
    require(zeros.value("state").toString() == "available" && !isNull(zeros.value("openOperations")) && zeros.value("openOperations").toULongLong() == 0
                && !isNull(zeros.value("unresolvedOperations")) && zeros.value("hasCounts").toBool() && zeros.value("scope").toString() == "all_time",
        "a real zero stays a PRESENT zero");

    app::CompactionCoverageObservation failed;
    failed.state = Cov::Unavailable;
    failed.error = ai::common::Error {"persistence.read_failed", "a damaged intent row"};
    failed.windowed = true;
    failed.fromInclusive = ms(1'700'000'000'000);
    failed.openOperations = 7;   // even a stray value must not be shown for an unavailable observation
    const QVariantMap unavailable = cov::coverageMap(failed);
    require(unavailable.value("state").toString() == "unavailable" && isNull(unavailable.value("openOperations")) && isNull(unavailable.value("unresolvedOperations"))
                && !unavailable.value("hasCounts").toBool() && unavailable.value("errorCode").toString() == "persistence.read_failed"
                && unavailable.value("errorMessage").toString() == "a damaged intent row",
        "unavailable: no counts, the original error");
    require(unavailable.value("windowed").toBool() && unavailable.value("scope").toString() == "period" && unavailable.value("hasFrom").toBool()
                && !unavailable.value("hasTo").toBool() && !unavailable.value("fromText").toString().isEmpty() && unavailable.value("toText").toString().isEmpty(),
        "the requested period survives a failed read; a bound that was not given stays empty");
}

void testAnUnfinishedProtocolRowHasNoMoneyAtAll()
{
    const QVariantMap open = cov::intentRowMap(intentRow("op-1", "p1", app::CompactionIntentState::Open, 1'700'000'000'000));
    const QVariantMap unresolved = cov::intentRowMap(intentRow("op-2", "p1", app::CompactionIntentState::Unresolved, 1'700'000'000'000));
    for (const char* forbidden : {"costMicros", "costText", "priceState", "priceIncomplete", "inputTokens", "outputTokens", "summary", "response"}) {
        require(!open.contains(forbidden) && !unresolved.contains(forbidden), std::string("an unfinished protocol has no '") + forbidden + "'");
    }
    require(open.value("state").toString() == "open" && open.value("markedAt").toString().isEmpty() && open.value("sourceCount").toULongLong() == 3
                && open.value("id").toString() == "op-1" && open.value("modelId").toString() == "model" && open.value("providerId").toString() == "prov",
        "an open row: ids as data, no mark");
    require(unresolved.value("state").toString() == "unresolved" && !unresolved.value("markedAt").toString().isEmpty(), "an unresolved row has its mark");
}

// ---- 2. the cost controller ------------------------------------------------------------------------------------------------------------------
struct CostRig {
    project_cost_test::Fixture fixture;
    app::InMemoryModelRepository names;
    app::ProviderCatalogService catalogService {fixture.providers, names};
    ProjectCostController controller {fixture.service, catalogService};

    CostRig()
    {
        app::Model model;
        model.id = "model";
        model.providerId = "prov";
        model.apiModelId = "model";
        model.displayName = "model";
        model.contextWindowTokens = 100'000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 2'000'000;
        model.outputPriceMicrosPerMillionTokens = 10'000'000;
        require(fixture.catalog.insert(model).success(), "seed the catalog");
        require(fixture.ledger.insertExecution(project_cost_test::ledgerRow("l1", "p1", "prov", "model", 10, 10, 900, true)).success(), "seed the ledger");
    }
};

void testTheCostControllerCarriesTheCoverageOfTheSameReadAndNeverReadsTwice()
{
    CostRig rig;
    rig.fixture.intents.listRows = {intentRow("a", "p1", app::CompactionIntentState::Open, 1000), intentRow("b", "p1", app::CompactionIntentState::Unresolved, 2000),
        intentRow("x", "p2", app::CompactionIntentState::Open, 1000)};
    require(rig.controller.coverage().value("state").toString() == "none", "idle: nothing measured");
    require(rig.controller.refreshProject("p1"), "the refresh succeeds");
    const QVariantMap coverage = rig.controller.coverage();
    require(coverage.value("state").toString() == "available" && coverage.value("openOperations").toULongLong() == 1
                && coverage.value("unresolvedOperations").toULongLong() == 1 && rig.fixture.intents.listCalls == 1,
        "the coverage of the same snapshot, from ONE intent read (the controller reads nothing itself)");
    require(rig.controller.loadState() == "available" && rig.controller.costText() == "$0.0009", "the money is the ledger's");

    // An intent read failure leaves the money exactly as it was; the coverage is unavailable with the ORIGINAL error and no counts.
    rig.fixture.intents.listFailure = ai::common::Error {"persistence.read_failed", "intents unreadable"};
    require(rig.controller.refreshProject("p1"), "the estimate still succeeds");
    const QVariantMap failed = rig.controller.coverage();
    require(rig.controller.loadState() == "available" && rig.controller.costText() == "$0.0009" && rig.controller.priceState() == "known",
        "the money is untouched by the intent failure");
    require(failed.value("state").toString() == "unavailable" && isNull(failed.value("openOperations")) && isNull(failed.value("unresolvedOperations"))
                && failed.value("errorCode").toString() == "persistence.read_failed" && failed.value("errorMessage").toString() == "intents unreadable",
        "unavailable: no stale counts, no zeros, the original error");

    // recovery, then a money failure: the coverage is cleared WITH the summary
    rig.fixture.intents.listFailure.reset();
    require(rig.controller.refreshProject("p1") && rig.controller.coverage().value("state").toString() == "available", "recovered");
    rig.fixture.ledger.listFailure = project_cost_test::scripted("test.ledger_down");
    require(!rig.controller.refreshProject("p1"), "the required money read fails");
    require(rig.controller.loadState() == "unavailable" && rig.controller.coverage().value("state").toString() == "none"
                && isNull(rig.controller.coverage().value("openOperations")),
        "a money failure clears the coverage together with the summary (no stale counts next to an unavailable estimate)");
    rig.fixture.ledger.listFailure.reset();
    require(rig.controller.refreshProject("p1") && rig.controller.coverage().value("state").toString() == "available", "recovered again");
    rig.controller.clear();
    require(rig.controller.coverage().value("state").toString() == "none" && isNull(rig.controller.coverage().value("openOperations")), "clear: nothing measured");
}

void testTheCostControllerKeepsTheRequestedPeriodAndOnlyOneReadPerRefresh()
{
    CostRig rig;
    rig.fixture.intents.listRows = {intentRow("a", "p1", app::CompactionIntentState::Open, 1'700'000'000'500)};
    const QDateTime from = QDateTime::fromMSecsSinceEpoch(1'700'000'000'000);
    const QDateTime to = QDateTime::fromMSecsSinceEpoch(1'700'000'001'000);
    require(rig.controller.refreshProjectRange("p1", from, to), "a range refresh");
    const QVariantMap coverage = rig.controller.coverage();
    require(coverage.value("windowed").toBool() && coverage.value("scope").toString() == "period" && coverage.value("hasFrom").toBool() && coverage.value("hasTo").toBool()
                && coverage.value("openOperations").toULongLong() == 1 && rig.fixture.intents.listCalls == 1,
        "windowed, the bounds are kept, one read");
    require(!rig.controller.refreshProjectRange("p1", to, from) && rig.fixture.intents.listCalls == 1, "an invalid range reads nothing, not even the intents");
    require(rig.controller.coverage().value("state").toString() == "none", "and leaves no coverage");
}

// ---- 3. the history snapshot --------------------------------------------------------------------------------------------------------------------
void testTheHistorySnapshotHasAnIndependentIntentSectionFromOneRead()
{
    using namespace ai::ui::test_support::memory_outcome;
    Rig rig;
    rig.intents.listRows = {intentRow("a", "cp1", app::CompactionIntentState::Open, 1000), intentRow("b", "cp1", app::CompactionIntentState::Unresolved, 2000),
        intentRow("c", "cp1", app::CompactionIntentState::Unresolved, 3000)};
    int errors = 0;
    QStringList errorCodes;
    QObject::connect(rig.controller.get(), &ProjectMemoryController::errorOccurred, rig.controller.get(),
        [&](const QString& code, const QString&) { ++errors; errorCodes.push_back(code); });

    QVariantMap snapshot = rig.controller->compactionHistorySnapshot("cp1");
    QVariantMap section = snapshot.value("intents").toMap();
    QVariantMap coverage = section.value("coverage").toMap();
    require(snapshot.value("state").toString() == "available" && section.value("state").toString() == "available" && rig.intents.listCalls == 1,
        "both sections available; ONE intent read for the snapshot");
    require(coverage.value("openOperations").toULongLong() == 1 && coverage.value("unresolvedOperations").toULongLong() == 2
                && section.value("rows").toList().size() == 3,
        "the counts and the rows of the section are those of that one read");
    require(errors == 0, "no error");

    // the intent read fails: the record section is untouched, the error is IN the snapshot and nowhere else
    rig.intents.listFailure = ai::common::Error {"persistence.read_failed", "storage boom"};
    snapshot = rig.controller->compactionHistorySnapshot("cp1");
    section = snapshot.value("intents").toMap();
    require(snapshot.value("state").toString() == "available" && snapshot.value("errorCode").toString().isEmpty(), "the record section is untouched");
    require(section.value("state").toString() == "unavailable" && section.value("rows").toList().isEmpty() && isNull(section.value("coverage").toMap().value("openOperations"))
                && section.value("errorCode").toString() == "persistence.read_failed" && section.value("errorMessage").toString() == "storage boom",
        "the intent section: unavailable, no rows, no counts, the original error");
    require(errors == 0, "an intent failure is NEVER reported through errorOccurred (that channel belongs to the record, segment and operation errors)");

    // the record read fails while the intent read works
    rig.intents.listFailure.reset();
    rig.repository.listFails = true;
    snapshot = rig.controller->compactionHistorySnapshot("cp1");
    section = snapshot.value("intents").toMap();
    require(snapshot.value("state").toString() == "unavailable" && snapshot.value("rows").toList().isEmpty() && errors == 1
                && errorCodes.front() == "persistence.read_failed",
        "the record section is unavailable and reported exactly once, as before");
    require(section.value("state").toString() == "available" && section.value("rows").toList().size() == 3,
        "...while the unfinished protocols are still visible");
    rig.repository.listFails = false;

    // no project: nothing is read, nothing is measured
    const int readsBefore = rig.intents.listCalls;
    snapshot = rig.controller->compactionHistorySnapshot("");
    require(rig.intents.listCalls == readsBefore && snapshot.value("intents").toMap().value("state").toString() == "none"
                && snapshot.value("intents").toMap().value("rows").toList().isEmpty() && isNull(snapshot.value("intents").toMap().value("coverage").toMap().value("openOperations")),
        "an empty project reads nothing and shows no counts");
}

// ---- 4. the startup recovery report -----------------------------------------------------------------------------------------------------------
void testTheRecoveryMapIsDatabaseScopeAndKeepsEveryCauseBoundToItsNative()
{
    const QVariantMap absent = rec::reportMap(std::nullopt, std::nullopt);
    require(absent.value("scope").toString() == "database" && !absent.value("applicable").toBool() && absent.value("status").toString() == "none"
                && isNull(absent.value("inspected")) && isNull(absent.value("markedUnresolved")) && !absent.value("firstNative").toMap().value("present").toBool()
                && !absent.value("needsAttention").toBool() && !absent.value("hasNotes").toBool(),
        "no report: not applicable, not 'complete', no counts at all");

    app::CompactionIntentRecoveryReport report;
    report.status = app::CompactionIntentRecoveryStatus::Complete;
    report.inspected = 2;
    report.markedUnresolved = 2;
    report.writeCauses.push_back({ai::common::Error {"persistence.commit_unknown", "the commit may or may not be stored"}, app::CompactionNativeDetail {10, 522, "disk I/O error"}});
    report.diagnostics.push_back({app::CompactionDiagnosticStage::IntentRecovery, ai::common::Error {"persistence.rollback_failed", "no transaction"}});
    const QVariantMap complete = rec::reportMap(report, std::nullopt);
    require(complete.value("applicable").toBool() && complete.value("status").toString() == "complete" && !complete.value("needsAttention").toBool()
                && complete.value("hasNotes").toBool() && complete.value("hasActivity").toBool() && !complete.value("hasFirstError").toBool()
                && complete.value("inspected").toULongLong() == 2 && complete.value("conflicts").toULongLong() == 0 && !isNull(complete.value("conflicts")),
        "a Complete report with notes stays Complete; its counters are real numbers");
    const QVariantList causes = complete.value("writeCauses").toList();
    require(causes.size() == 1 && causes.front().toMap().value("code").toString() == "persistence.commit_unknown"
                && causes.front().toMap().value("native").toMap().value("present").toBool()
                && causes.front().toMap().value("native").toMap().value("extendedCode").toInt() == 522
                && causes.front().toMap().value("native").toMap().value("message").toString() == "disk I/O error",
        "a write cause keeps ITS native detail, also in a Complete report");
    require(complete.value("diagnostics").toList().size() == 1 && complete.value("diagnostics").toList().front().toMap().value("stage").toString() == "intent_recovery",
        "the diagnostics keep their stage");

    app::CompactionIntentRecoveryReport partial;
    partial.status = app::CompactionIntentRecoveryStatus::Partial;
    partial.inspected = 3;
    partial.unconfirmed = 1;
    partial.firstError = ai::common::Error {"persistence.connection_unusable", "nothing was read"};
    partial.firstNative = app::CompactionNativeDetail {5, 5, "database is locked"};
    partial.diagnosticsIncomplete = true;
    const QVariantMap partialMap = rec::reportMap(partial, partial.firstError);
    require(partialMap.value("status").toString() == "partial" && partialMap.value("needsAttention").toBool() && partialMap.value("hasFirstError").toBool()
                && partialMap.value("firstErrorCode").toString() == "persistence.connection_unusable"
                && partialMap.value("firstNative").toMap().value("present").toBool() && partialMap.value("diagnosticsIncomplete").toBool()
                && partialMap.value("channelErrorCode").toString() == "persistence.connection_unusable",
        "Partial: the status, the original first error with its native detail, the incomplete flag and the separate channel error");

    app::CompactionIntentRecoveryReport clean;
    clean.status = app::CompactionIntentRecoveryStatus::Complete;
    const QVariantMap cleanMap = rec::reportMap(clean, std::nullopt);
    require(!cleanMap.value("hasNotes").toBool() && !cleanMap.value("hasActivity").toBool() && !cleanMap.value("needsAttention").toBool()
                && !cleanMap.value("firstNative").toMap().value("present").toBool(),
        "a Complete report without rows or notes carries nothing to show; an absent native is 'not present', not a 0");
}

void testTheBridgeWiresAReadOnlyRecoveryControllerAndTheCoverage()
{
    struct Workspace {
        fs::path root = fs::temp_directory_path() / ("aio-b3iii-qt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::path project = root / "project";
        fs::path database = root / "state" / "orchestrator.sqlite";
        Workspace() { fs::create_directories(project); }
        ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }
    } workspace;
    BackendBridge bridge(QString::fromStdWString(workspace.database.wstring()));
    require(bridge.storageAvailable(), "durable storage");
    QObject* const recovery = bridge.compactionRecovery();
    require(recovery != nullptr && dynamic_cast<CompactionIntentRecoveryController*>(recovery) != nullptr, "the Bridge exposes the recovery controller");
    const QVariantMap report = recovery->property("report").toMap();
    require(report.value("scope").toString() == "database" && report.value("applicable").toBool() && report.value("status").toString() == "complete"
                && report.value("inspected").toULongLong() == 0,
        "the owner's startup report of a fresh database: applicable, Complete, nothing inspected");
    require(recovery->metaObject()->methodCount() == recovery->metaObject()->methodOffset(),
        "the controller has no signal, slot or invokable method of its own: nothing to run, repeat or refresh");
    require(recovery->property("report").toMap() == report, "reading it again returns the same startup snapshot");

    const QString projectId = bridge.projectController().createProject(QStringLiteral("Coverage"), QStringLiteral("B3-III"),
        QString::fromStdWString(workspace.project.wstring()));
    require(!projectId.isEmpty(), "a project");
    require(bridge.projectCostController().refreshProject(projectId), "the real estimate refreshes");
    const QVariantMap coverage = bridge.projectCostController().coverage();
    require(coverage.value("state").toString() == "available" && coverage.value("hasCounts").toBool() && coverage.value("openOperations").toULongLong() == 0
                && coverage.value("unresolvedOperations").toULongLong() == 0,
        "the real runtime: Available with real zeros");
    const QVariantMap history = bridge.projectMemoryController().compactionHistorySnapshot(projectId);
    require(history.value("intents").toMap().value("state").toString() == "available" && history.value("intents").toMap().value("rows").toList().isEmpty(),
        "the real history snapshot has its intent section");
}

} // namespace

int runCompactionCoverageQtTests()
{
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"testTheCoverageMapNeverTurnsAnAbsenceIntoAZero", testTheCoverageMapNeverTurnsAnAbsenceIntoAZero},
        {"testAnUnfinishedProtocolRowHasNoMoneyAtAll", testAnUnfinishedProtocolRowHasNoMoneyAtAll},
        {"testTheCostControllerCarriesTheCoverageOfTheSameReadAndNeverReadsTwice", testTheCostControllerCarriesTheCoverageOfTheSameReadAndNeverReadsTwice},
        {"testTheCostControllerKeepsTheRequestedPeriodAndOnlyOneReadPerRefresh", testTheCostControllerKeepsTheRequestedPeriodAndOnlyOneReadPerRefresh},
        {"testTheHistorySnapshotHasAnIndependentIntentSectionFromOneRead", testTheHistorySnapshotHasAnIndependentIntentSectionFromOneRead},
        {"testTheRecoveryMapIsDatabaseScopeAndKeepsEveryCauseBoundToItsNative", testTheRecoveryMapIsDatabaseScopeAndKeepsEveryCauseBoundToItsNative},
        {"testTheBridgeWiresAReadOnlyRecoveryControllerAndTheCoverage", testTheBridgeWiresAReadOnlyRecoveryControllerAndTheCoverage},
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
    if (failures == 0) std::cerr << "[compaction-coverage-qt] " << std::size(cases) << " cases passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
