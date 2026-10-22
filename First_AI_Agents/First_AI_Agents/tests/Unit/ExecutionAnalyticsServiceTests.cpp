#include "ai/application/ExecutionAnalyticsService.hpp"
#include "ai/application/InMemoryRepositories.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

void requireAnalytics(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

constexpr const char* kRunId = "run-analytics";

// $2 / $10 per million tokens, flat (no tier).
Model flatModel(const char* id = "flat-model")
{
    Model model;
    model.id = id;
    model.providerId = "provider-1";
    model.apiModelId = id;
    model.displayName = id;
    model.contextWindowTokens = 1'050'000;
    model.maxOutputTokens = 4096;
    model.inputPriceMicrosPerMillionTokens = 2'000'000;
    model.outputPriceMicrosPerMillionTokens = 10'000'000;
    return model;
}

// The same prices with a long-context tier above 272,000 input tokens.
Model tierModel(const char* id = "tier-model")
{
    Model model = flatModel(id);
    model.cacheWritePriceMicrosPerMillionTokens = 2'500'000;
    model.cacheReadPriceMicrosPerMillionTokens = 200'000;
    model.longContextPricing = LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};
    return model;
}

ModelExecution row(const char* id, const ModelId& modelId, std::uint64_t input, std::uint64_t output,
    std::uint64_t costMicros, bool costPriceKnown)
{
    ModelExecution execution;
    execution.id = id;
    execution.source = ModelExecutionSource::Run;
    execution.sourceId = kRunId;
    execution.projectId = "project-1";
    execution.providerId = "provider-1";
    execution.modelId = modelId;
    execution.role = "worker";
    execution.status = ModelExecutionStatus::Completed;
    execution.inputTokens = input;
    execution.outputTokens = output;
    execution.costMicros = costMicros;
    execution.costPriceKnown = costPriceKnown;
    execution.createdAt = std::chrono::system_clock::now();
    return execution;
}

// The analytics projection of `rows` over a catalog holding `models`, through the public service.
ExecutionAnalyticsSnapshot snapshotOf(
    const std::vector<ModelExecution>& rows, const std::vector<Model>& models)
{
    InMemoryModelExecutionRepository executions;
    InMemoryModelRepository catalog;
    for (const auto& model : models) {
        requireAnalytics(catalog.insert(model).success(), "Could not insert the analytics model");
    }
    std::uint32_t position = 0;
    for (auto execution : rows) {
        // The ledger keeps (source, source id, position, attempt) unique.
        execution.position = position++;
        requireAnalytics(executions.insertExecution(execution).success(),
            "Could not insert the analytics execution");
    }
    const ExecutionAnalyticsService service {executions, catalog};
    const auto snapshot = service.sourceSnapshot(ModelExecutionSource::Run, kRunId);
    requireAnalytics(snapshot.success(), "The analytics snapshot must be produced");
    return snapshot.value();
}

// A stored cost with a stored price state is authoritative: it is reported as it was recorded and
// the catalog is never consulted, so a later catalog edit cannot rewrite history.
void testStoredCostAndStoredPriceStateAreAuthoritative()
{
    // Known: the stored amount, exactly known. The catalog price (flat $2/$10) would give 1,000,
    // the stored amount is deliberately different to prove the stored value wins.
    const auto known = snapshotOf({row("e1", "flat-model", 100, 50, 777, true)}, {flatModel()});
    requireAnalytics(known.totals.costMicros == 777 && known.totals.unknownPriceExecutionCount == 0
            && known.totals.priceState() == ExecutionPriceState::Known,
        "A stored known cost must be reported as stored");

    // Unknown with a positive lower bound: the stored lower bound, flagged, never revalued.
    const auto lowerBound = snapshotOf({row("e1", "flat-model", 100, 50, 777, false)}, {flatModel()});
    requireAnalytics(lowerBound.totals.costMicros == 777 && lowerBound.totals.unknownPriceExecutionCount == 1
            && lowerBound.totals.priceState() == ExecutionPriceState::Partial,
        "A stored unknown-price cost must stay a flagged lower bound");

    // Unknown with a ZERO lower bound: an unpriced execution is not free and not repriced from a
    // catalog that may have changed since - even though the flat model WOULD price this usage.
    const auto unknownZero = snapshotOf({row("e1", "flat-model", 100'000, 1'000, 0, false)}, {flatModel()});
    requireAnalytics(unknownZero.totals.costMicros == 0 && unknownZero.totals.unknownPriceExecutionCount == 1
            && unknownZero.totals.priceState() == ExecutionPriceState::Unknown,
        "A stored unknown zero must never be recomputed and must not read as a known zero");
}

