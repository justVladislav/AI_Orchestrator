// P3-0: the verified copy SqliteDatabase::open() takes of an existing database BEFORE it upgrades the schema.
//
// Every database here is a temporary fixture. The schema version stays 70: the "database that needs an upgrade" is a freshly migrated
// database whose step-70 table is dropped and whose user_version is rewound to 69 (the same technique the step-70 tests use). The
// current schema is 71: every upgrade here also runs step 71, and the fixtures drop the step-71 table too so that a "v69" or "v70"
// database really lacks the objects of the later steps.
//
// The checks use throwing require(), so they stay active under NDEBUG.
#include "SqlitePreMigrationBackup.hpp"

#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using ai::infrastructure::PreMigrationBackupInfo;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteOpenOptions;
namespace detail = ai::infrastructure::sqlite_detail;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDir {
    fs::path root;
    fs::path database;

    TempDir()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path()
            / ("ai-orchestrator-premigration-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
                + std::to_string(counter.fetch_add(1)));
        database = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(database.parent_path());
    }

    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

// Resets the closed test seam whatever happens.
struct SeamGuard {
    explicit SeamGuard(detail::BackupTestSeam seam) { detail::setBackupTestSeam(std::move(seam)); }
    ~SeamGuard() { detail::setBackupTestSeam({}); }
};

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Runs SQL through a raw connection; returns "" on success, the SQLite message otherwise.
std::string rawExec(const fs::path& databasePath, const char* sql, bool readOnly = false)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(databasePath).c_str(), &handle,
                readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open the database through native sqlite3");
    char* message = nullptr;
    const int result = sqlite3_exec(handle, sql, nullptr, nullptr, &message);
    std::string text = result == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
    sqlite3_free(message);
    sqlite3_close_v2(handle);
    return text;
}

std::string rawText(const fs::path& databasePath, const char* sql, bool readOnly = false)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(databasePath).c_str(), &handle,
                readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open the database through native sqlite3");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK, "Could not prepare the probe");
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(statement, 0);
        value = text != nullptr ? reinterpret_cast<const char*>(text) : "";
    } else {
        value = "<no row>";
    }
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return value;
}

long long rawNumber(const fs::path& databasePath, const char* sql, bool readOnly = false)
{
    return std::stoll(rawText(databasePath, sql, readOnly));
}

std::string fileBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::vector<std::string> namesIn(const fs::path& directory)
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : fs::directory_iterator(directory, ignored)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// A migrated database with `rows` projects whose step-70 table is dropped and whose version is rewound to 69.
void makeV69Database(const fs::path& databasePath, int rows)
{
    {
        const auto opened = SqliteDatabase::open(databasePath);
        require(opened.success(), "Could not create the fixture database");
    }
    for (int row = 1; row <= rows; ++row) {
        const std::string sql = "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) VALUES('p"
            + std::to_string(row) + "','Fixture " + std::to_string(row) + "','d','C:/fixture/" + std::to_string(row)
            + "','active',1,1);";
        require(rawExec(databasePath, sql.c_str()).empty(), "Could not seed the fixture");
    }
    require(rawExec(databasePath, "DROP TABLE project_output_approvals; DROP TABLE project_verification_readiness; PRAGMA user_version = 69;")
                .empty(),
        "Could not rewind the fixture to v69");
}

// A migrated database whose step-71 table is dropped and whose version is rewound to 70: a real "previous release" database.
void makeV70Database(const fs::path& databasePath, int rows)
{
    {
        const auto opened = SqliteDatabase::open(databasePath);
        require(opened.success(), "Could not create the fixture database");
    }
    for (int row = 1; row <= rows; ++row) {
        const std::string sql = "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) VALUES('p"
            + std::to_string(row) + "','Fixture " + std::to_string(row) + "','d','C:/fixture/" + std::to_string(row)
            + "','active',1,1);";
        require(rawExec(databasePath, sql.c_str()).empty(), "Could not seed the fixture");
    }
    require(rawExec(databasePath, "DROP TABLE project_output_approvals; PRAGMA user_version = 70;").empty(),
        "Could not rewind the fixture to v70");
    require(rawNumber(databasePath,
                "SELECT count(*) FROM sqlite_master WHERE name IN ('project_output_approvals','project_output_approvals_pair_idx',"
                "'project_output_approvals_limit');")
            == 0,
        "The v70 fixture must carry no object of step 71");
}

