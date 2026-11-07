// B3-III: the compaction recording coverage as an Application read model.
//  1. Pure rules: Open and Unresolved apart, a zero is a present zero and Unavailable has NO counts, the half-open period [from, to) (also
//     one-sided), refused data (a foreign project, a repeated id), and the shared period helper agrees with the money composition.
//  2. The reader over a scripted intent port: exactly ONE strict list per call for the counts AND the rows, the original error, no read
//     on an invalid period, nothing is ever written, marked, closed or recovered.
//  3. ProjectCostService: a failed required read keeps its old all-or-nothing failure; a failed intent read leaves a SUCCESSFUL money
//     snapshot with an Unavailable coverage; intents are in no amount, price state, evidence count, provider row or record count.
// Scripted ports are labelled as such; what real SQL and the real runtime show is in CompactionCoverageRuntimeTests.cpp. throwing require().
#include "ProjectCostTestSupport.hpp"

#include "ai/application/analytics/ProjectCostComposition.hpp"
#include "ai/application/conversations/CompactionCoveragePolicy.hpp"
#include "ai/application/conversations/CompactionCreatedAtWindow.hpp"
#include "ai/application/conversations/CompactionIntentReadModelService.hpp"

#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace project_cost_test;
using S = ProjectMemoryCompactionStatus;
using Cov = CompactionCoverageState;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point ms(std::int64_t value) { return std::chrono::system_clock::time_point(std::chrono::milliseconds(value)); }

ProjectMemoryCompactionIntent intent(const std::string& id, const std::string& project, CompactionIntentState state, std::int64_t createdMs)
{
    ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = project;
    value.providerId = "prov";
    value.modelId = "model";
    value.sourceSegmentIds = {"s1", "s2"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = ms(createdMs);
    value.state = state;
    if (state == CompactionIntentState::Unresolved) value.unresolvedMarkedAt = ms(createdMs + 5);
    return value;
}

// A scripted intent port: it can LIST (with a counter, a scripted failure and a hook that may change the source between two calls) and
// counts every other call - a read path must never call any of them.
class IntentSource final : public IProjectMemoryCompactionIntentRepository {
public:
    std::vector<ProjectMemoryCompactionIntent> intents;
    std::optional<common::Error> listFailure;
    std::function<void()> afterList;
    mutable int listCalls {0};
    mutable int otherCalls {0};

    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent&) override
    {
        ++otherCalls;
        return {};
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId&) const override
    {
        ++otherCalls;
        return common::Result<std::optional<ProjectMemoryCompactionIntent>>::ok(std::nullopt);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId& projectId, std::optional<CompactionIntentState> state) const override
    {
        ++listCalls;
        if (state.has_value()) ++otherCalls;   // the reader must not ask for one state
        if (listFailure.has_value()) return common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(*listFailure);
        std::vector<ProjectMemoryCompactionIntent> selected;
        for (const auto& value : intents) {
            if (value.projectId == projectId || foreignRowsLeak) selected.push_back(value);
        }
        auto result = common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok(std::move(selected));
        if (afterList) const_cast<IntentSource*>(this)->afterList();
        return result;
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState) const override
    {
        ++otherCalls;
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({});
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(const CompactionIntentIdentity&, std::chrono::system_clock::time_point) override
    {
        ++otherCalls;
        return {};
    }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity&) override
    {
        ++otherCalls;
        return {};
    }
    [[nodiscard]] CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity&) const override
    {
        ++otherCalls;
        return {};
    }
    bool foreignRowsLeak {false};   // a buggy source that ignores the project filter
};

struct Stack {
    LedgerSource ledger;
    CatalogSource catalog;
    RecordSource compactions;
    NoMemory memory;
    NoClients clients;
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    ExecutionAnalyticsService analytics {ledger, catalog};
    test_support::ScriptedCompactionIntentRepository dispatchPort;   // only to build the compaction service; never used here
    ProjectMemoryCompactionService compactionService {memory, compactions, dispatchPort, projects, providers, catalog, clients};
    IntentSource source;
    CompactionIntentReadModelService reader {source};
    ProjectCostService service {analytics, compactionService, reader};

