#include "ai/application/ManifestApplyVerificationGate.hpp"

#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/IExecutionManifestRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/ManifestVerificationManualHandoffApplyAuthority.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestProposalContentHash.hpp"
#include "ai/application/IVerificationBootstrapRepository.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestWorkLedgerTypes.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

ManifestApplyEligibility allow(std::string code)
{
    return ManifestApplyEligibility {true, std::move(code), std::string()};
}

ManifestApplyEligibility deny(std::string code, std::string message)
{
    return ManifestApplyEligibility {false, std::move(code), std::move(message)};
}

// AG-16.1 hardening: gathers this proposal's already-stored content into
// the canonical hash-input shape and delegates to
// computeManifestProposalContentHash - the single algorithm shared with
// ManifestProposalOverlay::build() (see ManifestProposalContentHash.hpp's
// own doc comment). This gate never touches the filesystem or depends on
// any overlay having ever been built; it only reads a proposal's own
// durable, immutable stored content (change_id/operation/relative_path/
// source_path, plus a SHA-256 of the stored proposed content for Create/
// Modify - Delete/Rename carry no content of their own).
common::Result<std::string> computeProposalContentHash(
    const ChangeSetId& proposalId,
    const std::vector<ProposedFileChange>& changes,
    ChangeProposalService& proposals)
{
    std::vector<ManifestProposalContentHashChangeInput> inputs;
    inputs.reserve(changes.size());
    for (const auto& change : changes) {
        ManifestProposalContentHashChangeInput input;
        input.operation = change.operation;
        input.relativePath = change.relativePath;
        input.sourcePath = change.sourcePath;
        if (change.operation == FileChangeOperation::Create
                || change.operation == FileChangeOperation::Modify) {
            const auto content = proposals.readProposedContent(proposalId, change.changeId);
            if (!content.success()) {
                return common::Result<std::string>::failure(*content.error());
            }
            input.contentSha256 = sha256Hex(content.value());
        }
        inputs.push_back(std::move(input));
    }
    return common::Result<std::string>::ok(
        computeManifestProposalContentHash(proposalId, inputs));
}

common::Result<ChangeSetId> resolveProposalLineageRoot(
    ChangeProposalService& proposals, const ChangeSetId& proposalId)
{
    std::unordered_set<ChangeSetId> visited;
    std::optional<ProjectId> projectId;
    ChangeSetId current = proposalId;
    for (int hops = 0; hops < 32; ++hops) {
        if (!visited.insert(current).second) {
            return common::Result<ChangeSetId>::failure({
                "manifest_apply.manual_handoff_lineage_corrupt",
                "A manual-handoff proposal lineage contains a cycle"});
        }
        const auto proposal = proposals.findChangeSet(current);
        if (!proposal.success()) {
            return common::Result<ChangeSetId>::failure({
                "manifest_apply.manual_handoff_lineage_corrupt",
                "A manual-handoff proposal lineage member could not be read"});
        }
        if (!projectId.has_value()) projectId = proposal.value().projectId;
        if (proposal.value().projectId != *projectId) {
            return common::Result<ChangeSetId>::failure({
                "manifest_apply.manual_handoff_lineage_corrupt",
                "A manual-handoff proposal lineage crosses project boundaries"});
        }
        if (!proposal.value().supersedesChangeSetId.has_value()) {
            return common::Result<ChangeSetId>::ok(current);
        }
        current = *proposal.value().supersedesChangeSetId;
    }
    return common::Result<ChangeSetId>::failure({
        "manifest_apply.manual_handoff_lineage_corrupt",
        "A manual-handoff proposal lineage exceeds the bounded hop limit"});
}

