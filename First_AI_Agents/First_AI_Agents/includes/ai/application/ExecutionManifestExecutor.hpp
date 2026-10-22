#pragma once

#include "ai/application/AgentToolExecutionService.hpp"
#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/ExecutionManifestScheduler.hpp"
#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/TaskRouteResolver.hpp"
#include "ai/application/VerificationBootstrapTypes.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"
#include "ai/models/TokenUsage.hpp"
#include "ai/providers/IProviderAgentSession.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace ai::application {

class ModelExecutionService;
class RunWorkspaceScopeService;

// Manifest verification reliability hardening: everything a bounded
// verification-repair worker session needs beyond the task's own frozen
// allocation (see buildTaskUserContent). promptPrefix is fully rendered,
// secret-free, leak-safe text - the verified Backend diagnostics for this
// task's own requirements plus its previous proposal's content, framed
// exactly like AgentToolTurnLoopRunner's own established one-shot repair
// prompt ("fresh session, no memory of your previous attempt"). Never
// carries a raw command payload, an absolute path, or a raw provider
// response - see buildVerificationRepairContext in the .cpp.
struct ManifestTaskRepairContext {
    std::string promptPrefix;
    ChangeSetId supersedesChangeSetId;
    // MW-P5.4: set only when supersedesChangeSetId belongs to a DIFFERENT
    // Run than the one this execution request targets - the cross-Run
    // continuation-repair case. Unset (the default) for the existing
    // within-Run bounded verification-repair wave, which keeps requiring
    // the predecessor to belong to this same Run exactly as before.
    std::optional<RunId> predecessorRunId;
};

struct ExecutionManifestExecutionRequest {
    ExecutionManifest manifest;
    RunId runId;
    ProjectId projectId;

    // The Run-wide DEFAULT route. Before AG-17.2 one provider/model executed every
    // Inspect/Propose-capability task; since AG-17.2 a task's route comes from
    // routeResolver (below) when one is set, and these fields (provider, model,
    // output ceiling, pricing) are used only when it is null - the unchanged
    // single-route behaviour direct callers and older Manifests keep. Manifest
    // tasks never carry a provider/model choice of their own: the task names an
    // assignee member, the Backend resolves the rest - see ExecutionManifestTask.
    ProviderId providerId;
    ModelId modelId;
    std::string apiModelId;
    std::string providerKind;
    std::uint64_t maxOutputTokens {0};
    // Backend-owned catalog snapshot used only for cost calculation when a
    // worker attempt is closed. Production callers supply the same Model
    // they resolved for modelId. Legacy/test callers may omit it; any
    // non-zero usage is then persisted with priceKnown=false rather than as
    // a misleading known-free provider call.
    std::optional<Model> pricingModel;
    // AG-17.2: when set, every provider-backed task runs on the route this resolver
    // returns for it (its own provider/model/output ceiling/turn timeout/pricing/
    // instructions). Every provider-backed task is resolved BEFORE any worker is
    // dispatched; an unresolvable one fails the whole execution closed
    // (manifest.route_unresolved) with no session, accounting row or capacity
    // reservation created. The Run-wide guards (providerBudgetGuard, capacity
    // reservation, terminal forcing) stay shared and route-independent.
    std::shared_ptr<const TaskRouteResolver> routeResolver;
    // The caller owns the model-execution namespace that surrounds this
    // executor. ManifestWorkflowRunner reserves its Planner assignment's
    // position and starts workers at the next position. Each Manifest task
    // keeps one stable position across bounded attempts; `attempt` is the
    // retry discriminator in the durable (source, sourceId, position,
    // attempt) identity.
    std::uint32_t workerModelExecutionPositionBase {0};

    std::filesystem::path workspaceRoot;
    // PC-2 (gap MW-P9): the Run's frozen, validated interface contracts. The executor renders, verbatim and
    // before the task objective, every contract whose scope covers a path this task owns, creates, must read
    // or references (an unscoped contract reaches every provider-backed task). The Run text itself is never
    // part of a worker prompt, so this is the only way a shared data shape reaches a worker, and the Planner
    // is not in the path. Empty for direct callers and Runs without contracts: the prompt is then unchanged.
    std::vector<RunInterfaceContract> interfaceContracts;
    // Trusted read policy for dispatching tool calls (AgentToolExecutionService::
    // buildPolicy(workspaceRoot)) - never derived from model input.
    WorkspaceReadPolicy inspectPolicy;

