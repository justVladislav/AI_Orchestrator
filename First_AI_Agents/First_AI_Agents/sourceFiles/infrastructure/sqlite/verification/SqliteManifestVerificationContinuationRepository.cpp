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



std::string serializeVerificationAttributions(
    const std::vector<application::ManifestVerificationFindingAttribution>& findings)
{
    nlohmann::json values = nlohmann::json::array();
    for (const auto& finding : findings) {
        nlohmann::json value {
            {"diagnosticCode", finding.diagnosticCode},
            {"requirementId", finding.requirementId},
            {"relativePath", finding.relativePath},
            {"line", finding.line},
            {"column", finding.column},
            {"tag", finding.tag},
            {"kind", application::toString(finding.kind)}};
        value["ownerTaskId"] = finding.ownerTaskId.has_value()
            ? nlohmann::json(*finding.ownerTaskId) : nlohmann::json(nullptr);
        value["ownerProposalId"] = finding.ownerProposalId.has_value()
            ? nlohmann::json(*finding.ownerProposalId) : nlohmann::json(nullptr);
        values.push_back(std::move(value));
    }
    return values.dump();
}

common::Result<std::vector<application::ManifestVerificationFindingAttribution>>
deserializeVerificationAttributions(const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.size() > 256) {
        return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::failure(
            error("manifest_verification_attribution.corrupt_findings",
                "Stored verification attribution findings are invalid"));
    }
    std::vector<application::ManifestVerificationFindingAttribution> result;
    for (const auto& value : parsed) {
        if (!value.is_object() || !value.contains("diagnosticCode")
            || !value["diagnosticCode"].is_string() || !value.contains("requirementId")
            || !value["requirementId"].is_string() || !value.contains("relativePath")
            || !value["relativePath"].is_string() || !value.contains("line")
            || !value["line"].is_number_integer() || !value.contains("column")
            || !value["column"].is_number_integer() || !value.contains("tag")
            || !value["tag"].is_string() || !value.contains("kind")
            || !value["kind"].is_string()) {
            return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::failure(
                error("manifest_verification_attribution.corrupt_findings",
                    "Stored verification attribution findings are invalid"));
        }
        const auto kind = application::manifestVerificationFindingAttributionKindFromString(
            value["kind"].get<std::string>());
        if (!kind.has_value()) {
            return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::failure(
                error("manifest_verification_attribution.corrupt_findings",
                    "Stored verification attribution kind is invalid"));
        }
        application::ManifestVerificationFindingAttribution finding;
        finding.diagnosticCode = value["diagnosticCode"].get<std::string>();
        finding.requirementId = value["requirementId"].get<std::string>();
        finding.relativePath = value["relativePath"].get<std::string>();
        finding.line = value["line"].get<int>();
        finding.column = value["column"].get<int>();
        finding.tag = value["tag"].get<std::string>();
        finding.kind = *kind;
        if (value.contains("ownerTaskId") && value["ownerTaskId"].is_string())
            finding.ownerTaskId = value["ownerTaskId"].get<std::string>();
        if (value.contains("ownerProposalId") && value["ownerProposalId"].is_string())
            finding.ownerProposalId = value["ownerProposalId"].get<std::string>();
        result.push_back(std::move(finding));
    }
    return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::ok(
        std::move(result));
}

