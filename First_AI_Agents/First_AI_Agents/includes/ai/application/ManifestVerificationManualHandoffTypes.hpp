#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// QML-P4: a free (no provider/model call), user-authored counterpart to
// MW-P5.3/P5.4's paid verification continuation. Deliberately a SEPARATE
// workflow, never a reuse of kManifestVerificationContinuationWorkflowId:
// that child Run is single-use-then-terminal by construction (repair()
// always calls finishChildRun), while a manual handoff must support an
// unbounded number of free reverify attempts against the SAME immutable
// contract - a shape ManifestVerificationContinuationInput's 1:1-per-source
// uniqueness cannot express.
inline constexpr std::string_view kManifestVerificationManualHandoffWorkflowId =
    "execution-manifest-verification-manual-handoff-v1";

// QML-P4R.1: v2 - a handoff is no longer anchored to a single sourceRunId.
// The real retained scenario is
// origin Manifest Run -> paid continuation child -> manual handoff:
// originManifestRunId owns the frozen ExecutionManifest/tasks, while
// verificationAnchorRunId is the terminal Run whose exact combined
// verification is actually being repaired (they are the SAME Run in the
// simple, never-paid-repaired case; ManifestVerificationManualHandoffService
// currently only materializes that simple case - see its own doc comment
// for why the mixed case fails closed until QML-P4R.2's provider-free
// origin resolution exists). A handoff is keyed unique by
// verificationAnchorRunId: at most one immutable candidate exists per
// terminal verification being repaired, regardless of how many Runs
// contributed members to its exact proposal set.
inline constexpr std::string_view kManifestVerificationManualHandoffSchemaVersion =
    "manifest-verification-manual-handoff.v2";
inline constexpr std::string_view kManifestVerificationManualHandoffAttemptSchemaVersion =
    "manifest-verification-manual-handoff-attempt.v2";
inline constexpr std::string_view kManifestVerificationManualHandoffAttemptResultSchemaVersion =
    "manifest-verification-manual-handoff-attempt-result.v1";

inline constexpr std::size_t kManifestVerificationManualHandoffMaxAllowedPaths = 256;
inline constexpr std::size_t kManifestVerificationManualHandoffMaxFrozenMembers = 64;

// QML-P4R.3.5: bounded retention. A handoff's own materialized candidate
// directory is a real, disk-resident copy of workspace content, so the
// number retained at once is bounded - never left to grow unboundedly
// across an app's lifetime. A handoff younger than the minimum retention
// age is never eligible for automatic eviction regardless of capacity
// pressure, so a user actively mid-edit on a just-materialized candidate
// is never at risk of losing it to a background/reactive sweep.
inline constexpr std::size_t kManifestVerificationManualHandoffMaxRetainedHandoffs = 10;
inline constexpr std::chrono::hours kManifestVerificationManualHandoffMinRetentionAge {24};

// One relative path the user is permitted to edit inside the materialized
// candidate: restricted, at materialize() time, to a Create/Modify target
// that a remaining owner finding (ManifestVerificationFindingAttribution,
// kind SourceProposalOwner/IntegrationOwner) actually names - never every
// file the frozen source proposal set happens to touch. Rename/Delete
// targets are never included (see ManifestVerificationManualHandoffService's
// own doc comment for why Rename is excluded from this MVP).
struct ManifestVerificationManualHandoffAllowedPath {
    std::string relativePath;
    ChangeSetId ownerProposalId;
    std::string ownerTaskId;
};

// QML-P4R.1: one member of the exact verified proposal set the handoff was
// frozen against, replacing the old parallel orderedProposalIds/
// orderedProposalContentHashes arrays (which could only express "every
// member belongs to one sourceRunId"). owningRunId is that member's REAL
// owning Run - for the real retained scenario, most members still belong to
// the origin Manifest Run while at least one paid-repair successor belongs
// to the continuation child. lineageRootProposalId is the ORIGINAL,
// never-superseded proposal id this member's own supersedesChangeSetId
// chain ultimately traces back to (itself, for a member that has never been
// amended by anything) - AG-16's manual-handoff eligibility path uses it to
// prove every frozen member is represented exactly once by itself or one
// genuine successor, regardless of how many repair generations separate
// them.
struct ManifestVerificationManualHandoffFrozenMember {
    ChangeSetId proposalId;
    RunId owningRunId;
    std::string contentHash;
    ChangeSetId lineageRootProposalId;
};

// Immutable, frozen at most once per verification anchor Run: the exact
// reconstructable candidate the user is handed for manual editing, the
// bounded set of paths they may change, and the "before" snapshot every
// free reverify attempt diffs the live (possibly edited) candidate
// directory against. Never mutated after creation - a reverify attempt is
// always a new, sibling ManifestVerificationManualHandoffAttempt row, never
// a change to this one.
struct ManifestVerificationManualHandoff {
    std::string schemaVersion {std::string(kManifestVerificationManualHandoffSchemaVersion)};
    std::string id;
    RunId originManifestRunId;
    RunId verificationAnchorRunId;
    ProjectId sourceProjectId;
    // Safe relative storage key under the Backend-owned manual-handoff root
    // - never an absolute path. The real directory is always
    // reconstructed as (manualHandoffRoot / storageKey) with a canonical
    // containment check before every access, exactly like
    // ManifestProposalOverlayDescriptor::root vs. its trustedScratchRoot.
    std::string storageKey;
    std::string workspaceIdentity;
    std::string baselineFingerprint;
    std::string sourceOverlayFingerprint;
    std::vector<ManifestVerificationManualHandoffFrozenMember> frozenMembers;
    std::vector<ManifestVerificationManualHandoffAllowedPath> allowedPaths;
    // The whole materialized candidate directory's own baseline snapshot,
    // captured immediately after materialization (and confirmed stable by a
    // second, matching capture - see manual_handoff.workspace_unstable)
    // before the user ever opens it. Every free reverify attempt diffs the
    // live directory against exactly this immutable snapshot, never against
    // a previous attempt's edited state.
    WorkspaceBaselineSnapshot originalSnapshot;
    std::chrono::system_clock::time_point createdAt;
};

