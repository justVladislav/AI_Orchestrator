// A1-min: the Qt boundary of the project cost estimate - the projection (money, price state), the chart shares (and their equivalence with
// the older provider model), the provider model, every transition of the controller (success, failure, recovery, invalid range, clear, scope
// change, the observable ORDER of a failure) and the wiring on a real BackendBridge. The controller runs over the REAL services and the
// scripted sources of First_AI_Agents/tests/Unit/ProjectCostTestSupport.hpp (counters and failures per source). No network, no paid call;
// the checks throw, so they stay active under NDEBUG.
#include <ProjectCostTestSupport.hpp>

#include <ai/application/analytics/ProjectCostComposition.hpp>

#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/ProjectCostController.hpp>
#include <ai/ui/ProjectCostProjection.hpp>
#include <ai/ui/ProjectCostProviderModel.hpp>
#include <ai/ui/ProviderExecutionAnalyticsModel.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace project_cost_test;
namespace app = ai::application;
namespace fs = std::filesystem;
using S = app::ProjectMemoryCompactionStatus;
using ai::ui::project_cost::costText;
using ai::ui::project_cost::priceIncomplete;
using ai::ui::project_cost::priceStateName;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

QString q(const char* text) { return QString::fromUtf8(text); }

app::Model catalogModel(const char* id, const char* provider, const char* displayName)
{
    app::Model model;
    model.id = id;
    model.providerId = provider;
    model.apiModelId = id;
    model.displayName = displayName;
    model.contextWindowTokens = 100'000;
    model.maxOutputTokens = 4096;
    model.inputPriceMicrosPerMillionTokens = 2'000'000;
    model.outputPriceMicrosPerMillionTokens = 10'000'000;
    return model;
}

app::ProjectCostAggregate aggregateOf(std::uint64_t ledgerCount, std::uint64_t ledgerUnknown, std::uint64_t ledgerCost,
    std::initializer_list<app::ProjectMemoryCompactionRecord> records)
{
    app::ProjectCostAggregate aggregate;
    aggregate.ledger.executionCount = ledgerCount;
    aggregate.ledger.unknownPriceExecutionCount = ledgerUnknown;
    aggregate.ledger.costMicros = ledgerCost;
    for (const auto& record : records) app::addCompactionRecord(aggregate.compaction, record);
    return aggregate;
}

app::ProviderProjectCost providerOf(const char* id, const app::ProjectCostAggregate& totals)
{
    app::ProviderProjectCost provider;
    provider.providerId = id;
    provider.totals = totals;
    return provider;
}

