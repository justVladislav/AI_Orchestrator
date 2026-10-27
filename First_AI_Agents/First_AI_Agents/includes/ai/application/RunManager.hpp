#pragma once

#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/IWorkflowResolver.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestWorkLedgerService.hpp"
#include "ai/application/ModelExecutionService.hpp"
#include "ai/application/ProjectMemoryService.hpp"
#include "ai/application/RunPathClaimRegistry.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace ai::application {

class IOutcomeBriefTrigger;

// A Run with a parent is created only by a command that owns that lineage (Retry, Edit & rerun,
// a continuation, a legacy conversion). The generic fresh-plan path - OrchestrationService::
// startRun and RunManager::enqueue - never is one: a caller-supplied parentRunId there would
// forge lineage (and, for a Manifest child of a legacy Run, skip the conversion audit).
inline constexpr const char* kRunParentNotAllowedCode = "run.parent_not_allowed";

/**
 * Owns the single-worker MVP queue and the lifetime of asynchronous workflows.
 *
 * Repository mutations, queue changes, and cancellation are serialized by one
 * mutex. External event handlers are always invoked after releasing that mutex.
 * The design keeps Qt's GUI thread out of provider/workflow execution.
 *
 * RunManager no longer owns one fixed IWorkflowRunner: every enqueue() call
 * routes through an IWorkflowResolver, so the Mock workflow and the
 * production execution-profile workflow can coexist, and resolution
 * failures (unknown/disabled profile, invalid assignment, no real provider
 * runtime) surface as an ordinary Result failure before any Run row is even
 * created.
 */
class RunManager final : private IRunEventSink {
public:
    // projectMemoryService is an optional best-effort dependency (Task B of
    // PROJECT_MEMORY_PLAN.md): when non-null, a successfully persisted
    // RunStatus::Completed terminal update records one deterministic
    // ProjectMemorySegment. Defaulted to nullptr so every existing
    // three-argument test/composition-root construction keeps compiling
    // unchanged and simply opts out of project memory.
    // pathClaimRegistry is likewise optional and defaulted: when non-null,
    // every terminal run (any status, via finishRun) releases its cross-agent
    // workspace path claims (see RunPathClaimRegistry/ChangeProposalService)
    // so process memory never grows unbounded across the app's lifetime.
    // modelExecutionService is likewise optional and defaulted (AG-10): when
    // non-null, every terminal run (any status, via finishRun) reconciles any
    // model_executions row still linked to it that never reached a terminal
    // status itself - the in-process safety net for a bug somewhere in the
    // workflow runner's own execution lifecycle (e.g. an early return that
    // skips finishExecution()), complementing
    // ModelExecutionService::recoverInterruptedExecutions()'s startup-only,
    // whole-process recovery of rows abandoned by a prior process exit.
    // manifestWorkLedgerService is likewise optional and defaulted (AG-14):
    // when non-null, every terminal run (Completed/Failed/Cancelled, via
    // finishRun) records one durable Work Ledger entry - but only for a
    // kExecutionManifestWorkflowId Run; every other workflow is a no-op
    // inside the service itself. A Run a crash left behind is recorded by
    // startup recovery's ledger pass instead (ai::backend::runStartupRecovery).
    // Every terminal transition this class performs (finishRun, a queued Run
    // cancelled, a queued Run interrupted by shutdown) first settles the Run's
    // stages - see settleLiveStages - so a terminal Run owns no live stage.
    RunManager(
        IProjectRepository& projectRepository,
        IRunRepository& runRepository,
        IWorkflowResolver& workflowResolver,
        ProjectMemoryService* projectMemoryService = nullptr,
        RunPathClaimRegistry* pathClaimRegistry = nullptr,
        ModelExecutionService* modelExecutionService = nullptr,
        ManifestWorkLedgerService* manifestWorkLedgerService = nullptr);
    ~RunManager();

    RunManager(const RunManager&) = delete;
    RunManager& operator=(const RunManager&) = delete;

