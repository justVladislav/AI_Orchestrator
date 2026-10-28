#pragma once

#include "ai/application/agents/ModelExecutionTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

class IModelExecutionRepository {
public:
    virtual ~IModelExecutionRepository() = default;
    virtual common::Result<ModelExecution> insertExecution(ModelExecution value) = 0;
    virtual common::Result<ModelExecution> updateExecution(ModelExecution value) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ModelExecution>> findExecution(const ModelExecutionId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ModelExecution>> listExecutions(ModelExecutionSource source, const std::string& sourceId) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ModelExecution>> listProjectExecutions(const ProjectId& projectId) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const = 0;
    virtual common::Result<QualityEvaluation> insertEvaluation(QualityEvaluation value) = 0;
    [[nodiscard]] virtual common::Result<std::optional<QualityEvaluation>> findEvaluation(const QualityEvaluationId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<QualityEvaluation>> listEvaluations(const ModelExecutionId& executionId) const = 0;
};

} // namespace ai::application