    // Agent 2's workflow composition enables this after it has persistence
    // for the typed verification outcome. Kept opt-in for old callers that
    // have no structured acceptance contract yet.
    bool requireCombinedVerification {false};
    // Per-Run immutable policy. Disabled preserves the primary combined
    // verification result and forbids the additional provider-backed repair
    // wave; it never skips or weakens verification itself.
    VerificationRepairPolicy verificationRepairPolicy {
        VerificationRepairPolicy::OneBoundedWave};
    std::optional<std::filesystem::path> verificationScratchRoot;
    std::optional<std::filesystem::path> dependencyOverlayScratchRoot;
    WorkspaceBaselineSnapshot baselineSnapshot;
    IManifestVerificationCommandRunner* verificationCommandRunner {nullptr};
    // AG-15: optional. When non-null, each Propose-capability task's turn
    // loop gets its frozen requiredReadPaths as a same-attempt read gate
    // (see AgentToolTurnLoopRequest::requiredReadPathKeys) and the
    // request_scope_expansion tool for auditable, bounded growth of that
    // set. Exact initial read/write allow-lists remain enforced even when
    // this dependency is null; null only disables audited expansion.
    RunWorkspaceScopeService* runWorkspaceScopeService {nullptr};

    // Restart/resume parity: task ids present here keep their prior terminal
    // state instead of being re-run. Empty means a fresh execution.
    std::unordered_map<std::string, ManifestTaskState> seededStates;

    // Bounded worker retries: a task whose worker outcome is not a genuine
    // success gets re-dispatched up to this many additional times before
    // being recorded Failed. Mirrors the "at most one bounded repair"
    // philosophy already used by the planner (Phase 3) and the turn-loop's
    // own JSON-repair retry - never unbounded.
    std::size_t maxAttemptsPerTask {2};

    // Optional: invoked once a task reaches a terminal outcome (Completed/
    // Failed/Skipped) and has just been applied to the scheduler, still on
    // the same thread execute() itself runs on (never from a task's own
    // std::async worker thread). Lets a caller (ManifestWorkflowRunner)
    // persist live per-task progress instead of only a final snapshot after
    // the whole manifest finishes - purely additive, default no-op, so
    // every existing caller/test keeps compiling and behaving unchanged.
    std::function<void(const std::string& taskId, const ManifestTaskState&)> onTaskFinished;
    std::shared_ptr<ProviderSessionBudgetGuard> providerBudgetGuard;

    // MW-P5.4: task id -> the fully-rendered repair context this task's very
    // FIRST attempt must materialize a successor from, instead of creating
    // a fresh proposal - a continuation child Run's repair-only Manifest,
    // where every Propose-capability task exists specifically to amend one
    // already-durable source proposal (see ManifestTaskRepairContext's own
    // predecessorRunId field for the cross-Run identity proof). The caller
    // (ManifestVerificationContinuationRepairService) renders promptPrefix
    // itself - this request type has no access to the source Run's durable
    // Work Ledger verification identity that prompt is built from. Empty
    // map (the default) leaves ordinary primary dispatch completely
    // unchanged; only that one caller ever populates this.
    std::unordered_map<std::string, ManifestTaskRepairContext> primaryRepairContextsByTaskId;

    // MW-P5.4: when a task's first attempt uses a primaryRepairContextsByTaskId
    // entry AND that task has no in-manifest dependency (the repair-only
    // manifest deliberately declares none - every repair task amends its own
    // source proposal independently, not against a sibling's fresh output),
    // its read-only candidate root is this materialized overlay instead of
    // the plain workspaceRoot - the caller (ManifestVerificationContinuationRepairService)
    // builds it once from the exact reconstructed source candidate set. A
    // task with a real dependencyOverlay (ordinary primary dispatch, or a
    // future repair-only manifest that does declare dependencies) always
    // takes precedence over this field. nullopt (the default) leaves every
    // existing caller's candidate-root resolution completely unchanged.
    std::optional<std::filesystem::path> primaryRepairCandidateReadRoot;
};

struct ExecutionManifestExecutionOutcome {
    ManifestExecutionStatus status {ManifestExecutionStatus::InProgress};
    bool cancelled {false};
    // Populated only when execution itself is rejected before any task
    // attempt is started (for example the post-freeze Run-wide provider-turn
    // capacity admission). Kept separate from verificationErrorCode because
    // combined verification has not run at this boundary.
    std::string executionErrorCode;
    std::string executionErrorMessage;
    std::unordered_map<std::string, ManifestTaskState> taskStates;
    models::TokenUsage usage;
    std::size_t totalTurns {0};
    std::size_t totalToolCalls {0};
    std::size_t successfulInspectionCalls {0};
    std::size_t totalReturnedBytes {0};
    std::unordered_map<std::string, ManifestWorkerNoChanges> workerNoChanges;
    std::unordered_map<std::string, ManifestTaskVerificationSubmission>
        taskVerifications;
    // Populated only after combined proposal-set verification. Always the
    // FINAL authoritative result: when a bounded verification repair wave
    // ran (see below), this is the repeat verification after the repair,
    // never the primary one - the only result callers persist/act on
    // (ledger, Apply-gate, read model). A required failure/not-verifiable
    // result must force status=Failed before this outcome is handed to
    // persistence/UI composition.
    std::optional<ManifestProposalSetVerification> proposalSetVerification;
    std::string verificationErrorCode;
    std::string verificationErrorMessage;
    // Manifest verification reliability hardening: populated only when the
    // primary combined verification failed/was-not-verifiable AND a bounded
    // repair wave was actually attempted - the untouched result of that
    // FIRST verification, kept separate so it is never overwritten by the
    // final one above. Absent when verification passed on the first try, or
    // when it failed but was not eligible for repair (proposalSetVerification
    // above is then already the one-and-only result).
    std::optional<ManifestProposalSetVerification> primaryProposalSetVerification;
    // true only when a bounded verification repair wave actually ran (one or
    // more repair worker sessions were dispatched) - never true merely
    // because primary verification failed and was found ineligible for
    // repair.
    bool verificationRepairAttempted {false};
    // Task ids that received exactly one repair worker session in the one
    // bounded repair wave, in dispatch order. Empty when
    // verificationRepairAttempted is false.
    std::vector<std::string> verificationRepairedTaskIds;
    // AG-18.3: populated only when request.manifest.bootstrapDeclaration
    // was present and the combined proposal overlay's rediscovery
    // successfully bound every deferred requirement to a real candidate
    // profile - never set on a binding failure (status is already Failed
    // via verificationErrorCode/Message in that case). The caller
    // (ManifestWorkflowRunner) persists this as a durable
    // VerificationBootstrapTransition.
    std::optional<VerificationBootstrapCandidateIdentity> bootstrapCandidateIdentity;
};

