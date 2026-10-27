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



constexpr const char* manualHandoffColumns =
    "id,origin_manifest_run_id,verification_anchor_run_id,schema_version,source_project_id,"
    "storage_key,workspace_identity,baseline_fingerprint,source_overlay_fingerprint,"
    "frozen_members_json,allowed_paths_json,original_snapshot_json,created_at_ms";

common::Result<application::ManifestVerificationManualHandoff> readManualHandoff(
    sqlite3_stmt* statement)
{
    application::ManifestVerificationManualHandoff handoff;
    handoff.id = columnText(statement, 0);
    handoff.originManifestRunId = columnText(statement, 1);
    handoff.verificationAnchorRunId = columnText(statement, 2);
    handoff.schemaVersion = columnText(statement, 3);
    handoff.sourceProjectId = columnText(statement, 4);
    handoff.storageKey = columnText(statement, 5);
    handoff.workspaceIdentity = columnText(statement, 6);
    handoff.baselineFingerprint = columnText(statement, 7);
    handoff.sourceOverlayFingerprint = columnText(statement, 8);
    const auto corrupt = [] {
        return common::Result<application::ManifestVerificationManualHandoff>::failure({
            "manifest_verification_manual_handoff.corrupt",
            "Stored manual-handoff identity is invalid"});
    };
    const auto frozenMembersJson = nlohmann::json::parse(columnText(statement, 9), nullptr, false);
    const auto allowedPathsJson = nlohmann::json::parse(columnText(statement, 10), nullptr, false);
    if (frozenMembersJson.is_discarded() || allowedPathsJson.is_discarded()
        || !frozenMembersJson.is_array() || !allowedPathsJson.is_array()) {
        return corrupt();
    }
    try {
        for (const auto& entry : frozenMembersJson) {
            if (!entry.is_object() || !entry.contains("proposalId")
                || !entry.contains("owningRunId") || !entry.contains("contentHash")
                || !entry.contains("lineageRootProposalId")) {
                return corrupt();
            }
            application::ManifestVerificationManualHandoffFrozenMember member;
            member.proposalId = entry.at("proposalId").get<std::string>();
            member.owningRunId = entry.at("owningRunId").get<std::string>();
            member.contentHash = entry.at("contentHash").get<std::string>();
            member.lineageRootProposalId = entry.at("lineageRootProposalId").get<std::string>();
            handoff.frozenMembers.push_back(std::move(member));
        }
        for (const auto& entry : allowedPathsJson) {
            if (!entry.is_object() || !entry.contains("relativePath")
                || !entry.contains("ownerProposalId") || !entry.contains("ownerTaskId")) {
                return corrupt();
            }
            application::ManifestVerificationManualHandoffAllowedPath allowed;
            allowed.relativePath = entry.at("relativePath").get<std::string>();
            allowed.ownerProposalId = entry.at("ownerProposalId").get<std::string>();
            allowed.ownerTaskId = entry.at("ownerTaskId").get<std::string>();
            handoff.allowedPaths.push_back(std::move(allowed));
        }
    } catch (...) {
        return corrupt();
    }
    const auto snapshot = application::WorkspaceBaselineProvenance::parseJson(
        columnText(statement, 11));
    if (!snapshot.success()) return corrupt();
    handoff.originalSnapshot = snapshot.value();
    handoff.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    const auto valid = application::validateManifestVerificationManualHandoff(handoff);
    if (!valid.success()) return corrupt();
    return common::Result<application::ManifestVerificationManualHandoff>::ok(
        std::move(handoff));
}

constexpr const char* manualHandoffAttemptColumns =
    "id,handoff_id,verification_anchor_run_id,attempt_run_id,schema_version,attempt_ordinal,"
    "previous_attempt_id,edited_snapshot_fingerprint,created_at_ms";

