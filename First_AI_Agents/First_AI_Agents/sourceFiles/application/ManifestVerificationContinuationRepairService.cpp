#include "ai/application/ManifestVerificationContinuationRepairService.hpp"

#include "ai/application/planning/IOutcomeBriefTrigger.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/ExecutionManifestExecutionCapacity.hpp"
#include "ai/application/verification/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/verification/ManifestProposalOverlay.hpp"
#include "ai/application/ManifestVerificationRepairability.hpp"
#include "ai/application/TaskRouteResolver.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

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

struct OverlayCleanup {
    const ManifestProposalOverlayDescriptor* descriptor {nullptr};
    ~OverlayCleanup()
    {
        if (descriptor != nullptr) {
            static_cast<void>(ManifestProposalOverlay::cleanup(*descriptor));
        }
    }
};

} // namespace

ManifestVerificationContinuationRepairService::ManifestVerificationContinuationRepairService(
    IRunRepository& runs, IProjectRepository& projects, ExecutionManifestService& manifests,
    IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationContinuationRepository& continuations,
    IManifestTaskExecutionRepository& taskExecutions, IProviderRepository& providers,
    IModelRepository& models, ChangeProposalService& proposals,
    ManifestWorkLedgerService& workLedgerService, ArtifactService& artifacts,
    providers::IProviderAgentSessionFactory& sessionFactory,
    AgentToolExecutionService& toolExecution, IManifestVerificationCommandRunner& commandRunner,
    ModelExecutionService* modelExecutionService, std::filesystem::path trustedScratchRoot)
    : runs_(runs), projects_(projects), manifests_(manifests), workLedger_(workLedger),
      continuations_(continuations), taskExecutions_(taskExecutions), providers_(providers),
      models_(models), proposals_(proposals), workLedgerService_(workLedgerService),
      artifacts_(artifacts), commandRunner_(commandRunner),
      trustedScratchRoot_(trustedScratchRoot.empty()
          ? std::filesystem::temp_directory_path() / "aio-v"
          : std::move(trustedScratchRoot)),
      executor_(sessionFactory, toolExecution, proposals, modelExecutionService)
{
}

common::Result<void> ManifestVerificationContinuationRepairService::finishChildRun(
    const RunId& childRunId, RunStatus status, std::string errorMessage)
{
    const auto found = runs_.findRun(childRunId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return fail<void>("verification_continuation.repair_child_missing",
            "The continuation child Run disappeared before its terminal status could be recorded");
    }
    Run run = *found.value();
    const auto finishedAt = std::chrono::system_clock::now();
    const auto stages = runs_.listStages(childRunId);
    if (!stages.success()) return common::Result<void>::failure(*stages.error());
    const auto stageIt = std::find_if(stages.value().begin(), stages.value().end(),
        [&](const RunStage& stage) { return stage.id == childRunId + ":continuation"; });
    if (stageIt == stages.value().end()) {
        return fail<void>("verification_continuation.repair_stage_missing",
            "The continuation child Run's durable stage is missing");
    }
    RunStage stage = *stageIt;
    stage.status = status == RunStatus::Completed ? StageStatus::Completed
        : status == RunStatus::Cancelled ? StageStatus::Cancelled
                                         : StageStatus::Failed;
    stage.progress = status == RunStatus::Completed ? 1.0 : stage.progress;
    stage.errorMessage = errorMessage;
    stage.finishedAt = finishedAt;
    const auto updatedStage = runs_.updateStage(std::move(stage));
    if (!updatedStage.success()) return common::Result<void>::failure(*updatedStage.error());

    run.status = status;
    run.errorMessage = std::move(errorMessage);
    run.finishedAt = finishedAt;
    run.currentStageId.reset();
    if (status == RunStatus::Completed) run.progress = 1.0;
    const auto updated = runs_.updateRun(std::move(run));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    const auto recorded = workLedgerService_.recordTerminalRun(childRunId);
    // PC-4C: the paid child bypasses RunManager::finishRun, so it nudges the Outcome Brief itself - also when its ledger write failed
    // (the brief then says so: source_ledger_missing). Best effort.
    if (outcomeBriefTrigger_ != nullptr) {
        outcomeBriefTrigger_->runMayHaveChanged(childRunId);
    }
    if (!recorded.success()) return common::Result<void>::failure(*recorded.error());
    return common::Result<void>::ok();
}

