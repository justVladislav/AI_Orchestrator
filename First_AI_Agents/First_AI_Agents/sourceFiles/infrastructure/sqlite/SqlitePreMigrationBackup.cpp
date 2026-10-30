#include "SqlitePreMigrationBackup.hpp"

#include "SqlitePrimitives.hpp"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>

#include <cerrno>
#endif

namespace ai::infrastructure::sqlite_detail {

namespace fs = std::filesystem;

namespace {

constexpr int kBackupPagesPerStep = 256;
constexpr int kMaxBusyRetries = 40;      // 40 x 50 ms: a bounded wait, never an endless one
constexpr int kBusySleepMilliseconds = 50;
constexpr int kMaxNameAttempts = 100;
constexpr std::uintmax_t kFreeSpaceMargin = 1024ULL * 1024ULL;

std::mutex seamMutex;
BackupTestSeam testSeam;

BackupTestSeam currentSeam()
{
    std::lock_guard lock(seamMutex);
    return testSeam;
}

common::Result<PreMigrationBackupInfo> failed(const fs::path& directory, const std::string& reason)
{
    return common::Result<PreMigrationBackupInfo>::failure(error(kBackupFailedCode,
        "The database was not upgraded: a verified copy of it could not be made before the upgrade (" + reason
            + "). Backup folder: " + pathToUtf8(directory)));
}

std::string utcStamp()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts {};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32] {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &parts);
    return buffer;
}

fs::path finalName(const fs::path& directory, const fs::path& databasePath, int from, int to, const std::string& stamp, int attempt)
{
    std::string suffix = ".pre-migration-v" + std::to_string(from) + "-to-v" + std::to_string(to) + "." + stamp;
    if (attempt > 0) suffix += "." + std::to_string(attempt);
    suffix += ".sqlite";
    fs::path name = databasePath.filename();
    name += suffix;
    return directory / name;
}

fs::path sidecar(const fs::path& file, const char* suffix)
{
    fs::path result = file;
    result += suffix;
    return result;
}

enum class PublishOutcome { Published, Exists, Failed };

// Moves `from` to `to` only when `to` does not exist: no replacement, so a file that appeared under the final name is never lost.
PublishOutcome publishWithoutReplacing(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), 0) != 0) return PublishOutcome::Published;
    const DWORD code = GetLastError();
    return code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ? PublishOutcome::Exists : PublishOutcome::Failed;
#else
    if (::link(from.c_str(), to.c_str()) != 0) return errno == EEXIST ? PublishOutcome::Exists : PublishOutcome::Failed;
    ::unlink(from.c_str());
    return PublishOutcome::Published;
#endif
}

// The working area of ONE attempt: a directory this attempt created itself (create_directory reports false when the name was already
// taken, so an existing file or folder is never adopted). The copy, and every sidecar SQLite makes for it (-journal, -wal, -shm), live
// only inside it, so everything in it is the attempt's own and the whole directory goes - on failure, and after a published copy has
// left it. Nothing outside it is ever removed, whatever names the backup folder holds.
struct OwnedAttemptDirectory {
    fs::path directory;

