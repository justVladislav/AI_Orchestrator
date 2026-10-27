#pragma once

#include "ai/application/AgentToolExecutionService.hpp"
#include "ai/application/ChangeProposalService.hpp"
#include "ai/application/ExecutionManifestExecutor.hpp"
#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/IManifestTaskExecutionRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/IVerificationScratchRootProvider.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestWorkLedgerService.hpp"
#include "ai/providers/IProviderAgentSession.hpp"

#include <filesystem>
#include <stop_token>
#include <vector>

namespace ai::application {

class IOutcomeBriefTrigger;

class ArtifactService;
class ModelExecutionService;

// MW-P5.4's outcome: the terminal status this call left the child Run in,
// which source tasks were actually repaired, and (only when execution
// genuinely reached the final re-verification step) its result. A caller
// never needs to separately re-read the child Run/ledger just to know what
// happened - though both remain the durable source of truth afterward.
struct ManifestVerificationContinuationRepairOutcome {
    RunId childRunId;
    RunStatus finalStatus {RunStatus::Failed};
    std::vector<std::string> repairedTaskIds;
    std::optional<ManifestProposalSetVerification> finalVerification;
};

// MW-P5.4: the only command boundary allowed to promote a P5.3 Prepared
// continuation child Run into real executable work (see RunStatus::Prepared's
// own doc comment in ApplicationTypes.hpp). One synchronous call:
//
//   1. Compiles a minimal Backend-authored repair Manifest for the child Run
//      containing exactly one task per admitted diagnostic owner (P5.2's
//      durable ManifestVerificationAttributionCheckpoint findings whose kind
//      is SourceProposalOwner or IntegrationOwner - never
//      PreExistingVerificationDebt/IgnorableCascade), each task copied
//      verbatim from the source Manifest's own already-validated definition
//      (same ownedPaths/plannedCreatePaths/requiredReadPaths/referencePaths/
//      acceptanceContract - dependencies cleared, since every repair worker
//      amends its own source proposal independently against the frozen exact
//      source candidate, never against a sibling's fresh output).
//   2. Dispatches each owner's repair worker through
//      ExecutionManifestExecutor's existing bounded-repair materialization
//      pipeline (ManifestTaskRepairContext::predecessorRunId names the
//      source Run explicitly), reading the reconstructed exact source
//      candidate overlay (never the live baseline, never a sibling's
//      in-flight repair).
//   3. Assembles the final exact proposal set: every repaired task's new
//      successor replaces its source predecessor, every other source member
//      is referenced unchanged - never copied - in the source's own
//      orderedProposalIds sequence.
//   4. Re-runs the COMPLETE original combined-verification contract (the
//      source Manifest's full task list, not just the repaired subset)
//      against that final set - the same authoritative check an ordinary
//      Manifest Run's primary verification performs.
//   5. Records the child Run terminal (Completed only when every acceptance
//      requirement passed) and its Work Ledger entry via the same
//      ManifestWorkLedgerService::recordTerminalRun every other Manifest Run
//      already uses.
//
// Never mutates or re-verifies the source Run itself. Never Applies
// anything - AG-16 (ManifestApplyVerificationGate) continues to gate Apply
// separately, narrowly extended to accept this cross-Run final set only
// through this exact typed checkpoint chain.
class ManifestVerificationContinuationRepairService final {
public:
    ManifestVerificationContinuationRepairService(
        IRunRepository& runs,
        IProjectRepository& projects,
        ExecutionManifestService& manifests,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationContinuationRepository& continuations,
        IManifestTaskExecutionRepository& taskExecutions,
        IProviderRepository& providers,
        IModelRepository& models,
        ChangeProposalService& proposals,
        ManifestWorkLedgerService& workLedgerService,
        ArtifactService& artifacts,
        providers::IProviderAgentSessionFactory& sessionFactory,
        AgentToolExecutionService& toolExecution,
        IManifestVerificationCommandRunner& commandRunner,
        ModelExecutionService* modelExecutionService = nullptr,
        std::filesystem::path trustedScratchRoot = {});

    [[nodiscard]] common::Result<ManifestVerificationContinuationRepairOutcome> repair(
        const RunId& childRunId, std::stop_token stopToken = {});

    // PC-4C: the best-effort nudge to the Outcome Brief reconciler (see IOutcomeBriefTrigger). Set once while the runtime is
    // composed, before any concurrent use; null (the default) means no nudge.
    void setOutcomeBriefTrigger(IOutcomeBriefTrigger* trigger) noexcept { outcomeBriefTrigger_ = trigger; }
    // So that the composition root's wiring can be asserted: this nudge fires only at the end of a verified, paid flow.
    [[nodiscard]] IOutcomeBriefTrigger* outcomeBriefTrigger() const noexcept { return outcomeBriefTrigger_; }

    // The validated root of the temporary verification copies. Set once while the runtime is composed; null keeps the constructor's
    // root unchanged. With a provider repair() is refused - before anything is copied - when the root is not usable.
    void setVerificationScratchProvider(const IVerificationScratchRootProvider* provider) noexcept
    {
        scratchProvider_ = provider;
    }

private:
    IOutcomeBriefTrigger* outcomeBriefTrigger_ {nullptr};
    const IVerificationScratchRootProvider* scratchProvider_ {nullptr};
    [[nodiscard]] common::Result<void> finishChildRun(
        const RunId& childRunId, RunStatus status, std::string errorMessage);

    IRunRepository& runs_;
    IProjectRepository& projects_;
    ExecutionManifestService& manifests_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationContinuationRepository& continuations_;
    IManifestTaskExecutionRepository& taskExecutions_;
    IProviderRepository& providers_;
    IModelRepository& models_;
    ChangeProposalService& proposals_;
    ManifestWorkLedgerService& workLedgerService_;
    ArtifactService& artifacts_;
    IManifestVerificationCommandRunner& commandRunner_;
    std::filesystem::path trustedScratchRoot_;
    ExecutionManifestExecutor executor_;
};

} // namespace ai::application