void requireVerifiedV70Copy(const fs::path& copy, long long rows)
{
    require(rawNumber(copy, "PRAGMA user_version;", true) == 70, "The copy must carry the original version 70");
    require(rawText(copy, "PRAGMA integrity_check;", true) == "ok", "The copy must pass the integrity check");
    require(rawNumber(copy, "SELECT count(*) FROM projects;", true) == rows, "The copy must hold the data");
    require(rawNumber(copy, "SELECT count(*) FROM sqlite_master WHERE name = 'project_output_approvals';", true) == 0,
        "The copy must not carry the table of step 71");
    require(rawNumber(copy, "SELECT count(*) FROM sqlite_master WHERE name = 'project_verification_readiness' AND type = 'table';", true)
            == 1,
        "The copy must carry the table of step 70");
}

SqliteOpenOptions optionsFor(const fs::path& directory)
{
    SqliteOpenOptions options;
    options.preMigrationBackupDirectory = directory;
    return options;
}

void requireStillV69Untouched(const fs::path& databasePath, long long rows)
{
    require(rawNumber(databasePath, "PRAGMA user_version;") == 69, "The database must still be at version 69");
    require(rawNumber(databasePath,
                "SELECT count(*) FROM sqlite_master WHERE name = 'project_verification_readiness' AND type = 'table';")
            == 0,
        "No migration statement may have run");
    require(rawNumber(databasePath, "SELECT count(*) FROM projects;") == rows, "The data must be intact");
}

void requireVerifiedV69Copy(const fs::path& copy, long long rows)
{
    // Raw read-only: the version of the copy is observed BEFORE any ordinary open() could migrate it.
    require(rawNumber(copy, "PRAGMA user_version;", true) == 69, "The copy must carry the original version 69");
    require(rawText(copy, "PRAGMA integrity_check;", true) == "ok", "The copy must pass the integrity check");
    require(rawNumber(copy, "SELECT count(*) FROM projects;", true) == rows, "The copy must hold the data");
    require(rawNumber(copy,
                "SELECT count(*) FROM sqlite_master WHERE name = 'project_verification_readiness' AND type = 'table';", true)
            == 0,
        "The copy must be the state from BEFORE the upgrade");
}

// PB1
void testAnUpgradeTakesAVerifiedRawCopyFirst()
{
    TempDir temp;
    makeV69Database(temp.database, 3);
    const fs::path backups = temp.root / "backups";
    fs::path backupPath;
    {
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "The upgrade must succeed");
        const auto& info = opened.value()->preMigrationBackup();
        require(info.has_value(), "The upgrade must report its copy");
        require(info->fromVersion == 69 && info->toVersion == 71, "The copy must name both versions");
        require(info->path.parent_path() == backups, "The copy lives in the configured folder");
        require(info->sizeBytes > 0 && fs::exists(info->path), "The copy must exist");
        backupPath = info->path;
    }
    requireVerifiedV69Copy(backupPath, 3);
    require(rawNumber(temp.database, "PRAGMA user_version;") == 71, "The working database is upgraded");
    require(rawNumber(temp.database, "SELECT count(*) FROM projects;") == 3, "...and keeps its data");
    require(namesIn(backups).size() == 1, "Exactly one copy: no .part and no second file");
    require(namesIn(backups).front().find(".part") == std::string::npos, "...and it is the published name");
}

