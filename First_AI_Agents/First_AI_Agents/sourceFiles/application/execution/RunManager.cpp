#include "ai/application/execution/RunManager.hpp"

#include "ai/application/budget/CostCalculation.hpp"
#include "ai/application/planning/IOutcomeBriefTrigger.hpp"

#include "ai/application/IdGenerator.hpp"
#include "ai/application/execution/RunStageSettlement.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

RunEvent terminalEvent(const Run& run)
{
    RunEvent event;
    event.runId = run.id;
    event.runProgress = run.progress;
    event.usage = run.usage;
    event.message = run.errorMessage;
    switch (run.status) {
    case RunStatus::Completed: event.type = RunEventType::RunCompleted; break;
    case RunStatus::Cancelled: event.type = RunEventType::RunCancelled; break;
    case RunStatus::Interrupted: event.type = RunEventType::RunInterrupted; break;
    default: event.type = RunEventType::RunFailed; break;
    }
    return event;
}

// What a Run's terminal transition does to a stage that was still running when the Run ended
// (see settleLiveStages): a cancellation and a controlled application shutdown CANCEL it, a failure
// FAILS it, and a Completed Run leaves it alone - a running stage under a Completed Run is a runner
// defect and must stay visible rather than be rewritten.
std::optional<StageStatus> runningStageOutcome(RunStatus terminal, bool applicationShutdown)
{
    switch (terminal) {
    case RunStatus::Cancelled: return StageStatus::Cancelled;
    case RunStatus::Interrupted: return applicationShutdown ? StageStatus::Cancelled : StageStatus::Failed;
    case RunStatus::Failed: return StageStatus::Failed;
    default: return std::nullopt;
    }
}

// The message a settled stage carries when its Run ended without one (a Completed Run).
constexpr const char* kStageNeverStartedMessage = "The run finished before this stage started";

} // namespace

RunManager::RunManager(
    IProjectRepository& projectRepository,
    IRunRepository& runRepository,
    IWorkflowResolver& workflowResolver,
    ProjectMemoryService* projectMemoryService,
    RunPathClaimRegistry* pathClaimRegistry,
    ModelExecutionService* modelExecutionService,
    ManifestWorkLedgerService* manifestWorkLedgerService)
    : projectRepository_(projectRepository)
    , runRepository_(runRepository)
    , workflowResolver_(workflowResolver)
    , projectMemoryService_(projectMemoryService)
    , pathClaimRegistry_(pathClaimRegistry)
    , modelExecutionService_(modelExecutionService)
    , manifestWorkLedgerService_(manifestWorkLedgerService)
    // The worker exists for the manager lifetime, sleeps on an empty queue, and
    // receives a separate stop token from the token used to cancel one run.
    , worker_([this](std::stop_token token) { workerLoop(token); })
{
}

RunManager::~RunManager()
{
    shutdown();
}

common::Result<RunId> RunManager::enqueue(StartRunRequest request)
{
    if (request.parentRunId.has_value()) {
        return common::Result<RunId>::failure(error(kRunParentNotAllowedCode,
            "A new Run cannot name a parent Run; use Retry, Edit & rerun or a conversion"));
    }
    // Resolve fresh: this is the only enqueue path that may consult "the
    // current default profile" - retries never take it (see enqueueWithPlan).
    auto plan = workflowResolver_.resolvePlan(request);
    return enqueueInternal(std::move(request), std::move(plan), std::nullopt);
}

common::Result<RunId> RunManager::enqueueWithPlan(
    StartRunRequest request,
    ResolvedExecutionPlan plan,
    const std::optional<RunId>& taskLinkSourceRunId)
{
    return enqueueInternal(
        std::move(request),
        common::Result<ResolvedExecutionPlan>::ok(std::move(plan)),
        taskLinkSourceRunId);
}

