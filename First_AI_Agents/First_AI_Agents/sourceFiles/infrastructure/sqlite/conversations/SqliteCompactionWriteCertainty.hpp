#pragma once

// Persistence-private. After a compaction write FAILED and its transaction (if any) has been closed, decide - from what is really
// stored - whether that write left anything behind. It only reads, outside any transaction, on the connection the caller already
// holds the mutex of. It never writes and never decides what to do about the answer (the accounting fallback is the application's).

#include "../SqliteConnectionHealth.hpp"
#include "../SqliteFaultForTesting.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"

#include <sqlite3.h>

#include <optional>
#include <string>
#include <vector>

namespace ai::infrastructure::sqlite_detail {

struct WriteProofRequest {
    std::string recordId;                           // the id of the record this write tried to store
    std::optional<std::string> resultSegmentId;     // and, for a successful compaction, of its result segment
    // The primary error was "this id already exists": the row that is there was NOT written by this call.
    bool recordIdCollision {false};
    bool segmentIdCollision {false};
};

struct WriteProof {
    application::CompactionPersistState state {application::CompactionPersistState::Unknown};
    std::vector<application::CompactionDiagnostic> diagnostics;
};

// NotCommitted only when no transaction is open (autocommit != 0) AND neither the record nor the result segment of THIS write is
// stored. A row found under a colliding id is reported (a diagnostic) but is not this write's. Anything else - a transaction still
// open, an unreadable proof, a row that contradicts the error - is Unknown. Reading one's own uncommitted rows is not proof, and
// this function refuses to run while a transaction is open.
[[nodiscard]] WriteProof proveWriteNotStored(sqlite3* db, FaultPlan& fault, const WriteProofRequest& request);

// Appends a diagnostic, bounded: at most kMaxCompactionDiagnostics entries, each message at most
// kMaxCompactionDiagnosticMessageBytes and never cut inside a UTF-8 character.
void appendDiagnostic(std::vector<application::CompactionDiagnostic>& into, application::CompactionDiagnosticStage stage,
    common::Error error);

// The native codes captured at the moment of the failure, as plain integers for the neutral outcome (the text is bounded and kept
// valid UTF-8). nullopt in, nullopt out.
[[nodiscard]] std::optional<application::CompactionNativeDetail> toNativeDetail(const std::optional<NativeError>& native);

} // namespace ai::infrastructure::sqlite_detail
