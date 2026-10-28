#include "ai/application/execution/ManifestWorkLedgerService.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/execution/ExecutionManifestPlanner.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/verification/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/application/execution/ManifestWorkerNoChanges.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

bool isTerminalManifestStatus(RunStatus status)
{
    switch (status) {
    case RunStatus::Completed:
    case RunStatus::Failed:
    case RunStatus::Cancelled:
    case RunStatus::Interrupted:
        return true;
    default:
        return false;
    }
}

std::string boundedCopy(std::string value, std::size_t maxBytes)
{
    if (value.size() > maxBytes) value.resize(maxBytes);
    return value;
}

bool isSha256Digest(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
    });
}

} // namespace

ManifestWorkLedgerService::ManifestWorkLedgerService(
    IManifestWorkLedgerRepository& ledger,
    IRunRepository& runs,
    IExecutionManifestRepository& manifests,
    IManifestTaskExecutionRepository& taskExecutions,
    IChangeProposalRepository& proposals,
    ArtifactService* artifacts,
    IChangeApprovalRepository* approvals,
    IProposalReviewRepository* reviews,
    IRunWorkspaceScopeRepository* workspaceScope)
    : ledger_(ledger)
    , runs_(runs)
    , manifests_(manifests)
    , taskExecutions_(taskExecutions)
    , proposals_(proposals)
    , artifacts_(artifacts)
    , approvals_(approvals)
    , reviews_(reviews)
    , workspaceScope_(workspaceScope)
{
}

namespace {

// AG-14.2: tri-state lookup of one named artifact's content for runId -
// distinct from a simple std::optional, which cannot separate "genuinely
// absent" from "could not tell." Result::failure means listArtifacts/
// getArtifactContent themselves failed (a transient infra problem - the
// caller must propagate this as its own failure, exactly like the
// repository reads below, never silently treat it as absence).
// Result::ok(nullopt) means the listing itself succeeded and no such
// artifact exists - legitimate, e.g. a Run that failed before this
// artifact was ever due to be written. Result::ok(content) means found.
// artifacts == nullptr (no ArtifactService wired at all) is also
// Result::ok(nullopt): a deliberate configuration, not an infra failure.
common::Result<std::optional<std::string>> findArtifactContentOrFail(
    ArtifactService* artifacts, const RunId& runId, std::string_view name)
{
    if (artifacts == nullptr) {
        return common::Result<std::optional<std::string>>::ok(std::nullopt);
    }
    const auto artifactList = artifacts->listArtifacts(runId);
    if (!artifactList.success()) {
        return common::Result<std::optional<std::string>>::failure(*artifactList.error());
    }
    for (const auto& artifact : artifactList.value()) {
        if (artifact.name != name) continue;
        const auto content = artifacts->getArtifactContent(artifact.id);
        if (!content.success()) {
            return common::Result<std::optional<std::string>>::failure(*content.error());
        }
        return common::Result<std::optional<std::string>>::ok(content.value());
    }
    return common::Result<std::optional<std::string>>::ok(std::nullopt);
}

// A continuation child and a manual-handoff attempt deliberately do not
// capture another live-workspace baseline: they verify a candidate rooted in
// their parent Manifest lineage. Their Work Ledger rows nevertheless retain
// that lineage's baseline fingerprint for AG-16. When one of those derived
// rows is the project's newest ledger entry, resolve the artifact through its
// immutable Run parent chain instead of incorrectly demanding a duplicate
// workspace-baseline artifact from the derived Run itself.
//
// This is not a missing-artifact bypass. Only the two known derived workflows
// may delegate to a parent, every hop must remain in the same project, cycles
// fail closed, and the ordinary Manifest ancestor's artifact is still required
// and checked against the newest ledger entry's exact fingerprint below.
common::Result<RunId> resolvePlannerBaselineArtifactRun(
    IRunRepository& runs, const ManifestWorkLedgerEntry& newestEntry)
{
    RunId currentRunId = newestEntry.runId;
    std::unordered_set<RunId> visited;
    for (;;) {
        if (!visited.insert(currentRunId).second) {
            return common::Result<RunId>::failure({
                "manifest_work_ledger.context_baseline_lineage_invalid",
                "The most recent ledger entry has a cyclic baseline lineage"});
        }

        const auto found = runs.findRun(currentRunId);
        if (!found.success()) return common::Result<RunId>::failure(*found.error());
        if (!found.value().has_value()) {
            return common::Result<RunId>::failure({
                "manifest_work_ledger.context_baseline_lineage_invalid",
                "The most recent ledger entry's baseline lineage names a missing Run"});
        }
        const Run& run = *found.value();
        if (run.projectId != newestEntry.projectId) {
            return common::Result<RunId>::failure({
                "manifest_work_ledger.context_baseline_lineage_invalid",
                "The most recent ledger entry's baseline lineage crosses projects"});
        }
        if (run.workflowId == kExecutionManifestWorkflowId) {
            return common::Result<RunId>::ok(run.id);
        }
        if (run.workflowId != kManifestVerificationContinuationWorkflowId
            && run.workflowId != kManifestVerificationManualHandoffWorkflowId) {
            return common::Result<RunId>::failure({
                "manifest_work_ledger.context_baseline_lineage_invalid",
                "The most recent ledger entry is not rooted in a Manifest workflow"});
        }
        if (!run.parentRunId.has_value() || run.parentRunId->empty()) {
            return common::Result<RunId>::failure({
                "manifest_work_ledger.context_baseline_lineage_invalid",
                "A derived verification Run has no parent baseline lineage"});
        }
        currentRunId = *run.parentRunId;
    }
}

void persistWriteFailureArtifact(
    ArtifactService* artifacts, const RunId& runId,
    std::string_view operation, const common::Error& error)
{
    if (artifacts == nullptr) return;
    std::ostringstream report;
    report << "# Manifest Work Ledger write failed\n\n"
           << "Operation: " << operation << "\n\n"
           << "Error code: " << error.code << "\n\n"
           << "Message: " << error.message << "\n\n"
           << "This Run's own terminal status/approval/apply/rejection was already "
              "committed successfully; only this durable continuity record failed to "
              "persist. A later startup reconciliation pass may recover a missing "
              "terminal entry or review/apply transition automatically (see "
              "ManifestWorkLedgerService::reconcileMissingEntries and "
              "reconcileMissingTransitions).\n";
    const std::string text = report.str();
    constexpr std::string_view kReportName = "manifest-work-ledger-write-failed.md";
    // The startup reconciliation re-tries a Run whose record can never be written (for example a legacy Completed
    // no-changes-needed Run without a planner identity, which this service must not invent), and every try fails the
    // same way. The report says nothing new the second time, so an identical one - same name, same bytes - already
    // stored for THIS Run is not written again; a different failure (another operation, code or message) still is.
    // If the Run's artifacts cannot be listed, write anyway: a possible duplicate is better than a hidden failure.
    const std::string digest = sha256Hex(text);
    if (const auto stored = artifacts->listArtifacts(runId); stored.success()) {
        const bool alreadyReported = std::any_of(
            stored.value().begin(), stored.value().end(), [&](const Artifact& artifact) {
                return artifact.name == kReportName && artifact.sha256 == digest;
            });
        if (alreadyReported) return;
    }
    static_cast<void>(artifacts->createDiagnosticArtifact(
        runId, std::nullopt, ArtifactKind::Report,
        std::string(kReportName), "text/markdown", text));
}

} // namespace

