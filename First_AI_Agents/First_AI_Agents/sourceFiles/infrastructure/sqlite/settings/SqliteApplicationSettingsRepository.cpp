#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ApplicationSettingsTypes.hpp"

#include <sqlite3.h>

#include <cstdint>
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
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;

} // namespace

SqliteApplicationSettingsRepository::SqliteApplicationSettingsRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::ApplicationSettings>
SqliteApplicationSettingsRepository::load() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    constexpr const char* sql =
        "SELECT theme_mode,interface_scale_percent,compact_mode,reduce_animations,"
        "run_context_policy,default_execution_profile_id,default_timeout_ms,"
        "default_max_parallel_agents,confirm_many_models,chat_context_policy,"
        "default_chat_provider_id,default_chat_model_id,chat_history_limit,revision,"
        "updated_at_ms FROM application_settings WHERE id=1;";
    const auto prepared = prepare(handle, sql, statement);
    if (!prepared.success())
        return common::Result<application::ApplicationSettings>::failure(*prepared.error());
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW) {
        return common::Result<application::ApplicationSettings>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read application settings"));
    }
    const auto theme = application::themePreferenceFromString(columnText(statement.get(), 0));
    const auto runPolicy = application::projectContextPolicyFromString(columnText(statement.get(), 4));
    const auto chatPolicy = application::projectContextPolicyFromString(columnText(statement.get(), 9));
    if (!theme.has_value() || !runPolicy.has_value() || !chatPolicy.has_value()) {
        return common::Result<application::ApplicationSettings>::failure(error(
            "persistence.invalid_data", "Application settings contain an unknown enum value"));
    }
    application::ApplicationSettings value;
    value.theme = *theme;
    value.interfaceScalePercent = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 1));
    value.compactMode = sqlite3_column_int(statement.get(), 2) != 0;
    value.reduceAnimations = sqlite3_column_int(statement.get(), 3) != 0;
    value.runContextPolicy = *runPolicy;
    value.defaultExecutionProfileId = optionalText(statement.get(), 5);
    value.defaultTimeoutMs = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 6));
    value.defaultMaxParallelAgents = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 7));
    value.confirmManyModels = sqlite3_column_int(statement.get(), 8) != 0;
    value.chatContextPolicy = *chatPolicy;
    value.defaultChatProviderId = optionalText(statement.get(), 10);
    value.defaultChatModelId = optionalText(statement.get(), 11);
    value.chatHistoryLimit = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 12));
    value.revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 13));
    value.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 14));
    return common::Result<application::ApplicationSettings>::ok(std::move(value));
}

common::Result<application::ApplicationSettings>
SqliteApplicationSettingsRepository::save(application::ApplicationSettings value)
{
    if (value.revision == 0) {
        return common::Result<application::ApplicationSettings>::failure(error(
            "settings.revision_conflict", "Application settings revision did not advance"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    constexpr const char* sql =
        "UPDATE application_settings SET theme_mode=?,interface_scale_percent=?,"
        "compact_mode=?,reduce_animations=?,run_context_policy=?,"
        "default_execution_profile_id=?,default_timeout_ms=?,"
        "default_max_parallel_agents=?,confirm_many_models=?,chat_context_policy=?,"
        "default_chat_provider_id=?,default_chat_model_id=?,chat_history_limit=?,"
        "revision=?,updated_at_ms=? WHERE id=1 AND revision=?;";
    const auto prepared = prepare(handle, sql, statement);
    if (!prepared.success())
        return common::Result<application::ApplicationSettings>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, std::string(application::toString(value.theme))),
        bindUnsigned(handle, statement.get(), 2, value.interfaceScalePercent),
        bindUnsigned(handle, statement.get(), 3, value.compactMode ? 1U : 0U),
        bindUnsigned(handle, statement.get(), 4, value.reduceAnimations ? 1U : 0U),
        bindText(handle, statement.get(), 5, std::string(application::toString(value.runContextPolicy))),
        bindNullableText(handle, statement.get(), 6, value.defaultExecutionProfileId),
        bindUnsigned(handle, statement.get(), 7, value.defaultTimeoutMs),
        bindUnsigned(handle, statement.get(), 8, value.defaultMaxParallelAgents),
        bindUnsigned(handle, statement.get(), 9, value.confirmManyModels ? 1U : 0U),
        bindText(handle, statement.get(), 10, std::string(application::toString(value.chatContextPolicy))),
        bindNullableText(handle, statement.get(), 11, value.defaultChatProviderId),
        bindNullableText(handle, statement.get(), 12, value.defaultChatModelId),
        bindUnsigned(handle, statement.get(), 13, value.chatHistoryLimit),
        bindUnsigned(handle, statement.get(), 14, value.revision),
        bindInt64(handle, statement.get(), 15, toMilliseconds(value.updatedAt)),
        bindUnsigned(handle, statement.get(), 16, value.revision - 1)};
    for (const auto& binding : bindings) {
        if (!binding.success())
            return common::Result<application::ApplicationSettings>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<application::ApplicationSettings>::failure(databaseError(
            handle, "persistence.write_failed", "Could not update application settings"));
    }
    if (sqlite3_changes(handle) != 1) {
        return common::Result<application::ApplicationSettings>::failure(error(
            "settings.revision_conflict", "Application settings were changed concurrently"));
    }
    return common::Result<application::ApplicationSettings>::ok(std::move(value));
}

} // namespace ai::infrastructure
