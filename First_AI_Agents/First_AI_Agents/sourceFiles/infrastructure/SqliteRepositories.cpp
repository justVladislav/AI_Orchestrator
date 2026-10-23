#include "ai/infrastructure/SqliteRepositories.hpp"
#include "sqlite/SqliteConnection.hpp"
#include "sqlite/SqlitePrimitives.hpp"
#include "sqlite/catalog/SqliteProfileMapping.hpp"
#include "sqlite/roster/SqliteRosterMapping.hpp"
#include "sqlite/execution/SqliteRunStatusMapping.hpp"
#include "sqlite/planning/SqliteRunDraftGroupStorage.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/ChatProjectContext.hpp"
#include "ai/application/RunDraftGroup.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"

// Native SQLite C API only. This backend must build and link without the Qt
// SDK (no Qt6::Sql/QSqlDatabase); see CODEX_CONTEXT.md for why an earlier
// Qt SQL based revision of this file was reverted.
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ai::infrastructure {

namespace {

using application::Artifact;
using application::ArtifactKind;
using application::ApplyHashEntry;
using application::ApprovalRecord;
using application::ChangeSetState;
using application::CacheCreationDurationUsageProblem;
using application::cacheCreationDurationUsageProblemCode;
using application::cacheCreationDurationUsageProblemMessage;
using application::checkCacheCreationDurationUsage;
using application::ChangeId;
using application::ChangeSetId;
using application::FileChangeOperation;
using application::LongContextPricing;
using application::LongContextPricingProblem;
using application::ProjectMemoryCompactionRecord;
using application::ProjectMemoryCompactionStatus;
using application::ProposalReviewRejection;
using application::ProposedChangeSet;
using application::ProposedFileChange;
using application::ValidationFinding;
using application::ChatConversation;
using application::ChatMemorySegment;
using application::ChatMessage;
using application::ChatProjectContextAttachment;
using application::PreparedChatTurn;
using application::ExecutionProfile;
using application::RunDraft;
using application::RunDraftId;
using application::RunLaunchMode;
using application::Model;
using application::ModelExecution;
using application::QualityEvaluation;
using application::Project;
using application::ProjectMemorySegment;
using application::ProjectTask;
using application::ProjectStatus;
using application::Provider;
using application::ProviderExecutionBudgetOverrides;
using application::ProviderExecutionBudgetPolicy;
using application::ProviderExecutionBudgetUsage;
using application::Run;
using application::RunStage;
using application::RunStatus;
using application::StageModelAssignment;
using application::StageStatus;
using application::TaskPriority;
using application::TaskStatus;

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
using sqlite_detail::bindOptionalTime;
using sqlite_detail::optionalTime;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;
using sqlite_detail::pathToUtf8;
using sqlite_detail::pathFromUtf8;
using sqlite_detail::providerBudgetOverridesJson;
using sqlite_detail::parseProviderBudgetOverrides;
using sqlite_detail::readAssignment;
using sqlite_detail::serializeStringVector;
using sqlite_detail::parseStringVector;
using sqlite_detail::readRunAssignment;
using sqlite_detail::runStatusFromString;
using sqlite_detail::insertRunDraftGroupRows;
using sqlite_detail::findRunDraftGroupWhere;
using sqlite_detail::readRunDraftGroupDrafts;



} // namespace


namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

namespace {

} // namespace

SqliteModelExecutionRepository::SqliteModelExecutionRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* modelExecutionColumns =
    "id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,"
    "request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,"
    "cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,"
    "cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,"
    "cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,"
    "cache_creation_one_hour_input_tokens";

common::Result<ModelExecution> readModelExecution(sqlite3_stmt* statement)
{
    const auto source = application::modelExecutionSourceFromString(columnText(statement, 1));
    const auto status = application::modelExecutionStatusFromString(columnText(statement, 9));
    if (!source || !status)
        return common::Result<ModelExecution>::failure(error("persistence.invalid_data", "Model execution contains an unknown enum value"));
    ModelExecution value;
    value.id = columnText(statement, 0);
    value.source = *source;
    value.sourceId = columnText(statement, 2);
    value.projectId = optionalText(statement, 3);
    value.providerId = columnText(statement, 4);
    value.modelId = columnText(statement, 5);
    value.role = columnText(statement, 6);
    value.position = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 7));
    const auto attempt = sqlite3_column_int64(statement, 8);
    if (attempt <= 0) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution attempt must be positive"));
    }
    value.attempt = static_cast<std::uint32_t>(attempt);
    value.status = *status;
    value.requestHash = columnText(statement, 10);
    value.contextHash = columnText(statement, 11);
    value.outputArtifactId = optionalText(statement, 12);
    value.outputContent = columnText(statement, 13);
    value.outputPreview = columnText(statement, 14);
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 15));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 16));
    value.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 17));
    value.latencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 18));
    value.errorCode = columnText(statement, 19);
    value.errorMessage = columnText(statement, 20);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 21));
    value.startedAt = optionalTime(statement, 22);
    value.finishedAt = optionalTime(statement, 23);
    value.costPriceKnown = sqlite3_column_int(statement, 24) != 0;
    const std::int64_t cacheCreation = sqlite3_column_int64(statement, 25);
    const std::int64_t cacheRead = sqlite3_column_int64(statement, 26);
    const std::int64_t durationKnown = sqlite3_column_int64(statement, 27);
    const std::int64_t fiveMinute = sqlite3_column_int64(statement, 28);
    const std::int64_t oneHour = sqlite3_column_int64(statement, 29);
    if (cacheCreation < 0 || cacheRead < 0 || fiveMinute < 0 || oneHour < 0
        || (durationKnown != 0 && durationKnown != 1)) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution contains invalid cache usage"));
    }
    value.cacheCreationInputTokens = static_cast<std::uint64_t>(cacheCreation);
    value.cacheReadInputTokens = static_cast<std::uint64_t>(cacheRead);
    value.cacheCreationDuration.known = durationKnown != 0;
    value.cacheCreationDuration.fiveMinuteTokens = static_cast<std::uint64_t>(fiveMinute);
    value.cacheCreationDuration.oneHourTokens = static_cast<std::uint64_t>(oneHour);
    if (checkCacheCreationDurationUsage(
            value.cacheCreationInputTokens,
            value.cacheCreationDuration)
            != CacheCreationDurationUsageProblem::None
        || value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution contains inconsistent cache usage"));
    }
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<void> bindModelExecutionFields(sqlite3* handle, sqlite3_stmt* statement, const ModelExecution& value, int first)
{
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<void>::failure(error(
            "model_execution.invalid_cache_usage",
            "Cache-creation and cache-read tokens must fit within input tokens"));
    }
    const common::Result<void> bindings[] = {
        bindText(handle, statement, first, std::string(application::toString(value.source))),
        bindText(handle, statement, first + 1, value.sourceId),
        bindNullableText(handle, statement, first + 2, value.projectId),
        bindText(handle, statement, first + 3, value.providerId),
        bindText(handle, statement, first + 4, value.modelId),
        bindText(handle, statement, first + 5, value.role),
        bindUnsigned(handle, statement, first + 6, value.position),
        bindUnsigned(handle, statement, first + 7, value.attempt),
        bindText(handle, statement, first + 8, std::string(application::toString(value.status))),
        bindText(handle, statement, first + 9, value.requestHash),
        bindText(handle, statement, first + 10, value.contextHash),
        bindNullableText(handle, statement, first + 11, value.outputArtifactId),
        bindText(handle, statement, first + 12, value.outputContent),
        bindText(handle, statement, first + 13, value.outputPreview),
        bindUnsigned(handle, statement, first + 14, value.inputTokens),
        bindUnsigned(handle, statement, first + 15, value.outputTokens),
        bindUnsigned(handle, statement, first + 16, value.costMicros),
        bindUnsigned(handle, statement, first + 17, value.latencyMs),
        bindText(handle, statement, first + 18, value.errorCode),
        bindText(handle, statement, first + 19, value.errorMessage),
        bindInt64(handle, statement, first + 20, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement, first + 21, value.startedAt),
        bindOptionalTime(handle, statement, first + 22, value.finishedAt),
        bindInt64(handle, statement, first + 23, value.costPriceKnown ? 1 : 0),
        bindUnsigned(handle, statement, first + 24, value.cacheCreationInputTokens),
        bindUnsigned(handle, statement, first + 25, value.cacheReadInputTokens),
        bindInt64(handle, statement, first + 26, value.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement, first + 27,
            value.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement, first + 28,
            value.cacheCreationDuration.oneHourTokens)};
    for (const auto& binding : bindings) if (!binding.success()) return binding;
    return common::Result<void>::ok();
}

