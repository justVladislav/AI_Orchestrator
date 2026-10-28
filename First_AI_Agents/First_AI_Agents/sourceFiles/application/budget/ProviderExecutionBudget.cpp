#include "ai/application/budget/ProviderExecutionBudget.hpp"
#include "ai/application/budget/CacheDurationAccounting.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace ai::application {
namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

common::Result<void> validateLimit(
    std::uint64_t value,
    std::uint64_t ceiling,
    std::string_view name)
{
    if (value == 0 || value > ceiling) {
        return common::Result<void>::failure(error(
            "provider_budget.invalid_policy",
            std::string(name) + " must be positive and not exceed the Backend ceiling"));
    }
    return common::Result<void>::ok();
}

bool addWouldOverflow(std::uint64_t left, std::uint64_t right)
{
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

// How a reservation owner is named in a message: the Manifest namespaces are shown as the task they
// belong to (the key prefix is an implementation detail, not something an operator should read).
std::string describeReservationOwner(const std::string& owner)
{
    const auto strip = [&](std::string_view prefix) -> std::optional<std::string> {
        if (owner.size() > prefix.size() && owner.compare(0, prefix.size(), prefix) == 0) {
            return owner.substr(prefix.size());
        }
        return std::nullopt;
    };
    if (const auto task = strip(kPrimaryReservationOwnerPrefix)) {
        return "Task '" + *task + "'";
    }
    if (const auto task = strip(kRetryReservationOwnerPrefix)) {
        return "Task '" + *task + "' (protected retry reservation)";
    }
    return "Task '" + owner + "'";
}

enum class TurnUsageOutcome { Accumulated, Unavailable, Overflow, Inconsistent };

// Adds one completed provider response to the Run usage. Unusable usage (no
// tokens reported, or a sum past the representable range) is left out and marks
// the accounting non-authoritative; the caller decides whether that stops the Run.
//
// A response whose cache breakdown does not fit into its own input total
// (cache-read and/or cache-write larger than input_tokens) is NOT trusted: it
// earns no cache discount, so its whole input is spendable, and its breakdown is
// not recorded as if it were real. The full input and output still reach the Run
// totals (the provider processed them - the audit must not lose them), the
// accounting becomes non-authoritative, and the caller stops the Run.
TurnUsageOutcome accumulateTurnUsage(
    ProviderExecutionBudgetUsage& usage,
    const models::TokenUsage& turnUsage)
{
    if (turnUsage.inputTokens == 0 && turnUsage.outputTokens == 0) {
        usage.usageAuthoritative = false;
        return TurnUsageOutcome::Unavailable;
    }
    const bool consistent = turnUsage.cacheBreakdownIsConsistent();
    const auto durationProblem = checkCacheCreationDurationUsage(
        turnUsage.cacheCreationInputTokens, turnUsage.cacheCreationDuration);
    const bool durationConsistent = consistent
        && durationProblem == CacheCreationDurationUsageProblem::None;
    const std::uint64_t cacheRead = consistent ? turnUsage.cacheReadInputTokens : 0;
    const std::uint64_t cacheWrite = consistent ? turnUsage.cacheCreationInputTokens : 0;
    // See ProviderExecutionBudgetUsage::inputTokens's doc comment: cache-read
    // tokens (already billed once, re-served from the provider's cache at a
    // steep discount) are tracked separately and never counted against
    // maxInputTokens - only genuinely fresh input and cache-write tokens do.
    // cacheRead <= inputTokens holds here (it is zero for an inconsistent report).
    const std::uint64_t budgetCountableInput = turnUsage.inputTokens - cacheRead;
    if (addWouldOverflow(usage.inputTokens, budgetCountableInput)
        || addWouldOverflow(usage.outputTokens, turnUsage.outputTokens)
        || addWouldOverflow(usage.cacheReadInputTokens, cacheRead)
        || addWouldOverflow(usage.cacheCreationInputTokens, cacheWrite)
        || (durationConsistent
            && usage.cacheCreationDuration.known && turnUsage.cacheCreationDuration.known
            && (addWouldOverflow(
                    usage.cacheCreationDuration.fiveMinuteTokens,
                    turnUsage.cacheCreationDuration.fiveMinuteTokens)
                || addWouldOverflow(
                    usage.cacheCreationDuration.oneHourTokens,
                    turnUsage.cacheCreationDuration.oneHourTokens)))) {
        usage.usageAuthoritative = false;
        return TurnUsageOutcome::Overflow;
    }
    usage.inputTokens += budgetCountableInput;
    usage.outputTokens += turnUsage.outputTokens;
    usage.cacheReadInputTokens += cacheRead;
    const std::uint64_t previousCacheCreation = usage.cacheCreationInputTokens;
    usage.cacheCreationInputTokens += cacheWrite;
    if (!durationConsistent) {
        usage.cacheCreationDuration = {};
    } else if (!turnUsage.cacheCreationDuration.known) {
        if (cacheWrite > 0) usage.cacheCreationDuration = {};
    } else if (!usage.cacheCreationDuration.known) {
        if (previousCacheCreation == 0) {
            usage.cacheCreationDuration = turnUsage.cacheCreationDuration;
        }
    } else {
        usage.cacheCreationDuration.fiveMinuteTokens +=
            turnUsage.cacheCreationDuration.fiveMinuteTokens;
        usage.cacheCreationDuration.oneHourTokens +=
            turnUsage.cacheCreationDuration.oneHourTokens;
    }
    if (!consistent || !durationConsistent) {
        usage.usageAuthoritative = false;
        return TurnUsageOutcome::Inconsistent;
    }
    return TurnUsageOutcome::Accumulated;
}

} // namespace

