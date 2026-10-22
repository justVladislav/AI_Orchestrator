#pragma once

#include "ai/application/IModelExecutionRepository.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

struct CreateModelExecutionRequest {
    ModelExecutionSource source {ModelExecutionSource::Run};
    std::string sourceId;
    std::optional<ProjectId> projectId;
    ProviderId providerId;
    ModelId modelId;
    std::string role;
    std::uint32_t position {0};
    std::uint32_t attempt {1};
    std::string requestHash;
    std::string contextHash;
};

struct FinishModelExecutionRequest {
    ModelExecutionStatus status {ModelExecutionStatus::Completed};
    std::optional<std::string> outputArtifactId;
    std::string outputContent;
    std::string outputPreview;
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
    // MW-P2: see ModelExecution::costPriceKnown's doc comment.
    bool costPriceKnown {true};
    std::uint64_t latencyMs {0};
    std::string errorCode;
    std::string errorMessage;
    std::uint64_t cacheCreationInputTokens {0};
    std::uint64_t cacheReadInputTokens {0};
    models::TokenUsage::CacheCreationDurationShare cacheCreationDuration;
};

struct CreateQualityEvaluationRequest {
    ModelExecutionId executionId;
    QualityEvaluatorKind evaluatorKind {QualityEvaluatorKind::Automated};
    std::optional<ProviderId> evaluatorProviderId;
    std::optional<ModelId> evaluatorModelId;
    std::string rubricVersion;
    std::uint32_t scoreBasisPoints {0};
    std::string criteriaJson;
    std::string explanation;
    std::string evidence;
};

struct QualityCriterionDefinition {
    std::string id;
    std::uint32_t weightBasisPoints {0};
};

struct QualityCriterionAssessment {
    std::string id;
    std::uint32_t scoreBasisPoints {0};
    std::string evidence;
};

/**
 * Reproducible quality input. The caller supplies criterion-level evidence;
 * the Application Layer validates the complete rubric and computes the final
 * weighted score. UI code and evaluator providers must never supply their own
 * independently calculated overall score.
 */
struct CreateDeterministicQualityEvaluationRequest {
    ModelExecutionId executionId;
    QualityEvaluatorKind evaluatorKind {QualityEvaluatorKind::Automated};
    std::optional<ProviderId> evaluatorProviderId;
    std::optional<ModelId> evaluatorModelId;
    std::string rubricVersion;
    std::vector<QualityCriterionDefinition> rubric;
    std::vector<QualityCriterionAssessment> criteria;
    std::string explanation;
};

/**
 * Application boundary for model-level execution history shared by Runs,
 * Comparison and Chats. Identity and request snapshot fields are immutable
 * after createExecution(); callers may only advance the lifecycle through
 * the explicit commands below.
 */
class ModelExecutionService final {
public:
    explicit ModelExecutionService(IModelExecutionRepository& repository);

    common::Result<ModelExecution> createExecution(const CreateModelExecutionRequest& request);
    common::Result<ModelExecution> startExecution(const ModelExecutionId& id);
    common::Result<ModelExecution> finishExecution(
        const ModelExecutionId& id, const FinishModelExecutionRequest& request);
    common::Result<QualityEvaluation> addEvaluation(const CreateQualityEvaluationRequest& request);
    common::Result<QualityEvaluation> addDeterministicEvaluation(
        const CreateDeterministicQualityEvaluationRequest& request);

    [[nodiscard]] common::Result<std::optional<ModelExecution>> findExecution(
        const ModelExecutionId& id) const;
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listExecutions(
        ModelExecutionSource source, const std::string& sourceId) const;
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listProjectExecutions(
        const ProjectId& projectId) const;
    [[nodiscard]] common::Result<std::vector<QualityEvaluation>> listEvaluations(
        const ModelExecutionId& executionId) const;
    /// Every Pending / Running row. A SETTLER that knows a better truth than the generic orphan recovery (the follow-up
    /// composer reconciling a paid answer whose log write failed) starts from this.
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const;

    /// Converts work abandoned by a previous process into a truthful terminal
    /// state. Safe to call repeatedly; terminal rows remain untouched.
    common::Result<void> recoverInterruptedExecutions();

    /// AG-10 safety net: closes every non-terminal (Pending/Running)
    /// model_executions row still linked to runId (ModelExecutionSource::Run)
    /// as Failed with a stable "model_execution.orphaned" code, without
    /// inventing usage (existing token/cost/output fields are left exactly as
    /// they were). Intended to run once a Run itself has just reached a
    /// terminal disposition (see RunManager::finishRun) - a same-process
    /// complement to recoverInterruptedExecutions()'s startup-only,
    /// whole-process recovery. Safe to call repeatedly; terminal rows remain
    /// untouched.
    common::Result<void> reconcileRunExecutions(const RunId& runId);

private:
    IModelExecutionRepository& repository_;
};

} // namespace ai::application
