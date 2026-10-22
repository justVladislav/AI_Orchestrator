#pragma once

#include "ai/application/ModelExecutionTypes.hpp"

#include <cstdint>
#include <vector>

namespace ai::application {

// How completely the cost of a set of executions is known. The same four states and the same
// derivation as ManifestUsagePriceState, so a surface that shows a Manifest member's cost and one
// that shows an analytics aggregate cannot disagree about what "unknown" means.
enum class ExecutionPriceState {
    None,     // no execution at all
    Known,    // every execution was fully priced: costMicros is a precise total
    Partial,  // some price was unset or unknowable: costMicros is a lower bound
    Unknown,  // nothing could be priced (costMicros == 0): there is no amount, and it is NOT zero
};

// The stable vocabulary a presentation layer receives ("none" | "known" | "partial" | "unknown"):
// the same strings as the Manifest member projection's price state.
[[nodiscard]] constexpr const char* toString(ExecutionPriceState state) noexcept
{
    switch (state) {
    case ExecutionPriceState::None: return "none";
    case ExecutionPriceState::Known: return "known";
    case ExecutionPriceState::Partial: return "partial";
    case ExecutionPriceState::Unknown: return "unknown";
    }
    return "none";
}

struct ExecutionAggregate {
    std::uint64_t executionCount {0};
    std::uint64_t pendingCount {0};
    std::uint64_t runningCount {0};
    std::uint64_t completedCount {0};
    std::uint64_t partialCount {0};
    std::uint64_t failedCount {0};
    std::uint64_t cancelledCount {0};
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
    std::uint64_t totalLatencyMs {0};
    std::uint64_t averageLatencyMs {0};
    // Executions whose cost is not a precise amount: the stored price was incomplete
    // (costPriceKnown == false), or the row has real usage but no durable pricing result and
    // none can be derived (a tier model, or a model that is gone). costMicros holds only what
    // WAS priced, so with any of these it is a lower bound. Appended last so no positional
    // initialisation shifts.
    std::uint64_t unknownPriceExecutionCount {0};

    [[nodiscard]] constexpr ExecutionPriceState priceState() const noexcept
    {
        if (executionCount == 0) return ExecutionPriceState::None;
        if (unknownPriceExecutionCount == 0) return ExecutionPriceState::Known;
        // A positive amount is an honest lower bound even when every execution was incomplete.
        if (costMicros > 0) return ExecutionPriceState::Partial;
        if (unknownPriceExecutionCount < executionCount) return ExecutionPriceState::Partial;
        // Nothing was priced at all: there is no amount, and it is not zero.
        return ExecutionPriceState::Unknown;
    }
};

struct ProviderExecutionAggregate {
    ProviderId providerId;
    ExecutionAggregate totals;
    struct ModelAggregate {
        ModelId modelId;
        ExecutionAggregate totals;
    };
    std::vector<ModelAggregate> models;
};

struct ExecutionAnalyticsSnapshot {
    ExecutionAggregate totals;
    std::vector<ProviderExecutionAggregate> providers;
};

} // namespace ai::application
