#include "SqliteCompactionIdentityCodec.hpp"
#include "SqliteCompactionWriteCertainty.hpp"
#include "../SqliteCompactionIntentMigration.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../SqliteTransactionScope.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::CompactionConnectionState;
using application::CompactionDiagnosticStage;
using application::CompactionIntentIdentity;
using application::CompactionIntentMutation;
using application::CompactionIntentMutationOutcome;
using application::CompactionIntentObservation;
using application::CompactionIntentObservationOutcome;
using application::CompactionIntentObservationSource;
using application::CompactionIntentState;
using application::CompactionIntentWriteOutcome;
using application::CompactionPersistOutcome;
using application::CompactionPersistState;
using application::ProjectMemoryCompactionIntent;
using sqlite_detail::BeginOutcome;
using sqlite_detail::ConnectionReport;
using sqlite_detail::SqliteTransactionScope;
using sqlite_detail::Statement;
using sqlite_detail::appendDiagnostic;
using sqlite_detail::bindInt64;
using sqlite_detail::bindText;
using sqlite_detail::columnText;
using sqlite_detail::databaseError;
using sqlite_detail::error;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::prepare;
using sqlite_detail::toMilliseconds;

// The owner tag of this component's statements on the shared connection (a string literal, see ConnectionHealth).
constexpr const char* kTransactionOwner = "project_memory.compaction_intent";

constexpr const char* kIntentColumns =
    "id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,created_at_ms,state,unresolved_marked_at_ms";

std::optional<CompactionConnectionState> toApplication(ConnectionReport report)
{
    switch (report) {
    case ConnectionReport::Healthy: return CompactionConnectionState::Healthy;
    case ConnectionReport::Quarantined: return CompactionConnectionState::Quarantined;
    case ConnectionReport::IsolationFailed: return CompactionConnectionState::IsolationFailed;
    }
    return std::nullopt;
}

// The health of the connection as it is right now (a read does not change it).
CompactionConnectionState currentConnection(const sqlite_detail::ConnectionHealth& health)
{
    if (health.isolationFailed()) return CompactionConnectionState::IsolationFailed;
    if (health.quarantined()) return CompactionConnectionState::Quarantined;
    return CompactionConnectionState::Healthy;
}

// The explicit gate of every READ of this component. The authorizer of a quarantined connection normally refuses the SQL, but when
// its registration failed (IsolationFailed) nothing guarantees that a filter is installed - so health itself forbids the read. It
// is checked under the connection's mutex BEFORE any prepare/step, it neither lifts the quarantine nor touches a transaction, and
// it never reads the handle's last-error state (no SQL ran, so there is no native code to report).
std::optional<common::Error> unusableConnectionForRead(const sqlite_detail::ConnectionHealth& health)
{
    if (!health.quarantined() && !health.isolationFailed()) return std::nullopt;
    std::string message = "The SQLite connection is not usable (";
    message += sqlite_detail::toString(health.reason());
    message += health.isolationFailed() ? "; isolation incomplete" : "";
    message += "); nothing was read";
    return error("persistence.connection_unusable", std::move(message));
}

void addEntryNotes(CompactionPersistOutcome& outcome, const BeginOutcome& entry)
{
    for (const auto& note : entry.notes) appendDiagnostic(outcome.diagnostics, CompactionDiagnosticStage::Rollback, note);
    if (entry.diagnosticsIncomplete) outcome.diagnosticsIncomplete = true;
}

// The write was not started (the entry refused it): nothing of it was issued.
CompactionPersistOutcome refusedOutcome(const BeginOutcome& began)
{
    CompactionPersistOutcome outcome;
    outcome.result = common::Result<void>::failure(began.error.value_or(
        error("persistence.transaction_failed", "The write could not be started")));
    outcome.primaryNative = sqlite_detail::toNativeDetail(began.native);
    outcome.state = began.notStartedProven ? CompactionPersistState::NotCommitted : CompactionPersistState::Unknown;
    outcome.connection = toApplication(began.connection);
    addEntryNotes(outcome, began);
    return outcome;
}

// ---- strict row decoding ------------------------------------------------------------------------------------------------------

common::Result<void> damagedIntent(const char* what)
{
    return common::Result<void>::failure(error("persistence.read_failed", std::string("A stored compaction intent is damaged: ") + what));
}

bool isIntegerColumn(sqlite3_stmt* statement, int column)
{
    return sqlite3_column_type(statement, column) == SQLITE_INTEGER;
}

