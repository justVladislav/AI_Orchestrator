#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ComparisonSession;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::bindOptionalTime;
using sqlite_detail::optionalTime;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;

constexpr const char* comparisonSessionColumns =
    "id,project_id,title,prompt,status,request_hash,context_hash,"
    "preferred_execution_id,synthesis_selection_json,synthesis_execution_id,"
    "error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,revision";

std::string serializeExecutionIds(const std::vector<application::ModelExecutionId>& ids)
{
    nlohmann::ordered_json value = nlohmann::ordered_json::array();
    for (const auto& id : ids) value.push_back(id);
    return value.dump();
}

common::Result<std::vector<application::ModelExecutionId>> parseExecutionIds(
    const std::string& encoded)
{
    const auto value = nlohmann::json::parse(encoded, nullptr, false);
    if (value.is_discarded() || !value.is_array()) {
        return common::Result<std::vector<application::ModelExecutionId>>::failure(
            error("persistence.invalid_data", "Comparison synthesis selection is not a JSON array"));
    }
    std::vector<application::ModelExecutionId> ids;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            return common::Result<std::vector<application::ModelExecutionId>>::failure(
                error("persistence.invalid_data", "Comparison synthesis selection contains an invalid id"));
        }
        ids.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<application::ModelExecutionId>>::ok(std::move(ids));
}

common::Result<ComparisonSession> readComparisonSession(sqlite3_stmt* statement)
{
    const auto status = application::comparisonSessionStatusFromString(columnText(statement, 4));
    if (!status.has_value()) {
        return common::Result<ComparisonSession>::failure(
            error("persistence.invalid_data", "Comparison session has an unknown status"));
    }
    const auto selection = parseExecutionIds(columnText(statement, 8));
    if (!selection.success())
        return common::Result<ComparisonSession>::failure(*selection.error());
    const auto revision = sqlite3_column_int64(statement, 15);
    if (revision < 0) {
        return common::Result<ComparisonSession>::failure(
            error("persistence.invalid_data", "Comparison session revision is negative"));
    }

    ComparisonSession value;
    value.id = columnText(statement, 0);
    value.projectId = columnText(statement, 1);
    value.title = columnText(statement, 2);
    value.prompt = columnText(statement, 3);
    value.status = *status;
    value.requestHash = columnText(statement, 5);
    value.contextHash = columnText(statement, 6);
    value.preferredExecutionId = optionalText(statement, 7);
    value.synthesisSelection = selection.value();
    value.synthesisExecutionId = optionalText(statement, 9);
    value.errorCode = columnText(statement, 10);
    value.errorMessage = columnText(statement, 11);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    value.startedAt = optionalTime(statement, 13);
    value.finishedAt = optionalTime(statement, 14);
    value.revision = static_cast<std::uint64_t>(revision);
    return common::Result<ComparisonSession>::ok(std::move(value));
}

} // namespace

SqliteComparisonSessionRepository::SqliteComparisonSessionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ComparisonSession> SqliteComparisonSessionRepository::insertSession(
    ComparisonSession value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO comparison_sessions VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<ComparisonSession>::failure(*prepared.error());
    const std::string selection = serializeExecutionIds(value.synthesisSelection);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, value.id),
        bindText(handle, statement.get(), 2, value.projectId),
        bindText(handle, statement.get(), 3, value.title),
        bindText(handle, statement.get(), 4, value.prompt),
        bindText(handle, statement.get(), 5, std::string(application::toString(value.status))),
        bindText(handle, statement.get(), 6, value.requestHash),
        bindText(handle, statement.get(), 7, value.contextHash),
        bindNullableText(handle, statement.get(), 8, value.preferredExecutionId),
        bindText(handle, statement.get(), 9, selection),
        bindNullableText(handle, statement.get(), 10, value.synthesisExecutionId),
        bindText(handle, statement.get(), 11, value.errorCode),
        bindText(handle, statement.get(), 12, value.errorMessage),
        bindInt64(handle, statement.get(), 13, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement.get(), 14, value.startedAt),
        bindOptionalTime(handle, statement.get(), 15, value.finishedAt),
        bindUnsigned(handle, statement.get(), 16, value.revision)};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<ComparisonSession>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ComparisonSession>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert comparison session"));
    }
    return common::Result<ComparisonSession>::ok(std::move(value));
}

