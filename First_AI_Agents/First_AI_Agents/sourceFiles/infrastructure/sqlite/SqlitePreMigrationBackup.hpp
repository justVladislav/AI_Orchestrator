#pragma once

// Persistence-private. The verified copy of an existing database that SqliteDatabase::open() takes BEFORE it upgrades the schema.
//
// Nothing here is reachable from the UI, an agent or a preset: the only caller is SqliteDatabase::open(), and only when the
// composition root that owns the file passed a backup directory (SqliteOpenOptions).
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <filesystem>
#include <string>

struct sqlite3;

namespace ai::infrastructure::sqlite_detail {

inline constexpr const char* kBackupFailedCode = "migration.backup_failed";
inline constexpr const char* kUnversionedDatabaseCode = "migration.unversioned_database";

// What open() must know about an existing file before it changes anything: its user_version and whether it holds user objects
// (anything in sqlite_master that is not an internal sqlite_* object). Reads only.
struct PreMigrationState {
    int userVersion {0};
    bool hasUserObjects {false};
};

[[nodiscard]] common::Result<PreMigrationState> inspectPreMigrationState(sqlite3* handle);

// Takes a consistent snapshot of the open `source` (the SQLite Online Backup API: it includes committed content that is still in
// the WAL, which a copy of the single .db file would lose), checks the finished copy with its own raw read-only connection - never
// through SqliteDatabase::open(), which would migrate it - and only then publishes it under a name that is never replaced.
//
// Returns the published copy. The attempt works inside a directory that it creates itself and that holds the copy and every sidecar
// SQLite makes for it; on any failure (and after publication) exactly that directory is removed. An earlier backup, a file or folder
// that already existed under any name in the backup folder, the source's WAL/SHM and the files of other attempts are never
// touched. Failures return kBackupFailedCode. The source database is not modified.
[[nodiscard]] common::Result<PreMigrationBackupInfo> createPreMigrationBackup(
    sqlite3* source, const std::filesystem::path& databasePath, const std::filesystem::path& directory,
    int fromVersion, int toVersion);

// Closed seam for the persistence tests only (the header is not on any exported include path): it makes a real failure branch of
// the real code run, it never skips or weakens a check.
enum class BackupFaultForTesting {
    None,
    Copy,    // the first backup step reports SQLITE_FULL
    Verify,  // the copy is compared with a wrong expected version
    Publish  // publishing the finished copy fails
};

struct BackupTestSeam {
    BackupFaultForTesting fault {BackupFaultForTesting::None};
    // A fixed time stamp for the file names (empty = the current UTC time): makes name collisions reproducible.
    std::string fixedTimestamp;
    // Puts stand-ins for -journal/-wal/-shm files into the attempt's own working folder before the copy is checked, so a test can see
    // that the attempt removes its own sidecars.
    bool plantOwnSidecars {false};
};

void setBackupTestSeam(BackupTestSeam seam);

} // namespace ai::infrastructure::sqlite_detail
