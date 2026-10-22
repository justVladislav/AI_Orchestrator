#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kVerificationBootstrapDecisionSchemaVersion =
    "verification-bootstrap-decision.v1";
inline constexpr std::string_view kVerificationBootstrapTransitionSchemaVersion =
    "verification-bootstrap-transition.v1";
inline constexpr std::string_view kVerificationBootstrapRetryLinkSchemaVersion =
    "verification-bootstrap-retry-link.v1";

inline constexpr std::size_t kVerificationBootstrapMaxSetupPaths = 4;
inline constexpr std::size_t kVerificationBootstrapMaxOwningTasks = 4;
inline constexpr std::size_t kVerificationBootstrapMaxMessageBytes = 2000;
inline constexpr std::size_t kVerificationBootstrapMaxReasonCodeBytes = 128;

// AG-18.3: the disposition of a Manifest Run whose AG-18.1 snapshot has no
// existing usable verification profile (VerificationEnvironmentState::
// SetupRequired). ExistingProfile is not represented here at all - it is
// the unchanged AG-18.2 path, which never creates a VerificationBootstrapDecision.
enum class VerificationBootstrapDisposition {
    // A same-Run bootstrap was safely declared and admitted by the frozen
    // Manifest (see ManifestBootstrapValidator); its resulting profile is
    // not available until the worker proposals are composed into the
    // candidate overlay and rediscovered there.
    BootstrapPending,
    // No safe same-Run bootstrap was declared or possible; the Run stopped
    // before worker spend with a durable, non-green disposition.
    ActionRequired,
    // The candidate overlay produced a real Backend-discovered profile and
    // every deferred verification requirement passed against it.
    BootstrapVerified,
    // The candidate overlay's bootstrap could not be verified (missing/
    // malformed preset, ambiguous or absent candidate profile, or a failed
    // configure/build/lint) - remains non-green and never unlocks Apply.
    BootstrapFailed,
};

[[nodiscard]] const char* toString(VerificationBootstrapDisposition value) noexcept;
[[nodiscard]] std::optional<VerificationBootstrapDisposition>
verificationBootstrapDispositionFromString(std::string_view value) noexcept;

// Backend-synthesized (never provider-authored) identity of the candidate
// verification result once a BootstrapPending Manifest's overlay has been
// rediscovered and verified. Reuses the existing overlay's own content
// identity (overlayFingerprint) rather than inventing a second one.
struct VerificationBootstrapCandidateIdentity {
    std::string overlayFingerprint;
    std::string candidateProfileId;
    std::string candidateBuildProfileId;
    std::string candidateTargetId; // empty when the verified kind needed no target
};

// Immutable, once per Run: why this Run entered the bootstrap lifecycle
// (BootstrapPending or ActionRequired) instead of the plain AG-18.2 path.
// Never mutated afterward - later progress is recorded as an appended
// VerificationBootstrapTransition instead (see below), mirroring the
// existing execution_manifests / run_workspace_scopes "insert once, never
// updated" contract.
struct VerificationBootstrapDecision {
    std::string schemaVersion {std::string(kVerificationBootstrapDecisionSchemaVersion)};
    RunId runId;
    ProjectId projectId;
    // The original Run's own WorkspaceBaselineSnapshot::fingerprint and
    // AG-18.1 VerificationEnvironmentSnapshot schema version, captured at
    // the moment this decision was made - never re-derived later, and
    // never confused with any later candidate-overlay identity.
    std::string baselineFingerprint;
    std::string verificationSnapshotSchemaVersion;
    VerificationBootstrapDisposition disposition {VerificationBootstrapDisposition::ActionRequired};
    // Stable, bounded, leak-safe: e.g.
    // "verification_environment.action_required". Never a raw provider
    // rejection string, SQLite error, or absolute path.
    std::string reasonCode;
    std::string message;
    // Exact workspace-relative bootstrap setup paths this decision covers
    // (empty for ActionRequired). Restricted to the two Backend-understood
    // root preset files - see ManifestBootstrapValidator.
    std::vector<std::filesystem::path> bootstrapSetupPaths;
    std::vector<std::string> owningTaskIds;
    // Set only when this Run is itself a linked retry created after a
    // predecessor Run's ActionRequired decision.
    std::optional<RunId> predecessorRunId;
    std::chrono::system_clock::time_point decidedAt;
};

// Append-only progress record for a Run that already has a
// VerificationBootstrapDecision. Never replaces or mutates the original
// decision row.
struct VerificationBootstrapTransition {
    std::string schemaVersion {std::string(kVerificationBootstrapTransitionSchemaVersion)};
    std::string id;
    RunId runId;
    VerificationBootstrapDisposition toDisposition {
        VerificationBootstrapDisposition::BootstrapVerified};
    std::string reasonCode;
    std::string message;
    std::optional<VerificationBootstrapCandidateIdentity> candidateIdentity;
    std::chrono::system_clock::time_point transitionedAt;
};

// AG-18.3: durable linkage created when a new Run is started specifically
// to retry after a predecessor Run's ActionRequired disposition (manual/
// external setup, or a separately scoped setup Run). At most one retry
// link per predecessor Run - a second request for the same predecessor is
// an idempotent no-op that returns the existing link (see
// IVerificationBootstrapRepository::recordRetryLink).
struct VerificationBootstrapRetryLink {
    std::string schemaVersion {std::string(kVerificationBootstrapRetryLinkSchemaVersion)};
    RunId predecessorRunId;
    RunId newRunId;
    ProjectId projectId;
    std::string reason;
    std::chrono::system_clock::time_point createdAt;
};

} // namespace ai::application
