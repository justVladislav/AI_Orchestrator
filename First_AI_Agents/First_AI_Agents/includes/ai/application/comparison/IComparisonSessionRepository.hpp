#pragma once

#include "ai/application/comparison/ComparisonTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

class IComparisonSessionRepository {
public:
    virtual ~IComparisonSessionRepository() = default;

    virtual common::Result<ComparisonSession> insertSession(ComparisonSession value) = 0;
    virtual common::Result<ComparisonSession> updateSession(ComparisonSession value) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ComparisonSession>> findSession(
        const ComparisonSessionId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ComparisonSession>> listSessions(
        const std::optional<ProjectId>& projectId = std::nullopt) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ComparisonSession>>
    listUnfinishedSessions() const = 0;
};

} // namespace ai::application
