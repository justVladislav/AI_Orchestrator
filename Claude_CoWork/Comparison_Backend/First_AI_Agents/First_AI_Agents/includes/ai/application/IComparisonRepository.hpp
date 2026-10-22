#pragma once

#include "ai/application/ComparisonTypes.hpp"
#include "ai/common/Result.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace ai::application {

/**
 * Persistence port for comparison sessions and their per-provider results.
 *
 * Application services depend on this interface; in-memory and SQLite
 * implementations remain replaceable infrastructure details.
 */
class IComparisonRepository {
public:
    virtual ~IComparisonRepository() = default;

    // Session operations
    virtual common::Result<ComparisonSession> insertSession(ComparisonSession session) = 0;
    virtual common::Result<ComparisonSession> updateSession(ComparisonSession session) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ComparisonSession>> findSessionById(
        const ComparisonId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ComparisonSession>> listSessionsByProject(
        const ProjectId& projectId) const = 0;

    // Result operations
    virtual common::Result<ComparisonResult> insertResult(ComparisonResult result) = 0;
    virtual common::Result<ComparisonResult> updateResult(ComparisonResult result) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ComparisonResult>> findResultById(
        const ComparisonResultId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ComparisonResult>> listResultsBySession(
        const ComparisonId& comparisonId) const = 0;
};

// Factory for in-memory implementation (tests and unavailable-storage fallback).
[[nodiscard]] std::unique_ptr<IComparisonRepository> makeInMemoryComparisonRepository();

} // namespace ai::application