common::Result<application::ManifestVerificationContinuationInput> readContinuationInput(
    sqlite3_stmt* statement)
{
    application::ManifestVerificationContinuationInput input;
    input.sourceRunId = columnText(statement, 0);
    input.childRunId = columnText(statement, 1);
    input.schemaVersion = columnText(statement, 2);
    input.sourceProjectId = columnText(statement, 3);
    input.attributionId = columnText(statement, 4);
    input.baselineFingerprint = columnText(statement, 5);
    input.sourceWorkspaceIdentity = columnText(statement, 6);
    input.sourceOverlayFingerprint = columnText(statement, 7);
    const auto ids = nlohmann::json::parse(columnText(statement, 8), nullptr, false);
    const auto hashes = nlohmann::json::parse(columnText(statement, 9), nullptr, false);
    if (ids.is_discarded() || hashes.is_discarded() || !ids.is_array() || !hashes.is_array()) {
        return common::Result<application::ManifestVerificationContinuationInput>::failure({
            "manifest_verification_continuation.corrupt_input",
            "Stored continuation proposal identity is invalid"});
    }
    try {
        input.orderedProposalIds = ids.get<std::vector<std::string>>();
        input.orderedProposalContentHashes = hashes.get<std::vector<std::string>>();
    } catch (...) {
        return common::Result<application::ManifestVerificationContinuationInput>::failure({
            "manifest_verification_continuation.corrupt_input",
            "Stored continuation proposal identity is invalid"});
    }
    input.inputFingerprint = columnText(statement, 10);
    const auto plannerRequests = sqlite3_column_int64(statement, 11);
    if (plannerRequests < 0) return common::Result<application::ManifestVerificationContinuationInput>::failure({
        "manifest_verification_continuation.corrupt_input",
        "Stored continuation request count is invalid"});
    input.plannerRequests = static_cast<std::uint64_t>(plannerRequests);
    input.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    const auto valid = application::validateManifestVerificationContinuationInput(input);
    if (!valid.success()) return common::Result<application::ManifestVerificationContinuationInput>::failure({
        "manifest_verification_continuation.corrupt_input",
        "Stored continuation input violates its typed contract"});
    return common::Result<application::ManifestVerificationContinuationInput>::ok(std::move(input));
}

constexpr const char* continuationInputColumns =
    "source_run_id,child_run_id,schema_version,source_project_id,attribution_id,"
    "baseline_fingerprint,source_workspace_identity,source_overlay_fingerprint,"
    "proposal_ids_json,proposal_hashes_json,input_fingerprint,planner_requests,created_at_ms";

} // namespace

SqliteManifestVerificationContinuationRepository::SqliteManifestVerificationContinuationRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestVerificationContinuationRepository::insert(
    application::ManifestVerificationContinuationCheckpoint checkpoint)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_continuations "
        "(source_run_id,id,schema_version,source_project_id,status,denial_code,"
        "denial_message,created_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, checkpoint.sourceRunId),
        bindText(handle, statement.get(), 2, checkpoint.id),
        bindText(handle, statement.get(), 3, checkpoint.schemaVersion),
        bindText(handle, statement.get(), 4, checkpoint.sourceProjectId),
        bindText(handle, statement.get(), 5,
            application::toString(checkpoint.status)),
        bindText(handle, statement.get(), 6, checkpoint.denialCode),
        bindText(handle, statement.get(), 7, checkpoint.denialMessage),
        bindInt64(handle, statement.get(), 8, toMilliseconds(checkpoint.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "manifest_verification_continuation.already_exists",
                "A verification-continuation checkpoint already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "manifest_verification_continuation.write_failed",
            "Could not persist the verification-continuation checkpoint"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>
SqliteManifestVerificationContinuationRepository::findBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,source_project_id,status,denial_code,denial_message,"
        "created_at_ms FROM manifest_verification_continuations WHERE source_run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success()) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(*bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(databaseError(handle, "manifest_verification_continuation.read_failed",
                "Could not read the verification-continuation checkpoint"));
    }
    application::ManifestVerificationContinuationCheckpoint checkpoint;
    checkpoint.sourceRunId = sourceRunId;
    checkpoint.id = columnText(statement.get(), 0);
    checkpoint.schemaVersion = columnText(statement.get(), 1);
    checkpoint.sourceProjectId = columnText(statement.get(), 2);
    const auto status =
        application::manifestVerificationContinuationStatusFromString(columnText(statement.get(), 3));
    if (!status.has_value()) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(error("manifest_verification_continuation.corrupt_status",
                "The stored verification-continuation status is invalid"));
    }
    checkpoint.status = *status;
    checkpoint.denialCode = columnText(statement.get(), 4);
    checkpoint.denialMessage = columnText(statement.get(), 5);
    checkpoint.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
    return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
        ok(checkpoint);
}

