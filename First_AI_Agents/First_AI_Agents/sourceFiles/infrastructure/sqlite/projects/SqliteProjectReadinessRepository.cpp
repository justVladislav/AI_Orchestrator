#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/verification/IProjectReadinessRepository.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ai::infrastructure {
namespace {

using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;

} // namespace

SqliteProjectReadinessRepository::SqliteProjectReadinessRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<std::optional<application::ProjectReadinessRecord>> SqliteProjectReadinessRepository::find(
    const application::ProjectId& projectId) const
{
    using Result = common::Result<std::optional<application::ProjectReadinessRecord>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_root,configure_preset,build_preset,binary_dir,state,inputs_fingerprint,toolchain_identity,targets_json,"
        "targets_complete,checked_at_ms,last_success_at_ms,failure_code,failure_message "
        "FROM project_verification_readiness WHERE project_id = ?;",
        statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read the project readiness"));
    }
    application::ProjectReadinessRecord record;
    record.projectId = projectId;
    record.projectRoot = columnText(statement.get(), 0);
    record.configurePreset = columnText(statement.get(), 1);
    record.buildPreset = columnText(statement.get(), 2);
    record.binaryDir = columnText(statement.get(), 3);
    const auto state = application::projectReadinessStateFromString(columnText(statement.get(), 4));
    if (!state.has_value()) {
        return Result::failure(error("persistence.invalid_data", "The stored project readiness state is unknown"));
    }
    record.state = *state;
    record.inputsFingerprint = columnText(statement.get(), 5);
    record.toolchainIdentity = columnText(statement.get(), 6);
    const auto targets = nlohmann::json::parse(columnText(statement.get(), 7), nullptr, false);
    if (targets.is_array()) {
        for (const auto& target : targets) {
            if (target.is_string()) record.targets.push_back(target.get<std::string>());
        }
    }
    record.targetsComplete = sqlite3_column_int(statement.get(), 8) != 0;
    record.checkedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 9));
    if (sqlite3_column_type(statement.get(), 10) != SQLITE_NULL) {
        record.lastSuccessAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 10));
    }
    record.failureCode = columnText(statement.get(), 11);
    record.failureMessage = columnText(statement.get(), 12);
    return Result::ok(std::move(record));
}

common::Result<void> SqliteProjectReadinessRepository::upsert(const application::ProjectReadinessRecord& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO project_verification_readiness (project_id,project_root,configure_preset,build_preset,binary_dir,state,"
        "inputs_fingerprint,toolchain_identity,targets_json,targets_complete,checked_at_ms,last_success_at_ms,failure_code,"
        "failure_message) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(project_id) DO UPDATE SET project_root = excluded.project_root, configure_preset = excluded.configure_preset, "
        "build_preset = excluded.build_preset, binary_dir = excluded.binary_dir, state = excluded.state, "
        "inputs_fingerprint = excluded.inputs_fingerprint, toolchain_identity = excluded.toolchain_identity, "
        "targets_json = excluded.targets_json, targets_complete = excluded.targets_complete, "
        "checked_at_ms = excluded.checked_at_ms, last_success_at_ms = excluded.last_success_at_ms, "
        "failure_code = excluded.failure_code, failure_message = excluded.failure_message;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& target : record.targets) targets.push_back(target);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.projectId),
        bindText(handle, statement.get(), 2, record.projectRoot),
        bindText(handle, statement.get(), 3, record.configurePreset),
        bindText(handle, statement.get(), 4, record.buildPreset),
        bindText(handle, statement.get(), 5, record.binaryDir),
        bindText(handle, statement.get(), 6, application::toString(record.state)),
        bindText(handle, statement.get(), 7, record.inputsFingerprint),
        bindText(handle, statement.get(), 8, record.toolchainIdentity),
        bindText(handle, statement.get(), 9, targets.dump()),
        bindInt64(handle, statement.get(), 10, record.targetsComplete ? 1 : 0),
        bindInt64(handle, statement.get(), 11, toMilliseconds(record.checkedAt)),
        record.lastSuccessAt.has_value() ? bindInt64(handle, statement.get(), 12, toMilliseconds(*record.lastSuccessAt))
                                         : bindNullableText(handle, statement.get(), 12, std::nullopt),
        bindText(handle, statement.get(), 13, record.failureCode),
        bindText(handle, statement.get(), 14, record.failureMessage)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
            return common::Result<void>::failure(error("project.not_found", "The project does not exist"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not store the project readiness"));
    }
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
