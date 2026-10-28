#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "SqliteRunDraftGroupStorage.hpp"

#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/planning/RunDraftTypes.hpp"
#include "ai/application/execution/RunInterfaceContract.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::RunDraft;
using application::RunLaunchMode;
using application::StageModelAssignment;
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
using sqlite_detail::columnText;
using sqlite_detail::optionalText;
using sqlite_detail::pathToUtf8;
using sqlite_detail::pathFromUtf8;
using sqlite_detail::serializeStringVector;
using sqlite_detail::parseStringVector;
using sqlite_detail::readRunAssignment;
using sqlite_detail::providerBudgetOverridesJson;
using sqlite_detail::parseProviderBudgetOverrides;
using sqlite_detail::insertRunDraftGroupRows;
using sqlite_detail::findRunDraftGroupWhere;
using sqlite_detail::readRunDraftGroupDrafts;

constexpr const char* runDraftColumns =
    "id, name, project_id, title, task, workflow_id, launch_mode, execution_profile_id, "
    "max_parallelism, workspace_path, created_at_ms, updated_at_ms, verification_repair_policy, "
    "provider_budget_json, run_interface_contracts_json, "
    "group_id, group_ordinal, plan_key, depends_on_json, created_content_fingerprint";

constexpr const char* runDraftGroupColumns =
    "id, project_id, origin_kind, idempotency_key, plan_title, plan_fingerprint, source_conversation_id, "
    "source_message_id, source_message_fingerprint, context_fingerprint, source_run_ids_json, created_at_ms, "
    "source_request_id";

// Same column shape/order as runStageAssignmentColumns (minus the first
// column being draft_id instead of run_id) so readRunAssignment() can be
// reused verbatim for both tables.
constexpr const char* runDraftAssignmentColumns =
    "draft_id, stage_type, provider_id, model_id, timeout_ms, agent_name, role, instructions, "
    "position, lead_architect, dependencies_json, max_attempts, fallback_provider_id, fallback_model_id, "
    "workspace_access";

// The five provenance columns of a run_drafts row starting at `first` (group_id, group_ordinal, plan_key,
// depends_on_json, created_content_fingerprint). All NULL = a manual draft (no provenance); all set = a draft of a
// group. Anything in between was not written by this code and is reported, never repaired.
common::Result<std::optional<application::RunDraftProvenance>> readRunDraftProvenance(
    sqlite3_stmt* statement, int first)
{
    using Result = common::Result<std::optional<application::RunDraftProvenance>>;
    std::size_t present = 0;
    for (int column = first; column < first + 5; ++column) {
        if (sqlite3_column_type(statement, column) != SQLITE_NULL) ++present;
    }
    if (present == 0) return Result::ok(std::nullopt);
    const std::int64_t ordinal = sqlite3_column_int64(statement, first + 1);
    if (present != 5 || ordinal < 0 || ordinal > std::numeric_limits<std::uint32_t>::max()) {
        return Result::failure(error("persistence.invalid_data", "Run draft row contains incomplete group provenance"));
    }
    auto dependsOn = parseStringVector(columnText(statement, first + 3), "Run draft depends_on_json");
    if (!dependsOn.success()) return Result::failure(*dependsOn.error());
    application::RunDraftProvenance value;
    value.groupId = columnText(statement, first);
    value.ordinal = static_cast<std::uint32_t>(ordinal);
    value.planKey = columnText(statement, first + 2);
    value.dependsOn = std::move(dependsOn.value());
    value.createdContentFingerprint = columnText(statement, first + 4);
    return Result::ok(std::move(value));
}

