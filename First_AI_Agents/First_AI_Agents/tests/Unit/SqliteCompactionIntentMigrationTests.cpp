// B3 phase I: schema step 75 (the compaction intent table, its indexes and its guarded triggers) - the upgrade from a REAL version-74
// database, the replay over its own objects, the refusal of foreign same-named objects, the rollback, the pre-migration copy and the
// refusal of a newer schema.
//
// Every database is a temporary file. A "version 74" database is a freshly migrated database from which ONLY the objects of step 75
// are removed again (and the version rewound), so everything else is exactly the shipped schema. The checks use throwing require(),
// so they stay active under NDEBUG.
#include "SqliteCompactionIntentMigration.hpp"
#include "SqliteSchema.hpp"

#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace detail = ai::infrastructure::sqlite_detail;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteOpenOptions;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDir {
    fs::path root;
    fs::path database;
    TempDir()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path() / ("aio-b3i-mig-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                            + "-" + std::to_string(counter.fetch_add(1)));
        database = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(database.parent_path());
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string rawExec(const fs::path& file, const std::string& sql, bool readOnly = false)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    char* message = nullptr;
    const int rc = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
    std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
    sqlite3_free(message);
    sqlite3_close_v2(handle);
    return text;
}

std::string rawText(const fs::path& file, const std::string& sql)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed: " + sql);
    std::string value = "<none>";
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(statement, 0);
        value = text != nullptr ? reinterpret_cast<const char*>(text) : "<null>";
    }
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return value;
}

long long rawNumber(const fs::path& file, const std::string& sql)
{
    return std::stoll(rawText(file, sql));
}

