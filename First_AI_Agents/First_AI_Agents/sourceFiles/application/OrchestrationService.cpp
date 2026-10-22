#include "ai/application/OrchestrationService.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/LegacyRosterAdapter.hpp"
#include "ai/application/RunStageSettlement.hpp"

#include <algorithm>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

constexpr const char* kLegacyReadOnlyAnalysisWorkflowId = kRetiredReadOnlyAnalysisWorkflowId;

common::Result<Run> findTerminalRun(
    const IRunRepository& repository,
    const RunId& runId)
{
    const auto found = repository.findRun(runId);
    if (!found.success())
        return common::Result<Run>::failure(*found.error());
    if (!found.value().has_value())
        return common::Result<Run>::failure(error("run.not_found", "Run not found"));
    if (!isTerminal(found.value()->status)) {
        return common::Result<Run>::failure(
            error("run.not_finished", "Only a finished run can be rerun"));
    }
    return common::Result<Run>::ok(*found.value());
}

std::vector<StageModelAssignment> migrateLegacyReadOnlyAssignments(const Run& original)
{
    // AG-17.7A: the single definition lives with the legacy roster adapter.
    return LegacyRosterAdapter::migrateRetiredReadOnlyAssignments(original.assignmentSnapshot);
}

// The two workflows whose finished Runs have a legacy roster the adapter can classify.
bool isLegacyRosterSource(std::string_view workflowId)
{
    return workflowId == kExecutionProfileWorkflowId || workflowId == kRetiredReadOnlyAnalysisWorkflowId;
}

common::Error notConvertible(const LegacyRosterConversionPlan& plan)
{
    return error(plan.disposition == LegacyRosterDisposition::StayLegacy
            ? kLegacyConversionStaysLegacyCode : kLegacyConversionNotConvertibleCode,
        plan.reasonMessage + " (" + plan.reasonCode + ")");
}

common::Error confirmationRequired(const LegacyRosterConversionPlan& plan)
{
    return error(kLegacyConversionConfirmationRequiredCode,
        "Converting this legacy roster to a Manifest Run does not honour " + std::to_string(plan.dropped.size())
            + " of its settings; review the conversion with Edit & rerun and confirm it there");
}

} // namespace

OrchestrationService::OrchestrationService(
    IProjectRepository& projectRepository,
    IRunRepository& runRepository,
    RunManager& runManager)
    : projectRepository_(projectRepository)
    , runRepository_(runRepository)
    , runManager_(runManager)
{
}

common::Result<RunId> OrchestrationService::startRun(const StartRunRequest& request)
{
    // The generic start is a FRESH Run: lineage is assigned only by the commands that own it
    // (retryRun, startEditedRerun, startConvertedRerun, the continuation services). Accepting a
    // caller-supplied parent here would let a Manifest child of a legacy Run skip the
    // conversion audit.
    if (request.parentRunId.has_value()) {
        return common::Result<RunId>::failure(error(kRunParentNotAllowedCode,
            "A new Run cannot name a parent Run; use Retry, Edit & rerun or a conversion"));
    }
    // Fail synchronously for an invalid project; accepted work is then owned by
    // RunManager and immediately receives a persistent Queued state.
    const auto project = projectRepository_.findById(request.projectId);
    if (!project.success()) {
        return common::Result<RunId>::failure(*project.error());
    }
    if (!project.value().has_value()) {
        return common::Result<RunId>::failure(error("project.not_found", "Project not found"));
    }
    return runManager_.enqueue(request);
}

common::Result<void> OrchestrationService::cancelRun(const RunId& runId)
{
    return runManager_.requestCancellation(runId);
}

