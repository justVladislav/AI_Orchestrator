// Recursive strict-schema contract tests: proves validateStrictJsonSchema()
// actually enforces the OpenAI/Anthropic strict subset at every nesting
// level, and that the Planner's own terminal tool schemas
// (submitExecutionManifestToolDefinition/submitNoChangesNeededToolDefinition)
// genuinely satisfy it - not just that they happen to compile. No network,
// no HTTP fakes: this exercises the schema shape only.

#include "ai/application/agents/AgentToolTurnLoopRunner.hpp"
#include "ai/application/execution/ExecutionManifestPlanner.hpp"
#include "ai/application/proposals/ProposedChangeSetParser.hpp"
#include "ai/providers/StrictSchemaValidation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

void testPlannerTerminalSchemasAreStrictCompatible()
{
    const auto manifestDef = ai::application::submitExecutionManifestToolDefinition();
    req(manifestDef.strictInputSchema,
        "submit_execution_manifest must declare strictInputSchema");
    const auto manifestSchema = nlohmann::json::parse(manifestDef.inputSchemaJson);
    const auto manifestValid =
        ai::providers::validateStrictJsonSchema(manifestSchema, manifestDef.name);
    if (!manifestValid.success()) {
        throw std::runtime_error(
            "submit_execution_manifest schema is not strict-compatible: "
            + manifestValid.error()->message);
    }

    // Parser/schema drift is more dangerous than a merely non-strict schema:
    // AG-15 made all six path roles mandatory in ExecutionManifestParser, so
    // the provider contract must force every key (including empty arrays)
    // before a submission can reach the Backend parser.
    const auto& taskSchema = manifestSchema.at("properties").at("tasks").at("items");
    const auto& taskProperties = taskSchema.at("properties");
    const auto& taskRequired = taskSchema.at("required");
    req(taskProperties.size() == 14 && taskRequired.size() == 14,
        "submit_execution_manifest task field set drifted from the Backend parser");
    for (const char* field : {"ownedPaths", "plannedCreatePaths", "requiredReadPaths",
             "referencePaths", "excludedPaths", "deferredPaths"}) {
        req(taskProperties.contains(field),
            "submit_execution_manifest schema omitted an AG-15 path-role property");
        req(std::find(taskRequired.begin(), taskRequired.end(), field) != taskRequired.end(),
            "submit_execution_manifest schema did not require an AG-15 path-role field");
        req(taskProperties.at(field).at("type") == "array",
            "submit_execution_manifest path-role field must be an array");
    }
    for (const char* field :
            {"ownedPaths", "plannedCreatePaths", "requiredReadPaths", "referencePaths"}) {
        req(taskProperties.at(field).at("items").at("type") == "string",
            "active Manifest path-role items must be strings");
    }
    for (const char* field : {"excludedPaths", "deferredPaths"}) {
        const auto& item = taskProperties.at(field).at("items");
        req(item.at("type") == "object",
            "excluded/deferred Manifest path-role items must be objects");
        req(item.at("properties").size() == 2
                && item.at("properties").contains("path")
                && item.at("properties").contains("reason"),
            "excluded/deferred Manifest path-role item properties drifted from the parser");
        req(item.at("required").size() == 2
                && std::find(item.at("required").begin(), item.at("required").end(), "path")
                    != item.at("required").end()
                && std::find(item.at("required").begin(), item.at("required").end(), "reason")
                    != item.at("required").end(),
            "excluded/deferred Manifest path-role item must require path and reason");
        req(item.at("additionalProperties") == false,
            "excluded/deferred Manifest path-role item must reject extra fields");
        req(item.at("properties").at("path").at("type") == "string"
                && item.at("properties").at("reason").at("type") == "string"
                && item.at("properties").at("reason").at("minLength") == 1
                && item.at("properties").at("reason").at("maxLength") == 2000,
            "excluded/deferred path or reason type/bounds drifted from the parser");
    }

    // AG-18.2: the acceptanceContract requirement schema now carries three
    // additional Backend-catalog selection fields
    // (verificationProfileId/verificationBuildProfileId/verificationTargetId)
    // instead of letting the provider author raw preset/buildPreset/
    // buildDirectory/target values - every one of them must be present,
    // required, and typed as a nullable string ["string","null"], the
    // strict-mode-compatible way to express "a value or null is expected
    // and the key itself must never be omitted".
    const auto& requirementSchema = taskSchema.at("properties").at("acceptanceContract")
        .at("properties").at("requirements").at("items");
    const auto& requirementProperties = requirementSchema.at("properties");
    const auto& requirementRequired = requirementSchema.at("required");
    req(requirementProperties.size() == 10 && requirementRequired.size() == 10,
        "submit_execution_manifest acceptanceContract requirement field set drifted from "
        "the Backend parser");
    for (const char* field :
            {"verificationProfileId", "verificationBuildProfileId", "verificationTargetId"}) {
        req(requirementProperties.contains(field),
            "submit_execution_manifest schema omitted an AG-18.2 verification selection field");
        req(std::find(requirementRequired.begin(), requirementRequired.end(), field)
                != requirementRequired.end(),
            "submit_execution_manifest schema did not require an AG-18.2 verification "
            "selection field");
        const auto& fieldType = requirementProperties.at(field).at("type");
        req(fieldType.is_array() && fieldType.size() == 2
                && std::find(fieldType.begin(), fieldType.end(), "string") != fieldType.end()
                && std::find(fieldType.begin(), fieldType.end(), "null") != fieldType.end(),
            "AG-18.2 verification selection field must be typed [\"string\",\"null\"]");
    }
    req(requirementProperties.at("verificationArguments").at("type") == "array"
            && requirementProperties.at("verificationArguments").at("items")
                   .at("additionalProperties") == false,
        "verificationArguments must remain a bounded array of exact name/value objects");

    // AG-18.3: verificationBootstrapPending is a plain (non-nullable)
    // boolean, always required - the same-Run bootstrap deferral flag.
    req(requirementProperties.contains("verificationBootstrapPending")
            && requirementProperties.at("verificationBootstrapPending").at("type") == "boolean"
            && std::find(requirementRequired.begin(), requirementRequired.end(),
                   "verificationBootstrapPending") != requirementRequired.end(),
        "submit_execution_manifest schema omitted or mistyped the AG-18.3 "
        "verificationBootstrapPending field");

    // AG-18.3: the top-level bootstrapDeclaration is required as a key,
    // nullable object, matching the same strict-mode pattern acceptanceContract
    // itself already uses.
    const auto& manifestProperties = manifestSchema.at("properties");
    const auto& manifestRequired = manifestSchema.at("required");
    req(manifestProperties.contains("bootstrapDeclaration"),
        "submit_execution_manifest schema omitted the AG-18.3 bootstrapDeclaration field");
    req(std::find(manifestRequired.begin(), manifestRequired.end(), "bootstrapDeclaration")
            != manifestRequired.end(),
        "submit_execution_manifest schema did not require the AG-18.3 bootstrapDeclaration key");
    const auto& bootstrapDeclarationType =
        manifestProperties.at("bootstrapDeclaration").at("type");
    req(bootstrapDeclarationType.is_array() && bootstrapDeclarationType.size() == 2
            && std::find(bootstrapDeclarationType.begin(), bootstrapDeclarationType.end(),
                   "object") != bootstrapDeclarationType.end()
            && std::find(bootstrapDeclarationType.begin(), bootstrapDeclarationType.end(),
                   "null") != bootstrapDeclarationType.end(),
        "bootstrapDeclaration must be typed [\"object\",\"null\"]");
    const auto& bootstrapDeclarationProperties =
        manifestProperties.at("bootstrapDeclaration").at("properties");
    req(bootstrapDeclarationProperties.contains("bootstrapTaskId")
            && bootstrapDeclarationProperties.contains("setupPaths")
            && manifestProperties.at("bootstrapDeclaration").at("additionalProperties") == false,
        "bootstrapDeclaration must carry exactly bootstrapTaskId/setupPaths with no extras");

    const auto noChangesDef = ai::application::submitNoChangesNeededToolDefinition();
    req(noChangesDef.strictInputSchema,
        "submit_no_changes_needed must declare strictInputSchema");
    const auto noChangesSchema = nlohmann::json::parse(noChangesDef.inputSchemaJson);
    const auto noChangesValid =
        ai::providers::validateStrictJsonSchema(noChangesSchema, noChangesDef.name);
    req(noChangesValid.success(), "submit_no_changes_needed schema is not strict-compatible");
}