std::vector<std::string> namesIn(const fs::path& directory)
{
    std::vector<std::string> names;
    if (!fs::exists(directory)) return names;
    for (const auto& entry : fs::directory_iterator(directory)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

SqliteOpenOptions backupTo(const fs::path& directory)
{
    SqliteOpenOptions options;
    options.preMigrationBackupDirectory = directory;
    return options;
}

// The objects of step 75 as they are listed in sqlite_master.
long long step75ObjectCount(const fs::path& file)
{
    return rawNumber(file,
        "SELECT count(*) FROM sqlite_master WHERE name IN ('project_memory_compaction_intents',"
        "'project_memory_compaction_intents_project_state','project_memory_compaction_intents_state_time',"
        "'project_memory_compaction_intents_no_used_id','project_memory_compaction_records_intent_guard',"
        "'project_memory_compaction_records_intent_close');");
}

const char* kDropStep75Objects =
    "DROP TRIGGER IF EXISTS project_memory_compaction_intents_no_used_id;"
    "DROP TRIGGER IF EXISTS project_memory_compaction_records_intent_guard;"
    "DROP TRIGGER IF EXISTS project_memory_compaction_records_intent_close;"
    "DROP TABLE IF EXISTS project_memory_compaction_intents;";

// A REAL version-74 database with a project, segments and three compaction records written without any intent.
void makeV74Database(const fs::path& file, bool keepStep75Objects = false)
{
    {
        const auto created = SqliteDatabase::open(file);
        require(created.success(), "could not create the fixture database");
    }
    require(rawExec(file,
                "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                "VALUES('p1','Fixture','d','C:/fixture/1','active',1,1);"
                "INSERT INTO project_memory_segments(id,project_id,run_id,summary,source_token_count,summary_token_count,created_at_ms) "
                "VALUES('s1','p1','r','summary one',5,3,10),('s2','p1','r','summary two',6,4,11);"
                "INSERT INTO project_memory_compaction_records(id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,"
                "result_segment_id,input_tokens,output_tokens,cost_micros,status,error_code,error_message,created_at_ms,cost_price_known) VALUES"
                "('legacy-priced','p1','prov','model','[\"s1\",\"s2\"]','h1',NULL,1000,200,250700,'completed','','',1,1),"
                "('legacy-nulls','p1','prov','model','[]','h2',NULL,100,10,0,'completed','','',2,NULL),"
                "('legacy-failed','p1','prov','model','[]','h3',NULL,0,0,0,'failed','provider.failed','x',3,0);")
                .empty(),
        "could not seed the legacy rows");
    if (!keepStep75Objects) require(rawExec(file, kDropStep75Objects).empty(), "could not remove the objects of step 75");
    require(rawExec(file, "PRAGMA user_version = 74;").empty(), "could not rewind the version");
    require(rawNumber(file, "PRAGMA user_version;") == 74, "the fixture is at version 74");
    if (!keepStep75Objects) require(step75ObjectCount(file) == 0, "the fixture has none of the step-75 objects");
}

// Every column of the history, as one text: compared byte for byte before and after the upgrade.
std::string legacyDump(const fs::path& file)
{
    return rawText(file,
               "SELECT group_concat(id||'|'||project_id||'|'||provider_id||'|'||model_id||'|'||source_segment_ids||'|'||source_content_hash"
               "||'|'||ifnull(result_segment_id,'~')||'|'||input_tokens||'|'||output_tokens||'|'||cost_micros||'|'||status||'|'||error_code"
               "||'|'||error_message||'|'||created_at_ms||'|'||ifnull(cost_price_known,'~'),';') "
               "FROM (SELECT * FROM project_memory_compaction_records ORDER BY id);")
        + "#"
        + rawText(file,
            "SELECT group_concat(id||'|'||summary||'|'||source_token_count||'|'||summary_token_count||'|'||created_at_ms,';') "
            "FROM (SELECT * FROM project_memory_segments ORDER BY id);");
}

// ---- the upgrade ------------------------------------------------------------------------------------------------------------------

void testAVersion74DatabaseUpgradesKeepsEveryRowAndTheCopyStaysAt74()
{
    TempDir temp;
    makeV74Database(temp.database);
    const std::string before = legacyDump(temp.database);
    require(before.find("legacy-priced") != std::string::npos && before.find("summary two") != std::string::npos, "the dump sees the data");
    const fs::path backups = temp.root / "backups";
    fs::path copy;
    {
        const auto opened = SqliteDatabase::open(temp.database, backupTo(backups));
        require(opened.success(), "the version-74 database must upgrade");
        const auto& info = opened.value()->preMigrationBackup();
        require(info.has_value() && info->fromVersion == 74 && info->toVersion == 75, "the copy names versions 74 and 75");
        copy = info->path;
    }
    require(rawNumber(copy, "PRAGMA user_version;") == 74, "the copy stays at version 74");
    require(step75ObjectCount(copy) == 0, "...without any object of step 75");
    require(legacyDump(copy) == before, "...with the data byte for byte");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 75, "the working database is at 75");
    require(step75ObjectCount(temp.database) == 6, "...with all six objects");
    require(legacyDump(temp.database) == before, "the financial history and the segments are byte for byte as before");
    require(namesIn(backups).size() == 1, "exactly one copy was taken");

    // Legacy, intent-less inserts are still allowed after the upgrade.
    require(rawExec(temp.database,
                "INSERT INTO project_memory_compaction_records(id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,"
                "status,created_at_ms) VALUES('after','p1','prov','model','[]','h','failed',9);")
                .empty(),
        "a record without an intent is accepted after the upgrade");
    // Reopening the current database neither copies nor changes anything.
    {
        const auto again = SqliteDatabase::open(temp.database, backupTo(backups));
        require(again.success() && !again.value()->preMigrationBackup().has_value(), "a current database is reopened without a copy");
    }
    require(namesIn(backups).size() == 1, "...and no second file appears");
}

void testNewDatabasesAndTheLatestVersionArePinned()
{
    TempDir temp;
    {
        const auto opened = SqliteDatabase::open(temp.database);
        require(opened.success(), "a new database opens");
    }
    require(rawNumber(temp.database, "PRAGMA user_version;") == 75 && detail::supportedSchemaVersion == 75, "the latest schema is 75");
    require(step75ObjectCount(temp.database) == 6, "a new database has all six objects");
}

void testAFutureVersionIsStillRefused()
{
    TempDir temp;
    {
        const auto opened = SqliteDatabase::open(temp.database);
        require(opened.success(), "create");
    }
    require(rawExec(temp.database, "PRAGMA user_version = 76;").empty(), "rewind forward");
    const auto refused = SqliteDatabase::open(temp.database, backupTo(temp.root / "backups"));
    require(!refused.success() && refused.error()->code == "migration.unsupported_version", "a version 76 database is refused");
    require(namesIn(temp.root / "backups").empty(), "...without a copy");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 76 && step75ObjectCount(temp.database) == 6, "...and untouched");
}

// ---- the replay over the step's own objects -------------------------------------------------------------------------------------

