#include "ai/application/CostCalculation.hpp"
#include "ai/application/CacheDurationAccounting.hpp"

#include <algorithm>

namespace ai::application {

namespace {

// A sum that stops at the representable maximum instead of wrapping, so a
// display/audit total can never look small because its inputs were huge.
std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right)
{
    constexpr std::uint64_t maximum = ~std::uint64_t {0};
    return right > maximum - left ? maximum : left + right;
}

// Adds `addend` to `value`, stopping at the representable maximum, and REPORTS whether the exact
// sum did not fit (the value is then saturated: the true count is lost). Adding zero to an
// already-maximal value is not an overflow; adding anything non-zero to it is, and a sum that
// lands exactly on the maximum still fits.
bool addSaturating(std::uint64_t& value, std::uint64_t addend) noexcept
{
    constexpr std::uint64_t maximum = ~std::uint64_t {0};
    if (addend > maximum - value) {
        value = maximum;
        return true;
    }
    value += addend;
    return false;
}

// 2^64, exactly representable as a floating-point value: anything at or above
// it does not fit a std::uint64_t, and casting it would be undefined.
constexpr long double kUint64Range = 18446744073709551616.0L;

// The four counters of one priceable slice of usage (a whole execution, its short
// remainder, or its long-context partition). `input` is the TOTAL input and includes
// both cache subsets, exactly like TokenUsage::inputTokens.
struct Counters {
    std::uint64_t input {0};
    std::uint64_t output {0};
    std::uint64_t cacheWrite {0};
    std::uint64_t cacheRead {0};
    models::TokenUsage::CacheCreationDurationShare cacheCreationDuration;
};

struct Prices {
    std::uint64_t input {0};
    std::uint64_t output {0};
    std::uint64_t cacheWrite {0};
    std::uint64_t cacheRead {0};
    std::uint64_t cacheWriteOneHour {0};
};

struct PricedPart {
    long double cost {0};
    bool known {true};
};

// The cache-breakdown invariant for a slice, written as two subtractions so nothing wraps.
bool countersConsistent(const Counters& counters) noexcept
{
    return counters.cacheWrite <= counters.input
        && counters.cacheRead <= counters.input - counters.cacheWrite;
}

// Prices a CONSISTENT slice (the caller has proven it) category by category: plain input,
// cache-write, cache-read and output each at their own rate. A used category whose price
// was not entered (0) contributes nothing and makes the part unknown - never "free".
PricedPart priceConsistentCounters(const Counters& counters, const Prices& prices) noexcept
{
    const std::uint64_t plain = counters.input - counters.cacheWrite - counters.cacheRead;
    PricedPart part;
    const long double plainCost =
        static_cast<long double>(plain) * prices.input / 1'000'000.0L;
    const auto cacheWrite = computeCacheCreationDurationCost(
        {counters.cacheWrite,
         counters.cacheCreationDuration.known,
         counters.cacheCreationDuration.fiveMinuteTokens,
         counters.cacheCreationDuration.oneHourTokens},
        {prices.cacheWrite, prices.cacheWriteOneHour});
    const long double cacheWriteCost = static_cast<long double>(cacheWrite.costMicros)
        + static_cast<long double>(cacheWrite.costRemainderMillionthsOfMicro) / 1'000'000.0L;
    const long double cacheReadCost =
        static_cast<long double>(counters.cacheRead) * prices.cacheRead / 1'000'000.0L;
    const long double outputCost =
        static_cast<long double>(counters.output) * prices.output / 1'000'000.0L;
    part.cost = plainCost + cacheWriteCost + cacheReadCost + outputCost;
    part.known = !(plain > 0 && prices.input == 0)
        && cacheWrite.priceKnown && !cacheWrite.arithmeticOverflow
        && !(counters.cacheRead > 0 && prices.cacheRead == 0)
        && !(counters.output > 0 && prices.output == 0);
    return part;
}

void mergeCacheCreationDuration(
    models::TokenUsage::CacheCreationDurationShare& total,
    std::uint64_t previousTotalTokens,
    std::uint64_t addedTokens,
    const models::TokenUsage::CacheCreationDurationShare& added,
    bool& trusted,
    bool& saturated) noexcept
{
    if (checkCacheCreationDurationUsage(addedTokens, added)
            != CacheCreationDurationUsageProblem::None) {
        trusted = false;
        total = {};
        return;
    }
    if (!added.known) {
        if (addedTokens > 0) total = {};
        return;
    }
    if (!total.known) {
        if (previousTotalTokens == 0) total = added;
        return;
    }
    if (addSaturating(total.fiveMinuteTokens, added.fiveMinuteTokens)) saturated = true;
    if (addSaturating(total.oneHourTokens, added.oneHourTokens)) saturated = true;
}

} // namespace

