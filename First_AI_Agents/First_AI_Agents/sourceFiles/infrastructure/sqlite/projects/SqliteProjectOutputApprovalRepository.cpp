#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/workspace/ProjectOutputApproval.hpp"

#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ProjectOutputApproval;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindInt64;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;

constexpr const char* kSelectColumns =
    "id,project_id,workspace_root,workspace_key,relative_path,path_key,inspection_complete,nested_sources_acknowledged,"
    "approved_by,approved_at_ms";

// A row that does not hold what the table's CHECKs guarantee is corrupt: an error, never a record that is silently skipped.
common::Result<std::vector<ProjectOutputApproval>> readRows(sqlite3* handle, Statement& statement)
{
    using Result = common::Result<std::vector<ProjectOutputApproval>>;
    std::vector<ProjectOutputApproval> rows;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, application::kOutputApprovalStorageFailedCode,
                "Could not read the output approvals"));
        }
        ProjectOutputApproval row;
        row.id = columnText(statement.get(), 0);
        row.projectId = columnText(statement.get(), 1);
        row.workspaceRoot = columnText(statement.get(), 2);
        row.workspaceKey = columnText(statement.get(), 3);
        row.relativePath = columnText(statement.get(), 4);
        row.pathKey = columnText(statement.get(), 5);
        const auto complete = sqlite3_column_int64(statement.get(), 6);
        const auto nested = sqlite3_column_int64(statement.get(), 7);
        row.approvedBy = columnText(statement.get(), 8);
        row.approvedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 9));
        if (row.id.empty() || row.projectId.empty() || row.workspaceRoot.empty() || row.workspaceKey.empty()
            || row.relativePath.empty() || row.pathKey.empty() || (complete != 0 && complete != 1)
            || (nested != 0 && nested != 1)) {
            return Result::failure(error(application::kOutputApprovalStorageFailedCode, "A stored output approval is corrupt"));
        }
        row.inspectionComplete = complete == 1;
        row.nestedSourcesAcknowledged = nested == 1;
        rows.push_back(std::move(row));
    }
    return Result::ok(std::move(rows));
}

// Ends a transaction that did not commit.
struct RollbackUnlessCommitted {
    sqlite3* handle;
    bool committed {false};
    ~RollbackUnlessCommitted()
    {
        if (!committed) static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
    }
};

} // namespace

