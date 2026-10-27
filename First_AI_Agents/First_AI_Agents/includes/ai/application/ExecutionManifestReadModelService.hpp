#pragma once

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/IManifestTaskExecutionRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunWorkspaceScopeRepository.hpp"
#include "ai/application/IVerificationBootstrapRepository.hpp"
#include "ai/application/agents/IModelExecutionRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/IVerificationEnvironmentRepository.hpp"
#include "ai/application/ManifestMemberUsage.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// AG-15.3: one frozen-scope-or-approved-expansion entry, projected for this
// task only. `role` is the stable persisted string (WorkspaceScopeRole::
// toString), never re-derived or re-interpreted by a UI layer.
struct ManifestTaskScopeEntryView {
    std::string relativePath;
    std::string role;
    bool isExpansion {false};
};

// AG-15.3: one Backend-derived file-level dependency edge (see
// RunWorkspaceScopeService::freezeForManifest's own doc comment) - Run-level,
// not per-task, since it names both sides.
struct ManifestScopeDependencyEdgeView {
    std::string dependentTaskId;
    std::string providerTaskId;
    std::string relativePath;
};

// One manifest task joined with its latest known execution state - the
// UI-safe, denormalized shape a Qt/QML adapter should read manifest/task
// state through, the same role AgentToolReadModelService already plays for
// tool sessions/proposals. Never exposes an absolute filesystem path (owned
// paths are already workspace-relative in the frozen manifest) or anything
// from provider tool-call arguments.
struct ManifestTaskView {
    std::string id;
    std::string objective;
    std::string role;
    std::vector<std::string> dependencies;
    std::vector<std::string> ownedPaths;
    std::vector<std::string> acceptanceCriteria;
    // AG-05: the typed requirement *definitions* (id, description, required
    // outputs, verification kind, required/optional) this task was actually
    // frozen with - already durable via the frozen ExecutionManifest's own
    // persistence, this is purely the read-side projection that was missing.
    // acceptanceCriteria above stays the legacy prose list for
    // execution-manifest.v1 manifests that never carried a typed contract;
    // this is absent for exactly those.
    std::optional<ExecutionAcceptanceContract> acceptanceContract;
    std::vector<std::string> contextReferences;
    WorkspaceCapability requestedCapability {WorkspaceCapability::None};

    // Absent only for a task the scheduler never reached at all (should not
    // happen once a manifest is frozen and executed, but a caller must not
    // assume every manifest task id necessarily has a row yet - e.g. between
    // freeze and the first executor dispatch).
    ManifestTaskStatus status {ManifestTaskStatus::Pending};
    std::string errorCode;
    std::string errorMessage;
    std::optional<ChangeSetId> changeSetId;
    std::size_t attempts {0};
    std::string outputSummary;

    // AG-04: set only for a task whose worker actually resolved as no-op
    // (see ManifestTaskNoChangesRecord). Absent for every other task,
    // including one that never ran at all - never inferred from outputSummary.
    std::optional<ManifestWorkerNoChanges> workerNoChanges;

    // AG-15.3: workspace-scope projection, all Backend truth - a QML/UI
    // layer must never compute any of this itself. Empty whenever no
    // IRunWorkspaceScopeRepository was wired into the read model (see
    // ExecutionManifestReadModelService's constructor), never a silent
    // substitute for "genuinely no scope was frozen".
    //
    // scopeEntries: every frozen entry (any of the six roles) plus every
    // approved expansion (isExpansion=true) whose taskId is this task -
    // one deterministic pass, so a path can appear at most once per
    // (role, isExpansion) pair and is never double-counted across
    // categories a caller might derive from role.
    std::vector<ManifestTaskScopeEntryView> scopeEntries;
    // inspectedPaths: this task's FINAL worker attempt's actually-read
    // paths (ManifestTaskState::inspectedRelativePaths - already durable,
    // AG-15.2's same-attempt read gate evidence).
    std::vector<std::string> inspectedPaths;
    // proposedCreatedPaths / proposedModifiedPaths: derived from this
    // task's own changeSetId (when set) via IChangeProposalRepository -
    // Create changes populate the former, every other operation (Modify/
    // Delete/Rename) the latter, since there is no dedicated
    // deleted/renamed bucket in this compact projection. Both empty
    // whenever no IChangeProposalRepository was wired, or the task never
    // produced a change set.
    std::vector<std::string> proposedCreatedPaths;
    std::vector<std::string> proposedModifiedPaths;