// Schedules and dispatches a frozen ExecutionManifest's task DAG. Owns
// exactly: bounded-parallel ready-task dispatch (ExecutionManifestScheduler),
// per-task worker execution for WorkspaceCapability::Inspect/Propose tasks through
// the same audited AgentToolTurnLoopRunner every other workspace-access
// caller uses, and cancellation/retry around that dispatch.
//
// Ownership enforcement happens in two layers, matching the target
// architecture's "enforce owned paths before RunPathClaimRegistry; retain
// the registry only as the final race guard": each task's worker session is
// given a ProjectWritePolicy scoped to that task's declared ownedPaths
// (ChangeProposalValidator rejects any change outside them before the
// change set ever reaches ReadyForReview), and only a change set that
// already passed that check goes on to compete for RunPathClaimRegistry's
// cross-task race claim (via the caller-supplied ChangeProposalService,
// exactly as it already does for every other change-proposing caller).
//
// None is a strictly empty coordination node. Inspect launches an audited,
// read-only worker and can terminate only through typed verification
// evidence. Propose launches the proposal worker and uses a distinct typed,
// evidence-backed terminal when the assigned work is already satisfied.
class ExecutionManifestExecutor final {
public:
    ExecutionManifestExecutor(
        providers::IProviderAgentSessionFactory& sessionFactory,
        AgentToolExecutionService& toolExecution,
        ChangeProposalService& changeProposals,
        ModelExecutionService* modelExecutionService = nullptr);

    [[nodiscard]] ExecutionManifestExecutionOutcome execute(
        ExecutionManifestExecutionRequest request,
        std::stop_token stopToken) const;

private:
    // Everything one task attempt produces, kept out of the shared
    // aggregate outcome so concurrently-dispatched tasks (execute() runs a
    // ready batch on separate threads) never write shared state - each
    // future's result is merged into the aggregate sequentially instead.
    struct TaskAttemptResult {
        ManifestTaskState state;
        models::TokenUsage usage;
        std::size_t totalTurns {0};
        std::size_t totalToolCalls {0};
        std::size_t successfulInspectionCalls {0};
        std::size_t totalReturnedBytes {0};
        // Propagated from AgentToolTurnLoopOutcome::repairAttempted (see its
        // doc comment). A Failed attempt that already consumed its one-shot
        // internal repair must never receive an additional outer worker
        // retry - see the retry decision in execute() below.
        bool repairAttempted {false};
        // Propagated from AgentToolTurnLoopOutcome::nonEmptyRejected (see
        // its doc comment). Independent of repairAttempted: a non-repairable
        // finding on a non-empty proposal never triggers internal repair at
        // all, so this must gate outer retry on its own.
        bool nonEmptyRejected {false};
        bool nonRetryable {false};
        std::optional<ManifestWorkerNoChanges> workerNoChanges;
        std::optional<ManifestTaskVerificationSubmission> taskVerification;
    };

    [[nodiscard]] TaskAttemptResult runOneTask(
        const ExecutionManifestExecutionRequest& request,
        const ExecutionManifestTask& task,
        const std::string& dependencyOutputs,
        const std::optional<std::filesystem::path>& candidateReadRoot,
        std::size_t attemptNumber,
        std::stop_token stopToken,
        const std::optional<ManifestTaskRepairContext>& repairContext = std::nullopt) const;

    providers::IProviderAgentSessionFactory& sessionFactory_;
    AgentToolExecutionService& toolExecution_;
    ChangeProposalService& changeProposals_;
    ModelExecutionService* modelExecutionService_;
    AgentToolTurnLoopRunner turnLoop_;
};

} // namespace ai::application
