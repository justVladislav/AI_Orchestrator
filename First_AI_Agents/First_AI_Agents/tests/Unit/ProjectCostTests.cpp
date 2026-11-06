// A1-min: the project cost estimate over the execution ledger and the stored memory-compaction records.
// Pure rules first (classification, counters, price-state algebra), then the real ProjectCostService over scripted sources
// (reads, filters, failures, duplicates, consistency). Checks use throwing require(); no network, no database.
#include "ProjectCostTestSupport.hpp"

#include "ai/application/analytics/ProjectCostComposition.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace project_cost_test;
using S = ProjectMemoryCompactionStatus;
using P = ExecutionPriceState;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

ProjectMemoryCompactionRecord rec(std::optional<bool> known, std::uint64_t cost, std::uint64_t input, std::uint64_t output,
    S status, const char* id = "r")
{
    return compactionRecord(id, "p", "prov", "model", known, cost, input, output, status);
}

ExecutionAggregate ledgerAggregate(std::uint64_t executions, std::uint64_t unknownPrice, std::uint64_t cost)
{
    ExecutionAggregate aggregate;
    aggregate.executionCount = executions;
    aggregate.unknownPriceExecutionCount = unknownPrice;
    aggregate.costMicros = cost;
    aggregate.totalLatencyMs = executions * 100;
    aggregate.averageLatencyMs = executions == 0 ? 0 : 100;
    return aggregate;
}

ProjectCostAggregate combine(const ExecutionAggregate& ledger, std::initializer_list<ProjectMemoryCompactionRecord> records)
{
    ProjectCostAggregate aggregate;
    aggregate.ledger = ledger;
    for (const auto& record : records) addCompactionRecord(aggregate.compaction, record);
    return aggregate;
}

// ---- classification (the seven stored shapes) ------------------------------------------------------------------------------
struct Shape {
    const char* name;
    ProjectMemoryCompactionRecord record;
    P state;
    std::uint64_t evidence;
    std::uint64_t incomplete;
    std::uint64_t none;
    std::uint64_t cost;
    std::uint64_t tokens;
};

void testEverySevenStoredShapeIsClassifiedByTheAcceptedFunctionWithoutAUsageFilter()
{
    const std::vector<Shape> shapes = {
        {"1 true, cost>0, tokens 0", rec(true, 700, 0, 0, S::Failed), P::Known, 1, 0, 0, 700, 0},
        {"1b true, cost>0, tokens>0", rec(true, 700, 10, 5, S::Completed), P::Known, 1, 0, 0, 700, 15},
        {"2 known zero", rec(true, 0, 10, 5, S::Completed), P::Known, 1, 0, 0, 0, 15},
        {"2b known zero, no tokens, Failed", rec(true, 0, 0, 0, S::Failed), P::Known, 1, 0, 0, 0, 0},
        {"3 false, cost>0, tokens 0", rec(false, 400, 0, 0, S::Cancelled), P::Partial, 1, 1, 0, 400, 0},
        {"3b NULL, cost>0, tokens 0", rec(std::nullopt, 400, 0, 0, S::Completed), P::Partial, 1, 1, 0, 400, 0},
        {"4 explicit unknown", rec(false, 0, 0, 0, S::Failed), P::Unknown, 1, 1, 0, 0, 0},
        {"4b explicit unknown with tokens", rec(false, 0, 9, 1, S::Completed), P::Unknown, 1, 1, 0, 0, 10},
        {"5 NULL, cost 0, tokens>0", rec(std::nullopt, 0, 3, 0, S::Failed), P::Unknown, 1, 1, 0, 0, 3},
        {"6 legacy Completed 0/0", rec(std::nullopt, 0, 0, 0, S::Completed), P::Unknown, 1, 1, 0, 0, 0},
        {"7 NULL Failed 0/0", rec(std::nullopt, 0, 0, 0, S::Failed), P::None, 0, 0, 1, 0, 0},
        {"7b NULL Cancelled 0/0", rec(std::nullopt, 0, 0, 0, S::Cancelled), P::None, 0, 0, 1, 0, 0},
    };
    for (const auto& shape : shapes) {
        const auto aggregate = combine({}, {shape.record});
        require(compactionPriceState(shape.record) == shape.state, std::string("accepted state: ") + shape.name);
        require(aggregate.compaction.recordCount == 1, std::string("recordCount: ") + shape.name);
        require(aggregate.pricingEvidenceCount() == shape.evidence, std::string("evidence: ") + shape.name);
        require(aggregate.incompletePriceCount() == shape.incomplete, std::string("incomplete: ") + shape.name);
        require(aggregate.compactionNoneCount() == shape.none, std::string("none: ") + shape.name);
        require(aggregate.costMicros() == shape.cost, std::string("cost: ") + shape.name);
        require(aggregate.inputTokens() + aggregate.outputTokens() == shape.tokens, std::string("tokens: ") + shape.name);
        require(aggregate.compaction.knownCount + aggregate.compaction.partialCount + aggregate.compaction.unknownCount
                    + aggregate.compaction.noneCount == aggregate.compaction.recordCount,
            std::string("a record counts in exactly one state: ") + shape.name);
        // A none record holds nothing, so summing without a condition equals summing only the others.
        if (shape.state == P::None) {
            require(shape.cost == 0 && shape.tokens == 0, std::string("none holds no cost and no tokens: ") + shape.name);
        }
    }
    // The positive amount at zero tokens is visible in the combined price state, not dropped.
    require(combine({}, {rec(true, 700, 0, 0, S::Failed)}).costMicros() == 700, "A recorded amount at zero tokens must be summed");
    require(combine({}, {rec(true, 700, 0, 0, S::Failed)}).priceState() == P::Known, "...and a known amount is known");
    // The statuses are counted, never used to decide the cost.
    const auto statuses = combine({}, {rec(true, 1, 1, 1, S::Completed, "a"), rec(true, 1, 1, 1, S::Failed, "b"),
        rec(true, 1, 1, 1, S::Cancelled, "c")});
    require(statuses.compaction.completedCount == 1 && statuses.compaction.failedCount == 1 && statuses.compaction.cancelledCount == 1
            && statuses.costMicros() == 3,
        "Completed, Failed and Cancelled all count by status and all contribute their stored amount");
}

