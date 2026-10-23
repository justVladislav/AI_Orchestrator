#include "ai/infrastructure/SqliteRepositories.hpp"
#include "sqlite/SqliteConnection.hpp"
#include "sqlite/SqlitePrimitives.hpp"
#include "sqlite/catalog/SqliteProfileMapping.hpp"
#include "sqlite/roster/SqliteRosterMapping.hpp"
#include "sqlite/planning/SqliteRunDraftGroupStorage.hpp"

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

std::optional<RunStatus> runStatusFromString(std::string_view value)
{
    if (value == "prepared") return RunStatus::Prepared;
    if (value == "queued") return RunStatus::Queued;
    if (value == "running") return RunStatus::Running;
    if (value == "cancelling") return RunStatus::Cancelling;
    if (value == "completed") return RunStatus::Completed;
    if (value == "failed") return RunStatus::Failed;
    if (value == "cancelled") return RunStatus::Cancelled;
    if (value == "interrupted") return RunStatus::Interrupted;
    return std::nullopt;
}

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
constexpr const char* artifactColumns =
    "id, run_id, stage_id, kind, name, media_type, relative_path, byte_size, sha256, created_at_ms";
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

namespace {

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

namespace {

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

} // namespace

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

namespace {

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

namespace {

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

} // namespace

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

namespace {

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

} // namespace

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

namespace {

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

namespace {

bool safeVerificationTarget(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

bool safeVerificationRelativePath(std::string_view value)
{
    if (value.empty()
        || value.size() > application::kVerificationEnvironmentMaxRelativePathBytes
        || value.front() == '/' || value.back() == '/'
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos
        || value.find('$') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(start,
            (end == std::string_view::npos ? value.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (std::any_of(part.begin(), part.end(), [](unsigned char ch) {
                return ch < 0x20 || ch == 0x7f || ch == '*' || ch == '?' || ch == '['
                    || ch == ']' || ch == '{' || ch == '}';
            })) {
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

nlohmann::json buildPresetToJson(const application::VerificationBuildPresetDescriptor& preset)
{
    return nlohmann::json {
        {"name", preset.name},
        {"displayName", preset.displayName},
        {"source", application::toString(preset.source)},
        {"declaredConfigurePreset", preset.declaredConfigurePreset},
        {"declaredTargets", preset.declaredTargets},
        {"allowedTargetsConfirmed", preset.allowedTargetsConfirmed},
        {"allowedTargets", preset.allowedTargets},
        {"allowedTargetsComplete", preset.allowedTargetsComplete}
    };
}

common::Result<application::VerificationBuildPresetDescriptor> buildPresetFromJson(
    const nlohmann::json& j)
{
    auto corrupt = [] {
        return common::Result<application::VerificationBuildPresetDescriptor>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt verification environment build preset"));
    };
    const bool hasAllowedTargetsConfirmed = j.contains("allowedTargetsConfirmed");
    const bool hasAllowedTargets = j.contains("allowedTargets");
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()
        || !j.contains("source") || !j["source"].is_string()
        || !j.contains("declaredTargets") || !j["declaredTargets"].is_array()
        || j["declaredTargets"].size() > application::kVerificationEnvironmentMaxTargetsPerPreset
        || hasAllowedTargetsConfirmed != hasAllowedTargets
        || (hasAllowedTargetsConfirmed && !j["allowedTargetsConfirmed"].is_boolean())
        || (j.contains("allowedTargetsComplete") && !j["allowedTargetsComplete"].is_boolean())
        || (hasAllowedTargets && (!j["allowedTargets"].is_array()
            || j["allowedTargets"].size() > application::kVerificationEnvironmentMaxTargetsPerPreset))) {
        return corrupt();
    }
    const auto source = application::verificationPresetFileSourceFromString(
        j["source"].get<std::string>());
    if (!source.has_value()) return corrupt();
    application::VerificationBuildPresetDescriptor preset;
    preset.name = j["name"].get<std::string>();
    if (j.contains("displayName") && j["displayName"].is_string())
        preset.displayName = j["displayName"].get<std::string>();
    preset.source = *source;
    if (j.contains("declaredConfigurePreset") && j["declaredConfigurePreset"].is_string())
        preset.declaredConfigurePreset = j["declaredConfigurePreset"].get<std::string>();
    for (const auto& target : j["declaredTargets"]) {
        if (!target.is_string()) return corrupt();
        preset.declaredTargets.push_back(target.get<std::string>());
    }
    // Rows written before the additive target-catalog fields existed remain
    // readable as an explicitly unconfirmed catalog. AG-18.2 must treat this
    // state as setup-required/blocked, never as an unrestricted allow-list.
    preset.allowedTargetsConfirmed = hasAllowedTargetsConfirmed
        && j["allowedTargetsConfirmed"].get<bool>();
    // AG-17.0: additive. A row written before the field existed carries no
    // completeness proof and reads as NOT complete (unknown), never complete.
    preset.allowedTargetsComplete = j.contains("allowedTargetsComplete")
        && j["allowedTargetsComplete"].get<bool>();
    if (preset.allowedTargetsComplete && !preset.allowedTargetsConfirmed) return corrupt();
    std::unordered_set<std::string> seenAllowedTargets;
    if (hasAllowedTargets) {
        for (const auto& target : j["allowedTargets"]) {
            if (!target.is_string() || !safeVerificationTarget(target.get<std::string>())) return corrupt();
            const auto value = target.get<std::string>();
            if (!seenAllowedTargets.insert(value).second) return corrupt();
            preset.allowedTargets.push_back(value);
        }
    }
    if (!preset.allowedTargetsConfirmed && !preset.allowedTargets.empty()) return corrupt();
    return common::Result<application::VerificationBuildPresetDescriptor>::ok(std::move(preset));
}

nlohmann::json buildPresetsToJson(
    const std::vector<application::VerificationBuildPresetDescriptor>& presets)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& preset : presets) array.push_back(buildPresetToJson(preset));
    return array;
}

common::Result<std::vector<application::VerificationBuildPresetDescriptor>>
buildPresetsFromJsonArray(const nlohmann::json& parsed, std::size_t maxCount)
{
    if (!parsed.is_array() || parsed.size() > maxCount) {
        return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt or out-of-bound verification environment build presets"));
    }
    std::vector<application::VerificationBuildPresetDescriptor> presets;
    presets.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto preset = buildPresetFromJson(item);
        if (!preset.success()) {
            return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
                *preset.error());
        }
        presets.push_back(std::move(preset.value()));
    }
    return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::ok(
        std::move(presets));
}

common::Result<std::vector<application::VerificationBuildPresetDescriptor>>
buildPresetsFromJson(const std::string& json, std::size_t maxCount)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded()) {
        return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt verification environment build presets column"));
    }
    return buildPresetsFromJsonArray(parsed, maxCount);
}

nlohmann::json profileToJson(const application::VerificationProfile& profile)
{
    return nlohmann::json {
        {"profileId", profile.profileId},
        {"configurePresetName", profile.configurePresetName},
        {"configureDisplayName", profile.configureDisplayName},
        {"configureSource", application::toString(profile.configureSource)},
        {"declaredBinaryDir", profile.declaredBinaryDir},
        {"resolvedBinaryDir", profile.resolvedBinaryDir},
        {"origin", application::toString(profile.origin)},
        {"buildPresetsConfirmed", profile.buildPresetsConfirmed},
        {"buildPresets", buildPresetsToJson(profile.buildPresets)}
    };
}

common::Result<application::VerificationProfile> profileFromJson(const nlohmann::json& j)
{
    auto corrupt = [] {
        return common::Result<application::VerificationProfile>::failure(
            error("verification_environment.corrupt_entry", "Corrupt verification environment profile"));
    };
    if (!j.is_object()
        || !j.contains("profileId") || !j["profileId"].is_string() || j["profileId"].get<std::string>().empty()
        || !j.contains("configurePresetName") || !j["configurePresetName"].is_string()
        || j["configurePresetName"].get<std::string>().empty()
        || !j.contains("configureSource") || !j["configureSource"].is_string()
        || !j.contains("origin") || !j["origin"].is_string()
        || !j.contains("buildPresetsConfirmed") || !j["buildPresetsConfirmed"].is_boolean()
        || !j.contains("buildPresets") || !j["buildPresets"].is_array()
        || j["buildPresets"].size() > application::kVerificationEnvironmentMaxBuildPresetsPerProfile) {
        return corrupt();
    }
    const auto source = application::verificationPresetFileSourceFromString(
        j["configureSource"].get<std::string>());
    const auto origin = application::verificationProfileOriginFromString(j["origin"].get<std::string>());
    if (!source.has_value() || !origin.has_value()) return corrupt();
    application::VerificationProfile profile;
    profile.profileId = j["profileId"].get<std::string>();
    profile.configurePresetName = j["configurePresetName"].get<std::string>();
    if (j.contains("configureDisplayName") && j["configureDisplayName"].is_string())
        profile.configureDisplayName = j["configureDisplayName"].get<std::string>();
    profile.configureSource = *source;
    if (j.contains("declaredBinaryDir") && j["declaredBinaryDir"].is_string())
        profile.declaredBinaryDir = j["declaredBinaryDir"].get<std::string>();
    if (j.contains("resolvedBinaryDir") && j["resolvedBinaryDir"].is_string())
        profile.resolvedBinaryDir = j["resolvedBinaryDir"].get<std::string>();
    if (!profile.resolvedBinaryDir.empty()
        && !safeVerificationRelativePath(profile.resolvedBinaryDir)) {
        return corrupt();
    }
    profile.origin = *origin;
    profile.buildPresetsConfirmed = j["buildPresetsConfirmed"].get<bool>();
    auto buildPresets = buildPresetsFromJsonArray(
        j["buildPresets"], application::kVerificationEnvironmentMaxBuildPresetsPerProfile);
    if (!buildPresets.success()) {
        return common::Result<application::VerificationProfile>::failure(*buildPresets.error());
    }
    profile.buildPresets = std::move(buildPresets.value());
    return common::Result<application::VerificationProfile>::ok(std::move(profile));
}

std::string profilesToJson(const std::vector<application::VerificationProfile>& profiles)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& profile : profiles) array.push_back(profileToJson(profile));
    return array.dump();
}

common::Result<std::vector<application::VerificationProfile>> profilesFromJson(const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()
        || parsed.size() > application::kVerificationEnvironmentMaxProfiles) {
        return common::Result<std::vector<application::VerificationProfile>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt or out-of-bound verification environment profiles column"));
    }
    std::vector<application::VerificationProfile> profiles;
    profiles.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto profile = profileFromJson(item);
        if (!profile.success()) {
            return common::Result<std::vector<application::VerificationProfile>>::failure(*profile.error());
        }
        profiles.push_back(std::move(profile.value()));
    }
    return common::Result<std::vector<application::VerificationProfile>>::ok(std::move(profiles));
}

} // namespace

SqliteVerificationEnvironmentRepository::SqliteVerificationEnvironmentRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteVerificationEnvironmentRepository::recordSnapshot(
    application::VerificationEnvironmentSnapshot snapshot)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_environment_snapshots "
        "(run_id,project_id,schema_version,baseline_fingerprint,state,"
        "profiles_json,unlinked_build_presets_json,diagnostic,discovered_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, snapshot.runId),
        bindText(handle, statement.get(), 2, snapshot.projectId),
        bindText(handle, statement.get(), 3, snapshot.schemaVersion),
        bindText(handle, statement.get(), 4, snapshot.baselineFingerprint),
        bindText(handle, statement.get(), 5, application::toString(snapshot.state)),
        bindText(handle, statement.get(), 6, profilesToJson(snapshot.profiles)),
        bindText(handle, statement.get(), 7, buildPresetsToJson(snapshot.unlinkedBuildPresets).dump()),
        bindText(handle, statement.get(), 8, snapshot.diagnostic),
        bindInt64(handle, statement.get(), 9, toMilliseconds(snapshot.discoveredAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "verification_environment.already_recorded",
                "A verification environment snapshot already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification environment snapshot"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::VerificationEnvironmentSnapshot>>
SqliteVerificationEnvironmentRepository::findForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_id,schema_version,baseline_fingerprint,state,"
        "profiles_json,unlinked_build_presets_json,diagnostic,discovered_at_ms "
        "FROM verification_environment_snapshots WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::ok(
            std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification environment snapshot"));
    }
    auto corrupt = [] {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            error("verification_environment.corrupt_entry", "Corrupt verification environment snapshot row"));
    };
    application::VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = columnText(statement.get(), 0);
    snapshot.schemaVersion = columnText(statement.get(), 1);
    // Fail closed on identity: a snapshot from an unrecognized schema
    // version, or with a blank required identity field, must never be
    // silently accepted as if it were today's contract.
    if (snapshot.schemaVersion != application::kVerificationEnvironmentSnapshotSchemaVersion
        || snapshot.projectId.empty()) {
        return corrupt();
    }
    snapshot.baselineFingerprint = columnText(statement.get(), 2);
    const auto state = application::verificationEnvironmentStateFromString(columnText(statement.get(), 3));
    if (!state.has_value()) return corrupt();
    snapshot.state = *state;
    auto profiles = profilesFromJson(columnText(statement.get(), 4));
    if (!profiles.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *profiles.error());
    }
    snapshot.profiles = std::move(profiles.value());
    auto unlinkedBuildPresets = buildPresetsFromJson(
        columnText(statement.get(), 5), application::kVerificationEnvironmentMaxUnlinkedBuildPresets);
    if (!unlinkedBuildPresets.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *unlinkedBuildPresets.error());
    }
    snapshot.unlinkedBuildPresets = std::move(unlinkedBuildPresets.value());
    snapshot.diagnostic = columnText(statement.get(), 6);
    snapshot.discoveredAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    // Fail closed on state/content consistency: a stored row claiming
    // ExistingProfile with no profiles, or a non-ExistingProfile state that
    // somehow carries profiles, is corrupt data, not a value this reader
    // silently trusts.
    const bool hasProfiles = !snapshot.profiles.empty();
    const bool claimsExistingProfile =
        snapshot.state == application::VerificationEnvironmentState::ExistingProfile;
    if (claimsExistingProfile != hasProfiles) return corrupt();
    return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::ok(
        std::move(snapshot));
}

namespace {

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

namespace {

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

} // namespace

common::Result<std::optional<application::VerificationBootstrapDecision>>
SqliteVerificationBootstrapRepository::findDecisionForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readDecisionForRunUnlocked(database_->impl_->handle, runId);
}

namespace {

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

} // namespace

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