common::Result<void> ManifestWorkLedgerService::recordTerminalRun(const RunId& runId)
{
    const auto failVisible = [&](const common::Error& error) {
        persistWriteFailureArtifact(artifacts_, runId, "recordTerminalRun", error);
        return common::Result<void>::failure(error);
    };
    const auto foundRun = runs_.findRun(runId);
    if (!foundRun.success()) return failVisible(*foundRun.error());
    if (!foundRun.value().has_value()) return common::Result<void>::ok();
    const Run run = *foundRun.value();
    // MW-P5.4: a continuation child Run (see
    // ManifestVerificationContinuationRepairService) is functionally a
    // Manifest execution too - it freezes a Backend-authored
    // ExecutionManifest, dispatches Propose-capability tasks, and produces
    // its own combined-verification identity artifact exactly like an
    // ordinary Manifest Run. Recording nothing for it would silently strand
    // AG-16's continuation eligibility path (which reads this exact ledger
    // entry) and this crash-recovery pass's own reconcileMissingEntries()
    // sweep below, so it is admitted here rather than duplicating this
    // entire method for one additional workflowId.
    if (run.workflowId != kExecutionManifestWorkflowId
        && run.workflowId != kManifestVerificationContinuationWorkflowId) {
        return common::Result<void>::ok();
    }
    if (!isTerminalManifestStatus(run.status)) return common::Result<void>::ok();

    // Called only after the Run's terminal status is durably committed (see
    // this class's own header doc comment) - every repository read below is
    // therefore correctly timed, and its own failure propagates as this
    // call's failure rather than being silently treated as "this Run has
    // none of that" - the AG-14.1 fail-closed fix.

    const auto foundManifest = manifests_.findForRun(runId);
    if (!foundManifest.success()) return failVisible(*foundManifest.error());
    const std::optional<ExecutionManifest> manifest = foundManifest.value();

    std::unordered_map<std::string, ManifestTaskState> taskStates;
    {
        const auto found = taskExecutions_.listForRun(runId);
        if (!found.success()) return failVisible(*found.error());
        for (const auto& record : found.value()) taskStates[record.taskId] = record.state;
    }

    std::unordered_map<std::string, ManifestWorkerNoChanges> taskNoChanges;
    {
        const auto found = taskExecutions_.listNoChangesForRun(runId);
        if (!found.success()) return failVisible(*found.error());
        for (const auto& record : found.value()) taskNoChanges[record.taskId] = record.evidence;
    }

    const auto proposalsFound = proposals_.listForRun(runId);
    if (!proposalsFound.success()) return failVisible(*proposalsFound.error());

    // The port returns a change set as a HEADER; its file changes are a separate read. ONE batched read for every proposal
    // of the Run (none when there is no proposal). It fails closed like every read above: a failed read must never become
    // an entry with no paths, and the change items are never copied back into the headers.
    std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> fileChangesBySet;
    if (!proposalsFound.value().empty()) {
        std::vector<ChangeSetId> changeSetIds;
        changeSetIds.reserve(proposalsFound.value().size());
        for (const auto& changeSet : proposalsFound.value()) changeSetIds.push_back(changeSet.changeSetId);
        auto fileChangesFound = proposals_.listChangesForSets(changeSetIds);
        if (!fileChangesFound.success()) return failVisible(*fileChangesFound.error());
        fileChangesBySet = std::move(fileChangesFound.value());
    }

    // Baseline fingerprint, verification identity, and the Planner's own
    // no-changes-needed rationale are all recovered from typed JSON
    // artifacts ManifestWorkflowRunner::run() persisted while it still held
    // this data in memory (see this class's own header doc comment on the
    // run()/recordTerminalRun timing boundary). AG-14.2: every read below is
    // fail-closed via findArtifactContentOrFail - an infra failure (listing/
    // reading itself failed) or a malformed-but-present artifact both
    // propagate as this call's own failure, exactly like the repository
    // reads above; only a genuinely successful, empty lookup is absence.
    std::string baselineFingerprint;
    {
        const auto content = findArtifactContentOrFail(
            artifacts_, runId, "workspace-baseline.v1.json");
        if (!content.success()) return failVisible(*content.error());
        if (content.value().has_value()) {
            const auto parsed = WorkspaceBaselineProvenance::parseJson(*content.value());
            if (!parsed.success()) return failVisible(*parsed.error());
            baselineFingerprint = parsed.value().fingerprint;
        }
    }

    std::optional<ManifestProposalSetVerification> verification;
    {
        const auto content = findArtifactContentOrFail(
            artifacts_, runId, "manifest-verification-identity.v1.json");
        if (!content.success()) return failVisible(*content.error());
        if (content.value().has_value()) {
            auto parsed = parseManifestProposalSetVerificationJson(*content.value());
            if (!parsed.success()) return failVisible(*parsed.error());
            verification = std::move(parsed.value());
        }
    }

    std::optional<PlannerNoChangesNeeded> plannerNoChanges;
    {
        const auto content = findArtifactContentOrFail(
            artifacts_, runId, "manifest-planning-no-changes-identity.v1.json");
        if (!content.success()) return failVisible(*content.error());
        if (content.value().has_value()) {
            auto parsed = parsePlannerNoChangesNeededJson(*content.value());
            if (!parsed.success()) return failVisible(*parsed.error());
            plannerNoChanges = std::move(parsed.value());
        }
    }

    std::string planningInputFingerprint;
    {
        const auto content = findArtifactContentOrFail(
            artifacts_, runId, "manifest-planning-input-fingerprint.txt");
        if (!content.success()) return failVisible(*content.error());
        if (content.value().has_value()) {
            if (!isSha256Digest(*content.value())) {
                return failVisible({"manifest_work_ledger.corrupt_planning_identity",
                    "The stored planning input fingerprint is invalid"});
            }
            planningInputFingerprint = *content.value();
        }
    }
    std::string planningFailureSignature;
    {
        const auto content = findArtifactContentOrFail(
            artifacts_, runId, "manifest-planning-failure-signature.txt");
        if (!content.success()) return failVisible(*content.error());
        if (content.value().has_value()) {
            if (!isSha256Digest(*content.value())) {
                return failVisible({"manifest_work_ledger.corrupt_planning_identity",
                    "The stored planning failure signature is invalid"});
            }
            planningFailureSignature = *content.value();
        }
    }

    ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = run.projectId;
    entry.terminalStatus = run.status;
    entry.terminalReason = boundedCopy(run.errorMessage, kManifestWorkLedgerMaxReasonBytes);
    entry.baselineFingerprint = baselineFingerprint;
    entry.planningInputFingerprint = planningInputFingerprint;
    entry.planningFailureSignature = planningFailureSignature;
    entry.createdAt = std::chrono::system_clock::now();

    // Disposition: a frozen ExecutionManifest means the Planner produced
    // one; its genuine absence on an otherwise-Completed Run means the
    // Planner's own evidence-backed no-op path was taken (never persisted
    // itself, since a no-op deliberately creates no Manifest/task rows -
    // this is the ledger's own equivalent inference). Any other absence
    // means planning/verification failed, was cancelled, or the process was
    // interrupted before a disposition ever existed.
    if (manifest.has_value()) {
        entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
        entry.manifestSchemaVersion = manifest->schemaVersion;
    } else if (run.status == RunStatus::Completed) {
        entry.disposition = ManifestWorkLedgerDisposition::NoChangesNeeded;
    } else {
        entry.disposition = ManifestWorkLedgerDisposition::NotProduced;
    }

    // AG-14.2: defensive invariant check. ManifestWorkflowRunner::run() now
    // gates a Run's own Completed status on successfully persisting the
    // matching typed identity artifact (see its own AG-14.2 comments at the
    // manifest-executed and Planner no-op return points), so
    // ManifestReady+Completed guarantees a verification identity artifact
    // exists, and NoChangesNeeded (only ever set for Completed - see just
    // above) guarantees a no-changes identity artifact exists. Reaching
    // here with the fail-closed reads above having succeeded yet the
    // expected identity still absent means that invariant did not hold -
    // never silently write a "Completed" row the ledger cannot explain.
    if (entry.disposition == ManifestWorkLedgerDisposition::ManifestReady
        && run.status == RunStatus::Completed && !verification.has_value()) {
        const common::Error invariantError {
            "manifest_work_ledger.missing_required_identity",
            "A Completed manifest-executed Run is missing its required verification identity"};
        return failVisible(invariantError);
    }
    if (entry.disposition == ManifestWorkLedgerDisposition::NoChangesNeeded
        && !plannerNoChanges.has_value()) {
        const common::Error invariantError {
            "manifest_work_ledger.missing_required_identity",
            "A Completed no-changes-needed Run is missing its required planner identity"};
        return failVisible(invariantError);
    }

    // Task dispositions: bounded and taskId-ordered for a deterministic
    // entry regardless of dispatch/map-iteration order. Also collects a
    // bounded note of Failed/Skipped tasks for remainingWorkNote below.
    std::vector<std::string> orderedTaskIds;
    orderedTaskIds.reserve(taskStates.size());
    for (const auto& [taskId, state] : taskStates) {
        static_cast<void>(state);
        orderedTaskIds.push_back(taskId);
    }
    std::sort(orderedTaskIds.begin(), orderedTaskIds.end());
    std::vector<std::string> unfinishedTaskNotes;
    for (const auto& taskId : orderedTaskIds) {
        const auto& state = taskStates.at(taskId);
        if (entry.taskDispositions.size() < kManifestWorkLedgerMaxTaskDispositions) {
            ManifestWorkLedgerTaskDisposition disposition;
            disposition.taskId = taskId;
            disposition.status = toString(state.status);
            disposition.changeSetId = state.changeSetId;
            const auto reasonIt = taskNoChanges.find(taskId);
            if (reasonIt != taskNoChanges.end()) {
                disposition.noChangesReason = toString(reasonIt->second.reason);
            }
            entry.taskDispositions.push_back(std::move(disposition));
        }
        if (state.status == ManifestTaskStatus::Failed || state.status == ManifestTaskStatus::Skipped) {
            std::string note = taskId + ": "
                + (state.errorCode.empty() ? std::string(toString(state.status)) : state.errorCode);
            unfinishedTaskNotes.push_back(std::move(note));
        }
    }

    // Proposal ids, lineage, and affected paths: from this Run's own change
    // proposals (the paths from their file-change items), deduped/sorted/bounded
    // - never raw model output or file content. The set keeps the paths unique
    // and sorted whatever the order of the proposals, so the bound below keeps
    // the first paths in that canonical order.
    std::set<std::string> affectedPaths;
    for (const auto& changeSet : proposalsFound.value()) {
        if (entry.proposalIds.size() < kManifestWorkLedgerMaxProposalIds) {
            entry.proposalIds.push_back(changeSet.changeSetId);
            ManifestWorkLedgerProposalLineage lineage;
            lineage.changeSetId = changeSet.changeSetId;
            lineage.state = toString(changeSet.state);
            lineage.supersedesChangeSetId = changeSet.supersedesChangeSetId;
            entry.proposalLineage.push_back(std::move(lineage));
        }
        const auto items = fileChangesBySet.find(changeSet.changeSetId);
        if (items == fileChangesBySet.end()) continue;
        for (const auto& change : items->second) affectedPaths.insert(change.relativePath.generic_string());
    }
    for (const auto& path : affectedPaths) {
        if (entry.affectedRelativePaths.size() >= kManifestWorkLedgerMaxAffectedPaths) break;
        entry.affectedRelativePaths.push_back(path);
    }

    // Verification identity + summary: only ever reflects a verification
    // that actually ran. A Planner no-op or an early planning/environment
    // failure never claims "passed" - see AG-14.1.
    entry.verification = verification;
    if (entry.verification.has_value()) {
        const auto& v = *entry.verification;
        std::ostringstream summary;
        summary << "safety: " << toString(v.proposalSafetyStatus)
                << ", build/lint/test: " << toString(v.buildLintTestStatus)
                << ", acceptance: " << toString(v.acceptanceStatus);
        if (const auto* diagnostic = primaryFailureDiagnostic(v)) {
            summary << " [" << diagnostic->code << "]";
        }
        entry.verificationSummary = boundedCopy(summary.str(), kManifestWorkLedgerMaxSummaryBytes);
    } else if (plannerNoChanges.has_value()) {
        entry.verificationSummary =
            "not_run: planner reported no changes needed, combined verification never executed";
    } else if (run.status == RunStatus::Completed) {
        entry.verificationSummary = "not_run: no combined verification record for this Run";
    } else {
        entry.verificationSummary = entry.terminalReason;
    }

    // Remaining work: the Planner's own non-authoritative suggestion when
    // this Run ended in a no-op, otherwise a bounded synthesized note
    // naming Failed/Skipped tasks and their error codes.
    if (plannerNoChanges.has_value() && plannerNoChanges->recommendedNextStep.has_value()) {
        entry.remainingWorkNote = boundedCopy(
            *plannerNoChanges->recommendedNextStep, kManifestWorkLedgerMaxReasonBytes);
    } else if (!unfinishedTaskNotes.empty()) {
        std::string note = "Unfinished tasks: ";
        for (std::size_t i = 0; i < unfinishedTaskNotes.size(); ++i) {
            if (i > 0) note += "; ";
            note += unfinishedTaskNotes[i];
        }
        entry.remainingWorkNote = boundedCopy(note, kManifestWorkLedgerMaxReasonBytes);
    }

    // AG-15.3: this Run's frozen workspace scope identity + a bounded
    // deterministic summary. Fail-closed like every other read above: a
    // genuine repository error must not produce a ledger entry that's
    // silently missing scope provenance it should have had.
    if (workspaceScope_ != nullptr) {
        const auto scope = workspaceScope_->findForRun(runId);
        if (!scope.success()) {
            persistWriteFailureArtifact(artifacts_, runId, "recordTerminalRun", *scope.error());
            return common::Result<void>::failure(*scope.error());
        }
        if (scope.value().has_value()) {
            entry.workspaceScopeSchemaVersion = scope.value()->schemaVersion;
            const auto expansions = workspaceScope_->listExpansionsForRun(runId);
            if (!expansions.success()) {
                persistWriteFailureArtifact(artifacts_, runId, "recordTerminalRun", *expansions.error());
                return common::Result<void>::failure(*expansions.error());
            }
            std::size_t writeCount = 0;
            std::size_t readCount = 0;
            std::size_t referenceCount = 0;
            std::size_t excludedCount = 0;
            std::size_t deferredCount = 0;
            std::vector<std::string> deferredPaths;
            for (const auto& scopeEntry : scope.value()->entries) {
                switch (scopeEntry.role) {
                case WorkspaceScopeRole::OwnedWrite:
                case WorkspaceScopeRole::PlannedCreate:
                    ++writeCount;
                    break;
                case WorkspaceScopeRole::RequiredRead:
                    ++readCount;
                    break;
                case WorkspaceScopeRole::ReferenceOnly:
                    ++referenceCount;
                    break;
                case WorkspaceScopeRole::Excluded:
                    ++excludedCount;
                    break;
                case WorkspaceScopeRole::Deferred:
                    ++deferredCount;
                    if (deferredPaths.size() < 16) deferredPaths.push_back(scopeEntry.relativePath);
                    break;
                }
            }
            std::ostringstream scopeSummary;
            scopeSummary << "planned write: " << writeCount << ", required read: " << readCount
                << ", reference: " << referenceCount << ", excluded: " << excludedCount
                << ", deferred: " << deferredCount
                << ", approved expansions: " << expansions.value().size();
            if (!deferredPaths.empty()) {
                scopeSummary << " [deferred: ";
                for (std::size_t i = 0; i < deferredPaths.size(); ++i) {
                    if (i > 0) scopeSummary << ", ";
                    scopeSummary << deferredPaths[i];
                }
                scopeSummary << "]";
            }
            entry.workspaceScopeSummary =
                boundedCopy(scopeSummary.str(), kManifestWorkLedgerMaxSummaryBytes);

            ManifestWorkLedgerWorkspaceSnapshot snapshot;
            snapshot.entries = scope.value()->entries;
            snapshot.expansions = expansions.value();
            snapshot.dependencyEdges = scope.value()->dependencyEdges;
            for (const auto& taskId : orderedTaskIds) {
                ManifestWorkLedgerTaskWorkspaceActivity activity;
                activity.taskId = taskId;
                activity.inspectedRelativePaths = taskStates.at(taskId).inspectedRelativePaths;
                const auto changeSetId = taskStates.at(taskId).changeSetId;
                if (changeSetId.has_value()) {
                    const auto proposal = std::find_if(
                        proposalsFound.value().begin(), proposalsFound.value().end(),
                        [&](const ProposedChangeSet& candidate) {
                            return candidate.changeSetId == *changeSetId;
                        });
                    if (proposal != proposalsFound.value().end()) {
                        // The task's created / modified paths come from the same batched items as the affected paths above:
                        // the header carries none (in production this activity was recorded empty).
                        const auto items = fileChangesBySet.find(proposal->changeSetId);
                        static const std::vector<ProposedFileChange> kNoItems;
                        for (const auto& change : items == fileChangesBySet.end() ? kNoItems : items->second) {
                            auto& paths = change.operation == FileChangeOperation::Create
                                ? activity.proposedCreatedPaths
                                : activity.proposedModifiedPaths;
                            paths.push_back(change.relativePath.generic_string());
                            if (change.operation == FileChangeOperation::Rename
                                && change.sourcePath.has_value()) {
                                activity.proposedModifiedPaths.push_back(
                                    change.sourcePath->generic_string());
                            }
                        }
                    }
                }
                const auto normalize = [](std::vector<std::string>& paths) {
                    std::sort(paths.begin(), paths.end());
                    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
                };
                normalize(activity.inspectedRelativePaths);
                normalize(activity.proposedCreatedPaths);
                normalize(activity.proposedModifiedPaths);
                snapshot.taskActivities.push_back(std::move(activity));
            }
            entry.workspaceSnapshot = std::move(snapshot);
        }
    }

    const auto inserted = ledger_.insertEntry(entry);
    if (inserted.success()) return common::Result<void>::ok();
    if (inserted.error()->code == "manifest_work_ledger.duplicate_entry") {
        // Immutable: an entry already exists for this Run (a prior
        // successful write, a concurrent call, or a reconciliation retry
        // racing the original write) - benign idempotent no-op, never a
        // silent overwrite of a possibly-better entry.
        return common::Result<void>::ok();
    }
    persistWriteFailureArtifact(artifacts_, runId, "recordTerminalRun", *inserted.error());
    return common::Result<void>::failure(*inserted.error());
}