common::Result<void> validateProviderExecutionBudgetPolicy(
    const ProviderExecutionBudgetPolicy& policy)
{
    if (policy.schemaVersion != kProviderExecutionBudgetSchemaVersion) {
        return common::Result<void>::failure(error(
            "provider_budget.unsupported_schema",
            "Provider execution budget schema is unsupported"));
    }
    const struct Limit {
        std::uint64_t value;
        std::uint64_t ceiling;
        const char* name;
    } limits[] = {
        {policy.maxInputTokens, kProviderBudgetHardMaxInputTokens, "maxInputTokens"},
        {policy.maxOutputTokens, kProviderBudgetHardMaxOutputTokens, "maxOutputTokens"},
        {policy.maxTurns, kProviderBudgetHardMaxTurns, "maxTurns"},
        {policy.maxToolCalls, kProviderBudgetHardMaxToolCalls, "maxToolCalls"},
        {policy.maxToolResultBytes, kProviderBudgetHardMaxToolResultBytes,
            "maxToolResultBytes"},
        {policy.maxWallTimeMs, kProviderBudgetHardMaxWallTimeMs, "maxWallTimeMs"},
        {policy.maxRequestTimeMs, kProviderBudgetHardMaxRequestTimeMs,
            "maxRequestTimeMs"},
    };
    for (const auto& limit : limits) {
        const auto valid = validateLimit(limit.value, limit.ceiling, limit.name);
        if (!valid.success()) return valid;
    }
    return common::Result<void>::ok();
}

common::Result<ProviderExecutionBudgetPolicy> resolveProviderExecutionBudget(
    const ProviderExecutionBudgetOverrides& overrides)
{
    ProviderExecutionBudgetPolicy policy;
    if (overrides.maxInputTokens) policy.maxInputTokens = *overrides.maxInputTokens;
    if (overrides.maxOutputTokens) policy.maxOutputTokens = *overrides.maxOutputTokens;
    if (overrides.maxTurns) policy.maxTurns = *overrides.maxTurns;
    // An explicit turn limit is pinned; only the Backend default may be raised.
    policy.autoExtendTurns = !overrides.maxTurns.has_value();
    if (overrides.maxToolCalls) policy.maxToolCalls = *overrides.maxToolCalls;
    if (overrides.maxToolResultBytes)
        policy.maxToolResultBytes = *overrides.maxToolResultBytes;
    if (overrides.maxWallTimeMs) policy.maxWallTimeMs = *overrides.maxWallTimeMs;
    if (overrides.maxRequestTimeMs)
        policy.maxRequestTimeMs = *overrides.maxRequestTimeMs;
    const auto valid = validateProviderExecutionBudgetPolicy(policy);
    if (!valid.success()) {
        return common::Result<ProviderExecutionBudgetPolicy>::failure(*valid.error());
    }
    return common::Result<ProviderExecutionBudgetPolicy>::ok(std::move(policy));
}