common::Result<RunDraft> readRunDraft(sqlite3_stmt* statement)
{
    // runDraftColumns order: id(0) name(1) project_id(2) title(3) task(4)
    // workflow_id(5) launch_mode(6) execution_profile_id(7)
    // max_parallelism(8) workspace_path(9) created_at_ms(10) updated_at_ms(11)
    // verification_repair_policy(12) provider_budget_json(13) run_interface_contracts_json(14)
    const std::int64_t launchModeValue = sqlite3_column_int64(statement, 6);
    if (launchModeValue < 0 || launchModeValue > 2) {
        return common::Result<RunDraft>::failure(
            error("persistence.invalid_data", "Run draft row contains an unknown launch mode"));
    }
    const std::int64_t maxParallelism = sqlite3_column_int64(statement, 8);
    if (maxParallelism <= 0) {
        return common::Result<RunDraft>::failure(
            error("persistence.invalid_data", "Run draft row contains a non-positive max parallelism"));
    }
    const auto verificationRepairPolicy =
        application::verificationRepairPolicyFromString(columnText(statement, 12));
    if (!verificationRepairPolicy.has_value()) {
        return common::Result<RunDraft>::failure(error(
            "persistence.invalid_data",
            "Run draft row contains an unknown verification repair policy"));
    }
    const auto providerBudget = parseProviderBudgetOverrides(columnText(statement, 13));
    if (!providerBudget.success()) {
        return common::Result<RunDraft>::failure(error(
            "persistence.invalid_data", "Run draft row contains corrupt provider budget limits"));
    }
    const auto interfaceContracts =
        application::parseRunInterfaceContracts(columnText(statement, 14));
    if (!interfaceContracts.success()) {
        return common::Result<RunDraft>::failure(*interfaceContracts.error());
    }

    // PC-3B provenance: group_id(15) group_ordinal(16) plan_key(17) depends_on_json(18)
    // created_content_fingerprint(19).
    auto provenance = readRunDraftProvenance(statement, 15);
    if (!provenance.success()) return common::Result<RunDraft>::failure(*provenance.error());

    RunDraft draft;
    draft.id = columnText(statement, 0);
    draft.name = columnText(statement, 1);
    draft.projectId = columnText(statement, 2);
    draft.title = columnText(statement, 3);
    draft.task = columnText(statement, 4);
    draft.workflowId = columnText(statement, 5);
    draft.launchMode = static_cast<RunLaunchMode>(launchModeValue);
    draft.executionProfileId = optionalText(statement, 7);
    draft.maxParallelism = static_cast<std::uint32_t>(maxParallelism);
    const auto workspaceText = optionalText(statement, 9);
    draft.workspacePath = workspaceText.has_value()
        ? std::optional<std::filesystem::path>(pathFromUtf8(*workspaceText))
        : std::nullopt;
    draft.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    draft.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 11));
    draft.verificationRepairPolicy = *verificationRepairPolicy;
    draft.providerBudget = providerBudget.value();
    draft.interfaceContracts = interfaceContracts.value();
    draft.provenance = std::move(provenance.value());
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<void> bindNullableInt64(
    sqlite3* handle, sqlite3_stmt* statement, int index, const std::optional<std::int64_t>& value)
{
    if (value.has_value()) return bindInt64(handle, statement, index, *value);
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind null integer value"));
    }
    return common::Result<void>::ok();
}

// runDraftGroupColumns order: id(0) project_id(1) origin_kind(2) idempotency_key(3) plan_title(4)
// plan_fingerprint(5) source_conversation_id(6) source_message_id(7) source_message_fingerprint(8)
// context_fingerprint(9) source_run_ids_json(10) created_at_ms(11) source_request_id(12)
common::Result<application::RunDraftGroup> readRunDraftGroup(sqlite3_stmt* statement)
{
    const auto origin = application::runDraftGroupOriginFromString(columnText(statement, 2));
    if (!origin.has_value()) {
        return common::Result<application::RunDraftGroup>::failure(
            error("persistence.invalid_data", "Run draft group row contains an unknown origin"));
    }
    auto sourceRuns = parseStringVector(columnText(statement, 10), "Run draft group source_run_ids_json");
    if (!sourceRuns.success()) {
        return common::Result<application::RunDraftGroup>::failure(*sourceRuns.error());
    }
    application::RunDraftGroup group;
    group.id = columnText(statement, 0);
    group.projectId = columnText(statement, 1);
    group.origin = *origin;
    group.idempotencyKey = columnText(statement, 3);
    group.planTitle = columnText(statement, 4);
    group.planFingerprint = columnText(statement, 5);
    group.sourceConversationId = optionalText(statement, 6);
    group.sourceMessageId = optionalText(statement, 7);
    group.sourceMessageFingerprint = optionalText(statement, 8);
    group.contextFingerprint = optionalText(statement, 9);
    group.sourceRunIds = std::move(sourceRuns.value());
    group.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 11));
    group.sourceRequestId = optionalText(statement, 12);
    return common::Result<application::RunDraftGroup>::ok(std::move(group));
}

