#include "ai/application/execution/ManifestMemberUsage.hpp"

#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/execution/TaskRouteResolver.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>

namespace ai::application {

namespace {

constexpr std::size_t kAmbiguousMember = std::numeric_limits<std::size_t>::max();

void addSaturating(std::uint64_t& target, std::uint64_t value, bool& saturated) noexcept
{
    if (value > std::numeric_limits<std::uint64_t>::max() - target) {
        target = std::numeric_limits<std::uint64_t>::max();
        saturated = true;
        return;
    }
    target += value;
}

void compareField(std::vector<std::string>& mismatches, const char* field,
    std::uint64_t partition, std::uint64_t run)
{
    if (partition == run) return;
    mismatches.push_back(std::string("usage.partition_mismatch: ") + field + " partition="
        + std::to_string(partition) + " run=" + std::to_string(run));
}

} // namespace

const char* toString(ManifestUsagePriceState state) noexcept
{
    switch (state) {
    case ManifestUsagePriceState::None: return "none";
    case ManifestUsagePriceState::Known: return "known";
    case ManifestUsagePriceState::Partial: return "partial";
    case ManifestUsagePriceState::Unknown: return "unknown";
    }
    return "none";
}

ManifestUsagePriceState ManifestUsageTotals::priceState() const noexcept
{
    if (executions == 0) return ManifestUsagePriceState::None;
    if (unknownPriceExecutions == 0) return ManifestUsagePriceState::Known;
    // A model price was unset for a usage category of at least one execution. The recorded
    // costMicros then holds the part that WAS priced (CostBreakdown: a best-effort
    // underestimate), so a positive amount is an honest lower bound even when every
    // execution is unpriced - e.g. unset cache prices next to a set input/output price.
    if (costMicros > 0) return ManifestUsagePriceState::Partial;
    if (unknownPriceExecutions < executions) return ManifestUsagePriceState::Partial;
    // Nothing was priced at all: there is no amount, and it is not zero.
    return ManifestUsagePriceState::Unknown;
}

void ManifestUsageTotals::addExecution(const ModelExecution& execution) noexcept
{
    addSaturating(executions, 1, saturated);
    switch (execution.status) {
    case ModelExecutionStatus::Pending:
    case ModelExecutionStatus::Running:
        addSaturating(inFlight, 1, saturated);
        break;
    case ModelExecutionStatus::Completed:
        addSaturating(completed, 1, saturated);
        break;
    case ModelExecutionStatus::Partial:
        addSaturating(partial, 1, saturated);
        break;
    case ModelExecutionStatus::Failed:
        addSaturating(failed, 1, saturated);
        if (execution.errorCode == kModelExecutionOrphanedErrorCode
            || execution.errorCode == kModelExecutionInterruptedErrorCode) {
            addSaturating(interrupted, 1, saturated);
        }
        break;
    case ModelExecutionStatus::Cancelled:
        addSaturating(cancelled, 1, saturated);
        break;
    }
    addSaturating(inputTokens, execution.inputTokens, saturated);
    addSaturating(outputTokens, execution.outputTokens, saturated);
    addSaturating(costMicros, execution.costMicros, saturated);
    if (!execution.costPriceKnown) addSaturating(unknownPriceExecutions, 1, saturated);
}

void ManifestUsageTotals::add(const ManifestUsageTotals& other) noexcept
{
    addSaturating(executions, other.executions, saturated);
    addSaturating(completed, other.completed, saturated);
    addSaturating(partial, other.partial, saturated);
    addSaturating(failed, other.failed, saturated);
    addSaturating(cancelled, other.cancelled, saturated);
    addSaturating(inFlight, other.inFlight, saturated);
    addSaturating(interrupted, other.interrupted, saturated);
    addSaturating(inputTokens, other.inputTokens, saturated);
    addSaturating(outputTokens, other.outputTokens, saturated);
    addSaturating(costMicros, other.costMicros, saturated);
    addSaturating(unknownPriceExecutions, other.unknownPriceExecutions, saturated);
    if (other.saturated) saturated = true;
}

ManifestUsageTotals ManifestUsageProjection::lead() const noexcept
{
    ManifestUsageTotals total = planning;
    total.add(integration);
    return total;
}

ManifestUsageTotals ManifestUsageProjection::totalFor(const std::string& memberId) const noexcept
{
    const auto found = std::find_if(members.begin(), members.end(),
        [&](const ManifestMemberUsage& member) { return member.memberId == memberId; });
    if (found == members.end()) return {};
    ManifestUsageTotals total = found->tasks;
    if (found->lead) total.add(lead());
    return total;
}

std::vector<std::string> usagePartitionMismatches(const ManifestUsageProjection& projection)
{
    ManifestUsageTotals partition;
    for (const auto& member : projection.members) partition.add(member.tasks);
    partition.add(projection.planning);
    partition.add(projection.integration);
    partition.add(projection.unattributed);

    std::vector<std::string> mismatches;
    const auto& run = projection.runTotal;
    compareField(mismatches, "executions", partition.executions, run.executions);
    compareField(mismatches, "completed", partition.completed, run.completed);
    compareField(mismatches, "partial", partition.partial, run.partial);
    compareField(mismatches, "failed", partition.failed, run.failed);
    compareField(mismatches, "cancelled", partition.cancelled, run.cancelled);
    compareField(mismatches, "inFlight", partition.inFlight, run.inFlight);
    compareField(mismatches, "interrupted", partition.interrupted, run.interrupted);
    compareField(mismatches, "inputTokens", partition.inputTokens, run.inputTokens);
    compareField(mismatches, "outputTokens", partition.outputTokens, run.outputTokens);
    compareField(mismatches, "costMicros", partition.costMicros, run.costMicros);
    compareField(mismatches, "unknownPriceExecutions",
        partition.unknownPriceExecutions, run.unknownPriceExecutions);
    return mismatches;
}

ManifestUsageProjection projectManifestMemberUsage(
    const ExecutionManifest& manifest,
    const std::vector<ModelExecution>& executions)
{
    ManifestUsageProjection result;
    if (!manifest.routing.has_value() || ManifestRouting::recordProblem(*manifest.routing)) {
        return result;
    }
    const auto& record = *manifest.routing;
    const auto leadMember = std::find_if(record.members.begin(), record.members.end(),
        [](const ManifestRoutingMember& member) { return member.leadArchitect; });
    if (leadMember == record.members.end()) return result;
    result.available = true;

    result.members.reserve(record.members.size());
    for (const auto& member : record.members) {
        result.members.push_back({member.memberId, member.leadArchitect, {}});
    }

    // The writer's own position allocation, inverted: position -> member index.
    std::unordered_map<std::uint32_t, std::size_t> memberByPosition;
    std::optional<std::uint32_t> integrationPosition;
    std::string planProblem;
    const std::uint64_t workerBase = static_cast<std::uint64_t>(leadMember->position) + 1;
    if (workerBase > std::numeric_limits<std::uint32_t>::max()) {
        planProblem = "model_execution.position_invalid: The lead position leaves no room for "
                      "worker model executions";
    } else {
        const auto plan = allocateModelExecutionPositionsFromRecord(
            record, manifest, static_cast<std::uint32_t>(workerBase));
        if (!plan.success()) {
            planProblem = plan.error()->code + ": " + plan.error()->message;
        } else {
            for (const auto& task : manifest.tasks) {
                const auto slot = plan.value().byTaskId.find(task.id);
                if (slot == plan.value().byTaskId.end()) continue;
                const auto* assignee = ManifestRouting::assigneeOf(record, task);
                if (assignee == nullptr) continue;
                const auto index = static_cast<std::size_t>(assignee - record.members.data());
                const auto [entry, inserted] = memberByPosition.emplace(slot->second, index);
                if (!inserted && entry->second != index) entry->second = kAmbiguousMember;
            }
            if (plan.value().lastPosition < std::numeric_limits<std::uint32_t>::max()) {
                integrationPosition = plan.value().lastPosition + 1;
            }
        }
    }

    const auto unattribute = [&](const ModelExecution& execution, std::string reason) {
        result.unattributed.addExecution(execution);
        result.unattributedExecutions.push_back(
            {execution.id, execution.position, execution.attempt, execution.role, std::move(reason)});
    };

    for (const auto& execution : executions) {
        result.runTotal.addExecution(execution);

        // The two lead rows are told apart from a worker by position AND role: a worker's
        // role is user-chosen text and may well be "planner".
        if (execution.position == leadMember->position && execution.role == "planner") {
            result.planning.addExecution(execution);
            continue;
        }
        if (integrationPosition.has_value() && execution.position == *integrationPosition
            && execution.role == "integration") {
            result.integration.addExecution(execution);
            continue;
        }
        if (!planProblem.empty()) {
            unattribute(execution, "the position allocation is unavailable for this Manifest");
            continue;
        }
        const auto found = memberByPosition.find(execution.position);
        if (found == memberByPosition.end()) {
            unattribute(execution, "position " + std::to_string(execution.position)
                + " matches no planner, integration or task slot of the frozen Manifest");
        } else if (found->second == kAmbiguousMember) {
            unattribute(execution, "position " + std::to_string(execution.position)
                + " is allocated to tasks of different members");
        } else {
            result.members[found->second].tasks.addExecution(execution);
        }
    }

    if (!planProblem.empty()) {
        result.diagnostics.push_back("usage.position_plan_unavailable: " + planProblem);
    }
    if (result.unattributed.executions > 0) {
        result.diagnostics.push_back("usage.unattributed: "
            + std::to_string(result.unattributed.executions)
            + " execution(s) match no member, planning or integration slot of the frozen "
              "Manifest; the position allocation may have drifted from the persisted rows");
    }
    auto mismatches = usagePartitionMismatches(result);
    result.partitionExact = mismatches.empty();
    for (auto& mismatch : mismatches) result.diagnostics.push_back(std::move(mismatch));

    bool saturated = result.runTotal.saturated || result.unattributed.saturated
        || result.planning.saturated || result.integration.saturated;
    for (const auto& member : result.members) saturated = saturated || member.tasks.saturated;
    if (saturated) {
        result.diagnostics.push_back(
            "usage.counter_saturated: a usage counter reached its ceiling; totals are lower bounds");
    }
    return result;
}

} // namespace ai::application
