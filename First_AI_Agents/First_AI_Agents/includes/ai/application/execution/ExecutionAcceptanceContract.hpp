#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kExecutionAcceptanceSchemaVersion =
    "execution-acceptance.v1";
inline constexpr std::string_view kManifestProposalSetVerificationSchemaVersion =
    "manifest-proposal-set-verification.v1";

inline constexpr std::size_t kMaximumAcceptanceRequirements = 64;
inline constexpr std::size_t kMaximumRequiredOutputsPerRequirement = 32;
inline constexpr std::size_t kMaximumVerificationArguments = 16;
inline constexpr std::size_t kMaximumAcceptanceStringBytes = 4096;
inline constexpr std::size_t kMaximumVerificationDiagnostics = 128;
inline constexpr std::size_t kMaximumVerificationArtifactReferences = 32;

// The provider selects only one of these Backend-known profiles. Arguments are
// data interpreted by the profile; they are never concatenated into a shell
// command supplied by the provider.
enum class ExecutionVerificationKind {
    InspectPaths,
    CMakeBuild,
    QmlLint,
    CMakeTargetConsistency,
};

enum class ExecutionVerificationStatus {
    NotRun,
    Passed,
    Failed,
    NotVerifiable,
};

[[nodiscard]] const char* toString(ExecutionVerificationKind value) noexcept;
[[nodiscard]] std::optional<ExecutionVerificationKind>
executionVerificationKindFromString(std::string_view value) noexcept;
[[nodiscard]] const char* toString(ExecutionVerificationStatus value) noexcept;
[[nodiscard]] std::optional<ExecutionVerificationStatus>
executionVerificationStatusFromString(std::string_view value) noexcept;

struct ExecutionVerificationArgument {
    std::string name;
    std::string value;
};

// AG-17.0: who authored a frozen acceptance requirement. Only the Backend's
// mandatory-verification policy (manifest_mandatory_verification_v1) ever
// sets BackendMandatory, and only on a requirement it actually added. A
// provider submission can never carry this field (the draft/Manifest parsers
// reject unknown keys), and an absent value in an already-persisted Manifest
// means PlannerDeclared, exactly as before this field existed.
enum class ExecutionRequirementOrigin {
    PlannerDeclared,
    BackendMandatory,
};

[[nodiscard]] const char* toString(ExecutionRequirementOrigin value) noexcept;
[[nodiscard]] std::optional<ExecutionRequirementOrigin>
executionRequirementOriginFromString(std::string_view value) noexcept;

struct ExecutionAcceptanceRequirement {
    std::string id;
    std::string description;
    std::vector<std::string> requiredOutputs;
    ExecutionVerificationKind verificationKind {ExecutionVerificationKind::InspectPaths};
    // Backend-resolved exact arguments for cmake_build/qml_lint (never
    // provider-authored). cmake_target_consistency's task-owned
    // definitionFile/registrationFile/target shape is likewise never
    // provider-authored in a FRESH submission as of the Manifest
    // verification reliability hardening package:
    // ExecutionManifestParser fail-closed rejects any new submission using
    // this kind (acceptance.untrusted_cmake_consistency_selection) before it
    // can ever reach this struct via that path - there is no Backend-owned
    // trusted catalog of admissible definition/registration/target triples
    // yet. This shape remains reachable only via an already-persisted
    // legacy/trusted Manifest (read through a separate deserialization path
    // that never calls the parser) or a programmatically-constructed
    // trusted contract, both of which the internal verifier still validates.
    std::vector<ExecutionVerificationArgument> verificationArguments;
    bool required {true};
    // AG-18.2: provider-submitted stable selections from the Backend's
    // trusted VerificationEnvironmentSnapshot catalog - never a raw preset,
    // buildPreset, buildDirectory, or target value. Resolved into
    // verificationArguments (above) by ManifestVerificationProfileBinder
    // before manifests_.freeze(); preserved afterward on the frozen
    // requirement as the durable record of what was actually selected.
    // Always null for inspect_paths; verificationProfileId/
    // verificationBuildProfileId are always null for
    // cmake_target_consistency (see validateExecutionVerificationDraftSelections
    // for the exact per-kind shape a provider submission must have before
    // binding, and ManifestVerificationProfileBinder for the actual
    // resolution). Deliberately declared after verificationArguments/
    // required, not alongside verificationKind, so every pre-AG-18.2
    // positional aggregate-initializer (id, description, requiredOutputs,
    // verificationKind, verificationArguments, required) elsewhere in this
    // codebase keeps compiling unchanged, with these three defaulting to
    // std::nullopt.
    std::optional<std::string> verificationProfileId;
    std::optional<std::string> verificationBuildProfileId;
    std::optional<std::string> verificationTargetId;
    // AG-18.3: true only when this requirement's verification is explicitly,
    // typedly deferred pending a same-Run bootstrap declared on the owning
    // ExecutionManifest::bootstrapDeclaration. Distinct from an unbound
    // draft (all three ids above stay null and verificationArguments stays
    // empty exactly as a fresh submission would) - the difference is this
    // flag itself, which only the full manifest-level bootstrap admission
    // predicate (ManifestBootstrapValidator) may legitimately let reach
    // freeze() as true. Never true for inspect_paths or
    // cmake_target_consistency. Always false again once
    // ManifestVerificationProfileBinder has bound a real candidate profile
    // against it.
    bool verificationBootstrapPending {false};
    // Backend-owned QML-lint diagnostic policy frozen with the bound
    // requirement before Manifest freeze. Fresh provider submissions must
    // leave this null; ManifestVerificationProfileBinder supplies the stable
    // id. A missing id on an already-persisted requirement is the explicit
    // legacy strict policy, preserving pre-hardening Runs on replay/Reverify.
    // Kept after every pre-existing field so positional test/trusted
    // aggregate initializers preserve their original meaning.
    std::optional<std::string> qmlLintDiagnosticPolicyId;
    // AG-17.0: see ExecutionRequirementOrigin. Declared last for the same
    // positional-initializer reason as qmlLintDiagnosticPolicyId above.
    ExecutionRequirementOrigin origin {ExecutionRequirementOrigin::PlannerDeclared};
};

