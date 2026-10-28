#include "ai/application/verification/ManifestVerificationContinuationReadModelService.hpp"

#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"

#include <algorithm>
#include <unordered_map>

namespace ai::application {
namespace {

template<class T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

bool isOwnerFinding(const ManifestVerificationFindingAttribution& finding)
{
    return finding.kind == ManifestVerificationFindingAttributionKind::SourceProposalOwner
        || finding.kind == ManifestVerificationFindingAttributionKind::IntegrationOwner;
}

} // namespace

ManifestVerificationContinuationReadModelService::
ManifestVerificationContinuationReadModelService(
    IRunRepository& runs, IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationContinuationRepository& continuations,
    IManifestVerificationBaselineNormalizationRepository& baselineNormalizations,
    ManifestVerificationContinuationService& admission)
    : runs_(runs), workLedger_(workLedger), continuations_(continuations),
      baselineNormalizations_(baselineNormalizations), admission_(admission)
{
}

common::Result<std::optional<ManifestVerificationContinuationView>>
ManifestVerificationContinuationReadModelService::viewForRun(
    const RunId& selectedRunId) const
{
    const auto selectedResult = runs_.findRun(selectedRunId);
    if (!selectedResult.success()) return fail<std::optional<ManifestVerificationContinuationView>>(
        selectedResult.error()->code, selectedResult.error()->message);
    if (!selectedResult.value().has_value())
        return common::Result<std::optional<ManifestVerificationContinuationView>>::ok(std::nullopt);
    const Run& selected = *selectedResult.value();

    RunId sourceRunId;
    bool selectedIsChild = false;
    if (selected.workflowId == kManifestVerificationContinuationWorkflowId
        && selected.parentRunId.has_value()) {
        sourceRunId = *selected.parentRunId;
        selectedIsChild = true;
    } else if (selected.workflowId == kExecutionManifestWorkflowId
        && selected.status == RunStatus::Failed) {
        sourceRunId = selected.id;
    } else {
        return common::Result<std::optional<ManifestVerificationContinuationView>>::ok(std::nullopt);
    }

    const auto sourceResult = runs_.findRun(sourceRunId);
    if (!sourceResult.success() || !sourceResult.value().has_value()) {
        return fail<std::optional<ManifestVerificationContinuationView>>(
            "verification_continuation.read_source_missing",
            "The verification-continuation source Run is unavailable");
    }
    const Run& source = *sourceResult.value();
    const auto eligibility = admission_.evaluate(sourceRunId);
    if (!eligibility.success()) return fail<std::optional<ManifestVerificationContinuationView>>(
        eligibility.error()->code, eligibility.error()->message);
    const auto durableCheckpoint = continuations_.findBySourceRun(sourceRunId);
    const auto attribution = continuations_.findAttributionBySourceRun(sourceRunId);
    const auto input = continuations_.findInputBySourceRun(sourceRunId);
    const auto normalization = baselineNormalizations_.findBySourceRun(sourceRunId);
    if (!durableCheckpoint.success() || !attribution.success() || !input.success()
        || !normalization.success()) {
        const auto& readError = !durableCheckpoint.success() ? *durableCheckpoint.error()
            : !attribution.success() ? *attribution.error()
            : !input.success() ? *input.error() : *normalization.error();
        return fail<std::optional<ManifestVerificationContinuationView>>(
            readError.code, readError.message);
    }
    if (normalization.value().has_value()) {
        const auto valid = validateManifestVerificationBaselineNormalization(
            *normalization.value());
        if (!valid.success() || normalization.value()->sourceRunId != sourceRunId) {
            return fail<std::optional<ManifestVerificationContinuationView>>(
                "verification_continuation.read_normalization_corrupt",
                "The durable baseline-normalization proof does not match its source Run");
        }
    }
    if (selectedIsChild && !input.value().has_value()) {
        return fail<std::optional<ManifestVerificationContinuationView>>(
            "verification_continuation.read_input_corrupt",
            "The selected continuation child has no durable continuation input");
    }

    ManifestVerificationContinuationView view;
    view.selectedRunId = selectedRunId;
    view.sourceRunId = sourceRunId;
    view.selectedRole = selectedIsChild ? "child" : "source";
    view.sourceStatus = toString(source.status);
    view.hasDurableCheckpoint = durableCheckpoint.value().has_value();
    view.checkpointStatus = toString(eligibility.value().status);
    view.canContinue = !selectedIsChild
        && eligibility.value().status == ManifestVerificationContinuationStatus::Admitted;
    view.blockCode = eligibility.value().denialCode;
    view.blockMessage = eligibility.value().denialMessage;

    std::optional<Run> child;
    if (input.value().has_value()) {
        const auto valid = validateManifestVerificationContinuationInput(*input.value());
        if (!valid.success() || input.value()->sourceRunId != sourceRunId
            || input.value()->sourceProjectId != source.projectId) {
            return fail<std::optional<ManifestVerificationContinuationView>>(
                "verification_continuation.read_input_corrupt",
                "The durable continuation input does not match its source Run");
        }
        view.childRunId = input.value()->childRunId;
        view.plannerSkipped = input.value()->plannerRequests == 0;
        const auto childResult = runs_.findRun(view.childRunId);
        if (!childResult.success()) return fail<std::optional<ManifestVerificationContinuationView>>(
            childResult.error()->code, childResult.error()->message);
        child = childResult.value();
        if (child.has_value()) {
            if (child->parentRunId != std::optional<RunId>(sourceRunId)
                || child->workflowId != kManifestVerificationContinuationWorkflowId
                || child->projectId != source.projectId) {
                return fail<std::optional<ManifestVerificationContinuationView>>(
                    "verification_continuation.read_child_identity_conflict",
                    "The continuation child Run has a conflicting durable identity");
            }
            view.childStatus = toString(child->status);
            if (child->status != RunStatus::Prepared) {
                view.canContinue = false;
                view.blockCode = "verification_continuation.child_already_terminal_or_active";
                view.blockMessage = "The linked continuation child has already started or finished";
            }
        }
    }

    if (attribution.value().has_value()) {
        const auto valid = validateManifestVerificationAttributionCheckpoint(*attribution.value());
        bool inputIdentityMatches = true;
        if (input.value().has_value()) {
            const bool sameBaseline = input.value()->baselineFingerprint
                == attribution.value()->baselineFingerprint;
            const bool exactNormalizedBaseline = !sameBaseline
                && normalization.value().has_value()
                && normalization.value()->durableBaselineFingerprint
                    == attribution.value()->baselineFingerprint
                && normalization.value()->normalizedBaselineFingerprint
                    == input.value()->baselineFingerprint;
            // The overlay hashes baseline content. It may differ only when
            // this exact durable-to-normalized baseline proof was actually
            // exercised. Merely having an unrelated proof never weakens the
            // ordinary strict comparison.
            const bool overlayMatches = exactNormalizedBaseline
                || input.value()->sourceOverlayFingerprint
                    == attribution.value()->sourceOverlayFingerprint;
            inputIdentityMatches = input.value()->attributionId == attribution.value()->id
                && (sameBaseline || exactNormalizedBaseline) && overlayMatches;
        }
        if (!valid.success() || attribution.value()->sourceRunId != sourceRunId
            || attribution.value()->sourceProjectId != source.projectId
            || !inputIdentityMatches) {
            return fail<std::optional<ManifestVerificationContinuationView>>(
                "verification_continuation.read_attribution_corrupt",
                "The durable diagnostic attribution does not match its continuation identity");
        }
        view.hasAttribution = true;
        view.attributionStatus = toString(attribution.value()->status);
        bool hasOwner = false;
        bool hasDebt = false;
        for (const auto& finding : attribution.value()->findings) {
            hasOwner = hasOwner || isOwnerFinding(finding);
            hasDebt = hasDebt || finding.kind
                == ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt;
            view.findings.push_back({finding.diagnosticCode, finding.requirementId,
                finding.relativePath, toString(finding.kind),
                finding.ownerTaskId.value_or(""), finding.ownerProposalId.value_or("")});
        }
        if (attribution.value()->status == ManifestVerificationAttributionStatus::Blocked) {
            view.canContinue = false;
            view.blockCode = attribution.value()->blockCode;
            view.blockMessage = attribution.value()->blockMessage;
        } else if (hasDebt) {
            view.canContinue = false;
            view.blockCode = "verification_continuation.pre_existing_debt_requires_prerequisite";
            view.blockMessage = "Pre-existing verification debt requires an explicit prerequisite Run";
        } else if (!hasOwner) {
            view.canContinue = false;
            view.blockCode = "verification_continuation.repair_scope_empty";
            view.blockMessage = "No proposal owner is eligible for a repair worker";
        }
        view.canStartRepair = child.has_value()
            && child->status == RunStatus::Prepared
            && attribution.value()->status == ManifestVerificationAttributionStatus::Attributed
            && hasOwner && !hasDebt;
    }

    const auto sourceLedger = workLedger_.findEntry(sourceRunId);
    if (!sourceLedger.success()) return fail<std::optional<ManifestVerificationContinuationView>>(
        sourceLedger.error()->code, sourceLedger.error()->message);
    std::optional<ManifestProposalSetVerification> sourceVerification;
    if (sourceLedger.value().has_value()) sourceVerification = sourceLedger.value()->verification;
    std::optional<ManifestProposalSetVerification> finalVerification;
    if (child.has_value()) {
        const auto childLedger = workLedger_.findEntry(child->id);
        if (!childLedger.success()) return fail<std::optional<ManifestVerificationContinuationView>>(
            childLedger.error()->code, childLedger.error()->message);
        if (childLedger.value().has_value()) finalVerification = childLedger.value()->verification;
    }
    std::unordered_map<std::string, std::string> finalStatus;
    if (finalVerification.has_value()) {
        for (const auto& criterion : finalVerification->criteria)
            finalStatus[criterion.requirementId] = toString(criterion.status);
    }
    if (sourceVerification.has_value()) {
        for (const auto& criterion : sourceVerification->criteria) {
            view.criteria.push_back({criterion.requirementId, criterion.required,
                toString(criterion.status), finalStatus[criterion.requirementId]});
        }
    }
    // QML-P0: layered strictly on top of the child's own already-terminal
    // Failed status - never computed for a Prepared/Running/Completed/
    // Cancelled child, and never itself changes childStatus or authorizes
    // Apply. See ManifestContentOutcome's own doc comment.
    if (child.has_value() && child->status == RunStatus::Failed && finalVerification.has_value()) {
        const auto outcome = classifyManifestContentOutcome(*finalVerification);
        view.contentOutcome = toString(outcome);
        if (outcome == ManifestContentOutcome::NeedsManualAction) {
            for (const auto& diagnostic : finalVerification->diagnostics) {
                const bool belongsToFailedQmlLintCheck = std::any_of(
                    finalVerification->checks.begin(), finalVerification->checks.end(),
                    [&](const ManifestVerificationCheckResult& check) {
                        return check.kind == ExecutionVerificationKind::QmlLint
                            && check.status != ExecutionVerificationStatus::Passed
                            && diagnostic.requirementId.has_value()
                            && *diagnostic.requirementId == check.profileId;
                    });
                if (!belongsToFailedQmlLintCheck) continue;
                for (const auto& finding : diagnostic.structuredFindings) {
                    view.remainingFindings.push_back({diagnostic.code,
                        *diagnostic.requirementId, finding.fileKey, finding.line, finding.column,
                        finding.tag, finding.text,
                        toString(classifyQmlLintFindingSeverity(finding.tag, finding.text))});
                }
            }
        }
    }
    return common::Result<std::optional<ManifestVerificationContinuationView>>::ok(
        std::move(view));
}

} // namespace ai::application
