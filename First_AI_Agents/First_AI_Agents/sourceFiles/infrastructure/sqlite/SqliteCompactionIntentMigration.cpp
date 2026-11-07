#include "SqliteCompactionIntentMigration.hpp"
#include "SqlitePrimitives.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <utility>

namespace ai::infrastructure::sqlite_detail {

namespace {

using sqlite_detail::Statement;

// The objects, in creation order. The triggers on the record table come last: they reference the intent table by name only.
constexpr CompactionIntentSchemaObject kObjects[kCompactionIntentSchemaObjectCount] = {
    {"table", "project_memory_compaction_intents", "project_memory_compaction_intents",
     R"sql(CREATE TABLE project_memory_compaction_intents (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    provider_id TEXT NOT NULL,
    model_id TEXT NOT NULL,
    source_segment_ids TEXT NOT NULL,
    source_content_hash TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('open','unresolved')),
    unresolved_marked_at_ms INTEGER,
    CHECK((state = 'open' AND unresolved_marked_at_ms IS NULL)
       OR (state = 'unresolved' AND unresolved_marked_at_ms IS NOT NULL))
))sql"},
    {"index", "project_memory_compaction_intents_project_state", "project_memory_compaction_intents",
     "CREATE INDEX project_memory_compaction_intents_project_state "
     "ON project_memory_compaction_intents(project_id,state,created_at_ms,id)"},
    {"index", "project_memory_compaction_intents_state_time", "project_memory_compaction_intents",
     "CREATE INDEX project_memory_compaction_intents_state_time "
     "ON project_memory_compaction_intents(state,created_at_ms,id)"},
    {"trigger", "project_memory_compaction_intents_no_used_id", "project_memory_compaction_intents",
     R"sql(CREATE TRIGGER project_memory_compaction_intents_no_used_id
BEFORE INSERT ON project_memory_compaction_intents
WHEN EXISTS(SELECT 1 FROM project_memory_compaction_records WHERE id = NEW.id)
BEGIN
    SELECT RAISE(ABORT, 'AIO_COMPACTION_INTENT_ID_OCCUPIED');
END)sql"},
    {"trigger", "project_memory_compaction_records_intent_guard", "project_memory_compaction_records",
     R"sql(CREATE TRIGGER project_memory_compaction_records_intent_guard
BEFORE INSERT ON project_memory_compaction_records
WHEN EXISTS(SELECT 1 FROM project_memory_compaction_intents WHERE id = NEW.id)
 AND NOT EXISTS(SELECT 1 FROM project_memory_compaction_intents
                WHERE id = NEW.id AND project_id = NEW.project_id AND provider_id = NEW.provider_id
                  AND model_id = NEW.model_id AND source_segment_ids = NEW.source_segment_ids
                  AND source_content_hash = NEW.source_content_hash AND created_at_ms = NEW.created_at_ms)
BEGIN
    SELECT RAISE(ABORT, 'AIO_COMPACTION_INTENT_IDENTITY_CONFLICT');
END)sql"},
    {"trigger", "project_memory_compaction_records_intent_close", "project_memory_compaction_records",
     R"sql(CREATE TRIGGER project_memory_compaction_records_intent_close
AFTER INSERT ON project_memory_compaction_records
BEGIN
    DELETE FROM project_memory_compaction_intents WHERE id = NEW.id;
END)sql"},
};

bool isWordCharacter(char ch) noexcept
{
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) != 0 || ch == '_' || ch == '$' || byte >= 0x80;
}

