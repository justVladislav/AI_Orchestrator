#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/execution/LegacyConversionRecord.hpp"
#include "ai/common/Result.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::application {

// PC-4C: where the next page of listTerminalRunsPage continues - strictly AFTER this (createdAt, id) in the order
// (createdAt DESC, id DESC). A Run's own (createdAt, id) is the cursor of the page that ends with it.
struct RunPageCursor {
    std::chrono::system_clock::time_point createdAt;
    RunId id;
};

inline constexpr std::size_t kMaxRunPageSize = 1000;

// The page rule in one place (the SQLite adapter expresses the same rule in SQL): the terminal Runs among `runs` whose workflow is in
// `workflowIds`, newest first (createdAt DESC, id DESC), strictly after `after`, at most `limit`.
[[nodiscard]] inline std::vector<Run> selectTerminalRunsPage(
    std::vector<Run> runs, const std::vector<std::string>& workflowIds, const std::optional<RunPageCursor>& after, std::size_t limit)
{
    const auto wanted = [&](const Run& run) {
        return isTerminal(run.status) && std::find(workflowIds.begin(), workflowIds.end(), run.workflowId) != workflowIds.end();
    };
    runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const Run& run) { return !wanted(run); }), runs.end());
    std::sort(runs.begin(), runs.end(), [](const Run& left, const Run& right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.id > right.id;
    });
    if (after.has_value()) {
        runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const Run& run) {
            if (run.createdAt != after->createdAt) return run.createdAt > after->createdAt;   // newer than the cursor: already seen
            return run.id >= after->id;                                                       // the cursor itself, or after it in the order
        }), runs.end());
    }
    if (runs.size() > limit) runs.resize(limit);
    return runs;
}

/**
 * Persistence port for run aggregates and their ordered stage snapshots.
 *
 * updateRun uses Run::revision for optimistic concurrency. Initial aggregate
 * creation is one operation so observers can never see a Run without the
 * ordered stage timeline declared by its workflow.
 */
class IRunRepository {
public:
    virtual ~IRunRepository() = default;
    virtual common::Result<Run> insertRun(Run run) = 0;
    // This is the production creation path used by RunManager. Implementations
    // must commit either the Run, every ordered stage, its assignment snapshot,
    // and optionally copied task links, or no part of them.
    virtual common::Result<Run> insertRunWithStages(
        Run run,
        std::vector<RunStage> stages) = 0;
    // Rerun-specific aggregate boundary. Adapters without task-link storage
    // inherit the ordinary atomic Run+stages behavior; SQLite overrides it to
    // copy links in the same transaction.
    virtual common::Result<Run> insertRunWithStagesAndCopiedTaskLinks(
        Run run,
        std::vector<RunStage> stages,
        const RunId& taskLinkSourceRunId)
    {
        static_cast<void>(taskLinkSourceRunId);
        return insertRunWithStages(std::move(run), std::move(stages));
    }
    // Brand-new-Run counterpart to insertRunWithStagesAndCopiedTaskLinks:
    // links exactly one ProjectTask to the newly created Run atomically with
    // the aggregate insert, instead of the caller making a second, separate
    // ITaskRepository::linkRun() call after the Run already exists (and may
    // already be executing). Adapters without task-link storage inherit the
    // ordinary atomic Run+stages behavior; SQLite overrides it.
    virtual common::Result<Run> insertRunWithStagesAndTaskLink(
        Run run,
        std::vector<RunStage> stages,
        const TaskId& taskId)
    {
        static_cast<void>(taskId);
        return insertRunWithStages(std::move(run), std::move(stages));
    }
    // AG-17.7C: the aggregate of a Run created by a legacy roster -> Manifest conversion:
    // the Run, every ordered stage, its assignment snapshot, the source Run's copied task
    // links (when `taskLinkSourceRunId` is set) AND the immutable conversion record commit in
    // one operation, or none of them does. There is never a conversion record without its Run
    // and never a converted Run without its record. A repository that cannot store the audit
    // must NOT fall back to a plain insert (that would create an unaudited conversion), so the
    // default refuses with kLegacyConversionUnsupportedCode. Implementations validate the
    // record against the Run (legacyConversionAggregateProblem) before writing anything.
    virtual common::Result<Run> insertRunWithStagesAndLegacyConversion(
        Run run,
        std::vector<RunStage> stages,
        LegacyConversionRecord record,
        const std::optional<RunId>& taskLinkSourceRunId)
    {
        static_cast<void>(run);
        static_cast<void>(stages);
        static_cast<void>(record);
        static_cast<void>(taskLinkSourceRunId);
        return common::Result<Run>::failure({kLegacyConversionUnsupportedCode,
            "This run repository cannot store a legacy conversion record"});
    }
    // The conversion record of a converted Run, or nullopt for any other Run. A stored record
    // that fails its own integrity or does not match its Run is reported as
    // kLegacyConversionRecordCorruptCode - never returned half trusted.
    [[nodiscard]] virtual common::Result<std::optional<LegacyConversionRecord>> findLegacyConversion(
        const RunId& runId) const
    {
        static_cast<void>(runId);
        return common::Result<std::optional<LegacyConversionRecord>>::ok(std::nullopt);
    }
    // AG-17.7C-3: the index of every converted Run (see LegacyConversionIndexEntry) - one cheap
    // read for the history list instead of one record read per row. A repository without audit
    // support has no converted Runs, so the default is an empty index.
    [[nodiscard]] virtual common::Result<std::vector<LegacyConversionIndexEntry>>
    listLegacyConversionIndex() const
    {
        return common::Result<std::vector<LegacyConversionIndexEntry>>::ok({});
    }
    virtual common::Result<Run> updateRun(Run run) = 0;
    [[nodiscard]] virtual common::Result<std::optional<Run>> findRun(const RunId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<Run>> listRuns(
        const std::optional<ProjectId>& projectId = std::nullopt) const = 0;
    // PC-4C: a bounded, STABLE page of one project's terminal Runs of the given workflows (see RunPageCursor and
    // selectTerminalRunsPage). `limit` is 1..kMaxRunPageSize. The default reads the project's Runs and pages them in memory - correct for
    // a test double; the SQLite adapter overrides it with a keyset query so that a long history is never loaded whole.
    [[nodiscard]] virtual common::Result<std::vector<Run>> listTerminalRunsPage(
        const ProjectId& projectId, const std::vector<std::string>& workflowIds, const std::optional<RunPageCursor>& after,
        std::size_t limit) const
    {
        if (limit == 0 || limit > kMaxRunPageSize) {
            return common::Result<std::vector<Run>>::failure({"run.invalid_page", "The page size must be between 1 and 1000"});
        }
        auto all = listRuns(projectId);
        if (!all.success()) return common::Result<std::vector<Run>>::failure(*all.error());
        return common::Result<std::vector<Run>>::ok(selectTerminalRunsPage(std::move(all.value()), workflowIds, after, limit));
    }
    // Used during startup recovery to remove stale Queued/Running states.
    [[nodiscard]] virtual common::Result<std::vector<Run>> listUnfinishedRuns() const = 0;
    virtual common::Result<void> replaceStages(const RunId& runId, std::vector<RunStage> stages) = 0;
    virtual common::Result<RunStage> updateStage(RunStage stage) = 0;
    [[nodiscard]] virtual common::Result<std::vector<RunStage>> listStages(const RunId& runId) const = 0;
};

} // namespace ai::application