ProviderExecutionBudgetOverrides providerBudgetOverridesFromPolicy(
    const ProviderExecutionBudgetPolicy& policy)
{
    const ProviderExecutionBudgetPolicy defaults;
    ProviderExecutionBudgetOverrides overrides;
    if (policy.maxInputTokens != defaults.maxInputTokens) overrides.maxInputTokens = policy.maxInputTokens;
    if (policy.maxOutputTokens != defaults.maxOutputTokens) overrides.maxOutputTokens = policy.maxOutputTokens;
    if (!policy.autoExtendTurns) overrides.maxTurns = policy.maxTurns;
    if (policy.maxToolCalls != defaults.maxToolCalls) overrides.maxToolCalls = policy.maxToolCalls;
    if (policy.maxToolResultBytes != defaults.maxToolResultBytes) {
        overrides.maxToolResultBytes = policy.maxToolResultBytes;
    }
    if (policy.maxWallTimeMs != defaults.maxWallTimeMs) overrides.maxWallTimeMs = policy.maxWallTimeMs;
    if (policy.maxRequestTimeMs != defaults.maxRequestTimeMs) {
        overrides.maxRequestTimeMs = policy.maxRequestTimeMs;
    }
    return overrides;
}

ProviderSessionBudgetGuard::ProviderSessionBudgetGuard(
    ProviderExecutionBudgetPolicy policy,
    ProviderExecutionBudgetUsage initialUsage,
    PersistUsage persistUsage,
    Clock clock,
    std::size_t maxConcurrentRequests)
    : policy_(std::move(policy))
    , maxConcurrentRequests_(std::max<std::size_t>(1, maxConcurrentRequests))
    , usage_(std::move(initialUsage))
    , persistUsage_(std::move(persistUsage))
    , clock_(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); })
    , startedAt_(clock_())
{
    // A guard rebuilt from a persisted Run (restart, continuation repair) must
    // keep a limit the Backend already raised: the frozen policy alone would
    // report the turns that raise paid for as an over-spent budget.
    if (policy_.autoExtendTurns && usage_.grantedMaxTurns > policy_.maxTurns) {
        policy_.maxTurns = std::min(usage_.grantedMaxTurns, kProviderBudgetHardMaxTurns);
    }
}

std::size_t ProviderSessionBudgetGuard::setMaxConcurrentRequests(
    std::size_t maxConcurrentRequests)
{
    std::size_t previous = 0;
    {
        std::lock_guard lock(mutex_);
        previous = maxConcurrentRequests_;
        maxConcurrentRequests_ = std::max<std::size_t>(1, maxConcurrentRequests);
    }
    // Waiters re-evaluate their admission predicate; a raised bound frees them.
    requestAvailable_.notify_all();
    return previous;
}

std::size_t ProviderSessionBudgetGuard::maxConcurrentRequests() const
{
    std::lock_guard lock(mutex_);
    return maxConcurrentRequests_;
}

common::Result<void> ProviderSessionBudgetGuard::checkActiveLocked(
    std::chrono::steady_clock::time_point now) const
{
    if (!usage_.stopCode.empty()) {
        return common::Result<void>::failure(error(usage_.stopCode, stopMessage_));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - startedAt_).count();
    if (elapsed < 0 || static_cast<std::uint64_t>(elapsed) >= policy_.maxWallTimeMs) {
        return common::Result<void>::failure(error(
            "provider_budget.wall_time_limit_reached",
            "Run provider wall-time budget was exhausted"));
    }
    if (usage_.turnsStarted >= policy_.maxTurns) {
        return common::Result<void>::failure(error(
            "provider_budget.turn_limit_reached",
            "Run provider turn budget was exhausted"));
    }
    if (usage_.inputTokens >= policy_.maxInputTokens) {
        return common::Result<void>::failure(error(
            "provider_budget.input_token_limit_reached",
            "Run provider input-token budget was exhausted"));
    }
    if (usage_.outputTokens >= policy_.maxOutputTokens) {
        return common::Result<void>::failure(error(
            "provider_budget.output_token_limit_reached",
            "Run provider output-token budget was exhausted"));
    }
    if (usage_.toolCalls >= policy_.maxToolCalls) {
        return common::Result<void>::failure(error(
            "provider_budget.tool_call_limit_reached",
            "Run tool-call budget was exhausted"));
    }
    if (usage_.toolResultBytes >= policy_.maxToolResultBytes) {
        return common::Result<void>::failure(error(
            "provider_budget.tool_result_bytes_limit_reached",
            "Run tool-result byte budget was exhausted"));
    }
    return common::Result<void>::ok();
}