common::Result<RunId> RunManager::enqueueEditedRerun(
    StartRunRequest request,
    const RunId& taskLinkSourceRunId)
{
    auto plan = workflowResolver_.resolvePlan(request);
    return enqueueInternal(
        std::move(request), std::move(plan), taskLinkSourceRunId);
}

common::Result<RunId> RunManager::enqueueConvertedRetry(
    StartRunRequest request,
    ResolvedExecutionPlan plan,
    const LegacyConversionDraft& conversion)
{
    const RunId source = conversion.sourceRunId;
    return enqueueInternal(
        std::move(request),
        common::Result<ResolvedExecutionPlan>::ok(std::move(plan)),
        source,
        &conversion);
}

common::Result<RunId> RunManager::enqueueConvertedRerun(
    StartRunRequest request,
    const LegacyConversionDraft& conversion)
{
    auto plan = workflowResolver_.resolvePlan(request);
    const RunId source = conversion.sourceRunId;
    return enqueueInternal(std::move(request), std::move(plan), source, &conversion);
}

common::Result<RunId> RunManager::enqueueInternal(
    StartRunRequest request,
    common::Result<ResolvedExecutionPlan> plan,
    const std::optional<RunId>& taskLinkSourceRunId,
    const LegacyConversionDraft* conversion)
{
    // Command validation is completed before taking the queue mutex. This keeps
    // the critical section short and prevents invalid runs from being persisted.
    if (request.title.empty()) {
        return common::Result<RunId>::failure(error("run.empty_title", "Run title cannot be empty"));
    }
    if (request.task.empty()) {
        return common::Result<RunId>::failure(error("run.empty_task", "Run task cannot be empty"));
    }
    if (!plan.success()) {
        return common::Result<RunId>::failure(*plan.error());
    }
    // PC-2: the authoritative contracts are validated before anything is persisted. Only the Manifest
    // workflow has worker prompts to carry them, so any other workflow refuses them instead of storing a
    // contract that nothing would ever deliver.
    auto contracts = normalizeRunInterfaceContracts(std::move(request.interfaceContracts));
    if (!contracts.success()) {
        return common::Result<RunId>::failure(*contracts.error());
    }
    if (!contracts.value().empty() && plan.value().workflowId != kExecutionManifestWorkflowId) {
        return common::Result<RunId>::failure(error("run_contracts.requires_manifest_workflow",
            "Interface contracts are delivered only by the Manifest workflow; choose a roster Run on the "
            "Manifest workflow or remove the contracts"));
    }
    // The runner must exist before anything is persisted: a resolution that
    // names a workflow with no registered runner (e.g. the production
    // workflow when no real provider adapter is compiled in) must fail
    // synchronously here, never create a Run row that can then never run.
    const auto runner = workflowResolver_.runnerFor(plan.value().workflowId);
    if (!runner.success()) {
        return common::Result<RunId>::failure(*runner.error());
    }

    const auto projectResult = projectRepository_.findById(request.projectId);
    if (!projectResult.success()) {
        return common::Result<RunId>::failure(*projectResult.error());
    }
    if (!projectResult.value().has_value()) {
        return common::Result<RunId>::failure(error("project.not_found", "Project not found"));
    }
    if (projectResult.value()->status != ProjectStatus::Active) {
        return common::Result<RunId>::failure(error("project.not_active", "Project is not active"));
    }

    // Snapshot the effective workspace for every Run, including the ordinary
    // "use this Project's root" case. Project.rootPath is mutable; resolving
    // it again during Retry, proposal review, or Apply could otherwise make an
    // historical Run silently point at a different checkout.
    const std::filesystem::path requestedWorkspace = request.workspacePath.value_or(
        projectResult.value()->rootPath);
    if (requestedWorkspace.empty()) {
        return common::Result<RunId>::failure(
            error("run.empty_workspace", "Run workspace path cannot be empty"));
    }
    std::error_code pathError;
    if (!std::filesystem::exists(requestedWorkspace, pathError) || pathError) {
        return common::Result<RunId>::failure(
            error("run.workspace_not_found", "Run workspace path does not exist"));
    }
    if (!std::filesystem::is_directory(requestedWorkspace, pathError) || pathError) {
        return common::Result<RunId>::failure(
            error("run.workspace_not_directory", "Run workspace path is not a directory"));
    }
    const std::filesystem::path canonicalWorkspacePath =
        std::filesystem::weakly_canonical(requestedWorkspace, pathError);
    if (pathError) {
        return common::Result<RunId>::failure(
            error("run.workspace_unavailable", "Run workspace path cannot be resolved"));
    }

    Run run;
    // AG-18.3 hardening (blocker 4): a preselected id (only ever set by
    // VerificationBootstrapRetryService's crash-safe reservation protocol)
    // must still pass through the same insert-time uniqueness check every
    // other Run id does - it is never trusted merely because it was
    // supplied.
    run.id = request.preselectedRunId.has_value() ? *request.preselectedRunId : generateUuid();
    run.projectId = request.projectId;
    run.parentRunId = std::move(request.parentRunId);
    run.title = std::move(request.title);
    run.task = std::move(request.task);
    run.workflowId = plan.value().workflowId;
    run.executionProfileId = plan.value().executionProfileId;
    run.assignmentSnapshot = plan.value().assignments;
    run.maxParallelism = plan.value().maxParallelism;
    run.workspacePath = canonicalWorkspacePath;
    run.planningCompletionPolicy = request.planningCompletionPolicy;
    run.verificationRepairPolicy = request.verificationRepairPolicy;
    run.interfaceContracts = std::move(contracts.value());
    run.providerBudget = plan.value().providerBudget;
    run.providerBudgetUsage = {};
    run.status = RunStatus::Queued;
    run.createdAt = std::chrono::system_clock::now();

    {
        // Persist the aggregate and full Pending timeline before it becomes
        // visible to the worker or an external RunQueued observer.
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            return common::Result<RunId>::failure(error("run_manager.shutting_down", "Run manager is shutting down"));
        }
        // The repository owns this aggregate boundary: a Run is not visible
        // unless its complete Pending timeline commits in the same operation.
        const auto stages = runner.value().get().createStages(run.id, plan.value());
        // A rerun's copied links take priority over a caller-supplied
        // projectTaskId, which enqueueWithPlan/enqueueEditedRerun never set
        // anyway (see their call sites in OrchestrationService) - this order
        // just documents that precedence rather than relying on it silently.
        common::Result<Run> inserted = common::Result<Run>::failure(
            error("run.not_inserted", "The Run aggregate was not inserted"));
        if (conversion != nullptr) {
            // AG-17.7C: the conversion record is created HERE, with the new Run's id and
            // creation time, and the user's confirmation time is the Backend's own clock - it
            // is never supplied by a caller, and a safe automatic Retry never gets one.
            // The clock may step backwards (NTP); the confirmation is never earlier than the
            // Run it confirms, which the record's own integrity check requires.
            auto record = makeLegacyConversionRecord(*conversion, run.id, run.createdAt,
                legacyConversionConfirmationTime(std::chrono::system_clock::now(), run.createdAt));
            if (const auto problem = legacyConversionAggregateProblem(run, record)) {
                return common::Result<RunId>::failure(
                    error(kLegacyConversionRecordInvalidCode, *problem));
            }
            inserted = runRepository_.insertRunWithStagesAndLegacyConversion(
                run, stages, std::move(record), taskLinkSourceRunId);
        } else {
            inserted = taskLinkSourceRunId.has_value()
                ? runRepository_.insertRunWithStagesAndCopiedTaskLinks(
                    run, stages, *taskLinkSourceRunId)
                : request.projectTaskId.has_value()
                    ? runRepository_.insertRunWithStagesAndTaskLink(
                        run, stages, *request.projectTaskId)
                    : runRepository_.insertRunWithStages(run, stages);
        }
        if (!inserted.success()) {
            return common::Result<RunId>::failure(*inserted.error());
        }
        queue_.push_back(run.id);
    }

    condition_.notify_one();
    RunEvent event;
    event.type = RunEventType::RunQueued;
    event.runId = run.id;
    event.message = "Run queued";
    notify(event);
    return common::Result<RunId>::ok(std::move(run.id));
}