// MW-P5.4: the continuation-workflow counterpart to the ordinary
// kExecutionManifestWorkflowId eligibility checks below. A continuation
// child Run's own Work Ledger entry only ever records task dispositions for
// the (small) subset of tasks it actually dispatched (the repaired ones) -
// unlike an ordinary Manifest Run, its verified proposal set legitimately
// also contains members that still live, unmutated, on the immutable source
// Run. Every such cross-Run member is accepted ONLY by proving it was part
// of the exact source candidate P5.3 already froze for this exact child -
// never merely because it points at some other Run.
ManifestApplyEligibility evaluateContinuationEligibility(
    ChangeProposalService& proposals,
    IManifestWorkLedgerRepository& ledger,
    IManifestVerificationContinuationRepository& continuations,
    const Run& run,
    const ProposedChangeSet& proposal,
    const ChangeSetId& changeSetId)
{
    if (!run.parentRunId.has_value()) {
        return deny("manifest_apply.continuation_source_missing",
            "A verification continuation Run has no recorded source Run");
    }
    const auto inputResult = continuations.findInputBySourceRun(*run.parentRunId);
    if (!inputResult.success()) {
        return deny(inputResult.error()->code, inputResult.error()->message);
    }
    if (!inputResult.value().has_value() || inputResult.value()->childRunId != run.id) {
        return deny("manifest_apply.continuation_input_missing",
            "No frozen continuation input names this exact Run");
    }
    const ManifestVerificationContinuationInput& input = *inputResult.value();

    const auto entryResult = ledger.findEntry(run.id);
    if (!entryResult.success()) {
        return deny("manifest_apply.verification_corrupt",
            "The Manifest Work Ledger entry for this Run could not be read");
    }
    if (!entryResult.value().has_value() || !entryResult.value()->verification.has_value()) {
        return deny("manifest_apply.verification_missing",
            "No authoritative combined verification exists yet for this Run");
    }
    const ManifestWorkLedgerEntry& ledgerEntry = *entryResult.value();
    const ManifestProposalSetVerification& verification = *ledgerEntry.verification;

    if (ledgerEntry.runId != run.id || ledgerEntry.projectId != run.projectId) {
        return deny("manifest_apply.verification_identity_mismatch",
            "The Work Ledger entry's own identity does not match this Run");
    }
    if (ledgerEntry.terminalStatus != RunStatus::Completed) {
        return deny("manifest_apply.run_not_completed",
            "This Run has not reached a Completed terminal status");
    }
    if (ledgerEntry.disposition != ManifestWorkLedgerDisposition::ManifestReady) {
        return deny("manifest_apply.verification_missing",
            "This Run's Work Ledger entry does not represent a completed Manifest execution");
    }
    const auto structurallyValid = validateManifestProposalSetVerification(verification);
    if (!structurallyValid.success()) {
        return deny("manifest_apply.verification_corrupt",
            "The stored combined verification record is structurally invalid");
    }
    if (verification.workspaceIdentity != run.projectId
            || verification.baselineFingerprint != ledgerEntry.baselineFingerprint) {
        return deny("manifest_apply.verification_identity_mismatch",
            "The verified workspace/baseline identity does not match this Run");
    }
    if (!requiredAcceptancePassed(verification)) {
        return deny("manifest_apply.verification_not_passed",
            "The Run's authoritative combined verification did not pass every required check");
    }
    std::unordered_set<std::string> verifiedIds;
    for (const auto& id : verification.orderedProposalIds) {
        if (!verifiedIds.insert(id).second) {
            return deny("manifest_apply.verified_set_invalid",
                "The verified proposal set contains a duplicate proposal id");
        }
    }
    bool targetInVerifiedSet = false;
    for (const auto& id : verification.orderedProposalIds) {
        if (id == changeSetId) { targetInVerifiedSet = true; break; }
    }
    if (!targetInVerifiedSet) {
        return deny("manifest_apply.proposal_not_in_verified_set",
            "This proposal was not part of the Run's verified combined proposal set");
    }
    if (proposal.state != ChangeSetState::ReadyForReview) {
        return deny("proposal.not_ready", "Change set is not in ReadyForReview state");
    }

    // Every child-run-origin member must be provably this Run's own recorded
    // final proposal - the same disposition-membership guarantee the
    // ordinary path enforces, restricted to the subset that legitimately
    // belongs to this Run at all.
    std::unordered_set<std::string> childTaskProposalIds;
    for (const auto& disposition : ledgerEntry.taskDispositions) {
        if (disposition.changeSetId.has_value()) childTaskProposalIds.insert(*disposition.changeSetId);
    }
    const std::unordered_set<std::string> sourceCandidateIds(
        input.orderedProposalIds.begin(), input.orderedProposalIds.end());

    for (std::size_t i = 0; i < verification.orderedProposalIds.size(); ++i) {
        const auto& memberId = verification.orderedProposalIds[i];
        const auto member = proposals.findChangeSet(memberId);
        if (!member.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A member of the Run's verified proposal set is no longer available");
        }
        const ProposedChangeSet& memberProposal = member.value();
        if (memberProposal.runId == run.id) {
            if (memberProposal.projectId != run.projectId
                    || (memberProposal.state != ChangeSetState::ReadyForReview
                        && memberProposal.state != ChangeSetState::Applied)
                    || !childTaskProposalIds.contains(memberId)) {
                return deny("manifest_apply.proposal_set_diverged",
                    "A member of the Run's verified proposal set is no longer eligible");
            }
        } else {
            // A referenced, never-copied source member: it must be the exact
            // source Run this child names, AND must have been part of the
            // exact candidate set P5.3 already froze - never merely some
            // other proposal that happens to point at the source Run.
            if (memberProposal.runId != input.sourceRunId
                    || memberProposal.projectId != run.projectId
                    || !sourceCandidateIds.contains(memberId)
                    || (memberProposal.state != ChangeSetState::ReadyForReview
                        && memberProposal.state != ChangeSetState::Applied)) {
                return deny("manifest_apply.proposal_set_diverged",
                    "A referenced source member is no longer part of the admitted continuation "
                    "candidate");
            }
        }
        const auto changes = proposals.listChanges(memberId);
        if (!changes.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A member of the Run's verified proposal set could not be read");
        }
        const auto recomputedHash = computeProposalContentHash(memberId, changes.value(), proposals);
        if (!recomputedHash.success()
                || recomputedHash.value() != verification.orderedProposalContentHashes[i]) {
            const bool isTarget = memberId == changeSetId;
            return deny(
                isTarget ? "manifest_apply.content_hash_mismatch"
                         : "manifest_apply.proposal_set_diverged",
                "A proposal's durable content no longer matches what was verified");
        }
    }

    return allow(ManifestApplyVerificationGate::kEligibleCode);
}