common::Result<QualityEvaluation> readQualityEvaluation(sqlite3_stmt* statement)
{
    const auto kind = application::qualityEvaluatorKindFromString(columnText(statement, 2));
    if (!kind) return common::Result<QualityEvaluation>::failure(error("persistence.invalid_data", "Quality evaluation contains an unknown evaluator kind"));
    QualityEvaluation value;
    value.id = columnText(statement, 0);
    value.executionId = columnText(statement, 1);
    value.evaluatorKind = *kind;
    value.evaluatorProviderId = optionalText(statement, 3);
    value.evaluatorModelId = optionalText(statement, 4);
    value.rubricVersion = columnText(statement, 5);
    value.scoreBasisPoints = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 6));
    value.criteriaJson = columnText(statement, 7);
    value.explanation = columnText(statement, 8);
    value.evidence = columnText(statement, 9);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

} // namespace

common::Result<ModelExecution> SqliteModelExecutionRepository::insertExecution(ModelExecution value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "INSERT INTO model_executions (id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<ModelExecution>::failure(*prepared.error());
    auto idBound = bindText(handle, statement.get(), 1, value.id);
    if (!idBound.success()) return common::Result<ModelExecution>::failure(*idBound.error());
    auto fieldsBound = bindModelExecutionFields(handle, statement.get(), value, 2);
    if (!fieldsBound.success()) return common::Result<ModelExecution>::failure(*fieldsBound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        return common::Result<ModelExecution>::failure(databaseError(handle, "persistence.write_failed", "Could not insert model execution"));
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<ModelExecution> SqliteModelExecutionRepository::updateExecution(ModelExecution value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "UPDATE model_executions SET source_type=?,source_id=?,project_id=?,provider_id=?,model_id=?,role=?,position=?,attempt=?,status=?,request_hash=?,context_hash=?,output_artifact_id=?,output_content=?,output_preview=?,input_tokens=?,output_tokens=?,cost_micros=?,latency_ms=?,error_code=?,error_message=?,created_at_ms=?,started_at_ms=?,finished_at_ms=?,cost_price_known=?,cache_creation_input_tokens=?,cache_read_input_tokens=?,cache_creation_duration_partition_known=?,cache_creation_five_minute_input_tokens=?,cache_creation_one_hour_input_tokens=? WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<ModelExecution>::failure(*prepared.error());
    auto fieldsBound = bindModelExecutionFields(handle, statement.get(), value, 1);
    if (!fieldsBound.success()) return common::Result<ModelExecution>::failure(*fieldsBound.error());
    auto idBound = bindText(handle, statement.get(), 30, value.id);
    if (!idBound.success()) return common::Result<ModelExecution>::failure(*idBound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        return common::Result<ModelExecution>::failure(databaseError(handle, "persistence.write_failed", "Could not update model execution"));
    if (sqlite3_changes(handle) == 0)
        return common::Result<ModelExecution>::failure(error("model_execution.not_found", "Execution not found"));
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<std::optional<ModelExecution>> SqliteModelExecutionRepository::findExecution(const application::ModelExecutionId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE id=?;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<ModelExecution>>::failure(*prepared.error());
    auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ModelExecution>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ModelExecution>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not read model execution"));
    auto value = readModelExecution(statement.get());
    if (!value.success()) return common::Result<std::optional<ModelExecution>>::failure(*value.error());
    return common::Result<std::optional<ModelExecution>>::ok(value.value());
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listExecutions(application::ModelExecutionSource source, const std::string& sourceId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE source_type=? AND source_id=? ORDER BY position,created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    const common::Result<void> bindings[] = {bindText(handle, statement.get(), 1, std::string(application::toString(source))), bindText(handle, statement.get(), 2, sourceId)};
    for (const auto& binding : bindings) if (!binding.success()) return common::Result<std::vector<ModelExecution>>::failure(*binding.error());
    std::vector<ModelExecution> values;
    for (;;) { const int step = sqlite3_step(statement.get()); if (step == SQLITE_DONE) break; if (step != SQLITE_ROW) return common::Result<std::vector<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not list model executions")); auto value = readModelExecution(statement.get()); if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error()); values.push_back(std::move(value.value())); }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listProjectExecutions(const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE project_id=? ORDER BY created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    auto bound = bindText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return common::Result<std::vector<ModelExecution>>::failure(*bound.error());
    std::vector<ModelExecution> values;
    for (;;) { const int step = sqlite3_step(statement.get()); if (step == SQLITE_DONE) break; if (step != SQLITE_ROW) return common::Result<std::vector<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not list project executions")); auto value = readModelExecution(statement.get()); if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error()); values.push_back(std::move(value.value())); }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listUnfinishedExecutions() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns
        + " FROM model_executions WHERE status IN ('pending','running') ORDER BY created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    std::vector<ModelExecution> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ModelExecution>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list unfinished model executions"));
        auto value = readModelExecution(statement.get());
        if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<QualityEvaluation> SqliteModelExecutionRepository::insertEvaluation(QualityEvaluation value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    auto prepared = prepare(handle, "INSERT INTO quality_evaluations (id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<QualityEvaluation>::failure(*prepared.error());
    const common::Result<void> bindings[] = {bindText(handle,statement.get(),1,value.id),bindText(handle,statement.get(),2,value.executionId),bindText(handle,statement.get(),3,std::string(application::toString(value.evaluatorKind))),bindNullableText(handle,statement.get(),4,value.evaluatorProviderId),bindNullableText(handle,statement.get(),5,value.evaluatorModelId),bindText(handle,statement.get(),6,value.rubricVersion),bindUnsigned(handle,statement.get(),7,value.scoreBasisPoints),bindText(handle,statement.get(),8,value.criteriaJson),bindText(handle,statement.get(),9,value.explanation),bindText(handle,statement.get(),10,value.evidence),bindInt64(handle,statement.get(),11,toMilliseconds(value.createdAt))};
    for (const auto& binding : bindings) if (!binding.success()) return common::Result<QualityEvaluation>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return common::Result<QualityEvaluation>::failure(databaseError(handle,"persistence.write_failed","Could not insert quality evaluation"));
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

common::Result<std::optional<QualityEvaluation>> SqliteModelExecutionRepository::findEvaluation(const application::QualityEvaluationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* handle=database_->impl_->handle; Statement statement;
    auto prepared=prepare(handle,"SELECT id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms FROM quality_evaluations WHERE id=?;",statement); if(!prepared.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*prepared.error()); auto bound=bindText(handle,statement.get(),1,id); if(!bound.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*bound.error()); const int step=sqlite3_step(statement.get()); if(step==SQLITE_DONE) return common::Result<std::optional<QualityEvaluation>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<QualityEvaluation>>::failure(databaseError(handle,"persistence.read_failed","Could not read quality evaluation")); auto value=readQualityEvaluation(statement.get()); if(!value.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*value.error()); return common::Result<std::optional<QualityEvaluation>>::ok(value.value());
}

common::Result<std::vector<QualityEvaluation>> SqliteModelExecutionRepository::listEvaluations(const application::ModelExecutionId& executionId) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* handle=database_->impl_->handle; Statement statement;
    // ORDER BY created_at_ms,rowid - not ,id. `id` is a random UUID
    // (generateUuid(), IdGenerator.cpp), which is not time-ordered; when two
    // evaluations for the same execution are inserted within the same
    // millisecond (common - two evaluations recorded back-to-back have no
    // reason to straddle a millisecond boundary), sorting by created_at_ms
    // then the random id ties on the first key and then orders by the
    // second key essentially randomly, silently reordering the two rows.
    // rowid is SQLite's own implicit, strictly-insertion-order column for
    // this ordinary (non-WITHOUT-ROWID) table - it is what "created_at_ms"
    // was actually trying to express as a tiebreaker, and a caller
    // reasonably expects listEvaluations() to come back in the order the
    // evaluations were actually added.
    auto prepared=prepare(handle,"SELECT id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms FROM quality_evaluations WHERE execution_id=? ORDER BY created_at_ms,rowid;",statement); if(!prepared.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*prepared.error()); auto bound=bindText(handle,statement.get(),1,executionId); if(!bound.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*bound.error()); std::vector<QualityEvaluation> values; for(;;){const int step=sqlite3_step(statement.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<QualityEvaluation>>::failure(databaseError(handle,"persistence.read_failed","Could not list quality evaluations")); auto value=readQualityEvaluation(statement.get()); if(!value.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*value.error()); values.push_back(std::move(value.value()));} return common::Result<std::vector<QualityEvaluation>>::ok(std::move(values));
}


// ============================================================
// SqliteChangeProposalRepository
// ============================================================

namespace {

// Read a ProposedChangeSet header row from a prepared statement.
// Column order must match changeSetColumns below.
ProposedChangeSet readChangeSet(sqlite3_stmt* stmt)
{
    ProposedChangeSet cs;
    cs.changeSetId           = columnText(stmt, 0);
    cs.runId                 = columnText(stmt, 1);
    cs.projectId             = columnText(stmt, 2);
    cs.providerId            = columnText(stmt, 3);
    cs.modelId               = columnText(stmt, 4);
    const std::string mexId  = columnText(stmt, 5);
    if (!mexId.empty()) cs.modelExecutionId = mexId;
    cs.agentRole             = columnText(stmt, 6);
    cs.summary               = columnText(stmt, 7);
    cs.baseRevision          = columnText(stmt, 8);
    cs.policySnapshotHash    = columnText(stmt, 9);
    cs.artifactRelativeRoot  = columnText(stmt, 10);
    cs.state                 = application::changeSetStateFromString(columnText(stmt, 11));
    cs.schemaVersion         = columnText(stmt, 12);
    cs.createdAt             = fromMilliseconds(sqlite3_column_int64(stmt, 13));
    cs.missingEvidenceCode    = optionalText(stmt, 14);
    cs.missingEvidenceMessage = optionalText(stmt, 15);
    cs.supersedesChangeSetId  = optionalText(stmt, 16);
    return cs;
}

constexpr const char* changeSetColumns =
    "id,run_id,project_id,provider_id,model_id,model_execution_id,"
    "agent_role,summary,base_revision,policy_snapshot_hash,"
    "artifact_relative_root,state,schema_version,created_at_ms,"
    "missing_evidence_code,missing_evidence_message,supersedes_change_set_id";

class ScopedTransactionRollback {
public:
    explicit ScopedTransactionRollback(sqlite3* handle)
        : handle_(handle)
    {
    }

    ~ScopedTransactionRollback()
    {
        if (active_) {
            static_cast<void>(
                execute(handle_, "ROLLBACK;", "persistence.rollback_failed"));
        }
    }

    void release() noexcept { active_ = false; }

private:
    sqlite3* handle_;
    bool active_ {true};
};

} // namespace

SqliteChangeProposalRepository::SqliteChangeProposalRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteChangeProposalRepository::insertChangeSet(
    const ProposedChangeSet& cs)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const auto began =
        execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) {
        return common::Result<void>::failure(*began.error());
    }
    ScopedTransactionRollback rollback {handle};

    // Insert the change-set header row.
    {
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposed_change_sets "
            "(id,run_id,project_id,provider_id,model_id,model_execution_id,"
            "agent_role,summary,base_revision,policy_snapshot_hash,"
            "artifact_relative_root,state,schema_version,created_at_ms,"
            "missing_evidence_code,missing_evidence_message,supersedes_change_set_id) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const std::string mexId = cs.modelExecutionId.has_value() ? *cs.modelExecutionId : "";
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, cs.changeSetId),
            bindText(handle, stmt.get(), 2, cs.runId),
            bindText(handle, stmt.get(), 3, cs.projectId),
            bindText(handle, stmt.get(), 4, cs.providerId),
            bindText(handle, stmt.get(), 5, cs.modelId),
            bindText(handle, stmt.get(), 6, mexId),
            bindText(handle, stmt.get(), 7, cs.agentRole),
            bindText(handle, stmt.get(), 8, cs.summary),
            bindText(handle, stmt.get(), 9, cs.baseRevision),
            bindText(handle, stmt.get(), 10, cs.policySnapshotHash),
            bindText(handle, stmt.get(), 11, cs.artifactRelativeRoot),
            bindText(handle, stmt.get(), 12, application::toString(cs.state)),
            bindText(handle, stmt.get(), 13, cs.schemaVersion),
            bindInt64(handle, stmt.get(), 14, toMilliseconds(cs.createdAt)),
            bindNullableText(handle, stmt.get(), 15, cs.missingEvidenceCode),
            bindNullableText(handle, stmt.get(), 16, cs.missingEvidenceMessage),
            bindNullableText(handle, stmt.get(), 17, cs.supersedesChangeSetId)};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(handle);
            if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return common::Result<void>::failure(
                    error("proposal.duplicate_id", "Change set id already exists"));
            }
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert change set"));
        }
    }

    // Insert per-change rows.
    for (std::size_t i = 0; i < cs.changes.size(); ++i) {
        const auto& ch = cs.changes[i];
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposed_file_changes "
            "(id,change_set_id,change_id,operation,relative_path,source_path,expected_original_hash,ordinal) "
            "VALUES (?,?,?,?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const std::string srcPath = ch.sourcePath.has_value()
                                      ? pathToUtf8(*ch.sourcePath) : "";
        const std::string expHash = ch.expectedOriginalHash.value_or("");
        // Use changeSetId + ordinal as a globally-unique primary key.
        const std::string rowId = cs.changeSetId + "_" + std::to_string(i);
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, rowId),
            bindText(handle, stmt.get(), 2, cs.changeSetId),
            bindText(handle, stmt.get(), 3, ch.changeId),
            bindText(handle, stmt.get(), 4, application::toString(ch.operation)),
            bindText(handle, stmt.get(), 5, pathToUtf8(ch.relativePath)),
            bindText(handle, stmt.get(), 6, srcPath),
            bindText(handle, stmt.get(), 7, expHash),
            bindInt64(handle, stmt.get(), 8, static_cast<std::int64_t>(i))};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert file change"));
        }
    }

    // Insert validation findings.
    for (std::size_t i = 0; i < cs.validationFindings.size(); ++i) {
        const auto& f = cs.validationFindings[i];
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposal_validation_findings "
            "(change_set_id,code,message,change_id,ordinal) VALUES (?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, cs.changeSetId),
            bindText(handle, stmt.get(), 2, f.code),
            bindText(handle, stmt.get(), 3, f.message),
            bindText(handle, stmt.get(), 4, f.changeId),
            bindInt64(handle, stmt.get(), 5, static_cast<std::int64_t>(i))};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert validation finding"));
        }
    }

    const auto committed =
        execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) {
        return common::Result<void>::failure(*committed.error());
    }
    rollback.release();
    return common::Result<void>::ok();
}