[[nodiscard]] common::Result<void> validateManifestVerificationManualHandoff(
    const ManifestVerificationManualHandoff& handoff);

// One free reverify attempt against an immutable handoff. Never mutated
// after creation. editedSnapshotFingerprint pins exactly which edited state
// of the candidate directory this attempt verified. attemptOrdinal/
// previousAttemptId are audit-only sibling bookkeeping, never a lineage
// successor chain: every attempt's successors always amend the handoff's
// own frozen members directly (see
// ManifestVerificationManualHandoffService::importAndReverify's own doc
// comment), never a previous attempt's successor.
struct ManifestVerificationManualHandoffAttempt {
    std::string schemaVersion {
        std::string(kManifestVerificationManualHandoffAttemptSchemaVersion)};
    std::string id;
    std::string handoffId;
    RunId verificationAnchorRunId;
    RunId attemptRunId;
    std::uint64_t attemptOrdinal {0};
    std::optional<std::string> previousAttemptId;
    std::string editedSnapshotFingerprint;
    std::chrono::system_clock::time_point createdAt;
};

[[nodiscard]] common::Result<void> validateManifestVerificationManualHandoffAttempt(
    const ManifestVerificationManualHandoffAttempt& attempt);

// QML-P4R.3.4: the durable, Backend-owned classification of how an
// attempt ended - the typed alternative to inferring verified-vs-
// retryable status by probing for ledger presence. Deliberately never
// derived from an error message's text: only from which structural event
// actually produced it (a Work Ledger commit vs. an early return with no
// ledger commit) - see ManifestVerificationManualHandoffService's own
// call sites for the single, non-textual rule each value is written
// under.
enum class ManifestVerificationManualHandoffAttemptResultKind {
    // ManifestProposalSetVerifier::verify() itself succeeded AND the
    // required acceptance passed. The Work Ledger's own entry for this
    // attempt Run is the ultimate authority; this row is always writable
    // (or self-healable after a crash) from that entry's terminalStatus
    // alone. A later call against the SAME edited fingerprint always
    // replays this result - never re-verifies.
    VerifiedSuccess,
    // ManifestProposalSetVerifier::verify() itself succeeded but the
    // required acceptance did NOT pass - a genuine, authoritative content
    // verdict, still backed by its own Work Ledger entry exactly like
    // VerifiedSuccess. A later call against the SAME edited fingerprint
    // always replays this result - never re-verifies, and never treats a
    // content failure as retryable.
    VerifiedContentFailure,
    // QML-P4R.3.4 hardening: the edited candidate was rejected BEFORE the
    // final combined ManifestProposalSetVerifier::verify() call could even
    // run - a deterministic judgment of the edited bytes themselves
    // (materializeRepairSuccessor's own schema/safety-scan/policy/hash-
    // evidence validation rejecting the amendment, or a bounded per-file
    // content check such as the edited-file-too-large limit), never a
    // transient/tool/infrastructure condition. Distinguished structurally
    // (by WHICH call in ManifestVerificationManualHandoffService::
    // importAndReverify failed - never by inspecting its error text) from
    // Incomplete below: retrying the exact SAME edited bytes against a
    // deterministic rejection can never produce a different outcome, so a
    // later call against the same fingerprint always replays this SAME
    // rejection (via rejectionCode/rejectionMessage below) rather than
    // allocating an unbounded number of new ordinals for a verdict that
    // will never change without the user actually editing something.
    RejectedBeforeVerification,
    // The attempt ended WITHOUT ever producing a Work Ledger entry for a
    // reason that is NOT a deterministic verdict on the edited bytes: a
    // genuine transient/tool/infrastructure failure (a repository read,
    // overlay build, or the verifier call itself failing structurally) or
    // a cancellation. A later call against the SAME edited fingerprint
    // always allocates a fresh ordinal retry, never replays this row -
    // retrying might genuinely succeed once the transient condition
    // clears, unlike RejectedBeforeVerification above.
    Incomplete,
};

// One attempt's own typed result - immutable and at most one per
// attemptId, cascade-deleted with its attempt (ON DELETE CASCADE) rather
// than living as mutable state on the attempt row itself. A retried
// attempt for the same edited fingerprint always gets a brand-new
// attemptId (and therefore its own new result row) - this row is NEVER
// updated in place.
struct ManifestVerificationManualHandoffAttemptResult {
    std::string schemaVersion {
        std::string(kManifestVerificationManualHandoffAttemptResultSchemaVersion)};
    std::string id;
    std::string attemptId;
    RunId attemptRunId;
    ManifestVerificationManualHandoffAttemptResultKind kind {
        ManifestVerificationManualHandoffAttemptResultKind::Incomplete};
    // Populated if and only if kind == RejectedBeforeVerification - the
    // stable (code, message) a later call against the same edited
    // fingerprint replays verbatim, exactly as the original rejecting
    // call itself returned it (never re-derived or re-worded).
    std::optional<std::string> rejectionCode;
    std::optional<std::string> rejectionMessage;
    std::chrono::system_clock::time_point recordedAt;
};

[[nodiscard]] common::Result<void> validateManifestVerificationManualHandoffAttemptResult(
    const ManifestVerificationManualHandoffAttemptResult& result);

} // namespace ai::application
