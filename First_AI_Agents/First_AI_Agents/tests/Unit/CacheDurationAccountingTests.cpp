#include "ai/application/budget/CacheDurationAccounting.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace ai::application;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

CacheCreationDurationUsage legacy(std::uint64_t total)
{
    CacheCreationDurationUsage usage;
    usage.totalTokens = total;
    return usage;
}

CacheCreationDurationUsage partitioned(
    std::uint64_t total,
    std::uint64_t fiveMinutes,
    std::uint64_t oneHour)
{
    CacheCreationDurationUsage usage;
    usage.totalTokens = total;
    usage.durationPartitionKnown = true;
    usage.fiveMinuteTokens = fiveMinutes;
    usage.oneHourTokens = oneHour;
    return usage;
}

void testLegacyUsageKeepsTheExistingFiveMinutePrice()
{
    const auto cost = computeCacheCreationDurationCost(
        legacy(100), CacheCreationDurationPrices {2'500'000, 4'000'000});
    require(cost.usageProblem == CacheCreationDurationUsageProblem::None,
        "legacy: valid usage was rejected");
    require(cost.priceKnown && !cost.arithmeticOverflow,
        "legacy: a complete price must be exact");
    require(cost.costMicros == 250 && cost.unpartitionedCost.costMicros == 250
            && cost.unpartitionedCost.remainderMillionthsOfMicro == 0,
        "legacy: the existing cache-write price was not preserved");
    require(cost.fiveMinuteCost.costMicros == 0 && cost.oneHourCost.costMicros == 0,
        "legacy: an unknown partition was invented");
}

void testKnownDurationsArePricedSeparately()
{
    const auto cost = computeCacheCreationDurationCost(
        partitioned(200, 80, 120), CacheCreationDurationPrices {2'500'000, 4'000'000});
    require(cost.priceKnown && cost.costMicros == 680,
        "mixed: total duration-aware price is wrong");
    require(cost.fiveMinuteCost.costMicros == 200
            && cost.oneHourCost.costMicros == 480,
        "mixed: duration contributions are wrong");
    require(cost.fiveMinuteCost.remainderMillionthsOfMicro == 0
            && cost.oneHourCost.remainderMillionthsOfMicro == 0,
        "mixed: integral contributions retained a remainder");
    require(cost.unpartitionedCost.costMicros == 0 && cost.unpricedTokens == 0,
        "mixed: valid detail was reported as legacy or unpriced");
}