common::Result<void> RunManager::requestCancellation(const RunId& runId)
{
    RunEvent event;
    {
        std::lock_guard lock(mutex_);
        const auto found = runRepository_.findRun(runId);
        if (!found.success()) {
            return common::Result<void>::failure(*found.error());
        }
        if (!found.value().has_value()) {
            return common::Result<void>::failure(error("run.not_found", "Run not found"));
        }

        Run run = *found.value();
        if (isTerminal(run.status)) {
            return common::Result<void>::failure(error("run.already_finished", "Run is already finished"));
        }

        const auto queued = std::find(queue_.begin(), queue_.end(), runId);
        if (queued != queue_.end()) {
            // Queued work can become terminal immediately because no workflow
            // owns it yet and no provider cleanup is required.
            // Its stages never started: they are settled (Skipped) BEFORE the Run is terminal.
            const auto finishedAt = std::chrono::system_clock::now();
            const std::string message = "Run cancelled before execution";
            const auto settled = settleLiveStages(
                runRepository_, runId, StageStatus::Cancelled, finishedAt, message);
            if (!settled.success()) {
                return common::Result<void>::failure(*settled.error());
            }
            queue_.erase(queued);
            run.status = RunStatus::Cancelled;
            run.finishedAt = finishedAt;
            run.errorMessage = message;
            const auto updated = runRepository_.updateRun(run);
            if (!updated.success()) {
                return common::Result<void>::failure(*updated.error());
            }
            event = terminalEvent(updated.value());
        } else if (currentRunId_.has_value() && *currentRunId_ == runId) {
            // Active work first exposes Cancelling, then cooperatively observes
            // currentRunStopSource_ and finishes as Cancelled.
            if (run.status != RunStatus::Cancelling) {
                run.status = RunStatus::Cancelling;
                run.errorMessage = "Cancellation requested";
                const auto updated = runRepository_.updateRun(run);
                if (!updated.success()) {
                    return common::Result<void>::failure(*updated.error());
                }
            }
            currentRunStopSource_.request_stop();
            event.type = RunEventType::RunCancelling;
            event.runId = runId;
            event.runProgress = run.progress;
            event.message = "Cancellation requested";
        } else {
            return common::Result<void>::failure(error("run.not_active", "Run is not active"));
        }
    }

    notify(event);
    return common::Result<void>::ok();
}

