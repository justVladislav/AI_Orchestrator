#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/execution/ExecutionAcceptanceContract.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kExecutionManifestSchemaVersion =
    "execution-manifest.v1";

enum class WorkspaceCapability {
    None,
    Inspect,
    Propose
};

[[nodiscard]] const char* toString(WorkspaceCapability capability) noexcept;
[[nodiscard]] std::optional<WorkspaceCapability> workspaceCapabilityFromString(
    std::string_view value) noexcept;

// AG-15: a bounded, Planner-authored note on one explicitly excluded or
// deferred path. `reason` is genuinely model-authored (unlike
// RunWorkspaceScopeEntry::reason, which is Backend-synthesized) since it
// explains a deliberate planning decision that only the Planner can state -
// never used as an authorization input, only as an audit annotation.
struct ManifestExcludedOrDeferredPath {
    std::filesystem::path path;
    std::string reason;
};

struct ExecutionManifestTask {
    std::string id;
    std::string objective;
    std::string role;
    std::vector<std::string> dependencies;
    // AG-15 workspace-scope roles for this task. `ownedPaths` ("owned_write")
    // and `plannedCreatePaths` ("planned_create") are permitted write
    // targets - existing and not-yet-existing baseline paths respectively
    // (RunWorkspaceScopeService::freezeForManifest enforces the baseline
    // existence/absence split; the validator only checks path safety and
    // role conflicts). `requiredReadPaths` ("required_read") must be
    // successfully inspected in the same worker attempt before a proposal
    // touching them is accepted (see AgentToolTurnLoopRunner's
    // preflightRequiredReadPaths). `referencePaths` ("reference_only") are
    // bounded optional context with no read-gate. "Expected outputs" is
    // deliberately not a separate field - it is the union of ownedPaths and
    // plannedCreatePaths (what a task may write is what it is expected to
    // output). File-level dependency edges are Backend-derived from
    // requiredReadPaths against other tasks' write paths, never
    // Planner-declared, so they can never contradict the roles above.
    std::vector<std::filesystem::path> ownedPaths;
    std::vector<std::filesystem::path> plannedCreatePaths;
    std::vector<std::filesystem::path> requiredReadPaths;
    std::vector<std::filesystem::path> referencePaths;
    std::vector<ManifestExcludedOrDeferredPath> excludedPaths;
    std::vector<ManifestExcludedOrDeferredPath> deferredPaths;
    // Optional versioned contract used by deterministic verification. The
    // legacy prose list remains readable for execution-manifest.v1, but is
    // never silently promoted into verified requirements.
    std::optional<ExecutionAcceptanceContract> acceptanceContract;
    std::vector<std::string> acceptanceCriteria;
    std::vector<std::string> contextReferences;
    WorkspaceCapability requestedCapability {WorkspaceCapability::None};
    // MW-P3A package 2 (Run a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b): a
    // Backend-known SUBSET of requiredReadPaths above that must NEVER
    // derive a cross-task dependency edge (deriveManifestDependencyGraph
    // skips exactly these entries) - a read of the Run's immutable frozen
    // baseline as it exists now, never of another task's proposed output,
    // even when the same path also happens to be another task's write
    // target. Always empty for a manifest built via the legacy
    // ExecutionManifestParser (execution-manifest.v1), which never
    // populates this field and therefore keeps that pathway's existing
    // all-reads-are-edge-forming behavior exactly unchanged. Only
    // ExecutionManifestDraftCompiler populates this, as a real subset
    // copied verbatim from the draft's own baselineRequiredReadPaths.
    // Declared last so every existing positional aggregate initializer of
    // this struct elsewhere in the codebase keeps compiling unchanged.
    std::vector<std::filesystem::path> baselineOnlyReadPaths;
    // AG-17.1: the roster member (ManifestRoutingMember::memberId) that executes
    // this provider-backed task. The provider only ever names a memberId; the
    // Backend resolves provider/model/role/instructions/pricing from the frozen
    // Run assignment snapshot and records them in ExecutionManifest::routing.
    // Absent means "the single worker" of a single-assignment Manifest (the
    // pre-AG-17 behaviour, unchanged). Exactly one assignee per task; the data
    // model deliberately allows several tasks to share one member (a later
    // package relaxes today's one-task-per-member planning rule without a
    // migration). Declared last so positional initializers keep their meaning.
    std::optional<std::string> assigneeMemberId;
};