common::Result<ProposedChangeSet> SqliteChangeProposalRepository::updateChangeSetState(
    const ChangeSetId& id, ChangeSetState newState)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    {
        Statement stmt;
        const auto prep = prepare(handle,
            "UPDATE proposed_change_sets SET state=? WHERE id=?;", stmt);
        if (!prep.success()) return common::Result<ProposedChangeSet>::failure(*prep.error());
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, application::toString(newState)),
            bindText(handle, stmt.get(), 2, id)};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<ProposedChangeSet>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<ProposedChangeSet>::failure(
                databaseError(handle, "persistence.write_failed", "Could not update change-set state"));
        }
        if (sqlite3_changes(handle) == 0) {
            return common::Result<ProposedChangeSet>::failure(
                error("proposal.not_found", "Change set not found: " + id));
        }
    }

    // Re-read the updated row.
    const std::string sql = std::string("SELECT ") + changeSetColumns
                          + " FROM proposed_change_sets WHERE id=?;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) return common::Result<ProposedChangeSet>::failure(*prep.error());
    const auto bound = bindText(handle, stmt.get(), 1, id);
    if (!bound.success()) return common::Result<ProposedChangeSet>::failure(*bound.error());
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return common::Result<ProposedChangeSet>::failure(
            error("proposal.not_found", "Change set disappeared after update: " + id));
    }
    return common::Result<ProposedChangeSet>::ok(readChangeSet(stmt.get()));
}

