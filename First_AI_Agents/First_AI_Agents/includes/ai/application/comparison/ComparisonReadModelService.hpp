#pragma once

#include "ai/application/analytics/ExecutionAnalyticsTypes.hpp"
#include "ai/application/comparison/ComparisonService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

struct ComparisonResultReadModel {
    ModelExecution execution;
    std::string providerName;
    std::string modelName;
    std::optional<QualityEvaluation> latestEvaluation;
    bool preferred {false};
    bool selectedForSynthesis {false};
};

struct ComparisonSummaryReadModel {
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
    std::uint64_t averageLatencyMs {0};
    std::optional<ModelExecutionId> recommendedExecutionId;
    std::string recommendationSource;
    bool qualityComplete {false};
    bool synthesisEnabled {false};
    // The completeness of costMicros, over EXACTLY the executions it was summed from (every result of the
    // session, whatever its status: a failed or cancelled attempt that was charged is still a cost). Appended
    // last so no positional initialisation shifts. `unknownPriceExecutionCount` counts the results whose stored
    // costPriceKnown is false; it is sticky - one unpriced result keeps the total incomplete however many known
    // ones sit next to it. The stored amounts are summed as recorded, never re-priced from today's catalog.
    std::uint64_t executionCount {0};
    std::uint64_t unknownPriceExecutionCount {0};

    // The shared price vocabulary (none | known | partial | unknown) of the same rule every other cost
    // aggregate uses, so a comparison total cannot disagree with Analytics about what "unknown" means.
    [[nodiscard]] ExecutionPriceState priceState() const noexcept
    {
        ExecutionAggregate aggregate;
        aggregate.executionCount = executionCount;
        aggregate.costMicros = costMicros;
        aggregate.unknownPriceExecutionCount = unknownPriceExecutionCount;
        return aggregate.priceState();
    }
};

struct ComparisonSessionReadModel {
    ComparisonSession session;
    std::vector<ComparisonResultReadModel> results;
    ComparisonSummaryReadModel summary;
};

/**
 * Authoritative read projection for Comparison. It deliberately lives in the
 * Application Layer so Qt/QML never calculates usage totals, quality winners,
 * preferred state, or synthesis eligibility.
 */
class ComparisonReadModelService final {
public:
    ComparisonReadModelService(
        ComparisonService& comparisons,
        ModelExecutionService& executions,
        ProviderCatalogService& catalog);

    [[nodiscard]] common::Result<ComparisonSessionReadModel> session(
        const ComparisonSessionId& id) const;

private:
    ComparisonService& comparisons_;
    ModelExecutionService& executions_;
    ProviderCatalogService& catalog_;
};

} // namespace ai::application
