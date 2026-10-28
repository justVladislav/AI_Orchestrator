#include "ai/application/execution/ExecutionManifestDraftParser.hpp"

#include "ai/application/execution/ManifestJsonParsingSupport.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/verification/VerificationBootstrapTypes.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace ai::application {

namespace {

using json = nlohmann::json;
using namespace manifest_parsing;

constexpr std::size_t kMaximumSubmissionBytes = 2ULL * 1024 * 1024;
constexpr std::size_t kMaximumCoordinationTasks = 64;
constexpr std::size_t kMaximumWorkTasks = 64;

common::Result<ManifestDraftCoordinationTask> parseCoordinationTask(
    const json& value, std::size_t taskIndex)
{
    const std::string location = "coordinationTasks[" + std::to_string(taskIndex) + "]";
    const auto fields = requireExactFields(value, location,
        {"id", "objective", "role", "dependencies"});
    if (!fields.success()) return fail<ManifestDraftCoordinationTask>(
        fields.error()->code, fields.error()->message);

    auto id = readString(value, "id");
    auto objective = readString(value, "objective");
    auto role = readString(value, "role");
    auto dependencies = readStringArray(value, "dependencies");
    if (!id.success()) return fail<ManifestDraftCoordinationTask>(id.error()->code, id.error()->message);
    if (!objective.success()) return fail<ManifestDraftCoordinationTask>(objective.error()->code, objective.error()->message);
    if (!role.success()) return fail<ManifestDraftCoordinationTask>(role.error()->code, role.error()->message);
    if (!dependencies.success()) return fail<ManifestDraftCoordinationTask>(dependencies.error()->code, dependencies.error()->message);

    ManifestDraftCoordinationTask task;
    task.id = std::move(id.value());
    task.objective = std::move(objective.value());
    task.role = std::move(role.value());
    task.dependencies = std::move(dependencies.value());
    return common::Result<ManifestDraftCoordinationTask>::ok(std::move(task));
}

common::Result<ManifestDraftWorkTask> parseWorkTask(const json& value, std::size_t taskIndex)
{
    const std::string location = "workTasks[" + std::to_string(taskIndex) + "]";
    const auto fields = requireExactFields(value, location,
        {"id", "objective", "role", "dependencies", "writePaths",
         "baselineRequiredReadPaths", "proposedDependencyReadPaths", "referencePaths",
         "excludedPaths", "deferredPaths",
         "acceptanceCriteria", "contextReferences", "requestedCapability",
         "acceptanceContract"},
        {"assigneeMemberId"});
    if (!fields.success()) return fail<ManifestDraftWorkTask>(
        fields.error()->code, fields.error()->message);

    ManifestDraftWorkTask task;
    // AG-17.4: optional on the wire (a single-assignment Run's schema does not
    // advertise it); a JSON null reads as absent, like the budget fields. The
    // roster rules (required, known, workspace-capable, never the lead) are the
    // validator's, judged against the frozen routing record.
    if (value.contains("assigneeMemberId") && !value.at("assigneeMemberId").is_null()) {
        auto assignee = readString(value, "assigneeMemberId");
        if (!assignee.success()) return fail<ManifestDraftWorkTask>(
            assignee.error()->code, assignee.error()->message);
        if (assignee.value().empty()
            || assignee.value().size() > kManifestRoutingMaxMemberIdBytes) {
            return fail<ManifestDraftWorkTask>(
                "manifest.invalid_field",
                location + ".assigneeMemberId must be a non-empty memberId of bounded length");
        }
        task.assigneeMemberId = std::move(assignee.value());
    }
    auto id = readString(value, "id");
    auto objective = readString(value, "objective");
    auto role = readString(value, "role");
    auto dependencies = readStringArray(value, "dependencies");
    auto writePaths = readStringArray(value, "writePaths");
    auto baselineRequiredReadPaths = readStringArray(value, "baselineRequiredReadPaths");
    auto proposedDependencyReadPaths = readStringArray(value, "proposedDependencyReadPaths");
    auto referencePaths = readStringArray(value, "referencePaths");
    auto acceptance = readStringArray(value, "acceptanceCriteria");
    auto context = readStringArray(value, "contextReferences");
    auto capabilityText = readString(value, "requestedCapability");
    if (!id.success()) return fail<ManifestDraftWorkTask>(id.error()->code, id.error()->message);
    if (!objective.success()) return fail<ManifestDraftWorkTask>(objective.error()->code, objective.error()->message);
    if (!role.success()) return fail<ManifestDraftWorkTask>(role.error()->code, role.error()->message);
    if (!dependencies.success()) return fail<ManifestDraftWorkTask>(dependencies.error()->code, dependencies.error()->message);
    if (!writePaths.success()) return fail<ManifestDraftWorkTask>(writePaths.error()->code, writePaths.error()->message);
    if (!baselineRequiredReadPaths.success()) return fail<ManifestDraftWorkTask>(baselineRequiredReadPaths.error()->code, baselineRequiredReadPaths.error()->message);
    if (!proposedDependencyReadPaths.success()) return fail<ManifestDraftWorkTask>(proposedDependencyReadPaths.error()->code, proposedDependencyReadPaths.error()->message);
    if (!referencePaths.success()) return fail<ManifestDraftWorkTask>(referencePaths.error()->code, referencePaths.error()->message);
    if (!acceptance.success()) return fail<ManifestDraftWorkTask>(acceptance.error()->code, acceptance.error()->message);
    if (!context.success()) return fail<ManifestDraftWorkTask>(context.error()->code, context.error()->message);
    if (!capabilityText.success()) return fail<ManifestDraftWorkTask>(capabilityText.error()->code, capabilityText.error()->message);

    const auto capability = manifestDraftCapabilityFromString(capabilityText.value());
    if (!capability.has_value()) {
        return fail<ManifestDraftWorkTask>(
            "manifest.invalid_capability",
            "requestedCapability is not recognized for a work task (must be "
            "inspect or propose - a coordination task belongs in "
            "coordinationTasks instead)");
    }

    task.id = std::move(id.value());
    task.objective = std::move(objective.value());
    task.role = std::move(role.value());
    task.dependencies = std::move(dependencies.value());
    for (const auto& path : writePaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_write_path");
        if (!validated.success()) return fail<ManifestDraftWorkTask>(
            validated.error()->code, validated.error()->message);
        task.writePaths.push_back(pathFromUtf8(validated.value()));
    }
    for (const auto& path : baselineRequiredReadPaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_baseline_required_read_path");
        if (!validated.success()) return fail<ManifestDraftWorkTask>(
            validated.error()->code, validated.error()->message);
        task.baselineRequiredReadPaths.push_back(pathFromUtf8(validated.value()));
    }
    for (const auto& path : proposedDependencyReadPaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_proposed_dependency_read_path");
        if (!validated.success()) return fail<ManifestDraftWorkTask>(
            validated.error()->code, validated.error()->message);
        task.proposedDependencyReadPaths.push_back(pathFromUtf8(validated.value()));
    }
    for (const auto& path : referencePaths.value()) {
        const auto validated = validateRawScopePath(path, "manifest.invalid_reference_path");
        if (!validated.success()) return fail<ManifestDraftWorkTask>(
            validated.error()->code, validated.error()->message);
        task.referencePaths.push_back(pathFromUtf8(validated.value()));
    }
    auto excludedPaths = parsePathReasonArray(
        value, "excludedPaths", "manifest.invalid_excluded_path", location);
    if (!excludedPaths.success()) return fail<ManifestDraftWorkTask>(
        excludedPaths.error()->code, excludedPaths.error()->message);
    task.excludedPaths = std::move(excludedPaths.value());
    auto deferredPaths = parsePathReasonArray(
        value, "deferredPaths", "manifest.invalid_deferred_path", location);
    if (!deferredPaths.success()) return fail<ManifestDraftWorkTask>(
        deferredPaths.error()->code, deferredPaths.error()->message);
    task.deferredPaths = std::move(deferredPaths.value());

    // Unlike the legacy execution-manifest.v1 task, acceptanceContract is a
    // required key with a required OBJECT value - there is no JSON null
    // branch to check for. A malformed or absent value fails exactly like
    // any other required object field via requireExactFields/at() above;
    // this is the structural fix for
    // manifest.missing_acceptance_contract (Run
    // 3ffb52df-98ca-472f-9b1c-537ab134cf18): the old schema let the model
    // satisfy the wire-required key with a null value the runtime then
    // rejected for every inspect/propose task.
    auto contract = parseAcceptanceContract(
        value.at("acceptanceContract"), location + ".acceptanceContract");
    if (!contract.success()) return fail<ManifestDraftWorkTask>(
        contract.error()->code, contract.error()->message);
    task.acceptanceContract = std::move(contract.value());

    task.acceptanceCriteria = std::move(acceptance.value());
    task.contextReferences = std::move(context.value());
    task.requestedCapability = *capability;
    return common::Result<ManifestDraftWorkTask>::ok(std::move(task));
}

} // namespace