common::Result<RunId> OrchestrationService::retryRun(const RunId& runId)
{
    // A retry is a new aggregate linked to the immutable original run.
    const auto originalResult = findTerminalRun(runRepository_, runId);
    if (!originalResult.success())
        return common::Result<RunId>::failure(*originalResult.error());
    const Run& original = originalResult.value();

    // AG-17.7C: a legacy LONE WORKSPACE LEAD cannot execute on the legacy runner at all (it fails
    // closed with run.workspace_ownership_missing), so Retry never sends it there. It converts
    // to a single-assignment Manifest Run - automatically only when the conversion drops
    // nothing; otherwise the user must confirm it through Edit & rerun. Every other shape falls
    // through to the historical behaviour below, unchanged.
    if (isLegacyRosterSource(original.workflowId)) {
        const auto conversion = LegacyRosterAdapter::buildConversionPlan(
            original.workflowId, original.assignmentSnapshot);
        if (conversion.shape == LegacyRosterShape::LoneWorkspaceLead) {
            if (!conversion.converts()) {
                return common::Result<RunId>::failure(notConvertible(conversion));
            }
            if (!LegacyRosterAdapter::isSafeAutomaticRetry(conversion)) {
                return common::Result<RunId>::failure(confirmationRequired(conversion));
            }
            const auto draft = makeLegacyConversionDraft(
                original.id, conversion, LegacyConversionConfirmationMode::NotRequiredSafeRetry);
            if (!draft.success()) return common::Result<RunId>::failure(*draft.error());

            StartRunRequest converted;
            converted.projectId = original.projectId;
            converted.title = original.title;
            converted.task = original.task;
            converted.workflowId = conversion.targetWorkflowId;
            converted.parentRunId = original.id;
            converted.maxParallelism = original.maxParallelism;
            converted.workspacePath = original.workspacePath;
            converted.planningCompletionPolicy = original.planningCompletionPolicy;
            converted.verificationRepairPolicy = original.verificationRepairPolicy;
            converted.interfaceContracts = original.interfaceContracts;

            // The immutable source snapshot, converted - never a re-resolved profile.
            ResolvedExecutionPlan convertedPlan;
            convertedPlan.workflowId = conversion.targetWorkflowId;
            convertedPlan.assignments = conversion.roster;
            convertedPlan.maxParallelism = original.maxParallelism;
            convertedPlan.providerBudget = original.providerBudget;
            return runManager_.enqueueConvertedRetry(
                std::move(converted), std::move(convertedPlan), draft.value());
        }
    }

    StartRunRequest request;
    request.projectId = original.projectId;
    request.title = original.title;
    request.task = original.task;
    request.workflowId = original.workflowId;
    request.executionProfileId = original.executionProfileId;
    request.parentRunId = original.id;
    request.maxParallelism = original.maxParallelism;
    request.workspacePath = original.workspacePath;
    request.planningCompletionPolicy = original.planningCompletionPolicy;
    request.verificationRepairPolicy = original.verificationRepairPolicy;
    request.interfaceContracts = original.interfaceContracts;

    // Reuse the original run's own resolved plan verbatim instead of
    // resolving fresh: a retry must execute against exactly the
    // provider/model/timeout snapshot the original run captured, even if
    // the referenced execution profile (or the workflow's default profile)
    // has since been edited or repointed.
    ResolvedExecutionPlan plan;
    plan.workflowId = original.workflowId;
    plan.executionProfileId = original.executionProfileId;
    plan.assignments = original.assignmentSnapshot;
    plan.maxParallelism = original.maxParallelism;
    plan.providerBudget = original.providerBudget;

    // Historical read-only-analysis rows remain retryable after that dedicated
    // runner was retired: adapt their immutable provider/model snapshot to the
    // unified workspace-enabled execution runner instead of returning
    // workflow.unsupported.
    if (original.workflowId == kLegacyReadOnlyAnalysisWorkflowId) {
        plan.workflowId = kExecutionProfileWorkflowId;
        plan.executionProfileId.reset();
        plan.assignments = migrateLegacyReadOnlyAssignments(original);
        if (plan.assignments.empty()) {
            return common::Result<RunId>::failure(error(
                "run.legacy_snapshot_unavailable",
                "Legacy analysis run has no provider/model snapshot to retry"));
        }
        request.workflowId = plan.workflowId;
        request.executionProfileId.reset();
    }
    return runManager_.enqueueWithPlan(
        std::move(request), std::move(plan), original.id);
}