// Columns 0..8 of kIntentColumns, starting at `first`.
common::Result<ProjectMemoryCompactionIntent> readIntentRow(sqlite3_stmt* statement, int first = 0)
{
    using IntentResult = common::Result<ProjectMemoryCompactionIntent>;
    const auto damaged = [](const char* what) { return IntentResult::failure(*damagedIntent(what).error()); };
    ProjectMemoryCompactionIntent intent;
    intent.id = columnText(statement, first);
    intent.projectId = columnText(statement, first + 1);
    intent.providerId = columnText(statement, first + 2);
    intent.modelId = columnText(statement, first + 3);
    auto sources = sqlite_detail::deserializeCanonicalSegmentIdList(columnText(statement, first + 4));
    if (!sources.success()) return IntentResult::failure(*sources.error());
    intent.sourceSegmentIds = std::move(sources.value());
    intent.sourceContentHash = columnText(statement, first + 5);
    if (!isIntegerColumn(statement, first + 6)) return damaged("created_at_ms is not an integer");
    intent.createdAt = fromMilliseconds(sqlite3_column_int64(statement, first + 6));
    const std::string state = columnText(statement, first + 7);
    const bool hasMark = sqlite3_column_type(statement, first + 8) != SQLITE_NULL;
    if (state == "open") {
        if (hasMark) return damaged("an open intent carries a mark");
        intent.state = CompactionIntentState::Open;
    } else if (state == "unresolved") {
        if (!hasMark || !isIntegerColumn(statement, first + 8)) return damaged("an unresolved intent has no integer mark");
        intent.state = CompactionIntentState::Unresolved;
        intent.unresolvedMarkedAt = fromMilliseconds(sqlite3_column_int64(statement, first + 8));
    } else {
        return damaged("unknown state");
    }
    return IntentResult::ok(std::move(intent));
}

// The identity columns 1..6 of a final record row (the same order), starting at `first` = the id column.
common::Result<CompactionIntentIdentity> readRecordIdentity(sqlite3_stmt* statement, int first)
{
    using IdentityResult = common::Result<CompactionIntentIdentity>;
    CompactionIntentIdentity identity;
    identity.id = columnText(statement, first);
    identity.projectId = columnText(statement, first + 1);
    identity.providerId = columnText(statement, first + 2);
    identity.modelId = columnText(statement, first + 3);
    // The record side is read as the record lookup always reads it (strict, but a harmless spelling of a valid array is accepted).
    auto sources = sqlite_detail::deserializeSegmentIdListStrict(columnText(statement, first + 4));
    if (!sources.success()) return IdentityResult::failure(*sources.error());
    identity.sourceSegmentIds = std::move(sources.value());
    identity.sourceContentHash = columnText(statement, first + 5);
    if (!isIntegerColumn(statement, first + 6)) {
        return IdentityResult::failure(error("persistence.read_failed", "A stored compaction record has a non-integer created_at_ms"));
    }
    identity.createdAt = fromMilliseconds(sqlite3_column_int64(statement, first + 6));
    return IdentityResult::ok(std::move(identity));
}

// One intent by id, strictly. The caller holds the mutex and has checked that no transaction is open.
common::Result<std::optional<ProjectMemoryCompactionIntent>> readIntentById(sqlite3* h, const std::string& id)
{
    using Result = common::Result<std::optional<ProjectMemoryCompactionIntent>>;
    const std::string sql = std::string("SELECT ") + kIntentColumns + " FROM project_memory_compaction_intents WHERE id=?;";
    Statement s;
    const auto prepared = prepare(h, sql.c_str(), s);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(h, s.get(), 1, id);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) return Result::failure(databaseError(h, "persistence.read_failed", "Could not look up a compaction intent"));
    auto intent = readIntentRow(s.get());
    if (!intent.success()) return Result::failure(*intent.error());
    return Result::ok(std::move(intent.value()));
}