void RunManager::setEventHandler(RunEventHandler handler)
{
    std::lock_guard lock(mutex_);
    eventHandler_ = std::move(handler);
}

bool RunManager::isActive(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    return (currentRunId_.has_value() && *currentRunId_ == runId)
        || std::find(queue_.begin(), queue_.end(), runId) != queue_.end();
}

void RunManager::shutdown()
{
    std::vector<RunEvent> interruptedEvents;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;

        // A process restart cannot reconstruct the in-memory queue safely, so
        // queued entries are explicitly persisted as Interrupted.
        for (const RunId& runId : queue_) {
            const auto found = runRepository_.findRun(runId);
            if (!found.success() || !found.value().has_value()) {
                continue;
            }
            Run run = *found.value();
            // Stages first (Pending -> Skipped), then the Run, one instant. A Run whose stages
            // could not be settled stays Queued and is recovered by the next startup.
            const auto finishedAt = std::chrono::system_clock::now();
            const std::string message = "Application shutdown before execution";
            if (!settleLiveStages(runRepository_, runId, StageStatus::Cancelled, finishedAt, message).success()) {
                continue;
            }
            run.status = RunStatus::Interrupted;
            run.finishedAt = finishedAt;
            run.errorMessage = message;
            const auto updated = runRepository_.updateRun(run);
            if (updated.success()) {
                interruptedEvents.push_back(terminalEvent(updated.value()));
            }
        }
        queue_.clear();
        currentRunStopSource_.request_stop();
    }

    // Stop both levels: current workflow work and the lifetime worker wait loop.
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    for (const auto& event : interruptedEvents) {
        notify(event);
    }
}

