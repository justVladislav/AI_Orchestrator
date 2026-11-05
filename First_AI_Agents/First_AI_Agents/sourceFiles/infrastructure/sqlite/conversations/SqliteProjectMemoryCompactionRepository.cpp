#include "SqliteCompactionWriteCertainty.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../SqliteTransactionScope.hpp"

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ProjectMemoryCompactionRecord;
using application::ProjectMemoryCompactionStatus;
using application::ProjectMemorySegment;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;

} // namespace

namespace {

std::string serializeSegmentIdList(const std::vector<application::ProjectMemorySegmentId>& ids)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& id : ids) array.push_back(id);
    return array.dump();
}

// How a stored source list is read.
//   Lenient  the pinned behaviour of the history listing (see testCorruptCompactionRowsAreHandledAsPinned): an unreadable list reads
//            as empty and non-string entries are skipped. NOT a design goal; it is kept only so that the listing is unchanged.
//   Strict   the identity lookup: a list that is not exactly a JSON array of strings is a damaged row and is refused - the caller must
//            never compare against, or accept as a repeat, a record whose provenance was silently repaired.
enum class SourceListReading { Lenient, Strict };

std::vector<application::ProjectMemorySegmentId> deserializeSegmentIdList(const std::string& json)
{
    std::vector<application::ProjectMemorySegmentId> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

common::Result<std::vector<application::ProjectMemorySegmentId>> deserializeSegmentIdListStrict(const std::string& json)
{
    using ListResult = common::Result<std::vector<application::ProjectMemorySegmentId>>;
    const auto damaged = [] {
        return ListResult::failure({"persistence.read_failed",
            "The stored source segment ids of a project memory compaction record are not a JSON array of strings"});
    };
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return damaged();
    std::vector<application::ProjectMemorySegmentId> result;
    result.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) return damaged();
        result.push_back(item.get<std::string>());
    }
    return ListResult::ok(std::move(result));
}

std::string projectMemoryCompactionStatusToString(ProjectMemoryCompactionStatus status)
{
    return application::toString(status);
}

std::optional<ProjectMemoryCompactionStatus> projectMemoryCompactionStatusFromString(
    std::string_view value)
{
    if (value == "completed") return ProjectMemoryCompactionStatus::Completed;
    if (value == "failed") return ProjectMemoryCompactionStatus::Failed;
    if (value == "cancelled") return ProjectMemoryCompactionStatus::Cancelled;
    return std::nullopt;
}

constexpr const char* projectMemoryCompactionRecordColumns =
    "id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,"
    "result_segment_id,input_tokens,output_tokens,cost_micros,status,error_code,"
    "error_message,created_at_ms,cost_price_known";

// cost_price_known is NULL (no evidence of completeness), 0 (incomplete price) or 1 (complete price). Persistence-private:
// the repository port keeps speaking std::optional<bool>.
common::Result<void> bindNullableFlag(sqlite3* h, sqlite3_stmt* statement, int index, const std::optional<bool>& value)
{
    const int code = value.has_value() ? sqlite3_bind_int64(statement, index, *value ? 1 : 0)
                                       : sqlite3_bind_null(statement, index);
    if (code != SQLITE_OK) {
        return common::Result<void>::failure(databaseError(h, "persistence.write_failed", "Could not bind a nullable flag"));
    }
    return common::Result<void>::ok();
}

