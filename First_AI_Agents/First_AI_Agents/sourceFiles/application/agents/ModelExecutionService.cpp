#include "ai/application/agents/ModelExecutionService.hpp"

#include "ai/application/CacheDurationAccounting.hpp"

#include "ai/application/IdGenerator.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {
namespace {

common::Error executionError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

template <typename T>
common::Result<T> propagate(const common::Result<std::optional<ModelExecution>>& result)
{
    return common::Result<T>::failure(*result.error());
}

} // namespace

ModelExecutionService::ModelExecutionService(IModelExecutionRepository& repository)
    : repository_(repository)
{
}

common::Result<ModelExecution> ModelExecutionService::createExecution(
    const CreateModelExecutionRequest& request)
{
    if (request.sourceId.empty() || request.providerId.empty() || request.modelId.empty()
        || request.attempt == 0) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.invalid_request", "Source, provider and model are required"));
    }

    ModelExecution value;
    value.id = generateUuid();
    value.source = request.source;
    value.sourceId = request.sourceId;
    value.projectId = request.projectId;
    value.providerId = request.providerId;
    value.modelId = request.modelId;
    value.role = request.role;
    value.position = request.position;
    value.attempt = request.attempt;
    value.status = ModelExecutionStatus::Pending;
    value.requestHash = request.requestHash;
    value.contextHash = request.contextHash;
    value.createdAt = std::chrono::system_clock::now();
    return repository_.insertExecution(std::move(value));
}

common::Result<ModelExecution> ModelExecutionService::startExecution(const ModelExecutionId& id)
{
    auto found = repository_.findExecution(id);
    if (!found.success()) return propagate<ModelExecution>(found);
    if (!found.value().has_value())
        return common::Result<ModelExecution>::failure(
            executionError("model_execution.not_found", "Execution not found"));

    ModelExecution value = *found.value();
    if (value.status != ModelExecutionStatus::Pending) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.invalid_transition", "Only a pending execution can start"));
    }
    value.status = ModelExecutionStatus::Running;
    value.startedAt = std::chrono::system_clock::now();
    return repository_.updateExecution(std::move(value));
}

common::Result<ModelExecution> ModelExecutionService::finishExecution(
    const ModelExecutionId& id, const FinishModelExecutionRequest& request)
{
    if (!isTerminal(request.status)) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.invalid_outcome", "An execution outcome must be terminal"));
    }

    auto found = repository_.findExecution(id);
    if (!found.success()) return propagate<ModelExecution>(found);
    if (!found.value().has_value())
        return common::Result<ModelExecution>::failure(
            executionError("model_execution.not_found", "Execution not found"));

    ModelExecution value = *found.value();
    if (value.status != ModelExecutionStatus::Pending
        && value.status != ModelExecutionStatus::Running) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.invalid_transition", "A finished execution cannot be changed"));
    }
    if ((request.status == ModelExecutionStatus::Completed
            || request.status == ModelExecutionStatus::Partial)
        && value.status != ModelExecutionStatus::Running) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.invalid_transition", "A successful outcome requires a running execution"));
    }

    value.status = request.status;
    value.outputArtifactId = request.outputArtifactId;
    value.outputContent = request.outputContent;
    value.outputPreview = request.outputPreview;
    value.inputTokens = request.inputTokens;
    value.outputTokens = request.outputTokens;
    value.costMicros = request.costMicros;
    value.costPriceKnown = request.costPriceKnown;
    value.latencyMs = request.latencyMs;
    value.errorCode = request.errorCode;
    value.errorMessage = request.errorMessage;
    value.cacheCreationInputTokens = request.cacheCreationInputTokens;
    value.cacheReadInputTokens = request.cacheReadInputTokens;
    value.cacheCreationDuration = request.cacheCreationDuration;
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<ModelExecution>::failure(executionError(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<ModelExecution>::failure(executionError(
            "model_execution.invalid_cache_usage",
            "Model execution cache usage is inconsistent with its input total"));
    }
    value.finishedAt = std::chrono::system_clock::now();
    return repository_.updateExecution(std::move(value));
}