    ~OwnedAttemptDirectory()
    {
        if (directory.empty()) return;
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
};

constexpr const char* kAttemptCopyName = "backup.sqlite";

struct DestinationHandle {
    sqlite3* handle {nullptr};
    ~DestinationHandle()
    {
        if (handle != nullptr) sqlite3_close_v2(handle);
    }
};

struct BackupHandle {
    sqlite3_backup* backup {nullptr};
    ~BackupHandle()
    {
        if (backup != nullptr) sqlite3_backup_finish(backup);
    }
};

bool scalarInt(sqlite3* handle, const char* sql, long long& out)
{
    Statement statement;
    if (!prepare(handle, sql, statement).success()) return false;
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return false;
    out = sqlite3_column_int64(statement.get(), 0);
    return true;
}

bool integrityCheckPassed(sqlite3* handle)
{
    Statement statement;
    if (!prepare(handle, "PRAGMA integrity_check;", statement).success()) return false;
    int rows = 0;
    bool ok = false;
    int step = sqlite3_step(statement.get());
    for (; step == SQLITE_ROW; step = sqlite3_step(statement.get())) {
        ++rows;
        const unsigned char* text = sqlite3_column_text(statement.get(), 0);
        ok = text != nullptr && std::string(reinterpret_cast<const char*>(text)) == "ok";
    }
    return step == SQLITE_DONE && rows == 1 && ok;
}

// The copy is read through its own raw read-only connection. It is deliberately NOT opened through SqliteDatabase::open(): that call
// would migrate an older copy to the current version.
std::string verifyCopy(const fs::path& copy, int expectedVersion, long long expectedObjects)
{
    sqlite3* raw = nullptr;
    const std::string utf8 = pathToUtf8(copy);
    if (sqlite3_open_v2(utf8.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (raw != nullptr) sqlite3_close_v2(raw);
        return "the finished copy could not be opened read-only";
    }
    DestinationHandle guard {raw};
    long long version = 0;
    if (!scalarInt(raw, "PRAGMA user_version;", version)) return "the version of the copy could not be read";
    if (version != expectedVersion) return "the copy reports schema version " + std::to_string(version);
    if (!integrityCheckPassed(raw)) return "the integrity check of the copy did not report ok";
    long long objects = 0;
    if (!scalarInt(raw, "SELECT count(*) FROM sqlite_master;", objects)) return "the schema of the copy could not be read";
    if (objects != expectedObjects) return "the copy holds a different number of schema objects than the database";
    return {};
}

} // namespace

void setBackupTestSeam(BackupTestSeam seam)
{
    std::lock_guard lock(seamMutex);
    testSeam = std::move(seam);
}

common::Result<PreMigrationState> inspectPreMigrationState(sqlite3* handle)
{
    PreMigrationState state;
    long long version = 0;
    if (!scalarInt(handle, "PRAGMA user_version;", version)) {
        return common::Result<PreMigrationState>::failure(
            databaseError(handle, "migration.version_failed", "Could not read schema version"));
    }
    long long objects = 0;
    if (!scalarInt(handle, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite\\_%' ESCAPE '\\';", objects)) {
        return common::Result<PreMigrationState>::failure(
            databaseError(handle, "migration.version_failed", "Could not inspect the existing database"));
    }
    state.userVersion = static_cast<int>(version);
    state.hasUserObjects = objects > 0;
    return common::Result<PreMigrationState>::ok(state);
}

common::Result<PreMigrationBackupInfo> createPreMigrationBackup(
    sqlite3* source, const fs::path& databasePath, const fs::path& directory, int fromVersion, int toVersion)
{
    const BackupTestSeam seam = currentSeam();

    std::error_code fsError;
    fs::create_directories(directory, fsError);
    if (fsError || !fs::is_directory(directory, fsError) || fsError) {
        return failed(directory, "the backup folder could not be created or is not a folder");
    }

    // Preliminary check only: a full disk is still handled where it really happens (SQLITE_FULL on a step).
    std::uintmax_t needed = kFreeSpaceMargin;
    for (const char* suffix : {"", "-wal"}) {
        std::error_code sizeError;
        const auto size = fs::file_size(sidecar(databasePath, suffix), sizeError);
        if (!sizeError) needed += size;
    }
    const auto space = fs::space(directory, fsError);
    if (!fsError && space.available < needed) {
        return failed(directory, "not enough free disk space");
    }

    long long sourceObjects = 0;
    if (!scalarInt(source, "SELECT count(*) FROM sqlite_master;", sourceObjects)) {
        return failed(directory, "the schema of the database could not be read");
    }

    const std::string stamp = seam.fixedTimestamp.empty() ? utcStamp() : seam.fixedTimestamp;

    // Reserve the attempt's own directory: it exists only if THIS call created it, so every file inside is the attempt's.
    OwnedAttemptDirectory attempt;
    int reservedAttempt = -1;
    for (int index = 0; index < kMaxNameAttempts; ++index) {
        const fs::path candidate = sidecar(finalName(directory, databasePath, fromVersion, toVersion, stamp, index), ".attempt");
        std::error_code createError;
        if (fs::create_directory(candidate, createError) && !createError) {
            attempt.directory = candidate;
            reservedAttempt = index;
            break;
        }
    }
    if (reservedAttempt < 0) return failed(directory, "no free working folder was found in the backup folder");
    const fs::path partFile = attempt.directory / kAttemptCopyName;

    {
        DestinationHandle destination;
        const std::string partUtf8 = pathToUtf8(partFile);
        if (sqlite3_open_v2(partUtf8.c_str(), &destination.handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
            != SQLITE_OK) {
            return failed(directory, "the copy could not be opened for writing");
        }
        if (sqlite3_exec(destination.handle, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return failed(directory, "the copy could not be configured");
        }

        {
            BackupHandle backup;
            backup.backup = sqlite3_backup_init(destination.handle, "main", source, "main");
            if (backup.backup == nullptr) return failed(directory, "the backup could not be started");

            int busyRetries = 0;
            bool faultPending = seam.fault == BackupFaultForTesting::Copy;
            for (;;) {
                int rc = sqlite3_backup_step(backup.backup, kBackupPagesPerStep);
                if (faultPending) {
                    faultPending = false;
                    rc = SQLITE_FULL;
                }
                if (rc == SQLITE_DONE) break;
                if (rc == SQLITE_OK) continue;
                if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && ++busyRetries <= kMaxBusyRetries) {
                    sqlite3_sleep(kBusySleepMilliseconds);
                    continue;
                }
                return failed(directory, std::string("copying stopped: ") + sqlite3_errstr(rc));
            }
            const int finished = sqlite3_backup_finish(backup.backup);
            backup.backup = nullptr;
            if (finished != SQLITE_OK) return failed(directory, std::string("copying did not finish: ") + sqlite3_errstr(finished));
        }

        // The copy carries the source's WAL header marker: make it one self-contained file.
        std::string journalMode;
        const auto collect = [](void* context, int, char** values, char**) {
            if (values != nullptr && values[0] != nullptr) static_cast<std::string*>(context)->assign(values[0]);
            return 0;
        };
        if (sqlite3_exec(destination.handle, "PRAGMA journal_mode = DELETE;", collect, &journalMode, nullptr) != SQLITE_OK
            || journalMode != "delete") {
            return failed(directory, "the copy could not be made self-contained");
        }
        const int closed = sqlite3_close(destination.handle);
        if (closed != SQLITE_OK) return failed(directory, "the copy could not be closed");
        destination.handle = nullptr;
    }

    if (seam.plantOwnSidecars) {
        // Test only: stand-ins for sidecar files of the attempt itself, so the cleanup of an attempt's own files is observable.
        for (const char* suffix : {"-journal", "-wal", "-shm"}) {
            std::ofstream planted(sidecar(partFile, suffix), std::ios::binary | std::ios::trunc);
            planted << "planted by the attempt";
        }
    }

    const int expectedVersion = seam.fault == BackupFaultForTesting::Verify ? fromVersion + 1 : fromVersion;
    const std::string problem = verifyCopy(partFile, expectedVersion, sourceObjects);
    if (!problem.empty()) return failed(directory, problem);

    for (int index = reservedAttempt; index < reservedAttempt + kMaxNameAttempts; ++index) {
        const fs::path target = finalName(directory, databasePath, fromVersion, toVersion, stamp, index);
        const PublishOutcome outcome = seam.fault == BackupFaultForTesting::Publish
            ? PublishOutcome::Failed
            : publishWithoutReplacing(partFile, target);
        if (outcome == PublishOutcome::Exists) continue;
        if (outcome == PublishOutcome::Failed) return failed(directory, "the finished copy could not be published");
        PreMigrationBackupInfo info;
        info.path = target;
        info.fromVersion = fromVersion;
        info.toVersion = toVersion;
        std::error_code sizeError;
        info.sizeBytes = fs::file_size(target, sizeError);
        if (sizeError) info.sizeBytes = 0;
        return common::Result<PreMigrationBackupInfo>::ok(std::move(info));
    }
    return failed(directory, "no free file name was found in the backup folder");
}

} // namespace ai::infrastructure::sqlite_detail
