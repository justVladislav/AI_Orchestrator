#pragma once

// Persistence-private schema ownership. Only SqliteDatabase::open() calls this.
#include "ai/common/Result.hpp"

#include <sqlite3.h>

namespace ai::infrastructure::sqlite_detail {

// The ONE source of the schema version this build supports. ensureSchema() migrates up to it and refuses anything newer; the
// pre-migration backup decides from it whether an existing database needs a copy and names that copy by it.
inline constexpr int supportedSchemaVersion = 71;

// Brings a freshly opened database to the supported schema version.
//
// The handle is non-null, already configured (foreign keys, WAL, busy timeout)
// and not yet published to any repository; the caller keeps sole ownership and
// closes it. ensureSchema neither opens nor closes the handle, takes no
// repository lock and returns the original error of the failing step. It is not
// for running migrations on a database that is already in use.
common::Result<void> ensureSchema(sqlite3* handle);

} // namespace ai::infrastructure::sqlite_detail