common::Result<application::ManifestVerificationManualHandoffAttempt> readManualHandoffAttempt(
    sqlite3_stmt* statement)
{
    application::ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = columnText(statement, 0);
    attempt.handoffId = columnText(statement, 1);
    attempt.verificationAnchorRunId = columnText(statement, 2);
    attempt.attemptRunId = columnText(statement, 3);
    attempt.schemaVersion = columnText(statement, 4);
    const auto ordinal = sqlite3_column_int64(statement, 5);
    if (ordinal < 0) {
        return common::Result<application::ManifestVerificationManualHandoffAttempt>::failure({
            "manifest_verification_manual_handoff.corrupt_attempt",
            "Stored manual-handoff attempt ordinal is invalid"});
    }
    attempt.attemptOrdinal = static_cast<std::uint64_t>(ordinal);
    if (sqlite3_column_type(statement, 6) != SQLITE_NULL) {
        attempt.previousAttemptId = columnText(statement, 6);
    }
    attempt.editedSnapshotFingerprint = columnText(statement, 7);
    attempt.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 8));
    const auto valid = application::validateManifestVerificationManualHandoffAttempt(attempt);
    if (!valid.success()) {
        return common::Result<application::ManifestVerificationManualHandoffAttempt>::failure({
            "manifest_verification_manual_handoff.corrupt_attempt",
            "Stored manual-handoff attempt violates its typed contract"});
    }
    return common::Result<application::ManifestVerificationManualHandoffAttempt>::ok(
        std::move(attempt));
}

const char* manualHandoffAttemptResultKindToText(
    application::ManifestVerificationManualHandoffAttemptResultKind kind)
{
    switch (kind) {
        case application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess:
            return "verified_success";
        case application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure:
            return "verified_content_failure";
        case application::ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification:
            return "rejected_before_verification";
        case application::ManifestVerificationManualHandoffAttemptResultKind::Incomplete:
            return "incomplete";
    }
    return "incomplete";
}

// Fails closed on any value that is not one of the four the CHECK
// constraint itself already limits the column to - a corrupt or
// out-of-band row is never guessed into a default kind (see
// ManifestVerificationManualHandoffAttemptResultKind's own doc comment).
common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>
manualHandoffAttemptResultKindFromText(const std::string& text)
{
    if (text == "verified_success") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess);
    }
    if (text == "verified_content_failure") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure);
    }
    if (text == "rejected_before_verification") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification);
    }
    if (text == "incomplete") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::Incomplete);
    }
    return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::failure({
        "manifest_verification_manual_handoff.corrupt_attempt_result",
        "Stored manual-handoff attempt result kind is not one of the recognised typed values"});
}

} // namespace