// ---- the price-state algebra (fourteen combinations) ---------------------------------------------------------------------------
struct Combo {
    const char* name;
    ProjectCostAggregate aggregate;
    P state;
    std::uint64_t evidence;
    std::uint64_t incomplete;
    std::uint64_t cost;
    std::uint64_t none;
};

void testThePriceStateAlgebraMatchesTheNormativeTable()
{
    const auto known0 = [] { return rec(true, 0, 10, 5, S::Completed, "k0"); };
    const auto unknown = [] { return rec(false, 0, 0, 0, S::Failed, "u"); };
    const auto none = [] { return rec(std::nullopt, 0, 0, 0, S::Cancelled, "n"); };
    const std::vector<Combo> combos = {
        {"1 only Known0", combine({}, {known0()}), P::Known, 1, 0, 0, 0},
        {"2 cost>0 tokens 0 flag true", combine({}, {rec(true, 900, 0, 0, S::Failed)}), P::Known, 1, 0, 900, 0},
        {"3a cost>0 tokens 0 flag false", combine({}, {rec(false, 900, 0, 0, S::Failed)}), P::Partial, 1, 1, 900, 0},
        {"3b cost>0 tokens 0 flag NULL", combine({}, {rec(std::nullopt, 900, 0, 0, S::Failed)}), P::Partial, 1, 1, 900, 0},
        {"4 explicit unknown", combine({}, {unknown()}), P::Unknown, 1, 1, 0, 0},
        {"5 legacy Completed NULL 0/0", combine({}, {rec(std::nullopt, 0, 0, 0, S::Completed)}), P::Unknown, 1, 1, 0, 0},
        {"6 only None", combine({}, {none(), rec(std::nullopt, 0, 0, 0, S::Failed, "n2")}), P::None, 0, 0, 0, 2},
        {"7 Unknown + None", combine({}, {unknown(), none()}), P::Unknown, 1, 1, 0, 1},
        {"8 Known0 + Unknown", combine({}, {known0(), unknown()}), P::Partial, 2, 1, 0, 0},
        {"9 ledger Known + compaction None", combine(ledgerAggregate(3, 0, 500), {none()}), P::Known, 3, 0, 500, 1},
        {"10 ledger Known + compaction Unknown", combine(ledgerAggregate(3, 0, 500), {unknown()}), P::Partial, 4, 1, 500, 0},
        {"11 ledger only-unknown + Known0 compaction", combine(ledgerAggregate(2, 2, 0), {known0()}), P::Partial, 3, 2, 0, 0},
        {"12 ledger empty + compaction Known", combine({}, {rec(true, 321, 10, 5, S::Completed)}), P::Known, 1, 0, 321, 0},
        {"13 ledger Partial + compaction Partial",
            combine(ledgerAggregate(2, 1, 100), {rec(false, 50, 5, 5, S::Failed)}), P::Partial, 3, 2, 150, 0},
        {"14 empty", combine({}, {}), P::None, 0, 0, 0, 0},
    };
    for (const auto& combo : combos) {
        require(combo.aggregate.priceState() == combo.state, std::string("price state: ") + combo.name);
        require(combo.aggregate.pricingEvidenceCount() == combo.evidence, std::string("evidence: ") + combo.name);
        require(combo.aggregate.incompletePriceCount() == combo.incomplete, std::string("incomplete: ") + combo.name);
        require(combo.aggregate.costMicros() == combo.cost, std::string("cost: ") + combo.name);
        require(combo.aggregate.compactionNoneCount() == combo.none, std::string("none count: ") + combo.name);
    }
    // A project with ONLY compactions is not "none" merely because the ledger is empty.
    require(combine({}, {rec(true, 321, 10, 5, S::Completed)}).priceState() != P::None, "compaction-only is not none");
}