common::Result<void> ManifestWorkLedgerService::recordReviewTransition(
    const ChangeSetId& changeSetId,
    ManifestWorkLedgerTransitionKind kind,
    std::optional<std::string> actor,
    std::optional<std::string> reason,
    std::optional<std::string> postActionWorkspaceFingerprint,
    std::optional<std::chrono::system_clock::time_point> occurredAt)
{
    const auto foundChangeSet = proposals_.findChangeSet(changeSetId);
    if (!foundChangeSet.success()) return common::Result<void>::failure(*foundChangeSet.error());
    if (!foundChangeSet.value().has_value()) return common::Result<void>::ok();
    const RunId runId = foundChangeSet.value()->runId;

    const auto foundRun = runs_.findRun(runId);
    if (!foundRun.success()) return common::Result<void>::failure(*foundRun.error());
    if (!foundRun.value().has_value()) return common::Result<void>::ok();
    if (foundRun.value()->workflowId != kExecutionManifestWorkflowId
        && foundRun.value()->workflowId != kManifestVerificationContinuationWorkflowId
        && foundRun.value()->workflowId != kManifestVerificationManualHandoffWorkflowId) {
        return common::Result<void>::ok();
    }

    ManifestWorkLedgerReviewTransition transition;
    // AG-14.2: deterministic, not a random UUID - a change set has at most
    // one Approved, one Rejected, and one Applied transition ever (state is
    // a one-way DAG), so this id makes a retry (or reconcileMissingTransitions
    // recovering the same fact after the fact) a benign idempotent no-op
    // instead of a duplicate row.
    transition.id = changeSetId + ":" + std::string(toString(kind));
    transition.runId = runId;
    transition.changeSetId = changeSetId;
    transition.kind = kind;
    transition.occurredAt = occurredAt.value_or(std::chrono::system_clock::now());
    transition.actor = std::move(actor);
    transition.reason = std::move(reason);
    transition.postActionWorkspaceFingerprint = std::move(postActionWorkspaceFingerprint);

    const auto appended = ledger_.appendTransition(transition);
    if (appended.success()) return common::Result<void>::ok();
    if (appended.error()->code == "manifest_work_ledger.duplicate_transition") {
        // Immutable/idempotent: this exact (changeSetId, kind) transition
        // already exists - benign no-op, never a duplicate row.
        return common::Result<void>::ok();
    }
    persistWriteFailureArtifact(artifacts_, runId, "recordReviewTransition", *appended.error());
    return common::Result<void>::failure(*appended.error());
}

