#pragma once

#include "ai/application/IComparisonSessionRepository.hpp"

#include <mutex>
#include <unordered_map>

namespace ai::application {

class InMemoryComparisonSessionRepository final : public IComparisonSessionRepository {
public:
    common::Result<ComparisonSession> insertSession(ComparisonSession value) override;
    common::Result<ComparisonSession> updateSession(ComparisonSession value) override;
    [[nodiscard]] common::Result<std::optional<ComparisonSession>> findSession(
        const ComparisonSessionId& id) const override;
    [[nodiscard]] common::Result<std::vector<ComparisonSession>> listSessions(
        const std::optional<ProjectId>& projectId = std::nullopt) const override;
    [[nodiscard]] common::Result<std::vector<ComparisonSession>>
    listUnfinishedSessions() const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ComparisonSessionId, ComparisonSession> values_;
};

} // namespace ai::application