common::Result<void> bindRunDraftGroup(
    sqlite3* handle, sqlite3_stmt* statement, const application::RunDraftGroup& group)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, group.id),
        bindText(handle, statement, 2, group.projectId),
        bindText(handle, statement, 3, application::toString(group.origin)),
        bindText(handle, statement, 4, group.idempotencyKey),
        bindText(handle, statement, 5, group.planTitle),
        bindText(handle, statement, 6, group.planFingerprint),
        bindNullableText(handle, statement, 7, group.sourceConversationId),
        bindNullableText(handle, statement, 8, group.sourceMessageId),
        bindNullableText(handle, statement, 9, group.sourceMessageFingerprint),
        bindNullableText(handle, statement, 10, group.contextFingerprint),
        bindText(handle, statement, 11, serializeStringVector(group.sourceRunIds)),
        bindInt64(handle, statement, 12, toMilliseconds(group.createdAt)),
        bindNullableText(handle, statement, 13, group.sourceRequestId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    return common::Result<void>::ok();
}

common::Result<void> bindRunDraft(sqlite3* handle, sqlite3_stmt* statement, const RunDraft& draft)
{
    const auto& provenance = draft.provenance;
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, draft.id),
        bindText(handle, statement, 2, draft.name),
        bindText(handle, statement, 3, draft.projectId),
        bindText(handle, statement, 4, draft.title),
        bindText(handle, statement, 5, draft.task),
        bindText(handle, statement, 6, draft.workflowId),
        bindInt64(handle, statement, 7, static_cast<std::int64_t>(draft.launchMode)),
        bindNullableText(handle, statement, 8, draft.executionProfileId),
        bindUnsigned(handle, statement, 9, draft.maxParallelism),
        bindNullableText(handle, statement, 10,
            draft.workspacePath.has_value()
                ? std::optional<std::string>(pathToUtf8(*draft.workspacePath))
                : std::nullopt),
        bindInt64(handle, statement, 11, toMilliseconds(draft.createdAt)),
        bindInt64(handle, statement, 12, toMilliseconds(draft.updatedAt)),
        bindText(handle, statement, 13,
            application::toString(draft.verificationRepairPolicy)),
        bindText(handle, statement, 14, providerBudgetOverridesJson(draft.providerBudget)),
        bindText(handle, statement, 15,
            application::serializeRunInterfaceContracts(draft.interfaceContracts)),
        // A manual draft binds five NULLs; a draft of a group binds its provenance (insertGroup only).
        bindNullableText(handle, statement, 16,
            provenance.has_value() ? std::optional<std::string>(provenance->groupId) : std::nullopt),
        bindNullableInt64(handle, statement, 17,
            provenance.has_value() ? std::optional<std::int64_t>(provenance->ordinal) : std::nullopt),
        bindNullableText(handle, statement, 18,
            provenance.has_value() ? std::optional<std::string>(provenance->planKey) : std::nullopt),
        bindNullableText(handle, statement, 19,
            provenance.has_value() ? std::optional<std::string>(serializeStringVector(provenance->dependsOn))
                                   : std::nullopt),
        bindNullableText(handle, statement, 20,
            provenance.has_value() ? std::optional<std::string>(provenance->createdContentFingerprint)
                                   : std::nullopt)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

