#pragma once

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/verification/IManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/verification/ManifestVerificationContinuationTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

#include <string_view>
#include <vector>

namespace ai::application {

// MW-P5.6D: the Backend-owned normalization policy - a deliberately narrow,
// hardcoded allowlist, never Run/config-supplied (widening it requires a
// code change and a new policyId, so a stored proof always names exactly
// which rule set produced it). Currently covers only Qt Creator's own IDE
// housekeeping directory, the real drift that first motivated this package
// (see 75afa2b's WorkspaceReadPolicy fix and Run
// bb56e001-eac0-4b59-8d0b-d037c6bc1031).
// MW-P5.6D: proves - or refuses to prove - that a source Run's observed
// workspace baseline drift is limited to the Backend-owned approved
// prefixes above, and durably records the proof so
// ManifestVerificationContinuationPreparationService::prepare() never
// re-derives it from scratch on a retry. Never mutates the source Run's own
// durable baseline/overlay identity or Work Ledger verification record -
// see ManifestVerificationBaselineNormalization's own doc comment for the
// exact non-goals this stays inside.
class ManifestVerificationLegacyBaselineNormalizationService final {
public:
    ManifestVerificationLegacyBaselineNormalizationService(
        IManifestVerificationBaselineNormalizationRepository& normalizations,
        ArtifactService& artifacts,
        ChangeProposalService& proposals);

    // `sourceRunId`'s durable workspace-baseline.v1.json artifact is
    // compared against `currentBaseline` (the caller's own fresh capture,
    // already known to mismatch `durableBaselineFingerprint`). The artifact
    // must itself hash to that exact Work-Ledger-owned durable fingerprint;
    // a different valid artifact can never mint a proof. Eligibility
    // requires ALL of:
    //   - exactly one durable baseline artifact is found for the Run;
    //   - every added/modified/deleted path in the diff falls under one of
    //     kApprovedLegacyBaselineNormalizationPrefixes;
    //   - every `sourceProposalId` still belongs to sourceRunId, remains
    //     ReadyForReview, is unique, and none of its own changes
    //     (relativePath or sourcePath) falls under an approved prefix - this
    //     is rechecked even when an immutable proof already exists;
    //   - pruning the durable baseline by the approved prefixes reproduces
    //     `currentBaseline.fingerprint` exactly (a self-consistency check,
    //     defense in depth beyond the per-path prefix check above).
    // Returns std::nullopt (success) when any of the above does not hold -
    // the caller must fail with its own ordinary baseline_drift code, never
    // a distinct "ineligible" error. A genuine repository/internal error
    // (not "ineligible") is returned as a failure.
    [[nodiscard]] common::Result<std::optional<ManifestVerificationBaselineNormalization>>
    normalizeOrEvaluate(
        const RunId& sourceRunId,
        const std::string& durableBaselineFingerprint,
        const WorkspaceBaselineSnapshot& currentBaseline,
        const std::vector<ChangeSetId>& sourceProposalIds);

    // The read policy under which a live capture is comparable with the
    // baseline `ownerRunId` recorded: current static defaults plus the
    // dynamic `path:` exclusions frozen in that Run's own baseline artifact
    // (see RecordedBaselinePolicy for the owner/identity contract and the
    // distinct outcomes). Lives here only because this service already owns
    // the durable-baseline artifact access of the continuation and handoff
    // flows. It does not change normalization: `expectedFingerprint` is the
    // ORIGINAL fingerprint the artifact carries, never a normalised or
    // pinned one.
    [[nodiscard]] common::Result<WorkspaceReadPolicy> policyForRecordedBaseline(
        const RunId& ownerRunId,
        const std::string& expectedFingerprint,
        std::filesystem::path workspaceRoot) const;

    // Stores a baseline snapshot as `runId`'s own baseline artifact (see
    // RecordedBaselinePolicy::persist); same access reason as above.
    [[nodiscard]] common::Result<void> persistRecordedBaseline(
        const RunId& runId,
        const std::optional<StageId>& stageId,
        const WorkspaceBaselineSnapshot& snapshot) const;

private:
    IManifestVerificationBaselineNormalizationRepository& normalizations_;
    ArtifactService& artifacts_;
    ChangeProposalService& proposals_;
};

} // namespace ai::application
