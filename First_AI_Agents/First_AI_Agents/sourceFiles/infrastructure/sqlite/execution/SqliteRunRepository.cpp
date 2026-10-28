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

std::optional<StageStatus> stageStatusFromString(std::string_view value)
{
    if (value == "pending") return StageStatus::Pending;
    if (value == "running") return StageStatus::Running;
    if (value == "completed") return StageStatus::Completed;
    if (value == "failed") return StageStatus::Failed;
    if (value == "skipped") return StageStatus::Skipped;
    if (value == "cancelled") return StageStatus::Cancelled;
    return std::nullopt;
}

std::string providerBudgetPolicyJson(const ProviderExecutionBudgetPolicy& value)
{
    return nlohmann::json {
        {"schemaVersion", value.schemaVersion},
        {"maxInputTokens", value.maxInputTokens},
        {"maxOutputTokens", value.maxOutputTokens},
        {"maxTurns", value.maxTurns},
        {"maxToolCalls", value.maxToolCalls},
        {"maxToolResultBytes", value.maxToolResultBytes},
        {"maxWallTimeMs", value.maxWallTimeMs},
        {"maxRequestTimeMs", value.maxRequestTimeMs},
        {"autoExtendTurns", value.autoExtendTurns},
    }.dump();
}

common::Result<ProviderExecutionBudgetPolicy> parseProviderBudgetPolicy(
    std::string_view raw)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        if (!document.is_object()) throw std::runtime_error("not an object");
        ProviderExecutionBudgetPolicy value;
        if (!document.empty()) {
            static const std::array<const char*, 8> fields {
                "schemaVersion", "maxInputTokens", "maxOutputTokens", "maxTurns",
                "maxToolCalls", "maxToolResultBytes", "maxWallTimeMs", "maxRequestTimeMs"};
            // autoExtendTurns was added after rows already existed without it, and a frozen
            // policy cannot say whether its limit was typed or defaulted. A row frozen with
            // exactly the Backend default turn limit is read as automatic (so Retry / Edit &
            // rerun of such a Run is not doomed to repeat a shortfall the Backend now covers);
            // any other value was chosen by somebody and stays pinned.
            const bool hasAutoExtend = document.contains("autoExtendTurns");
            if (document.size() != fields.size() + (hasAutoExtend ? 1 : 0)) {
                throw std::runtime_error("field count");
            }
            for (const auto* field : fields) {
                if (!document.contains(field)) throw std::runtime_error("missing field");
            }
            value.autoExtendTurns = hasAutoExtend
                ? document.at("autoExtendTurns").get<bool>()
                : document.at("maxTurns").get<std::uint64_t>()
                    == ProviderExecutionBudgetPolicy {}.maxTurns;
            value.schemaVersion = document.at("schemaVersion").get<std::string>();
            value.maxInputTokens = document.at("maxInputTokens").get<std::uint64_t>();
            value.maxOutputTokens = document.at("maxOutputTokens").get<std::uint64_t>();
            value.maxTurns = document.at("maxTurns").get<std::uint64_t>();
            value.maxToolCalls = document.at("maxToolCalls").get<std::uint64_t>();
            value.maxToolResultBytes = document.at("maxToolResultBytes").get<std::uint64_t>();
            value.maxWallTimeMs = document.at("maxWallTimeMs").get<std::uint64_t>();
            value.maxRequestTimeMs = document.at("maxRequestTimeMs").get<std::uint64_t>();
        } else {
            // '{}' - a Run that carries no budget of its own: the Backend defaults, automatic.
            value.autoExtendTurns = true;
        }
        const auto valid = validateProviderExecutionBudgetPolicy(value);
        if (!valid.success()) {
            return common::Result<ProviderExecutionBudgetPolicy>::failure(*valid.error());
        }
        return common::Result<ProviderExecutionBudgetPolicy>::ok(std::move(value));
    } catch (...) {
        return common::Result<ProviderExecutionBudgetPolicy>::failure(error(
            "persistence.invalid_data", "Run row contains a corrupt provider budget policy"));
    }
}

std::string providerBudgetUsageJson(const ProviderExecutionBudgetUsage& value)
{
    nlohmann::json document {
        {"turnsStarted", value.turnsStarted},
        {"turnsCompleted", value.turnsCompleted},
        {"inputTokens", value.inputTokens},
        {"outputTokens", value.outputTokens},
        {"toolCalls", value.toolCalls},
        {"toolResultBytes", value.toolResultBytes},
        {"usageAuthoritative", value.usageAuthoritative},
        {"stopCode", value.stopCode},
        {"cacheReadInputTokens", value.cacheReadInputTokens},
        {"cacheCreationInputTokens", value.cacheCreationInputTokens},
        {"cacheCreationDurationPartitionKnown", value.cacheCreationDuration.known},
        {"cacheCreationFiveMinuteInputTokens", value.cacheCreationDuration.fiveMinuteTokens},
        {"cacheCreationOneHourInputTokens", value.cacheCreationDuration.oneHourTokens},
    };
    // Written only once the Backend actually raised the turn limit, so a Run
    // that never needed it keeps its exact historical shape.
    if (value.grantedMaxTurns > 0) document["grantedMaxTurns"] = value.grantedMaxTurns;
    return document.dump();
}

common::Result<void> validateProviderBudgetUsageForPersistence(
    const ProviderExecutionBudgetUsage& value)
{
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens) {
        return common::Result<void>::failure(error(
            "persistence.invalid_data", "Run contains invalid provider budget usage"));
    }
    return common::Result<void>::ok();
}

