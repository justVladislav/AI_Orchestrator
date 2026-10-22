#pragma once

#include "ai/application/IComparisonRepository.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <memory>

namespace ai::infrastructure {

/**
 * SQLite-backed IComparisonRepository.
 *
 * Follows the exact same shared-database pattern as the other Sqlite*
 * repository classes in SqliteRepositories.hpp. The schema v5 migration that
 * creates comparison_sessions and comparison_results runs inside
 * SqliteDatabase::open() alongside all previous migrations.
 *
 * MERGE NOTE: Add `friend class SqliteComparisonRepository;` to the
 * SqliteDatabase friend list in SqliteRepositories.hpp (see the delta file
 * in this CoWork package). Without that friend declaration this class cannot
 * access SqliteDatabase::Impl::handle_ or the shared mutex.
 */
class SqliteComparisonRepository final : public application::IComparisonRepository {
public:
    explicit SqliteComparisonRepository(std::shared_ptr<SqliteDatabase> database);

    application::common::Result<application::ComparisonSession> insertSession(
        application::ComparisonSession session) override;
    application::common::Result<application::ComparisonSession> updateSession(
        application::ComparisonSession session) override;
    [[nodiscard]] application::common::Result<std::optional<application::ComparisonSession>>
        findSessionById(const application::ComparisonId& id) const override;
    [[nodiscard]] application::common::Result<std::vector<application::ComparisonSession>>
        listSessionsByProject(const application::ProjectId& projectId) const override;

    application::common::Result<application::ComparisonResult> insertResult(
        application::ComparisonResult result) override;
    application::common::Result<application::ComparisonResult> updateResult(
        application::ComparisonResult result) override;
    [[nodiscard]] application::common::Result<std::optional<application::ComparisonResult>>
        findResultById(const application::ComparisonResultId& id) const override;
    [[nodiscard]] application::common::Result<std::vector<application::ComparisonResult>>
        listResultsBySession(const application::ComparisonId& comparisonId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