// PB2: committed content that is still only in the WAL must be in the copy - what separates the Online Backup API from copying .db.
void testCommittedWalContentIsInTheCopy()
{
    TempDir temp;
    makeV69Database(temp.database, 1);

    sqlite3* side = nullptr;
    require(sqlite3_open_v2(utf8(temp.database).c_str(), &side, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "side connection");
    require(sqlite3_exec(side, "PRAGMA journal_mode = WAL; PRAGMA wal_autocheckpoint = 0;", nullptr, nullptr, nullptr) == SQLITE_OK,
        "WAL without checkpoints");
    require(sqlite3_exec(side,
                "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                "VALUES('wal-only','In the WAL','d','C:/fixture/wal','active',1,1);",
                nullptr, nullptr, nullptr)
            == SQLITE_OK,
        "committed write into the WAL");

    const fs::path walFile = fs::path(temp.database.string() + "-wal");
    require(fs::exists(walFile) && fs::file_size(walFile) > 0, "The committed row must be waiting in a non-empty WAL");

    // Copying only the .db file loses it: this is the naive copy the Online Backup API replaces.
    const fs::path naive = temp.root / "naive" / "orchestrator.sqlite3";
    fs::create_directories(naive.parent_path());
    fs::copy_file(temp.database, naive);
    require(rawNumber(naive, "SELECT count(*) FROM projects WHERE id = 'wal-only';") == 0,
        "The row must NOT be in the main file: a .db-only copy would lose it");

    fs::path backupPath;
    {
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(temp.root / "backups"));
        require(opened.success(), "The upgrade must succeed while another connection is idle");
        backupPath = opened.value()->preMigrationBackup()->path;
    }
    sqlite3_close_v2(side);

    require(rawNumber(backupPath, "SELECT count(*) FROM projects WHERE id = 'wal-only';", true) == 1,
        "The WAL-resident committed row must be in the copy");
    requireVerifiedV69Copy(backupPath, 2);
}

// PB3
void testANewOrEmptyDatabaseGetsNoCopy()
{
    {
        TempDir temp;
        const fs::path backups = temp.root / "backups";
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "A new database is created");
        require(!opened.value()->preMigrationBackup().has_value(), "...without a copy");
        require(!fs::exists(backups), "...and without creating the backup folder");
        require(rawNumber(temp.database, "PRAGMA user_version;") == 71, "...at the current version");
    }
    {
        TempDir temp;
        writeBytes(temp.database, "");
        const fs::path backups = temp.root / "backups";
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success() && !opened.value()->preMigrationBackup().has_value(), "A zero-byte file is a new database");
        require(!fs::exists(backups), "...with no backup folder");
    }
    {
        // An initialised but empty database: version 0, no objects.
        TempDir temp;
        sqlite3* handle = nullptr;
        require(sqlite3_open_v2(utf8(temp.database).c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK,
            "create an empty database");
        sqlite3_exec(handle, "CREATE TABLE tmp(x); DROP TABLE tmp;", nullptr, nullptr, nullptr);
        sqlite3_close_v2(handle);
        const fs::path backups = temp.root / "backups";
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success() && !opened.value()->preMigrationBackup().has_value() && !fs::exists(backups),
            "An empty version-0 database is new");
    }
}

