#include "ai/application/ExecutionManifestScheduler.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_set>

namespace ai::application {

const char* toString(ManifestTaskStatus status) noexcept
{
    switch (status) {
    case ManifestTaskStatus::Pending: return "pending";
    case ManifestTaskStatus::Running: return "running";
    case ManifestTaskStatus::Completed: return "completed";
    case ManifestTaskStatus::Failed: return "failed";
    case ManifestTaskStatus::Skipped: return "skipped";
    }
    return "pending";
}

ManifestTaskStatus manifestTaskStatusFromString(std::string_view value)
{
    if (value == "pending") return ManifestTaskStatus::Pending;
    if (value == "running") return ManifestTaskStatus::Running;
    if (value == "completed") return ManifestTaskStatus::Completed;
    if (value == "failed") return ManifestTaskStatus::Failed;
    if (value == "skipped") return ManifestTaskStatus::Skipped;
    throw std::invalid_argument("Unknown ManifestTaskStatus: " + std::string(value));
}

ExecutionManifestScheduler::ExecutionManifestScheduler(
    const ExecutionManifest& manifest,
    std::unordered_map<std::string, ManifestTaskState> initialStates)
    : manifest_(manifest)
    , states_(std::move(initialStates))
{
    for (const auto& task : manifest_.tasks) {
        states_.try_emplace(task.id, ManifestTaskState {});
        for (const auto& dependency : task.dependencies) {
            dependents_[dependency].push_back(task.id);
        }
    }
}

std::vector<std::string> ExecutionManifestScheduler::nextReadyBatch(
    std::size_t parallelismLimit) const
{
    std::vector<std::string> ready;
    if (parallelismLimit == 0) return ready;

    for (const auto& task : manifest_.tasks) {
        if (ready.size() >= parallelismLimit) break;
        const auto it = states_.find(task.id);
        if (it == states_.end() || it->second.status != ManifestTaskStatus::Pending) continue;

        const bool dependenciesSatisfied = std::all_of(
            task.dependencies.begin(), task.dependencies.end(),
            [this](const std::string& dependencyId) {
                const auto depIt = states_.find(dependencyId);
                return depIt != states_.end()
                    && depIt->second.status == ManifestTaskStatus::Completed;
            });
        if (dependenciesSatisfied) ready.push_back(task.id);
    }
    return ready;
}

void ExecutionManifestScheduler::markRunning(const std::string& taskId)
{
    const auto it = states_.find(taskId);
    if (it == states_.end()) return;
    it->second.status = ManifestTaskStatus::Running;
}

void ExecutionManifestScheduler::applyOutcome(
    const std::string& taskId, ManifestTaskState outcome)
{
    const auto it = states_.find(taskId);
    if (it == states_.end()) return;
    it->second = std::move(outcome);
    if (it->second.status == ManifestTaskStatus::Failed) {
        skipDependents(taskId);
    }
}

void ExecutionManifestScheduler::skipDependents(const std::string& failedTaskId)
{
    // Breadth-first cascade so a diamond dependency (two tasks both skipped
    // by the same failure, with a shared downstream task) is only visited
    // once, and a task several levels below the failure is still reached.
    std::deque<std::string> queue {failedTaskId};
    std::unordered_set<std::string> visited {failedTaskId};

    while (!queue.empty()) {
        const std::string current = std::move(queue.front());
        queue.pop_front();
        const auto depIt = dependents_.find(current);
        if (depIt == dependents_.end()) continue;

        for (const auto& dependentId : depIt->second) {
            if (!visited.insert(dependentId).second) continue;
            auto stateIt = states_.find(dependentId);
            if (stateIt == states_.end()) continue;
            if (stateIt->second.status == ManifestTaskStatus::Pending
                    || stateIt->second.status == ManifestTaskStatus::Running) {
                stateIt->second.status = ManifestTaskStatus::Skipped;
                stateIt->second.errorCode = "manifest.dependency_failed";
                stateIt->second.errorMessage =
                    "Skipped because a declared dependency did not complete";
            }
            queue.push_back(dependentId);
        }
    }
}

bool ExecutionManifestScheduler::isTerminal() const
{
    return std::none_of(states_.begin(), states_.end(), [](const auto& entry) {
        return entry.second.status == ManifestTaskStatus::Pending
            || entry.second.status == ManifestTaskStatus::Running;
    });
}

ManifestExecutionStatus ExecutionManifestScheduler::overallStatus() const
{
    if (!isTerminal()) return ManifestExecutionStatus::InProgress;
    const bool anyFailed = std::any_of(states_.begin(), states_.end(), [](const auto& entry) {
        return entry.second.status == ManifestTaskStatus::Failed;
    });
    return anyFailed ? ManifestExecutionStatus::Failed : ManifestExecutionStatus::Completed;
}

const ManifestTaskState& ExecutionManifestScheduler::stateOf(const std::string& taskId) const
{
    static const ManifestTaskState kUnknown {};
    const auto it = states_.find(taskId);
    return it == states_.end() ? kUnknown : it->second;
}

} // namespace ai::application