    // AG-17.6: the frozen route of this provider-backed task, read from the
    // Manifest's routing record and the provider/model catalog - never computed by
    // a UI layer. Empty for a coordination task (no provider work) and for a
    // Manifest frozen before AG-17.1 (no record). In a single-assignment Manifest
    // the sole member is the implicit assignee.
    std::string assigneeMemberId;
    std::string assigneeRole;
    // "Provider / Model" (ids when the catalog row is gone).
    std::string assigneeLabel;
};

// QML-lint advisory-severity policy (2026-09-28): one durable, Backend-owned
// advisory finding, safe to render directly - relativePath is already
// candidate-relative and safety-validated (see
// QmlLintDiagnosticPolicy::displayQmlLintFindingPath, the sole implementation
// this projection reuses), never an absolute path. A QML/UI layer must never
// classify `tag` itself; the finding is in this list at all only because the
// one Backend-owned classifier (QmlLintDiagnosticPolicy::
// classifyQmlLintFindingSeverity) already decided Advisory and the one
// Backend-owned verifier (ManifestProposalSetVerifier::qmlLintBaselineDeltaCheck)
// already attached it to the stable verification.qml_warning_advisory
// diagnostic code.
struct ManifestVerificationAdvisoryFindingView {
    std::string relativePath;
    int line {0};
    int column {0};
    std::string tag;
    std::string text;
    std::optional<std::string> requirementId;
};

// QML-lint advisory-severity policy (2026-09-28): Run-level (not per-task)
// projection of this Run's authoritative combined verification's own
// durable verification.qml_warning_advisory diagnostics, exactly as
// recorded - never re-derived, re-classified, or re-parsed from Markdown/
// prose. Source: ManifestWorkLedgerEntry::verification (see
// ExecutionManifestReadModelService's constructor); never a second
// persistence/repository model.
//
// verificationAvailable is false whenever no IManifestWorkLedgerRepository
// was wired into this service, or this Run's Work Ledger entry has no
// verification identity yet (still executing, or never reached combined
// verification) - distinguished from "verification available but zero
// advisories" (verificationAvailable=true, hasAdvisories=false).
// verificationPassed mirrors the authoritative requiredAcceptancePassed()
// result for this same stored verification: advisories never change it, and
// this projection never lets a caller infer Apply eligibility from advisory
// presence alone - AG-16 (ManifestApplyVerificationGate) remains the sole
// Apply authority and never reads this view.
struct ManifestVerificationAdvisoryView {
    bool verificationAvailable {false};
    bool verificationPassed {false};
    bool hasAdvisories {false};
    std::size_t advisoryCount {0};
    std::vector<ManifestVerificationAdvisoryFindingView> advisories;
};

// AG-17.6: how a routing member took part in this Manifest. A closed, Backend-owned
// classification a UI renders verbatim (it never counts tasks itself):
//   lead           - plans (and may write the integration advice); never an assignee;
//   assigned       - a worker that owns at least one task;
//   unused         - an assignable worker with no task. This is NOT an error: the
//                    roster is a pool of available workers, not mandatory occupancy;
//   not_assignable - a member without workspace access, which a Manifest never
//                    assigns provider-backed tasks to.
struct ManifestRouteMemberView {
    std::string memberId;
    std::string role;
    std::string providerId;
    std::string providerName;
    std::string modelId;
    std::string modelName;
    // "Provider / Model" (the ids when the catalog row is gone).
    std::string label;
    bool lead {false};
    bool workspaceAccess {false};
    std::size_t assignedTaskCount {0};
    std::string participation;
    // AG-17.7B: everything attributable to this member - its task executions, plus the
    // Planner and integration executions when it is the lead (a single-assignment member
    // plans and executes). All-zero when ExecutionManifestView::usageAvailable is false.
    // The price state matters as much as the number: priceState() is Unknown/Partial when
    // a model price was unset, and costMicros is then NOT a precise total.
    ManifestUsageTotals usage;
};