// QML-P4: the manual-handoff-workflow counterpart to
// evaluateContinuationEligibility above. Structurally the same shape - own-
// Run members proven via the attempt's own recorded task dispositions,
// cross-Run members proven via an immutable frozen identity - but anchored
// to ManifestVerificationManualHandoff/Attempt, never
// ManifestVerificationContinuationInput.
ManifestApplyEligibility evaluateManualHandoffEligibility(
    ChangeProposalService& proposals,
    const ManifestVerificationManualHandoff& handoff,
    const ManifestVerificationManualHandoffApplyAuthority& authority,
    const ProposedChangeSet& proposal,
    const ChangeSetId& changeSetId)
{
    const auto validHandoff = validateManifestVerificationManualHandoff(handoff);
    if (!validHandoff.success()) {
        return deny("manifest_apply.manual_handoff_authority_corrupt",
            "The frozen manual-handoff identity is structurally invalid");
    }
    const auto& run = authority.run;
    const auto& attempt = authority.attempt;
    const ManifestWorkLedgerEntry& ledgerEntry = authority.ledgerEntry;
    const ManifestProposalSetVerification& verification = *ledgerEntry.verification;

    if (handoff.sourceProjectId != run.projectId
            || attempt.handoffId != handoff.id
            || attempt.attemptRunId != run.id
            || attempt.verificationAnchorRunId != handoff.verificationAnchorRunId
            || run.parentRunId != std::optional<RunId>(handoff.verificationAnchorRunId)
            || ledgerEntry.runId != run.id || ledgerEntry.projectId != run.projectId) {
        return deny("manifest_apply.verification_identity_mismatch",
            "The manual-handoff attempt, Run, handoff, and Work Ledger identities disagree");
    }
    if (verification.workspaceIdentity != handoff.workspaceIdentity
            || verification.baselineFingerprint != handoff.baselineFingerprint
            || ledgerEntry.baselineFingerprint != handoff.baselineFingerprint) {
        return deny("manifest_apply.verification_identity_mismatch",
            "The verified workspace/baseline identity does not match this handoff");
    }

    std::unordered_set<std::string> attemptTaskProposalIds;
    for (const auto& disposition : ledgerEntry.taskDispositions) {
        if (disposition.changeSetId.has_value()) attemptTaskProposalIds.insert(*disposition.changeSetId);
    }
    if (verification.orderedProposalIds.size() != handoff.frozenMembers.size()
            || verification.orderedProposalContentHashes.size() != handoff.frozenMembers.size()) {
        return deny("manifest_apply.verified_set_mismatch",
            "The verified proposal set does not preserve the frozen handoff cardinality");
    }

    // Exact positional representation: frozen member i must appear at i as
    // itself or as one direct, genuine successor created by this authoritative
    // attempt. Before trusting either representation, re-bind the immutable
    // frozen member to its own durable id/owning Run/content hash/lineage root.
    // This prevents a syntactically valid verified set from laundering a
    // corrupted frozen identity or reordering/substituting lineage members.
    for (std::size_t i = 0; i < handoff.frozenMembers.size(); ++i) {
        const auto& frozen = handoff.frozenMembers[i];
        const auto frozenProposal = proposals.findChangeSet(frozen.proposalId);
        if (!frozenProposal.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A frozen manual-handoff member is no longer available");
        }
        const auto frozenChanges = proposals.listChanges(frozen.proposalId);
        const auto frozenRoot = resolveProposalLineageRoot(proposals, frozen.proposalId);
        if (!frozenChanges.success() || !frozenRoot.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A frozen manual-handoff member identity could not be reconstructed");
        }
        const auto frozenHash = computeProposalContentHash(
            frozen.proposalId, frozenChanges.value(), proposals);
        if (!frozenHash.success() || frozenProposal.value().runId != frozen.owningRunId
                || frozenProposal.value().projectId != handoff.sourceProjectId
                || frozenHash.value() != frozen.contentHash
                || frozenRoot.value() != frozen.lineageRootProposalId) {
            return deny("manifest_apply.proposal_set_diverged",
                "A frozen manual-handoff member no longer matches its recorded identity");
        }

        const auto& representedId = verification.orderedProposalIds[i];
        const auto represented = proposals.findChangeSet(representedId);
        if (!represented.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A member of the authoritative manual-handoff set is no longer available");
        }
        const bool unchanged = representedId == frozen.proposalId;
        if (represented.value().projectId != handoff.sourceProjectId
                || (represented.value().state != ChangeSetState::ReadyForReview
                    && represented.value().state != ChangeSetState::Applied)
                || (unchanged && represented.value().runId != frozen.owningRunId)
                || (!unchanged
                    && (represented.value().runId != run.id
                        || !attemptTaskProposalIds.contains(representedId)
                        || represented.value().supersedesChangeSetId
                            != std::optional<ChangeSetId>(frozen.proposalId)))) {
            return deny("manifest_apply.proposal_set_diverged",
                "A verified member is not the frozen proposal or its genuine attempt successor");
        }
        const auto representedRoot = resolveProposalLineageRoot(proposals, representedId);
        const auto representedChanges = proposals.listChanges(representedId);
        if (!representedRoot.success() || representedRoot.value() != frozen.lineageRootProposalId
                || !representedChanges.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A verified member does not preserve its frozen lineage identity");
        }
        const auto representedHash = computeProposalContentHash(
            representedId, representedChanges.value(), proposals);
        if (!representedHash.success()
                || representedHash.value() != verification.orderedProposalContentHashes[i]
                || (unchanged && representedHash.value() != frozen.contentHash)) {
            const bool isTarget = representedId == changeSetId;
            return deny(
                isTarget ? "manifest_apply.content_hash_mismatch"
                         : "manifest_apply.proposal_set_diverged",
                "A proposal's durable content no longer matches what was verified");
        }
    }

    const bool targetInVerifiedSet = std::find(verification.orderedProposalIds.begin(),
        verification.orderedProposalIds.end(), changeSetId) != verification.orderedProposalIds.end();
    if (!targetInVerifiedSet) {
        return deny("manifest_apply.proposal_not_in_verified_set",
            "This proposal is not part of the handoff's single authoritative verified set");
    }
    if (proposal.state != ChangeSetState::ReadyForReview) {
        return deny("proposal.not_ready", "Change set is not in ReadyForReview state");
    }

    return allow(ManifestApplyVerificationGate::kEligibleCode);
}

} // namespace