// PB3b: user_version 0 does not prove the file is new.
void testANonEmptyUnversionedDatabaseIsRefusedBeforeAnyChange()
{
    TempDir temp;
    {
        sqlite3* handle = nullptr;
        require(sqlite3_open_v2(utf8(temp.database).c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK,
            "create the foreign database");
        require(sqlite3_exec(handle, "CREATE TABLE notes(text TEXT); INSERT INTO notes VALUES('somebody''s data');", nullptr,
                    nullptr, nullptr)
                == SQLITE_OK,
            "seed the foreign database");
        sqlite3_close_v2(handle);
    }
    const std::string before = fileBytes(temp.database);
    const fs::path backups = temp.root / "backups";
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
    require(!opened.success() && opened.error()->code == "migration.unversioned_database",
        "A database with data and no version is refused with its own code");
    require(fileBytes(temp.database) == before, "The file must be byte-for-byte unchanged");
    require(rawText(temp.database, "PRAGMA journal_mode;") == "delete", "The journal mode must not have been switched");
    require(rawNumber(temp.database, "SELECT count(*) FROM sqlite_master WHERE name = 'projects';") == 0,
        "No migration DDL may have run");
    require(!fs::exists(backups), "...and no backup folder is created");
}

// PB4
void testACurrentDatabaseIsReopenedWithoutCopies()
{
    TempDir temp;
    const fs::path backups = temp.root / "backups";
    for (int round = 0; round < 3; ++round) {
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success() && !opened.value()->preMigrationBackup().has_value(), "A current database needs no copy");
    }
    require(!fs::exists(backups), "...and repeated opens create nothing");
}

// PB5
void testWithoutTheOptionNothingChanges()
{
    {
        TempDir temp;
        makeV69Database(temp.database, 2);
        const auto opened = SqliteDatabase::open(temp.database);
        require(opened.success() && !opened.value()->preMigrationBackup().has_value(), "No option, no copy");
        require(rawNumber(temp.database, "PRAGMA user_version;") == 71, "...and the upgrade runs as it always did");
        require(!fs::exists(temp.root / "backups"), "...with no folder");
    }
    {
        TempDir temp;
        const fs::path backups = temp.root / "backups";
        const auto opened = SqliteDatabase::open(":memory:", optionsFor(backups));
        require(opened.success() && !opened.value()->preMigrationBackup().has_value() && !fs::exists(backups),
            "An in-memory database never gets a copy");
    }
}

// PB6
void testAnUnusableBackupFolderBlocksTheUpgrade()
{
    TempDir temp;
    makeV69Database(temp.database, 2);
    const fs::path blocker = temp.root / "backups";
    writeBytes(blocker, "not a folder");
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(blocker));
    require(!opened.success() && opened.error()->code == "migration.backup_failed", "No folder, no upgrade");
    requireStillV69Untouched(temp.database, 2);
    require(fileBytes(blocker) == "not a folder", "The blocking file is untouched");
}

// PB7
void testCopyVerifyAndPublishFailuresBlockTheUpgradeAndLeaveNothingBehind()
{
    const detail::BackupFaultForTesting faults[] = {detail::BackupFaultForTesting::Copy, detail::BackupFaultForTesting::Verify,
        detail::BackupFaultForTesting::Publish};
    for (const auto fault : faults) {
        TempDir temp;
        makeV69Database(temp.database, 2);
        const fs::path backups = temp.root / "backups";
        writeBytes(backups / "earlier.sqlite", "an earlier backup");
        {
            SeamGuard guard({fault, {}});
            const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
            require(!opened.success() && opened.error()->code == "migration.backup_failed", "The failure blocks the upgrade");
            require(opened.error()->message.find("verified copy") != std::string::npos, "...with a readable reason");
        }
        requireStillV69Untouched(temp.database, 2);
        require(namesIn(backups) == std::vector<std::string> {"earlier.sqlite"},
            "Only the files of the failed attempt are removed: nothing is left, nothing else is touched");
        require(fileBytes(backups / "earlier.sqlite") == "an earlier backup", "An earlier backup is never touched");

        // The same database upgrades once the fault is gone.
        const auto again = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(again.success() && again.value()->preMigrationBackup().has_value(), "A later attempt succeeds");
        require(namesIn(backups).size() == 2, "...and adds exactly one copy");
    }
}