UsageTokenBreakdown computeUsageTokenBreakdown(const models::TokenUsage& usage)
{
    // usage.inputTokens is the sum of plain + cache-creation + cache-read
    // (see TokenUsage's class comment); the parser that produced it always
    // maintains that invariant, so this subtraction never underflows in
    // practice, but a saturating clamp keeps a malformed/mocked usage value
    // from wrapping to a huge unsigned "non-cache" count instead of failing
    // safely closed on display/cost accuracy.
    const std::uint64_t nonCacheInputTokens = usage.cacheBreakdownIsConsistent()
        ? usage.inputTokens - usage.cacheCreationInputTokens - usage.cacheReadInputTokens
        : 0;

    UsageTokenBreakdown breakdown;
    breakdown.rawInputTokens = usage.inputTokens;
    breakdown.nonCacheInputTokens = nonCacheInputTokens;
    breakdown.cacheWriteTokens = usage.cacheCreationInputTokens;
    breakdown.cacheReadTokens = usage.cacheReadInputTokens;
    breakdown.outputTokens = usage.outputTokens;
    // rawInputTokens already equals nonCacheInputTokens + cacheWriteTokens +
    // cacheReadTokens (when the invariant above holds), so this reduces to
    // rawInputTokens + outputTokens - never double-counting the cache
    // subsets on top of an already-inclusive raw total. Summed with saturation:
    // a valid but enormous raw total plus its output, or malformed huge cache
    // subsets, must stop at the maximum rather than wrap to a small number.
    breakdown.grandProviderUsageTokens = saturatingAdd(
        saturatingAdd(
            saturatingAdd(nonCacheInputTokens, usage.cacheCreationInputTokens),
            usage.cacheReadInputTokens),
        usage.outputTokens);
    return breakdown;
}

namespace {

// The flat, single-tariff, cache-aware price of ONE already-classified usage: every counter at the
// model's flat prices. PRIVATE on purpose: whether a usage is one Response or an aggregate, and
// which tariff applies to it, is exactly what a caller must say - through
// computeSingleResponseCostMicros() or computeAggregatedExecutionCostMicros() - so no call site can
// price a usage without classifying it.
CostBreakdown priceUsageAtFlatRates(const Model& model, const models::TokenUsage& usage)
{
    const auto breakdown = computeUsageTokenBreakdown(usage);
    const std::uint64_t plainInputTokens = breakdown.nonCacheInputTokens;

    CostBreakdown result;
    const long double plainCost = static_cast<long double>(plainInputTokens)
        * model.inputPriceMicrosPerMillionTokens / 1'000'000.0L;
    const auto durationCacheWrite = computeCacheCreationDurationCost(
        {usage.cacheCreationInputTokens,
         usage.cacheCreationDuration.known,
         usage.cacheCreationDuration.fiveMinuteTokens,
         usage.cacheCreationDuration.oneHourTokens},
        {model.cacheWritePriceMicrosPerMillionTokens,
         model.cacheWriteOneHourPriceMicrosPerMillionTokens});
    const long double cacheWriteCost =
        static_cast<long double>(durationCacheWrite.costMicros)
        + static_cast<long double>(durationCacheWrite.costRemainderMillionthsOfMicro)
            / 1'000'000.0L;
    const long double cacheReadCost = static_cast<long double>(usage.cacheReadInputTokens)
        * model.cacheReadPriceMicrosPerMillionTokens / 1'000'000.0L;
    const long double outputCost = static_cast<long double>(usage.outputTokens)
        * model.outputPriceMicrosPerMillionTokens / 1'000'000.0L;
    const long double totalCost = plainCost + cacheWriteCost + cacheReadCost + outputCost;
    const bool costFitsRange = totalCost < kUint64Range;
    result.costMicros = costFitsRange
        ? static_cast<std::uint64_t>(totalCost)
        : ~std::uint64_t {0};

    // A usage report whose cache subsets do not fit into its own total input
    // (or a cost past the representable range) is malformed: the plain share
    // above was clamped to zero, so the arithmetic is a guess, never an exact
    // price - flag it exactly like a missing price instead of presenting it as
    // known.
    result.priceKnown =
        usage.cacheBreakdownIsConsistent() && costFitsRange
        && !(plainInputTokens > 0 && model.inputPriceMicrosPerMillionTokens == 0)
        && durationCacheWrite.priceKnown && !durationCacheWrite.arithmeticOverflow
        && !(usage.cacheReadInputTokens > 0 && model.cacheReadPriceMicrosPerMillionTokens == 0)
        && !(usage.outputTokens > 0 && model.outputPriceMicrosPerMillionTokens == 0);

    return result;
}

} // namespace