// Same column shape as run_stage_assignments (see runDraftAssignmentColumns),
// so readRunAssignment() is reused as-is.
common::Result<std::vector<StageModelAssignment>> fetchAssignmentsForDraft(
    sqlite3* handle,
    const std::string& draftId)
{
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftAssignmentColumns
        + " FROM run_draft_assignments WHERE draft_id=? ORDER BY position,stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, draftId);
    if (!bound.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*bound.error());
    }

    std::vector<StageModelAssignment> assignments;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<StageModelAssignment>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read run draft assignments"));
        }
        auto assignment = readRunAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::vector<StageModelAssignment>>::failure(*assignment.error());
        }
        assignments.push_back(assignment.value());
    }
    return common::Result<std::vector<StageModelAssignment>>::ok(std::move(assignments));
}

common::Result<void> insertRunDraftAssignmentRow(
    sqlite3* handle,
    const application::RunDraftId& draftId,
    const StageModelAssignment& assignment)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_draft_assignments (draft_id,stage_type,provider_id,model_id,timeout_ms,"
        "agent_name,role,instructions,position,lead_architect,dependencies_json,max_attempts,"
        "fallback_provider_id,fallback_model_id,workspace_access) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, draftId),
        bindText(handle, statement.get(), 2, assignment.stageType),
        bindText(handle, statement.get(), 3, assignment.providerId),
        bindText(handle, statement.get(), 4, assignment.modelId),
        bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count())),
        bindText(handle, statement.get(), 6, assignment.agentName),
        bindText(handle, statement.get(), 7, assignment.role),
        bindText(handle, statement.get(), 8, assignment.instructions),
        bindInt64(handle, statement.get(), 9, assignment.position),
        bindInt64(handle, statement.get(), 10, assignment.leadArchitect ? 1 : 0),
        bindText(handle, statement.get(), 11, serializeStringVector(assignment.dependencies)),
        bindUnsigned(handle, statement.get(), 12, assignment.maxAttempts),
        bindNullableText(handle, statement.get(), 13, assignment.fallbackProviderId),
        bindNullableText(handle, statement.get(), 14, assignment.fallbackModelId),
        bindInt64(handle, statement.get(), 15, assignment.workspaceAccess ? 1 : 0)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "run_draft.duplicate_stage_type",
                "A stage type cannot appear more than once in one draft's roster"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run draft assignment"));
    }
    return common::Result<void>::ok();
}

} // namespace