    // Resolves a fresh plan for request (respecting an explicit
    // executionProfileId or falling back to a matching default profile). A fresh Run has no
    // parent: a request that names one is refused with kRunParentNotAllowedCode.
    common::Result<RunId> enqueue(StartRunRequest request);
    // Used only by OrchestrationService::retryRun: reuses an already-resolved
    // plan captured on the original run (workflowId/executionProfileId/
    // assignmentSnapshot) instead of calling the resolver again, so a retry
    // can never silently pick up a changed default profile or a profile
    // edited since the original run.
    common::Result<RunId> enqueueWithPlan(
        StartRunRequest request,
        ResolvedExecutionPlan plan,
        const std::optional<RunId>& taskLinkSourceRunId = std::nullopt);
    // Resolves an edited plan afresh but copies the source Run's task links in
    // the same persistence transaction as the new aggregate.
    common::Result<RunId> enqueueEditedRerun(
        StartRunRequest request,
        const RunId& taskLinkSourceRunId);
    // AG-17.7C: the two ways a legacy roster Run becomes a Manifest Run, both persisting the
    // durable conversion record in the SAME aggregate insert as the new Run (never a Run without
    // its record, never a record without its Run). `request.parentRunId` must be the draft's
    // source Run (the aggregate check refuses anything else). The source Run's task links are
    // copied exactly as for any rerun.
    //   enqueueConvertedRetry - the plan is supplied (the immutable source snapshot, already
    //     converted); no profile and no catalog is consulted here, but the Manifest runner
    //     resolves every provider/model identity before its first provider call;
    //   enqueueConvertedRerun - a user-confirmed Edit & rerun: the plan is resolved afresh
    //     (validated against the current catalog) like enqueueEditedRerun. The confirmation
    //     time is stamped here, by the Backend, never supplied by a caller.
    common::Result<RunId> enqueueConvertedRetry(
        StartRunRequest request,
        ResolvedExecutionPlan plan,
        const LegacyConversionDraft& conversion);
    common::Result<RunId> enqueueConvertedRerun(
        StartRunRequest request,
        const LegacyConversionDraft& conversion);
    common::Result<void> requestCancellation(const RunId& runId);
    void setEventHandler(RunEventHandler handler);
    // Forwards a RunWarning event (see ArtifactService::setWarningObserver) to the event handler. It persists nothing:
    // the warning is already durable when this is called, and a warning is not a stage or Run state change.
    void announceWarning(const RunEvent& event) { notify(event); }

    [[nodiscard]] bool isActive(const RunId& runId) const;
    // Idempotent. Queued/current work becomes Interrupted and the worker joins.
    void shutdown();

    // PC-4C: the best-effort nudge to the Outcome Brief reconciler (see IOutcomeBriefTrigger). Set once while the runtime is
    // composed, before any concurrent use; null (the default) means no nudge.
    void setOutcomeBriefTrigger(IOutcomeBriefTrigger* trigger) noexcept { outcomeBriefTrigger_ = trigger; }

private:
    IOutcomeBriefTrigger* outcomeBriefTrigger_ {nullptr};
    common::Result<RunId> enqueueInternal(
        StartRunRequest request,
        common::Result<ResolvedExecutionPlan> plan,
        const std::optional<RunId>& taskLinkSourceRunId,
        const LegacyConversionDraft* conversion = nullptr);
    void workerLoop(std::stop_token shutdownToken);
    void executeRun(const RunId& runId);
    void publish(const RunEvent& event) override;
    common::Result<void> persistProviderBudgetUsage(
        const RunId& runId,
        const ProviderExecutionBudgetUsage& usage) override;
    void notify(const RunEvent& event);
    void finishRun(const RunId& runId, const WorkflowResult& result, bool shuttingDown);

    IProjectRepository& projectRepository_;
    IRunRepository& runRepository_;
    IWorkflowResolver& workflowResolver_;
    ProjectMemoryService* projectMemoryService_ {nullptr};
    RunPathClaimRegistry* pathClaimRegistry_ {nullptr};
    ModelExecutionService* modelExecutionService_ {nullptr};
    ManifestWorkLedgerService* manifestWorkLedgerService_ {nullptr};

    // Guards runtime state and serializes run revision updates against cancel.
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<RunId> queue_;
    std::optional<RunId> currentRunId_;
    std::stop_source currentRunStopSource_;
    RunEventHandler eventHandler_;
    bool shuttingDown_ {false};
    std::jthread worker_;
};

} // namespace ai::application
