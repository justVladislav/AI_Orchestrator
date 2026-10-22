#include "ai/application/ExecutionManifestDraftParser.hpp"
#include "ai/application/ExecutionManifestDraftTypes.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// MW-P3A package 1 ("Typed Draft"): coverage for the new provider-facing
// intake boundary in isolation. This parser is not wired into
// ExecutionManifestPlanner yet (that is package 2's job); these tests only
// prove the draft shape itself is safe and that the two real defect classes
// named in ARCHITECTURE_GAPS.md's MW-P3A (Runs
// 3ffb52df-98ca-472f-9b1c-537ab134cf18 / a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b)
// are now structurally unrepresentable rather than merely rejected by a
// post-parse business rule.

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string acceptanceContractJson(const std::string& id, const std::string& requiredOutput)
{
    return "{\"schemaVersion\":\"execution-acceptance.v1\","
           "\"requirements\":[{\"id\":\"" + id + "\",\"description\":\"Verify " + id + "\","
           "\"requiredOutputs\":[\"" + requiredOutput + "\"],\"verificationKind\":\"inspect_paths\","
           "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
           "\"verificationTargetId\":null,\"verificationArguments\":[],\"required\":true,"
           "\"verificationBootstrapPending\":false}]}";
}

std::string workTaskJson(
    const std::string& id,
    const std::string& dependencies,
    const std::string& writePath,
    const std::string& capability,
    const std::string& acceptanceContractField)
{
    return "{\"id\":\"" + id
        + "\",\"objective\":\"Complete " + id
        + "\",\"role\":\"worker\",\"dependencies\":" + dependencies
        + ",\"writePaths\":[\"" + writePath
        + "\"],\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
          "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
          "\"acceptanceCriteria\":[\"Tests pass\"],"
          "\"acceptanceContract\":" + acceptanceContractField + ","
          "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
          "\"requestedCapability\":\"" + capability + "\"}";
}

std::string workTaskJson(
    const std::string& id, const std::string& dependencies,
    const std::string& writePath, const std::string& capability)
{
    return workTaskJson(id, dependencies, writePath, capability,
        acceptanceContractJson("verify-" + id, writePath));
}

std::string coordinationTaskJson(const std::string& id, const std::string& dependencies)
{
    return "{\"id\":\"" + id + "\",\"objective\":\"Coordinate " + id
        + "\",\"role\":\"coordinator\",\"dependencies\":" + dependencies + "}";
}

std::string draftJson(
    const std::string& coordinationTasksArray,
    const std::string& workTasksArray,
    std::uint32_t parallelism = 1)
{
    return "{\"schemaVersion\":\"execution-manifest-draft.v1\","
           "\"coordinationTasks\":" + coordinationTasksArray + ","
           "\"workTasks\":" + workTasksArray + ","
           "\"requestedMaxParallelism\":" + std::to_string(parallelism) + ","
           "\"requestedTokenBudget\":null,\"requestedCostMicros\":null,"
           "\"bootstrapDeclaration\":null}";
}

