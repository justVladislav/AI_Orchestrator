#pragma once

#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/execution/ExecutionManifestExecutionCapacity.hpp"
#include "ai/application/execution/ExecutionManifestExecutor.hpp"
#include "ai/application/execution/ExecutionManifestPlanner.hpp"
#include "ai/application/execution/ExecutionManifestService.hpp"
#include "ai/application/verification/ManifestVerificationEnvironmentPreflight.hpp"
#include "ai/application/execution/IManifestTaskExecutionRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/IProviderClient.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/execution/IWorkflowRunner.hpp"
#include "ai/application/verification/IVerificationBootstrapRepository.hpp"
#include "ai/application/verification/IVerificationScratchRootProvider.hpp"
#include "ai/application/execution/ManifestWorkLedgerService.hpp"

namespace ai::application {

class ArtifactService;
class ModelExecutionService;
class RunWorkspaceScopeService;
class VerificationEnvironmentDiscoveryService;
class ProjectReadinessService;

// Backend-computed planning-input identity (SHA-256 hex). Exposed so tests can
// prove that execution-capacity policy identity is part of it. `capacityPolicyIdentity`
// must be executionCapacityPolicyIdentity() in production; a different value
// (for example an older policy's identity, or empty) yields a different
// fingerprint, so a Retry can never replay an old policy as unchanged input.
[[nodiscard]] std::string manifestPlanningInputFingerprint(
    const WorkflowRunContext& context,
    const StageModelAssignment& assignment,
    const Provider& provider,
    const Model& model,
    const WorkspaceBaselineSnapshot& baseline,
    const std::optional<VerificationEnvironmentSnapshot>& verificationEnvironment,
    std::string_view capacityPolicyIdentity);

/**
 * Production manifest workflow: runs the inspect-only planner (Phase 3) to
 * either freeze one ExecutionManifest for this Run or return a typed,
 * evidence-backed NoChangesNeeded disposition. A no-op completes planning,
 * skips execution without creating worker/task/manifest rows, persists its
 * planning report, and never launches a recommended next stage. Otherwise
 * the manifest executor (Phase 4) dispatches the frozen graph. Both paths
 * produce a deterministic Backend completion summary. It is registered
 * beside kExecutionProfileWorkflowId
 * and kMockWorkflowId under kExecutionManifestWorkflowId - selecting it
 * never changes legacy roster scheduling in place.
 *
 * Selection: exactly like the execution-profile workflow, through an
 * ExecutionProfile whose own workflowId is kExecutionManifestWorkflowId.
 * A one-member profile keeps its historical implicit Architect semantics.
 * A multi-member profile carries exactly one explicit lead Architect plus
 * available workspace-capable workers. The lead plans and owns optional
 * integration; the frozen Manifest assigns each provider-backed task to a
 * worker and TaskRouteResolver supplies that member's provider/model,
 * instructions, pricing and timeout. The whole snapshot is resolved before
 * the first provider call and frozen into manifest-routing.v1.
 *
 * Two fixed stages are declared upfront (planning, execution) since a
 * manifest's actual task DAG is only known after planning completes -
 * unlike the legacy roster, it cannot be reflected as one RunStage per task
 * in this slice. Per-task status is instead persisted live through an
 * optional IManifestTaskExecutionRepository (via ExecutionManifestExecutor's
 * onTaskFinished callback, plus one final full sync after execute() returns
 * to also capture cascaded Skipped tasks the callback never dispatches) and
 * exposed to Qt/QML through ExecutionManifestReadModelService.
 *
 * A billable Architect integration call (through IProviderClientFactory,
 * the same mechanism the legacy runner's "synthesis" stage already uses)
 * only runs when the executor did not report ManifestExecutionStatus::
 * Completed - i.e. a real conflict or an incomplete task, never a clean
 * run. Its response is persisted separately as explicitly non-authoritative
 * advice and is never appended to the deterministic Run report/finalOutput;
 * its own failure remains a diagnostic note rather than flipping an
 * already-decided terminal status.
 */
class ManifestWorkflowRunner final : public IWorkflowRunner {
public:
    // manifestWorkLedgerService is optional (AG-14): when non-null, the
    // Planner's userContent is extended with a bounded slice of prior
    // Manifest Runs for this project (ManifestWorkLedgerService::
    // buildPlannerContext), and a manifest-work-ledger-context.md artifact
    // names the exact ledger entry ids included - see run()'s own
    // plannerRequest construction. Terminal-run recording itself is wired
    // through RunManager, not here (see RunManager.hpp) - a workflow runner
    // only ever sees its own single run() call, never the Run's final
    // committed status/event.
    ManifestWorkflowRunner(
        providers::IProviderAgentSessionFactory& agentSessionFactory,
        AgentToolExecutionService& agentToolExecution,
        ChangeProposalService& changeProposals,
        ExecutionManifestService& manifests,
        IProviderClientFactory& clientFactory,
        IProviderRepository& providers,
        IModelRepository& models,
        ArtifactService* artifactService = nullptr,
        ModelExecutionService* modelExecutionService = nullptr,
        IManifestTaskExecutionRepository* taskExecutions = nullptr,
        IManifestVerificationCommandRunner* verificationCommandRunner = nullptr,
        ManifestWorkLedgerService* manifestWorkLedgerService = nullptr,
        RunWorkspaceScopeService* runWorkspaceScopeService = nullptr,
        VerificationEnvironmentDiscoveryService* verificationEnvironmentDiscoveryService = nullptr,
        IVerificationBootstrapRepository* verificationBootstrapRepository = nullptr);