void testReplayingTheStepOverItsOwnObjectsKeepsEveryIntent()
{
    TempDir temp;
    makeV74Database(temp.database, true);   // the objects are still there
    require(step75ObjectCount(temp.database) == 6, "the replay fixture has its own objects");
    require(rawExec(temp.database,
                "INSERT INTO project_memory_compaction_intents VALUES('open-1','p1','prov','model','[\"s1\"]','h',10,'open',NULL),"
                "('unresolved-1','p1','prov','model','[]','h',11,'unresolved',500);")
                .empty(),
        "plant intents");
    const std::string before = rawText(temp.database,
        "SELECT group_concat(id||'|'||state||'|'||ifnull(unresolved_marked_at_ms,'~'),';') FROM (SELECT * FROM project_memory_compaction_intents ORDER BY id);");
    {
        const auto opened = SqliteDatabase::open(temp.database, backupTo(temp.root / "backups"));
        require(opened.success(), "a replayed step over its own objects succeeds");
    }
    require(rawNumber(temp.database, "PRAGMA user_version;") == 75, "...and ends at 75");
    require(rawText(temp.database,
                "SELECT group_concat(id||'|'||state||'|'||ifnull(unresolved_marked_at_ms,'~'),';') FROM (SELECT * FROM project_memory_compaction_intents ORDER BY id);")
            == before,
        "...without losing or changing an intent");
    require(step75ObjectCount(temp.database) == 6, "...and without duplicating an object");
}

void testHarmlessCommentsAndSpacingInTheStoredSqlAreAccepted()
{
    TempDir temp;
    makeV74Database(temp.database);
    // The same table, spelled with other whitespace and a comment: it IS this step's table.
    require(rawExec(temp.database,
                "CREATE TABLE project_memory_compaction_intents (id TEXT PRIMARY KEY NOT NULL, -- the operation\n"
                " project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
                " provider_id TEXT NOT NULL, model_id TEXT NOT NULL, source_segment_ids TEXT NOT NULL,\n"
                " source_content_hash TEXT NOT NULL, created_at_ms INTEGER NOT NULL,\n"
                " state TEXT NOT NULL CHECK(state IN ('open','unresolved')), unresolved_marked_at_ms INTEGER,\n"
                " /* consistency */ CHECK((state = 'open' AND unresolved_marked_at_ms IS NULL) OR\n"
                "    (state = 'unresolved' AND unresolved_marked_at_ms IS NOT NULL)));")
                .empty(),
        "plant the respelled table");
    const auto opened = SqliteDatabase::open(temp.database);
    require(opened.success(), "a respelled copy of the step's own table is accepted");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 75 && step75ObjectCount(temp.database) == 6, "...and the step completes");
}

// ---- foreign same-named objects ---------------------------------------------------------------------------------------------------

struct Foreign {
    const char* name;
    const char* sql;   // planted in a version-74 database that has none of the step's objects
};

void requireTheStepRefusedAndRolledBack(const Foreign& foreign)
{
    TempDir temp;
    makeV74Database(temp.database);
    const std::string before = legacyDump(temp.database);
    require(rawExec(temp.database, foreign.sql).empty(), std::string("plant: ") + foreign.name);
    const long long foreignObjects = step75ObjectCount(temp.database);
    require(foreignObjects == 1, std::string("exactly the planted object exists: ") + foreign.name);
    const std::string foreignSql = rawText(temp.database,
        "SELECT group_concat(sql,'~') FROM (SELECT sql FROM sqlite_master WHERE name LIKE 'project_memory_compaction_%intent%' ORDER BY name);");

    const fs::path backups = temp.root / "backups";
    const auto opened = SqliteDatabase::open(temp.database, backupTo(backups));
    require(!opened.success() && opened.error()->code == "migration.v75_failed", std::string("the step refuses with its own code: ") + foreign.name);
    require(opened.error()->message.find("kept at:") != std::string::npos, "...and says where the verified copy is");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 74, std::string("the version stays 74: ") + foreign.name);
    require(step75ObjectCount(temp.database) == foreignObjects, std::string("nothing the step created survived the rollback: ") + foreign.name);
    require(rawText(temp.database,
                "SELECT group_concat(sql,'~') FROM (SELECT sql FROM sqlite_master WHERE name LIKE 'project_memory_compaction_%intent%' ORDER BY name);")
            == foreignSql,
        std::string("the foreign object is exactly as planted: ") + foreign.name);
    require(legacyDump(temp.database) == before, "the history is untouched");
    const auto names = namesIn(backups);
    require(names.size() == 1 && rawNumber(backups / names.front(), "PRAGMA user_version;") == 74, "the verified copy is kept at version 74");
}

