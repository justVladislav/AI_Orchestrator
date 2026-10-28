#include "ai/application/projects/TaskService.hpp"

#include "ai/application/IdGenerator.hpp"

#include <algorithm>
#include <cctype>

namespace ai::application {
namespace {

std::string trim(std::string value)
{
    const auto text = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), text);
    const auto last = std::find_if(value.rbegin(), value.rend(), text).base();
    return first < last ? std::string(first, last) : std::string {};
}

common::Error taskError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

TaskService::TaskService(ITaskRepository& tasks, IProjectRepository& projects)
    : tasks_(tasks), projects_(projects)
{
}

common::Result<ProjectTask> TaskService::createTask(const CreateTaskRequest& request)
{
    const auto project = projects_.findById(request.projectId);
    if (!project.success()) return common::Result<ProjectTask>::failure(*project.error());
    if (!project.value().has_value()) {
        return common::Result<ProjectTask>::failure(
            taskError("task.project_not_found", "Project not found"));
    }
    const std::string title = trim(request.title);
    if (title.empty()) {
        return common::Result<ProjectTask>::failure(
            taskError("task.empty_title", "Task title cannot be empty"));
    }
    const auto now = std::chrono::system_clock::now();
    ProjectTask task;
    task.id = generateUuid();
    task.projectId = request.projectId;
    task.title = title;
    task.description = trim(request.description);
    task.status = request.status;
    task.priority = request.priority;
    task.requiresAttention = request.requiresAttention;
    task.dueAt = request.dueAt;
    task.plannedRunAt = request.plannedRunAt;
    task.createdAt = now;
    task.updatedAt = now;
    return tasks_.insert(std::move(task));
}

common::Result<ProjectTask> TaskService::updateTask(const UpdateTaskRequest& request)
{
    const auto found = tasks_.findById(request.taskId);
    if (!found.success()) return common::Result<ProjectTask>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<ProjectTask>::failure(taskError("task.not_found", "Task not found"));
    }
    const std::string title = trim(request.title);
    if (title.empty()) {
        return common::Result<ProjectTask>::failure(
            taskError("task.empty_title", "Task title cannot be empty"));
    }
    ProjectTask task = *found.value();
    task.title = title;
    task.description = trim(request.description);
    task.status = request.status;
    task.priority = request.priority;
    task.requiresAttention = request.requiresAttention;
    task.dueAt = request.dueAt;
    task.plannedRunAt = request.plannedRunAt;
    task.updatedAt = std::chrono::system_clock::now();
    return tasks_.update(std::move(task));
}

common::Result<ProjectTask> TaskService::cancelTask(const TaskId& taskId)
{
    const auto found = tasks_.findById(taskId);
    if (!found.success()) return common::Result<ProjectTask>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<ProjectTask>::failure(taskError("task.not_found", "Task not found"));
    }
    ProjectTask task = *found.value();
    task.status = TaskStatus::Cancelled;
    task.requiresAttention = false;
    task.updatedAt = std::chrono::system_clock::now();
    return tasks_.update(std::move(task));
}

common::Result<std::vector<ProjectTask>> TaskService::listTasks(
    const std::optional<ProjectId>& projectId) const
{
    return tasks_.list(projectId);
}

common::Result<void> TaskService::linkRun(const TaskId& taskId, const RunId& runId)
{
    return tasks_.linkRun(taskId, runId);
}

common::Result<std::vector<RunId>> TaskService::linkedRuns(const TaskId& taskId) const
{
    return tasks_.linkedRuns(taskId);
}

} // namespace ai::application
