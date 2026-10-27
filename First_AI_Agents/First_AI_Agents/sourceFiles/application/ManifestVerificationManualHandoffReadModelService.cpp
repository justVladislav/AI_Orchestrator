#include "ai/application/ManifestVerificationManualHandoffReadModelService.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationManualHandoffApplyAuthority.hpp"
#include "ai/application/ManifestVerificationRepairability.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {
namespace {

template<class T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

const char* candidateHealthToString(ManifestVerificationManualHandoffCandidateHealth health)
{
    switch (health) {
        case ManifestVerificationManualHandoffCandidateHealth::Unchanged: return "unchanged";
        case ManifestVerificationManualHandoffCandidateHealth::Edited: return "edited";
        case ManifestVerificationManualHandoffCandidateHealth::Missing: return "missing";
        case ManifestVerificationManualHandoffCandidateHealth::Unreadable: return "unreadable";
        case ManifestVerificationManualHandoffCandidateHealth::StorageMismatch:
            return "storage_mismatch";
    }
    return "missing";
}

const char* attemptResultKindToString(ManifestVerificationManualHandoffAttemptResultKind kind)
{
    switch (kind) {
        case ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess:
            return "verified_success";
        case ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure:
            return "verified_content_failure";
        case ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification:
            return "rejected_before_verification";
        case ManifestVerificationManualHandoffAttemptResultKind::Incomplete:
            return "incomplete";
    }
    return "incomplete";
}

std::optional<std::string> normalizedCandidateDiagnosticPath(const std::string& fileKey)
{
    std::string relative = fileKey;
    bool candidateRelative = false;
    for (const std::string_view prefix :
        {std::string_view("<candidate>/"), std::string_view("<candidate>\\")}) {
        if (relative.starts_with(prefix)) {
            relative = relative.substr(prefix.size());
            candidateRelative = true;
            break;
        }
    }
    if (!candidateRelative || relative.empty()) return std::nullopt;
    const auto path = detail::pathFromUtf8(relative).lexically_normal();
    const auto key = detail::normalizedPathKey(path);
    if (key.empty() || key == "." || key.starts_with("../")
        || key.find("/../") != std::string::npos || key.find(':') != std::string::npos) {
        return std::nullopt;
    }
    // Case-preserved, forward-slash display form: detail::normalizedPathKey
    // above is a lowercase-folded lookup key only (its own doc comment says
    // so explicitly) - the value returned here is shown verbatim to the
    // user, so it must not silently relabel a real "A.qml" as "a.qml".
    return path.generic_string();
}

std::string formatStructuredDiagnostic(const ManifestVerificationDiagnostic& diagnostic,
    const ManifestVerificationDiagnosticFinding& finding, const std::string& relativePath)
{
    std::string line = diagnostic.code;
    if (diagnostic.requirementId.has_value()) line += " [" + *diagnostic.requirementId + "]";
    line += ": " + relativePath;
    if (finding.line > 0) line += ":" + std::to_string(finding.line);
    if (finding.column > 0) line += ":" + std::to_string(finding.column);
    line += ": " + finding.text;
    if (!finding.tag.empty()) line += " [" + finding.tag + "]";
    return line;
}

std::string formatUnmappedStructuredDiagnostic(const ManifestVerificationDiagnostic& diagnostic,
    const ManifestVerificationDiagnosticFinding& finding)
{
    std::string line = diagnostic.code;
    if (diagnostic.requirementId.has_value()) line += " [" + *diagnostic.requirementId + "]";
    line += ": ";
    line += finding.text.empty() ? diagnostic.message : finding.text;
    if (!finding.tag.empty()) line += " [" + finding.tag + "]";
    return line;
}

constexpr std::size_t kMaximumListedOutsidePaths = 5;

} // namespace