    [[nodiscard]] std::vector<RunStage> createStages(
        const RunId& runId, const ResolvedExecutionPlan& plan) const override;

    WorkflowResult run(
        const WorkflowRunContext& context,
        std::stop_token stopToken,
        IRunEventSink& eventSink) override;

    // The validated root of the TEMPORARY verification copies (see VerificationScratchPolicy). Set once while the runtime is
    // composed; null (the default) keeps the executor's own default root. With a provider, a Run that must produce a Manifest is
    // refused BEFORE the planner is paid when the root is not usable, any Run is refused before a worker is paid, and the executor
    // builds every dependency and combined-verification overlay under exactly the root the provider validated.
    void setVerificationScratchProvider(const IVerificationScratchRootProvider* provider) noexcept
    {
        scratchProvider_ = provider;
    }

    // AG-18.4B-R1: the project's durable verification readiness. With it, a Run on a project that has CMake presets proves (or
    // reuses the proof of) a configured, buildable setup BEFORE any provider session exists when it must produce a Manifest, and
    // before the first worker is paid otherwise. A Run that may legitimately end in "no changes needed" is not blocked up front;
    // one that did produce a Manifest is. Null (the default) keeps the previous behaviour exactly.
    void setProjectReadinessService(ProjectReadinessService* service) noexcept
    {
        projectReadiness_ = service;
    }

private:
    ProjectReadinessService* projectReadiness_ {nullptr};
    const IVerificationScratchRootProvider* scratchProvider_ {nullptr};
    ExecutionManifestPlanner planner_;
    ExecutionManifestExecutor executor_;
    SystemManifestVerificationCommandRunner systemVerificationCommandRunner_;
    IManifestVerificationCommandRunner* verificationCommandRunner_;
    IProviderClientFactory& clientFactory_;
    IProviderRepository& providers_;
    IModelRepository& models_;
    ArtifactService* artifactService_;
    ModelExecutionService* modelExecutionService_;
    IManifestTaskExecutionRepository* taskExecutions_;
    ManifestWorkLedgerService* manifestWorkLedgerService_;
    // AG-15: optional; when non-null, run() freezes a RunWorkspaceScope
    // immediately after the ExecutionManifest itself is frozen, and the
    // Planner's userContent is extended with a bounded discovery catalog
    // rendered from the already-captured baseline (see
    // WorkspaceBaselineProvenance::renderDiscoveryCatalog). A freeze failure
    // fails the Run, mirroring the AG-14.2 required-identity-persistence
    // gate pattern.
    RunWorkspaceScopeService* runWorkspaceScopeService_;
    // AG-18.1: optional; when non-null, run() discovers and durably records
    // a VerificationEnvironmentSnapshot before the Planner ever runs. Purely
    // additive in this package - never feeds the Planner's prompt, never
    // gates the Run (AG-18.2/18.3's job); a discovery-content failure (no
    // preset file, cmake unavailable) is folded into the persisted snapshot
    // itself, never surfaced here. Only a genuine persistence failure fails
    // the Run, mirroring the workspace-baseline-persistence-required gate
    // immediately above it in run().
    VerificationEnvironmentDiscoveryService* verificationEnvironmentDiscoveryService_;
    // AG-18.3: optional; when non-null, run() records a durable
    // VerificationBootstrapDecision whenever this Run enters the bootstrap
    // lifecycle (BootstrapPending on a validated same-Run bootstrap
    // declaration, or ActionRequired when planning could not otherwise
    // produce a usable manifest against a SetupRequired snapshot). Never
    // wired means AG-18.3 behaves as a strict no-op superset of AG-18.2 -
    // an unavailable/unwired repository never changes whether a Run
    // succeeds or fails, only whether that fact is durably queryable.
    IVerificationBootstrapRepository* verificationBootstrapRepository_;
};

} // namespace ai::application