common::Result<ManifestWorkLedgerService::ReconciliationResult>
ManifestWorkLedgerService::reconcileMissingEntries(const std::set<RunId>& deferredRunIds)
{
    ReconciliationResult result;
    const auto allRuns = runs_.listRuns();
    if (!allRuns.success()) {
        return common::Result<ReconciliationResult>::failure(*allRuns.error());
    }
    for (const auto& run : allRuns.value()) {
        if (run.workflowId != kExecutionManifestWorkflowId
            && run.workflowId != kManifestVerificationContinuationWorkflowId) continue;
        if (!isTerminalManifestStatus(run.status)) continue;
        const auto existing = ledger_.findEntry(run.id);
        if (!existing.success()) {
            result.stillMissing.push_back(run.id);
            continue;
        }
        if (existing.value().has_value()) continue; // already recorded; immutable, never re-touched.
        if (deferredRunIds.contains(run.id)) {
            // Its account is not complete yet and an entry is immutable: wait for the next pass.
            result.stillMissing.push_back(run.id);
            continue;
        }
        const auto recorded = recordTerminalRun(run.id);
        if (recorded.success()) ++result.recorded;
        else result.stillMissing.push_back(run.id);
    }
    return common::Result<ReconciliationResult>::ok(std::move(result));
}