common::Result<QualityEvaluation> ModelExecutionService::addEvaluation(
    const CreateQualityEvaluationRequest& request)
{
    if (request.executionId.empty() || request.rubricVersion.empty()
        || request.scoreBasisPoints > 10'000) {
        return common::Result<QualityEvaluation>::failure(executionError(
            "quality_evaluation.invalid", "Execution, rubric and a score from 0 to 10000 are required"));
    }
    if (request.evaluatorKind == QualityEvaluatorKind::BlindedModel
        && (!request.evaluatorProviderId.has_value() || !request.evaluatorModelId.has_value())) {
        return common::Result<QualityEvaluation>::failure(executionError(
            "quality_evaluation.evaluator_required", "A blinded model evaluator requires provider and model"));
    }

    auto execution = repository_.findExecution(request.executionId);
    if (!execution.success())
        return common::Result<QualityEvaluation>::failure(*execution.error());
    if (!execution.value().has_value())
        return common::Result<QualityEvaluation>::failure(
            executionError("model_execution.not_found", "Execution not found"));
    if (execution.value()->status != ModelExecutionStatus::Completed
        && execution.value()->status != ModelExecutionStatus::Partial) {
        return common::Result<QualityEvaluation>::failure(executionError(
            "quality_evaluation.execution_not_reviewable", "Only completed or partial output can be evaluated"));
    }

    QualityEvaluation value;
    value.id = generateUuid();
    value.executionId = request.executionId;
    value.evaluatorKind = request.evaluatorKind;
    value.evaluatorProviderId = request.evaluatorProviderId;
    value.evaluatorModelId = request.evaluatorModelId;
    value.rubricVersion = request.rubricVersion;
    value.scoreBasisPoints = request.scoreBasisPoints;
    value.criteriaJson = request.criteriaJson;
    value.explanation = request.explanation;
    value.evidence = request.evidence;
    value.createdAt = std::chrono::system_clock::now();
    return repository_.insertEvaluation(std::move(value));
}