common::Result<void> SqliteChangeProposalRepository::markMissingEvidence(
    const ChangeSetId& id, std::string code, std::string message)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "UPDATE proposed_change_sets SET missing_evidence_code=?,"
        "missing_evidence_message=? WHERE id=?;",
        stmt);
    if (!prep.success()) return common::Result<void>::failure(*prep.error());
    const common::Result<void> bindings[] = {
        bindText(handle, stmt.get(), 1, code),
        bindText(handle, stmt.get(), 2, message),
        bindText(handle, stmt.get(), 3, id)};
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<void>::failure(*b.error());
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record missing-evidence reason"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(
            error("proposal.not_found", "Change set not found: " + id));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposedChangeSet>> SqliteChangeProposalRepository::findChangeSet(
    const ChangeSetId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
                          + " FROM proposed_change_sets WHERE id=?;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, id);
    if (!bound.success()) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(*bound.error());
    }
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read change set"));
    }
    return common::Result<std::optional<ProposedChangeSet>>::ok(readChangeSet(stmt.get()));
}

common::Result<std::vector<ProposedChangeSet>> SqliteChangeProposalRepository::listForRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
        + " FROM proposed_change_sets WHERE run_id=? ORDER BY created_at_ms ASC;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*bound.error());
    }
    std::vector<ProposedChangeSet> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readChangeSet(stmt.get()));
    }
    return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
}

common::Result<std::vector<ProposedChangeSet>> SqliteChangeProposalRepository::listForProject(
    const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
        + " FROM proposed_change_sets WHERE project_id=? ORDER BY created_at_ms ASC;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, projectId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*bound.error());
    }
    std::vector<ProposedChangeSet> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readChangeSet(stmt.get()));
    }
    return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
}

common::Result<std::vector<ProposedFileChange>> SqliteChangeProposalRepository::listChanges(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "SELECT change_id,change_set_id,operation,relative_path,source_path,"
        "expected_original_hash,ordinal "
        "FROM proposed_file_changes WHERE change_set_id=? ORDER BY ordinal ASC;",
        stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedFileChange>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, changeSetId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedFileChange>>::failure(*bound.error());
    }

    std::vector<ProposedFileChange> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ProposedFileChange ch;
        ch.changeId   = columnText(stmt.get(), 0);
        // column 1 is change_set_id (ignored)
        ch.operation  = application::fileChangeOperationFromString(columnText(stmt.get(), 2));
        ch.relativePath = pathFromUtf8(columnText(stmt.get(), 3));
        const std::string src = columnText(stmt.get(), 4);
        if (!src.empty()) ch.sourcePath = pathFromUtf8(src);
        const std::string hash = columnText(stmt.get(), 5);
        if (!hash.empty()) ch.expectedOriginalHash = hash;
        // proposedContent and capturedOriginalContent are NOT stored in SQLite.
        result.push_back(std::move(ch));
    }
    return common::Result<std::vector<ProposedFileChange>>::ok(std::move(result));
}