common::Result<std::vector<ProjectMemoryCompactionIntent>> readIntentList(
    sqlite3* h, const char* whereAndOrder, const std::string* projectId, std::optional<CompactionIntentState> state)
{
    using ListResult = common::Result<std::vector<ProjectMemoryCompactionIntent>>;
    const std::string sql = std::string("SELECT ") + kIntentColumns + " FROM project_memory_compaction_intents " + whereAndOrder;
    Statement s;
    const auto prepared = prepare(h, sql.c_str(), s);
    if (!prepared.success()) return ListResult::failure(*prepared.error());
    int index = 1;
    if (projectId != nullptr) {
        const auto bound = bindText(h, s.get(), index++, *projectId);
        if (!bound.success()) return ListResult::failure(*bound.error());
    }
    if (state.has_value()) {
        const auto bound = bindText(h, s.get(), index++, application::toString(*state));
        if (!bound.success()) return ListResult::failure(*bound.error());
    }
    std::vector<ProjectMemoryCompactionIntent> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return ListResult::failure(databaseError(h, "persistence.read_failed", "Could not list compaction intents"));
        auto intent = readIntentRow(s.get());
        if (!intent.success()) return ListResult::failure(*intent.error());
        out.push_back(std::move(intent.value()));
    }
    return ListResult::ok(std::move(out));
}

// ---- proofs outside a transaction ---------------------------------------------------------------------------------------------

// NotCommitted only when no transaction is open, the proof read works, and nothing of THIS write is stored. `rowIsForeign`: the
// error said the id is already taken, so a row under the id was not written by this call.
CompactionPersistState proveIntentInsertNotStored(sqlite3* h, sqlite_detail::FaultPlan& fault, const std::string& id, bool rowIsForeign,
    std::vector<application::CompactionDiagnostic>& diagnostics)
{
    if (sqlite3_get_autocommit(h) == 0) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"persistence.proof_unavailable", "A transaction is still open, so the stored intent cannot be proven"});
        return CompactionPersistState::Unknown;
    }
    if (fault.failProofRead) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"persistence.read_failed", "The proof read was not performed (fault injection)"});
        return CompactionPersistState::Unknown;
    }
    const auto found = readIntentById(h, id);
    if (!found.success()) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead, *found.error());
        return CompactionPersistState::Unknown;
    }
    if (found.value().has_value() && !rowIsForeign) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"project_memory.proof_contradiction",
             "The write reported a failure but an intent with its id is stored; the outcome is not proven"});
        return CompactionPersistState::Unknown;
    }
    if (found.value().has_value()) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"project_memory.intent_id_present", "An intent with this id already exists; it was not written by this call"});
    }
    return CompactionPersistState::NotCommitted;
}

// After a FAILED guarded change of an intent: NotCommitted when the row is still exactly as the change expected to find it.
CompactionPersistState proveIntentUnchanged(sqlite3* h, sqlite_detail::FaultPlan& fault, const CompactionIntentIdentity& expected,
    CompactionIntentState expectedState, bool compareState, std::vector<application::CompactionDiagnostic>& diagnostics)
{
    if (sqlite3_get_autocommit(h) == 0) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"persistence.proof_unavailable", "A transaction is still open, so the stored intent cannot be proven"});
        return CompactionPersistState::Unknown;
    }
    if (fault.failProofRead) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"persistence.read_failed", "The proof read was not performed (fault injection)"});
        return CompactionPersistState::Unknown;
    }
    const auto found = readIntentById(h, expected.id);
    if (!found.success()) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead, *found.error());
        return CompactionPersistState::Unknown;
    }
    const bool unchanged = found.value().has_value()
        && !application::compactionIntentIdentityDifference(expected, application::compactionIntentIdentityOf(*found.value())).has_value()
        && (!compareState || found.value()->state == expectedState);
    if (!unchanged) {
        appendDiagnostic(diagnostics, CompactionDiagnosticStage::ProofRead,
            {"project_memory.proof_inconclusive", "The stored intent is not as it was before the failed change; the outcome is not proven"});
        return CompactionPersistState::Unknown;
    }
    return CompactionPersistState::NotCommitted;
}

CompactionPersistOutcome failedWrite(sqlite3* h, common::Error primary, std::optional<sqlite_detail::NativeError> native,
    const BeginOutcome& entry, CompactionPersistState state, std::vector<application::CompactionDiagnostic> proofDiagnostics)
{
    CompactionPersistOutcome outcome;
    outcome.result = common::Result<void>::failure(std::move(primary));
    outcome.primaryNative = sqlite_detail::toNativeDetail(native);
    outcome.connection = CompactionConnectionState::Healthy;
    addEntryNotes(outcome, entry);
    for (auto& finding : proofDiagnostics) appendDiagnostic(outcome.diagnostics, finding.stage, std::move(finding.error));
    outcome.state = state;
    static_cast<void>(h);
    return outcome;
}

