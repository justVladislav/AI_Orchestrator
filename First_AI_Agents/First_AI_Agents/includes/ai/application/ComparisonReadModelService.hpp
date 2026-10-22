#pragma once

#include "ai/application/ComparisonService.hpp"
#include "ai/application/ProviderCatalogService.hpp"

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