common::Result<ProviderExecutionBudgetUsage> parseProviderBudgetUsage(
    std::string_view raw)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        if (!document.is_object()) throw std::runtime_error("not an object");
        ProviderExecutionBudgetUsage value;
        if (document.empty()) return common::Result<ProviderExecutionBudgetUsage>::ok(value);
        static const std::array<const char*, 8> requiredFields {
            "turnsStarted", "turnsCompleted", "inputTokens", "outputTokens",
            "toolCalls", "toolResultBytes", "usageAuthoritative", "stopCode"};
        // cacheReadInputTokens and cacheCreationInputTokens (498dd8c6/
        // af4e5703 cache-budget fixes, 2026-09-15) were each added after rows
        // already existed without them; accept any of the three shapes (0,
        // 1 or 2 of these present) rather than rejecting an older Run's
        // persisted usage as corrupt.
        static const std::array<const char*, 6> optionalFields {
            "cacheReadInputTokens", "cacheCreationInputTokens", "grantedMaxTurns",
            "cacheCreationDurationPartitionKnown", "cacheCreationFiveMinuteInputTokens",
            "cacheCreationOneHourInputTokens"};
        for (const auto* field : requiredFields) {
            if (!document.contains(field)) throw std::runtime_error("missing field");
        }
        std::size_t knownFieldCount = requiredFields.size();
        for (const auto* field : optionalFields) {
            if (document.contains(field)) ++knownFieldCount;
        }
        if (document.size() != knownFieldCount) throw std::runtime_error("field count");
        value.turnsStarted = document.at("turnsStarted").get<std::uint64_t>();
        value.turnsCompleted = document.at("turnsCompleted").get<std::uint64_t>();
        value.inputTokens = document.at("inputTokens").get<std::uint64_t>();
        value.outputTokens = document.at("outputTokens").get<std::uint64_t>();
        value.toolCalls = document.at("toolCalls").get<std::uint64_t>();
        value.toolResultBytes = document.at("toolResultBytes").get<std::uint64_t>();
        value.usageAuthoritative = document.at("usageAuthoritative").get<bool>();
        value.stopCode = document.at("stopCode").get<std::string>();
        value.cacheReadInputTokens = document.contains("cacheReadInputTokens")
            ? document.at("cacheReadInputTokens").get<std::uint64_t>()
            : static_cast<std::uint64_t>(0);
        value.cacheCreationInputTokens = document.contains("cacheCreationInputTokens")
            ? document.at("cacheCreationInputTokens").get<std::uint64_t>()
            : static_cast<std::uint64_t>(0);
        value.grantedMaxTurns = document.contains("grantedMaxTurns")
            ? document.at("grantedMaxTurns").get<std::uint64_t>()
            : static_cast<std::uint64_t>(0);
        const bool hasDurationKnown = document.contains("cacheCreationDurationPartitionKnown");
        const bool hasFiveMinute = document.contains("cacheCreationFiveMinuteInputTokens");
        const bool hasOneHour = document.contains("cacheCreationOneHourInputTokens");
        if (hasDurationKnown != hasFiveMinute || hasDurationKnown != hasOneHour) {
            throw std::runtime_error("partial cache duration usage");
        }
        value.cacheCreationDuration.known = hasDurationKnown
            ? document.at("cacheCreationDurationPartitionKnown").get<bool>() : false;
        value.cacheCreationDuration.fiveMinuteTokens = hasFiveMinute
            ? document.at("cacheCreationFiveMinuteInputTokens").get<std::uint64_t>() : 0;
        value.cacheCreationDuration.oneHourTokens = hasOneHour
            ? document.at("cacheCreationOneHourInputTokens").get<std::uint64_t>() : 0;
        if (value.turnsCompleted > value.turnsStarted) throw std::runtime_error("turn count");
        if (checkCacheCreationDurationUsage(
                value.cacheCreationInputTokens, value.cacheCreationDuration)
                != CacheCreationDurationUsageProblem::None
            || value.cacheCreationInputTokens > value.inputTokens) {
            throw std::runtime_error("cache duration usage");
        }
        return common::Result<ProviderExecutionBudgetUsage>::ok(std::move(value));
    } catch (...) {
        return common::Result<ProviderExecutionBudgetUsage>::failure(error(
            "persistence.invalid_data", "Run row contains corrupt provider budget usage"));
    }
}

common::Result<Run> readRun(sqlite3_stmt* statement)
{
    const auto status = runStatusFromString(columnText(statement, 6));
    if (!status.has_value()) {
        return common::Result<Run>::failure(
            error("persistence.invalid_data", "Run row contains an unknown status"));
    }
    const auto planningCompletionPolicy =
        application::planningCompletionPolicyFromString(columnText(statement, 19));
    if (!planningCompletionPolicy.has_value()) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data",
            "Run row contains an unknown planning completion policy"));
    }
    const auto verificationRepairPolicy =
        application::verificationRepairPolicyFromString(columnText(statement, 24));
    if (!verificationRepairPolicy.has_value()) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data",
            "Run row contains an unknown verification repair policy"));
    }

    const std::int64_t inputTokens = sqlite3_column_int64(statement, 10);
    const std::int64_t outputTokens = sqlite3_column_int64(statement, 11);
    const std::int64_t revision = sqlite3_column_int64(statement, 15);
    const std::int64_t maxParallelism = sqlite3_column_int64(statement, 17);
    const std::int64_t cacheCreationInputTokens = sqlite3_column_int64(statement, 22);
    const std::int64_t cacheReadInputTokens = sqlite3_column_int64(statement, 23);
    const std::int64_t cacheDurationKnown = sqlite3_column_int64(statement, 26);
    const std::int64_t cacheFiveMinuteTokens = sqlite3_column_int64(statement, 27);
    const std::int64_t cacheOneHourTokens = sqlite3_column_int64(statement, 28);
    if (inputTokens < 0 || outputTokens < 0 || revision < 0
        || maxParallelism <= 0 || maxParallelism > 32
        || cacheCreationInputTokens < 0 || cacheReadInputTokens < 0
        || (cacheDurationKnown != 0 && cacheDurationKnown != 1)
        || cacheFiveMinuteTokens < 0 || cacheOneHourTokens < 0) {
        return common::Result<Run>::failure(
            error("persistence.invalid_data", "Run row contains a negative unsigned value"));
    }

    Run run;
    run.id = columnText(statement, 0);
    run.projectId = columnText(statement, 1);
    run.parentRunId = optionalText(statement, 2);
    run.title = columnText(statement, 3);
    run.task = columnText(statement, 4);
    run.workflowId = columnText(statement, 5);
    run.status = *status;
    run.progress = sqlite3_column_double(statement, 7);
    run.currentStageId = optionalText(statement, 8);
    run.errorMessage = columnText(statement, 9);
    run.usage.inputTokens = static_cast<std::uint64_t>(inputTokens);
    run.usage.outputTokens = static_cast<std::uint64_t>(outputTokens);
    run.usage.cacheCreationInputTokens = static_cast<std::uint64_t>(cacheCreationInputTokens);
    run.usage.cacheReadInputTokens = static_cast<std::uint64_t>(cacheReadInputTokens);
    run.usage.cacheCreationDuration.known = cacheDurationKnown != 0;
    run.usage.cacheCreationDuration.fiveMinuteTokens =
        static_cast<std::uint64_t>(cacheFiveMinuteTokens);
    run.usage.cacheCreationDuration.oneHourTokens =
        static_cast<std::uint64_t>(cacheOneHourTokens);
    if (!run.usage.cacheBreakdownIsConsistent()
        || checkCacheCreationDurationUsage(
            run.usage.cacheCreationInputTokens,
            run.usage.cacheCreationDuration)
        != CacheCreationDurationUsageProblem::None) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data", "Run row contains inconsistent cache usage"));
    }
    run.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    run.startedAt = optionalTime(statement, 13);
    run.finishedAt = optionalTime(statement, 14);
    run.revision = static_cast<std::uint64_t>(revision);
    // assignmentSnapshot is intentionally not read here: it lives in the
    // separate run_stage_assignments table and is attached by the caller
    // (see fetchAssignmentsForRun) so this single-row parse never needs a
    // second query of its own.
    run.executionProfileId = optionalText(statement, 16);
    run.maxParallelism = static_cast<std::uint32_t>(maxParallelism);
    const auto workspaceText = optionalText(statement, 18);
    run.workspacePath = workspaceText.has_value()
        ? std::optional<std::filesystem::path>(pathFromUtf8(*workspaceText))
        : std::nullopt;
    run.planningCompletionPolicy = *planningCompletionPolicy;
    const auto providerBudget = parseProviderBudgetPolicy(columnText(statement, 20));
    if (!providerBudget.success()) return common::Result<Run>::failure(*providerBudget.error());
    const auto providerBudgetUsage = parseProviderBudgetUsage(columnText(statement, 21));
    if (!providerBudgetUsage.success())
        return common::Result<Run>::failure(*providerBudgetUsage.error());
    run.providerBudget = providerBudget.value();
    run.providerBudgetUsage = providerBudgetUsage.value();
    run.verificationRepairPolicy = *verificationRepairPolicy;
    // A damaged or non-canonical contracts column is reported, never repaired or dropped (PC-2).
    const auto interfaceContracts =
        application::parseRunInterfaceContracts(columnText(statement, 25));
    if (!interfaceContracts.success())
        return common::Result<Run>::failure(*interfaceContracts.error());
    run.interfaceContracts = interfaceContracts.value();
    return common::Result<Run>::ok(std::move(run));
}

