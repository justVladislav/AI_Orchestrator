#include "ai/infrastructure/SqliteRepositories.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "../planning/SqliteRunDraftGroupStorage.hpp"

#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/budget/CacheDurationAccounting.hpp"
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



std::string stringArrayToJson(const std::vector<std::string>& values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) array.push_back(value);
    return array.dump();
}

std::string pathArrayToJson(const std::vector<std::filesystem::path>& values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) array.push_back(pathToUtf8(value));
    return array.dump();
}

common::Result<std::vector<std::string>> stringArrayFromJson(
    const std::string& json, std::size_t maxItems, std::string_view corruptCode)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.size() > maxItems) {
        return common::Result<std::vector<std::string>>::failure(
            error(std::string(corruptCode), "Corrupt or out-of-bound bootstrap string array column"));
    }
    std::vector<std::string> values;
    values.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) {
            return common::Result<std::vector<std::string>>::failure(
                error(std::string(corruptCode), "Corrupt bootstrap string array column"));
        }
        values.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<std::string>>::ok(std::move(values));
}

common::Result<std::vector<std::filesystem::path>> pathArrayFromJson(
    const std::string& json, std::size_t maxItems, std::string_view corruptCode)
{
    auto strings = stringArrayFromJson(json, maxItems, corruptCode);
    if (!strings.success()) {
        return common::Result<std::vector<std::filesystem::path>>::failure(*strings.error());
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(strings.value().size());
    for (auto& value : strings.value()) paths.push_back(pathFromUtf8(value));
    return common::Result<std::vector<std::filesystem::path>>::ok(std::move(paths));
}

std::string candidateIdentityToJson(
    const std::optional<application::VerificationBootstrapCandidateIdentity>& identity)
{
    if (!identity.has_value()) return {};
    nlohmann::json j = {
        {"overlayFingerprint", identity->overlayFingerprint},
        {"candidateProfileId", identity->candidateProfileId},
        {"candidateBuildProfileId", identity->candidateBuildProfileId},
        {"candidateTargetId", identity->candidateTargetId}
    };
    return j.dump();
}

common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>
candidateIdentityFromJson(const std::string& json)
{
    if (json.empty()) {
        return common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>::ok(
            std::nullopt);
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()
        || !parsed.contains("overlayFingerprint") || !parsed["overlayFingerprint"].is_string()
        || !parsed.contains("candidateProfileId") || !parsed["candidateProfileId"].is_string()
        || !parsed.contains("candidateBuildProfileId") || !parsed["candidateBuildProfileId"].is_string()
        || !parsed.contains("candidateTargetId") || !parsed["candidateTargetId"].is_string()) {
        return common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>::failure(
            error("verification_environment.corrupt_bootstrap_record",
                "Corrupt bootstrap candidate identity column"));
    }
    application::VerificationBootstrapCandidateIdentity identity;
    identity.overlayFingerprint = parsed["overlayFingerprint"].get<std::string>();
    identity.candidateProfileId = parsed["candidateProfileId"].get<std::string>();
    identity.candidateBuildProfileId = parsed["candidateBuildProfileId"].get<std::string>();
    identity.candidateTargetId = parsed["candidateTargetId"].get<std::string>();
    return common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>::ok(
        std::move(identity));
}

// Forward-declared here (defined below, after findDecisionForRun's public
// wrapper) so recordDecision's own duplicate-conflict check - which already
// holds database_->impl_->mutex - can call it directly instead of the
// locking public findDecisionForRun, which would deadlock on this
// non-recursive mutex.
common::Result<std::optional<application::VerificationBootstrapDecision>>
readDecisionForRunUnlocked(sqlite3* handle, const application::RunId& runId);

// Forward-declared for the same reason as readDecisionForRunUnlocked above
// - recordRetryLink's own duplicate-conflict check already holds the mutex.
common::Result<std::optional<application::VerificationBootstrapRetryLink>>
readRetryLinkForPredecessorUnlocked(sqlite3* handle, const application::RunId& predecessorRunId);

// Shared by findDecisionForRun (which takes the lock itself) and
// recordDecision's own duplicate-conflict check (which already holds the
// lock when it needs to read the just-conflicting row back) - calling the
// locking public method from inside an already-locked method would
// deadlock on this non-recursive mutex.
common::Result<std::optional<application::VerificationBootstrapDecision>>
readDecisionForRunUnlocked(sqlite3* handle, const application::RunId& runId)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_id,schema_version,baseline_fingerprint,"
        "verification_snapshot_schema_version,disposition,reason_code,message,"
        "bootstrap_setup_paths_json,owning_task_ids_json,predecessor_run_id,decided_at_ms "
        "FROM verification_bootstrap_decisions WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification bootstrap decision"));
    }
    auto corrupt = [] {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            error("verification_environment.corrupt_bootstrap_record",
                "Corrupt verification bootstrap decision row"));
    };
    application::VerificationBootstrapDecision decision;
    decision.runId = runId;
    decision.projectId = columnText(statement.get(), 0);
    decision.schemaVersion = columnText(statement.get(), 1);
    if (decision.schemaVersion != application::kVerificationBootstrapDecisionSchemaVersion
        || decision.projectId.empty()) {
        return corrupt();
    }
    decision.baselineFingerprint = columnText(statement.get(), 2);
    decision.verificationSnapshotSchemaVersion = columnText(statement.get(), 3);
    const auto disposition = application::verificationBootstrapDispositionFromString(
        columnText(statement.get(), 4));
    if (!disposition.has_value()) return corrupt();
    decision.disposition = *disposition;
    decision.reasonCode = columnText(statement.get(), 5);
    decision.message = columnText(statement.get(), 6);
    auto setupPaths = pathArrayFromJson(columnText(statement.get(), 7),
        application::kVerificationBootstrapMaxSetupPaths, "verification_environment.corrupt_bootstrap_record");
    if (!setupPaths.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *setupPaths.error());
    }
    decision.bootstrapSetupPaths = std::move(setupPaths.value());
    auto owningTaskIds = stringArrayFromJson(columnText(statement.get(), 8),
        application::kVerificationBootstrapMaxOwningTasks, "verification_environment.corrupt_bootstrap_record");
    if (!owningTaskIds.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *owningTaskIds.error());
    }
    decision.owningTaskIds = std::move(owningTaskIds.value());
    if (sqlite3_column_type(statement.get(), 9) != SQLITE_NULL) {
        decision.predecessorRunId = columnText(statement.get(), 9);
    }
    decision.decidedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 10));
    // A BootstrapPending/BootstrapVerified/BootstrapFailed decision without
    // any declared setup path is corrupt data - only ActionRequired
    // legitimately has none.
    if (decision.disposition != application::VerificationBootstrapDisposition::ActionRequired
        && decision.bootstrapSetupPaths.empty()) {
        return corrupt();
    }
    return common::Result<std::optional<application::VerificationBootstrapDecision>>::ok(
        std::move(decision));
}