common::Result<QualityEvaluation> ModelExecutionService::addDeterministicEvaluation(
    const CreateDeterministicQualityEvaluationRequest& request)
{
    if (request.rubricVersion.empty() || request.rubric.empty()) {
        return common::Result<QualityEvaluation>::failure(executionError(
            "quality_evaluation.rubric_required",
            "A versioned, non-empty quality rubric is required"));
    }

    std::unordered_map<std::string, std::uint32_t> weights;
    std::uint64_t totalWeight = 0;
    for (const auto& criterion : request.rubric) {
        if (criterion.id.empty() || criterion.weightBasisPoints == 0
            || criterion.weightBasisPoints > 10'000
            || !weights.emplace(criterion.id, criterion.weightBasisPoints).second) {
            return common::Result<QualityEvaluation>::failure(executionError(
                "quality_evaluation.invalid_rubric",
                "Rubric criterion ids and positive weights must be unique"));
        }
        totalWeight += criterion.weightBasisPoints;
    }
    if (totalWeight != 10'000) {
        return common::Result<QualityEvaluation>::failure(executionError(
            "quality_evaluation.invalid_rubric_weight",
            "Quality rubric weights must total exactly 10000 basis points"));
    }

    std::unordered_map<std::string, const QualityCriterionAssessment*> assessments;
    for (const auto& criterion : request.criteria) {
        if (criterion.id.empty() || criterion.scoreBasisPoints > 10'000
            || criterion.evidence.empty()
            || !assessments.emplace(criterion.id, &criterion).second) {
            return common::Result<QualityEvaluation>::failure(executionError(
                "quality_evaluation.invalid_criteria",
                "Criterion ids must be unique and each score requires evidence"));
        }
    }
    if (assessments.size() != weights.size()) {
        return common::Result<QualityEvaluation>::failure(executionError(
            "quality_evaluation.incomplete_criteria",
            "Every rubric criterion must have exactly one assessment"));
    }

    std::uint64_t weightedScore = 0;
    nlohmann::ordered_json criteriaJson = nlohmann::ordered_json::array();
    nlohmann::ordered_json evidenceJson = nlohmann::ordered_json::array();
    for (const auto& definition : request.rubric) {
        const auto found = assessments.find(definition.id);
        if (found == assessments.end()) {
            return common::Result<QualityEvaluation>::failure(executionError(
                "quality_evaluation.incomplete_criteria",
                "Every rubric criterion must have exactly one assessment"));
        }
        const auto& assessment = *found->second;
        weightedScore += static_cast<std::uint64_t>(definition.weightBasisPoints)
            * assessment.scoreBasisPoints;
        criteriaJson.push_back({
            {"id", definition.id},
            {"weight_basis_points", definition.weightBasisPoints},
            {"score_basis_points", assessment.scoreBasisPoints}});
        evidenceJson.push_back({{"id", definition.id}, {"evidence", assessment.evidence}});
    }

    // Integer half-up rounding makes the result stable across compilers and
    // avoids floating-point differences in persisted Comparison scores.
    const auto overallScore = static_cast<std::uint32_t>((weightedScore + 5'000) / 10'000);

    CreateQualityEvaluationRequest persisted;
    persisted.executionId = request.executionId;
    persisted.evaluatorKind = request.evaluatorKind;
    persisted.evaluatorProviderId = request.evaluatorProviderId;
    persisted.evaluatorModelId = request.evaluatorModelId;
    persisted.rubricVersion = request.rubricVersion;
    persisted.scoreBasisPoints = overallScore;
    persisted.criteriaJson = criteriaJson.dump();
    persisted.explanation = request.explanation;
    persisted.evidence = evidenceJson.dump();
    return addEvaluation(persisted);
}

common::Result<std::optional<ModelExecution>> ModelExecutionService::findExecution(
    const ModelExecutionId& id) const
{
    return repository_.findExecution(id);
}

common::Result<std::vector<ModelExecution>> ModelExecutionService::listExecutions(
    ModelExecutionSource source, const std::string& sourceId) const
{
    return repository_.listExecutions(source, sourceId);
}

common::Result<std::vector<ModelExecution>> ModelExecutionService::listProjectExecutions(
    const ProjectId& projectId) const
{
    return repository_.listProjectExecutions(projectId);
}

common::Result<std::vector<ModelExecution>> ModelExecutionService::listUnfinishedExecutions() const
{
    return repository_.listUnfinishedExecutions();
}

common::Result<std::vector<QualityEvaluation>> ModelExecutionService::listEvaluations(
    const ModelExecutionId& executionId) const
{
    return repository_.listEvaluations(executionId);
}

common::Result<void> ModelExecutionService::recoverInterruptedExecutions()
{
    auto unfinished = repository_.listUnfinishedExecutions();
    if (!unfinished.success()) return common::Result<void>::failure(*unfinished.error());

    for (ModelExecution value : unfinished.value()) {
        // PC-5C-2b: follow-up composer rows have a stronger durable owner. Its request may
        // already contain the paid response's usage and price, so the follow-up startup pass
        // must restore that exact outcome before anything can flatten the row into a generic
        // interruption. It also closes no-response follow-up rows itself. The generic pass
        // therefore never owns this source, regardless of startup call order.
        if (value.source == ModelExecutionSource::FollowUp) continue;
        value.status = ModelExecutionStatus::Failed;
        value.errorCode = kModelExecutionInterruptedErrorCode;
        value.errorMessage = "Recovered after an interrupted application session";
        value.finishedAt = std::chrono::system_clock::now();
        auto updated = repository_.updateExecution(std::move(value));
        if (!updated.success()) return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> ModelExecutionService::reconcileRunExecutions(const RunId& runId)
{
    auto executions = repository_.listExecutions(ModelExecutionSource::Run, runId);
    if (!executions.success()) return common::Result<void>::failure(*executions.error());

    for (ModelExecution value : executions.value()) {
        if (isTerminal(value.status)) continue;
        value.status = ModelExecutionStatus::Failed;
        value.errorCode = kModelExecutionOrphanedErrorCode;
        value.errorMessage =
            "The owning Run reached a terminal state while this execution "
            "was still Pending or Running";
        value.finishedAt = std::chrono::system_clock::now();
        auto updated = repository_.updateExecution(std::move(value));
        if (!updated.success()) return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