void bindIdentityPredicate(sqlite3* h, sqlite3_stmt* statement, int first, const CompactionIntentIdentity& expected,
    const std::string& sourceIdsText, std::vector<common::Result<void>>& binds)
{
    binds.push_back(bindText(h, statement, first, expected.id));
    binds.push_back(bindText(h, statement, first + 1, expected.projectId));
    binds.push_back(bindText(h, statement, first + 2, expected.providerId));
    binds.push_back(bindText(h, statement, first + 3, expected.modelId));
    binds.push_back(bindText(h, statement, first + 4, sourceIdsText));
    binds.push_back(bindText(h, statement, first + 5, expected.sourceContentHash));
    binds.push_back(bindInt64(h, statement, first + 6, toMilliseconds(expected.createdAt)));
}

constexpr const char* kIdentityPredicate =
    "id=? AND project_id=? AND provider_id=? AND model_id=? AND source_segment_ids=? AND source_content_hash=? AND created_at_ms=?";

// The shared body of mark/close: one guarded autocommit statement, then - for a zero-row result - the read that explains it. The
// caller holds the connection's mutex.
CompactionIntentMutationOutcome guardedMutation(sqlite3* h, sqlite_detail::ConnectionHealth& health, sqlite_detail::FaultPlan& fault,
    const CompactionIntentIdentity& expected, bool isMark, std::chrono::system_clock::time_point markedAt)
{
    CompactionIntentMutationOutcome result;
    std::string sourceIdsText;
    try {
        sourceIdsText = sqlite_detail::serializeSegmentIdList(expected.sourceSegmentIds);
    } catch (const std::exception&) {
        result.persist.result = common::Result<void>::failure(error("persistence.write_failed",
            "The source segment ids of the expected intent cannot be put into their canonical form"));
        result.persist.state = CompactionPersistState::NotCommitted;   // refused before the connection is touched
        return result;
    }

    const BeginOutcome admitted = SqliteTransactionScope::admit(h, health, fault, kTransactionOwner);
    if (!admitted.started) {
        result.persist = refusedOutcome(admitted);
        return result;
    }

    // -1: the statement did not run to completion (the primary error is in result.persist); otherwise the number of changed rows.
    std::int64_t changes = -1;
    {
        Statement statement;
        const char* markSql =
            "UPDATE project_memory_compaction_intents SET state='unresolved', unresolved_marked_at_ms=? WHERE state='open' AND "
            "id=? AND project_id=? AND provider_id=? AND model_id=? AND source_segment_ids=? AND source_content_hash=? AND created_at_ms=?;";
        const std::string closeSql = std::string("DELETE FROM project_memory_compaction_intents WHERE ") + kIdentityPredicate + ";";
        const auto prepared = prepare(h, isMark ? markSql : closeSql.c_str(), statement);
        std::optional<common::Error> failure;
        if (!prepared.success()) {
            failure = *prepared.error();
        } else {
            std::vector<common::Result<void>> binds;
            int first = 1;
            if (isMark) {
                binds.push_back(bindInt64(h, statement.get(), 1, toMilliseconds(markedAt)));
                first = 2;
            }
            bindIdentityPredicate(h, statement.get(), first, expected, sourceIdsText, binds);
            for (const auto& bound : binds) {
                if (!failure.has_value() && !bound.success()) failure = *bound.error();
            }
        }
        if (!failure.has_value()) {
            if (sqlite3_step(statement.get()) == SQLITE_DONE) {
                changes = sqlite3_changes(h);
            } else {
                failure = databaseError(h, "persistence.write_failed",
                    isMark ? "Could not mark a compaction intent unresolved" : "Could not close a compaction intent");
            }
        }
        if (failure.has_value()) {
            result.persist.primaryNative = sqlite_detail::toNativeDetail(sqlite_detail::tryCaptureNativeError(h));
            result.persist.result = common::Result<void>::failure(std::move(*failure));
        }
    }

    result.persist.connection = CompactionConnectionState::Healthy;
    addEntryNotes(result.persist, admitted);
    if (changes < 0) {
        std::vector<application::CompactionDiagnostic> proof;
        result.persist.state = proveIntentUnchanged(h, fault, expected, CompactionIntentState::Open, isMark, proof);
        for (auto& finding : proof) appendDiagnostic(result.persist.diagnostics, finding.stage, std::move(finding.error));
        result.mutation = CompactionIntentMutation::Unconfirmed;
        return result;
    }
    if (changes == 1) {
        result.mutation = CompactionIntentMutation::Applied;
        result.persist.state = CompactionPersistState::Committed;
        return result;
    }

    // Zero rows changed: nothing was written by this call. A read outside any transaction says WHY - never an assumption.
    result.persist.state = CompactionPersistState::NotCommitted;
    if (changes > 1 || sqlite3_get_autocommit(h) == 0) {
        result.mutation = CompactionIntentMutation::Unconfirmed;
        result.persist.result = common::Result<void>::failure(error("persistence.write_failed",
            "A guarded intent change reported an impossible state"));
        return result;
    }
    if (fault.failProofRead) {
        result.mutation = CompactionIntentMutation::Unconfirmed;
        result.persist.result = common::Result<void>::failure(
            error("persistence.read_failed", "The read that explains a zero-row change was not performed (fault injection)"));
        return result;
    }
    const auto found = readIntentById(h, expected.id);
    if (!found.success()) {
        result.mutation = CompactionIntentMutation::Unconfirmed;
        result.persist.result = common::Result<void>::failure(*found.error());
        return result;
    }
    if (!found.value().has_value()) {
        result.mutation = CompactionIntentMutation::NotFound;
        return result;
    }
    const ProjectMemoryCompactionIntent& stored = *found.value();
    const bool sameIdentity =
        !application::compactionIntentIdentityDifference(expected, application::compactionIntentIdentityOf(stored)).has_value();
    if (isMark && sameIdentity && stored.state == CompactionIntentState::Unresolved) {
        result.mutation = CompactionIntentMutation::AlreadyApplied;   // markedAt of the stored row is NOT rewritten
        return result;
    }
    if (sameIdentity) {   // it matches and still the statement changed nothing: not explainable, so not claimed
        result.mutation = CompactionIntentMutation::Unconfirmed;
        result.persist.result = common::Result<void>::failure(error(application::kIntentObservationInconsistentCode,
            "A guarded intent change found a matching row but changed nothing"));
        return result;
    }
    result.mutation = CompactionIntentMutation::IdentityConflict;
    result.persist.result = common::Result<void>::failure(error(application::kIntentIdentityConflictCode,
        "The stored compaction intent does not match the expected operation; nothing was changed"));
    return result;
}

} // namespace