bool candidateIdentitiesEqual(
    const std::optional<application::VerificationBootstrapCandidateIdentity>& a,
    const std::optional<application::VerificationBootstrapCandidateIdentity>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->overlayFingerprint == b->overlayFingerprint
        && a->candidateProfileId == b->candidateProfileId
        && a->candidateBuildProfileId == b->candidateBuildProfileId
        && a->candidateTargetId == b->candidateTargetId;
}

// See InMemoryVerificationBootstrapRepository's identical helper doc
// comment: the target id is legitimately empty for a cmake_build-only
// bootstrap, so completeness never requires it.
bool candidateIdentityIsComplete(const application::VerificationBootstrapCandidateIdentity& identity)
{
    return !identity.overlayFingerprint.empty() && !identity.candidateProfileId.empty()
        && !identity.candidateBuildProfileId.empty();
}

// Mirrors readDecisionForRunUnlocked's own rationale: recordRetryLink's
// duplicate-conflict check already holds database_->impl_->mutex when it
// needs to compare against the just-conflicting row, so it must call this
// instead of the locking public findRetryLinkForPredecessor.
common::Result<std::optional<application::VerificationBootstrapRetryLink>>
readRetryLinkForPredecessorUnlocked(sqlite3* handle, const application::RunId& predecessorRunId)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT new_run_id,project_id,schema_version,reason,created_at_ms "
        "FROM verification_bootstrap_retry_links WHERE predecessor_run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, predecessorRunId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification bootstrap retry link"));
    }
    application::VerificationBootstrapRetryLink link;
    link.predecessorRunId = predecessorRunId;
    link.newRunId = columnText(statement.get(), 0);
    link.projectId = columnText(statement.get(), 1);
    link.schemaVersion = columnText(statement.get(), 2);
    if (link.schemaVersion != application::kVerificationBootstrapRetryLinkSchemaVersion
        || link.newRunId.empty()) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            error("verification_environment.corrupt_bootstrap_record",
                "Corrupt verification bootstrap retry link row"));
    }
    link.reason = columnText(statement.get(), 3);
    link.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 4));
    return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::ok(std::move(link));
}

} // namespace

