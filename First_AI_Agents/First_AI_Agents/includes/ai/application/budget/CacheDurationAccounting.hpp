#pragma once

#include "ai/models/TokenUsage.hpp"

#include <cstdint>

namespace ai::application {

// ACD-1: provider-independent accounting for cache-creation tokens whose
// provider reports the write duration. The existing aggregate counter remains
// the authority; this value only says whether its duration split is known.
// An unpartitioned value is how legacy rows and providers without detailed
// usage remain representable without pretending that their duration is known.
struct CacheCreationDurationUsage {
    std::uint64_t totalTokens {0};
    bool durationPartitionKnown {false};
    std::uint64_t fiveMinuteTokens {0};
    std::uint64_t oneHourTokens {0};
};

enum class CacheCreationDurationUsageProblem {
    None,
    DetailWithoutKnownPartition,
    PartitionDoesNotMatchTotal
};

// Pure invariant used at every future provider/persistence boundary. When the
// partition is known it must be complete, not merely a subset: otherwise an
// unclassified write could silently receive the cheaper price.
[[nodiscard]] CacheCreationDurationUsageProblem checkCacheCreationDurationUsage(
    const CacheCreationDurationUsage& usage) noexcept;

// Adapter for the durable/provider-facing TokenUsage representation. All
// Application and persistence boundaries use the same ACD-1 rule and stable
// error taxonomy rather than reimplementing its arithmetic.
[[nodiscard]] CacheCreationDurationUsageProblem checkCacheCreationDurationUsage(
    std::uint64_t totalTokens,
    const models::TokenUsage::CacheCreationDurationShare& duration) noexcept;

[[nodiscard]] const char* cacheCreationDurationUsageProblemCode(
    CacheCreationDurationUsageProblem problem) noexcept;

[[nodiscard]] const char* cacheCreationDurationUsageProblemMessage(
    CacheCreationDurationUsageProblem problem) noexcept;

struct CacheCreationDurationPrices {
    // Also prices unpartitioned legacy usage, preserving the current catalog
    // meaning until ACD-2 adds the separately persisted one-hour rate.
    std::uint64_t fiveMinuteMicrosPerMillionTokens {0};
    std::uint64_t oneHourMicrosPerMillionTokens {0};
};

struct CacheCreationDurationCostComponent {
    std::uint64_t costMicros {0};
    // Exact remainder after costMicros, in millionths of one micro.
    // Always below 1,000,000; zero if this component overflowed.
    std::uint64_t remainderMillionthsOfMicro {0};
};

struct CacheCreationDurationCostBreakdown {
    // Best known lower bound. A used category with a missing price contributes
    // zero and makes priceKnown false; arithmetic overflow saturates it.
    std::uint64_t costMicros {0};
    // Exact, independently auditable components. Their whole micros need not
    // sum to costMicros because their remainders are combined before the final
    // truncation; the component pairs do reconstruct that carry exactly.
    CacheCreationDurationCostComponent unpartitionedCost;
    CacheCreationDurationCostComponent fiveMinuteCost;
    CacheCreationDurationCostComponent oneHourCost;
    // Exact remainder after costMicros, in millionths of one micro. ACD-2
    // can combine it with the other input/output categories before the final
    // integer-micro truncation instead of losing sub-micro contributions.
    // Always below 1,000,000; zero when arithmeticOverflow is true.
    std::uint64_t costRemainderMillionthsOfMicro {0};
    std::uint64_t unpricedTokens {0};
    CacheCreationDurationUsageProblem usageProblem {
        CacheCreationDurationUsageProblem::None};
    bool priceKnown {true};
    bool arithmeticOverflow {false};
};

// Prices cache writes only. It deliberately does not touch TokenUsage, Model,
// persistence or production cost call sites: ACD-1 is the pure reviewed seam
// that ACD-2 will embed and wire. Costs are evaluated in integer micros using
// exact quotient/remainder arithmetic, never floating point.
[[nodiscard]] CacheCreationDurationCostBreakdown computeCacheCreationDurationCost(
    const CacheCreationDurationUsage& usage,
    const CacheCreationDurationPrices& prices) noexcept;

} // namespace ai::application