void accumulateResponseUsage(
    models::TokenUsage& total,
    const models::TokenUsage& response,
    std::uint64_t longContextThresholdInputTokens) noexcept
{
    // A counter that saturates has LOST its exact value: the aggregate may no longer claim an
    // exact price. Each add is its own statement - never short-circuited past the others.
    bool saturated = false;
    const std::uint64_t previousCacheCreation = total.cacheCreationInputTokens;
    if (addSaturating(total.inputTokens, response.inputTokens)) saturated = true;
    if (addSaturating(total.outputTokens, response.outputTokens)) saturated = true;
    if (addSaturating(total.cacheCreationInputTokens, response.cacheCreationInputTokens)) {
        saturated = true;
    }
    if (addSaturating(total.cacheReadInputTokens, response.cacheReadInputTokens)) saturated = true;
    mergeCacheCreationDuration(
        total.cacheCreationDuration,
        previousCacheCreation,
        response.cacheCreationInputTokens,
        response.cacheCreationDuration,
        total.accountingTrusted,
        saturated);

    // The partition is Backend-derived: a response is never trusted to bring one. Neither is a
    // response whose own cache breakdown does not fit its input (the budget guard stops such a
    // Run; the aggregate must not look exact) or one that is itself an untrusted aggregate.
    if (!response.longContext.empty() || !response.accountingTrusted
        || !response.cacheBreakdownIsConsistent()) {
        total.accountingTrusted = false;
    }

    // Classified by THIS response's own input, strictly greater - never by the accumulated total.
    if (longContextThresholdInputTokens > 0
        && response.inputTokens > longContextThresholdInputTokens) {
        auto& share = total.longContext;
        const std::uint64_t previousLongCacheCreation = share.cacheCreationInputTokens;
        if (addSaturating(share.inputTokens, response.inputTokens)) saturated = true;
        if (addSaturating(share.outputTokens, response.outputTokens)) saturated = true;
        if (addSaturating(share.cacheCreationInputTokens, response.cacheCreationInputTokens)) {
            saturated = true;
        }
        if (addSaturating(share.cacheReadInputTokens, response.cacheReadInputTokens)) {
            saturated = true;
        }
        mergeCacheCreationDuration(
            share.cacheCreationDuration,
            previousLongCacheCreation,
            response.cacheCreationInputTokens,
            response.cacheCreationDuration,
            total.accountingTrusted,
            saturated);
    }

    // Sticky, like every other loss of trust: nothing ever restores it.
    if (saturated) {
        total.accountingTrusted = false;
    }
}