common::Result<void> SqliteManifestVerificationContinuationRepository::insertAttribution(
    application::ManifestVerificationAttributionCheckpoint checkpoint)
{
    const auto valid = application::validateManifestVerificationAttributionCheckpoint(checkpoint);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_attributions "
        "(source_run_id,id,schema_version,source_project_id,status,block_code,block_message,"
        "baseline_fingerprint,source_overlay_fingerprint,findings_json,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, checkpoint.sourceRunId),
        bindText(handle, statement.get(), 2, checkpoint.id),
        bindText(handle, statement.get(), 3, checkpoint.schemaVersion),
        bindText(handle, statement.get(), 4, checkpoint.sourceProjectId),
        bindText(handle, statement.get(), 5, application::toString(checkpoint.status)),
        bindText(handle, statement.get(), 6, checkpoint.blockCode),
        bindText(handle, statement.get(), 7, checkpoint.blockMessage),
        bindText(handle, statement.get(), 8, checkpoint.baselineFingerprint),
        bindText(handle, statement.get(), 9, checkpoint.sourceOverlayFingerprint),
        bindText(handle, statement.get(), 10, serializeVerificationAttributions(checkpoint.findings)),
        bindInt64(handle, statement.get(), 11, toMilliseconds(checkpoint.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure({
                "manifest_verification_attribution.already_exists",
                "A verification-attribution checkpoint already exists for this Run"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_attribution.write_failed",
            "Could not persist the verification-attribution checkpoint"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>
SqliteManifestVerificationContinuationRepository::findAttributionBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,source_project_id,status,block_code,block_message,"
        "baseline_fingerprint,source_overlay_fingerprint,findings_json,created_at_ms "
        "FROM manifest_verification_attributions WHERE source_run_id=?;", statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(
        databaseError(handle, "manifest_verification_attribution.read_failed",
            "Could not read the verification-attribution checkpoint"));
    application::ManifestVerificationAttributionCheckpoint checkpoint;
    checkpoint.sourceRunId = sourceRunId;
    checkpoint.id = columnText(statement.get(), 0);
    checkpoint.schemaVersion = columnText(statement.get(), 1);
    checkpoint.sourceProjectId = columnText(statement.get(), 2);
    const auto status = application::manifestVerificationAttributionStatusFromString(columnText(statement.get(), 3));
    if (!status.has_value() || checkpoint.schemaVersion != application::kManifestVerificationAttributionSchemaVersion) {
        return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure({
            "manifest_verification_attribution.corrupt_identity",
            "Stored verification-attribution identity is invalid"});
    }
    checkpoint.status = *status;
    checkpoint.blockCode = columnText(statement.get(), 4);
    checkpoint.blockMessage = columnText(statement.get(), 5);
    checkpoint.baselineFingerprint = columnText(statement.get(), 6);
    checkpoint.sourceOverlayFingerprint = columnText(statement.get(), 7);
    const auto findings = deserializeVerificationAttributions(columnText(statement.get(), 8));
    if (!findings.success()) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(*findings.error());
    checkpoint.findings = std::move(findings.value());
    checkpoint.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 9));
    const auto valid = application::validateManifestVerificationAttributionCheckpoint(checkpoint);
    if (!valid.success()) {
        return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(
            {"manifest_verification_attribution.corrupt_identity",
                "Stored verification-attribution data violates its typed contract"});
    }
    return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::ok(std::move(checkpoint));
}

common::Result<void> SqliteManifestVerificationContinuationRepository::insertInput(
    application::ManifestVerificationContinuationInput input)
{
    const auto valid = application::validateManifestVerificationContinuationInput(input);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_continuation_inputs "
        "(source_run_id,child_run_id,schema_version,source_project_id,attribution_id,"
        "baseline_fingerprint,source_workspace_identity,source_overlay_fingerprint,"
        "proposal_ids_json,proposal_hashes_json,input_fingerprint,planner_requests,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const std::string ids = nlohmann::json(input.orderedProposalIds).dump();
    const std::string hashes = nlohmann::json(input.orderedProposalContentHashes).dump();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, input.sourceRunId),
        bindText(handle, statement.get(), 2, input.childRunId),
        bindText(handle, statement.get(), 3, input.schemaVersion),
        bindText(handle, statement.get(), 4, input.sourceProjectId),
        bindText(handle, statement.get(), 5, input.attributionId),
        bindText(handle, statement.get(), 6, input.baselineFingerprint),
        bindText(handle, statement.get(), 7, input.sourceWorkspaceIdentity),
        bindText(handle, statement.get(), 8, input.sourceOverlayFingerprint),
        bindText(handle, statement.get(), 9, ids),
        bindText(handle, statement.get(), 10, hashes),
        bindText(handle, statement.get(), 11, input.inputFingerprint),
        bindInt64(handle, statement.get(), 12, static_cast<std::int64_t>(input.plannerRequests)),
        bindInt64(handle, statement.get(), 13, toMilliseconds(input.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            // Both immutable identities are unique. SQLite is free to report the
            // child_run_id index first when the exact same row is inserted again,
            // so the extended error code alone cannot distinguish idempotent
            // source duplication from a child-id collision. Resolve that ordering
            // ambiguity against the authoritative table while holding the same
            // repository lock.
            Statement existingSource;
            const auto sourcePrepared = prepare(handle,
                "SELECT 1 FROM manifest_verification_continuation_inputs "
                "WHERE source_run_id=? LIMIT 1;", existingSource);
            if (!sourcePrepared.success())
                return common::Result<void>::failure(*sourcePrepared.error());
            const auto sourceBound = bindText(handle, existingSource.get(), 1, input.sourceRunId);
            if (!sourceBound.success())
                return common::Result<void>::failure(*sourceBound.error());
            const bool sourceAlreadyExists = sqlite3_step(existingSource.get()) == SQLITE_ROW;
            return common::Result<void>::failure({
                sourceAlreadyExists
                    ? "manifest_verification_continuation.input_already_exists"
                    : "manifest_verification_continuation.child_already_reserved",
                "A continuation input or child reservation already exists"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_continuation.input_write_failed",
            "Could not persist the continuation input"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationContinuationInput>>
SqliteManifestVerificationContinuationRepository::findInputBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + continuationInputColumns
        + " FROM manifest_verification_continuation_inputs WHERE source_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(
        databaseError(handle, "manifest_verification_continuation.input_read_failed", "Could not read continuation input"));
    auto input = readContinuationInput(statement.get());
    if (!input.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*input.error());
    return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::move(input.value()));
}

common::Result<std::optional<application::ManifestVerificationContinuationInput>>
SqliteManifestVerificationContinuationRepository::findInputByChildRun(
    const application::RunId& childRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + continuationInputColumns
        + " FROM manifest_verification_continuation_inputs WHERE child_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, childRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(
        databaseError(handle, "manifest_verification_continuation.input_read_failed", "Could not read continuation input"));
    auto input = readContinuationInput(statement.get());
    if (!input.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*input.error());
    return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::move(input.value()));
}

common::Result<std::vector<application::ManifestVerificationContinuationInput>>
SqliteManifestVerificationContinuationRepository::listInputs() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + continuationInputColumns
        + " FROM manifest_verification_continuation_inputs ORDER BY created_at_ms,source_run_id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::failure(*prepared.error());
    std::vector<application::ManifestVerificationContinuationInput> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::failure(
            databaseError(handle, "manifest_verification_continuation.input_read_failed", "Could not list continuation inputs"));
        auto input = readContinuationInput(statement.get());
        if (!input.success()) return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::failure(*input.error());
        result.push_back(std::move(input.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::ok(std::move(result));
}

} // namespace ai::infrastructure