// `native` (optional) receives the native codes of a failed PREPARE or STEP, captured before anything else touches the handle.
common::Result<void> insertProjectMemoryCompactionRecord(
    sqlite3* h, const ProjectMemoryCompactionRecord& record, std::optional<sqlite_detail::NativeError>* native = nullptr)
{
    Statement statement;
    const auto prepared = prepare(h,
        "INSERT INTO project_memory_compaction_records (id,project_id,provider_id,model_id,"
        "source_segment_ids,source_content_hash,result_segment_id,input_tokens,output_tokens,"
        "cost_micros,status,error_code,error_message,created_at_ms,cost_price_known) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) {
        if (native != nullptr) *native = sqlite_detail::tryCaptureNativeError(h);
        return common::Result<void>::failure(*prepared.error());
    }
    const common::Result<void> binds[] = {
        bindText(h, statement.get(), 1, record.id),
        bindText(h, statement.get(), 2, record.projectId),
        bindText(h, statement.get(), 3, record.providerId),
        bindText(h, statement.get(), 4, record.modelId),
        bindText(h, statement.get(), 5, serializeSegmentIdList(record.sourceSegmentIds)),
        bindText(h, statement.get(), 6, record.sourceContentHash),
        bindNullableText(h, statement.get(), 7, record.resultSegmentId),
        bindUnsigned(h, statement.get(), 8, record.inputTokens),
        bindUnsigned(h, statement.get(), 9, record.outputTokens),
        bindUnsigned(h, statement.get(), 10, record.costMicros),
        bindText(h, statement.get(), 11, projectMemoryCompactionStatusToString(record.status)),
        bindText(h, statement.get(), 12, record.errorCode),
        bindText(h, statement.get(), 13, record.errorMessage),
        bindInt64(h, statement.get(), 14, toMilliseconds(record.createdAt)),
        bindNullableFlag(h, statement.get(), 15, record.costPriceKnown)};
    for (const auto& bound : binds) {
        if (!bound.success()) return common::Result<void>::failure(*bound.error());
    }
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return common::Result<void>::ok();

    const int code = sqlite3_extended_errcode(h);
    if (native != nullptr) *native = sqlite_detail::tryCaptureNativeError(h);
    if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
        return common::Result<void>::failure(
            error("project_memory.duplicate_id", "Compaction record id already exists"));
    }
    if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return common::Result<void>::failure(
            error("project_memory.project_not_found", "Project does not exist"));
    }
    return common::Result<void>::failure(databaseError(
        h, "persistence.write_failed", "Could not insert project memory compaction record"));
}

common::Result<ProjectMemoryCompactionRecord> readProjectMemoryCompactionRecord(
    sqlite3_stmt* statement, SourceListReading sourceList)
{
    ProjectMemoryCompactionRecord record;
    record.id = columnText(statement, 0);
    record.projectId = columnText(statement, 1);
    record.providerId = columnText(statement, 2);
    record.modelId = columnText(statement, 3);
    if (sourceList == SourceListReading::Strict) {
        auto sources = deserializeSegmentIdListStrict(columnText(statement, 4));
        if (!sources.success()) return common::Result<ProjectMemoryCompactionRecord>::failure(*sources.error());
        record.sourceSegmentIds = std::move(sources.value());
    } else {
        record.sourceSegmentIds = deserializeSegmentIdList(columnText(statement, 4));
    }
    record.sourceContentHash = columnText(statement, 5);
    record.resultSegmentId = optionalText(statement, 6);
    record.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 7));
    record.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 8));
    record.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 9));
    const auto status = projectMemoryCompactionStatusFromString(columnText(statement, 10));
    if (!status.has_value()) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(
            {"persistence.read_failed", "Unknown project memory compaction status"});
    }
    record.status = *status;
    record.errorCode = columnText(statement, 11);
    record.errorMessage = columnText(statement, 12);
    record.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 13));
    // Only SQL NULL, INTEGER 0 and INTEGER 1 mean something. Any other value (a stray text, a real, 2) is a damaged row:
    // it is refused instead of being coerced into a price state.
    const int flagType = sqlite3_column_type(statement, 14);
    if (flagType == SQLITE_INTEGER) {
        const std::int64_t flag = sqlite3_column_int64(statement, 14);
        if (flag != 0 && flag != 1) {
            return common::Result<ProjectMemoryCompactionRecord>::failure(
                {"persistence.read_failed", "Invalid project memory compaction price flag"});
        }
        record.costPriceKnown = (flag == 1);
    } else if (flagType != SQLITE_NULL) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(
            {"persistence.read_failed", "Invalid project memory compaction price flag"});
    }
    return common::Result<ProjectMemoryCompactionRecord>::ok(std::move(record));
}

// ---- S2b-1: the write steps, with the proof of what they left behind -----------------------------------------------------------------

// The owner tag of this component's transactions on the shared connection (a string literal, see ConnectionHealth).
constexpr const char* kTransactionOwner = "project_memory.compaction";

using application::CompactionConnectionState;
using application::CompactionDiagnosticStage;
using application::CompactionPersistOutcome;
using application::CompactionPersistState;
using sqlite_detail::appendDiagnostic;
using sqlite_detail::BeginOutcome;
using sqlite_detail::CleanupReport;
using sqlite_detail::ConnectionReport;
using sqlite_detail::SqliteTransactionScope;

struct StepFailure {
    common::Error error;               // the PRIMARY error of the write
    bool recordIdCollision {false};    // "this record id already exists": that row is not ours
    bool segmentIdCollision {false};
    // The native codes of the failed SQL step, captured where it failed - before the rollback and the proof read overwrite them.
    std::optional<sqlite_detail::NativeError> native;
};

