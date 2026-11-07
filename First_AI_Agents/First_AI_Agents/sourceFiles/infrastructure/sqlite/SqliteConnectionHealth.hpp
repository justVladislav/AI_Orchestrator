#pragma once

// Persistence-private (never on an exported include path). The health of ONE shared SQLite connection: whether it may still be used
// by every repository, who owns the transaction that is open on it, and the isolation ("quarantine") of a connection that must not
// be used any more.
//
// Why an authorizer: one sqlite3* and one mutex serve ~36 repository files. If a ROLLBACK fails and the transaction stays open, any
// other repository would write INTO it (uncommitted, lost on close) or fail on its own BEGIN. Isolation has to reach all of them
// without touching them, without closing or replacing the handle behind their back and without a process-global registry:
// sqlite3_set_authorizer is a per-connection, per-statement-compilation filter. It was probed on the two SQLite builds this
// project links (see the S2b contract): re-registering the callback expires statements prepared EARLIER, so they are refused at
// their next step with SQLITE_AUTH; merely flipping a flag does NOT (statements prepared before the flip still ran and wrote).
// Hence the two rules this class enforces: the callback is (re)registered on EVERY transition into and out of the quarantine, and the
// result of that registration is checked. An already EXECUTING statement is not stopped by an authorizer: the caller has to have
// finalized/reset its statements before a transition (SqliteTransactionScope does, by construction).
//
// Everything here is called with the owning SqliteDatabase's mutex held. The callback itself never throws, never calls SQLite and
// never locks.

#include "ai/common/Error.hpp"
#include "ai/common/Result.hpp"

#include <sqlite3.h>

#include <atomic>

// The minimum supported SQLite is 3.44.0: the per-connection association below uses sqlite3_set_clientdata/sqlite3_get_clientdata,
// which exist only from that version (https://www.sqlite.org/c3ref/get_clientdata.html). A build against an older SQLite stops here,
// with this message, instead of failing on a missing symbol. (The vcpkg baseline is NOT pinned to one version for this.)
#define AIO_MINIMUM_SQLITE_VERSION_NUMBER 3044000
static_assert(SQLITE_VERSION_NUMBER >= AIO_MINIMUM_SQLITE_VERSION_NUMBER,
    "AI Orchestrator persistence needs SQLite 3.44.0 or newer (sqlite3_set_clientdata)");

namespace ai::infrastructure::sqlite_detail {

inline constexpr int kMinimumSqliteVersionNumber = AIO_MINIMUM_SQLITE_VERSION_NUMBER;

// "<what> (sqlite result <rc>): <the handle's text>" as a persistence.isolation_failed error. May throw (it allocates).
[[nodiscard]] common::Error makeIsolationError(const char* what, int rc, sqlite3* db);

enum class QuarantineReason {
    None,
    RollbackFailed,          // OUR transaction could not be closed (the owner tag is set)
    ForeignOpenTransaction,  // a transaction was already open at entry and it is not ours: never rolled back by us
    BeginStateUnknown        // BEGIN failed and the transaction state is not what it must be
};

[[nodiscard]] const char* toString(QuarantineReason reason) noexcept;

class ConnectionHealth final {
public:
    ConnectionHealth() = default;
    ConnectionHealth(const ConnectionHealth&) = delete;
    ConnectionHealth& operator=(const ConnectionHealth&) = delete;

    [[nodiscard]] bool quarantined() const noexcept { return reason_.load(std::memory_order_acquire) != QuarantineReason::None; }
    [[nodiscard]] QuarantineReason reason() const noexcept { return reason_.load(std::memory_order_acquire); }
    // True when entering the quarantine was requested but the authorizer could not be installed: the connection is unusable and
    // NOT isolated.
    [[nodiscard]] bool isolationFailed() const noexcept { return isolationFailed_.load(std::memory_order_acquire); }

    // The tag of the component whose transaction is open on this connection (a string literal), or nullptr. It is set when that
    // component's BEGIN succeeds and cleared only by a confirmed COMMIT/ROLLBACK, so "is this transaction ours?" has an answer that
    // does not depend on guessing from text.
    [[nodiscard]] const char* transactionOwner() const noexcept { return owner_.load(std::memory_order_acquire); }
    void markTransactionOwner(const char* tag) noexcept { owner_.store(tag, std::memory_order_release); }
    void clearTransactionOwner() noexcept { owner_.store(nullptr, std::memory_order_release); }

    // Enter the quarantine: record the reason, then (re)register the authorizer. The first reason is kept. A failed registration
    // is reported and recorded (isolationFailed()); it is never presented as an isolated connection.
    [[nodiscard]] common::Result<void> enterQuarantine(sqlite3* db, QuarantineReason reason);

    // The same transition WITHOUT anything that can throw (no string, no Error): records the reason, re-registers the authorizer and
    // returns the SQLite result of that registration (SQLITE_OK = isolated; anything else = isolationFailed() is set and the
    // connection is NOT isolated). This is the one the cleanup uses, so that isolating a connection never depends on an allocation.
    [[nodiscard]] int enterQuarantineNoThrow(sqlite3* db, QuarantineReason reason) noexcept;

    // Leave the quarantine: FIRST explicitly remove the authorizer (this registers again and expires prepared statements), and only
    // when that worked clear the state. If the removal fails the connection stays exactly as isolated as it was - it is never
    // declared healthy on the strength of a setter that did not work. `simulateSetterFailure` is the test seam's way of making the
    // removal fail without performing it.
    [[nodiscard]] common::Result<void> release(sqlite3* db, bool simulateSetterFailure = false);

    // Removes the callback from `db` (before the handle is closed). Never throws.
    void detach(sqlite3* db) noexcept;

    // The narrow window in which the OWNER may close its own transaction: while it is open, a quarantined connection lets exactly
    // one operation through - SQLITE_TRANSACTION/ROLLBACK. BEGIN, COMMIT and every statement stay refused.
    class RecoveryWindow final {
    public:
        explicit RecoveryWindow(ConnectionHealth& health) noexcept : health_(health) { health_.recovery_.store(true, std::memory_order_release); }
        ~RecoveryWindow() { health_.recovery_.store(false, std::memory_order_release); }
        RecoveryWindow(const RecoveryWindow&) = delete;
        RecoveryWindow& operator=(const RecoveryWindow&) = delete;

    private:
        ConnectionHealth& health_;
    };

    // The callback registered with sqlite3_set_authorizer (user data = this object).
    static int authorizer(void* user, int action, const char* arg1, const char*, const char*, const char*) noexcept;

    // Per-connection association used by the shared primitives to recognise a refusal that comes from THIS quarantine (and not from
    // any other authorization error). Stored as sqlite3 client data: no registry, released with the connection.
    [[nodiscard]] bool attach(sqlite3* db) noexcept;

private:
    std::atomic<QuarantineReason> reason_ {QuarantineReason::None};
    std::atomic<bool> recovery_ {false};
    std::atomic<bool> isolationFailed_ {false};
    std::atomic<const char*> owner_ {nullptr};
};

// The health object attached to `db`, or nullptr (a connection that was not opened through SqliteDatabase::open).
[[nodiscard]] const ConnectionHealth* connectionHealthOf(sqlite3* db) noexcept;

// How many statements of `db` are executing right now (stepped, not reset/finalized). A transition must find none.
[[nodiscard]] int busyStatementCount(sqlite3* db) noexcept;

} // namespace ai::infrastructure::sqlite_detail