/**
 * Trusted, immutable ceiling copied into the accepted manifest. A planner may
 * request less authority than this envelope, but can never enlarge it.
 */
struct ExecutionManifestPolicy {
    std::uint32_t maxTasks {16};
    std::uint32_t maxDepth {8};
    std::uint32_t maxParallelism {4};
    // AG-15: shared ceiling applied independently to each of ownedPaths,
    // plannedCreatePaths, requiredReadPaths, and referencePaths.
    std::uint32_t maxOwnedPathsPerTask {64};
    std::uint32_t maxExcludedOrDeferredPathsPerTask {16};
    std::uint32_t maxAcceptanceCriteriaPerTask {16};
    std::uint32_t maxContextReferencesPerTask {32};
    WorkspaceCapability maximumCapability {WorkspaceCapability::Propose};
    // MW-P0: non-authoritative. These are only ever compared against the
    // Planner's own requestedTokenBudget/requestedCostMicros claim below
    // (see ExecutionManifestValidator), and only after planning has already
    // happened. The runtime spending authority for every provider request
    // in this Run - Planner primary/repair and every worker session - is
    // the Backend-frozen ai::application::ProviderExecutionBudgetPolicy on
    // Run::providerBudget, enforced continuously by ProviderSessionBudgetGuard.
    std::optional<std::uint64_t> tokenBudgetCeiling;
    std::optional<std::uint64_t> costMicrosCeiling;
};

// AG-18.3: the Planner's explicit, typed declaration that this Manifest
// creates or modifies the exact CMake setup file(s) needed to establish a
// verification profile, in the same Run, as ordinary reviewable
// owned/planned-create work. Absence of this field must never be inferred
// as consent to bootstrap - see ManifestBootstrapValidator for the full
// admission predicate a Manifest carrying it (or carrying any requirement
// with verificationBootstrapPending=true) must satisfy before it can ever
// be frozen.
struct ManifestBootstrapDeclaration {
    std::string bootstrapTaskId;
    std::vector<std::filesystem::path> setupPaths;
};

// AG-17.1: Backend-owned, frozen routing record. It is the immutable identity of
// WHO may execute the Manifest's provider-backed tasks, captured from the Run's
// already-frozen assignment snapshot. Never provider-authored, never re-derived
// after freeze (Reverify, continuation, UI read it). Full instructions are NOT
// duplicated here - only their SHA-256 - so a later reader can detect drift
// against the Run's assignment snapshot without a second copy of the text.
inline constexpr std::string_view kManifestRoutingSchemaVersion = "manifest-routing.v1";

enum class ManifestRoutingMode {
    // Exactly one member (the Architect): it plans and executes every task, and
    // a task without assigneeMemberId means that member. The unchanged
    // pre-AG-17 Manifest workflow.
    SingleAssignment,
    // One lead Architect (plans, never a task assignee) plus workers. Every
    // provider-backed task carries an explicit assigneeMemberId.
    Roster,
};

[[nodiscard]] const char* toString(ManifestRoutingMode value) noexcept;
[[nodiscard]] std::optional<ManifestRoutingMode> manifestRoutingModeFromString(
    std::string_view value) noexcept;

struct ManifestRoutingMember {
    // The roster member's stable id (StageModelAssignment::stageType).
    std::string memberId;
    std::string providerId;
    std::string modelId;
    std::string role;
    bool leadArchitect {false};
    // Whether the member may run workspace-backed (Inspect/Propose) tasks.
    bool workspaceAccess {false};
    // Lower-case hex SHA-256 of the member's exact instruction text (the hash of
    // the empty string when the member has none).
    std::string instructionsHash;
    // Stable ModelExecution position identity (StageModelAssignment::position).
    std::uint32_t position {0};
    std::uint64_t timeoutMs {0};
};

struct ManifestRoutingRecord {
    std::string schemaVersion {std::string(kManifestRoutingSchemaVersion)};
    ManifestRoutingMode mode {ManifestRoutingMode::SingleAssignment};
    std::vector<ManifestRoutingMember> members;
    // SHA-256 over the canonical member list (ManifestRouting::computeFingerprint);
    // recomputed and compared by the validator, so a tampered record cannot freeze.
    std::string fingerprint;
};

