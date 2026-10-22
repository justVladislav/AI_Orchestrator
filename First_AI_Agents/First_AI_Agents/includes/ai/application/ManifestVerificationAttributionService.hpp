#pragma once

#include "ai/application/IExecutionManifestRepository.hpp"
#include "ai/application/IManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunWorkspaceScopeRepository.hpp"
#include "ai/application/ManifestVerificationContinuationTypes.hpp"

namespace ai::application {

// MW-P5.2: turns two trusted results produced by the same Backend verifier
// (source baseline and exact proposal overlay) into one durable, fail-closed
// diagnostic attribution. It never parses prose and never calls a provider.
//
// MW-P5.6C: ownership resolution consults the frozen AG-15 RunWorkspaceScope
// as a cross-check/fallback alongside the frozen Manifest's own
// ownedPaths/plannedCreatePaths - never a replacement for it. This closes a
// real gap: a Manifest whose AG-15 path-role fields were silently dropped by
// a since-fixed persistence bug (see SqliteRepositories.cpp's
// executionManifestToJson/FromJson) still has its correct ownership recorded
// in the separately-persisted, immutable run_workspace_scopes row, since that
// row is derived directly from the in-memory Manifest at freeze time, before
// any persistence round-trip could lose it. When that row exists, the service
// validates the complete immutable scope (identity, bounds, every entry,
// ownership uniqueness, and derived dependency edges) before classifying any
// finding; it never trusts only the one entry needed by the current lookup.
//
// MW-P5.6D: the durable source verification's baseline/overlay fingerprints
// were captured once, permanently, when the source Run originally ran - a
// later-approved legacy baseline normalization (see
// ManifestVerificationLegacyBaselineNormalizationService) legitimately makes
// a freshly reconstructed candidate/baseline verification's own
// baselineFingerprint (and, as an unavoidable consequence, overlayFingerprint)
// differ from that frozen durable value. sameSourceIdentity() only accepts
// such a difference when it is pinned to an existing immutable
// ManifestVerificationBaselineNormalization proof for this exact source Run
// (durable side matches the proof's own durable fingerprint, observed side
// matches its normalized fingerprint) - never a blanket "ignore fingerprint
// drift" allowance.
class ManifestVerificationAttributionService final {
public:
    ManifestVerificationAttributionService(
        IExecutionManifestRepository& manifests,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationContinuationRepository& continuations,
        IRunWorkspaceScopeRepository& workspaceScopes,
        IManifestVerificationBaselineNormalizationRepository& baselineNormalizations);

    // baselineVerification must be the source-only overlay result and
    // candidateVerification the freshly reconstructed exact source proposal
    // set result. P5.3 owns their materialization/execution; this boundary
    // validates both identities before persisting any conclusion.
    [[nodiscard]] common::Result<ManifestVerificationAttributionCheckpoint>
    attributeOrEvaluate(
        const RunId& sourceRunId,
        const ManifestProposalSetVerification& baselineVerification,
        const ManifestProposalSetVerification& candidateVerification);

    // QML-P4R.2: the mixed-run counterpart of attributeOrEvaluate above, for
    // a verificationAnchorRunId that is itself a paid continuation child
    // rather than the Manifest-owning Run. Cached/keyed by the ANCHOR (never
    // colliding with any attribution checkpoint the origin's own paid
    // continuation prepare() already created and cached under
    // originManifestRunId - that checkpoint reflects the origin's OWN
    // source-only candidate, never this anchor's mixed one). Reads the
    // admission checkpoint, frozen Manifest, frozen workspace scope, and
    // baseline-normalization proof from originManifestRunId (properties of
    // the Manifest/workspace itself), but the Work Ledger entry - the actual
    // durable evidence being attributed - from verificationAnchorRunId. A
    // real mixed-run anchor's own admission checkpoint always already exists
    // under originManifestRunId (prepare() could not have created this
    // anchor's own continuation input otherwise), so this never itself
    // durably admits anything new - it only attributes.
    [[nodiscard]] common::Result<ManifestVerificationAttributionCheckpoint>
    attributeOrEvaluateForAnchor(
        const RunId& verificationAnchorRunId,
        const RunId& originManifestRunId,
        const ManifestProposalSetVerification& baselineVerification,
        const ManifestProposalSetVerification& candidateVerification);

private:
    IExecutionManifestRepository& manifests_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationContinuationRepository& continuations_;
    IRunWorkspaceScopeRepository& workspaceScopes_;
    IManifestVerificationBaselineNormalizationRepository& baselineNormalizations_;
};

} // namespace ai::application