ManifestVerificationRecordedFindingsView buildManifestVerificationRecordedFindings(
    bool anchorFailed,
    const std::optional<ManifestWorkLedgerEntry>& anchorLedger,
    ChangeProposalService& proposals,
    const std::unordered_set<std::string>* allowedPathKeys)
{
    ManifestVerificationRecordedFindingsView out;
    out.verdictCode = "manual_handoff.recorded_findings_unavailable";
    out.verdictMessage = "The anchor Run has no recorded verification findings";
    if (!anchorFailed || !anchorLedger.has_value() || !anchorLedger->verification.has_value()) {
        return out;
    }
    const auto& verification = *anchorLedger->verification;

    std::unordered_set<std::string> targetKeys;
    bool targetsKnown = false;
    if (allowedPathKeys == nullptr) {
        // Create/Modify only: a Rename or Delete target can never enter a handoff's allowedPaths.
        if (verification.orderedProposalIds.empty()) {
            out.classificationErrorCode = "manual_handoff.classification_unavailable";
            out.classificationErrorMessage =
                "The failed verification names no proposals, so the editable files cannot be classified";
        } else {
            targetsKnown = true;
            for (const auto& proposalId : verification.orderedProposalIds) {
                // listChanges() cannot tell a missing proposal from one with no changes, so the
                // proposal itself must exist first - an absent one would otherwise make every file
                // look "outside" every target.
                const auto header = proposals.findChangeSet(proposalId);
                const auto changes = header.success()
                    ? proposals.listChanges(proposalId)
                    : common::Result<std::vector<ProposedFileChange>>::failure(*header.error());
                if (!changes.success()) {
                    targetsKnown = false;
                    targetKeys.clear();
                    out.classificationErrorCode = "manual_handoff.classification_unavailable";
                    out.classificationErrorMessage =
                        "The changes of a frozen proposal could not be read (" + changes.error()->code
                        + "), so the editable files cannot be classified";
                    break;
                }
                for (const auto& change : changes.value()) {
                    if (change.operation != FileChangeOperation::Create
                        && change.operation != FileChangeOperation::Modify) continue;
                    targetKeys.insert(detail::normalizedPathKey(change.relativePath));
                }
            }
        }
    }

    bool hasUnmappedBlocking = false;
    bool sampleMayBeTruncated = false;
    std::size_t mappedBlockingCount = 0;
    std::size_t outsideBlockingCount = 0;
    std::unordered_set<std::string> seenOutside;
    for (const auto& diagnostic : verification.diagnostics) {
        if (diagnostic.code == kQmlBaselineDebtDiagnosticCode) {
            out.debtCount += diagnostic.structuredFindings.empty() ? 1 : diagnostic.structuredFindings.size();
            continue;
        }
        const bool blockingDiagnostic = !isNonBlockingQmlWarningCode(diagnostic.code)
            && !isIgnorableNonRepairDiagnosticCode(diagnostic.code);
        const std::string severity = blockingDiagnostic ? "blocking" : "advisory";
        if (diagnostic.structuredFindings.empty()) {
            ManifestVerificationRecordedFindingView row;
            row.diagnosticCode = diagnostic.code;
            row.severity = severity;
            row.text = diagnostic.code + ": " + diagnostic.message;
            row.authority = "unknown";
            out.findings.push_back(std::move(row));
            if (blockingDiagnostic) hasUnmappedBlocking = true;
            continue;
        }
        if (blockingDiagnostic
            && diagnostic.structuredFindings.size() >= kMaximumRecordedFindingsPerDiagnostic) {
            sampleMayBeTruncated = true;
        }
        for (const auto& finding : diagnostic.structuredFindings) {
            ManifestVerificationRecordedFindingView row;
            row.diagnosticCode = diagnostic.code;
            row.severity = severity;
            row.line = finding.line;
            row.column = finding.column;
            row.tag = finding.tag;
            const auto relativePath = normalizedCandidateDiagnosticPath(finding.fileKey);
            if (!relativePath.has_value()) {
                row.text = formatUnmappedStructuredDiagnostic(diagnostic, finding);
                row.authority = "unknown";
                if (blockingDiagnostic) hasUnmappedBlocking = true;
                out.findings.push_back(std::move(row));
                continue;
            }
            row.relativePath = *relativePath;
            row.text = formatStructuredDiagnostic(diagnostic, finding, *relativePath);
            const auto key = detail::normalizedPathKey(detail::pathFromUtf8(*relativePath));
            if (allowedPathKeys != nullptr) {
                row.authority = allowedPathKeys->contains(key) ? "editable" : "not_editable";
            } else if (targetsKnown) {
                row.authority = targetKeys.contains(key)
                    ? "unknown_until_created" : "outside_all_create_modify_targets";
            } else {
                row.authority = "unknown";
            }
            if (blockingDiagnostic) {
                ++mappedBlockingCount;
                if (row.authority == "not_editable"
                    || row.authority == "outside_all_create_modify_targets") {
                    ++outsideBlockingCount;
                    if (seenOutside.insert(key).second) out.outsideScopePaths.push_back(*relativePath);
                }
            }
            out.findings.push_back(std::move(row));
        }
    }

    // A proven outside-scope blocker is a positive fact: it wins over any unknown data below.
    if (outsideBlockingCount > 0) {
        out.verdict = "blocked_outside_scope";
        out.verdictCode = "manual_handoff.blocking_findings_outside_scope";
        std::string files;
        for (std::size_t i = 0; i < out.outsideScopePaths.size() && i < kMaximumListedOutsidePaths; ++i) {
            if (i > 0) files += ", ";
            files += out.outsideScopePaths[i];
        }
        if (out.outsideScopePaths.size() > kMaximumListedOutsidePaths) {
            files += " (+" + std::to_string(out.outsideScopePaths.size() - kMaximumListedOutsidePaths)
                + " more)";
        }
        out.verdictMessage = std::to_string(outsideBlockingCount)
            + " recorded blocking finding(s) lie in file(s) this handoff will certainly not be able to "
              "edit: " + files + ". Editing the repair workspace cannot clear them.";
        return out;
    }
    out.verdict = "unknown";
    if (!out.classificationErrorCode.empty()) {
        out.verdictCode = out.classificationErrorCode;
        out.verdictMessage = out.classificationErrorMessage;
    } else if (hasUnmappedBlocking) {
        out.verdictCode = "manual_handoff.verdict_unmapped_blocking_finding";
        out.verdictMessage = "A blocking failure has no file this handoff could be compared with";
    } else if (mappedBlockingCount == 0) {
        out.verdictCode = "manual_handoff.verdict_no_blocking_finding_recorded";
        out.verdictMessage = "No blocking finding is recorded for the failed verification";
    } else if (sampleMayBeTruncated) {
        out.verdictCode = "manual_handoff.verdict_sample_may_be_truncated";
        out.verdictMessage = "The recorded findings are a bounded sample that may be incomplete";
    } else {
        out.verdict = "no_known_obstacle";
        out.verdictCode = "manual_handoff.no_known_obstacle";
        out.verdictMessage = "No recorded blocking finding is proven uneditable; the full verification decides";
    }
    return out;
}