struct ExecutionManifestView {
    RunId runId;
    std::uint32_t requestedMaxParallelism {1};
    // In manifest task order (stable/deterministic), not execution order.
    std::vector<ManifestTaskView> tasks;

    // AG-15.3: Run-level, not per-task - see ManifestScopeDependencyEdgeView.
    // Empty whenever no scope was ever frozen for this Run (a manifest
    // frozen before AG-15.2 was wired, or the workspaceScope repository was
    // not passed to this service) - workspaceScopeAvailable distinguishes
    // that case from a genuinely-frozen scope with zero dependency edges.
    bool workspaceScopeAvailable {false};
    std::vector<ManifestScopeDependencyEdgeView> scopeDependencyEdges;

    // QML-lint advisory-severity policy (2026-09-28): see
    // ManifestVerificationAdvisoryView's own doc comment.
    ManifestVerificationAdvisoryView verificationAdvisory;

    // AG-17.6: the frozen routing identity. routingMode is "none" for a Manifest
    // frozen before AG-17.1 (no record), otherwise toString(ManifestRoutingMode)
    // ("single_assignment" | "roster"); routingMembers is the record's member list in
    // record order (empty when mode is "none").
    std::string routingMode {"none"};
    std::vector<ManifestRouteMemberView> routingMembers;

    // AG-17.7B: member usage/cost reconstructed from the Run's ModelExecution rows
    // (ManifestMemberUsage.hpp). usageAvailable is false for a Manifest without a routing
    // record, when the execution repository is not wired, or when reading it failed (then
    // usageDiagnostics says why). The partition is exact when members + lead + unattributed
    // equals usageRunTotal; unattributed rows are diagnostic debt and are listed, never lost.
    bool usageAvailable {false};
    bool usagePartitionExact {false};
    ManifestUsageTotals usageRunTotal;
    ManifestUsageTotals usageUnattributed;
    std::vector<ManifestUnattributedExecution> usageUnattributedExecutions;
    std::vector<std::string> usageDiagnostics;
};

// AG-18.4A: an orthogonal qualifier describing WHY the primary
// verificationEnvironmentState is what it is - derived from the same
// snapshot's own structured signal (VerificationProfile::
// buildPresetsConfirmed) plus its Backend-authored diagnostic text (a
// closed, stable set of literals VerificationEnvironmentDiscoveryService
// itself produces - never provider/user content). This is not a
// replacement for state: `ExistingProfile` can co-occur with
// `ToolUnavailable` (a partially-confirmed profile), and `SetupRequired`
// can co-occur with `Drifted`, `ToolUnavailable`, or neither. Matching
// against fixed Backend-authored diagnostic literals is a stopgap until
// VerificationEnvironmentSnapshot carries a typed sub-reason field of its
// own (tracked as a known limitation, not a new AG-18.1/18.2 contract
// change - see the AG-18.4A closure notes in ARCHITECTURE_GAPS.md).
enum class ManifestVerificationEnvironmentIssue {
    None,
    Drifted,
    ToolUnavailable,
};
[[nodiscard]] const char* toString(ManifestVerificationEnvironmentIssue value) noexcept;

// One discovered build preset's stable name and confirmed target catalog -
// safe to render directly.
struct ManifestVerificationBuildPresetView {
    std::string name;
    bool allowedTargetsConfirmed {false};
    std::vector<std::string> allowedTargets;
};