common::Result<RerunDraft> OrchestrationService::prepareRerunDraft(
    const RunId& runId) const
{
    const auto originalResult = findTerminalRun(runRepository_, runId);
    if (!originalResult.success())
        return common::Result<RerunDraft>::failure(*originalResult.error());
    const Run& original = originalResult.value();

    RerunDraft draft;
    draft.sourceRunId = original.id;
    draft.request.projectId = original.projectId;
    draft.request.title = original.title;
    draft.request.task = original.task;
    draft.request.workflowId = original.workflowId;
    draft.request.maxParallelism = original.maxParallelism;
    draft.request.workspacePath = original.workspacePath;
    draft.request.planningCompletionPolicy = original.planningCompletionPolicy;
    draft.request.verificationRepairPolicy = original.verificationRepairPolicy;
    draft.request.interfaceContracts = original.interfaceContracts;

    if (original.workflowId == kLegacyReadOnlyAnalysisWorkflowId) {
        draft.launchMode = RunLaunchMode::CustomRoster;
        draft.migratedFromLegacyWorkflow = true;
        draft.request.workflowId = kExecutionProfileWorkflowId;
        draft.request.assignmentOverrides = migrateLegacyReadOnlyAssignments(original);
        if (draft.request.assignmentOverrides.empty()) {
            return common::Result<RerunDraft>::failure(error(
                "run.legacy_snapshot_unavailable",
                "Legacy analysis run has no provider/model snapshot to edit"));
        }
    } else if (original.workflowId == kMockWorkflowId) {
        draft.launchMode = RunLaunchMode::Mock;
    } else if (original.executionProfileId.has_value()) {
        // Preserve the launch mode and stable profile identity. Exact Retry
        // remains the command that reuses the old assignment snapshot; Edit &
        // rerun intentionally lets the selected saved profile resolve anew.
        draft.launchMode = RunLaunchMode::SavedExecutionProfile;
        draft.request.executionProfileId = original.executionProfileId;
    } else {
        draft.launchMode = RunLaunchMode::CustomRoster;
        // A per-Run roster that executed through the Manifest workflow reopens as
        // a Manifest roster; every other custom roster keeps the legacy runner.
        draft.request.workflowId = original.workflowId == kExecutionManifestWorkflowId
            ? kExecutionManifestWorkflowId : kExecutionProfileWorkflowId;
        draft.request.assignmentOverrides = original.assignmentSnapshot;
        // The limits somebody chose for the source Run come back as explicit values; the Backend
        // defaults (and an automatic turn limit) come back as "no explicit limit".
        draft.request.providerBudget = providerBudgetOverridesFromPolicy(original.providerBudget);
    }

    return common::Result<RerunDraft>::ok(std::move(draft));
}

common::Result<RunId> OrchestrationService::startEditedRerun(
    const RunId& sourceRunId,
    StartRunRequest request)
{
    // Validate the source at command time and assign lineage inside Backend;
    // parentRunId received from a UI DTO is never trusted.
    const auto original = findTerminalRun(runRepository_, sourceRunId);
    if (!original.success())
        return common::Result<RunId>::failure(*original.error());

    // AG-17.7C: the generic Edit & rerun is NOT a way around the conversion rules.
    if (isLegacyRosterSource(original.value().workflowId)) {
        const auto conversion = LegacyRosterAdapter::buildConversionPlan(
            original.value().workflowId, original.value().assignmentSnapshot);
        // Switching a legacy roster Run to the Manifest workflow IS a conversion: it must go
        // through startConvertedRerun, where the user confirms what is not honoured.
        if (request.workflowId == kExecutionManifestWorkflowId) {
            return common::Result<RunId>::failure(conversion.converts()
                ? confirmationRequired(conversion) : notConvertible(conversion));
        }
        // A lone workspace lead cannot run on the legacy runner: refuse to create a Run that is
        // known to fail closed. (Editing it into a real roster is still allowed.)
        if (conversion.shape == LegacyRosterShape::LoneWorkspaceLead
            && !request.assignmentOverrides.empty()
            && LegacyRosterAdapter::buildConversionPlan(
                   kExecutionProfileWorkflowId, request.assignmentOverrides).shape
                == LegacyRosterShape::LoneWorkspaceLead) {
            return common::Result<RunId>::failure(conversion.converts()
                ? confirmationRequired(conversion) : notConvertible(conversion));
        }
    }

    request.parentRunId = original.value().id;
    return runManager_.enqueueEditedRerun(
        std::move(request), original.value().id);
}