common::Result<ManifestVerificationContinuationRepairOutcome>
ManifestVerificationContinuationRepairService::repair(
    const RunId& childRunId, std::stop_token stopToken)
{
    const auto child = runs_.findRun(childRunId);
    if (!child.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        child.error()->code, child.error()->message);
    if (!child.value().has_value()) return fail<ManifestVerificationContinuationRepairOutcome>(
        "verification_continuation.repair_child_missing", "The continuation child Run does not exist");
    const Run childRun = *child.value();
    if (childRun.workflowId != kManifestVerificationContinuationWorkflowId
            || !childRun.parentRunId.has_value()) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_not_a_continuation",
            "This Run is not a verification continuation child");
    }
    if (childRun.status != RunStatus::Prepared) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_not_prepared",
            "The continuation child Run is not in the Prepared state");
    }
    if (stopToken.stop_requested()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Cancelled,
            "Verification continuation repair cancelled"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_cancelled",
            "Verification continuation repair was cancelled");
    }
    const RunId sourceRunId = *childRun.parentRunId;

    const auto inputResult = continuations_.findInputBySourceRun(sourceRunId);
    if (!inputResult.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        inputResult.error()->code, inputResult.error()->message);
    if (!inputResult.value().has_value() || inputResult.value()->childRunId != childRunId) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_input_missing",
            "No frozen continuation input names this exact child Run");
    }
    const ManifestVerificationContinuationInput input = *inputResult.value();
    const auto validInput = validateManifestVerificationContinuationInput(input);
    if (!validInput.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        validInput.error()->code, validInput.error()->message);

    const auto attributionResult = continuations_.findAttributionBySourceRun(sourceRunId);
    if (!attributionResult.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        attributionResult.error()->code, attributionResult.error()->message);
    if (!attributionResult.value().has_value()
            || attributionResult.value()->id != input.attributionId
            || attributionResult.value()->status != ManifestVerificationAttributionStatus::Attributed) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_attribution_missing",
            "No attributed diagnostic checkpoint matches this continuation input");
    }
    const ManifestVerificationAttributionCheckpoint attribution = *attributionResult.value();

    const auto sourceManifest = manifests_.findForRun(sourceRunId);
    if (!sourceManifest.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        sourceManifest.error()->code, sourceManifest.error()->message);
    if (!sourceManifest.value().has_value()) return fail<ManifestVerificationContinuationRepairOutcome>(
        "verification_continuation.source_corrupt", "The source Manifest no longer exists");

    const auto ledger = workLedger_.findEntry(sourceRunId);
    if (!ledger.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        ledger.error()->code, ledger.error()->message);
    if (!ledger.value().has_value() || !ledger.value()->verification.has_value()) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.source_corrupt",
            "The source Run's durable verification identity is missing");
    }
    const ManifestProposalSetVerification sourceVerification = *ledger.value()->verification;

    const auto project = projects_.findById(input.sourceProjectId);
    if (!project.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        project.error()->code, project.error()->message);
    if (!project.value().has_value()) return fail<ManifestVerificationContinuationRepairOutcome>(
        "verification_continuation.source_project_missing", "The source project does not exist");
    const auto workspaceRoot = childRun.workspacePath.value_or(project.value()->rootPath);
    // The validated root of the temporary verification copies: a root that is not usable is refused here, before the workspace is
    // captured and long before the paid repair session.
    const auto scratchRoot = resolveVerificationScratchRoot(scratchProvider_, trustedScratchRoot_, workspaceRoot);
    if (!scratchRoot.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        scratchRoot.error()->code, scratchRoot.error()->message);
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto currentBaseline = WorkspaceBaselineProvenance::capture(policy);
    if (!currentBaseline.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        currentBaseline.error()->code, currentBaseline.error()->message);
    if (currentBaseline.value().fingerprint != input.baselineFingerprint) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.baseline_drift: The workspace no longer matches the "
            "prepared continuation baseline"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.baseline_drift",
            "The workspace no longer matches the prepared continuation baseline; implicit rebase "
            "is forbidden");
    }

    for (const auto& id : input.orderedProposalIds) {
        const auto proposal = proposals_.findChangeSet(id);
        if (!proposal.success() || proposal.value().runId != sourceRunId
                || proposal.value().projectId != input.sourceProjectId
                || proposal.value().state != ChangeSetState::ReadyForReview) {
            static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
                "verification_continuation.source_proposal_unavailable: An exact source proposal "
                "is missing, changed, or no longer reviewable"));
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.source_proposal_unavailable",
                "An exact source proposal is missing, changed, or no longer reviewable");
        }
    }

    // The exact reconstructed source candidate is both (a) the read-only
    // overlay every repair worker inspects, and (b) re-proven here to match
    // the durable identity P5.3 already froze - the same defence P5.3 itself
    // performs, repeated because time has passed since that call.
    BuildManifestProposalOverlayRequest sourceOverlayRequest;
    sourceOverlayRequest.workspaceRoot = workspaceRoot;
    sourceOverlayRequest.trustedScratchRoot = scratchRoot.value();
    sourceOverlayRequest.workspaceIdentity = input.sourceWorkspaceIdentity;
    sourceOverlayRequest.baselinePolicy = policy;
    sourceOverlayRequest.baselineSnapshot = currentBaseline.value();
    sourceOverlayRequest.orderedProposalIds = input.orderedProposalIds;
    auto sourceOverlay = ManifestProposalOverlay::build(sourceOverlayRequest, proposals_);
    if (!sourceOverlay.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        sourceOverlay.error()->code, sourceOverlay.error()->message);
    OverlayCleanup sourceOverlayCleanup {&sourceOverlay.value()};
    if (sourceOverlay.value().orderedProposalContentHashes != input.orderedProposalContentHashes
            || sourceOverlay.value().overlayFingerprint != input.sourceOverlayFingerprint) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.source_candidate_mismatch: The reconstructed source "
            "proposal set no longer matches its frozen continuation input"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.source_candidate_mismatch",
            "The reconstructed source proposal set no longer matches its frozen continuation input");
    }

    // Repair scope: one task per unique diagnostic owner the attribution
    // durably named as responsible for content the candidate itself
    // introduced (SourceProposalOwner/IntegrationOwner). A
    // PreExistingVerificationDebt/IgnorableCascade finding never earns a
    // repair task on its own.
    std::vector<std::string> repairTaskIds;
    std::unordered_map<std::string, ChangeSetId> predecessorByTask;
    {
        std::unordered_set<std::string> seen;
        for (const auto& finding : attribution.findings) {
            if (finding.kind != ManifestVerificationFindingAttributionKind::SourceProposalOwner
                    && finding.kind != ManifestVerificationFindingAttributionKind::IntegrationOwner) {
                continue;
            }
            if (!finding.ownerTaskId.has_value() || !finding.ownerProposalId.has_value()) {
                return fail<ManifestVerificationContinuationRepairOutcome>(
                    "verification_continuation.repair_attribution_corrupt",
                    "A repairable attribution finding is missing its owner identity");
            }
            const auto existing = predecessorByTask.find(*finding.ownerTaskId);
            if (existing != predecessorByTask.end() && existing->second != *finding.ownerProposalId) {
                return fail<ManifestVerificationContinuationRepairOutcome>(
                    "verification_continuation.repair_attribution_corrupt",
                    "A repair owner task maps to more than one source proposal");
            }
            predecessorByTask[*finding.ownerTaskId] = *finding.ownerProposalId;
            if (seen.insert(*finding.ownerTaskId).second) {
                repairTaskIds.push_back(*finding.ownerTaskId);
            }
        }
    }
    if (repairTaskIds.empty()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.repair_scope_empty: The attributed diagnostics named no "
            "task to repair"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_scope_empty",
            "The attributed diagnostics named no task to repair");
    }

    std::unordered_map<std::string, ManifestTaskRepairContext> repairContextsByTaskId;
    ExecutionManifest repairManifest;
    repairManifest.runId = childRunId;
    repairManifest.policy = sourceManifest.value()->policy;
    repairManifest.policy.maxParallelism =
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(repairTaskIds.size()));
    repairManifest.requestedMaxParallelism = repairManifest.policy.maxParallelism;
    for (const auto& taskId : repairTaskIds) {
        const auto taskIt = std::find_if(sourceManifest.value()->tasks.begin(),
            sourceManifest.value()->tasks.end(),
            [&](const ExecutionManifestTask& t) { return t.id == taskId; });
        if (taskIt == sourceManifest.value()->tasks.end()
                || taskIt->requestedCapability != WorkspaceCapability::Propose) {
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.repair_attribution_corrupt",
                "A repair owner task does not exist or cannot propose in the source Manifest");
        }
        ExecutionManifestTask repairTask = *taskIt;
        repairTask.dependencies.clear();
        repairManifest.tasks.push_back(repairTask);

        const ChangeSetId predecessorId = predecessorByTask.at(taskId);
        ManifestTaskRepairContext context;
        context.supersedesChangeSetId = predecessorId;
        context.predecessorRunId = sourceRunId;
        context.promptPrefix = buildVerificationRepairContext(
            repairTask, sourceVerification, predecessorId, proposals_);
        repairContextsByTaskId.emplace(taskId, std::move(context));
    }
    // AG-17.0: a repair Manifest holds only the repair-owner tasks, each copied
    // with its frozen requirements - including their origin, so a
    // backend_mandatory requirement keeps its provenance. Nothing is derived
    // again (no policy evaluation, no duplicate). The source's decision record
    // is carried verbatim, field for field, only when it is fully consistent
    // with the subset's own tasks (its anchor and every requirement it names
    // are present); otherwise it is omitted rather than left dangling or
    // partially projected. The authoritative final verification of this
    // continuation runs over the SOURCE Manifest's tasks either way.
    repairManifest.mandatoryVerification = ManifestMandatoryVerificationPolicy::inheritRecord(
        sourceManifest.value()->mandatoryVerification, repairManifest);
    // AG-17.1: the repair Manifest inherits the SOURCE routing record verbatim, so a
    // repair task keeps its frozen assignee and route and nothing is re-planned or
    // re-resolved. (Each copied task keeps its own assigneeMemberId.)
    repairManifest.routing = sourceManifest.value()->routing;

    const auto frozenRepairManifest = manifests_.freeze(repairManifest, repairManifest.policy);
    if (!frozenRepairManifest.success()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.repair_manifest_invalid: " + frozenRepairManifest.error()->code
                + ": " + frozenRepairManifest.error()->message));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            frozenRepairManifest.error()->code, frozenRepairManifest.error()->message);
    }

    const auto baselineJson = WorkspaceBaselineProvenance::renderJson(currentBaseline.value());
    if (!baselineJson.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
        baselineJson.error()->code, baselineJson.error()->message);
    const std::optional<StageId> stageId = childRunId + ":continuation";
    const auto baselineArtifact = artifacts_.createArtifact(
        childRunId, stageId, ArtifactKind::Report,
        "workspace-baseline.v1.json", "application/json", baselineJson.value());
    if (!baselineArtifact.success()) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "workspace_baseline.persistence_failed", baselineArtifact.error()->message);
    }

    if (childRun.assignmentSnapshot.empty()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.repair_assignment_invalid: The continuation child Run does "
            "not carry an inherited execution assignment"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_assignment_invalid",
            "The continuation child Run does not carry an inherited execution assignment");
    }
    StageModelAssignment assignment;
    Provider provider;
    Model model;
    std::shared_ptr<const TaskRouteResolver> routeResolver;
    if (frozenRepairManifest.value().routing.has_value()) {
        auto resolved = buildManifestRunRoutingContext(
            *frozenRepairManifest.value().routing, childRun.assignmentSnapshot,
            providers_, models_, childRun.providerBudget.maxOutputTokens);
        if (!resolved.success()) {
            static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
                "verification_continuation.repair_assignment_invalid: ["
                    + resolved.error()->code + "] " + resolved.error()->message));
            return fail<ManifestVerificationContinuationRepairOutcome>(
                resolved.error()->code, resolved.error()->message);
        }
        auto context = std::move(resolved.value());
        assignment = std::move(context.leadAssignment);
        provider = std::move(context.leadProvider);
        model = std::move(context.leadModel);
        routeResolver = std::make_shared<const TaskRouteResolver>(
            std::move(context.taskRoutes));
    } else {
        // A pre-AG-17.1 source Manifest has no routing record and can only
        // carry the historical one-assignment continuation contract.
        if (childRun.assignmentSnapshot.size() != 1) {
            static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
                "verification_continuation.repair_assignment_invalid: A legacy continuation "
                "without routing must carry exactly one inherited assignment"));
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.repair_assignment_invalid",
                "A legacy continuation without routing must carry exactly one inherited assignment");
        }
        assignment = childRun.assignmentSnapshot.front();
        const auto foundProvider = providers_.findById(assignment.providerId);
        const auto foundModel = models_.findById(assignment.modelId);
        if (!foundProvider.success() || !foundProvider.value().has_value()
                || !foundModel.success() || !foundModel.value().has_value()) {
            static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
                "verification_continuation.repair_assignment_invalid: Provider or model "
                "referenced by the inherited assignment no longer exists"));
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.repair_assignment_invalid",
                "Provider or model referenced by the inherited assignment no longer exists");
        }
        provider = *foundProvider.value();
        model = *foundModel.value();
    }

    auto providerBudgetGuard = std::make_shared<ProviderSessionBudgetGuard>(
        childRun.providerBudget, childRun.providerBudgetUsage,
        [this, childRunId](const ProviderExecutionBudgetUsage& usage) {
            const auto found = runs_.findRun(childRunId);
            if (!found.success()) return common::Result<void>::failure(*found.error());
            if (!found.value().has_value()) {
                return common::Result<void>::failure({"run.not_found",
                    "Run not found while persisting provider usage"});
            }
            Run run = *found.value();
            run.providerBudgetUsage = usage;
            run.usage.inputTokens = usage.inputTokens + usage.cacheReadInputTokens;
            run.usage.outputTokens = usage.outputTokens;
            run.usage.cacheCreationInputTokens = usage.cacheCreationInputTokens;
            run.usage.cacheReadInputTokens = usage.cacheReadInputTokens;
            run.usage.cacheCreationDuration = usage.cacheCreationDuration;
            const auto updated = runs_.updateRun(std::move(run));
            if (!updated.success()) return common::Result<void>::failure(*updated.error());
            return common::Result<void>::ok();
        });

    ExecutionManifestExecutionRequest executionRequest;
    executionRequest.manifest = frozenRepairManifest.value();
    executionRequest.runId = childRunId;
    executionRequest.projectId = input.sourceProjectId;
    executionRequest.providerId = assignment.providerId;
    executionRequest.modelId = assignment.modelId;
    executionRequest.apiModelId = model.apiModelId;
    executionRequest.providerKind = provider.kind;
    executionRequest.pricingModel = model;
    executionRequest.maxOutputTokens = model.maxOutputTokens;
    // AG-17.2: the repair Manifest inherited the SOURCE routing record, so its
    // workers run on the very route that was frozen - resolved here against the
    // child Run's inherited assignment snapshot and verified against the record
    // (nothing is re-planned or re-chosen). The shared resolver also applies the
    // worker output ceiling floor (max(model max_output_tokens, 16384), capped by
    // the Run budget) that the primary workflow has used since the
    // max_output_tokens incident; the raw model value set above was a gap in this
    // path and is only the fallback for a source Manifest frozen before AG-17.1.
    executionRequest.routeResolver = std::move(routeResolver);
    executionRequest.providerBudgetGuard = providerBudgetGuard;
    executionRequest.workspaceRoot = workspaceRoot;
    executionRequest.inspectPolicy = policy;
    executionRequest.requireCombinedVerification = false;
    executionRequest.verificationRepairPolicy = childRun.verificationRepairPolicy;
    // PC-2: the child Run copied its source's frozen contracts; the repair workers receive them exactly like
    // the primary workers did, so a repair cannot drift from the shared shape it is repairing.
    executionRequest.interfaceContracts = childRun.interfaceContracts;
    executionRequest.baselineSnapshot = currentBaseline.value();
    executionRequest.verificationCommandRunner = &commandRunner_;
    executionRequest.primaryRepairContextsByTaskId = repairContextsByTaskId;
    executionRequest.primaryRepairCandidateReadRoot = sourceOverlay.value().root;
    executionRequest.onTaskFinished =
        [this, childRunId](const std::string& taskId, const ManifestTaskState& state) {
            static_cast<void>(taskExecutions_.upsert(childRunId, taskId, state));
        };

    // The child Run is still Prepared here. Apply the same post-freeze
    // authority as the primary workflow before crossing its paid-work
    // boundary, so a capacity failure cannot leave a Running projection or
    // any task attempt/provider/model/proposal side effect behind.
    const auto executionCapacity = admitManifestWorkerReservations(
        executionRequest.manifest, executionRequest.seededStates,
        providerBudgetGuard->policy(), providerBudgetGuard->usage());
    if (!executionCapacity.success()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            executionCapacity.error()->code + ": "
                + executionCapacity.error()->message));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            executionCapacity.error()->code, executionCapacity.error()->message);
    }

    // Persist the paid-work boundary before the first provider request. A
    // process crash can therefore never leave a child looking safely
    // Prepared and invite an accidental duplicate repair. Ordinary startup
    // recovery will turn this Running state into Interrupted; terminal Runs
    // are never implicitly resumed.
    {
        const auto current = runs_.findRun(childRunId);
        if (!current.success() || !current.value().has_value()
            || current.value()->status != RunStatus::Prepared) {
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.repair_not_prepared",
                "The continuation child could not enter its paid repair boundary");
        }
        Run running = *current.value();
        running.status = RunStatus::Running;
        running.startedAt = std::chrono::system_clock::now();
        running.currentStageId = childRunId + ":continuation";
        const auto updated = runs_.updateRun(std::move(running));
        if (!updated.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
            updated.error()->code, updated.error()->message);

        const auto stages = runs_.listStages(childRunId);
        if (!stages.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
            stages.error()->code, stages.error()->message);
        const auto stageIt = std::find_if(stages.value().begin(), stages.value().end(),
            [&](const RunStage& stage) { return stage.id == childRunId + ":continuation"; });
        if (stageIt == stages.value().end()) {
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.repair_stage_missing",
                "The continuation child Run's durable stage is missing");
        }
        RunStage stage = *stageIt;
        stage.status = StageStatus::Running;
        stage.startedAt = std::chrono::system_clock::now();
        const auto updatedStage = runs_.updateStage(std::move(stage));
        if (!updatedStage.success()) return fail<ManifestVerificationContinuationRepairOutcome>(
            updatedStage.error()->code, updatedStage.error()->message);
    }

    const auto outcome = executor_.execute(std::move(executionRequest), stopToken);
    for (const auto& [taskId, state] : outcome.taskStates) {
        static_cast<void>(taskExecutions_.upsert(childRunId, taskId, state));
    }
    for (const auto& [taskId, evidence] : outcome.workerNoChanges) {
        static_cast<void>(taskExecutions_.upsertNoChanges(childRunId, taskId, evidence));
    }

    if (outcome.cancelled || outcome.status == ManifestExecutionStatus::Cancelled) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Cancelled,
            "Verification continuation repair cancelled"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_cancelled",
            "Verification continuation repair was cancelled");
    }
    if (!outcome.executionErrorCode.empty()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            outcome.executionErrorCode + ": " + outcome.executionErrorMessage));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            outcome.executionErrorCode, outcome.executionErrorMessage);
    }

    std::unordered_map<std::string, ChangeSetId> successorByTask;
    for (const auto& taskId : repairTaskIds) {
        const auto stateIt = outcome.taskStates.find(taskId);
        if (stateIt == outcome.taskStates.end()
                || stateIt->second.status != ManifestTaskStatus::Completed
                || !stateIt->second.changeSetId.has_value()) {
            const std::string message = "verification_continuation.repair_task_failed: task "
                + taskId + " did not produce an accepted replacement proposal";
            static_cast<void>(finishChildRun(childRunId, RunStatus::Failed, message));
            return fail<ManifestVerificationContinuationRepairOutcome>(
                "verification_continuation.repair_task_failed", message);
        }
        successorByTask[taskId] = *stateIt->second.changeSetId;
    }

    // Final exact set: every source member in its original order, replaced
    // by its successor exactly where a repair happened - never copied,
    // never reordered.
    std::unordered_map<ChangeSetId, std::string> taskByPredecessor;
    for (const auto& [taskId, predecessorId] : predecessorByTask) {
        taskByPredecessor[predecessorId] = taskId;
    }
    std::vector<ChangeSetId> finalProposalIds;
    finalProposalIds.reserve(input.orderedProposalIds.size());
    for (const auto& sourceId : input.orderedProposalIds) {
        const auto ownerIt = taskByPredecessor.find(sourceId);
        if (ownerIt != taskByPredecessor.end()) {
            finalProposalIds.push_back(successorByTask.at(ownerIt->second));
        } else {
            finalProposalIds.push_back(sourceId);
        }
    }

    BuildManifestProposalOverlayRequest finalOverlayRequest;
    finalOverlayRequest.workspaceRoot = workspaceRoot;
    finalOverlayRequest.trustedScratchRoot = scratchRoot.value();
    finalOverlayRequest.workspaceIdentity = input.sourceWorkspaceIdentity;
    finalOverlayRequest.baselinePolicy = policy;
    finalOverlayRequest.baselineSnapshot = currentBaseline.value();
    finalOverlayRequest.orderedProposalIds = finalProposalIds;
    auto finalOverlay = ManifestProposalOverlay::build(finalOverlayRequest, proposals_);
    if (!finalOverlay.success()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.repair_unavailable: " + finalOverlay.error()->message));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            finalOverlay.error()->code, finalOverlay.error()->message);
    }
    OverlayCleanup finalOverlayCleanup {&finalOverlay.value()};

    auto baselineOverlayRequest = finalOverlayRequest;
    baselineOverlayRequest.orderedProposalIds.clear();
    auto baselineOverlay = ManifestProposalOverlay::build(baselineOverlayRequest, proposals_);
    if (!baselineOverlay.success()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.repair_unavailable: "
                + baselineOverlay.error()->message));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            baselineOverlay.error()->code, baselineOverlay.error()->message);
    }
    OverlayCleanup baselineOverlayCleanup {&baselineOverlay.value()};

    if (stopToken.stop_requested()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Cancelled,
            "Verification continuation repair cancelled"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_cancelled",
            "Verification continuation repair was cancelled");
    }
    const auto finalVerification = ManifestProposalSetVerifier::verify(
        {finalOverlay.value(), sourceManifest.value()->tasks, &baselineOverlay.value()},
        proposals_, commandRunner_, stopToken);
    if (stopToken.stop_requested()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Cancelled,
            "Verification continuation repair cancelled"));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification_continuation.repair_cancelled",
            "Verification continuation repair was cancelled");
    }
    if (!finalVerification.success()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification_continuation.repair_unavailable: " + finalVerification.error()->message));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            finalVerification.error()->code, finalVerification.error()->message);
    }

    const auto verificationJson =
        renderManifestProposalSetVerificationJson(finalVerification.value());
    const auto identityArtifact = artifacts_.createArtifact(
        childRunId, stageId, ArtifactKind::Report,
        "manifest-verification-identity.v1.json", "application/json", verificationJson);
    if (!identityArtifact.success()) {
        static_cast<void>(finishChildRun(childRunId, RunStatus::Failed,
            "verification.identity_persistence_failed: " + identityArtifact.error()->message));
        return fail<ManifestVerificationContinuationRepairOutcome>(
            "verification.identity_persistence_failed", identityArtifact.error()->message);
    }

    const bool passed = requiredAcceptancePassed(finalVerification.value());
    const RunStatus finalStatus = passed ? RunStatus::Completed : RunStatus::Failed;
    const auto finished = finishChildRun(childRunId, finalStatus,
        passed ? std::string {}
               : "verification.required_acceptance_failed: Combined candidate failed or could not "
                 "verify a required acceptance item");
    if (!finished.success()) {
        return fail<ManifestVerificationContinuationRepairOutcome>(
            finished.error()->code, finished.error()->message);
    }

    ManifestVerificationContinuationRepairOutcome result;
    result.childRunId = childRunId;
    result.finalStatus = finalStatus;
    result.repairedTaskIds = repairTaskIds;
    result.finalVerification = finalVerification.value();
    return common::Result<ManifestVerificationContinuationRepairOutcome>::ok(std::move(result));
}

} // namespace ai::application
