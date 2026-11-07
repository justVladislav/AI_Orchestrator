#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionIntent.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentOutcome.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace ai::application {

// Persistence port of the compaction intents (B3 phase I). Deliberately SEPARATE from IProjectMemoryCompactionRepository: an intent
// is not an immutable history record and must never appear in the (lenient) history listing.
//
// There is no generic delete, update or upsert: a row is inserted once, marked unresolved by a guarded conditional update, and
// closed either by the storage itself - atomically with the matching final record - or by the guarded close below. No implementation
// may offer a way to re-insert, replace or delete by id alone.
class IProjectMemoryCompactionIntentRepository {
public:
    virtual ~IProjectMemoryCompactionIntentRepository() = default;

    // Inserts a NEW open intent (state Open, no markedAt). One storage operation: the insert and the check that no final record is
    // stored under the same operation id are atomic. Admission (CompactionIntentWriteOutcome::newIntentInserted with a Committed
    // state) is the only thing that allows the provider request. A duplicate intent id or a final record already stored under the
    // id is kIntentDuplicateIdCode (even when the content is identical); an unknown project is project_memory.project_not_found.
    virtual CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent& intent) = 0;

    // Strict reads: a damaged row, including a source list that is not the canonical JSON text, is a read failure and is never
    // repaired or skipped. Success with no value is "no such row".
    [[nodiscard]] virtual common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(
        const ProjectMemoryCompactionId& id) const = 0;
    // Ordered by createdAt then id; optionally one state.
    [[nodiscard]] virtual common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId& projectId, std::optional<CompactionIntentState> state) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(
        CompactionIntentState state) const = 0;

    // Open -> Unresolved, only when ALL seven identity fields and the state Open match. `markedAt` is written once. A zero-row
    // result is explained by a read outside any transaction (AlreadyUnresolved / NotFound / IdentityConflict / Unconfirmed); it is
    // never declared "already done" without that proof.
    virtual CompactionIntentMutationOutcome markUnresolvedObserved(
        const CompactionIntentIdentity& expected, std::chrono::system_clock::time_point markedAt) = 0;

    // Deletes the intent only when all seven identity fields match (the state is not compared). Meant for a defensive, verified
    // reconcile; the normal closure is the storage's own atomic closure with the final record. A zero-row result is explained by a
    // read outside any transaction (NotFound / IdentityConflict / Unconfirmed).
    virtual CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity& expected) = 0;

    // The ONE proof operation: the final record and the intent of an operation, read together from one consistent snapshot, with the
    // strict decoding and the expected identity applied to both. Never writes, never repairs. Outside an open transaction only.
    [[nodiscard]] virtual CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity& expected) const = 0;
};

} // namespace ai::application
