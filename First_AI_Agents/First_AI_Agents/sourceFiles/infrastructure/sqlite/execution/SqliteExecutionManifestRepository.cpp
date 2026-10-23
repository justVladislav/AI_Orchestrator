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

nlohmann::json executionManifestToJson(
    const application::ExecutionManifest& manifest)
{
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& task : manifest.tasks) {
        const auto pathsToJson = [](const std::vector<std::filesystem::path>& paths) {
            nlohmann::json array = nlohmann::json::array();
            for (const auto& path : paths) {
                array.push_back(pathToUtf8(path));
            }
            return array;
        };
        const auto excludedOrDeferredToJson =
            [](const std::vector<application::ManifestExcludedOrDeferredPath>& paths) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto& entry : paths) {
                    array.push_back({{"path", pathToUtf8(entry.path)}, {"reason", entry.reason}});
                }
                return array;
            };
        nlohmann::json taskJson = {
            {"id", task.id},
            {"objective", task.objective},
            {"role", task.role},
            {"dependencies", task.dependencies},
            {"ownedPaths", pathsToJson(task.ownedPaths)},
            // AG-15: previously only ownedPaths round-tripped through this
            // repository - a real Run (bb56e001-eac0-4b59-8d0b-d037c6bc1031)
            // froze a RunWorkspaceScope with real planned_create entries
            // (proving the in-memory manifest had them at freeze time), yet
            // the reloaded manifest showed none, because these six fields
            // were never written to manifest_json at all. Every task using
            // AG-15 roles silently lost them on the very first persist.
            {"plannedCreatePaths", pathsToJson(task.plannedCreatePaths)},
            {"requiredReadPaths", pathsToJson(task.requiredReadPaths)},
            {"referencePaths", pathsToJson(task.referencePaths)},
            {"excludedPaths", excludedOrDeferredToJson(task.excludedPaths)},
            {"deferredPaths", excludedOrDeferredToJson(task.deferredPaths)},
            {"baselineOnlyReadPaths", pathsToJson(task.baselineOnlyReadPaths)},
            {"acceptanceCriteria", task.acceptanceCriteria},
            {"contextReferences", task.contextReferences},
            {"requestedCapability", application::toString(task.requestedCapability)},
            // AG-17.1: absent (null) = the single worker of a single-assignment Manifest.
            {"assigneeMemberId", task.assigneeMemberId.has_value()
                ? nlohmann::json(*task.assigneeMemberId) : nlohmann::json(nullptr)}
        };
        // Previously omitted entirely: a frozen manifest's structured
        // acceptanceContract never round-tripped through this repository, so
        // every reload (a resumed Run, a restarted app, or combined
        // verification re-reading the frozen manifest) silently lost every
        // task's acceptance requirements - the exact condition that produced
        // a real verification.invalid_evidence_reference failure with no
        // recoverable diagnostic, since the contract that verification
        // needed was already gone from what got persisted.
        if (task.acceptanceContract.has_value()) {
            nlohmann::json requirements = nlohmann::json::array();
            for (const auto& requirement : task.acceptanceContract->requirements) {
                nlohmann::json arguments = nlohmann::json::array();
                for (const auto& argument : requirement.verificationArguments) {
                    arguments.push_back({
                        {"name", argument.name}, {"value", argument.value}});
                }
                // AG-18.2: the Backend-resolved selection identity that
                // produced verificationArguments above - persisted so a
                // restarted process reads back the same immutable binding
                // rather than ever re-resolving it. Absent (null) for
                // inspect_paths and for any pre-AG-18.2 frozen manifest.
                const auto optionalSelection =
                    [](const std::optional<std::string>& value) {
                        return value.has_value()
                            ? nlohmann::json(*value) : nlohmann::json(nullptr);
                    };
                requirements.push_back({
                    {"id", requirement.id},
                    {"description", requirement.description},
                    {"requiredOutputs", requirement.requiredOutputs},
                    {"verificationKind", application::toString(requirement.verificationKind)},
                    {"verificationArguments", std::move(arguments)},
                    {"required", requirement.required},
                    {"verificationProfileId", optionalSelection(requirement.verificationProfileId)},
                    {"verificationBuildProfileId",
                        optionalSelection(requirement.verificationBuildProfileId)},
                    {"verificationTargetId", optionalSelection(requirement.verificationTargetId)},
                    {"qmlLintDiagnosticPolicyId",
                        optionalSelection(requirement.qmlLintDiagnosticPolicyId)},
                    // AG-17.0: absent in a pre-AG-17.0 row = planner_declared.
                    {"origin", application::toString(requirement.origin)}
                });
            }
            taskJson["acceptanceContract"] = {
                {"schemaVersion", task.acceptanceContract->schemaVersion},
                {"requirements", std::move(requirements)}
            };
        } else {
            taskJson["acceptanceContract"] = nullptr;
        }
        tasks.push_back(std::move(taskJson));
    }

    const auto optionalUnsigned = [](const std::optional<std::uint64_t>& value) {
        return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    // AG-17.0: the frozen mandatory-verification decision record (null when
    // the policy was not evaluated, e.g. a pre-AG-17.0 Manifest).
    nlohmann::json mandatoryVerification = nullptr;
    if (manifest.mandatoryVerification.has_value()) {
        nlohmann::json decisions = nlohmann::json::array();
        for (const auto& decision : manifest.mandatoryVerification->decisions) {
            decisions.push_back({
                {"kind", application::toString(decision.kind)},
                {"status", application::toString(decision.status)},
                {"reasonCode", decision.reasonCode},
                {"requirementId", decision.requirementId},
                {"taskId", decision.taskId},
                {"profileId", decision.profileId},
                {"buildProfileId", decision.buildProfileId},
                {"targetId", decision.targetId}
            });
        }
        mandatoryVerification = {
            {"policyId", manifest.mandatoryVerification->policyId},
            {"mode", manifest.mandatoryVerification->mode},
            {"decisions", std::move(decisions)}
        };
    }
    // AG-17.1: the frozen Backend-owned routing record (null when none).
    nlohmann::json routing = nullptr;
    if (manifest.routing.has_value()) {
        nlohmann::json members = nlohmann::json::array();
        for (const auto& member : manifest.routing->members) {
            members.push_back({
                {"memberId", member.memberId},
                {"providerId", member.providerId},
                {"modelId", member.modelId},
                {"role", member.role},
                {"leadArchitect", member.leadArchitect},
                {"workspaceAccess", member.workspaceAccess},
                {"instructionsHash", member.instructionsHash},
                {"position", member.position},
                {"timeoutMs", member.timeoutMs}
            });
        }
        routing = {
            {"schemaVersion", manifest.routing->schemaVersion},
            {"mode", application::toString(manifest.routing->mode)},
            {"members", std::move(members)},
            {"fingerprint", manifest.routing->fingerprint}
        };
    }
    return {
        {"schemaVersion", manifest.schemaVersion},
        {"runId", manifest.runId},
        {"tasks", std::move(tasks)},
        {"routing", std::move(routing)},
        {"mandatoryVerification", std::move(mandatoryVerification)},
        {"requestedMaxParallelism", manifest.requestedMaxParallelism},
        {"requestedTokenBudget", optionalUnsigned(manifest.requestedTokenBudget)},
        {"requestedCostMicros", optionalUnsigned(manifest.requestedCostMicros)},
        {"policy", {
            {"maxTasks", manifest.policy.maxTasks},
            {"maxDepth", manifest.policy.maxDepth},
            {"maxParallelism", manifest.policy.maxParallelism},
            {"maxOwnedPathsPerTask", manifest.policy.maxOwnedPathsPerTask},
            {"maxAcceptanceCriteriaPerTask", manifest.policy.maxAcceptanceCriteriaPerTask},
            {"maxContextReferencesPerTask", manifest.policy.maxContextReferencesPerTask},
            {"maximumCapability", application::toString(manifest.policy.maximumCapability)},
            {"tokenBudgetCeiling", optionalUnsigned(manifest.policy.tokenBudgetCeiling)},
            {"costMicrosCeiling", optionalUnsigned(manifest.policy.costMicrosCeiling)}
        }}
    };
}