void testTheCombinedAlgebraEqualsTheLedgerSemanticsWhenThereIsNoCompaction()
{
    for (std::uint64_t executions = 0; executions <= 4; ++executions) {
        for (std::uint64_t unknownPrice = 0; unknownPrice <= executions; ++unknownPrice) {
            for (const std::uint64_t cost : {std::uint64_t {0}, std::uint64_t {1}, std::uint64_t {123456}}) {
                const auto ledger = ledgerAggregate(executions, unknownPrice, cost);
                ProjectCostAggregate combined;
                combined.ledger = ledger;
                require(combined.priceState() == ledger.priceState(),
                    "ledger-only price state must equal ExecutionAggregate::priceState() for executions="
                        + std::to_string(executions) + " unknown=" + std::to_string(unknownPrice) + " cost=" + std::to_string(cost));
                require(combined.pricingEvidenceCount() == ledger.executionCount, "evidence equals the ledger execution count");
                require(combined.incompletePriceCount() == ledger.unknownPriceExecutionCount, "incomplete equals the ledger unknown count");
            }
        }
    }
}

void testCompactionNeverEntersTheLedgerExecutionCountOrTheLatency()
{
    const auto ledger = ledgerAggregate(2, 0, 200);
    const auto withCompactions = combine(ledger, {rec(true, 10, 1, 1, S::Completed, "a"), rec(true, 10, 1, 1, S::Failed, "b"),
        rec(false, 0, 0, 0, S::Cancelled, "c")});
    require(withCompactions.ledgerExecutionCount() == 2, "executionCount stays the number of ledger rows");
    require(withCompactions.ledgerAverageLatencyMs() == 100, "latency stays ledger-only and is not divided by compactions");
    require(withCompactions.compactionRecordCount() == 3, "compactions are counted on their own");
    require(withCompactions.ledger.executionCount == ledger.executionCount && withCompactions.ledger.totalLatencyMs == ledger.totalLatencyMs,
        "the ledger part is exactly what the analytics service returned");
}

// ---- the real service over scripted sources ------------------------------------------------------------------------------------
void addLedger(Fixture& fixture, ModelExecution row)
{
    require(fixture.ledger.insertExecution(std::move(row)).success(), "could not seed the ledger");
}

Model catalogModel(const char* id, const char* provider)
{
    Model model;
    model.id = id;
    model.providerId = provider;
    model.apiModelId = id;
    model.displayName = id;
    model.contextWindowTokens = 100'000;
    model.maxOutputTokens = 4096;
    model.inputPriceMicrosPerMillionTokens = 2'000'000;
    model.outputPriceMicrosPerMillionTokens = 10'000'000;
    return model;
}