// One discovered profile's stable id/source/frozen values - safe to render
// directly (resolvedBinaryDir is already candidate/workspace-relative,
// never an absolute host path).
struct ManifestVerificationProfileView {
    std::string profileId;
    std::string configurePresetName;
    std::string configureSource; // toString(VerificationPresetFileSource)
    std::string resolvedBinaryDir;
    bool buildPresetsConfirmed {false};
    std::vector<ManifestVerificationBuildPresetView> buildPresets;
};

// AG-18.4A: this Run's centrally-classified verification-environment
// projection - independent of whether a Manifest was ever frozen (an
// ActionRequired Run never freezes one, so this cannot live inside
// ExecutionManifestView). See
// ExecutionManifestReadModelService::verificationEnvironmentViewForRun.
struct ManifestVerificationEnvironmentView {
    // toString(VerificationEnvironmentState) - reuses the existing
    // three-value enum (see that enum's own doc comment: "later packages
    // never need a second enum"), never a second state type. A stored
    // SetupRequired snapshot is reclassified to "proposed_bootstrap" here,
    // at read time only, when a durable BootstrapPending decision exists
    // for this Run - the persisted AG-18.1 snapshot itself is never
    // mutated.
    std::string state;
    // toString(ManifestVerificationEnvironmentIssue) - see that enum.
    std::string issue;
    // Bounded, Backend-authored (never a raw provider string or absolute
    // path) - the same diagnostic VerificationEnvironmentSnapshot itself
    // already carries, for operator visibility only. The typed state/issue
    // above are the caller's classification; never re-derive additional
    // meaning from this string in a UI layer.
    std::string diagnostic;
    std::vector<ManifestVerificationProfileView> profiles;

    // Present only when this Run has a durable VerificationBootstrapDecision
    // (AG-18.3).
    bool hasBootstrapDecision {false};
    std::string bootstrapDisposition; // toString(VerificationBootstrapDisposition)
    std::string bootstrapReasonCode;
    std::string bootstrapMessage;
    std::vector<std::string> bootstrapSetupPaths;
    // Set only once a resolving (BootstrapVerified/BootstrapFailed)
    // transition exists for this Run.
    bool hasBootstrapTransition {false};
    std::string bootstrapTransitionDisposition;
    std::string bootstrapCandidateProfileId;
    std::string bootstrapCandidateBuildProfileId;
    std::string bootstrapCandidateTargetId;

    // AG-18.4A hardening: "Retry after setup" action availability - true
    // only when this Run has a durable ActionRequired decision. The write
    // command additionally checks the live workspace for a well-known preset
    // file before reserving/creating a retry; this immutable projection never
    // watches mutable filesystem state. This is NOT a "create a scoped setup Run" action -
    // VerificationBootstrapRetryService::createLinkedRetry only ever starts
    // a fresh, baseline-bound retry Run AFTER the user has already
    // configured or created a usable profile/preset manually or externally
    // (see the class doc comment and AG-18.4C, which is the still-open,
    // not-yet-implemented package for an actual in-product scoped setup Run
    // launcher). canRetryAfterSetupReason is always set (bounded,
    // non-blank) when false - a UI layer must never compute this
    // eligibility itself.
    bool canRetryAfterSetup {false};
    std::string canRetryAfterSetupReason;

    // AG-18.4A/4B boundary: creation-time preset setup now exists, but
    // configuring an already-existing Project from Run Details does not.
    // Always false on this surface, with a stable reason code, so the UI can
    // explain the narrower missing capability instead of making the stale
    // claim that no Backend-owned setup exists anywhere.
    bool canConfigureBackendProfile {false};
    std::string canConfigureBackendProfileReasonCode;
    std::string canConfigureBackendProfileReason;
};