common::Result<std::vector<ValidationFinding>> SqliteChangeProposalRepository::listValidationFindings(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "SELECT code,message,change_id FROM proposal_validation_findings "
        "WHERE change_set_id=? ORDER BY ordinal ASC;",
        stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ValidationFinding>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, changeSetId);
    if (!bound.success()) {
        return common::Result<std::vector<ValidationFinding>>::failure(*bound.error());
    }

    std::vector<ValidationFinding> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ValidationFinding f;
        f.code     = columnText(stmt.get(), 0);
        f.message  = columnText(stmt.get(), 1);
        f.changeId = columnText(stmt.get(), 2);
        result.push_back(std::move(f));
    }
    return common::Result<std::vector<ValidationFinding>>::ok(std::move(result));
}

namespace {

// Conservative cap safely below any SQLite build's compiled-in
// SQLITE_LIMIT_VARIABLE_NUMBER (999 pre-3.32.0, 32766 by default since), so a
// batched IN(...) query below never risks "too many SQL variables" no matter
// how many ids a caller passes.
constexpr std::size_t kBatchedInQueryChunkSize = 500;

// Deduplicates (a repeated id must not produce repeated rows in the result,
// and would otherwise waste bound parameters/chunk headroom for nothing) and
// splits into chunks no larger than kBatchedInQueryChunkSize. A single id's
// rows always come from exactly one chunk's query - chunks partition the
// deduplicated id list - so each chunk's own "ORDER BY change_set_id,ordinal"
// is never interleaved with another chunk's rows for the same id.
std::vector<std::vector<ChangeSetId>> dedupedIdChunks(const std::vector<ChangeSetId>& ids)
{
    std::vector<ChangeSetId> unique;
    unique.reserve(ids.size());
    std::unordered_set<ChangeSetId> seen;
    seen.reserve(ids.size());
    for (const auto& id : ids) {
        if (seen.insert(id).second) unique.push_back(id);
    }
    std::vector<std::vector<ChangeSetId>> chunks;
    for (std::size_t offset = 0; offset < unique.size(); offset += kBatchedInQueryChunkSize) {
        const std::size_t count = std::min(kBatchedInQueryChunkSize, unique.size() - offset);
        chunks.emplace_back(
            unique.begin() + static_cast<std::ptrdiff_t>(offset),
            unique.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    return chunks;
}

} // namespace

common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
SqliteChangeProposalRepository::listChangesForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>;
    std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> byChangeSet;
    if (changeSetIds.empty()) {
        return ResultType::ok(std::move(byChangeSet));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    for (const auto& chunk : dedupedIdChunks(changeSetIds)) {
        std::string placeholders(chunk.size() * 2 - 1, ',');
        for (std::size_t i = 0; i < chunk.size(); ++i) placeholders[i * 2] = '?';

        Statement stmt;
        const std::string sql =
            "SELECT change_set_id,change_id,operation,relative_path,source_path,"
            "expected_original_hash,ordinal FROM proposed_file_changes WHERE change_set_id IN ("
            + placeholders + ") ORDER BY change_set_id,ordinal ASC;";
        const auto prep = prepare(handle, sql.c_str(), stmt);
        if (!prep.success()) return ResultType::failure(*prep.error());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto bound = bindText(handle, stmt.get(), static_cast<int>(i) + 1, chunk[i]);
            if (!bound.success()) return ResultType::failure(*bound.error());
        }

        // Every non-ROW step() result must be checked explicitly: SQLITE_DONE
        // means the chunk finished cleanly, anything else (SQLITE_BUSY after
        // the busy-timeout elapses, SQLITE_IOERR, SQLITE_CORRUPT, ...) is a
        // real failure. Returning failure here discards this function's own
        // accumulated byChangeSet entirely - a caller never sees a partial
        // result silently reported as success.
        for (;;) {
            const int step = sqlite3_step(stmt.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return ResultType::failure(databaseError(
                    handle, "persistence.read_failed", "Could not read proposed file changes"));
            }
            const std::string changeSetId = columnText(stmt.get(), 0);
            ProposedFileChange ch;
            ch.changeId = columnText(stmt.get(), 1);
            ch.operation = application::fileChangeOperationFromString(columnText(stmt.get(), 2));
            ch.relativePath = pathFromUtf8(columnText(stmt.get(), 3));
            const std::string src = columnText(stmt.get(), 4);
            if (!src.empty()) ch.sourcePath = pathFromUtf8(src);
            const std::string hash = columnText(stmt.get(), 5);
            if (!hash.empty()) ch.expectedOriginalHash = hash;
            // proposedContent and capturedOriginalContent are NOT stored in SQLite.
            byChangeSet[changeSetId].push_back(std::move(ch));
        }
    }
    return ResultType::ok(std::move(byChangeSet));
}

common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
SqliteChangeProposalRepository::listValidationFindingsForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>;
    std::unordered_map<ChangeSetId, std::vector<ValidationFinding>> byChangeSet;
    if (changeSetIds.empty()) {
        return ResultType::ok(std::move(byChangeSet));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    for (const auto& chunk : dedupedIdChunks(changeSetIds)) {
        std::string placeholders(chunk.size() * 2 - 1, ',');
        for (std::size_t i = 0; i < chunk.size(); ++i) placeholders[i * 2] = '?';

        Statement stmt;
        const std::string sql =
            "SELECT change_set_id,code,message,change_id FROM proposal_validation_findings "
            "WHERE change_set_id IN (" + placeholders + ") ORDER BY change_set_id,ordinal ASC;";
        const auto prep = prepare(handle, sql.c_str(), stmt);
        if (!prep.success()) return ResultType::failure(*prep.error());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto bound = bindText(handle, stmt.get(), static_cast<int>(i) + 1, chunk[i]);
            if (!bound.success()) return ResultType::failure(*bound.error());
        }

        for (;;) {
            const int step = sqlite3_step(stmt.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return ResultType::failure(databaseError(
                    handle, "persistence.read_failed", "Could not read validation findings"));
            }
            const std::string changeSetId = columnText(stmt.get(), 0);
            ValidationFinding f;
            f.code = columnText(stmt.get(), 1);
            f.message = columnText(stmt.get(), 2);
            f.changeId = columnText(stmt.get(), 3);
            byChangeSet[changeSetId].push_back(std::move(f));
        }
    }
    return ResultType::ok(std::move(byChangeSet));
}

// ── SqliteChangeApprovalRepository ───────────────────────────────────────────