// The real propose_change_set contract (not the Planner's terminal schemas
// above): proves it declares strictInputSchema, passes the same recursive
// validator both provider adapters fail-fast against, has the exact
// proposal-submission.v2 top-level/per-change field sets, and - the actual
// point of the strict propose_change_set contract package - never has a
// hash field anywhere in its wire shape, so a model is physically unable to
// send expected_original_hash through this schema at all.
void testProposeChangeSetSchemaIsStrictCompatible()
{
    const auto def = ai::application::proposeChangeSetToolDefinition();
    req(def.strictInputSchema, "propose_change_set must declare strictInputSchema");
    req(def.version == "2", "propose_change_set tool version must match submission v2");
    const auto schema = nlohmann::json::parse(def.inputSchemaJson);
    const auto valid = ai::providers::validateStrictJsonSchema(schema, def.name);
    if (!valid.success()) {
        throw std::runtime_error(
            "propose_change_set schema is not strict-compatible: " + valid.error()->message);
    }

    req(schema.at("additionalProperties").get<bool>() == false,
        "propose_change_set root must set additionalProperties:false");
    const auto& required = schema.at("required");
    req(required.size() == 3, "propose_change_set root required must have exactly 3 fields");
    for (const char* f : {"schemaVersion", "summary", "changes"}) {
        req(std::find(required.begin(), required.end(), f) != required.end(),
            "propose_change_set root required is missing a mandated field");
    }

    const auto& itemSchema = schema.at("properties").at("changes").at("items");
    req(itemSchema.at("additionalProperties").get<bool>() == false,
        "propose_change_set change items must set additionalProperties:false");
    const auto& itemRequired = itemSchema.at("required");
    req(itemRequired.size() == 5,
        "propose_change_set change items required must have exactly 5 fields");
    for (const char* f : {"change_id", "operation", "relative_path", "source_path", "proposed_content"}) {
        req(std::find(itemRequired.begin(), itemRequired.end(), f) != itemRequired.end(),
            "propose_change_set change items required is missing a mandated field");
    }
    req(!itemSchema.at("properties").contains("expected_original_hash"),
        "propose_change_set change items must never carry a hash field");
    req(schema.at("properties").at("summary").at("maxLength")
            == ai::application::kProposalSubmissionMaxSummaryBytes,
        "summary schema bound drifted from the Backend parser");
    req(schema.at("properties").at("changes").at("maxItems")
            == ai::application::kProposalSubmissionMaxChanges,
        "changes schema bound drifted from the Backend parser");
    req(itemSchema.at("properties").at("change_id").at("maxLength")
            == ai::application::kProposalSubmissionMaxChangeIdBytes,
        "change_id schema bound drifted from the Backend parser");
    req(itemSchema.at("properties").at("relative_path").at("maxLength")
            == ai::application::kProposalSubmissionMaxPathBytes,
        "relative_path schema bound drifted from the Backend parser");
    req(itemSchema.at("properties").at("source_path").at("maxLength")
            == ai::application::kProposalSubmissionMaxPathBytes,
        "source_path schema bound drifted from the Backend parser");
    req(itemSchema.at("properties").at("proposed_content").at("maxLength")
            == ai::application::kProposalSubmissionMaxContentBytes,
        "proposed_content schema bound drifted from the Backend parser");
}