common::Result<RunStage> readStage(sqlite3_stmt* statement)
{
    const auto status = stageStatusFromString(columnText(statement, 6));
    if (!status.has_value()) {
        return common::Result<RunStage>::failure(
            error("persistence.invalid_data", "Stage row contains an unknown status"));
    }

    const std::int64_t inputTokens = sqlite3_column_int64(statement, 10);
    const std::int64_t outputTokens = sqlite3_column_int64(statement, 11);
    if (inputTokens < 0 || outputTokens < 0) {
        return common::Result<RunStage>::failure(
            error("persistence.invalid_data", "Stage row contains a negative token count"));
    }

    RunStage stage;
    stage.id = columnText(statement, 0);
    stage.runId = columnText(statement, 1);
    stage.type = columnText(statement, 2);
    stage.title = columnText(statement, 3);
    stage.description = columnText(statement, 4);
    stage.status = *status;
    stage.progress = sqlite3_column_double(statement, 7);
    stage.providerId = columnText(statement, 8);
    stage.modelId = columnText(statement, 9);
    stage.usage.inputTokens = static_cast<std::uint64_t>(inputTokens);
    stage.usage.outputTokens = static_cast<std::uint64_t>(outputTokens);
    stage.errorMessage = columnText(statement, 12);
    stage.startedAt = optionalTime(statement, 13);
    stage.finishedAt = optionalTime(statement, 14);
    return common::Result<RunStage>::ok(std::move(stage));
}

common::Result<void> bindRunFields(sqlite3* handle, sqlite3_stmt* statement, const Run& run)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, run.id),
        bindText(handle, statement, 2, run.projectId),
        bindNullableText(handle, statement, 3, run.parentRunId),
        bindText(handle, statement, 4, run.title),
        bindText(handle, statement, 5, run.task),
        bindText(handle, statement, 6, run.workflowId),
        bindText(handle, statement, 7, application::toString(run.status)),
        bindInt64(handle, statement, 8, 0),
        bindNullableText(handle, statement, 9, run.currentStageId),
        bindText(handle, statement, 10, run.errorMessage),
        bindUnsigned(handle, statement, 11, run.usage.inputTokens),
        bindUnsigned(handle, statement, 12, run.usage.outputTokens),
        bindInt64(handle, statement, 13, toMilliseconds(run.createdAt)),
        bindOptionalTime(handle, statement, 14, run.startedAt),
        bindOptionalTime(handle, statement, 15, run.finishedAt),
        bindUnsigned(handle, statement, 16, run.revision),
        bindNullableText(handle, statement, 17, run.executionProfileId),
        bindUnsigned(handle, statement, 18, run.maxParallelism),
        bindNullableText(handle, statement, 19,
            run.workspacePath.has_value()
                ? std::optional<std::string>(pathToUtf8(*run.workspacePath))
                : std::nullopt),
        bindText(handle, statement, 20,
            application::toString(run.planningCompletionPolicy)),
        bindText(handle, statement, 21, providerBudgetPolicyJson(run.providerBudget)),
        bindText(handle, statement, 22, providerBudgetUsageJson(run.providerBudgetUsage)),
        bindUnsigned(handle, statement, 23, run.usage.cacheCreationInputTokens),
        bindUnsigned(handle, statement, 24, run.usage.cacheReadInputTokens),
        bindText(handle, statement, 25,
            application::toString(run.verificationRepairPolicy)),
        bindText(handle, statement, 26,
            application::serializeRunInterfaceContracts(run.interfaceContracts)),
        bindInt64(handle, statement, 27, run.usage.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement, 28,
            run.usage.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement, 29,
            run.usage.cacheCreationDuration.oneHourTokens)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement, 8, run.progress) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind run progress"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindStageFields(
    sqlite3* handle,
    sqlite3_stmt* statement,
    const RunStage& stage,
    std::optional<std::int64_t> position)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, stage.id),
        bindText(handle, statement, 2, stage.runId),
        bindText(handle, statement, 3, stage.type),
        bindText(handle, statement, 4, stage.title),
        bindText(handle, statement, 5, stage.description),
        bindText(handle, statement, 6, application::toString(stage.status)),
        bindInt64(handle, statement, 7, 0),
        bindText(handle, statement, 8, stage.providerId),
        bindText(handle, statement, 9, stage.modelId),
        bindUnsigned(handle, statement, 10, stage.usage.inputTokens),
        bindUnsigned(handle, statement, 11, stage.usage.outputTokens),
        bindText(handle, statement, 12, stage.errorMessage),
        bindOptionalTime(handle, statement, 13, stage.startedAt),
        bindOptionalTime(handle, statement, 14, stage.finishedAt)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement, 7, stage.progress) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind stage progress"));
    }
    if (position.has_value()) {
        return bindInt64(handle, statement, 15, *position);
    }
    return common::Result<void>::ok();
}