void mergeAccumulatedUsage(
    models::TokenUsage& total,
    const models::TokenUsage& addend) noexcept
{
    bool saturated = false;
    const std::uint64_t previousCacheCreation = total.cacheCreationInputTokens;
    if (addSaturating(total.inputTokens, addend.inputTokens)) saturated = true;
    if (addSaturating(total.outputTokens, addend.outputTokens)) saturated = true;
    if (addSaturating(total.cacheCreationInputTokens, addend.cacheCreationInputTokens)) {
        saturated = true;
    }
    if (addSaturating(total.cacheReadInputTokens, addend.cacheReadInputTokens)) {
        saturated = true;
    }
    mergeCacheCreationDuration(
        total.cacheCreationDuration,
        previousCacheCreation,
        addend.cacheCreationInputTokens,
        addend.cacheCreationDuration,
        total.accountingTrusted,
        saturated);

    auto& totalLong = total.longContext;
    const auto& addedLong = addend.longContext;
    const std::uint64_t previousLongCacheCreation = totalLong.cacheCreationInputTokens;
    if (addSaturating(totalLong.inputTokens, addedLong.inputTokens)) saturated = true;
    if (addSaturating(totalLong.outputTokens, addedLong.outputTokens)) saturated = true;
    if (addSaturating(
            totalLong.cacheCreationInputTokens, addedLong.cacheCreationInputTokens)) {
        saturated = true;
    }
    if (addSaturating(totalLong.cacheReadInputTokens, addedLong.cacheReadInputTokens)) {
        saturated = true;
    }
    mergeCacheCreationDuration(
        totalLong.cacheCreationDuration,
        previousLongCacheCreation,
        addedLong.cacheCreationInputTokens,
        addedLong.cacheCreationDuration,
        total.accountingTrusted,
        saturated);

    if (!addend.accountingTrusted || !longContextPartitionIsValid(addend)
        || !addend.cacheBreakdownIsConsistent()) {
        total.accountingTrusted = false;
    }
    if (saturated) total.accountingTrusted = false;
}

bool longContextPartitionIsValid(const models::TokenUsage& usage) noexcept
{
    const auto& share = usage.longContext;
    // 1. Subset relation FIRST: nothing below subtracts before it is proven.
    if (share.inputTokens > usage.inputTokens
        || share.outputTokens > usage.outputTokens
        || share.cacheCreationInputTokens > usage.cacheCreationInputTokens
        || share.cacheReadInputTokens > usage.cacheReadInputTokens) {
        return false;
    }
    // 2. The partition is a consistent cache breakdown on its own...
    if (!share.cacheBreakdownIsConsistent()) {
        return false;
    }
    if (checkCacheCreationDurationUsage(
            usage.cacheCreationInputTokens, usage.cacheCreationDuration)
            != CacheCreationDurationUsageProblem::None
        || checkCacheCreationDurationUsage(
            share.cacheCreationInputTokens, share.cacheCreationDuration)
            != CacheCreationDurationUsageProblem::None) {
        return false;
    }
    if (usage.cacheCreationDuration.known && share.cacheCreationDuration.known
        && (share.cacheCreationDuration.fiveMinuteTokens
                > usage.cacheCreationDuration.fiveMinuteTokens
            || share.cacheCreationDuration.oneHourTokens
                > usage.cacheCreationDuration.oneHourTokens)) {
        return false;
    }
    if (usage.cacheCreationDuration.known
        && share.cacheCreationInputTokens > 0
        && !share.cacheCreationDuration.known) {
        return false;
    }
    // 3. ...and so is what is left once it is taken out, and the total.
    Counters remainder {
        usage.inputTokens - share.inputTokens,
        usage.outputTokens - share.outputTokens,
        usage.cacheCreationInputTokens - share.cacheCreationInputTokens,
        usage.cacheReadInputTokens - share.cacheReadInputTokens,
        {}};
    auto durationRemainder = models::TokenUsage::CacheCreationDurationShare {};
    if (usage.cacheCreationDuration.known) {
        durationRemainder.known = true;
        durationRemainder.fiveMinuteTokens = usage.cacheCreationDuration.fiveMinuteTokens
            - (share.cacheCreationDuration.known
                ? share.cacheCreationDuration.fiveMinuteTokens : 0);
        durationRemainder.oneHourTokens = usage.cacheCreationDuration.oneHourTokens
            - (share.cacheCreationDuration.known
                ? share.cacheCreationDuration.oneHourTokens : 0);
    }
    remainder.cacheCreationDuration = durationRemainder;
    return countersConsistent(remainder) && usage.cacheBreakdownIsConsistent();
}