    void seedMoney()
    {
        require(ledger.inner.insertExecution(ledgerRow("l1", "p1", "prov", "model", 100, 50, 900, true)).success(), "seed the ledger");
        compactions.records.push_back(compactionRecord("r1", "p1", "prov", "model", true, 700, 20, 10, S::Completed));
    }
};

// ---- 1. pure rules -------------------------------------------------------------------------------------------------------------------
void testOpenAndUnresolvedAreCountedApartAndAZeroIsAPresentZero()
{
    const auto snapshot = composeCompactionIntentSnapshot("p1",
        {intent("a", "p1", CompactionIntentState::Open, 1000), intent("b", "p1", CompactionIntentState::Open, 2000),
            intent("c", "p1", CompactionIntentState::Unresolved, 3000)},
        std::nullopt, std::nullopt);
    require(snapshot.coverage.state == Cov::Available && snapshot.coverage.openOperations == 2u && snapshot.coverage.unresolvedOperations == 1u
                && snapshot.rows.size() == 3 && !snapshot.coverage.windowed && !snapshot.coverage.error.has_value(),
        "Open and Unresolved are separate counts and the rows are the very rows counted");

    const auto empty = composeCompactionIntentSnapshot("p1", {}, std::nullopt, std::nullopt);
    require(empty.coverage.state == Cov::Available && empty.coverage.openOperations.has_value() && *empty.coverage.openOperations == 0
                && empty.coverage.unresolvedOperations.has_value() && *empty.coverage.unresolvedOperations == 0 && empty.rows.empty(),
        "no rows: Available with BOTH counts present as zeros");

    const CompactionCoverageObservation untouched;
    require(untouched.state == Cov::Unavailable && !untouched.openOperations.has_value() && !untouched.unresolvedOperations.has_value()
                && !untouched.error.has_value(),
        "a default observation is Unavailable and confirms no zero");
    const ProjectCostSnapshot defaultSnapshot;
    require(defaultSnapshot.compactionCoverage.state == Cov::Unavailable && !defaultSnapshot.compactionCoverage.openOperations.has_value(),
        "so is a default project cost snapshot");
}

void testThePeriodIsHalfOpenAlsoOneSidedAndKeptWhenUnavailable()
{
    const std::vector<ProjectMemoryCompactionIntent> rows = {
        intent("a", "p1", CompactionIntentState::Open, 1000), intent("b", "p1", CompactionIntentState::Open, 2000),
        intent("c", "p1", CompactionIntentState::Unresolved, 2000), intent("d", "p1", CompactionIntentState::Open, 3000)};
    const auto both = composeCompactionIntentSnapshot("p1", rows, ms(1000), ms(3000));
    require(both.coverage.windowed && both.coverage.fromInclusive == ms(1000) && both.coverage.toExclusive == ms(3000)
                && both.coverage.openOperations == 2u && both.coverage.unresolvedOperations == 1u && both.rows.size() == 3,
        "[from, to): the lower bound is included, the upper bound excluded");
    const auto fromOnly = composeCompactionIntentSnapshot("p1", rows, ms(2000), std::nullopt);
    require(fromOnly.coverage.windowed && fromOnly.coverage.openOperations == 2u && fromOnly.coverage.unresolvedOperations == 1u
                && !fromOnly.coverage.toExclusive.has_value(),
        "a lower bound alone: nothing is invented for the upper one");
    const auto toOnly = composeCompactionIntentSnapshot("p1", rows, std::nullopt, ms(2000));
    require(toOnly.coverage.windowed && toOnly.coverage.openOperations == 1u && toOnly.coverage.unresolvedOperations == 0u
                && !toOnly.coverage.fromInclusive.has_value(),
        "an upper bound alone");
    const auto subMillisecond = composeCompactionIntentSnapshot("p1", rows, ms(2000) + std::chrono::microseconds(1), std::nullopt);
    require(subMillisecond.coverage.openOperations == 1u && subMillisecond.coverage.unresolvedOperations == 0u,
        "a bound one microsecond after a millisecond stamp excludes it (the same comparison as the money)");
    const auto emptyWindow = composeCompactionIntentSnapshot("p1", rows, ms(5000), ms(6000));
    require(emptyWindow.coverage.state == Cov::Available && emptyWindow.coverage.openOperations == 0u && emptyWindow.coverage.windowed,
        "a window with no rows is a real zero, windowed");

    const auto unavailable = unavailableCompactionCoverage(common::Error {"persistence.read_failed", "damaged row"}, ms(1000), ms(3000));
    require(unavailable.state == Cov::Unavailable && unavailable.windowed && unavailable.fromInclusive == ms(1000) && unavailable.toExclusive == ms(3000)
                && !unavailable.openOperations.has_value() && !unavailable.unresolvedOperations.has_value() && unavailable.error.has_value()
                && unavailable.error->code == "persistence.read_failed" && unavailable.error->message == "damaged row",
        "Unavailable keeps the requested period (the question) and the original error, with no counts");
}