common::Result<void> validateProgress(double progress, std::string_view aggregate)
{
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
        return common::Result<void>::failure(error(
            "persistence.invalid_progress",
            std::string(aggregate) + " progress must be finite and in the [0, 1] range"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateRunCacheDuration(const Run& run)
{
    if (!run.usage.cacheBreakdownIsConsistent()) {
        return common::Result<void>::failure(error(
            "persistence.invalid_data",
            "Run cache-creation and cache-read tokens must fit within input tokens"));
    }
    const auto problem = checkCacheCreationDurationUsage(
        run.usage.cacheCreationInputTokens, run.usage.cacheCreationDuration);
    if (problem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(problem),
            cacheCreationDurationUsageProblemMessage(problem)));
    }
    return common::Result<void>::ok();
}

common::Result<void> insertRunRow(sqlite3* handle, const Run& run)
{
    // Shared by the legacy fixture/recovery insert and the aggregate
    // transaction so their bindings and typed duplicate behavior cannot drift.
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) return validCacheDuration;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO runs (id,project_id,parent_run_id,title,task,workflow_id,status,progress,"
        "current_stage_id,error_message,input_tokens,output_tokens,created_at_ms,started_at_ms,"
        "finished_at_ms,revision,execution_profile_id,max_parallelism,workspace_path,"
        "planning_completion_policy,provider_budget_json,provider_budget_usage_json,"
        "cache_creation_input_tokens,cache_read_input_tokens,verification_repair_policy,"
        "run_interface_contracts_json,cache_creation_duration_partition_known,"
        "cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const auto bound = bindRunFields(handle, statement.get(), run);
    if (!bound.success()) return bound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error("run.duplicate_id", "Run id already exists"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run"));
    }
    return common::Result<void>::ok();
}

common::Result<void> insertStageRow(
    sqlite3* handle,
    const RunStage& stage,
    std::size_t position)
{
    // Position belongs to persistence metadata rather than the domain DTO;
    // writing it here preserves the exact workflow-declared order on reload.
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_stages (id,run_id,type,title,description,status,progress,provider_id,model_id,"
        "input_tokens,output_tokens,error_message,started_at_ms,finished_at_ms,position) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const auto bound = bindStageFields(
        handle, statement.get(), stage, static_cast<std::int64_t>(position));
    if (!bound.success()) return bound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run stage"));
    }
    return common::Result<void>::ok();
}

// Writes one immutable snapshot row. Called only from insertRunWithStages,
// inside the same transaction as the run/stage rows, so a Run row can never
// become visible with a partially-written snapshot.
common::Result<void> insertRunStageAssignmentRow(
    sqlite3* handle,
    const application::RunId& runId,
    const StageModelAssignment& assignment)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_stage_assignments (run_id,stage_type,provider_id,model_id,timeout_ms,"
        "agent_name,role,instructions,position,lead_architect,dependencies_json,max_attempts,"
        "fallback_provider_id,fallback_model_id,workspace_access) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
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
                "execution_profile.duplicate_stage_type",
                "A stage type cannot appear more than once in one run's assignment snapshot"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run stage assignment"));
    }
    return common::Result<void>::ok();
}

constexpr const char* runColumns =
    "id, project_id, parent_run_id, title, task, workflow_id, status, progress, "
    "current_stage_id, error_message, input_tokens, output_tokens, created_at_ms, "
    "started_at_ms, finished_at_ms, revision, execution_profile_id, max_parallelism, "
    "workspace_path, planning_completion_policy, provider_budget_json, provider_budget_usage_json, "
    "cache_creation_input_tokens, cache_read_input_tokens, verification_repair_policy, "
    "run_interface_contracts_json, cache_creation_duration_partition_known, "
    "cache_creation_five_minute_input_tokens, cache_creation_one_hour_input_tokens";

constexpr const char* runStageAssignmentColumns =
    "run_id, stage_type, provider_id, model_id, timeout_ms, agent_name, role, instructions, "
    "position, lead_architect, dependencies_json, max_attempts, fallback_provider_id, fallback_model_id, "
    "workspace_access";

constexpr const char* stageColumns =
    "id, run_id, type, title, description, position, status, progress, provider_id, "
    "model_id, input_tokens, output_tokens, error_message, started_at_ms, finished_at_ms";

// Shared by findRun/listRuns/listUnfinishedRuns so every Run read path
// attaches the same immutable roster snapshot in execution order.
common::Result<std::vector<StageModelAssignment>> fetchAssignmentsForRun(
    sqlite3* handle,
    const std::string& runId)
{
    Statement statement;
    const std::string sql = std::string("SELECT ") + runStageAssignmentColumns
        + " FROM run_stage_assignments WHERE run_id=? ORDER BY position,stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*bound.error());
    }

    std::vector<StageModelAssignment> assignments;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<StageModelAssignment>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read run stage assignments"));
        }
        auto assignment = readRunAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::vector<StageModelAssignment>>::failure(*assignment.error());
        }
        assignments.push_back(assignment.value());
    }
    return common::Result<std::vector<StageModelAssignment>>::ok(std::move(assignments));
}

// Batched counterpart of fetchAssignmentsForRun for listRuns/listUnfinishedRuns:
// those callers previously issued one fetchAssignmentsForRun query PER run in
// the result set (N+1), which the UI feels directly - RunController::handleEvent
// re-runs a full listRuns() on every single RunEvent (stage started, each
// tool-call progress tick, terminal), on the GUI thread, while holding the
// shared SQLite mutex for the whole call. With run history now in the
// dozens and growing, and a workspaceAccess agent's turn loop emitting one
// progress event per turn, this N+1 was a real, measurable source of the
// reported UI freeze during an active run. One IN(...) query replaces the
// N separate ones; an empty runIds list short-circuits (SQL "IN ()" is
// invalid) and returns an empty map without touching the database.
common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>
fetchAssignmentsForRuns(sqlite3* handle, const std::vector<std::string>& runIds)
{
    std::unordered_map<std::string, std::vector<StageModelAssignment>> byRunId;
    if (runIds.empty()) {
        return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::ok(
            std::move(byRunId));
    }

    std::string placeholders(runIds.size() * 2 - 1, ',');
    for (std::size_t i = 0; i < runIds.size(); ++i) placeholders[i * 2] = '?';

    Statement statement;
    const std::string sql = std::string("SELECT ") + runStageAssignmentColumns
        + " FROM run_stage_assignments WHERE run_id IN (" + placeholders + ") "
          "ORDER BY run_id,position,stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
            *prepared.error());
    }
    for (std::size_t i = 0; i < runIds.size(); ++i) {
        const auto bound = bindText(handle, statement.get(), static_cast<int>(i) + 1, runIds[i]);
        if (!bound.success()) {
            return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
                *bound.error());
        }
    }

    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read run stage assignments"));
        }
        const std::string runId = columnText(statement.get(), 0);
        auto assignment = readRunAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
                *assignment.error());
        }
        byRunId[runId].push_back(std::move(assignment.value()));
    }
    return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::ok(
        std::move(byRunId));
}

} // namespace

