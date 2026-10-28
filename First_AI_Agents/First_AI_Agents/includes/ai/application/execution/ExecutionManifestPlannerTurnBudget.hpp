#pragma once

// Backend-owned, provider-neutral partition of the frozen Run-wide provider
// turn ceiling (ProviderExecutionBudgetPolicy::maxTurns) between Planner
// inspection, the Planner's own Backend-forced terminal submission, the
// Planner's bounded terminalization/validation recovery, and the downstream
// worker reserve that the SAME shared ProviderSessionBudgetGuard also
// governs (ExecutionManifestPlannerRequest::providerBudgetGuard is reused
// unchanged for every worker session on this Run).
//
// Real-Run incidents (2026-09-28): Run ade2ca50-0c15-4762-a2b8-3ffb0e6f0b3e
// and its Edit & rerun retry 7385ad56-1720-4835-b717-123bcff298f9 both spent
// every one of the frozen 24 Run-wide turns on unrestricted Planner
// inspection (48 and 47 inspection calls respectively) and never reached a
// terminal Manifest submission, leaving zero turns for validation recovery
// and zero for any worker. This module exists so the Planner can never
// repeat that failure mode: the partition below is computed once, before
// any provider dispatch, purely from the frozen Run-wide maxTurns - never
// from a user prompt, a per-provider special case, or the content of any
// specific Run. The policy id also versions the forced-terminal wire
// semantics used at that boundary; v3 keeps v2's provider-native named-tool
// forcing and caps Planner inspection so every turn added above the original
// 24-turn profile increases the downstream worker reserve instead.

#include "ai/common/Result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ai::application {

inline constexpr std::string_view kPlannerTurnBudgetPartitionPolicyId =
    "planner_turn_budget_partition_v3";

// Fixed Backend-owned reserves. None of these is overridable by a user
// prompt, a saved Execution Profile, or a per-Run/per-provider special case.
//
//   - kPlannerTerminalTurnReserve: the one turn reserved for the Planner's
//     own Backend-forced terminal submission the instant the inspection
//     boundary below is crossed.
//   - kPlannerRecoveryTurnReserve: covers the two independently-bounded
//     recovery mechanisms ExecutionManifestPlanner::plan() already supports
//     (one terminalization recovery for a missing terminal call, one
//     validation repair for a rejected typed submission). Each recovery
//     opens a fresh provider session whose own first turn (beginSession(),
//     which cannot carry a NextTurnToolPolicy - see IProviderAgentSession)
//     is unrestricted, and whose very next turn is always Backend-forced
//     terminal, because by the time any recovery starts this deep into the
//     partition the shared inspection boundary has always already been
//     crossed. So each recovery costs at most 2 turns, and both together
//     cost at most 4.
//   - kPlannerMinimumWorkerTurnReserve: turns guaranteed to remain in the SAME
//     shared ProviderSessionBudgetGuard for downstream Manifest workers
//     after the Planner returns, whether planning succeeds or is exhausted.
//     Manifest task allocation is not known before planning completes, so
//     this is the minimum pre-freeze floor. Once a Manifest is frozen, the
//     separate execution-capacity admission validates its actual pending
//     provider-backed task count against the guard's exact remaining turns.
//
// availableForInspectionAndWorkers = runWideMaxTurns - (terminal + recovery)
// inspectionTurns = min(kPlannerMaximumInspectionTurns,
//                       available - minimum worker reserve)
// workerReserveTurns = available - inspectionTurns
//
// At 24 this deliberately remains 15 inspection / 4 worker turns. At 40 the
// inspection allowance stays 15 and the guaranteed worker reserve becomes
// 20. At least kPlannerMinimumInspectionTurns must remain, or the partition
// fails closed before any provider dispatch.
inline constexpr std::uint64_t kPlannerTerminalTurnReserve = 1;
inline constexpr std::uint64_t kPlannerRecoveryTurnReserve = 4;
inline constexpr std::uint64_t kPlannerMinimumWorkerTurnReserve = 4;
// Backward-compatible spelling for callers/tests that describe the minimum
// floor rather than a concrete partition's possibly larger worker reserve.
inline constexpr std::uint64_t kPlannerWorkerTurnReserve =
    kPlannerMinimumWorkerTurnReserve;
inline constexpr std::uint64_t kPlannerMaximumInspectionTurns = 15;
inline constexpr std::uint64_t kPlannerMinimumInspectionTurns = 1;
inline constexpr std::uint64_t kPlannerMinimumRunWideTurnsForPartition =
    kPlannerTerminalTurnReserve + kPlannerRecoveryTurnReserve
    + kPlannerMinimumWorkerTurnReserve + kPlannerMinimumInspectionTurns;

struct PlannerTurnBudgetPartition {
    std::string policyId {std::string(kPlannerTurnBudgetPartitionPolicyId)};
    std::uint64_t runWideMaxTurns {0};
    std::uint64_t inspectionTurns {0};
    std::uint64_t terminalReserveTurns {kPlannerTerminalTurnReserve};
    std::uint64_t recoveryReserveTurns {kPlannerRecoveryTurnReserve};
    std::uint64_t workerReserveTurns {kPlannerWorkerTurnReserve};
    // The 0-based provider-turn index - counted exactly the way
    // ExecutionManifestPlanner::plan() counts its own persisted turns,
    // cumulatively across the primary attempt and every recovery/repair
    // attempt within the same plan() call - at or after which the Planner
    // must stop requesting further inspection: its next submitted turn is
    // Backend-forced to a terminal-only NextTurnToolPolicy.
    std::uint64_t inspectionBoundaryTurn {0};
};

// Fails closed with "planner_turn_budget.insufficient_run_wide_turns" when
// runWideMaxTurns cannot even satisfy the fixed reserves plus one inspection
// turn. Callers must perform this check before creating any provider
// session for the Run - see ExecutionManifestPlanner::plan().
[[nodiscard]] common::Result<PlannerTurnBudgetPartition> partitionPlannerTurnBudget(
    std::uint64_t runWideMaxTurns);

} // namespace ai::application
