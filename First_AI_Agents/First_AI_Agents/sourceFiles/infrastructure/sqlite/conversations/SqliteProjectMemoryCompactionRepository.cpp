#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
    "error_message,created_at_ms";

common::Result<void> insertProjectMemoryCompactionRecord(
    sqlite3* h, const ProjectMemoryCompactionRecord& record)
{
    Statement statement;
    const auto prepared = prepare(h,
        "INSERT INTO project_memory_compaction_records (id,project_id,provider_id,model_id,"
        "source_segment_ids,source_content_hash,result_segment_id,input_tokens,output_tokens,"
        "cost_micros,status,error_code,error_message,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
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
        bindInt64(h, statement.get(), 14, toMilliseconds(record.createdAt))};
    for (const auto& bound : binds) {
        if (!bound.success()) return common::Result<void>::failure(*bound.error());
    }
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return common::Result<void>::ok();

    const int code = sqlite3_extended_errcode(h);
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

common::Result<ProjectMemoryCompactionRecord> readProjectMemoryCompactionRecord(sqlite3_stmt* statement)
{
    ProjectMemoryCompactionRecord record;
    record.id = columnText(statement, 0);
    record.projectId = columnText(statement, 1);
    record.providerId = columnText(statement, 2);
    record.modelId = columnText(statement, 3);
    record.sourceSegmentIds = deserializeSegmentIdList(columnText(statement, 4));
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
    return common::Result<ProjectMemoryCompactionRecord>::ok(std::move(record));
}

} // namespace

SqliteProjectMemoryCompactionRepository::SqliteProjectMemoryCompactionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteProjectMemoryCompactionRepository::insertCompactionRecord(
    const ProjectMemoryCompactionRecord& record)
{
    if (record.status == ProjectMemoryCompactionStatus::Completed) {
        return common::Result<void>::failure(error(
            "project_memory.invalid_compaction_commit",
            "Completed compaction records require an atomic memory commit"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    return insertProjectMemoryCompactionRecord(database_->impl_->handle, record);
}

common::Result<void> SqliteProjectMemoryCompactionRepository::commitSuccessfulCompaction(
    const ProjectMemoryCompactionRecord& record,
    ProjectMemorySegment resultSegment)
{
    if (record.status != ProjectMemoryCompactionStatus::Completed
        || !record.resultSegmentId.has_value() || *record.resultSegmentId != resultSegment.id
        || record.projectId != resultSegment.projectId || record.sourceSegmentIds.empty()) {
        return common::Result<void>::failure(error(
            "project_memory.invalid_compaction_commit",
            "Successful compaction commit fields are inconsistent"));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    const auto began = execute(h, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(h, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    // Requiring exactly one deleted row per source id is the optimistic
    // concurrency check. Another compaction or clear cannot make a stale
    // provider response overwrite the newer memory state.
    for (const auto& id : record.sourceSegmentIds) {
        Statement removed;
        const auto prepared = prepare(
            h, "DELETE FROM project_memory_segments WHERE id=? AND project_id=?;", removed);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> binds[] = {
            bindText(h, removed.get(), 1, id), bindText(h, removed.get(), 2, record.projectId)};
        for (const auto& bound : binds) {
            if (!bound.success()) return rollbackWith(*bound.error());
        }
        if (sqlite3_step(removed.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                h, "persistence.write_failed", "Could not remove a source memory segment"));
        }
        if (sqlite3_changes(h) != 1) {
            return rollbackWith(error(
                "project_memory.compaction_conflict",
                "Project memory changed while compaction was running"));
        }
    }

    Statement inserted;
    const auto insertPrepared = prepare(h,
        "INSERT INTO project_memory_segments (id,project_id,run_id,"
        "summary,source_token_count,summary_token_count,created_at_ms) VALUES (?,?,?,?,?,?,?);",
        inserted);
    if (!insertPrepared.success()) return rollbackWith(*insertPrepared.error());
    const common::Result<void> insertBinds[] = {
        bindText(h, inserted.get(), 1, resultSegment.id),
        bindText(h, inserted.get(), 2, resultSegment.projectId),
        bindText(h, inserted.get(), 3, resultSegment.runId),
        bindText(h, inserted.get(), 4, resultSegment.summary),
        bindUnsigned(h, inserted.get(), 5, resultSegment.sourceTokenCount),
        bindUnsigned(h, inserted.get(), 6, resultSegment.summaryTokenCount),
        bindInt64(h, inserted.get(), 7, toMilliseconds(resultSegment.createdAt))};
    for (const auto& bound : insertBinds) {
        if (!bound.success()) return rollbackWith(*bound.error());
    }
    if (sqlite3_step(inserted.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(h);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return rollbackWith(error(
                "project_memory.duplicate_id", "Memory segment id already exists"));
        }
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
            return rollbackWith(error(
                "project_memory.project_not_found", "Project does not exist"));
        }
        return rollbackWith(databaseError(
            h, "persistence.write_failed", "Could not insert compacted memory segment"));
    }

    const auto recorded = insertProjectMemoryCompactionRecord(h, record);
    if (!recorded.success()) return rollbackWith(*recorded.error());

    const auto committed = execute(h, "COMMIT;", "persistence.transaction_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
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
        auto record = readProjectMemoryCompactionRecord(s.get());
        if (!record.success())
            return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*record.error());
        out.push_back(std::move(record.value()));
    }
    return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok(std::move(out));
}

} // namespace ai::infrastructure