SqliteRunDraftRepository::SqliteRunDraftRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<RunDraft> SqliteRunDraftRepository::insert(RunDraft draft)
{
    // A draft that belongs to a group is written only together with its group row (insertGroup).
    if (draft.provenance.has_value()) {
        return common::Result<RunDraft>::failure(error("run_draft.group_insert_required",
            "A draft that belongs to a group is created through insertGroup"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<RunDraft>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<RunDraft>::failure(failure);
    };

    {
        Statement statement;
        const std::string sql = std::string("INSERT INTO run_drafts (") + runDraftColumns
            + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        const auto prepared = prepare(handle, sql.c_str(), statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindRunDraft(handle, statement.get(), draft);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(error("run_draft.duplicate_id", "Run draft id already exists"));
            }
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not insert run draft"));
        }
    }

    for (const auto& assignment : draft.roster) {
        const auto inserted = insertRunDraftAssignmentRow(handle, draft.id, assignment);
        if (!inserted.success()) return rollbackWith(*inserted.error());
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<RunDraft> SqliteRunDraftRepository::update(RunDraft draft)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<RunDraft>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<RunDraft>::failure(failure);
    };

    // The group and the plan provenance never change: read what is stored (inside the transaction, so nothing
    // can move between the check and the write) and refuse a draft that says otherwise. The UPDATE below does
    // not even name those columns, and the v62 triggers refuse them for any other writer.
    {
        Statement stored;
        const auto prepared = prepare(handle,
            "SELECT project_id, group_id, group_ordinal, plan_key, depends_on_json, created_content_fingerprint "
            "FROM run_drafts WHERE id=?;",
            stored);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, stored.get(), 1, draft.id);
        if (!bound.success()) return rollbackWith(*bound.error());
        const int step = sqlite3_step(stored.get());
        if (step == SQLITE_DONE) return rollbackWith(error("run_draft.not_found", "Run draft not found"));
        if (step != SQLITE_ROW) {
            return rollbackWith(databaseError(handle, "persistence.read_failed", "Could not read run draft"));
        }
        const auto provenance = readRunDraftProvenance(stored.get(), 1);
        if (!provenance.success()) return rollbackWith(*provenance.error());
        const auto unchanged = application::checkRunDraftProvenanceUnchanged(
            provenance.value(), columnText(stored.get(), 0), draft);
        if (!unchanged.success()) return rollbackWith(*unchanged.error());
    }

    {
        Statement statement;
        const auto prepared = prepare(handle,
            "UPDATE run_drafts SET name=?,project_id=?,title=?,task=?,workflow_id=?,launch_mode=?,"
            "execution_profile_id=?,max_parallelism=?,workspace_path=?,created_at_ms=?,updated_at_ms=?,"
            "verification_repair_policy=?,provider_budget_json=?,run_interface_contracts_json=? "
            "WHERE id=?;",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, draft.name),
            bindText(handle, statement.get(), 2, draft.projectId),
            bindText(handle, statement.get(), 3, draft.title),
            bindText(handle, statement.get(), 4, draft.task),
            bindText(handle, statement.get(), 5, draft.workflowId),
            bindInt64(handle, statement.get(), 6, static_cast<std::int64_t>(draft.launchMode)),
            bindNullableText(handle, statement.get(), 7, draft.executionProfileId),
            bindUnsigned(handle, statement.get(), 8, draft.maxParallelism),
            bindNullableText(handle, statement.get(), 9,
                draft.workspacePath.has_value()
                    ? std::optional<std::string>(pathToUtf8(*draft.workspacePath))
                    : std::nullopt),
            bindInt64(handle, statement.get(), 10, toMilliseconds(draft.createdAt)),
            bindInt64(handle, statement.get(), 11, toMilliseconds(draft.updatedAt)),
            bindText(handle, statement.get(), 12,
                application::toString(draft.verificationRepairPolicy)),
            bindText(handle, statement.get(), 13, providerBudgetOverridesJson(draft.providerBudget)),
            bindText(handle, statement.get(), 14,
                application::serializeRunInterfaceContracts(draft.interfaceContracts)),
            bindText(handle, statement.get(), 15, draft.id)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not update run draft"));
        }
        if (sqlite3_changes(handle) == 0) {
            return rollbackWith(error("run_draft.not_found", "Run draft not found"));
        }
    }

    {
        Statement remove;
        const auto removePrepared = prepare(
            handle, "DELETE FROM run_draft_assignments WHERE draft_id=?;", remove);
        if (!removePrepared.success()) return rollbackWith(*removePrepared.error());
        const auto removeBound = bindText(handle, remove.get(), 1, draft.id);
        if (!removeBound.success()) return rollbackWith(*removeBound.error());
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not replace run draft assignments"));
        }
    }

    for (const auto& assignment : draft.roster) {
        const auto inserted = insertRunDraftAssignmentRow(handle, draft.id, assignment);
        if (!inserted.success()) return rollbackWith(*inserted.error());
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<std::optional<RunDraft>> SqliteRunDraftRepository::findById(
    const application::RunDraftId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftColumns + " FROM run_drafts WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::optional<RunDraft>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<RunDraft>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<RunDraft>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<RunDraft>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read run draft"));
    }
    auto draft = readRunDraft(statement.get());
    if (!draft.success()) {
        return common::Result<std::optional<RunDraft>>::failure(*draft.error());
    }

    const auto roster = fetchAssignmentsForDraft(handle, id);
    if (!roster.success()) {
        return common::Result<std::optional<RunDraft>>::failure(*roster.error());
    }
    draft.value().roster = roster.value();
    return common::Result<std::optional<RunDraft>>::ok(draft.value());
}