SqliteRunRepository::SqliteRunRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Run> SqliteRunRepository::insertRun(Run run)
{
    const auto validProviderUsage = validateProviderBudgetUsageForPersistence(
        run.providerBudgetUsage);
    if (!validProviderUsage.success()) {
        return common::Result<Run>::failure(*validProviderUsage.error());
    }
    const auto validProgress = validateProgress(run.progress, "Run");
    if (!validProgress.success()) return common::Result<Run>::failure(*validProgress.error());
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto inserted = insertRunRow(handle, run);
    if (!inserted.success()) return common::Result<Run>::failure(*inserted.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> SqliteRunRepository::insertRunWithStages(
    Run run,
    std::vector<RunStage> stages)
{
    return insertRunWithStagesAndCopiedTaskLinks(
        std::move(run), std::move(stages), {});
}

common::Result<Run> SqliteRunRepository::insertRunWithStagesAndCopiedTaskLinks(
    Run run,
    std::vector<RunStage> stages,
    const application::RunId& taskLinkSourceRunId)
{
    // Validate all provider-neutral invariants before opening the transaction;
    // database constraints remain the final guard for duplicate identifiers.
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) return common::Result<Run>::failure(*validRunProgress.error());
    for (const auto& stage : stages) {
        if (stage.runId != run.id) {
            return common::Result<Run>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validStageProgress = validateProgress(stage.progress, "Stage");
        if (!validStageProgress.success()) {
            return common::Result<Run>::failure(*validStageProgress.error());
        }
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Run>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        // Rollback is best-effort here; the original failure is the actionable
        // error the Application Layer must receive.
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Run>::failure(failure);
    };

    const auto insertedRun = insertRunRow(handle, run);
    if (!insertedRun.success()) return rollbackWith(*insertedRun.error());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto insertedStage = insertStageRow(handle, stages[index], index);
        if (!insertedStage.success()) return rollbackWith(*insertedStage.error());
    }
    // The immutable execution snapshot commits atomically with the run and
    // its stages: a Run row can never be observed with a partial or missing
    // snapshot. Empty for the Mock workflow (run.assignmentSnapshot is empty).
    for (const auto& assignment : run.assignmentSnapshot) {
        const auto insertedAssignment = insertRunStageAssignmentRow(handle, run.id, assignment);
        if (!insertedAssignment.success()) return rollbackWith(*insertedAssignment.error());
    }
    if (!taskLinkSourceRunId.empty()) {
        // Copy only links whose Task belongs to the new Run's Project. This
        // keeps Edit & rerun across Projects from importing foreign planning
        // state, while Retry in the same Project preserves every link.
        Statement copyLinks;
        const auto prepared = prepare(handle,
            "INSERT OR IGNORE INTO task_run_links(task_id,run_id) "
            "SELECT links.task_id,? FROM task_run_links AS links "
            "JOIN project_tasks AS tasks ON tasks.id=links.task_id "
            "WHERE links.run_id=? AND tasks.project_id=?;",
            copyLinks);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, copyLinks.get(), 1, run.id),
            bindText(handle, copyLinks.get(), 2, taskLinkSourceRunId),
            bindText(handle, copyLinks.get(), 3, run.projectId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(copyLinks.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed",
                "Could not copy task links for rerun"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> SqliteRunRepository::insertRunWithStagesAndTaskLink(
    Run run,
    std::vector<RunStage> stages,
    const application::TaskId& taskId)
{
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) return common::Result<Run>::failure(*validRunProgress.error());
    for (const auto& stage : stages) {
        if (stage.runId != run.id) {
            return common::Result<Run>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validStageProgress = validateProgress(stage.progress, "Stage");
        if (!validStageProgress.success()) {
            return common::Result<Run>::failure(*validStageProgress.error());
        }
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Run>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Run>::failure(failure);
    };

    const auto insertedRun = insertRunRow(handle, run);
    if (!insertedRun.success()) return rollbackWith(*insertedRun.error());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto insertedStage = insertStageRow(handle, stages[index], index);
        if (!insertedStage.success()) return rollbackWith(*insertedStage.error());
    }
    for (const auto& assignment : run.assignmentSnapshot) {
        const auto insertedAssignment = insertRunStageAssignmentRow(handle, run.id, assignment);
        if (!insertedAssignment.success()) return rollbackWith(*insertedAssignment.error());
    }

    if (!taskId.empty()) {
        // Same statement SqliteTaskRepository::linkRun() runs standalone -
        // here it commits in the same transaction as the Run/stage insert so
        // a caller (RunManager) can never observe a persisted new Run
        // without its requested ProjectTask link, or vice versa.
        Statement link;
        const auto prepared = prepare(handle,
            "INSERT OR REPLACE INTO task_run_links(task_id,run_id) VALUES (?,?);", link);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, link.get(), 1, taskId),
            bindText(handle, link.get(), 2, run.id)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(link.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not link task to new run"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> SqliteRunRepository::insertRunWithStagesAndLegacyConversion(
    Run run,
    std::vector<RunStage> stages,
    application::LegacyConversionRecord record,
    const std::optional<application::RunId>& taskLinkSourceRunId)
{
    // Every validation that does not need the database runs BEFORE the transaction opens, so a
    // refused record can never leave a Run (or half of one) behind.
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) return common::Result<Run>::failure(*validRunProgress.error());
    for (const auto& stage : stages) {
        if (stage.runId != run.id) {
            return common::Result<Run>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validStageProgress = validateProgress(stage.progress, "Stage");
        if (!validStageProgress.success()) {
            return common::Result<Run>::failure(*validStageProgress.error());
        }
    }
    if (const auto problem = application::legacyConversionAggregateProblem(run, record)) {
        return common::Result<Run>::failure(
            error(application::kLegacyConversionRecordInvalidCode, *problem));
    }
    const auto droppedJson = application::legacyConversionDroppedToJson(record.dropped);
    if (!droppedJson.success()) return common::Result<Run>::failure(*droppedJson.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Run>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Run>::failure(failure);
    };

    const auto insertedRun = insertRunRow(handle, run);
    if (!insertedRun.success()) return rollbackWith(*insertedRun.error());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto insertedStage = insertStageRow(handle, stages[index], index);
        if (!insertedStage.success()) return rollbackWith(*insertedStage.error());
    }
    for (const auto& assignment : run.assignmentSnapshot) {
        const auto insertedAssignment = insertRunStageAssignmentRow(handle, run.id, assignment);
        if (!insertedAssignment.success()) return rollbackWith(*insertedAssignment.error());
    }
    if (taskLinkSourceRunId.has_value() && !taskLinkSourceRunId->empty()) {
        // Same statement the other rerun aggregate uses: only links whose Task belongs to the
        // new Run's Project are copied.
        Statement copyLinks;
        const auto prepared = prepare(handle,
            "INSERT OR IGNORE INTO task_run_links(task_id,run_id) "
            "SELECT links.task_id,? FROM task_run_links AS links "
            "JOIN project_tasks AS tasks ON tasks.id=links.task_id "
            "WHERE links.run_id=? AND tasks.project_id=?;",
            copyLinks);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, copyLinks.get(), 1, run.id),
            bindText(handle, copyLinks.get(), 2, *taskLinkSourceRunId),
            bindText(handle, copyLinks.get(), 3, run.projectId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(copyLinks.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not copy task links for the conversion"));
        }
    }

    // The audit row commits (or rolls back) with everything above.
    Statement insertRecord;
    const auto preparedRecord = prepare(handle,
        "INSERT INTO run_legacy_conversions(run_id,source_run_id,source_workflow_id,policy_id,"
        "reason_code,source_snapshot_fingerprint,target_roster_fingerprint,"
        "confirmation_fingerprint,dropped_json,confirmation_mode,confirmed_at_ms,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?);",
        insertRecord);
    if (!preparedRecord.success()) return rollbackWith(*preparedRecord.error());
    const common::Result<void> recordBindings[] = {
        bindText(handle, insertRecord.get(), 1, record.runId),
        bindText(handle, insertRecord.get(), 2, record.sourceRunId),
        bindText(handle, insertRecord.get(), 3, record.sourceWorkflowId),
        bindText(handle, insertRecord.get(), 4, record.policyId),
        bindText(handle, insertRecord.get(), 5, record.reasonCode),
        bindText(handle, insertRecord.get(), 6, record.sourceSnapshotFingerprint),
        bindText(handle, insertRecord.get(), 7, record.targetRosterFingerprint),
        bindText(handle, insertRecord.get(), 8, record.confirmationFingerprint),
        bindText(handle, insertRecord.get(), 9, droppedJson.value()),
        bindText(handle, insertRecord.get(), 10,
            std::string(application::toString(record.confirmationMode))),
        bindOptionalTime(handle, insertRecord.get(), 11, record.confirmedAt),
        bindInt64(handle, insertRecord.get(), 12, toMilliseconds(record.createdAt))};
    for (const auto& binding : recordBindings) {
        if (!binding.success()) return rollbackWith(*binding.error());
    }
    if (sqlite3_step(insertRecord.get()) != SQLITE_DONE) {
        return rollbackWith(databaseError(
            handle, "persistence.write_failed", "Could not store the legacy conversion record"));
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<std::optional<application::LegacyConversionRecord>>
SqliteRunRepository::findLegacyConversion(const application::RunId& runId) const
{
    using Result = common::Result<std::optional<application::LegacyConversionRecord>>;
    // The Run is read first: findRun takes the database lock itself.
    const auto run = findRun(runId);
    if (!run.success()) return Result::failure(*run.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT run_id,source_run_id,source_workflow_id,policy_id,reason_code,"
        "source_snapshot_fingerprint,target_roster_fingerprint,confirmation_fingerprint,"
        "dropped_json,confirmation_mode,confirmed_at_ms,created_at_ms "
        "FROM run_legacy_conversions WHERE run_id=?;",
        statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return Result::failure(databaseError(
            handle, "persistence.read_failed", "Could not read the legacy conversion record"));
    }

    // From here on any defect is corruption: the record is never returned half trusted.
    const auto corrupt = [](std::string message) {
        return Result::failure(error(application::kLegacyConversionRecordCorruptCode, std::move(message)));
    };
    application::LegacyConversionRecord record;
    record.runId = columnText(statement.get(), 0);
    record.sourceRunId = columnText(statement.get(), 1);
    record.sourceWorkflowId = columnText(statement.get(), 2);
    record.policyId = columnText(statement.get(), 3);
    record.reasonCode = columnText(statement.get(), 4);
    record.sourceSnapshotFingerprint = columnText(statement.get(), 5);
    record.targetRosterFingerprint = columnText(statement.get(), 6);
    record.confirmationFingerprint = columnText(statement.get(), 7);
    const auto dropped = application::legacyConversionDroppedFromJson(columnText(statement.get(), 8));
    if (!dropped.success()) return Result::failure(*dropped.error());
    record.dropped = dropped.value();
    const auto mode = application::legacyConversionConfirmationModeFromString(
        columnText(statement.get(), 9));
    if (!mode.has_value()) return corrupt("The stored confirmation mode is not recognised");
    record.confirmationMode = *mode;
    record.confirmedAt = optionalTime(statement.get(), 10);
    record.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 11));

    if (!run.value().has_value()) return corrupt("The stored conversion record has no Run");
    if (const auto problem = application::legacyConversionAggregateProblem(*run.value(), record)) {
        return corrupt(*problem);
    }
    return Result::ok(std::move(record));
}

common::Result<std::vector<application::LegacyConversionIndexEntry>>
SqliteRunRepository::listLegacyConversionIndex() const
{
    using Result = common::Result<std::vector<application::LegacyConversionIndexEntry>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Only the key and the mode: the index is a marker, the full record is read (and validated)
    // by findLegacyConversion for the one Run the user opens.
    const auto prepared = prepare(handle,
        "SELECT run_id,confirmation_mode FROM run_legacy_conversions ORDER BY run_id;", statement);
    if (!prepared.success()) return Result::failure(*prepared.error());

    std::vector<application::LegacyConversionIndexEntry> index;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed",
                "Could not read the legacy conversion index"));
        }
        index.push_back({columnText(statement.get(), 0),
            application::legacyConversionConfirmationModeFromString(columnText(statement.get(), 1))});
    }
    return Result::ok(std::move(index));
}

