#pragma once

#include "ai/application/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace ai::application {

class IOutcomeBriefTrigger;

class IChangeApprovalRepository;
class IChangeProposalRepository;
class IChangeProposalStore;
class IProjectRepository;
class IRunRepository;
class ArtifactService;
class ManifestWorkLedgerService;
class ManifestApplyVerificationGate;

// Result of a successful apply.
struct ApplyChangeSetResult {
    std::size_t appliedCount; // number of file operations completed
    // Apply has already succeeded when this is populated. It reports that
    // the best-effort post-apply workspace provenance artifact could not be
    // produced; callers must surface the warning without claiming rollback.
    std::optional<std::string> provenanceErrorCode;
};

// Application service: Stage 8 safe apply pipeline.
//
// Applies a ReadyForReview ProposedChangeSet to the real workspace.
//
// SECURITY INVARIANTS:
//   - Only ReadyForReview proposals are accepted; all other states are blocked.
//   - apply() additionally requires a durable ApprovalRecord (see approve())
//     whose snapshotHash matches the change set's current manifest - a human
//     must have explicitly confirmed applying this exact, immutable snapshot
//     through the UI before any file is touched.
//   - Every resolved path is re-verified to lie strictly inside projectRoot
//     (defence-in-depth on top of the path checks done at proposal-creation time).
//   - Hashes are re-checked against the current on-disk content immediately
//     before each write (stale-world protection; catches concurrent edits).
//   - Delete removes a file only after its current on-disk content matches
//     expectedOriginalHash; Rename requires the source to match that hash and
//     the target to not already exist. Both are still gated end-to-end by
//     ChangeProposalValidator's allowDelete/allowRename policy flags upstream
//     of apply() ever seeing the change set.
//   - The workspace root is resolved from ChangeSet -> Run inside Backend.
//     Neither model output nor a UI parameter can retarget Apply.
//
// ATOMICITY:
//   On any failure, all already-applied changes are rolled back in reverse
//   order.  If rollback itself fails, the change-set state transitions to
//   ApplyFailed to flag that the workspace may be in an indeterminate state.
//   On full success the state transitions to Applied.
//   On apply failure with successful rollback the state remains ReadyForReview
//   (the proposal is still valid and can be retried).
class ChangeSetApplyService {
public:
    // manifestWorkLedgerService is optional (AG-14): when non-null, a
    // successful approve()/apply() appends a durable Approved/Applied
    // Work Ledger transition - but only for a proposal whose owning Run is
    // a kExecutionManifestWorkflowId Run; the service itself is a no-op for
    // any other workflow. See ManifestWorkLedgerService::recordReviewTransition.
    //
    // verificationGate is optional (AG-16.1): when non-null, both approve()
    // and apply() re-run ManifestApplyVerificationGate::evaluate() and fail
    // closed on any denial - see that class's own doc comment for the exact
    // requirement (an authoritative, exact-match, fully-passed
    // ManifestProposalSetVerification for a kExecutionManifestWorkflowId
    // Run's proposal). A null value keeps pre-AG-16.1 behavior unchanged -
    // every existing caller/test.
    ChangeSetApplyService(
        IChangeProposalRepository& repository,
        IChangeProposalStore& store,
        IRunRepository& runs,
        IProjectRepository& projects,
        IChangeApprovalRepository& approvals,
        ArtifactService* artifacts = nullptr,
        ManifestWorkLedgerService* manifestWorkLedgerService = nullptr,
        ManifestApplyVerificationGate* verificationGate = nullptr);

    // Records that a human confirmed applying this exact, currently
    // ReadyForReview snapshot. Idempotent: re-approving the same, unchanged
    // change set returns the existing record rather than erroring. apply()
    // will refuse to run without a matching record - see class doc comment.
    common::Result<ApprovalRecord> approve(
        const ChangeSetId& changeSetId,
        std::optional<std::string> approvedBy = std::nullopt);

    // Apply all changes to the immutable workspace snapshot of the Run that
    // produced this change set. Requires a prior, matching approve() call.
    common::Result<ApplyChangeSetResult> apply(const ChangeSetId& changeSetId);

    // PC-4C: the best-effort nudge to the Outcome Brief reconciler (see IOutcomeBriefTrigger). Set once while the runtime is
    // composed, before any concurrent use; null (the default) means no nudge.
    void setOutcomeBriefTrigger(IOutcomeBriefTrigger* trigger) noexcept { outcomeBriefTrigger_ = trigger; }

private:
    IOutcomeBriefTrigger* outcomeBriefTrigger_ {nullptr};
    // The original apply(), unchanged; apply() wraps it so every exit nudges the brief once.
    common::Result<ApplyChangeSetResult> applyUnnotified(const ChangeSetId& changeSetId);
    IChangeProposalRepository& repository_;
    IChangeProposalStore& store_;
    IRunRepository& runs_;
    IProjectRepository& projects_;
    IChangeApprovalRepository& approvals_;
    ArtifactService* artifacts_;
    ManifestWorkLedgerService* manifestWorkLedgerService_;
    ManifestApplyVerificationGate* verificationGate_;
};

} // namespace ai::application