const char* toString(ManifestVerificationManualHandoffResolution resolution) noexcept
{
    switch (resolution) {
        case ManifestVerificationManualHandoffResolution::NotApplicable:
            return "not_applicable";
        case ManifestVerificationManualHandoffResolution::Unresolved:
            return "unresolved";
        case ManifestVerificationManualHandoffResolution::VerifiedAwaitingApply:
            return "verified_awaiting_apply";
        case ManifestVerificationManualHandoffResolution::PartiallyApplied:
            return "partially_applied";
        case ManifestVerificationManualHandoffResolution::ResolvedManuallyApplied:
            return "resolved_manually_applied";
    }
    return "not_applicable";
}

ManifestVerificationManualHandoffReadModelService::
ManifestVerificationManualHandoffReadModelService(
    IRunRepository& runs, IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationManualHandoffRepository& manualHandoffs,
    ManifestVerificationManualHandoffService& manualHandoffService,
    ManifestApplyVerificationGate& applyGate,
    ChangeProposalService& proposals,
    ProposalReviewService& reviewService)
    : runs_(runs), workLedger_(workLedger), manualHandoffs_(manualHandoffs),
      manualHandoffService_(manualHandoffService), applyGate_(applyGate),
      proposals_(proposals), reviewService_(reviewService)
{
}