common::Result<Run> SqliteRunRepository::updateRun(Run run)
{
    const auto validProviderUsage = validateProviderBudgetUsageForPersistence(
        run.providerBudgetUsage);
    if (!validProviderUsage.success()) {
        return common::Result<Run>::failure(*validProviderUsage.error());
    }
    const auto validProgress = validateProgress(run.progress, "Run");
    if (!validProgress.success()) return common::Result<Run>::failure(*validProgress.error());
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) {
        return common::Result<Run>::failure(*validCacheDuration.error());
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE runs SET project_id=?,parent_run_id=?,title=?,task=?,workflow_id=?,status=?,progress=?,"
        "current_stage_id=?,error_message=?,input_tokens=?,output_tokens=?,cache_creation_input_tokens=?,"
        "cache_read_input_tokens=?,cache_creation_duration_partition_known=?,"
        "cache_creation_five_minute_input_tokens=?,cache_creation_one_hour_input_tokens=?,"
        "created_at_ms=?,started_at_ms=?,finished_at_ms=?,max_parallelism=?,"
        "provider_budget_usage_json=?,revision=revision+1 "
        "WHERE id=? AND revision=?;",
        statement);
    if (!prepared.success()) return common::Result<Run>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, run.projectId),
        bindNullableText(handle, statement.get(), 2, run.parentRunId),
        bindText(handle, statement.get(), 3, run.title),
        bindText(handle, statement.get(), 4, run.task),
        bindText(handle, statement.get(), 5, run.workflowId),
        bindText(handle, statement.get(), 6, application::toString(run.status)),
        bindNullableText(handle, statement.get(), 8, run.currentStageId),
        bindText(handle, statement.get(), 9, run.errorMessage),
        bindUnsigned(handle, statement.get(), 10, run.usage.inputTokens),
        bindUnsigned(handle, statement.get(), 11, run.usage.outputTokens),
        bindUnsigned(handle, statement.get(), 12, run.usage.cacheCreationInputTokens),
        bindUnsigned(handle, statement.get(), 13, run.usage.cacheReadInputTokens),
        bindInt64(handle, statement.get(), 14,
            run.usage.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement.get(), 15,
            run.usage.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement.get(), 16,
            run.usage.cacheCreationDuration.oneHourTokens),
        bindInt64(handle, statement.get(), 17, toMilliseconds(run.createdAt)),
        bindOptionalTime(handle, statement.get(), 18, run.startedAt),
        bindOptionalTime(handle, statement.get(), 19, run.finishedAt),
        bindUnsigned(handle, statement.get(), 20, run.maxParallelism),
        bindText(handle, statement.get(), 21, providerBudgetUsageJson(run.providerBudgetUsage)),
        bindText(handle, statement.get(), 22, run.id),
        bindUnsigned(handle, statement.get(), 23, run.revision)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Run>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement.get(), 7, run.progress) != SQLITE_OK) {
        return common::Result<Run>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind run progress"));
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<Run>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update run"));
    }
    if (sqlite3_changes(handle) == 0) {
        // A zero-row UPDATE is ambiguous between "id does not exist" and "id
        // exists but the caller's revision is stale". Distinguish them with a
        // follow-up read so RunManager/OrchestrationService callers get the
        // correct typed error instead of a generic not-found.
        Statement exists;
        const auto existsPrepared = prepare(handle, "SELECT 1 FROM runs WHERE id=?;", exists);
        if (!existsPrepared.success()) return common::Result<Run>::failure(*existsPrepared.error());
        const auto existsBound = bindText(handle, exists.get(), 1, run.id);
        if (!existsBound.success()) return common::Result<Run>::failure(*existsBound.error());
        if (sqlite3_step(exists.get()) == SQLITE_ROW) {
            return common::Result<Run>::failure(
                error("run.revision_conflict", "Run was updated concurrently"));
        }
        return common::Result<Run>::failure(error("run.not_found", "Run not found"));
    }
    // Provider budget, verification-repair policy and interface contracts are immutable Run input:
    // the UPDATE above deliberately never touches these columns. Re-read the persisted values so a
    // stale/mutated caller copy cannot change them, matching InMemoryRunRepository::updateRun.
    Statement policyStatement;
    const auto policyPrepared = prepare(
        handle,
        "SELECT provider_budget_json,verification_repair_policy,run_interface_contracts_json "
        "FROM runs WHERE id=?;",
        policyStatement);
    if (!policyPrepared.success()) return common::Result<Run>::failure(*policyPrepared.error());
    const auto policyBound = bindText(handle, policyStatement.get(), 1, run.id);
    if (!policyBound.success()) return common::Result<Run>::failure(*policyBound.error());
    if (sqlite3_step(policyStatement.get()) != SQLITE_ROW) {
        return common::Result<Run>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read updated run's provider budget"));
    }
    const auto persistedPolicy = parseProviderBudgetPolicy(columnText(policyStatement.get(), 0));
    if (!persistedPolicy.success()) return common::Result<Run>::failure(*persistedPolicy.error());
    const auto persistedRepairPolicy = application::verificationRepairPolicyFromString(
        columnText(policyStatement.get(), 1));
    if (!persistedRepairPolicy.has_value()) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data",
            "Run row contains an unknown verification repair policy"));
    }
    const auto persistedContracts = application::parseRunInterfaceContracts(
        columnText(policyStatement.get(), 2));
    if (!persistedContracts.success())
        return common::Result<Run>::failure(*persistedContracts.error());
    run.providerBudget = persistedPolicy.value();
    run.verificationRepairPolicy = *persistedRepairPolicy;
    run.interfaceContracts = persistedContracts.value();
    ++run.revision;
    return common::Result<Run>::ok(std::move(run));
}

