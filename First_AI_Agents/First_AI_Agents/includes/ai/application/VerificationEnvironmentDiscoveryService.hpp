#pragma once

#include "ai/application/IVerificationEnvironmentRepository.hpp"
#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/VerificationEnvironmentSnapshotTypes.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <stop_token>

namespace ai::application {

// AG-18.1: Backend-owned, provider-free discovery of the workspace's CMake
// verification environment, run once per Manifest-workflow Run before the
// Planner is ever invoked (see ManifestWorkflowRunner::run()). Persists
// exactly one immutable VerificationEnvironmentSnapshot per Run through
// IVerificationEnvironmentRepository - the same "discover, freeze, never
// duplicate" shape as RunWorkspaceScopeService.
//
// Deliberately does NOT feed its result into the Planner's prompt or gate
// the Run in any way (AG-18.2/18.3's job) - this package only guarantees the
// fact is discovered once, correctly, and durably recorded with baseline
// provenance. Every discovery-content outcome (no preset files, an unusable
// or unparseable preset file, cmake unavailable/timed out/failed) is
// therefore folded into the returned snapshot itself (state=SetupRequired
// plus a bounded diagnostic) rather than surfaced as a failure - only a
// genuine persistence failure (the snapshot could not be durably recorded)
// propagates as an error, mirroring how a WorkspaceBaselineSnapshot
// persistence failure already fails a Run in ManifestWorkflowRunner::run().
// The configure/build preset pair the user selected for the project (AG-18.4B-R1). When a Run's discovery is given one, the frozen
// snapshot - and so everything the Planner and the mandatory-verification policy may choose from - holds exactly that pair: a
// second configured profile can no longer make the QML-lint target ambiguous after the user chose.
struct VerificationProfileSelection {
    std::string configurePreset;
    std::string buildPreset;
};

class VerificationEnvironmentDiscoveryService {
public:
    explicit VerificationEnvironmentDiscoveryService(IVerificationEnvironmentRepository& repository);

    // Discovers and persists exactly once for runId. commandRunner is used
    // only for `cmake --list-presets=configure`/`--list-presets=build` - the
    // same bounded, read-only, argument-vector-only invocation
    // ManifestVerificationEnvironmentPreflight already trusts (never a
    // provider-authored shell string), and only when at least one of
    // CMakePresets.json/CMakeUserPresets.json exists at workspaceRoot.
    //
    // baseline is the Run's own already-captured WorkspaceBaselineSnapshot.
    // Before trusting anything read from disk, and again after all probes
    // finish but before persistence, discovery re-captures the
    // full policy-visible workspace via inspectPolicy (the same
    // WorkspaceReadPolicy/WorkspaceBaselineProvenance::capture the Run's own
    // baseline was built from) and requires
    // WorkspaceBaselineProvenance::compare(baseline, recaptured) to report
    // "unchanged". This is deliberately the whole workspace, not just the
    // two preset files: a preset can pull additional definitions in via
    // CMake's own `include` mechanism, which this Backend does not follow,
    // so only a full re-comparison reliably catches drift in whatever a
    // preset transitively depends on. Any drift means the workspace changed
    // before or during discovery (or was never policy-visible to begin with)
    // and is folded into a SetupRequired classification rather
    // than trusted - see the .cpp for the exact rule.
    //
    // A second call for the same runId is a benign idempotent success (see
    // IVerificationEnvironmentRepository::recordSnapshot) - the
    // already-recorded snapshot is re-read and returned unchanged.
    common::Result<VerificationEnvironmentSnapshot> discoverAndRecord(
        const RunId& runId,
        const ProjectId& projectId,
        const std::filesystem::path& workspaceRoot,
        const WorkspaceBaselineSnapshot& baseline,
        const WorkspaceReadPolicy& inspectPolicy,
        IManifestVerificationCommandRunner& commandRunner,
        std::stop_token stopToken,
        const std::optional<VerificationProfileSelection>& selection = std::nullopt);

    [[nodiscard]] common::Result<std::optional<VerificationEnvironmentSnapshot>> findForRun(
        const RunId& runId) const;

    // Cheap command-boundary preflight used by "Retry after setup". It
    // checks only whether at least one of the two well-known preset paths is
    // currently a regular file. It never launches cmake, mutates the
    // workspace, persists a snapshot, or claims that the file is valid - the
    // fresh retry Run still performs the complete baseline-bound discovery.
    // A filesystem inspection failure is surfaced as a typed error rather
    // than treated as permission to create a new Run.
    [[nodiscard]] static common::Result<bool> hasPresetFileAtWorkspaceRoot(
        const std::filesystem::path& workspaceRoot);

    // AG-18.3: the same provider-free profile/build-preset discovery,
    // applied to an explicitly-supplied isolated candidate root (a
    // ManifestProposalOverlay's own scratch root) instead of the Run's live
    // workspace. Deliberately bounded, non-persisting (never touches
    // IVerificationEnvironmentRepository), and carries no baseline-drift
    // check at all - a freshly-materialized overlay has no meaningful
    // "baseline" of its own to drift from; the caller is responsible for
    // identity-binding whatever result it decides to keep (see
    // ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements).
    // runId/projectId/baselineFingerprint on the returned snapshot are left
    // empty - this snapshot is never persisted or compared against a Run's
    // own identity, only used in-memory to resolve a bootstrap-pending
    // requirement.
    [[nodiscard]] static common::Result<VerificationEnvironmentSnapshot> discoverForCandidateRoot(
        const std::filesystem::path& candidateRoot,
        IManifestVerificationCommandRunner& commandRunner,
        std::stop_token stopToken);

private:
    IVerificationEnvironmentRepository& repository_;
};

} // namespace ai::application