SqliteManifestVerificationManualHandoffRepository::
SqliteManifestVerificationManualHandoffRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::insertHandoff(
    application::ManifestVerificationManualHandoff handoff)
{
    const auto valid = application::validateManifestVerificationManualHandoff(handoff);
    if (!valid.success()) return valid;
    const auto snapshotJson = application::WorkspaceBaselineProvenance::renderJson(
        handoff.originalSnapshot);
    if (!snapshotJson.success()) return common::Result<void>::failure(*snapshotJson.error());
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<void>::failure(*began.error());
    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_manual_handoffs "
        "(id,origin_manifest_run_id,verification_anchor_run_id,schema_version,source_project_id,"
        "storage_key,workspace_identity,baseline_fingerprint,source_overlay_fingerprint,"
        "frozen_members_json,allowed_paths_json,original_snapshot_json,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return rollbackWith(*prepared.error());
    nlohmann::json frozenMembersJson = nlohmann::json::array();
    for (const auto& member : handoff.frozenMembers) {
        frozenMembersJson.push_back({
            {"proposalId", member.proposalId},
            {"owningRunId", member.owningRunId},
            {"contentHash", member.contentHash},
            {"lineageRootProposalId", member.lineageRootProposalId}});
    }
    const std::string frozenMembers = frozenMembersJson.dump();
    nlohmann::json allowedPathsJson = nlohmann::json::array();
    for (const auto& allowed : handoff.allowedPaths) {
        allowedPathsJson.push_back({
            {"relativePath", allowed.relativePath},
            {"ownerProposalId", allowed.ownerProposalId},
            {"ownerTaskId", allowed.ownerTaskId}});
    }
    const std::string allowedPaths = allowedPathsJson.dump();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, handoff.id),
        bindText(handle, statement.get(), 2, handoff.originManifestRunId),
        bindText(handle, statement.get(), 3, handoff.verificationAnchorRunId),
        bindText(handle, statement.get(), 4, handoff.schemaVersion),
        bindText(handle, statement.get(), 5, handoff.sourceProjectId),
        bindText(handle, statement.get(), 6, handoff.storageKey),
        bindText(handle, statement.get(), 7, handoff.workspaceIdentity),
        bindText(handle, statement.get(), 8, handoff.baselineFingerprint),
        bindText(handle, statement.get(), 9, handoff.sourceOverlayFingerprint),
        bindText(handle, statement.get(), 10, frozenMembers),
        bindText(handle, statement.get(), 11, allowedPaths),
        bindText(handle, statement.get(), 12, snapshotJson.value()),
        bindInt64(handle, statement.get(), 13, toMilliseconds(handoff.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return rollbackWith(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            return rollbackWith({
                "manifest_verification_manual_handoff.already_exists",
                "A manual handoff already exists for this verification anchor Run"});
        }
        return rollbackWith(databaseError(handle,
            "manifest_verification_manual_handoff.write_failed",
            "Could not persist the manual handoff"));
    }

    // Normalized, indexed member rows - what AG-16's reverse lookup actually
    // queries (see findHandoffByMemberProposalId). proposal_id's own UNIQUE
    // constraint is the durable, cross-handoff enforcement of "a proposal is
    // frozen into at most one handoff" - a SQLITE_CONSTRAINT_UNIQUE here
    // means some OTHER handoff already claimed one of these proposal ids.
    for (std::size_t ordinal = 0; ordinal < handoff.frozenMembers.size(); ++ordinal) {
        const auto& member = handoff.frozenMembers[ordinal];
        Statement memberStatement;
        const auto memberPrepared = prepare(handle,
            "INSERT INTO manifest_verification_manual_handoff_members "
            "(handoff_id,ordinal,proposal_id,owning_run_id,content_hash,lineage_root_proposal_id) "
            "VALUES (?,?,?,?,?,?);", memberStatement);
        if (!memberPrepared.success()) return rollbackWith(*memberPrepared.error());
        const common::Result<void> memberBindings[] = {
            bindText(handle, memberStatement.get(), 1, handoff.id),
            bindInt64(handle, memberStatement.get(), 2, static_cast<std::int64_t>(ordinal)),
            bindText(handle, memberStatement.get(), 3, member.proposalId),
            bindText(handle, memberStatement.get(), 4, member.owningRunId),
            bindText(handle, memberStatement.get(), 5, member.contentHash),
            bindText(handle, memberStatement.get(), 6, member.lineageRootProposalId)};
        for (const auto& binding : memberBindings)
            if (!binding.success()) return rollbackWith(*binding.error());
        if (sqlite3_step(memberStatement.get()) != SQLITE_DONE) {
            const int extended = sqlite3_extended_errcode(handle);
            if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
                return rollbackWith({
                    "manifest_verification_manual_handoff.member_already_claimed",
                    "A frozen member proposal is already claimed by a different manual handoff"});
            }
            return rollbackWith(databaseError(handle,
                "manifest_verification_manual_handoff.write_failed",
                "Could not persist a frozen manual-handoff member"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::findHandoffByAnchorRun(
    const application::RunId& verificationAnchorRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffColumns
        + " FROM manifest_verification_manual_handoffs WHERE verification_anchor_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, verificationAnchorRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not read manual handoff"));
    auto handoff = readManualHandoff(statement.get());
    if (!handoff.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::move(handoff.value()));
}

common::Result<std::optional<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::findHandoffById(
    const std::string& handoffId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffColumns
        + " FROM manifest_verification_manual_handoffs WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, handoffId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not read manual handoff"));
    auto handoff = readManualHandoff(statement.get());
    if (!handoff.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::move(handoff.value()));
}