// PB8
void testAnOccupiedFinalOrPartNameIsNeverOverwritten()
{
    const std::string stamp = "20250101T000000Z";
    const std::string finalName = "orchestrator.sqlite3.pre-migration-v69-to-v71." + stamp + ".sqlite";
    {   // the final name is taken
        TempDir temp;
        makeV69Database(temp.database, 2);
        const fs::path backups = temp.root / "backups";
        writeBytes(backups / finalName, "SENTINEL-FINAL");
        SeamGuard guard({detail::BackupFaultForTesting::None, stamp});
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "A taken final name does not block the upgrade");
        require(fileBytes(backups / finalName) == "SENTINEL-FINAL", "The existing file is not replaced");
        const fs::path published = opened.value()->preMigrationBackup()->path;
        require(published != backups / finalName && published.filename().string().find(".1.sqlite") != std::string::npos,
            "The copy takes the next free name");
        requireVerifiedV69Copy(published, 2);
    }
    {   // the .part name is taken
        TempDir temp;
        makeV69Database(temp.database, 2);
        const fs::path backups = temp.root / "backups";
        writeBytes(backups / (finalName + ".part"), "SENTINEL-PART");
        SeamGuard guard({detail::BackupFaultForTesting::None, stamp});
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "A taken .part name does not block the upgrade");
        require(fileBytes(backups / (finalName + ".part")) == "SENTINEL-PART", "The foreign .part is neither opened nor removed");
        requireVerifiedV69Copy(opened.value()->preMigrationBackup()->path, 2);
    }
    {   // the working-folder name is taken by a foreign folder with content, and by a foreign file
        for (const bool folder : {true, false}) {
            TempDir temp;
            makeV69Database(temp.database, 2);
            const fs::path backups = temp.root / "backups";
            const fs::path foreign = backups / (finalName + ".attempt");
            if (folder) writeBytes(foreign / "keep.txt", "SENTINEL-FOLDER");
            else writeBytes(foreign, "SENTINEL-FILE");
            SeamGuard guard({detail::BackupFaultForTesting::None, stamp});
            const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
            require(opened.success(), "A taken working-folder name does not block the upgrade");
            require(fileBytes(folder ? foreign / "keep.txt" : foreign) == (folder ? "SENTINEL-FOLDER" : "SENTINEL-FILE"),
                "The foreign working folder or file is neither used nor removed");
            requireVerifiedV69Copy(opened.value()->preMigrationBackup()->path, 2);
        }
    }
    {   // both are taken
        TempDir temp;
        makeV69Database(temp.database, 2);
        const fs::path backups = temp.root / "backups";
        writeBytes(backups / finalName, "SENTINEL-FINAL");
        writeBytes(backups / (finalName + ".part"), "SENTINEL-PART");
        SeamGuard guard({detail::BackupFaultForTesting::None, stamp});
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "Both names taken: still a free name");
        require(fileBytes(backups / finalName) == "SENTINEL-FINAL" && fileBytes(backups / (finalName + ".part")) == "SENTINEL-PART",
            "Neither foreign file changes");
    }
}