void testRequestScopeExpansionSchemaIsStrictCompatible()
{
    const auto def = ai::application::requestScopeExpansionToolDefinition();
    req(def.strictInputSchema,
        "request_scope_expansion must declare strictInputSchema");
    const auto schema = nlohmann::json::parse(def.inputSchemaJson);
    const auto valid = ai::providers::validateStrictJsonSchema(schema, def.name);
    if (!valid.success()) {
        throw std::runtime_error(
            "request_scope_expansion schema is not strict-compatible: "
            + valid.error()->message);
    }
    const auto& required = schema.at("required");
    req(required.size() == 3,
        "request_scope_expansion must require exactly path, role, and reason");
    for (const char* field : {"relativePath", "role", "reason"}) {
        req(std::find(required.begin(), required.end(), field) != required.end(),
            "request_scope_expansion is missing a required field");
    }
}

void testMissingAdditionalPropertiesFalseIsRejected()
{
    const auto schema = nlohmann::json::parse(
        R"({"type":"object","properties":{"a":{"type":"string"}},"required":["a"]})");
    const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
    req(!result.success(), "object schema without additionalProperties:false was accepted");
    req(result.error()->code == "agent_session.malformed_strict_schema",
        "wrong error code for missing additionalProperties:false");
}

void testPropertyMissingFromRequiredIsRejected()
{
    const auto schema = nlohmann::json::parse(
        R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},)"
        R"("required":["a"],"additionalProperties":false})");
    const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
    req(!result.success(), "object schema with an optional (non-required) property was accepted");
}