SqliteVerificationBootstrapRepository::SqliteVerificationBootstrapRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteVerificationBootstrapRepository::recordDecision(
    application::VerificationBootstrapDecision decision)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_bootstrap_decisions "
        "(run_id,project_id,schema_version,baseline_fingerprint,"
        "verification_snapshot_schema_version,disposition,reason_code,message,"
        "bootstrap_setup_paths_json,owning_task_ids_json,predecessor_run_id,decided_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, decision.runId),
        bindText(handle, statement.get(), 2, decision.projectId),
        bindText(handle, statement.get(), 3, decision.schemaVersion),
        bindText(handle, statement.get(), 4, decision.baselineFingerprint),
        bindText(handle, statement.get(), 5, decision.verificationSnapshotSchemaVersion),
        bindText(handle, statement.get(), 6, application::toString(decision.disposition)),
        bindText(handle, statement.get(), 7, decision.reasonCode),
        bindText(handle, statement.get(), 8, decision.message),
        bindText(handle, statement.get(), 9, pathArrayToJson(decision.bootstrapSetupPaths)),
        bindText(handle, statement.get(), 10, stringArrayToJson(decision.owningTaskIds)),
        bindNullableText(handle, statement.get(), 11, decision.predecessorRunId),
        bindInt64(handle, statement.get(), 12, toMilliseconds(decision.decidedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            // Idempotent only when the already-recorded decision agrees;
            // a genuinely conflicting duplicate must fail closed instead of
            // silently keeping whichever row happened to be inserted first.
            const auto existing = readDecisionForRunUnlocked(handle, decision.runId);
            if (existing.success() && existing.value().has_value()) {
                const auto& recorded = *existing.value();
                if (recorded.disposition == decision.disposition
                    && recorded.reasonCode == decision.reasonCode
                    && recorded.baselineFingerprint == decision.baselineFingerprint
                    && recorded.bootstrapSetupPaths == decision.bootstrapSetupPaths) {
                    return common::Result<void>::ok();
                }
            }
            return common::Result<void>::failure(error("verification_environment.decision_conflict",
                "A different verification bootstrap decision already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification bootstrap decision"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::VerificationBootstrapDecision>>
SqliteVerificationBootstrapRepository::findDecisionForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readDecisionForRunUnlocked(database_->impl_->handle, runId);
}

common::Result<void> SqliteVerificationBootstrapRepository::appendTransition(
    application::VerificationBootstrapTransition transition)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    // AG-18.3 hardening (blocker 1): the owning decision must exist and be
    // BootstrapPending, and this Run's bootstrap lifecycle must not already
    // have a resolving transition - the same fail-closed semantics as the
    // InMemory implementation, expressed against SQLite directly.
    const auto owningDecision = readDecisionForRunUnlocked(handle, transition.runId);
    if (!owningDecision.success()) return common::Result<void>::failure(*owningDecision.error());
    if (!owningDecision.value().has_value()
        || owningDecision.value()->disposition
               != application::VerificationBootstrapDisposition::BootstrapPending) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "A verification bootstrap transition requires an owning BootstrapPending decision"});
    }
    if (transition.toDisposition != application::VerificationBootstrapDisposition::BootstrapVerified
        && transition.toDisposition != application::VerificationBootstrapDisposition::BootstrapFailed) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "Only BootstrapVerified or BootstrapFailed may be appended as a transition"});
    }
    if (transition.toDisposition == application::VerificationBootstrapDisposition::BootstrapVerified
        && (!transition.candidateIdentity.has_value()
            || !candidateIdentityIsComplete(*transition.candidateIdentity))) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "BootstrapVerified requires a complete candidate identity"});
    }
    if (transition.toDisposition == application::VerificationBootstrapDisposition::BootstrapFailed
        && transition.candidateIdentity.has_value()) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "BootstrapFailed must not carry a candidate identity"});
    }

    {
        Statement existingStatement;
        const auto preparedExisting = prepare(handle,
            "SELECT schema_version,to_disposition,reason_code,candidate_identity_json "
            "FROM verification_bootstrap_transitions WHERE id=?;",
            existingStatement);
        if (!preparedExisting.success()) {
            return common::Result<void>::failure(*preparedExisting.error());
        }
        const auto boundId = bindText(handle, existingStatement.get(), 1, transition.id);
        if (!boundId.success()) return common::Result<void>::failure(*boundId.error());
        const int existingStep = sqlite3_step(existingStatement.get());
        if (existingStep == SQLITE_ROW) {
            const bool schemaMatches = columnText(existingStatement.get(), 0) == transition.schemaVersion;
            const auto storedDisposition = application::verificationBootstrapDispositionFromString(
                columnText(existingStatement.get(), 1));
            const bool dispositionMatches = storedDisposition.has_value()
                && *storedDisposition == transition.toDisposition;
            const bool reasonMatches = columnText(existingStatement.get(), 2) == transition.reasonCode;
            const bool hasStoredCandidateJson =
                sqlite3_column_type(existingStatement.get(), 3) != SQLITE_NULL;
            auto storedCandidate = candidateIdentityFromJson(
                hasStoredCandidateJson ? columnText(existingStatement.get(), 3) : std::string {});
            const bool candidateMatches = storedCandidate.success()
                && candidateIdentitiesEqual(storedCandidate.value(), transition.candidateIdentity);
            if (schemaMatches && dispositionMatches && reasonMatches && candidateMatches) {
                return common::Result<void>::ok();
            }
            return common::Result<void>::failure({"verification_environment.decision_conflict",
                "A different verification bootstrap transition already exists with this identity"});
        }
        if (existingStep != SQLITE_DONE) {
            return common::Result<void>::failure(databaseError(handle,
                "verification_environment.read_failed",
                "Could not check for an existing verification bootstrap transition"));
        }
    }

    {
        Statement resolvedStatement;
        const auto preparedResolved = prepare(handle,
            "SELECT 1 FROM verification_bootstrap_transitions "
            "WHERE run_id=? AND to_disposition IN ('bootstrap_verified','bootstrap_failed') LIMIT 1;",
            resolvedStatement);
        if (!preparedResolved.success()) {
            return common::Result<void>::failure(*preparedResolved.error());
        }
        const auto boundRun = bindText(handle, resolvedStatement.get(), 1, transition.runId);
        if (!boundRun.success()) return common::Result<void>::failure(*boundRun.error());
        const int resolvedStep = sqlite3_step(resolvedStatement.get());
        if (resolvedStep == SQLITE_ROW) {
            return common::Result<void>::failure({"verification_environment.decision_conflict",
                "This Run's bootstrap lifecycle already has a resolving transition"});
        }
        if (resolvedStep != SQLITE_DONE) {
            return common::Result<void>::failure(databaseError(handle,
                "verification_environment.read_failed",
                "Could not check for an existing resolving verification bootstrap transition"));
        }
    }

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_bootstrap_transitions "
        "(id,run_id,schema_version,to_disposition,reason_code,message,"
        "candidate_identity_json,transitioned_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const std::string candidateJson = candidateIdentityToJson(transition.candidateIdentity);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, transition.id),
        bindText(handle, statement.get(), 2, transition.runId),
        bindText(handle, statement.get(), 3, transition.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(transition.toDisposition)),
        bindText(handle, statement.get(), 5, transition.reasonCode),
        bindText(handle, statement.get(), 6, transition.message),
        bindNullableText(handle, statement.get(), 7,
            candidateJson.empty() ? std::optional<std::string>{} : std::optional<std::string>{candidateJson}),
        bindInt64(handle, statement.get(), 8, toMilliseconds(transition.transitionedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not append verification bootstrap transition"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::VerificationBootstrapTransition>>
SqliteVerificationBootstrapRepository::listTransitionsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,to_disposition,reason_code,message,"
        "candidate_identity_json,transitioned_at_ms "
        "FROM verification_bootstrap_transitions WHERE run_id=? ORDER BY transitioned_at_ms ASC;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
            *bound.error());
    }
    std::vector<application::VerificationBootstrapTransition> transitions;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                databaseError(handle, "verification_environment.read_failed",
                    "Could not read verification bootstrap transitions"));
        }
        application::VerificationBootstrapTransition transition;
        transition.id = columnText(statement.get(), 0);
        transition.runId = runId;
        transition.schemaVersion = columnText(statement.get(), 1);
        if (transition.schemaVersion != application::kVerificationBootstrapTransitionSchemaVersion) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                error("verification_environment.corrupt_bootstrap_record",
                    "Corrupt verification bootstrap transition row"));
        }
        const auto disposition = application::verificationBootstrapDispositionFromString(
            columnText(statement.get(), 2));
        if (!disposition.has_value()) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                error("verification_environment.corrupt_bootstrap_record",
                    "Corrupt verification bootstrap transition disposition"));
        }
        transition.toDisposition = *disposition;
        transition.reasonCode = columnText(statement.get(), 3);
        transition.message = columnText(statement.get(), 4);
        const bool hasCandidateJson = sqlite3_column_type(statement.get(), 5) != SQLITE_NULL;
        auto candidate = candidateIdentityFromJson(
            hasCandidateJson ? columnText(statement.get(), 5) : std::string {});
        if (!candidate.success()) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                *candidate.error());
        }
        transition.candidateIdentity = std::move(candidate.value());
        transition.transitionedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        transitions.push_back(std::move(transition));
    }
    return common::Result<std::vector<application::VerificationBootstrapTransition>>::ok(
        std::move(transitions));
}