common::Result<void> ProviderSessionBudgetGuard::persistLocked()
{
    if (!persistUsage_) return common::Result<void>::ok();
    const auto persisted = persistUsage_(usage_);
    if (!persisted.success()) {
        usage_.stopCode = "provider_budget.persistence_failed";
        stopMessage_ = "Provider budget usage could not be persisted";
        return common::Result<void>::failure(error(usage_.stopCode, stopMessage_));
    }
    return common::Result<void>::ok();
}

common::Result<void> ProviderSessionBudgetGuard::stopLocked(
    std::string code,
    std::string message)
{
    if (usage_.stopCode.empty()) {
        usage_.stopCode = std::move(code);
        stopMessage_ = std::move(message);
        const auto persisted = persistLocked();
        if (!persisted.success()) return persisted;
    }
    return common::Result<void>::failure(error(usage_.stopCode, stopMessage_));
}

std::uint64_t ProviderSessionBudgetGuard::reservedUnusedLocked() const
{
    std::uint64_t total = 0;
    for (const auto& [owner, reservation] : turnReservations_) {
        static_cast<void>(owner);
        if (reservation.reserved > reservation.used) {
            total += reservation.reserved - reservation.used;
        }
    }
    return total;
}

std::uint64_t ProviderSessionBudgetGuard::unreservedRemainingLocked() const
{
    if (usage_.turnsStarted >= policy_.maxTurns) return 0;
    const std::uint64_t unstarted = policy_.maxTurns - usage_.turnsStarted;
    const std::uint64_t reserved = reservedUnusedLocked();
    return unstarted > reserved ? unstarted - reserved : 0;
}