common::Result<std::optional<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::findHandoffByMemberProposalId(
    const application::ChangeSetId& proposalId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ")
        + "h.id,h.origin_manifest_run_id,h.verification_anchor_run_id,h.schema_version,"
          "h.source_project_id,h.storage_key,h.workspace_identity,h.baseline_fingerprint,"
          "h.source_overlay_fingerprint,h.frozen_members_json,h.allowed_paths_json,"
          "h.original_snapshot_json,h.created_at_ms "
          "FROM manifest_verification_manual_handoff_members AS m "
          "JOIN manifest_verification_manual_handoffs AS h ON h.id=m.handoff_id "
          "WHERE m.proposal_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, proposalId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not read manual handoff"));
    auto handoff = readManualHandoff(statement.get());
    if (!handoff.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::move(handoff.value()));
}

common::Result<std::vector<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::listAllHandoffs() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffColumns
        + " FROM manifest_verification_manual_handoffs;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    std::vector<application::ManifestVerificationManualHandoff> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::failure(
            databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not list manual handoffs"));
        auto handoff = readManualHandoff(statement.get());
        if (!handoff.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
        result.push_back(std::move(handoff.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::ok(std::move(result));
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::discardHandoff(
    const std::string& handoffId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // A single DELETE on the parent row. The real schema's ON DELETE CASCADE
    // (foreign_keys=ON, established once at connection open - see
    // SqliteDatabase::open()) already removes every attempt AND member row
    // belonging to this handoff within the same implicit transaction; no
    // separate member/attempt cleanup statement is needed here, and adding
    // one would only risk drifting out of sync with the schema's own
    // cascade. Neither child table's own reject-update trigger fires on a
    // DELETE, only an UPDATE, so this cascade is never blocked by them.
    const auto prepared = prepare(
        handle, "DELETE FROM manifest_verification_manual_handoffs WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, handoffId);
    if (!bound.success()) return common::Result<void>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_verification_manual_handoff.delete_failed",
            "Could not delete the manual handoff"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(error(
            "manifest_verification_manual_handoff.not_found",
            "No manual handoff exists with this id"));
    }
    return common::Result<void>::ok();
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::insertAttempt(
    application::ManifestVerificationManualHandoffAttempt attempt)
{
    const auto valid = application::validateManifestVerificationManualHandoffAttempt(attempt);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_manual_handoff_attempts "
        "(id,handoff_id,verification_anchor_run_id,attempt_run_id,schema_version,attempt_ordinal,"
        "previous_attempt_id,edited_snapshot_fingerprint,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, attempt.id),
        bindText(handle, statement.get(), 2, attempt.handoffId),
        bindText(handle, statement.get(), 3, attempt.verificationAnchorRunId),
        bindText(handle, statement.get(), 4, attempt.attemptRunId),
        bindText(handle, statement.get(), 5, attempt.schemaVersion),
        bindInt64(handle, statement.get(), 6, static_cast<std::int64_t>(attempt.attemptOrdinal)),
        bindNullableText(handle, statement.get(), 7, attempt.previousAttemptId),
        bindText(handle, statement.get(), 8, attempt.editedSnapshotFingerprint),
        bindInt64(handle, statement.get(), 9, toMilliseconds(attempt.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.attempt_already_exists",
                "A manual-handoff attempt already exists for this attempt Run or edited state"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_manual_handoff.attempt_write_failed",
            "Could not persist the manual-handoff attempt"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::findAttemptByRunId(
    const application::RunId& attemptRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts WHERE attempt_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, attemptRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not read manual-handoff attempt"));
    auto attempt = readManualHandoffAttempt(statement.get());
    if (!attempt.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(attempt.value()));
}

common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::findAttemptByFingerprint(
    const std::string& handoffId, const std::string& editedSnapshotFingerprint) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts "
          "WHERE handoff_id=? AND edited_snapshot_fingerprint=? "
          "ORDER BY attempt_ordinal DESC LIMIT 1;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, handoffId),
        bindText(handle, statement.get(), 2, editedSnapshotFingerprint)};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*binding.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not read manual-handoff attempt"));
    auto attempt = readManualHandoffAttempt(statement.get());
    if (!attempt.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(attempt.value()));
}

common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::listAttemptsForHandoff(
    const std::string& handoffId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts "
          "WHERE handoff_id=? ORDER BY attempt_ordinal;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, handoffId);
    if (!bound.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*bound.error());
    std::vector<application::ManifestVerificationManualHandoffAttempt> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(
            databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not list manual-handoff attempts"));
        auto attempt = readManualHandoffAttempt(statement.get());
        if (!attempt.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
        result.push_back(std::move(attempt.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(result));
}

common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::listAllAttempts() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    std::vector<application::ManifestVerificationManualHandoffAttempt> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(
            databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not list manual-handoff attempts"));
        auto attempt = readManualHandoffAttempt(statement.get());
        if (!attempt.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
        result.push_back(std::move(attempt.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(result));
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::insertAttemptResult(
    application::ManifestVerificationManualHandoffAttemptResult result)
{
    const auto valid = application::validateManifestVerificationManualHandoffAttemptResult(result);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_manual_handoff_attempt_results "
        "(id,attempt_id,attempt_run_id,schema_version,kind,rejection_code,rejection_message,"
        "recorded_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, result.id),
        bindText(handle, statement.get(), 2, result.attemptId),
        bindText(handle, statement.get(), 3, result.attemptRunId),
        bindText(handle, statement.get(), 4, result.schemaVersion),
        bindText(handle, statement.get(), 5, manualHandoffAttemptResultKindToText(result.kind)),
        bindNullableText(handle, statement.get(), 6, result.rejectionCode),
        bindNullableText(handle, statement.get(), 7, result.rejectionMessage),
        bindInt64(handle, statement.get(), 8, toMilliseconds(result.recordedAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.attempt_result_already_exists",
                "A typed result already exists for this manual-handoff attempt"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_manual_handoff.attempt_result_write_failed",
            "Could not persist the manual-handoff attempt result"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>
SqliteManifestVerificationManualHandoffRepository::findAttemptResultByAttemptId(
    const std::string& attemptId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,attempt_id,attempt_run_id,schema_version,kind,rejection_code,"
        "rejection_message,recorded_at_ms "
        "FROM manifest_verification_manual_handoff_attempt_results WHERE attempt_id=?;",
        statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, attemptId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.attempt_result_read_failed",
            "Could not read manual-handoff attempt result"));
    application::ManifestVerificationManualHandoffAttemptResult result;
    result.id = columnText(statement.get(), 0);
    result.attemptId = columnText(statement.get(), 1);
    result.attemptRunId = columnText(statement.get(), 2);
    result.schemaVersion = columnText(statement.get(), 3);
    const auto kind = manualHandoffAttemptResultKindFromText(columnText(statement.get(), 4));
    if (!kind.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(*kind.error());
    result.kind = kind.value();
    if (sqlite3_column_type(statement.get(), 5) != SQLITE_NULL) {
        result.rejectionCode = columnText(statement.get(), 5);
    }
    if (sqlite3_column_type(statement.get(), 6) != SQLITE_NULL) {
        result.rejectionMessage = columnText(statement.get(), 6);
    }
    result.recordedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    const auto valid = application::validateManifestVerificationManualHandoffAttemptResult(result);
    if (!valid.success()) {
        return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure({
            "manifest_verification_manual_handoff.corrupt_attempt_result",
            "Stored manual-handoff attempt result violates its typed contract"});
    }
    return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::ok(
        std::move(result));
}

} // namespace ai::infrastructure
