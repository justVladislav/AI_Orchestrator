#pragma once

#include "ai/application/ComparisonTypes.hpp"
#include "ai/application/IComparisonSessionRepository.hpp"
#include "ai/application/IProjectRepository.hpp"
#include "ai/application/ModelExecutionService.hpp"
#include "ai/application/ProviderCatalogService.hpp"

namespace ai::application {

/**
 * Owns durable Comparison session state. Candidate attempts themselves are
 * always ModelExecution rows with source=Comparison and sourceId=session.id;
 * this service deliberately does not duplicate provider output, usage, cost,
 * latency, or quality scores in a second result aggregate.
 */
class ComparisonService final {
public:
    ComparisonService(
        IComparisonSessionRepository& sessions,
        IProjectRepository& projects,
        ProviderCatalogService& catalog,
        ModelExecutionService& executions);

    common::Result<ComparisonSession> createSession(
        const CreateComparisonSessionRequest& request);
    common::Result<ComparisonSession> startSession(const ComparisonSessionId& id);
    common::Result<ComparisonSession> finishSession(
        const ComparisonSessionId& id,
        ComparisonSessionStatus status,
        std::string errorCode = {},
        std::string errorMessage = {});
    common::Result<ComparisonSession> setPreferredExecution(
        const ComparisonSessionId& id,
        const std::optional<ModelExecutionId>& executionId);
    common::Result<ComparisonSession> setSynthesisSelection(
        const ComparisonSessionId& id,
        std::vector<ModelExecutionId> executionIds);
    common::Result<ComparisonSession> setSynthesisExecution(
        const ComparisonSessionId& id,
        const std::optional<ModelExecutionId>& executionId);

    [[nodiscard]] common::Result<std::optional<ComparisonSession>> findSession(
        const ComparisonSessionId& id) const;
    [[nodiscard]] common::Result<std::vector<ComparisonSession>> listSessions(
        const std::optional<ProjectId>& projectId = std::nullopt) const;
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listResults(
        const ComparisonSessionId& id) const;
    common::Result<void> recoverInterruptedSessions();

private:
    common::Result<ComparisonSession> loadRequired(const ComparisonSessionId& id) const;
    common::Result<void> validateSelectableExecutions(
        const ComparisonSessionId& id,
        const std::vector<ModelExecutionId>& executionIds) const;

    IComparisonSessionRepository& sessions_;
    IProjectRepository& projects_;
    ProviderCatalogService& catalog_;
    ModelExecutionService& executions_;
};

} // namespace ai::application