common::Result<LegacyConversionPreview> OrchestrationService::previewLegacyConversion(
    const RunId& sourceRunId) const
{
    const auto original = findTerminalRun(runRepository_, sourceRunId);
    if (!original.success())
        return common::Result<LegacyConversionPreview>::failure(*original.error());
    LegacyConversionPreview preview;
    preview.sourceRunId = original.value().id;
    preview.plan = LegacyRosterAdapter::buildConversionPlan(
        original.value().workflowId, original.value().assignmentSnapshot);
    preview.confirmationFingerprint =
        LegacyRosterAdapter::confirmationFingerprint(original.value().id, preview.plan);
    preview.safeAutomaticRetry = LegacyRosterAdapter::isSafeAutomaticRetry(preview.plan);
    preview.legacyRerunAvailable = preview.plan.shape != LegacyRosterShape::LoneWorkspaceLead;
    preview.defaults.title = original.value().title;
    preview.defaults.task = original.value().task;
    preview.defaults.workspacePath = original.value().workspacePath;
    preview.defaults.maxParallelism = original.value().maxParallelism;
    preview.defaults.planningCompletionPolicy = original.value().planningCompletionPolicy;
    preview.defaults.verificationRepairPolicy = original.value().verificationRepairPolicy;
    return common::Result<LegacyConversionPreview>::ok(std::move(preview));
}

common::Result<RunId> OrchestrationService::startConvertedRerun(
    const RunId& sourceRunId,
    ConvertedRerunOptions options,
    const std::string& expectedConfirmationFingerprint)
{
    const auto original = findTerminalRun(runRepository_, sourceRunId);
    if (!original.success())
        return common::Result<RunId>::failure(*original.error());
    // The plan is rebuilt from the immutable source Run here, in the Backend: nothing the caller
    // sends (no roster, no workflow, no "confirmed" flag) is trusted.
    const auto plan = LegacyRosterAdapter::buildConversionPlan(
        original.value().workflowId, original.value().assignmentSnapshot);
    if (!plan.converts()) {
        return common::Result<RunId>::failure(notConvertible(plan));
    }
    if (expectedConfirmationFingerprint.empty()) {
        return common::Result<RunId>::failure(confirmationRequired(plan));
    }
    if (expectedConfirmationFingerprint
        != LegacyRosterAdapter::confirmationFingerprint(original.value().id, plan)) {
        return common::Result<RunId>::failure(error(kLegacyConversionConfirmationStaleCode,
            "The conversion changed since it was shown; review it again before confirming"));
    }
    const auto draft = makeLegacyConversionDraft(
        original.value().id, plan, LegacyConversionConfirmationMode::ExplicitUser);
    if (!draft.success()) return common::Result<RunId>::failure(*draft.error());

    StartRunRequest request;
    request.projectId = original.value().projectId;
    request.title = std::move(options.title);
    request.task = std::move(options.task);
    request.workflowId = plan.targetWorkflowId;
    request.assignmentOverrides = plan.roster;
    request.parentRunId = original.value().id;
    request.maxParallelism = options.maxParallelism;
    request.workspacePath = std::move(options.workspacePath);
    request.planningCompletionPolicy = options.planningCompletionPolicy;
    request.verificationRepairPolicy = options.verificationRepairPolicy;
    return runManager_.enqueueConvertedRerun(std::move(request), draft.value());
}

