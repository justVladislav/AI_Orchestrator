#pragma once

#include "ai/application/catalog/ExecutionProfileTypes.hpp"
#include "ai/application/budget/ProviderExecutionBudget.hpp"
#include "ai/application/execution/RunInterfaceContract.hpp"
#include "ai/models/TokenUsage.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// Application-facing identifiers stay provider- and UI-neutral. UUID strings
// are used at runtime; aliases keep repository/Qt conversion straightforward.
using ProjectId = std::string;
using RunId = std::string;
using StageId = std::string;
using TaskId = std::string;

// Persistent project availability. Archiving is intentionally non-destructive.
enum class ProjectStatus { Active, Archived, Unavailable };

// Complete run lifecycle shared by services, repositories, and the future Qt adapter.
// Prepared is a durable, non-executing continuation aggregate. It is never
// placed on RunManager's queue and startup recovery must not reinterpret it as
// an interrupted provider operation. MW-P5.4 is the only package allowed to
// promote it into executable work.
enum class RunStatus { Queued, Running, Cancelling, Completed, Failed, Cancelled, Interrupted, Prepared };

// A Run-level completion contract for the Manifest planning stage. Product
// implementation Runs fail closed unless the planner freezes a non-empty
// Manifest; audit/verification Runs may explicitly opt into the separately
// validated evidence-backed no-op terminal outcome.
enum class PlanningCompletionPolicy {
    RequireManifest,
    AllowEvidenceBackedNoChanges
};

// Immutable Run-level spending/repair policy for the combined verification
// boundary. OneBoundedWave preserves the historical behavior: after a
// repairable primary verification failure, dispatch at most one repair
// session per affected task and verify the exact successor set once more.
// Disabled keeps the primary verification authoritative and terminates the
// Run without making those additional provider calls, allowing an operator
// to choose the provider-free manual-handoff path instead.
enum class VerificationRepairPolicy {
    OneBoundedWave,
    Disabled
};

// A stage has its own lifecycle because a cancelled/failed run may retain
// already completed stages for diagnostics and retry decisions.
enum class StageStatus { Pending, Running, Completed, Failed, Skipped, Cancelled };

enum class TaskStatus { Backlog, Planned, InProgress, Blocked, Done, Cancelled };
enum class TaskPriority { Low, Normal, High, Critical };

struct ProjectTask {
    TaskId id;
    ProjectId projectId;
    std::string title;
    std::string description;
    TaskStatus status {TaskStatus::Backlog};
    TaskPriority priority {TaskPriority::Normal};
    bool requiresAttention {false};
    std::optional<std::chrono::system_clock::time_point> dueAt;
    std::optional<std::chrono::system_clock::time_point> plannedRunAt;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
};

struct CreateTaskRequest {
    ProjectId projectId;
    std::string title;
    std::string description;
    TaskStatus status {TaskStatus::Backlog};
    TaskPriority priority {TaskPriority::Normal};
    bool requiresAttention {false};
    std::optional<std::chrono::system_clock::time_point> dueAt;
    std::optional<std::chrono::system_clock::time_point> plannedRunAt;
};

struct UpdateTaskRequest {
    TaskId taskId;
    std::string title;
    std::string description;
    TaskStatus status {TaskStatus::Backlog};
    TaskPriority priority {TaskPriority::Normal};
    bool requiresAttention {false};
    std::optional<std::chrono::system_clock::time_point> dueAt;
    std::optional<std::chrono::system_clock::time_point> plannedRunAt;
};

// Persistent project aggregate exposed by ProjectService.
struct Project {
    ProjectId id;
    std::string name;
    std::string description;
    std::filesystem::path rootPath;
    ProjectStatus status {ProjectStatus::Active};
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
};

// Command DTO accepted at the application boundary before validat./canonicalization.
struct CreateProjectRequest {
    std::string name;
    std::string description;
    std::filesystem::path rootPath;
};

// Persisted state of one workflow stage. Progress is normalized to [0.0, 1.0].
struct RunStage {
    StageId id;
    RunId runId;
    std::string type;
    std::string title;
    std::string description;
    StageStatus status {StageStatus::Pending};
    double progress {0.0};
    std::string providerId;
    std::string modelId;
    models::TokenUsage usage;
    std::string errorMessage;
    std::optional<std::chrono::system_clock::time_point> startedAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
};

