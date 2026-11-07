#pragma once

// Persistence-private (never on an exported include path; only the persistence tests add this folder to their include path).
// The CLOSED fault seam of the compaction write path. It makes a REAL failure branch of the REAL code run, it never skips or weakens
// a check and it never fakes a result:
//   - it does not substitute an error for a statement that ran; where it prevents a statement (a ROLLBACK) the REAL transaction is
//     left open, so autocommit, the quarantine and the next repository call all see the truth;
//   - it is per connection (it lives in that SqliteDatabase's Impl) and scoped (RAII): there is no process-global switch, no public
//     hook, no constructor or setter on any service or repository;
//   - by default nothing is armed. Production code only READS the plan, under the connection's mutex.
// This is NOT the process-global setBackupTestSeam of the pre-migration backup: that one stays as it is.

#include <sqlite3.h>

#include <mutex>
#include <new>
#include <optional>

namespace ai::infrastructure {
class SqliteDatabase;
}

namespace ai::infrastructure::sqlite_detail {

class ConnectionHealth;

struct FaultPlan {
    // The next N ROLLBACK attempts of the transaction scope are NOT executed. The real transaction stays open; the attempt reports a
    // failure with a stated, synthetic cause.
    int skipRollbackAttempts {0};
    // The proof read (is the record/segment stored?) fails.
    bool failProofRead {false};
    // The repository throws right after its BEGIN succeeded (the RAII cleanup path).
    bool throwAfterBegin {false};
    // Building/recording the cause of a failed ROLLBACK attempt throws std::bad_alloc (the allocation of a diagnostic failing in
    // the middle of a cleanup). The cleanup must still count the attempt, isolate the connection and not throw.
    bool throwWhileRecordingRollbackFailure {false};
    // The next N removals of the authorizer (the lifting of a quarantine) report a failure WITHOUT being performed, so the
    // authorizer stays installed exactly as the real setter's failure would leave it.
    int failAuthorizerRemovals {0};

    // Consumes one skipped ROLLBACK attempt. Called under the connection's mutex.
    bool consumeSkippedRollback() noexcept
    {
        if (skipRollbackAttempts <= 0) return false;
        --skipRollbackAttempts;
        return true;
    }
    bool consumeFailedAuthorizerRemoval() noexcept
    {
        if (failAuthorizerRemovals <= 0) return false;
        --failAuthorizerRemovals;
        return true;
    }
    void maybeThrowWhileRecordingRollbackFailure() const
    {
        if (throwWhileRecordingRollbackFailure) throw std::bad_alloc();
    }
};

struct ConnectionHandles {
    sqlite3* handle {nullptr};
    std::mutex* mutex {nullptr};
    ConnectionHealth* health {nullptr};
    FaultPlan* fault {nullptr};
};

// Friend of SqliteDatabase (a forward declaration in its header; nothing else of the persistence internals is exposed).
struct ConnectionAccess {
    static ConnectionHandles of(SqliteDatabase& database);
};

// Arms `plan` on one connection for the lifetime of this object and, optionally, replaces the busy timeout (restored afterwards).
class ScopedConnectionFault final {
public:
    explicit ScopedConnectionFault(SqliteDatabase& database, FaultPlan plan, std::optional<int> busyTimeoutMs = std::nullopt)
        : handles_(ConnectionAccess::of(database))
    {
        std::lock_guard lock(*handles_.mutex);
        *handles_.fault = plan;
        if (busyTimeoutMs.has_value()) {
            sqlite3_busy_timeout(handles_.handle, *busyTimeoutMs);
            busyTimeoutChanged_ = true;
        }
    }

    ~ScopedConnectionFault()
    {
        std::lock_guard lock(*handles_.mutex);
        *handles_.fault = FaultPlan {};
        if (busyTimeoutChanged_) sqlite3_busy_timeout(handles_.handle, 5000);   // the production value (SqliteDatabase.cpp)
    }

    ScopedConnectionFault(const ScopedConnectionFault&) = delete;
    ScopedConnectionFault& operator=(const ScopedConnectionFault&) = delete;

    [[nodiscard]] const ConnectionHandles& handles() const noexcept { return handles_; }

private:
    ConnectionHandles handles_;
    bool busyTimeoutChanged_ {false};
};

} // namespace ai::infrastructure::sqlite_detail