common::Result<std::optional<Run>> SqliteRunRepository::findRun(
    const application::RunId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runColumns + " FROM runs WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Run>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Run>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Run>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Run>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read run"));
    }
    auto run = readRun(statement.get());
    if (!run.success()) return common::Result<std::optional<Run>>::failure(*run.error());
    const auto assignments = fetchAssignmentsForRun(handle, id);
    if (!assignments.success()) return common::Result<std::optional<Run>>::failure(*assignments.error());
    run.value().assignmentSnapshot = assignments.value();
    return common::Result<std::optional<Run>>::ok(run.value());
}

common::Result<std::vector<Run>> SqliteRunRepository::listRuns(
    const std::optional<application::ProjectId>& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runColumns + " FROM runs"
        + (projectId.has_value() ? " WHERE project_id=?" : "")
        + " ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Run>>::failure(*prepared.error());
    if (projectId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *projectId);
        if (!bound.success()) return common::Result<std::vector<Run>>::failure(*bound.error());
    }

    std::vector<Run> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Run>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list runs"));
        }
        auto run = readRun(statement.get());
        if (!run.success()) return common::Result<std::vector<Run>>::failure(*run.error());
        runs.push_back(std::move(run.value()));
    }

    std::vector<std::string> runIds;
    runIds.reserve(runs.size());
    for (const auto& run : runs) runIds.push_back(run.id);
    const auto assignmentsByRun = fetchAssignmentsForRuns(handle, runIds);
    if (!assignmentsByRun.success()) return common::Result<std::vector<Run>>::failure(*assignmentsByRun.error());
    for (auto& run : runs) {
        const auto found = assignmentsByRun.value().find(run.id);
        if (found != assignmentsByRun.value().end()) run.assignmentSnapshot = found->second;
    }
    return common::Result<std::vector<Run>>::ok(std::move(runs));
}