// R1 (review of 32fbd0c): files that already existed next to the names an attempt might use are never the attempt's to delete.
// The sentinels sit under the old "<final>.part" family of names (-journal, -wal, -shm), one at a time, while the attempt fails in
// each of its three failure branches; they must come back byte for byte, and the failed attempt must leave nothing of its own.
void testForeignSidecarsSurviveAFailedAttempt()
{
    const std::string stamp = "20250101T000000Z";
    const std::string finalName = "orchestrator.sqlite3.pre-migration-v69-to-v71." + stamp + ".sqlite";
    const char* suffixes[] = {".part-journal", ".part-wal", ".part-shm"};
    const detail::BackupFaultForTesting faults[] = {detail::BackupFaultForTesting::Copy, detail::BackupFaultForTesting::Verify,
        detail::BackupFaultForTesting::Publish};
    for (const char* suffix : suffixes) {
        for (const auto fault : faults) {
            TempDir temp;
            makeV69Database(temp.database, 2);
            const fs::path backups = temp.root / "backups";
            const std::string sentinelName = finalName + suffix;
            writeBytes(backups / sentinelName, std::string("FOREIGN-") + suffix);
            {
                SeamGuard guard({fault, stamp});
                const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
                require(!opened.success() && opened.error()->code == "migration.backup_failed", "The failure blocks the upgrade");
            }
            require(fs::exists(backups / sentinelName), "A foreign sidecar-named file must survive the failed attempt");
            require(fileBytes(backups / sentinelName) == std::string("FOREIGN-") + suffix, "...byte for byte");
            require(namesIn(backups) == std::vector<std::string> {sentinelName}, "...and the failed attempt left nothing of its own");
            requireStillV69Untouched(temp.database, 2);

            // A successful attempt does not touch it either.
            SeamGuard second({detail::BackupFaultForTesting::None, stamp});
            const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
            require(opened.success(), "A later attempt succeeds");
            require(fileBytes(backups / sentinelName) == std::string("FOREIGN-") + suffix, "...and leaves the foreign file alone");
            requireVerifiedV69Copy(opened.value()->preMigrationBackup()->path, 2);
        }
    }
}

// The cleanup of an attempt's OWN files: its working folder, including sidecars it created, is gone after a failure and after a
// published copy.
void testAnAttemptRemovesItsOwnFiles()
{
    {
        TempDir temp;
        makeV69Database(temp.database, 2);
        const fs::path backups = temp.root / "backups";
        {
            SeamGuard guard({detail::BackupFaultForTesting::Verify, {}, true});
            const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
            require(!opened.success() && opened.error()->code == "migration.backup_failed", "The failure blocks the upgrade");
        }
        require(namesIn(backups).empty(), "The attempt removed its working folder together with the sidecars it had made");
        requireStillV69Untouched(temp.database, 2);
    }
    {
        TempDir temp;
        makeV69Database(temp.database, 2);
        const fs::path backups = temp.root / "backups";
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "upgrade");
        const auto names = namesIn(backups);
        require(names.size() == 1 && names.front() == opened.value()->preMigrationBackup()->path.filename().string(),
            "After a published copy no working folder is left");
    }
}

// PB9
void testANewerDatabaseIsStillRefusedWithoutACopy()
{
    TempDir temp;
    makeV69Database(temp.database, 1);
    require(rawExec(temp.database, "PRAGMA user_version = 999;").empty(), "mark the database as newer");
    const fs::path backups = temp.root / "backups";
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
    require(!opened.success() && opened.error()->code == "migration.unsupported_version", "A newer schema is refused as before");
    require(!fs::exists(backups), "...and gets no copy");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 999, "...and keeps its version");
}

// PB10
void testTheCopyRestoresToASeparatePlace()
{
    TempDir temp;
    makeV69Database(temp.database, 3);
    fs::path backupPath;
    {
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(temp.root / "backups"));
        require(opened.success(), "upgrade");
        backupPath = opened.value()->preMigrationBackup()->path;
    }
    const std::string copyBefore = fileBytes(backupPath);
    requireVerifiedV69Copy(backupPath, 3);

    const fs::path restored = temp.root / "restored" / "orchestrator.sqlite3";
    fs::create_directories(restored.parent_path());
    fs::copy_file(backupPath, restored);
    {
        const auto opened = SqliteDatabase::open(restored);
        require(opened.success(), "The restored copy opens normally");
    }
    require(rawNumber(restored, "PRAGMA user_version;") == 71, "...and an ordinary open upgrades the restored copy");
    require(rawNumber(restored, "SELECT count(*) FROM projects;") == 3, "...with all its data");
    require(rawText(restored, "SELECT name FROM projects WHERE id = 'p2';") == "Fixture 2", "...unchanged");
    require(fileBytes(backupPath) == copyBefore, "The backup itself is not changed by restoring from it");
    require(rawNumber(temp.database, "SELECT count(*) FROM projects;") == 3, "The original fixture is untouched");
}

