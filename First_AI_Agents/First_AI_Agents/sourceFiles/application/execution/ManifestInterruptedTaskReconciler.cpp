#include "ai/application/execution/ManifestInterruptedTaskReconciler.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/agents/ModelExecutionTypes.hpp"
#include "ai/application/execution/TaskRouteResolver.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace ai::application {

namespace {

// The workflows that record a Work Ledger entry (see ManifestWorkLedgerService::recordTerminalRun).
bool isLedgerWorkflow(const std::string& workflowId)
{
    return workflowId == kExecutionManifestWorkflowId
        || workflowId == kManifestVerificationContinuationWorkflowId;
}

bool isTerminalTaskStatus(ManifestTaskStatus status)
{
    return status == ManifestTaskStatus::Completed || status == ManifestTaskStatus::Failed
        || status == ManifestTaskStatus::Skipped;
}

// What one Run's reconciliation achieved: the rows it wrote, and whether the account is COMPLETE.
// An incomplete account holds the Run's immutable ledger entry back (the next startup retries).
struct RunReconciliation {
    std::size_t written {0};
    bool complete {true};
};

bool isInterruptedExecution(const ModelExecution& execution)
{
    return execution.status == ModelExecutionStatus::Failed
        && execution.errorCode == kModelExecutionInterruptedErrorCode;
}

// Writes the provable interrupted-task rows of one Run. nullopt = a repository read failed, so
// nothing could be decided; otherwise the rows written and whether the account is complete.
//
// COMPLETE means: nothing interrupted is left unaccounted for. That is not the same as "nothing
// was written". The cases are told apart on purpose:
//   - no Manifest, no interrupted execution, or a Manifest frozen before routing records existed
//     (reconciliation does not apply to it): complete, nothing to write;
//   - an interrupted execution that cannot be attributed with certainty - a routing record that is
//     present but damaged, no lead, a position allocation that cannot be computed, a position that
//     is not exactly one task's slot, or provider/model identities that disagree with the frozen
//     member - is an INCOMPLETE account. It must never be read as "no task was interrupted",
//     because the ledger entry written next is immutable.
// The lead's own rows are told apart from a worker's by POSITION, never by role: a worker's role is
// user-chosen text and may well be "planner" or "integration".
std::optional<RunReconciliation> reconcileRun(
    const Run& run,
    IExecutionManifestRepository& manifests,
    IManifestTaskExecutionRepository& taskExecutions,
    IModelExecutionRepository& executions)
{
    const auto foundManifest = manifests.findForRun(run.id);
    if (!foundManifest.success()) return std::nullopt;
    // A Run that never froze a Manifest has no tasks to account for.
    if (!foundManifest.value().has_value()) return RunReconciliation {};
    const ExecutionManifest& manifest = *foundManifest.value();

    const auto foundExecutions = executions.listExecutions(ModelExecutionSource::Run, run.id);
    if (!foundExecutions.success()) return std::nullopt;
    bool anyInterrupted = false;
    for (const ModelExecution& execution : foundExecutions.value()) {
        // Recovery of the unfinished executions has not settled this one: the account is not
        // complete, and an interrupted task cannot be proven from a still-running execution.
        if (execution.status == ModelExecutionStatus::Pending
            || execution.status == ModelExecutionStatus::Running) {
            return RunReconciliation {0, false};
        }
        anyInterrupted = anyInterrupted || isInterruptedExecution(execution);
    }
    // Nothing was interrupted: there is nothing to attribute, whatever the routing record says.
    if (!anyInterrupted) return RunReconciliation {};

    // A Manifest frozen before routing records existed (AG-17.1) is out of this pass's scope.
    if (!manifest.routing.has_value()) return RunReconciliation {};
    // From here an interrupted execution exists and the Manifest claims a routing record: if the
    // record cannot be trusted to attribute it, the account is incomplete.
    const RunReconciliation incomplete {0, false};
    if (ManifestRouting::recordProblem(*manifest.routing)) return incomplete;
    const ManifestRoutingRecord& routing = *manifest.routing;
    const auto lead = std::find_if(routing.members.begin(), routing.members.end(),
        [](const ManifestRoutingMember& member) { return member.leadArchitect; });
    if (lead == routing.members.end()) return incomplete;
    const std::uint64_t workerBase = static_cast<std::uint64_t>(lead->position) + 1;
    if (workerBase > std::numeric_limits<std::uint32_t>::max()) return incomplete;
    const auto plan = allocateModelExecutionPositionsFromRecord(
        routing, manifest, static_cast<std::uint32_t>(workerBase));
    if (!plan.success()) return incomplete;
    // The integration call sits one past the last task slot (see allocateModelExecutionPositions).
    std::optional<std::uint32_t> integrationPosition;
    if (plan.value().lastPosition < std::numeric_limits<std::uint32_t>::max()) {
        integrationPosition = plan.value().lastPosition + 1;
    }

    const auto foundRows = taskExecutions.listForRun(run.id);
    if (!foundRows.success()) return std::nullopt;
    std::unordered_map<std::string, ManifestTaskState> rows;
    for (const auto& row : foundRows.value()) rows[row.taskId] = row.state;

    // The writer's position allocation, inverted: position -> the task(s) it was allocated to.
    std::unordered_map<std::uint32_t, std::vector<const ExecutionManifestTask*>> tasksByPosition;
    for (const ExecutionManifestTask& task : manifest.tasks) {
        const auto slot = plan.value().byTaskId.find(task.id);
        if (slot != plan.value().byTaskId.end()) tasksByPosition[slot->second].push_back(&task);
    }

    // The latest interrupted execution of each provably-identified task.
    bool complete = true;
    std::unordered_map<std::string, std::pair<const ExecutionManifestTask*, const ModelExecution*>> interrupted;
    for (const ModelExecution& execution : foundExecutions.value()) {
        if (!isInterruptedExecution(execution)) continue;
        // The lead's own Planner and integration rows are not a task: told apart by position.
        if (execution.position == lead->position) continue;
        if (integrationPosition.has_value() && execution.position == *integrationPosition) continue;
        // Everything else interrupted must be exactly one task's execution, by the frozen route.
        const auto candidates = tasksByPosition.find(execution.position);
        if (candidates == tasksByPosition.end() || candidates->second.size() != 1) {
            complete = false;
            continue;
        }
        const ExecutionManifestTask& task = *candidates->second.front();
        const ManifestRoutingMember* member = ManifestRouting::assigneeOf(routing, task);
        if (member == nullptr
            || execution.providerId != member->providerId || execution.modelId != member->modelId) {
            complete = false;
            continue;
        }
        auto& slot = interrupted[task.id];
        if (slot.second == nullptr || execution.attempt > slot.second->attempt) {
            slot = {&task, &execution};
        }
    }

    std::size_t written = 0;
    for (const ExecutionManifestTask& task : manifest.tasks) {
        const auto found = interrupted.find(task.id);
        if (found == interrupted.end()) continue;
        const auto existing = rows.find(task.id);
        if (existing != rows.end() && isTerminalTaskStatus(existing->second.status)) continue;

        ManifestTaskState state;
        state.status = ManifestTaskStatus::Failed;
        state.errorCode = kManifestTaskInterruptedErrorCode;
        state.errorMessage = kManifestTaskInterruptedMessage;
        state.attempts = found->second.second->attempt;
        if (taskExecutions.upsert(run.id, task.id, state).success()) {
            ++written;
        } else {
            complete = false;
        }
    }
    return RunReconciliation {written, complete};
}
} // namespace