common::Result<ComparisonSession> SqliteComparisonSessionRepository::updateSession(
    ComparisonSession value)
{
    if (value.revision == 0) {
        return common::Result<ComparisonSession>::failure(
            error("comparison.revision_conflict", "Comparison revision did not advance"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE comparison_sessions SET project_id=?,title=?,prompt=?,status=?,"
        "request_hash=?,context_hash=?,preferred_execution_id=?,synthesis_selection_json=?,"
        "synthesis_execution_id=?,error_code=?,error_message=?,created_at_ms=?,started_at_ms=?,"
        "finished_at_ms=?,revision=? WHERE id=? AND revision=?;",
        statement);
    if (!prepared.success()) return common::Result<ComparisonSession>::failure(*prepared.error());
    const std::string selection = serializeExecutionIds(value.synthesisSelection);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, value.projectId),
        bindText(handle, statement.get(), 2, value.title),
        bindText(handle, statement.get(), 3, value.prompt),
        bindText(handle, statement.get(), 4, std::string(application::toString(value.status))),
        bindText(handle, statement.get(), 5, value.requestHash),
        bindText(handle, statement.get(), 6, value.contextHash),
        bindNullableText(handle, statement.get(), 7, value.preferredExecutionId),
        bindText(handle, statement.get(), 8, selection),
        bindNullableText(handle, statement.get(), 9, value.synthesisExecutionId),
        bindText(handle, statement.get(), 10, value.errorCode),
        bindText(handle, statement.get(), 11, value.errorMessage),
        bindInt64(handle, statement.get(), 12, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement.get(), 13, value.startedAt),
        bindOptionalTime(handle, statement.get(), 14, value.finishedAt),
        bindUnsigned(handle, statement.get(), 15, value.revision),
        bindText(handle, statement.get(), 16, value.id),
        bindUnsigned(handle, statement.get(), 17, value.revision - 1)};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<ComparisonSession>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ComparisonSession>::failure(databaseError(
            handle, "persistence.write_failed", "Could not update comparison session"));
    }
    if (sqlite3_changes(handle) != 1) {
        return common::Result<ComparisonSession>::failure(error(
            "comparison.revision_conflict", "Comparison session was changed concurrently or removed"));
    }
    return common::Result<ComparisonSession>::ok(std::move(value));
}

common::Result<std::optional<ComparisonSession>>
SqliteComparisonSessionRepository::findSession(
    const application::ComparisonSessionId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + comparisonSessionColumns
        + " FROM comparison_sessions WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ComparisonSession>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success())
        return common::Result<std::optional<ComparisonSession>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return common::Result<std::optional<ComparisonSession>>::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<ComparisonSession>>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read comparison session"));
    auto value = readComparisonSession(statement.get());
    if (!value.success())
        return common::Result<std::optional<ComparisonSession>>::failure(*value.error());
    return common::Result<std::optional<ComparisonSession>>::ok(value.value());
}

common::Result<std::vector<ComparisonSession>>
SqliteComparisonSessionRepository::listSessions(
    const std::optional<application::ProjectId>& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + comparisonSessionColumns
        + (projectId.has_value()
            ? " FROM comparison_sessions WHERE project_id=? ORDER BY created_at_ms,id;"
            : " FROM comparison_sessions ORDER BY created_at_ms,id;");
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::vector<ComparisonSession>>::failure(*prepared.error());
    if (projectId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *projectId);
        if (!bound.success())
            return common::Result<std::vector<ComparisonSession>>::failure(*bound.error());
    }
    std::vector<ComparisonSession> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ComparisonSession>>::failure(databaseError(
                handle, "persistence.read_failed", "Could not list comparison sessions"));
        auto value = readComparisonSession(statement.get());
        if (!value.success())
            return common::Result<std::vector<ComparisonSession>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(values));
}

common::Result<std::vector<ComparisonSession>>
SqliteComparisonSessionRepository::listUnfinishedSessions() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + comparisonSessionColumns
        + " FROM comparison_sessions WHERE status IN ('pending','running') "
          "ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::vector<ComparisonSession>>::failure(*prepared.error());
    std::vector<ComparisonSession> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ComparisonSession>>::failure(databaseError(
                handle, "persistence.read_failed", "Could not list unfinished comparison sessions"));
        auto value = readComparisonSession(statement.get());
        if (!value.success())
            return common::Result<std::vector<ComparisonSession>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(values));
}

} // namespace ai::infrastructure
