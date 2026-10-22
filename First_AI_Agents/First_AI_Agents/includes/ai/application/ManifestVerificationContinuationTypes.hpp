#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kManifestVerificationContinuationSchemaVersion =
    "manifest-verification-continuation.v1";

// Whether a terminal Manifest Run's combined-verification failure was
// admitted for MW-P5 continuation. Computed once per source Run (see
// ManifestVerificationContinuationService::admitOrEvaluate) - idempotent on
// repeat calls for the same sourceRunId, never silently re-evaluated.
enum class ManifestVerificationContinuationStatus {
    Admitted,
    Denied,
};

[[nodiscard]] const char* toString(ManifestVerificationContinuationStatus value) noexcept;
[[nodiscard]] std::optional<ManifestVerificationContinuationStatus>
manifestVerificationContinuationStatusFromString(std::string_view value) noexcept;

// One durable, immutable admission decision for a terminal Manifest Run's
// combined-verification failure (MW-P5.1). Deliberately does NOT copy the
// source Run's frozen Manifest, proposals, or verification identity: those
// remain authoritative in their own already-durable repositories
// (IExecutionManifestRepository, IChangeProposalRepository,
// IManifestWorkLedgerRepository via ManifestWorkLedgerEntry::verification)
// and are always re-read by sourceRunId, never duplicated here - the
// "reference immutable source ids/hashes, never copy" principle MW-P5's own
// non-goals require (see ARCHITECTURE_GAPS.md's MW-P5 section). This row is
// the audit trail of the admission decision itself: was this source Run's
// failure ever evaluated for continuation eligibility, and what was
// decided - at most once, ever, per source Run.
struct ManifestVerificationContinuationCheckpoint {
    std::string schemaVersion {std::string(kManifestVerificationContinuationSchemaVersion)};
    // Deterministic, derived from sourceRunId (see
    // ManifestVerificationContinuationService) - there is at most one
    // checkpoint per source Run, so a random identity would add nothing.
    std::string id;
    RunId sourceRunId;
    ProjectId sourceProjectId;
    ManifestVerificationContinuationStatus status {
        ManifestVerificationContinuationStatus::Denied};
    // Denied only: one of ManifestVerificationContinuationService's stable
    // "verification_continuation.*" codes. Empty when Admitted.
    std::string denialCode;
    std::string denialMessage;
    std::chrono::system_clock::time_point createdAt;
};

inline constexpr std::string_view kManifestVerificationAttributionSchemaVersion =
    "manifest-verification-continuation-attribution.v1";

enum class ManifestVerificationFindingAttributionKind {
    SourceProposalOwner,
    IntegrationOwner,
    PreExistingVerificationDebt,
    IgnorableCascade,
};

[[nodiscard]] const char* toString(ManifestVerificationFindingAttributionKind value) noexcept;
[[nodiscard]] std::optional<ManifestVerificationFindingAttributionKind>
manifestVerificationFindingAttributionKindFromString(std::string_view value) noexcept;

enum class ManifestVerificationAttributionStatus {
    Attributed,
    Blocked,
};

[[nodiscard]] const char* toString(ManifestVerificationAttributionStatus value) noexcept;
[[nodiscard]] std::optional<ManifestVerificationAttributionStatus>
manifestVerificationAttributionStatusFromString(std::string_view value) noexcept;

// One normalized, leak-safe attribution for a source verification diagnostic.
// relativePath is always candidate-relative and canonical; raw command output,
// absolute paths, and provider payloads are never persisted here.
struct ManifestVerificationFindingAttribution {
    std::string diagnosticCode;
    std::string requirementId;
    std::string relativePath;
    int line {0};
    int column {0};
    std::string tag;
    ManifestVerificationFindingAttributionKind kind {
        ManifestVerificationFindingAttributionKind::SourceProposalOwner};
    std::optional<std::string> ownerTaskId;
    std::optional<ChangeSetId> ownerProposalId;
};