common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>
ManifestVerificationManualHandoffReadModelService::resolutionsForRuns(
    const std::vector<Run>& runs) const
{
    const auto allHandoffs = manualHandoffs_.listAllHandoffs();
    if (!allHandoffs.success()) {
        return fail<std::vector<ManifestVerificationManualHandoffRunResolution>>(
            allHandoffs.error()->code, allHandoffs.error()->message);
    }

    std::unordered_map<RunId, const ManifestVerificationManualHandoff*> byAnchor;
    byAnchor.reserve(allHandoffs.value().size());
    for (const auto& handoff : allHandoffs.value()) {
        const auto [ignored, inserted] = byAnchor.emplace(
            handoff.verificationAnchorRunId, &handoff);
        if (!inserted) {
            return fail<std::vector<ManifestVerificationManualHandoffRunResolution>>(
                "manual_handoff.duplicate_anchor",
                "More than one manual handoff is bound to the same verification anchor Run");
        }
    }

    std::vector<ManifestVerificationManualHandoffRunResolution> result;
    result.reserve(runs.size());
    for (const auto& run : runs) {
        ManifestVerificationManualHandoffRunResolution row;
        row.runId = run.id;
        if (run.status != RunStatus::Failed) {
            result.push_back(std::move(row));
            continue;
        }

        row.resolution = ManifestVerificationManualHandoffResolution::Unresolved;
        const auto handoffIt = byAnchor.find(run.id);
        if (handoffIt == byAnchor.end()) {
            result.push_back(std::move(row));
            continue;
        }

        const auto authority = resolveManualHandoffApplyAuthority(
            manualHandoffs_, workLedger_, runs_, *handoffIt->second);
        if (!authority.success()) {
            return fail<std::vector<ManifestVerificationManualHandoffRunResolution>>(
                authority.error()->code, authority.error()->message);
        }
        if (!authority.value().has_value()) {
            result.push_back(std::move(row));
            continue;
        }

        const auto& proposalIds = authority.value()->ledgerEntry.proposalIds;
        if (proposalIds.empty()) {
            return fail<std::vector<ManifestVerificationManualHandoffRunResolution>>(
                "manual_handoff.authoritative_set_empty",
                "The authoritative manual-handoff verification has no proposal members");
        }

        std::size_t appliedCount = 0;
        for (const auto& proposalId : proposalIds) {
            const auto proposal = proposals_.findChangeSet(proposalId);
            if (!proposal.success()) {
                return fail<std::vector<ManifestVerificationManualHandoffRunResolution>>(
                    proposal.error()->code, proposal.error()->message);
            }
            if (proposal.value().state == ChangeSetState::Applied) ++appliedCount;
        }

        if (appliedCount == proposalIds.size()) {
            row.resolution =
                ManifestVerificationManualHandoffResolution::ResolvedManuallyApplied;
        } else if (appliedCount > 0) {
            row.resolution = ManifestVerificationManualHandoffResolution::PartiallyApplied;
        } else {
            row.resolution =
                ManifestVerificationManualHandoffResolution::VerifiedAwaitingApply;
        }
        result.push_back(std::move(row));
    }
    return common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>::ok(
        std::move(result));
}