std::string lowered(std::string text)
{
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

common::Error failure(const std::string& message)
{
    return error("migration.v75_failed", message);
}

// Creates `object` when nothing of that name exists; accepts an existing one only when it is exactly this step's own.
common::Result<void> ensureObject(sqlite3* handle, const CompactionIntentSchemaObject& object)
{
    const bool isTrigger = std::strcmp(object.type, "trigger") == 0;
    bool found = false;
    {
        Statement lookup;
        const auto prepared = prepare(handle,
            isTrigger ? "SELECT type,tbl_name,sql FROM sqlite_master WHERE type='trigger' AND name = ?1 COLLATE NOCASE;"
                      : "SELECT type,tbl_name,sql FROM sqlite_master WHERE type IN ('table','view','index') AND name = ?1 COLLATE NOCASE;",
            lookup);
        if (!prepared.success()) return common::Result<void>::failure(failure(prepared.error()->message));
        const auto bound = bindText(handle, lookup.get(), 1, object.name);
        if (!bound.success()) return common::Result<void>::failure(failure(bound.error()->message));
        const std::optional<std::string> expected = normalizeSchemaSqlForComparison(object.sql);
        int rows = 0;
        for (;;) {
            const int step = sqlite3_step(lookup.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return common::Result<void>::failure(failure(
                    std::string("Could not inspect the existing schema object ") + object.name));
            }
            ++rows;
            const bool hasSql = sqlite3_column_type(lookup.get(), 2) != SQLITE_NULL;
            const std::optional<std::string> stored =
                hasSql ? normalizeSchemaSqlForComparison(columnText(lookup.get(), 2)) : std::nullopt;
            const bool same = columnText(lookup.get(), 0) == object.type
                && lowered(columnText(lookup.get(), 1)) == lowered(object.table)
                && stored.has_value() && expected.has_value() && *stored == *expected;
            if (!same || rows > 1) {
                return common::Result<void>::failure(failure(
                    std::string("The schema object ") + object.name + " already exists with a different definition"));
            }
            found = true;
        }
    }
    if (found) return common::Result<void>::ok();
    return execute(handle, (std::string(object.sql) + ";").c_str(), "migration.v75_failed");
}

} // namespace

bool isCompactionIntentTriggerAbort(int extendedCode, const char* message, const char* token) noexcept
{
    return extendedCode == SQLITE_CONSTRAINT_TRIGGER && message != nullptr && token != nullptr && std::strcmp(message, token) == 0;
}

const CompactionIntentSchemaObject* compactionIntentSchemaObjects() noexcept
{
    return kObjects;
}

std::optional<std::string> normalizeSchemaSqlForComparison(const std::string& sql)
{
    std::string out;
    bool pendingSpace = false;
    const std::size_t size = sql.size();
    for (std::size_t i = 0; i < size;) {
        const char ch = sql[i];
        if (ch == '-' && i + 1 < size && sql[i + 1] == '-') {
            while (i < size && sql[i] != '\n') ++i;
            pendingSpace = true;
            continue;
        }
        if (ch == '/' && i + 1 < size && sql[i + 1] == '*') {
            const std::size_t close = sql.find("*/", i + 2);
            if (close == std::string::npos) return std::nullopt;
            i = close + 2;
            pendingSpace = true;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`' || ch == '[') {
            const char closing = ch == '[' ? ']' : ch;
            std::size_t j = i + 1;
            for (;;) {
                if (j >= size) return std::nullopt;
                if (sql[j] == closing) {
                    if (closing != ']' && j + 1 < size && sql[j + 1] == closing) { j += 2; continue; }
                    break;
                }
                ++j;
            }
            out.append(sql, i, j + 1 - i);   // delimiters and content verbatim
            pendingSpace = false;
            i = j + 1;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            pendingSpace = true;
            ++i;
            continue;
        }
        if (pendingSpace && !out.empty() && isWordCharacter(out.back()) && isWordCharacter(ch)) out += ' ';
        pendingSpace = false;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        ++i;
    }
    while (!out.empty() && out.back() == ';') out.pop_back();
    return out;
}

common::Result<void> applyCompactionIntentMigration(sqlite3* handle)
{
    const auto begun = execute(handle, "BEGIN IMMEDIATE;", "migration.v75_failed");
    if (!begun.success()) return begun;

    const auto rollBackWith = [&](common::Error cause) {
        static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
        return common::Result<void>::failure(std::move(cause));
    };
    for (const CompactionIntentSchemaObject& object : kObjects) {
        const auto ensured = ensureObject(handle, object);
        if (!ensured.success()) return rollBackWith(*ensured.error());
    }
    const auto versioned = execute(handle, "PRAGMA user_version = 75;", "migration.v75_failed");
    if (!versioned.success()) return rollBackWith(*versioned.error());
    const auto committed = execute(handle, "COMMIT;", "migration.v75_failed");
    if (!committed.success()) return rollBackWith(*committed.error());
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure::sqlite_detail
