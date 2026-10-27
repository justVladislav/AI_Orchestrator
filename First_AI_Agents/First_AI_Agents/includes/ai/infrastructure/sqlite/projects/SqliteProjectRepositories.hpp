#pragma once

#include "ai/application/verification/IProjectReadinessRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/projects/ITaskRepository.hpp"
#include "ai/application/IWorkspaceStateRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace ai::infrastructure {

class SqliteTaskRepository final : public application::ITaskRepository {
public:
    explicit SqliteTaskRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ProjectTask> insert(application::ProjectTask task) override;
    common::Result<application::ProjectTask> update(application::ProjectTask task) override;
    [[nodiscard]] common::Result<std::optional<application::ProjectTask>> findById(
        const application::TaskId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ProjectTask>> list(
        const std::optional<application::ProjectId>& projectId = std::nullopt) const override;
    common::Result<void> linkRun(
        const application::TaskId& taskId, const application::RunId& runId) override;
    [[nodiscard]] common::Result<std::vector<application::RunId>> linkedRuns(
        const application::TaskId& taskId) const override;
private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the project persistence port. */
class SqliteProjectRepository final : public application::IProjectRepository {
public:
    explicit SqliteProjectRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Project> insert(application::Project project) override;
    common::Result<application::Project> update(application::Project project) override;
    [[nodiscard]] common::Result<std::optional<application::Project>> findById(
        const application::ProjectId& id) const override;
    [[nodiscard]] common::Result<std::optional<application::Project>> findByRootPath(
        const std::filesystem::path& rootPath) const override;
    [[nodiscard]] common::Result<std::vector<application::Project>> listAll() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the per-project readiness port (table project_verification_readiness, schema step 70). */
class SqliteProjectReadinessRepository final : public application::IProjectReadinessRepository {
public:
    explicit SqliteProjectReadinessRepository(std::shared_ptr<SqliteDatabase> database);

    [[nodiscard]] common::Result<std::optional<application::ProjectReadinessRecord>> find(
        const application::ProjectId& projectId) const override;
    common::Result<void> upsert(const application::ProjectReadinessRecord& record) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the single active-project workspace state port. */
class SqliteWorkspaceStateRepository final : public application::IWorkspaceStateRepository {
public:
    explicit SqliteWorkspaceStateRepository(std::shared_ptr<SqliteDatabase> database);

    [[nodiscard]] common::Result<std::optional<application::ProjectId>> activeProjectId() const override;
    common::Result<void> setActiveProjectId(
        const std::optional<application::ProjectId>& projectId) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
