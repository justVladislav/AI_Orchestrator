#include "ai/infrastructure/SqliteRepositories.hpp"
#include "SqliteConnection.hpp"
#include "SqlitePreMigrationBackup.hpp"
#include "SqlitePrimitives.hpp"
#include "SqliteSchema.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace ai::infrastructure {

namespace {

using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::pathToUtf8;

} // namespace

SqliteDatabase::SqliteDatabase(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

SqliteDatabase::~SqliteDatabase() = default;

common::Result<std::shared_ptr<SqliteDatabase>> SqliteDatabase::open(
    const std::filesystem::path& databasePath, const SqliteOpenOptions& options)
{
    if (databasePath.empty()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
            error("persistence.empty_path", "SQLite database path cannot be empty"));
    }

    std::error_code filesystemError;
    const auto parent = databasePath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "persistence.directory_failed",
                "Could not create SQLite database directory: " + filesystemError.message()));
        }
    }

    auto impl = std::make_unique<Impl>();
    impl->databasePath = databasePath;
    const std::string utf8Path = pathToUtf8(databasePath);
    // SQLITE_OPEN_FULLMUTEX forces serialized threading mode for this
    // connection regardless of how the linked sqlite3 library was compiled,
    // so the Impl-level mutex above and SQLite's own internal locking agree
    // on "one statement in flight at a time" instead of relying on a
    // compile-time default this project does not control.
    if (sqlite3_open_v2(
            utf8Path.c_str(),
            &impl->handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr)
        != SQLITE_OK) {
        const common::Error openError = databaseError(
            impl->handle,
            "persistence.open_failed",
            "Could not open SQLite database");
        if (impl->handle != nullptr) sqlite3_close_v2(impl->handle);
        impl->handle = nullptr;
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(openError);
    }

    sqlite3_extended_result_codes(impl->handle, 1);
    sqlite3_busy_timeout(impl->handle, 5000);

    // Before ANYTHING changes the file - the journal mode, a pragma that writes, the first migration statement - decide whether an
    // existing database is about to be upgraded and, if so, keep a verified copy of it. Only the composition root that owns the file
    // asks for this (SqliteOpenOptions); a new or empty database, a current one and ":memory:" get no copy.
    if (!options.preMigrationBackupDirectory.empty() && utf8Path != ":memory:") {
        const auto state = sqlite_detail::inspectPreMigrationState(impl->handle);
        if (!state.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*state.error());
        }
        const int version = state.value().userVersion;
        if (version == 0 && state.value().hasUserObjects) {
            // user_version 0 does not prove that the file is new: objects without a version are somebody's data.
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                sqlite_detail::kUnversionedDatabaseCode,
                "The database holds data but no schema version, so it is not upgraded"));
        }
        if (version >= 1 && version < sqlite_detail::supportedSchemaVersion) {
            auto backup = sqlite_detail::createPreMigrationBackup(impl->handle, databasePath,
                options.preMigrationBackupDirectory, version, sqlite_detail::supportedSchemaVersion);
            if (!backup.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*backup.error());
            }
            impl->preMigrationBackup = std::move(backup.value());
        }
    }

    const auto foreignKeys = execute(impl->handle, "PRAGMA foreign_keys = ON;", "migration.pragma_failed");
    if (!foreignKeys.success()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*foreignKeys.error());
    }

    // WAL instead of the default rollback journal: the UI thread's own reads
    // (e.g. AgentSessionController::refresh() on a QML poll timer) share this
    // one connection/mutex with the run worker thread's writes. In the
    // default journal mode a writer and a reader exclude each other at the
    // SQLite level too, so a burst of writes from several genuinely-parallel
    // workspaceAccess agents (tool-call audit rows, turns, proposals) could
    // stall a UI-thread read for the whole burst - observed as the GUI
    // freezing while a run is active. WAL lets readers proceed alongside a
    // writer; NORMAL is the documented safe-with-WAL synchronous level (only
    // fsyncs at checkpoints, not on every commit).
    const auto walMode = execute(impl->handle, "PRAGMA journal_mode = WAL;", "migration.pragma_failed");
    if (!walMode.success()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*walMode.error());
    }
    const auto synchronousMode = execute(impl->handle, "PRAGMA synchronous = NORMAL;", "migration.pragma_failed");
    if (!synchronousMode.success()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*synchronousMode.error());
    }

    const auto schema = sqlite_detail::ensureSchema(impl->handle);
    if (!schema.success()) {
        if (impl->preMigrationBackup.has_value()) {
            // The upgrade failed AFTER a verified copy was made: keep the copy, keep the original error and say where the copy is.
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(schema.error()->code,
                schema.error()->message + " A verified copy of the database from before the upgrade was kept at: "
                    + pathToUtf8(impl->preMigrationBackup->path)));
        }
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*schema.error());
    }

    return common::Result<std::shared_ptr<SqliteDatabase>>::ok(
        std::shared_ptr<SqliteDatabase>(new SqliteDatabase(std::move(impl))));
}

const std::filesystem::path& SqliteDatabase::path() const noexcept
{
    return impl_->databasePath;
}

const std::optional<PreMigrationBackupInfo>& SqliteDatabase::preMigrationBackup() const noexcept
{
    return impl_->preMigrationBackup;
}

} // namespace ai::infrastructure