// AG-17.0: Backend-owned mandatory verification policy
// (ManifestMandatoryVerificationPolicy). The policy id participates in the
// planning-input fingerprint and is recorded on every frozen Manifest the
// policy evaluated, so a later reader (Reverify, continuation, UI, audit)
// reads what was decided and never re-derives it.
inline constexpr std::string_view kManifestMandatoryVerificationPolicyId =
    "manifest_mandatory_verification_v1";
// Only `auto` exists in AG-17.0 (a Backend constant). A persisted
// required/explicitly_disabled_with_reason override is the separate AG-17.0b.
inline constexpr std::string_view kManifestMandatoryVerificationModeAuto = "auto";

enum class MandatoryVerificationDecisionStatus {
    // The Manifest writes nothing this kind of verification is required for.
    NotTriggered,
    // The Backend added a requirement (origin=backend_mandatory). A repeated
    // evaluation that finds its own earlier requirement reports Injected
    // again, which keeps derive/merge idempotent.
    Injected,
    // An exact, required, planner-declared requirement already satisfied the
    // policy; nothing was added.
    SatisfiedByExisting,
    // The policy provably has nothing to require (stable reasonCode).
    NotApplicable,
};

[[nodiscard]] const char* toString(MandatoryVerificationDecisionStatus value) noexcept;
[[nodiscard]] std::optional<MandatoryVerificationDecisionStatus>
mandatoryVerificationDecisionStatusFromString(std::string_view value) noexcept;

struct ManifestMandatoryVerificationDecision {
    ExecutionVerificationKind kind {ExecutionVerificationKind::QmlLint};
    MandatoryVerificationDecisionStatus status {MandatoryVerificationDecisionStatus::NotTriggered};
    // Stable machine code explaining the status (why triggered/not applicable).
    std::string reasonCode;
    // Injected: the added requirement and its anchor task. SatisfiedByExisting:
    // the planner-declared requirement that satisfied the policy and its task.
    std::string requirementId;
    std::string taskId;
    // The selection identity the decision refers to; empty when none.
    std::string profileId;
    std::string buildProfileId;
    std::string targetId;
};

struct ManifestMandatoryVerificationRecord {
    std::string policyId {std::string(kManifestMandatoryVerificationPolicyId)};
    std::string mode {std::string(kManifestMandatoryVerificationModeAuto)};
    // Canonical order: qml_lint, then cmake_build.
    std::vector<ManifestMandatoryVerificationDecision> decisions;
};

/**
 * Versioned execution input frozen once per Run. Phase 2 only defines,
 * validates and persists this contract; it deliberately does not schedule or
 * execute tasks.
 */
struct ExecutionManifest {
    std::string schemaVersion {kExecutionManifestSchemaVersion};
    RunId runId;
    std::vector<ExecutionManifestTask> tasks;
    std::uint32_t requestedMaxParallelism {1};
    // MW-P0: subordinate, model-authored estimates only - never the runtime
    // authority. See ExecutionManifestPolicy::tokenBudgetCeiling's comment.
    std::optional<std::uint64_t> requestedTokenBudget;
    std::optional<std::uint64_t> requestedCostMicros;
    ExecutionManifestPolicy policy;
    std::chrono::system_clock::time_point createdAt;
    // AG-18.3: present only when the Planner explicitly declared a same-Run
    // verification-environment bootstrap. nullopt whenever no task's
    // acceptanceContract requirement carries verificationBootstrapPending -
    // that is the unchanged AG-18.2 ExistingProfile path.
    std::optional<ManifestBootstrapDeclaration> bootstrapDeclaration;
    // AG-17.0: nullopt for a Manifest frozen before the policy existed, for a
    // Backend-built repair Manifest that carries the source's record verbatim
    // or none, and whenever no verification environment snapshot was
    // available to evaluate against.
    std::optional<ManifestMandatoryVerificationRecord> mandatoryVerification;
    // AG-17.1: the frozen routing record. nullopt for a Manifest frozen before
    // AG-17.1 and for the legacy roster runner's own Manifests; every Manifest
    // frozen through ManifestWorkflowRunner carries one.
    std::optional<ManifestRoutingRecord> routing;
};

struct ExecutionManifestFinding {
    std::string code;
    std::string message;
    std::optional<std::string> taskId;
};

} // namespace ai::application
