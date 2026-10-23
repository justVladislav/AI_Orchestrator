#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ApplicationTypes.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::Project;
using application::ProjectStatus;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindInt64;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;
using sqlite_detail::pathToUtf8;
using sqlite_detail::pathFromUtf8;

constexpr const char* projectColumns =
    "id, name, description, root_path, status, created_at_ms, updated_at_ms";

std::optional<ProjectStatus> projectStatusFromString(std::string_view value)
{
    if (value == "active") return ProjectStatus::Active;
    if (value == "archived") return ProjectStatus::Archived;
    if (value == "unavailable") return ProjectStatus::Unavailable;
    return std::nullopt;
}

common::Result<Project> readProject(sqlite3_stmt* statement)
{
    const auto status = projectStatusFromString(columnText(statement, 4));
    if (!status.has_value()) {
        return common::Result<Project>::failure(
            error("persistence.invalid_data", "Project row contains an unknown status"));
    }

    Project project;
    project.id = columnText(statement, 0);
    project.name = columnText(statement, 1);
    project.description = columnText(statement, 2);
    project.rootPath = pathFromUtf8(columnText(statement, 3));
    project.status = *status;
    project.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 5));
    project.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 6));
    return common::Result<Project>::ok(std::move(project));
}

common::Result<void> bindProject(sqlite3* handle, sqlite3_stmt* statement, const Project& project)
{
    const std::string path = pathToUtf8(project.rootPath);
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, project.id),
        bindText(handle, statement, 2, project.name),
        bindText(handle, statement, 3, project.description),
        bindText(handle, statement, 4, path),
        bindText(handle, statement, 5, application::toString(project.status)),
        bindInt64(handle, statement, 6, toMilliseconds(project.createdAt)),
        bindInt64(handle, statement, 7, toMilliseconds(project.updatedAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

} // namespace

SqliteProjectRepository::SqliteProjectRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Project> SqliteProjectRepository::insert(Project project)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO projects (id,name,description,root_path,status,created_at_ms,updated_at_ms) "
        "VALUES (?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Project>::failure(*prepared.error());
    const auto bound = bindProject(handle, statement.get(), project);
    if (!bound.success()) return common::Result<Project>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Project>::failure(
                error("project.duplicate_id", "Project id already exists"));
        }
        if (code == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<Project>::failure(
                error("project.duplicate_path", "Project path is already registered"));
        }
        return common::Result<Project>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert project"));
    }
    return common::Result<Project>::ok(std::move(project));
}

common::Result<Project> SqliteProjectRepository::update(Project project)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE projects SET name=?,description=?,root_path=?,status=?,created_at_ms=?,updated_at_ms=? "
        "WHERE id=?;",
        statement);
    if (!prepared.success()) return common::Result<Project>::failure(*prepared.error());

    const std::string path = pathToUtf8(project.rootPath);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, project.name),
        bindText(handle, statement.get(), 2, project.description),
        bindText(handle, statement.get(), 3, path),
        bindText(handle, statement.get(), 4, application::toString(project.status)),
        bindInt64(handle, statement.get(), 5, toMilliseconds(project.createdAt)),
        bindInt64(handle, statement.get(), 6, toMilliseconds(project.updatedAt)),
        bindText(handle, statement.get(), 7, project.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Project>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<Project>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update project"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<Project>::failure(error("project.not_found", "Project not found"));
    }
    return common::Result<Project>::ok(std::move(project));
}

common::Result<std::optional<Project>> SqliteProjectRepository::findById(
    const application::ProjectId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + projectColumns + " FROM projects WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Project>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Project>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Project>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Project>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read project"));
    }
    const auto project = readProject(statement.get());
    if (!project.success()) return common::Result<std::optional<Project>>::failure(*project.error());
    return common::Result<std::optional<Project>>::ok(project.value());
}

common::Result<std::optional<Project>> SqliteProjectRepository::findByRootPath(
    const std::filesystem::path& rootPath) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + projectColumns + " FROM projects WHERE root_path=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Project>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, pathToUtf8(rootPath));
    if (!bound.success()) return common::Result<std::optional<Project>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Project>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Project>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read project by root path"));
    }
    const auto project = readProject(statement.get());
    if (!project.success()) return common::Result<std::optional<Project>>::failure(*project.error());
    return common::Result<std::optional<Project>>::ok(project.value());
}

common::Result<std::vector<Project>> SqliteProjectRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + projectColumns
        + " FROM projects ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Project>>::failure(*prepared.error());

    std::vector<Project> projects;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Project>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list projects"));
        }
        auto project = readProject(statement.get());
        if (!project.success()) return common::Result<std::vector<Project>>::failure(*project.error());
        projects.push_back(project.value());
    }
    return common::Result<std::vector<Project>>::ok(std::move(projects));
}

} // namespace ai::infrastructure