common::Result<std::vector<RunDraft>> SqliteRunDraftRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftColumns
        + " FROM run_drafts ORDER BY updated_at_ms DESC,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<RunDraft>>::failure(*prepared.error());

    std::vector<RunDraft> drafts;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<RunDraft>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run drafts"));
        }
        auto draft = readRunDraft(statement.get());
        if (!draft.success()) {
            return common::Result<std::vector<RunDraft>>::failure(*draft.error());
        }
        drafts.push_back(draft.value());
    }

    for (auto& draft : drafts) {
        const auto roster = fetchAssignmentsForDraft(handle, draft.id);
        if (!roster.success()) {
            return common::Result<std::vector<RunDraft>>::failure(*roster.error());
        }
        draft.roster = roster.value();
    }
    return common::Result<std::vector<RunDraft>>::ok(std::move(drafts));
}

common::Result<void> SqliteRunDraftRepository::remove(const application::RunDraftId& id)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // One transaction: the draft, and - when it was the last one of its group - the group row. If the group
    // cannot be removed the draft stays too.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    std::optional<std::string> groupId;
    {
        Statement find;
        const auto prepared = prepare(handle, "SELECT group_id FROM run_drafts WHERE id=?;", find);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, find.get(), 1, id);
        if (!bound.success()) return rollbackWith(*bound.error());
        const int step = sqlite3_step(find.get());
        if (step == SQLITE_DONE) return rollbackWith(error("run_draft.not_found", "Run draft not found"));
        if (step != SQLITE_ROW) {
            return rollbackWith(databaseError(handle, "persistence.read_failed", "Could not read run draft"));
        }
        groupId = optionalText(find.get(), 0);
    }
    {
        Statement statement;
        const auto prepared = prepare(handle, "DELETE FROM run_drafts WHERE id=?;", statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, statement.get(), 1, id);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(handle, "persistence.write_failed", "Could not remove run draft"));
        }
        if (sqlite3_changes(handle) == 0) return rollbackWith(error("run_draft.not_found", "Run draft not found"));
    }
    if (groupId.has_value()) {
        // The group row goes only when no draft references it any more.
        Statement statement;
        const auto prepared = prepare(handle,
            "DELETE FROM run_draft_groups WHERE id=? AND NOT EXISTS "
            "(SELECT 1 FROM run_drafts WHERE group_id=?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, *groupId), bindText(handle, statement.get(), 2, *groupId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(handle, "persistence.write_failed", "Could not remove run draft group"));
        }
    }
    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

namespace sqlite_detail {

// The rows of ONE group and its drafts, written inside a transaction the CALLER owns (insertGroup's own, or the atomic
// Confirm of a follow-up request). The drafts must already be validated and sorted by ordinal.
common::Result<void> insertRunDraftGroupRows(
    sqlite3* handle, const application::RunDraftGroup& group, const std::vector<RunDraft>& drafts)
{
    {
        Statement statement;
        const std::string sql = std::string("INSERT INTO run_draft_groups (") + runDraftGroupColumns
            + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);";
        const auto prepared = prepare(handle, sql.c_str(), statement);
        if (!prepared.success()) return prepared;
        const auto bound = bindRunDraftGroup(handle, statement.get(), group);
        if (!bound.success()) return bound;
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(handle);
            if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return common::Result<void>::failure(
                    error("run_draft_group.duplicate_id", "Run draft group id already exists"));
            }
            if (code == SQLITE_CONSTRAINT_UNIQUE) {
                // Two columns are unique: the idempotency key and (for a follow-up group) the request id.
                const std::string_view message(sqlite3_errmsg(handle));
                if (message.find("source_request_id") != std::string_view::npos) {
                    return common::Result<void>::failure(error("run_draft_group.duplicate_source_request",
                        "A group of this follow-up request already exists"));
                }
                return common::Result<void>::failure(error("run_draft_group.duplicate_idempotency_key",
                    "A group with this idempotency key already exists"));
            }
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert run draft group"));
        }
    }

    for (const auto& draft : drafts) {
        {
            Statement statement;
            const std::string sql = std::string("INSERT INTO run_drafts (") + runDraftColumns
                + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            const auto prepared = prepare(handle, sql.c_str(), statement);
            if (!prepared.success()) return prepared;
            const auto bound = bindRunDraft(handle, statement.get(), draft);
            if (!bound.success()) return bound;
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                    return common::Result<void>::failure(error("run_draft.duplicate_id", "Run draft id already exists"));
                }
                return common::Result<void>::failure(
                    databaseError(handle, "persistence.write_failed", "Could not insert run draft"));
            }
        }
        for (const auto& assignment : draft.roster) {
            const auto inserted = insertRunDraftAssignmentRow(handle, draft.id, assignment);
            if (!inserted.success()) return inserted;
        }
    }
    return common::Result<void>::ok();
}

} // namespace sqlite_detail

