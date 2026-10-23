#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace ai::infrastructure {
namespace {

using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindNullableText;
using sqlite_detail::optionalText;

} // namespace

SqliteWorkspaceStateRepository::SqliteWorkspaceStateRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<std::optional<application::ProjectId>> SqliteWorkspaceStateRepository::activeProjectId() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(
        handle, "SELECT active_project_id FROM workspace_state WHERE id = 1;", statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ProjectId>>::failure(*prepared.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        // The singleton row is always inserted by the v3 migration itself;
        // a database that could reach this code without that row would
        // already have failed open() at an older/newer schema version.
        return common::Result<std::optional<application::ProjectId>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ProjectId>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read workspace state"));
    }
    return common::Result<std::optional<application::ProjectId>>::ok(optionalText(statement.get(), 0));
}

common::Result<void> SqliteWorkspaceStateRepository::setActiveProjectId(
    const std::optional<application::ProjectId>& projectId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(
        handle, "UPDATE workspace_state SET active_project_id = ? WHERE id = 1;", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto bound = bindNullableText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return common::Result<void>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_FOREIGNKEY) {
            // Should not normally happen: WorkspaceContextService validates
            // the project exists/is Active before calling this. Still fails
            // closed with a typed code rather than a generic write error.
            return common::Result<void>::failure(
                error("workspace.project_not_found", "Project not found"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update workspace state"));
    }
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