namespace {

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

common::Result<std::optional<application::VerificationBootstrapRetryLink>>
SqliteVerificationBootstrapRepository::findRetryLinkForPredecessor(
    const application::RunId& predecessorRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readRetryLinkForPredecessorUnlocked(database_->impl_->handle, predecessorRunId);
}

SqliteManifestTaskExecutionRepository::SqliteManifestTaskExecutionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestTaskExecutionRepository::upsert(
    const application::RunId& runId, const std::string& taskId,
    const application::ManifestTaskState& state)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO manifest_task_executions "
        "(run_id,task_id,status,error_code,error_message,change_set_id,attempts,"
        "output_summary,inspected_relative_paths,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    // AG-15: same JSON-array-of-strings shape as
    // manifest_task_no_changes.inspected_relative_paths (v32).
    nlohmann::json inspectedArray = nlohmann::json::array();
    for (const auto& path : state.inspectedRelativePaths) inspectedArray.push_back(path);

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, taskId),
        bindText(handle, statement.get(), 3, application::toString(state.status)),
        bindText(handle, statement.get(), 4, state.errorCode),
        bindText(handle, statement.get(), 5, state.errorMessage),
        bindNullableText(handle, statement.get(), 6, state.changeSetId),
        bindInt64(handle, statement.get(), 7, static_cast<std::int64_t>(state.attempts)),
        bindText(handle, statement.get(), 8, state.outputSummary),
        bindText(handle, statement.get(), 9, inspectedArray.dump()),
        bindInt64(handle, statement.get(), 10, toMilliseconds(std::chrono::system_clock::now()))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_task.write_failed", "Could not persist manifest task execution state"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestTaskExecutionRecord>>
SqliteManifestTaskExecutionRepository::listForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT task_id,status,error_code,error_message,change_set_id,attempts,"
        "output_summary,inspected_relative_paths,updated_at_ms FROM manifest_task_executions "
        "WHERE run_id=? ORDER BY task_id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            *bound.error());
    }

    std::vector<application::ManifestTaskExecutionRecord> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                databaseError(handle, "manifest_task.read_failed",
                    "Could not read manifest task execution state"));
        }
        application::ManifestTaskExecutionRecord record;
        record.runId = runId;
        record.taskId = columnText(statement.get(), 0);
        record.state.status = application::manifestTaskStatusFromString(columnText(statement.get(), 1));
        record.state.errorCode = columnText(statement.get(), 2);
        record.state.errorMessage = columnText(statement.get(), 3);
        record.state.changeSetId = optionalText(statement.get(), 4);
        record.state.attempts = static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 5));
        record.state.outputSummary = columnText(statement.get(), 6);
        {
            const auto parsed = nlohmann::json::parse(
                columnText(statement.get(), 7), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                    error("manifest_task.corrupt_inspected_paths",
                        "Stored inspected-path evidence is corrupt"));
            }
            for (const auto& item : parsed) {
                if (!item.is_string()) {
                    return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                        error("manifest_task.corrupt_inspected_paths",
                            "Stored inspected-path evidence contains an invalid item"));
                }
                record.state.inspectedRelativePaths.push_back(item.get<std::string>());
            }
        }
        record.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 8));
        results.push_back(std::move(record));
    }
    return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::ok(std::move(results));
}

namespace {

std::string serializeNoChangesCriterionEvidence(
    const std::vector<application::ManifestWorkerCriterionEvidence>& evidence)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : evidence) {
        array.push_back(nlohmann::json {
            {"requirementId", item.requirementId},
            {"inspectedPath", item.inspectedPath},
            {"observation", item.observation}});
    }
    return array.dump();
}

std::vector<application::ManifestWorkerCriterionEvidence> deserializeNoChangesCriterionEvidence(
    const std::string& json)
{
    std::vector<application::ManifestWorkerCriterionEvidence> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkerCriterionEvidence evidence;
        if (item.contains("requirementId") && item["requirementId"].is_string())
            evidence.requirementId = item["requirementId"].get<std::string>();
        if (item.contains("inspectedPath") && item["inspectedPath"].is_string())
            evidence.inspectedPath = item["inspectedPath"].get<std::string>();
        if (item.contains("observation") && item["observation"].is_string())
            evidence.observation = item["observation"].get<std::string>();
        result.push_back(std::move(evidence));
    }
    return result;
}

std::string serializeNoChangesInspectedPaths(const std::vector<std::string>& paths)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& path : paths) array.push_back(path);
    return array.dump();
}

std::vector<std::string> deserializeNoChangesInspectedPaths(const std::string& json)
{
    std::vector<std::string> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

} // namespace

common::Result<void> SqliteManifestTaskExecutionRepository::upsertNoChanges(
    const application::RunId& runId, const std::string& taskId,
    const application::ManifestWorkerNoChanges& evidence)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO manifest_task_no_changes "
        "(run_id,task_id,schema_version,reason,summary,criterion_evidence,"
        "inspected_relative_paths,updated_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, taskId),
        bindText(handle, statement.get(), 3, evidence.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(evidence.reason)),
        bindText(handle, statement.get(), 5, evidence.summary),
        bindText(handle, statement.get(), 6, serializeNoChangesCriterionEvidence(evidence.criterionEvidence)),
        bindText(handle, statement.get(), 7, serializeNoChangesInspectedPaths(evidence.inspectedRelativePaths)),
        bindInt64(handle, statement.get(), 8, toMilliseconds(std::chrono::system_clock::now()))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_task.write_failed", "Could not persist manifest task no-op evidence"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestTaskNoChangesRecord>>
SqliteManifestTaskExecutionRepository::listNoChangesForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT task_id,schema_version,reason,summary,criterion_evidence,"
        "inspected_relative_paths,updated_at_ms FROM manifest_task_no_changes WHERE run_id=? "
        "ORDER BY task_id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            *bound.error());
    }

    std::vector<application::ManifestTaskNoChangesRecord> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
                databaseError(handle, "manifest_task.read_failed",
                    "Could not read manifest task no-op evidence"));
        }
        application::ManifestTaskNoChangesRecord record;
        record.runId = runId;
        record.taskId = columnText(statement.get(), 0);
        record.evidence.schemaVersion = columnText(statement.get(), 1);
        const auto reason = application::manifestWorkerNoChangesReasonFromString(
            columnText(statement.get(), 2));
        if (!reason.has_value()) {
            return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
                error("manifest_task.corrupt_no_changes", "Unknown no-op reason in stored evidence"));
        }
        record.evidence.reason = *reason;
        record.evidence.summary = columnText(statement.get(), 3);
        record.evidence.criterionEvidence =
            deserializeNoChangesCriterionEvidence(columnText(statement.get(), 4));
        record.evidence.inspectedRelativePaths =
            deserializeNoChangesInspectedPaths(columnText(statement.get(), 5));
        record.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(record));
    }
    return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::ok(std::move(results));
}

namespace {

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

namespace {

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

SqliteRunWarningRepository::SqliteRunWarningRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<bool> SqliteRunWarningRepository::recordWarning(application::RunWarning warning)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Deliberately a plain INSERT: OR IGNORE would also swallow a CHECK violation (an empty or oversized message) and a
    // missing Run, and the caller has to hear about those.
    const auto prepared = prepare(handle,
        "INSERT INTO run_warnings (id,run_id,stage_id,code,message,artifact_name,created_at_ms) VALUES (?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<bool>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, warning.id),
        bindText(handle, statement.get(), 2, warning.runId),
        bindNullableText(handle, statement.get(), 3, warning.stageId),
        bindText(handle, statement.get(), 4, warning.code),
        bindText(handle, statement.get(), 5, warning.message),
        bindText(handle, statement.get(), 6, warning.artifactName),
        bindInt64(handle, statement.get(), 7, toMilliseconds(warning.createdAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<bool>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        // The identity index is the only UNIQUE constraint besides the id (a fresh UUID): the same failure again.
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<bool>::ok(false);
        }
        return common::Result<bool>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record run warning"));
    }
    return common::Result<bool>::ok(true);
}

common::Result<std::vector<application::RunWarning>> SqliteRunWarningRepository::listWarnings(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,run_id,stage_id,code,message,artifact_name,created_at_ms FROM run_warnings "
        "WHERE run_id=? ORDER BY created_at_ms,id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWarning>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<application::RunWarning>>::failure(*bound.error());

    std::vector<application::RunWarning> warnings;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWarning>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run warnings"));
        }
        application::RunWarning warning;
        warning.id = columnText(statement.get(), 0);
        warning.runId = columnText(statement.get(), 1);
        warning.stageId = optionalText(statement.get(), 2);
        warning.code = columnText(statement.get(), 3);
        warning.message = columnText(statement.get(), 4);
        warning.artifactName = columnText(statement.get(), 5);
        warning.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        warnings.push_back(std::move(warning));
    }
    return common::Result<std::vector<application::RunWarning>>::ok(std::move(warnings));
}

SqliteOutcomeBriefRepository::SqliteOutcomeBriefRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* outcomeBriefRevisionColumns =
    "id,run_id,project_id,ordinal,schema_version,policy_id,workflow_id,terminal_status,state,reasons_json,counts_json,"
    "member_decisions_json,recovery_eligible,requires_inspection,continuation_source_run_id,"
    "continuation_child_run_id,manual_handoff_id,source_state_json,source_state_fingerprint,rendered_text,"
    "content_fingerprint,created_at_ms";

// What EVERY read selects (so no read can skip the Run authority): the revision, then - from the Run it is about - the Run's
// current project and status, and whether this revision is the newest of its Run. The LEFT JOIN keeps a revision whose Run is
// gone in the result, so that it can be refused instead of disappearing. Columns 0..21 are outcomeBriefRevisionColumns.
constexpr const char* outcomeBriefSelect =
    "SELECT r.id,r.run_id,r.project_id,r.ordinal,r.schema_version,r.policy_id,r.workflow_id,r.terminal_status,r.state,"
    "r.reasons_json,r.counts_json,r.member_decisions_json,r.recovery_eligible,r.requires_inspection,"
    "r.continuation_source_run_id,r.continuation_child_run_id,r.manual_handoff_id,r.source_state_json,"
    "r.source_state_fingerprint,r.rendered_text,r.content_fingerprint,r.created_at_ms,"
    "runs.project_id,runs.status,"
    "r.ordinal = (SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = r.run_id) "
    "FROM outcome_brief_revisions AS r LEFT JOIN runs ON runs.id = r.run_id ";

using OutcomeBriefCountField = std::uint32_t application::OutcomeBriefCounts::*;
// One table for both directions, so what is written is exactly what is read back.
const std::pair<const char*, OutcomeBriefCountField> outcomeBriefCountFields[] = {
    {"members", &application::OutcomeBriefCounts::members},
    {"reviewable", &application::OutcomeBriefCounts::reviewable},
    {"applied", &application::OutcomeBriefCounts::applied},
    {"humanRejected", &application::OutcomeBriefCounts::humanRejected},
    {"approved", &application::OutcomeBriefCounts::approved},
    {"undecided", &application::OutcomeBriefCounts::undecided},
    {"applyFailed", &application::OutcomeBriefCounts::applyFailed},
    {"noChanges", &application::OutcomeBriefCounts::noChanges},
    {"validatorRejected", &application::OutcomeBriefCounts::validatorRejected},
    {"superseded", &application::OutcomeBriefCounts::superseded},
    {"unsettled", &application::OutcomeBriefCounts::unsettled},
    {"inconsistent", &application::OutcomeBriefCounts::inconsistent},
};

std::string outcomeBriefReasonsJson(const std::vector<application::OutcomeBriefReason>& reasons)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto reason : reasons) array.push_back(application::toString(reason));
    return array.dump();
}

std::string outcomeBriefCountsJson(const application::OutcomeBriefCounts& counts)
{
    nlohmann::json object = nlohmann::json::object();
    for (const auto& [name, field] : outcomeBriefCountFields) object[name] = counts.*field;
    return object.dump();
}

std::string outcomeBriefMemberDecisionsJson(const std::vector<application::OutcomeBriefMemberDecision>& members)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& member : members) {
        array.push_back({{"changeSetId", member.changeSetId}, {"decision", application::toString(member.decision)}});
    }
    return array.dump();
}

bool parseOutcomeBriefReasons(const std::string& text, std::vector<application::OutcomeBriefReason>& reasons)
{
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array()) return false;
    for (const auto& item : parsed) {
        if (!item.is_string()) return false;
        const auto reason = application::outcomeBriefReasonFromString(item.get_ref<const std::string&>());
        if (!reason.has_value()) return false;
        reasons.push_back(*reason);
    }
    return true;
}

bool parseOutcomeBriefCounts(const std::string& text, application::OutcomeBriefCounts& counts)
{
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_object() || parsed.size() != std::size(outcomeBriefCountFields)) return false;
    for (const auto& [name, field] : outcomeBriefCountFields) {
        const auto found = parsed.find(name);
        if (found == parsed.end() || !found->is_number_unsigned()) return false;
        const auto value = found->get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint32_t>::max()) return false;
        counts.*field = static_cast<std::uint32_t>(value);
    }
    return true;
}

bool parseOutcomeBriefMemberDecisions(const std::string& text, std::vector<application::OutcomeBriefMemberDecision>& members)
{
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array()) return false;
    for (const auto& item : parsed) {
        if (!item.is_object() || item.size() != 2) return false;
        const auto id = item.find("changeSetId");
        const auto decision = item.find("decision");
        if (id == item.end() || decision == item.end() || !id->is_string() || !decision->is_string()) return false;
        const auto parsedDecision = application::outcomeProposalDecisionFromString(decision->get_ref<const std::string&>());
        if (!parsedDecision.has_value()) return false;
        members.push_back({id->get<std::string>(), *parsedDecision});
    }
    return true;
}