common::Result<ManifestWorkLedgerService::ReconciliationResult>
ManifestWorkLedgerService::reconcileMissingTransitions()
{
    ReconciliationResult result;
    if (approvals_ == nullptr || reviews_ == nullptr) {
        return common::Result<ReconciliationResult>::ok(std::move(result));
    }

    const auto allRuns = runs_.listRuns();
    if (!allRuns.success()) {
        return common::Result<ReconciliationResult>::failure(*allRuns.error());
    }
    for (const auto& run : allRuns.value()) {
        if (run.workflowId != kExecutionManifestWorkflowId
            && run.workflowId != kManifestVerificationContinuationWorkflowId
            && run.workflowId != kManifestVerificationManualHandoffWorkflowId) continue;

        const auto runProposals = proposals_.listForRun(run.id);
        if (!runProposals.success()) {
            result.stillMissing.push_back(run.id);
            continue;
        }
        if (runProposals.value().empty()) continue;

        const auto existingTransitions = ledger_.listTransitionsForRun(run.id);
        if (!existingTransitions.success()) {
            result.stillMissing.push_back(run.id);
            continue;
        }
        auto hasTransition = [&](const ChangeSetId& changeSetId, ManifestWorkLedgerTransitionKind kind) {
            return std::any_of(existingTransitions.value().begin(), existingTransitions.value().end(),
                [&](const ManifestWorkLedgerReviewTransition& t) {
                    return t.changeSetId == changeSetId && t.kind == kind;
                });
        };

        bool runOk = true;
        for (const auto& changeSet : runProposals.value()) {
            const auto approval = approvals_->findApproval(changeSet.changeSetId);
            if (!approval.success()) {
                runOk = false;
                continue;
            }
            if (approval.value().has_value()) {
                const auto& record = *approval.value();
                if (!hasTransition(changeSet.changeSetId, ManifestWorkLedgerTransitionKind::Approved)) {
                    const auto recorded = recordReviewTransition(
                        changeSet.changeSetId, ManifestWorkLedgerTransitionKind::Approved,
                        record.approvedBy, std::nullopt, std::nullopt, record.approvedAt);
                    if (recorded.success()) ++result.recorded;
                    else runOk = false;
                }
                if (record.applyResult.has_value() && *record.applyResult == "applied"
                    && !hasTransition(changeSet.changeSetId, ManifestWorkLedgerTransitionKind::Applied)) {
                    const auto recorded = recordReviewTransition(
                        changeSet.changeSetId, ManifestWorkLedgerTransitionKind::Applied,
                        std::nullopt, std::nullopt,
                        record.postApplyWorkspaceFingerprint,
                        record.appliedAt.value_or(record.approvedAt));
                    if (recorded.success()) ++result.recorded;
                    else runOk = false;
                }
            }

            const auto rejection = reviews_->findRejection(changeSet.changeSetId);
            if (!rejection.success()) {
                runOk = false;
                continue;
            }
            if (rejection.value().has_value()
                && !hasTransition(changeSet.changeSetId, ManifestWorkLedgerTransitionKind::Rejected)) {
                const auto& record = *rejection.value();
                const auto recorded = recordReviewTransition(
                    changeSet.changeSetId, ManifestWorkLedgerTransitionKind::Rejected,
                    record.rejectedBy, record.reason, std::nullopt, record.rejectedAt);
                if (recorded.success()) ++result.recorded;
                else runOk = false;
            }
        }
        if (!runOk) result.stillMissing.push_back(run.id);
    }
    return common::Result<ReconciliationResult>::ok(std::move(result));
}

