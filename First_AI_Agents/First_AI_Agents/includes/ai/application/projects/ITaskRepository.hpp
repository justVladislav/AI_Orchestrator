#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

class ITaskRepository {
public:
    virtual ~ITaskRepository() = default;
    virtual common::Result<ProjectTask> insert(ProjectTask task) = 0;
    virtual common::Result<ProjectTask> update(ProjectTask task) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ProjectTask>> findById(
        const TaskId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ProjectTask>> list(
        const std::optional<ProjectId>& projectId = std::nullopt) const = 0;
    virtual common::Result<void> linkRun(const TaskId& taskId, const RunId& runId) = 0;
    [[nodiscard]] virtual common::Result<std::vector<RunId>> linkedRuns(
        const TaskId& taskId) const = 0;
};

} // namespace ai::application
