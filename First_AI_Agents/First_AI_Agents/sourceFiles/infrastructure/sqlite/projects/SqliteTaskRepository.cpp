#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ApplicationTypes.hpp"

#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ProjectTask;
using application::TaskPriority;
using application::TaskStatus;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindOptionalTime;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::optionalTime;
using sqlite_detail::columnText;

constexpr const char* taskColumns =
    "id, project_id, title, description, status, priority, requires_attention, "
    "due_at_ms, planned_run_at_ms, created_at_ms, updated_at_ms";

std::optional<TaskStatus> taskStatusFromString(std::string_view value)
{
    if (value == "backlog") return TaskStatus::Backlog;
    if (value == "planned") return TaskStatus::Planned;
    if (value == "in_progress") return TaskStatus::InProgress;
    if (value == "blocked") return TaskStatus::Blocked;
    if (value == "done") return TaskStatus::Done;
    if (value == "cancelled") return TaskStatus::Cancelled;
    return std::nullopt;
}

std::optional<TaskPriority> taskPriorityFromString(std::string_view value)
{
    if (value == "low") return TaskPriority::Low;
    if (value == "normal") return TaskPriority::Normal;
    if (value == "high") return TaskPriority::High;
    if (value == "critical") return TaskPriority::Critical;
    return std::nullopt;
}

common::Result<ProjectTask> readTask(sqlite3_stmt* statement)
{
    const auto status = taskStatusFromString(columnText(statement, 4));
    const auto priority = taskPriorityFromString(columnText(statement, 5));
    if (!status.has_value() || !priority.has_value()) {
        return common::Result<ProjectTask>::failure(
            error("persistence.invalid_data", "Task row contains an unknown status or priority"));
    }
    ProjectTask task;
    task.id = columnText(statement, 0);
    task.projectId = columnText(statement, 1);
    task.title = columnText(statement, 2);
    task.description = columnText(statement, 3);
    task.status = *status;
    task.priority = *priority;
    task.requiresAttention = sqlite3_column_int(statement, 6) != 0;
    task.dueAt = optionalTime(statement, 7);
    task.plannedRunAt = optionalTime(statement, 8);
    task.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 9));
    task.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    return common::Result<ProjectTask>::ok(std::move(task));
}

} // namespace

SqliteTaskRepository::SqliteTaskRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ProjectTask> SqliteTaskRepository::insert(ProjectTask task)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO project_tasks "
        "(id,project_id,title,description,status,priority,requires_attention,due_at_ms,"
        "planned_run_at_ms,created_at_ms,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<ProjectTask>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, task.id),
        bindText(handle, statement.get(), 2, task.projectId),
        bindText(handle, statement.get(), 3, task.title),
        bindText(handle, statement.get(), 4, task.description),
        bindText(handle, statement.get(), 5, application::toString(task.status)),
        bindText(handle, statement.get(), 6, application::toString(task.priority)),
        bindInt64(handle, statement.get(), 7, task.requiresAttention ? 1 : 0),
        bindOptionalTime(handle, statement.get(), 8, task.dueAt),
        bindOptionalTime(handle, statement.get(), 9, task.plannedRunAt),
        bindInt64(handle, statement.get(), 10, toMilliseconds(task.createdAt)),
        bindInt64(handle, statement.get(), 11, toMilliseconds(task.updatedAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<ProjectTask>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ProjectTask>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert task"));
    }
    return common::Result<ProjectTask>::ok(std::move(task));
}

common::Result<ProjectTask> SqliteTaskRepository::update(ProjectTask task)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE project_tasks SET title=?,description=?,status=?,priority=?,requires_attention=?,"
        "due_at_ms=?,planned_run_at_ms=?,updated_at_ms=? WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<ProjectTask>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, task.title),
        bindText(handle, statement.get(), 2, task.description),
        bindText(handle, statement.get(), 3, application::toString(task.status)),
        bindText(handle, statement.get(), 4, application::toString(task.priority)),
        bindInt64(handle, statement.get(), 5, task.requiresAttention ? 1 : 0),
        bindOptionalTime(handle, statement.get(), 6, task.dueAt),
        bindOptionalTime(handle, statement.get(), 7, task.plannedRunAt),
        bindInt64(handle, statement.get(), 8, toMilliseconds(task.updatedAt)),
        bindText(handle, statement.get(), 9, task.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<ProjectTask>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ProjectTask>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update task"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<ProjectTask>::failure(error("task.not_found", "Task not found"));
    }
    return common::Result<ProjectTask>::ok(std::move(task));
}

common::Result<std::optional<ProjectTask>> SqliteTaskRepository::findById(
    const application::TaskId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + taskColumns
        + " FROM project_tasks WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<ProjectTask>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ProjectTask>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ProjectTask>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProjectTask>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read task"));
    }
    auto task = readTask(statement.get());
    if (!task.success()) return common::Result<std::optional<ProjectTask>>::failure(*task.error());
    return common::Result<std::optional<ProjectTask>>::ok(task.value());
}

common::Result<std::vector<ProjectTask>> SqliteTaskRepository::list(
    const std::optional<application::ProjectId>& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + taskColumns + " FROM project_tasks"
        + (projectId.has_value() ? " WHERE project_id=?" : "")
        + " ORDER BY updated_at_ms DESC,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ProjectTask>>::failure(*prepared.error());
    if (projectId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *projectId);
        if (!bound.success()) return common::Result<std::vector<ProjectTask>>::failure(*bound.error());
    }
    std::vector<ProjectTask> tasks;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ProjectTask>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list tasks"));
        }
        auto task = readTask(statement.get());
        if (!task.success()) return common::Result<std::vector<ProjectTask>>::failure(*task.error());
        tasks.push_back(task.value());
    }
    return common::Result<std::vector<ProjectTask>>::ok(std::move(tasks));
}

common::Result<void> SqliteTaskRepository::linkRun(
    const application::TaskId& taskId, const application::RunId& runId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO task_run_links(task_id,run_id) VALUES (?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto first = bindText(handle, statement.get(), 1, taskId);
    if (!first.success()) return common::Result<void>::failure(*first.error());
    const auto second = bindText(handle, statement.get(), 2, runId);
    if (!second.success()) return common::Result<void>::failure(*second.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not link task to run"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::RunId>> SqliteTaskRepository::linkedRuns(
    const application::TaskId& taskId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT run_id FROM task_run_links WHERE task_id=? ORDER BY rowid;", statement);
    if (!prepared.success()) return common::Result<std::vector<application::RunId>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, taskId);
    if (!bound.success()) return common::Result<std::vector<application::RunId>>::failure(*bound.error());
    std::vector<application::RunId> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunId>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list task runs"));
        }
        runs.push_back(columnText(statement.get(), 0));
    }
    return common::Result<std::vector<application::RunId>>::ok(std::move(runs));
}

} // namespace ai::infrastructure
