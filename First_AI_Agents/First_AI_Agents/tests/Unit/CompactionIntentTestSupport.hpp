#pragma once

// B3-II-A TEST SUPPORT (never production code): a scripted stand-in for the compaction intent port. It is NOT an alternative backend:
// it keeps nothing durable, understands no SQL and closes nothing on its own. Tests that need real atomicity use the SQLite port.
//
// Default behaviour (nothing scripted): every recordIntentObserved of a NEW id is an admission (Committed, new row, Healthy); a repeated
// id is the typed duplicate refusal; observeOperation answers the stored `defaultProof` (IntentPresent, no cause) so that a test of the
// record accounting does not have to care. Everything a test checks about the intent is counted:
//   recordCalls   the admission writes (one per call of the service, never more)
//   observeCalls  the pair proofs
//   otherCalls    find / list / mark / close - the service must never call them (stays 0)
#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace ai::application::test_support {

class ScriptedCompactionIntentRepository final : public IProjectMemoryCompactionIntentRepository {
public:
    std::deque<CompactionIntentWriteOutcome> writeScript;
    mutable std::deque<CompactionIntentObservationOutcome> proofScript;
    CompactionIntentObservationOutcome defaultProof = [] {
        CompactionIntentObservationOutcome proof;
        proof.observation = CompactionIntentObservation::IntentPresent;
        proof.source = CompactionIntentObservationSource::AtomicPairRead;
        return proof;
    }();

    // Called inside recordIntentObserved after the outcome is decided (e.g. to request a stop right after an admission).
    std::function<void(const ProjectMemoryCompactionIntent&, const CompactionIntentWriteOutcome&)> onRecord;

    std::vector<ProjectMemoryCompactionIntent> recorded;
    mutable std::vector<CompactionIntentIdentity> proofExpected;
    int recordCalls {0};
    mutable int observeCalls {0};
    mutable int otherCalls {0};

    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent& intent) override
    {
        ++recordCalls;
        recorded.push_back(intent);
        CompactionIntentWriteOutcome outcome;
        if (!writeScript.empty()) {
            outcome = std::move(writeScript.front());
            writeScript.pop_front();
        } else if (!usedIds_.insert(intent.id).second) {
            outcome = duplicate();
        } else {
            outcome = admitted();
        }
        if (onRecord) onRecord(intent, outcome);
        return outcome;
    }

    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId&) const override
    {
        ++otherCalls;
        return common::Result<std::optional<ProjectMemoryCompactionIntent>>::ok(std::nullopt);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId&, std::optional<CompactionIntentState>) const override
    {
        ++otherCalls;
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({});
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState) const override
    {
        ++otherCalls;
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({});
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(const CompactionIntentIdentity&, std::chrono::system_clock::time_point) override
    {
        ++otherCalls;
        return {};
    }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity&) override
    {
        ++otherCalls;
        return {};
    }

    [[nodiscard]] CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity& expected) const override
    {
        ++observeCalls;
        proofExpected.push_back(expected);
        if (!proofScript.empty()) {
            CompactionIntentObservationOutcome outcome = std::move(proofScript.front());
            proofScript.pop_front();
            return outcome;
        }
        return defaultProof;
    }

    static CompactionIntentWriteOutcome admitted()
    {
        CompactionIntentWriteOutcome outcome;
        outcome.persist.state = CompactionPersistState::Committed;
        outcome.persist.connection = CompactionConnectionState::Healthy;
        outcome.newIntentInserted = true;
        return outcome;
    }
    static CompactionIntentWriteOutcome duplicate()
    {
        CompactionIntentWriteOutcome outcome;
        outcome.persist.result = common::Result<void>::failure({kIntentDuplicateIdCode, kIntentDuplicateIdMessage});
        outcome.persist.state = CompactionPersistState::NotCommitted;
        outcome.persist.connection = CompactionConnectionState::Healthy;
        return outcome;
    }

private:
    std::set<ProjectMemoryCompactionId> usedIds_;
};

} // namespace ai::application::test_support
