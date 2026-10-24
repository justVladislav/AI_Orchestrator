#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ai::infrastructure {
namespace {

using sqlite_detail::Statement;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::toMilliseconds;
using sqlite_detail::optionalText;
using sqlite_detail::pathToUtf8;
using sqlite_detail::pathFromUtf8;

} // namespace

SqliteVerificationScratchSettingRepository::SqliteVerificationScratchSettingRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<std::optional<std::filesystem::path>> SqliteVerificationScratchSettingRepository::load() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "SELECT root_path FROM verification_scratch_setting WHERE id = 1;", statement);
    if (!prepared.success()) {
        return common::Result<std::optional<std::filesystem::path>>::failure(*prepared.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<std::filesystem::path>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<std::filesystem::path>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read the verification folder setting"));
    }
    const auto text = optionalText(statement.get(), 0);
    if (!text.has_value()) return common::Result<std::optional<std::filesystem::path>>::ok(std::nullopt);
    return common::Result<std::optional<std::filesystem::path>>::ok(pathFromUtf8(*text));
}

common::Result<void> SqliteVerificationScratchSettingRepository::store(
    const std::optional<std::filesystem::path>& root)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_scratch_setting (id,root_path,updated_at_ms) VALUES (1,?,?) "
        "ON CONFLICT(id) DO UPDATE SET root_path = excluded.root_path, updated_at_ms = excluded.updated_at_ms;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    std::optional<std::string> text;
    if (root.has_value()) text = pathToUtf8(*root);
    const common::Result<void> bindings[] = {
        bindNullableText(handle, statement.get(), 1, text),
        bindInt64(handle, statement.get(), 2, toMilliseconds(std::chrono::system_clock::now()))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not store the verification folder setting"));
    }
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
