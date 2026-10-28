#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/RunWorkspaceScopeTypes.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kManifestWorkLedgerSchemaVersion =
    "manifest-work-ledger.v1";

// Bounded caps, matching the same "Backend-authored context is always
// bounded" convention as ExecutionManifestPolicy elsewhere in this codebase.
// A ledger entry/transition is durable provenance, never an unbounded
// transcript or raw provider payload (AG-14's own exit criteria).
inline constexpr std::size_t kManifestWorkLedgerMaxTaskDispositions = 64;
inline constexpr std::size_t kManifestWorkLedgerMaxProposalIds = 64;
inline constexpr std::size_t kManifestWorkLedgerMaxAffectedPaths = 256;
inline constexpr std::size_t kManifestWorkLedgerMaxReasonBytes = 2000;
inline constexpr std::size_t kManifestWorkLedgerMaxSummaryBytes = 4096;

// What the terminal Run actually produced at the planning level. Distinct
// from ChangeSetState (a per-proposal outcome) and from PlanningDisposition
// (ExecutionManifestPlanner's own in-memory enum, not persisted) - this is
// the durable ledger's own summary of that same fact.
enum class ManifestWorkLedgerDisposition {
    ManifestReady,
    NoChangesNeeded,
    // Planning/verification failed, was cancelled, or the process was
    // interrupted before any disposition existed.
    NotProduced,
};

[[nodiscard]] const char* toString(ManifestWorkLedgerDisposition value) noexcept;
[[nodiscard]] std::optional<ManifestWorkLedgerDisposition>
manifestWorkLedgerDispositionFromString(std::string_view value) noexcept;

// One task's outcome as last known when the owning Run went terminal.
// Mirrors ManifestTaskState (ExecutionManifestScheduler.hpp) plus the
// AG-04 no-changes reason, flattened into the ledger's own bounded shape -
// never a second source of truth, only a durable snapshot of what
// IManifestTaskExecutionRepository already reported.
struct ManifestWorkLedgerTaskDisposition {
    std::string taskId;
    std::string status; // ManifestTaskStatus's toString() value
    std::optional<ChangeSetId> changeSetId;
    std::optional<std::string> noChangesReason;
};

// One proposal's review lineage as last known when the owning Run went
// terminal. `state` and `supersedesChangeSetId` are snapshots of
// ProposedChangeSet's own fields at ledger-write time (see
// ChangeProposalTypes.hpp) - this is provenance, not a live join; a later
// Approve/Reject/Apply is recorded separately as a
// ManifestWorkLedgerReviewTransition, never by mutating this row.
struct ManifestWorkLedgerProposalLineage {
    ChangeSetId changeSetId;
    std::string state; // ChangeSetState's toString() value
    std::optional<ChangeSetId> supersedesChangeSetId;
};

struct ManifestWorkLedgerTaskWorkspaceActivity {
    std::string taskId;
    std::vector<std::string> inspectedRelativePaths;
    std::vector<std::string> proposedCreatedPaths;
    std::vector<std::string> proposedModifiedPaths;
};

// Typed terminal snapshot of the scope facts needed by the next Run. The
// dedicated scope/task/proposal repositories remain authoritative; this is
// the immutable Work Ledger snapshot that preserves what was frozen, what
// was expanded, what was actually inspected, what was proposed, and the
// Backend-derived dependency graph together at the Run boundary.
struct ManifestWorkLedgerWorkspaceSnapshot {
    std::string schemaVersion {"manifest-workspace-ledger-snapshot.v1"};
    std::vector<RunWorkspaceScopeEntry> entries;
    std::vector<RunWorkspaceScopeExpansion> expansions;
    std::vector<WorkspaceScopeDependencyEdge> dependencyEdges;
    std::vector<ManifestWorkLedgerTaskWorkspaceActivity> taskActivities;
};

