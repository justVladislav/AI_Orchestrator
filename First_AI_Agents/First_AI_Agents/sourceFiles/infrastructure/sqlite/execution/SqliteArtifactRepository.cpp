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
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

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

// Column order for readArtifact/bindArtifact/artifactColumns below:
// id, run_id, stage_id, kind, name, media_type, relative_path, byte_size, sha256, created_at_ms
common::Result<Artifact> readArtifact(sqlite3_stmt* statement)
{
    const auto kind = application::artifactKindFromString(columnText(statement, 3));
    if (!kind.has_value()) {
        return common::Result<Artifact>::failure(
            error("persistence.invalid_data", "Artifact row contains an unknown kind"));
    }
    const std::int64_t byteSize = sqlite3_column_int64(statement, 7);
    if (byteSize < 0) {
        return common::Result<Artifact>::failure(
            error("persistence.invalid_data", "Artifact row contains a negative byte size"));
    }

    Artifact artifact;
    artifact.id = columnText(statement, 0);
    artifact.runId = columnText(statement, 1);
    artifact.stageId = optionalText(statement, 2);
    artifact.kind = *kind;
    artifact.name = columnText(statement, 4);
    artifact.mediaType = columnText(statement, 5);
    artifact.relativePath = pathFromUtf8(columnText(statement, 6));
    artifact.byteSize = static_cast<std::uint64_t>(byteSize);
    artifact.sha256 = columnText(statement, 8);
    artifact.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 9));
    return common::Result<Artifact>::ok(std::move(artifact));
}

common::Result<void> bindArtifact(sqlite3* handle, sqlite3_stmt* statement, const Artifact& artifact)
{
    const std::string relativePath = pathToUtf8(artifact.relativePath);
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, artifact.id),
        bindText(handle, statement, 2, artifact.runId),
        bindNullableText(handle, statement, 3, artifact.stageId),
        bindText(handle, statement, 4, application::toString(artifact.kind)),
        bindText(handle, statement, 5, artifact.name),
        bindText(handle, statement, 6, artifact.mediaType),
        bindText(handle, statement, 7, relativePath),
        bindUnsigned(handle, statement, 8, artifact.byteSize),
        bindText(handle, statement, 9, artifact.sha256),
        bindInt64(handle, statement, 10, toMilliseconds(artifact.createdAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

constexpr const char* artifactColumns =
    "id, run_id, stage_id, kind, name, media_type, relative_path, byte_size, sha256, created_at_ms";

} // namespace

SqliteArtifactRepository::SqliteArtifactRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Artifact> SqliteArtifactRepository::insertArtifact(Artifact artifact)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO artifacts (id,run_id,stage_id,kind,name,media_type,relative_path,byte_size,"
        "sha256,created_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Artifact>::failure(*prepared.error());
    const auto bound = bindArtifact(handle, statement.get(), artifact);
    if (!bound.success()) return common::Result<Artifact>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Artifact>::failure(
                error("artifact.duplicate_id", "Artifact id already exists"));
        }
        return common::Result<Artifact>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert artifact"));
    }
    return common::Result<Artifact>::ok(std::move(artifact));
}

common::Result<std::optional<Artifact>> SqliteArtifactRepository::findArtifact(
    const application::ArtifactId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + artifactColumns + " FROM artifacts WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Artifact>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Artifact>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Artifact>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Artifact>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read artifact"));
    }
    const auto artifact = readArtifact(statement.get());
    if (!artifact.success()) return common::Result<std::optional<Artifact>>::failure(*artifact.error());
    return common::Result<std::optional<Artifact>>::ok(artifact.value());
}

common::Result<std::vector<Artifact>> SqliteArtifactRepository::listArtifacts(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Deterministic order matches the IArtifactRepository contract every
    // adapter (including InMemoryArtifactRepository) must honor.
    const std::string sql = std::string("SELECT ") + artifactColumns
        + " FROM artifacts WHERE run_id=? ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Artifact>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<Artifact>>::failure(*bound.error());

    std::vector<Artifact> artifacts;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Artifact>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list artifacts"));
        }
        auto artifact = readArtifact(statement.get());
        if (!artifact.success()) return common::Result<std::vector<Artifact>>::failure(*artifact.error());
        artifacts.push_back(artifact.value());
    }
    return common::Result<std::vector<Artifact>>::ok(std::move(artifacts));
}

} // namespace ai::infrastructure
