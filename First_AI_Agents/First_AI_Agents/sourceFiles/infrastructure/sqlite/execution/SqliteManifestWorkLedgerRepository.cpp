#include "ai/infrastructure/SqliteRepositories.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "../planning/SqliteRunDraftGroupStorage.hpp"

#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
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

std::string serializeTaskDispositions(
    const std::vector<application::ManifestWorkLedgerTaskDisposition>& dispositions)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : dispositions) {
        nlohmann::json entry {
            {"taskId", item.taskId},
            {"status", item.status}};
        entry["changeSetId"] = item.changeSetId.has_value()
            ? nlohmann::json(*item.changeSetId) : nlohmann::json(nullptr);
        entry["noChangesReason"] = item.noChangesReason.has_value()
            ? nlohmann::json(*item.noChangesReason) : nlohmann::json(nullptr);
        array.push_back(std::move(entry));
    }
    return array.dump();
}

// AG-14.1: a malformed/corrupt column now fails the whole row read
// (manifest_work_ledger.corrupt_entry) instead of silently substituting an
// empty vector, which was indistinguishable from "this Run genuinely has
// none." An individual array *item* that is itself malformed is still
// defensively skipped rather than failing the whole column - only total
// parse failure or the wrong root JSON type is treated as corruption.
common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>
deserializeTaskDispositions(const std::string& json)
{
    std::vector<application::ManifestWorkLedgerTaskDisposition> result;
    if (json.empty()) {
        return common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>::ok(result);
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>::failure(
            error("manifest_work_ledger.corrupt_entry", "Corrupt task_dispositions column"));
    }
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkLedgerTaskDisposition disposition;
        if (item.contains("taskId") && item["taskId"].is_string())
            disposition.taskId = item["taskId"].get<std::string>();
        if (item.contains("status") && item["status"].is_string())
            disposition.status = item["status"].get<std::string>();
        if (item.contains("changeSetId") && item["changeSetId"].is_string())
            disposition.changeSetId = item["changeSetId"].get<std::string>();
        if (item.contains("noChangesReason") && item["noChangesReason"].is_string())
            disposition.noChangesReason = item["noChangesReason"].get<std::string>();
        result.push_back(std::move(disposition));
    }
    return common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>::ok(
        std::move(result));
}

std::string serializeStringArray(const std::vector<std::string>& values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) array.push_back(value);
    return array.dump();
}

common::Result<std::vector<std::string>> deserializeStringArray(
    const std::string& json, const char* columnName)
{
    std::vector<std::string> result;
    if (json.empty()) return common::Result<std::vector<std::string>>::ok(result);
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<std::string>>::failure(
            error("manifest_work_ledger.corrupt_entry",
                std::string("Corrupt ") + columnName + " column"));
    }
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<std::string>>::ok(std::move(result));
}

std::string serializeProposalLineage(
    const std::vector<application::ManifestWorkLedgerProposalLineage>& lineage)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : lineage) {
        nlohmann::json entry {
            {"changeSetId", item.changeSetId},
            {"state", item.state}};
        entry["supersedesChangeSetId"] = item.supersedesChangeSetId.has_value()
            ? nlohmann::json(*item.supersedesChangeSetId) : nlohmann::json(nullptr);
        array.push_back(std::move(entry));
    }
    return array.dump();
}

common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>
deserializeProposalLineage(const std::string& json)
{
    std::vector<application::ManifestWorkLedgerProposalLineage> result;
    if (json.empty()) {
        return common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>::ok(result);
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>::failure(
            error("manifest_work_ledger.corrupt_entry", "Corrupt proposal_lineage column"));
    }
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkLedgerProposalLineage lineage;
        if (item.contains("changeSetId") && item["changeSetId"].is_string())
            lineage.changeSetId = item["changeSetId"].get<std::string>();
        if (item.contains("state") && item["state"].is_string())
            lineage.state = item["state"].get<std::string>();
        if (item.contains("supersedesChangeSetId") && item["supersedesChangeSetId"].is_string())
            lineage.supersedesChangeSetId = item["supersedesChangeSetId"].get<std::string>();
        result.push_back(std::move(lineage));
    }
    return common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>::ok(
        std::move(result));
}