SqliteProjectMemoryCompactionIntentRepository::SqliteProjectMemoryCompactionIntentRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

CompactionIntentWriteOutcome SqliteProjectMemoryCompactionIntentRepository::recordIntentObserved(const ProjectMemoryCompactionIntent& intent)
{
    CompactionIntentWriteOutcome outcome;
    if (const auto valid = application::validateNewCompactionIntent(intent); !valid.success()) {
        outcome.persist.result = common::Result<void>::failure(*valid.error());
        outcome.persist.state = CompactionPersistState::NotCommitted;   // refused before the connection is touched
        return outcome;
    }
    std::string sourceIdsText;
    try {
        sourceIdsText = sqlite_detail::serializeSegmentIdList(intent.sourceSegmentIds);
    } catch (const std::exception&) {
        outcome.persist.result = common::Result<void>::failure(error("persistence.write_failed",
            "The source segment ids of the compaction intent cannot be put into their canonical form"));
        outcome.persist.state = CompactionPersistState::NotCommitted;
        return outcome;
    }

    std::lock_guard lock(database_->impl_->mutex);
    auto& impl = *database_->impl_;
    sqlite3* h = impl.handle;
    const BeginOutcome admitted = SqliteTransactionScope::admit(h, impl.health, impl.fault, kTransactionOwner);
    if (!admitted.started) {
        outcome.persist = refusedOutcome(admitted);
        return outcome;
    }

    common::Error primary {};
    std::optional<sqlite_detail::NativeError> native;
    bool duplicate = false;
    bool failed = false;
    {
        Statement statement;
        const auto prepared = prepare(h,
            "INSERT INTO project_memory_compaction_intents (id,project_id,provider_id,model_id,source_segment_ids,"
            "source_content_hash,created_at_ms,state,unresolved_marked_at_ms) VALUES (?,?,?,?,?,?,?,'open',NULL);",
            statement);
        if (!prepared.success()) {
            failed = true;
            primary = *prepared.error();
            native = sqlite_detail::tryCaptureNativeError(h);
        } else {
            const common::Result<void> binds[] = {
                bindText(h, statement.get(), 1, intent.id),
                bindText(h, statement.get(), 2, intent.projectId),
                bindText(h, statement.get(), 3, intent.providerId),
                bindText(h, statement.get(), 4, intent.modelId),
                bindText(h, statement.get(), 5, sourceIdsText),
                bindText(h, statement.get(), 6, intent.sourceContentHash),
                bindInt64(h, statement.get(), 7, toMilliseconds(intent.createdAt))};
            for (const auto& bound : binds) {
                if (!failed && !bound.success()) {
                    failed = true;
                    primary = *bound.error();
                }
            }
            if (!failed && sqlite3_step(statement.get()) != SQLITE_DONE) {
                failed = true;
                const int code = sqlite3_extended_errcode(h);
                const bool occupied = sqlite_detail::isCompactionIntentTriggerAbort(
                    code, sqlite3_errmsg(h), sqlite_detail::kCompactionIntentIdOccupiedToken);
                native = sqlite_detail::tryCaptureNativeError(h);
                if (code == SQLITE_CONSTRAINT_PRIMARYKEY || occupied) {
                    duplicate = true;
                    primary = error(application::kIntentDuplicateIdCode, application::kIntentDuplicateIdMessage);
                } else if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
                    primary = error("project_memory.project_not_found", "Project does not exist");
                } else {
                    primary = databaseError(h, "persistence.write_failed", "Could not insert a compaction intent");
                }
            } else if (failed) {
                native = sqlite_detail::tryCaptureNativeError(h);
            }
        }
    }

    if (!failed) {
        outcome.persist.state = CompactionPersistState::Committed;
        outcome.persist.connection = CompactionConnectionState::Healthy;
        outcome.newIntentInserted = true;
        addEntryNotes(outcome.persist, admitted);
        return outcome;
    }
    std::vector<application::CompactionDiagnostic> proof;
    const CompactionPersistState state = proveIntentInsertNotStored(h, impl.fault, intent.id, duplicate, proof);
    outcome.persist = failedWrite(h, std::move(primary), std::move(native), admitted, state, std::move(proof));
    return outcome;
}