// A stored row is read back like any consumer would: it must still be exactly what the resolver could have derived, and still
// agree with the Run it is about (project of every revision; terminal status of the current one when `headStatusAuthority`).
// A row that was damaged behind the repository's back, or went stale, is refused, never repaired and never shown.
common::Result<application::OutcomeBriefRevision> readOutcomeBriefRevision(sqlite3_stmt* statement, bool headStatusAuthority)
{
    using Result = common::Result<application::OutcomeBriefRevision>;
    const auto invalid = [](const std::string& what) {
        return Result::failure(error("persistence.invalid_data", "Outcome brief row " + what));
    };
    application::OutcomeBriefRevision revision;
    application::OutcomeBrief& brief = revision.brief;
    revision.id = columnText(statement, 0);
    brief.runId = columnText(statement, 1);
    brief.projectId = columnText(statement, 2);
    const std::int64_t ordinal = sqlite3_column_int64(statement, 3);
    if (ordinal < 1) return invalid("has an invalid ordinal");
    revision.ordinal = static_cast<std::uint64_t>(ordinal);
    brief.schemaVersion = columnText(statement, 4);
    brief.policyId = columnText(statement, 5);
    brief.workflowId = columnText(statement, 6);
    const auto status = runStatusFromString(columnText(statement, 7));
    if (!status.has_value()) return invalid("contains an unknown terminal status");
    brief.terminalStatus = *status;
    const auto state = application::outcomeBriefStateFromString(columnText(statement, 8));
    if (!state.has_value()) return invalid("contains an unknown state");
    brief.state = *state;
    if (!parseOutcomeBriefReasons(columnText(statement, 9), brief.reasons)) return invalid("has unreadable reasons");
    if (!parseOutcomeBriefCounts(columnText(statement, 10), brief.counts)) return invalid("has unreadable counts");
    if (!parseOutcomeBriefMemberDecisions(columnText(statement, 11), brief.memberDecisions)) {
        return invalid("has unreadable member decisions");
    }
    brief.recoveryEligible = sqlite3_column_int(statement, 12) != 0;
    brief.requiresInspection = sqlite3_column_int(statement, 13) != 0;
    brief.continuationSourceRunId = optionalText(statement, 14);
    brief.continuationChildRunId = optionalText(statement, 15);
    brief.manualHandoffId = optionalText(statement, 16);
    brief.sourceStateJson = columnText(statement, 17);
    brief.sourceStateFingerprint = columnText(statement, 18);
    brief.renderedText = columnText(statement, 19);
    brief.contentFingerprint = columnText(statement, 20);
    revision.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 21));

    const auto gate = application::validateOutcomeBrief(brief);
    if (!gate.success()) {
        return invalid("does not pass the Outcome Brief gate: " + gate.error()->message);
    }

    std::optional<application::OutcomeBriefRunOwner> owner;
    if (sqlite3_column_type(statement, 22) != SQLITE_NULL) {
        const auto ownerStatus = runStatusFromString(columnText(statement, 23));
        if (!ownerStatus.has_value()) return invalid("is about a Run with an unknown status");
        owner = application::OutcomeBriefRunOwner {columnText(statement, 22), *ownerStatus};
    }
    const bool isHead = sqlite3_column_int(statement, 24) != 0;
    const auto authority = application::checkOutcomeBriefReadAuthority(brief, owner, isHead && headStatusAuthority);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(std::move(revision));
}

common::Result<std::vector<application::OutcomeBriefRevision>> collectOutcomeBriefRevisions(
    sqlite3* handle, sqlite3_stmt* statement, bool headStatusAuthority = true)
{
    using Result = common::Result<std::vector<application::OutcomeBriefRevision>>;
    std::vector<application::OutcomeBriefRevision> revisions;
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read outcome briefs"));
        }
        auto revision = readOutcomeBriefRevision(statement, headStatusAuthority);
        if (!revision.success()) return Result::failure(*revision.error());
        revisions.push_back(std::move(revision.value()));
    }
    return Result::ok(std::move(revisions));
}

// The head of a Run is its revision with the highest ordinal. The outcome_brief_heads row is only a derived pointer to it
// (maintained by append and guarded by triggers), so a lost or stale pointer can never hide or invent a state. `append` reads it
// with `headStatusAuthority = false`: the head it compares with may have gone stale because the Run moved on, and the new brief
// that replaces it must not be blocked by that (a foreign project, a missing Run or a damaged row still refuse).
common::Result<std::optional<application::OutcomeBriefRevision>> readOutcomeBriefHead(
    sqlite3* handle, const application::RunId& runId, bool headStatusAuthority)
{
    using Result = common::Result<std::optional<application::OutcomeBriefRevision>>;
    Statement statement;
    const std::string sql = std::string(outcomeBriefSelect) + "WHERE r.run_id=? ORDER BY r.ordinal DESC LIMIT 1;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectOutcomeBriefRevisions(handle, statement.get(), headStatusAuthority);
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<void> bindOutcomeBriefRevision(
    sqlite3* handle, sqlite3_stmt* statement, const application::OutcomeBriefRevision& revision)
{
    const application::OutcomeBrief& brief = revision.brief;
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, revision.id),
        bindText(handle, statement, 2, brief.runId),
        bindText(handle, statement, 3, brief.projectId),
        bindUnsigned(handle, statement, 4, revision.ordinal),
        bindText(handle, statement, 5, brief.schemaVersion),
        bindText(handle, statement, 6, brief.policyId),
        bindText(handle, statement, 7, brief.workflowId),
        bindText(handle, statement, 8, application::toString(brief.terminalStatus)),
        bindText(handle, statement, 9, application::toString(brief.state)),
        bindText(handle, statement, 10, outcomeBriefReasonsJson(brief.reasons)),
        bindText(handle, statement, 11, outcomeBriefCountsJson(brief.counts)),
        bindText(handle, statement, 12, outcomeBriefMemberDecisionsJson(brief.memberDecisions)),
        bindInt64(handle, statement, 13, brief.recoveryEligible ? 1 : 0),
        bindInt64(handle, statement, 14, brief.requiresInspection ? 1 : 0),
        bindNullableText(handle, statement, 15, brief.continuationSourceRunId),
        bindNullableText(handle, statement, 16, brief.continuationChildRunId),
        bindNullableText(handle, statement, 17, brief.manualHandoffId),
        bindText(handle, statement, 18, brief.sourceStateJson),
        bindText(handle, statement, 19, brief.sourceStateFingerprint),
        bindText(handle, statement, 20, brief.renderedText),
        bindText(handle, statement, 21, brief.contentFingerprint),
        bindInt64(handle, statement, 22, toMilliseconds(revision.createdAt)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    return common::Result<void>::ok();
}

// Points the head row at the newest revision. A pointer that is already right is left alone (the DO UPDATE does not fire),
// one that is missing is created and one that is stale is moved: the same statement repairs a lost pointer.
common::Result<void> pointOutcomeBriefHead(
    sqlite3* handle, const application::RunId& runId, const std::string& revisionId,
    std::chrono::system_clock::time_point updatedAt)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO outcome_brief_heads (run_id,revision_id,updated_at_ms) VALUES (?,?,?) "
        "ON CONFLICT(run_id) DO UPDATE SET revision_id=excluded.revision_id, updated_at_ms=excluded.updated_at_ms "
        "WHERE outcome_brief_heads.revision_id <> excluded.revision_id;",
        statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, revisionId),
        bindInt64(handle, statement.get(), 3, toMilliseconds(updatedAt)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not move the outcome brief head"));
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<application::OutcomeBriefAppendResult> SqliteOutcomeBriefRepository::append(
    application::OutcomeBrief brief, application::OutcomeBriefExpectedHead expected)
{
    using Result = common::Result<application::OutcomeBriefAppendResult>;
    // Validated before the first statement; a refusal never opens a transaction.
    const auto checked = application::checkOutcomeBriefAppendRequest(brief, expected);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // BEGIN IMMEDIATE takes the write lock first, so a second writer (another thread or another connection) waits here
    // and then sees the head this transaction leaves behind: the compare-and-set below cannot be raced.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    {
        // The write authority (shared with the in-memory adapter): the Run this brief is about must exist, belong to the
        // brief's project and still be in the status the brief says.
        Statement statement;
        const auto prepared = prepare(handle, "SELECT project_id,status FROM runs WHERE id=?;", statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, statement.get(), 1, brief.runId);
        if (!bound.success()) return rollbackWith(*bound.error());
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_DONE && step != SQLITE_ROW) {
            return rollbackWith(databaseError(handle, "persistence.read_failed", "Could not read the Run"));
        }
        std::optional<application::OutcomeBriefRunOwner> owner;
        if (step == SQLITE_ROW) {
            const auto ownerStatus = runStatusFromString(columnText(statement.get(), 1));
            if (!ownerStatus.has_value()) {
                return rollbackWith(error("persistence.invalid_data", "Run row contains an unknown status"));
            }
            owner = application::OutcomeBriefRunOwner {columnText(statement.get(), 0), *ownerStatus};
        }
        const auto authority = application::checkOutcomeBriefWriteAuthority(brief, owner);
        if (!authority.success()) return rollbackWith(*authority.error());
    }

    auto head = readOutcomeBriefHead(handle, brief.runId, /*headStatusAuthority=*/false);
    if (!head.success()) return rollbackWith(*head.error());
    const auto plan = application::planOutcomeBriefAppend(brief, expected, head.value());
    if (!plan.success()) return rollbackWith(*plan.error());

    application::OutcomeBriefAppendResult result;
    if (!plan.value().appendRevision) {
        // Nothing to add. The pointer is still made right, so a lost one is repaired by the next ordinary reconcile.
        const auto pointed = pointOutcomeBriefHead(
            handle, head.value()->brief.runId, head.value()->id, head.value()->createdAt);
        if (!pointed.success()) return rollbackWith(*pointed.error());
        result.head = *head.value();
        result.appended = false;
    } else {
        auto revision = application::makeOutcomeBriefRevision(std::move(brief), plan.value().ordinal);
        {
            Statement statement;
            const std::string sql = std::string("INSERT INTO outcome_brief_revisions (") + outcomeBriefRevisionColumns
                + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            const auto prepared = prepare(handle, sql.c_str(), statement);
            if (!prepared.success()) return rollbackWith(*prepared.error());
            const auto bound = bindOutcomeBriefRevision(handle, statement.get(), revision);
            if (!bound.success()) return rollbackWith(*bound.error());
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                return rollbackWith(
                    databaseError(handle, "persistence.write_failed", "Could not insert the outcome brief revision"));
            }
        }
        const auto pointed = pointOutcomeBriefHead(handle, revision.brief.runId, revision.id, revision.createdAt);
        if (!pointed.success()) return rollbackWith(*pointed.error());
        result.head = std::move(revision);
        result.appended = true;
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return Result::ok(std::move(result));
}

common::Result<std::optional<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::findHead(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readOutcomeBriefHead(database_->impl_->handle, runId, /*headStatusAuthority=*/true);
}

common::Result<std::optional<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::findRevision(
    const application::OutcomeBriefRevisionId& id) const
{
    using Result = common::Result<std::optional<application::OutcomeBriefRevision>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string(outcomeBriefSelect) + "WHERE r.id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectOutcomeBriefRevisions(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<std::vector<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::listRevisions(
    const application::RunId& runId, std::size_t limit) const
{
    using Result = common::Result<std::vector<application::OutcomeBriefRevision>>;
    const auto checked = application::checkOutcomeBriefListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // The newest `limit` revisions, handed back oldest first.
    const std::string sql = std::string(outcomeBriefSelect) + "WHERE r.run_id=? ORDER BY r.ordinal DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindInt64(handle, statement.get(), 2, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    auto rows = collectOutcomeBriefRevisions(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    std::reverse(rows.value().begin(), rows.value().end());
    return Result::ok(std::move(rows.value()));
}

common::Result<std::vector<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::listHeads(
    const application::ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<application::OutcomeBriefRevision>>;
    const auto checked = application::checkOutcomeBriefListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Same definition of the head as readOutcomeBriefHead (the highest ordinal of the Run), and the same order as
    // application::outcomeBriefHeadComesBefore. A head belongs to the list of the project it NAMES and to the list of the project
    // its Run belongs to: when the two differ, both lists meet the row and both fail closed.
    const std::string sql = std::string(outcomeBriefSelect)
        + "WHERE (r.project_id=? OR runs.project_id=?) AND r.ordinal = "
          "(SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = r.run_id) "
          "ORDER BY r.created_at_ms DESC, r.run_id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, projectId),
        bindText(handle, statement.get(), 2, projectId),
        bindInt64(handle, statement.get(), 3, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    return collectOutcomeBriefRevisions(handle, statement.get());
}

common::Result<std::optional<application::OutcomeBriefHeadVersion>> SqliteOutcomeBriefRepository::findHeadVersionForAppend(
    const application::RunId& runId) const
{
    using Result = common::Result<std::optional<application::OutcomeBriefHeadVersion>>;
    std::lock_guard lock(database_->impl_->mutex);
    // The very read that `append` compares with (gate, Run and project checked, status NOT), reduced to which revision it is.
    const auto head = readOutcomeBriefHead(database_->impl_->handle, runId, /*headStatusAuthority=*/false);
    if (!head.success()) return Result::failure(*head.error());
    if (!head.value().has_value()) {
        // No head: the Run must still exist ("no head" must not hide "no Run").
        Statement statement;
        const auto prepared = prepare(database_->impl_->handle, "SELECT 1 FROM runs WHERE id=?;", statement);
        if (!prepared.success()) return Result::failure(*prepared.error());
        const auto bound = bindText(database_->impl_->handle, statement.get(), 1, runId);
        if (!bound.success()) return Result::failure(*bound.error());
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            return Result::failure(databaseError(database_->impl_->handle, "persistence.read_failed", "Could not read the Run"));
        }
        const auto exists = application::checkOutcomeBriefRunExists(step == SQLITE_ROW);
        if (!exists.success()) return Result::failure(*exists.error());
        return Result::ok(std::nullopt);
    }
    return Result::ok(application::OutcomeBriefHeadVersion {head.value()->id, head.value()->ordinal});
}

SqliteFollowUpPlanRepository::SqliteFollowUpPlanRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* followUpRequestColumns =
    "id,project_id,source_run_id,kind,goal,max_drafts,split_hint,outcome_source_state_fingerprint,snapshot_fingerprint,"
    "context_files_json,context_policy_id,context_fingerprint,provider_id,model_id,provider_kind,provider_base_url,"
    "provider_credential_reference,provider_identity_fingerprint,prompt_cache_mode,pricing_identity_json,"
    "pricing_identity_fingerprint,estimated_input_tokens,cacheable_input_tokens,effective_max_output_tokens,cap_micros,"
    "estimated_cost_micros,state,created_at_ms,quote_expires_at_ms,updated_at_ms,authorized_at_ms,claimed_at_ms,"
    "finished_at_ms,reply_text,plan_fingerprint,advisories_json,failure_code,failure_message,draft_group_id,accounting_json";

// What EVERY read selects (so no read can skip the Run authority): the request, then the project of the Run it is about. The
// LEFT JOIN keeps a request whose Run is gone in the result, so that it can be refused instead of disappearing. Columns 0..39
// are followUpRequestColumns.
constexpr const char* followUpRequestSelect =
    "SELECT r.id,r.project_id,r.source_run_id,r.kind,r.goal,r.max_drafts,r.split_hint,r.outcome_source_state_fingerprint,"
    "r.snapshot_fingerprint,r.context_files_json,r.context_policy_id,r.context_fingerprint,r.provider_id,r.model_id,"
    "r.provider_kind,r.provider_base_url,r.provider_credential_reference,r.provider_identity_fingerprint,"
    "r.prompt_cache_mode,r.pricing_identity_json,r.pricing_identity_fingerprint,r.estimated_input_tokens,"
    "r.cacheable_input_tokens,r.effective_max_output_tokens,r.cap_micros,r.estimated_cost_micros,r.state,r.created_at_ms,"
    "r.quote_expires_at_ms,r.updated_at_ms,r.authorized_at_ms,r.claimed_at_ms,r.finished_at_ms,r.reply_text,"
    "r.plan_fingerprint,r.advisories_json,r.failure_code,r.failure_message,r.draft_group_id,r.accounting_json,runs.project_id "
    "FROM follow_up_plan_requests AS r LEFT JOIN runs ON runs.id = r.source_run_id ";