common::Result<ManifestWorkLedgerService::PlannerContext>
ManifestWorkLedgerService::buildPlannerContext(
    const ProjectId& projectId, const WorkspaceBaselineSnapshot& currentBaseline,
    std::size_t maxEntries, std::size_t maxBytes) const
{
    PlannerContext context;
    const auto recent = ledger_.listRecentForProject(projectId, maxEntries);
    if (!recent.success()) {
        return common::Result<PlannerContext>::failure(*recent.error());
    }
    const bool hasHistory = !recent.value().empty();

    std::ostringstream anchorStream;
    anchorStream << "# Project Work Ledger (most recent Manifest Runs first)\n\n"
                 << "Current workspace baseline fingerprint: " << currentBaseline.fingerprint
                 << " (" << currentBaseline.files.size() << " files, "
                 << currentBaseline.totalBytes << " bytes)\n\n";
    const std::string anchor = anchorStream.str();
    if (anchor.size() > maxBytes) {
        return common::Result<PlannerContext>::ok(std::move(context));
    }
    context.renderedText = anchor;
    std::unordered_set<RunId> included;

    // A known prior baseline is authoritative input. Failure to recover it
    // is therefore an error, never an invitation to silently plan against a
    // partial history. Large path lists are incrementally bounded so the
    // current-baseline anchor and Run summaries retain budget.
    if (hasHistory && !recent.value().front().baselineFingerprint.empty()) {
        const ManifestWorkLedgerEntry& newestEntry = recent.value().front();
        if (artifacts_ == nullptr) {
            return common::Result<PlannerContext>::failure({
                "manifest_work_ledger.context_artifacts_unavailable",
                "Cannot recover the previous workspace baseline without ArtifactService"});
        }
        const auto baselineArtifactRun = resolvePlannerBaselineArtifactRun(runs_, newestEntry);
        if (!baselineArtifactRun.success()) {
            return common::Result<PlannerContext>::failure(*baselineArtifactRun.error());
        }
        const auto previousContent = findArtifactContentOrFail(
            artifacts_, baselineArtifactRun.value(), "workspace-baseline.v1.json");
        if (!previousContent.success()) {
            return common::Result<PlannerContext>::failure(*previousContent.error());
        }
        if (!previousContent.value().has_value()) {
            return common::Result<PlannerContext>::failure({
                "manifest_work_ledger.context_baseline_missing",
                "The most recent ledger entry names a baseline that cannot be recovered"});
        }
        const auto previousSnapshot =
            WorkspaceBaselineProvenance::parseJson(*previousContent.value());
        if (!previousSnapshot.success()) {
            return common::Result<PlannerContext>::failure(*previousSnapshot.error());
        }
        if (previousSnapshot.value().fingerprint
            != newestEntry.baselineFingerprint) {
            return common::Result<PlannerContext>::failure({
                "manifest_work_ledger.context_baseline_identity_mismatch",
                "The recovered baseline does not match the ledger entry fingerprint"});
        }
        const auto diff = WorkspaceBaselineProvenance::compare(
            previousSnapshot.value(), currentBaseline);
        if (!diff.success()) {
            return common::Result<PlannerContext>::failure(*diff.error());
        }

        const std::size_t diffAllowance = std::max<std::size_t>(256, maxBytes / 3);
        const std::size_t diffLimit = std::min(maxBytes, anchor.size() + diffAllowance);
        static constexpr std::string_view truncationMarker =
            "... workspace diff paths truncated to context budget ...\n";
        auto appendDiffText = [&](const std::string& text) {
            if (context.renderedText.size() + text.size() > diffLimit) return false;
            context.renderedText += text;
            return true;
        };
        const std::string summary = "Changes since the most recent Manifest Run ("
            + newestEntry.runId + "): " + diff.value().classification + "\n";
        const bool renderedPriorReference = appendDiffText(summary);
        bool truncated = false;
        auto appendPaths = [&](std::string_view title, const std::vector<std::string>& paths) {
            if (!renderedPriorReference || paths.empty() || truncated) return;
            if (!appendDiffText(std::string(title) + ":\n")) {
                truncated = true;
                return;
            }
            for (const auto& path : paths) {
                const std::string line = "- " + path + "\n";
                if (context.renderedText.size() + line.size() + truncationMarker.size()
                    > diffLimit) {
                    truncated = true;
                    return;
                }
                context.renderedText += line;
            }
        };
        appendPaths("Added", diff.value().addedPaths);
        appendPaths("Modified", diff.value().modifiedPaths);
        appendPaths("Deleted", diff.value().deletedPaths);
        if (truncated) {
            static_cast<void>(appendDiffText(std::string(truncationMarker)));
        }
        if (renderedPriorReference) {
            static_cast<void>(appendDiffText("\n"));
            included.insert(newestEntry.runId);
            context.includedRunIds.push_back(newestEntry.runId);
        }
    }

    if (!hasHistory) {
        return common::Result<PlannerContext>::ok(std::move(context));
    }

    std::string previousRejectedFingerprint;
    std::string previousRejectedSignature;
    for (const auto& entry : recent.value()) {
        const bool rejectedPlanning = entry.disposition == ManifestWorkLedgerDisposition::NotProduced
            && !entry.planningInputFingerprint.empty()
            && !entry.planningFailureSignature.empty();
        if (rejectedPlanning
            && entry.planningInputFingerprint == previousRejectedFingerprint
            && entry.planningFailureSignature == previousRejectedSignature) {
            continue;
        }
        previousRejectedFingerprint = rejectedPlanning
            ? entry.planningInputFingerprint : std::string {};
        previousRejectedSignature = rejectedPlanning
            ? entry.planningFailureSignature : std::string {};
        std::ostringstream section;
        section << "## Run " << entry.runId << " - " << toString(entry.terminalStatus) << "\n";
        section << "Disposition: " << toString(entry.disposition) << "\n";
        if (rejectedPlanning) {
            section << "Planning attempt: rejected (non-authoritative allocation history)\n"
                    << "Planning input fingerprint: " << entry.planningInputFingerprint << "\n"
                    << "Planning failure signature: " << entry.planningFailureSignature << "\n"
                    << "Consecutive unchanged repeats are collapsed.\n";
        }
        if (!entry.baselineFingerprint.empty()) {
            section << "Baseline fingerprint: " << entry.baselineFingerprint << "\n";
        }
        if (!entry.terminalReason.empty()) section << "Reason: " << entry.terminalReason << "\n";
        if (!entry.verificationSummary.empty()) {
            section << "Verification: " << entry.verificationSummary << "\n";
        }
        if (entry.verification.has_value()) {
            section << "Verification statuses: safety="
                    << toString(entry.verification->proposalSafetyStatus)
                    << ", build/lint/test=" << toString(entry.verification->buildLintTestStatus)
                    << ", acceptance=" << toString(entry.verification->acceptanceStatus) << "\n";
        }
        if (!entry.remainingWorkNote.empty()) {
            section << "Remaining work: " << entry.remainingWorkNote << "\n";
        }
        // AG-15.3: the most recent Run's frozen workspace scope summary -
        // current workspace files/baseline (rendered separately above)
        // remain authoritative; this is only a bounded provenance note about
        // what that prior Run's scope covered and left deferred, never a
        // substitute for re-checking the current filesystem.
        if (!entry.workspaceScopeSummary.empty()) {
            section << "Workspace scope: " << entry.workspaceScopeSummary << "\n";
        }
        if (entry.workspaceSnapshot.has_value()) {
            const auto& snapshot = *entry.workspaceSnapshot;
            section << "Workspace dependency edges: "
                    << snapshot.dependencyEdges.size() << "\n";
            for (const auto& activity : snapshot.taskActivities) {
                section << "- workspace " << activity.taskId
                        << ": inspected=";
                for (std::size_t i = 0; i < activity.inspectedRelativePaths.size(); ++i) {
                    if (i != 0) section << ",";
                    section << activity.inspectedRelativePaths[i];
                }
                section << "; proposed_create=";
                for (std::size_t i = 0; i < activity.proposedCreatedPaths.size(); ++i) {
                    if (i != 0) section << ",";
                    section << activity.proposedCreatedPaths[i];
                }
                section << "; proposed_modify=";
                for (std::size_t i = 0; i < activity.proposedModifiedPaths.size(); ++i) {
                    if (i != 0) section << ",";
                    section << activity.proposedModifiedPaths[i];
                }
                section << "\n";
            }
        }
        if (!entry.taskDispositions.empty()) {
            section << "Tasks:\n";
            for (const auto& task : entry.taskDispositions) {
                section << "- " << task.taskId << ": " << task.status;
                if (task.changeSetId.has_value()) section << " [proposal " << *task.changeSetId << "]";
                if (task.noChangesReason.has_value()) {
                    section << " [no changes: " << *task.noChangesReason << "]";
                }
                section << "\n";
            }
        }
        if (!entry.proposalLineage.empty()) {
            section << "Proposal lineage:\n";
            for (const auto& proposal : entry.proposalLineage) {
                section << "- " << proposal.changeSetId << ": " << proposal.state;
                if (proposal.supersedesChangeSetId.has_value()) {
                    section << " [supersedes " << *proposal.supersedesChangeSetId << "]";
                }
                section << "\n";
            }
        }
        if (!entry.affectedRelativePaths.empty()) {
            section << "Affected paths:\n";
            for (const auto& path : entry.affectedRelativePaths) section << "- " << path << "\n";
        }
        // Do not make an unrelated, already-out-of-budget older entry's
        // transition repository availability a prerequisite for this Run.
        if (context.renderedText.size() + section.str().size() + 1 > maxBytes) break;
        const auto transitions = ledger_.listTransitionsForRun(entry.runId);
        if (!transitions.success()) {
            return common::Result<PlannerContext>::failure(*transitions.error());
        }
        if (!transitions.value().empty()) {
            section << "Review/apply history:\n";
            for (const auto& transition : transitions.value()) {
                section << "- " << transition.changeSetId << ": " << toString(transition.kind);
                if (transition.reason.has_value()) section << " (" << *transition.reason << ")";
                if (transition.postActionWorkspaceFingerprint.has_value()) {
                    section << " [post-action workspace "
                            << *transition.postActionWorkspaceFingerprint << "]";
                }
                section << "\n";
            }
        }
        section << "\n";
        const std::string rendered = section.str();
        if (context.renderedText.size() + rendered.size() > maxBytes) break;
        context.renderedText += rendered;
        if (included.insert(entry.runId).second) context.includedRunIds.push_back(entry.runId);
    }
    return common::Result<PlannerContext>::ok(std::move(context));
}

common::Result<void> ManifestWorkLedgerService::authorizePlanningAttempt(
    const ProjectId& projectId,
    std::string_view planningInputFingerprint,
    bool explicitlyConfirmedPaidRetry) const
{
    if (!isSha256Digest(planningInputFingerprint)) {
        return common::Result<void>::failure({
            "planner.invalid_planning_input_fingerprint",
            "The Backend planning input fingerprint is invalid"});
    }
    if (explicitlyConfirmedPaidRetry) return common::Result<void>::ok();

    const auto recent = ledger_.listRecentForProject(projectId, 64);
    if (!recent.success()) return common::Result<void>::failure(*recent.error());
    const auto duplicate = std::find_if(recent.value().begin(), recent.value().end(),
        [&](const ManifestWorkLedgerEntry& entry) {
            return entry.disposition == ManifestWorkLedgerDisposition::NotProduced
                && entry.planningInputFingerprint == planningInputFingerprint
                && isSha256Digest(entry.planningFailureSignature);
        });
    if (duplicate != recent.value().end()) {
        return common::Result<void>::failure({
            "planner.unchanged_failed_input",
            "An unchanged planning attempt already failed; use an explicit Retry or Edit and rerun to authorize another paid provider attempt"});
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
