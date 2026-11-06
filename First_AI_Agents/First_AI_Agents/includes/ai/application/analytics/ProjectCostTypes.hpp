#pragma once

#include "ai/application/analytics/ExecutionAnalyticsTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ai::application {

// A1-min: the project cost estimate is a COMPOSITION, at read time, of two owners that are never merged or rewritten:
//   - the execution ledger (model_executions), summarised by ExecutionAnalyticsService exactly as it is today, and
//   - the stored memory-compaction records (project_memory_compaction_records), classified only by the accepted
//     compactionPriceState - never revalued from the catalog.
// These are the types and the algebra of that composition. Pure: no I/O, no Qt.

// Errors this composition raises itself. A failed required read keeps its ORIGINAL code and message instead.
inline constexpr const char* kProjectCostDuplicateCompactionRecordCode = "project_cost.duplicate_compaction_record";
inline constexpr const char* kProjectCostDuplicateCompactionRecordMessage =
    "The memory compaction history contains duplicate record IDs.";
inline constexpr const char* kProjectCostInvalidDateRangeCode = "project_cost.invalid_date_range";
inline constexpr const char* kProjectCostInvalidDateRangeMessage = "The project cost date range is invalid.";

// What the stored compaction records of one scope add up to. A record counts in exactly ONE of known/partial/unknown/none, so
// recordCount == knownCount + partialCount + unknownCount + noneCount. The money and tokens are those of the STORED records:
// a record whose price state is none carries no cost and no tokens by definition, so it adds nothing to either.
struct CompactionCostAggregate {
    std::uint64_t recordCount {0};
    std::uint64_t knownCount {0};
    std::uint64_t partialCount {0};
    std::uint64_t unknownCount {0};
    // Records without any price data (a failed or cancelled attempt that never received a response). NOT "free", and NOT
    // evidence of a price: they are counted here and nowhere else.
    std::uint64_t noneCount {0};
    std::uint64_t completedCount {0};
    std::uint64_t failedCount {0};
    std::uint64_t cancelledCount {0};
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
};

// One level (total, provider or model) of the composed estimate: the ledger aggregate exactly as the analytics service returned
// it, next to the compaction aggregate. The ledger part is never recomputed or widened here.
struct ProjectCostAggregate {
    ExecutionAggregate ledger;
    CompactionCostAggregate compaction;

    [[nodiscard]] constexpr std::uint64_t costMicros() const noexcept { return ledger.costMicros + compaction.costMicros; }
    [[nodiscard]] constexpr std::uint64_t inputTokens() const noexcept { return ledger.inputTokens + compaction.inputTokens; }
    [[nodiscard]] constexpr std::uint64_t outputTokens() const noexcept { return ledger.outputTokens + compaction.outputTokens; }

    // Ledger rows only. A compaction record is not a ModelExecution and not a "call"; it never enters the execution count or
    // the latency.
    [[nodiscard]] constexpr std::uint64_t ledgerExecutionCount() const noexcept { return ledger.executionCount; }
    [[nodiscard]] constexpr std::uint64_t ledgerAverageLatencyMs() const noexcept { return ledger.averageLatencyMs; }
    [[nodiscard]] constexpr std::uint64_t compactionRecordCount() const noexcept { return compaction.recordCount; }
    [[nodiscard]] constexpr std::uint64_t compactionNoneCount() const noexcept { return compaction.noneCount; }

    // How many items carry any evidence about their price (a none record does not), and how many of those have an incomplete one.
    [[nodiscard]] constexpr std::uint64_t pricingEvidenceCount() const noexcept
    {
        return ledger.executionCount + compaction.knownCount + compaction.partialCount + compaction.unknownCount;
    }
    [[nodiscard]] constexpr std::uint64_t incompletePriceCount() const noexcept
    {
        return ledger.unknownPriceExecutionCount + compaction.partialCount + compaction.unknownCount;
    }

    // The same four states and the same rules as ExecutionAggregate::priceState(), over the combined counters. With no
    // compaction record they are identical to it; a project with only compactions is NOT "none" just because it has no ledger
    // row. The state is about the PRICE of what was recorded, never about whether every operation was recorded.
    [[nodiscard]] constexpr ExecutionPriceState priceState() const noexcept
    {
        const auto evidence = pricingEvidenceCount();
        const auto incomplete = incompletePriceCount();
        if (evidence == 0) return ExecutionPriceState::None;
        if (incomplete == 0) return ExecutionPriceState::Known;
        if (costMicros() > 0 || evidence > incomplete) return ExecutionPriceState::Partial;
        return ExecutionPriceState::Unknown;
    }
};

struct ModelProjectCost {
    ModelId modelId;
    ProjectCostAggregate totals;
};

struct ProviderProjectCost {
    ProviderId providerId;
    ProjectCostAggregate totals;
    std::vector<ModelProjectCost> models;
};

// One read: the total, and the provider and model rows built from the very same data, so every additive field of a level equals
// the sum of the level below it.
struct ProjectCostSnapshot {
    ProjectCostAggregate totals;
    std::vector<ProviderProjectCost> providers;
};

} // namespace ai::application