common::Result<std::optional<ManifestVerificationManualHandoffView>>
ManifestVerificationManualHandoffReadModelService::viewForRun(
    const RunId& verificationAnchorRunId) const
{
    const auto anchorResult = runs_.findRun(verificationAnchorRunId);
    if (!anchorResult.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        anchorResult.error()->code, anchorResult.error()->message);
    if (!anchorResult.value().has_value())
        return common::Result<std::optional<ManifestVerificationManualHandoffView>>::ok(std::nullopt);
    const Run& anchor = *anchorResult.value();

    // The only two anchor workflows materialize() itself ever accepts (see
    // its own doc comment) - nothing a manual handoff could mean for any
    // other Run, mirroring ManifestVerificationContinuationReadModelService's
    // own precedent for a selected Run outside its own scope.
    if (anchor.workflowId != kExecutionManifestWorkflowId
        && anchor.workflowId != kManifestVerificationContinuationWorkflowId) {
        return common::Result<std::optional<ManifestVerificationManualHandoffView>>::ok(std::nullopt);
    }

    ManifestVerificationManualHandoffView view;
    view.verificationAnchorRunId = verificationAnchorRunId;
    view.verificationAnchorStatus = toString(anchor.status);

    const auto anchorLedger = workLedger_.findEntry(verificationAnchorRunId);
    if (!anchorLedger.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        anchorLedger.error()->code, anchorLedger.error()->message);

    const auto handoffResult = manualHandoffs_.findHandoffByAnchorRun(verificationAnchorRunId);
    if (!handoffResult.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        handoffResult.error()->code, handoffResult.error()->message);
    if (!handoffResult.value().has_value()) {
        // The recorded findings are visible before any repair workspace exists; a read failure inside
        // the classification never hides them (it becomes an unknown authority with a Backend code).
        view.recordedFindings = buildManifestVerificationRecordedFindings(
            anchor.status == RunStatus::Failed, anchorLedger.value(), proposals_, nullptr);
        // Not yet created - every field below stays at its default except
        // canCreate/createReasonCode/createReasonMessage, which are the only
        // Backend-owned answer to "is Create actually offered here" - QML
        // never guesses this from verificationAnchorStatus itself.
        const auto eligibility = manualHandoffService_.evaluateCreateEligibility(anchor);
        if (!eligibility.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
            eligibility.error()->code, eligibility.error()->message);
        view.canCreate = eligibility.value().eligible;
        view.createReasonCode = eligibility.value().reasonCode;
        view.createReasonMessage = eligibility.value().reasonMessage;
        return common::Result<std::optional<ManifestVerificationManualHandoffView>>::ok(
            std::move(view));
    }
    const ManifestVerificationManualHandoff& handoff = *handoffResult.value();
    view.hasHandoff = true;
    view.handoffId = handoff.id;
    view.originManifestRunId = handoff.originManifestRunId;

    const auto originResult = runs_.findRun(handoff.originManifestRunId);
    if (!originResult.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        originResult.error()->code, originResult.error()->message);
    if (!originResult.value().has_value()) {
        return fail<std::optional<ManifestVerificationManualHandoffView>>(
            "manual_handoff.read_origin_missing",
            "The manual handoff's own origin Manifest Run is unavailable");
    }
    view.originManifestStatus = toString(originResult.value()->status);

    view.allowedRelativePaths.reserve(handoff.allowedPaths.size());
    std::unordered_set<std::string> allowedPathKeys;
    for (const auto& allowed : handoff.allowedPaths) {
        view.allowedRelativePaths.push_back(allowed.relativePath);
        allowedPathKeys.insert(
            detail::normalizedPathKey(detail::pathFromUtf8(allowed.relativePath)));
    }

    view.recordedFindings = buildManifestVerificationRecordedFindings(
        anchor.status == RunStatus::Failed, anchorLedger.value(), proposals_, &allowedPathKeys);
    if (anchorLedger.value().has_value() && anchorLedger.value()->verification.has_value()) {
        for (const auto& diagnostic : anchorLedger.value()->verification->diagnostics) {
            if (diagnostic.code == kQmlBaselineDebtDiagnosticCode) continue;
            for (const auto& finding : diagnostic.structuredFindings) {
                const auto relativePath = normalizedCandidateDiagnosticPath(finding.fileKey);
                if (!relativePath.has_value()) continue;
                const auto matchKey =
                    detail::normalizedPathKey(detail::pathFromUtf8(*relativePath));
                if (!allowedPathKeys.contains(matchKey)) continue;
                view.initialScopeDiagnostics.push_back(
                    formatStructuredDiagnostic(diagnostic, finding, *relativePath));
            }
        }
    }

    const auto candidate = manualHandoffService_.inspectCandidateEdits(handoff);
    if (!candidate.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        candidate.error()->code, candidate.error()->message);
    view.candidateHealth = candidateHealthToString(candidate.value().candidate.health);
    view.editableChangedRelativePaths = candidate.value().editableChangedPaths;
    view.resetRequiredRelativePaths = candidate.value().resetRequiredPaths;

    const auto attempts = manualHandoffs_.listAttemptsForHandoff(handoff.id);
    if (!attempts.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        attempts.error()->code, attempts.error()->message);

    for (const auto& attempt : attempts.value()) {
        const auto attemptRun = runs_.findRun(attempt.attemptRunId);
        if (!attemptRun.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
            attemptRun.error()->code, attemptRun.error()->message);

        ManifestVerificationManualHandoffAttemptHistoryView attemptView;
        attemptView.attemptRunId = attempt.attemptRunId;
        attemptView.attemptOrdinal = attempt.attemptOrdinal;
        attemptView.attemptStatus = attemptRun.value().has_value()
            ? std::string(toString(attemptRun.value()->status)) : std::string();

        const auto typedResult = manualHandoffs_.findAttemptResultByAttemptId(attempt.id);
        if (!typedResult.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
            typedResult.error()->code, typedResult.error()->message);
        if (typedResult.value().has_value()) {
            attemptView.resultKind = attemptResultKindToString(typedResult.value()->kind);
            if (typedResult.value()->kind
                    == ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification) {
                attemptView.rejectionCode = typedResult.value()->rejectionCode.value_or(std::string());
                attemptView.rejectionMessage =
                    typedResult.value()->rejectionMessage.value_or(std::string());
            }
        }

        // A failed verified attempt is still valuable durable evidence even
        // though it can never be Apply authority. Project its own exact
        // Work Ledger verification into the handoff history instead of
        // forcing users to discover an internal synthetic Run and its old
        // generic error string in the global Runs list.
        const auto attemptLedger = workLedger_.findEntry(attempt.attemptRunId);
        if (!attemptLedger.success()) {
            return fail<std::optional<ManifestVerificationManualHandoffView>>(
                attemptLedger.error()->code, attemptLedger.error()->message);
        }
        if (attemptLedger.value().has_value()) {
            const auto& ledger = *attemptLedger.value();
            if (ledger.runId != attempt.attemptRunId
                || ledger.projectId != handoff.sourceProjectId
                || (attemptRun.value().has_value()
                    && ledger.terminalStatus != attemptRun.value()->status)) {
                return fail<std::optional<ManifestVerificationManualHandoffView>>(
                    "manual_handoff.read_attempt_identity_conflict",
                    "A manual-handoff attempt's Run and Work Ledger identities disagree");
            }
            if (ledger.verification.has_value()) {
                const auto& verification = *ledger.verification;
                attemptView.hasVerification = true;
                attemptView.proposalSafetyStatus = toString(verification.proposalSafetyStatus);
                attemptView.buildLintTestStatus = toString(verification.buildLintTestStatus);
                attemptView.acceptanceStatus = toString(verification.acceptanceStatus);
                attemptView.verificationDiagnostics.reserve(verification.diagnostics.size());
                for (const auto& diagnostic : verification.diagnostics) {
                    std::string line = diagnostic.code;
                    if (diagnostic.requirementId.has_value()) {
                        line += " [" + *diagnostic.requirementId + "]";
                    }
                    line += ": " + diagnostic.message;
                    attemptView.verificationDiagnostics.push_back(line);
                    // Every structured finding contributes to exactly one
                    // presentation bucket. A mixed diagnostic may contain both
                    // safely candidate-relative findings and a tool/module-level
                    // finding with no safe relative path; dropping the latter
                    // merely because a sibling mapped successfully would hide
                    // evidence. Keep that finding in generalDiagnostics without
                    // duplicating the whole parent diagnostic line.
                    bool hasStructuredFindings = false;
                    for (const auto& finding : diagnostic.structuredFindings) {
                        hasStructuredFindings = true;
                        const auto relativePath = normalizedCandidateDiagnosticPath(finding.fileKey);
                        if (!relativePath.has_value()) {
                            attemptView.generalDiagnostics.push_back(
                                formatUnmappedStructuredDiagnostic(diagnostic, finding));
                            continue;
                        }
                        const auto matchKey =
                            detail::normalizedPathKey(detail::pathFromUtf8(*relativePath));
                        auto findingLine = formatStructuredDiagnostic(
                            diagnostic, finding, *relativePath);
                        const bool isAllowedPath = allowedPathKeys.contains(matchKey);
                        if (isAllowedPath) {
                            attemptView.actionableDiagnostics.push_back(findingLine);
                        } else {
                            attemptView.outsideScopeDiagnostics.push_back(findingLine);
                        }
                        // The two baseline-aware codes carry their own
                        // provenance regardless of allowlist membership (an
                        // outside-Manifest finding is never handoff-allowed
                        // by construction, and baseline debt is debt
                        // wherever it lives); every other qml-lint code
                        // (shared by the legacy per-task-scope path and the
                        // baseline-aware path's own "new, in-scope" case)
                        // falls back to the SAME allowlist split the legacy
                        // buckets above already use, so this segmentation
                        // never disagrees with them for a Run that never
                        // took the baseline-aware branch.
                        if (diagnostic.code == kQmlBaselineDebtDiagnosticCode) {
                            attemptView.unchangedBaselineDebtDiagnostics.push_back(
                                std::move(findingLine));
                        } else if (diagnostic.code == "verification.qml_warning_outside_manifest") {
                            attemptView.newOutsideManifestOrContextDiagnostics.push_back(
                                std::move(findingLine));
                        } else if (isAllowedPath) {
                            attemptView.newActionableDiagnostics.push_back(std::move(findingLine));
                        } else {
                            attemptView.newOutsideManifestOrContextDiagnostics.push_back(
                                std::move(findingLine));
                        }
                    }
                    if (!hasStructuredFindings) {
                        attemptView.generalDiagnostics.push_back(std::move(line));
                    }
                }
            }
        }
        view.attempts.push_back(std::move(attemptView));
    }

    // QML-P4R.5 hardening: the authoritative attempt is resolved by the SAME
    // shared Backend resolver AG-16 itself uses for the real Apply decision
    // (see resolveManualHandoffApplyAuthority's own doc comment) - never
    // re-derived locally, so this read model can never disagree with it.
    const auto authority = resolveManualHandoffApplyAuthority(
        manualHandoffs_, workLedger_, runs_, handoff);
    if (!authority.success()) return fail<std::optional<ManifestVerificationManualHandoffView>>(
        authority.error()->code, authority.error()->message);

    if (authority.value().has_value()) {
        const auto& won = *authority.value();
        for (auto& attemptView : view.attempts) {
            attemptView.isAuthoritative = attemptView.attemptRunId == won.attempt.attemptRunId;
        }

        if (won.ledgerEntry.verification.has_value()) {
            view.hasFinalVerification = true;
            view.finalProposalSafetyStatus =
                toString(won.ledgerEntry.verification->proposalSafetyStatus);
            view.finalBuildLintTestStatus =
                toString(won.ledgerEntry.verification->buildLintTestStatus);
            view.finalAcceptanceStatus = toString(won.ledgerEntry.verification->acceptanceStatus);
        }

        // AG-16 is the sole Apply-eligibility authority. A handoff verifies a
        // SET of proposals, each independently Apply-eligible or not (an
        // earlier member may already have been Applied while a later one is
        // still ReadyForReview) - so canApply must reflect whether ANY
        // not-yet-applied member is still eligible, never just the first id
        // in the set.
        for (const auto& proposalId : won.ledgerEntry.proposalIds) {
            const auto eligibility = applyGate_.evaluate(proposalId);
            view.applyReasonCode = eligibility.reasonCode;
            view.applyReasonMessage = eligibility.reasonMessage;
            if (eligibility.allowed) {
                view.canApply = true;
                break;
            }
        }

        // Post-P4R.6 human review/Apply hardening: project EVERY member of
        // the authoritative exact verified set for full human review,
        // independent of canApply's own short-circuiting loop above - a
        // reviewer must be able to inspect and individually Apply/Reject
        // each member, not just learn that SOME member is eligible. Only
        // ever populated once an authoritative attempt actually exists
        // (hasFinalVerification) - a failed/superseded attempt provides no
        // authority and therefore no review surface, exactly like canApply.
        if (view.hasFinalVerification) {
            view.reviewMembers.reserve(won.ledgerEntry.proposalIds.size());
            for (const auto& proposalId : won.ledgerEntry.proposalIds) {
                // A missing or otherwise unreadable member of the Run's OWN
                // recorded verified set is a durable-storage integrity
                // failure, never a normal "not found" - fail the whole view
                // closed rather than silently omitting the member.
                const auto proposalResult = proposals_.findChangeSet(proposalId);
                if (!proposalResult.success()) {
                    return fail<std::optional<ManifestVerificationManualHandoffView>>(
                        proposalResult.error()->code, proposalResult.error()->message);
                }
                const ProposedChangeSet& proposal = proposalResult.value();

                ManifestVerificationManualHandoffReviewMemberView member;
                member.proposalId = proposalId;
                member.owningRunId = proposal.runId;
                member.summary = proposal.summary;
                member.state = toString(proposal.state);
                member.alreadyApplied = proposal.state == ChangeSetState::Applied;

                const auto rejection = reviewService_.findRejection(proposalId);
                if (!rejection.success()) {
                    return fail<std::optional<ManifestVerificationManualHandoffView>>(
                        rejection.error()->code, rejection.error()->message);
                }
                if (rejection.value().has_value()) {
                    member.reviewRejected = true;
                    member.reviewRejectedReason = rejection.value()->reason.value_or(std::string());
                }

                const auto changes = proposals_.listChanges(proposalId);
                if (!changes.success()) {
                    return fail<std::optional<ManifestVerificationManualHandoffView>>(
                        changes.error()->code, changes.error()->message);
                }
                member.changes.reserve(changes.value().size());
                for (const auto& change : changes.value()) {
                    ManifestVerificationManualHandoffReviewChangeView changeView;
                    changeView.changeId = change.changeId;
                    changeView.operation = toString(change.operation);
                    changeView.relativePath = change.relativePath.generic_string();
                    changeView.hasExpectedHash = change.expectedOriginalHash.has_value();
                    member.changes.push_back(std::move(changeView));
                }

                const auto findings = proposals_.listValidationFindings(proposalId);
                if (!findings.success()) {
                    return fail<std::optional<ManifestVerificationManualHandoffView>>(
                        findings.error()->code, findings.error()->message);
                }
                member.findings.reserve(findings.value().size());
                for (const auto& finding : findings.value()) {
                    member.findings.push_back({finding.code, finding.message, finding.changeId});
                }

                // AG-16 itself - freshly re-evaluated per member, never
                // cached from the canApply loop above.
                const auto memberEligibility = applyGate_.evaluate(proposalId);
                member.applyAllowed = memberEligibility.allowed;
                member.applyBlockCode = memberEligibility.reasonCode;
                member.applyBlockMessage = memberEligibility.reasonMessage;

                view.reviewMembers.push_back(std::move(member));
            }
        }
    }
    if (!view.canApply && view.applyReasonCode.empty()) {
        view.applyReasonCode = "manual_handoff.no_authoritative_attempt";
        view.applyReasonMessage = "No attempt has yet produced an authoritative verified result";
    }

    return common::Result<std::optional<ManifestVerificationManualHandoffView>>::ok(
        std::move(view));
}

} // namespace ai::application
