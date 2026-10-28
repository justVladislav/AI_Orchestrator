#include "ai/application/verification/ManifestVerificationContinuationService.hpp"

#include "ai/application/execution/IWorkflowRunner.hpp"
#include "ai/application/verification/ManifestVerificationRepairability.hpp"

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

struct Denial {
    std::string code;
    std::string message;

    [[nodiscard]] bool eligible() const noexcept { return code.empty(); }
};

// Fail-closed eligibility, each return path named after the MW-P5.1 clause
// it implements (ARCHITECTURE_GAPS.md's MW-P5 section): a terminal Manifest
// Run whose frozen Manifest reached combined verification, whose proposal
// safety passed, whose failure is entirely on the content-repairable
// allowlist, and whose exact verified proposal set is still durable and
// untouched since. Reads only already-durable repositories - no provider
// call, no mutation of the source Run.
Denial determineEligibility(
    const Run& sourceRun,
    IManifestWorkLedgerRepository& workLedger,
    IChangeProposalRepository& changeProposals)
{
    if (sourceRun.workflowId != kExecutionManifestWorkflowId) {
        return {"verification_continuation.not_manifest_workflow",
            "Only a Manifest-workflow Run's combined-verification failure is "
            "eligible for continuation"};
    }
    if (sourceRun.status != RunStatus::Failed) {
        return {"verification_continuation.source_run_not_failed",
            "Continuation is only eligible for a terminal Failed Run"};
    }

    const auto ledgerResult = workLedger.findEntry(sourceRun.id);
    if (!ledgerResult.success() || !ledgerResult.value().has_value()) {
        return {"verification_continuation.missing_work_ledger_entry",
            "The source Run has no durable Work Ledger entry"};
    }
    const auto& entry = *ledgerResult.value();

    // QML-P4R.2: the ledger-shape checks (disposition, verification
    // identity, proposal safety, at least one failed required criterion,
    // every diagnostic content-repairable) are shared verbatim with
    // ManifestVerificationManualHandoffService's own mixed-run anchor
    // eligibility - see determineContentRepairableFailureDenial's own doc
    // comment for why the workflowId/status gate above stays here, never
    // moved into the shared function.
    const auto repairableDenial = determineContentRepairableFailureDenial(entry);
    if (!repairableDenial.eligible()) {
        return {repairableDenial.code, repairableDenial.message};
    }
    const auto& verification = *entry.verification;

    // The exact verified proposal set must still be exactly what it was
    // when combined verification ran: untouched since, never approved,
    // applied, rejected, or superseded by a later Run. A later package
    // (MW-P5.3+) reconstructs the continuation candidate from these same
    // ids/hashes; admitting a checkpoint whose members have since moved on
    // would let it reference a set that no longer means what it did here.
    for (const auto& changeSetId : verification.orderedProposalIds) {
        const auto found = changeProposals.findChangeSet(changeSetId);
        if (!found.success() || !found.value().has_value()) {
            return {"verification_continuation.source_proposal_state_changed",
                "A source proposal referenced by the verified set could not be read"};
        }
        if (found.value()->state != ChangeSetState::ReadyForReview) {
            return {"verification_continuation.source_proposal_state_changed",
                "A source proposal has since been reviewed, applied, or superseded"};
        }
    }

    return {};
}

} // namespace

ManifestVerificationContinuationService::ManifestVerificationContinuationService(
    IRunRepository& runs,
    IManifestWorkLedgerRepository& workLedger,
    IChangeProposalRepository& changeProposals,
    IManifestVerificationContinuationRepository& continuations)
    : runs_(runs)
    , workLedger_(workLedger)
    , changeProposals_(changeProposals)
    , continuations_(continuations)
{
}

common::Result<ManifestVerificationContinuationCheckpoint>
ManifestVerificationContinuationService::evaluate(const RunId& sourceRunId) const
{
    const auto existing = continuations_.findBySourceRun(sourceRunId);
    if (!existing.success()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::failure(
            *existing.error());
    }
    if (existing.value().has_value()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::ok(*existing.value());
    }

    const auto foundRun = runs_.findRun(sourceRunId);
    if (!foundRun.success()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::failure(
            *foundRun.error());
    }
    if (!foundRun.value().has_value()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::failure(
            error("verification_continuation.source_run_not_found",
                "The source Run does not exist"));
    }
    const auto& sourceRun = *foundRun.value();

    const auto denial = determineEligibility(sourceRun, workLedger_, changeProposals_);

    ManifestVerificationContinuationCheckpoint checkpoint;
    checkpoint.id = "verification-continuation:" + sourceRunId;
    checkpoint.sourceRunId = sourceRunId;
    checkpoint.sourceProjectId = sourceRun.projectId;
    checkpoint.status = denial.eligible()
        ? ManifestVerificationContinuationStatus::Admitted
        : ManifestVerificationContinuationStatus::Denied;
    checkpoint.denialCode = denial.code;
    checkpoint.denialMessage = denial.message;
    checkpoint.createdAt = std::chrono::system_clock::now();

    return common::Result<ManifestVerificationContinuationCheckpoint>::ok(
        std::move(checkpoint));
}

common::Result<ManifestVerificationContinuationCheckpoint>
ManifestVerificationContinuationService::admitOrEvaluate(const RunId& sourceRunId)
{
    const auto evaluated = evaluate(sourceRunId);
    if (!evaluated.success()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::failure(
            *evaluated.error());
    }

    // evaluate() returns a durable checkpoint unchanged when one already
    // exists. Do not attempt another immutable insert in that case.
    const auto existing = continuations_.findBySourceRun(sourceRunId);
    if (!existing.success()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::failure(
            *existing.error());
    }
    if (existing.value().has_value()) {
        return common::Result<ManifestVerificationContinuationCheckpoint>::ok(
            *existing.value());
    }

    auto checkpoint = evaluated.value();

    const auto inserted = continuations_.insert(checkpoint);
    if (!inserted.success()) {
        // A concurrent admitOrEvaluate call for the same sourceRunId lost
        // the race to insert first - benign: the other call's checkpoint is
        // the durable answer, read it back rather than treating this as a
        // real failure.
        if (inserted.error()->code == "manifest_verification_continuation.already_exists") {
            const auto raced = continuations_.findBySourceRun(sourceRunId);
            if (raced.success() && raced.value().has_value()) {
                return common::Result<ManifestVerificationContinuationCheckpoint>::ok(
                    *raced.value());
            }
        }
        return common::Result<ManifestVerificationContinuationCheckpoint>::failure(
            *inserted.error());
    }
    return common::Result<ManifestVerificationContinuationCheckpoint>::ok(std::move(checkpoint));
}

} // namespace ai::application
