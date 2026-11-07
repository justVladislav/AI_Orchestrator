#pragma once

#include "ai/common/Error.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// C2-B/S2b: the neutral vocabulary in which a compaction repository reports WHAT HAPPENED to one write, not only that it failed.
// It contains no SQLite or Qt type: the application layer decides what to do with an outcome (the Failed accounting fallback, the
// identity check, the user-facing message); the persistence layer only proves what it can prove.

// The fate of THIS write (the record - and for a successful compaction the result segment - that the caller asked to store).
//   Committed     the write is stored (the call succeeded).
//   NotCommitted  PROVEN not stored: either nothing of it was ever started, or the transaction was closed and a read outside any
//                 transaction found none of its rows. A row that already existed under the same id is not "stored by this write".
//   Unknown       not proven either way: the transaction could not be closed, the proof could not be read, or the proof contradicts
//                 the error. An Unknown is never turned into Committed or NotCommitted by a later guess.
enum class CompactionPersistState { Committed, NotCommitted, Unknown };

// The usability of the shared SQLite connection AFTER the call - a separate axis from the fate of the write.
//   Healthy          the connection is usable by every repository.
//   Quarantined      the connection is isolated: every statement of every repository is refused (persistence.connection_unusable)
//                    until the transaction that was left open is closed by its owner.
//   IsolationFailed  the connection is unusable but could NOT be isolated (the isolation setter itself failed): callers must not
//                    treat it as healthy.
// A repository that could not observe it (a legacy implementation) reports no value at all (std::optional), never Healthy.
enum class CompactionConnectionState { Healthy, Quarantined, IsolationFailed };

// Where a secondary cause happened. The primary error of the operation is NEVER one of these: it travels in the outcome's result.
enum class CompactionDiagnosticStage {
    PrimaryWrite,       // a detail of the primary write that is not itself the primary error
    Rollback,           // a ROLLBACK attempt that failed (one entry per attempt)
    ProofRead,          // the read that proves what is stored, or its finding
    FallbackWrite,      // reserved for the service: the failed accounting fallback insert
    FallbackRollback,   // reserved for the service
    FallbackProofRead,  // reserved for the service
    IdentityCheck,      // reserved for the service: the comparison with an already stored row
    IntentWrite,        // B3-II-A: the write of the compaction intent (admission), or a cleanup of that write
    IntentProofRead     // B3-II-A: the one read that observes the intent/record pair, or its finding
};

struct CompactionDiagnostic {
    CompactionDiagnosticStage stage {CompactionDiagnosticStage::PrimaryWrite};
    common::Error error;   // code and message of ONE cause; causes are never concatenated
};

inline constexpr std::size_t kMaxCompactionDiagnostics = 8;
inline constexpr std::size_t kMaxCompactionDiagnosticMessageBytes = 512;

// The native codes of the PRIMARY failure, captured at the moment it happened (before any rollback or proof read touched the
// connection's last-error state). Plain integers: no SQLite type crosses this boundary and nothing is reclassified from them.
struct CompactionNativeDetail {
    int code {0};            // the primary result code (e.g. 19 = constraint)
    int extendedCode {0};    // the extended code (e.g. 787 = foreign-key constraint, 1555 = primary-key constraint)
    std::string message;     // the native text, at most kMaxCompactionDiagnosticMessageBytes
};

struct CompactionPersistOutcome {
    // The PRIMARY error of this write (or success). Cleanup and proof failures never replace it.
    common::Result<void> result {common::Result<void>::ok()};
    // The native codes of that primary error when it came from a failed SQL step; nullopt for failures that did not come from SQLite
    // (a refused entry, a stale source) and for a success.
    std::optional<CompactionNativeDetail> primaryNative;
    // true: at least one secondary cause happened but could not be recorded (building a diagnostic itself failed, e.g. out of
    // memory). The guarantees of the outcome (state, connection) do not depend on diagnostics and are never weakened by this.
    bool diagnosticsIncomplete {false};
    CompactionPersistState state {CompactionPersistState::Unknown};
    // nullopt = the implementation did not observe the connection (legacy default); it is NOT "healthy".
    std::optional<CompactionConnectionState> connection;
    // Secondary causes in the order they happened, bounded (kMaxCompactionDiagnostics, each message
    // kMaxCompactionDiagnosticMessageBytes).
    std::vector<CompactionDiagnostic> diagnostics;
};

} // namespace ai::application