ProjectCostSnapshot costOf(Fixture& fixture, const char* project = "p")
{
    const auto result = fixture.service.projectCost(project);
    require(result.success(), "the project cost must be produced");
    return result.value();
}

void testSavedCompletedPaidFailedAndCancelledAreEachCountedOnceWithTheirStoredAmounts()
{
    Fixture fixture;
    fixture.compactions.records = {
        compactionRecord("c1", "p", "prov", "m", true, 500, 10, 5, S::Completed, 1),
        compactionRecord("c2", "p", "prov", "m", false, 300, 4, 0, S::Failed, 2),
        compactionRecord("c3", "p", "prov", "m", true, 200, 2, 2, S::Cancelled, 3),
    };
    const auto cost = costOf(fixture);
    require(cost.totals.costMicros() == 1000, "all three stored amounts are summed regardless of status");
    require(cost.totals.compactionRecordCount() == 3 && cost.totals.ledgerExecutionCount() == 0, "three compactions, no ledger rows");
    require(cost.totals.compaction.completedCount == 1 && cost.totals.compaction.failedCount == 1
            && cost.totals.compaction.cancelledCount == 1,
        "the statuses are counted");
    require(cost.totals.priceState() == P::Partial, "one partial record makes the estimate incomplete");
    require(cost.totals.inputTokens() == 16 && cost.totals.outputTokens() == 7, "tokens of the records are summed");
}

void testASavedFallbackIsExactlyOneContributionWithTheAmountOfTheOriginalAttempt()
{
    Fixture fixture;
    // The accounting fallback is one Failed row that carries the usage and the amount of the attempt whose commit failed.
    auto fallback = compactionRecord("attempt-1", "p", "prov", "m", true, 640, 100, 20, S::Failed, 1);
    fallback.errorCode = "project_memory.commit_failed";
    fallback.errorMessage = "The compaction could not be committed";
    fixture.compactions.records = {fallback};
    const auto cost = costOf(fixture);
    require(cost.totals.compactionRecordCount() == 1 && cost.totals.costMicros() == 640,
        "the fallback row is ONE contribution with the amount of the original attempt");
    require(cost.totals.inputTokens() == 100 && cost.totals.outputTokens() == 20, "its tokens are counted once");
}

void testLegacyShapesAreClassifiedFromTheStoredRecordOnly()
{
    Fixture fixture;
    fixture.compactions.records = {
        compactionRecord("n1", "p", "prov", "m", std::nullopt, 40, 0, 0, S::Completed, 1),   // partial: amount, no flag
        compactionRecord("n2", "p", "prov", "m", std::nullopt, 0, 7, 0, S::Failed, 2),       // unknown: tokens, no amount
        compactionRecord("n3", "p", "prov", "m", false, 0, 0, 0, S::Completed, 3),           // unknown: explicit false
        compactionRecord("n4", "p", "prov", "m", true, 0, 0, 0, S::Completed, 4),            // known zero
        compactionRecord("n5", "p", "prov", "m", std::nullopt, 0, 0, 0, S::Cancelled, 5),    // none
    };
    const auto cost = costOf(fixture);
    require(cost.totals.compaction.partialCount == 1 && cost.totals.compaction.unknownCount == 2
            && cost.totals.compaction.knownCount == 1 && cost.totals.compaction.noneCount == 1,
        "NULL with an amount is partial, NULL with tokens and false are unknown, true is known, NULL empty Failed/Cancelled is none");
    require(cost.totals.costMicros() == 40, "only the stored amount is summed; no catalog revaluation");
    require(cost.totals.pricingEvidenceCount() == 4 && cost.totals.incompletePriceCount() == 3,
        "the none record is neither evidence nor incomplete");
}

void testAnEmptyProjectIsNoneWithNoRowsAndNoZeroInvented()
{
    Fixture fixture;
    const auto cost = costOf(fixture);
    require(cost.totals.priceState() == P::None && cost.totals.costMicros() == 0, "an empty project is none");
    require(cost.providers.empty(), "no provider rows for an empty project");
    require(cost.totals.ledgerExecutionCount() == 0 && cost.totals.compactionRecordCount() == 0, "nothing recorded");
}

