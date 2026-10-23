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

nlohmann::json workspaceScopeEntryToJson(const application::RunWorkspaceScopeEntry& entry)
{
    nlohmann::json j {
        {"relativePath", entry.relativePath},
        {"role", application::toString(entry.role)},
        {"reason", entry.reason},
        {"taskId", entry.taskId},
        {"origin", application::toString(entry.origin)}
    };
    j["baselineHash"] = entry.baselineHash.has_value()
        ? nlohmann::json(*entry.baselineHash) : nlohmann::json(nullptr);
    return j;
}

// AG-15: fails closed on any malformed entry - never silently drops or
// defaults a workspace-scope entry, matching AG-14.2's own array-parsing
// discipline.
common::Result<application::RunWorkspaceScopeEntry> workspaceScopeEntryFromJson(
    const nlohmann::json& j)
{
    const auto corrupt = [] {
        return common::Result<application::RunWorkspaceScopeEntry>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope entry"));
    };
    if (!j.is_object() || !j.contains("relativePath") || !j["relativePath"].is_string()
        || !j.contains("role") || !j["role"].is_string()
        || !j.contains("reason") || !j["reason"].is_string()
        || !j.contains("taskId") || !j["taskId"].is_string()
        || !j.contains("origin") || !j["origin"].is_string()
        || !j.contains("baselineHash")
        || !(j["baselineHash"].is_null() || j["baselineHash"].is_string())) {
        return corrupt();
    }
    application::RunWorkspaceScopeEntry entry;
    entry.relativePath = j["relativePath"].get<std::string>();
    const auto role = application::workspaceScopeRoleFromString(j["role"].get<std::string>());
    if (!role.has_value()) return corrupt();
    entry.role = *role;
    entry.reason = j["reason"].get<std::string>();
    entry.taskId = j["taskId"].get<std::string>();
    if (j["baselineHash"].is_string()) entry.baselineHash = j["baselineHash"].get<std::string>();
    const auto origin = application::workspaceScopeOriginFromString(j["origin"].get<std::string>());
    if (!origin.has_value()) return corrupt();
    entry.origin = *origin;
    return common::Result<application::RunWorkspaceScopeEntry>::ok(std::move(entry));
}

std::string workspaceScopeEntriesToJson(const std::vector<application::RunWorkspaceScopeEntry>& entries)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries) array.push_back(workspaceScopeEntryToJson(entry));
    return array.dump();
}

common::Result<std::vector<application::RunWorkspaceScopeEntry>> workspaceScopeEntriesFromJson(
    const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::RunWorkspaceScopeEntry>>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope entries column"));
    }
    std::vector<application::RunWorkspaceScopeEntry> entries;
    entries.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto entry = workspaceScopeEntryFromJson(item);
        if (!entry.success()) return common::Result<std::vector<application::RunWorkspaceScopeEntry>>::failure(
            *entry.error());
        entries.push_back(std::move(entry.value()));
    }
    return common::Result<std::vector<application::RunWorkspaceScopeEntry>>::ok(std::move(entries));
}

nlohmann::json workspaceScopeEdgeToJson(const application::WorkspaceScopeDependencyEdge& edge)
{
    return nlohmann::json {
        {"dependentTaskId", edge.dependentTaskId},
        {"providerTaskId", edge.providerTaskId},
        {"relativePath", edge.relativePath}
    };
}

common::Result<application::WorkspaceScopeDependencyEdge> workspaceScopeEdgeFromJson(
    const nlohmann::json& j)
{
    if (!j.is_object() || !j.contains("dependentTaskId") || !j["dependentTaskId"].is_string()
        || !j.contains("providerTaskId") || !j["providerTaskId"].is_string()
        || !j.contains("relativePath") || !j["relativePath"].is_string()) {
        return common::Result<application::WorkspaceScopeDependencyEdge>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope dependency edge"));
    }
    return common::Result<application::WorkspaceScopeDependencyEdge>::ok({
        j["dependentTaskId"].get<std::string>(),
        j["providerTaskId"].get<std::string>(),
        j["relativePath"].get<std::string>()});
}

std::string workspaceScopeEdgesToJson(
    const std::vector<application::WorkspaceScopeDependencyEdge>& edges)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& edge : edges) array.push_back(workspaceScopeEdgeToJson(edge));
    return array.dump();
}

common::Result<std::vector<application::WorkspaceScopeDependencyEdge>> workspaceScopeEdgesFromJson(
    const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::WorkspaceScopeDependencyEdge>>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope dependency edges column"));
    }
    std::vector<application::WorkspaceScopeDependencyEdge> edges;
    edges.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto edge = workspaceScopeEdgeFromJson(item);
        if (!edge.success()) return common::Result<std::vector<application::WorkspaceScopeDependencyEdge>>::failure(
            *edge.error());
        edges.push_back(std::move(edge.value()));
    }
    return common::Result<std::vector<application::WorkspaceScopeDependencyEdge>>::ok(std::move(edges));
}

} // namespace

