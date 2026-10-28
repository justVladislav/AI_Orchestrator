#include "ai/application/budget/CacheDurationAccounting.hpp"

#include <limits>

namespace ai::application {

namespace {

constexpr std::uint64_t kTokensPerMillion = 1'000'000;
constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();

struct ScaledCost {
    std::uint64_t wholeMicros {0};
    std::uint64_t remainder {0};
    bool overflow {false};
};

CacheCreationDurationCostComponent toPublicComponent(const ScaledCost& value) noexcept
{
    return {value.wholeMicros, value.overflow ? 0 : value.remainder};
}

bool checkedAdd(std::uint64_t& value, std::uint64_t addend) noexcept
{
    if (addend > kMaximum - value) {
        value = kMaximum;
        return false;
    }
    value += addend;
    return true;
}

bool checkedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& product) noexcept
{
    if (left != 0 && right > kMaximum / left) {
        product = kMaximum;
        return false;
    }
    product = left * right;
    return true;
}

// Exact floor(tokens * microsPerMillion / 1,000,000), plus the remainder.
// Splitting both operands avoids a wide integer type and stays portable to
// MSVC, where unsigned __int128 is unavailable.
ScaledCost scaleTokens(std::uint64_t tokens, std::uint64_t price) noexcept
{
    const std::uint64_t tokenMillions = tokens / kTokensPerMillion;
    const std::uint64_t tokenRemainder = tokens % kTokensPerMillion;
    const std::uint64_t priceMillions = price / kTokensPerMillion;
    const std::uint64_t priceRemainder = price % kTokensPerMillion;

    ScaledCost result;
    std::uint64_t wholeMillions = 0;
    std::uint64_t remainderAtWholePrice = 0;
    if (!checkedMultiply(tokenMillions, price, wholeMillions)
        || !checkedMultiply(tokenRemainder, priceMillions, remainderAtWholePrice)
        || !checkedAdd(wholeMillions, remainderAtWholePrice)) {
        result.wholeMicros = kMaximum;
        result.overflow = true;
        return result;
    }

    // Both factors are below one million, so this product is below 10^12.
    const std::uint64_t fractionalProduct = tokenRemainder * priceRemainder;
    if (!checkedAdd(wholeMillions, fractionalProduct / kTokensPerMillion)) {
        result.wholeMicros = kMaximum;
        result.overflow = true;
        return result;
    }
    result.wholeMicros = wholeMillions;
    result.remainder = fractionalProduct % kTokensPerMillion;
    return result;
}

void addScaledCost(
    CacheCreationDurationCostBreakdown& result,
    const ScaledCost& part,
    std::uint64_t& remainder) noexcept
{
    if (result.arithmeticOverflow || part.overflow) {
        result.costMicros = kMaximum;
        result.arithmeticOverflow = true;
        return;
    }

    if (!checkedAdd(result.costMicros, part.wholeMicros)) {
        result.arithmeticOverflow = true;
        return;
    }

    // At most two duration categories are combined; nevertheless use checked
    // addition so this helper remains honest if the pure model grows later.
    if (!checkedAdd(remainder, part.remainder)) {
        result.costMicros = kMaximum;
        result.arithmeticOverflow = true;
        return;
    }
    const std::uint64_t carry = remainder / kTokensPerMillion;
    remainder %= kTokensPerMillion;
    if (!checkedAdd(result.costMicros, carry)) {
        result.arithmeticOverflow = true;
    }
}

} // namespace

CacheCreationDurationUsageProblem checkCacheCreationDurationUsage(
    const CacheCreationDurationUsage& usage) noexcept
{
    if (!usage.durationPartitionKnown) {
        return usage.fiveMinuteTokens == 0 && usage.oneHourTokens == 0
            ? CacheCreationDurationUsageProblem::None
            : CacheCreationDurationUsageProblem::DetailWithoutKnownPartition;
    }

    if (usage.fiveMinuteTokens > usage.totalTokens
        || usage.oneHourTokens != usage.totalTokens - usage.fiveMinuteTokens) {
        return CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal;
    }
    return CacheCreationDurationUsageProblem::None;
}