constexpr const char* followUpOpenStates = "('quoted','authorized','composing','plan_ready')";

// A stored row is read back like any consumer would: it must still be exactly what the rules could have produced and still
// belong to the project of its source Run. A row that was damaged or made behind the repository's back is refused, never
// repaired and never shown.
common::Result<application::FollowUpPlanRequest> readFollowUpRequest(sqlite3_stmt* statement)
{
    using Result = common::Result<application::FollowUpPlanRequest>;
    const auto invalid = [](const std::string& what) {
        return Result::failure(error("persistence.invalid_data", "Follow-up request row " + what));
    };
    application::FollowUpPlanRequest request;
    request.id = columnText(statement, 0);
    request.projectId = columnText(statement, 1);
    request.sourceRunId = columnText(statement, 2);
    const auto kind = application::followUpPlanKindFromString(columnText(statement, 3));
    if (!kind.has_value()) return invalid("has an unknown kind");
    request.kind = *kind;
    request.goal = columnText(statement, 4);
    const std::int64_t maxDrafts = sqlite3_column_int64(statement, 5);
    if (maxDrafts < 1 || maxDrafts > 255) return invalid("has an invalid draft limit");
    request.maxDrafts = static_cast<std::uint32_t>(maxDrafts);
    const auto hint = application::followUpSplitHintFromString(columnText(statement, 6));
    if (!hint.has_value()) return invalid("has an unknown split hint");
    request.splitHint = *hint;
    request.outcomeSourceStateFingerprint = columnText(statement, 7);
    request.snapshotFingerprint = columnText(statement, 8);
    auto files = application::parseFollowUpPinnedFiles(columnText(statement, 9));
    if (!files.success()) return Result::failure(*files.error());
    request.files = std::move(files.value());

    auto& quote = request.quote;
    quote.policyId = columnText(statement, 10);
    quote.contextFingerprint = columnText(statement, 11);
    quote.providerIdentity.providerId = columnText(statement, 12);
    quote.providerIdentity.kind = columnText(statement, 14);
    quote.providerIdentity.baseUrl = columnText(statement, 15);
    quote.providerIdentity.credentialReference = optionalText(statement, 16);
    quote.providerIdentityFingerprint = columnText(statement, 17);
    const auto cacheMode = application::followUpPromptCacheModeFromString(columnText(statement, 18));
    if (!cacheMode.has_value()) return invalid("has an unknown cache mode");
    auto pricing = application::parseFollowUpPricingIdentity(columnText(statement, 19));
    if (!pricing.success()) return Result::failure(*pricing.error());
    if (pricing.value().promptCache.mode != *cacheMode || pricing.value().modelId != columnText(statement, 13)
            || pricing.value().providerId != quote.providerIdentity.providerId) {
        return invalid("has copies of its identity that disagree with the stored pricing identity");
    }
    quote.worstCase.pricingIdentity = std::move(pricing.value());
    quote.worstCase.pricingIdentityFingerprint = columnText(statement, 20);
    quote.worstCase.estimatedInputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 21));
    quote.worstCase.cacheableInputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 22));
    quote.worstCase.maximumOutputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 23));
    quote.capMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 24));
    quote.worstCase.estimatedCostMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 25));
    quote.worstCase.priceKnown = true;

    const auto state = application::followUpPlanRequestStateFromString(columnText(statement, 26));
    if (!state.has_value()) return invalid("has an unknown state");
    request.state = *state;
    request.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 27));
    request.quoteExpiresAt = fromMilliseconds(sqlite3_column_int64(statement, 28));
    request.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 29));
    request.authorizedAt = optionalTime(statement, 30);
    request.claimedAt = optionalTime(statement, 31);
    request.finishedAt = optionalTime(statement, 32);

    const auto reply = optionalText(statement, 33);
    const auto planFingerprint = optionalText(statement, 34);
    const auto advisories = optionalText(statement, 35);
    const auto failureCode = optionalText(statement, 36);
    const auto failureMessage = optionalText(statement, 37);
    if (planFingerprint.has_value()) {
        if (!reply.has_value() || !advisories.has_value()) return invalid("has a plan without its reply or advisories");
        request.answer = application::FollowUpPlanAnswer {*reply, *planFingerprint, *advisories};
    } else if (reply.has_value() && !failureCode.has_value()) {
        return invalid("has a reply that is neither a plan nor a failure");
    }
    if (failureCode.has_value()) {
        if (!failureMessage.has_value()) return invalid("has a failure code without a message");
        request.failure = application::FollowUpPlanFailure {*failureCode, *failureMessage, reply};
    }
    request.draftGroupId = optionalText(statement, 38);
    if (const auto accounting = optionalText(statement, 39); accounting.has_value()) {
        auto parsed = application::parseFollowUpAccounting(*accounting);
        if (!parsed.success()) return Result::failure(*parsed.error());
        request.accounting = std::move(parsed.value());
    }

    const auto gate = application::validateFollowUpPlanRequest(request);
    if (!gate.success()) return invalid("does not pass the follow-up request gate: " + gate.error()->message);

    std::optional<application::ProjectId> runProject;
    if (sqlite3_column_type(statement, 40) != SQLITE_NULL) runProject = columnText(statement, 40);
    const auto authority = application::checkFollowUpRequestReadAuthority(request, runProject);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(std::move(request));
}

common::Result<std::vector<application::FollowUpPlanRequest>> collectFollowUpRequests(
    sqlite3* handle, sqlite3_stmt* statement)
{
    using Result = common::Result<std::vector<application::FollowUpPlanRequest>>;
    std::vector<application::FollowUpPlanRequest> requests;
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read follow-up requests"));
        }
        auto request = readFollowUpRequest(statement);
        if (!request.success()) return Result::failure(*request.error());
        requests.push_back(std::move(request.value()));
    }
    return Result::ok(std::move(requests));
}