void testFractionalDurationCostsAreCombinedBeforeRounding()
{
    const auto cost = computeCacheCreationDurationCost(
        partitioned(2, 1, 1), CacheCreationDurationPrices {500'000, 500'000});
    require(cost.fiveMinuteCost.costMicros == 0
            && cost.oneHourCost.costMicros == 0,
        "fraction: component displays must use integer micros");
    require(cost.fiveMinuteCost.remainderMillionthsOfMicro == 500'000
            && cost.oneHourCost.remainderMillionthsOfMicro == 500'000,
        "fraction: per-duration remainders were lost");
    require(cost.costMicros == 1 && cost.priceKnown,
        "fraction: two half-micros must combine into one exact micro");
    require(cost.costRemainderMillionthsOfMicro == 0,
        "fraction: a whole combined micro retained a remainder");

    const auto halfMicro = computeCacheCreationDurationCost(
        partitioned(1, 0, 1), CacheCreationDurationPrices {0, 500'000});
    require(halfMicro.costMicros == 0
            && halfMicro.costRemainderMillionthsOfMicro == 500'000
            && halfMicro.priceKnown,
        "fraction: the exact sub-micro remainder was lost");
}

void testMissingDurationPriceProducesAnHonestLowerBound()
{
    const auto cost = computeCacheCreationDurationCost(
        partitioned(200, 80, 120), CacheCreationDurationPrices {2'500'000, 0});
    require(!cost.priceKnown && !cost.arithmeticOverflow,
        "missing price: lower bound was presented as exact");
    require(cost.costMicros == 200 && cost.fiveMinuteCost.costMicros == 200
            && cost.oneHourCost.costMicros == 0,
        "missing price: known contribution was not retained");
    require(cost.unpricedTokens == 120,
        "missing price: unpriced token count is wrong");

    const auto legacyMissing = computeCacheCreationDurationCost(
        legacy(7), CacheCreationDurationPrices {});
    require(!legacyMissing.priceKnown && legacyMissing.costMicros == 0
            && legacyMissing.unpricedTokens == 7,
        "missing price: unpartitioned legacy usage must also be unknown");
}

void testUnusedPricesMayBeAbsent()
{
    const auto emptyLegacy = computeCacheCreationDurationCost(legacy(0), {});
    const auto onlyFive = computeCacheCreationDurationCost(
        partitioned(10, 10, 0), CacheCreationDurationPrices {3'000'000, 0});
    const auto onlyOneHour = computeCacheCreationDurationCost(
        partitioned(10, 0, 10), CacheCreationDurationPrices {0, 4'000'000});
    require(emptyLegacy.priceKnown && emptyLegacy.costMicros == 0,
        "unused price: empty usage must be known zero");
    require(onlyFive.priceKnown && onlyFive.costMicros == 30,
        "unused price: absent one-hour price must not affect five-minute-only usage");
    require(onlyOneHour.priceKnown && onlyOneHour.costMicros == 40,
        "unused price: absent five-minute price must not affect one-hour-only usage");
}

void testInvalidPartitionsFailClosedWithoutArithmetic()
{
    CacheCreationDurationUsage hiddenDetail = legacy(10);
    hiddenDetail.fiveMinuteTokens = 10;
    const auto hidden = computeCacheCreationDurationCost(
        hiddenDetail, CacheCreationDurationPrices {2'500'000, 4'000'000});
    require(hidden.usageProblem
            == CacheCreationDurationUsageProblem::DetailWithoutKnownPartition,
        "invalid: detail without the known marker was accepted");
    require(!hidden.priceKnown && hidden.costMicros == 0,
        "invalid: malformed usage was priced");

    const CacheCreationDurationUsage mismatches[] = {
        partitioned(10, 6, 3),
        partitioned(10, 11, 0),
        partitioned(10, 0, 11),
    };
    for (const auto& usage : mismatches) {
        const auto cost = computeCacheCreationDurationCost(
            usage, CacheCreationDurationPrices {2'500'000, 4'000'000});
        require(cost.usageProblem
                == CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal,
            "invalid: incomplete or oversized partition was accepted");
        require(!cost.priceKnown && cost.costMicros == 0,
            "invalid: mismatched partition was priced");
    }
}

void testBoundaryArithmeticIsExactOrSaturatingNeverWrapped()
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const auto exactMaximum = computeCacheCreationDurationCost(
        legacy(maximum), CacheCreationDurationPrices {1'000'000, 0});
    require(exactMaximum.priceKnown && !exactMaximum.arithmeticOverflow
            && exactMaximum.costMicros == maximum,
        "overflow: an exactly representable maximum was rejected");

    const auto overflow = computeCacheCreationDurationCost(
        legacy(maximum), CacheCreationDurationPrices {maximum, 0});
    require(!overflow.priceKnown && overflow.arithmeticOverflow
            && overflow.costMicros == maximum
            && overflow.costRemainderMillionthsOfMicro == 0,
        "overflow: an unrepresentable cost wrapped or looked exact");

    const std::uint64_t half = (maximum - 1) / 2;
    const auto combinedOverflow = computeCacheCreationDurationCost(
        partitioned(maximum - 1, half, half),
        CacheCreationDurationPrices {1'000'001, 1'000'001});
    require(combinedOverflow.fiveMinuteCost.costMicros < maximum
            && combinedOverflow.oneHourCost.costMicros < maximum,
        "overflow: a component overflowed before the intended aggregate check");
    require(!combinedOverflow.priceKnown && combinedOverflow.arithmeticOverflow
            && combinedOverflow.costMicros == maximum,
        "overflow: combining duration categories wrapped");
}

void testProblemsHaveStableCodesAndMessages()
{
    require(std::string(cacheCreationDurationUsageProblemCode(
                CacheCreationDurationUsageProblem::None)).empty(),
        "problem text: None must have no code");
    require(std::string(cacheCreationDurationUsageProblemMessage(
                CacheCreationDurationUsageProblem::None)).empty(),
        "problem text: None must have no message");
    require(std::string(cacheCreationDurationUsageProblemCode(
                CacheCreationDurationUsageProblem::DetailWithoutKnownPartition))
            == "cache_duration.detail_without_partition",
        "problem text: detail code changed");
    require(std::string(cacheCreationDurationUsageProblemCode(
                CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal))
            == "cache_duration.partition_total_mismatch",
        "problem text: mismatch code changed");
    require(std::string(cacheCreationDurationUsageProblemMessage(
                CacheCreationDurationUsageProblem::DetailWithoutKnownPartition)).find("known")
            != std::string::npos,
        "problem text: detail message is not actionable");
    require(std::string(cacheCreationDurationUsageProblemMessage(
                CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal)).find("exactly")
            != std::string::npos,
        "problem text: mismatch message is not actionable");
}

void testDurableTokenUsageAdapterUsesTheSameInvariant()
{
    ai::models::TokenUsage::CacheCreationDurationShare duration;
    duration.fiveMinuteTokens = 7;
    require(checkCacheCreationDurationUsage(7, duration)
            == CacheCreationDurationUsageProblem::DetailWithoutKnownPartition,
        "TokenUsage adapter hid detail under an unknown partition");
    duration.known = true;
    require(checkCacheCreationDurationUsage(8, duration)
            == CacheCreationDurationUsageProblem::PartitionDoesNotMatchTotal,
        "TokenUsage adapter accepted an incomplete known partition");
    duration.oneHourTokens = 1;
    require(checkCacheCreationDurationUsage(8, duration)
            == CacheCreationDurationUsageProblem::None,
        "TokenUsage adapter rejected an exact known partition");
}

} // namespace

int main()
{
    try {
        testLegacyUsageKeepsTheExistingFiveMinutePrice();
        testKnownDurationsArePricedSeparately();
        testFractionalDurationCostsAreCombinedBeforeRounding();
        testMissingDurationPriceProducesAnHonestLowerBound();
        testUnusedPricesMayBeAbsent();
        testInvalidPartitionsFailClosedWithoutArithmetic();
        testBoundaryArithmeticIsExactOrSaturatingNeverWrapped();
        testProblemsHaveStableCodesAndMessages();
        testDurableTokenUsageAdapterUsesTheSameInvariant();
        std::cout << "CacheDurationAccountingTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CacheDurationAccountingTests: " << error.what() << '\n';
        return 1;
    }
}
