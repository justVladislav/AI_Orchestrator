#pragma once

#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"

#include <string>
#include <utility>

namespace ai::backend::detail {

// The compaction intent port of a runtime whose metadata database is unavailable (B3-II-A). Fail-closed: it never admits, never
// observes and never pretends a write happened, so ProjectMemoryCompactionService can be constructed and called but cannot issue a
// request through it (the service reports project_memory.intent_not_recorded with this storage error kept as its own cause).
// BackendRuntime composes it only for an unavailable database; it has no storage of its own and is not an in-memory backend.
class UnavailableProjectMemoryCompactionIntentRepository final : public application::IProjectMemoryCompactionIntentRepository {
public:
    explicit UnavailableProjectMemoryCompactionIntentRepository(std::string reason) : reason_(std::move(reason)) {}

    application::CompactionIntentWriteOutcome recordIntentObserved(const application::ProjectMemoryCompactionIntent&) override
    {
        application::CompactionIntentWriteOutcome outcome;
        outcome.persist.result = common::Result<void>::failure(error());
        outcome.persist.state = application::CompactionPersistState::NotCommitted;   // nothing was ever started
        outcome.newIntentInserted = false;
        return outcome;
    }

    [[nodiscard]] common::Result<std::optional<application::ProjectMemoryCompactionIntent>> findIntent(
        const application::ProjectMemoryCompactionId&) const override
    {
        return common::Result<std::optional<application::ProjectMemoryCompactionIntent>>::failure(error());
    }
    [[nodiscard]] common::Result<std::vector<application::ProjectMemoryCompactionIntent>> listIntents(
        const application::ProjectId&, std::optional<application::CompactionIntentState>) const override
    {
        return common::Result<std::vector<application::ProjectMemoryCompactionIntent>>::failure(error());
    }
    [[nodiscard]] common::Result<std::vector<application::ProjectMemoryCompactionIntent>> listIntentsByState(
        application::CompactionIntentState) const override
    {
        return common::Result<std::vector<application::ProjectMemoryCompactionIntent>>::failure(error());
    }

    application::CompactionIntentMutationOutcome markUnresolvedObserved(
        const application::CompactionIntentIdentity&, std::chrono::system_clock::time_point) override
    {
        return unconfirmed();
    }
    application::CompactionIntentMutationOutcome closeIntentObserved(const application::CompactionIntentIdentity&) override
    {
        return unconfirmed();
    }

    [[nodiscard]] application::CompactionIntentObservationOutcome observeOperation(
        const application::CompactionIntentIdentity&) const override
    {
        application::CompactionIntentObservationOutcome outcome;
        outcome.observation = application::CompactionIntentObservation::Unreadable;
        outcome.error = error();
        return outcome;
    }

private:
    [[nodiscard]] common::Error error() const { return common::Error {"storage.unavailable", reason_}; }
    [[nodiscard]] application::CompactionIntentMutationOutcome unconfirmed() const
    {
        application::CompactionIntentMutationOutcome outcome;   // Unconfirmed, state Unknown: nothing is claimed
        outcome.persist.result = common::Result<void>::failure(error());
        return outcome;
    }

    std::string reason_;
};

} // namespace ai::backend::detail