common::Result<std::vector<RunDraft>> SqliteRunDraftRepository::insertGroup(
    application::RunDraftGroup group, std::vector<RunDraft> drafts)
{
    using Drafts = std::vector<RunDraft>;
    // A follow-up group is born only from the Confirm of its request (IFollowUpPlanConfirmPort), never through this port.
    const auto allowed = application::checkRunDraftGroupPublicInsert(group);
    if (!allowed.success()) return common::Result<Drafts>::failure(*allowed.error());
    // Validated before the first write; a refusal never opens a transaction.
    const auto valid = application::validateRunDraftGroupInsertion(group, drafts);
    if (!valid.success()) return common::Result<Drafts>::failure(*valid.error());
    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        return left.provenance->ordinal < right.provenance->ordinal;
    });

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Drafts>::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Drafts>::failure(failure);
    };

    const auto written = insertRunDraftGroupRows(handle, group, drafts);
    if (!written.success()) return rollbackWith(*written.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Drafts>::ok(std::move(drafts));
}

namespace sqlite_detail {

common::Result<std::optional<application::RunDraftGroup>> findRunDraftGroupWhere(
    sqlite3* handle, const char* whereClause, const std::string& value)
{
    using Result = common::Result<std::optional<application::RunDraftGroup>>;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftGroupColumns + " FROM run_draft_groups WHERE "
        + whereClause + ";";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, value);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read run draft group"));
    }
    auto group = readRunDraftGroup(statement.get());
    if (!group.success()) return Result::failure(*group.error());
    return Result::ok(std::move(group.value()));
}

} // namespace sqlite_detail

common::Result<std::optional<application::RunDraftGroup>> SqliteRunDraftRepository::findGroupById(
    const application::RunDraftGroupId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return findRunDraftGroupWhere(database_->impl_->handle, "id=?", id);
}

common::Result<std::optional<application::RunDraftGroup>> SqliteRunDraftRepository::findGroupByIdempotencyKey(
    const std::string& idempotencyKey) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return findRunDraftGroupWhere(database_->impl_->handle, "idempotency_key=?", idempotencyKey);
}

namespace sqlite_detail {

// The drafts of one group (roster included) in ordinal order; the caller holds the database lock.
common::Result<std::vector<RunDraft>> readRunDraftGroupDrafts(sqlite3* handle, const application::RunDraftGroupId& id)
{
    using Drafts = std::vector<RunDraft>;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftColumns
        + " FROM run_drafts WHERE group_id=? ORDER BY group_ordinal;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<Drafts>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<Drafts>::failure(*bound.error());

    Drafts drafts;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<Drafts>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run draft group"));
        }
        auto draft = readRunDraft(statement.get());
        if (!draft.success()) return common::Result<Drafts>::failure(*draft.error());
        drafts.push_back(std::move(draft.value()));
    }
    for (auto& draft : drafts) {
        const auto roster = fetchAssignmentsForDraft(handle, draft.id);
        if (!roster.success()) return common::Result<Drafts>::failure(*roster.error());
        draft.roster = roster.value();
    }
    return common::Result<Drafts>::ok(std::move(drafts));
}

} // namespace sqlite_detail

common::Result<std::vector<RunDraft>> SqliteRunDraftRepository::listGroupDrafts(
    const application::RunDraftGroupId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readRunDraftGroupDrafts(database_->impl_->handle, id);
}

} // namespace ai::infrastructure
