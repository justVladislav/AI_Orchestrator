#pragma once

// Structural validation for a tool's input_schema against the strict/
// structured-output JSON Schema subset shared by OpenAI (Responses API
// "strict": true) and Anthropic (Messages API "strict": true). Both
// providers document the same core constraints: every object requires
// "additionalProperties": false and every one of its own "properties" must
// appear in "required" (logically optional values are expressed as a
// nullable type, e.g. ["string", "null"], never by omission from
// "required"); conditional composition keywords (allOf/anyOf/oneOf/if/then/
// else/not) are not part of either provider's supported subset; and every
// schema node requires an explicit "type" key, even one that is already
// unambiguous via "const" or "enum" alone - discovered from a real OpenAI
// HTTP 400 ("schema must have a 'type' key") that this validator's first
// version did not check, because it only enforced the rules this codebase's
// own prompt had written down, not the provider's actual full requirement.
//
// This check is purely structural: it walks the schema's own shape (key
// names defined by JSON Schema itself, plus this codebase's own property
// names) and never inspects, stores, or echoes any submitted value.

#include "ai/common/Result.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace ai::providers {

namespace strict_schema_detail {

inline common::Result<void> fail(const std::string& location, const char* reason)
{
    return common::Result<void>::failure(
        {"agent_session.malformed_strict_schema",
         std::string("Strict schema rejected at ") + location + ": " + reason});
}

inline common::Result<void> validateNode(const nlohmann::json& node, const std::string& location)
{
    if (!node.is_object()) return common::Result<void>::ok();

    if (!node.contains("type")) {
        return fail(location, "schema must have an explicit 'type' key");
    }

    for (const char* unsupported : {"allOf", "anyOf", "oneOf", "if", "then", "else", "not"}) {
        if (node.contains(unsupported)) {
            return fail(location,
                "conditional/composition keywords are not supported by strict schemas");
        }
    }

    const bool looksLikeObjectSchema = node.contains("properties")
        || (node.contains("type") && node.at("type").is_string()
            && node.at("type").get<std::string>() == "object");
    if (looksLikeObjectSchema) {
        if (!node.contains("additionalProperties") || !node.at("additionalProperties").is_boolean()
            || node.at("additionalProperties").get<bool>() != false) {
            return fail(location, "object schemas require additionalProperties: false");
        }
        const nlohmann::json emptyProps = nlohmann::json::object();
        const auto& properties = node.contains("properties") ? node.at("properties") : emptyProps;
        if (!properties.is_object()) {
            return fail(location, "properties must be an object");
        }
        std::size_t requiredCount = 0;
        if (node.contains("required")) {
            if (!node.at("required").is_array()) {
                return fail(location, "required must be an array when present");
            }
            requiredCount = node.at("required").size();
        }
        if (!properties.empty() && requiredCount != properties.size()) {
            return fail(location,
                "every property must be listed in required for a strict object schema");
        }
        if (node.contains("required")) {
            for (const auto& name : node.at("required")) {
                if (!name.is_string() || !properties.contains(name.get<std::string>())) {
                    return fail(location, "required lists a name outside properties");
                }
            }
        }
        for (const auto& [propertyName, propertySchema] : properties.items()) {
            const auto nested =
                validateNode(propertySchema, location + "." + propertyName);
            if (!nested.success()) return nested;
        }
    }

    if (node.contains("items")) {
        const auto nested = validateNode(node.at("items"), location + "[]");
        if (!nested.success()) return nested;
    }

    return common::Result<void>::ok();
}

} // namespace strict_schema_detail

// location is a caller-supplied label (e.g. the tool name) used only to
// build a Backend-safe diagnostic prefix; it must never contain provider or
// user content.
[[nodiscard]] inline common::Result<void> validateStrictJsonSchema(
    const nlohmann::json& schema, const std::string& toolNameLocation)
{
    return strict_schema_detail::validateNode(schema, toolNameLocation);
}

} // namespace ai::providers