void RunManager::workerLoop(std::stop_token shutdownToken)
{
    while (!shutdownToken.stop_requested()) {
        RunId runId;
        {
            std::unique_lock lock(mutex_);
            // stop_token-aware wait guarantees shutdown does not depend on a
            // synthetic queue item or an unbounded blocking sleep.
            condition_.wait(lock, shutdownToken, [this] {
                return shuttingDown_ || !queue_.empty();
            });
            if (shutdownToken.stop_requested() || shuttingDown_) {
                break;
            }
            runId = queue_.front();
            queue_.pop_front();
            currentRunId_ = runId;
            // Every dequeued run receives a fresh cancellation domain.
            currentRunStopSource_ = std::stop_source {};
        }

        executeRun(runId);

        {
            std::lock_guard lock(mutex_);
            if (currentRunId_.has_value() && *currentRunId_ == runId) {
                currentRunId_.reset();
            }
        }
    }
}

void RunManager::executeRun(const RunId& runId)
{
    Run runningRun;
    Project project;
    std::vector<RunStage> stages;
    std::optional<WorkflowResult> preflightFailure;

    {
        // Serialize the Queued -> Running revision change against a cancellation
        // request. Repository calls are currently in-memory and non-blocking.
        std::lock_guard lock(mutex_);
        const auto runResult = runRepository_.findRun(runId);
        if (!runResult.success() || !runResult.value().has_value()) {
            return;
        }
        const auto projectResult = projectRepository_.findById(runResult.value()->projectId);
        if (!projectResult.success() || !projectResult.value().has_value()) {
            preflightFailure = WorkflowResult {false, RunStatus::Failed, "Project not found", {}};
        } else {
            const auto stagesResult = runRepository_.listStages(runId);
            if (!stagesResult.success()) {
                preflightFailure = WorkflowResult {false, RunStatus::Failed, "Run stages unavailable", {}};
            } else {
                runningRun = *runResult.value();
                runningRun.status = RunStatus::Running;
                runningRun.startedAt = std::chrono::system_clock::now();
                runningRun.errorMessage.clear();
                const auto updated = runRepository_.updateRun(runningRun);
                if (!updated.success()) {
                    return;
                }
                runningRun = updated.value();
                project = *projectResult.value();
                stages = stagesResult.value();
            }
        }
    }

    if (preflightFailure.has_value()) {
        finishRun(runId, *preflightFailure, false);
        return;
    }

    // The runner is resolved again here purely by the already-persisted,
    // immutable run.workflowId (a stable map lookup) - never by re-running
    // resolvePlan(), so a queued/resumed run always executes against
    // whatever runner its own snapshot named, regardless of any profile
    // change made after it was enqueued.
    const auto runner = workflowResolver_.runnerFor(runningRun.workflowId);
    if (!runner.success()) {
        finishRun(runId, WorkflowResult {false, RunStatus::Failed, runner.error()->message, {}}, false);
        return;
    }

    RunEvent started;
    started.type = RunEventType::RunStarted;
    started.runId = runId;
    started.message = "Run started";
    notify(started);

    // Workflow code executes outside the manager lock on the worker thread.
    const WorkflowRunContext context {runningRun, project, stages};
    const WorkflowResult result = runner.value().get().run(
        context,
        currentRunStopSource_.get_token(),
        *this);

    bool shuttingDown = false;
    {
        std::lock_guard lock(mutex_);
        shuttingDown = shuttingDown_;
    }
    finishRun(runId, result, shuttingDown);
}

