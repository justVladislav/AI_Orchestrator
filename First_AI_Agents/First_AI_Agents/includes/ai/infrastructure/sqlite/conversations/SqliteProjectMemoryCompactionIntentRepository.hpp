#pragma once

#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <memory>

namespace ai::infrastructure {

/**
 * SQLite implementation of the compaction intent port (B3 phase I). It shares the connection and the mutex of every other repository,
 * honours the connection quarantine and never exposes a raw handle. The closure of an intent by the matching final record is done
 * by triggers of schema step 75, not by this class.
 */
class SqliteProjectMemoryCompactionIntentRepository final : public application::IProjectMemoryCompactionIntentRepository {
public:
    explicit SqliteProjectMemoryCompactionIntentRepository(std::shared_ptr<SqliteDatabase> database);

    application::CompactionIntentWriteOutcome recordIntentObserved(const application::ProjectMemoryCompactionIntent& intent) override;

    [[nodiscard]] common::Result<std::optional<application::ProjectMemoryCompactionIntent>> findIntent(
        const application::ProjectMemoryCompactionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ProjectMemoryCompactionIntent>> listIntents(
        const application::ProjectId& projectId, std::optional<application::CompactionIntentState> state) const override;
    [[nodiscard]] common::Result<std::vector<application::ProjectMemoryCompactionIntent>> listIntentsByState(
        application::CompactionIntentState state) const override;

    application::CompactionIntentMutationOutcome markUnresolvedObserved(
        const application::CompactionIntentIdentity& expected, std::chrono::system_clock::time_point markedAt) override;
    application::CompactionIntentMutationOutcome closeIntentObserved(const application::CompactionIntentIdentity& expected) override;

    [[nodiscard]] application::CompactionIntentObservationOutcome observeOperation(
        const application::CompactionIntentIdentity& expected) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
