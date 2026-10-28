#pragma once

#include "ai/application/execution/IExecutionManifestRepository.hpp"
#include "ai/application/verification/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/verification/IVerificationScratchRootProvider.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/verification/ManifestProposalSetVerifier.hpp"
#include "ai/application/verification/ManifestVerificationAttributionService.hpp"
#include "ai/application/verification/ManifestVerificationContinuationService.hpp"
#include "ai/application/verification/ManifestVerificationLegacyBaselineNormalizationService.hpp"

namespace ai::application {

class IOutcomeBriefTrigger;

inline constexpr std::string_view kManifestVerificationContinuationWorkflowId =
    "execution-manifest-verification-continuation-v1";

struct ManifestVerificationContinuationPreparationResult {
    Run childRun;
    ManifestVerificationContinuationInput input;
    ManifestVerificationAttributionCheckpoint attribution;
};

struct ManifestVerificationContinuationRecoveryResult {
    std::vector<RunId> recreatedChildRunIds;
};

// MW-P5.3's only command boundary. It performs no provider request and never
// calls a Planner. It reconstructs both source-only and exact-source-proposal
// candidates with the existing overlay/verifier, asks P5.2 for authoritative
// attribution, freezes one immutable input, and creates a non-executing child
// Run in Prepared state. P5.4 alone may execute that child.
class ManifestVerificationContinuationPreparationService final {
public:
    ManifestVerificationContinuationPreparationService(
        IRunRepository& runs,
        IProjectRepository& projects,
        IExecutionManifestRepository& manifests,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationContinuationRepository& continuations,
        ManifestVerificationContinuationService& admission,
        ManifestVerificationAttributionService& attribution,
        ChangeProposalService& proposals,
        IManifestVerificationCommandRunner& commandRunner,
        ManifestVerificationLegacyBaselineNormalizationService& baselineNormalization,
        std::filesystem::path trustedScratchRoot = {});

    [[nodiscard]] common::Result<ManifestVerificationContinuationPreparationResult>
    prepare(const RunId& sourceRunId, std::stop_token stopToken = {});

    // Recreates only a missing Prepared child for an already immutable input.
    // It never repeats verification or issues provider work.
    [[nodiscard]] common::Result<ManifestVerificationContinuationRecoveryResult>
    reconcileMissingChildren();

    // PC-4C: the best-effort nudge to the Outcome Brief reconciler (see IOutcomeBriefTrigger). Set once while the runtime is
    // composed, before any concurrent use; null (the default) means no nudge.
    void setOutcomeBriefTrigger(IOutcomeBriefTrigger* trigger) noexcept { outcomeBriefTrigger_ = trigger; }
    // So that the composition root's wiring can be asserted: this nudge fires only at the end of a verified, paid flow.
    [[nodiscard]] IOutcomeBriefTrigger* outcomeBriefTrigger() const noexcept { return outcomeBriefTrigger_; }

    // The validated root of the temporary verification copies. Set once while the runtime is composed; null keeps the constructor's
    // root unchanged. With a provider prepare() is refused - before anything is copied - when the root is not usable.
    void setVerificationScratchProvider(const IVerificationScratchRootProvider* provider) noexcept
    {
        scratchProvider_ = provider;
    }

private:
    IOutcomeBriefTrigger* outcomeBriefTrigger_ {nullptr};
    const IVerificationScratchRootProvider* scratchProvider_ {nullptr};
    [[nodiscard]] common::Result<Run> ensurePreparedChild(
        const ManifestVerificationContinuationInput& input,
        const Run& sourceRun);

    IRunRepository& runs_;
    IProjectRepository& projects_;
    IExecutionManifestRepository& manifests_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationContinuationRepository& continuations_;
    ManifestVerificationContinuationService& admission_;
    ManifestVerificationAttributionService& attribution_;
    ChangeProposalService& proposals_;
    IManifestVerificationCommandRunner& commandRunner_;
    ManifestVerificationLegacyBaselineNormalizationService& baselineNormalization_;
    std::filesystem::path trustedScratchRoot_;
};

} // namespace ai::application