void testRefusedDataIsUnavailableNeverASilentFilter()
{
    const auto foreign = composeCompactionIntentSnapshot("p1",
        {intent("a", "p1", CompactionIntentState::Open, 1000), intent("b", "p2", CompactionIntentState::Open, 1000)}, std::nullopt, std::nullopt);
    require(foreign.coverage.state == Cov::Unavailable && foreign.coverage.error.has_value() && foreign.coverage.error->code == kIntentObservationInconsistentCode
                && !foreign.coverage.openOperations.has_value() && foreign.rows.empty(),
        "a row of another project: refused, not filtered, no counts, no rows");
    const auto repeated = composeCompactionIntentSnapshot("p1",
        {intent("a", "p1", CompactionIntentState::Open, 1000), intent("a", "p1", CompactionIntentState::Unresolved, 2000)}, std::nullopt, std::nullopt);
    require(repeated.coverage.state == Cov::Unavailable && repeated.coverage.error->code == kIntentObservationInconsistentCode
                && !repeated.coverage.unresolvedOperations.has_value() && repeated.rows.empty(),
        "a repeated operation id: refused");
}

void testTheSharedPeriodHelperPicksTheSameRowsAsTheMoneyComposition()
{
    // The money composition of the records and the coverage take their rows with the very same helper: same boundaries, same rows.
    std::vector<ProjectMemoryCompactionRecord> records;
    std::vector<ProjectMemoryCompactionIntent> intents;
    for (int i = 0; i < 6; ++i) {
        auto record = compactionRecord(("r" + std::to_string(i)).c_str(), "p1", "prov", "model", true, 10, 1, 1, S::Completed, i);
        records.push_back(record);
        auto row = intent("i" + std::to_string(i), "p1", CompactionIntentState::Open, 0);
        row.createdAt = record.createdAt;
        intents.push_back(row);
    }
    const ExecutionAnalyticsSnapshot ledger;
    const std::vector<std::pair<std::optional<std::chrono::system_clock::time_point>, std::optional<std::chrono::system_clock::time_point>>> windows = {
        {std::nullopt, std::nullopt}, {at(2), at(4)}, {at(0), at(6)}, {at(3), std::nullopt}, {std::nullopt, at(3)}, {at(6), at(7)}};
    for (const auto& [from, to] : windows) {
        const auto money = composeProjectCost(ledger, records, from, to);
        const auto coverage = composeCompactionIntentSnapshot("p1", intents, from, to);
        require(money.success() && coverage.coverage.state == Cov::Available
                    && money.value().totals.compaction.recordCount == *coverage.coverage.openOperations,
            "the money and the coverage select the same number of rows for the same period");
    }
    require(inCreatedAtWindow(at(2), at(2), at(3)) && !inCreatedAtWindow(at(3), at(2), at(3)) && inCreatedAtWindow(at(9), std::nullopt, std::nullopt),
        "the helper itself: lower included, upper excluded, open when not given");
}