common::Result<std::vector<Run>> SqliteRunRepository::listTerminalRunsPage(
    const application::ProjectId& projectId, const std::vector<std::string>& workflowIds,
    const std::optional<application::RunPageCursor>& after, std::size_t limit) const
{
    using Result = common::Result<std::vector<Run>>;
    if (limit == 0 || limit > application::kMaxRunPageSize) {
        return Result::failure(error("run.invalid_page", "The page size must be between 1 and 1000"));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // (No workflow asked is an empty IN () list, which SQLite answers with no row.)
    // A keyset page on the existing (project_id, created_at_ms, id) index: strictly after the cursor in (created_at_ms DESC, id DESC).
    std::string sql = std::string("SELECT ") + runColumns
        + " FROM runs WHERE project_id=? AND status IN ('completed','failed','cancelled','interrupted') AND workflow_id IN (";
    for (std::size_t index = 0; index < workflowIds.size(); ++index) sql += index == 0 ? "?" : ",?";
    sql += ")";
    if (after.has_value()) sql += " AND (created_at_ms < ? OR (created_at_ms = ? AND id < ?))";
    sql += " ORDER BY created_at_ms DESC, id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    int index = 0;
    const auto bound = [&](const common::Result<void>& binding) { return binding.success(); };
    if (!bound(bindText(handle, statement.get(), ++index, projectId))) return Result::failure(error("persistence.bind_failed", "Could not bind the project"));
    for (const auto& workflowId : workflowIds) {
        if (!bound(bindText(handle, statement.get(), ++index, workflowId))) return Result::failure(error("persistence.bind_failed", "Could not bind a workflow"));
    }
    if (after.has_value()) {
        const std::int64_t cursorMs = toMilliseconds(after->createdAt);
        if (!bound(bindInt64(handle, statement.get(), ++index, cursorMs)) || !bound(bindInt64(handle, statement.get(), ++index, cursorMs))
            || !bound(bindText(handle, statement.get(), ++index, after->id))) {
            return Result::failure(error("persistence.bind_failed", "Could not bind the cursor"));
        }
    }
    if (!bound(bindInt64(handle, statement.get(), ++index, static_cast<std::int64_t>(limit)))) {
        return Result::failure(error("persistence.bind_failed", "Could not bind the limit"));
    }

    std::vector<Run> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed", "Could not page runs"));
        }
        auto run = readRun(statement.get());
        if (!run.success()) return Result::failure(*run.error());
        runs.push_back(std::move(run.value()));
    }

    std::vector<std::string> runIds;
    runIds.reserve(runs.size());
    for (const auto& run : runs) runIds.push_back(run.id);
    const auto assignmentsByRun = fetchAssignmentsForRuns(handle, runIds);
    if (!assignmentsByRun.success()) return Result::failure(*assignmentsByRun.error());
    for (auto& run : runs) {
        const auto found = assignmentsByRun.value().find(run.id);
        if (found != assignmentsByRun.value().end()) run.assignmentSnapshot = found->second;
    }
    return Result::ok(std::move(runs));
}

common::Result<std::vector<Run>> SqliteRunRepository::listUnfinishedRuns() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runColumns
        + " FROM runs WHERE status NOT IN ('prepared','completed','failed','cancelled','interrupted') "
          "ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Run>>::failure(*prepared.error());

    std::vector<Run> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Run>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list unfinished runs"));
        }
        auto run = readRun(statement.get());
        if (!run.success()) return common::Result<std::vector<Run>>::failure(*run.error());
        runs.push_back(std::move(run.value()));
    }

    std::vector<std::string> runIds;
    runIds.reserve(runs.size());
    for (const auto& run : runs) runIds.push_back(run.id);
    const auto assignmentsByRun = fetchAssignmentsForRuns(handle, runIds);
    if (!assignmentsByRun.success()) return common::Result<std::vector<Run>>::failure(*assignmentsByRun.error());
    for (auto& run : runs) {
        const auto found = assignmentsByRun.value().find(run.id);
        if (found != assignmentsByRun.value().end()) run.assignmentSnapshot = found->second;
    }
    return common::Result<std::vector<Run>>::ok(std::move(runs));
}

common::Result<void> SqliteRunRepository::replaceStages(
    const application::RunId& runId,
    std::vector<RunStage> stages)
{
    for (const auto& stage : stages) {
        if (stage.runId != runId) {
            return common::Result<void>::failure(
                error("stage.run_mismatch", "Every stage must belong to the replaced run"));
        }
        const auto validProgress = validateProgress(stage.progress, "Stage");
        if (!validProgress.success()) return validProgress;
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // Delete-then-insert must be one transaction: a reader (GUI thread listing
    // stages) must never observe a run with zero stages between the two
    // statements, and a crash mid-replace must not leave a partial stage list.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    {
        Statement runExists;
        const auto existsPrepared = prepare(handle, "SELECT 1 FROM runs WHERE id=?;", runExists);
        if (!existsPrepared.success()) return rollbackWith(*existsPrepared.error());
        const auto existsBound = bindText(handle, runExists.get(), 1, runId);
        if (!existsBound.success()) return rollbackWith(*existsBound.error());
        if (sqlite3_step(runExists.get()) != SQLITE_ROW) {
            return rollbackWith(error("run.not_found", "Run not found"));
        }
    }

    {
        Statement remove;
        const auto removePrepared = prepare(handle, "DELETE FROM run_stages WHERE run_id=?;", remove);
        if (!removePrepared.success()) return rollbackWith(*removePrepared.error());
        const auto removeBound = bindText(handle, remove.get(), 1, runId);
        if (!removeBound.success()) return rollbackWith(*removeBound.error());
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not replace run stages"));
        }
    }

    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto inserted = insertStageRow(handle, stages[index], index);
        if (!inserted.success()) return rollbackWith(*inserted.error());
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

common::Result<RunStage> SqliteRunRepository::updateStage(RunStage stage)
{
    const auto validProgress = validateProgress(stage.progress, "Stage");
    if (!validProgress.success()) return common::Result<RunStage>::failure(*validProgress.error());
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE run_stages SET type=?,title=?,description=?,status=?,progress=?,provider_id=?,model_id=?,"
        "input_tokens=?,output_tokens=?,error_message=?,started_at_ms=?,finished_at_ms=? "
        "WHERE id=? AND run_id=?;",
        statement);
    if (!prepared.success()) return common::Result<RunStage>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, stage.type),
        bindText(handle, statement.get(), 2, stage.title),
        bindText(handle, statement.get(), 3, stage.description),
        bindText(handle, statement.get(), 4, application::toString(stage.status)),
        bindText(handle, statement.get(), 6, stage.providerId),
        bindText(handle, statement.get(), 7, stage.modelId),
        bindUnsigned(handle, statement.get(), 8, stage.usage.inputTokens),
        bindUnsigned(handle, statement.get(), 9, stage.usage.outputTokens),
        bindText(handle, statement.get(), 10, stage.errorMessage),
        bindOptionalTime(handle, statement.get(), 11, stage.startedAt),
        bindOptionalTime(handle, statement.get(), 12, stage.finishedAt),
        bindText(handle, statement.get(), 13, stage.id),
        bindText(handle, statement.get(), 14, stage.runId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<RunStage>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement.get(), 5, stage.progress) != SQLITE_OK) {
        return common::Result<RunStage>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind stage progress"));
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<RunStage>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update stage"));
    }
    if (sqlite3_changes(handle) == 0) {
        // A plain WHERE match collapses "run has no stages yet" and "stage id
        // unknown for this run" into one case; both already report the same
        // code here, so this is not an observable behavior change.
        return common::Result<RunStage>::failure(error("stage.not_found", "Stage not found"));
    }
    return common::Result<RunStage>::ok(std::move(stage));
}

common::Result<std::vector<RunStage>> SqliteRunRepository::listStages(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + stageColumns
        + " FROM run_stages WHERE run_id=? ORDER BY position;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<RunStage>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<RunStage>>::failure(*bound.error());

    std::vector<RunStage> stages;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<RunStage>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run stages"));
        }
        auto stage = readStage(statement.get());
        if (!stage.success()) return common::Result<std::vector<RunStage>>::failure(*stage.error());
        stages.push_back(stage.value());
    }
    return common::Result<std::vector<RunStage>>::ok(std::move(stages));
}

} // namespace ai::infrastructure