ManifestApplyVerificationGate::ManifestApplyVerificationGate(
    ChangeProposalService& proposals,
    IManifestWorkLedgerRepository& ledger,
    IRunRepository& runs,
    IVerificationBootstrapRepository* verificationBootstrapRepository,
    IExecutionManifestRepository* manifestRepository,
    IManifestVerificationContinuationRepository* continuationRepository,
    IManifestVerificationManualHandoffRepository* manualHandoffRepository)
    : proposals_(proposals)
    , ledger_(ledger)
    , runs_(runs)
    , verificationBootstrapRepository_(verificationBootstrapRepository)
    , manifestRepository_(manifestRepository)
    , continuationRepository_(continuationRepository)
    , manualHandoffRepository_(manualHandoffRepository)
{
}

ManifestApplyEligibility ManifestApplyVerificationGate::evaluate(
    const ChangeSetId& changeSetId) const
{
    // 1. The proposal itself must be readable and exist. findChangeSet()
    // already fails with the established "proposal.not_found" code for a
    // missing row - reused verbatim rather than wrapped in a new one.
    const auto proposalResult = proposals_.findChangeSet(changeSetId);
    if (!proposalResult.success()) {
        return deny(proposalResult.error()->code, proposalResult.error()->message);
    }
    const ProposedChangeSet& proposal = proposalResult.value();

    // 2. The owning Run must be readable and exist. A repository failure or
    // a missing Run always fails closed here - neither is ever reported as
    // kGateNotApplicableCode, which is reserved for a Run that was actually
    // found and simply belongs to a different workflow (see the class doc
    // comment's scope decision).
    const auto runResult = runs_.findRun(proposal.runId);
    if (!runResult.success()) {
        return deny(runResult.error()->code, runResult.error()->message);
    }
    if (!runResult.value().has_value()) {
        return deny("proposal.run_not_found",
            "The Run that produced this change set no longer exists");
    }
    const Run& run = *runResult.value();

    // QML-P4R.1 hardening: this reverse lookup is indexed by proposal id,
    // never by Run id, so it correctly resolves an unmodified frozen member
    // regardless of which Run currently owns it or that Run's own workflow -
    // including a continuation child's own proposal, which the old Run-id-
    // keyed lookup (gated to kExecutionManifestWorkflowId only) could never
    // reach: that Run's own workflow branch below would already have
    // returned via its own direct forward path first. findHandoffByMemberProposalId
    // only ever matches an ORIGINAL frozen member (never a manual-handoff
    // successor - successors are new proposal ids, created after freezing,
    // and are never themselves inserted as frozen members), so running this
    // check first is safe: a genuine successor always falls through
    // untouched to the ordinary workflow dispatch below.
    if (manualHandoffRepository_ != nullptr) {
        const auto handoffResult = manualHandoffRepository_->findHandoffByMemberProposalId(changeSetId);
        if (!handoffResult.success()) {
            return deny(handoffResult.error()->code, handoffResult.error()->message);
        }
        if (handoffResult.value().has_value()) {
            const auto& handoff = *handoffResult.value();
            const auto authority = resolveManualHandoffApplyAuthority(
                *manualHandoffRepository_, ledger_, runs_, handoff);
            if (!authority.success()) {
                return deny(authority.error()->code, authority.error()->message);
            }
            if (!authority.value().has_value()) {
                return deny("manifest_apply.manual_handoff_verification_missing",
                    "This handoff has no authoritative successful verification attempt");
            }
            if (proposal.projectId != authority.value()->run.projectId) {
                return deny("proposal.project_mismatch",
                    "Change set and authoritative attempt belong to different projects");
            }
            return evaluateManualHandoffEligibility(
                proposals_, handoff, *authority.value(), proposal, changeSetId);
        }
    }

    // 3. Only a genuinely found Run's own workflowId decides applicability.
    // MW-P5.4: a verification-continuation child Run gets its own, separate
    // eligibility path (see evaluateContinuationEligibility's own doc
    // comment above) - never a relaxation of the ordinary Manifest-workflow
    // checks below, and never silently reported as kGateNotApplicableCode
    // (that code means "verification does not apply here"; a continuation
    // Run's proposals are very much still gated, just via a different
    // durable proof chain).
    if (run.workflowId == kManifestVerificationContinuationWorkflowId) {
        if (continuationRepository_ == nullptr) {
            return deny("manifest_apply.continuation_gate_unavailable",
                "This Backend composition cannot evaluate verification-continuation eligibility");
        }
        if (proposal.projectId != run.projectId) {
            return deny("proposal.project_mismatch",
                "Change set and Run belong to different projects");
        }
        return evaluateContinuationEligibility(
            proposals_, ledger_, *continuationRepository_, run, proposal, changeSetId);
    }

    // QML-P4: a manual-handoff attempt Run gets its own, separate
    // eligibility path (see evaluateManualHandoffEligibility's own doc
    // comment above) - same "never kGateNotApplicableCode, never a
    // relaxation" discipline as the continuation branch above.
    if (run.workflowId == kManifestVerificationManualHandoffWorkflowId) {
        if (manualHandoffRepository_ == nullptr) {
            return deny("manifest_apply.manual_handoff_gate_unavailable",
                "This Backend composition cannot evaluate manual-handoff eligibility");
        }
        if (proposal.projectId != run.projectId) {
            return deny("proposal.project_mismatch",
                "Change set and Run belong to different projects");
        }
        const auto attemptResult = manualHandoffRepository_->findAttemptByRunId(run.id);
        if (!attemptResult.success()) {
            return deny(attemptResult.error()->code, attemptResult.error()->message);
        }
        if (!attemptResult.value().has_value()) {
            return deny("manifest_apply.manual_handoff_attempt_missing",
                "No manual-handoff attempt record names this exact Run");
        }
        const auto handoffResult = manualHandoffRepository_->findHandoffById(
            attemptResult.value()->handoffId);
        if (!handoffResult.success()) {
            return deny(handoffResult.error()->code, handoffResult.error()->message);
        }
        if (!handoffResult.value().has_value()) {
            return deny("manifest_apply.manual_handoff_missing",
                "No frozen manual handoff owns this attempt Run");
        }
        const auto authority = resolveManualHandoffApplyAuthority(
            *manualHandoffRepository_, ledger_, runs_, *handoffResult.value());
        if (!authority.success()) {
            return deny(authority.error()->code, authority.error()->message);
        }
        if (!authority.value().has_value()) {
            return deny("manifest_apply.manual_handoff_verification_missing",
                "This handoff has no authoritative successful verification attempt");
        }
        if (authority.value()->attempt.id != attemptResult.value()->id) {
            return deny("manifest_apply.manual_handoff_attempt_superseded",
                "A newer successful attempt is now the sole Apply authority for this handoff");
        }
        return evaluateManualHandoffEligibility(
            proposals_, *handoffResult.value(), *authority.value(), proposal, changeSetId);
    }

    // MW-P5.4: this proposal's OWN Run may be an ordinary Manifest Run that
    // itself failed and was admitted for continuation - in which case this
    // exact proposal can ALSO be an unchanged, referenced member of some
    // continuation child's already-verified final set, even though it was
    // never copied there. Resolving strictly via proposal.runId above would
    // otherwise always fall through to the checks below, which read only
    // THIS (failed) Run's own Work Ledger entry and can never pass for an
    // admitted continuation's source (its own verification failed by
    // definition - that is the entire premise of admission). Checked before
    // the ordinary path, never after: an admitted continuation source Run
    // can never coincidentally also pass its own ordinary check.
    if (run.workflowId == kExecutionManifestWorkflowId && continuationRepository_ != nullptr) {
        const auto inputResult = continuationRepository_->findInputBySourceRun(run.id);
        if (!inputResult.success()) {
            return deny(inputResult.error()->code, inputResult.error()->message);
        }
        if (inputResult.value().has_value()) {
            const auto& input = *inputResult.value();
            const bool referenced = std::find(input.orderedProposalIds.begin(),
                input.orderedProposalIds.end(), changeSetId) != input.orderedProposalIds.end();
            if (referenced) {
                const auto childRunResult = runs_.findRun(input.childRunId);
                if (!childRunResult.success()) {
                    return deny(childRunResult.error()->code, childRunResult.error()->message);
                }
                if (!childRunResult.value().has_value()) {
                    return deny("manifest_apply.continuation_child_missing",
                        "The continuation child Run for this referenced source proposal no "
                        "longer exists");
                }
                if (proposal.projectId != childRunResult.value()->projectId) {
                    return deny("proposal.project_mismatch",
                        "Change set and Run belong to different projects");
                }
                return evaluateContinuationEligibility(
                    proposals_, ledger_, *continuationRepository_, *childRunResult.value(),
                    proposal, changeSetId);
            }
        }
    }

    if (run.workflowId != kExecutionManifestWorkflowId) {
        return allow(kGateNotApplicableCode);
    }

    // AG-16.1 hardening: the proposal's own project identity must match its
    // owning Run's - the same check ChangeSetApplyService::apply() already
    // performs independently (proposal.project_mismatch), reused verbatim
    // here so the gate's own decision never authorizes a proposal whose
    // project identity is inconsistent with the Run it claims to belong to.
    if (proposal.projectId != run.projectId) {
        return deny("proposal.project_mismatch",
            "Change set and Run belong to different projects");
    }

    // 4. From here on this is a Manifest-workflow Run: every AG-16
    // requirement is strict. The authoritative durable source is the Work
    // Ledger entry's typed verification identity - never Markdown, UI
    // state, or a model-authored summary.
    const auto entryResult = ledger_.findEntry(run.id);
    if (!entryResult.success()) {
        return deny("manifest_apply.verification_corrupt",
            "The Manifest Work Ledger entry for this Run could not be read");
    }
    if (!entryResult.value().has_value() || !entryResult.value()->verification.has_value()) {
        return deny("manifest_apply.verification_missing",
            "No authoritative combined verification exists yet for this Run");
    }
    const ManifestWorkLedgerEntry& ledgerEntry = *entryResult.value();
    const ManifestProposalSetVerification& verification = *ledgerEntry.verification;

    // AG-16.1 hardening: the ledger entry itself must be bound to the exact
    // Run being evaluated - defence in depth against a corrupted or
    // mis-keyed row (findEntry() is already keyed by run.id, so this can
    // only ever fire on a genuinely inconsistent stored row, never on a
    // legitimate cross-Run lookup, since there is no such thing here).
    if (ledgerEntry.runId != run.id || ledgerEntry.projectId != run.projectId) {
        return deny("manifest_apply.verification_identity_mismatch",
            "The Work Ledger entry's own identity does not match this Run");
    }

    // AG-16.1 hardening: only a Run that actually reached a Completed
    // terminal status can ever have an ordinary-Apply-eligible verification
    // - a Failed/Cancelled/Interrupted Run's verification (if one exists at
    // all) never authorized anything, since the Run itself did not finish
    // safely.
    if (ledgerEntry.terminalStatus != RunStatus::Completed) {
        return deny("manifest_apply.run_not_completed",
            "This Run has not reached a Completed terminal status");
    }
    // AG-16.1 hardening: ManifestReady is the only disposition a genuinely
    // manifest-executed, verification-bearing Run can have (see
    // ManifestWorkLedgerService::recordTerminalRun's own invariant, which
    // refuses to write a Completed+ManifestReady entry without a
    // verification in the first place) - checked independently here rather
    // than only implied by verification.has_value() above.
    if (ledgerEntry.disposition != ManifestWorkLedgerDisposition::ManifestReady) {
        return deny("manifest_apply.verification_missing",
            "This Run's Work Ledger entry does not represent a completed Manifest execution");
    }

    const auto structurallyValid = validateManifestProposalSetVerification(verification);
    if (!structurallyValid.success()) {
        return deny("manifest_apply.verification_corrupt",
            "The stored combined verification record is structurally invalid");
    }

    // AG-16.1 hardening: the verification's own recorded workspace/baseline
    // identity must match this exact Run - never a verification that
    // happens to have passed for some other workspace or baseline.
    // workspaceIdentity is always the owning project's id (see
    // ExecutionManifestExecutor.cpp, which sets
    // BuildManifestProposalOverlayRequest::workspaceIdentity =
    // request.projectId for both the dependency and final combined
    // overlay); baselineFingerprint is the same captured
    // WorkspaceBaselineSnapshot::fingerprint independently recovered by
    // ManifestWorkLedgerService::recordTerminalRun via the persisted
    // workspace-baseline.v1.json artifact.
    if (verification.workspaceIdentity != run.projectId
            || verification.baselineFingerprint != ledgerEntry.baselineFingerprint) {
        return deny("manifest_apply.verification_identity_mismatch",
            "The verified workspace/baseline identity does not match this Run");
    }

    if (!requiredAcceptancePassed(verification)) {
        return deny("manifest_apply.verification_not_passed",
            "The Run's authoritative combined verification did not pass every required check");
    }

    // AG-16.1 hardening: the verified set's own cardinality/membership must
    // be internally duplicate-free, and must exactly match (no extra proposal,
    // none missing) the changeSetIds this Run's own Work Ledger task
    // dispositions recorded as each task's final proposal - a second,
    // independently-recorded source of truth for what a Completed Manifest
    // Run's real proposal set was (ManifestWorkLedgerService::recordTerminalRun
    // reads the same live per-task execution state the executor pass that
    // built orderedProposalIds itself already produced - see
    // ExecutionManifestExecutor.cpp's own combined-verification block).
    std::unordered_set<std::string> verifiedIds;
    for (const auto& id : verification.orderedProposalIds) {
        if (!verifiedIds.insert(id).second) {
            return deny("manifest_apply.verified_set_invalid",
                "The verified proposal set contains a duplicate proposal id");
        }
    }
    std::unordered_set<std::string> finalTaskProposalIds;
    for (const auto& disposition : ledgerEntry.taskDispositions) {
        if (disposition.changeSetId.has_value()) {
            finalTaskProposalIds.insert(*disposition.changeSetId);
        }
    }
    if (verifiedIds != finalTaskProposalIds) {
        return deny("manifest_apply.verified_set_mismatch",
            "The verified proposal set does not exactly match this Run's recorded final proposals");
    }

    // The target proposal must be a member of the exact verified set.
    bool targetInVerifiedSet = false;
    for (const auto& id : verification.orderedProposalIds) {
        if (id == changeSetId) {
            targetInVerifiedSet = true;
            break;
        }
    }
    if (!targetInVerifiedSet) {
        return deny("manifest_apply.proposal_not_in_verified_set",
            "This proposal was not part of the Run's verified combined proposal set");
    }

    // AG-16.1 hardening: the TARGET proposal specifically must currently be
    // ReadyForReview - Applied is only ever tolerated for OTHER members of
    // the same verified set (the per-member loop below), a normal
    // one-at-a-time apply sequence over an already-verified combined set,
    // never for the one proposal this evaluate() call is actually being
    // asked to authorize.
    if (proposal.state != ChangeSetState::ReadyForReview) {
        return deny("proposal.not_ready", "Change set is not in ReadyForReview state");
    }

    // The durable proposal set must not have diverged from what was
    // verified: every member of the verified set (not only the one being
    // applied) must still resolve to the same Run and project, remain in a
    // state consistent with having been verified together (ReadyForReview,
    // or already Applied - the target's own stricter ReadyForReview-only
    // requirement was already enforced above), and carry byte-identical
    // durable content to what was actually verified. A repaired, rejected,
    // superseded, or foreign proposal is never a member of this set in the
    // first place (ManifestProposalOverlay::build() only ever admits
    // ReadyForReview proposals into orderedProposalIds), so no separate
    // lineage check is needed beyond this membership/consistency loop.
    for (std::size_t i = 0; i < verification.orderedProposalIds.size(); ++i) {
        const auto& memberId = verification.orderedProposalIds[i];
        const auto member = proposals_.findChangeSet(memberId);
        if (!member.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A member of the Run's verified proposal set is no longer available");
        }
        const ProposedChangeSet& memberProposal = member.value();
        if (memberProposal.runId != run.id || memberProposal.projectId != run.projectId
                || (memberProposal.state != ChangeSetState::ReadyForReview
                    && memberProposal.state != ChangeSetState::Applied)) {
            return deny("manifest_apply.proposal_set_diverged",
                "A member of the Run's verified proposal set is no longer eligible");
        }
        const auto changes = proposals_.listChanges(memberId);
        if (!changes.success()) {
            return deny("manifest_apply.proposal_set_diverged",
                "A member of the Run's verified proposal set could not be read");
        }
        const auto recomputedHash =
            computeProposalContentHash(memberId, changes.value(), proposals_);
        if (!recomputedHash.success()
                || recomputedHash.value() != verification.orderedProposalContentHashes[i]) {
            const bool isTarget = memberId == changeSetId;
            return deny(
                isTarget ? "manifest_apply.content_hash_mismatch"
                         : "manifest_apply.proposal_set_diverged",
                "A proposal's durable content no longer matches what was verified");
        }
    }

    // AG-18.3 hardening (blocker 1): the narrow bootstrap-lifecycle
    // prerequisite. This does NOT re-derive AG-16's verification algorithm
    // above - it only additionally requires, for a Run whose Manifest
    // declared a same-Run bootstrap, that the bootstrap lifecycle itself
    // durably reached BootstrapVerified for THIS exact candidate identity
    // before the already-passed AG-16 verification above is allowed to
    // authorize Apply. Bootstrap applicability is derived from the frozen
    // Manifest, never inferred from whether a decision row happens to exist:
    // a missing row for a declared bootstrap therefore fails closed, while
    // ordinary non-bootstrap Runs retain current AG-16 behavior exactly.
    if (verificationBootstrapRepository_ != nullptr && manifestRepository_ != nullptr) {
        const auto manifest = manifestRepository_->findForRun(run.id);
        if (!manifest.success() || !manifest.value().has_value()) {
            return deny("manifest_apply.bootstrap_lifecycle_corrupt",
                "The frozen Manifest needed to verify this Run's bootstrap lifecycle could not "
                "be read");
        }
        const bool bootstrapDeclared = manifest.value()->bootstrapDeclaration.has_value();
        const auto bootstrapDecision = verificationBootstrapRepository_->findDecisionForRun(run.id);
        if (!bootstrapDecision.success()) {
            return deny("manifest_apply.bootstrap_lifecycle_corrupt",
                "The bootstrap lifecycle decision for this Run could not be read");
        }
        if (bootstrapDeclared && !bootstrapDecision.value().has_value()) {
            return deny("manifest_apply.bootstrap_not_verified",
                "This Run declared a verification bootstrap but has no durable lifecycle "
                "decision");
        }
        if (!bootstrapDeclared && bootstrapDecision.value().has_value()) {
            return deny("manifest_apply.bootstrap_lifecycle_corrupt",
                "This Run has a bootstrap lifecycle decision that does not match its frozen "
                "Manifest");
        }
        if (bootstrapDecision.value().has_value()) {
            const VerificationBootstrapDecision& decision = *bootstrapDecision.value();
            // ActionRequired never carries a candidate identity to verify
            // against - such a Run never reaches Completed/ManifestReady in
            // the first place (see ManifestWorkflowRunner), so this branch
            // is defence in depth against a corrupted/mis-keyed row.
            if (decision.runId != run.id || decision.projectId != run.projectId
                || decision.baselineFingerprint != ledgerEntry.baselineFingerprint) {
                return deny("manifest_apply.bootstrap_lifecycle_corrupt",
                    "This Run's bootstrap lifecycle identity does not match its durable Run "
                    "identity");
            }
            if (decision.disposition != VerificationBootstrapDisposition::BootstrapPending) {
                return deny("manifest_apply.bootstrap_not_verified",
                    "This Run's bootstrap lifecycle did not reach a verifiable disposition");
            }
            const auto transitions = verificationBootstrapRepository_->listTransitionsForRun(run.id);
            if (!transitions.success()) {
                return deny("manifest_apply.bootstrap_lifecycle_corrupt",
                    "This Run's bootstrap lifecycle transitions could not be read");
            }
            if (transitions.value().size() != 1) {
                return deny("manifest_apply.bootstrap_lifecycle_corrupt",
                    "This Run's bootstrap lifecycle does not contain exactly one terminal "
                    "transition");
            }
            const auto& verified = transitions.value().front();
            if (verified.runId != run.id
                    || verified.toDisposition
                        != VerificationBootstrapDisposition::BootstrapVerified
                    || !verified.candidateIdentity.has_value()
                    || verified.candidateIdentity->overlayFingerprint
                        != verification.overlayFingerprint) {
                return deny("manifest_apply.bootstrap_not_verified",
                    "This Run's bootstrap candidate was never verified against the "
                    "authoritative proposal-overlay identity being applied");
            }
        }
    }

    return allow(kEligibleCode);
}

} // namespace ai::application