// Persisted run aggregate. revision enables optimistic concurrency when the
// in-memory repository is replaced by SQLite.
struct Run {
    RunId id;
    ProjectId projectId;
    std::optional<RunId> parentRunId;
    std::string title;
    std::string task;
    std::string workflowId;
    RunStatus status {RunStatus::Queued};
    double progress {0.0};
    std::optional<StageId> currentStageId;
    std::string errorMessage;
    models::TokenUsage usage;
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> startedAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
    std::uint64_t revision {0};

    // Set only for a production run started against an ExecutionProfile;
    // absent for the Mock workflow. Never set directly by a caller - only
    // WorkflowRouter::resolvePlan() populates it, from either an explicit
    // StartRunRequest::executionProfileId or a resolved default profile.
    std::optional<ExecutionProfileId> executionProfileId;

    // Immutable snapshot of exactly which provider/model/timeout each stage
    // used, captured once when the run was enqueued. Empty for the Mock
    // workflow. This is what makes a retry reproducible: retrying reuses
    // this snapshot verbatim instead of re-resolving the (possibly since
    // changed) execution profile, so "retry" never silently executes a
    // different provider/model than the original run did.
    std::vector<StageModelAssignment> assignmentSnapshot;

    // Immutable bounded concurrency for the task graph. It is persisted on
    // the Run so retries and process-visible read models retain the exact
    // execution policy selected when the run was created.
    std::uint32_t maxParallelism {4};

    // Canonical snapshot of the effective workspace used by this Run. New
    // Runs always set it, even when the user selected the Project's own
    // rootPath. nullopt is retained only for rows created before schema v24;
    // callers may use Project::rootPath as a legacy fallback while loading
    // such history. Keeping the resolved path immutable prevents a later
    // Project path edit from retargeting Retry or proposal Apply.
    std::optional<std::filesystem::path> workspacePath;

    // Immutable after enqueue and persisted with the Run so retry/restart
    // cannot silently change whether a planner no-op is an accepted outcome.
    PlanningCompletionPolicy planningCompletionPolicy {
        PlanningCompletionPolicy::RequireManifest};

    // Immutable after enqueue. Retried/continued Runs inherit this value so
    // a restart or lineage transition cannot silently re-enable paid repair.
    VerificationRepairPolicy verificationRepairPolicy {
        VerificationRepairPolicy::OneBoundedWave};

    // Immutable Backend-resolved provider spending policy and its durable live
    // counters. The policy is snapshotted before Planner execution; usage is
    // updated after every completed provider turn/tool batch.
    ProviderExecutionBudgetPolicy providerBudget;
    ProviderExecutionBudgetUsage providerBudgetUsage;

    // PC-2 (gap MW-P9): the Run author's authoritative interface contracts and constraints. Validated and
    // canonicalized once at enqueue (normalizeRunInterfaceContracts), immutable afterwards like every other
    // Run input, and inherited by Retry, Edit & rerun prefill and a continuation child. The Backend, not
    // the Planner, delivers them verbatim to the workers they apply to.
    std::vector<RunInterfaceContract> interfaceContracts;
};

// Command DTO used by UI/CLI callers. parentRunId links retries without
// overwriting the original run and its artifacts.
struct StartRunRequest {
    ProjectId projectId;
    std::string title;
    std::string task;
    std::string workflowId {"mock-sequential-v1"};
    std::optional<RunId> parentRunId;

    // Explicit execution profile selection. This is deliberately a separate
    // field from workflowId - a profile selects providers/models/timeouts
    // for a workflow, it is never itself a workflow identifier. When unset,
    // WorkflowRouter looks for an enabled default profile whose own
    // workflowId matches the requested one (see WorkflowRouter.hpp).
    std::optional<ExecutionProfileId> executionProfileId;

    // Optional ad-hoc production roster. It is validated and snapshotted
    // verbatim so retries keep the exact provider/model/role configuration.
    std::vector<StageModelAssignment> assignmentOverrides;

    // Explicit provider limits for a per-Run roster (assignmentOverrides). Empty = the Backend
    // defaults with the turn limit in automatic mode; a maxTurns here pins it. A saved execution
    // profile carries its own limits, so any value here together with a profile (or without a
    // roster) is refused by WorkflowRouter rather than silently ignored.
    ProviderExecutionBudgetOverrides providerBudget;

    // Applies to concurrently-ready graph nodes, never to provider-internal
    // threads. Values outside [1, 32] are rejected before persistence.
    std::uint32_t maxParallelism {4};