// Read-only projection joining Phase 2's frozen ExecutionManifest with Phase
// 5's persisted per-task execution state for one Run. Owns no write path of
// its own - ManifestWorkflowRunner/ExecutionManifestExecutor remain the only
// callers that ever change either side.
class ExecutionManifestReadModelService {
public:
    // workspaceScope/changeProposals are optional (AG-15.3): passing nullptr
    // keeps every pre-AG-15 caller/test compiling and behaving unchanged
    // (empty scopeEntries/proposedCreatedPaths/proposedModifiedPaths,
    // workspaceScopeAvailable stays false) - matches every other optional-
    // pointer AG-14/AG-15 dependency in this codebase. verificationEnvironment/
    // verificationBootstrap are optional the same way (AG-18.4A): passing
    // nullptr keeps every pre-AG-18.4 caller/test compiling and behaving
    // unchanged (verificationEnvironmentViewForRun always returns nullopt).
    // workLedger is optional the same way (QML-lint advisory-severity
    // policy, 2026-09-28): passing nullptr keeps every pre-existing caller/
    // test compiling and behaving unchanged (viewForRun's
    // verificationAdvisory stays default-constructed,
    // verificationAvailable=false) - reuses the one already-existing
    // production IManifestWorkLedgerRepository instance wired through
    // BackendRuntime, never a second repository/persistence model.
    ExecutionManifestReadModelService(
        ExecutionManifestService& manifests,
        IManifestTaskExecutionRepository& taskExecutions,
        IRunWorkspaceScopeRepository* workspaceScope = nullptr,
        IChangeProposalRepository* changeProposals = nullptr,
        IVerificationEnvironmentRepository* verificationEnvironment = nullptr,
        IVerificationBootstrapRepository* verificationBootstrap = nullptr,
        IManifestWorkLedgerRepository* workLedger = nullptr,
        // AG-17.6: optional the same way - they only supply display names for the
        // routing projection; without them the projection falls back to the ids.
        const IProviderRepository* providers = nullptr,
        const IModelRepository* models = nullptr,
        // AG-17.7B: optional the same way - the Run's ModelExecution rows, read only to
        // reconstruct member usage/cost. Without it usageAvailable stays false and every
        // other projection is unchanged.
        const IModelExecutionRepository* modelExecutions = nullptr);

    [[nodiscard]] common::Result<std::optional<ExecutionManifestView>> viewForRun(
        const RunId& runId) const;

    // AG-18.4A: independent of viewForRun/ExecutionManifestView - a Run
    // that never froze a Manifest (still Planning, or a terminal
    // ActionRequired disposition) still has verification-environment/
    // bootstrap-lifecycle data worth projecting. Returns nullopt only when
    // no VerificationEnvironmentSnapshot was ever recorded for this Run at
    // all (a non-Manifest-workflow Run, or a Manifest Run that failed
    // before discovery ran), or when verificationEnvironment was not wired
    // into this service.
    //
    // Fail-closed (AG-18.4A hardening): a durable
    // VerificationBootstrapDecision whose own runId/projectId/
    // baselineFingerprint/verificationSnapshotSchemaVersion does not
    // exactly match this Run's snapshot, a transition whose runId does not
    // match, or more than one resolving (BootstrapVerified/BootstrapFailed)
    // transition for the same Run all fail this call with
    // "verification_environment.corrupt_bootstrap_record" - never silently
    // picked from or ignored. A caller must surface this as a real error,
    // never as "no bootstrap decision".
    [[nodiscard]] common::Result<std::optional<ManifestVerificationEnvironmentView>>
    verificationEnvironmentViewForRun(const RunId& runId) const;

private:
    ExecutionManifestService& manifests_;
    IManifestTaskExecutionRepository& taskExecutions_;
    IRunWorkspaceScopeRepository* workspaceScope_;
    IChangeProposalRepository* changeProposals_;
    IVerificationEnvironmentRepository* verificationEnvironment_;
    IVerificationBootstrapRepository* verificationBootstrap_;
    IManifestWorkLedgerRepository* workLedger_;
    const IProviderRepository* providers_;
    const IModelRepository* models_;
    const IModelExecutionRepository* modelExecutions_;
};

} // namespace ai::application
