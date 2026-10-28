#pragma once

#include "ai/application/execution/ExecutionManifestScheduler.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/budget/ProviderExecutionBudget.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ai::application {

// kExecutionCapacityInsufficientCode now lives in ProviderExecutionBudget.hpp
// (the reservation-aware guard raises it as well); this header keeps using it
// through that include.

// Stable identity of the conservative worker-capacity reservation policy
// below. It participates in planning-input identity (see
// ManifestWorkflowRunner's planning-input fingerprint) so that a Retry after
// this policy changed is never replayed as an unchanged input of an older
// policy. Bump it whenever reservedProviderTurnsForTask() or the reservation
// semantics change.
//
// v2: the turn limit of ONE worker session is no longer the fixed
// kAgentToolTurnLoopMaxTurns (12) for every task. It is derived from the task's
// own floor (workerSessionTurnLimitForTask()), so a task that declares many
// required reads is no longer refused for exceeding a limit the Backend itself
// invented; only the Backend hard ceiling bounds it. The floor formula
// (required reads + the terminal turn) is unchanged.
//
// v3: a task is also guaranteed a SECOND fresh session. Besides its primary
// reservation (the floor), every pending provider-backed task holds a separate,
// protected reservation of the same size - the "retry" reservation, registered
// under its own owner (manifestRetryReservationOwner()) - that only a fresh
// repair/retry session of THAT task can spend. Real Runs ee528eed and 8b23fbba
// showed the gap: a primary session that used every turn it could left the
// retry session with the remainder of a Run-wide pool that had no surplus (5
// turns, then 0) - a worker could not even re-read its required files. The
// automatic Run-wide turn limit and the Planner-boundary check count both
// halves; a pinned limit is never raised for them.
//
// v4: a Propose task's floor also counts the distinct EXISTING files it writes
// (ownedPaths, filled from the frozen baseline; a planned-create file has nothing to
// read): required reads + existing files to write + the terminal turn. Run 80f904ed:
// one task wrote 11 existing files and had 1 required read, so its floor was 2; its
// retry session, allowed one tool call per turn, spent 19 turns on single reads and
// ran the whole Run out of turns before it could propose anything.
inline constexpr std::string_view kWorkerCapacityReservationPolicyId =
    "worker_capacity_reservation_v4";

// How much of a task's capacity the Backend reserves before dispatch.
enum class ManifestRetryReservation {
    // One fresh worker session per task: only the primary floor is reserved. The
    // verification-repair wave (a single repair attempt per task) and an executor
    // configured with maxAttemptsPerTask = 1 use it.
    None,
    // The primary floor plus a separate protected reservation of the same size for the
    // task's retry/repair session (policy v3). The default everywhere a task may be
    // retried.
    OneProtectedRetry
};

// The guard owner keys of a task's two reservations. They live in two DISJOINT key spaces
// ("primary:<taskId>" and "retry:<taskId>", see kPrimaryReservationOwnerPrefix /
// kRetryReservationOwnerPrefix) - a raw task id is never an owner key - so no task id, not even a
// Planner-chosen "retry:x" next to a task "x", can make two reservations share a key: with unique
// task ids every key is unique. admitManifestWorkerReservations() still refuses a duplicate key
// with kTurnReservationConflictCode instead of letting a map silently drop one.
[[nodiscard]] std::string manifestPrimaryReservationOwner(std::string_view taskId);
[[nodiscard]] std::string manifestRetryReservationOwner(std::string_view taskId);

// Stable identity of the automatic Run-wide turn-limit extension below. Like
// the reservation policy it decides whether an otherwise identical Run is
// admitted, so it participates in planning-input identity (through
// executionCapacityPolicyIdentity()). Bump it whenever the extension formula or
// its eligibility rules change.
inline constexpr std::string_view kManifestTurnLimitPolicyId =
    "manifest_turn_limit_auto_v1";

// Extra Run-wide turns granted on top of the exact requirement
// (turnsStarted + the pending tasks' reserved floors). Zero by decision: the
// limit is raised to exactly what the frozen Manifest needs.
inline constexpr std::uint64_t kManifestTurnLimitHeadroomTurns = 0;

// One task's serial-safe reserved floor does not fit into a single worker
// session (it exceeds the Backend hard ceiling). Raised before any worker dispatch.
inline constexpr std::string_view kTaskCapacityInsufficientCode =
    "provider_budget.task_capacity_insufficient";