struct ExecutionAcceptanceContract {
    std::string schemaVersion {std::string(kExecutionAcceptanceSchemaVersion)};
    std::vector<ExecutionAcceptanceRequirement> requirements;
};

// Safe, provider-neutral validation. Diagnostics never echo model strings or
// absolute paths.
[[nodiscard]] common::Result<void> validateExecutionAcceptanceContract(
    const ExecutionAcceptanceContract& contract);

// Manifest-admission validation for the Backend-known verification profiles.
// Kept separate from the structural contract validator so the post-execution
// verifier can still report legacy/corrupt persisted profiles as typed
// NotVerifiable results instead of discarding their diagnostics. Validates
// the FINAL, Backend-resolved verificationArguments shape - only meaningful
// after ManifestVerificationProfileBinder has run (or for a legacy
// pre-AG-18.2 persisted contract). Never call this on a fresh provider
// submission before binding: verificationArguments is legitimately empty
// for cmake_build/qml_lint at that point - see
// validateExecutionVerificationDraftSelections for the pre-binding check.
[[nodiscard]] common::Result<void> validateExecutionVerificationProfileArguments(
    const ExecutionAcceptanceRequirement& requirement);

// AG-18.2: pre-binding shape validation of a provider submission - which of
// verificationProfileId/verificationBuildProfileId/verificationTargetId must
// be present (non-null) or absent (null) for the requirement's kind, and
// whether raw verificationArguments are absent as required for every fresh
// provider submission. A fresh cmake_target_consistency requirement is
// rejected earlier by ExecutionManifestParser because the Backend has no
// trusted catalog for its definition/registration/target tuple; every other
// kind must carry an empty array at this stage, since any non-empty value is
// provider-authored environment data. Never validates that a submitted id is
// actually known to any catalog - that is ManifestVerificationProfileBinder's
// job, since only it has the trusted snapshot.
[[nodiscard]] common::Result<void> validateExecutionVerificationDraftSelections(
    const ExecutionAcceptanceRequirement& requirement);

// AG-18.2: the same requirement object is validated by
// ExecutionManifestValidator::validate() at two different points in the
// Planner's flow - once on the fresh, still-unbound provider submission
// (evaluateSubmission, before ManifestVerificationProfileBinder runs) and
// once more inside ExecutionManifestService::freeze() itself (after
// binding has resolved verificationArguments to its final shape) as
// freeze()'s own unconditional defense-in-depth re-validation. A
// requirement is therefore valid at this checkpoint whenever it satisfies
// EITHER validateExecutionVerificationDraftSelections (not yet bound) OR
// validateExecutionVerificationProfileArguments (already bound) - never
// neither. This is deliberately more permissive than either check alone;
// it is not a substitute for calling the strict draft check directly at
// the actual untrusted-input boundary (the parser, which never sees an
// already-bound requirement and must keep rejecting a provider that tries
// to submit one).
[[nodiscard]] common::Result<void> validateExecutionVerificationRequirementState(
    const ExecutionAcceptanceRequirement& requirement);

