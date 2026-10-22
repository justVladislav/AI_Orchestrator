#pragma once

#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/application/IModelRepository.hpp"
#include "ai/application/IProviderClient.hpp"
#include "ai/application/IProviderRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"

#include <optional>

namespace ai::application {

class ArtifactService;
class ExecutionManifestService;
class IProjectMemoryRepository;
class ModelExecutionService;

/**
 * Production workflow runner: executes the immutable
 * Run::assignmentSnapshot as a validated dependency graph. Provider calls
 * are bounded by Run::maxParallelism; each assignment may retry and fall
 * back to another model. Legacy/mixed text-output graphs use a final
 * synthesis stage through IProviderClientFactory; proposal-only worker
 * graphs skip that billable call and end with a deterministic Backend
 * summary of proposal/validation state.
 * If any roster worker has workspace access, the Architect must first return
 * a validated execution-manifest.v1 containing exactly one allocation per
 * worker. Ownership overlap, missing roster tasks, or capability mismatch
 * fails before provider fan-out. Accepted dependencies replace the legacy
 * defaults, and each workspace proposal is hard-scoped to its task's
 * ownedPaths through ProjectWritePolicy.allowedRoots.
 *
 *
 * Context assembly per stage includes the full Project::description, selected
 * durable project memory, full Run::task, and relevant successful dependency
 * outputs. A manifest worker receives only its own objective, role,
 * ownedPaths, acceptance criteria, and context references; the Architect's
 * full roster manifest is deliberately hidden. No truncation is performed
 * here - Project memory is selected once per run under an explicit
 * model-aware budget (15% of the smallest assigned input capacity, capped at
 * 16k tokens); segments are skipped whole, never cut. The exact selected text
 * and segment ids are recorded in the ContextSnapshot artifact.
 *
 * A stage's raw text output is stored as a Report artifact (through the
 * optional ArtifactService, same as MockWorkflowRunner) so it is reviewable
 * regardless of whether the run ultimately completes or fails. Proposal-only
 * graphs also store a deterministic run-level completion summary. The Run is
 * only Completed if every required assignment completes successfully;
 * Rejected proposals fail closed, and an optional synthesis stage may be
 * truthfully Skipped when structured proposal evidence needs no provider
 * reconciliation. Failed dependencies skip their downstream tasks, while
 * cancellation and unrecovered provider errors finish the run as
 * Cancelled/Failed rather than masking an incomplete graph.
 */
class ExecutionProfileWorkflowRunner final : public IWorkflowRunner {
public:
    // agentSessionFactory/agentToolExecution/changeProposals are optional
    // and used only by assignments with workspaceAccess == true (see
    // StageModelAssignment::workspaceAccess): they give an individual
    // roster agent the same read-only-workspace-tools +
    // propose_change_set pipeline read-only analysis already has, via the
    // shared AgentToolTurnLoopRunner. A workspaceAccess assignment fails
    // closed with "run.workspace_access_unavailable" when any of the three
    // is unwired - callers that never need workspace access (Mock/CLI
    // compositions, or a roster with no workspaceAccess agent) can omit them.
    ExecutionProfileWorkflowRunner(
        IProviderClientFactory& clientFactory,
        IProviderRepository& providers,
        IModelRepository& models,
        ArtifactService* artifactService = nullptr,
        ModelExecutionService* modelExecutionService = nullptr,
        IProjectMemoryRepository* projectMemoryRepository = nullptr,
        providers::IProviderAgentSessionFactory* agentSessionFactory = nullptr,
        AgentToolExecutionService* agentToolExecution = nullptr,
        ChangeProposalService* changeProposals = nullptr,
        ExecutionManifestService* executionManifests = nullptr);

    // One Pending stage per graph assignment plus the optional final
    // synthesis stage. Proposal-only worker graphs keep that stage in the
    // persisted timeline but mark it Skipped without a provider execution.
    // (architect, implementation, documentation) rather than by whatever
    // order the profile happened to store them in, so the timeline RunsPage
    // shows always reads the same way.
    [[nodiscard]] std::vector<RunStage> createStages(
        const RunId& runId, const ResolvedExecutionPlan& plan) const override;

    WorkflowResult run(
        const WorkflowRunContext& context,
        std::stop_token stopToken,
        IRunEventSink& eventSink) override;

private:
    IProviderClientFactory& clientFactory_;
    IProviderRepository& providers_;
    IModelRepository& models_;
    ArtifactService* artifactService_;
    ModelExecutionService* modelExecutionService_;
    IProjectMemoryRepository* projectMemoryRepository_;
    providers::IProviderAgentSessionFactory* agentSessionFactory_;
    AgentToolExecutionService* agentToolExecution_;
    ChangeProposalService* changeProposals_;
    // Optional persistence for the Architect's accepted ownership plan. The
    // same plan is still parsed and validated fail-closed when this service
    // is absent (for lightweight compositions/tests).
    ExecutionManifestService* executionManifests_;
    // Constructed only when all three workspace-access dependencies above
    // are wired; a workspaceAccess assignment fails closed otherwise.
    std::optional<AgentToolTurnLoopRunner> workspaceTurnLoop_;
};

} // namespace ai::application
