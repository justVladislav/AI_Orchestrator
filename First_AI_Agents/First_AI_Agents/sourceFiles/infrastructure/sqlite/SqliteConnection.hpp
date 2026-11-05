#pragma once

// Definition shared only by persistence implementation translation units.
// There is still exactly one sqlite3 handle and one mutex per SqliteDatabase.
#include "ai/infrastructure/SqliteRepositories.hpp"
#include "SqliteConnectionHealth.hpp"
#include "SqliteFaultForTesting.hpp"

#include <sqlite3.h>
#include <mutex>

namespace ai::infrastructure {

// Owns the single native sqlite3 connection and the mutex that serializes
// every statement this process issues against it. Repository instances
// share this one connection (via shared_ptr) rather than opening a
// connection per call: SQLITE_OPEN_FULLMUTEX plus this mutex already make
// each repository method atomic with respect to every other thread, so a
// persistent connection is both simpler and avoids repeatedly paying
// sqlite3_open_v2/close_v2 cost on every single read.
struct SqliteDatabase::Impl {
    sqlite3* handle {nullptr};
    std::filesystem::path databasePath;
    // The verified copy open() took before upgrading an existing database, if it did.
    std::optional<PreMigrationBackupInfo> preMigrationBackup;
    mutable std::mutex mutex;
    // S2b: whether this connection may still be used by every repository, and who owns the transaction open on it. Lives exactly as
    // long as the connection (the authorizer callback and the client-data association point at it) - declared BEFORE the destructor
    // closes the handle and destroyed AFTER it.
    sqlite_detail::ConnectionHealth health;
    // S2b: the closed test seam; nothing is armed by default and production code only reads it under `mutex`.
    sqlite_detail::FaultPlan fault;

    ~Impl()
    {
        // Impl owns the native handle even while open()/migration is failing;
        // this closes every early-return path as well as normal destruction.
        if (handle != nullptr) {
            // No callback may outlive the object it points at; a transaction still open at close is rolled back by SQLite itself.
            health.detach(handle);
            sqlite3_close_v2(handle);
        }
    }
};

} // namespace ai::infrastructure
