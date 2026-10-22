#include "ai/application/ExecutionManifestExecutionCapacity.hpp"

#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/application/ExecutionManifestPlannerTurnBudget.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_set>

namespace ai::application {
namespace {

std::string normalizedOutputKey(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Distinct normalized requiredOutputs across the task's REQUIRED acceptance
// requirements - exactly the set parseTaskVerification() demands same-attempt
// inspection evidence for.
std::size_t distinctRequiredOutputCount(const ExecutionManifestTask& task)
{
    if (!task.acceptanceContract.has_value()) return 0;
    std::unordered_set<std::string> outputs;
    for (const auto& requirement : task.acceptanceContract->requirements) {
        if (!requirement.required) continue;
        for (const auto& output : requirement.requiredOutputs) {
            outputs.insert(normalizedOutputKey(output));
        }
    }
    return outputs.size();
}

bool isTerminalSeededState(ManifestTaskStatus status) noexcept
{
    return status == ManifestTaskStatus::Completed
        || status == ManifestTaskStatus::Failed
        || status == ManifestTaskStatus::Skipped;
}

// Why the Run-wide turn limit was not raised to cover `requiredMaxTurns`, so a
// refusal tells the operator the next step instead of just the shortfall.
std::string turnLimitHint(
    const ProviderExecutionBudgetPolicy& policy,
    std::uint64_t requiredMaxTurns)
{
    if (!policy.autoExtendTurns) {
        return "; the turn limit was set explicitly and is not raised automatically";
    }
    if (requiredMaxTurns > kProviderBudgetHardMaxTurns) {
        return "; the automatic turn-limit extension would need "
            + std::to_string(requiredMaxTurns) + " turn(s), above the Backend ceiling of "
            + std::to_string(kProviderBudgetHardMaxTurns);
    }
    return {};
}

// The distinct EXISTING files a Propose task writes (ownedPaths, which the compiler fills from the
// frozen baseline - a new file is plannedCreatePaths and has nothing to read), not counting a path
// the task already reads as a required read (that turn is counted once). Compared case- and
// separator-insensitively. Worker_capacity_reservation_v4: a worker that modifies a file must see
// its current content, and the retry protocol allows ONE tool call per turn, so each such file is
// a potential read turn of its own. Run 80f904ed: 11 owned files, 1 required read, a floor of 2
// turns, a retry that spent 19 turns on single reads and ran the Run out of turns.
std::size_t existingWrittenFileCount(const ExecutionManifestTask& task) noexcept
{
    try {
        std::unordered_set<std::string> requiredReads;
        for (const auto& path : task.requiredReadPaths)
            requiredReads.insert(normalizedOutputKey(path.generic_string()));
        std::unordered_set<std::string> written;
        for (const auto& path : task.ownedPaths) {
            const auto key = normalizedOutputKey(path.generic_string());
            if (!requiredReads.contains(key)) written.insert(key);
        }
        return written.size();
    } catch (...) {
        // Allocation failure: the conservative upper bound, never a smaller floor.
        return task.ownedPaths.size();
    }
}

// What a task's serial-safe floor is made of, shape-specific: a Propose floor is
// one turn per required read plus one per existing file it writes, an Inspect floor is one
// turn per distinct required output (at least one) - never mix the two.
std::string floorBreakdown(const ExecutionManifestTask& task)
{
    std::ostringstream breakdown;
    if (task.requestedCapability == WorkspaceCapability::Inspect) {
        breakdown << std::max<std::size_t>(1, distinctRequiredOutputCount(task))
                  << " distinct required-output evidence turn(s)";
    } else {
        breakdown << task.requiredReadPaths.size() << " required read(s) + "
                  << existingWrittenFileCount(task) << " existing file(s) to write";
    }
    return breakdown.str();
}

} // namespace

std::uint64_t minimumProviderTurnsForTask(const ExecutionManifestTask& task) noexcept
{
    switch (task.requestedCapability) {
    case WorkspaceCapability::None:
        return 0;
    case WorkspaceCapability::Inspect:
        // Exempt from the requiredReadPathKeys gate at the protocol level -
        // see this function's doc comment in the header.
        return 1;
    case WorkspaceCapability::Propose:
        return task.requiredReadPaths.empty() ? 1 : 2;
    }
    return 1;
}

std::uint64_t reservedProviderTurnsForTask(const ExecutionManifestTask& task) noexcept
{
    switch (task.requestedCapability) {
    case WorkspaceCapability::None:
        return 0;
    case WorkspaceCapability::Inspect: {
        // parseTaskVerification(): every required output needs same-attempt
        // inspection evidence, and at least one successful inspection must
        // exist at all. Serial-safe: one evidence turn per distinct output.
        const auto outputs = static_cast<std::uint64_t>(distinctRequiredOutputCount(task));
        return std::max<std::uint64_t>(1, outputs) + 1;
    }
    case WorkspaceCapability::Propose:
        // One serial-safe evidence turn per required read and per distinct existing file the
        // task writes (v4), plus the terminal submission turn.
        return static_cast<std::uint64_t>(task.requiredReadPaths.size())
            + static_cast<std::uint64_t>(existingWrittenFileCount(task)) + 1;
    }
    return 1;
}

std::string manifestPrimaryReservationOwner(std::string_view taskId)
{
    return std::string(kPrimaryReservationOwnerPrefix) + std::string(taskId);
}

std::string manifestRetryReservationOwner(std::string_view taskId)
{
    return std::string(kRetryReservationOwnerPrefix) + std::string(taskId);
}

static_assert(kWorkerSessionBaselineTurns == kAgentToolTurnLoopMaxTurns,
    "The worker session baseline is the agent tool turn loop's own fixed length");
static_assert(kAgentToolTurnLoopMaxTotalToolCalls % kAgentToolTurnLoopMaxTurns == 0,
    "The baseline tool-call limit must be a whole number of calls per turn");

std::uint64_t workerSessionTurnLimitForTask(const ExecutionManifestTask& task) noexcept
{
    return std::min<std::uint64_t>(
        kProviderBudgetHardMaxTurns,
        std::max<std::uint64_t>(kWorkerSessionBaselineTurns, reservedProviderTurnsForTask(task)));
}

std::uint64_t workerSessionToolCallLimitForTask(const ExecutionManifestTask& task) noexcept
{
    constexpr std::uint64_t callsPerTurn =
        kAgentToolTurnLoopMaxTotalToolCalls / kAgentToolTurnLoopMaxTurns;
    return std::max<std::uint64_t>(
        kAgentToolTurnLoopMaxTotalToolCalls, callsPerTurn * workerSessionTurnLimitForTask(task));
}

bool taskFitsWorkerSession(const ExecutionManifestTask& task) noexcept
{
    return reservedProviderTurnsForTask(task) <= workerSessionTurnLimitForTask(task);
}

common::Result<ManifestExecutionTurnCapacity> admitManifestExecutionTurnCapacity(
    const ExecutionManifest& manifest,
    const std::unordered_map<std::string, ManifestTaskState>& seededStates,
    const ProviderExecutionBudgetPolicy& policy,
    const ProviderExecutionBudgetUsage& usage)
{
    ManifestExecutionTurnCapacity capacity;
    capacity.remainingTurns = usage.turnsStarted <= policy.maxTurns
        ? policy.maxTurns - usage.turnsStarted
        : 0;

    for (const auto& task : manifest.tasks) {
        if (task.requestedCapability == WorkspaceCapability::None) continue;
        const auto seeded = seededStates.find(task.id);
        if (seeded != seededStates.end() && isTerminalSeededState(seeded->second.status)) {
            continue;
        }
        ++capacity.pendingProviderTaskCount;
        capacity.minimumRequiredTurns += minimumProviderTurnsForTask(task);
    }

    if (usage.turnsStarted > policy.maxTurns
        || capacity.remainingTurns < capacity.minimumRequiredTurns) {
        std::ostringstream message;
        message << "The frozen Manifest requires at least "
                << capacity.minimumRequiredTurns
                << " remaining provider turn(s) for "
                << capacity.pendingProviderTaskCount
                << " pending provider-backed task(s), but the shared Run-wide budget has "
                << capacity.remainingTurns << " turn(s) remaining"
                << turnLimitHint(policy, usage.turnsStarted + capacity.minimumRequiredTurns);
        return common::Result<ManifestExecutionTurnCapacity>::failure(
            {std::string(kExecutionCapacityInsufficientCode), message.str()});
    }

    return common::Result<ManifestExecutionTurnCapacity>::ok(std::move(capacity));
}

common::Result<ManifestWorkerReservationPlan> admitManifestWorkerReservations(
    const ExecutionManifest& manifest,
    const std::unordered_map<std::string, ManifestTaskState>& seededStates,
    const ProviderExecutionBudgetPolicy& policy,
    const ProviderExecutionBudgetUsage& usage,
    ManifestRetryReservation retry)
{
    // 1. The unchanged theoretical protocol minimum stays the first gate.
    const auto theoretical = admitManifestExecutionTurnCapacity(
        manifest, seededStates, policy, usage);
    if (!theoretical.success()) {
        return common::Result<ManifestWorkerReservationPlan>::failure(*theoretical.error());
    }

    ManifestWorkerReservationPlan plan;
    plan.remainingTurns = theoretical.value().remainingTurns;

    // 2. Per-task serial-safe floor versus one worker session.
    for (const auto& task : manifest.tasks) {
        if (task.requestedCapability == WorkspaceCapability::None) continue;
        const auto seeded = seededStates.find(task.id);
        if (seeded != seededStates.end() && isTerminalSeededState(seeded->second.status)) {
            continue;
        }
        const std::uint64_t reserved = reservedProviderTurnsForTask(task);
        if (!taskFitsWorkerSession(task)) {
            std::ostringstream message;
            message << "Task '" << task.id << "' needs a serial-safe floor of "
                    << reserved << " provider turn(s) (" << floorBreakdown(task)
                    << " plus its terminal turn under policy "
                    << kWorkerCapacityReservationPolicyId
                    << "), which exceeds the Backend ceiling of "
                    << workerSessionTurnLimitForTask(task)
                    << " turn(s) for one worker session";
            return common::Result<ManifestWorkerReservationPlan>::failure(
                {std::string(kTaskCapacityInsufficientCode), message.str()});
        }
        // A duplicate key (only possible with duplicate task ids, which the Manifest validator
        // refuses) must fail closed here: std::unordered_map::emplace would silently drop the
        // second entry and two reservations would end up sharing one.
        const auto registerOwner = [&](std::string owner) {
            return plan.reservedTurnsByTask.emplace(std::move(owner), reserved).second;
        };
        if (!registerOwner(manifestPrimaryReservationOwner(task.id))) {
            return common::Result<ManifestWorkerReservationPlan>::failure(
                {std::string(kTurnReservationConflictCode),
                 "Task '" + task.id + "' maps to a reservation owner that another task already holds"});
        }
        plan.primaryReservedTurns += reserved;
        if (retry == ManifestRetryReservation::OneProtectedRetry) {
            // The task's protected retry reservation: its own owner, the same size, never
            // spendable by the primary session or by another task.
            if (!registerOwner(manifestRetryReservationOwner(task.id))) {
                return common::Result<ManifestWorkerReservationPlan>::failure(
                    {std::string(kTurnReservationConflictCode),
                     "Task '" + task.id
                         + "' maps to a retry reservation owner that another task already holds"});
            }
            plan.retryReservedTurns += reserved;
        }
        ++plan.pendingProviderTaskCount;
    }
    plan.totalReservedTurns = plan.primaryReservedTurns + plan.retryReservedTurns;

    // 3. The sum of every pending task's floor - and its protected retry reservation - versus
    //    the shared Run-wide remainder.
    if (plan.totalReservedTurns > plan.remainingTurns) {
        std::ostringstream message;
        message << "The frozen Manifest needs " << plan.totalReservedTurns
                << " reserved provider turn(s) for " << plan.pendingProviderTaskCount
                << " pending provider-backed task(s) under policy "
                << kWorkerCapacityReservationPolicyId;
        if (plan.retryReservedTurns > 0) {
            message << " (" << plan.primaryReservedTurns << " for the first attempts + "
                    << plan.retryReservedTurns << " protected for one retry each)";
        }
        message << ", but the shared Run-wide budget has " << plan.remainingTurns
                << " turn(s) remaining"
                << turnLimitHint(policy, usage.turnsStarted + plan.totalReservedTurns);
        return common::Result<ManifestWorkerReservationPlan>::failure(
            {std::string(kExecutionCapacityInsufficientCode), message.str()});
    }

    return common::Result<ManifestWorkerReservationPlan>::ok(std::move(plan));
}

std::vector<ExecutionManifestFinding> manifestCapacityFindings(
    const ExecutionManifest& manifest,
    const ProviderExecutionBudgetPolicy& policy,
    const ProviderExecutionBudgetUsage& usage,
    ManifestRetryReservation retry)
{
    // The post-freeze admission is the one authority: ask it with the largest Run-wide
    // limit the policy permits. For an automatic turn limit that is the ceiling the
    // workflow would raise it to; a pinned limit is asked as it is.
    ProviderExecutionBudgetPolicy permitted = policy;
    if (policy.autoExtendTurns) {
        permitted.maxTurns = std::max<std::uint64_t>(policy.maxTurns, kProviderBudgetHardMaxTurns);
    }
    if (admitManifestWorkerReservations(manifest, {}, permitted, usage, retry).success()) return {};

    const std::uint64_t copies =
        retry == ManifestRetryReservation::OneProtectedRetry ? 2 : 1;
    std::vector<ExecutionManifestFinding> findings;
    std::uint64_t totalReserved = 0;
    std::size_t pendingTasks = 0;
    std::ostringstream perTask;
    constexpr std::size_t kListedTasks = 12;
    for (const auto& task : manifest.tasks) {
        if (task.requestedCapability == WorkspaceCapability::None) continue;
        const std::uint64_t reserved = reservedProviderTurnsForTask(task);
        // Every pending task reserves its floor once for its first attempt and, under policy
        // v3, once more as its protected retry reservation.
        totalReserved += reserved * copies;
        if (pendingTasks < kListedTasks) {
            perTask << (pendingTasks == 0 ? "" : ", ") << task.id << " " << reserved;
        }
        ++pendingTasks;
        if (!taskFitsWorkerSession(task)) {
            std::ostringstream message;
            message << "Task '" << task.id << "' needs " << reserved << " provider turn(s) ("
                    << floorBreakdown(task) << " plus its terminal turn), more than the Backend "
                    << "ceiling of " << workerSessionTurnLimitForTask(task)
                    << " turn(s) for one worker session. Reduce this task's required reads "
                    << "(move optional context to referencePaths) or split the task so that "
                    << "each part reads and writes far fewer existing files.";
            findings.push_back({"planner.capacity_task_floor", message.str(), task.id});
        }
    }
    if (pendingTasks > kListedTasks) perTask << ", ... (" << pendingTasks << " tasks)";

    const std::uint64_t limit = permitted.maxTurns;
    const std::uint64_t remaining = usage.turnsStarted <= limit ? limit - usage.turnsStarted : 0;
    // Always added: it carries the totals and the guidance. A task finding implies it (a floor
    // above the session ceiling is above every Run-wide limit, so the sum cannot fit either).
    {
        std::ostringstream message;
        message << "The tasks need " << totalReserved << " provider turn(s) in total (" << perTask.str();
        if (copies > 1) {
            message << "; every floor is reserved twice - once for the task's first attempt and once "
                    << "as its own protected retry reservation, so a turn less in a task's floor "
                    << "saves two";
        }
        message << "; a propose task needs one turn per required read and per existing file it "
                << "writes plus one terminal turn, an "
                << "inspect task one per distinct required output plus one terminal turn), but this "
                << "Run can use at most " << limit << " turn(s) ("
                << (policy.autoExtendTurns
                        ? "the Backend ceiling; the limit is raised automatically up to it"
                        : "the Run's explicit turn limit")
                << ") and has already used " << usage.turnsStarted << ", leaving " << remaining
                << " for workers";
        if (totalReserved > remaining) {
            message << ": reduce the total by at least " << (totalReserved - remaining)
                    << ". This repair session itself uses Run turns (up to "
                    << kPlannerRecoveryTurnReserve << "), so leave a margin. ";
        } else {
            message << ". ";
        }
        message << "Reduce the number of required reads (keep only files a task must read before "
                << "proposing; optional context belongs in referencePaths), split a task that "
                << "writes many existing files into smaller tasks, or drop work that is not "
                << "essential.";
        findings.push_back({"planner.capacity_run_wide", message.str(), std::nullopt});
    }
    return findings;
}

common::Result<std::optional<ManifestTurnLimitExtension>> extendManifestTurnLimitIfNeeded(
    ProviderSessionBudgetGuard& guard,
    const ExecutionManifest& manifest,
    const std::unordered_map<std::string, ManifestTaskState>& seededStates,
    ManifestRetryReservation retry)
{
    using Outcome = common::Result<std::optional<ManifestTurnLimitExtension>>;
    const auto policy = guard.policy();
    if (!policy.autoExtendTurns) return Outcome::ok(std::nullopt);
    const auto usage = guard.usage();

    ManifestTurnLimitExtension extension;
    extension.previousMaxTurns = policy.maxTurns;
    extension.turnsStarted = usage.turnsStarted;
    for (const auto& task : manifest.tasks) {
        if (task.requestedCapability == WorkspaceCapability::None) continue;
        const auto seeded = seededStates.find(task.id);
        if (seeded != seededStates.end() && isTerminalSeededState(seeded->second.status)) {
            continue;
        }
        const std::uint64_t reserved = reservedProviderTurnsForTask(task);
        // A task that cannot fit a worker session (floor above the Backend ceiling) also pushes
        // `required` below past the same ceiling, so nothing is raised and the admission reports
        // the real refusal (kTaskCapacityInsufficientCode).
        extension.reservedTurns += reserved;
        if (retry == ManifestRetryReservation::OneProtectedRetry) {
            extension.retryReservedTurns += reserved;
        }
        ++extension.pendingProviderTaskCount;
    }
    if (extension.pendingProviderTaskCount == 0) return Outcome::ok(std::nullopt);

    const std::uint64_t required = extension.turnsStarted + extension.reservedTurns
        + extension.retryReservedTurns + kManifestTurnLimitHeadroomTurns;
    if (required <= policy.maxTurns || required > kProviderBudgetHardMaxTurns) {
        return Outcome::ok(std::nullopt);
    }
    extension.newMaxTurns = required;
    const auto raised = guard.raiseTurnLimit(required);
    if (!raised.success()) return Outcome::failure(*raised.error());
    return Outcome::ok(std::optional<ManifestTurnLimitExtension> {extension});
}

std::string executionCapacityPolicyIdentity()
{
    std::string identity = "admission=";
    identity += kPlannerTurnBudgetPartitionPolicyId;
    identity += ";worker-reservation=";
    identity += kWorkerCapacityReservationPolicyId;
    identity += ";turn-limit=";
    identity += kManifestTurnLimitPolicyId;
    return identity;
}

} // namespace ai::application