common::Result<std::optional<application::FollowUpPlanRequest>> loadFollowUpRequest(
    sqlite3* handle, const application::FollowUpPlanRequestId& id)
{
    using Result = common::Result<std::optional<application::FollowUpPlanRequest>>;
    Statement statement;
    const std::string sql = std::string(followUpRequestSelect) + "WHERE r.id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectFollowUpRequests(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<std::optional<application::FollowUpPlanRequest>> loadOpenFollowUpRequest(
    sqlite3* handle, const application::RunId& runId)
{
    using Result = common::Result<std::optional<application::FollowUpPlanRequest>>;
    Statement statement;
    const std::string sql = std::string(followUpRequestSelect) + "WHERE r.source_run_id=? AND r.state IN "
        + followUpOpenStates + ";";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectFollowUpRequests(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

// The Run's project, read inside the write transaction: the write authority (shared with the in-memory adapter).
common::Result<std::optional<application::ProjectId>> loadRunProject(sqlite3* handle, const application::RunId& runId)
{
    using Result = common::Result<std::optional<application::ProjectId>>;
    Statement statement;
    const auto prepared = prepare(handle, "SELECT project_id FROM runs WHERE id=?;", statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read the Run"));
    return Result::ok(std::optional<application::ProjectId>(columnText(statement.get(), 0)));
}

// The columns a transition may change, bound from `index` on: state, updated, authorized, claimed, finished, reply,
// plan fingerprint, advisories, failure code, failure message, group id, accounting.
common::Result<void> bindFollowUpMutableColumns(
    sqlite3* handle, sqlite3_stmt* statement, int index, const application::FollowUpPlanRequest& request)
{
    std::optional<std::string> reply;
    if (request.answer.has_value()) reply = request.answer->rawReply;
    else if (request.failure.has_value()) reply = request.failure->rawReply;
    const common::Result<void> bindings[] = {
        bindText(handle, statement, index, application::toString(request.state)),
        bindInt64(handle, statement, index + 1, toMilliseconds(request.updatedAt)),
        bindOptionalTime(handle, statement, index + 2, request.authorizedAt),
        bindOptionalTime(handle, statement, index + 3, request.claimedAt),
        bindOptionalTime(handle, statement, index + 4, request.finishedAt),
        bindNullableText(handle, statement, index + 5, reply),
        bindNullableText(handle, statement, index + 6,
            request.answer.has_value() ? std::optional<std::string>(request.answer->planFingerprint) : std::nullopt),
        bindNullableText(handle, statement, index + 7,
            request.answer.has_value() ? std::optional<std::string>(request.answer->advisoriesJson) : std::nullopt),
        bindNullableText(handle, statement, index + 8,
            request.failure.has_value() ? std::optional<std::string>(request.failure->code) : std::nullopt),
        bindNullableText(handle, statement, index + 9,
            request.failure.has_value() ? std::optional<std::string>(request.failure->message) : std::nullopt),
        bindNullableText(handle, statement, index + 10, request.draftGroupId),
        bindNullableText(handle, statement, index + 11,
            request.accounting.has_value() ? std::optional<std::string>(application::serializeFollowUpAccounting(*request.accounting))
                                           : std::nullopt),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    return common::Result<void>::ok();
}

common::Result<void> insertFollowUpRequestRow(sqlite3* handle, const application::FollowUpPlanRequest& request)
{
    Statement statement;
    const std::string sql = std::string("INSERT INTO follow_up_plan_requests (") + followUpRequestColumns
        + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return prepared;
    const auto& quote = request.quote;
    const auto& worst = quote.worstCase;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, request.id),
        bindText(handle, statement.get(), 2, request.projectId),
        bindText(handle, statement.get(), 3, request.sourceRunId),
        bindText(handle, statement.get(), 4, application::toString(request.kind)),
        bindText(handle, statement.get(), 5, request.goal),
        bindInt64(handle, statement.get(), 6, request.maxDrafts),
        bindText(handle, statement.get(), 7, application::toString(request.splitHint)),
        bindText(handle, statement.get(), 8, request.outcomeSourceStateFingerprint),
        bindText(handle, statement.get(), 9, request.snapshotFingerprint),
        bindText(handle, statement.get(), 10, application::serializeFollowUpPinnedFiles(request.files)),
        bindText(handle, statement.get(), 11, quote.policyId),
        bindText(handle, statement.get(), 12, quote.contextFingerprint),
        bindText(handle, statement.get(), 13, quote.providerIdentity.providerId),
        bindText(handle, statement.get(), 14, worst.pricingIdentity.modelId),
        bindText(handle, statement.get(), 15, quote.providerIdentity.kind),
        bindText(handle, statement.get(), 16, quote.providerIdentity.baseUrl),
        bindNullableText(handle, statement.get(), 17, quote.providerIdentity.credentialReference),
        bindText(handle, statement.get(), 18, quote.providerIdentityFingerprint),
        bindText(handle, statement.get(), 19, std::string(models::toString(worst.pricingIdentity.promptCache.mode))),
        bindText(handle, statement.get(), 20, application::serializeFollowUpPricingIdentity(worst.pricingIdentity)),
        bindText(handle, statement.get(), 21, worst.pricingIdentityFingerprint),
        bindUnsigned(handle, statement.get(), 22, worst.estimatedInputTokens),
        bindUnsigned(handle, statement.get(), 23, worst.cacheableInputTokens),
        bindUnsigned(handle, statement.get(), 24, worst.maximumOutputTokens),
        bindUnsigned(handle, statement.get(), 25, quote.capMicros),
        bindUnsigned(handle, statement.get(), 26, worst.estimatedCostMicros),
        bindText(handle, statement.get(), 27, application::toString(request.state)),
        bindInt64(handle, statement.get(), 28, toMilliseconds(request.createdAt)),
        bindInt64(handle, statement.get(), 29, toMilliseconds(request.quoteExpiresAt)),
        bindInt64(handle, statement.get(), 30, toMilliseconds(request.updatedAt)),
        bindOptionalTime(handle, statement.get(), 31, request.authorizedAt),
        bindOptionalTime(handle, statement.get(), 32, request.claimedAt),
        bindOptionalTime(handle, statement.get(), 33, request.finishedAt),
        bindNullableText(handle, statement.get(), 34,
            request.answer.has_value() ? std::optional<std::string>(request.answer->rawReply)
                : request.failure.has_value() ? request.failure->rawReply : std::nullopt),
        bindNullableText(handle, statement.get(), 35,
            request.answer.has_value() ? std::optional<std::string>(request.answer->planFingerprint) : std::nullopt),
        bindNullableText(handle, statement.get(), 36,
            request.answer.has_value() ? std::optional<std::string>(request.answer->advisoriesJson) : std::nullopt),
        bindNullableText(handle, statement.get(), 37,
            request.failure.has_value() ? std::optional<std::string>(request.failure->code) : std::nullopt),
        bindNullableText(handle, statement.get(), 38,
            request.failure.has_value() ? std::optional<std::string>(request.failure->message) : std::nullopt),
        bindNullableText(handle, statement.get(), 39, request.draftGroupId),
        bindNullableText(handle, statement.get(), 40,
            request.accounting.has_value() ? std::optional<std::string>(application::serializeFollowUpAccounting(*request.accounting))
                                           : std::nullopt),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("follow_up.duplicate_id", "A follow-up request with this id already exists"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert the follow-up request"));
    }
    return common::Result<void>::ok();
}

// The compare-and-set write of a computed next record: only while the stored state is still `expected`.
common::Result<void> updateFollowUpRequestRow(
    sqlite3* handle, const application::FollowUpPlanRequest& next, application::FollowUpPlanRequestState expected)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE follow_up_plan_requests SET state=?,updated_at_ms=?,authorized_at_ms=?,claimed_at_ms=?,finished_at_ms=?,"
        "reply_text=?,plan_fingerprint=?,advisories_json=?,failure_code=?,failure_message=?,draft_group_id=?,accounting_json=? "
        "WHERE id=? AND state=?;",
        statement);
    if (!prepared.success()) return prepared;
    const auto bound = bindFollowUpMutableColumns(handle, statement.get(), 1, next);
    if (!bound.success()) return bound;
    const auto idBound = bindText(handle, statement.get(), 13, next.id);
    if (!idBound.success()) return idBound;
    const auto stateBound = bindText(handle, statement.get(), 14, application::toString(expected));
    if (!stateBound.success()) return stateBound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update the follow-up request"));
    }
    if (sqlite3_changes(handle) != 1) {
        return common::Result<void>::failure(
            error("follow_up.state_changed", "The follow-up request is no longer in the state the step expected"));
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<application::FollowUpInsertResult> SqliteFollowUpPlanRepository::insertQuoted(
    application::FollowUpPlanRequest request)
{
    using Result = common::Result<application::FollowUpInsertResult>;
    // Validated before the first statement; a refusal never opens a transaction.
    const auto gate = application::validateFollowUpPlanRequest(request);
    if (!gate.success()) return Result::failure(*gate.error());
    if (request.state != application::FollowUpPlanRequestState::Quoted) {
        return Result::failure(error("follow_up.invalid_request", "A request is inserted as quoted"));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // BEGIN IMMEDIATE takes the write lock first, so a second writer waits here and sees what this transaction leaves behind.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    const auto runProject = loadRunProject(handle, request.sourceRunId);
    if (!runProject.success()) return rollbackWith(*runProject.error());
    const auto authority = application::checkFollowUpRequestWriteAuthority(request, runProject.value());
    if (!authority.success()) return rollbackWith(*authority.error());

    const auto open = loadOpenFollowUpRequest(handle, request.sourceRunId);
    if (!open.success()) return rollbackWith(*open.error());
    const auto replaced = application::planFollowUpQuotedInsert(request, open.value());
    if (!replaced.success()) return rollbackWith(*replaced.error());
    if (replaced.value().has_value()) {
        application::FollowUpTransition discard;
        discard.event = application::FollowUpPlanEvent::Discard;
        discard.expectedState = application::FollowUpPlanRequestState::Quoted;
        const auto next = application::applyFollowUpTransition(*open.value(), discard, request.createdAt);
        if (!next.success()) return rollbackWith(*next.error());
        const auto stored = updateFollowUpRequestRow(handle, next.value(), application::FollowUpPlanRequestState::Quoted);
        if (!stored.success()) return rollbackWith(*stored.error());
    }
    const auto inserted = insertFollowUpRequestRow(handle, request);
    if (!inserted.success()) return rollbackWith(*inserted.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    application::FollowUpInsertResult result;
    result.request = std::move(request);
    result.discardedQuoteId = replaced.value();
    return Result::ok(std::move(result));
}

common::Result<application::FollowUpPlanRequest> SqliteFollowUpPlanRepository::transition(
    const application::FollowUpPlanRequestId& id,
    application::FollowUpTransition transition,
    std::chrono::system_clock::time_point now)
{
    using Result = common::Result<application::FollowUpPlanRequest>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    const auto current = loadFollowUpRequest(handle, id);
    if (!current.success()) return rollbackWith(*current.error());
    if (!current.value().has_value()) return rollbackWith(error("follow_up.not_found", "No such follow-up request"));
    auto next = application::applyFollowUpTransition(*current.value(), transition, now);
    if (!next.success()) return rollbackWith(*next.error());
    const auto stored = updateFollowUpRequestRow(handle, next.value(), transition.expectedState);
    if (!stored.success()) return rollbackWith(*stored.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return Result::ok(std::move(next.value()));
}

common::Result<std::optional<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::findById(
    const application::FollowUpPlanRequestId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return loadFollowUpRequest(database_->impl_->handle, id);
}

common::Result<std::optional<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::findOpenForSourceRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return loadOpenFollowUpRequest(database_->impl_->handle, runId);
}

common::Result<std::optional<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::findLatestForSourceRun(
    const application::RunId& runId) const
{
    using Result = common::Result<std::optional<application::FollowUpPlanRequest>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string(followUpRequestSelect)
        + "WHERE r.source_run_id=? ORDER BY r.created_at_ms DESC, r.id DESC LIMIT 1;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectFollowUpRequests(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<std::vector<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::listForProject(
    const application::ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<application::FollowUpPlanRequest>>;
    const auto checked = application::checkFollowUpListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // A request belongs to the list of the project it NAMES and to the list of the project its Run belongs to: when the two
    // differ, both lists meet the row and both fail closed.
    const std::string sql = std::string(followUpRequestSelect)
        + "WHERE (r.project_id=? OR runs.project_id=?) ORDER BY r.created_at_ms DESC, r.id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, projectId),
        bindText(handle, statement.get(), 2, projectId),
        bindInt64(handle, statement.get(), 3, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    return collectFollowUpRequests(handle, statement.get());
}

common::Result<std::vector<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::listByState(
    application::FollowUpPlanRequestState state, std::size_t limit) const
{
    return listByStateAfter(state, limit, std::nullopt);
}

common::Result<std::vector<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::listByStateAfter(
    application::FollowUpPlanRequestState state, std::size_t limit,
    const std::optional<application::FollowUpStateCursor>& after) const
{
    using Result = common::Result<std::vector<application::FollowUpPlanRequest>>;
    const auto checked = application::checkFollowUpListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Strictly after the cursor in (created_at_ms, id) order; without a cursor the bound is below every row.
    const std::string sql = std::string(followUpRequestSelect)
        + "WHERE r.state=? AND (r.created_at_ms > ? OR (r.created_at_ms = ? AND r.id > ?)) "
          "ORDER BY r.created_at_ms ASC, r.id ASC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const std::int64_t afterMs = after.has_value() ? toMilliseconds(after->createdAt) : std::numeric_limits<std::int64_t>::min();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, application::toString(state)),
        bindInt64(handle, statement.get(), 2, afterMs),
        bindInt64(handle, statement.get(), 3, afterMs),
        bindText(handle, statement.get(), 4, after.has_value() ? after->id : std::string()),
        bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    return collectFollowUpRequests(handle, statement.get());
}

common::Result<application::FollowUpConfirmResult> SqliteFollowUpPlanRepository::confirmDrafts(
    const application::FollowUpPlanRequestId& requestId,
    const std::string& expectedPlanFingerprint,
    application::RunDraftGroup group,
    std::vector<RunDraft> drafts,
    std::chrono::system_clock::time_point now)
{
    using Result = common::Result<application::FollowUpConfirmResult>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    const auto loaded = loadFollowUpRequest(handle, requestId);
    if (!loaded.success()) return rollbackWith(*loaded.error());
    if (!loaded.value().has_value()) return rollbackWith(error("follow_up.not_found", "No such follow-up request"));
    const auto& request = *loaded.value();
    const auto action = application::planFollowUpConfirm(request, expectedPlanFingerprint, group, drafts);
    if (!action.success()) return rollbackWith(*action.error());

    if (action.value() == application::FollowUpConfirmAction::ReturnExisting) {
        const auto storedGroup = findRunDraftGroupWhere(handle, "id=?", *request.draftGroupId);
        if (!storedGroup.success()) return rollbackWith(*storedGroup.error());
        if (!storedGroup.value().has_value()
                || storedGroup.value()->sourceRequestId != std::optional<std::string>(requestId)) {
            return rollbackWith(error("follow_up.drafts_removed",
                "The drafts of this request were removed after they were saved"));
        }
        auto stored = readRunDraftGroupDrafts(handle, storedGroup.value()->id);
        if (!stored.success()) return rollbackWith(*stored.error());
        application::FollowUpConfirmResult result;
        result.request = request;
        result.group = std::move(*storedGroup.value());
        result.drafts = std::move(stored.value());
        result.created = false;
        // Nothing was written: the transaction only read.
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::ok(std::move(result));
    }

    auto next = application::applyFollowUpConfirm(request, group.id, now);
    if (!next.success()) return rollbackWith(*next.error());
    const auto valid = application::validateRunDraftGroupInsertion(group, drafts);
    if (!valid.success()) return rollbackWith(*valid.error());
    // Every Run the group names is read INSIDE this transaction: it exists and belongs to the request's project.
    const auto sources = application::checkFollowUpGroupSourceRuns(group, request.projectId,
        [&](const application::RunId& runId) { return loadRunProject(handle, runId); });
    if (!sources.success()) return rollbackWith(*sources.error());
    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        return left.provenance->ordinal < right.provenance->ordinal;
    });
    // The group first (its trigger requires the request to be plan_ready), then the request (its trigger requires the group).
    const auto written = insertRunDraftGroupRows(handle, group, drafts);
    if (!written.success()) return rollbackWith(*written.error());
    const auto stored = updateFollowUpRequestRow(handle, next.value(), application::FollowUpPlanRequestState::PlanReady);
    if (!stored.success()) return rollbackWith(*stored.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    application::FollowUpConfirmResult result;
    result.request = std::move(next.value());
    result.group = std::move(group);
    result.drafts = std::move(drafts);
    result.created = true;
    return Result::ok(std::move(result));
}

SqliteModelExecutionRepository::SqliteModelExecutionRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* modelExecutionColumns =
    "id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,"
    "request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,"
    "cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,"
    "cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,"
    "cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,"
    "cache_creation_one_hour_input_tokens";

common::Result<ModelExecution> readModelExecution(sqlite3_stmt* statement)
{
    const auto source = application::modelExecutionSourceFromString(columnText(statement, 1));
    const auto status = application::modelExecutionStatusFromString(columnText(statement, 9));
    if (!source || !status)
        return common::Result<ModelExecution>::failure(error("persistence.invalid_data", "Model execution contains an unknown enum value"));
    ModelExecution value;
    value.id = columnText(statement, 0);
    value.source = *source;
    value.sourceId = columnText(statement, 2);
    value.projectId = optionalText(statement, 3);
    value.providerId = columnText(statement, 4);
    value.modelId = columnText(statement, 5);
    value.role = columnText(statement, 6);
    value.position = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 7));
    const auto attempt = sqlite3_column_int64(statement, 8);
    if (attempt <= 0) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution attempt must be positive"));
    }
    value.attempt = static_cast<std::uint32_t>(attempt);
    value.status = *status;
    value.requestHash = columnText(statement, 10);
    value.contextHash = columnText(statement, 11);
    value.outputArtifactId = optionalText(statement, 12);
    value.outputContent = columnText(statement, 13);
    value.outputPreview = columnText(statement, 14);
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 15));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 16));
    value.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 17));
    value.latencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 18));
    value.errorCode = columnText(statement, 19);
    value.errorMessage = columnText(statement, 20);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 21));
    value.startedAt = optionalTime(statement, 22);
    value.finishedAt = optionalTime(statement, 23);
    value.costPriceKnown = sqlite3_column_int(statement, 24) != 0;
    const std::int64_t cacheCreation = sqlite3_column_int64(statement, 25);
    const std::int64_t cacheRead = sqlite3_column_int64(statement, 26);
    const std::int64_t durationKnown = sqlite3_column_int64(statement, 27);
    const std::int64_t fiveMinute = sqlite3_column_int64(statement, 28);
    const std::int64_t oneHour = sqlite3_column_int64(statement, 29);
    if (cacheCreation < 0 || cacheRead < 0 || fiveMinute < 0 || oneHour < 0
        || (durationKnown != 0 && durationKnown != 1)) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution contains invalid cache usage"));
    }
    value.cacheCreationInputTokens = static_cast<std::uint64_t>(cacheCreation);
    value.cacheReadInputTokens = static_cast<std::uint64_t>(cacheRead);
    value.cacheCreationDuration.known = durationKnown != 0;
    value.cacheCreationDuration.fiveMinuteTokens = static_cast<std::uint64_t>(fiveMinute);
    value.cacheCreationDuration.oneHourTokens = static_cast<std::uint64_t>(oneHour);
    if (checkCacheCreationDurationUsage(
            value.cacheCreationInputTokens,
            value.cacheCreationDuration)
            != CacheCreationDurationUsageProblem::None
        || value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution contains inconsistent cache usage"));
    }
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<void> bindModelExecutionFields(sqlite3* handle, sqlite3_stmt* statement, const ModelExecution& value, int first)
{
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<void>::failure(error(
            "model_execution.invalid_cache_usage",
            "Cache-creation and cache-read tokens must fit within input tokens"));
    }
    const common::Result<void> bindings[] = {
        bindText(handle, statement, first, std::string(application::toString(value.source))),
        bindText(handle, statement, first + 1, value.sourceId),
        bindNullableText(handle, statement, first + 2, value.projectId),
        bindText(handle, statement, first + 3, value.providerId),
        bindText(handle, statement, first + 4, value.modelId),
        bindText(handle, statement, first + 5, value.role),
        bindUnsigned(handle, statement, first + 6, value.position),
        bindUnsigned(handle, statement, first + 7, value.attempt),
        bindText(handle, statement, first + 8, std::string(application::toString(value.status))),
        bindText(handle, statement, first + 9, value.requestHash),
        bindText(handle, statement, first + 10, value.contextHash),
        bindNullableText(handle, statement, first + 11, value.outputArtifactId),
        bindText(handle, statement, first + 12, value.outputContent),
        bindText(handle, statement, first + 13, value.outputPreview),
        bindUnsigned(handle, statement, first + 14, value.inputTokens),
        bindUnsigned(handle, statement, first + 15, value.outputTokens),
        bindUnsigned(handle, statement, first + 16, value.costMicros),
        bindUnsigned(handle, statement, first + 17, value.latencyMs),
        bindText(handle, statement, first + 18, value.errorCode),
        bindText(handle, statement, first + 19, value.errorMessage),
        bindInt64(handle, statement, first + 20, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement, first + 21, value.startedAt),
        bindOptionalTime(handle, statement, first + 22, value.finishedAt),
        bindInt64(handle, statement, first + 23, value.costPriceKnown ? 1 : 0),
        bindUnsigned(handle, statement, first + 24, value.cacheCreationInputTokens),
        bindUnsigned(handle, statement, first + 25, value.cacheReadInputTokens),
        bindInt64(handle, statement, first + 26, value.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement, first + 27,
            value.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement, first + 28,
            value.cacheCreationDuration.oneHourTokens)};
    for (const auto& binding : bindings) if (!binding.success()) return binding;
    return common::Result<void>::ok();
}

common::Result<QualityEvaluation> readQualityEvaluation(sqlite3_stmt* statement)
{
    const auto kind = application::qualityEvaluatorKindFromString(columnText(statement, 2));
    if (!kind) return common::Result<QualityEvaluation>::failure(error("persistence.invalid_data", "Quality evaluation contains an unknown evaluator kind"));
    QualityEvaluation value;
    value.id = columnText(statement, 0);
    value.executionId = columnText(statement, 1);
    value.evaluatorKind = *kind;
    value.evaluatorProviderId = optionalText(statement, 3);
    value.evaluatorModelId = optionalText(statement, 4);
    value.rubricVersion = columnText(statement, 5);
    value.scoreBasisPoints = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 6));
    value.criteriaJson = columnText(statement, 7);
    value.explanation = columnText(statement, 8);
    value.evidence = columnText(statement, 9);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

} // namespace