// AG-14.1: thin wrappers around the shared ExecutionAcceptanceContract
// serialize/parse implementation - one field-by-field JSON shape, reused by
// both the manifest-verification-identity.v1.json artifact and this
// column, never re-derived twice. serializeVerificationIdentity/
// deserializeVerificationIdentity only add this column's own "empty means
// never ran" convention on top.
std::string serializeVerificationIdentity(const application::ManifestProposalSetVerification& v)
{
    return application::renderManifestProposalSetVerificationJson(v);
}

// AG-14.1: an empty column means "combined verification never ran for this
// Run" (a legitimate, common case - Planner no-op, early planning failure)
// and returns std::nullopt success. A non-empty but malformed column fails
// the whole row read, exactly like the other JSON columns above - it must
// never be silently reinterpreted as "never ran."
common::Result<std::optional<application::ManifestProposalSetVerification>>
deserializeVerificationIdentity(const std::string& json)
{
    if (json.empty()) {
        return common::Result<std::optional<application::ManifestProposalSetVerification>>::ok(std::nullopt);
    }
    auto parsed = application::parseManifestProposalSetVerificationJson(json);
    if (!parsed.success()) {
        return common::Result<std::optional<application::ManifestProposalSetVerification>>::failure(
            error("manifest_work_ledger.corrupt_entry", "Corrupt verification_identity_json column"));
    }
    return common::Result<std::optional<application::ManifestProposalSetVerification>>::ok(
        std::move(parsed.value()));
}

std::string serializeWorkspaceLedgerSnapshot(
    const application::ManifestWorkLedgerWorkspaceSnapshot& snapshot)
{
    nlohmann::json root;
    root["schemaVersion"] = snapshot.schemaVersion;
    root["entries"] = nlohmann::json::array();
    for (const auto& entry : snapshot.entries) {
        root["entries"].push_back({
            {"relativePath", entry.relativePath},
            {"role", application::toString(entry.role)},
            {"reason", entry.reason},
            {"taskId", entry.taskId},
            {"baselineHash", entry.baselineHash.has_value()
                ? nlohmann::json(*entry.baselineHash) : nlohmann::json(nullptr)},
            {"origin", application::toString(entry.origin)}});
    }

    root["expansions"] = nlohmann::json::array();
    for (const auto& expansion : snapshot.expansions) {
        root["expansions"].push_back({
            {"id", expansion.id}, {"runId", expansion.runId},
            {"taskId", expansion.taskId}, {"relativePath", expansion.relativePath},
            {"role", application::toString(expansion.role)}, {"reason", expansion.reason},
            {"baselineHash", expansion.baselineHash.has_value()
                ? nlohmann::json(*expansion.baselineHash) : nlohmann::json(nullptr)},
            {"approvedAtMs", toMilliseconds(expansion.approvedAt)}});
    }
    root["dependencyEdges"] = nlohmann::json::array();
    for (const auto& edge : snapshot.dependencyEdges) {
        root["dependencyEdges"].push_back({
            {"dependentTaskId", edge.dependentTaskId},
            {"providerTaskId", edge.providerTaskId},
            {"relativePath", edge.relativePath}});
    }
    root["taskActivities"] = nlohmann::json::array();
    for (const auto& activity : snapshot.taskActivities) {
        root["taskActivities"].push_back({
            {"taskId", activity.taskId},
            {"inspectedRelativePaths", activity.inspectedRelativePaths},
            {"proposedCreatedPaths", activity.proposedCreatedPaths},
            {"proposedModifiedPaths", activity.proposedModifiedPaths}});
    }
    return root.dump();
}