common::Result<void> SqliteVerificationBootstrapRepository::recordRetryLink(
    application::VerificationBootstrapRetryLink link)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_bootstrap_retry_links "
        "(predecessor_run_id,new_run_id,project_id,schema_version,reason,created_at_ms) "
        "VALUES (?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, link.predecessorRunId),
        bindText(handle, statement.get(), 2, link.newRunId),
        bindText(handle, statement.get(), 3, link.projectId),
        bindText(handle, statement.get(), 4, link.schemaVersion),
        bindText(handle, statement.get(), 5, link.reason),
        bindInt64(handle, statement.get(), 6, toMilliseconds(link.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            // AG-18.3 hardening (blocker 4): the primary key already
            // existing must never itself mean success - re-read the stored
            // row and only treat a byte-identical repeat as idempotent; a
            // conflicting one (a different newRunId/projectId for the same
            // predecessor) fails closed.
            const auto existing = readRetryLinkForPredecessorUnlocked(handle, link.predecessorRunId);
            if (existing.success() && existing.value().has_value()
                && existing.value()->newRunId == link.newRunId
                && existing.value()->projectId == link.projectId) {
                return common::Result<void>::ok();
            }
            return common::Result<void>::failure({"verification_environment.decision_conflict",
                "A different verification bootstrap retry link already exists for this "
                "predecessor"});
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification bootstrap retry link"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::VerificationBootstrapRetryLink>>
SqliteVerificationBootstrapRepository::findRetryLinkForPredecessor(
    const application::RunId& predecessorRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readRetryLinkForPredecessorUnlocked(database_->impl_->handle, predecessorRunId);
}

} // namespace ai::infrastructure
