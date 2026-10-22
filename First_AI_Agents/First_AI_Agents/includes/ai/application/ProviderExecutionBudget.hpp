#pragma once

#include "ai/common/Result.hpp"
#include "ai/models/TokenUsage.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ai::application {

inline constexpr std::string_view kProviderExecutionBudgetSchemaVersion =
    "provider-execution-budget.v1";

// Stable Backend codes for the reservation-aware layer that sits on top of the
// Run-wide turn ceiling. The hard ceiling itself is still reported through the
// pre-existing `provider_budget.turn_limit_reached`.
//
// - Execution capacity: the sum of the turns that must be reserved for the
//   pending provider-backed tasks does not fit into the Run-wide remaining
//   turns. Raised both by post-freeze admission and by the guard's atomic
//   reservation registration.
// - Task turn allocation exhausted: one specific task consumed its own
//   reservation and no unreserved Run-wide turn is left for it. Deliberately
//   NOT sticky - other tasks still hold reservations and must be able to use
//   them.
// - Reservation conflict: an owner already holds an active reservation.
inline constexpr std::string_view kExecutionCapacityInsufficientCode =
    "provider_budget.execution_capacity_insufficient";
inline constexpr std::string_view kTaskTurnAllocationExhaustedCode =
    "provider_budget.task_turn_allocation_exhausted";
inline constexpr std::string_view kTurnReservationConflictCode =
    "provider_budget.turn_reservation_conflict";

// Owner-key namespaces of the Manifest worker reservations (see ExecutionManifestExecutionCapacity:
// manifestPrimaryReservationOwner / manifestRetryReservationOwner). A task id is never used as an
// owner key by itself: the primary and the retry reservation of a task live in two disjoint key
// spaces, so no task id - not even one that reads like another task's retry key - can make two
// reservations share a key. The guard only uses the prefixes to word its messages.
inline constexpr std::string_view kPrimaryReservationOwnerPrefix = "primary:";
inline constexpr std::string_view kRetryReservationOwnerPrefix = "retry:";

// Absolute Backend ceilings. A saved profile may only select lower values.
inline constexpr std::uint64_t kProviderBudgetHardMaxInputTokens = 1'000'000;
inline constexpr std::uint64_t kProviderBudgetHardMaxOutputTokens = 128'000;
inline constexpr std::uint64_t kProviderBudgetHardMaxTurns = 64;
inline constexpr std::uint64_t kProviderBudgetHardMaxToolCalls = 256;
inline constexpr std::uint64_t kProviderBudgetHardMaxToolResultBytes = 16ULL * 1024 * 1024;
inline constexpr std::uint64_t kProviderBudgetHardMaxWallTimeMs = 30ULL * 60 * 1000;
inline constexpr std::uint64_t kProviderBudgetHardMaxRequestTimeMs = 5ULL * 60 * 1000;

struct ProviderExecutionBudgetOverrides {
    std::optional<std::uint64_t> maxInputTokens;
    std::optional<std::uint64_t> maxOutputTokens;
    std::optional<std::uint64_t> maxTurns;
    std::optional<std::uint64_t> maxToolCalls;
    std::optional<std::uint64_t> maxToolResultBytes;
    std::optional<std::uint64_t> maxWallTimeMs;
    std::optional<std::uint64_t> maxRequestTimeMs;
};

// True when any limit is set explicitly (an empty override means "the Backend defaults").
[[nodiscard]] inline bool hasExplicitProviderBudgetLimits(
    const ProviderExecutionBudgetOverrides& overrides) noexcept
{
    return overrides.maxInputTokens || overrides.maxOutputTokens || overrides.maxTurns
        || overrides.maxToolCalls || overrides.maxToolResultBytes || overrides.maxWallTimeMs
        || overrides.maxRequestTimeMs;
}