// PB12: the upgrade fails AFTER the copy was verified.
void testAFailedUpgradeAfterTheCopyKeepsTheCopyAndTheOriginalError()
{
    TempDir temp;
    makeV69Database(temp.database, 3);
    // An index with the name step 70 wants for its table: CREATE TABLE IF NOT EXISTS still fails on it.
    require(rawExec(temp.database,
                "CREATE TABLE blocker(x); CREATE INDEX project_verification_readiness ON blocker(x);")
                .empty(),
        "plant the conflict");
    const fs::path backups = temp.root / "backups";
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
    require(!opened.success(), "The step must fail");
    require(opened.error()->code != "migration.backup_failed", "The migration error is not replaced by a backup error");
    require(opened.error()->code == "migration.v70_failed", "...it is the original error of the failing step");
    require(opened.error()->message.find("kept at:") != std::string::npos, "...and it says where the copy is");
    const auto names = namesIn(backups);
    require(names.size() == 1, "The verified copy is kept");
    requireVerifiedV69Copy(backups / names.front(), 3);
    require(opened.error()->message.find(names.front()) != std::string::npos, "...and the message names that very file");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 69 && rawNumber(temp.database, "SELECT count(*) FROM projects;") == 3,
        "The database itself was rolled back to version 69 with its data");
}

// P3a: the upgrade of a real previous-release (v70) database takes its copy first, and the copy stays at v70 without the new table.
void testAVersion70DatabaseIsCopiedBeforeStep71()
{
    TempDir temp;
    makeV70Database(temp.database, 3);
    const fs::path backups = temp.root / "backups";
    fs::path backupPath;
    {
        const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
        require(opened.success(), "The v70 database must upgrade");
        const auto& info = opened.value()->preMigrationBackup();
        require(info.has_value() && info->fromVersion == 70 && info->toVersion == 71, "The copy names versions 70 and 71");
        backupPath = info->path;
    }
    requireVerifiedV70Copy(backupPath, 3);
    require(rawNumber(temp.database, "PRAGMA user_version;") == 71, "The working database is at 71");
    require(rawNumber(temp.database, "SELECT count(*) FROM sqlite_master WHERE name = 'project_output_approvals' AND type = 'table';") == 1,
        "...with the new table");
    require(rawNumber(temp.database, "SELECT count(*) FROM projects;") == 3, "...and its data");
    require(rawNumber(temp.database, "SELECT count(*) FROM project_output_approvals;") == 0, "...and no approvals");
    // Reopening the current database takes no further copy.
    const auto again = SqliteDatabase::open(temp.database, optionsFor(backups));
    require(again.success() && !again.value()->preMigrationBackup().has_value() && namesIn(backups).size() == 1,
        "A current database is reopened without a copy");
}

void testAFailedStep71KeepsTheCopyAndTheVersion70Database()
{
    TempDir temp;
    makeV70Database(temp.database, 2);
    // Something that is not the table already carries the name the step wants: the step refuses, it does not replace.
    require(rawExec(temp.database, "CREATE TABLE blocker(x); CREATE INDEX project_output_approvals ON blocker(x);").empty(),
        "plant the conflict");
    const fs::path backups = temp.root / "backups";
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
    require(!opened.success() && opened.error()->code == "migration.v71_failed", "Step 71 refuses with its own code");
    require(opened.error()->message.find("kept at:") != std::string::npos, "...and says where the copy is");
    const auto names = namesIn(backups);
    require(names.size() == 1, "The verified copy is kept");
    require(rawNumber(backups / names.front(), "PRAGMA user_version;", true) == 70, "...at version 70");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 70 && rawNumber(temp.database, "SELECT count(*) FROM projects;") == 2,
        "The working database is unchanged at version 70");
    require(rawNumber(temp.database, "SELECT count(*) FROM sqlite_master WHERE name = 'project_output_approvals' AND type = 'index';") == 1,
        "The foreign object was not destroyed");
}