common::Result<std::optional<LegacyConversionRecord>> OrchestrationService::findLegacyConversion(
    const RunId& runId) const
{
    return runRepository_.findLegacyConversion(runId);
}

common::Result<std::optional<LegacyConversionView>> OrchestrationService::findLegacyConversionView(
    const RunId& runId) const
{
    using Result = common::Result<std::optional<LegacyConversionView>>;
    const auto record = runRepository_.findLegacyConversion(runId);
    if (!record.success()) return Result::failure(*record.error());
    if (!record.value().has_value()) return Result::ok(std::nullopt);

    // A failed read of the source is a FAULT of this view. Only a successful read that finds
    // nothing means the source is gone (the audit deliberately outlives it).
    const auto source = runRepository_.findRun(record.value()->sourceRunId);
    if (!source.success()) return Result::failure(*source.error());

    LegacyConversionView view;
    view.record = *record.value();
    view.source = source.value();
    return Result::ok(std::move(view));
}

common::Result<std::vector<LegacyConversionIndexEntry>>
OrchestrationService::listLegacyConversionIndex() const
{
    return runRepository_.listLegacyConversionIndex();
}

common::Result<std::vector<Run>> OrchestrationService::listRuns(
    const std::optional<ProjectId>& projectId) const
{
    return runRepository_.listRuns(projectId);
}

common::Result<std::optional<Run>> OrchestrationService::findRun(const RunId& runId) const
{
    return runRepository_.findRun(runId);
}

common::Result<std::vector<RunStage>> OrchestrationService::listStages(const RunId& runId) const
{
    return runRepository_.listStages(runId);
}

common::Result<void> OrchestrationService::recoverInterruptedRuns()
{
    // In-memory recovery proves the state transition. SQLite will call this at
    // startup after loading rows left Queued/Running/Cancelling by a prior process.
    const auto unfinished = runRepository_.listUnfinishedRuns();
    if (!unfinished.success()) {
        return common::Result<void>::failure(*unfinished.error());
    }
    // ONE instant for everything this recovery settles - every stage and the Run itself - so the
    // timeline of a recovered Run reads as one event, and the message is the stable constant.
    const auto recoveredAt = std::chrono::system_clock::now();
    for (Run run : unfinished.value()) {
        // AG-17.7D-1: the STAGES are settled FIRST, then the Run. A stage row is driven only by
        // events the runner emits; a crash emits none, so without this a terminal Interrupted Run
        // would keep a stage that reads "running" (or "pending") forever. The order is what makes
        // an interruption of the recovery itself safe: a Run is still unfinished until its last
        // write, so the next startup finds it again and repeats an idempotent settlement.
        //   Running -> Failed  : the execution was LOST (not cancelled by anyone), so Failed;
        //   Pending -> Skipped : the stage never started;
        //   every already-terminal stage is left exactly as it is.
        const auto settled = settleLiveStages(
            runRepository_, run.id, StageStatus::Failed, recoveredAt, kInterruptedRunRecoveryMessage);
        if (!settled.success()) {
            return common::Result<void>::failure(*settled.error());
        }

        run.status = RunStatus::Interrupted;
        run.finishedAt = recoveredAt;
        run.errorMessage = kInterruptedRunRecoveryMessage;
        const auto updated = runRepository_.updateRun(std::move(run));
        if (!updated.success()) {
            return common::Result<void>::failure(*updated.error());
        }
        // The Run's Work Ledger entry is NOT written here. A recovered Manifest Run's executions
        // are still unfinished at this point and its interrupted tasks are not yet recorded; the
        // ledger entry is immutable, so writing it now would freeze an incomplete account. It is
        // written by the startup sequence's ledger pass, after those two steps (see
        // ai::backend::runStartupRecovery) - and found again by that pass if this process dies
        // first, because a terminal Manifest Run without an entry is exactly what it looks for.
    }
    return common::Result<void>::ok();
}

void OrchestrationService::setEventHandler(RunEventHandler handler)
{
    runManager_.setEventHandler(std::move(handler));
}

} // namespace ai::application