namespace {

std::string serializeApplyHashLog(const std::vector<ApplyHashEntry>& entries)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries) {
        array.push_back({
            {"changeId", entry.changeId},
            {"relativePath", pathToUtf8(entry.relativePath)},
            {"originalHash", entry.originalHash.value_or(std::string {})},
            {"newHash", entry.newHash.value_or(std::string {})}
        });
    }
    return array.dump();
}

std::vector<ApplyHashEntry> deserializeApplyHashLog(const std::string& json)
{
    std::vector<ApplyHashEntry> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        ApplyHashEntry entry;
        entry.changeId = item.value("changeId", std::string {});
        entry.relativePath = pathFromUtf8(item.value("relativePath", std::string {}));
        const std::string originalHash = item.value("originalHash", std::string {});
        if (!originalHash.empty()) entry.originalHash = originalHash;
        const std::string newHash = item.value("newHash", std::string {});
        if (!newHash.empty()) entry.newHash = newHash;
        result.push_back(std::move(entry));
    }
    return result;
}

common::Result<ApprovalRecord> readApprovalRecord(sqlite3_stmt* statement)
{
    ApprovalRecord record;
    record.changeSetId = columnText(statement, 0);
    record.snapshotHash = columnText(statement, 1);
    record.approvedAt = fromMilliseconds(sqlite3_column_int64(statement, 2));
    record.approvedBy = optionalText(statement, 3);
    record.appliedAt = optionalTime(statement, 4);
    record.applyResult = optionalText(statement, 5);
    record.applyErrorCode = optionalText(statement, 6);
    record.applyHashLog = deserializeApplyHashLog(columnText(statement, 7));
    record.postApplyWorkspaceFingerprint = optionalText(statement, 8);
    return common::Result<ApprovalRecord>::ok(std::move(record));
}

constexpr const char* changeSetApprovalColumns =
    "change_set_id,snapshot_hash,approved_at_ms,approved_by,"
    "applied_at_ms,apply_result,apply_error_code,apply_hash_log,"
    "post_apply_workspace_fingerprint";

} // namespace

SqliteChangeApprovalRepository::SqliteChangeApprovalRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteChangeApprovalRepository::recordApproval(
    const ApprovalRecord& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO change_set_approvals "
        "(change_set_id,snapshot_hash,approved_at_ms,approved_by) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.changeSetId),
        bindText(handle, statement.get(), 2, record.snapshotHash),
        bindInt64(handle, statement.get(), 3, toMilliseconds(record.approvedAt)),
        bindNullableText(handle, statement.get(), 4, record.approvedBy)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("proposal.duplicate_id", "Change set already has an approval record"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert approval record"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ApprovalRecord>> SqliteChangeApprovalRepository::findApproval(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetApprovalColumns
        + " FROM change_set_approvals WHERE change_set_id=?;";
    Statement statement;
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, changeSetId);
    if (!bound.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*bound.error());

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ApprovalRecord>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read approval record"));
    }
    auto record = readApprovalRecord(statement.get());
    if (!record.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*record.error());
    return common::Result<std::optional<ApprovalRecord>>::ok(std::move(record.value()));
}

common::Result<void> SqliteChangeApprovalRepository::recordApplyOutcome(
    const ChangeSetId& changeSetId,
    const std::string& result,
    const std::optional<std::string>& errorCode,
    const std::vector<ApplyHashEntry>& hashLog,
    const std::optional<std::string>& postApplyWorkspaceFingerprint)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE change_set_approvals SET applied_at_ms=?,apply_result=?,"
        "apply_error_code=?,apply_hash_log=?,post_apply_workspace_fingerprint=? "
        "WHERE change_set_id=?;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindInt64(handle, statement.get(), 1, toMilliseconds(std::chrono::system_clock::now())),
        bindText(handle, statement.get(), 2, result),
        bindNullableText(handle, statement.get(), 3, errorCode),
        bindText(handle, statement.get(), 4, serializeApplyHashLog(hashLog)),
        bindNullableText(handle, statement.get(), 5, postApplyWorkspaceFingerprint),
        bindText(handle, statement.get(), 6, changeSetId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record apply outcome"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(
            error("proposal.not_found", "No approval record exists for this change set"));
    }
    return common::Result<void>::ok();
}

// ── SqliteProposalReviewRepository ───────────────────────────────────────────

namespace {

constexpr const char* proposalReviewRejectionColumns =
    "change_set_id,rejected_at_ms,rejected_by,reason";

common::Result<ProposalReviewRejection> readProposalReviewRejection(sqlite3_stmt* statement)
{
    ProposalReviewRejection record;
    record.changeSetId = columnText(statement, 0);
    record.rejectedAt = fromMilliseconds(sqlite3_column_int64(statement, 1));
    record.rejectedBy = optionalText(statement, 2);
    record.reason = optionalText(statement, 3);
    return common::Result<ProposalReviewRejection>::ok(std::move(record));
}

} // namespace

SqliteProposalReviewRepository::SqliteProposalReviewRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteProposalReviewRepository::recordRejection(
    const ProposalReviewRejection& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO change_set_reviewer_rejections "
        "(change_set_id,rejected_at_ms,rejected_by,reason) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.changeSetId),
        bindInt64(handle, statement.get(), 2, toMilliseconds(record.rejectedAt)),
        bindNullableText(handle, statement.get(), 3, record.rejectedBy),
        bindNullableText(handle, statement.get(), 4, record.reason)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("proposal.duplicate_id",
                    "Change set already has a reviewer rejection record"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert reviewer rejection record"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposalReviewRejection>>
SqliteProposalReviewRepository::findRejection(const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + proposalReviewRejectionColumns
        + " FROM change_set_reviewer_rejections WHERE change_set_id=?;";
    Statement statement;
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, changeSetId);
    if (!bound.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*bound.error());

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ProposalReviewRejection>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProposalReviewRejection>>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read reviewer rejection record"));
    }
    auto record = readProposalReviewRejection(statement.get());
    if (!record.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*record.error());
    return common::Result<std::optional<ProposalReviewRejection>>::ok(std::move(record.value()));
}

// ── SqliteAgentToolSessionRepository ─────────────────────────────────────────

