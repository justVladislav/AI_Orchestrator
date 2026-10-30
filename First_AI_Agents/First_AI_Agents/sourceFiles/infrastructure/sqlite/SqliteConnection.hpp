#pragma once

// Definition shared only by persistence implementation translation units.
// There is still exactly one sqlite3 handle and one mutex per SqliteDatabase.
#include "ai/infrastructure/SqliteRepositories.hpp"

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

    ~Impl()
    {
        // Impl owns the native handle even while open()/migration is failing;
        // this closes every early-return path as well as normal destruction.
        if (handle != nullptr) {
            sqlite3_close_v2(handle);
        }
    }
};

} // namespace ai::infrastructure
