#include "ai/application/ExecutionManifestParser.hpp"

#include "ai/application/ManifestJsonParsingSupport.hpp"
#include "ai/application/VerificationBootstrapTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

namespace ai::application {

namespace {

using json = nlohmann::json;
// MW-P3A package 1: every string/array/path-safety primitive below now
// lives in ManifestJsonParsingSupport, shared with ExecutionManifestDraftParser
// so the two provider-facing parsers can never drift apart on byte/item
// ceilings or path-safety rules. Nothing in this file's own parsing
// behavior changed by this move.
using namespace manifest_parsing;

constexpr std::size_t kMaximumSubmissionBytes = 2ULL * 1024 * 1024;
constexpr std::size_t kMaximumTasks = 64;

common::Result<std::string> validateRawOwnedPath(const std::string& value)
{
    return validateRawScopePath(value, "manifest.invalid_owned_path");
}

common::Result<ExecutionManifestTask> parseTask(const json& value, std::size_t taskIndex)
{
    const std::string location = "tasks[" + std::to_string(taskIndex) + "]";
    const auto fields = requireExactFields(value, location,
        {"id", "objective", "role", "dependencies", "ownedPaths",
         "plannedCreatePaths", "requiredReadPaths", "referencePaths",
         "excludedPaths", "deferredPaths",
         "acceptanceCriteria", "contextReferences", "requestedCapability"},
        {"acceptanceContract"});
    if (!fields.success()) return fail<ExecutionManifestTask>(
        fields.error()->code, fields.error()->message);

    ExecutionManifestTask task;
    auto id = readString(value, "id");
    auto objective = readString(value, "objective");
    auto role = readString(value, "role");
    auto dependencies = readStringArray(value, "dependencies");
    auto ownedPaths = readStringArray(value, "ownedPaths");
    auto plannedCreatePaths = readStringArray(value, "plannedCreatePaths");
    auto requiredReadPaths = readStringArray(value, "requiredReadPaths");
    auto referencePaths = readStringArray(value, "referencePaths");
    auto acceptance = readStringArray(value, "acceptanceCriteria");
    auto context = readStringArray(value, "contextReferences");
    auto capabilityText = readString(value, "requestedCapability");
    if (!id.success()) return fail<ExecutionManifestTask>(id.error()->code, id.error()->message);
    if (!objective.success()) return fail<ExecutionManifestTask>(objective.error()->code, objective.error()->message);
    if (!role.success()) return fail<ExecutionManifestTask>(role.error()->code, role.error()->message);
    if (!dependencies.success()) return fail<ExecutionManifestTask>(dependencies.error()->code, dependencies.error()->message);
    if (!ownedPaths.success()) return fail<ExecutionManifestTask>(ownedPaths.error()->code, ownedPaths.error()->message);
    if (!plannedCreatePaths.success()) return fail<ExecutionManifestTask>(plannedCreatePaths.error()->code, plannedCreatePaths.error()->message);
    if (!requiredReadPaths.success()) return fail<ExecutionManifestTask>(requiredReadPaths.error()->code, requiredReadPaths.error()->message);
    if (!referencePaths.success()) return fail<ExecutionManifestTask>(referencePaths.error()->code, referencePaths.error()->message);
    if (!acceptance.success()) return fail<ExecutionManifestTask>(acceptance.error()->code, acceptance.error()->message);
    if (!context.success()) return fail<ExecutionManifestTask>(context.error()->code, context.error()->message);
    if (!capabilityText.success()) return fail<ExecutionManifestTask>(capabilityText.error()->code, capabilityText.error()->message);

    const auto capability = workspaceCapabilityFromString(capabilityText.value());
    if (!capability.has_value()) {
        return fail<ExecutionManifestTask>(
            "manifest.invalid_capability", "requestedCapability is not recognized");
    }

    task.id = std::move(id.value());
    task.objective = std::move(objective.value());
    task.role = std::move(role.value());
    task.dependencies = std::move(dependencies.value());
    for (const auto& path : ownedPaths.value()) {
        const auto validated = validateRawOwnedPath(path);
        if (!validated.success()) return fail<ExecutionManifestTask>(
            validated.error()->code, validated.error()->message);
        task.ownedPaths.push_back(pathFromUtf8(validated.value()));
    }
    for (const auto& path : plannedCreatePaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_planned_create_path");
        if (!validated.success()) return fail<ExecutionManifestTask>(
            validated.error()->code, validated.error()->message);
        task.plannedCreatePaths.push_back(pathFromUtf8(validated.value()));
    }
    for (const auto& path : requiredReadPaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_required_read_path");
        if (!validated.success()) return fail<ExecutionManifestTask>(
            validated.error()->code, validated.error()->message);
        task.requiredReadPaths.push_back(pathFromUtf8(validated.value()));
    }
    for (const auto& path : referencePaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_reference_path");
        if (!validated.success()) return fail<ExecutionManifestTask>(
            validated.error()->code, validated.error()->message);
        task.referencePaths.push_back(pathFromUtf8(validated.value()));
    }
    auto excludedPaths = parsePathReasonArray(
        value, "excludedPaths", "manifest.invalid_excluded_path", location);
    if (!excludedPaths.success()) return fail<ExecutionManifestTask>(
        excludedPaths.error()->code, excludedPaths.error()->message);
    task.excludedPaths = std::move(excludedPaths.value());
    auto deferredPaths = parsePathReasonArray(
        value, "deferredPaths", "manifest.invalid_deferred_path", location);
    if (!deferredPaths.success()) return fail<ExecutionManifestTask>(
        deferredPaths.error()->code, deferredPaths.error()->message);
    task.deferredPaths = std::move(deferredPaths.value());
    if (value.contains("acceptanceContract") && !value.at("acceptanceContract").is_null()) {
        auto contract = parseAcceptanceContract(
            value.at("acceptanceContract"), location + ".acceptanceContract");
        if (!contract.success()) return fail<ExecutionManifestTask>(
            contract.error()->code, contract.error()->message);
        task.acceptanceContract = std::move(contract.value());
    }
    task.acceptanceCriteria = std::move(acceptance.value());
    task.contextReferences = std::move(context.value());
    task.requestedCapability = *capability;
    return common::Result<ExecutionManifestTask>::ok(std::move(task));
}

} // namespace

common::Result<ExecutionManifest> ExecutionManifestParser::parse(
    std::string_view rawSubmission,
    const RunId& trustedRunId)
{
    if (rawSubmission.size() > kMaximumSubmissionBytes) {
        return fail<ExecutionManifest>(
            "manifest.output_too_large", "Manifest submission exceeds the parser byte limit");
    }

    json document;
    try {
        document = json::parse(rawSubmission.begin(), rawSubmission.end());
    } catch (const std::exception&) {
        return fail<ExecutionManifest>(
            "manifest.parse_error", "Manifest submission is not valid JSON");
    }

    const auto fields = requireExactFields(document, /*location=*/"",
        {"schemaVersion", "tasks", "requestedMaxParallelism"},
        {"requestedTokenBudget", "requestedCostMicros", "bootstrapDeclaration"});
    if (!fields.success()) return fail<ExecutionManifest>(
        fields.error()->code, fields.error()->message);

    auto schemaVersion = readString(document, "schemaVersion");
    auto parallelism = readUint32(document, "requestedMaxParallelism");
    if (!schemaVersion.success()) return fail<ExecutionManifest>(
        schemaVersion.error()->code, schemaVersion.error()->message);
    if (!parallelism.success()) return fail<ExecutionManifest>(
        parallelism.error()->code, parallelism.error()->message);
    if (!document.at("tasks").is_array()) {
        return fail<ExecutionManifest>(
            "manifest.invalid_field", "tasks must be an array");
    }
    if (document.at("tasks").size() > kMaximumTasks) {
        return fail<ExecutionManifest>(
            "manifest.too_many_tasks", "tasks exceeds the absolute parser limit");
    }

    ExecutionManifest manifest;
    manifest.schemaVersion = std::move(schemaVersion.value());
    manifest.runId = trustedRunId;
    manifest.requestedMaxParallelism = parallelism.value();
    std::size_t taskIndex = 0;
    for (const auto& taskJson : document.at("tasks")) {
        auto task = parseTask(taskJson, taskIndex);
        ++taskIndex;
        if (!task.success()) return fail<ExecutionManifest>(
            task.error()->code, task.error()->message);
        manifest.tasks.push_back(std::move(task.value()));
    }

    for (const auto* field : {"requestedTokenBudget", "requestedCostMicros"}) {
        if (!document.contains(field) || document.at(field).is_null()) continue;
        auto value = readUnsigned(document, field);
        if (!value.success()) return fail<ExecutionManifest>(
            value.error()->code, value.error()->message);
        if (std::string_view(field) == "requestedTokenBudget")
            manifest.requestedTokenBudget = value.value();
        else
            manifest.requestedCostMicros = value.value();
    }

    // AG-18.3: an explicit, typed same-Run bootstrap declaration - absence
    // (the key missing, or present as JSON null) must never be inferred as
    // consent; ManifestBootstrapValidator enforces the full admission
    // predicate once every task has been parsed.
    if (document.contains("bootstrapDeclaration") && !document.at("bootstrapDeclaration").is_null()) {
        const auto& declarationJson = document.at("bootstrapDeclaration");
        const auto declarationFields = requireExactFields(
            declarationJson, "bootstrapDeclaration", {"bootstrapTaskId", "setupPaths"});
        if (!declarationFields.success()) return fail<ExecutionManifest>(
            declarationFields.error()->code, declarationFields.error()->message);
        auto bootstrapTaskId = readString(declarationJson, "bootstrapTaskId");
        auto setupPaths = readStringArray(declarationJson, "setupPaths");
        if (!bootstrapTaskId.success()) return fail<ExecutionManifest>(
            bootstrapTaskId.error()->code, bootstrapTaskId.error()->message);
        if (!setupPaths.success()) return fail<ExecutionManifest>(
            setupPaths.error()->code, setupPaths.error()->message);
        if (setupPaths.value().size() > kVerificationBootstrapMaxSetupPaths) {
            return fail<ExecutionManifest>("manifest.invalid_field",
                "bootstrapDeclaration.setupPaths exceeds the bounded limit");
        }
        ManifestBootstrapDeclaration declaration;
        declaration.bootstrapTaskId = std::move(bootstrapTaskId.value());
        for (const auto& path : setupPaths.value()) {
            const auto validated = validateRawScopePath(path, "manifest.invalid_bootstrap_setup_path");
            if (!validated.success()) return fail<ExecutionManifest>(
                validated.error()->code, validated.error()->message);
            declaration.setupPaths.push_back(pathFromUtf8(validated.value()));
        }
        manifest.bootstrapDeclaration = std::move(declaration);
    }

    return common::Result<ExecutionManifest>::ok(std::move(manifest));
}

} // namespace ai::application
