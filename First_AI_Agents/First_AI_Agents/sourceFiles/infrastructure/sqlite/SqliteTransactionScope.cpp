#include "SqliteTransactionScope.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace ai::infrastructure::sqlite_detail {

namespace {

bool sameOwner(const char* a, const char* b) noexcept
{
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// Appends a secondary cause. If recording it fails (allocation), the cause is lost but the loss is flagged - and nothing else
// depends on it.
void keep(std::vector<common::Error>& into, common::Error cause, bool& lost) noexcept
{
    try {
        into.push_back(std::move(cause));
    } catch (...) {
        lost = true;
    }
}

// One ROLLBACK attempt of the owner. Under a quarantine it is the only statement the authorizer lets through, and only now.
std::optional<common::Error> attemptRollback(sqlite3* db, ConnectionHealth& health, FaultPlan& fault)
{
    if (fault.consumeSkippedRollback()) {
        // Test seam: the statement is NOT executed, the real transaction stays open and the caller sees a failed attempt.
        return common::Error {"persistence.rollback_failed", "ROLLBACK was not executed (fault injection); the transaction stays open"};
    }
    ConnectionHealth::RecoveryWindow window(health);
    const auto rolledBack = execute(db, "ROLLBACK;", "persistence.rollback_failed");
    if (rolledBack.success()) return std::nullopt;
    return *rolledBack.error();
}

common::Error unusable(const char* why)
{
    return error("persistence.connection_unusable", why);
}

} // namespace

CleanupReport closeOpenTransaction(sqlite3* db, ConnectionHealth& health, FaultPlan& fault, int& attemptsUsed) noexcept
{
    CleanupReport report;
    // Phase 1: the attempts. Each is counted BEFORE it is made, and whatever its description does (an allocation may fail while
    // the cause of a failed ROLLBACK is built or stored) neither gives the attempt back nor skips the phases below.
    try {
        report.busyStatements = busyStatementCount(db);
        while (attemptsUsed < kMaxRollbackAttemptsPerEpisode) {
            // SQLite may already have rolled the transaction back by itself (docs: lang_transaction); then there is nothing to roll back.
            if (sqlite3_get_autocommit(db) != 0) break;
            ++attemptsUsed;
            ++report.rollbackAttempts;
            try {
                if (auto failure = attemptRollback(db, health, fault)) {
                    fault.maybeThrowWhileRecordingRollbackFailure();   // test seam: the allocation of the diagnostic failing
                    report.rollbackErrors.push_back(std::move(*failure));
                }
            } catch (...) {
                report.diagnosticsIncomplete = true;
            }
        }
    } catch (...) {
        report.diagnosticsIncomplete = true;
    }

    // Phase 2: the verdict and the isolation. Nothing here allocates except the optional description of a failed registration.
    report.transactionClosed = sqlite3_get_autocommit(db) != 0;
    if (report.transactionClosed) {
        health.clearTransactionOwner();
        return report;
    }
    // Still open after the permitted attempts: the connection must not be handed to the other repositories.
    const int registered = health.enterQuarantineNoThrow(db, QuarantineReason::RollbackFailed);
    if (registered == SQLITE_OK) {
        report.connection = ConnectionReport::Quarantined;
    } else {
        report.connection = ConnectionReport::IsolationFailed;
        try {
            report.isolationError = makeIsolationError("Could not register the SQLite authorizer", registered, db);
        } catch (...) {
            report.diagnosticsIncomplete = true;
        }
    }
    return report;
}

BeginOutcome SqliteTransactionScope::admit(sqlite3* db, ConnectionHealth& health, FaultPlan& fault, const char* ownerTag)
{
    BeginOutcome outcome;
    const bool openTransaction = sqlite3_get_autocommit(db) == 0;
    if (!health.quarantined() && !openTransaction) {
        outcome.started = true;   // "admitted": nothing prevents the write
        return outcome;
    }

    // Lifting the isolation. It is never presumed: if the authorizer cannot be removed the connection stays isolated, the write is
    // refused and the cause is kept, so a failed setter can never turn into "healthy, go ahead".
    const auto lift = [&]() -> bool {
        const auto released = health.release(db, fault.consumeFailedAuthorizerRemoval());
        if (released.success()) return true;
        outcome.started = false;
        outcome.connection = health.isolationFailed() ? ConnectionReport::IsolationFailed : ConnectionReport::Quarantined;
        keep(outcome.notes, *released.error(), outcome.diagnosticsIncomplete);
        outcome.error = unusable("The isolation of the connection could not be lifted; it stays isolated and the write was not started");
        return false;
    };

    // Our own earlier cleanup closed the transaction but the isolation could not be lifted then: only the lifting remains.
    if (health.quarantined() && !openTransaction && health.reason() == QuarantineReason::RollbackFailed) {
        if (lift()) outcome.started = true;
        return outcome;
    }

    // Either the connection is isolated, or a transaction is open at entry. Is that transaction OURS?
    const bool ours = sameOwner(health.transactionOwner(), ownerTag);
    const bool recoverable = ours && (!health.quarantined() || health.reason() == QuarantineReason::RollbackFailed);
    if (recoverable) {
        // Our own earlier cleanup left it open: close it (bounded, owner-only) and, if that worked, lift the isolation. This does
        // not turn an earlier Unknown into anything - the earlier write keeps the outcome it was reported with.
        int attempts = 0;
        CleanupReport recovery = closeOpenTransaction(db, health, fault, attempts);
        if (recovery.diagnosticsIncomplete) outcome.diagnosticsIncomplete = true;
        for (auto& failure : recovery.rollbackErrors) keep(outcome.notes, std::move(failure), outcome.diagnosticsIncomplete);
        if (recovery.isolationError.has_value()) keep(outcome.notes, std::move(*recovery.isolationError), outcome.diagnosticsIncomplete);
        if (recovery.transactionClosed) {
            if (health.quarantined() && !lift()) return outcome;
            outcome.started = true;
            return outcome;
        }
        outcome.started = false;
        outcome.connection = recovery.connection;
        outcome.error = unusable("A transaction left open by an earlier failed cleanup still cannot be closed; the connection stays isolated");
        return outcome;
    }

    outcome.started = false;
    if (health.quarantined()) {
        outcome.connection = health.isolationFailed() ? ConnectionReport::IsolationFailed : ConnectionReport::Quarantined;
        outcome.error = unusable("The connection is isolated; the write was not started");
        return outcome;
    }
    // Not quarantined, a transaction is open and it is not ours: it is never rolled back here. Isolate the connection so that no
    // repository writes into it.
    const int registered = health.enterQuarantineNoThrow(db, QuarantineReason::ForeignOpenTransaction);
    if (registered != SQLITE_OK) {
        outcome.connection = ConnectionReport::IsolationFailed;
        try {
            outcome.notes.push_back(makeIsolationError("Could not register the SQLite authorizer", registered, db));
        } catch (...) {
            outcome.diagnosticsIncomplete = true;
        }
    } else {
        outcome.connection = ConnectionReport::Quarantined;
    }
    outcome.error = unusable("A transaction that is not ours was already open on the connection; it was left untouched and the "
                             "connection is isolated; the write was not started");
    return outcome;
}

BeginOutcome SqliteTransactionScope::begin()
{
    BeginOutcome outcome = admit(db_, health_, fault_, owner_);
    if (!outcome.started) return outcome;   // refused at entry: nothing of the caller's write was issued

    auto began = execute(db_, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) {
        // Before anything that allocates: the state of the connection and its native codes. The isolation of a connection that
        // is in a state this code does not create must not depend on describing the failure first.
        const bool transactionOpen = sqlite3_get_autocommit(db_) == 0;
        std::optional<NativeError> native = tryCaptureNativeError(db_);
        int registered = SQLITE_OK;
        if (transactionOpen) registered = health_.enterQuarantineNoThrow(db_, QuarantineReason::BeginStateUnknown);

        outcome.started = false;
        outcome.native = std::move(native);
        outcome.error = std::move(*began.error());
        if (!transactionOpen) {
            outcome.notStartedProven = true;   // the usual BUSY after the busy timeout: no transaction exists
            return outcome;
        }
        // BEGIN failed yet a transaction is open: not a state this code creates. It was not touched; the connection is isolated.
        outcome.notStartedProven = false;
        if (registered != SQLITE_OK) {
            outcome.connection = ConnectionReport::IsolationFailed;
            try {
                outcome.notes.push_back(makeIsolationError("Could not register the SQLite authorizer", registered, db_));
            } catch (...) {
                outcome.diagnosticsIncomplete = true;
            }
        } else {
            outcome.connection = ConnectionReport::Quarantined;
        }
        return outcome;
    }
    health_.markTransactionOwner(owner_);
    active_ = true;
    return outcome;
}

CommitOutcome SqliteTransactionScope::commit()
{
    CommitOutcome outcome;
    if (!active_) {
        outcome.primary = error("persistence.transaction_failed", "COMMIT without an active transaction");
        return outcome;
    }
    const auto committed = execute(db_, "COMMIT;", "persistence.transaction_failed");
    if (committed.success()) {
        active_ = false;
        health_.clearTransactionOwner();
        outcome.committed = true;
        return outcome;
    }
    // The native codes first (they live in the handle and the cleanup below overwrites them), then the primary error.
    outcome.native = tryCaptureNativeError(db_);
    outcome.primary = *committed.error();
    outcome.cleanup = closeOpenTransaction(db_, health_, fault_, rollbackAttemptsUsed_);
    active_ = !outcome.cleanup.transactionClosed;
    return outcome;
}

CleanupReport SqliteTransactionScope::rollback()
{
    if (!active_) return CleanupReport {};
    CleanupReport report = closeOpenTransaction(db_, health_, fault_, rollbackAttemptsUsed_);
    active_ = !report.transactionClosed;
    return report;
}

SqliteTransactionScope::~SqliteTransactionScope() noexcept
{
    // The destructor spends what is left of the SAME budget (an explicit cleanup already used its attempts: no third round) and,
    // if the transaction is still open, makes sure the connection is isolated - closeOpenTransaction cannot throw, so an exception
    // in flight is never replaced and an open transaction is never released unisolated.
    if (!active_) return;
    static_cast<void>(closeOpenTransaction(db_, health_, fault_, rollbackAttemptsUsed_));
}

} // namespace ai::infrastructure::sqlite_detail
