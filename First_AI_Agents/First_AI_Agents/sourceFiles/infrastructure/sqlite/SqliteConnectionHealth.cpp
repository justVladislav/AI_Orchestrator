#include "SqliteConnectionHealth.hpp"
#include "SqliteConnection.hpp"
#include "SqliteFaultForTesting.hpp"

#include "ai/common/Error.hpp"

#include <cctype>
#include <cstring>
#include <string>

namespace ai::infrastructure::sqlite_detail {

namespace {

constexpr const char* kClientDataKey = "aio.connection_health";

bool iequals(const char* a, const char* b) noexcept
{
    if (a == nullptr || b == nullptr) return false;
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return *a == *b;
}

} // namespace

common::Error makeIsolationError(const char* what, int rc, sqlite3* db)
{
    std::string message = what;
    message += " (sqlite result ";
    message += std::to_string(rc);
    message += "): ";
    message += db != nullptr ? sqlite3_errmsg(db) : "SQLite handle is unavailable";
    return {"persistence.isolation_failed", std::move(message)};
}

const char* toString(QuarantineReason reason) noexcept
{
    switch (reason) {
    case QuarantineReason::None: return "none";
    case QuarantineReason::RollbackFailed: return "rollback_failed";
    case QuarantineReason::ForeignOpenTransaction: return "foreign_open_transaction";
    case QuarantineReason::BeginStateUnknown: return "begin_state_unknown";
    }
    return "unknown";
}

int ConnectionHealth::authorizer(void* user, int action, const char* arg1, const char*, const char*, const char*) noexcept
{
    const auto* health = static_cast<const ConnectionHealth*>(user);
    if (health == nullptr) return SQLITE_OK;
    if (health->reason_.load(std::memory_order_acquire) == QuarantineReason::None) return SQLITE_OK;
    // Quarantined: everything is refused - including a ROLLBACK from a caller that does not own the open transaction - except the
    // owner's own rollback inside its recovery window.
    const bool ownerRollback = action == SQLITE_TRANSACTION && iequals(arg1, "ROLLBACK")
        && health->recovery_.load(std::memory_order_acquire);
    return ownerRollback ? SQLITE_OK : SQLITE_DENY;
}

int ConnectionHealth::enterQuarantineNoThrow(sqlite3* db, QuarantineReason reason) noexcept
{
    if (reason == QuarantineReason::None) return SQLITE_MISUSE;
    QuarantineReason expected = QuarantineReason::None;
    reason_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);   // the first reason is kept
    // ALWAYS (re)register: this expires statements prepared earlier. Flipping the state alone would let them run.
    const int rc = sqlite3_set_authorizer(db, &ConnectionHealth::authorizer, this);
    isolationFailed_.store(rc != SQLITE_OK, std::memory_order_release);
    return rc;
}

common::Result<void> ConnectionHealth::enterQuarantine(sqlite3* db, QuarantineReason reason)
{
    if (reason == QuarantineReason::None) {
        return common::Result<void>::failure({"persistence.isolation_failed", "A quarantine needs a reason"});
    }
    // The state is set (and the authorizer registered) before anything that allocates: an exception while describing a failure
    // cannot undo the isolation.
    const int rc = enterQuarantineNoThrow(db, reason);
    if (rc != SQLITE_OK) {
        return common::Result<void>::failure(makeIsolationError("Could not register the SQLite authorizer", rc, db));
    }
    return common::Result<void>::ok();
}

common::Result<void> ConnectionHealth::release(sqlite3* db, bool simulateSetterFailure)
{
    // Explicit removal, again expiring prepared statements so that they are re-prepared without the filter. It comes FIRST: if it
    // fails nothing below happens, the reason stays set and the (still installed) callback keeps refusing - the connection is
    // never reported healthy because a flag was cleared before the setter was known to have worked.
    const int rc = simulateSetterFailure ? SQLITE_ERROR : sqlite3_set_authorizer(db, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        return common::Result<void>::failure(makeIsolationError(
            simulateSetterFailure ? "Could not remove the SQLite authorizer (fault injection: not performed)"
                                  : "Could not remove the SQLite authorizer",
            rc, db));
    }
    reason_.store(QuarantineReason::None, std::memory_order_release);
    recovery_.store(false, std::memory_order_release);
    isolationFailed_.store(false, std::memory_order_release);
    return common::Result<void>::ok();
}

void ConnectionHealth::detach(sqlite3* db) noexcept
{
    if (db != nullptr) {
        sqlite3_set_authorizer(db, nullptr, nullptr);
        sqlite3_set_clientdata(db, kClientDataKey, nullptr, nullptr);
    }
}

bool ConnectionHealth::attach(sqlite3* db) noexcept
{
    return db != nullptr && sqlite3_set_clientdata(db, kClientDataKey, this, nullptr) == SQLITE_OK;
}

const ConnectionHealth* connectionHealthOf(sqlite3* db) noexcept
{
    return db == nullptr ? nullptr : static_cast<const ConnectionHealth*>(sqlite3_get_clientdata(db, kClientDataKey));
}

int busyStatementCount(sqlite3* db) noexcept
{
    int busy = 0;
    if (db == nullptr) return busy;
    for (sqlite3_stmt* statement = sqlite3_next_stmt(db, nullptr); statement != nullptr; statement = sqlite3_next_stmt(db, statement)) {
        if (sqlite3_stmt_busy(statement) != 0) ++busy;
    }
    return busy;
}

// The friend of SqliteDatabase that hands the persistence tests the connection's internals (handle, mutex, health, fault plan).
ConnectionHandles ConnectionAccess::of(SqliteDatabase& database)
{
    auto& impl = *database.impl_;
    return ConnectionHandles {impl.handle, &impl.mutex, &impl.health, &impl.fault};
}

} // namespace ai::infrastructure::sqlite_detail