namespace {

std::string agentToolSessionStateToString(application::AgentToolSessionState s)
{
    switch (s) {
    case application::AgentToolSessionState::Active:      return "active";
    case application::AgentToolSessionState::Completed:   return "completed";
    case application::AgentToolSessionState::Interrupted: return "interrupted";
    }
    return "active";
}

std::optional<application::AgentToolSessionState> agentToolSessionStateFromString(
    std::string_view v)
{
    if (v == "active")      return application::AgentToolSessionState::Active;
    if (v == "completed")   return application::AgentToolSessionState::Completed;
    if (v == "interrupted") return application::AgentToolSessionState::Interrupted;
    return std::nullopt;
}

std::string agentToolCallStateToString(application::AgentToolCallState s)
{
    switch (s) {
    case application::AgentToolCallState::Requested:  return "requested";
    case application::AgentToolCallState::Running:    return "running";
    case application::AgentToolCallState::Completed:  return "completed";
    case application::AgentToolCallState::Failed:     return "failed";
    case application::AgentToolCallState::Interrupted:return "interrupted";
    }
    return "requested";
}

std::optional<application::AgentToolCallState> agentToolCallStateFromString(
    std::string_view v)
{
    if (v == "requested")  return application::AgentToolCallState::Requested;
    if (v == "running")    return application::AgentToolCallState::Running;
    if (v == "completed")  return application::AgentToolCallState::Completed;
    if (v == "failed")     return application::AgentToolCallState::Failed;
    if (v == "interrupted")return application::AgentToolCallState::Interrupted;
    return std::nullopt;
}

} // namespace

SqliteAgentToolSessionRepository::SqliteAgentToolSessionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::AgentToolSession>
SqliteAgentToolSessionRepository::insertSession(application::AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_tool_sessions
              (id,run_id,provider_id,model_id,state,
               total_turns,total_calls,total_returned_bytes,
               created_at_ms,finished_at_ms)
              VALUES (?,?,?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentToolSession>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,session.sessionId);       !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,2,session.runId);           !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,3,session.providerId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,4,session.modelId);         !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,5,agentToolSessionStateToString(session.state)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,6,session.totalTurns);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,session.totalCalls);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,8,session.totalReturnedBytes);  !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindInt64(h,s,9,toMilliseconds(session.createdAt)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,10,session.finishedAt);     !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentToolSession>::failure(
            databaseError(h, "persistence.insert_failed", "agent_tool_sessions insert"));

    return common::Result<application::AgentToolSession>::ok(std::move(session));
}

common::Result<application::AgentToolSession>
SqliteAgentToolSessionRepository::updateSession(application::AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_sessions SET
              run_id=?,provider_id=?,model_id=?,state=?,
              total_turns=?,total_calls=?,total_returned_bytes=?,
              created_at_ms=?,finished_at_ms=?
              WHERE id=?)sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentToolSession>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,session.runId);           !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,2,session.providerId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,3,session.modelId);         !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,4,agentToolSessionStateToString(session.state)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,5,session.totalTurns);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,6,session.totalCalls);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,session.totalReturnedBytes);  !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindInt64(h,s,8,toMilliseconds(session.createdAt)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,9,session.finishedAt);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,10,session.sessionId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentToolSession>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_sessions update"));

    if (sqlite3_changes(h) == 0)
        return common::Result<application::AgentToolSession>::failure(
            error("agent_session.not_found", "No agent_tool_session row with that id"));

    return common::Result<application::AgentToolSession>::ok(std::move(session));
}

namespace {
application::AgentToolSession readAgentToolSession(sqlite3_stmt* s)
{
    application::AgentToolSession sess;
    sess.sessionId          = columnText(s, 0);
    sess.runId              = columnText(s, 1);
    sess.providerId         = columnText(s, 2);
    sess.modelId            = columnText(s, 3);
    const auto state        = agentToolSessionStateFromString(columnText(s, 4));
    sess.state              = state.value_or(application::AgentToolSessionState::Active);
    sess.totalTurns         = static_cast<std::size_t>(sqlite3_column_int64(s, 5));
    sess.totalCalls         = static_cast<std::size_t>(sqlite3_column_int64(s, 6));
    sess.totalReturnedBytes = static_cast<std::size_t>(sqlite3_column_int64(s, 7));
    sess.createdAt          = fromMilliseconds(sqlite3_column_int64(s, 8));
    sess.finishedAt         = optionalTime(s, 9);
    return sess;
}
} // namespace

common::Result<std::optional<application::AgentToolSession>>
SqliteAgentToolSessionRepository::findSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,run_id,provider_id,model_id,state,
                     total_turns,total_calls,total_returned_bytes,
                     created_at_ms,finished_at_ms
              FROM agent_tool_sessions WHERE id=?)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::optional<application::AgentToolSession>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::optional<application::AgentToolSession>>::failure(*r.error());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
        return common::Result<std::optional<application::AgentToolSession>>::ok(std::nullopt);
    if (rc != SQLITE_ROW)
        return common::Result<std::optional<application::AgentToolSession>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_sessions findSession"));

    return common::Result<std::optional<application::AgentToolSession>>::ok(
        readAgentToolSession(stmt.get()));
}

common::Result<std::vector<application::AgentToolSession>>
SqliteAgentToolSessionRepository::listForRun(const std::string& runId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,run_id,provider_id,model_id,state,
                     total_turns,total_calls,total_returned_bytes,
                     created_at_ms,finished_at_ms
              FROM agent_tool_sessions WHERE run_id=? ORDER BY created_at_ms ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentToolSession>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, runId); !r.success())
        return common::Result<std::vector<application::AgentToolSession>>::failure(*r.error());

    std::vector<application::AgentToolSession> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
        result.push_back(readAgentToolSession(stmt.get()));

    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentToolSession>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_sessions listForRun"));

    return common::Result<std::vector<application::AgentToolSession>>::ok(std::move(result));
}

common::Result<application::AgentProviderTurn>
SqliteAgentToolSessionRepository::insertTurn(application::AgentProviderTurn turn)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_provider_turns
              (id,session_id,turn_index,finish_reason,
               input_tokens,output_tokens,call_count,created_at_ms)
              VALUES (?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentProviderTurn>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,turn.turnId);              !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindText(h,s,2,turn.sessionId);           !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,3,turn.turnIndex);       !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindText(h,s,4,turn.finishReason);        !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,5,static_cast<std::int64_t>(turn.inputTokens));  !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,6,static_cast<std::int64_t>(turn.outputTokens)); !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,turn.callCount);       !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,8,toMilliseconds(turn.createdAt)); !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentProviderTurn>::failure(
            databaseError(h, "persistence.insert_failed", "agent_provider_turns insert"));

    return common::Result<application::AgentProviderTurn>::ok(std::move(turn));
}

common::Result<std::vector<application::AgentProviderTurn>>
SqliteAgentToolSessionRepository::listTurns(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,turn_index,finish_reason,
                     input_tokens,output_tokens,call_count,created_at_ms
              FROM agent_provider_turns WHERE session_id=? ORDER BY turn_index ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(*r.error());

    std::vector<application::AgentProviderTurn> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        application::AgentProviderTurn turn;
        turn.turnId       = columnText(stmt.get(), 0);
        turn.sessionId    = columnText(stmt.get(), 1);
        turn.turnIndex    = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 2));
        turn.finishReason = columnText(stmt.get(), 3);
        turn.inputTokens  = static_cast<int>(sqlite3_column_int64(stmt.get(), 4));
        turn.outputTokens = static_cast<int>(sqlite3_column_int64(stmt.get(), 5));
        turn.callCount    = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 6));
        turn.createdAt    = fromMilliseconds(sqlite3_column_int64(stmt.get(), 7));
        result.push_back(std::move(turn));
    }
    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(
            databaseError(h, "persistence.query_failed", "agent_provider_turns listTurns"));

    return common::Result<std::vector<application::AgentProviderTurn>>::ok(std::move(result));
}