void RunManager::publish(const RunEvent& event)
{
    {
        // Persist stage and aggregate progress before notifying UI. The future
        // SQLite adapter will group these updates in one transaction.
        std::lock_guard lock(mutex_);
        if (event.stageId.has_value()) {
            const auto stagesResult = runRepository_.listStages(event.runId);
            if (stagesResult.success()) {
                const auto stageIt = std::find_if(
                    stagesResult.value().begin(),
                    stagesResult.value().end(),
                    [&event](const RunStage& stage) { return stage.id == *event.stageId; });
                if (stageIt != stagesResult.value().end()) {
                    RunStage stage = *stageIt;
                    stage.progress = std::clamp(event.stageProgress, 0.0, 1.0);
                    switch (event.type) {
                    case RunEventType::StageStarted:
                        stage.status = StageStatus::Running;
                        stage.startedAt = event.timestamp;
                        break;
                    case RunEventType::StageCompleted:
                        stage.status = StageStatus::Completed;
                        stage.finishedAt = event.timestamp;
                        stage.usage = event.usage;
                        break;
                    case RunEventType::StageFailed:
                        stage.status = StageStatus::Failed;
                        stage.finishedAt = event.timestamp;
                        stage.errorMessage = event.message;
                        break;
                    case RunEventType::StageSkipped:
                        stage.status = StageStatus::Skipped;
                        stage.finishedAt = event.timestamp;
                        stage.errorMessage = event.message;
                        break;
                    case RunEventType::StageCancelled:
                        stage.status = StageStatus::Cancelled;
                        stage.finishedAt = event.timestamp;
                        stage.errorMessage = event.message;
                        break;
                    default:
                        break;
                    }
                    static_cast<void>(runRepository_.updateStage(std::move(stage)));
                }
            }
        }

        const auto runResult = runRepository_.findRun(event.runId);
        if (runResult.success() && runResult.value().has_value()) {
            Run run = *runResult.value();
            run.progress = std::clamp(event.runProgress, 0.0, 1.0);
            run.currentStageId = event.stageId;
            if (event.type == RunEventType::StageCompleted
                && run.providerBudgetUsage.turnsStarted == 0) {
                mergeAccumulatedUsage(run.usage, event.usage);
            } else if (event.type == RunEventType::StageFailed) {
                run.errorMessage = event.message;
            }
            static_cast<void>(runRepository_.updateRun(std::move(run)));
        }
    }
    notify(event);
}

common::Result<void> RunManager::persistProviderBudgetUsage(
    const RunId& runId,
    const ProviderExecutionBudgetUsage& usage)
{
    RunEvent event;
    {
        std::lock_guard lock(mutex_);
        const auto found = runRepository_.findRun(runId);
        if (!found.success()) return common::Result<void>::failure(*found.error());
        if (!found.value().has_value()) {
            return common::Result<void>::failure(
                error("run.not_found", "Run not found while persisting provider usage"));
        }
        Run run = *found.value();
        run.providerBudgetUsage = usage;
        // Real-Run observation (af4e5703-90c7-401d-9235-808872719abf,
        // 2026-09-15): this is the only live updater of run.usage while a
        // Run is active (the final WorkflowResult-based save only lands on
        // a clean completion). It used to do `run.usage = {usage.inputTokens,
        // usage.outputTokens}` - a 2-field aggregate-init that silently
        // zeroed run.usage.cacheCreationInputTokens/cacheReadInputTokens on
        // every single turn, and, after ProviderExecutionBudgetUsage.inputTokens
        // was changed to exclude cache-read tokens (see its class comment),
        // also stopped being the honest total models::TokenUsage.inputTokens
        // is documented to be - it silently became the budget-countable
        // figure instead. Reconstructed from the guard's now-complete
        // breakdown: usage.inputTokens (fresh + cache-write) plus
        // usage.cacheReadInputTokens recovers the honest total.
        run.usage.inputTokens = usage.inputTokens + usage.cacheReadInputTokens;
        run.usage.outputTokens = usage.outputTokens;
        run.usage.cacheCreationInputTokens = usage.cacheCreationInputTokens;
        run.usage.cacheReadInputTokens = usage.cacheReadInputTokens;
        run.usage.cacheCreationDuration = usage.cacheCreationDuration;
        const auto updated = runRepository_.updateRun(std::move(run));
        if (!updated.success()) return common::Result<void>::failure(*updated.error());
        event.type = RunEventType::RunUsageUpdated;
        event.runId = runId;
        event.runProgress = updated.value().progress;
        event.stageId = updated.value().currentStageId;
        event.usage = updated.value().usage;
        event.message = usage.stopCode;
    }
    notify(event);
    return common::Result<void>::ok();
}

