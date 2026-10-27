#include "ai/infrastructure/SqliteRepositories.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "../planning/SqliteRunDraftGroupStorage.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/conversations/ChatProjectContext.hpp"
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

std::string serializeNoChangesCriterionEvidence(
    const std::vector<application::ManifestWorkerCriterionEvidence>& evidence)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : evidence) {
        array.push_back(nlohmann::json {
            {"requirementId", item.requirementId},
            {"inspectedPath", item.inspectedPath},
            {"observation", item.observation}});
    }
    return array.dump();
}

std::vector<application::ManifestWorkerCriterionEvidence> deserializeNoChangesCriterionEvidence(
    const std::string& json)
{
    std::vector<application::ManifestWorkerCriterionEvidence> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkerCriterionEvidence evidence;
        if (item.contains("requirementId") && item["requirementId"].is_string())
            evidence.requirementId = item["requirementId"].get<std::string>();
        if (item.contains("inspectedPath") && item["inspectedPath"].is_string())
            evidence.inspectedPath = item["inspectedPath"].get<std::string>();
        if (item.contains("observation") && item["observation"].is_string())
            evidence.observation = item["observation"].get<std::string>();
        result.push_back(std::move(evidence));
    }
    return result;
}

std::string serializeNoChangesInspectedPaths(const std::vector<std::string>& paths)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& path : paths) array.push_back(path);
    return array.dump();
}

std::vector<std::string> deserializeNoChangesInspectedPaths(const std::string& json)
{
    std::vector<std::string> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

} // namespace

SqliteManifestTaskExecutionRepository::SqliteManifestTaskExecutionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestTaskExecutionRepository::upsert(
    const application::RunId& runId, const std::string& taskId,
    const application::ManifestTaskState& state)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO manifest_task_executions "
        "(run_id,task_id,status,error_code,error_message,change_set_id,attempts,"
        "output_summary,inspected_relative_paths,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    // AG-15: same JSON-array-of-strings shape as
    // manifest_task_no_changes.inspected_relative_paths (v32).
    nlohmann::json inspectedArray = nlohmann::json::array();
    for (const auto& path : state.inspectedRelativePaths) inspectedArray.push_back(path);

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, taskId),
        bindText(handle, statement.get(), 3, application::toString(state.status)),
        bindText(handle, statement.get(), 4, state.errorCode),
        bindText(handle, statement.get(), 5, state.errorMessage),
        bindNullableText(handle, statement.get(), 6, state.changeSetId),
        bindInt64(handle, statement.get(), 7, static_cast<std::int64_t>(state.attempts)),
        bindText(handle, statement.get(), 8, state.outputSummary),
        bindText(handle, statement.get(), 9, inspectedArray.dump()),
        bindInt64(handle, statement.get(), 10, toMilliseconds(std::chrono::system_clock::now()))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_task.write_failed", "Could not persist manifest task execution state"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestTaskExecutionRecord>>
SqliteManifestTaskExecutionRepository::listForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT task_id,status,error_code,error_message,change_set_id,attempts,"
        "output_summary,inspected_relative_paths,updated_at_ms FROM manifest_task_executions "
        "WHERE run_id=? ORDER BY task_id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            *bound.error());
    }

    std::vector<application::ManifestTaskExecutionRecord> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                databaseError(handle, "manifest_task.read_failed",
                    "Could not read manifest task execution state"));
        }
        application::ManifestTaskExecutionRecord record;
        record.runId = runId;
        record.taskId = columnText(statement.get(), 0);
        record.state.status = application::manifestTaskStatusFromString(columnText(statement.get(), 1));
        record.state.errorCode = columnText(statement.get(), 2);
        record.state.errorMessage = columnText(statement.get(), 3);
        record.state.changeSetId = optionalText(statement.get(), 4);
        record.state.attempts = static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 5));
        record.state.outputSummary = columnText(statement.get(), 6);
        {
            const auto parsed = nlohmann::json::parse(
                columnText(statement.get(), 7), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                    error("manifest_task.corrupt_inspected_paths",
                        "Stored inspected-path evidence is corrupt"));
            }
            for (const auto& item : parsed) {
                if (!item.is_string()) {
                    return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                        error("manifest_task.corrupt_inspected_paths",
                            "Stored inspected-path evidence contains an invalid item"));
                }
                record.state.inspectedRelativePaths.push_back(item.get<std::string>());
            }
        }
        record.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 8));
        results.push_back(std::move(record));
    }
    return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::ok(std::move(results));
}

common::Result<void> SqliteManifestTaskExecutionRepository::upsertNoChanges(
    const application::RunId& runId, const std::string& taskId,
    const application::ManifestWorkerNoChanges& evidence)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO manifest_task_no_changes "
        "(run_id,task_id,schema_version,reason,summary,criterion_evidence,"
        "inspected_relative_paths,updated_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, taskId),
        bindText(handle, statement.get(), 3, evidence.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(evidence.reason)),
        bindText(handle, statement.get(), 5, evidence.summary),
        bindText(handle, statement.get(), 6, serializeNoChangesCriterionEvidence(evidence.criterionEvidence)),
        bindText(handle, statement.get(), 7, serializeNoChangesInspectedPaths(evidence.inspectedRelativePaths)),
        bindInt64(handle, statement.get(), 8, toMilliseconds(std::chrono::system_clock::now()))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_task.write_failed", "Could not persist manifest task no-op evidence"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestTaskNoChangesRecord>>
SqliteManifestTaskExecutionRepository::listNoChangesForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT task_id,schema_version,reason,summary,criterion_evidence,"
        "inspected_relative_paths,updated_at_ms FROM manifest_task_no_changes WHERE run_id=? "
        "ORDER BY task_id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            *bound.error());
    }

    std::vector<application::ManifestTaskNoChangesRecord> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
                databaseError(handle, "manifest_task.read_failed",
                    "Could not read manifest task no-op evidence"));
        }
        application::ManifestTaskNoChangesRecord record;
        record.runId = runId;
        record.taskId = columnText(statement.get(), 0);
        record.evidence.schemaVersion = columnText(statement.get(), 1);
        const auto reason = application::manifestWorkerNoChangesReasonFromString(
            columnText(statement.get(), 2));
        if (!reason.has_value()) {
            return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
                error("manifest_task.corrupt_no_changes", "Unknown no-op reason in stored evidence"));
        }
        record.evidence.reason = *reason;
        record.evidence.summary = columnText(statement.get(), 3);
        record.evidence.criterionEvidence =
            deserializeNoChangesCriterionEvidence(columnText(statement.get(), 4));
        record.evidence.inspectedRelativePaths =
            deserializeNoChangesInspectedPaths(columnText(statement.get(), 5));
        record.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(record));
    }
    return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::ok(std::move(results));
}

} // namespace ai::infrastructure
