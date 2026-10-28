#pragma once

#include "ai/application/execution/ExecutionManifestScheduler.hpp"
#include "ai/application/execution/ManifestWorkerNoChanges.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <vector>

namespace ai::application {

// One durable snapshot of a manifest task's execution state, as last
// reported by ExecutionManifestExecutor (via ManifestWorkflowRunner's
// onTaskFinished callback) - never derived, never guessed.
struct ManifestTaskExecutionRecord {
    RunId runId;
    std::string taskId;
    ManifestTaskState state;
    std::chrono::system_clock::time_point updatedAt;
};

// AG-04: durable, terminal-only evidence for why a task's worker resolved
// as no-op (ManifestWorkerNoChanges - reason, summary, criterion evidence,
// inspected paths). Deliberately a separate record from
// ManifestTaskExecutionRecord above, the same way ProjectMemoryCompactionRecord
// is kept separate from ProjectMemorySegment: ManifestTaskState/upsert() is
// the hot path every task attempt/status transition writes, while this
// evidence exists only for the (rare) subset of tasks that actually resolve
// no-op. A task revisiting no-op across a bounded retry naturally replaces
// its own prior record, exactly like ManifestTaskExecutionRecord's own
// (runId, taskId) upsert semantics.
struct ManifestTaskNoChangesRecord {
    RunId runId;
    std::string taskId;
    ManifestWorkerNoChanges evidence;
    std::chrono::system_clock::time_point updatedAt;
};

// Persistence port for per-task manifest execution progress. This is
// deliberately separate from IExecutionManifestRepository (the frozen,
// immutable manifest document itself, Phase 2): a task's execution state
// changes repeatedly while a Run is in flight (Pending -> Running ->
// Completed/Failed/Skipped, possibly through a bounded retry), which would
// violate IExecutionManifestRepository's intentional "insert once, never
// update" contract if folded into the same aggregate.
//
// Exists so a UI can show live per-task progress and so a restarted process
// can resume ExecutionManifestExecutor from exactly the states it last
// durably reported, instead of only the manifest's static task list.
class IManifestTaskExecutionRepository {
public:
    virtual ~IManifestTaskExecutionRepository() = default;

    // Idempotent: (runId, taskId) is the natural key, and a later call for
    // the same pair replaces the earlier snapshot (a task legitimately moves
    // through several states across its lifetime, and - through the bounded
    // retry path - can even revisit Pending).
    virtual common::Result<void> upsert(
        const RunId& runId, const std::string& taskId, const ManifestTaskState& state) = 0;

    [[nodiscard]] virtual common::Result<std::vector<ManifestTaskExecutionRecord>> listForRun(
        const RunId& runId) const = 0;

    // AG-04: see ManifestTaskNoChangesRecord above. Idempotent on (runId,
    // taskId), matching upsert()'s own semantics.
    virtual common::Result<void> upsertNoChanges(
        const RunId& runId, const std::string& taskId, const ManifestWorkerNoChanges& evidence) = 0;

    [[nodiscard]] virtual common::Result<std::vector<ManifestTaskNoChangesRecord>> listNoChangesForRun(
        const RunId& runId) const = 0;
};

} // namespace ai::application
