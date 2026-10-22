#pragma once

#include "ai/application/IProjectRepository.hpp"
#include "ai/application/ITaskRepository.hpp"

namespace ai::application {

class TaskService final {
public:
    TaskService(ITaskRepository& tasks, IProjectRepository& projects);

    common::Result<ProjectTask> createTask(const CreateTaskRequest& request);
    common::Result<ProjectTask> updateTask(const UpdateTaskRequest& request);
    common::Result<ProjectTask> cancelTask(const TaskId& taskId);
    [[nodiscard]] common::Result<std::vector<ProjectTask>> listTasks(
        const std::optional<ProjectId>& projectId = std::nullopt) const;
    common::Result<void> linkRun(const TaskId& taskId, const RunId& runId);
    [[nodiscard]] common::Result<std::vector<RunId>> linkedRuns(const TaskId& taskId) const;

private:
    ITaskRepository& tasks_;
    IProjectRepository& projects_;
};

} // namespace ai::application