common::Result<std::optional<application::ManifestWorkLedgerWorkspaceSnapshot>>
deserializeWorkspaceLedgerSnapshot(const std::string& json)
{
    using Snapshot = application::ManifestWorkLedgerWorkspaceSnapshot;
    if (json.empty())
        return common::Result<std::optional<Snapshot>>::ok(std::nullopt);
    const auto root = nlohmann::json::parse(json, nullptr, false);
    const auto corrupt = [] {
        return common::Result<std::optional<Snapshot>>::failure(error(
            "manifest_work_ledger.corrupt_entry",
            "Corrupt workspace_scope_snapshot_json column"));
    };
    if (root.is_discarded() || !root.is_object()
        || root.value("schemaVersion", std::string {})
            != "manifest-workspace-ledger-snapshot.v1"
        || !root.contains("entries") || !root["entries"].is_array()
        || !root.contains("expansions") || !root["expansions"].is_array()
        || !root.contains("dependencyEdges") || !root["dependencyEdges"].is_array()
        || !root.contains("taskActivities") || !root["taskActivities"].is_array())
        return corrupt();

    Snapshot snapshot;
    try {
        for (const auto& item : root["entries"]) {
            if (!item.is_object()) return corrupt();
            application::RunWorkspaceScopeEntry entry;
            entry.relativePath = item.at("relativePath").get<std::string>();
            entry.reason = item.at("reason").get<std::string>();
            entry.taskId = item.at("taskId").get<std::string>();
            const auto role = application::workspaceScopeRoleFromString(
                item.at("role").get<std::string>());
            const auto origin = application::workspaceScopeOriginFromString(
                item.at("origin").get<std::string>());
            if (!role.has_value() || !origin.has_value()) return corrupt();
            entry.role = *role;
            entry.origin = *origin;
            if (!item.at("baselineHash").is_null())
                entry.baselineHash = item.at("baselineHash").get<std::string>();
            snapshot.entries.push_back(std::move(entry));
        }
        for (const auto& item : root["expansions"]) {
            if (!item.is_object()) return corrupt();
            application::RunWorkspaceScopeExpansion expansion;
            expansion.id = item.at("id").get<std::string>();
            expansion.runId = item.at("runId").get<std::string>();
            expansion.taskId = item.at("taskId").get<std::string>();
            expansion.relativePath = item.at("relativePath").get<std::string>();
            expansion.reason = item.at("reason").get<std::string>();
            const auto role = application::workspaceScopeRoleFromString(
                item.at("role").get<std::string>());
            if (!role.has_value()) return corrupt();
            expansion.role = *role;
            if (!item.at("baselineHash").is_null())
                expansion.baselineHash = item.at("baselineHash").get<std::string>();
            expansion.approvedAt = fromMilliseconds(item.at("approvedAtMs").get<std::int64_t>());
            snapshot.expansions.push_back(std::move(expansion));
        }
        for (const auto& item : root["dependencyEdges"]) {
            if (!item.is_object()) return corrupt();
            snapshot.dependencyEdges.push_back({
                item.at("dependentTaskId").get<std::string>(),
                item.at("providerTaskId").get<std::string>(),
                item.at("relativePath").get<std::string>()});
        }
        for (const auto& item : root["taskActivities"]) {
            if (!item.is_object()) return corrupt();
            application::ManifestWorkLedgerTaskWorkspaceActivity activity;
            activity.taskId = item.at("taskId").get<std::string>();
            activity.inspectedRelativePaths =
                item.at("inspectedRelativePaths").get<std::vector<std::string>>();
            activity.proposedCreatedPaths =
                item.at("proposedCreatedPaths").get<std::vector<std::string>>();
            activity.proposedModifiedPaths =
                item.at("proposedModifiedPaths").get<std::vector<std::string>>();
            snapshot.taskActivities.push_back(std::move(activity));
        }
    } catch (const std::exception&) {
        return corrupt();
    }
    if (snapshot.entries.size() > application::kRunWorkspaceScopeMaxEntries
        || snapshot.dependencyEdges.size() > application::kRunWorkspaceScopeMaxDependencyEdges)
        return corrupt();
    return common::Result<std::optional<Snapshot>>::ok(std::move(snapshot));
}

