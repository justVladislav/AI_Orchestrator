#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ai::application {

enum class ManifestTaskStatus {
    Pending,
    Running,
    Completed,
    Failed,
    // A task whose declared dependency ended Failed/Skipped can never
    // legally run - it is skipped rather than left Pending forever.
    Skipped
};

// Stable lowercase string conversions, used by persistence and any display
// code (the deterministic run summary, a future UI read model) that needs
// one canonical spelling instead of each caller inventing its own.
[[nodiscard]] const char* toString(ManifestTaskStatus status) noexcept;
[[nodiscard]] ManifestTaskStatus manifestTaskStatusFromString(std::string_view value);

struct ManifestTaskState {
    ManifestTaskStatus status {ManifestTaskStatus::Pending};
    std::string errorCode;
    std::string errorMessage;
    std::optional<ChangeSetId> changeSetId;
    // Number of worker attempts made for this task (including the one that
    // finally succeeded/failed), so a caller can see whether a Completed/
    // Failed outcome consumed a bounded retry.
    std::size_t attempts {0};
    // Safe, secret-free summary of what this task produced (a proposal's
    // summary, or terminal assistant text for a no-proposal task), fed as
    // dependency-output context to any task that declares this one as a
    // dependency. Also lets a resumed/restarted execution reconstruct that
    // context for an already-Completed task without re-running its worker.
    std::string outputSummary;
    // AG-15: this task's FINAL worker attempt's actually-inspected paths
    // (AgentToolTurnLoopOutcome::inspectedRelativePaths - already normalized,
    // never a union across attempts), durable evidence for the Run Details
    // read model and the Work Ledger, independent of outputSummary. Empty
    // for a None-capability task or one that never reached a worker attempt.
    std::vector<std::string> inspectedRelativePaths;
};

enum class ManifestExecutionStatus {
    InProgress,
    Completed,
    // At least one task ended Failed (its dependents are Skipped, not
    // silently dropped) and nothing is left runnable.
    Failed,
    Cancelled
};

// Pure, deterministic scheduling logic for one frozen ExecutionManifest's
// task DAG - no provider/filesystem/persistence I/O of any kind. Exists so
// "which tasks are safe to run right now, and what happened to the ones that
// aren't" is fully unit-testable without a real worker, and so restart is
// just "construct with the persisted per-task states from last time" rather
// than special-cased logic.
//
// The DAG itself (acyclic, dependencies exist, ownership disjoint) is
// already guaranteed by ExecutionManifestValidator at freeze time (Phase 2);
// this class trusts that invariant and does not re-validate it.
class ExecutionManifestScheduler final {
public:
    // initialStates seeds already-known task outcomes (restart/resume
    // parity): any task id present keeps its given state; every task id in
    // manifest.tasks NOT present starts at Pending. Passing an empty map is
    // a fresh start.
    ExecutionManifestScheduler(
        const ExecutionManifest& manifest,
        std::unordered_map<std::string, ManifestTaskState> initialStates = {});

    // Up to parallelismLimit Pending task ids whose every dependency is
    // Completed, in manifest task order (stable/deterministic). Returns an
    // empty vector when nothing is currently runnable - this can legally
    // happen mid-run while other tasks are still Running.
    [[nodiscard]] std::vector<std::string> nextReadyBatch(
        std::size_t parallelismLimit) const;

    // Caller must mark a task Running before dispatching it so it is not
    // returned by nextReadyBatch again.
    void markRunning(const std::string& taskId);

    // Records a task's terminal outcome. A Failed outcome cascades: every
    // task (transitively) depending on it that is still Pending/Running
    // becomes Skipped, since it can never legally satisfy its dependency.
    void applyOutcome(const std::string& taskId, ManifestTaskState outcome);

    // True once no task is Pending or Running - every task reached
    // Completed/Failed/Skipped.
    [[nodiscard]] bool isTerminal() const;

    [[nodiscard]] ManifestExecutionStatus overallStatus() const;

    [[nodiscard]] const ManifestTaskState& stateOf(const std::string& taskId) const;

    [[nodiscard]] const std::unordered_map<std::string, ManifestTaskState>&
    allStates() const { return states_; }

private:
    void skipDependents(const std::string& failedTaskId);

    const ExecutionManifest& manifest_;
    std::unordered_map<std::string, ManifestTaskState> states_;
    // taskId -> ids of tasks that declare it as a dependency, built once at
    // construction so cascade/ready lookups do not rescan every task.
    std::unordered_map<std::string, std::vector<std::string>> dependents_;
};

} // namespace ai::application
