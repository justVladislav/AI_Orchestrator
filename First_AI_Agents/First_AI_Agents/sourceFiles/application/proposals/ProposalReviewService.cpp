#include "ai/application/proposals/ProposalReviewService.hpp"

#include "ai/application/planning/IOutcomeBriefTrigger.hpp"

#include "ai/application/execution/ManifestWorkLedgerService.hpp"

#include <utility>

namespace ai::application {

ProposalReviewService::ProposalReviewService(
    IChangeProposalRepository& proposals,
    IProposalReviewRepository& reviews,
    ManifestWorkLedgerService* manifestWorkLedgerService)
    : proposals_(proposals)
    , reviews_(reviews)
    , manifestWorkLedgerService_(manifestWorkLedgerService)
{
}

common::Result<ProposalReviewRejection> ProposalReviewService::reject(
    const ChangeSetId& changeSetId,
    std::optional<std::string> rejectedBy,
    std::optional<std::string> reason)
{
    if (reason.has_value() && reason->size() > kMaximumReasonBytes) {
        return common::Result<ProposalReviewRejection>::failure(
            {"proposal.review_reason_too_large",
             "Rejection reason exceeds the maximum allowed length"});
    }

    const auto existing = reviews_.findRejection(changeSetId);
    if (!existing.success()) {
        return common::Result<ProposalReviewRejection>::failure(*existing.error());
    }
    if (existing.value().has_value()) {
        // Idempotent: a repeated reject command (double click, retried UI
        // call) is not an error - the human's decision already stands.
        return common::Result<ProposalReviewRejection>::ok(*existing.value());
    }

    const auto found = proposals_.findChangeSet(changeSetId);
    if (!found.success()) {
        return common::Result<ProposalReviewRejection>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<ProposalReviewRejection>::failure(
            {"proposal.not_found", "Change set does not exist"});
    }
    if (found.value()->state != ChangeSetState::ReadyForReview) {
        return common::Result<ProposalReviewRejection>::failure(
            {"proposal.not_ready_for_review",
             "Only a ReadyForReview change set can be rejected by a reviewer"});
    }

    ProposalReviewRejection record;
    record.changeSetId = changeSetId;
    record.rejectedAt = std::chrono::system_clock::now();
    record.rejectedBy = std::move(rejectedBy);
    record.reason = std::move(reason);

    const auto recorded = reviews_.recordRejection(record);
    if (!recorded.success()) {
        return common::Result<ProposalReviewRejection>::failure(*recorded.error());
    }
    // AG-14: only the first, genuine rejection - never the idempotent replay
    // above - becomes a Work Ledger transition. Best-effort: failure here
    // must never undo the just-recorded rejection.
    if (manifestWorkLedgerService_ != nullptr) {
        static_cast<void>(manifestWorkLedgerService_->recordReviewTransition(
            changeSetId, ManifestWorkLedgerTransitionKind::Rejected,
            record.rejectedBy, record.reason, std::nullopt));
    }
    // PC-4C: only the first, genuine rejection nudges the Outcome Brief (best effort, after the ledger transition).
    if (outcomeBriefTrigger_ != nullptr) {
        outcomeBriefTrigger_->proposalResolved(changeSetId);
    }
    return common::Result<ProposalReviewRejection>::ok(std::move(record));
}

common::Result<void> ProposalReviewService::assertNotRejected(
    const ChangeSetId& changeSetId) const
{
    const auto existing = reviews_.findRejection(changeSetId);
    if (!existing.success()) {
        return common::Result<void>::failure(*existing.error());
    }
    if (existing.value().has_value()) {
        return common::Result<void>::failure(
            {"proposal.rejected_by_reviewer",
             "This proposal was rejected by a reviewer and cannot be approved or applied"});
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposalReviewRejection>> ProposalReviewService::findRejection(
    const ChangeSetId& changeSetId) const
{
    return reviews_.findRejection(changeSetId);
}

} // namespace ai::application