// ---- projection -----------------------------------------------------------------------------------------------------------------
void testTheProjectionWordsEveryStateAndNeverInventsAnAmount()
{
    const auto known = aggregateOf(0, 0, 0, {compactionRecord("a", "p", "pr", "m", true, 123'456, 1, 1, S::Completed)});
    require(priceStateName(known) == "known" && costText(known) == q("$0.1235") && !priceIncomplete(known), "known: amount, not incomplete");
    const auto knownZero = aggregateOf(0, 0, 0, {compactionRecord("a", "p", "pr", "m", true, 0, 1, 1, S::Completed)});
    require(priceStateName(knownZero) == "known" && costText(knownZero) == q("$0.0000"), "a known zero is shown as $0.0000");
    app::ProjectCostAggregate hundred;
    hundred.ledger.executionCount = 1;
    hundred.ledger.costMicros = 123'456'789;
    require(costText(hundred) == q("$123.46"), "two decimals from $100");
    const auto partial = aggregateOf(0, 0, 0, {compactionRecord("a", "p", "pr", "m", false, 500, 0, 0, S::Failed)});
    require(priceStateName(partial) == "partial" && costText(partial) == q("$0.0005") && priceIncomplete(partial), "partial: the recorded amount, incomplete");
    const auto unknown = aggregateOf(0, 0, 0, {compactionRecord("a", "p", "pr", "m", false, 0, 0, 0, S::Failed)});
    require(priceStateName(unknown) == "unknown" && costText(unknown).isEmpty() && priceIncomplete(unknown), "unknown: no text, never $0");
    const auto none = aggregateOf(0, 0, 0, {compactionRecord("a", "p", "pr", "m", std::nullopt, 0, 0, 0, S::Cancelled)});
    require(priceStateName(none) == "none" && costText(none).isEmpty() && !priceIncomplete(none), "none: no text, not incomplete");
    const auto map = ai::ui::project_cost::aggregateMap(aggregateOf(2, 0, 10, {compactionRecord("a", "p", "pr", "m", true, 5, 3, 4, S::Completed)}));
    require(map.value("ledgerExecutionCount").toULongLong() == 2 && map.value("compactionRecordCount").toULongLong() == 1
            && map.value("inputTokens").toULongLong() == 3 && map.value("outputTokens").toULongLong() == 4,
        "the two sources are carried apart; tokens are the sum of both");
}

// ---- chart shares ---------------------------------------------------------------------------------------------------------------
app::ProjectCostAggregate ledgerOnly(std::uint64_t count, std::uint64_t unknown, std::uint64_t cost, std::uint64_t in, std::uint64_t out)
{
    app::ProjectCostAggregate aggregate;
    aggregate.ledger.executionCount = count;
    aggregate.ledger.unknownPriceExecutionCount = unknown;
    aggregate.ledger.costMicros = cost;
    aggregate.ledger.inputTokens = in;
    aggregate.ledger.outputTokens = out;
    return aggregate;
}

void testSharesAreRatiosToTheLargestExactAmountAndFollowTheirBasis()
{
    std::vector<app::ProviderProjectCost> rows = {providerOf("a", ledgerOnly(2, 0, 400, 1, 1)), providerOf("b", ledgerOnly(1, 0, 100, 1, 1)),
        providerOf("c", ledgerOnly(1, 0, 0, 1, 1))};
    auto result = computeProjectCostShares(rows);
    require(result.basis == "cost" && result.shares.size() == 3 && result.shares[0] == 1.0 && result.shares[1] == 0.25 && result.shares[2] == 0.0,
        "all Known with a positive cost: the largest is 1 and the rest are ratios to it (not percentages of the total)");

    rows = {providerOf("a", ledgerOnly(1, 0, 0, 30, 10)), providerOf("b", ledgerOnly(1, 0, 0, 10, 10))};
    result = computeProjectCostShares(rows);
    require(result.basis == "tokens" && result.shares[0] == 1.0 && result.shares[1] == 0.5, "every cost a known zero: by tokens");

    rows = {providerOf("a", ledgerOnly(2, 1, 400, 1, 1)), providerOf("b", ledgerOnly(1, 0, 100, 1, 1))};
    result = computeProjectCostShares(rows);
    require(result.basis == "incomplete" && result.shares[0] == 0.0 && result.shares[1] == 0.0, "a partial row: no share for any row");
    rows = {providerOf("a", ledgerOnly(1, 1, 0, 1, 1)), providerOf("b", ledgerOnly(1, 0, 100, 1, 1))};
    require(computeProjectCostShares(rows).basis == "incomplete", "an unknown row: incomplete");

    // The new case: a row whose records carry no price data is NOT a known zero, and does not worsen the estimate.
    const auto noneOnly = aggregateOf(0, 0, 0, {compactionRecord("n", "p", "pr", "m", std::nullopt, 0, 0, 0, S::Cancelled)});
    rows = {providerOf("a", ledgerOnly(1, 0, 400, 1, 1)), providerOf("n", noneOnly)};
    result = computeProjectCostShares(rows);
    require(result.basis == "unavailable" && result.shares[0] == 0.0 && result.shares[1] == 0.0, "Known + None rows: unavailable, no share");
    rows = {providerOf("a", ledgerOnly(1, 1, 400, 1, 1)), providerOf("n", noneOnly)};
    require(computeProjectCostShares(rows).basis == "incomplete", "a partial row wins over a no-price-data row");
    rows = {providerOf("n", noneOnly)};
    require(computeProjectCostShares(rows).basis == "unavailable", "only no-price-data rows: unavailable");

    result = computeProjectCostShares({});
    require(result.basis == "empty" && result.shares.empty(), "no rows: nothing to compare - not an incomplete price");
}

// The older model keeps its own rule; the new one is equivalent to it where the rows are ledger-only.
void testTheNewSharesEqualTheOlderModelOnLedgerOnlyRows()
{
    struct Case {
        const char* name;
        std::vector<std::array<std::uint64_t, 5>> rows;   // count, unknown, cost, in, out
    };
    const std::vector<Case> cases = {
        {"all Known by cost", {{{2, 0, 400, 10, 10}}, {{1, 0, 100, 5, 5}}, {{3, 0, 0, 7, 7}}}},
        {"all Known by tokens", {{{1, 0, 0, 30, 10}}, {{1, 0, 0, 10, 10}}}},
        {"one Partial", {{{2, 1, 400, 10, 10}}, {{1, 0, 100, 5, 5}}}},
        {"one Unknown", {{{1, 1, 0, 10, 10}}, {{1, 0, 100, 5, 5}}}},
        {"a single Known", {{{1, 0, 50, 1, 1}}}},
    };
    for (const auto& item : cases) {
        std::vector<ProjectCostProviderRow> rows;
        std::vector<ProviderExecutionAnalyticsRow> oldRows;
        for (std::size_t i = 0; i < item.rows.size(); ++i) {
            const auto& r = item.rows[i];
            const auto totals = ledgerOnly(r[0], r[1], r[2], r[3], r[4]);
            ProjectCostProviderRow row;
            row.cost = providerOf(("p" + std::to_string(i)).c_str(), totals);
            rows.push_back(row);
            ProviderExecutionAnalyticsRow old;
            old.aggregate.providerId = row.cost.providerId;
            old.aggregate.totals = totals.ledger;
            oldRows.push_back(old);
        }
        ProjectCostProviderModel fresh;
        fresh.replace(rows);
        ProviderExecutionAnalyticsModel older;
        older.replace(oldRows);
        const int shareRole = fresh.roleNames().key("costShare");
        const int basisRole = fresh.roleNames().key("shareBasis");
        const int oldShareRole = older.roleNames().key("costShare");
        const int oldBasisRole = older.roleNames().key("shareBasis");
        require(fresh.rowCount() == older.rowCount(), std::string("same rows: ") + item.name);
        for (int row = 0; row < fresh.rowCount(); ++row) {
            require(fresh.data(fresh.index(row), shareRole) == older.data(older.index(row), oldShareRole),
                std::string("same share: ") + item.name);
            require(fresh.data(fresh.index(row), basisRole) == older.data(older.index(row), oldBasisRole),
                std::string("same basis: ") + item.name);
        }
    }
}

void testTheProviderModelKeepsTheTwoSourcesApartAndFallsBackToIds()
{
    ProjectCostProviderRow row;
    row.cost = providerOf("prov", aggregateOf(2, 0, 20, {compactionRecord("a", "p", "prov", "m1", true, 5, 3, 4, S::Completed)}));
    row.cost.models.push_back(app::ModelProjectCost {"m1", aggregateOf(2, 0, 20, {compactionRecord("a", "p", "prov", "m1", true, 5, 3, 4, S::Completed)})});
    row.cost.models.push_back(app::ModelProjectCost {"m2", aggregateOf(0, 0, 0, {compactionRecord("b", "p", "prov", "m2", std::nullopt, 0, 0, 0, S::Cancelled)})});
    row.name = QStringLiteral("Provider name");
    row.modelNames.insert(QStringLiteral("m1"), QStringLiteral("Model one"));
    ProjectCostProviderModel model;
    model.replace({row});
    require(model.rowCount() == 1, "one provider row");
    const auto role = [&](const char* name) { return model.data(model.index(0), model.roleNames().key(name)); };
    require(role("providerName").toString() == QStringLiteral("Provider name") && role("ledgerExecutionCount").toULongLong() == 2
            && role("compactionRecordCount").toULongLong() == 1 && role("compactionNoneCount").toULongLong() == 0,
        "ledger executions and compactions are separate roles");
    const QVariantList models = role("models").toList();
    require(models.size() == 2, "two model entries");
    require(models[0].toMap().value("modelName").toString() == QStringLiteral("Model one"), "a catalog name is used");
    require(models[1].toMap().value("modelName").toString() == QStringLiteral("m2"), "a missing catalog name falls back to the id");
    require(models[1].toMap().value("compactionNoneCount").toULongLong() == 1 && models[1].toMap().value("priceState").toString() == "none",
        "a model with only a no-price-data record is 'none', counted apart");
    require(role("pricingEvidenceCount").toULongLong() == 3 && role("incompletePriceCount").toULongLong() == 0, "evidence excludes the none record");
    model.replace({});
    require(model.rowCount() == 0 && model.sharesBasis() == "empty", "an empty model: no rows, basis empty");
}

// ---- the controller -------------------------------------------------------------------------------------------------------------
struct Rig {
    Fixture fixture;
    app::InMemoryModelRepository names;   // provider/model display names (metadata): a different repository from the one the ledger reads
    app::ProviderCatalogService catalogService {fixture.providers, names};
    ProjectCostController controller {fixture.service, catalogService};
    QObject listener;
    int summaryChanges {0};
    QStringList errorCodes;
    QStringList stateWhenSummaryChanged;
    QStringList stateWhenErrorEmitted;
    QStringList providerRowsWhenErrorEmitted;
    QStringList costTextWhenErrorEmitted;

    Rig()
    {
        require(fixture.catalog.insert(catalogModel("m", "prov", "Model M")).success(), "seed the analytics catalog");
        QObject::connect(&controller, &ProjectCostController::summaryChanged, &listener, [this] {
            ++summaryChanges;
            stateWhenSummaryChanged << controller.loadState();
        });
        QObject::connect(&controller, &ProjectCostController::errorOccurred, &listener, [this](const QString& code, const QString&) {
            errorCodes << code;
            stateWhenErrorEmitted << controller.loadState() + QLatin1Char('|') + controller.errorCode();
            providerRowsWhenErrorEmitted << QString::number(controller.providers()->rowCount());
            costTextWhenErrorEmitted << controller.costText() + QLatin1Char('|') + QString::number(controller.ledgerExecutionCount());
        });
    }
    void seed()
    {
        auto row = ledgerRow("e1", "p", "prov", "m", 10, 10, 200, true, 1);
        require(fixture.ledger.insertExecution(row).success(), "seed the ledger");
        fixture.compactions.records = {compactionRecord("c1", "p", "prov", "m", true, 300, 5, 5, S::Completed, 2),
            compactionRecord("c2", "p", "prov", "m", std::nullopt, 0, 0, 0, S::Cancelled, 3),
            compactionRecord("o1", "other", "prov", "m", true, 9999, 1, 1, S::Completed, 4)};
    }
};

void testASuccessfulRefreshReplacesEverythingAndKeepsTheSourcesApart()
{
    Rig rig;
    require(rig.controller.loadState() == "idle" && rig.controller.scopeId().isEmpty() && rig.controller.priceState() == "none"
            && rig.controller.providers()->rowCount() == 0 && rig.controller.shareBasis() == "empty",
        "a new controller is idle: no scope, no rows");
    rig.seed();
    require(rig.controller.refreshProject(QStringLiteral("p")), "the refresh succeeds");
    require(rig.controller.loadState() == "available" && rig.controller.scopeId() == "p" && rig.controller.errorCode().isEmpty(), "available for p");
    require(rig.controller.ledgerExecutionCount() == 1 && rig.controller.compactionRecordCount() == 2 && rig.controller.compactionNoneCount() == 1
            && rig.controller.pricingEvidenceCount() == 2 && rig.controller.incompletePriceCount() == 0,
        "ledger rows and compactions are counted apart; the none record is no evidence and not incomplete");
    require(rig.controller.priceState() == "known" && rig.controller.costText() == q("$0.0005"), "$0.0002 ledger + $0.0003 compaction = $0.0005");
    require(rig.controller.inputTokens() == 15 && rig.controller.outputTokens() == 15, "tokens of both sources");
    require(rig.controller.ledgerAverageLatencyMs() == 100, "latency is the ledger's");
    require(rig.controller.providers()->rowCount() == 1 && rig.errorCodes.isEmpty(), "one provider row, no error");
    require(rig.fixture.ledger.projectReads == 1 && rig.fixture.catalog.listCalls == 1 && rig.fixture.compactions.listCalls == 1,
        "ONE refresh = one read of each source (the rows are not read again for the provider table)");
    require(rig.fixture.compactions.writes == 0 && rig.fixture.ledger.writes == 0, "a refresh never writes");
}

void testAFailureReplacesTheStateBeforeEitherObserverAndEmitsTheErrorExactlyOnce()
{
    Rig rig;
    rig.seed();
    require(rig.controller.refreshProject(QStringLiteral("p")), "success first");
    require(rig.controller.costText() == q("$0.0005"), "the old sum exists");
    rig.errorCodes.clear();
    rig.summaryChanges = 0;
    rig.stateWhenSummaryChanged.clear();

    rig.fixture.compactions.listFailure = scripted("test.compaction_down");
    require(!rig.controller.refreshProject(QStringLiteral("p")), "the refresh fails");
    require(rig.controller.loadState() == "unavailable" && rig.controller.errorCode() == "test.compaction_down"
            && rig.controller.errorMessage() == QStringLiteral("scripted: test.compaction_down"),
        "unavailable with the ORIGINAL code and message");
    require(rig.controller.costText().isEmpty() && rig.controller.ledgerExecutionCount() == 0 && rig.controller.compactionRecordCount() == 0
            && rig.controller.inputTokens() == 0 && rig.controller.priceState() == "none" && rig.controller.providers()->rowCount() == 0,
        "no old sum, no counters, no rows: the previous result is gone, not shown as a zero price");
    require(rig.controller.scopeId() == "p", "the scope is the requested one");
    require(rig.summaryChanges == 1 && rig.errorCodes.size() == 1 && rig.errorCodes[0] == "test.compaction_down", "one summaryChanged and ONE errorOccurred");
    require(rig.stateWhenSummaryChanged == QStringList {QStringLiteral("unavailable")}, "summaryChanged is delivered with the state already unavailable");
    require(rig.stateWhenErrorEmitted == QStringList {QStringLiteral("unavailable|test.compaction_down")},
        "errorOccurred is delivered AFTER the transition: the properties already say unavailable with the same code");
    require(rig.providerRowsWhenErrorEmitted == QStringList {QStringLiteral("0")} && rig.costTextWhenErrorEmitted == QStringList {QStringLiteral("|0")},
        "...and the rows and sums are already cleared inside the error slot");

    // Recovery: only the next successful refresh; it clears its own error and emits none.
    rig.fixture.compactions.listFailure.reset();
    rig.errorCodes.clear();
    require(rig.controller.refreshProject(QStringLiteral("p")), "recovery");
    require(rig.controller.loadState() == "available" && rig.controller.errorCode().isEmpty() && rig.controller.errorMessage().isEmpty()
            && rig.controller.costText() == q("$0.0005") && rig.errorCodes.isEmpty(),
        "available again; the error is gone; a success emits no error");
}

void testEachSourceFailureIsUnavailableWithItsOwnErrorAndNeverALedgerOnlyTotal()
{
    struct Case {
        const char* name;
        void (*arm)(Rig&);
        const char* code;
    };
    const Case cases[] = {
        {"ledger", [](Rig& r) { r.fixture.ledger.listFailure = scripted("test.ledger_down"); }, "test.ledger_down"},
        {"catalog", [](Rig& r) { r.fixture.catalog.listFailure = scripted("test.catalog_down"); }, "test.catalog_down"},
        {"compactions", [](Rig& r) { r.fixture.compactions.listFailure = scripted("test.compaction_down"); }, "test.compaction_down"},
    };
    for (const auto& item : cases) {
        Rig rig;
        rig.seed();
        require(rig.controller.refreshProject(QStringLiteral("p")), "success first");
        item.arm(rig);
        require(!rig.controller.refreshProject(QStringLiteral("p")), std::string("fails: ") + item.name);
        require(rig.controller.loadState() == "unavailable" && rig.controller.errorCode() == item.code && rig.controller.costText().isEmpty()
                && rig.controller.ledgerExecutionCount() == 0 && rig.errorCodes.size() == 1,
            std::string("unavailable, original code, no ledger-only total, one error: ") + item.name);
    }
}

void testAnInvalidRangeAndEveryOtherTransitionLeaveAConsistentState()
{
    Rig rig;
    rig.seed();
    require(rig.controller.refreshProject(QStringLiteral("p")), "success first");
    rig.errorCodes.clear();
    const int ledgerReads = rig.fixture.ledger.projectReads;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    require(!rig.controller.refreshProjectRange(QStringLiteral("p"), now, now.addDays(-1)), "an inverted range is refused");
    require(rig.controller.loadState() == "unavailable" && rig.controller.errorCode() == "project_cost.invalid_date_range"
            && rig.controller.errorMessage() == QStringLiteral("The project cost date range is invalid.") && rig.controller.costText().isEmpty()
            && rig.controller.scopeId() == "p" && rig.errorCodes == QStringList {QStringLiteral("project_cost.invalid_date_range")},
        "invalid range: unavailable, the accepted code and text, no old sum, one error");
    require(!rig.controller.refreshProjectRange(QStringLiteral("p"), QDateTime(), now), "an invalid QDateTime is refused too");
    require(rig.errorCodes.size() == 2 && rig.fixture.ledger.projectReads == ledgerReads, "one error per refusal, and no source was read");

    // A valid window recovers.
    rig.errorCodes.clear();
    require(rig.controller.refreshProjectRange(QStringLiteral("p"), QDateTime::fromSecsSinceEpoch(0, Qt::UTC), now.addYears(1)), "a valid window");
    require(rig.controller.loadState() == "available" && rig.errorCodes.isEmpty(), "available; no error");

    // Another project replaces everything.
    require(rig.controller.refreshProject(QStringLiteral("other")), "another project");
    require(rig.controller.scopeId() == "other" && rig.controller.ledgerExecutionCount() == 0 && rig.controller.compactionRecordCount() == 1
            && rig.controller.costText() == q("$0.0100"),
        "the scope and every number belong to the new project (9999 micros), nothing of p remains");

    // An empty id and clear(): idle, no error, the previous error is cleared.
    rig.fixture.compactions.listFailure = scripted("test.compaction_down");
    require(!rig.controller.refreshProject(QStringLiteral("p")), "fails");
    rig.errorCodes.clear();
    require(rig.controller.refreshProject(QString()), "an empty project id is not a failure");
    require(rig.controller.loadState() == "idle" && rig.controller.scopeId().isEmpty() && rig.controller.errorCode().isEmpty()
            && rig.errorCodes.isEmpty(),
        "idle: no scope, no error, no error signal");
    require(rig.controller.refreshProjectRange(QString(), now, now), "an empty project id with a range is not a failure");
    rig.fixture.compactions.listFailure.reset();
    require(rig.controller.refreshProject(QStringLiteral("p")), "available");
    rig.controller.clear();
    require(rig.controller.loadState() == "idle" && rig.controller.providers()->rowCount() == 0 && rig.controller.costText().isEmpty(), "clear: idle");
}

void testADuplicateCompactionIdIsUnavailableWithTheAcceptedCode()
{
    Rig rig;
    rig.fixture.compactions.records = {compactionRecord("same", "p", "prov", "m", true, 1, 1, 1, S::Completed, 1),
        compactionRecord("same", "p", "prov", "m", true, 1, 1, 1, S::Failed, 2)};
    require(!rig.controller.refreshProject(QStringLiteral("p")), "refused");
    require(rig.controller.loadState() == "unavailable" && rig.controller.errorCode() == "project_cost.duplicate_compaction_record"
            && rig.controller.errorMessage() == QStringLiteral("The memory compaction history contains duplicate record IDs.")
            && rig.controller.providers()->rowCount() == 0,
        "duplicate record ids: unavailable, the accepted code and text, no partial rows");
}

void testNamesAreMetadataAndTheirLossIsNotAFailure()
{
    Rig rig;
    rig.seed();
    require(rig.controller.refreshProject(QStringLiteral("p")), "refresh with an empty name catalog");
    const auto* model = rig.controller.providers();
    const auto name = model->data(model->index(0, 0), model->roleNames().key("providerName")).toString();
    require(name == QStringLiteral("prov"), "an unknown provider name falls back to the id");
}

// ---- the bridge -----------------------------------------------------------------------------------------------------------------
struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("aio-pcost-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";
    TempWorkspace() { fs::create_directories(projectPath); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }
    [[nodiscard]] QString projectPathQt() const { return QString::fromStdWString(projectPath.wstring()); }
};

void testTheBridgeExposesTheHeapOwnedControllerOverTheRealRuntime()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    require(bridge.storageAvailable(), "durable storage engaged");
    ProjectCostController& controller = bridge.projectCostController();
    require(bridge.projectCost() == &controller, "QML receives the controller through backend.projectCost");
    require(bridge.projectCost() != bridge.executionAnalytics(), "the older analytics controller is a different, untouched object");
    const QString projectId = bridge.projectController().createProject(
        QStringLiteral("Cost project"), QStringLiteral("A1-min"), workspace.projectPathQt());
    require(!projectId.isEmpty(), "a project");
    require(controller.refreshProject(projectId) && controller.loadState() == "available" && controller.scopeId() == projectId,
        "the real runtime produces an available estimate");
    require(controller.priceState() == "none" && controller.ledgerExecutionCount() == 0 && controller.compactionRecordCount() == 0
            && controller.providers()->rowCount() == 0 && controller.shareBasis() == "empty",
        "an empty project is none with no rows (not a zero price)");
}

} // namespace

int runProjectCostQtTests()
{
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"testTheProjectionWordsEveryStateAndNeverInventsAnAmount", testTheProjectionWordsEveryStateAndNeverInventsAnAmount},
        {"testSharesAreRatiosToTheLargestExactAmountAndFollowTheirBasis", testSharesAreRatiosToTheLargestExactAmountAndFollowTheirBasis},
        {"testTheNewSharesEqualTheOlderModelOnLedgerOnlyRows", testTheNewSharesEqualTheOlderModelOnLedgerOnlyRows},
        {"testTheProviderModelKeepsTheTwoSourcesApartAndFallsBackToIds", testTheProviderModelKeepsTheTwoSourcesApartAndFallsBackToIds},
        {"testASuccessfulRefreshReplacesEverythingAndKeepsTheSourcesApart", testASuccessfulRefreshReplacesEverythingAndKeepsTheSourcesApart},
        {"testAFailureReplacesTheStateBeforeEitherObserverAndEmitsTheErrorExactlyOnce",
            testAFailureReplacesTheStateBeforeEitherObserverAndEmitsTheErrorExactlyOnce},
        {"testEachSourceFailureIsUnavailableWithItsOwnErrorAndNeverALedgerOnlyTotal",
            testEachSourceFailureIsUnavailableWithItsOwnErrorAndNeverALedgerOnlyTotal},
        {"testAnInvalidRangeAndEveryOtherTransitionLeaveAConsistentState", testAnInvalidRangeAndEveryOtherTransitionLeaveAConsistentState},
        {"testADuplicateCompactionIdIsUnavailableWithTheAcceptedCode", testADuplicateCompactionIdIsUnavailableWithTheAcceptedCode},
        {"testNamesAreMetadataAndTheirLossIsNotAFailure", testNamesAreMetadataAndTheirLossIsNotAFailure},
        {"testTheBridgeExposesTheHeapOwnedControllerOverTheRealRuntime", testTheBridgeExposesTheHeapOwnedControllerOverTheRealRuntime},
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
    if (failures == 0) std::cerr << "[project-cost-qt] " << std::size(cases) << " cases passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
