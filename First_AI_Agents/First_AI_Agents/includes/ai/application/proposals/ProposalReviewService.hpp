#pragma once

#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/proposals/IProposalReviewRepository.hpp"
#include "ai/application/proposals/ProposalReviewTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>

namespace ai::application {

class IOutcomeBriefTrigger;

class ManifestWorkLedgerService;

// Application service for AG-07's explicit human review decision: reject/
// discard a proposal beside the existing approve-and-apply path
// (ChangeSetApplyService), without touching that service, ChangeSetState, or
// ChangeProposalValidator. See ProposalReviewTypes.hpp for the exact
// separation-of-facts rationale.
//
// Callers that lead to ChangeSetApplyService::apply() (the desktop's
// AgentSessionController::approveAndApply()/applyProposal()) must call
// assertNotRejected() first and refuse to proceed on failure - this service
// has no reference to ChangeSetApplyService and cannot enforce that itself.
class ProposalReviewService {
public:
    // Bounded human-entered reason length. Matches the order of magnitude of
    // other short human-facing text fields in this codebase (well below the
    // 64 KiB caps used for bulk model/evidence text) - this is a UI reason
    // field, not a report.
    static constexpr std::size_t kMaximumReasonBytes = 2000;

    // manifestWorkLedgerService is optional (AG-14): when non-null, a
    // genuine (non-idempotent-replay) reject() appends a durable Rejected
    // Work Ledger transition - but only for a proposal whose owning Run is
    // a kExecutionManifestWorkflowId Run; the service itself is a no-op for
    // any other workflow.
    ProposalReviewService(
        IChangeProposalRepository& proposals,
        IProposalReviewRepository& reviews,
        ManifestWorkLedgerService* manifestWorkLedgerService = nullptr);

    // Rejects/discards a ReadyForReview proposal. Idempotent: rejecting an
    // already-rejected change set returns the existing record unchanged
    // (a double-click or repeated command is not an error). Fails with:
    //   - "proposal.not_found" if changeSetId is unknown;
    //   - "proposal.not_ready_for_review" if the change set's current state
    //     is not ChangeSetState::ReadyForReview - a proposal that already
    //     failed validation, already applied, left the workspace in an
    //     indeterminate ApplyFailed state, or produced no changes has
    //     nothing left for a human to discard;
    //   - "proposal.review_reason_too_large" if reason exceeds
    //     kMaximumReasonBytes.
    // Reachable while an ApprovalRecord already exists (state is still
    // ReadyForReview after a failed-apply-with-successful-rollback - see
    // ChangeSetApplyService's own doc comment) so a human can still abandon a
    // proposal whose first apply attempt failed instead of only retrying it.
    common::Result<ProposalReviewRejection> reject(
        const ChangeSetId& changeSetId,
        std::optional<std::string> rejectedBy,
        std::optional<std::string> reason);

    // Fails with "proposal.rejected_by_reviewer" if a rejection record
    // exists for changeSetId; succeeds (returns void) otherwise, including
    // when changeSetId is unknown (a caller that got this far already has
    // its own "not found" handling for that case - this method only ever
    // blocks on an explicit prior rejection).
    [[nodiscard]] common::Result<void> assertNotRejected(
        const ChangeSetId& changeSetId) const;

    [[nodiscard]] common::Result<std::optional<ProposalReviewRejection>> findRejection(
        const ChangeSetId& changeSetId) const;

    // PC-4C: the best-effort nudge to the Outcome Brief reconciler (see IOutcomeBriefTrigger). Set once while the runtime is
    // composed, before any concurrent use; null (the default) means no nudge.
    void setOutcomeBriefTrigger(IOutcomeBriefTrigger* trigger) noexcept { outcomeBriefTrigger_ = trigger; }

private:
    IOutcomeBriefTrigger* outcomeBriefTrigger_ {nullptr};
    IChangeProposalRepository& proposals_;
    IProposalReviewRepository& reviews_;
    ManifestWorkLedgerService* manifestWorkLedgerService_;
};

} // namespace ai::application
