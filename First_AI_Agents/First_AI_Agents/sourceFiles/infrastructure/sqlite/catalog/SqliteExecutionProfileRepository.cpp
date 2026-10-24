#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "SqliteProfileMapping.hpp"

#include "ai/application/ExecutionProfileTypes.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ExecutionProfile;
using application::StageModelAssignment;
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
using sqlite_detail::providerBudgetOverridesJson;
using sqlite_detail::parseProviderBudgetOverrides;
using sqlite_detail::readAssignment;

constexpr const char* executionProfileColumns =
    "id, name, workflow_id, enabled, is_default, created_at_ms, updated_at_ms, provider_budget_json";
constexpr const char* executionProfileAssignmentColumns =
    "profile_id, stage_type, provider_id, model_id, timeout_ms";

common::Result<ExecutionProfile> readExecutionProfile(sqlite3_stmt* statement)
{
    ExecutionProfile profile;
    profile.id = columnText(statement, 0);
    profile.name = columnText(statement, 1);
    profile.workflowId = columnText(statement, 2);
    profile.enabled = sqlite3_column_int(statement, 3) != 0;
    profile.isDefault = sqlite3_column_int(statement, 4) != 0;
    profile.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 5));
    profile.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 6));
    const auto providerBudget = parseProviderBudgetOverrides(columnText(statement, 7));
    if (!providerBudget.success())
        return common::Result<ExecutionProfile>::failure(*providerBudget.error());
    profile.providerBudget = providerBudget.value();
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<void> bindExecutionProfile(
    sqlite3* handle, sqlite3_stmt* statement, const ExecutionProfile& profile)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, profile.id),
        bindText(handle, statement, 2, profile.name),
        bindText(handle, statement, 3, profile.workflowId),
        bindInt64(handle, statement, 4, profile.enabled ? 1 : 0),
        bindInt64(handle, statement, 5, profile.isDefault ? 1 : 0),
        bindInt64(handle, statement, 6, toMilliseconds(profile.createdAt)),
        bindInt64(handle, statement, 7, toMilliseconds(profile.updatedAt)),
        bindText(handle, statement, 8, providerBudgetOverridesJson(profile.providerBudget))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

// Shared by findById and listAll so both read exactly the same
// deterministically-ordered assignment set for a profile.
common::Result<std::vector<StageModelAssignment>> fetchAssignmentsForProfile(
    sqlite3* handle,
    const std::string& profileId)
{
    Statement statement;
    const std::string sql = std::string("SELECT ") + executionProfileAssignmentColumns
        + " FROM execution_profile_assignments WHERE profile_id=? ORDER BY stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, profileId);
    if (!bound.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*bound.error());
    }

    std::vector<StageModelAssignment> assignments;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<StageModelAssignment>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read execution profile assignments"));
        }
        auto assignment = readAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::vector<StageModelAssignment>>::failure(*assignment.error());
        }
        assignments.push_back(assignment.value());
    }
    return common::Result<std::vector<StageModelAssignment>>::ok(std::move(assignments));
}

} // namespace