common::Result<std::size_t> SqliteAgentToolSessionRepository::interruptActiveSessions()
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        "UPDATE agent_tool_sessions SET state='interrupted',finished_at_ms=? WHERE state='active'",
        stmt);
    if (!prepared.success())
        return common::Result<std::size_t>::failure(*prepared.error());

    if (auto r = bindInt64(database_->impl_->handle, stmt.get(), 1, nowMs); !r.success())
        return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(database_->impl_->handle,
                "persistence.update_failed", "agent_tool_sessions interruptActiveSessions"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(database_->impl_->handle)));
}

// ── SqliteAgentToolAuditRepository ───────────────────────────────────────────

SqliteAgentToolAuditRepository::SqliteAgentToolAuditRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteAgentToolAuditRepository::insert(
    const application::AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_tool_calls
              (id,session_id,run_id,provider_call_id,tool_name,state,
               arguments_sha256,result_sha256,result_artifact_id,
               error_code,returned_bytes,truncated,created_at_ms,finished_at_ms)
              VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,record.auditId);                              !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,2,record.sessionId);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,3,record.runId);                                !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,4,record.providerCallId);                       !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,5,record.toolName);                             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,6,agentToolCallStateToString(record.state));    !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,7,record.argumentsSha256);                      !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,8,record.resultSha256);                         !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindNullableText(h,s,9,record.resultArtifactId);             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,10,record.errorCode);                           !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,11,static_cast<std::uint64_t>(record.returnedBytes)); !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,12,record.truncated ? 1 : 0);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,13,toMilliseconds(record.createdAt));          !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,14,record.finishedAt);                  !r.success()) return common::Result<void>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.insert_failed", "agent_tool_calls insert"));

    return common::Result<void>::ok();
}

common::Result<void> SqliteAgentToolAuditRepository::update(
    const application::AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls SET
              session_id=?,run_id=?,provider_call_id=?,tool_name=?,state=?,
              arguments_sha256=?,result_sha256=?,result_artifact_id=?,
              error_code=?,returned_bytes=?,truncated=?,
              created_at_ms=?,finished_at_ms=?
              WHERE id=?)sql",
        stmt);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,record.sessionId);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,2,record.runId);                                !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,3,record.providerCallId);                       !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,4,record.toolName);                             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,5,agentToolCallStateToString(record.state));    !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,6,record.argumentsSha256);                      !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,7,record.resultSha256);                         !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindNullableText(h,s,8,record.resultArtifactId);             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,9,record.errorCode);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,10,static_cast<std::uint64_t>(record.returnedBytes)); !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,11,record.truncated ? 1 : 0);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,12,toMilliseconds(record.createdAt));          !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,13,record.finishedAt);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,14,record.auditId);                             !r.success()) return common::Result<void>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_calls update"));

    if (sqlite3_changes(h) == 0)
        return common::Result<void>::failure(
            error("agent_call.not_found", "No agent_tool_calls row with that id"));

    return common::Result<void>::ok();
}

namespace {
application::AgentToolAuditRecord readAgentToolAuditRecord(sqlite3_stmt* s)
{
    application::AgentToolAuditRecord r;
    r.auditId          = columnText(s, 0);
    r.sessionId        = columnText(s, 1);
    r.runId            = columnText(s, 2);
    r.providerCallId   = columnText(s, 3);
    r.toolName         = columnText(s, 4);
    const auto state   = agentToolCallStateFromString(columnText(s, 5));
    r.state            = state.value_or(application::AgentToolCallState::Requested);
    r.argumentsSha256  = columnText(s, 6);
    r.resultSha256     = columnText(s, 7);
    r.resultArtifactId = optionalText(s, 8);
    r.errorCode        = columnText(s, 9);
    r.returnedBytes    = static_cast<std::size_t>(sqlite3_column_int64(s, 10));
    r.truncated        = sqlite3_column_int64(s, 11) != 0;
    r.createdAt        = fromMilliseconds(sqlite3_column_int64(s, 12));
    r.finishedAt       = optionalTime(s, 13);
    return r;
}
} // namespace

common::Result<std::optional<application::AgentToolAuditRecord>>
SqliteAgentToolAuditRepository::find(const std::string& auditId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,run_id,provider_call_id,tool_name,state,
                     arguments_sha256,result_sha256,result_artifact_id,
                     error_code,returned_bytes,truncated,created_at_ms,finished_at_ms
              FROM agent_tool_calls WHERE id=?)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, auditId); !r.success())
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(*r.error());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
        return common::Result<std::optional<application::AgentToolAuditRecord>>::ok(std::nullopt);
    if (rc != SQLITE_ROW)
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_calls find"));

    return common::Result<std::optional<application::AgentToolAuditRecord>>::ok(
        readAgentToolAuditRecord(stmt.get()));
}

common::Result<std::vector<application::AgentToolAuditRecord>>
SqliteAgentToolAuditRepository::listForSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,run_id,provider_call_id,tool_name,state,
                     arguments_sha256,result_sha256,result_artifact_id,
                     error_code,returned_bytes,truncated,created_at_ms,finished_at_ms
              FROM agent_tool_calls WHERE session_id=? ORDER BY created_at_ms ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(*r.error());

    std::vector<application::AgentToolAuditRecord> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
        result.push_back(readAgentToolAuditRecord(stmt.get()));

    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_calls listForSession"));

    return common::Result<std::vector<application::AgentToolAuditRecord>>::ok(std::move(result));
}

common::Result<std::size_t> SqliteAgentToolAuditRepository::interruptRunning(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls
              SET state='interrupted', finished_at_ms=?
              WHERE session_id=? AND state IN ('requested','running'))sql",
        stmt);
    if (!prepared.success()) return common::Result<std::size_t>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindInt64(h, stmt.get(), 1, nowMs);       !r.success()) return common::Result<std::size_t>::failure(*r.error());
    if (auto r = bindText(h, stmt.get(), 2, sessionId);    !r.success()) return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_calls interruptRunning"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(h)));
}

common::Result<std::size_t> SqliteAgentToolAuditRepository::interruptAllRunning()
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls
              SET state='interrupted', finished_at_ms=?
              WHERE state IN ('requested','running'))sql",
        stmt);
    if (!prepared.success()) return common::Result<std::size_t>::failure(*prepared.error());

    if (auto r = bindInt64(database_->impl_->handle, stmt.get(), 1, nowMs); !r.success())
        return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(database_->impl_->handle,
                "persistence.update_failed", "agent_tool_calls interruptAllRunning"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(database_->impl_->handle)));
}

} // namespace ai::infrastructure