// Real-Run incident (Run 8081057a-a669-4f58-abec-952244624d74, "Stage
// 2A-canary 11", 2026-09-11): a bounded, per-line structural breakdown of
// `ManifestVerificationDiagnostic::message`'s already leak-safe, already
// byte/line-capped excerpt, produced from the exact same sanitized
// candidate lines the excerpt itself is built from (never a second,
// independent parse of raw command output). Populated only when a line
// matches the qmllint/compiler "<file>:<line>:<column>: <text> [<tag>]"
// shape; a line that does not match this shape is simply omitted from this
// list (the flat `message` excerpt remains the complete, authoritative
// record either way). Consumed by the bounded verification-repair prompt
// (ExecutionManifestExecutor.cpp) to group findings by file and attach a
// generic, tag-keyed correction hint, persisted in the verification identity
// JSON, and - since 2026-10-07 - named (bounded, candidate-relative display
// path only) in a Run's terminal message and in the verification report's
// Diagnostics section (QmlLintDiagnosticPolicy.hpp), so a baseline-aware lint
// failure whose flat message is fixed text still says what blocked.
// The persisted/displayed structured findings are a bounded sample: a diagnostic holding more than this
// many findings is stored with at most this many (per-file selection). A stored diagnostic with EXACTLY
// this many findings may therefore be complete or truncated - a reader must treat it as possibly
// truncated. The one definition for the verifier and every reader of the stored findings.
inline constexpr std::size_t kMaximumRecordedFindingsPerDiagnostic = 12;

struct ManifestVerificationDiagnosticFinding {
    std::string fileKey;
    int line {0};
    int column {0};
    std::string tag;
    std::string text;
};

struct ManifestVerificationDiagnostic {
    std::string code;
    std::string message;
    std::optional<std::string> requirementId;
    std::vector<ManifestVerificationDiagnosticFinding> structuredFindings;
};

struct ManifestVerificationCheckResult {
    std::string profileId;
    ExecutionVerificationKind kind {ExecutionVerificationKind::InspectPaths};
    ExecutionVerificationStatus status {ExecutionVerificationStatus::NotRun};
    std::string commandIdentity;
    std::optional<int> exitCode;
    std::string boundedOutputHash;
    std::optional<std::string> artifactRelativePath;
    std::uint64_t durationMilliseconds {0};
    std::vector<ManifestVerificationDiagnostic> diagnostics;
};

struct ManifestAcceptanceCriterionResult {
    std::string requirementId;
    bool required {true};
    ExecutionVerificationStatus status {ExecutionVerificationStatus::NotRun};
    std::vector<std::string> evidenceReferences;
};

// Immutable identity of the exact combined candidate that was checked.
struct ManifestProposalSetVerification {
    std::string schemaVersion {
        std::string(kManifestProposalSetVerificationSchemaVersion)};
    std::string workspaceIdentity;
    std::string baselineFingerprint;
    std::vector<ChangeSetId> orderedProposalIds;
    std::vector<std::string> orderedProposalContentHashes;
    std::string overlayFingerprint;

    ExecutionVerificationStatus proposalSafetyStatus {
        ExecutionVerificationStatus::NotRun};
    ExecutionVerificationStatus buildLintTestStatus {
        ExecutionVerificationStatus::NotRun};
    ExecutionVerificationStatus acceptanceStatus {
        ExecutionVerificationStatus::NotRun};

    std::vector<ManifestVerificationCheckResult> checks;
    std::vector<ManifestAcceptanceCriterionResult> criteria;
    std::vector<ManifestVerificationDiagnostic> diagnostics;
    std::vector<std::string> artifactRelativePaths;
};

[[nodiscard]] common::Result<void> validateManifestProposalSetVerification(
    const ManifestProposalSetVerification& verification);

[[nodiscard]] bool requiredAcceptancePassed(
    const ManifestProposalSetVerification& verification) noexcept;

// True when any check of this verification was REFUSED before a single command ran because the verification copy's path would
// exceed the Windows path budget of the build tools (ManifestProposalSetVerifier's overlayPathBudgetCheck, fail closed on purpose).
// Such a result says nothing about the code under test: callers must report the refusal itself and must not feed it to attribution,
// where it used to surface as the misleading "diagnostic has no structured file provenance".
[[nodiscard]] inline bool verificationRefusedForPathBudget(const ManifestProposalSetVerification& verification)
{
    for (const auto& check : verification.checks) {
        for (const auto& diagnostic : check.diagnostics) {
            if (diagnostic.code == "verification.overlay_path_too_long") return true;
        }
    }
    return false;
}
// AG-14.1: the full typed identity round-trip, reused both for the
// manifest-verification-identity.v1.json artifact
// ManifestWorkflowRunner::run() persists and for the Work Ledger's own
// verification_identity_json SQLite column - one shape, one implementation,
// never re-derived from manifest-verification.md prose. parseJson fails
// closed on malformed/truncated input rather than returning a
// plausible-but-wrong default-constructed value.
[[nodiscard]] std::string renderManifestProposalSetVerificationJson(
    const ManifestProposalSetVerification& verification);
[[nodiscard]] common::Result<ManifestProposalSetVerification>
parseManifestProposalSetVerificationJson(std::string_view json);

} // namespace ai::application