void RunManager::notify(const RunEvent& event)
{
    RunEventHandler handler;
    {
        std::lock_guard lock(mutex_);
        handler = eventHandler_;
    }
    // Never invoke external code while holding mutex_: Qt adapters may enqueue
    // work back onto the application boundary.
    if (handler) {
        handler(event);
    }
}

void RunManager::finishRun(
    const RunId& runId,
    const WorkflowResult& result,
    bool shuttingDown)
{
    RunEvent event;
    // Populated only when the run just became Completed and a project memory
    // writer is configured; recordRunOutcome() itself is always called
    // outside mutex_ (see below), never as a repository/service call made
    // while the lock is held.
    std::optional<Project> memoryProject;
    std::optional<Run> memoryRun;
    {
        std::lock_guard lock(mutex_);
        const auto found = runRepository_.findRun(runId);
        if (!found.success() || !found.value().has_value()) {
            return;
        }
        Run run = *found.value();
        // Shutdown has priority over a cooperative Cancelled result because the
        // run was interrupted by process lifecycle, not an explicit user action.
        if (shuttingDown) {
            run.status = RunStatus::Interrupted;
            run.errorMessage = "Application shutdown interrupted the run";
        } else if (run.status == RunStatus::Cancelling || result.finalStatus == RunStatus::Cancelled) {
            run.status = RunStatus::Cancelled;
            run.errorMessage = result.message;
        } else {
            run.status = result.finalStatus;
            run.errorMessage = result.success ? std::string {} : result.message;
        }
        const auto finishedAt = std::chrono::system_clock::now();
        run.finishedAt = finishedAt;
        run.currentStageId.reset();
        if (run.providerBudgetUsage.turnsStarted == 0) {
            run.usage = result.usage;
        } else {
            // Real-Run observation (af4e5703-90c7-401d-9235-808872719abf,
            // 2026-09-15): same reconstruction as
            // persistProviderBudgetUsage above, and for the same reason -
            // this used to be `{providerBudgetUsage.inputTokens,
            // providerBudgetUsage.outputTokens}`, a 2-field aggregate-init
            // that zeroed run.usage's cache fields and collapsed
            // inputTokens down to the budget-countable (cache-read
            // excluded) figure at the exact moment the Run terminates,
            // clobbering whatever persistProviderBudgetUsage had already
            // correctly set moments earlier.
            run.usage.inputTokens = run.providerBudgetUsage.inputTokens
                + run.providerBudgetUsage.cacheReadInputTokens;
            run.usage.outputTokens = run.providerBudgetUsage.outputTokens;
            run.usage.cacheCreationInputTokens = run.providerBudgetUsage.cacheCreationInputTokens;
            run.usage.cacheReadInputTokens = run.providerBudgetUsage.cacheReadInputTokens;
            run.usage.cacheCreationDuration = run.providerBudgetUsage.cacheCreationDuration;
        }
        if (run.status == RunStatus::Completed) {
            run.progress = 1.0;
        }
        // AG-17.7D-1: the central terminal transition. Whatever ended this Run - a runner that
        // returned before its first StageStarted (a route or budget refusal), a preflight failure,
        // a cancellation, a controlled shutdown - its stages are settled BEFORE the Run becomes
        // terminal, with the Run's own instant and message, so a terminal Run never owns a stage
        // that claims to be live. If the settlement cannot be written the Run is left unfinished
        // (like a failed Run write below): startup recovery settles it on the next start.
        const auto settled = settleLiveStages(
            runRepository_, runId, runningStageOutcome(run.status, shuttingDown), finishedAt,
            run.errorMessage.empty() ? std::string(kStageNeverStartedMessage) : run.errorMessage);
        if (!settled.success()) {
            return;
        }
        const auto updated = runRepository_.updateRun(run);
        if (!updated.success()) {
            return;
        }
        event = terminalEvent(updated.value());
        if (event.message.empty()) {
            event.message = result.message;
        }

        // Best-effort project memory hook: only for a terminal status that
        // actually became Completed (never Failed/Cancelled/Interrupted, and
        // never during shutdown - shuttingDown already forced Interrupted
        // above). Only the snapshot needed by the writer is captured here,
        // under the lock; the writer itself runs after the lock is released.
        if (projectMemoryService_ != nullptr && updated.value().status == RunStatus::Completed) {
            const auto projectResult = projectRepository_.findById(updated.value().projectId);
            if (projectResult.success() && projectResult.value().has_value()) {
                memoryProject = *projectResult.value();
                memoryRun = updated.value();
            }
        }
    }
    notify(event);

    // AG-14: unlike the project-memory hook above, the Work Ledger records
    // Completed/Failed/Cancelled alike (a Run interrupted by a clean shutdown
    // included; one a crash left behind is recorded by startup recovery's
    // ledger pass) - it is a workflow's
    // full terminal history, not only its successes. The service itself is
    // a no-op for a non-Manifest-workflow Run, so no workflowId check is
    // needed here. Failure here must never change the already-committed
    // status/event above - the return value is intentionally discarded,
    // matching modelExecutionService_/projectMemoryService_ below.
    if (manifestWorkLedgerService_ != nullptr) {
        static_cast<void>(manifestWorkLedgerService_->recordTerminalRun(runId));
    }

    // PC-4C: the Outcome Brief is derived from the ledger entry written just above, so it is nudged AFTER it. Best effort, like
    // the ledger write: it can never change the status/event already committed (the startup pass and the on-demand pull are the
    // source of truth if this call is lost).
    if (outcomeBriefTrigger_ != nullptr) {
        outcomeBriefTrigger_->runMayHaveChanged(runId);
    }

    // Every terminal run drops its cross-agent workspace path claims (see
    // RunPathClaimRegistry/ChangeProposalService) regardless of status - a
    // no-op lookup miss for runs that never claimed any path (Mock runs,
    // rosters with no workspaceAccess agent).
    if (pathClaimRegistry_ != nullptr) {
        pathClaimRegistry_->releaseRun(runId);
    }

    // AG-10 safety net: whatever this Run's exact terminal disposition was
    // (success, validation rejection surfaced as Failed, an internal repair
    // path, cancellation, or an exception the workflow runner turned into
    // Failed), the Run itself is now terminal - so no model_execution row
    // linked to it should legitimately remain Pending/Running. Failure here
    // must never change the already-committed Run status/event, matching the
    // project-memory call below; the return value is intentionally discarded.
    if (modelExecutionService_ != nullptr) {
        static_cast<void>(modelExecutionService_->reconcileRunExecutions(runId));
    }

    if (memoryProject.has_value() && memoryRun.has_value()) {
        // Failure here must never change the Completed status/event/result
        // already committed above - the return value is intentionally
        // discarded.
        static_cast<void>(projectMemoryService_->recordRunOutcome(
            *memoryProject, *memoryRun, result.finalOutput));
    }
}

} // namespace ai::application