common::Result<ModelExecution> SqliteModelExecutionRepository::insertExecution(ModelExecution value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "INSERT INTO model_executions (id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<ModelExecution>::failure(*prepared.error());
    auto idBound = bindText(handle, statement.get(), 1, value.id);
    if (!idBound.success()) return common::Result<ModelExecution>::failure(*idBound.error());
    auto fieldsBound = bindModelExecutionFields(handle, statement.get(), value, 2);
    if (!fieldsBound.success()) return common::Result<ModelExecution>::failure(*fieldsBound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        return common::Result<ModelExecution>::failure(databaseError(handle, "persistence.write_failed", "Could not insert model execution"));
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<ModelExecution> SqliteModelExecutionRepository::updateExecution(ModelExecution value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "UPDATE model_executions SET source_type=?,source_id=?,project_id=?,provider_id=?,model_id=?,role=?,position=?,attempt=?,status=?,request_hash=?,context_hash=?,output_artifact_id=?,output_content=?,output_preview=?,input_tokens=?,output_tokens=?,cost_micros=?,latency_ms=?,error_code=?,error_message=?,created_at_ms=?,started_at_ms=?,finished_at_ms=?,cost_price_known=?,cache_creation_input_tokens=?,cache_read_input_tokens=?,cache_creation_duration_partition_known=?,cache_creation_five_minute_input_tokens=?,cache_creation_one_hour_input_tokens=? WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<ModelExecution>::failure(*prepared.error());
    auto fieldsBound = bindModelExecutionFields(handle, statement.get(), value, 1);
    if (!fieldsBound.success()) return common::Result<ModelExecution>::failure(*fieldsBound.error());
    auto idBound = bindText(handle, statement.get(), 30, value.id);
    if (!idBound.success()) return common::Result<ModelExecution>::failure(*idBound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        return common::Result<ModelExecution>::failure(databaseError(handle, "persistence.write_failed", "Could not update model execution"));
    if (sqlite3_changes(handle) == 0)
        return common::Result<ModelExecution>::failure(error("model_execution.not_found", "Execution not found"));
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<std::optional<ModelExecution>> SqliteModelExecutionRepository::findExecution(const application::ModelExecutionId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE id=?;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<ModelExecution>>::failure(*prepared.error());
    auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ModelExecution>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ModelExecution>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not read model execution"));
    auto value = readModelExecution(statement.get());
    if (!value.success()) return common::Result<std::optional<ModelExecution>>::failure(*value.error());
    return common::Result<std::optional<ModelExecution>>::ok(value.value());
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listExecutions(application::ModelExecutionSource source, const std::string& sourceId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE source_type=? AND source_id=? ORDER BY position,created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    const common::Result<void> bindings[] = {bindText(handle, statement.get(), 1, std::string(application::toString(source))), bindText(handle, statement.get(), 2, sourceId)};
    for (const auto& binding : bindings) if (!binding.success()) return common::Result<std::vector<ModelExecution>>::failure(*binding.error());
    std::vector<ModelExecution> values;
    for (;;) { const int step = sqlite3_step(statement.get()); if (step == SQLITE_DONE) break; if (step != SQLITE_ROW) return common::Result<std::vector<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not list model executions")); auto value = readModelExecution(statement.get()); if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error()); values.push_back(std::move(value.value())); }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listProjectExecutions(const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE project_id=? ORDER BY created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    auto bound = bindText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return common::Result<std::vector<ModelExecution>>::failure(*bound.error());
    std::vector<ModelExecution> values;
    for (;;) { const int step = sqlite3_step(statement.get()); if (step == SQLITE_DONE) break; if (step != SQLITE_ROW) return common::Result<std::vector<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not list project executions")); auto value = readModelExecution(statement.get()); if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error()); values.push_back(std::move(value.value())); }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listUnfinishedExecutions() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns
        + " FROM model_executions WHERE status IN ('pending','running') ORDER BY created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    std::vector<ModelExecution> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ModelExecution>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list unfinished model executions"));
        auto value = readModelExecution(statement.get());
        if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<QualityEvaluation> SqliteModelExecutionRepository::insertEvaluation(QualityEvaluation value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    auto prepared = prepare(handle, "INSERT INTO quality_evaluations (id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<QualityEvaluation>::failure(*prepared.error());
    const common::Result<void> bindings[] = {bindText(handle,statement.get(),1,value.id),bindText(handle,statement.get(),2,value.executionId),bindText(handle,statement.get(),3,std::string(application::toString(value.evaluatorKind))),bindNullableText(handle,statement.get(),4,value.evaluatorProviderId),bindNullableText(handle,statement.get(),5,value.evaluatorModelId),bindText(handle,statement.get(),6,value.rubricVersion),bindUnsigned(handle,statement.get(),7,value.scoreBasisPoints),bindText(handle,statement.get(),8,value.criteriaJson),bindText(handle,statement.get(),9,value.explanation),bindText(handle,statement.get(),10,value.evidence),bindInt64(handle,statement.get(),11,toMilliseconds(value.createdAt))};
    for (const auto& binding : bindings) if (!binding.success()) return common::Result<QualityEvaluation>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return common::Result<QualityEvaluation>::failure(databaseError(handle,"persistence.write_failed","Could not insert quality evaluation"));
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

common::Result<std::optional<QualityEvaluation>> SqliteModelExecutionRepository::findEvaluation(const application::QualityEvaluationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* handle=database_->impl_->handle; Statement statement;
    auto prepared=prepare(handle,"SELECT id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms FROM quality_evaluations WHERE id=?;",statement); if(!prepared.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*prepared.error()); auto bound=bindText(handle,statement.get(),1,id); if(!bound.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*bound.error()); const int step=sqlite3_step(statement.get()); if(step==SQLITE_DONE) return common::Result<std::optional<QualityEvaluation>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<QualityEvaluation>>::failure(databaseError(handle,"persistence.read_failed","Could not read quality evaluation")); auto value=readQualityEvaluation(statement.get()); if(!value.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*value.error()); return common::Result<std::optional<QualityEvaluation>>::ok(value.value());
}

common::Result<std::vector<QualityEvaluation>> SqliteModelExecutionRepository::listEvaluations(const application::ModelExecutionId& executionId) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* handle=database_->impl_->handle; Statement statement;
    // ORDER BY created_at_ms,rowid - not ,id. `id` is a random UUID
    // (generateUuid(), IdGenerator.cpp), which is not time-ordered; when two
    // evaluations for the same execution are inserted within the same
    // millisecond (common - two evaluations recorded back-to-back have no
    // reason to straddle a millisecond boundary), sorting by created_at_ms
    // then the random id ties on the first key and then orders by the
    // second key essentially randomly, silently reordering the two rows.
    // rowid is SQLite's own implicit, strictly-insertion-order column for
    // this ordinary (non-WITHOUT-ROWID) table - it is what "created_at_ms"
    // was actually trying to express as a tiebreaker, and a caller
    // reasonably expects listEvaluations() to come back in the order the
    // evaluations were actually added.
    auto prepared=prepare(handle,"SELECT id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms FROM quality_evaluations WHERE execution_id=? ORDER BY created_at_ms,rowid;",statement); if(!prepared.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*prepared.error()); auto bound=bindText(handle,statement.get(),1,executionId); if(!bound.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*bound.error()); std::vector<QualityEvaluation> values; for(;;){const int step=sqlite3_step(statement.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<QualityEvaluation>>::failure(databaseError(handle,"persistence.read_failed","Could not list quality evaluations")); auto value=readQualityEvaluation(statement.get()); if(!value.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*value.error()); values.push_back(std::move(value.value()));} return common::Result<std::vector<QualityEvaluation>>::ok(std::move(values));
}


// ============================================================
// SqliteChangeProposalRepository
// ============================================================

namespace {

// Read a ProposedChangeSet header row from a prepared statement.
// Column order must match changeSetColumns below.
ProposedChangeSet readChangeSet(sqlite3_stmt* stmt)
{
    ProposedChangeSet cs;
    cs.changeSetId           = columnText(stmt, 0);
    cs.runId                 = columnText(stmt, 1);
    cs.projectId             = columnText(stmt, 2);
    cs.providerId            = columnText(stmt, 3);
    cs.modelId               = columnText(stmt, 4);
    const std::string mexId  = columnText(stmt, 5);
    if (!mexId.empty()) cs.modelExecutionId = mexId;
    cs.agentRole             = columnText(stmt, 6);
    cs.summary               = columnText(stmt, 7);
    cs.baseRevision          = columnText(stmt, 8);
    cs.policySnapshotHash    = columnText(stmt, 9);
    cs.artifactRelativeRoot  = columnText(stmt, 10);
    cs.state                 = application::changeSetStateFromString(columnText(stmt, 11));
    cs.schemaVersion         = columnText(stmt, 12);
    cs.createdAt             = fromMilliseconds(sqlite3_column_int64(stmt, 13));
    cs.missingEvidenceCode    = optionalText(stmt, 14);
    cs.missingEvidenceMessage = optionalText(stmt, 15);
    cs.supersedesChangeSetId  = optionalText(stmt, 16);
    return cs;
}

constexpr const char* changeSetColumns =
    "id,run_id,project_id,provider_id,model_id,model_execution_id,"
    "agent_role,summary,base_revision,policy_snapshot_hash,"
    "artifact_relative_root,state,schema_version,created_at_ms,"
    "missing_evidence_code,missing_evidence_message,supersedes_change_set_id";

class ScopedTransactionRollback {
public:
    explicit ScopedTransactionRollback(sqlite3* handle)
        : handle_(handle)
    {
    }

    ~ScopedTransactionRollback()
    {
        if (active_) {
            static_cast<void>(
                execute(handle_, "ROLLBACK;", "persistence.rollback_failed"));
        }
    }

    void release() noexcept { active_ = false; }

private:
    sqlite3* handle_;
    bool active_ {true};
};

} // namespace

SqliteChangeProposalRepository::SqliteChangeProposalRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteChangeProposalRepository::insertChangeSet(
    const ProposedChangeSet& cs)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const auto began =
        execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) {
        return common::Result<void>::failure(*began.error());
    }
    ScopedTransactionRollback rollback {handle};

    // Insert the change-set header row.
    {
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposed_change_sets "
            "(id,run_id,project_id,provider_id,model_id,model_execution_id,"
            "agent_role,summary,base_revision,policy_snapshot_hash,"
            "artifact_relative_root,state,schema_version,created_at_ms,"
            "missing_evidence_code,missing_evidence_message,supersedes_change_set_id) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const std::string mexId = cs.modelExecutionId.has_value() ? *cs.modelExecutionId : "";
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, cs.changeSetId),
            bindText(handle, stmt.get(), 2, cs.runId),
            bindText(handle, stmt.get(), 3, cs.projectId),
            bindText(handle, stmt.get(), 4, cs.providerId),
            bindText(handle, stmt.get(), 5, cs.modelId),
            bindText(handle, stmt.get(), 6, mexId),
            bindText(handle, stmt.get(), 7, cs.agentRole),
            bindText(handle, stmt.get(), 8, cs.summary),
            bindText(handle, stmt.get(), 9, cs.baseRevision),
            bindText(handle, stmt.get(), 10, cs.policySnapshotHash),
            bindText(handle, stmt.get(), 11, cs.artifactRelativeRoot),
            bindText(handle, stmt.get(), 12, application::toString(cs.state)),
            bindText(handle, stmt.get(), 13, cs.schemaVersion),
            bindInt64(handle, stmt.get(), 14, toMilliseconds(cs.createdAt)),
            bindNullableText(handle, stmt.get(), 15, cs.missingEvidenceCode),
            bindNullableText(handle, stmt.get(), 16, cs.missingEvidenceMessage),
            bindNullableText(handle, stmt.get(), 17, cs.supersedesChangeSetId)};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(handle);
            if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return common::Result<void>::failure(
                    error("proposal.duplicate_id", "Change set id already exists"));
            }
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert change set"));
        }
    }

    // Insert per-change rows.
    for (std::size_t i = 0; i < cs.changes.size(); ++i) {
        const auto& ch = cs.changes[i];
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposed_file_changes "
            "(id,change_set_id,change_id,operation,relative_path,source_path,expected_original_hash,ordinal) "
            "VALUES (?,?,?,?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const std::string srcPath = ch.sourcePath.has_value()
                                      ? pathToUtf8(*ch.sourcePath) : "";
        const std::string expHash = ch.expectedOriginalHash.value_or("");
        // Use changeSetId + ordinal as a globally-unique primary key.
        const std::string rowId = cs.changeSetId + "_" + std::to_string(i);
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, rowId),
            bindText(handle, stmt.get(), 2, cs.changeSetId),
            bindText(handle, stmt.get(), 3, ch.changeId),
            bindText(handle, stmt.get(), 4, application::toString(ch.operation)),
            bindText(handle, stmt.get(), 5, pathToUtf8(ch.relativePath)),
            bindText(handle, stmt.get(), 6, srcPath),
            bindText(handle, stmt.get(), 7, expHash),
            bindInt64(handle, stmt.get(), 8, static_cast<std::int64_t>(i))};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert file change"));
        }
    }

    // Insert validation findings.
    for (std::size_t i = 0; i < cs.validationFindings.size(); ++i) {
        const auto& f = cs.validationFindings[i];
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposal_validation_findings "
            "(change_set_id,code,message,change_id,ordinal) VALUES (?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, cs.changeSetId),
            bindText(handle, stmt.get(), 2, f.code),
            bindText(handle, stmt.get(), 3, f.message),
            bindText(handle, stmt.get(), 4, f.changeId),
            bindInt64(handle, stmt.get(), 5, static_cast<std::int64_t>(i))};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert validation finding"));
        }
    }

    const auto committed =
        execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) {
        return common::Result<void>::failure(*committed.error());
    }
    rollback.release();
    return common::Result<void>::ok();
}

common::Result<ProposedChangeSet> SqliteChangeProposalRepository::updateChangeSetState(
    const ChangeSetId& id, ChangeSetState newState)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    {
        Statement stmt;
        const auto prep = prepare(handle,
            "UPDATE proposed_change_sets SET state=? WHERE id=?;", stmt);
        if (!prep.success()) return common::Result<ProposedChangeSet>::failure(*prep.error());
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, application::toString(newState)),
            bindText(handle, stmt.get(), 2, id)};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<ProposedChangeSet>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<ProposedChangeSet>::failure(
                databaseError(handle, "persistence.write_failed", "Could not update change-set state"));
        }
        if (sqlite3_changes(handle) == 0) {
            return common::Result<ProposedChangeSet>::failure(
                error("proposal.not_found", "Change set not found: " + id));
        }
    }

    // Re-read the updated row.
    const std::string sql = std::string("SELECT ") + changeSetColumns
                          + " FROM proposed_change_sets WHERE id=?;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) return common::Result<ProposedChangeSet>::failure(*prep.error());
    const auto bound = bindText(handle, stmt.get(), 1, id);
    if (!bound.success()) return common::Result<ProposedChangeSet>::failure(*bound.error());
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return common::Result<ProposedChangeSet>::failure(
            error("proposal.not_found", "Change set disappeared after update: " + id));
    }
    return common::Result<ProposedChangeSet>::ok(readChangeSet(stmt.get()));
}