common::Result<application::ExecutionManifest> executionManifestFromJson(
    const std::string& raw,
    const std::string& storedSchemaVersion,
    std::chrono::system_clock::time_point createdAt)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        application::ExecutionManifest manifest;
        manifest.schemaVersion = document.at("schemaVersion").get<std::string>();
        manifest.runId = document.at("runId").get<std::string>();
        manifest.requestedMaxParallelism =
            document.at("requestedMaxParallelism").get<std::uint32_t>();
        if (!document.at("requestedTokenBudget").is_null())
            manifest.requestedTokenBudget =
                document.at("requestedTokenBudget").get<std::uint64_t>();
        if (!document.at("requestedCostMicros").is_null())
            manifest.requestedCostMicros =
                document.at("requestedCostMicros").get<std::uint64_t>();

        const auto& policy = document.at("policy");
        manifest.policy.maxTasks = policy.at("maxTasks").get<std::uint32_t>();
        manifest.policy.maxDepth = policy.at("maxDepth").get<std::uint32_t>();
        manifest.policy.maxParallelism = policy.at("maxParallelism").get<std::uint32_t>();
        manifest.policy.maxOwnedPathsPerTask =
            policy.at("maxOwnedPathsPerTask").get<std::uint32_t>();
        manifest.policy.maxAcceptanceCriteriaPerTask =
            policy.at("maxAcceptanceCriteriaPerTask").get<std::uint32_t>();
        manifest.policy.maxContextReferencesPerTask =
            policy.at("maxContextReferencesPerTask").get<std::uint32_t>();
        const auto maximumCapability = application::workspaceCapabilityFromString(
            policy.at("maximumCapability").get<std::string>());
        if (!maximumCapability.has_value())
            throw std::invalid_argument("invalid maximumCapability");
        manifest.policy.maximumCapability = *maximumCapability;
        if (!policy.at("tokenBudgetCeiling").is_null())
            manifest.policy.tokenBudgetCeiling =
                policy.at("tokenBudgetCeiling").get<std::uint64_t>();
        if (!policy.at("costMicrosCeiling").is_null())
            manifest.policy.costMicrosCeiling =
                policy.at("costMicrosCeiling").get<std::uint64_t>();

        for (const auto& taskJson : document.at("tasks")) {
            application::ExecutionManifestTask task;
            task.id = taskJson.at("id").get<std::string>();
            task.objective = taskJson.at("objective").get<std::string>();
            task.role = taskJson.at("role").get<std::string>();
            task.dependencies = taskJson.at("dependencies").get<std::vector<std::string>>();
            for (const auto& path : taskJson.at("ownedPaths"))
                task.ownedPaths.push_back(pathFromUtf8(path.get<std::string>()));
            // AG-15: absent entirely for a manifest persisted before this
            // fix (or any other pre-AG-15 row) - backward compatible,
            // defaults to empty exactly as an unpopulated field would.
            const auto readPaths = [&taskJson](const char* key) {
                std::vector<std::filesystem::path> paths;
                if (taskJson.contains(key)) {
                    for (const auto& path : taskJson.at(key))
                        paths.push_back(pathFromUtf8(path.get<std::string>()));
                }
                return paths;
            };
            task.plannedCreatePaths = readPaths("plannedCreatePaths");
            task.requiredReadPaths = readPaths("requiredReadPaths");
            task.referencePaths = readPaths("referencePaths");
            task.baselineOnlyReadPaths = readPaths("baselineOnlyReadPaths");
            const auto readExcludedOrDeferred = [&taskJson](const char* key) {
                std::vector<application::ManifestExcludedOrDeferredPath> paths;
                if (taskJson.contains(key)) {
                    for (const auto& entryJson : taskJson.at(key)) {
                        application::ManifestExcludedOrDeferredPath entry;
                        entry.path = pathFromUtf8(entryJson.at("path").get<std::string>());
                        entry.reason = entryJson.at("reason").get<std::string>();
                        paths.push_back(std::move(entry));
                    }
                }
                return paths;
            };
            task.excludedPaths = readExcludedOrDeferred("excludedPaths");
            task.deferredPaths = readExcludedOrDeferred("deferredPaths");
            task.acceptanceCriteria =
                taskJson.at("acceptanceCriteria").get<std::vector<std::string>>();
            task.contextReferences =
                taskJson.at("contextReferences").get<std::vector<std::string>>();
            const auto capability = application::workspaceCapabilityFromString(
                taskJson.at("requestedCapability").get<std::string>());
            if (!capability.has_value())
                throw std::invalid_argument("invalid requestedCapability");
            task.requestedCapability = *capability;
            // AG-17.1: absent in a pre-AG-17.1 row; a present non-string is corrupt.
            if (taskJson.contains("assigneeMemberId")
                    && !taskJson.at("assigneeMemberId").is_null()) {
                task.assigneeMemberId = taskJson.at("assigneeMemberId").get<std::string>();
            }
            if (taskJson.contains("acceptanceContract")
                    && !taskJson.at("acceptanceContract").is_null()) {
                const auto& contractJson = taskJson.at("acceptanceContract");
                application::ExecutionAcceptanceContract contract;
                contract.schemaVersion = contractJson.at("schemaVersion").get<std::string>();
                for (const auto& reqJson : contractJson.at("requirements")) {
                    application::ExecutionAcceptanceRequirement requirement;
                    requirement.id = reqJson.at("id").get<std::string>();
                    requirement.description = reqJson.at("description").get<std::string>();
                    requirement.requiredOutputs =
                        reqJson.at("requiredOutputs").get<std::vector<std::string>>();
                    const auto kind = application::executionVerificationKindFromString(
                        reqJson.at("verificationKind").get<std::string>());
                    if (!kind.has_value())
                        throw std::invalid_argument("invalid verificationKind");
                    requirement.verificationKind = *kind;
                    for (const auto& argJson : reqJson.at("verificationArguments")) {
                        requirement.verificationArguments.push_back({
                            argJson.at("name").get<std::string>(),
                            argJson.at("value").get<std::string>()});
                    }
                    requirement.required = reqJson.at("required").get<bool>();
                    // AG-18.2: absent entirely for a pre-AG-18.2 persisted
                    // manifest (backward compatible - defaults to nullopt,
                    // exactly as a fresh inspect_paths requirement would),
                    // present-but-null for inspect_paths itself.
                    const auto readOptionalSelection =
                        [&reqJson](const char* key) -> std::optional<std::string> {
                            if (!reqJson.contains(key) || reqJson.at(key).is_null())
                                return std::nullopt;
                            return reqJson.at(key).get<std::string>();
                        };
                    requirement.verificationProfileId =
                        readOptionalSelection("verificationProfileId");
                    requirement.verificationBuildProfileId =
                        readOptionalSelection("verificationBuildProfileId");
                    requirement.verificationTargetId =
                        readOptionalSelection("verificationTargetId");
                    requirement.qmlLintDiagnosticPolicyId =
                        readOptionalSelection("qmlLintDiagnosticPolicyId");
                    // AG-17.0: absent entirely for a pre-AG-17.0 manifest =
                    // planner_declared; a present but unknown value is corrupt.
                    if (reqJson.contains("origin")) {
                        const auto origin = application::executionRequirementOriginFromString(
                            reqJson.at("origin").get<std::string>());
                        if (!origin.has_value())
                            throw std::invalid_argument("invalid requirement origin");
                        requirement.origin = *origin;
                    }
                    contract.requirements.push_back(std::move(requirement));
                }
                task.acceptanceContract = std::move(contract);
            }
            manifest.tasks.push_back(std::move(task));
        }
        if (manifest.schemaVersion != storedSchemaVersion)
            throw std::invalid_argument("schema version mismatch");
        // AG-17.1: absent/null for a Manifest frozen before routing existed.
        if (document.contains("routing") && !document.at("routing").is_null()) {
            const auto& routingJson = document.at("routing");
            application::ManifestRoutingRecord routing;
            routing.schemaVersion = routingJson.at("schemaVersion").get<std::string>();
            const auto mode = application::manifestRoutingModeFromString(
                routingJson.at("mode").get<std::string>());
            if (!mode.has_value()) throw std::invalid_argument("invalid routing mode");
            routing.mode = *mode;
            for (const auto& memberJson : routingJson.at("members")) {
                application::ManifestRoutingMember member;
                member.memberId = memberJson.at("memberId").get<std::string>();
                member.providerId = memberJson.at("providerId").get<std::string>();
                member.modelId = memberJson.at("modelId").get<std::string>();
                member.role = memberJson.at("role").get<std::string>();
                member.leadArchitect = memberJson.at("leadArchitect").get<bool>();
                member.workspaceAccess = memberJson.at("workspaceAccess").get<bool>();
                member.instructionsHash = memberJson.at("instructionsHash").get<std::string>();
                member.position = memberJson.at("position").get<std::uint32_t>();
                member.timeoutMs = memberJson.at("timeoutMs").get<std::uint64_t>();
                routing.members.push_back(std::move(member));
            }
            routing.fingerprint = routingJson.at("fingerprint").get<std::string>();
            manifest.routing = std::move(routing);
        }
        // AG-17.0: absent/null for a Manifest frozen before the policy existed.
        if (document.contains("mandatoryVerification")
                && !document.at("mandatoryVerification").is_null()) {
            const auto& recordJson = document.at("mandatoryVerification");
            application::ManifestMandatoryVerificationRecord record;
            record.policyId = recordJson.at("policyId").get<std::string>();
            record.mode = recordJson.at("mode").get<std::string>();
            for (const auto& decisionJson : recordJson.at("decisions")) {
                application::ManifestMandatoryVerificationDecision decision;
                const auto kind = application::executionVerificationKindFromString(
                    decisionJson.at("kind").get<std::string>());
                const auto status = application::mandatoryVerificationDecisionStatusFromString(
                    decisionJson.at("status").get<std::string>());
                if (!kind.has_value() || !status.has_value())
                    throw std::invalid_argument("invalid mandatory verification decision");
                decision.kind = *kind;
                decision.status = *status;
                decision.reasonCode = decisionJson.at("reasonCode").get<std::string>();
                decision.requirementId = decisionJson.at("requirementId").get<std::string>();
                decision.taskId = decisionJson.at("taskId").get<std::string>();
                decision.profileId = decisionJson.at("profileId").get<std::string>();
                decision.buildProfileId = decisionJson.at("buildProfileId").get<std::string>();
                decision.targetId = decisionJson.at("targetId").get<std::string>();
                record.decisions.push_back(std::move(decision));
            }
            manifest.mandatoryVerification = std::move(record);
        }
        manifest.createdAt = createdAt;
        return common::Result<application::ExecutionManifest>::ok(std::move(manifest));
    } catch (const std::exception& exception) {
        return common::Result<application::ExecutionManifest>::failure(error(
            "manifest.corrupt_snapshot",
            std::string("Stored execution manifest is invalid: ") + exception.what()));
    }
}

} // namespace

