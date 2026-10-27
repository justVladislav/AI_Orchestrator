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

} // namespace ai::infrastructure