ManifestInterruptedTaskReconciler::ManifestInterruptedTaskReconciler(
    IRunRepository& runs,
    IExecutionManifestRepository& manifests,
    IManifestTaskExecutionRepository& taskExecutions,
    IModelExecutionRepository& executions,
    IManifestWorkLedgerRepository& ledger)
    : runs_(runs)
    , manifests_(manifests)
    , taskExecutions_(taskExecutions)
    , executions_(executions)
    , ledger_(ledger)
{
}

common::Result<ManifestInterruptedTaskReconciler::Outcome> ManifestInterruptedTaskReconciler::reconcile()
{
    Outcome outcome;
    const auto allRuns = runs_.listRuns();
    if (!allRuns.success()) {
        return common::Result<Outcome>::failure(*allRuns.error());
    }
    for (const Run& run : allRuns.value()) {
        if (run.status != RunStatus::Interrupted || !isLedgerWorkflow(run.workflowId)) continue;
        // A Run with a ledger entry is finished history: never touched, never re-derived.
        const auto entry = ledger_.findEntry(run.id);
        if (!entry.success()) {
            outcome.unreconciled.insert(run.id);
            continue;
        }
        if (entry.value().has_value()) continue;

        const auto reconciled = reconcileRun(run, manifests_, taskExecutions_, executions_);
        if (!reconciled.has_value()) {
            outcome.unreconciled.insert(run.id);
            continue;
        }
        outcome.materialized += reconciled->written;
        if (!reconciled->complete) outcome.unreconciled.insert(run.id);
    }
    return common::Result<Outcome>::ok(std::move(outcome));
}

} // namespace ai::application