void testValidDraftWithCoordinationAndWorkTasksParsesSuccessfully()
{
    const auto submission = draftJson(
        "[" + coordinationTaskJson("gate", "[]") + "]",
        "[" + workTaskJson("build", "[\"gate\"]", "src/app.cpp", "propose") + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(parsed.success(), "A well-formed draft with one coordination and one work task must parse");
    require(parsed.value().coordinationTasks.size() == 1, "Coordination task must round-trip");
    require(parsed.value().workTasks.size() == 1, "Work task must round-trip");
    require(parsed.value().workTasks[0].writePaths.size() == 1
        && parsed.value().workTasks[0].writePaths[0] == std::filesystem::path("src/app.cpp"),
        "writePaths must round-trip as a single undifferentiated list");
    require(parsed.value().workTasks[0].requestedCapability == ManifestDraftCapability::Propose,
        "requestedCapability must round-trip");
}

void testCoordinationTaskCannotCarryWritePathsOrAcceptance()
{
    // MW-P3A: a "none capability" task used to be representable with paths
    // and acceptance criteria attached, rejected only after parsing
    // (manifest.none_task_not_empty). A coordination task's object shape has
    // no such properties at all - this must fail at the unexpected-field
    // check, not a later semantic rule.
    const std::string malformedCoordinationTask =
        "{\"id\":\"gate\",\"objective\":\"Coordinate\",\"role\":\"coordinator\","
        "\"dependencies\":[],\"writePaths\":[\"src/app.cpp\"]}";
    const auto submission = draftJson(
        "[" + malformedCoordinationTask + "]",
        "[" + workTaskJson("build", "[]", "src/app.cpp", "propose") + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(!parsed.success(), "A coordination task with writePaths must be rejected");
    require(parsed.error()->code == "manifest.unexpected_field",
        "Rejection must be the structural unexpected-field check, not a semantic rule");
}

void testWorkTaskRequestedCapabilityNoneIsRejected()
{
    const auto submission = draftJson("[]",
        "[" + workTaskJson("build", "[]", "src/app.cpp", "none") + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(!parsed.success(),
        "A work task cannot request 'none' capability - use coordinationTasks instead");
    require(parsed.error()->code == "manifest.invalid_capability",
        "Rejection must name the invalid-capability code");
}

void testWorkTaskMissingAcceptanceContractKeyIsRejected()
{
    const std::string taskWithoutContract =
        "{\"id\":\"build\",\"objective\":\"Complete build\",\"role\":\"worker\","
        "\"dependencies\":[],\"writePaths\":[\"src/app.cpp\"],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"}";
    const auto submission = draftJson("[]", "[" + taskWithoutContract + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(!parsed.success(), "A work task missing the acceptanceContract key must be rejected");
    require(parsed.error()->code == "manifest.missing_field",
        "Rejection must be the structural missing-field check");
}

void testWorkTaskNullAcceptanceContractIsRejected()
{
    // MW-P3A's central structural fix (Run 3ffb52df-98ca-472f-9b1c-537ab134cf18):
    // the legacy schema let the model satisfy the wire-required
    // acceptanceContract key with a JSON null value for an inspect/propose
    // task; the wire schema permitted it and the runtime then rejected it
    // after parsing (manifest.missing_acceptance_contract), costing a whole
    // repair attempt. Here acceptanceContract is typed "object" only, not
    // ["object","null"], so null fails to parse at all - there is no later
    // repair round needed to discover this.
    const auto submission = draftJson("[]",
        "[" + workTaskJson("build", "[]", "src/app.cpp", "propose", "null") + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(!parsed.success(), "A work task with a null acceptanceContract must be rejected");
    require(parsed.error()->code == "manifest.parse_error",
        "Rejection must be the structural not-an-object check, never "
        "manifest.missing_acceptance_contract (that code no longer exists in this pipeline)");
}

void testWorkTaskUnsafeWritePathIsRejected()
{
    const auto submission = draftJson("[]",
        "[" + workTaskJson("build", "[]", "../escape.cpp", "propose") + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(!parsed.success(), "A write path with parent traversal must be rejected");
    require(parsed.error()->code == "manifest.invalid_write_path",
        "Rejection must name the write-path-specific safety code");
}

void testBaselineAndProposedDependencyReadPathsRoundTripSeparately()
{
    // MW-P3A package 2 ("Read Semantics", Run
    // a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b): the two read lists are typed
    // and separate at the wire level - the provider states the distinction
    // explicitly rather than the Backend inferring it.
    const std::string task =
        "{\"id\":\"build\",\"objective\":\"Complete build\",\"role\":\"worker\","
        "\"dependencies\":[],\"writePaths\":[\"src/app.cpp\"],"
        "\"baselineRequiredReadPaths\":[\"docs/PROJECT_CONTEXT.md\"],"
        "\"proposedDependencyReadPaths\":[\"src/other-task-output.cpp\"],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":" + acceptanceContractJson("verify-build", "src/app.cpp") + ","
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"}";
    const auto submission = draftJson("[]", "[" + task + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(parsed.success(), "Both typed read lists must parse independently");
    require(parsed.value().workTasks[0].baselineRequiredReadPaths.size() == 1
        && parsed.value().workTasks[0].baselineRequiredReadPaths[0]
            == std::filesystem::path("docs/PROJECT_CONTEXT.md"),
        "baselineRequiredReadPaths must round-trip");
    require(parsed.value().workTasks[0].proposedDependencyReadPaths.size() == 1
        && parsed.value().workTasks[0].proposedDependencyReadPaths[0]
            == std::filesystem::path("src/other-task-output.cpp"),
        "proposedDependencyReadPaths must round-trip");
}

void testUnsafeBaselineAndProposedDependencyReadPathsAreRejectedWithDistinctCodes()
{
    const std::string unsafeBaselineTask =
        "{\"id\":\"build\",\"objective\":\"Complete build\",\"role\":\"worker\","
        "\"dependencies\":[],\"writePaths\":[\"src/app.cpp\"],"
        "\"baselineRequiredReadPaths\":[\"../escape.md\"],"
        "\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":" + acceptanceContractJson("verify-build", "src/app.cpp") + ","
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"}";
    const auto unsafeBaselineParsed = ExecutionManifestDraftParser::parse(
        draftJson("[]", "[" + unsafeBaselineTask + "]"));
    require(!unsafeBaselineParsed.success(), "An unsafe baselineRequiredReadPaths entry must be rejected");
    require(unsafeBaselineParsed.error()->code == "manifest.invalid_baseline_required_read_path",
        "Rejection must name the baseline-read-specific safety code");

    const std::string unsafeProposedTask =
        "{\"id\":\"build\",\"objective\":\"Complete build\",\"role\":\"worker\","
        "\"dependencies\":[],\"writePaths\":[\"src/app.cpp\"],"
        "\"baselineRequiredReadPaths\":[],"
        "\"proposedDependencyReadPaths\":[\"../escape.md\"],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":" + acceptanceContractJson("verify-build", "src/app.cpp") + ","
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"}";
    const auto unsafeProposedParsed = ExecutionManifestDraftParser::parse(
        draftJson("[]", "[" + unsafeProposedTask + "]"));
    require(!unsafeProposedParsed.success(), "An unsafe proposedDependencyReadPaths entry must be rejected");
    require(unsafeProposedParsed.error()->code == "manifest.invalid_proposed_dependency_read_path",
        "Rejection must name the proposed-dependency-read-specific safety code");
}

void testExcludedAndDeferredPathsRoundTripThroughDraftParser()
{
    const std::string task =
        "{\"id\":\"build\",\"objective\":\"Complete build\",\"role\":\"worker\","
        "\"dependencies\":[],\"writePaths\":[\"src/app.cpp\"],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],"
        "\"excludedPaths\":[{\"path\":\"src/legacy.cpp\",\"reason\":\"Out of scope this Run\"}],"
        "\"deferredPaths\":[{\"path\":\"src/future.cpp\",\"reason\":\"Deferred to a later Run\"}],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":" + acceptanceContractJson("verify-build", "src/app.cpp") + ","
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"}";
    const auto submission = draftJson("[]", "[" + task + "]");
    const auto parsed = ExecutionManifestDraftParser::parse(submission);
    require(parsed.success(), "Excluded/deferred paths with nonblank reasons must parse");
    require(parsed.value().workTasks[0].excludedPaths.size() == 1
        && parsed.value().workTasks[0].excludedPaths[0].reason == "Out of scope this Run",
        "Excluded path and reason must round-trip");
    require(parsed.value().workTasks[0].deferredPaths.size() == 1
        && parsed.value().workTasks[0].deferredPaths[0].reason == "Deferred to a later Run",
        "Deferred path and reason must round-trip");
}

void testBootstrapDeclarationRoundTripsThroughDraftParser()
{
    const auto withoutBootstrap = draftJson("[]",
        "[" + workTaskJson("build", "[]", "src/app.cpp", "propose") + "]");
    auto document = nlohmann::json::parse(withoutBootstrap);
    document["bootstrapDeclaration"] = nlohmann::json{
        {"bootstrapTaskId", "build"},
        {"setupPaths", nlohmann::json::array({"CMakePresets.json"})}};
    const auto parsed = ExecutionManifestDraftParser::parse(document.dump());
    require(parsed.success(), "A well-formed bootstrapDeclaration must parse");
    require(parsed.value().bootstrapDeclaration.has_value(),
        "bootstrapDeclaration must be populated when present and non-null");
    require(parsed.value().bootstrapDeclaration->bootstrapTaskId == "build",
        "bootstrapTaskId must round-trip");
    require(parsed.value().bootstrapDeclaration->setupPaths.size() == 1,
        "setupPaths must round-trip");
}

void testTopLevelUnexpectedFieldIsRejected()
{
    const auto valid = draftJson("[]",
        "[" + workTaskJson("build", "[]", "src/app.cpp", "propose") + "]");
    auto document = nlohmann::json::parse(valid);
    document["ownedPaths"] = nlohmann::json::array();
    const auto parsed = ExecutionManifestDraftParser::parse(document.dump());
    require(!parsed.success(),
        "A stray legacy top-level field (e.g. ownedPaths) must be rejected");
    require(parsed.error()->code == "manifest.unexpected_field",
        "Rejection must be the structural unexpected-field check");
}

void testToolDefinitionIsStrictAndParsesAsValidJsonSchema()
{
    const auto definition = submitExecutionManifestDraftToolDefinition();
    require(definition.name == std::string(kSubmitExecutionManifestDraftToolName),
        "Tool name must match the exported constant");
    require(definition.strictInputSchema, "Draft tool schema must be strict-mode compatible");
    require(definition.readOnly, "Submitting a draft must be advertised as a read-only tool call");
    // Must be valid JSON, and must not silently regress to allowing a null
    // acceptanceContract for a work task.
    const auto schema = nlohmann::json::parse(definition.inputSchemaJson);
    const auto& workTaskSchema =
        schema["properties"]["workTasks"]["items"]["properties"]["acceptanceContract"];
    require(workTaskSchema["type"] == "object",
        "The work task acceptanceContract wire type must be exactly \"object\", never nullable");
}

// ---------------------------------------------------------------------------
// AG-17.4: assigneeMemberId in the strict draft schema.
// ---------------------------------------------------------------------------

std::string withAssignee(std::string task, const std::string& assigneeValueJson)
{
    task.pop_back(); // the closing brace
    return task + ",\"assigneeMemberId\":" + assigneeValueJson + "}";
}

// The single-assignment schema is part of every existing request (and of the
// provider's prompt-cache prefix): it must stay byte-for-byte what it was before
// AG-17.4, with or without the new optional argument. Golden hashes were taken
// from the pre-AG-17.4 implementation.
void testSingleAssignmentToolSchemaIsByteIdentical()
{
    for (const auto& definition : {
            submitExecutionManifestDraftToolDefinition(),
            submitExecutionManifestDraftToolDefinition({})}) {
        require(sha256Hex(definition.inputSchemaJson)
                == "bcb0937a1a691c4908e3a708ac1ba66654ed001d81aab355a0c4c188372fdc47",
            "The single-assignment draft schema must not change");
        require(sha256Hex(definition.description)
                == "18f366a8f11d8a69816c0e088d355edcd43217e1e766d3144397e952feff5d62",
            "The single-assignment draft tool description must not change");
        require(definition.inputSchemaJson.find("assigneeMemberId") == std::string::npos,
            "A single-assignment Run must never be offered an assignee field");
    }
}

void testRosterToolSchemaRequiresAnAssigneeFromTheAssignableIds()
{
    const auto legacy = submitExecutionManifestDraftToolDefinition();
    const auto roster = submitExecutionManifestDraftToolDefinition({"qml-worker", "cpp-worker"});
    require(roster.strictInputSchema && roster.readOnly && roster.name == legacy.name,
        "The roster variant stays the same strict, read-only terminal tool");

    auto schema = nlohmann::json::parse(roster.inputSchemaJson);
    auto& workItem = schema["properties"]["workTasks"]["items"];
    const auto& assignee = workItem["properties"]["assigneeMemberId"];
    require(assignee["type"] == "string"
            && assignee["enum"] == nlohmann::json::array({"qml-worker", "cpp-worker"}),
        "The assignee must be a string restricted to exactly the assignable member ids, in order");
    const auto& required = workItem["required"];
    require(std::find(required.begin(), required.end(), "assigneeMemberId") != required.end(),
        "A strict-schema provider must be required to supply the assignee on every work task");
    require(workItem["additionalProperties"] == false,
        "The work task shape must stay closed");
    require(!schema["properties"]["coordinationTasks"]["items"]["properties"]
            .contains("assigneeMemberId"),
        "A coordination task executes nothing and must not offer an assignee");

    // Apart from that one property and its required entry the roster schema is
    // exactly the single-assignment schema.
    workItem["properties"].erase("assigneeMemberId");
    auto& mutableRequired = workItem["required"];
    mutableRequired.erase(std::find(mutableRequired.begin(), mutableRequired.end(),
        "assigneeMemberId"));
    require(schema == nlohmann::json::parse(legacy.inputSchemaJson),
        "The roster variant must differ from the single-assignment schema only by the assignee");
}

void testWorkTaskAssigneeRoundTripsAndNullReadsAsAbsent()
{
    const auto base = workTaskJson("build", "[]", "src/app.cpp", "propose");
    const auto parse = [&](const std::string& task) {
        return ExecutionManifestDraftParser::parse(draftJson("[]", "[" + task + "]"));
    };

    const auto named = parse(withAssignee(base, "\"qml-worker\""));
    require(named.success() && named.value().workTasks[0].assigneeMemberId == "qml-worker",
        "A work task's assignee must round-trip");
    const auto nulled = parse(withAssignee(base, "null"));
    require(nulled.success() && !nulled.value().workTasks[0].assigneeMemberId.has_value(),
        "A JSON null assignee reads as absent (the validator then reports it missing)");
    const auto absent = parse(base);
    require(absent.success() && !absent.value().workTasks[0].assigneeMemberId.has_value(),
        "A task without the optional field keeps working (single-assignment schema)");
}

void testMalformedAssigneeIsRejected()
{
    const auto base = workTaskJson("build", "[]", "src/app.cpp", "propose");
    const auto rejected = [&](const std::string& valueJson) {
        return ExecutionManifestDraftParser::parse(
            draftJson("[]", "[" + withAssignee(base, valueJson) + "]"));
    };
    const auto empty = rejected("\"\"");
    require(!empty.success() && empty.error()->code == "manifest.invalid_field",
        "An empty assignee must be rejected");
    const auto tooLong = rejected("\"" + std::string(kManifestRoutingMaxMemberIdBytes + 1, 'a') + "\"");
    require(!tooLong.success() && tooLong.error()->code == "manifest.invalid_field",
        "An assignee longer than a routing member id can be must be rejected");
    require(!rejected("42").success() && !rejected("[\"qml-worker\"]").success(),
        "A non-string assignee must be rejected");
}

void testCoordinationTaskStillCannotNameAnAssignee()
{
    const auto coordination = withAssignee(coordinationTaskJson("gate", "[]"), "\"qml-worker\"");
    const auto parsed = ExecutionManifestDraftParser::parse(draftJson(
        "[" + coordination + "]",
        "[" + workTaskJson("build", "[]", "src/app.cpp", "propose") + "]"));
    require(!parsed.success() && parsed.error()->code == "manifest.unexpected_field",
        "A coordination task has no assignee field: it must fail the structural check");
}

} // namespace

int main()
{
    try {
        testValidDraftWithCoordinationAndWorkTasksParsesSuccessfully();
        testCoordinationTaskCannotCarryWritePathsOrAcceptance();
        testWorkTaskRequestedCapabilityNoneIsRejected();
        testWorkTaskMissingAcceptanceContractKeyIsRejected();
        testWorkTaskNullAcceptanceContractIsRejected();
        testWorkTaskUnsafeWritePathIsRejected();
        testBaselineAndProposedDependencyReadPathsRoundTripSeparately();
        testUnsafeBaselineAndProposedDependencyReadPathsAreRejectedWithDistinctCodes();
        testExcludedAndDeferredPathsRoundTripThroughDraftParser();
        testBootstrapDeclarationRoundTripsThroughDraftParser();
        testTopLevelUnexpectedFieldIsRejected();
        testToolDefinitionIsStrictAndParsesAsValidJsonSchema();
        testSingleAssignmentToolSchemaIsByteIdentical();
        testRosterToolSchemaRequiresAnAssigneeFromTheAssignableIds();
        testWorkTaskAssigneeRoundTripsAndNullReadsAsAbsent();
        testMalformedAssigneeIsRejected();
        testCoordinationTaskStillCannotNameAnAssignee();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