CostBreakdown computeSingleResponseCostMicros(
    const Model& model,
    const models::TokenUsage& responseUsage)
{
    // One classifier for everything: the single response goes through the very same
    // accumulator an execution does, with the threshold of the model that prices it.
    models::TokenUsage aggregate;
    accumulateResponseUsage(
        aggregate, responseUsage, model.longContextPricing.thresholdInputTokens);
    return computeAggregatedExecutionCostMicros(model, aggregate);
}

CostBreakdown computeAggregatedExecutionCostMicros(
    const Model& model,
    const models::TokenUsage& accumulatedUsage)
{
    const models::TokenUsage& usage = accumulatedUsage;
    if (usage.longContext.empty()) {
        // Every response was short (or the tier is off): the original calculation, exactly.
        CostBreakdown flat = priceUsageAtFlatRates(model, usage);
        flat.priceKnown = flat.priceKnown && usage.accountingTrusted;
        return flat;
    }

    CostBreakdown result;
    result.costMicros = 0;
    result.priceKnown = false;
    // A partition that is not provably sound proves nothing - not even the short part - so
    // there is no known cost to report. It is never priced, and never priced as flat.
    if (!longContextPartitionIsValid(usage)) {
        return result;
    }

    const auto& share = usage.longContext;
    const Counters remainder {
        usage.inputTokens - share.inputTokens,
        usage.outputTokens - share.outputTokens,
        usage.cacheCreationInputTokens - share.cacheCreationInputTokens,
        usage.cacheReadInputTokens - share.cacheReadInputTokens,
        {}};
    const Counters longPart {
        share.inputTokens, share.outputTokens,
        share.cacheCreationInputTokens, share.cacheReadInputTokens,
        share.cacheCreationDuration};
    auto shortPartCounters = remainder;
    if (usage.cacheCreationDuration.known) {
        shortPartCounters.cacheCreationDuration.known = true;
        shortPartCounters.cacheCreationDuration.fiveMinuteTokens =
            usage.cacheCreationDuration.fiveMinuteTokens
            - (share.cacheCreationDuration.known
                ? share.cacheCreationDuration.fiveMinuteTokens : 0);
        shortPartCounters.cacheCreationDuration.oneHourTokens =
            usage.cacheCreationDuration.oneHourTokens
            - (share.cacheCreationDuration.known
                ? share.cacheCreationDuration.oneHourTokens : 0);
    }

    const PricedPart shortPart = priceConsistentCounters(shortPartCounters, Prices {
        model.inputPriceMicrosPerMillionTokens,
        model.outputPriceMicrosPerMillionTokens,
        model.cacheWritePriceMicrosPerMillionTokens,
        model.cacheReadPriceMicrosPerMillionTokens,
        model.cacheWriteOneHourPriceMicrosPerMillionTokens});

    // The long partition may only ever be priced at the long-context prices. A model whose tier
    // is disabled has none: the partition then contributes 0 and the result is unknown.
    const auto& tier = model.longContextPricing;
    PricedPart longPriced;
    longPriced.cost = 0;
    longPriced.known = false;
    if (tier.enabled()) {
        longPriced = priceConsistentCounters(longPart, Prices {
            tier.inputPriceMicrosPerMillionTokens,
            tier.outputPriceMicrosPerMillionTokens,
            tier.cacheWritePriceMicrosPerMillionTokens,
            tier.cacheReadPriceMicrosPerMillionTokens,
            tier.cacheWriteOneHourPriceMicrosPerMillionTokens});
    }

    const long double totalCost = shortPart.cost + longPriced.cost;
    const bool costFitsRange = totalCost < kUint64Range;
    result.costMicros = costFitsRange
        ? static_cast<std::uint64_t>(totalCost)
        : ~std::uint64_t {0};
    result.priceKnown =
        shortPart.known && longPriced.known && costFitsRange && usage.accountingTrusted;
    return result;
}

} // namespace ai::application
