#pragma once

#include "ai/application/ComparisonTypes.hpp"
#include "ai/application/IComparisonRepository.hpp"
#include "ai/application/IProjectRepository.hpp"
#include "ai/common/Result.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ai::application {

/**
 * Application service for comparison session lifecycle.
 *
 * ComparisonService is intentionally I/O-free: it reads/writes through
 * IComparisonRepository and IProjectRepository only. Parallel provider
 * execution and Qt event dispatch are the responsibility of the Qt-layer
 * ComparisonController which calls the state-mutation methods here from its
 * own worker thread(s).
 */
class ComparisonService {
public:
    ComparisonService(
        IComparisonRepository& repository,
        IProjectRepository& projects,
        std::function<std::string()> idGenerator);

    // Creates a session and inserts a Pending ComparisonResult stub for each slot.
    // Fails with "comparison.project_not_found" if projectId does not exist.
    // Fails with "comparison.no_slots" if slots is empty.
    [[nodiscard]] common::Result<ComparisonSession> createSession(CreateComparisonRequest request);

    [[nodiscard]] common::Result<std::vector<ComparisonSession>> listSessionsByProject(
        const ProjectId& projectId) const;

    [[nodiscard]] common::Result<std::vector<ComparisonResult>> listResultsBySession(
        const ComparisonId& comparisonId) const;

    [[nodiscard]] common::Result<std::optional<ComparisonResult>> findResult(
        const ComparisonResultId& resultId) const;

    // Marks one result as Running (called by the worker before sending to a provider).
    [[nodiscard]] common::Result<void> setResultRunning(const ComparisonResultId& resultId);

    // Stores a successful provider response.
    [[nodiscard]] common::Result<void> setResultCompleted(
        const ComparisonResultId& resultId,
        const std::string& content,
        std::uint64_t inputTokens,
        std::uint64_t outputTokens,
        std::int64_t latencyMs,
        const std::string& totalCost);

    // Records a provider failure.
    [[nodiscard]] common::Result<void> setResultFailed(
        const ComparisonResultId& resultId,
        const std::string& errorMessage);

    // Records a partial result (content truncated / max-tokens hit).
    [[nodiscard]] common::Result<void> setResultPartial(
        const ComparisonResultId& resultId,
        const std::string& content,
        std::uint64_t inputTokens,
        std::uint64_t outputTokens,
        std::int64_t latencyMs,
        const std::string& totalCost);

    // Resets a result back to Pending for retry.
    [[nodiscard]] common::Result<void> resetResultPending(const ComparisonResultId& resultId);

    // Sets exactly one result as "best" within its session; clears the flag on all others.
    [[nodiscard]] common::Result<void> setResultBest(
        const ComparisonId& comparisonId,
        const ComparisonResultId& resultId);

    // Clears the "best" designation (no result becomes best).
    [[nodiscard]] common::Result<void> clearResultBest(const ComparisonId& comparisonId);

    // Toggles selectedForSynthesis on the given result (only if status=Completed).
    [[nodiscard]] common::Result<void> setResultSynthesisSelected(
        const ComparisonResultId& resultId,
        bool selected);

private:
    IComparisonRepository& repository_;
    IProjectRepository& projects_;
    std::function<std::string()> idGenerator_;
};

} // namespace ai::application
