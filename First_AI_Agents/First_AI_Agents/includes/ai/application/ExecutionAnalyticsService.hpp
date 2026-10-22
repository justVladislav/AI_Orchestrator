#pragma once

#include "ai/application/ExecutionAnalyticsTypes.hpp"
#include "ai/application/IModelRepository.hpp"
#include "ai/application/IModelExecutionRepository.hpp"

#include <chrono>
#include <optional>

namespace ai::application {

/**
 * Read-only aggregation boundary over the shared model-execution ledger.
 * QML/controllers consume this projection instead of duplicating cost/token
 * calculations across Overview, Runs, Chats and Comparison.
 */
class ExecutionAnalyticsService final {
public:
    ExecutionAnalyticsService(IModelExecutionRepository& repository,
                              IModelRepository& models);

    [[nodiscard]] common::Result<ExecutionAnalyticsSnapshot> projectSnapshot(
        const ProjectId& projectId,
        std::optional<std::chrono::system_clock::time_point> fromInclusive = std::nullopt,
        std::optional<std::chrono::system_clock::time_point> toExclusive = std::nullopt) const;
    [[nodiscard]] common::Result<ExecutionAnalyticsSnapshot> sourceSnapshot(
        ModelExecutionSource source, const std::string& sourceId) const;

private:
    [[nodiscard]] static ExecutionAnalyticsSnapshot aggregate(
        const std::vector<ModelExecution>& executions,
        const std::vector<Model>& models);

    IModelExecutionRepository& repository_;
    IModelRepository& models_;
};

} // namespace ai::application
