#include "ai/infrastructure/SqliteRepositories.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "../planning/SqliteRunDraftGroupStorage.hpp"

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

SqliteRunWarningRepository::SqliteRunWarningRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<bool> SqliteRunWarningRepository::recordWarning(application::RunWarning warning)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Deliberately a plain INSERT: OR IGNORE would also swallow a CHECK violation (an empty or oversized message) and a
    // missing Run, and the caller has to hear about those.
    const auto prepared = prepare(handle,
        "INSERT INTO run_warnings (id,run_id,stage_id,code,message,artifact_name,created_at_ms) VALUES (?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<bool>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, warning.id),
        bindText(handle, statement.get(), 2, warning.runId),
        bindNullableText(handle, statement.get(), 3, warning.stageId),
        bindText(handle, statement.get(), 4, warning.code),
        bindText(handle, statement.get(), 5, warning.message),
        bindText(handle, statement.get(), 6, warning.artifactName),
        bindInt64(handle, statement.get(), 7, toMilliseconds(warning.createdAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<bool>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        // The identity index is the only UNIQUE constraint besides the id (a fresh UUID): the same failure again.
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<bool>::ok(false);
        }
        return common::Result<bool>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record run warning"));
    }
    return common::Result<bool>::ok(true);
}

common::Result<std::vector<application::RunWarning>> SqliteRunWarningRepository::listWarnings(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,run_id,stage_id,code,message,artifact_name,created_at_ms FROM run_warnings "
        "WHERE run_id=? ORDER BY created_at_ms,id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWarning>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<application::RunWarning>>::failure(*bound.error());

    std::vector<application::RunWarning> warnings;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWarning>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run warnings"));
        }
        application::RunWarning warning;
        warning.id = columnText(statement.get(), 0);
        warning.runId = columnText(statement.get(), 1);
        warning.stageId = optionalText(statement.get(), 2);
        warning.code = columnText(statement.get(), 3);
        warning.message = columnText(statement.get(), 4);
        warning.artifactName = columnText(statement.get(), 5);
        warning.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        warnings.push_back(std::move(warning));
    }
    return common::Result<std::vector<application::RunWarning>>::ok(std::move(warnings));
}

} // namespace ai::infrastructure
