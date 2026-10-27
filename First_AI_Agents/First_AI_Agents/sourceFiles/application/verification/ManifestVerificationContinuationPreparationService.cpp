#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"

#include "ai/application/planning/IOutcomeBriefTrigger.hpp"

#include "ai/application/IdGenerator.hpp"
#include "ai/application/verification/ManifestProposalOverlay.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <sstream>

namespace ai::application {
namespace {

template<class T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

struct OverlayCleanup {
    std::optional<ManifestProposalOverlayDescriptor> baseline;
    std::optional<ManifestProposalOverlayDescriptor> candidate;
    ~OverlayCleanup()
    {
        if (candidate) static_cast<void>(ManifestProposalOverlay::cleanup(*candidate));
        if (baseline) static_cast<void>(ManifestProposalOverlay::cleanup(*baseline));
    }
};

// Deliberately does not compare child.status: the reserved child Run is
// expected to progress past Prepared (Running, then a terminal status) once
// it actually executes, and reconcileMissingChildren() re-checks every
// continuation input's child on every BackendRuntime startup - requiring
// Prepared here made recovery fail for any continuation whose child had
// already finished, which is the normal end state, not a conflict.
bool matchesChild(const Run& child, const ManifestVerificationContinuationInput& input)
{
    return child.id == input.childRunId && child.projectId == input.sourceProjectId
        && child.parentRunId == std::optional<RunId>(input.sourceRunId)
        && child.workflowId == kManifestVerificationContinuationWorkflowId;
}

} // namespace

ManifestVerificationContinuationPreparationService::
ManifestVerificationContinuationPreparationService(
    IRunRepository& runs, IProjectRepository& projects,
    IExecutionManifestRepository& manifests, IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationContinuationRepository& continuations,
    ManifestVerificationContinuationService& admission,
    ManifestVerificationAttributionService& attribution,
    ChangeProposalService& proposals,
    IManifestVerificationCommandRunner& commandRunner,
    ManifestVerificationLegacyBaselineNormalizationService& baselineNormalization,
    std::filesystem::path trustedScratchRoot)
    : runs_(runs), projects_(projects), manifests_(manifests), workLedger_(workLedger),
      continuations_(continuations), admission_(admission), attribution_(attribution),
      proposals_(proposals), commandRunner_(commandRunner),
      baselineNormalization_(baselineNormalization),
      trustedScratchRoot_(trustedScratchRoot.empty()
          ? std::filesystem::temp_directory_path() / "aio-v"
          : std::move(trustedScratchRoot))
{
}

common::Result<Run> ManifestVerificationContinuationPreparationService::ensurePreparedChild(
    const ManifestVerificationContinuationInput& input, const Run& sourceRun)
{
    const auto existing = runs_.findRun(input.childRunId);
    if (!existing.success()) return common::Result<Run>::failure(*existing.error());
    if (existing.value().has_value()) {
        if (!matchesChild(*existing.value(), input)) return fail<Run>(
            "verification_continuation.child_identity_conflict",
            "The reserved continuation child Run has a conflicting identity");
        return common::Result<Run>::ok(*existing.value());
    }

    Run child = sourceRun;
    child.id = input.childRunId;
    child.parentRunId = input.sourceRunId;
    child.title = sourceRun.title + " - verification continuation";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.status = RunStatus::Prepared;
    child.progress = 0.0;
    child.currentStageId.reset();
    child.errorMessage.clear();
    child.usage = {};
    child.providerBudgetUsage = {};
    child.createdAt = input.createdAt;
    child.startedAt.reset();
    child.finishedAt.reset();
    child.revision = 0;

    RunStage stage;
    stage.id = input.childRunId + ":continuation";
    stage.runId = input.childRunId;
    stage.type = "manifest-verification-continuation";
    stage.title = "Repair failed verification";
    stage.description = "Backend-prepared verification continuation; Planner was skipped";
    stage.status = StageStatus::Pending;

    auto inserted = runs_.insertRunWithStagesAndCopiedTaskLinks(
        child, {stage}, input.sourceRunId);
    if (!inserted.success() && inserted.error()->code == "run.duplicate_id") {
        const auto raced = runs_.findRun(input.childRunId);
        if (raced.success() && raced.value().has_value() && matchesChild(*raced.value(), input))
            return common::Result<Run>::ok(*raced.value());
    }
    return inserted;
}

common::Result<ManifestVerificationContinuationPreparationResult>
ManifestVerificationContinuationPreparationService::prepare(
    const RunId& sourceRunId, std::stop_token stopToken)
{
    const auto source = runs_.findRun(sourceRunId);
    if (!source.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        source.error()->code, source.error()->message);
    if (!source.value().has_value()) return fail<ManifestVerificationContinuationPreparationResult>(
        "verification_continuation.source_missing", "The source Run does not exist");

    const auto existingInput = continuations_.findInputBySourceRun(sourceRunId);
    if (!existingInput.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        existingInput.error()->code, existingInput.error()->message);
    if (existingInput.value().has_value()) {
        const auto valid = validateManifestVerificationContinuationInput(*existingInput.value());
        const auto existingAttribution = continuations_.findAttributionBySourceRun(sourceRunId);
        if (!valid.success() || !existingAttribution.success()
            || !existingAttribution.value().has_value()
            || existingAttribution.value()->status != ManifestVerificationAttributionStatus::Attributed
            || existingAttribution.value()->id != existingInput.value()->attributionId) {
            return fail<ManifestVerificationContinuationPreparationResult>(
                "verification_continuation.input_conflict",
                "The durable continuation input does not match its attribution");
        }
        auto child = ensurePreparedChild(*existingInput.value(), *source.value());
        if (!child.success()) return fail<ManifestVerificationContinuationPreparationResult>(
            child.error()->code, child.error()->message);
        return common::Result<ManifestVerificationContinuationPreparationResult>::ok(
            {child.value(), *existingInput.value(), *existingAttribution.value()});
    }

    const auto admitted = admission_.admitOrEvaluate(sourceRunId);
    if (!admitted.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        admitted.error()->code, admitted.error()->message);
    if (admitted.value().status != ManifestVerificationContinuationStatus::Admitted)
        return fail<ManifestVerificationContinuationPreparationResult>(
            admitted.value().denialCode, admitted.value().denialMessage);

    const auto manifest = manifests_.findForRun(sourceRunId);
    const auto ledger = workLedger_.findEntry(sourceRunId);
    if (!manifest.success() || !ledger.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        !manifest.success() ? manifest.error()->code : ledger.error()->code,
        !manifest.success() ? manifest.error()->message : ledger.error()->message);
    if (!manifest.value().has_value() || !ledger.value().has_value()
        || !ledger.value()->verification.has_value())
        return fail<ManifestVerificationContinuationPreparationResult>(
            "verification_continuation.source_corrupt",
            "The source Manifest or verification identity is missing");
    const auto& durable = *ledger.value()->verification;

    const auto project = projects_.findById(source.value()->projectId);
    if (!project.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        project.error()->code, project.error()->message);
    if (!project.value().has_value()) return fail<ManifestVerificationContinuationPreparationResult>(
        "verification_continuation.source_project_missing", "The source project does not exist");
    const auto workspaceRoot = source.value()->workspacePath.value_or(project.value()->rootPath);
    // The validated root of the temporary verification copies: a root that is not usable is refused here, before the workspace is
    // captured, copied or run.
    const auto scratchRoot = resolveVerificationScratchRoot(scratchProvider_, trustedScratchRoot_, workspaceRoot);
    if (!scratchRoot.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        scratchRoot.error()->code, scratchRoot.error()->message);
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto currentBaseline = WorkspaceBaselineProvenance::capture(policy);
    if (!currentBaseline.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        currentBaseline.error()->code, currentBaseline.error()->message);
    std::optional<ManifestVerificationBaselineNormalization> baselineNormalization;
    if (currentBaseline.value().fingerprint != durable.baselineFingerprint) {
        // MW-P5.6D: before failing outright, try to prove the drift is
        // limited to a Backend-owned, pre-approved delta (e.g. Qt Creator's
        // own IDE housekeeping directory) that no source proposal touches -
        // see ManifestVerificationLegacyBaselineNormalizationService's own
        // doc comment for the exact eligibility rules. std::nullopt means
        // "not eligible," not an error: fall through to the ordinary
        // baseline_drift rejection exactly as before this package.
        const auto normalized = baselineNormalization_.normalizeOrEvaluate(
            sourceRunId, durable.baselineFingerprint,
            currentBaseline.value(), durable.orderedProposalIds);
        if (!normalized.success()) return fail<ManifestVerificationContinuationPreparationResult>(
            normalized.error()->code, normalized.error()->message);
        if (!normalized.value().has_value()) {
            return fail<ManifestVerificationContinuationPreparationResult>(
                "verification_continuation.baseline_drift",
                "The workspace no longer matches the source Run baseline; implicit rebase is forbidden");
        }
        baselineNormalization = normalized.value();
    }

    // Recheck source membership before materialization. The overlay performs
    // the ordinary ReadyForReview/path/precondition/content checks again.
    for (const auto& id : durable.orderedProposalIds) {
        const auto proposal = proposals_.findChangeSet(id);
        if (!proposal.success() || proposal.value().runId != sourceRunId
            || proposal.value().projectId != source.value()->projectId
            || proposal.value().state != ChangeSetState::ReadyForReview) {
            return fail<ManifestVerificationContinuationPreparationResult>(
                "verification_continuation.source_proposal_unavailable",
                "An exact source proposal is missing, changed, or no longer reviewable");
        }
    }

    OverlayCleanup cleanup;
    BuildManifestProposalOverlayRequest baselineRequest;
    baselineRequest.workspaceRoot = workspaceRoot;
    baselineRequest.trustedScratchRoot = scratchRoot.value();
    baselineRequest.workspaceIdentity = durable.workspaceIdentity;
    baselineRequest.baselinePolicy = policy;
    baselineRequest.baselineSnapshot = currentBaseline.value();
    auto baselineOverlay = ManifestProposalOverlay::build(baselineRequest, proposals_);
    if (!baselineOverlay.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        baselineOverlay.error()->code, baselineOverlay.error()->message);
    cleanup.baseline = baselineOverlay.value();

    auto candidateRequest = baselineRequest;
    candidateRequest.orderedProposalIds = durable.orderedProposalIds;
    auto candidateOverlay = ManifestProposalOverlay::build(candidateRequest, proposals_);
    if (!candidateOverlay.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        candidateOverlay.error()->code, candidateOverlay.error()->message);
    cleanup.candidate = candidateOverlay.value();
    if (candidateOverlay.value().orderedProposalContentHashes
            != durable.orderedProposalContentHashes) {
        return fail<ManifestVerificationContinuationPreparationResult>(
            "verification_continuation.source_candidate_mismatch",
            "The reconstructed source proposal set does not match its durable verification identity");
    }
    // MW-P5.6D: overlayFingerprint mixes baseline file content into its
    // hash (unlike orderedProposalContentHashes above, which AG-16.1 made
    // baseline-independent by construction), so it legitimately changes
    // once a proven-narrow baseline delta is excluded from the live
    // baseline - that alone is not evidence of tampering.
    // ManifestProposalOverlay::build() already independently re-verified
    // every Create/Modify/Delete/Rename precondition against the live
    // workspace while materializing this very overlay, and the proposal
    // identity check above already passed - so skipping only this one
    // comparison, only once a durable normalization proof already exists
    // for this exact drift, stays sound.
    if (!baselineNormalization.has_value()
        && candidateOverlay.value().overlayFingerprint != durable.overlayFingerprint) {
        return fail<ManifestVerificationContinuationPreparationResult>(
            "verification_continuation.source_candidate_mismatch",
            "The reconstructed source proposal set does not match its durable verification identity");
    }

    const auto baselineVerification = ManifestProposalSetVerifier::verify(
        {baselineOverlay.value(), manifest.value()->tasks}, proposals_, commandRunner_, stopToken);
    if (!baselineVerification.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        baselineVerification.error()->code, baselineVerification.error()->message);
    ManifestProposalSetVerifierRequest candidateVerifierRequest {
        candidateOverlay.value(), manifest.value()->tasks};
    if (usesBaselineAwareQmlLintPolicy(durable)) {
        candidateVerifierRequest.baselineOverlay = &baselineOverlay.value();
        candidateVerifierRequest.baselineVerification = &baselineVerification.value();
    }
    const auto candidateVerification = ManifestProposalSetVerifier::verify(
        candidateVerifierRequest, proposals_, commandRunner_, stopToken);
    if (!candidateVerification.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        candidateVerification.error()->code, candidateVerification.error()->message);

    // A verification that was refused for the path budget is not a verdict on the code: say so instead of letting attribution
    // report an unmapped diagnostic.
    if (verificationRefusedForPathBudget(baselineVerification.value())
        || verificationRefusedForPathBudget(candidateVerification.value())) {
        return fail<ManifestVerificationContinuationPreparationResult>(
            "verification_continuation.verification_path_too_long",
            "Verification could not run: the temporary verification copy of the workspace would exceed the Windows path "
            "budget of the build tools. Use a shorter temporary-directory location (TEMP/TMP) or a shorter project location.");
    }
    const auto attributed = attribution_.attributeOrEvaluate(
        sourceRunId, baselineVerification.value(), candidateVerification.value());
    if (!attributed.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        attributed.error()->code, attributed.error()->message);
    if (attributed.value().status != ManifestVerificationAttributionStatus::Attributed)
        return fail<ManifestVerificationContinuationPreparationResult>(
            attributed.value().blockCode, attributed.value().blockMessage);

    ManifestVerificationContinuationInput input;
    input.sourceRunId = sourceRunId;
    input.childRunId = generateUuid();
    input.sourceProjectId = source.value()->projectId;
    input.attributionId = attributed.value().id;
    // MW-P5.6D: pinned to the workspace state this continuation was actually
    // verified against - always exactly durable.baselineFingerprint/
    // overlayFingerprint in the ordinary case (the strict checks above just
    // enforced that equality), and legitimately the current/live value when
    // a normalization proof narrowed the accepted drift.
    // ManifestVerificationContinuationRepairService re-derives and compares
    // against exactly these frozen values later, so this is what "the
    // workspace this continuation is pinned to" must mean.
    input.baselineFingerprint = currentBaseline.value().fingerprint;
    input.sourceWorkspaceIdentity = durable.workspaceIdentity;
    input.sourceOverlayFingerprint = candidateOverlay.value().overlayFingerprint;
    input.orderedProposalIds = durable.orderedProposalIds;
    input.orderedProposalContentHashes = durable.orderedProposalContentHashes;
    input.createdAt = std::chrono::system_clock::now();
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    const auto inserted = continuations_.insertInput(input);
    if (!inserted.success()) {
        if (inserted.error()->code == "manifest_verification_continuation.input_already_exists")
            return prepare(sourceRunId, stopToken);
        return fail<ManifestVerificationContinuationPreparationResult>(
            inserted.error()->code, inserted.error()->message);
    }
    auto child = ensurePreparedChild(input, *source.value());
    if (!child.success()) return fail<ManifestVerificationContinuationPreparationResult>(
        child.error()->code, child.error()->message);
    // PC-4C: the Failed source now has a typed continuation child, so its Outcome Brief becomes superseded_by_continuation (best effort).
    if (outcomeBriefTrigger_ != nullptr) {
        outcomeBriefTrigger_->runMayHaveChanged(sourceRunId);
    }
    return common::Result<ManifestVerificationContinuationPreparationResult>::ok(
        {child.value(), std::move(input), attributed.value()});
}

common::Result<ManifestVerificationContinuationRecoveryResult>
ManifestVerificationContinuationPreparationService::reconcileMissingChildren()
{
    const auto inputs = continuations_.listInputs();
    if (!inputs.success()) return common::Result<ManifestVerificationContinuationRecoveryResult>::failure(*inputs.error());
    ManifestVerificationContinuationRecoveryResult result;
    for (const auto& input : inputs.value()) {
        const auto child = runs_.findRun(input.childRunId);
        if (!child.success()) return common::Result<ManifestVerificationContinuationRecoveryResult>::failure(*child.error());
        if (child.value().has_value()) {
            if (!matchesChild(*child.value(), input)) return common::Result<ManifestVerificationContinuationRecoveryResult>::failure({
                "verification_continuation.child_identity_conflict",
                "A reserved continuation child has a conflicting identity"});
            continue;
        }
        const auto source = runs_.findRun(input.sourceRunId);
        if (!source.success() || !source.value().has_value()) return common::Result<ManifestVerificationContinuationRecoveryResult>::failure({
            "verification_continuation.recovery_source_missing",
            "A continuation input references a missing source Run"});
        auto recreated = ensurePreparedChild(input, *source.value());
        if (!recreated.success()) return common::Result<ManifestVerificationContinuationRecoveryResult>::failure(*recreated.error());
        result.recreatedChildRunIds.push_back(input.childRunId);
    }
    return common::Result<ManifestVerificationContinuationRecoveryResult>::ok(std::move(result));
}

} // namespace ai::application
