#pragma once

// Persistence-private. Schema step 75: the compaction intent table, its indexes and the three guarded triggers (see
// ProjectMemoryCompactionIntent.hpp). Only SqliteSchema.cpp calls applyCompactionIntentMigration(); the tokens and the SQL
// normalization are shared with the intent repository, the old record writer (error translation) and the tests.

#include "ai/common/Result.hpp"

#include <sqlite3.h>

#include <optional>
#include <string>

namespace ai::infrastructure::sqlite_detail {

inline constexpr const char* kCompactionIntentsTable = "project_memory_compaction_intents";

// The fixed messages of the two RAISE(ABORT, ...) of the step. A trigger error is recognised ONLY by SQLITE_CONSTRAINT_TRIGGER (1811)
// together with the EXACT message - never by a substring and never by "some constraint".
inline constexpr const char* kCompactionIntentConflictToken = "AIO_COMPACTION_INTENT_IDENTITY_CONFLICT";
inline constexpr const char* kCompactionIntentIdOccupiedToken = "AIO_COMPACTION_INTENT_ID_OCCUPIED";

[[nodiscard]] bool isCompactionIntentTriggerAbort(int extendedCode, const char* message, const char* token) noexcept;

// Step 75 as ONE transaction: BEGIN IMMEDIATE, every missing object created, every existing same-named object accepted only when its
// stored SQL is exactly this step's own (normalized), PRAGMA user_version = 75, COMMIT. Any failure rolls everything back and leaves
// user_version and every earlier object as they were (migration.v75_failed). Repeatable over its own objects without losing rows.
[[nodiscard]] common::Result<void> applyCompactionIntentMigration(sqlite3* handle);

// The SQL text of an object reduced to code only for an EXACT comparison: comments dropped, whitespace between tokens collapsed to
// nothing (a single space only between two word characters), letters upper-cased OUTSIDE string literals and quoted identifiers,
// whose content is kept verbatim, a trailing ';' dropped. nullopt for an unterminated comment, literal or quoted identifier
// (never equal to anything). The text of a guard inside a comment or a literal therefore can neither be hidden nor faked.
[[nodiscard]] std::optional<std::string> normalizeSchemaSqlForComparison(const std::string& sql);

// The exact SQL of this step's objects, as created (tests compare stored objects against these).
struct CompactionIntentSchemaObject {
    const char* type;     // table | index | trigger
    const char* name;
    const char* table;    // the table the object belongs to (sqlite_master.tbl_name)
    const char* sql;      // CREATE ... without a trailing semicolon
};
inline constexpr int kCompactionIntentSchemaObjectCount = 6;
[[nodiscard]] const CompactionIntentSchemaObject* compactionIntentSchemaObjects() noexcept;

} // namespace ai::infrastructure::sqlite_detail