// MW-P5.2's immutable result. A Blocked row is durable too: repeated attempts
// cannot silently reinterpret the same source verification after repository or
// environment changes. The source Manifest/proposals remain references, never
// copied into this row.
struct ManifestVerificationAttributionCheckpoint {
    std::string schemaVersion {std::string(kManifestVerificationAttributionSchemaVersion)};
    std::string id;
    RunId sourceRunId;
    ProjectId sourceProjectId;
    ManifestVerificationAttributionStatus status {
        ManifestVerificationAttributionStatus::Blocked};
    std::string blockCode;
    std::string blockMessage;
    std::string baselineFingerprint;
    std::string sourceOverlayFingerprint;
    std::vector<ManifestVerificationFindingAttribution> findings;
    std::chrono::system_clock::time_point createdAt;
};

inline constexpr std::string_view kManifestVerificationContinuationInputSchemaVersion =
    "manifest-verification-continuation-input.v1";

// Immutable P5.3 binding between an admitted source checkpoint and the exact
// prepared child Run. Proposal bodies are never copied: ids and canonical
// content hashes reference the source set, while inputFingerprint binds all
// identities into one deterministic restart-safe value.
struct ManifestVerificationContinuationInput {
    std::string schemaVersion {std::string(kManifestVerificationContinuationInputSchemaVersion)};
    RunId sourceRunId;
    RunId childRunId;
    ProjectId sourceProjectId;
    std::string attributionId;
    std::string baselineFingerprint;
    std::string sourceWorkspaceIdentity;
    std::string sourceOverlayFingerprint;
    std::vector<ChangeSetId> orderedProposalIds;
    std::vector<std::string> orderedProposalContentHashes;
    std::string inputFingerprint;
    std::uint64_t plannerRequests {0};
    std::chrono::system_clock::time_point createdAt;
};

[[nodiscard]] common::Result<void> validateManifestVerificationContinuationInput(
    const ManifestVerificationContinuationInput& input);
[[nodiscard]] std::string computeManifestVerificationContinuationInputFingerprint(
    const ManifestVerificationContinuationInput& input);

[[nodiscard]] common::Result<void> validateManifestVerificationAttributionCheckpoint(
    const ManifestVerificationAttributionCheckpoint& checkpoint);

inline constexpr std::string_view kManifestVerificationBaselineNormalizationSchemaVersion =
    "manifest-verification-continuation-baseline-normalization.v1";
inline constexpr std::string_view kLegacyBaselineNormalizationPolicyId =
    "legacy-normalization.v1";
inline const std::vector<std::string> kApprovedLegacyBaselineNormalizationPrefixes = {
    ".qtcreator/"};

// MW-P5.6D: a durable, immutable proof that a source Run's workspace baseline
// drift (observed at prepare() time, against the durable baseline captured
// when the source Run itself ran) was limited to a Backend-owned,
// pre-approved set of path prefixes - never a general "ignore drift" escape
// hatch. Written at most once per source Run by
// ManifestVerificationLegacyBaselineNormalizationService. Never rewrites the
// source Run's own durable baseline/overlay fingerprints (Work Ledger
// verification identity, the frozen ManifestVerificationAttributionCheckpoint)
// - this row is a separate, additive equivalence proof that
// ManifestVerificationContinuationPreparationService::prepare() consults only
// to decide whether the continuation's OWN frozen
// ManifestVerificationContinuationInput may legitimately be pinned to the
// current (not the source Run's original) baseline/overlay identity.
struct ManifestVerificationBaselineNormalization {
    std::string schemaVersion {
        std::string(kManifestVerificationBaselineNormalizationSchemaVersion)};
    // Deterministic, derived from sourceRunId - at most one proof per source
    // Run, same convention as ManifestVerificationContinuationCheckpoint::id.
    std::string id;
    RunId sourceRunId;
    // Names the exact Backend-owned prefix list that was applied - never
    // Run/config-supplied. A future widened policy gets a new policyId, so a
    // stored proof always names the exact rule set it was produced under.
    std::string policyId;
    std::string durableBaselineFingerprint;
    std::string normalizedBaselineFingerprint;
    std::vector<std::string> excludedPrefixes;
    // The union of added/modified/deleted paths the drift consisted of -
    // audit evidence only, never re-derived from or compared against later.
    std::vector<std::string> normalizedPaths;
    std::chrono::system_clock::time_point createdAt;
};

[[nodiscard]] common::Result<void> validateManifestVerificationBaselineNormalization(
    const ManifestVerificationBaselineNormalization& normalization);

} // namespace ai::application