SqliteRunWorkspaceScopeRepository::SqliteRunWorkspaceScopeRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteRunWorkspaceScopeRepository::freeze(application::RunWorkspaceScope scope)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_workspace_scopes "
        "(run_id,schema_version,manifest_schema_version,entries_json,dependency_edges_json,"
        "frozen_at_ms) VALUES (?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, scope.runId),
        bindText(handle, statement.get(), 2, scope.schemaVersion),
        bindText(handle, statement.get(), 3, scope.manifestSchemaVersion),
        bindText(handle, statement.get(), 4, workspaceScopeEntriesToJson(scope.entries)),
        bindText(handle, statement.get(), 5, workspaceScopeEdgesToJson(scope.dependencyEdges)),
        bindInt64(handle, statement.get(), 6, toMilliseconds(scope.frozenAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "workspace_scope.already_frozen",
                "A workspace scope already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "workspace_scope.write_failed", "Could not freeze workspace scope"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::RunWorkspaceScope>>
SqliteRunWorkspaceScopeRepository::findForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT schema_version,manifest_schema_version,entries_json,dependency_edges_json,"
        "frozen_at_ms FROM run_workspace_scopes WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(databaseError(
            handle, "workspace_scope.read_failed", "Could not read frozen workspace scope"));
    }
    application::RunWorkspaceScope scope;
    scope.runId = runId;
    scope.schemaVersion = columnText(statement.get(), 0);
    scope.manifestSchemaVersion = columnText(statement.get(), 1);
    auto entries = workspaceScopeEntriesFromJson(columnText(statement.get(), 2));
    if (!entries.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*entries.error());
    }
    scope.entries = std::move(entries.value());
    auto edges = workspaceScopeEdgesFromJson(columnText(statement.get(), 3));
    if (!edges.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*edges.error());
    }
    scope.dependencyEdges = std::move(edges.value());
    scope.frozenAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 4));
    return common::Result<std::optional<application::RunWorkspaceScope>>::ok(std::move(scope));
}

common::Result<void> SqliteRunWorkspaceScopeRepository::recordExpansion(
    application::RunWorkspaceScopeExpansion expansion)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_workspace_scope_expansions "
        "(id,run_id,task_id,relative_path,role,reason,baseline_hash,approved_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, expansion.id),
        bindText(handle, statement.get(), 2, expansion.runId),
        bindText(handle, statement.get(), 3, expansion.taskId),
        bindText(handle, statement.get(), 4, expansion.relativePath),
        bindText(handle, statement.get(), 5, application::toString(expansion.role)),
        bindText(handle, statement.get(), 6, expansion.reason),
        bindNullableText(handle, statement.get(), 7, expansion.baselineHash),
        bindInt64(handle, statement.get(), 8, toMilliseconds(expansion.approvedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY
            || sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure(error(
                "workspace_scope.expansion_already_recorded",
                "This path was already recorded as an expansion for this Run/task"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "workspace_scope.write_failed", "Could not persist workspace scope expansion"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::RunWorkspaceScopeExpansion>>
SqliteRunWorkspaceScopeRepository::listExpansionsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,task_id,relative_path,role,reason,baseline_hash,approved_at_ms "
        "FROM run_workspace_scope_expansions WHERE run_id=? ORDER BY approved_at_ms, id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
            *bound.error());
    }
    std::vector<application::RunWorkspaceScopeExpansion> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
                databaseError(handle, "workspace_scope.read_failed",
                    "Could not list workspace scope expansions"));
        }
        application::RunWorkspaceScopeExpansion expansion;
        expansion.id = columnText(statement.get(), 0);
        expansion.runId = runId;
        expansion.taskId = columnText(statement.get(), 1);
        expansion.relativePath = columnText(statement.get(), 2);
        const auto role = application::workspaceScopeRoleFromString(columnText(statement.get(), 3));
        if (!role.has_value()) {
            return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
                error("workspace_scope.corrupt_entry", "Unknown workspace scope expansion role"));
        }
        expansion.role = *role;
        expansion.reason = columnText(statement.get(), 4);
        expansion.baselineHash = optionalText(statement.get(), 5);
        expansion.approvedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(expansion));
    }
    return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::ok(std::move(results));
}

common::Result<void> SqliteRunWorkspaceScopeRepository::recordDiagnosticRead(
    application::RunWorkspaceScopeDiagnosticRead grant)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_workspace_scope_diagnostic_reads "
        "(id,run_id,task_id,relative_path,finding_code,reason,baseline_hash,granted_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, grant.id),
        bindText(handle, statement.get(), 2, grant.runId),
        bindText(handle, statement.get(), 3, grant.taskId),
        bindText(handle, statement.get(), 4, grant.relativePath),
        bindText(handle, statement.get(), 5, grant.findingCode),
        bindText(handle, statement.get(), 6, grant.reason),
        bindNullableText(handle, statement.get(), 7, grant.baselineHash),
        bindInt64(handle, statement.get(), 8, toMilliseconds(grant.grantedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY
            || sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure(error(
                "workspace_scope.diagnostic_read_already_recorded",
                "This path was already recorded as a diagnostic read for this Run/task"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "workspace_scope.write_failed", "Could not persist diagnostic read grant"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>
SqliteRunWorkspaceScopeRepository::listDiagnosticReadsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,task_id,relative_path,finding_code,reason,baseline_hash,granted_at_ms "
        "FROM run_workspace_scope_diagnostic_reads WHERE run_id=? ORDER BY granted_at_ms, id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::failure(
            *bound.error());
    }
    std::vector<application::RunWorkspaceScopeDiagnosticRead> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::failure(
                databaseError(handle, "workspace_scope.read_failed",
                    "Could not list diagnostic read grants"));
        }
        application::RunWorkspaceScopeDiagnosticRead grant;
        grant.id = columnText(statement.get(), 0);
        grant.runId = runId;
        grant.taskId = columnText(statement.get(), 1);
        grant.relativePath = columnText(statement.get(), 2);
        grant.findingCode = columnText(statement.get(), 3);
        grant.reason = columnText(statement.get(), 4);
        grant.baselineHash = optionalText(statement.get(), 5);
        grant.grantedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(grant));
    }
    return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::ok(
        std::move(results));
}

} // namespace ai::infrastructure