// Immutable resolved policy snapshotted onto a Run before Planner activity.
struct ProviderExecutionBudgetPolicy {
    std::string schemaVersion {kProviderExecutionBudgetSchemaVersion};
    std::uint64_t maxInputTokens {500'000};
    std::uint64_t maxOutputTokens {64'000};
    std::uint64_t maxTurns {24};
    std::uint64_t maxToolCalls {128};
    std::uint64_t maxToolResultBytes {8ULL * 1024 * 1024};
    std::uint64_t maxWallTimeMs {15ULL * 60 * 1000};
    std::uint64_t maxRequestTimeMs {240'000};
    // True when the Run's turn limit was NOT set explicitly (no maxTurns
    // override): the Manifest workflow may then raise the Run-wide limit once,
    // after the Manifest is frozen, to exactly what the frozen task set needs
    // (ProviderSessionBudgetGuard::raiseTurnLimit, never above
    // kProviderBudgetHardMaxTurns). An explicit maxTurns pins the limit: it is
    // never extended. This flag is part of the frozen policy; the raised value
    // itself is Run state (ProviderExecutionBudgetUsage::grantedMaxTurns).
    // A bare policy is pinned (fail-closed); only resolveProviderExecutionBudget()
    // - the one place a Run's budget is resolved - turns the automatic mode on.
    bool autoExtendTurns {false};
};

struct ProviderExecutionBudgetUsage {
    std::uint64_t turnsStarted {0};
    std::uint64_t turnsCompleted {0};
    // Real-Run observation (498dd8c6-b76e-4271-b078-b4b321565222, 2026-09-15):
    // unlike models::TokenUsage::inputTokens (the honest full total, see its
    // class comment), this field deliberately EXCLUDES cache-read tokens.
    // Anthropic's Messages API has no server-side conversation continuation
    // (no equivalent to OpenAI's previous_response_id): every turn resends
    // the entire growing history, so without this exclusion a session's
    // already-cached, ~10x-cheaper prefix would still be billed at full
    // weight against this cap on every single turn, and any moderately deep
    // Anthropic session (here: a Planner still in its inspection phase, no
    // manifest even submitted, 9 turns / 18 tool calls / 23,886 returned
    // bytes) exhausts a 500,000-token cap that a comparable OpenAI session
    // (which never resends prior turns) does not come close to. Cache-write
    // and genuinely fresh tokens still count fully - only re-served cache
    // reads are excluded, so a session that keeps growing with real new
    // content is still bounded. A response whose cache breakdown does not fit
    // into its own input total is malformed and earns NO discount: its whole
    // input counts here, its breakdown is not recorded, usageAuthoritative
    // becomes false and the Run stops with provider_budget.usage_invalid.
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t toolCalls {0};
    std::uint64_t toolResultBytes {0};
    // Accumulated separately from inputTokens above (see its comment) -
    // present for UI/audit transparency only, never compared against
    // policy_.maxInputTokens.
    std::uint64_t cacheReadInputTokens {0};
    // Real-Run observation (af4e5703-90c7-401d-9235-808872719abf,
    // 2026-09-15): cache-write tokens already count fully in inputTokens
    // above (no exclusion needed, unlike cache-read) - this field exists
    // purely so a live consumer (RunManager::persistProviderBudgetUsage)
    // can reconstruct models::TokenUsage's full cache breakdown
    // (inputTokens + cacheReadInputTokens = the honest total;
    // cacheCreationInputTokens here maps directly) without a second,
    // divergent source of truth for "how much was cached".
    std::uint64_t cacheCreationInputTokens {0};
    // ACD-3: provider-reported duration split for the aggregate cache writes.
    // It is audit state only and never changes admission arithmetic.
    models::TokenUsage::CacheCreationDurationShare cacheCreationDuration;
    // The Run-wide turn limit the Backend raised the policy to (0 = never
    // raised). The frozen policy's maxTurns stays the immutable Run input; this
    // is mutable Run state that a guard rebuilt from the persisted Run (restart,
    // continuation repair) applies again, so the raised limit is never lost.
    std::uint64_t grantedMaxTurns {0};
    bool usageAuthoritative {true};
    std::string stopCode;
};

// Result of admitting one provider turn for a reservation owner. Computed
// atomically under the guard's mutex together with the admission itself, so a
// caller can decide (for example to force a terminal-only next turn) without a
// check-then-act race against a parallel worker.
struct ProviderTurnAdmission {
    // Turns GUARANTEED to the owner after the admitted one: its unused
    // reservation, including the one surplus turn the guard leases to it while
    // the unreserved surplus lasts (see beginTurnImpl). A guaranteed turn can
    // never be taken by another task, so an unforced turn is always followed
    // by an available one. Zero means the admitted turn is the owner's last
    // available turn - the one a caller forces to a terminal-only policy.
    std::uint64_t ownerTurnsRemainingAfterAdmission {0};
    bool consumedOwnReservation {false};
};

struct ProviderTurnReservation {
    std::uint64_t reserved {0};
    std::uint64_t used {0};
};

[[nodiscard]] common::Result<ProviderExecutionBudgetPolicy>
resolveProviderExecutionBudget(const ProviderExecutionBudgetOverrides& overrides);

[[nodiscard]] common::Result<void> validateProviderExecutionBudgetPolicy(
    const ProviderExecutionBudgetPolicy& policy);

// The overrides that resolve back to `policy`: a limit that differs from the Backend default is
// explicit, and maxTurns is explicit exactly when the policy is pinned (not automatic). What
// Edit & rerun puts back into the dialog, so a rerun neither loses a limit somebody chose nor
// turns the Backend default into a typed one.
[[nodiscard]] ProviderExecutionBudgetOverrides providerBudgetOverridesFromPolicy(
    const ProviderExecutionBudgetPolicy& policy);

// One shared instance belongs to one Run. Planner primary/repair sessions and
// all worker sessions consult it before every provider request and report all
// completed turns/tool results back to it. The mutex makes parallel workers
// consume one atomic Run-wide allowance.
class ProviderSessionBudgetGuard final {
public:
    using PersistUsage =
        std::function<common::Result<void>(const ProviderExecutionBudgetUsage&)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // maxConcurrentRequests bounds how many provider requests this guard
    // admits at once for the whole Run. The default of 1 matches the
    // Manifest workflow's Planner/worker sessions, which never need true
    // concurrent HTTP dispatch and get the tightest possible overshoot bound
    // (at most one in-flight turn's usage can land after the cap is
    // reached). A caller whose workers are genuinely independent and meant
    // to run in parallel (see ExecutionProfileWorkflowRunner's
    // maxParallelism) may raise this so real concurrency is preserved; the
    // worst-case token overshoot beyond the cap is then bounded by at most
    // maxConcurrentRequests in-flight turns, still never unbounded.
    ProviderSessionBudgetGuard(
        ProviderExecutionBudgetPolicy policy,
        ProviderExecutionBudgetUsage initialUsage = {},
        PersistUsage persistUsage = {},
        Clock clock = {},
        std::size_t maxConcurrentRequests = 1);

    // The Run-wide bound on provider requests in flight. A composition whose
    // tasks run in parallel (ExecutionManifestExecutor does this for the length
    // of an execution, then restores the previous value) raises it to the number
    // of tasks it dispatches at once, so their provider turns really overlap
    // instead of queueing behind the constructor default of 1. Raising wakes
    // every waiting request; lowering below the current in-flight count only
    // delays NEW admissions - an admitted request is never revoked. Values below
    // 1 are clamped to 1. Returns the previous bound.
    std::size_t setMaxConcurrentRequests(std::size_t maxConcurrentRequests);
    [[nodiscard]] std::size_t maxConcurrentRequests() const;

    // Admits one provider turn that is not charged to any reservation owner
    // (for example the Planner). While reservations are outstanding it may
    // only use the UNRESERVED Run-wide surplus and can never consume a turn
    // reserved for a pending task.
    [[nodiscard]] common::Result<void> beginProviderTurn(
        std::stop_token stopToken = {});

    // Admits one provider turn charged to `owner` (a Manifest task id). The
    // owner spends its own reservation first and only afterwards the
    // unreserved surplus; it can never spend a turn reserved for another
    // owner. Exhaustion is reported as kTaskTurnAllocationExhaustedCode
    // without stopping the Run-wide guard.
    [[nodiscard]] common::Result<ProviderTurnAdmission> beginOwnedProviderTurn(
        std::string_view owner,
        std::stop_token stopToken = {});

    // Atomically registers one turn reservation per owner (all or nothing).
    // The sum must fit into the Run-wide turns that are neither started nor
    // already reserved; otherwise nothing is registered and
    // kExecutionCapacityInsufficientCode is returned. An owner that already
    // holds an active reservation is rejected with kTurnReservationConflictCode.
    // Reservations are in-memory scheduling state only: the persisted usage
    // (turnsStarted) stays the single durable record of consumption.
    [[nodiscard]] common::Result<void> reserveTurns(
        const std::unordered_map<std::string, std::uint64_t>& reservations);

    // Returns the owner's UNUSED reservation to the shared surplus. Idempotent;
    // releasing an unknown or already released owner is a no-op. Consumption
    // that already happened stays counted in the persisted usage.
    void releaseTurnReservation(std::string_view owner) noexcept;
    void releaseAllTurnReservations() noexcept;

    // Sum of the unused portions of every active reservation.
    [[nodiscard]] std::uint64_t reservedUnusedTurns() const;
    // Run-wide turns that are neither started nor reserved for a pending owner.
    [[nodiscard]] std::uint64_t unreservedTurnsRemaining() const;
    [[nodiscard]] std::optional<ProviderTurnReservation> turnReservation(
        std::string_view owner) const;

    // Raises the Run-wide turn limit to `requiredMaxTurns` (never lowers it) and
    // persists the raised value in the usage record before returning, so the
    // raise is durable before any worker is dispatched. Only a policy with
    // autoExtendTurns may be raised, and never above kProviderBudgetHardMaxTurns;
    // otherwise nothing changes and the call fails. A limit that already covers
    // `requiredMaxTurns` is a successful no-op.
    [[nodiscard]] common::Result<void> raiseTurnLimit(std::uint64_t requiredMaxTurns);

    // Records a finished response and releases its request slot. A response that
    // arrives after the Run already stopped (another parallel turn tripped a cap
    // while this one was in flight) is still billed, so its usage is added to the
    // Run totals and persisted; the call still fails with the ORIGINAL stop code,
    // which a late response never replaces.
    [[nodiscard]] common::Result<void> completeProviderTurn(
        const models::TokenUsage& usage);
    void abandonProviderTurn() noexcept;
    [[nodiscard]] common::Result<void> recordToolActivity(
        std::size_t calls,
        std::size_t returnedBytes);
    [[nodiscard]] common::Result<void> checkActive() const;
    [[nodiscard]] std::chrono::milliseconds boundedRequestTimeout(
        std::chrono::milliseconds requested) const;
    [[nodiscard]] ProviderExecutionBudgetUsage usage() const;
    // A snapshot: the Run-wide turn limit can be raised (raiseTurnLimit), so a
    // reference into the guard would race with that.
    [[nodiscard]] ProviderExecutionBudgetPolicy policy() const;

private:
    [[nodiscard]] common::Result<void> checkActiveLocked(
        std::chrono::steady_clock::time_point now) const;
    [[nodiscard]] common::Result<void> persistLocked();
    [[nodiscard]] common::Result<void> stopLocked(
        std::string code,
        std::string message);
    [[nodiscard]] std::uint64_t reservedUnusedLocked() const;
    [[nodiscard]] std::uint64_t unreservedRemainingLocked() const;
    [[nodiscard]] common::Result<ProviderTurnAdmission> beginTurnImpl(
        const std::string* owner,
        std::stop_token stopToken);

    ProviderExecutionBudgetPolicy policy_;
    std::unordered_map<std::string, ProviderTurnReservation> turnReservations_;
    mutable std::mutex mutex_;
    std::condition_variable_any requestAvailable_;
    std::size_t inFlightCount_ {0};
    std::size_t maxConcurrentRequests_ {1};
    ProviderExecutionBudgetUsage usage_;
    PersistUsage persistUsage_;
    Clock clock_;
    std::chrono::steady_clock::time_point startedAt_;
    std::string stopMessage_;
};

} // namespace ai::application