common::Result<std::optional<ProjectMemoryCompactionIntent>> SqliteProjectMemoryCompactionIntentRepository::findIntent(
    const application::ProjectMemoryCompactionId& id) const
{
    using LookupResult = common::Result<std::optional<ProjectMemoryCompactionIntent>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    if (const auto refused = unusableConnectionForRead(database_->impl_->health)) return LookupResult::failure(*refused);
    // A transaction open on the connection would show its own uncommitted rows: that is not what is stored.
    if (sqlite3_get_autocommit(h) == 0) {
        return LookupResult::failure(error("persistence.lookup_unavailable",
            "A transaction is open on the connection, so the stored intent cannot be looked up"));
    }
    return readIntentById(h, id);
}

common::Result<std::vector<ProjectMemoryCompactionIntent>> SqliteProjectMemoryCompactionIntentRepository::listIntents(
    const application::ProjectId& projectId, std::optional<CompactionIntentState> state) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    if (const auto refused = unusableConnectionForRead(database_->impl_->health)) {
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(*refused);
    }
    if (sqlite3_get_autocommit(h) == 0) {
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(error("persistence.lookup_unavailable",
            "A transaction is open on the connection, so the stored intents cannot be listed"));
    }
    return readIntentList(h,
        state.has_value() ? "WHERE project_id=? AND state=? ORDER BY created_at_ms,id;" : "WHERE project_id=? ORDER BY created_at_ms,id;",
        &projectId, state);
}

common::Result<std::vector<ProjectMemoryCompactionIntent>> SqliteProjectMemoryCompactionIntentRepository::listIntentsByState(
    CompactionIntentState state) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    if (const auto refused = unusableConnectionForRead(database_->impl_->health)) {
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(*refused);
    }
    if (sqlite3_get_autocommit(h) == 0) {
        return common::Result<std::vector<ProjectMemoryCompactionIntent>>::failure(error("persistence.lookup_unavailable",
            "A transaction is open on the connection, so the stored intents cannot be listed"));
    }
    return readIntentList(h, "WHERE state=? ORDER BY created_at_ms,id;", nullptr, state);
}

