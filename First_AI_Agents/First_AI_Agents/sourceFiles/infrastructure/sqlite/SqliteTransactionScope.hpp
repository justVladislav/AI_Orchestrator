#pragma once

// Persistence-private. One explicit write transaction on the shared connection, owned by a named component, with a checkable entry,
// a COMMIT whose primary error is captured before anything else touches the handle, a bounded cleanup and a no-throw destructor.
// It makes NO business decision: whether a failed write gets an accounting fallback is the application layer's call; this class only
// reports what it did and what it can prove.
//
// Everything runs with the connection's mutex held by the caller, for the whole life of the scope. All statements of the caller's
// operation must be finalized/reset BEFORE commit(), rollback() or the destructor (the repository keeps them in inner scopes):
// an authorizer does not stop a statement that is already executing, so quiescence is a property of the code, not of the callback.

#include "SqliteConnectionHealth.hpp"
#include "SqliteFaultForTesting.hpp"
#include "SqlitePrimitives.hpp"

#include "ai/common/Error.hpp"

#include <optional>
#include <vector>

namespace ai::infrastructure::sqlite_detail {

enum class ConnectionReport { Healthy, Quarantined, IsolationFailed };

// At most two ROLLBACK attempts per cleanup episode, with the autocommit state checked before each and after the last one; no sleep,
// no retry loop, and the destructor never starts a third round after an explicit cleanup already used its attempts.
inline constexpr int kMaxRollbackAttemptsPerEpisode = 2;

struct CleanupReport {
    bool transactionClosed {true};                 // autocommit != 0 afterwards
    int rollbackAttempts {0};
    std::vector<common::Error> rollbackErrors;     // one entry per failed ROLLBACK attempt, never merged
    ConnectionReport connection {ConnectionReport::Healthy};
    std::optional<common::Error> isolationError;   // the authorizer registration failed
    int busyStatements {0};                        // statements still executing when the cleanup began (a bug of the caller)
    // A cause happened but could not be recorded (describing it failed, e.g. out of memory). Only the DIAGNOSTICS are incomplete:
    // the attempts were counted, the autocommit state was checked and the connection was isolated without them.
    bool diagnosticsIncomplete {false};
};

struct BeginOutcome {
    bool started {false};
    // false: whether anything of the caller's write was started is NOT proven (BEGIN failed in a state it must not be in).
    bool notStartedProven {true};
    ConnectionReport connection {ConnectionReport::Healthy};
    std::optional<common::Error> error;            // why the entry or BEGIN was refused (the primary error of the operation)
    std::optional<NativeError> native;             // the native codes of a failed BEGIN, captured at once
    std::vector<common::Error> notes;              // secondary causes: a lazy recovery attempt, an isolation failure
    bool diagnosticsIncomplete {false};            // a secondary cause could not be recorded
};

struct CommitOutcome {
    bool committed {false};
    std::optional<common::Error> primary;          // the COMMIT error, captured before the cleanup
    std::optional<NativeError> native;             // its native codes
    CleanupReport cleanup;
};

class SqliteTransactionScope final {
public:
    // `ownerTag` is a string literal naming the component; it marks the transaction as ours for the whole time it is open.
    SqliteTransactionScope(sqlite3* db, ConnectionHealth& health, FaultPlan& fault, const char* ownerTag) noexcept
        : db_(db), health_(health), fault_(fault), owner_(ownerTag)
    {
    }
    ~SqliteTransactionScope() noexcept;

    SqliteTransactionScope(const SqliteTransactionScope&) = delete;
    SqliteTransactionScope& operator=(const SqliteTransactionScope&) = delete;

    // The entry protocol, without opening a transaction - for a write that is a single autocommit statement. Quarantined or with a
    // transaction already open: refused. A transaction left by an earlier cleanup of THIS owner is recovered here (at most two
    // ROLLBACK attempts, nothing else); a transaction that is not ours is never rolled back - the connection is isolated instead.
    [[nodiscard]] static BeginOutcome admit(sqlite3* db, ConnectionHealth& health, FaultPlan& fault, const char* ownerTag);

    // admit(), then BEGIN IMMEDIATE.
    [[nodiscard]] BeginOutcome begin();
    // COMMIT. On failure the primary error and its native codes are kept, then the open transaction is closed (cleanup()).
    [[nodiscard]] CommitOutcome commit();
    // Explicit cleanup after a failed step: close our transaction. It never throws. The explicit cleanup and the destructor share
    // ONE budget of kMaxRollbackAttemptsPerEpisode attempts; the destructor never starts another round, it only makes sure that a
    // transaction that is still open leaves the connection isolated.
    [[nodiscard]] CleanupReport rollback();

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    sqlite3* db_;
    ConnectionHealth& health_;
    FaultPlan& fault_;
    const char* owner_;
    bool active_ {false};
    // ROLLBACK attempts already spent by this scope's cleanups (the explicit one and the destructor share ONE budget). It is a
    // plain counter bumped BEFORE each attempt, so nothing that happens afterwards can give the budget back.
    int rollbackAttemptsUsed_ {0};
};

// Close the transaction open on `db` (ours): up to kMaxRollbackAttemptsPerEpisode ROLLBACKs inside the owner's recovery window,
// autocommit checked before each; if it is still open afterwards the connection is isolated. Used by the scope and by tests.
//
// It NEVER throws, whatever happens to the allocation of its diagnostics: every attempt is counted in `attemptsUsed` before it is
// made, every step that allocates is guarded (a failure only sets CleanupReport::diagnosticsIncomplete), and the isolation of a
// connection whose transaction stayed open is done by a path that allocates nothing (ConnectionHealth::enterQuarantineNoThrow).
// `attemptsUsed` is the budget already spent; the function stops at kMaxRollbackAttemptsPerEpisode in total.
[[nodiscard]] CleanupReport closeOpenTransaction(sqlite3* db, ConnectionHealth& health, FaultPlan& fault, int& attemptsUsed) noexcept;

} // namespace ai::infrastructure::sqlite_detail