    // Caller-chosen folder for this run, instead of the project's own
    // rootPath. nullopt (the default) keeps "the current" workspace, i.e.
    // the project's rootPath. Validated (exists/is-directory/canonicalize)
    // by RunManager exactly like a project path, before the Run is created.
    std::optional<std::filesystem::path> workspacePath;

    // Safe product default: implementation Runs must produce executable work.
    // Callers must opt in explicitly for audit/verification-only Runs.
    PlanningCompletionPolicy planningCompletionPolicy {
        PlanningCompletionPolicy::RequireManifest};

    // Safe backwards-compatible default: retain the existing single bounded
    // repair wave unless the caller explicitly opts out.
    VerificationRepairPolicy verificationRepairPolicy {
        VerificationRepairPolicy::OneBoundedWave};

    // Links this brand-new Run to a ProjectTask atomically with its
    // creation (IRunRepository::insertRunWithStagesAndTaskLink), the same
    // backend-owned guarantee a rerun already gets by copying its source
    // Run's task links. Only meaningful for a genuinely new Run: it is
    // ignored whenever RunManager is given a taskLinkSourceRunId (retry /
    // edited rerun), since those paths inherit their links instead.
    std::optional<TaskId> projectTaskId;

    // AG-18.3 hardening (blocker 4): when set, RunManager::enqueueInternal
    // uses this exact id instead of generating a fresh one. Exists solely
    // for VerificationBootstrapRetryService's crash-safe reservation
    // protocol - a retry link is durably persisted naming this id BEFORE
    // the Run itself is created, so a repeated call after an interruption
    // resumes with the same id instead of creating a second Run. Never set
    // by an ordinary caller; a caller-supplied id that collides with an
    // existing Run fails the normal insert-time uniqueness check like any
    // other id collision would.
    std::optional<RunId> preselectedRunId;

    // PC-2: see Run::interfaceContracts. Validated by RunManager; accepted only for the Manifest workflow,
    // so a contract can never be silently dropped by a workflow that has no worker prompt to carry it.
    std::vector<RunInterfaceContract> interfaceContracts;
};

// The durable source determines how Edit & rerun is presented. In particular,
// a saved-profile run must remain a saved-profile launch instead of being
// silently reconstructed as a lossy ad-hoc roster by QML.
enum class RunLaunchMode { Mock, SavedExecutionProfile, CustomRoster };

// Backend-owned prefill contract for Edit & rerun. The nested request contains
// only persisted source values; parentRunId is intentionally not trusted from
// the UI and is assigned again by OrchestrationService::startEditedRerun().
struct RerunDraft {
    RunId sourceRunId;
    StartRunRequest request;
    RunLaunchMode launchMode {RunLaunchMode::Mock};
    bool migratedFromLegacyWorkflow {false};
};

// A detached, append-only memory layer for a project. Segments summarize the
// outcome of a run (later possibly a chat) without ever mutating the
// project's own hand-authored description; removing a project cascades into
// its memory segments (enforced by ON DELETE CASCADE in the SQLite adapter).
// run_id deliberately carries no foreign key: memory belongs to the project
// and must outlive the deletion of any single run.
using ProjectMemorySegmentId = std::string;

struct ProjectMemorySegment {
    ProjectMemorySegmentId id;
    ProjectId projectId;
    RunId runId;
    std::string summary;
    std::uint64_t sourceTokenCount {0};
    std::uint64_t summaryTokenCount {0};
    std::chrono::system_clock::time_point createdAt;
};

// Stable lowercase values for persistence, logging, and Qt model roles.
[[nodiscard]] const char* toString(ProjectStatus status) noexcept;
[[nodiscard]] const char* toString(RunStatus status) noexcept;
[[nodiscard]] const char* toString(PlanningCompletionPolicy policy) noexcept;
[[nodiscard]] std::optional<PlanningCompletionPolicy>
planningCompletionPolicyFromString(std::string_view value) noexcept;
[[nodiscard]] const char* toString(VerificationRepairPolicy policy) noexcept;
[[nodiscard]] std::optional<VerificationRepairPolicy>
verificationRepairPolicyFromString(std::string_view value) noexcept;
[[nodiscard]] const char* toString(StageStatus status) noexcept;
[[nodiscard]] const char* toString(TaskStatus status) noexcept;
[[nodiscard]] const char* toString(TaskPriority priority) noexcept;
[[nodiscard]] const char* toString(RunLaunchMode mode) noexcept;
[[nodiscard]] bool isTerminal(RunStatus status) noexcept;

} // namespace ai::application
