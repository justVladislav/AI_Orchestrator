#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionIntent.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/common/Error.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// B3 phase I: what the intent port reports. Neutral data only (no SQLite or Qt type). Every default is the conservative one: a
// default-constructed value is neither Admitted, nor Healthy, nor proven, nor a statement that anything was or was not sent.

// Whether THIS call of the compaction obtained the right to issue its provider request.
//   NotApplicable  admission was never attempted (the call ended before a client existed). NOT Unknown.
//   Admitted       a NEW intent row of this operation was inserted and the insert is confirmed Committed.
//   NotAdmitted    admission was attempted and not confirmed; the request of this call was not issued.
enum class CompactionIntentAdmission { NotApplicable, Admitted, NotAdmitted };

// What ONE consistent read of the operation (its final record and its intent, under one statement) found.
//   NotObserved      no proof was attempted.
//   IntentPresent    the intent is stored and matches the expected identity; there is no record under this id.
//   FinalRecordPresent  a record under this id matches the expected identity; there is no intent.
//   NeitherPresent   neither exists. This is NOT proof that a request was or was not sent, that the intent was closed, or that the
//                    operation was free: an intent also disappears with its project and may not survive a power loss.
//   Inconsistent     both exist, or a row under this id contradicts the expected identity.
//   Unreadable       the read was attempted and proved nothing (an error, a quarantined connection, a damaged row).
enum class CompactionIntentObservation { NotObserved, IntentPresent, FinalRecordPresent, NeitherPresent, Inconsistent, Unreadable };

// Where an observation comes from.
//   None               nothing was observed.
//   ConfirmedFinalWrite  derived from a Committed final write whose trigger closes the intent in the same transaction (a later phase
//                        sets it; storage reads never do).
//   AtomicPairRead     the one consistent read of the port (observeOperation).
enum class CompactionIntentObservationSource { None, ConfirmedFinalWrite, AtomicPairRead };

// The write of a NEW intent. `persist.state == Committed` and `newIntentInserted` together are the only admission; a duplicate
// intent id, an operation id already used by a final record and every Unknown/NotCommitted/refused result are not.
struct CompactionIntentWriteOutcome {
    CompactionPersistOutcome persist;
    // true only when this call inserted the row and that is confirmed. A duplicate id never sets it, whatever the stored row holds.
    bool newIntentInserted {false};
};

// The result of a guarded state change of ONE intent (mark unresolved, close).
//   Applied        this call changed the row (marked, or deleted).
//   AlreadyApplied (mark only) the row was already unresolved with the same identity; nothing was changed, markedAt is untouched.
//   NotFound       a read outside any transaction showed no row under the id; nothing was changed.
//   IdentityConflict  a row exists but its identity (or, for mark, its state) does not allow the change; nothing was changed.
//   Unconfirmed    the change or the read that would explain a zero-row result could not be proven. NEVER read as Applied.
enum class CompactionIntentMutation { Applied, AlreadyApplied, NotFound, IdentityConflict, Unconfirmed };

struct CompactionIntentMutationOutcome {
    CompactionIntentMutation mutation {CompactionIntentMutation::Unconfirmed};
    // result: the PRIMARY error (success for Applied/AlreadyApplied/NotFound); state: the fate of THIS write (Committed only for
    // Applied). native/diagnostics/connection as for every compaction write.
    CompactionPersistOutcome persist;
};

struct CompactionIntentObservationOutcome {
    CompactionIntentObservation observation {CompactionIntentObservation::NotObserved};
    CompactionIntentObservationSource source {CompactionIntentObservationSource::None};
    // The strict, decoded rows that were read (when they were present and readable). The intent keeps its state/markedAt.
    std::optional<ProjectMemoryCompactionIntent> intent;
    std::optional<CompactionIntentIdentity> recordIdentity;
    // Why the observation is Unreadable (the original read error) or Inconsistent (kIntentObservationInconsistentCode).
    std::optional<common::Error> error;
    std::optional<CompactionNativeDetail> primaryNative;
    std::optional<CompactionConnectionState> connection;
    std::vector<CompactionDiagnostic> diagnostics;
    bool diagnosticsIncomplete {false};
};

} // namespace ai::application