constexpr const char* kManifestWorkLedgerEntryColumns =
    "run_id,project_id,schema_version,terminal_status,terminal_reason,"
    "baseline_fingerprint,manifest_schema_version,disposition,task_dispositions,"
    "proposal_ids,proposal_lineage,affected_relative_paths,verification_identity_json,"
    "verification_summary,remaining_work_note,workspace_scope_schema_version,"
    "workspace_scope_summary,planning_input_fingerprint,planning_failure_signature,"
    "workspace_scope_snapshot_json,created_at_ms";

common::Result<application::ManifestWorkLedgerEntry> readManifestWorkLedgerEntryRow(
    sqlite3* handle, sqlite3_stmt* statement)
{
    application::ManifestWorkLedgerEntry entry;
    entry.runId = columnText(statement, 0);
    entry.projectId = columnText(statement, 1);
    entry.schemaVersion = columnText(statement, 2);
    const auto status = runStatusFromString(columnText(statement, 3));
    if (!status.has_value()) {
        return common::Result<application::ManifestWorkLedgerEntry>::failure(
            error("manifest_work_ledger.corrupt_entry", "Unknown terminal status in stored entry"));
    }
    entry.terminalStatus = *status;
    entry.terminalReason = columnText(statement, 4);
    entry.baselineFingerprint = columnText(statement, 5);
    entry.manifestSchemaVersion = columnText(statement, 6);
    const auto disposition = application::manifestWorkLedgerDispositionFromString(
        columnText(statement, 7));
    if (!disposition.has_value()) {
        return common::Result<application::ManifestWorkLedgerEntry>::failure(
            error("manifest_work_ledger.corrupt_entry", "Unknown disposition in stored entry"));
    }
    entry.disposition = *disposition;

    auto taskDispositions = deserializeTaskDispositions(columnText(statement, 8));
    if (!taskDispositions.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*taskDispositions.error());
    entry.taskDispositions = std::move(taskDispositions.value());

    auto proposalIds = deserializeStringArray(columnText(statement, 9), "proposal_ids");
    if (!proposalIds.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*proposalIds.error());
    entry.proposalIds = std::move(proposalIds.value());

    auto proposalLineage = deserializeProposalLineage(columnText(statement, 10));
    if (!proposalLineage.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*proposalLineage.error());
    entry.proposalLineage = std::move(proposalLineage.value());

    auto affectedRelativePaths = deserializeStringArray(columnText(statement, 11), "affected_relative_paths");
    if (!affectedRelativePaths.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*affectedRelativePaths.error());
    entry.affectedRelativePaths = std::move(affectedRelativePaths.value());

    const auto verificationColumn = optionalText(statement, 12);
    auto verification = deserializeVerificationIdentity(verificationColumn.value_or(std::string {}));
    if (!verification.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*verification.error());
    entry.verification = std::move(verification.value());

    entry.verificationSummary = columnText(statement, 13);
    entry.remainingWorkNote = columnText(statement, 14);
    entry.workspaceScopeSchemaVersion = columnText(statement, 15);
    entry.workspaceScopeSummary = columnText(statement, 16);
    entry.planningInputFingerprint = columnText(statement, 17);
    entry.planningFailureSignature = columnText(statement, 18);
    const auto workspaceSnapshotColumn = optionalText(statement, 19);
    auto workspaceSnapshot = deserializeWorkspaceLedgerSnapshot(
        workspaceSnapshotColumn.value_or(std::string {}));
    if (!workspaceSnapshot.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(
            *workspaceSnapshot.error());
    entry.workspaceSnapshot = std::move(workspaceSnapshot.value());
    entry.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 20));
    static_cast<void>(handle);
    return common::Result<application::ManifestWorkLedgerEntry>::ok(std::move(entry));
}

} // namespace