// R1: a table that only has the right column names refuses step 71 after the copy; the copy is kept and nothing is replaced.
void testAnIncompatibleStep71TableKeepsTheCopyAndItsRows()
{
    TempDir temp;
    makeV70Database(temp.database, 2);
    require(rawExec(temp.database,
                "CREATE TABLE project_output_approvals (id TEXT, project_id TEXT, workspace_root TEXT, workspace_key TEXT, "
                "relative_path TEXT, path_key TEXT, inspection_complete INTEGER, nested_sources_acknowledged INTEGER, approved_by TEXT, "
                "approved_at_ms INTEGER); INSERT INTO project_output_approvals VALUES ('x','p1','C:/ws','c:/ws','Out','out',1,0,'',1);")
                .empty(),
        "plant the look-alike table");
    const fs::path backups = temp.root / "backups";
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(backups));
    require(!opened.success() && opened.error()->code == "migration.v71_failed", "The look-alike table refuses step 71");
    require(opened.error()->message.find("kept at:") != std::string::npos, "...and the message says where the copy is");
    const auto names = namesIn(backups);
    require(names.size() == 1 && rawNumber(backups / names.front(), "PRAGMA user_version;", true) == 70, "The verified version-70 copy is kept");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 70 && rawNumber(temp.database, "SELECT count(*) FROM project_output_approvals;") == 1,
        "The database stays at 70 with the foreign rows");
}

// PB13: a measurement, not an assertion on speed.
void testCopyTimeOnAnOrdinaryFixtureIsReported()
{
    TempDir temp;
    makeV69Database(temp.database, 1);
    require(rawExec(temp.database,
                "WITH RECURSIVE c(i) AS (SELECT 2 UNION ALL SELECT i+1 FROM c WHERE i < 20000) "
                "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                "SELECT 'bulk'||i, 'Bulk '||i, 'description text for the fixture', 'C:/bulk/'||i, 'active', 1, 1 FROM c;")
                .empty(),
        "bulk fixture");
    const auto started = std::chrono::steady_clock::now();
    const auto opened = SqliteDatabase::open(temp.database, optionsFor(temp.root / "backups"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    require(opened.success() && opened.value()->preMigrationBackup().has_value(), "bulk upgrade");
    std::cout << "[P3-0] copy + verification + upgrade of a " << opened.value()->preMigrationBackup()->sizeBytes / 1024
              << " KiB temporary database: " << elapsed.count() << " ms\n";
}

} // namespace

int runSqlitePreMigrationBackupTests()
{
    testAnUpgradeTakesAVerifiedRawCopyFirst();
    testCommittedWalContentIsInTheCopy();
    testANewOrEmptyDatabaseGetsNoCopy();
    testANonEmptyUnversionedDatabaseIsRefusedBeforeAnyChange();
    testACurrentDatabaseIsReopenedWithoutCopies();
    testWithoutTheOptionNothingChanges();
    testAnUnusableBackupFolderBlocksTheUpgrade();
    testCopyVerifyAndPublishFailuresBlockTheUpgradeAndLeaveNothingBehind();
    testAnOccupiedFinalOrPartNameIsNeverOverwritten();
    testForeignSidecarsSurviveAFailedAttempt();
    testAnAttemptRemovesItsOwnFiles();
    testANewerDatabaseIsStillRefusedWithoutACopy();
    testTheCopyRestoresToASeparatePlace();
    testAFailedUpgradeAfterTheCopyKeepsTheCopyAndTheOriginalError();
    testAVersion70DatabaseIsCopiedBeforeStep71();
    testAFailedStep71KeepsTheCopyAndTheVersion70Database();
    testAnIncompatibleStep71TableKeepsTheCopyAndItsRows();
    testCopyTimeOnAnOrdinaryFixtureIsReported();
    return 0;
}