CompactionIntentMutationOutcome SqliteProjectMemoryCompactionIntentRepository::markUnresolvedObserved(
    const CompactionIntentIdentity& expected, std::chrono::system_clock::time_point markedAt)
{
    std::lock_guard lock(database_->impl_->mutex);
    auto& impl = *database_->impl_;
    return guardedMutation(impl.handle, impl.health, impl.fault, expected, true, markedAt);
}

CompactionIntentMutationOutcome SqliteProjectMemoryCompactionIntentRepository::closeIntentObserved(const CompactionIntentIdentity& expected)
{
    std::lock_guard lock(database_->impl_->mutex);
    auto& impl = *database_->impl_;
    return guardedMutation(impl.handle, impl.health, impl.fault, expected, false, {});
}

CompactionIntentObservationOutcome SqliteProjectMemoryCompactionIntentRepository::observeOperation(const CompactionIntentIdentity& expected) const
{
    CompactionIntentObservationOutcome outcome;
    std::lock_guard lock(database_->impl_->mutex);
    auto& impl = *database_->impl_;
    sqlite3* h = impl.handle;
    outcome.connection = currentConnection(impl.health);

    const auto unreadable = [&](common::Error cause) {
        outcome.observation = CompactionIntentObservation::Unreadable;
        outcome.source = CompactionIntentObservationSource::None;
        outcome.intent.reset();
        outcome.recordIdentity.reset();
        outcome.primaryNative = sqlite_detail::toNativeDetail(sqlite_detail::tryCaptureNativeError(h));
        outcome.error = cause;
        appendDiagnostic(outcome.diagnostics, CompactionDiagnosticStage::ProofRead, std::move(cause));
        outcome.connection = currentConnection(impl.health);
        return outcome;
    };

    // An unusable connection proves nothing, whether or not an SQL filter is installed on it: no SQL runs, no row is reported, the
    // connection state stays what it is, and no native code is attributed to a statement that never ran.
    if (const auto refused = unusableConnectionForRead(impl.health)) {
        outcome.observation = CompactionIntentObservation::Unreadable;
        outcome.source = CompactionIntentObservationSource::None;
        outcome.error = *refused;
        appendDiagnostic(outcome.diagnostics, CompactionDiagnosticStage::ProofRead, *refused);
        return outcome;
    }
    // The proof reads what is STORED: never inside a transaction (it would show its own uncommitted rows).
    if (sqlite3_get_autocommit(h) == 0) {
        return unreadable(error("persistence.proof_unavailable", "A transaction is open on the connection, so the operation cannot be observed"));
    }
    if (impl.fault.failProofRead) {
        return unreadable(error("persistence.read_failed", "The observation was not performed (fault injection)"));
    }

    // ONE statement: both rows come from one consistent snapshot.
    Statement s;
    const auto prepared = prepare(h,
        "SELECT 'r',id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,created_at_ms,NULL,NULL "
        "FROM project_memory_compaction_records WHERE id=?1 "
        "UNION ALL "
        "SELECT 'i',id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,created_at_ms,state,unresolved_marked_at_ms "
        "FROM project_memory_compaction_intents WHERE id=?1;",
        s);
    if (!prepared.success()) return unreadable(*prepared.error());
    const auto bound = bindText(h, s.get(), 1, expected.id);
    if (!bound.success()) return unreadable(*bound.error());

    std::optional<CompactionIntentIdentity> record;
    std::optional<ProjectMemoryCompactionIntent> intent;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return unreadable(databaseError(h, "persistence.read_failed", "Could not observe a compaction operation"));
        const std::string kind = columnText(s.get(), 0);
        if (kind == "r") {
            auto identity = readRecordIdentity(s.get(), 1);
            if (!identity.success()) return unreadable(*identity.error());
            record = std::move(identity.value());
        } else {
            auto row = readIntentRow(s.get(), 1);
            if (!row.success()) return unreadable(*row.error());
            intent = std::move(row.value());
        }
    }

    outcome.observation = application::classifyCompactionOperation(expected, intent, record);
    outcome.source = CompactionIntentObservationSource::AtomicPairRead;
    outcome.intent = std::move(intent);
    outcome.recordIdentity = std::move(record);
    if (outcome.observation == CompactionIntentObservation::Inconsistent) {
        outcome.error = error(application::kIntentObservationInconsistentCode,
            "The stored operation contradicts the expected identity or is stored both as an intent and as a record");
    }
    return outcome;
}

} // namespace ai::infrastructure