void testForeignSameNamedObjectsAreRefusedAndEverythingRollsBack()
{
    const Foreign cases[] = {
        {"another table of the name",
         "CREATE TABLE project_memory_compaction_intents(id TEXT PRIMARY KEY);"},
        {"the right columns but no CHECK (the CHECK text only in a comment)",
         "CREATE TABLE project_memory_compaction_intents (id TEXT PRIMARY KEY NOT NULL,\n"
         " project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE, provider_id TEXT NOT NULL,\n"
         " model_id TEXT NOT NULL, source_segment_ids TEXT NOT NULL, source_content_hash TEXT NOT NULL, created_at_ms INTEGER NOT NULL,\n"
         " state TEXT NOT NULL, unresolved_marked_at_ms INTEGER\n"
         " /* state TEXT NOT NULL CHECK(state IN ('open','unresolved')), CHECK((state = 'open' AND unresolved_marked_at_ms IS NULL) OR\n"
         "    (state = 'unresolved' AND unresolved_marked_at_ms IS NOT NULL)) */);"},
        {"the right columns but the CHECK text only in a string literal",
         "CREATE TABLE project_memory_compaction_intents (id TEXT PRIMARY KEY NOT NULL,\n"
         " project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE, provider_id TEXT NOT NULL,\n"
         " model_id TEXT NOT NULL, source_segment_ids TEXT NOT NULL, source_content_hash TEXT NOT NULL, created_at_ms INTEGER NOT NULL,\n"
         " state TEXT NOT NULL DEFAULT 'CHECK(state IN (open,unresolved))', unresolved_marked_at_ms INTEGER);"},
        {"a view of the table's name",
         "CREATE VIEW project_memory_compaction_intents AS SELECT 1 AS id;"},
        {"an index of the name on another table",
         "CREATE INDEX project_memory_compaction_intents_state_time ON project_memory_compaction_records(id);"},
        {"an index of the name with other columns",
         "CREATE INDEX project_memory_compaction_intents_project_state ON project_memory_compaction_records(project_id,id);"},
        {"a guard trigger that is a no-op with the guard text in a comment",
         "CREATE TRIGGER project_memory_compaction_records_intent_guard BEFORE INSERT ON project_memory_compaction_records\n"
         "BEGIN SELECT 1; -- WHEN EXISTS(SELECT 1 FROM project_memory_compaction_intents WHERE id = NEW.id) RAISE(ABORT, 'AIO_COMPACTION_INTENT_IDENTITY_CONFLICT')\n"
         "END;"},
        {"a guard trigger that raises another text",
         "CREATE TRIGGER project_memory_compaction_records_intent_guard BEFORE INSERT ON project_memory_compaction_records\n"
         "WHEN EXISTS(SELECT 1 FROM project_memory_compaction_intents WHERE id = NEW.id)\n"
         " AND NOT EXISTS(SELECT 1 FROM project_memory_compaction_intents WHERE id = NEW.id AND project_id = NEW.project_id\n"
         "   AND provider_id = NEW.provider_id AND model_id = NEW.model_id AND source_segment_ids = NEW.source_segment_ids\n"
         "   AND source_content_hash = NEW.source_content_hash AND created_at_ms = NEW.created_at_ms)\n"
         "BEGIN SELECT RAISE(ABORT, 'SOMETHING_ELSE'); END;"},
        {"a closing trigger that guards the wrong table",
         "CREATE TRIGGER project_memory_compaction_records_intent_close AFTER INSERT ON project_memory_segments\n"
         "BEGIN DELETE FROM project_memory_compaction_intents WHERE id = NEW.id; END;"},
        {"an occupied-id trigger without its WHEN",
         "CREATE TRIGGER project_memory_compaction_intents_no_used_id BEFORE INSERT ON project_memory_compaction_records\n"
         "BEGIN SELECT RAISE(ABORT, 'AIO_COMPACTION_INTENT_ID_OCCUPIED'); END;"},
    };
    for (const Foreign& foreign : cases) requireTheStepRefusedAndRolledBack(foreign);
}