CacheCreationDurationUsageProblem checkCacheCreationDurationUsage(
    std::uint64_t totalTokens,
    const models::TokenUsage::CacheCreationDurationShare& duration) noexcept
{
    return checkCacheCreationDurationUsage({
        totalTokens,
        duration.known,
        duration.fiveMinuteTokens,
        duration.oneHourTokens});
}

const char* cacheCreationDurationUsageProblemCode(
    CacheCreationDurationUsageProblem problem) noexcept
{
    switch (problem) {
    case CacheCreationDurationUsageProblem::None:
        return "";
    case CacheCreationDurationUsageProblem::DetailWithoutKnownPartition:
        return "cache_duration.detail_without_partition";
    case CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal:
        return "cache_duration.partition_total_mismatch";
    }
    return "cache_duration.unknown_problem";
}

const char* cacheCreationDurationUsageProblemMessage(
    CacheCreationDurationUsageProblem problem) noexcept
{
    switch (problem) {
    case CacheCreationDurationUsageProblem::None:
        return "";
    case CacheCreationDurationUsageProblem::DetailWithoutKnownPartition:
        return "Cache-duration token details require a known partition.";
    case CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal:
        return "Cache-duration token details must exactly equal the aggregate cache-creation tokens.";
    }
    return "Unknown cache-duration accounting problem.";
}

CacheCreationDurationCostBreakdown computeCacheCreationDurationCost(
    const CacheCreationDurationUsage& usage,
    const CacheCreationDurationPrices& prices) noexcept
{
    CacheCreationDurationCostBreakdown result;
    result.usageProblem = checkCacheCreationDurationUsage(usage);
    if (result.usageProblem != CacheCreationDurationUsageProblem::None) {
        result.priceKnown = false;
        return result;
    }

    std::uint64_t combinedRemainder = 0;
    if (!usage.durationPartitionKnown) {
        const ScaledCost legacy = scaleTokens(
            usage.totalTokens, prices.fiveMinuteMicrosPerMillionTokens);
        result.unpartitionedCost = toPublicComponent(legacy);
        addScaledCost(result, legacy, combinedRemainder);
        if (usage.totalTokens > 0 && prices.fiveMinuteMicrosPerMillionTokens == 0) {
            result.unpricedTokens = usage.totalTokens;
            result.priceKnown = false;
        }
    } else {
        const ScaledCost fiveMinute = scaleTokens(
            usage.fiveMinuteTokens, prices.fiveMinuteMicrosPerMillionTokens);
        const ScaledCost oneHour = scaleTokens(
            usage.oneHourTokens, prices.oneHourMicrosPerMillionTokens);
        result.fiveMinuteCost = toPublicComponent(fiveMinute);
        result.oneHourCost = toPublicComponent(oneHour);
        addScaledCost(result, fiveMinute, combinedRemainder);
        addScaledCost(result, oneHour, combinedRemainder);

        if (usage.fiveMinuteTokens > 0 && prices.fiveMinuteMicrosPerMillionTokens == 0) {
            checkedAdd(result.unpricedTokens, usage.fiveMinuteTokens);
            result.priceKnown = false;
        }
        if (usage.oneHourTokens > 0 && prices.oneHourMicrosPerMillionTokens == 0) {
            checkedAdd(result.unpricedTokens, usage.oneHourTokens);
            result.priceKnown = false;
        }
    }

    if (result.arithmeticOverflow) {
        result.costMicros = kMaximum;
        result.costRemainderMillionthsOfMicro = 0;
        result.priceKnown = false;
    } else {
        result.costRemainderMillionthsOfMicro = combinedRemainder;
    }
    return result;
}

} // namespace ai::application