// ---- 2. the reader ----------------------------------------------------------------------------------------------------------------------
void testTheReaderMakesOneStrictListAndNothingElse()
{
    IntentSource source;
    source.intents = {intent("a", "p1", CompactionIntentState::Open, 1000), intent("b", "p1", CompactionIntentState::Unresolved, 2000),
        intent("x", "p2", CompactionIntentState::Open, 1000)};
    CompactionIntentReadModelService reader(source);
    const auto snapshot = reader.read("p1");
    require(source.listCalls == 1 && source.otherCalls == 0, "ONE list for the counts and the rows; no state list, no mark, no close, no recovery");
    require(snapshot.coverage.openOperations == 1u && snapshot.coverage.unresolvedOperations == 1u && snapshot.rows.size() == 2,
        "only the asked project; the counts equal the rows by construction");

    // The source changes between two calls: each snapshot is consistent in itself (counts == its own rows).
    source.afterList = [&] { source.intents.push_back(intent("c", "p1", CompactionIntentState::Open, 3000)); source.afterList = nullptr; };
    const auto first = reader.read("p1");
    const auto second = reader.read("p1");
    require(source.listCalls == 3 && first.rows.size() == 2 && *first.coverage.openOperations + *first.coverage.unresolvedOperations == first.rows.size()
                && second.rows.size() == 3 && *second.coverage.openOperations + *second.coverage.unresolvedOperations == second.rows.size(),
        "a source that changes between calls never makes the counts of one call disagree with its rows");
}

void testTheReaderReturnsTheOriginalErrorWithoutCountsOrZeros()
{
    IntentSource source;
    source.listFailure = common::Error {"persistence.read_failed", "a damaged intent row"};
    CompactionIntentReadModelService reader(source);
    const auto snapshot = reader.read("p1", ms(1000), std::nullopt);
    require(snapshot.coverage.state == Cov::Unavailable && snapshot.coverage.error.has_value() && snapshot.coverage.error->code == "persistence.read_failed"
                && snapshot.coverage.error->message == "a damaged intent row" && !snapshot.coverage.openOperations.has_value()
                && !snapshot.coverage.unresolvedOperations.has_value() && snapshot.rows.empty() && snapshot.coverage.windowed
                && snapshot.coverage.fromInclusive == ms(1000),
        "the ORIGINAL error, no counts (not zeros), no rows, the requested period kept");

    IntentSource leaking;
    leaking.foreignRowsLeak = true;
    leaking.intents = {intent("a", "p1", CompactionIntentState::Open, 1000), intent("b", "p2", CompactionIntentState::Open, 1000)};
    const auto refused = CompactionIntentReadModelService(leaking).read("p1");
    require(refused.coverage.state == Cov::Unavailable && refused.coverage.error->code == kIntentObservationInconsistentCode && refused.rows.empty(),
        "a source that returns a row of another project is not trusted");

    IntentSource untouched;
    const auto inverted = CompactionIntentReadModelService(untouched).read("p1", ms(3000), ms(3000));
    require(untouched.listCalls == 0 && inverted.coverage.state == Cov::Unavailable && inverted.coverage.error->code == kProjectCostInvalidDateRangeCode
                && inverted.coverage.fromInclusive == ms(3000),
        "an empty or inverted period is refused BEFORE any read");
}

// ---- 3. the project cost -------------------------------------------------------------------------------------------------------------------
void testIntentsAreInNoAmountNoPriceStateNoEvidenceAndNoRow()
{
    Stack without;
    without.seedMoney();
    const auto base = without.service.projectCost("p1");
    require(base.success() && base.value().compactionCoverage.state == Cov::Available && base.value().compactionCoverage.openOperations == 0u,
        "setup: the money without any intent, coverage Available with real zeros");

    Stack with;
    with.seedMoney();
    with.source.intents = {intent("a", "p1", CompactionIntentState::Open, 1000), intent("b", "p1", CompactionIntentState::Unresolved, 2000),
        intent("c", "p1", CompactionIntentState::Unresolved, 3000)};
    const auto result = with.service.projectCost("p1");
    require(result.success(), "the estimate succeeds with intents present");
    const auto& a = base.value();
    const auto& b = result.value();
    require(a.totals.costMicros() == b.totals.costMicros() && a.totals.inputTokens() == b.totals.inputTokens()
                && a.totals.outputTokens() == b.totals.outputTokens() && a.totals.priceState() == b.totals.priceState()
                && a.totals.pricingEvidenceCount() == b.totals.pricingEvidenceCount() && a.totals.incompletePriceCount() == b.totals.incompletePriceCount()
                && a.totals.compactionRecordCount() == b.totals.compactionRecordCount() && a.totals.ledgerExecutionCount() == b.totals.ledgerExecutionCount()
                && a.totals.ledgerAverageLatencyMs() == b.totals.ledgerAverageLatencyMs() && a.providers.size() == b.providers.size(),
        "intents change no amount, no tokens, no price state, no evidence count, no record or call count, no provider row");
    require(b.compactionCoverage.openOperations == 1u && b.compactionCoverage.unresolvedOperations == 2u && b.compactionCoverage.state == Cov::Available,
        "the coverage carries them, separately");
    require(with.source.listCalls == 1 && with.source.otherCalls == 0, "ONE intent read per estimate; nothing else of the port is called");
}