SqliteExecutionManifestRepository::SqliteExecutionManifestRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::ExecutionManifest>
SqliteExecutionManifestRepository::insert(application::ExecutionManifest manifest)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO execution_manifests "
        "(run_id,schema_version,manifest_json,created_at_ms) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success())
        return common::Result<application::ExecutionManifest>::failure(*prepared.error());

    const std::string document = executionManifestToJson(manifest).dump();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, manifest.runId),
        bindText(handle, statement.get(), 2, manifest.schemaVersion),
        bindText(handle, statement.get(), 3, document),
        bindInt64(handle, statement.get(), 4, toMilliseconds(manifest.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success())
            return common::Result<application::ExecutionManifest>::failure(*binding.error());
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<application::ExecutionManifest>::failure(error(
                "manifest.already_frozen",
                "Execution manifest is already frozen for this Run"));
        }
        return common::Result<application::ExecutionManifest>::failure(databaseError(
            handle, "manifest.insert_failed", "Could not freeze execution manifest"));
    }
    return common::Result<application::ExecutionManifest>::ok(std::move(manifest));
}

common::Result<std::optional<application::ExecutionManifest>>
SqliteExecutionManifestRepository::findForRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT schema_version,manifest_json,created_at_ms "
        "FROM execution_manifests WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            *bound.error());
    }

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::ExecutionManifest>>::ok(
            std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            databaseError(handle, "manifest.read_failed",
                "Could not read frozen execution manifest"));
    }
    auto parsed = executionManifestFromJson(
        columnText(statement.get(), 1),
        columnText(statement.get(), 0),
        fromMilliseconds(sqlite3_column_int64(statement.get(), 2)));
    if (!parsed.success()) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            *parsed.error());
    }
    if (parsed.value().runId != runId) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(error(
            "manifest.corrupt_snapshot", "Stored execution manifest Run id does not match its key"));
    }
    return common::Result<std::optional<application::ExecutionManifest>>::ok(
        std::move(parsed.value()));
}

} // namespace ai::infrastructure