// Provider-neutral post-freeze admission result. The per-task protocol
// minimum is derived from the actual AgentToolTurnLoopRunner contract, never
// a flat guess (see minimumProviderTurnsForTask() below). Outer task retry
// and the turn-loop's typed/JSON repair are conditional recovery paths, not
// a minimum, so this deliberately does not invent a provider-specific or
// historical turns-per-task multiplier. Coordination (WorkspaceCapability::
// None) tasks require no provider turn.
struct ManifestExecutionTurnCapacity {
    std::uint64_t pendingProviderTaskCount {0};
    std::uint64_t minimumRequiredTurns {0};
    std::uint64_t remainingTurns {0};
};

// The single Backend-owned source of "how many provider turns can this task
// shape ever complete in, at the theoretical best case" - shared by
// admission, ManifestWorkflowRunner, and every test so no caller can drift
// from a second, inconsistent implementation.
//
// - WorkspaceCapability::None (coordination): 0. No provider session is ever
//   opened for it.
// - WorkspaceCapability::Propose with an empty requiredReadPaths: 1. Per
//   AgentToolTurnLoopRequest::requiredReadPathKeys' own contract (see
//   AgentToolTurnLoopRunner.hpp), a ReadyForReview/NoChangesNeeded proposal
//   is gated on this task's required reads already being present in
//   attemptInspectedPaths for THIS attempt - so with none required, the
//   very first beginSession() response may already be the terminal
//   proposal.
// - WorkspaceCapability::Propose with a non-empty requiredReadPaths: 2. The
//   same gate means the model must spend at least one turn actually
//   inspecting those paths (any number of required paths still collapses to
//   one turn - parallel tool calls let it read all of them at once) before a
//   second, separate terminal turn is even offered.
// - WorkspaceCapability::Inspect (typed verification): 1. The turn loop's
//   own contract explicitly exempts Inspect-mode submit_task_verification
//   from the requiredReadPathKeys gate above ("never before an Inspect-mode
//   submit_task_verification, which has its own independent per-requirement
//   evidence contract") - so, unlike Propose, an Inspect task's required
//   reads never block its first-turn terminal call at the protocol level;
//   ManifestProposalSetVerifier is the separate, later authority that
//   actually judges its evidence.
[[nodiscard]] std::uint64_t minimumProviderTurnsForTask(
    const ExecutionManifestTask& task) noexcept;

// Evaluates the immutable Manifest and the shared Run-wide budget snapshot.
// Terminal seeded states consume no new capacity; absent/Pending/Running
// provider-backed tasks each require minimumProviderTurnsForTask(task) turns.
// Parallelism is absent by design: it changes latency/concurrency, never the
// total number of turns. Fails closed with kExecutionCapacityInsufficientCode
// before any worker task attempt/session/model-execution/proposal is created
// when the minimum cannot fit. An already-inconsistent usage snapshot
// (turnsStarted > maxTurns) fails through the same stable code.
[[nodiscard]] common::Result<ManifestExecutionTurnCapacity>
admitManifestExecutionTurnCapacity(
    const ExecutionManifest& manifest,
    const std::unordered_map<std::string, ManifestTaskState>& seededStates,
    const ProviderExecutionBudgetPolicy& policy,
    const ProviderExecutionBudgetUsage& usage);

// ---------------------------------------------------------------------------
// Conservative worker-capacity reservation (policy
// kWorkerCapacityReservationPolicyId)
//
// minimumProviderTurnsForTask() above is the THEORETICAL protocol minimum: it
// relies on the provider batching every required read into one parallel turn
// (the protocol permits up to kAgentToolTurnLoopMaxToolCallsPerTurn calls per
// turn). It is deliberately kept unchanged and is NOT a realistic estimate.
//
// reservedProviderTurnsForTask() is the separate, conservative, serial-safe
// floor the Backend reserves for a task before any worker dispatch. It assumes
// the Backend has no deterministic batch/prefetch mechanism, so every piece of
// same-attempt evidence costs one evidence turn, plus one terminal turn:
//
// - None (coordination): 0; no provider session is ever opened for it.
// - Propose: requiredReadPaths.size() + E + 1, where E is the number of distinct
//   existing files in ownedPaths that are not also required reads (v4). Each required
//   read is one serial-safe evidence turn (AgentToolTurnLoopRequest::requiredReadPathKeys
//   gates ReadyForReview/NoChangesNeeded on them being inspected in the SAME
//   attempt), each existing file the task rewrites is one potential read turn of its
//   own (the retry protocol allows one tool call per turn), and the terminal
//   propose/no-changes submission is one more.
// - Inspect: max(1, U) + 1, where U is the number of distinct normalized
//   requiredOutputs across the task's REQUIRED acceptance requirements.
//   Derived from parseTaskVerification(): every required output needs
//   same-attempt inspection evidence and at least one successful inspection
//   must exist at all; the terminal submit_task_verification is one more turn.
//
// Conditional JSON repair and the outer task retry are NOT part of the floor:
// under v2 they could only spend the unreserved Run-wide surplus, which had none.
// Under v3 each of them is guaranteed its own equal reservation (see
// ManifestRetryReservation and manifestRetryReservationOwner) that the primary
// session can never touch.
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t reservedProviderTurnsForTask(
    const ExecutionManifestTask& task) noexcept;