// A failure of a SQL step of `h`: its native codes are captured NOW.
StepFailure sqlStepFailure(sqlite3* h, common::Error cause, bool recordIdCollision = false, bool segmentIdCollision = false)
{
    return StepFailure {std::move(cause), recordIdCollision, segmentIdCollision, sqlite_detail::tryCaptureNativeError(h)};
}

std::optional<CompactionConnectionState> toApplication(ConnectionReport report)
{
    switch (report) {
    case ConnectionReport::Healthy: return CompactionConnectionState::Healthy;
    case ConnectionReport::Quarantined: return CompactionConnectionState::Quarantined;
    case ConnectionReport::IsolationFailed: return CompactionConnectionState::IsolationFailed;
    }
    return std::nullopt;
}

// Cleanup causes, each one separately; they never replace the primary error.
void addCleanupDiagnostics(CompactionPersistOutcome& outcome, const CleanupReport& cleanup)
{
    for (const auto& failure : cleanup.rollbackErrors) {
        appendDiagnostic(outcome.diagnostics, CompactionDiagnosticStage::Rollback, failure);
    }
    if (cleanup.isolationError.has_value()) {
        appendDiagnostic(outcome.diagnostics, CompactionDiagnosticStage::Rollback, *cleanup.isolationError);
    }
}

// Secondary causes of a lazy recovery that happened at the ENTRY of a write (a transaction left open by an earlier failed cleanup of
// this owner was closed, or not). They belong to the outcome of the write that triggered it, whatever that write's own fate is.
void addEntryNotes(CompactionPersistOutcome& outcome, const BeginOutcome& entry)
{
    for (const auto& note : entry.notes) appendDiagnostic(outcome.diagnostics, CompactionDiagnosticStage::Rollback, note);
    if (entry.diagnosticsIncomplete) outcome.diagnosticsIncomplete = true;
}

// The write was not started (the entry refused it) or its start is not proven: nothing of it was issued.
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

// The write failed: the primary error stays the result; what is stored decides the state. `entry` (optional) is the entry of this
// same write - its lazy-recovery causes come first, in the order they happened.
CompactionPersistOutcome failedOutcome(sqlite3* h, sqlite_detail::FaultPlan& fault, StepFailure failure, const CleanupReport& cleanup,
    const std::string& recordId, const std::optional<std::string>& resultSegmentId, const BeginOutcome* entry = nullptr)
{
    CompactionPersistOutcome outcome;
    outcome.result = common::Result<void>::failure(std::move(failure.error));
    outcome.primaryNative = sqlite_detail::toNativeDetail(failure.native);
    outcome.connection = toApplication(cleanup.connection);
    if (entry != nullptr) addEntryNotes(outcome, *entry);
    addCleanupDiagnostics(outcome, cleanup);
    if (cleanup.diagnosticsIncomplete) outcome.diagnosticsIncomplete = true;
    sqlite_detail::WriteProofRequest request;
    request.recordId = recordId;
    request.resultSegmentId = resultSegmentId;
    request.recordIdCollision = failure.recordIdCollision;
    request.segmentIdCollision = failure.segmentIdCollision;
    sqlite_detail::WriteProof proof = sqlite_detail::proveWriteNotStored(h, fault, request);
    outcome.state = proof.state;
    for (auto& finding : proof.diagnostics) appendDiagnostic(outcome.diagnostics, finding.stage, std::move(finding.error));
    return outcome;
}