common::Result<ProviderTurnAdmission> ProviderSessionBudgetGuard::beginTurnImpl(
    const std::string* owner,
    std::stop_token stopToken)
{
    std::unique_lock lock(mutex_);
    // Token usage is only authoritative after a provider response. Admit one
    // paid request at a time for the Run so concurrent workers cannot each
    // cross the remaining token allowance before any of them reports usage.
    // The wait is cooperatively cancellable; the active request itself is
    // bounded by the frozen per-request timeout.
    if (!requestAvailable_.wait(lock, stopToken, [this] {
            return inFlightCount_ < maxConcurrentRequests_ || !usage_.stopCode.empty();
        })) {
        return common::Result<ProviderTurnAdmission>::failure(error(
            "provider_budget.cancelled",
            "Provider request was cancelled while waiting for Run budget"));
    }
    const auto active = checkActiveLocked(clock_());
    if (!active.success()) {
        const auto stopped = stopLocked(active.error()->code, active.error()->message);
        return common::Result<ProviderTurnAdmission>::failure(*stopped.error());
    }

    // Reservation accounting. Everything below runs under the same mutex as
    // the turn counter itself, so no parallel worker can observe or take a
    // turn between the decision and the increment (no TOCTOU).
    //
    //   unstarted   = maxTurns - turnsStarted
    //   outstanding = sum(max(0, reserved - used)) over active owners
    //   surplus     = unstarted - outstanding
    //
    // An owner spends its own reservation first (surplus unchanged: both
    // `unstarted` and `outstanding` drop by one) and only afterwards the
    // surplus. A caller without an owning reservation may only use surplus.
    ProviderTurnReservation* own = nullptr;
    if (owner != nullptr) {
        const auto found = turnReservations_.find(*owner);
        if (found != turnReservations_.end()) own = &found->second;
    }
    const std::uint64_t surplus = unreservedRemainingLocked();
    bool consumedOwnReservation = false;
    if (own != nullptr && own->reserved > own->used) {
        consumedOwnReservation = true;
    } else if (surplus == 0 && !turnReservations_.empty()) {
        std::string message = owner != nullptr
            ? describeReservationOwner(*owner) + " exhausted its "
                + std::to_string(own != nullptr ? own->reserved : 0)
                + " reserved provider turn(s) and no unreserved Run-wide turn is left ("
            : std::string("An unowned provider request cannot use turns reserved for pending tasks (");
        message += std::to_string(policy_.maxTurns - usage_.turnsStarted)
            + " Run-wide turn(s) remain, "
            + std::to_string(reservedUnusedLocked()) + " reserved for pending tasks)";
        return common::Result<ProviderTurnAdmission>::failure(error(
            std::string(kTaskTurnAllocationExhaustedCode), std::move(message)));
    }

    ++inFlightCount_;
    ++usage_.turnsStarted;
    if (owner != nullptr) {
        if (own == nullptr) {
            // An owner without a registered reservation (for example a task
            // run again by the verification-repair wave after its own
            // reservation was released) is tracked from its first turn on,
            // so the lease below applies to it uniformly.
            own = &turnReservations_[*owner];
        }
        ++own->used;
        // Lease the NEXT turn while the unreserved surplus can still
        // guarantee one: once the owner's own reservation is spent, move one
        // surplus turn into its reservation atomically with this admission.
        // Without the lease a parallel worker could take the last surplus
        // turn between two of this owner's requests, and the owner would be
        // refused without ever having been offered a Backend-forced terminal
        // turn. With it, an unforced turn is always followed by an available
        // one; the turn admitted while no surplus is left is the owner's last
        // (ownerTurnsRemainingAfterAdmission == 0) and is forced terminal.
        // The lease is returned to the shared surplus when the reservation is
        // released.
        if (own->used >= own->reserved && unreservedRemainingLocked() > 0) {
            own->reserved = own->used + 1;
        }
    }
    ProviderTurnAdmission admission;
    admission.consumedOwnReservation = consumedOwnReservation;
    admission.ownerTurnsRemainingAfterAdmission =
        own != nullptr && own->reserved > own->used ? own->reserved - own->used : 0;
    const auto persisted = persistLocked();
    if (!persisted.success()) {
        --inFlightCount_;
        lock.unlock();
        requestAvailable_.notify_all();
        return common::Result<ProviderTurnAdmission>::failure(*persisted.error());
    }
    return common::Result<ProviderTurnAdmission>::ok(std::move(admission));
}

common::Result<void> ProviderSessionBudgetGuard::beginProviderTurn(
    std::stop_token stopToken)
{
    auto admitted = beginTurnImpl(nullptr, std::move(stopToken));
    if (!admitted.success()) {
        return common::Result<void>::failure(*admitted.error());
    }
    return common::Result<void>::ok();
}

common::Result<ProviderTurnAdmission> ProviderSessionBudgetGuard::beginOwnedProviderTurn(
    std::string_view owner,
    std::stop_token stopToken)
{
    const std::string ownerKey(owner);
    return beginTurnImpl(&ownerKey, std::move(stopToken));
}

common::Result<void> ProviderSessionBudgetGuard::reserveTurns(
    const std::unordered_map<std::string, std::uint64_t>& reservations)
{
    std::lock_guard lock(mutex_);
    std::uint64_t requested = 0;
    for (const auto& [owner, turns] : reservations) {
        if (owner.empty() || turns == 0) {
            return common::Result<void>::failure(error(
                "provider_budget.invalid_reservation",
                "A turn reservation needs a non-empty owner and a positive turn count"));
        }
        if (turnReservations_.contains(owner)) {
            return common::Result<void>::failure(error(
                std::string(kTurnReservationConflictCode),
                describeReservationOwner(owner)
                    + " already holds an active provider-turn reservation"));
        }
        if (addWouldOverflow(requested, turns)) {
            return common::Result<void>::failure(error(
                std::string(kExecutionCapacityInsufficientCode),
                "The requested provider-turn reservations exceed the representable range"));
        }
        requested += turns;
    }
    const std::uint64_t available = unreservedRemainingLocked();
    if (requested > available) {
        return common::Result<void>::failure(error(
            std::string(kExecutionCapacityInsufficientCode),
            "The pending tasks require " + std::to_string(requested)
                + " reserved provider turn(s), but only " + std::to_string(available)
                + " unreserved Run-wide turn(s) remain"));
    }
    for (const auto& [owner, turns] : reservations) {
        turnReservations_.emplace(owner, ProviderTurnReservation {turns, 0});
    }
    return common::Result<void>::ok();
}