common::Result<ExecutionManifestDraft> ExecutionManifestDraftParser::parse(
    std::string_view rawSubmission)
{
    if (rawSubmission.size() > kMaximumSubmissionBytes) {
        return fail<ExecutionManifestDraft>(
            "manifest.output_too_large", "Manifest draft submission exceeds the parser byte limit");
    }

    json document;
    try {
        document = json::parse(rawSubmission.begin(), rawSubmission.end());
    } catch (const std::exception&) {
        return fail<ExecutionManifestDraft>(
            "manifest.parse_error", "Manifest draft submission is not valid JSON");
    }

    const auto fields = requireExactFields(document, /*location=*/"",
        {"schemaVersion", "coordinationTasks", "workTasks", "requestedMaxParallelism"},
        {"requestedTokenBudget", "requestedCostMicros", "bootstrapDeclaration"});
    if (!fields.success()) return fail<ExecutionManifestDraft>(
        fields.error()->code, fields.error()->message);

    auto schemaVersion = readString(document, "schemaVersion");
    auto parallelism = readUint32(document, "requestedMaxParallelism");
    if (!schemaVersion.success()) return fail<ExecutionManifestDraft>(
        schemaVersion.error()->code, schemaVersion.error()->message);
    if (!parallelism.success()) return fail<ExecutionManifestDraft>(
        parallelism.error()->code, parallelism.error()->message);
    if (!document.at("coordinationTasks").is_array()) {
        return fail<ExecutionManifestDraft>(
            "manifest.invalid_field", "coordinationTasks must be an array");
    }
    if (!document.at("workTasks").is_array()) {
        return fail<ExecutionManifestDraft>(
            "manifest.invalid_field", "workTasks must be an array");
    }
    if (document.at("coordinationTasks").size() > kMaximumCoordinationTasks) {
        return fail<ExecutionManifestDraft>(
            "manifest.too_many_tasks", "coordinationTasks exceeds the absolute parser limit");
    }
    if (document.at("workTasks").size() > kMaximumWorkTasks) {
        return fail<ExecutionManifestDraft>(
            "manifest.too_many_tasks", "workTasks exceeds the absolute parser limit");
    }

    ExecutionManifestDraft draft;
    draft.schemaVersion = std::move(schemaVersion.value());
    draft.requestedMaxParallelism = parallelism.value();

    std::size_t coordinationIndex = 0;
    for (const auto& taskJson : document.at("coordinationTasks")) {
        auto task = parseCoordinationTask(taskJson, coordinationIndex);
        ++coordinationIndex;
        if (!task.success()) return fail<ExecutionManifestDraft>(
            task.error()->code, task.error()->message);
        draft.coordinationTasks.push_back(std::move(task.value()));
    }
    std::size_t workIndex = 0;
    for (const auto& taskJson : document.at("workTasks")) {
        auto task = parseWorkTask(taskJson, workIndex);
        ++workIndex;
        if (!task.success()) return fail<ExecutionManifestDraft>(
            task.error()->code, task.error()->message);
        draft.workTasks.push_back(std::move(task.value()));
    }

    for (const auto* field : {"requestedTokenBudget", "requestedCostMicros"}) {
        if (!document.contains(field) || document.at(field).is_null()) continue;
        auto value = readUnsigned(document, field);
        if (!value.success()) return fail<ExecutionManifestDraft>(
            value.error()->code, value.error()->message);
        if (std::string_view(field) == "requestedTokenBudget")
            draft.requestedTokenBudget = value.value();
        else
            draft.requestedCostMicros = value.value();
    }

    if (document.contains("bootstrapDeclaration") && !document.at("bootstrapDeclaration").is_null()) {
        const auto& declarationJson = document.at("bootstrapDeclaration");
        const auto declarationFields = requireExactFields(
            declarationJson, "bootstrapDeclaration", {"bootstrapTaskId", "setupPaths"});
        if (!declarationFields.success()) return fail<ExecutionManifestDraft>(
            declarationFields.error()->code, declarationFields.error()->message);
        auto bootstrapTaskId = readString(declarationJson, "bootstrapTaskId");
        auto setupPaths = readStringArray(declarationJson, "setupPaths");
        if (!bootstrapTaskId.success()) return fail<ExecutionManifestDraft>(
            bootstrapTaskId.error()->code, bootstrapTaskId.error()->message);
        if (!setupPaths.success()) return fail<ExecutionManifestDraft>(
            setupPaths.error()->code, setupPaths.error()->message);
        if (setupPaths.value().size() > kVerificationBootstrapMaxSetupPaths) {
            return fail<ExecutionManifestDraft>("manifest.invalid_field",
                "bootstrapDeclaration.setupPaths exceeds the bounded limit");
        }
        ManifestBootstrapDeclaration declaration;
        declaration.bootstrapTaskId = std::move(bootstrapTaskId.value());
        for (const auto& path : setupPaths.value()) {
            const auto validated = validateRawScopePath(path, "manifest.invalid_bootstrap_setup_path");
            if (!validated.success()) return fail<ExecutionManifestDraft>(
                validated.error()->code, validated.error()->message);
            declaration.setupPaths.push_back(pathFromUtf8(validated.value()));
        }
        draft.bootstrapDeclaration = std::move(declaration);
    }

    return common::Result<ExecutionManifestDraft>::ok(std::move(draft));
}