common::Result<void> SqliteChangeProposalRepository::markMissingEvidence(
    const ChangeSetId& id, std::string code, std::string message)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "UPDATE proposed_change_sets SET missing_evidence_code=?,"
        "missing_evidence_message=? WHERE id=?;",
        stmt);
    if (!prep.success()) return common::Result<void>::failure(*prep.error());
    const common::Result<void> bindings[] = {
        bindText(handle, stmt.get(), 1, code),
        bindText(handle, stmt.get(), 2, message),
        bindText(handle, stmt.get(), 3, id)};
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<void>::failure(*b.error());
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record missing-evidence reason"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(
            error("proposal.not_found", "Change set not found: " + id));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposedChangeSet>> SqliteChangeProposalRepository::findChangeSet(
    const ChangeSetId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
                          + " FROM proposed_change_sets WHERE id=?;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, id);
    if (!bound.success()) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(*bound.error());
    }
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read change set"));
    }
    return common::Result<std::optional<ProposedChangeSet>>::ok(readChangeSet(stmt.get()));
}

common::Result<std::vector<ProposedChangeSet>> SqliteChangeProposalRepository::listForRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
        + " FROM proposed_change_sets WHERE run_id=? ORDER BY created_at_ms ASC;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*bound.error());
    }
    std::vector<ProposedChangeSet> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readChangeSet(stmt.get()));
    }
    return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
}

common::Result<std::vector<ProposedChangeSet>> SqliteChangeProposalRepository::listForProject(
    const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
        + " FROM proposed_change_sets WHERE project_id=? ORDER BY created_at_ms ASC;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, projectId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*bound.error());
    }
    std::vector<ProposedChangeSet> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readChangeSet(stmt.get()));
    }
    return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
}

common::Result<std::vector<ProposedFileChange>> SqliteChangeProposalRepository::listChanges(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "SELECT change_id,change_set_id,operation,relative_path,source_path,"
        "expected_original_hash,ordinal "
        "FROM proposed_file_changes WHERE change_set_id=? ORDER BY ordinal ASC;",
        stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedFileChange>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, changeSetId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedFileChange>>::failure(*bound.error());
    }

    std::vector<ProposedFileChange> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ProposedFileChange ch;
        ch.changeId   = columnText(stmt.get(), 0);
        // column 1 is change_set_id (ignored)
        ch.operation  = application::fileChangeOperationFromString(columnText(stmt.get(), 2));
        ch.relativePath = pathFromUtf8(columnText(stmt.get(), 3));
        const std::string src = columnText(stmt.get(), 4);
        if (!src.empty()) ch.sourcePath = pathFromUtf8(src);
        const std::string hash = columnText(stmt.get(), 5);
        if (!hash.empty()) ch.expectedOriginalHash = hash;
        // proposedContent and capturedOriginalContent are NOT stored in SQLite.
        result.push_back(std::move(ch));
    }
    return common::Result<std::vector<ProposedFileChange>>::ok(std::move(result));
}

common::Result<std::vector<ValidationFinding>> SqliteChangeProposalRepository::listValidationFindings(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "SELECT code,message,change_id FROM proposal_validation_findings "
        "WHERE change_set_id=? ORDER BY ordinal ASC;",
        stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ValidationFinding>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, changeSetId);
    if (!bound.success()) {
        return common::Result<std::vector<ValidationFinding>>::failure(*bound.error());
    }

    std::vector<ValidationFinding> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ValidationFinding f;
        f.code     = columnText(stmt.get(), 0);
        f.message  = columnText(stmt.get(), 1);
        f.changeId = columnText(stmt.get(), 2);
        result.push_back(std::move(f));
    }
    return common::Result<std::vector<ValidationFinding>>::ok(std::move(result));
}

namespace {

// Conservative cap safely below any SQLite build's compiled-in
// SQLITE_LIMIT_VARIABLE_NUMBER (999 pre-3.32.0, 32766 by default since), so a
// batched IN(...) query below never risks "too many SQL variables" no matter
// how many ids a caller passes.
constexpr std::size_t kBatchedInQueryChunkSize = 500;

// Deduplicates (a repeated id must not produce repeated rows in the result,
// and would otherwise waste bound parameters/chunk headroom for nothing) and
// splits into chunks no larger than kBatchedInQueryChunkSize. A single id's
// rows always come from exactly one chunk's query - chunks partition the
// deduplicated id list - so each chunk's own "ORDER BY change_set_id,ordinal"
// is never interleaved with another chunk's rows for the same id.
std::vector<std::vector<ChangeSetId>> dedupedIdChunks(const std::vector<ChangeSetId>& ids)
{
    std::vector<ChangeSetId> unique;
    unique.reserve(ids.size());
    std::unordered_set<ChangeSetId> seen;
    seen.reserve(ids.size());
    for (const auto& id : ids) {
        if (seen.insert(id).second) unique.push_back(id);
    }
    std::vector<std::vector<ChangeSetId>> chunks;
    for (std::size_t offset = 0; offset < unique.size(); offset += kBatchedInQueryChunkSize) {
        const std::size_t count = std::min(kBatchedInQueryChunkSize, unique.size() - offset);
        chunks.emplace_back(
            unique.begin() + static_cast<std::ptrdiff_t>(offset),
            unique.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    return chunks;
}

} // namespace

common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
SqliteChangeProposalRepository::listChangesForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>;
    std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> byChangeSet;
    if (changeSetIds.empty()) {
        return ResultType::ok(std::move(byChangeSet));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    for (const auto& chunk : dedupedIdChunks(changeSetIds)) {
        std::string placeholders(chunk.size() * 2 - 1, ',');
        for (std::size_t i = 0; i < chunk.size(); ++i) placeholders[i * 2] = '?';

        Statement stmt;
        const std::string sql =
            "SELECT change_set_id,change_id,operation,relative_path,source_path,"
            "expected_original_hash,ordinal FROM proposed_file_changes WHERE change_set_id IN ("
            + placeholders + ") ORDER BY change_set_id,ordinal ASC;";
        const auto prep = prepare(handle, sql.c_str(), stmt);
        if (!prep.success()) return ResultType::failure(*prep.error());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto bound = bindText(handle, stmt.get(), static_cast<int>(i) + 1, chunk[i]);
            if (!bound.success()) return ResultType::failure(*bound.error());
        }

        // Every non-ROW step() result must be checked explicitly: SQLITE_DONE
        // means the chunk finished cleanly, anything else (SQLITE_BUSY after
        // the busy-timeout elapses, SQLITE_IOERR, SQLITE_CORRUPT, ...) is a
        // real failure. Returning failure here discards this function's own
        // accumulated byChangeSet entirely - a caller never sees a partial
        // result silently reported as success.
        for (;;) {
            const int step = sqlite3_step(stmt.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return ResultType::failure(databaseError(
                    handle, "persistence.read_failed", "Could not read proposed file changes"));
            }
            const std::string changeSetId = columnText(stmt.get(), 0);
            ProposedFileChange ch;
            ch.changeId = columnText(stmt.get(), 1);
            ch.operation = application::fileChangeOperationFromString(columnText(stmt.get(), 2));
            ch.relativePath = pathFromUtf8(columnText(stmt.get(), 3));
            const std::string src = columnText(stmt.get(), 4);
            if (!src.empty()) ch.sourcePath = pathFromUtf8(src);
            const std::string hash = columnText(stmt.get(), 5);
            if (!hash.empty()) ch.expectedOriginalHash = hash;
            // proposedContent and capturedOriginalContent are NOT stored in SQLite.
            byChangeSet[changeSetId].push_back(std::move(ch));
        }
    }
    return ResultType::ok(std::move(byChangeSet));
}

common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
SqliteChangeProposalRepository::listValidationFindingsForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>;
    std::unordered_map<ChangeSetId, std::vector<ValidationFinding>> byChangeSet;
    if (changeSetIds.empty()) {
        return ResultType::ok(std::move(byChangeSet));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    for (const auto& chunk : dedupedIdChunks(changeSetIds)) {
        std::string placeholders(chunk.size() * 2 - 1, ',');
        for (std::size_t i = 0; i < chunk.size(); ++i) placeholders[i * 2] = '?';

        Statement stmt;
        const std::string sql =
            "SELECT change_set_id,code,message,change_id FROM proposal_validation_findings "
            "WHERE change_set_id IN (" + placeholders + ") ORDER BY change_set_id,ordinal ASC;";
        const auto prep = prepare(handle, sql.c_str(), stmt);
        if (!prep.success()) return ResultType::failure(*prep.error());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto bound = bindText(handle, stmt.get(), static_cast<int>(i) + 1, chunk[i]);
            if (!bound.success()) return ResultType::failure(*bound.error());
        }

        for (;;) {
            const int step = sqlite3_step(stmt.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return ResultType::failure(databaseError(
                    handle, "persistence.read_failed", "Could not read validation findings"));
            }
            const std::string changeSetId = columnText(stmt.get(), 0);
            ValidationFinding f;
            f.code = columnText(stmt.get(), 1);
            f.message = columnText(stmt.get(), 2);
            f.changeId = columnText(stmt.get(), 3);
            byChangeSet[changeSetId].push_back(std::move(f));
        }
    }
    return ResultType::ok(std::move(byChangeSet));
}

// ── SqliteChangeApprovalRepository ───────────────────────────────────────────

namespace {

std::string serializeApplyHashLog(const std::vector<ApplyHashEntry>& entries)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries) {
        array.push_back({
            {"changeId", entry.changeId},
            {"relativePath", pathToUtf8(entry.relativePath)},
            {"originalHash", entry.originalHash.value_or(std::string {})},
            {"newHash", entry.newHash.value_or(std::string {})}
        });
    }
    return array.dump();
}

std::vector<ApplyHashEntry> deserializeApplyHashLog(const std::string& json)
{
    std::vector<ApplyHashEntry> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        ApplyHashEntry entry;
        entry.changeId = item.value("changeId", std::string {});
        entry.relativePath = pathFromUtf8(item.value("relativePath", std::string {}));
        const std::string originalHash = item.value("originalHash", std::string {});
        if (!originalHash.empty()) entry.originalHash = originalHash;
        const std::string newHash = item.value("newHash", std::string {});
        if (!newHash.empty()) entry.newHash = newHash;
        result.push_back(std::move(entry));
    }
    return result;
}

common::Result<ApprovalRecord> readApprovalRecord(sqlite3_stmt* statement)
{
    ApprovalRecord record;
    record.changeSetId = columnText(statement, 0);
    record.snapshotHash = columnText(statement, 1);
    record.approvedAt = fromMilliseconds(sqlite3_column_int64(statement, 2));
    record.approvedBy = optionalText(statement, 3);
    record.appliedAt = optionalTime(statement, 4);
    record.applyResult = optionalText(statement, 5);
    record.applyErrorCode = optionalText(statement, 6);
    record.applyHashLog = deserializeApplyHashLog(columnText(statement, 7));
    record.postApplyWorkspaceFingerprint = optionalText(statement, 8);
    return common::Result<ApprovalRecord>::ok(std::move(record));
}

constexpr const char* changeSetApprovalColumns =
    "change_set_id,snapshot_hash,approved_at_ms,approved_by,"
    "applied_at_ms,apply_result,apply_error_code,apply_hash_log,"
    "post_apply_workspace_fingerprint";

} // namespace

SqliteChangeApprovalRepository::SqliteChangeApprovalRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteChangeApprovalRepository::recordApproval(
    const ApprovalRecord& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO change_set_approvals "
        "(change_set_id,snapshot_hash,approved_at_ms,approved_by) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.changeSetId),
        bindText(handle, statement.get(), 2, record.snapshotHash),
        bindInt64(handle, statement.get(), 3, toMilliseconds(record.approvedAt)),
        bindNullableText(handle, statement.get(), 4, record.approvedBy)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("proposal.duplicate_id", "Change set already has an approval record"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert approval record"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ApprovalRecord>> SqliteChangeApprovalRepository::findApproval(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetApprovalColumns
        + " FROM change_set_approvals WHERE change_set_id=?;";
    Statement statement;
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, changeSetId);
    if (!bound.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*bound.error());

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ApprovalRecord>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read approval record"));
    }
    auto record = readApprovalRecord(statement.get());
    if (!record.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*record.error());
    return common::Result<std::optional<ApprovalRecord>>::ok(std::move(record.value()));
}

common::Result<void> SqliteChangeApprovalRepository::recordApplyOutcome(
    const ChangeSetId& changeSetId,
    const std::string& result,
    const std::optional<std::string>& errorCode,
    const std::vector<ApplyHashEntry>& hashLog,
    const std::optional<std::string>& postApplyWorkspaceFingerprint)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE change_set_approvals SET applied_at_ms=?,apply_result=?,"
        "apply_error_code=?,apply_hash_log=?,post_apply_workspace_fingerprint=? "
        "WHERE change_set_id=?;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindInt64(handle, statement.get(), 1, toMilliseconds(std::chrono::system_clock::now())),
        bindText(handle, statement.get(), 2, result),
        bindNullableText(handle, statement.get(), 3, errorCode),
        bindText(handle, statement.get(), 4, serializeApplyHashLog(hashLog)),
        bindNullableText(handle, statement.get(), 5, postApplyWorkspaceFingerprint),
        bindText(handle, statement.get(), 6, changeSetId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record apply outcome"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(
            error("proposal.not_found", "No approval record exists for this change set"));
    }
    return common::Result<void>::ok();
}

// ── SqliteProposalReviewRepository ───────────────────────────────────────────

namespace {

constexpr const char* proposalReviewRejectionColumns =
    "change_set_id,rejected_at_ms,rejected_by,reason";

common::Result<ProposalReviewRejection> readProposalReviewRejection(sqlite3_stmt* statement)
{
    ProposalReviewRejection record;
    record.changeSetId = columnText(statement, 0);
    record.rejectedAt = fromMilliseconds(sqlite3_column_int64(statement, 1));
    record.rejectedBy = optionalText(statement, 2);
    record.reason = optionalText(statement, 3);
    return common::Result<ProposalReviewRejection>::ok(std::move(record));
}

} // namespace

SqliteProposalReviewRepository::SqliteProposalReviewRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteProposalReviewRepository::recordRejection(
    const ProposalReviewRejection& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO change_set_reviewer_rejections "
        "(change_set_id,rejected_at_ms,rejected_by,reason) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.changeSetId),
        bindInt64(handle, statement.get(), 2, toMilliseconds(record.rejectedAt)),
        bindNullableText(handle, statement.get(), 3, record.rejectedBy),
        bindNullableText(handle, statement.get(), 4, record.reason)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("proposal.duplicate_id",
                    "Change set already has a reviewer rejection record"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert reviewer rejection record"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposalReviewRejection>>
SqliteProposalReviewRepository::findRejection(const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + proposalReviewRejectionColumns
        + " FROM change_set_reviewer_rejections WHERE change_set_id=?;";
    Statement statement;
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, changeSetId);
    if (!bound.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*bound.error());

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ProposalReviewRejection>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProposalReviewRejection>>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read reviewer rejection record"));
    }
    auto record = readProposalReviewRejection(statement.get());
    if (!record.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*record.error());
    return common::Result<std::optional<ProposalReviewRejection>>::ok(std::move(record.value()));
}