// The length every worker session had before v2, and still the minimum of one
// (the agent tool turn loop's kAgentToolTurnLoopMaxTurns; the .cpp asserts they
// are the same number). The Planner and the legacy runner keep this fixed length.
inline constexpr std::uint64_t kWorkerSessionBaselineTurns = 12;

// The turn limit of ONE worker session for `task`:
//
//     max(kWorkerSessionBaselineTurns, reservedProviderTurnsForTask(task)),
//
// never above kProviderBudgetHardMaxTurns. A task whose floor fits the baseline
// keeps the session it always had (its own reservation still stops it earlier);
// a task with more required reads gets exactly the session its floor needs - a
// provider that reads one file per turn is protected, a provider that batches
// reads returns the unused part of the reservation to the Run. The single source
// for the Planner's pre-freeze capacity check, the post-freeze admission and the
// executor's session request.
[[nodiscard]] std::uint64_t workerSessionTurnLimitForTask(
    const ExecutionManifestTask& task) noexcept;

// The total tool calls one worker session may make: the baseline ratio
// (kAgentToolTurnLoopMaxTotalToolCalls per kAgentToolTurnLoopMaxTurns, i.e. four
// per turn) applied to workerSessionTurnLimitForTask(), never below the old fixed
// limit. A session that is allowed many turns for many serial reads must also be
// allowed the calls those reads are.
[[nodiscard]] std::uint64_t workerSessionToolCallLimitForTask(
    const ExecutionManifestTask& task) noexcept;

// True when the task's floor fits one worker session, i.e. does not exceed the
// Backend hard ceiling. The only way a single task can be too large.
[[nodiscard]] bool taskFitsWorkerSession(const ExecutionManifestTask& task) noexcept;

struct ManifestWorkerReservationPlan {
    // Reservation owner -> reserved turns: manifestPrimaryReservationOwner(taskId) of a pending
    // provider-backed task (its primary floor) and, under
    // ManifestRetryReservation::OneProtectedRetry, also manifestRetryReservationOwner(taskId)
    // (its protected retry reservation, the same size).
    // Never contains a coordination task or a task whose seeded state is already terminal.
    std::unordered_map<std::string, std::uint64_t> reservedTurnsByTask;
    std::uint64_t pendingProviderTaskCount {0};
    // primaryReservedTurns + retryReservedTurns.
    std::uint64_t totalReservedTurns {0};
    std::uint64_t primaryReservedTurns {0};
    std::uint64_t retryReservedTurns {0};
    std::uint64_t remainingTurns {0};
};

// The single authority every execution entry point (primary workflow, paid
// continuation repair, executor defense in depth) calls before any worker
// dispatch. In order:
//
// 1. The unchanged theoretical minimum (admitManifestExecutionTurnCapacity):
//    kExecutionCapacityInsufficientCode when even it cannot fit.
// 2. Per task: reservedProviderTurnsForTask(task) must fit into one worker
//    session (taskFitsWorkerSession(): the session limit is derived from the
//    floor, so only a floor above the Backend hard ceiling fails); otherwise
//    kTaskCapacityInsufficientCode naming the task, its reserved floor, the
//    ceiling and its required-read count. No amount of Run-wide budget can fix
//    this.
// 3. Sum: the reserved floors of every pending provider-backed task - and, under
//    ManifestRetryReservation::OneProtectedRetry, the same again as its protected retry
//    reservation - must fit into the Run-wide remaining turns; otherwise
//    kExecutionCapacityInsufficientCode.
//
// Terminal seeded states consume no capacity. maxParallelism is absent by
// design: it changes latency/concurrency, never the total reserved capacity.
// Messages carry only numeric facts and the task id - never prompts, file
// contents or raw responses.
[[nodiscard]] common::Result<ManifestWorkerReservationPlan>
admitManifestWorkerReservations(
    const ExecutionManifest& manifest,
    const std::unordered_map<std::string, ManifestTaskState>& seededStates,
    const ProviderExecutionBudgetPolicy& policy,
    const ProviderExecutionBudgetUsage& usage,
    ManifestRetryReservation retry = ManifestRetryReservation::OneProtectedRetry);