void testConditionalKeywordsAreRejected()
{
    for (const char* keyword : {"allOf", "anyOf", "oneOf", "if", "not"}) {
        nlohmann::json schema = {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"required", nlohmann::json::array()},
            {"additionalProperties", false},
        };
        schema[keyword] = nlohmann::json::array();
        const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
        req(!result.success(),
            (std::string("schema containing ") + keyword + " was accepted").c_str());
    }
}

void testRecursionReachesNestedArrayItemsAndObjectProperties()
{
    // A violation buried three levels deep (array of objects, each with a
    // nested object property that itself is missing additionalProperties)
    // must still be caught - proves the walk is genuinely recursive, not
    // just checking the top-level object.
    const auto schema = nlohmann::json::parse(R"({
        "type":"object",
        "properties":{
            "items":{"type":"array","items":{"type":"object","properties":{
                "nested":{"type":"object","properties":{"x":{"type":"string"}},"required":["x"]}
            },"required":["nested"],"additionalProperties":false}}
        },
        "required":["items"],"additionalProperties":false
    })");
    const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
    req(!result.success(), "deeply nested missing additionalProperties:false was not caught");
    req(result.error()->message.find("nested") != std::string::npos,
        "recursive violation location did not identify the nested property");
}

void testNullableTypeArrayIsAcceptedAsObjectSchema()
{
    // The exact pattern used for acceptanceContract: type:["object","null"]
    // with a full properties/required/additionalProperties body must still
    // pass - a nullable object is not the same as "no object schema here".
    const auto schema = nlohmann::json::parse(R"({
        "type":"object",
        "properties":{
            "acceptanceContract":{"type":["object","null"],"properties":{
                "schemaVersion":{"type":"string","const":"execution-acceptance.v1"},
                "requirements":{"type":"array","items":{"type":"string"}}
            },"required":["schemaVersion","requirements"],"additionalProperties":false}
        },
        "required":["acceptanceContract"],"additionalProperties":false
    })");
    const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
    req(result.success(), "a valid nullable object schema was rejected");
}

// Reproduces a real OpenAI HTTP 400 ("schema must have a 'type' key"): a
// const-only or enum-only property schema is not automatically typed, even
// though its allowed values make the type obvious to a human reader. This
// is the exact rule this validator's first version did not check.
void testConstOrEnumOnlySchemaWithoutTypeIsRejected()
{
    {
        const auto schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"schemaVersion":{"const":"v1"}},)"
            R"("required":["schemaVersion"],"additionalProperties":false})");
        const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
        req(!result.success(), "a const-only property schema without type was accepted");
        req(result.error()->code == "agent_session.malformed_strict_schema",
            "wrong error code for a const-only property schema without type");
    }
    {
        const auto schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"kind":{"enum":["a","b"]}},)"
            R"("required":["kind"],"additionalProperties":false})");
        const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
        req(!result.success(), "an enum-only property schema without type was accepted");
    }
    {
        // Adding an explicit type alongside const/enum is exactly what
        // fixes it - proves the rule targets the missing key, not const/enum
        // themselves.
        const auto schema = nlohmann::json::parse(
            R"({"type":"object","properties":{)"
            R"("schemaVersion":{"type":"string","const":"v1"},)"
            R"("kind":{"type":"string","enum":["a","b"]}},)"
            R"("required":["schemaVersion","kind"],"additionalProperties":false})");
        const auto result = ai::providers::validateStrictJsonSchema(schema, "t");
        req(result.success(), "const/enum with an explicit type was incorrectly rejected");
    }
}

} // namespace

int runStrictSchemaContractTests()
{
    testPlannerTerminalSchemasAreStrictCompatible();
    testRequestScopeExpansionSchemaIsStrictCompatible();
    testProposeChangeSetSchemaIsStrictCompatible();
    testMissingAdditionalPropertiesFalseIsRejected();
    testPropertyMissingFromRequiredIsRejected();
    testConditionalKeywordsAreRejected();
    testRecursionReachesNestedArrayItemsAndObjectProperties();
    testNullableTypeArrayIsAcceptedAsObjectSchema();
    testConstOrEnumOnlySchemaWithoutTypeIsRejected();
    return 0;
}