// The statements of the successful commit. EVERY statement lives and dies inside this function (and the helpers it calls), so
// nothing is executing when the caller commits or rolls back - an authorizer would not stop an executing statement.
std::optional<StepFailure> runCommitSteps(sqlite3* h, const ProjectMemoryCompactionRecord& record, const ProjectMemorySegment& resultSegment)
{
    // Requiring exactly one deleted row per source id is the optimistic
    // concurrency check. Another compaction or clear cannot make a stale
    // provider response overwrite the newer memory state.
    for (const auto& id : record.sourceSegmentIds) {
        Statement removed;
        const auto prepared = prepare(
            h, "DELETE FROM project_memory_segments WHERE id=? AND project_id=?;", removed);
        if (!prepared.success()) return sqlStepFailure(h, *prepared.error());
        const common::Result<void> binds[] = {
            bindText(h, removed.get(), 1, id), bindText(h, removed.get(), 2, record.projectId)};
        for (const auto& bound : binds) {
            if (!bound.success()) return StepFailure {*bound.error()};
        }
        if (sqlite3_step(removed.get()) != SQLITE_DONE) {
            return sqlStepFailure(h, databaseError(h, "persistence.write_failed", "Could not remove a source memory segment"));
        }
        if (sqlite3_changes(h) != 1) {
            return StepFailure {error(
                "project_memory.compaction_conflict",
                "Project memory changed while compaction was running")};
        }
    }

    {
        Statement inserted;
        const auto insertPrepared = prepare(h,
            "INSERT INTO project_memory_segments (id,project_id,run_id,"
            "summary,source_token_count,summary_token_count,created_at_ms) VALUES (?,?,?,?,?,?,?);",
            inserted);
        if (!insertPrepared.success()) return sqlStepFailure(h, *insertPrepared.error());
        const common::Result<void> insertBinds[] = {
            bindText(h, inserted.get(), 1, resultSegment.id),
            bindText(h, inserted.get(), 2, resultSegment.projectId),
            bindText(h, inserted.get(), 3, resultSegment.runId),
            bindText(h, inserted.get(), 4, resultSegment.summary),
            bindUnsigned(h, inserted.get(), 5, resultSegment.sourceTokenCount),
            bindUnsigned(h, inserted.get(), 6, resultSegment.summaryTokenCount),
            bindInt64(h, inserted.get(), 7, toMilliseconds(resultSegment.createdAt))};
        for (const auto& bound : insertBinds) {
            if (!bound.success()) return StepFailure {*bound.error()};
        }
        if (sqlite3_step(inserted.get()) != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(h);
            if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return sqlStepFailure(h, error("project_memory.duplicate_id", "Memory segment id already exists"), false, true);
            }
            if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
                return sqlStepFailure(h, error("project_memory.project_not_found", "Project does not exist"));
            }
            return sqlStepFailure(h, databaseError(h, "persistence.write_failed", "Could not insert compacted memory segment"));
        }
    }

    std::optional<sqlite_detail::NativeError> recordNative;
    const auto recorded = insertProjectMemoryCompactionRecord(h, record, &recordNative);
    if (!recorded.success()) {
        const bool collision = recorded.error()->code == "project_memory.duplicate_id";
        return StepFailure {*recorded.error(), collision, false, std::move(recordNative)};
    }
    return std::nullopt;
}

} // namespace

SqliteProjectMemoryCompactionRepository::SqliteProjectMemoryCompactionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

// The old methods are a thin view of the observed ones: the same single call, only the result.
common::Result<void> SqliteProjectMemoryCompactionRepository::insertCompactionRecord(
    const ProjectMemoryCompactionRecord& record)
{
    return insertCompactionRecordObserved(record).result;
}

common::Result<void> SqliteProjectMemoryCompactionRepository::commitSuccessfulCompaction(
    const ProjectMemoryCompactionRecord& record,
    ProjectMemorySegment resultSegment)
{
    return commitSuccessfulCompactionObserved(record, std::move(resultSegment)).result;
}

application::CompactionPersistOutcome SqliteProjectMemoryCompactionRepository::insertCompactionRecordObserved(
    const ProjectMemoryCompactionRecord& record)
{
    CompactionPersistOutcome outcome;
    if (record.status == ProjectMemoryCompactionStatus::Completed) {
        // Refused before the connection is touched: nothing was written (the connection was not observed).
        outcome.result = common::Result<void>::failure(error(
            "project_memory.invalid_compaction_commit",
            "Completed compaction records require an atomic memory commit"));
        outcome.state = CompactionPersistState::NotCommitted;
        return outcome;
    }
    std::lock_guard lock(database_->impl_->mutex);
    auto& impl = *database_->impl_;
    sqlite3* h = impl.handle;

    // A single autocommit INSERT opens no transaction of its own, but it must not run inside a transaction that is open on the
    // shared connection or on a connection that is isolated.
    const BeginOutcome admitted = SqliteTransactionScope::admit(h, impl.health, impl.fault, kTransactionOwner);
    if (!admitted.started) return refusedOutcome(admitted);

    std::optional<sqlite_detail::NativeError> native;
    const auto inserted = insertProjectMemoryCompactionRecord(h, record, &native);
    if (inserted.success()) {
        outcome.state = CompactionPersistState::Committed;
        outcome.connection = CompactionConnectionState::Healthy;
        addEntryNotes(outcome, admitted);   // a lazy recovery at the entry is not silent just because the new write worked
        return outcome;
    }
    StepFailure failure {*inserted.error()};
    failure.recordIdCollision = inserted.error()->code == "project_memory.duplicate_id";
    failure.native = std::move(native);
    return failedOutcome(h, impl.fault, std::move(failure), CleanupReport {}, record.id, std::nullopt, &admitted);
}

