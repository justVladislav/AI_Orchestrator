#pragma once

#include "ai/application/verification/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// QML-P4 persistence port. A handoff is immutable and at most one per
// verificationAnchorRunId; an attempt is immutable and append-only, many
// per handoff.
class IManifestVerificationManualHandoffRepository {
public:
    virtual ~IManifestVerificationManualHandoffRepository() = default;

    // Fails with "manifest_verification_manual_handoff.already_exists" when
    // a handoff for this exact verificationAnchorRunId already exists - the
    // same idempotent-duplicate discipline as
    // IManifestVerificationContinuationRepository::insertInput.
    // Also fails with "manifest_verification_manual_handoff.member_already_claimed"
    // when any of this handoff's frozenMembers.proposalId is already a frozen
    // member of a DIFFERENT, already-inserted handoff - a proposal can be
    // frozen into at most one handoff (see findHandoffByMemberProposalId's own
    // doc comment for why this must be a hard invariant, not merely typical).
    virtual common::Result<void> insertHandoff(ManifestVerificationManualHandoff handoff) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationManualHandoff>>
    findHandoffByAnchorRun(const RunId& verificationAnchorRunId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationManualHandoff>>
    findHandoffById(const std::string& handoffId) const = 0;
    // AG-16's reverse-lookup primitive: is this exact proposal id frozen as a
    // member of SOME handoff, regardless of which Run currently owns it or
    // that Run's own workflow? Backed by a normalized, indexed member table
    // (never a scan of a JSON blob or a guess at which Run is "the anchor") -
    // this is what lets AG-16 resolve an unmodified reference correctly even
    // when its owning Run is a continuation child rather than the origin
    // Manifest Run (a frozen member's own owningRunId, not the handoff's
    // anchor, is what actually names that Run - see
    // ManifestVerificationManualHandoffFrozenMember's own doc comment). A
    // proposal id is frozen into at most one handoff, so this returns at
    // most one match.
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationManualHandoff>>
    findHandoffByMemberProposalId(const ChangeSetId& proposalId) const = 0;

    // QML-P4R.3.5: the retention scan primitive - enumerates EVERY
    // handoff, regardless of state, mirroring listAllAttempts()'s own "no
    // filtering, caller decides" contract. Order is unspecified; a
    // retention pass sorts by createdAt itself when it needs oldest-first.
    [[nodiscard]] virtual common::Result<std::vector<ManifestVerificationManualHandoff>>
    listAllHandoffs() const = 0;

    // QML-P4R.3: a genuine, physical delete - the handoffs table's own
    // reject-update trigger makes a "soft discard" flag structurally
    // impossible, and the P4R.1-acceptance review is explicit that a
    // discarded handoff must release its own frozen members' GLOBAL
    // UNIQUE(proposal_id) claim, not merely be hidden, or it would
    // permanently block any future anchor from ever refreezing the same
    // proposal. Transactionally cascades to every attempt AND member row
    // belonging to this handoff (the real schema's ON DELETE CASCADE; the
    // in-memory fake mirrors it explicitly). Fails with
    // "manifest_verification_manual_handoff.not_found" for an unknown id -
    // callers needing "is this call itself idempotent" semantics check that
    // themselves. Deliberately does NOT touch the filesystem candidate
    // directory - that is the caller's own job, and only AFTER this call
    // commits (see ManifestVerificationManualHandoffService::discardHandoff's
    // own doc comment for why that order is the only safe one).
    virtual common::Result<void> discardHandoff(const std::string& handoffId) = 0;

    // Atomic reservation boundary, called before a Run, successor proposal,
    // or verification command is created. Fails with
    // "manifest_verification_manual_handoff.attempt_already_exists" on an id,
    // attemptRunId, or (handoffId, attemptOrdinal) collision. The same edited
    // snapshot may be reserved again only after its previous attempt ended
    // without a Work Ledger result (for example startup-recovered Interrupted).
    virtual common::Result<void> insertAttempt(
        ManifestVerificationManualHandoffAttempt attempt) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>
    findAttemptByRunId(const RunId& attemptRunId) const = 0;
    // Returns the newest attempt for this edited state. A completed attempt
    // (durable Work Ledger entry) is an idempotent replay; an unfinished active
    // attempt blocks a duplicate; an Interrupted/failed reservation without a
    // ledger may be followed by a new ordinal retry of the same snapshot.
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>
    findAttemptByFingerprint(
        const std::string& handoffId, const std::string& editedSnapshotFingerprint) const = 0;
    // Ordered by attemptOrdinal - used to compute the next ordinal and the
    // most recent previousAttemptId.
    [[nodiscard]] virtual common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>
    listAttemptsForHandoff(const std::string& handoffId) const = 0;
    // QML-P4R.3: the startup-reconciliation scan primitive - unlike
    // listAttemptsForHandoff, this enumerates every attempt across every
    // handoff without requiring the caller to already know which handoffs
    // exist, because a crash can leave an attempt row whose OWN handoff
    // lookup path (via its attemptRunId) is exactly what reconciliation is
    // trying to restore. Order is unspecified; a reconciliation pass must
    // not depend on it. Bounded retention (package 5 of this remediation
    // chain) is what keeps this list from growing unbounded over the life
    // of the database - this call itself performs no filtering.
    [[nodiscard]] virtual common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>
    listAllAttempts() const = 0;

    // QML-P4R.3.4: the durable, immutable, at-most-one-per-attempt typed
    // classification introduced by
    // ManifestVerificationManualHandoffAttemptResultKind - never an
    // UPDATE path; a retried attempt always gets its own new attempt row
    // and therefore its own new result row. Cascades with its attempt
    // (ON DELETE CASCADE), the same way an attempt cascades with its
    // handoff. Fails with
    // "manifest_verification_manual_handoff.attempt_result_already_exists"
    // on a duplicate attemptId.
    virtual common::Result<void> insertAttemptResult(
        ManifestVerificationManualHandoffAttemptResult result) = 0;
    [[nodiscard]] virtual common::Result<
        std::optional<ManifestVerificationManualHandoffAttemptResult>>
    findAttemptResultByAttemptId(const std::string& attemptId) const = 0;
};

} // namespace ai::application
