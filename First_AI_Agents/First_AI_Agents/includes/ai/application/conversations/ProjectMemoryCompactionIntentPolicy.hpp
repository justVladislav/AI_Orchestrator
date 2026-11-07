#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionIntent.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentOutcome.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

// Pure rules of the compaction intent (B3 phase I): no storage, no clock, no provider. The storage's guard repeats exactly the
// identity rule below in SQL; the two are pinned equal by tests on the domain of valid canonical stored identities.

[[nodiscard]] CompactionIntentIdentity compactionIntentIdentityOf(const ProjectMemoryCompactionIntent& intent);
[[nodiscard]] CompactionIntentIdentity compactionIntentIdentityOf(const ProjectMemoryCompactionRecord& record);

// The FIRST differing field in the fixed order id, project, provider, model, source ids, hash, createdAt - or nullopt when the two
// are the same operation. The order of the source ids is significant (and so are duplicates); createdAt is compared in whole
// milliseconds, exactly as it is stored. Status, result segment, usage, cost and error text are NOT part of the identity: the
// existing identityDifference of record-vs-record (ProjectMemoryCompactionAccounting.hpp) is not usable for an intent.
[[nodiscard]] std::optional<CompactionIntentIdentityField> compactionIntentIdentityDifference(
    const CompactionIntentIdentity& expected, const CompactionIntentIdentity& actual);
[[nodiscard]] std::optional<CompactionIntentIdentityField> compactionIntentIdentityDifference(
    const ProjectMemoryCompactionIntent& intent, const ProjectMemoryCompactionRecord& record);

// A NEW intent must be open, carry no mark and name its operation and project.
[[nodiscard]] common::Result<void> validateNewCompactionIntent(const ProjectMemoryCompactionIntent& intent);

// The only admission: the new intent row of THIS call is inserted and the insert is confirmed Committed. A duplicate, an id already
// used by a final record, NotCommitted, Unknown, a refused entry and a default-constructed outcome never admit.
[[nodiscard]] bool mayDispatch(const CompactionIntentWriteOutcome& outcome) noexcept;
// Admitted exactly when mayDispatch; otherwise NotAdmitted. (NotApplicable is the caller's, for a call that never tried.)
[[nodiscard]] CompactionIntentAdmission compactionIntentAdmissionOf(const CompactionIntentWriteOutcome& outcome) noexcept;

// What the two rows read for ONE operation (already strictly decoded) say about the expected operation. A row under the id that does
// not carry the expected identity, and the pair "both present", are Inconsistent. NeitherPresent proves nothing about delivery.
[[nodiscard]] CompactionIntentObservation classifyCompactionOperation(const CompactionIntentIdentity& expected,
    const std::optional<ProjectMemoryCompactionIntent>& intent, const std::optional<CompactionIntentIdentity>& record);

} // namespace ai::application