void testAFailureLateInTheStepRollsBackTheObjectsCreatedBeforeIt()
{
    // The foreign object is the LAST one the step reaches: the table, both indexes and two triggers were already created in the
    // transaction, and must be gone again.
    TempDir temp;
    makeV74Database(temp.database);
    require(rawExec(temp.database,
                "CREATE TRIGGER project_memory_compaction_records_intent_close AFTER INSERT ON project_memory_compaction_records\n"
                "BEGIN SELECT 1; END;")
                .empty(),
        "plant the foreign last trigger");
    const auto opened = SqliteDatabase::open(temp.database);
    require(!opened.success() && opened.error()->code == "migration.v75_failed", "the step refuses");
    require(rawNumber(temp.database, "PRAGMA user_version;") == 74 && step75ObjectCount(temp.database) == 1,
        "the version stays 74 and only the foreign trigger exists: table, indexes and triggers created earlier were rolled back");
    require(rawNumber(temp.database, "SELECT count(*) FROM sqlite_master WHERE name LIKE 'project_memory_compaction_intents%';") == 0,
        "no intent table or index is left");
    // The same database opens once the foreign object is gone.
    require(rawExec(temp.database, "DROP TRIGGER project_memory_compaction_records_intent_close;").empty(), "remove the foreign object");
    const auto retry = SqliteDatabase::open(temp.database);
    require(retry.success() && rawNumber(temp.database, "PRAGMA user_version;") == 75, "...and then the step completes");
}

void testTheNormalizationNeitherHidesNorFakesCode()
{
    using detail::normalizeSchemaSqlForComparison;
    require(normalizeSchemaSqlForComparison("CREATE  TABLE a (\n x INT -- c\n ) ;") == normalizeSchemaSqlForComparison("create table a(x int)"),
        "whitespace, case, a line comment and the trailing semicolon do not matter");
    require(normalizeSchemaSqlForComparison("CREATE TABLE a (x /* c */ INT)") == normalizeSchemaSqlForComparison("CREATE TABLE a (x INT)"),
        "a block comment does not matter");
    require(normalizeSchemaSqlForComparison("CREATE TABLE a (x INT CHECK(x>0))") != normalizeSchemaSqlForComparison("CREATE TABLE a (x INT /* CHECK(x>0) */)"),
        "the same text inside a comment is not code");
    require(normalizeSchemaSqlForComparison("SELECT RAISE(ABORT,'ABC')") != normalizeSchemaSqlForComparison("SELECT RAISE(ABORT,'abc')"),
        "a string literal is compared exactly, with its case");
    require(normalizeSchemaSqlForComparison("SELECT 'a -- not a comment'") == std::optional<std::string>("SELECT'a -- not a comment'"),
        "a comment marker inside a literal is part of the literal");
    require(normalizeSchemaSqlForComparison("CREATE TABLE ab (x INT)") != normalizeSchemaSqlForComparison("CREATE TABLE a b (x INT)"),
        "whitespace between two words is not dropped");
    require(!normalizeSchemaSqlForComparison("CREATE TABLE a (x /* unterminated").has_value() &&
            !normalizeSchemaSqlForComparison("SELECT 'unterminated").has_value() &&
            !normalizeSchemaSqlForComparison("SELECT \"unterminated").has_value(),
        "an unterminated comment or literal never compares equal to anything");
    // The step's own objects are all recognised as themselves and as nothing else.
    const detail::CompactionIntentSchemaObject* objects = detail::compactionIntentSchemaObjects();
    for (int i = 0; i < detail::kCompactionIntentSchemaObjectCount; ++i) {
        for (int j = 0; j < detail::kCompactionIntentSchemaObjectCount; ++j) {
            const bool same = normalizeSchemaSqlForComparison(objects[i].sql) == normalizeSchemaSqlForComparison(objects[j].sql);
            require(same == (i == j), "each object has its own normalized text");
        }
    }
}

} // namespace

int runSqliteCompactionIntentMigrationTests()
{
    testAVersion74DatabaseUpgradesKeepsEveryRowAndTheCopyStaysAt74();
    testNewDatabasesAndTheLatestVersionArePinned();
    testAFutureVersionIsStillRefused();
    testReplayingTheStepOverItsOwnObjectsKeepsEveryIntent();
    testHarmlessCommentsAndSpacingInTheStoredSqlAreAccepted();
    testForeignSameNamedObjectsAreRefusedAndEverythingRollsBack();
    testAFailureLateInTheStepRollsBackTheObjectsCreatedBeforeIt();
    testTheNormalizationNeitherHidesNorFakesCode();
    std::cout << "SqliteCompactionIntentMigrationTests: OK\n";
    return 0;
}