AgentToolDefinition submitExecutionManifestDraftToolDefinition(
    const std::vector<std::string>& assignableMemberIds)
{
    AgentToolDefinition definition;
    definition.name = kSubmitExecutionManifestDraftToolName;
    definition.version = "1";
    definition.description =
        "Submit the execution-manifest-draft.v1 task graph; this ends planning. "
        "The Backend compiles this draft into the final execution-manifest.v1, "
        "classifying write paths as existing/new against the frozen baseline "
        "and deriving cross-task file dependencies itself.";
    definition.inputSchemaJson = R"JSON({
"type":"object",
"properties":{
  "schemaVersion":{"type":"string","const":"execution-manifest-draft.v1"},
  "coordinationTasks":{"type":"array","maxItems":64,"description":"Pure dependency-anchor tasks with no writes, reads, or acceptance criteria of their own; use an empty array when every task does real work.","items":{"type":"object","properties":{
    "id":{"type":"string","minLength":1},"objective":{"type":"string","minLength":1},"role":{"type":"string","minLength":1},
    "dependencies":{"type":"array","items":{"type":"string"}}
  },"required":["id","objective","role","dependencies"],"additionalProperties":false}},
  "workTasks":{"type":"array","minItems":1,"maxItems":64,"description":"Only concrete remaining execution work; never repository-audit or report-only filler tasks","items":{"type":"object","properties":{
    "id":{"type":"string","minLength":1},"objective":{"type":"string","minLength":1},"role":{"type":"string","minLength":1},
    "dependencies":{"type":"array","items":{"type":"string"}},
    "writePaths":{"type":"array","maxItems":256,"description":"Every workspace-relative path this task writes, whether it already exists or is new - the Backend classifies each as owned_write/planned_create from the frozen baseline; never split this yourself.","items":{"type":"string","minLength":1}},
    "baselineRequiredReadPaths":{"type":"array","maxItems":256,"description":"Existing workspace-relative paths this task must inspect - the CURRENT, already-existing baseline content, regardless of whether some other task's writePaths also includes the same path to modify it LATER. Use this whenever you only need today's content, not another task's future result. Never derives a dependency on that other task.","items":{"type":"string","minLength":1}},
    "proposedDependencyReadPaths":{"type":"array","maxItems":256,"description":"Workspace-relative paths this task must inspect specifically because another task in THIS SAME submission is producing them (that other task's writePaths includes the exact same path) - a genuine ordering dependency. The Backend derives a real dependency on that other task from each entry here; every entry must actually be produced by some other work task's writePaths, never a path nobody in this submission writes.","items":{"type":"string","minLength":1}},
    "referencePaths":{"type":"array","maxItems":256,"description":"Read-only workspace-relative reference paths; use an empty array when none","items":{"type":"string","minLength":1}},
    "excludedPaths":{"type":"array","maxItems":256,"description":"Deliberately excluded workspace-relative paths with a nonblank audit reason; use an empty array when none","items":{"type":"object","properties":{"path":{"type":"string","minLength":1},"reason":{"type":"string","minLength":1,"maxLength":2000}},"required":["path","reason"],"additionalProperties":false}},
    "deferredPaths":{"type":"array","maxItems":256,"description":"Deliberately deferred workspace-relative paths with a nonblank audit reason; use an empty array when none","items":{"type":"object","properties":{"path":{"type":"string","minLength":1},"reason":{"type":"string","minLength":1,"maxLength":2000}},"required":["path","reason"],"additionalProperties":false}},
    "acceptanceCriteria":{"type":"array","maxItems":256,"items":{"type":"string","minLength":1}},
    "acceptanceContract":{"type":"object","description":"A real execution-acceptance.v1 object; every work task requires one, never a null literal.","properties":{
      "schemaVersion":{"type":"string","const":"execution-acceptance.v1"},
      "requirements":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","properties":{
        "id":{"type":"string","minLength":1,"maxLength":4096},
        "description":{"type":"string","minLength":1,"maxLength":4096},
        "requiredOutputs":{"type":"array","description":"Exact files from this same task's writePaths, baselineRequiredReadPaths, or proposedDependencyReadPaths; never report labels or descriptive phrases.","maxItems":32,"items":{"type":"string","minLength":1,"maxLength":4096}},
        "verificationKind":{"type":"string","description":"Backend profile. inspect_paths needs no profile/build-profile selection; cmake_build/qml_lint select a catalog profile+build-profile; qml_lint also selects a catalog target.","enum":["inspect_paths","cmake_build","qml_lint"]},
        "verificationProfileId":{"type":["string","null"],"description":"A profileId taken verbatim from the trusted verification environment catalog. Required for cmake_build/qml_lint; must be null for inspect_paths. Never invent an id not advertised in the catalog.","maxLength":128},
        "verificationBuildProfileId":{"type":["string","null"],"description":"A buildProfileId taken verbatim from the selected profile's own catalog entry. Required for cmake_build/qml_lint; must be null for inspect_paths. Never invent an id not advertised under the selected profile.","maxLength":128},
        "verificationTargetId":{"type":["string","null"],"description":"A target id taken verbatim from a confirmed catalog allow-list. Required for qml_lint (from the selected build profile); must be null for inspect_paths/cmake_build. Never invent a target not advertised in the catalog.","maxLength":128},
        "verificationArguments":{"type":"array","description":"Task-owned arguments only. Always empty [] - the Backend resolves inspect_paths/cmake_build/qml_lint environment arguments exclusively from your profile/build-profile/target selection and a non-empty array here is rejected as a forbidden raw override.","maxItems":16,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":4096},"value":{"type":"string","maxLength":4096}},"required":["name","value"],"additionalProperties":false}},
        "required":{"type":"boolean"},
        "verificationBootstrapPending":{"type":"boolean","description":"true ONLY when no existing verification profile is available and this exact requirement's environment must be resolved later from the manifest-level bootstrapDeclaration's candidate proposal. When true, verificationProfileId/verificationBuildProfileId/verificationTargetId must all be null and verificationArguments must be empty. Never true unless bootstrapDeclaration is also set; never true for inspect_paths."}
      },"required":["id","description","requiredOutputs","verificationKind","verificationProfileId","verificationBuildProfileId","verificationTargetId","verificationArguments","required","verificationBootstrapPending"],"additionalProperties":false}}
    },"required":["schemaVersion","requirements"],"additionalProperties":false},
    "contextReferences":{"type":"array","maxItems":256,"items":{"type":"string","minLength":1}},
    "requestedCapability":{"type":"string","enum":["inspect","propose"]}
  },"required":["id","objective","role","dependencies","writePaths","baselineRequiredReadPaths","proposedDependencyReadPaths","referencePaths","excludedPaths","deferredPaths","acceptanceCriteria","contextReferences","requestedCapability","acceptanceContract"],"additionalProperties":false}},
  "requestedMaxParallelism":{"type":"integer","minimum":1,"maximum":32},
  "requestedTokenBudget":{"type":["integer","null"],"minimum":0},
  "requestedCostMicros":{"type":["integer","null"],"minimum":0},
  "bootstrapDeclaration":{"type":["object","null"],"description":"Set ONLY when no existing verification profile is available and this Manifest creates or modifies the exact CMake preset file(s) needed to establish one, in this same Run, as ordinary reviewable owned/planned-create work. JSON null when no bootstrap is declared - never inferred from any other field.","properties":{
    "bootstrapTaskId":{"type":"string","minLength":1,"description":"The single work task that owns every declared setup file, via its own writePaths."},
    "setupPaths":{"type":"array","minItems":1,"maxItems":4,"description":"Exact repository-root paths, only CMakePresets.json and/or CMakeUserPresets.json - never a subdirectory, never any other filename.","items":{"type":"string","minLength":1}}
  },"required":["bootstrapTaskId","setupPaths"],"additionalProperties":false}
},
"required":["schemaVersion","coordinationTasks","workTasks","requestedMaxParallelism","requestedTokenBudget","requestedCostMicros","bootstrapDeclaration"],
"additionalProperties":false
})JSON";
    if (!assignableMemberIds.empty()) {
        // AG-17.4 roster variant. The single-assignment schema above is returned
        // byte-for-byte unchanged (its prompt-cache prefix and every existing
        // request stay identical); only a roster Run adds the assignee, as a
        // REQUIRED enum of exactly the Backend's assignable member ids, so a
        // provider cannot even express an unknown member or the lead.
        auto schema = json::parse(definition.inputSchemaJson);
        auto& workItem = schema.at("properties").at("workTasks").at("items");
        workItem.at("properties")["assigneeMemberId"] = json{
            {"type", "string"},
            {"enum", assignableMemberIds},
            {"description", "The memberId of the roster worker that executes this task, "
                "copied verbatim from the trusted roster in the planning context. "
                "Exactly one; never the lead Architect."}};
        workItem.at("required").push_back("assigneeMemberId");
        definition.inputSchemaJson = schema.dump();
        definition.description += " Every workTasks entry also names its roster worker in "
            "assigneeMemberId.";
    }
    definition.readOnly = true;
    definition.maxResultBytes = 4096;
    definition.strictInputSchema = true;
    return definition;
}

} // namespace ai::application
