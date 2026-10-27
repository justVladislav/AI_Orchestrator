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
#include "ai/application/planning/RunDraftGroup.hpp"
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

SqliteManifestVerificationBaselineNormalizationRepository::
SqliteManifestVerificationBaselineNormalizationRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestVerificationBaselineNormalizationRepository::insert(
    application::ManifestVerificationBaselineNormalization normalization)
{
    const auto valid = application::validateManifestVerificationBaselineNormalization(normalization);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_baseline_normalizations "
        "(source_run_id,id,schema_version,policy_id,durable_baseline_fingerprint,"
        "normalized_baseline_fingerprint,excluded_prefixes_json,normalized_paths_json,"
        "created_at_ms) VALUES (?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, normalization.sourceRunId),
        bindText(handle, statement.get(), 2, normalization.id),
        bindText(handle, statement.get(), 3, normalization.schemaVersion),
        bindText(handle, statement.get(), 4, normalization.policyId),
        bindText(handle, statement.get(), 5, normalization.durableBaselineFingerprint),
        bindText(handle, statement.get(), 6, normalization.normalizedBaselineFingerprint),
        bindText(handle, statement.get(), 7, nlohmann::json(normalization.excludedPrefixes).dump()),
        bindText(handle, statement.get(), 8, nlohmann::json(normalization.normalizedPaths).dump()),
        bindInt64(handle, statement.get(), 9, toMilliseconds(normalization.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure({
                "manifest_verification_baseline_normalization.already_exists",
                "A baseline-normalization proof already exists for this source Run"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_baseline_normalization.write_failed",
            "Could not persist the baseline-normalization proof"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
SqliteManifestVerificationBaselineNormalizationRepository::findBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,policy_id,durable_baseline_fingerprint,"
        "normalized_baseline_fingerprint,excluded_prefixes_json,normalized_paths_json,"
        "created_at_ms FROM manifest_verification_baseline_normalizations WHERE source_run_id=?;",
        statement);
    if (!prepared.success())
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success())
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure(databaseError(handle,
                "manifest_verification_baseline_normalization.read_failed",
                "Could not read the baseline-normalization proof"));
    application::ManifestVerificationBaselineNormalization normalization;
    normalization.sourceRunId = sourceRunId;
    normalization.id = columnText(statement.get(), 0);
    normalization.schemaVersion = columnText(statement.get(), 1);
    normalization.policyId = columnText(statement.get(), 2);
    normalization.durableBaselineFingerprint = columnText(statement.get(), 3);
    normalization.normalizedBaselineFingerprint = columnText(statement.get(), 4);
    try {
        normalization.excludedPrefixes =
            nlohmann::json::parse(columnText(statement.get(), 5))
                .get<std::vector<std::string>>();
        normalization.normalizedPaths =
            nlohmann::json::parse(columnText(statement.get(), 6))
                .get<std::vector<std::string>>();
    } catch (const std::exception&) {
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure({"manifest_verification_baseline_normalization.corrupt_identity",
                "Stored baseline-normalization path lists are invalid"});
    }
    normalization.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    const auto valid = application::validateManifestVerificationBaselineNormalization(normalization);
    if (!valid.success()) {
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure({"manifest_verification_baseline_normalization.corrupt_identity",
                "Stored baseline-normalization data violates its typed contract"});
    }
    return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>::ok(
        std::move(normalization));
}

} // namespace ai::infrastructure