void testAnIntentOnlyProjectHasNoMoneyButItsCoverage()
{
    Stack stack;
    stack.source.intents = {intent("a", "p1", CompactionIntentState::Unresolved, 1000)};
    const auto result = stack.service.projectCost("p1");
    require(result.success() && result.value().totals.pricingEvidenceCount() == 0 && result.value().totals.costMicros() == 0
                && result.value().totals.priceState() == ExecutionPriceState::None && result.value().providers.empty(),
        "no ledger and no record: no money, price state none, no provider or model row from the intent");
    require(result.value().compactionCoverage.unresolvedOperations == 1u, "...and the coverage is there");
}

void testAFailedIntentReadNeverSpoilsTheMoney()
{
    Stack ok;
    ok.seedMoney();
    const auto control = ok.service.projectCost("p1");

    Stack failing;
    failing.seedMoney();
    failing.source.listFailure = common::Error {"persistence.read_failed", "intents unreadable"};
    const auto result = failing.service.projectCost("p1", at(-10), at(10));
    require(result.success(), "an intent read failure does not fail the estimate");
    const auto& c = result.value().compactionCoverage;
    require(c.state == Cov::Unavailable && c.error.has_value() && c.error->code == "persistence.read_failed" && c.error->message == "intents unreadable"
                && !c.openOperations.has_value() && !c.unresolvedOperations.has_value() && c.windowed && c.fromInclusive == at(-10) && c.toExclusive == at(10),
        "Unavailable, the original error, no counts, the requested period");
    require(result.value().totals.costMicros() == control.value().totals.costMicros() && result.value().totals.priceState() == control.value().totals.priceState()
                && result.value().totals.inputTokens() == control.value().totals.inputTokens() && result.value().providers.size() == control.value().providers.size(),
        "the money is exactly the money of the control: not zeroed, not demoted, not ledger-only");
}

void testARequiredReadFailureKeepsTheOldFailureAndNeverReadsIntents()
{
    {
        Stack stack;
        stack.seedMoney();
        stack.ledger.listFailure = scripted("test.ledger_down");
        const auto result = stack.service.projectCost("p1");
        require(!result.success() && result.error()->code == "test.ledger_down" && stack.source.listCalls == 0, "a ledger failure: the old failure, intents not read");
    }
    {
        Stack stack;
        stack.seedMoney();
        stack.compactions.listFailure = scripted("test.records_down");
        const auto result = stack.service.projectCost("p1");
        require(!result.success() && result.error()->code == "test.records_down" && stack.source.listCalls == 0, "a record failure: the old failure, intents not read");
    }
    {
        Stack stack;
        stack.seedMoney();
        const auto result = stack.service.projectCost("p1", at(5), at(5));
        require(!result.success() && result.error()->code == kProjectCostInvalidDateRangeCode && stack.source.listCalls == 0
                    && stack.ledger.projectReads == 0 && stack.compactions.listCalls == 0,
            "an invalid period: no read at all, not even of the intents");
    }
}

} // namespace

int runCompactionCoverageTests()
{
    try {
        testOpenAndUnresolvedAreCountedApartAndAZeroIsAPresentZero();
        testThePeriodIsHalfOpenAlsoOneSidedAndKeptWhenUnavailable();
        testRefusedDataIsUnavailableNeverASilentFilter();
        testTheSharedPeriodHelperPicksTheSameRowsAsTheMoneyComposition();
        testTheReaderMakesOneStrictListAndNothingElse();
        testTheReaderReturnsTheOriginalErrorWithoutCountsOrZeros();
        testIntentsAreInNoAmountNoPriceStateNoEvidenceAndNoRow();
        testAnIntentOnlyProjectHasNoMoneyButItsCoverage();
        testAFailedIntentReadNeverSpoilsTheMoney();
        testARequiredReadFailureKeepsTheOldFailureAndNeverReadsIntents();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionCoverageTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionCoverageTests: OK\n";
    return 0;
}
