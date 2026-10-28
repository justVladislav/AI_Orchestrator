#include "ai/application/execution/ExecutionManifestPlannerTurnBudget.hpp"

#include <algorithm>

namespace ai::application {

common::Result<PlannerTurnBudgetPartition> partitionPlannerTurnBudget(
    std::uint64_t runWideMaxTurns)
{
    if (runWideMaxTurns < kPlannerMinimumRunWideTurnsForPartition) {
        return common::Result<PlannerTurnBudgetPartition>::failure(
            {"planner_turn_budget.insufficient_run_wide_turns",
             "The frozen Run-wide provider turn budget cannot satisfy the "
             "Backend-owned terminal submission reserve, bounded recovery "
             "reserve, and worker reserve, plus at least one inspection turn"});
    }
    PlannerTurnBudgetPartition partition;
    partition.runWideMaxTurns = runWideMaxTurns;
    partition.terminalReserveTurns = kPlannerTerminalTurnReserve;
    partition.recoveryReserveTurns = kPlannerRecoveryTurnReserve;
    const auto availableForInspectionAndWorkers = runWideMaxTurns
        - kPlannerTerminalTurnReserve - kPlannerRecoveryTurnReserve;
    partition.inspectionTurns = std::min(
        kPlannerMaximumInspectionTurns,
        availableForInspectionAndWorkers - kPlannerMinimumWorkerTurnReserve);
    partition.workerReserveTurns =
        availableForInspectionAndWorkers - partition.inspectionTurns;
    partition.inspectionBoundaryTurn = partition.inspectionTurns;
    return common::Result<PlannerTurnBudgetPartition>::ok(std::move(partition));
}

} // namespace ai::application
