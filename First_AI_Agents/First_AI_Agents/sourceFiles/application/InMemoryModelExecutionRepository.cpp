#include "ai/application/InMemoryRepositories.hpp"

#include "ai/application/budget/CacheDurationAccounting.hpp"

#include <algorithm>
#include <utility>

namespace ai::application {

namespace {
common::Error executionError(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

common::Result<void> validateCacheUsage(const ModelExecution& value)
{
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(executionError(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<void>::failure(executionError(
            "model_execution.invalid_cache_usage",
            "Cache-creation and cache-read tokens must fit within input tokens"));
    }
    return common::Result<void>::ok();
}
}

common::Result<ModelExecution> InMemoryModelExecutionRepository::insertExecution(ModelExecution value)
{
    const auto validCacheUsage = validateCacheUsage(value);
    if (!validCacheUsage.success()) {
        return common::Result<ModelExecution>::failure(*validCacheUsage.error());
    }
    std::lock_guard lock(mutex_);
    if (value.id.empty() || executions_.contains(value.id))
        return common::Result<ModelExecution>::failure(executionError("model_execution.duplicate", "Execution id is empty or already exists"));
    const auto duplicateIdentity = std::ranges::find_if(executions_, [&](const auto& entry) {
        const auto& existing = entry.second;
        return existing.source == value.source
            && existing.sourceId == value.sourceId
            && existing.position == value.position
            && existing.attempt == value.attempt;
    });
    if (duplicateIdentity != executions_.end()) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.duplicate_position_attempt",
            "Execution source, position and attempt must be unique"));
    }
    executions_.emplace(value.id, value);
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<ModelExecution> InMemoryModelExecutionRepository::updateExecution(ModelExecution value)
{
    const auto validCacheUsage = validateCacheUsage(value);
    if (!validCacheUsage.success()) {
        return common::Result<ModelExecution>::failure(*validCacheUsage.error());
    }
    std::lock_guard lock(mutex_);
    const auto found = executions_.find(value.id);
    if (found == executions_.end())
        return common::Result<ModelExecution>::failure(executionError("model_execution.not_found", "Execution not found"));
    const auto duplicateIdentity = std::ranges::find_if(executions_, [&](const auto& entry) {
        const auto& existing = entry.second;
        return entry.first != value.id
            && existing.source == value.source
            && existing.sourceId == value.sourceId
            && existing.position == value.position
            && existing.attempt == value.attempt;
    });
    if (duplicateIdentity != executions_.end()) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.duplicate_position_attempt",
            "Execution source, position and attempt must be unique"));
    }
    found->second = value;
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<std::optional<ModelExecution>> InMemoryModelExecutionRepository::findExecution(const ModelExecutionId& id) const
{
    std::lock_guard lock(mutex_);
    const auto found = executions_.find(id);
    return common::Result<std::optional<ModelExecution>>::ok(found == executions_.end() ? std::nullopt : std::optional<ModelExecution>{found->second});
}

common::Result<std::vector<ModelExecution>> InMemoryModelExecutionRepository::listExecutions(ModelExecutionSource source, const std::string& sourceId) const
{
    std::lock_guard lock(mutex_);
    std::vector<ModelExecution> values;
    for (const auto& [id, value] : executions_) if (value.source == source && value.sourceId == sourceId) values.push_back(value);
    std::ranges::sort(values, {}, [](const ModelExecution& value) { return std::pair{value.position, value.createdAt}; });
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> InMemoryModelExecutionRepository::listProjectExecutions(const ProjectId& projectId) const
{
    std::lock_guard lock(mutex_);
    std::vector<ModelExecution> values;
    for (const auto& [id, value] : executions_) if (value.projectId == projectId) values.push_back(value);
    std::ranges::sort(values, {}, &ModelExecution::createdAt);
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> InMemoryModelExecutionRepository::listUnfinishedExecutions() const
{
    std::lock_guard lock(mutex_);
    std::vector<ModelExecution> values;
    for (const auto& [id, value] : executions_) {
        static_cast<void>(id);
        if (!isTerminal(value.status)) values.push_back(value);
    }
    std::ranges::sort(values, {}, &ModelExecution::createdAt);
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<QualityEvaluation> InMemoryModelExecutionRepository::insertEvaluation(QualityEvaluation value)
{
    std::lock_guard lock(mutex_);
    if (!executions_.contains(value.executionId))
        return common::Result<QualityEvaluation>::failure(executionError("model_execution.not_found", "Execution not found"));
    if (value.scoreBasisPoints > 10000 || value.id.empty() || evaluations_.contains(value.id))
        return common::Result<QualityEvaluation>::failure(executionError("quality_evaluation.invalid", "Evaluation id or score is invalid"));
    evaluations_.emplace(value.id, value);
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

common::Result<std::optional<QualityEvaluation>> InMemoryModelExecutionRepository::findEvaluation(const QualityEvaluationId& id) const
{
    std::lock_guard lock(mutex_);
    const auto found = evaluations_.find(id);
    return common::Result<std::optional<QualityEvaluation>>::ok(found == evaluations_.end() ? std::nullopt : std::optional<QualityEvaluation>{found->second});
}

common::Result<std::vector<QualityEvaluation>> InMemoryModelExecutionRepository::listEvaluations(const ModelExecutionId& executionId) const
{
    std::lock_guard lock(mutex_);
    std::vector<QualityEvaluation> values;
    for (const auto& [id, value] : evaluations_) if (value.executionId == executionId) values.push_back(value);
    std::ranges::sort(values, {}, &QualityEvaluation::createdAt);
    return common::Result<std::vector<QualityEvaluation>>::ok(std::move(values));
}

} // namespace ai::application