SqliteExecutionProfileRepository::SqliteExecutionProfileRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ExecutionProfile> SqliteExecutionProfileRepository::insert(ExecutionProfile profile)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<ExecutionProfile>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<ExecutionProfile>::failure(failure);
    };

    {
        Statement statement;
        const auto prepared = prepare(handle,
            "INSERT INTO execution_profiles (id,name,workflow_id,enabled,is_default,created_at_ms,"
            "updated_at_ms,provider_budget_json) VALUES (?,?,?,?,?,?,?,?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindExecutionProfile(handle, statement.get(), profile);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(
                    error("execution_profile.duplicate_id", "Execution profile id already exists"));
            }
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not insert execution profile"));
        }
    }

    for (const auto& assignment : profile.assignments) {
        Statement statement;
        const auto prepared = prepare(handle,
            "INSERT INTO execution_profile_assignments (profile_id,stage_type,provider_id,model_id,"
            "timeout_ms) VALUES (?,?,?,?,?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, profile.id),
            bindText(handle, statement.get(), 2, assignment.stageType),
            bindText(handle, statement.get(), 3, assignment.providerId),
            bindText(handle, statement.get(), 4, assignment.modelId),
            bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count()))};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(error(
                    "execution_profile.duplicate_stage_type",
                    "A stage type cannot appear more than once in one profile"));
            }
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not insert execution profile assignment"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<ExecutionProfile> SqliteExecutionProfileRepository::update(ExecutionProfile profile)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<ExecutionProfile>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<ExecutionProfile>::failure(failure);
    };

    {
        // is_default is intentionally included as a straight passthrough
        // here (unlike Model), but callers always populate it from the
        // existing stored row before calling update() - only
        // setDefaultProfile() actually flips it.
        Statement statement;
        const auto prepared = prepare(handle,
            "UPDATE execution_profiles SET name=?,workflow_id=?,enabled=?,is_default=?,"
            "created_at_ms=?,updated_at_ms=?,provider_budget_json=? WHERE id=?;",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, profile.name),
            bindText(handle, statement.get(), 2, profile.workflowId),
            bindInt64(handle, statement.get(), 3, profile.enabled ? 1 : 0),
            bindInt64(handle, statement.get(), 4, profile.isDefault ? 1 : 0),
            bindInt64(handle, statement.get(), 5, toMilliseconds(profile.createdAt)),
            bindInt64(handle, statement.get(), 6, toMilliseconds(profile.updatedAt)),
            bindText(handle, statement.get(), 7, providerBudgetOverridesJson(profile.providerBudget)),
            bindText(handle, statement.get(), 8, profile.id)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not update execution profile"));
        }
        if (sqlite3_changes(handle) == 0) {
            return rollbackWith(
                error("execution_profile.not_found", "Execution profile not found"));
        }
    }

    {
        Statement remove;
        const auto removePrepared = prepare(
            handle, "DELETE FROM execution_profile_assignments WHERE profile_id=?;", remove);
        if (!removePrepared.success()) return rollbackWith(*removePrepared.error());
        const auto removeBound = bindText(handle, remove.get(), 1, profile.id);
        if (!removeBound.success()) return rollbackWith(*removeBound.error());
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not replace execution profile assignments"));
        }
    }

    for (const auto& assignment : profile.assignments) {
        Statement statement;
        const auto prepared = prepare(handle,
            "INSERT INTO execution_profile_assignments (profile_id,stage_type,provider_id,model_id,"
            "timeout_ms) VALUES (?,?,?,?,?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, profile.id),
            bindText(handle, statement.get(), 2, assignment.stageType),
            bindText(handle, statement.get(), 3, assignment.providerId),
            bindText(handle, statement.get(), 4, assignment.modelId),
            bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count()))};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(error(
                    "execution_profile.duplicate_stage_type",
                    "A stage type cannot appear more than once in one profile"));
            }
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not insert execution profile assignment"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<std::optional<ExecutionProfile>> SqliteExecutionProfileRepository::findById(
    const application::ExecutionProfileId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + executionProfileColumns
        + " FROM execution_profiles WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::optional<ExecutionProfile>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ExecutionProfile>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ExecutionProfile>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ExecutionProfile>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read execution profile"));
    }
    auto profile = readExecutionProfile(statement.get());
    if (!profile.success()) {
        return common::Result<std::optional<ExecutionProfile>>::failure(*profile.error());
    }

    const auto assignments = fetchAssignmentsForProfile(handle, id);
    if (!assignments.success()) {
        return common::Result<std::optional<ExecutionProfile>>::failure(*assignments.error());
    }
    profile.value().assignments = assignments.value();
    return common::Result<std::optional<ExecutionProfile>>::ok(profile.value());
}

common::Result<std::vector<ExecutionProfile>> SqliteExecutionProfileRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + executionProfileColumns
        + " FROM execution_profiles ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ExecutionProfile>>::failure(*prepared.error());

    std::vector<ExecutionProfile> profiles;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ExecutionProfile>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list execution profiles"));
        }
        auto profile = readExecutionProfile(statement.get());
        if (!profile.success()) {
            return common::Result<std::vector<ExecutionProfile>>::failure(*profile.error());
        }
        profiles.push_back(profile.value());
    }

    for (auto& profile : profiles) {
        const auto assignments = fetchAssignmentsForProfile(handle, profile.id);
        if (!assignments.success()) {
            return common::Result<std::vector<ExecutionProfile>>::failure(*assignments.error());
        }
        profile.assignments = assignments.value();
    }
    return common::Result<std::vector<ExecutionProfile>>::ok(std::move(profiles));
}

common::Result<void> SqliteExecutionProfileRepository::setDefaultProfile(
    const application::ExecutionProfileId& profileId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    {
        const auto unset = execute(
            handle, "UPDATE execution_profiles SET is_default=0 WHERE is_default=1;",
            "persistence.write_failed");
        if (!unset.success()) return rollbackWith(*unset.error());
    }

    {
        Statement statement;
        const auto prepared = prepare(
            handle, "UPDATE execution_profiles SET is_default=1 WHERE id=?;", statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, statement.get(), 1, profileId);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not set default execution profile"));
        }
        if (sqlite3_changes(handle) == 0) {
            return rollbackWith(
                error("execution_profile.not_found", "Execution profile not found"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