// The Planner-side twin of the admission above, run on the compiled, validated and
// verification-bound Manifest BEFORE it is accepted and frozen. The decision is the
// admission's own: it is asked with the largest Run-wide limit the policy permits (the
// Backend hard ceiling for an automatic turn limit - the raise the workflow would
// perform - or the frozen limit of a pinned one), with every task pending and the turns
// already used. A Manifest that passes here is therefore admitted by the post-freeze
// admission for the same policy and usage; that one stays as defense in depth.
//
// On a refusal the result is repairable feedback for the Planner: one finding per task
// whose floor exceeds a single worker session (code planner.capacity_task_floor, naming
// the task) and, when the floors do not fit the Run-wide limit, one graph-wide finding
// (planner.capacity_run_wide) with the numbers it needs to re-shape the work. Messages
// carry only numbers and task ids. Empty when the Manifest fits.
[[nodiscard]] std::vector<ExecutionManifestFinding> manifestCapacityFindings(
    const ExecutionManifest& manifest,
    const ProviderExecutionBudgetPolicy& policy,
    const ProviderExecutionBudgetUsage& usage,
    ManifestRetryReservation retry = ManifestRetryReservation::OneProtectedRetry);

// What an automatic Run-wide turn-limit extension did.
struct ManifestTurnLimitExtension {
    std::uint64_t previousMaxTurns {0};
    std::uint64_t newMaxTurns {0};
    std::uint64_t turnsStarted {0};
    // The pending tasks' primary floors, and (policy v3) their protected retry reservations.
    std::uint64_t reservedTurns {0};
    std::uint64_t retryReservedTurns {0};
    std::uint64_t pendingProviderTaskCount {0};
};

// Automatic turn-limit extension (policy kManifestTurnLimitPolicyId). The
// Backend knows the frozen Manifest's exact serial-safe requirement before any
// worker dispatch, so a shortfall of the Run-wide limit is a deterministic fact,
// not a guess. When the policy allows it (ProviderExecutionBudgetPolicy::
// autoExtendTurns, i.e. no explicit maxTurns) and
//
//     required = turnsStarted + sum(reservedProviderTurnsForTask(pending))
//                + sum(the same again, as the protected retry reservations -
//                      only under ManifestRetryReservation::OneProtectedRetry)
//                + kManifestTurnLimitHeadroomTurns
//
// exceeds the current limit but fits kProviderBudgetHardMaxTurns, the guard's
// limit is raised to exactly `required` and persisted (raiseTurnLimit), and the
// extension is returned for the caller's audit. Returns nullopt - changing
// nothing - when no raise is needed or allowed: a pinned limit, a requirement
// above the Backend ceiling, a task whose floor exceeds one worker session
// (taskFitsWorkerSession(); no Run-wide limit can fix that), or nothing pending. The caller then runs
// admitManifestWorkerReservations(), which reports the real refusal. The result
// fails only when the raise could not be persisted.
[[nodiscard]] common::Result<std::optional<ManifestTurnLimitExtension>>
extendManifestTurnLimitIfNeeded(
    ProviderSessionBudgetGuard& guard,
    const ExecutionManifest& manifest,
    const std::unordered_map<std::string, ManifestTaskState>& seededStates,
    ManifestRetryReservation retry = ManifestRetryReservation::OneProtectedRetry);

// Stable identity string of everything that determines execution-capacity
// semantics (the theoretical admission id, the reservation policy id and the
// automatic turn-limit extension id). Folded into planning identity so Retry
// can tell old and new policy apart.
[[nodiscard]] std::string executionCapacityPolicyIdentity();

} // namespace ai::application