SqliteManifestWorkLedgerRepository::SqliteManifestWorkLedgerRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestWorkLedgerRepository::insertEntry(
    application::ManifestWorkLedgerEntry entry)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // AG-14.1: plain INSERT, not INSERT OR REPLACE - an entry is immutable
    // once written (ManifestWorkLedgerService::recordTerminalRun's own
    // "insert once, never replace" contract). A second insert for the same
    // run_id fails with manifest_work_ledger.duplicate_entry below instead
    // of silently clobbering a possibly-more-complete earlier entry.
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_work_ledger_entries "
        "(run_id,project_id,schema_version,terminal_status,terminal_reason,"
        "baseline_fingerprint,manifest_schema_version,disposition,task_dispositions,"
        "proposal_ids,proposal_lineage,affected_relative_paths,verification_identity_json,"
        "verification_summary,remaining_work_note,workspace_scope_schema_version,"
        "workspace_scope_summary,planning_input_fingerprint,planning_failure_signature,"
        "workspace_scope_snapshot_json,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const std::optional<std::string> verificationJson = entry.verification.has_value()
        ? std::optional<std::string>(serializeVerificationIdentity(*entry.verification))
        : std::nullopt;
    const std::optional<std::string> workspaceSnapshotJson = entry.workspaceSnapshot.has_value()
        ? std::optional<std::string>(serializeWorkspaceLedgerSnapshot(*entry.workspaceSnapshot))
        : std::nullopt;

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, entry.runId),
        bindText(handle, statement.get(), 2, entry.projectId),
        bindText(handle, statement.get(), 3, entry.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(entry.terminalStatus)),
        bindText(handle, statement.get(), 5, entry.terminalReason),
        bindText(handle, statement.get(), 6, entry.baselineFingerprint),
        bindText(handle, statement.get(), 7, entry.manifestSchemaVersion),
        bindText(handle, statement.get(), 8, application::toString(entry.disposition)),
        bindText(handle, statement.get(), 9, serializeTaskDispositions(entry.taskDispositions)),
        bindText(handle, statement.get(), 10, serializeStringArray(entry.proposalIds)),
        bindText(handle, statement.get(), 11, serializeProposalLineage(entry.proposalLineage)),
        bindText(handle, statement.get(), 12, serializeStringArray(entry.affectedRelativePaths)),
        bindNullableText(handle, statement.get(), 13, verificationJson),
        bindText(handle, statement.get(), 14, entry.verificationSummary),
        bindText(handle, statement.get(), 15, entry.remainingWorkNote),
        bindText(handle, statement.get(), 16, entry.workspaceScopeSchemaVersion),
        bindText(handle, statement.get(), 17, entry.workspaceScopeSummary),
        bindText(handle, statement.get(), 18, entry.planningInputFingerprint),
        bindText(handle, statement.get(), 19, entry.planningFailureSignature),
        bindNullableText(handle, statement.get(), 20, workspaceSnapshotJson),
        bindInt64(handle, statement.get(), 21, toMilliseconds(entry.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "manifest_work_ledger.duplicate_entry",
                "A Work Ledger entry already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "manifest_work_ledger.write_failed", "Could not persist Work Ledger entry"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestWorkLedgerEntry>>
SqliteManifestWorkLedgerRepository::findEntry(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + kManifestWorkLedgerEntryColumns
        + " FROM manifest_work_ledger_entries WHERE run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            databaseError(handle, "manifest_work_ledger.read_failed", "Could not read Work Ledger entry"));
    }
    const auto entry = readManifestWorkLedgerEntryRow(handle, statement.get());
    if (!entry.success()) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            *entry.error());
    }
    return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::ok(entry.value());
}