// ── SqliteAgentToolSessionRepository ─────────────────────────────────────────

namespace {

std::string agentToolSessionStateToString(application::AgentToolSessionState s)
{
    switch (s) {
    case application::AgentToolSessionState::Active:      return "active";
    case application::AgentToolSessionState::Completed:   return "completed";
    case application::AgentToolSessionState::Interrupted: return "interrupted";
    }
    return "active";
}

std::optional<application::AgentToolSessionState> agentToolSessionStateFromString(
    std::string_view v)
{
    if (v == "active")      return application::AgentToolSessionState::Active;
    if (v == "completed")   return application::AgentToolSessionState::Completed;
    if (v == "interrupted") return application::AgentToolSessionState::Interrupted;
    return std::nullopt;
}

std::string agentToolCallStateToString(application::AgentToolCallState s)
{
    switch (s) {
    case application::AgentToolCallState::Requested:  return "requested";
    case application::AgentToolCallState::Running:    return "running";
    case application::AgentToolCallState::Completed:  return "completed";
    case application::AgentToolCallState::Failed:     return "failed";
    case application::AgentToolCallState::Interrupted:return "interrupted";
    }
    return "requested";
}

std::optional<application::AgentToolCallState> agentToolCallStateFromString(
    std::string_view v)
{
    if (v == "requested")  return application::AgentToolCallState::Requested;
    if (v == "running")    return application::AgentToolCallState::Running;
    if (v == "completed")  return application::AgentToolCallState::Completed;
    if (v == "failed")     return application::AgentToolCallState::Failed;
    if (v == "interrupted")return application::AgentToolCallState::Interrupted;
    return std::nullopt;
}

} // namespace

SqliteAgentToolSessionRepository::SqliteAgentToolSessionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::AgentToolSession>
SqliteAgentToolSessionRepository::insertSession(application::AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_tool_sessions
              (id,run_id,provider_id,model_id,state,
               total_turns,total_calls,total_returned_bytes,
               created_at_ms,finished_at_ms)
              VALUES (?,?,?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentToolSession>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,session.sessionId);       !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,2,session.runId);           !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,3,session.providerId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,4,session.modelId);         !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,5,agentToolSessionStateToString(session.state)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,6,session.totalTurns);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,session.totalCalls);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,8,session.totalReturnedBytes);  !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindInt64(h,s,9,toMilliseconds(session.createdAt)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,10,session.finishedAt);     !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentToolSession>::failure(
            databaseError(h, "persistence.insert_failed", "agent_tool_sessions insert"));

    return common::Result<application::AgentToolSession>::ok(std::move(session));
}

common::Result<application::AgentToolSession>
SqliteAgentToolSessionRepository::updateSession(application::AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_sessions SET
              run_id=?,provider_id=?,model_id=?,state=?,
              total_turns=?,total_calls=?,total_returned_bytes=?,
              created_at_ms=?,finished_at_ms=?
              WHERE id=?)sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentToolSession>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,session.runId);           !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,2,session.providerId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,3,session.modelId);         !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,4,agentToolSessionStateToString(session.state)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,5,session.totalTurns);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,6,session.totalCalls);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,session.totalReturnedBytes);  !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindInt64(h,s,8,toMilliseconds(session.createdAt)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,9,session.finishedAt);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,10,session.sessionId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentToolSession>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_sessions update"));

    if (sqlite3_changes(h) == 0)
        return common::Result<application::AgentToolSession>::failure(
            error("agent_session.not_found", "No agent_tool_session row with that id"));

    return common::Result<application::AgentToolSession>::ok(std::move(session));
}

namespace {
application::AgentToolSession readAgentToolSession(sqlite3_stmt* s)
{
    application::AgentToolSession sess;
    sess.sessionId          = columnText(s, 0);
    sess.runId              = columnText(s, 1);
    sess.providerId         = columnText(s, 2);
    sess.modelId            = columnText(s, 3);
    const auto state        = agentToolSessionStateFromString(columnText(s, 4));
    sess.state              = state.value_or(application::AgentToolSessionState::Active);
    sess.totalTurns         = static_cast<std::size_t>(sqlite3_column_int64(s, 5));
    sess.totalCalls         = static_cast<std::size_t>(sqlite3_column_int64(s, 6));
    sess.totalReturnedBytes = static_cast<std::size_t>(sqlite3_column_int64(s, 7));
    sess.createdAt          = fromMilliseconds(sqlite3_column_int64(s, 8));
    sess.finishedAt         = optionalTime(s, 9);
    return sess;
}
} // namespace

common::Result<std::optional<application::AgentToolSession>>
SqliteAgentToolSessionRepository::findSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,run_id,provider_id,model_id,state,
                     total_turns,total_calls,total_returned_bytes,
                     created_at_ms,finished_at_ms
              FROM agent_tool_sessions WHERE id=?)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::optional<application::AgentToolSession>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::optional<application::AgentToolSession>>::failure(*r.error());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
        return common::Result<std::optional<application::AgentToolSession>>::ok(std::nullopt);
    if (rc != SQLITE_ROW)
        return common::Result<std::optional<application::AgentToolSession>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_sessions findSession"));

    return common::Result<std::optional<application::AgentToolSession>>::ok(
        readAgentToolSession(stmt.get()));
}

common::Result<std::vector<application::AgentToolSession>>
SqliteAgentToolSessionRepository::listForRun(const std::string& runId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,run_id,provider_id,model_id,state,
                     total_turns,total_calls,total_returned_bytes,
                     created_at_ms,finished_at_ms
              FROM agent_tool_sessions WHERE run_id=? ORDER BY created_at_ms ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentToolSession>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, runId); !r.success())
        return common::Result<std::vector<application::AgentToolSession>>::failure(*r.error());

    std::vector<application::AgentToolSession> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
        result.push_back(readAgentToolSession(stmt.get()));

    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentToolSession>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_sessions listForRun"));

    return common::Result<std::vector<application::AgentToolSession>>::ok(std::move(result));
}

common::Result<application::AgentProviderTurn>
SqliteAgentToolSessionRepository::insertTurn(application::AgentProviderTurn turn)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_provider_turns
              (id,session_id,turn_index,finish_reason,
               input_tokens,output_tokens,call_count,created_at_ms)
              VALUES (?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentProviderTurn>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,turn.turnId);              !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindText(h,s,2,turn.sessionId);           !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,3,turn.turnIndex);       !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindText(h,s,4,turn.finishReason);        !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,5,static_cast<std::int64_t>(turn.inputTokens));  !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,6,static_cast<std::int64_t>(turn.outputTokens)); !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,turn.callCount);       !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,8,toMilliseconds(turn.createdAt)); !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentProviderTurn>::failure(
            databaseError(h, "persistence.insert_failed", "agent_provider_turns insert"));

    return common::Result<application::AgentProviderTurn>::ok(std::move(turn));
}

common::Result<std::vector<application::AgentProviderTurn>>
SqliteAgentToolSessionRepository::listTurns(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,turn_index,finish_reason,
                     input_tokens,output_tokens,call_count,created_at_ms
              FROM agent_provider_turns WHERE session_id=? ORDER BY turn_index ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(*r.error());

    std::vector<application::AgentProviderTurn> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        application::AgentProviderTurn turn;
        turn.turnId       = columnText(stmt.get(), 0);
        turn.sessionId    = columnText(stmt.get(), 1);
        turn.turnIndex    = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 2));
        turn.finishReason = columnText(stmt.get(), 3);
        turn.inputTokens  = static_cast<int>(sqlite3_column_int64(stmt.get(), 4));
        turn.outputTokens = static_cast<int>(sqlite3_column_int64(stmt.get(), 5));
        turn.callCount    = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 6));
        turn.createdAt    = fromMilliseconds(sqlite3_column_int64(stmt.get(), 7));
        result.push_back(std::move(turn));
    }
    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(
            databaseError(h, "persistence.query_failed", "agent_provider_turns listTurns"));

    return common::Result<std::vector<application::AgentProviderTurn>>::ok(std::move(result));
}

common::Result<std::size_t> SqliteAgentToolSessionRepository::interruptActiveSessions()
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        "UPDATE agent_tool_sessions SET state='interrupted',finished_at_ms=? WHERE state='active'",
        stmt);
    if (!prepared.success())
        return common::Result<std::size_t>::failure(*prepared.error());

    if (auto r = bindInt64(database_->impl_->handle, stmt.get(), 1, nowMs); !r.success())
        return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(database_->impl_->handle,
                "persistence.update_failed", "agent_tool_sessions interruptActiveSessions"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(database_->impl_->handle)));
}

// ── SqliteAgentToolAuditRepository ───────────────────────────────────────────

SqliteAgentToolAuditRepository::SqliteAgentToolAuditRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteAgentToolAuditRepository::insert(
    const application::AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_tool_calls
              (id,session_id,run_id,provider_call_id,tool_name,state,
               arguments_sha256,result_sha256,result_artifact_id,
               error_code,returned_bytes,truncated,created_at_ms,finished_at_ms)
              VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,record.auditId);                              !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,2,record.sessionId);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,3,record.runId);                                !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,4,record.providerCallId);                       !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,5,record.toolName);                             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,6,agentToolCallStateToString(record.state));    !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,7,record.argumentsSha256);                      !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,8,record.resultSha256);                         !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindNullableText(h,s,9,record.resultArtifactId);             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,10,record.errorCode);                           !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,11,static_cast<std::uint64_t>(record.returnedBytes)); !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,12,record.truncated ? 1 : 0);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,13,toMilliseconds(record.createdAt));          !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,14,record.finishedAt);                  !r.success()) return common::Result<void>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.insert_failed", "agent_tool_calls insert"));

    return common::Result<void>::ok();
}

common::Result<void> SqliteAgentToolAuditRepository::update(
    const application::AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls SET
              session_id=?,run_id=?,provider_call_id=?,tool_name=?,state=?,
              arguments_sha256=?,result_sha256=?,result_artifact_id=?,
              error_code=?,returned_bytes=?,truncated=?,
              created_at_ms=?,finished_at_ms=?
              WHERE id=?)sql",
        stmt);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,record.sessionId);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,2,record.runId);                                !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,3,record.providerCallId);                       !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,4,record.toolName);                             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,5,agentToolCallStateToString(record.state));    !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,6,record.argumentsSha256);                      !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,7,record.resultSha256);                         !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindNullableText(h,s,8,record.resultArtifactId);             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,9,record.errorCode);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,10,static_cast<std::uint64_t>(record.returnedBytes)); !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,11,record.truncated ? 1 : 0);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,12,toMilliseconds(record.createdAt));          !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,13,record.finishedAt);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,14,record.auditId);                             !r.success()) return common::Result<void>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_calls update"));

    if (sqlite3_changes(h) == 0)
        return common::Result<void>::failure(
            error("agent_call.not_found", "No agent_tool_calls row with that id"));

    return common::Result<void>::ok();
}

namespace {
application::AgentToolAuditRecord readAgentToolAuditRecord(sqlite3_stmt* s)
{
    application::AgentToolAuditRecord r;
    r.auditId          = columnText(s, 0);
    r.sessionId        = columnText(s, 1);
    r.runId            = columnText(s, 2);
    r.providerCallId   = columnText(s, 3);
    r.toolName         = columnText(s, 4);
    const auto state   = agentToolCallStateFromString(columnText(s, 5));
    r.state            = state.value_or(application::AgentToolCallState::Requested);
    r.argumentsSha256  = columnText(s, 6);
    r.resultSha256     = columnText(s, 7);
    r.resultArtifactId = optionalText(s, 8);
    r.errorCode        = columnText(s, 9);
    r.returnedBytes    = static_cast<std::size_t>(sqlite3_column_int64(s, 10));
    r.truncated        = sqlite3_column_int64(s, 11) != 0;
    r.createdAt        = fromMilliseconds(sqlite3_column_int64(s, 12));
    r.finishedAt       = optionalTime(s, 13);
    return r;
}
} // namespace

common::Result<std::optional<application::AgentToolAuditRecord>>
SqliteAgentToolAuditRepository::find(const std::string& auditId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,run_id,provider_call_id,tool_name,state,
                     arguments_sha256,result_sha256,result_artifact_id,
                     error_code,returned_bytes,truncated,created_at_ms,finished_at_ms
              FROM agent_tool_calls WHERE id=?)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, auditId); !r.success())
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(*r.error());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
        return common::Result<std::optional<application::AgentToolAuditRecord>>::ok(std::nullopt);
    if (rc != SQLITE_ROW)
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_calls find"));

    return common::Result<std::optional<application::AgentToolAuditRecord>>::ok(
        readAgentToolAuditRecord(stmt.get()));
}

common::Result<std::vector<application::AgentToolAuditRecord>>
SqliteAgentToolAuditRepository::listForSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,run_id,provider_call_id,tool_name,state,
                     arguments_sha256,result_sha256,result_artifact_id,
                     error_code,returned_bytes,truncated,created_at_ms,finished_at_ms
              FROM agent_tool_calls WHERE session_id=? ORDER BY created_at_ms ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(*r.error());

    std::vector<application::AgentToolAuditRecord> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
        result.push_back(readAgentToolAuditRecord(stmt.get()));

    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_calls listForSession"));

    return common::Result<std::vector<application::AgentToolAuditRecord>>::ok(std::move(result));
}

common::Result<std::size_t> SqliteAgentToolAuditRepository::interruptRunning(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls
              SET state='interrupted', finished_at_ms=?
              WHERE session_id=? AND state IN ('requested','running'))sql",
        stmt);
    if (!prepared.success()) return common::Result<std::size_t>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindInt64(h, stmt.get(), 1, nowMs);       !r.success()) return common::Result<std::size_t>::failure(*r.error());
    if (auto r = bindText(h, stmt.get(), 2, sessionId);    !r.success()) return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_calls interruptRunning"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(h)));
}

common::Result<std::size_t> SqliteAgentToolAuditRepository::interruptAllRunning()
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls
              SET state='interrupted', finished_at_ms=?
              WHERE state IN ('requested','running'))sql",
        stmt);
    if (!prepared.success()) return common::Result<std::size_t>::failure(*prepared.error());

    if (auto r = bindInt64(database_->impl_->handle, stmt.get(), 1, nowMs); !r.success())
        return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(database_->impl_->handle,
                "persistence.update_failed", "agent_tool_calls interruptAllRunning"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(database_->impl_->handle)));
}

} // namespace ai::infrastructure