void testAMissingCatalogModelKeepsTheContributionAndTheRows()
{
    Fixture fixture;
    fixture.compactions.records = {compactionRecord("c1", "p", "deleted-provider", "deleted-model", true, 90, 3, 3, S::Completed)};
    const auto cost = costOf(fixture);
    require(cost.totals.costMicros() == 90, "the stored amount is kept although the model is not in the catalog");
    require(cost.providers.size() == 1 && cost.providers[0].providerId == "deleted-provider"
            && cost.providers[0].models.size() == 1 && cost.providers[0].models[0].modelId == "deleted-model",
        "the provider and model rows are keyed by the stored ids");
}

void testAnotherProjectIsNeverIncluded()
{
    Fixture fixture;
    fixture.compactions.records = {compactionRecord("mine", "p", "prov", "m", true, 10, 1, 1, S::Completed),
        compactionRecord("theirs", "other", "prov", "m", true, 9999, 1, 1, S::Completed)};
    require(fixture.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
    addLedger(fixture, ledgerRow("mine", "p", "prov", "m", 10, 10, 20, true));
    addLedger(fixture, ledgerRow("theirs", "other", "prov", "m", 10, 10, 8888, true));
    const auto cost = costOf(fixture);
    require(cost.totals.costMicros() == 30 && cost.totals.ledgerExecutionCount() == 1 && cost.totals.compactionRecordCount() == 1,
        "only the requested project is composed");
}

void testTheHalfOpenWindowIsTheSameForBothSources()
{
    Fixture fixture;
    require(fixture.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
    for (int second : {10, 20, 30}) {
        const std::string id = "x" + std::to_string(second);
        addLedger(fixture, ledgerRow(id.c_str(), "p", "prov", "m", 1, 1, 100, true, second));
        fixture.compactions.records.push_back(
            compactionRecord(("c" + std::to_string(second)).c_str(), "p", "prov", "m", true, 1000, 1, 1, S::Completed, second));
    }
    const auto result = fixture.service.projectCost("p", at(20), at(30));
    require(result.success(), "the windowed cost must be produced");
    require(result.value().totals.ledgerExecutionCount() == 1 && result.value().totals.compactionRecordCount() == 1
            && result.value().totals.costMicros() == 1100,
        "from is inclusive and to is exclusive for the ledger AND for the compactions");
    const auto fromOnly = fixture.service.projectCost("p", at(30), std::nullopt);
    require(fromOnly.success() && fromOnly.value().totals.compactionRecordCount() == 1 && fromOnly.value().totals.ledgerExecutionCount() == 1,
        "an open upper bound keeps the record at the lower bound");
    const auto toOnly = fixture.service.projectCost("p", std::nullopt, at(10));
    require(toOnly.success() && toOnly.value().totals.compactionRecordCount() == 0 && toOnly.value().totals.ledgerExecutionCount() == 0,
        "an exclusive upper bound drops the record created exactly at it");
}

void testAnInvalidWindowFailsBeforeAnyRead()
{
    Fixture fixture;
    for (const auto& window : {std::make_pair(at(30), at(30)), std::make_pair(at(40), at(30))}) {
        const auto result = fixture.service.projectCost("p", window.first, window.second);
        require(!result.success() && result.error()->code == kProjectCostInvalidDateRangeCode
                && result.error()->message == kProjectCostInvalidDateRangeMessage,
            "an empty or inverted window is project_cost.invalid_date_range");
    }
    require(fixture.ledger.projectReads == 0 && fixture.catalog.listCalls == 0 && fixture.compactions.listCalls == 0,
        "an invalid window must not read any source");
}

void testEachRequiredReadFailsTheWholeResultWithItsOriginalError()
{
    struct Case {
        const char* name;
        void (*arm)(Fixture&);
        const char* code;
        int ledgerReads;
        int catalogReads;
        int compactionReads;
    };
    const Case cases[] = {
        {"the ledger rows", [](Fixture& f) { f.ledger.listFailure = scripted("test.ledger_down"); }, "test.ledger_down", 1, 0, 0},
        {"the model catalog", [](Fixture& f) { f.catalog.listFailure = scripted("test.catalog_down"); }, "test.catalog_down", 1, 1, 0},
        {"the compaction history", [](Fixture& f) { f.compactions.listFailure = scripted("test.compaction_down"); },
            "test.compaction_down", 1, 1, 1},
    };
    for (const auto& item : cases) {
        Fixture fixture;
        require(fixture.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
        addLedger(fixture, ledgerRow("e1", "p", "prov", "m", 1, 1, 10, true));
        fixture.compactions.records = {compactionRecord("c1", "p", "prov", "m", true, 10, 1, 1, S::Completed)};
        item.arm(fixture);
        const auto result = fixture.service.projectCost("p");
        require(!result.success(), std::string("the whole result fails when this read fails: ") + item.name);
        require(result.error()->code == item.code && result.error()->message == std::string("scripted: ") + item.code,
            std::string("the original code and message are kept: ") + item.name);
        require(fixture.ledger.projectReads == item.ledgerReads && fixture.catalog.listCalls == item.catalogReads
                && fixture.compactions.listCalls == item.compactionReads,
            std::string("the reads are sequential and stop at the first failure: ") + item.name);
    }
}

void testARepeatedCompactionIdFailsTheWholeResultButEqualIdsOfDifferentSourcesDoNot()
{
    Fixture fixture;
    require(fixture.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
    fixture.compactions.records = {compactionRecord("dup", "p", "prov", "m", true, 10, 1, 1, S::Completed, 1),
        compactionRecord("dup", "p", "prov", "m", true, 10, 1, 1, S::Failed, 2)};
    auto result = fixture.service.projectCost("p");
    require(!result.success() && result.error()->code == kProjectCostDuplicateCompactionRecordCode
            && result.error()->message == kProjectCostDuplicateCompactionRecordMessage,
        "a repeated compaction record id is project_cost.duplicate_compaction_record");
    // Even outside the requested window: a repeated key is a corrupt history.
    result = fixture.service.projectCost("p", at(100), at(200));
    require(!result.success() && result.error()->code == kProjectCostDuplicateCompactionRecordCode,
        "the duplicate is detected over the whole read, not only inside the window");

    Fixture other;
    require(other.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
    addLedger(other, ledgerRow("same-id", "p", "prov", "m", 1, 1, 70, true));
    other.compactions.records = {compactionRecord("same-id", "p", "prov", "m", true, 30, 1, 1, S::Completed)};
    const auto both = other.service.projectCost("p");
    require(both.success() && both.value().totals.costMicros() == 100 && both.value().totals.ledgerExecutionCount() == 1
            && both.value().totals.compactionRecordCount() == 1,
        "an id equal in the ledger and in the compaction history is NOT a duplicate; both contribute");
}

void testReadingIsRepeatableAndNeverWrites()
{
    Fixture fixture;
    require(fixture.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
    addLedger(fixture, ledgerRow("e1", "p", "prov", "m", 10, 10, 20, true));
    fixture.compactions.records = {compactionRecord("c1", "p", "prov", "m", true, 10, 1, 1, S::Completed)};
    const auto first = costOf(fixture);
    const auto second = costOf(fixture);
    require(first.totals.costMicros() == second.totals.costMicros() && first.totals.priceState() == second.totals.priceState()
            && first.providers.size() == second.providers.size(),
        "two reads of unchanged data are equal");
    require(fixture.compactions.writes == 0 && fixture.ledger.writes == 0, "a read never writes either source");
    const auto rows = fixture.ledger.inner.listProjectExecutions("p");
    require(rows.success() && rows.value().size() == 1 && fixture.compactions.records.size() == 1,
        "no ledger row is created for a compaction record");
    require(fixture.clients.requests == 0, "a read never asks for a provider client");
}

void testOneCallReadsEachRequiredSourceExactlyOnce()
{
    Fixture fixture;
    require(fixture.catalog.insert(catalogModel("m", "prov")).success(), "seed the catalog");
    addLedger(fixture, ledgerRow("e1", "p", "prov", "m", 10, 10, 20, true));
    fixture.compactions.records = {compactionRecord("c1", "p", "prov", "m", true, 10, 1, 1, S::Completed)};
    (void)costOf(fixture);
    require(fixture.ledger.projectReads == 1 && fixture.catalog.listCalls == 1 && fixture.compactions.listCalls == 1,
        "one projectCost call reads the ledger rows, the catalog and the compaction history once each");
}

void expectAdditiveConsistency(const ProjectCostAggregate& total, const std::vector<ProjectCostAggregate>& parts, const std::string& level)
{
    ProjectCostAggregate sum;
    for (const auto& part : parts) {
        sum.ledger.executionCount += part.ledger.executionCount;
        sum.ledger.completedCount += part.ledger.completedCount;
        sum.ledger.inputTokens += part.ledger.inputTokens;
        sum.ledger.outputTokens += part.ledger.outputTokens;
        sum.ledger.costMicros += part.ledger.costMicros;
        sum.ledger.totalLatencyMs += part.ledger.totalLatencyMs;
        sum.ledger.unknownPriceExecutionCount += part.ledger.unknownPriceExecutionCount;
        sum.compaction.recordCount += part.compaction.recordCount;
        sum.compaction.knownCount += part.compaction.knownCount;
        sum.compaction.partialCount += part.compaction.partialCount;
        sum.compaction.unknownCount += part.compaction.unknownCount;
        sum.compaction.noneCount += part.compaction.noneCount;
        sum.compaction.completedCount += part.compaction.completedCount;
        sum.compaction.failedCount += part.compaction.failedCount;
        sum.compaction.cancelledCount += part.compaction.cancelledCount;
        sum.compaction.inputTokens += part.compaction.inputTokens;
        sum.compaction.outputTokens += part.compaction.outputTokens;
        sum.compaction.costMicros += part.compaction.costMicros;
    }
    require(sum.ledger.executionCount == total.ledger.executionCount && sum.ledger.completedCount == total.ledger.completedCount
            && sum.ledger.inputTokens == total.ledger.inputTokens && sum.ledger.outputTokens == total.ledger.outputTokens
            && sum.ledger.costMicros == total.ledger.costMicros && sum.ledger.totalLatencyMs == total.ledger.totalLatencyMs
            && sum.ledger.unknownPriceExecutionCount == total.ledger.unknownPriceExecutionCount,
        level + ": every additive ledger field of the level above equals the sum of the rows below");
    require(sum.compaction.recordCount == total.compaction.recordCount && sum.compaction.knownCount == total.compaction.knownCount
            && sum.compaction.partialCount == total.compaction.partialCount && sum.compaction.unknownCount == total.compaction.unknownCount
            && sum.compaction.noneCount == total.compaction.noneCount && sum.compaction.completedCount == total.compaction.completedCount
            && sum.compaction.failedCount == total.compaction.failedCount && sum.compaction.cancelledCount == total.compaction.cancelledCount
            && sum.compaction.inputTokens == total.compaction.inputTokens && sum.compaction.outputTokens == total.compaction.outputTokens
            && sum.compaction.costMicros == total.compaction.costMicros,
        level + ": every additive compaction field of the level above equals the sum of the rows below");
}

void testTotalsEqualTheSumOfProvidersAndEachProviderTheSumOfItsModels()
{
    Fixture fixture;
    require(fixture.catalog.insert(catalogModel("m1", "prov-a")).success(), "seed the catalog");
    require(fixture.catalog.insert(catalogModel("m2", "prov-a")).success(), "seed the catalog");
    require(fixture.catalog.insert(catalogModel("m3", "prov-b")).success(), "seed the catalog");
    addLedger(fixture, ledgerRow("e1", "p", "prov-a", "m1", 10, 10, 20, true, 1));
    addLedger(fixture, ledgerRow("e2", "p", "prov-a", "m2", 10, 10, 0, false, 2));
    addLedger(fixture, ledgerRow("e3", "p", "prov-b", "m3", 5, 5, 11, true, 3));
    fixture.compactions.records = {
        compactionRecord("c1", "p", "prov-a", "m1", true, 100, 7, 3, S::Completed, 1),
        compactionRecord("c2", "p", "prov-a", "m9", false, 50, 0, 0, S::Failed, 2),
        compactionRecord("c3", "p", "prov-c", "m7", std::nullopt, 0, 0, 0, S::Cancelled, 3),
        compactionRecord("c4", "p", "prov-b", "m3", false, 0, 4, 0, S::Cancelled, 4),
    };
    const auto cost = costOf(fixture);
    std::vector<ProjectCostAggregate> providers;
    for (const auto& provider : cost.providers) {
        providers.push_back(provider.totals);
        std::vector<ProjectCostAggregate> models;
        for (const auto& model : provider.models) models.push_back(model.totals);
        expectAdditiveConsistency(provider.totals, models, "provider " + provider.providerId);
    }
    expectAdditiveConsistency(cost.totals, providers, "total");
    require(cost.providers.size() == 3, "the compaction-only provider has its own row (prov-a, prov-b, prov-c)");
    require(cost.totals.ledgerExecutionCount() == 3 && cost.totals.compactionRecordCount() == 4 && cost.totals.compactionNoneCount() == 1,
        "the total counts both sources separately");
    // Ordering: by cost, then by recorded items, then by id.
    for (std::size_t i = 1; i < cost.providers.size(); ++i) {
        const auto& left = cost.providers[i - 1].totals;
        const auto& right = cost.providers[i].totals;
        require(left.costMicros() >= right.costMicros(), "providers are ordered by cost, highest first");
    }
}

void testTheLedgerPartOfEveryLevelIsExactlyWhatTheAnalyticsServiceReturned()
{
    Fixture fixture;
    require(fixture.catalog.insert(catalogModel("m1", "prov-a")).success(), "seed the catalog");
    addLedger(fixture, ledgerRow("e1", "p", "prov-a", "m1", 10, 10, 20, true, 1));
    addLedger(fixture, ledgerRow("e2", "p", "prov-a", "m1", 4, 4, 0, false, 2));
    fixture.compactions.records = {compactionRecord("c1", "p", "prov-a", "m1", true, 100, 7, 3, S::Completed, 1)};
    const auto cost = costOf(fixture);
    const auto analytics = fixture.analytics.projectSnapshot("p");
    require(analytics.success(), "the analytics snapshot");
    require(cost.totals.ledger.executionCount == analytics.value().totals.executionCount
            && cost.totals.ledger.costMicros == analytics.value().totals.costMicros
            && cost.totals.ledger.unknownPriceExecutionCount == analytics.value().totals.unknownPriceExecutionCount
            && cost.totals.ledger.averageLatencyMs == analytics.value().totals.averageLatencyMs,
        "the ledger half of the total equals the analytics total");
    require(cost.providers.size() == 1 && cost.providers[0].totals.ledger.costMicros == analytics.value().providers[0].totals.costMicros
            && cost.providers[0].models[0].totals.ledger.executionCount == analytics.value().providers[0].models[0].totals.executionCount,
        "...and at the provider and model level");
}

} // namespace

int runProjectCostTests()
{
    testEverySevenStoredShapeIsClassifiedByTheAcceptedFunctionWithoutAUsageFilter();
    testThePriceStateAlgebraMatchesTheNormativeTable();
    testTheCombinedAlgebraEqualsTheLedgerSemanticsWhenThereIsNoCompaction();
    testCompactionNeverEntersTheLedgerExecutionCountOrTheLatency();
    testSavedCompletedPaidFailedAndCancelledAreEachCountedOnceWithTheirStoredAmounts();
    testASavedFallbackIsExactlyOneContributionWithTheAmountOfTheOriginalAttempt();
    testLegacyShapesAreClassifiedFromTheStoredRecordOnly();
    testAnEmptyProjectIsNoneWithNoRowsAndNoZeroInvented();
    testAMissingCatalogModelKeepsTheContributionAndTheRows();
    testAnotherProjectIsNeverIncluded();
    testTheHalfOpenWindowIsTheSameForBothSources();
    testAnInvalidWindowFailsBeforeAnyRead();
    testEachRequiredReadFailsTheWholeResultWithItsOriginalError();
    testARepeatedCompactionIdFailsTheWholeResultButEqualIdsOfDifferentSourcesDoNot();
    testReadingIsRepeatableAndNeverWrites();
    testOneCallReadsEachRequiredSourceExactlyOnce();
    testTotalsEqualTheSumOfProvidersAndEachProviderTheSumOfItsModels();
    testTheLedgerPartOfEveryLevelIsExactlyWhatTheAnalyticsServiceReturned();
    return 0;
}