common::Result<std::vector<application::ManifestWorkLedgerEntry>>
SqliteManifestWorkLedgerRepository::listRecentForProject(
    const application::ProjectId& projectId, std::size_t limit) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // run_id is a stable secondary sort key: two entries sharing the same
    // created_at_ms millisecond must still produce a deterministic order
    // (AG-14.1) rather than depending on SQLite's unspecified tie order.
    const std::string sql = std::string("SELECT ") + kManifestWorkLedgerEntryColumns
        + " FROM manifest_work_ledger_entries WHERE project_id=? "
          "ORDER BY created_at_ms DESC, run_id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
            *prepared.error());
    }
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, projectId),
        bindInt64(handle, statement.get(), 2, static_cast<std::int64_t>(limit))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) {
            return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
                *binding.error());
        }
    }
    std::vector<application::ManifestWorkLedgerEntry> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
                databaseError(handle, "manifest_work_ledger.read_failed",
                    "Could not list Work Ledger entries"));
        }
        const auto entry = readManifestWorkLedgerEntryRow(handle, statement.get());
        if (!entry.success()) {
            return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
                *entry.error());
        }
        results.push_back(entry.value());
    }
    return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::ok(std::move(results));
}

common::Result<void> SqliteManifestWorkLedgerRepository::appendTransition(
    application::ManifestWorkLedgerReviewTransition transition)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_work_ledger_transitions "
        "(id,run_id,change_set_id,kind,occurred_at_ms,actor,reason,"
        "post_action_workspace_fingerprint) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, transition.id),
        bindText(handle, statement.get(), 2, transition.runId),
        bindText(handle, statement.get(), 3, transition.changeSetId),
        bindText(handle, statement.get(), 4, application::toString(transition.kind)),
        bindInt64(handle, statement.get(), 5, toMilliseconds(transition.occurredAt)),
        bindNullableText(handle, statement.get(), 6, transition.actor),
        bindNullableText(handle, statement.get(), 7, transition.reason),
        bindNullableText(handle, statement.get(), 8, transition.postActionWorkspaceFingerprint)
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            // AG-14.2: deterministic transition ids (changeSetId + kind, see
            // ManifestWorkLedgerService::recordReviewTransition) make this a
            // benign idempotent duplicate, never a real conflict.
            return common::Result<void>::failure(error(
                "manifest_work_ledger.duplicate_transition",
                "A Work Ledger transition already exists for this change set and kind"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "manifest_work_ledger.write_failed", "Could not persist Work Ledger transition"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>
SqliteManifestWorkLedgerRepository::listTransitionsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,change_set_id,kind,occurred_at_ms,actor,reason,"
        "post_action_workspace_fingerprint FROM manifest_work_ledger_transitions "
        "WHERE run_id=? ORDER BY occurred_at_ms,id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
            *bound.error());
    }
    std::vector<application::ManifestWorkLedgerReviewTransition> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
                databaseError(handle, "manifest_work_ledger.read_failed",
                    "Could not read Work Ledger transitions"));
        }
        application::ManifestWorkLedgerReviewTransition transition;
        transition.id = columnText(statement.get(), 0);
        transition.runId = runId;
        transition.changeSetId = columnText(statement.get(), 1);
        const auto kind = application::manifestWorkLedgerTransitionKindFromString(
            columnText(statement.get(), 2));
        if (!kind.has_value()) {
            return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
                error("manifest_work_ledger.corrupt_transition", "Unknown transition kind in stored row"));
        }
        transition.kind = *kind;
        transition.occurredAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
        transition.actor = optionalText(statement.get(), 4);
        transition.reason = optionalText(statement.get(), 5);
        transition.postActionWorkspaceFingerprint = optionalText(statement.get(), 6);
        results.push_back(std::move(transition));
    }
    return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::ok(
        std::move(results));
}

} // namespace ai::infrastructure
