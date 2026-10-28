#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/catalog/ProviderCatalogTypes.hpp"

#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::Provider;
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
using sqlite_detail::optionalText;

constexpr const char* providerColumns =
    "id, name, kind, base_url, enabled, credential_reference, created_at_ms, updated_at_ms";

common::Result<Provider> readProvider(sqlite3_stmt* statement)
{
    Provider provider;
    provider.id = columnText(statement, 0);
    provider.name = columnText(statement, 1);
    provider.kind = columnText(statement, 2);
    provider.baseUrl = columnText(statement, 3);
    provider.enabled = sqlite3_column_int(statement, 4) != 0;
    provider.credentialReference = optionalText(statement, 5);
    provider.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 6));
    provider.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 7));
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<void> bindProvider(sqlite3* handle, sqlite3_stmt* statement, const Provider& provider)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, provider.id),
        bindText(handle, statement, 2, provider.name),
        bindText(handle, statement, 3, provider.kind),
        bindText(handle, statement, 4, provider.baseUrl),
        bindInt64(handle, statement, 5, provider.enabled ? 1 : 0),
        bindNullableText(handle, statement, 6, provider.credentialReference),
        bindInt64(handle, statement, 7, toMilliseconds(provider.createdAt)),
        bindInt64(handle, statement, 8, toMilliseconds(provider.updatedAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

} // namespace

SqliteProviderRepository::SqliteProviderRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Provider> SqliteProviderRepository::insert(Provider provider)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO providers (id,name,kind,base_url,enabled,credential_reference,created_at_ms,"
        "updated_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Provider>::failure(*prepared.error());
    const auto bound = bindProvider(handle, statement.get(), provider);
    if (!bound.success()) return common::Result<Provider>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Provider>::failure(
                error("provider.duplicate_id", "Provider id already exists"));
        }
        return common::Result<Provider>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert provider"));
    }
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<Provider> SqliteProviderRepository::update(Provider provider)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE providers SET name=?,kind=?,base_url=?,enabled=?,credential_reference=?,"
        "created_at_ms=?,updated_at_ms=? WHERE id=?;",
        statement);
    if (!prepared.success()) return common::Result<Provider>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, provider.name),
        bindText(handle, statement.get(), 2, provider.kind),
        bindText(handle, statement.get(), 3, provider.baseUrl),
        bindInt64(handle, statement.get(), 4, provider.enabled ? 1 : 0),
        bindNullableText(handle, statement.get(), 5, provider.credentialReference),
        bindInt64(handle, statement.get(), 6, toMilliseconds(provider.createdAt)),
        bindInt64(handle, statement.get(), 7, toMilliseconds(provider.updatedAt)),
        bindText(handle, statement.get(), 8, provider.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Provider>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<Provider>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update provider"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<Provider>::failure(error("provider.not_found", "Provider not found"));
    }
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<std::optional<Provider>> SqliteProviderRepository::findById(
    const application::ProviderId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + providerColumns + " FROM providers WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Provider>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Provider>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Provider>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Provider>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read provider"));
    }
    const auto provider = readProvider(statement.get());
    if (!provider.success()) return common::Result<std::optional<Provider>>::failure(*provider.error());
    return common::Result<std::optional<Provider>>::ok(provider.value());
}

common::Result<std::vector<Provider>> SqliteProviderRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + providerColumns
        + " FROM providers ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Provider>>::failure(*prepared.error());

    std::vector<Provider> providers;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Provider>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list providers"));
        }
        auto provider = readProvider(statement.get());
        if (!provider.success()) return common::Result<std::vector<Provider>>::failure(*provider.error());
        providers.push_back(provider.value());
    }
    return common::Result<std::vector<Provider>>::ok(std::move(providers));
}

} // namespace ai::infrastructure