SqliteProjectOutputApprovalRepository::SqliteProjectOutputApprovalRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::ProjectOutputApprovalCommitResult> SqliteProjectOutputApprovalRepository::commit(
    const ProjectOutputApproval& candidate, const application::ProjectOutputApprovalSetValidator& validator)
{
    using Result = common::Result<application::ProjectOutputApprovalCommitResult>;
    // The handle's mutex is held for the whole command: nothing else of this process touches the connection between the read of
    // the set and the insert. BEGIN IMMEDIATE additionally takes SQLite's write lock before the read, so a second CONNECTION to the
    // same file cannot slip in between either.
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const auto begun = execute(handle, "BEGIN IMMEDIATE;", application::kOutputApprovalStorageFailedCode);
    if (!begun.success()) return Result::failure(*begun.error());
    RollbackUnlessCommitted transaction {handle};

    std::vector<ProjectOutputApproval> current;
    {
        Statement select;
        const auto prepared = prepare(handle,
            (std::string("SELECT ") + kSelectColumns
                + " FROM project_output_approvals WHERE project_id = ? AND workspace_key = ? ORDER BY path_key;").c_str(),
            select);
        if (!prepared.success()) return Result::failure(*prepared.error());
        const auto a = bindText(handle, select.get(), 1, candidate.projectId);
        if (!a.success()) return Result::failure(*a.error());
        const auto b = bindText(handle, select.get(), 2, candidate.workspaceKey);
        if (!b.success()) return Result::failure(*b.error());
        auto rows = readRows(handle, select);
        if (!rows.success()) return Result::failure(*rows.error());
        current = std::move(rows.value());
    }

    for (const auto& record : current) {
        if (record.pathKey == candidate.pathKey) {
            return Result::ok({application::ProjectOutputApprovalCommitOutcome::AlreadyApproved, record});
        }
    }
    if (current.size() >= application::kProjectOutputApprovalLimit) {
        return Result::failure(error(application::kOutputApprovalLimitReachedCode,
            "This folder already has the most approvals it can hold"));
    }
    if (validator) {
        const auto valid = validator(current);
        if (!valid.success()) return Result::failure(*valid.error());
    }

    {
        Statement insert;
        const auto prepared = prepare(handle,
            "INSERT INTO project_output_approvals (id,project_id,workspace_root,workspace_key,relative_path,path_key,"
            "inspection_complete,nested_sources_acknowledged,approved_by,approved_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?);",
            insert);
        if (!prepared.success()) return Result::failure(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, insert.get(), 1, candidate.id),
            bindText(handle, insert.get(), 2, candidate.projectId),
            bindText(handle, insert.get(), 3, candidate.workspaceRoot),
            bindText(handle, insert.get(), 4, candidate.workspaceKey),
            bindText(handle, insert.get(), 5, candidate.relativePath),
            bindText(handle, insert.get(), 6, candidate.pathKey),
            bindInt64(handle, insert.get(), 7, candidate.inspectionComplete ? 1 : 0),
            bindInt64(handle, insert.get(), 8, candidate.nestedSourcesAcknowledged ? 1 : 0),
            bindText(handle, insert.get(), 9, candidate.approvedBy),
            bindInt64(handle, insert.get(), 10, toMilliseconds(candidate.approvedAt))};
        for (const auto& binding : bindings) {
            if (!binding.success()) return Result::failure(*binding.error());
        }
        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            // A constraint is a typed answer, never a generic failure and never a success: the transaction already holds the write
            // lock, so these only fire for a row the checks above could not see (a corrupt or foreign writer).
            switch (sqlite3_extended_errcode(handle)) {
            case SQLITE_CONSTRAINT_TRIGGER:
                return Result::failure(error(application::kOutputApprovalLimitReachedCode,
                    "This folder already has the most approvals it can hold"));
            case SQLITE_CONSTRAINT_UNIQUE:
            case SQLITE_CONSTRAINT_PRIMARYKEY:
                return Result::failure(error(application::kOutputApprovalConflictCode,
                    "Another approval for this folder was stored at the same time"));
            case SQLITE_CONSTRAINT_FOREIGNKEY:
                return Result::failure(error("project.not_found", "The project does not exist"));
            default:
                return Result::failure(databaseError(handle, application::kOutputApprovalStorageFailedCode,
                    "Could not store the output approval"));
            }
        }
    }

    const auto committed = execute(handle, "COMMIT;", application::kOutputApprovalStorageFailedCode);
    if (!committed.success()) return Result::failure(*committed.error());
    transaction.committed = true;
    return Result::ok({application::ProjectOutputApprovalCommitOutcome::Inserted, candidate});
}

common::Result<std::vector<ProjectOutputApproval>> SqliteProjectOutputApprovalRepository::listForWorkspace(
    const application::ProjectId& projectId, const std::string& workspaceKey) const
{
    using Result = common::Result<std::vector<ProjectOutputApproval>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement select;
    const auto prepared = prepare(handle,
        (std::string("SELECT ") + kSelectColumns
            + " FROM project_output_approvals WHERE project_id = ? AND workspace_key = ? ORDER BY path_key;").c_str(),
        select);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto a = bindText(handle, select.get(), 1, projectId);
    if (!a.success()) return Result::failure(*a.error());
    const auto b = bindText(handle, select.get(), 2, workspaceKey);
    if (!b.success()) return Result::failure(*b.error());
    return readRows(handle, select);
}

common::Result<std::vector<ProjectOutputApproval>> SqliteProjectOutputApprovalRepository::listForProject(
    const application::ProjectId& projectId) const
{
    using Result = common::Result<std::vector<ProjectOutputApproval>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement select;
    const auto prepared = prepare(handle,
        (std::string("SELECT ") + kSelectColumns
            + " FROM project_output_approvals WHERE project_id = ? ORDER BY workspace_key, path_key;").c_str(),
        select);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto a = bindText(handle, select.get(), 1, projectId);
    if (!a.success()) return Result::failure(*a.error());
    return readRows(handle, select);
}

common::Result<bool> SqliteProjectOutputApprovalRepository::revoke(
    const application::ProjectId& projectId, const std::string& approvalId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "DELETE FROM project_output_approvals WHERE project_id = ? AND id = ?;", statement);
    if (!prepared.success()) return common::Result<bool>::failure(*prepared.error());
    const auto a = bindText(handle, statement.get(), 1, projectId);
    if (!a.success()) return common::Result<bool>::failure(*a.error());
    const auto b = bindText(handle, statement.get(), 2, approvalId);
    if (!b.success()) return common::Result<bool>::failure(*b.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<bool>::failure(
            databaseError(handle, application::kOutputApprovalStorageFailedCode, "Could not revoke the output approval"));
    }
    return common::Result<bool>::ok(sqlite3_changes(handle) > 0);
}

} // namespace ai::infrastructure
