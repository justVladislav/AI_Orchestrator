#pragma once

#include "ai/application/IComparisonSessionRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace ai::infrastructure {

class SqliteComparisonSessionRepository final
    : public application::IComparisonSessionRepository {
public:
    explicit SqliteComparisonSessionRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ComparisonSession> insertSession(
        application::ComparisonSession value) override;
    common::Result<application::ComparisonSession> updateSession(
        application::ComparisonSession value) override;
    [[nodiscard]] common::Result<std::optional<application::ComparisonSession>> findSession(
        const application::ComparisonSessionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ComparisonSession>> listSessions(
        const std::optional<application::ProjectId>& projectId = std::nullopt) const override;
    [[nodiscard]] common::Result<std::vector<application::ComparisonSession>>
    listUnfinishedSessions() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