application::CompactionPersistOutcome SqliteProjectMemoryCompactionRepository::commitSuccessfulCompactionObserved(
    const ProjectMemoryCompactionRecord& record,
    ProjectMemorySegment resultSegment)
{
    CompactionPersistOutcome outcome;
    if (record.status != ProjectMemoryCompactionStatus::Completed
        || !record.resultSegmentId.has_value() || *record.resultSegmentId != resultSegment.id
        || record.projectId != resultSegment.projectId || record.sourceSegmentIds.empty()) {
        outcome.result = common::Result<void>::failure(error(
            "project_memory.invalid_compaction_commit",
            "Successful compaction commit fields are inconsistent"));
        outcome.state = CompactionPersistState::NotCommitted;   // refused before the connection is touched
        return outcome;
    }

    std::lock_guard lock(database_->impl_->mutex);
    auto& impl = *database_->impl_;
    sqlite3* h = impl.handle;

    // The scope's destructor closes the transaction on every path out of here, including an exception.
    SqliteTransactionScope scope(h, impl.health, impl.fault, kTransactionOwner);
    const BeginOutcome began = scope.begin();
    if (!began.started) return refusedOutcome(began);
    if (impl.fault.throwAfterBegin) throw std::runtime_error("fault injection: exception after BEGIN");

    if (std::optional<StepFailure> failure = runCommitSteps(h, record, resultSegment)) {
        const CleanupReport cleanup = scope.rollback();
        return failedOutcome(h, impl.fault, std::move(*failure), cleanup, record.id, record.resultSegmentId, &began);
    }

    sqlite_detail::CommitOutcome committed = scope.commit();
    if (committed.committed) {
        outcome.state = CompactionPersistState::Committed;
        outcome.connection = CompactionConnectionState::Healthy;
        addEntryNotes(outcome, began);   // a lazy recovery at the entry is not silent just because the new write worked
        return outcome;
    }
    StepFailure failure {*committed.primary};
    failure.native = std::move(committed.native);   // the codes captured by the scope BEFORE its cleanup (787, not the later 23/0)
    return failedOutcome(h, impl.fault, std::move(failure), committed.cleanup, record.id, record.resultSegmentId, &began);
}

common::Result<std::optional<ProjectMemoryCompactionRecord>> SqliteProjectMemoryCompactionRepository::findCompactionRecord(
    const application::ProjectMemoryCompactionId& id) const
{
    using LookupResult = common::Result<std::optional<ProjectMemoryCompactionRecord>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    // A transaction that is open on the connection would show its own uncommitted rows: that is not what is stored.
    if (sqlite3_get_autocommit(h) == 0) {
        return LookupResult::failure(error("persistence.lookup_unavailable",
            "A transaction is open on the connection, so the stored record cannot be looked up"));
    }
    const std::string sql = std::string("SELECT ") + projectMemoryCompactionRecordColumns
        + " FROM project_memory_compaction_records WHERE id=?;";
    Statement s;
    const auto prepared = prepare(h, sql.c_str(), s);
    if (!prepared.success()) return LookupResult::failure(*prepared.error());
    const auto bound = bindText(h, s.get(), 1, id);
    if (!bound.success()) return LookupResult::failure(*bound.error());
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) return LookupResult::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return LookupResult::failure(databaseError(h, "persistence.read_failed", "Could not look up a project memory compaction record"));
    }
    // Strict: a damaged provenance list is a read failure, never a repaired record (the identity check must not match against it).
    auto record = readProjectMemoryCompactionRecord(s.get(), SourceListReading::Strict);
    if (!record.success()) return LookupResult::failure(*record.error());
    return LookupResult::ok(std::move(record.value()));
}

common::Result<std::vector<ProjectMemoryCompactionRecord>>
SqliteProjectMemoryCompactionRepository::listCompactionRecords(
    const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    const std::string sql = std::string("SELECT ") + projectMemoryCompactionRecordColumns
        + " FROM project_memory_compaction_records WHERE project_id=? ORDER BY created_at_ms,id;";
    Statement s;
    const auto prepared = prepare(h, sql.c_str(), s);
    if (!prepared.success())
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*prepared.error());
    const auto bound = bindText(h, s.get(), 1, projectId);
    if (!bound.success())
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*bound.error());
    std::vector<ProjectMemoryCompactionRecord> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(databaseError(
                h, "persistence.read_failed", "Could not list project memory compaction records"));
        }
        // Lenient, exactly as before: the history listing keeps its pinned behaviour.
        auto record = readProjectMemoryCompactionRecord(s.get(), SourceListReading::Lenient);
        if (!record.success())
            return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*record.error());
        out.push_back(std::move(record.value()));
    }
    return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok(std::move(out));
}

} // namespace ai::infrastructure