void ProviderSessionBudgetGuard::releaseTurnReservation(std::string_view owner) noexcept
{
    {
        std::lock_guard lock(mutex_);
        const auto found = turnReservations_.find(std::string(owner));
        if (found == turnReservations_.end()) return;
        turnReservations_.erase(found);
    }
    requestAvailable_.notify_all();
}

void ProviderSessionBudgetGuard::releaseAllTurnReservations() noexcept
{
    {
        std::lock_guard lock(mutex_);
        turnReservations_.clear();
    }
    requestAvailable_.notify_all();
}

std::uint64_t ProviderSessionBudgetGuard::reservedUnusedTurns() const
{
    std::lock_guard lock(mutex_);
    return reservedUnusedLocked();
}

std::uint64_t ProviderSessionBudgetGuard::unreservedTurnsRemaining() const
{
    std::lock_guard lock(mutex_);
    return unreservedRemainingLocked();
}

std::optional<ProviderTurnReservation> ProviderSessionBudgetGuard::turnReservation(
    std::string_view owner) const
{
    std::lock_guard lock(mutex_);
    const auto found = turnReservations_.find(std::string(owner));
    if (found == turnReservations_.end()) return std::nullopt;
    return found->second;
}

common::Result<void> ProviderSessionBudgetGuard::raiseTurnLimit(
    std::uint64_t requiredMaxTurns)
{
    std::lock_guard lock(mutex_);
    if (!usage_.stopCode.empty()) {
        return common::Result<void>::failure(error(usage_.stopCode, stopMessage_));
    }
    if (!policy_.autoExtendTurns) {
        return common::Result<void>::failure(error(
            "provider_budget.turn_limit_pinned",
            "The Run's turn limit was set explicitly and is not raised automatically"));
    }
    if (requiredMaxTurns <= policy_.maxTurns) return common::Result<void>::ok();
    if (requiredMaxTurns > kProviderBudgetHardMaxTurns) {
        return common::Result<void>::failure(error(
            std::string(kExecutionCapacityInsufficientCode),
            "The Run needs a turn limit of " + std::to_string(requiredMaxTurns)
                + ", above the Backend ceiling of "
                + std::to_string(kProviderBudgetHardMaxTurns) + " turn(s)"));
    }
    const std::uint64_t previousMaxTurns = policy_.maxTurns;
    const std::uint64_t previousGranted = usage_.grantedMaxTurns;
    policy_.maxTurns = requiredMaxTurns;
    usage_.grantedMaxTurns = requiredMaxTurns;
    const auto persisted = persistLocked();
    if (!persisted.success()) {
        // persistLocked() has stopped the Run; do not leave a raised limit
        // behind that was never recorded.
        policy_.maxTurns = previousMaxTurns;
        usage_.grantedMaxTurns = previousGranted;
        return persisted;
    }
    return common::Result<void>::ok();
}