// A zero cost with a known price and NO usage is an exact zero (a row that never reached a provider).
void testAnExecutionWithoutUsageIsAnExactZero()
{
    const auto exact = snapshotOf({row("e1", "tier-model", 0, 0, 0, true)}, {tierModel()});
    requireAnalytics(exact.totals.costMicros == 0 && exact.totals.unknownPriceExecutionCount == 0
            && exact.totals.priceState() == ExecutionPriceState::Known,
        "A priced execution with no usage must be an exact, known zero");
}

// Only a historical row - zero cost, price "known" by default, but real usage - on a FLAT model may
// be re-derived with the flat formula; the result is as precise as the model's prices are complete.
void testLegacyFlatRowIsRecomputedWithTheFlatFormula()
{
    // 1,000 * $2 + 500 * $10 = 7,000 micros.
    const auto legacy = snapshotOf({row("e1", "flat-model", 1'000, 500, 0, true)}, {flatModel()});
    requireAnalytics(legacy.totals.costMicros == 7'000 && legacy.totals.unknownPriceExecutionCount == 0
            && legacy.totals.priceState() == ExecutionPriceState::Known,
        "A legacy flat row must be recomputed with the flat formula");

    // A flat model whose prices were never entered cannot price the row: unknown, not a free call.
    Model unpriced = flatModel();
    unpriced.inputPriceMicrosPerMillionTokens = 0;
    unpriced.outputPriceMicrosPerMillionTokens = 0;
    const auto unpricedLegacy = snapshotOf({row("e1", "flat-model", 1'000, 500, 0, true)}, {unpriced});
    requireAnalytics(unpricedLegacy.totals.costMicros == 0 && unpricedLegacy.totals.unknownPriceExecutionCount == 1
            && unpricedLegacy.totals.priceState() == ExecutionPriceState::Unknown,
        "A legacy row on a model without entered prices must be unknown");
}

void testModernKnownZeroWithCacheAuditIsNeverRevaluedAsPlainInput()
{
    Model model = flatModel();
    model.cacheReadPriceMicrosPerMillionTokens = 200'000;
    auto modern = row("e1", "flat-model", 1, 0, 0, true);
    modern.cacheReadInputTokens = 1;
    const auto snapshot = snapshotOf({modern}, {model});
    requireAnalytics(snapshot.totals.costMicros == 0
            && snapshot.totals.unknownPriceExecutionCount == 0
            && snapshot.totals.priceState() == ExecutionPriceState::Known,
        "A durable sub-micro cache cost was revalued as plain input");
}

// A zero cost with real usage on a TIER model (or a model that is gone) can no longer be derived: a
// single aggregated input says nothing about which Responses were long. It is unknown, never a
// known zero.
void testATierOrMissingModelNeverTurnsALegacyRowIntoAKnownZero()
{
    const auto tiered = snapshotOf({row("e1", "tier-model", 300'000, 1'000, 0, true)}, {tierModel()});
    requireAnalytics(tiered.totals.costMicros == 0 && tiered.totals.unknownPriceExecutionCount == 1
            && tiered.totals.priceState() == ExecutionPriceState::Unknown,
        "A zero-cost row with usage on a tier model must be unknown, not a known zero");

    const auto missing = snapshotOf({row("e1", "deleted-model", 1'000, 500, 0, true)}, {flatModel()});
    requireAnalytics(missing.totals.costMicros == 0 && missing.totals.unknownPriceExecutionCount == 1
            && missing.totals.priceState() == ExecutionPriceState::Unknown,
        "A zero-cost row whose model is gone must be unknown, not a known zero");
}

// Ten short Responses of 50,000 input tokens sum to 500,000 > 272,000, but not one of them was
// long. The stored cost (ten short Responses at the flat price: 500,000 * $2 = 1,000,000) must be
// reported as stored - never reclassified from the aggregated input as one long Response.
void testAnAggregateAboveTheThresholdIsNeverClassifiedAsLong()
{
    const auto aggregate = snapshotOf({row("e1", "tier-model", 500'000, 0, 1'000'000, true)}, {tierModel()});
    requireAnalytics(aggregate.totals.costMicros == 1'000'000 && aggregate.totals.unknownPriceExecutionCount == 0
            && aggregate.totals.priceState() == ExecutionPriceState::Known,
        "A stored aggregate cost above the threshold must not be reclassified as long");
}

// The state is carried at every level of the projection (totals, provider, model) and combines the
// way the Manifest member projection does: Known / Partial / Unknown / None.
void testPriceStateIsProjectedAtEveryLevelAndCombinesLikeTheManifestProjection()
{
    const auto none = snapshotOf({}, {flatModel()});
    requireAnalytics(none.totals.priceState() == ExecutionPriceState::None, "No execution has no price state");

    const std::vector<ModelExecution> rows {
        row("e1", "flat-model", 100, 50, 100, true),
        row("e2", "flat-model", 100, 50, 0, false),
    };
    const auto mixed = snapshotOf(rows, {flatModel()});
    requireAnalytics(mixed.totals.executionCount == 2 && mixed.totals.costMicros == 100
            && mixed.totals.unknownPriceExecutionCount == 1
            && mixed.totals.priceState() == ExecutionPriceState::Partial,
        "A known and an unknown execution together are a partial total");
    requireAnalytics(mixed.providers.size() == 1 && mixed.providers[0].models.size() == 1,
        "The mixed rows must form one provider with one model");
    requireAnalytics(mixed.providers[0].totals.unknownPriceExecutionCount == 1
            && mixed.providers[0].totals.priceState() == ExecutionPriceState::Partial,
        "The provider aggregate must carry the same price state");
    requireAnalytics(mixed.providers[0].models[0].totals.unknownPriceExecutionCount == 1
            && mixed.providers[0].models[0].totals.priceState() == ExecutionPriceState::Partial,
        "The model aggregate must carry the same price state");

    // An exact zero next to an unknown one with no amount: something WAS priced, so it is a lower
    // bound of zero (partial), not "nothing could be priced" (unknown).
    const auto exactAndUnknown = snapshotOf(
        {row("e1", "tier-model", 0, 0, 0, true), row("e2", "tier-model", 300'000, 1'000, 0, true)},
        {tierModel()});
    requireAnalytics(exactAndUnknown.totals.costMicros == 0 && exactAndUnknown.totals.unknownPriceExecutionCount == 1
            && exactAndUnknown.totals.priceState() == ExecutionPriceState::Partial,
        "An exact zero and an unknown execution together are a partial total");

    const auto allUnknown = snapshotOf(
        {row("e1", "flat-model", 100, 50, 0, false), row("e2", "flat-model", 100, 50, 0, false)}, {flatModel()});
    requireAnalytics(allUnknown.totals.priceState() == ExecutionPriceState::Unknown,
        "Only unknown executions with no amount at all are an unknown total");
}

} // namespace

int runExecutionAnalyticsServiceTests()
{
    testStoredCostAndStoredPriceStateAreAuthoritative();
    testAnExecutionWithoutUsageIsAnExactZero();
    testLegacyFlatRowIsRecomputedWithTheFlatFormula();
    testModernKnownZeroWithCacheAuditIsNeverRevaluedAsPlainInput();
    testATierOrMissingModelNeverTurnsALegacyRowIntoAKnownZero();
    testAnAggregateAboveTheThresholdIsNeverClassifiedAsLong();
    testPriceStateIsProjectedAtEveryLevelAndCombinesLikeTheManifestProjection();
    return 0;
}