// One durable, typed row for every terminal Manifest Run (Completed, Failed,
// Cancelled, or Interrupted - see ManifestWorkLedgerService::recordTerminalRun
// for the exact gating). Assembled entirely from already-persisted
// repositories, keyed only by runId: the frozen ExecutionManifest, per-task
// execution/no-changes evidence, and the run's change proposals. Never a
// second copy of raw provider output, unbounded command output, or file
// content - see AG-14 in ARCHITECTURE_GAPS.md.
struct ManifestWorkLedgerEntry {
    std::string schemaVersion {std::string(kManifestWorkLedgerSchemaVersion)};
    RunId runId;
    ProjectId projectId;
    // Only Completed/Failed/Cancelled/Interrupted are ever written here -
    // see ManifestWorkLedgerService's own doc comment.
    RunStatus terminalStatus {RunStatus::Failed};
    std::string terminalReason;
    // The real WorkspaceBaselineSnapshot::fingerprint (WorkspaceBaselineProvenance.hpp)
    // this Run captured before planning - the Backend's own workspace
    // identity, never a re-hash of an artifact's bytes. Recovered by
    // re-parsing the persisted workspace-baseline.v1.json artifact
    // (WorkspaceBaselineProvenance::parseJson), never substituted with the
    // artifact's own SHA-256.
    std::string baselineFingerprint;
    // ExecutionManifest::schemaVersion when a Manifest was actually frozen
    // for this Run; empty otherwise. runId itself remains the practical 1:1
    // identity of "the frozen Manifest of this Run" - at most one exists.
    std::string manifestSchemaVersion;
    ManifestWorkLedgerDisposition disposition {ManifestWorkLedgerDisposition::NotProduced};
    std::vector<ManifestWorkLedgerTaskDisposition> taskDispositions;
    std::vector<ChangeSetId> proposalIds;
    std::vector<ManifestWorkLedgerProposalLineage> proposalLineage;
    std::vector<std::string> affectedRelativePaths;
    // The full typed combined-verification identity (ExecutionAcceptanceContract.hpp)
    // when combined verification actually ran for this Run's proposal set -
    // absent (never fabricated) when it did not, e.g. a Planner no-op Run
    // that produced no Manifest at all. AG-16 reads this directly to gate
    // Apply against the exact verified candidate; never re-derive it from
    // manifest-verification.md prose.
    std::optional<ManifestProposalSetVerification> verification;
    // Bounded free-text summary derived from `verification`'s three
    // statuses when present, or an explicit "not_run: ..." explanation when
    // combined verification never executed (Planner no-op, or a Run that
    // failed before reaching execution) - never "passed" as a guess.
    std::string verificationSummary;
    // Bounded next-safe-step note: PlannerNoChangesNeeded::recommendedNextStep
    // when the Run ended in a Planner no-op, otherwise a synthesized bounded
    // list of Failed/Skipped task ids and error codes when any exist. A
    // suggestion for a human/future Planner, never an instruction the
    // Backend acts on.
    std::string remainingWorkNote;
    // AG-15.3: identity of the frozen RunWorkspaceScope for this Run (empty
    // when none was ever frozen - a Run predating AG-15.2, or one whose
    // Planner disposition never reached manifest freeze). Never a second
    // copy of the scope itself (see IRunWorkspaceScopeRepository) - just
    // enough identity for a caller to know one exists.
    std::string workspaceScopeSchemaVersion;
    // Bounded, deterministic summary rendered once at recordTerminalRun
    // time from already-durable scope/expansion data: role counts, approved
    // expansion count, and a bounded list of deferred paths. Never free
    // provider text. Empty whenever workspaceScopeSchemaVersion is empty.
    std::string workspaceScopeSummary;
    // MW-P3A: Backend-computed, leak-safe identities used to prevent an
    // accidental paid replay of an unchanged Planner failure. These are
    // SHA-256 digests only; the normalized request/provider/snapshot input
    // and rejected provider payload are deliberately not duplicated here.
    std::string planningInputFingerprint;
    std::string planningFailureSignature;
    std::optional<ManifestWorkLedgerWorkspaceSnapshot> workspaceSnapshot;
    std::chrono::system_clock::time_point createdAt;
};

enum class ManifestWorkLedgerTransitionKind {
    Approved,
    Rejected,
    Applied,
};

[[nodiscard]] const char* toString(ManifestWorkLedgerTransitionKind value) noexcept;
[[nodiscard]] std::optional<ManifestWorkLedgerTransitionKind>
manifestWorkLedgerTransitionKindFromString(std::string_view value) noexcept;

// One durable, append-only row per human review/apply decision on a
// Manifest-workflow proposal. Deliberately separate from ApprovalRecord and
// ProposalReviewRejection (this codebase's existing positive/negative review
// records): those remain the authoritative per-proposal state, while this is
// the cross-Run continuity trail the next Planner reads - the same
// separation-of-facts reasoning as ProposalReviewTypes.hpp's own doc
// comment. Never written for a non-Manifest-workflow Run - see
// ManifestWorkLedgerService::recordReviewTransition.
struct ManifestWorkLedgerReviewTransition {
    std::string id;
    RunId runId;
    ChangeSetId changeSetId;
    ManifestWorkLedgerTransitionKind kind {ManifestWorkLedgerTransitionKind::Approved};
    std::chrono::system_clock::time_point occurredAt;
    // Best-effort OS username annotation, matching ApprovalRecord::approvedBy
    // / ProposalReviewRejection::rejectedBy - not an authenticated identity.
    std::optional<std::string> actor;
    // Rejected only: the bounded human-entered reason, already validated by
    // ProposalReviewService against kMaximumReasonBytes before this is ever
    // constructed.
    std::optional<std::string> reason;
    // Applied only: the real WorkspaceBaselineSnapshot::fingerprint of the
    // workspace immediately after apply - the same value
    // ChangeSetApplyService's own createPostApplyProvenance() already
    // captures while building manifest-workspace-apply-provenance.md, never
    // a homegrown hash of the apply's own hash log.
    std::optional<std::string> postActionWorkspaceFingerprint;
};

} // namespace ai::application