common::Result<void> ProviderSessionBudgetGuard::completeProviderTurn(
    const models::TokenUsage& turnUsage)
{
    std::unique_lock lock(mutex_);
    const auto releaseRequest = [&] {
        if (inFlightCount_ > 0) --inFlightCount_;
        lock.unlock();
        requestAvailable_.notify_all();
    };
    if (!usage_.stopCode.empty()) {
        // The Run already stopped, but this response was admitted before that
        // and is paid for: with parallel provider turns another request can
        // trip a cap (or the wall-time/persistence stop) while this one is in
        // flight. Its usage still belongs in the Run totals, otherwise the
        // reported tokens and cost understate what the provider billed. The
        // original terminal reason is kept - a late response never replaces
        // it, and never re-trips a cap that is already stopped.
        const auto stopped = common::Result<void>::failure(error(usage_.stopCode, stopMessage_));
        ++usage_.turnsCompleted;
        static_cast<void>(accumulateTurnUsage(usage_, turnUsage));
        if (persistUsage_) {
            // Best effort: persistLocked() would overwrite stopCode with
            // persistence_failed, losing the real reason the Run stopped.
            static_cast<void>(persistUsage_(usage_));
        }
        releaseRequest();
        return stopped;
    }
    ++usage_.turnsCompleted;
    const auto accumulated = accumulateTurnUsage(usage_, turnUsage);
    if (accumulated == TurnUsageOutcome::Unavailable) {
        const auto stopped = stopLocked(
            "provider_budget.usage_unavailable",
            "Provider did not report token usage for a completed turn");
        releaseRequest();
        return stopped;
    }
    if (accumulated == TurnUsageOutcome::Overflow) {
        const auto stopped = stopLocked(
            "provider_budget.usage_overflow",
            "Provider token usage exceeded the representable range");
        releaseRequest();
        return stopped;
    }
    if (accumulated == TurnUsageOutcome::Inconsistent) {
        // Checked before the ceilings: a malformed report must never be judged
        // (or passed) as if its cache breakdown were real.
        const auto stopped = stopLocked(
            "provider_budget.usage_invalid",
            "Provider reported inconsistent cache token usage for a completed turn");
        releaseRequest();
        return stopped;
    }
    if (usage_.inputTokens > policy_.maxInputTokens) {
        const auto stopped = stopLocked(
            "provider_budget.input_token_limit_reached",
            "Run provider input-token budget was exceeded by the completed turn");
        releaseRequest();
        return stopped;
    }
    if (usage_.outputTokens > policy_.maxOutputTokens) {
        const auto stopped = stopLocked(
            "provider_budget.output_token_limit_reached",
            "Run provider output-token budget was exceeded by the completed turn");
        releaseRequest();
        return stopped;
    }
    const auto persisted = persistLocked();
    releaseRequest();
    return persisted;
}

void ProviderSessionBudgetGuard::abandonProviderTurn() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (inFlightCount_ > 0) --inFlightCount_;
    }
    requestAvailable_.notify_all();
}

common::Result<void> ProviderSessionBudgetGuard::recordToolActivity(
    std::size_t calls,
    std::size_t returnedBytes)
{
    std::lock_guard lock(mutex_);
    if (!usage_.stopCode.empty()) {
        return common::Result<void>::failure(error(usage_.stopCode, stopMessage_));
    }
    const auto callCount = static_cast<std::uint64_t>(calls);
    const auto byteCount = static_cast<std::uint64_t>(returnedBytes);
    if (addWouldOverflow(usage_.toolCalls, callCount)
        || addWouldOverflow(usage_.toolResultBytes, byteCount)) {
        return stopLocked(
            "provider_budget.usage_overflow",
            "Tool usage exceeded the representable range");
    }
    usage_.toolCalls += callCount;
    usage_.toolResultBytes += byteCount;
    if (usage_.toolCalls > policy_.maxToolCalls) {
        return stopLocked(
            "provider_budget.tool_call_limit_reached",
            "Run tool-call budget was exceeded");
    }
    if (usage_.toolResultBytes > policy_.maxToolResultBytes) {
        return stopLocked(
            "provider_budget.tool_result_bytes_limit_reached",
            "Run tool-result byte budget was exceeded");
    }
    return persistLocked();
}

common::Result<void> ProviderSessionBudgetGuard::checkActive() const
{
    std::lock_guard lock(mutex_);
    return checkActiveLocked(clock_());
}

std::chrono::milliseconds ProviderSessionBudgetGuard::boundedRequestTimeout(
    std::chrono::milliseconds requested) const
{
    const auto maximum = std::chrono::milliseconds(policy_.maxRequestTimeMs);
    if (requested.count() <= 0) return maximum;
    return std::min(requested, maximum);
}

ProviderExecutionBudgetUsage ProviderSessionBudgetGuard::usage() const
{
    std::lock_guard lock(mutex_);
    return usage_;
}

ProviderExecutionBudgetPolicy ProviderSessionBudgetGuard::policy() const
{
    std::lock_guard lock(mutex_);
    return policy_;
}

} // namespace ai::application
