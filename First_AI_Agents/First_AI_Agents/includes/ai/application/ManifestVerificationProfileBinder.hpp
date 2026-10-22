#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/VerificationBootstrapTypes.hpp"
#include "ai/application/VerificationEnvironmentSnapshotTypes.hpp"
#include "ai/common/Result.hpp"

#include <string>
#include <vector>

namespace ai::application {

// AG-18.2: the single Backend-owned resolver/binder between a Planner's
// draft profile/build/target *selections* (provider-facing,
// verificationProfileId/verificationBuildProfileId/verificationTargetId on
// ExecutionAcceptanceRequirement) and the exact, immutable
// verificationArguments the existing verification pipeline
// (ManifestVerificationEnvironmentPreflight, ManifestProposalSetVerifier)
// already consumes unchanged. Every method here is a pure function over an
// already-loaded VerificationEnvironmentSnapshot - it never queries
// IVerificationEnvironmentRepository itself and never re-runs discovery;
// the caller (ManifestWorkflowRunner) owns the one repository lookup per
// Run, and the same loaded snapshot is threaded unchanged through the
// Planner's primary attempt and its one bounded repair attempt.
class ManifestVerificationProfileBinder final {
public:
    ManifestVerificationProfileBinder() = delete;

    // Fail-closed identity/shape validation of an already-fetched snapshot
    // against the current Run's own identity, called once by
    // ManifestWorkflowRunner before a Planner session is ever created.
    // Checks: schema is supported; snapshot.runId/projectId/
    // baselineFingerprint match the current Run exactly; state/content
    // agree (ExistingProfile iff at least one profile); profile ids are
    // unique; build-preset names and advertised target names are unique
    // within each profile; every candidate-relative build path
    // (declaredBinaryDir/resolvedBinaryDir) is bounded and never absolute
    // or path-traversing. A repository read failure is the caller's own
    // concern (a hard failure, never routed through here as if it were an
    // empty catalog) - this function only validates a snapshot the caller
    // already successfully obtained.
    [[nodiscard]] static common::Result<void> validateSnapshotIdentity(
        const VerificationEnvironmentSnapshot& snapshot,
        const RunId& runId,
        const ProjectId& projectId,
        const std::string& baselineFingerprint);

    // AG-18.3: the structural-only subset of validateSnapshotIdentity's
    // checks (state/content agreement, bounded collections, id safety/
    // uniqueness, cross-links, unsafe paths) with no Run/project/baseline
    // identity comparison at all - reused for a freshly-discovered
    // candidate-overlay snapshot, which has no Run identity of its own to
    // compare against (see
    // VerificationEnvironmentDiscoveryService::discoverForCandidateRoot).
    [[nodiscard]] static common::Result<void> validateSnapshotStructure(
        const VerificationEnvironmentSnapshot& snapshot);

    // Deterministic, bounded prompt text advertising exactly what the
    // Planner may select: stable profile ids, their linked build-profile
    // ids, confirmed target allow-lists, buildPresetsConfirmed/
    // allowedTargetsConfirmed availability flags, a safe source/provenance
    // label, and whether a candidate-relative build directory is available.
    // Never an absolute host path, raw executable path, shell string,
    // environment variable, raw preset JSON, or arbitrary command
    // argument. A SetupRequired/ProposedBootstrap snapshot renders a short
    // explanatory note instead of any profile.
    [[nodiscard]] static std::string renderTrustedCatalog(
        const VerificationEnvironmentSnapshot& snapshot);

    // Resolves every task's acceptanceContract requirement's provider
    // selections against snapshot into exact, immutable
    // verificationArguments, in task/requirement order, stopping at the
    // first invalid selection (unknown, duplicated in spirit, unconfirmed,
    // or incompatible with its verificationKind). On success, every
    // resolved requirement also re-passes
    // validateExecutionVerificationProfileArguments (the existing,
    // unchanged final-shape check) as a defense-in-depth confirmation that
    // binding produced exactly the shape the rest of the verification
    // pipeline already expects. On failure, appends exactly one
    // ExecutionManifestFinding (with the owning task id) to findings and
    // returns a matching common::Result<void> failure - manifest itself is
    // left in a partially-bound, never-frozen state that the caller must
    // discard (it participates in the Planner's existing repair mechanism
    // exactly like any other rejected candidate).
    [[nodiscard]] static common::Result<void> bindManifest(
        ExecutionManifest& manifest,
        const VerificationEnvironmentSnapshot& snapshot,
        std::vector<ExecutionManifestFinding>& findings);

    // AG-18.3: resolves every requirement with verificationBootstrapPending
    // still true against a freshly-discovered CANDIDATE snapshot (the
    // combined proposal overlay's own rediscovery, never the live
    // workspace's own frozen snapshot). Requires candidateSnapshot to carry
    // exactly one usable profile with exactly one confirmed build preset -
    // ambiguity (zero or more than one of either) is rejected rather than
    // guessed, since a deferred requirement never carried a pre-selected id
    // to disambiguate with (none existed yet). For qml_lint, the single
    // build preset's confirmed allow-list must also carry exactly one
    // target for the same reason. On success, every previously-deferred
    // requirement's verificationBootstrapPending flips back to false and
    // verificationArguments is filled in exactly like bindManifest's normal
    // path; the returned identity records which candidate profile/build-
    // profile/target were actually used, for durable provenance. On
    // failure, appends one ExecutionManifestFinding and returns a matching
    // failure - the manifest is left partially bound and must be treated
    // as a non-green BootstrapFailed outcome, never partially applied.
    [[nodiscard]] static common::Result<VerificationBootstrapCandidateIdentity>
    bindDeferredBootstrapRequirements(
        ExecutionManifest& manifest,
        const VerificationEnvironmentSnapshot& candidateSnapshot,
        std::vector<ExecutionManifestFinding>& findings);
};

} // namespace ai::application
