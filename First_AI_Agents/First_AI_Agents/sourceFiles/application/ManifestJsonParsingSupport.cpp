#include "ai/application/ManifestJsonParsingSupport.hpp"

#include <limits>
#include <unordered_set>

namespace ai::application::manifest_parsing {

std::string fieldLocation(std::string_view location, const char* field)
{
    return location.empty() ? std::string(field)
                             : std::string(location) + "." + field;
}

common::Result<void> requireExactFields(
    const Json& value,
    std::string_view location,
    std::initializer_list<const char*> required,
    std::initializer_list<const char*> optional)
{
    if (!value.is_object()) {
        return common::Result<void>::failure(
            {"manifest.parse_error",
             "Manifest field must be a JSON object: "
                 + (location.empty() ? std::string("<root>") : std::string(location))});
    }

    std::unordered_set<std::string> allowed;
    for (const auto* field : required) allowed.emplace(field);
    for (const auto* field : optional) allowed.emplace(field);
    for (const auto& [key, unused] : value.items()) {
        static_cast<void>(unused);
        if (!allowed.contains(key)) {
            // Never echoes the unexpected key itself - only the Backend-known
            // parent location, so an attacker-controlled field name can never
            // reach a stored/displayed error message.
            return common::Result<void>::failure(
                {"manifest.unexpected_field",
                 "Manifest contains an unexpected field at "
                     + (location.empty() ? std::string("<root>") : std::string(location))});
        }
    }
    for (const auto* field : required) {
        if (!value.contains(field)) {
            return common::Result<void>::failure(
                {"manifest.missing_field",
                 "Manifest is missing a required field: " + fieldLocation(location, field)});
        }
    }
    return common::Result<void>::ok();
}

common::Result<std::string> readString(const Json& object, const char* field)
{
    if (!object.at(field).is_string()) {
        return fail<std::string>(
            "manifest.invalid_field", "Manifest field must be a string");
    }
    auto value = object.at(field).get<std::string>();
    if (value.size() > kMaximumStringBytes) {
        return fail<std::string>(
            "manifest.field_too_large", "Manifest string exceeds the parser byte limit");
    }
    return common::Result<std::string>::ok(std::move(value));
}

common::Result<std::uint64_t> readUnsigned(const Json& object, const char* field)
{
    const auto& value = object.at(field);
    if (!value.is_number_integer()) {
        return fail<std::uint64_t>(
            "manifest.invalid_field", "Manifest budget field must be an unsigned integer");
    }
    try {
        if (value.is_number_integer() && !value.is_number_unsigned()
            && value.get<std::int64_t>() < 0) {
            return fail<std::uint64_t>(
                "manifest.invalid_field", "Manifest budget field must not be negative");
        }
        return common::Result<std::uint64_t>::ok(value.get<std::uint64_t>());
    } catch (const std::exception&) {
        return fail<std::uint64_t>(
            "manifest.invalid_field", "Manifest unsigned integer is out of range");
    }
}

common::Result<std::uint32_t> readUint32(const Json& object, const char* field)
{
    const auto value = readUnsigned(object, field);
    if (!value.success()) return fail<std::uint32_t>(
        value.error()->code, value.error()->message);
    if (value.value() > std::numeric_limits<std::uint32_t>::max()) {
        return fail<std::uint32_t>(
            "manifest.invalid_field", "Manifest integer is out of range");
    }
    return common::Result<std::uint32_t>::ok(
        static_cast<std::uint32_t>(value.value()));
}

common::Result<std::vector<std::string>> readStringArray(
    const Json& object,
    const char* field)
{
    const auto& array = object.at(field);
    if (!array.is_array()) {
        return fail<std::vector<std::string>>(
            "manifest.invalid_field", "Manifest collection field must be an array");
    }
    if (array.size() > kMaximumItemsPerField) {
        return fail<std::vector<std::string>>(
            "manifest.field_too_large", "Manifest collection exceeds the parser item limit");
    }
    std::vector<std::string> result;
    result.reserve(array.size());
    for (const auto& item : array) {
        if (!item.is_string()) {
            return fail<std::vector<std::string>>(
                "manifest.invalid_field", "Manifest collection items must be strings");
        }
        auto text = item.get<std::string>();
        if (text.size() > kMaximumStringBytes) {
            return fail<std::vector<std::string>>(
                "manifest.field_too_large", "Manifest string exceeds the parser byte limit");
        }
        result.push_back(std::move(text));
    }
    return common::Result<std::vector<std::string>>::ok(std::move(result));
}

std::filesystem::path pathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

common::Result<std::string> validateRawScopePath(const std::string& value, std::string_view code)
{
    auto invalid = [&](const char* reason) {
        return fail<std::string>(std::string(code),
            std::string("Path rejected because ") + reason
                + "; only canonical workspace-relative paths are accepted");
    };
    if (value.empty()) return invalid("the path is empty");
    if (value.front() == '/' || value.find(':') != std::string::npos
        || value.starts_with("//")) {
        return invalid("the path is absolute or drive/UNC-qualified");
    }
    if (value.back() == '/') return invalid("the path has a trailing separator");
    if (value.find('\\') != std::string::npos) {
        return invalid("the path uses a non-canonical separator");
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto component = std::string_view(value).substr(begin,
            (end == std::string::npos ? value.size() : end) - begin);
        if (component.empty()) return invalid("the path contains an empty segment");
        if (component == ".") return invalid("the path contains a dot segment");
        if (component == "..") return invalid("the path contains parent traversal");
        for (const unsigned char ch : component) {
            if (ch < 0x20 || ch == 0x7f) {
                return invalid("the path contains a control character");
            }
            switch (ch) {
            case '*': case '?': case '[': case ']': case '{': case '}':
            case '<': case '>': case '|': case '"':
                return invalid("the path contains a glob or metacharacter");
            default: break;
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return common::Result<std::string>::ok(value);
}

common::Result<std::vector<ManifestExcludedOrDeferredPath>> parsePathReasonArray(
    const Json& object, const char* field, std::string_view invalidPathCode,
    const std::string& location)
{
    const auto& array = object.at(field);
    if (!array.is_array()) {
        return fail<std::vector<ManifestExcludedOrDeferredPath>>(
            "manifest.invalid_field", "Manifest collection field must be an array");
    }
    if (array.size() > kMaximumItemsPerField) {
        return fail<std::vector<ManifestExcludedOrDeferredPath>>(
            "manifest.field_too_large", "Manifest collection exceeds the parser item limit");
    }
    std::vector<ManifestExcludedOrDeferredPath> result;
    result.reserve(array.size());
    std::size_t index = 0;
    for (const auto& item : array) {
        const std::string itemLocation =
            location + "." + field + "[" + std::to_string(index) + "]";
        ++index;
        const auto itemFields = requireExactFields(item, itemLocation, {"path", "reason"});
        if (!itemFields.success()) return fail<std::vector<ManifestExcludedOrDeferredPath>>(
            itemFields.error()->code, itemFields.error()->message);
        auto rawPath = readString(item, "path");
        auto reason = readString(item, "reason");
        if (!rawPath.success()) return fail<std::vector<ManifestExcludedOrDeferredPath>>(
            rawPath.error()->code, rawPath.error()->message);
        if (!reason.success()) return fail<std::vector<ManifestExcludedOrDeferredPath>>(
            reason.error()->code, reason.error()->message);
        if (reason.value().empty() || reason.value().size() > kMaximumScopePathReasonBytes) {
            return fail<std::vector<ManifestExcludedOrDeferredPath>>(
                "manifest.invalid_field",
                "Excluded/deferred path reason must be nonblank and bounded: " + itemLocation);
        }
        const auto validated = validateRawScopePath(rawPath.value(), invalidPathCode);
        if (!validated.success()) return fail<std::vector<ManifestExcludedOrDeferredPath>>(
            validated.error()->code, validated.error()->message);
        result.push_back({pathFromUtf8(validated.value()), std::move(reason.value())});
    }
    return common::Result<std::vector<ManifestExcludedOrDeferredPath>>::ok(std::move(result));
}

common::Result<ExecutionAcceptanceContract> parseAcceptanceContract(
    const Json& value, const std::string& location)
{
    const auto fields = requireExactFields(value, location, {"schemaVersion", "requirements"});
    if (!fields.success()) return fail<ExecutionAcceptanceContract>(
        fields.error()->code, fields.error()->message);
    auto schema = readString(value, "schemaVersion");
    if (!schema.success()) return fail<ExecutionAcceptanceContract>(
        schema.error()->code, schema.error()->message);
    if (!value.at("requirements").is_array()
        || value.at("requirements").size() > kMaximumAcceptanceRequirements) {
        return fail<ExecutionAcceptanceContract>("acceptance.requirement_count",
            "Acceptance contract requirements must be a bounded array");
    }

    ExecutionAcceptanceContract contract;
    contract.schemaVersion = std::move(schema.value());
    std::size_t requirementIndex = 0;
    for (const auto& item : value.at("requirements")) {
        const std::string requirementLocation =
            location + ".requirements[" + std::to_string(requirementIndex) + "]";
        ++requirementIndex;
        const auto itemFields = requireExactFields(item, requirementLocation,
            {"id", "description", "requiredOutputs", "verificationKind",
             "verificationProfileId", "verificationBuildProfileId", "verificationTargetId",
             "verificationArguments", "required", "verificationBootstrapPending"});
        if (!itemFields.success()) return fail<ExecutionAcceptanceContract>(
            itemFields.error()->code, itemFields.error()->message);
        auto id = readString(item, "id");
        auto description = readString(item, "description");
        auto outputs = readStringArray(item, "requiredOutputs");
        auto kindText = readString(item, "verificationKind");
        if (!id.success() || !description.success() || !outputs.success()
            || !kindText.success() || !item.at("verificationArguments").is_array()
            || !item.at("required").is_boolean()
            || !item.at("verificationBootstrapPending").is_boolean()) {
            return fail<ExecutionAcceptanceContract>("acceptance.invalid_field",
                "Acceptance requirement field is missing or invalid: " + requirementLocation);
        }
        const auto kind = executionVerificationKindFromString(kindText.value());
        if (!kind.has_value()) {
            return fail<ExecutionAcceptanceContract>("acceptance.unknown_verification_kind",
                "Acceptance verification kind is not supported");
        }
        if (item.at("verificationArguments").size() > kMaximumVerificationArguments) {
            return fail<ExecutionAcceptanceContract>("acceptance.argument_limit",
                "Acceptance verification argument count exceeds the limit");
        }
        ExecutionAcceptanceRequirement requirement;
        requirement.id = std::move(id.value());
        requirement.description = std::move(description.value());
        requirement.requiredOutputs = std::move(outputs.value());
        requirement.verificationKind = *kind;
        requirement.required = item.at("required").get<bool>();
        requirement.verificationBootstrapPending =
            item.at("verificationBootstrapPending").get<bool>();
        // AG-18.2: stable, provider-selected catalog ids - never a raw
        // preset/buildPreset/buildDirectory/target value. Each is a
        // required key but a nullable value; only shape-checked here
        // (string or null, bounded) - whether a non-null value is actually
        // a known catalog entry is ManifestVerificationProfileBinder's job.
        for (const auto* field : {"verificationProfileId", "verificationBuildProfileId",
                 "verificationTargetId"}) {
            if (item.at(field).is_null()) continue;
            if (!item.at(field).is_string()) {
                return fail<ExecutionAcceptanceContract>("acceptance.invalid_field",
                    "Acceptance verification selection field must be a string or null: "
                        + requirementLocation);
            }
            auto value = readString(item, field);
            if (!value.success()) return fail<ExecutionAcceptanceContract>(
                value.error()->code, value.error()->message);
            if (std::string_view(field) == "verificationProfileId") {
                requirement.verificationProfileId = std::move(value.value());
            } else if (std::string_view(field) == "verificationBuildProfileId") {
                requirement.verificationBuildProfileId = std::move(value.value());
            } else {
                requirement.verificationTargetId = std::move(value.value());
            }
        }
        std::size_t argumentIndex = 0;
        for (const auto& argumentJson : item.at("verificationArguments")) {
            const std::string argumentLocation = requirementLocation
                + ".verificationArguments[" + std::to_string(argumentIndex) + "]";
            ++argumentIndex;
            const auto argumentFields =
                requireExactFields(argumentJson, argumentLocation, {"name", "value"});
            if (!argumentFields.success()) return fail<ExecutionAcceptanceContract>(
                argumentFields.error()->code, argumentFields.error()->message);
            auto name = readString(argumentJson, "name");
            auto argumentValue = readString(argumentJson, "value");
            if (!name.success() || !argumentValue.success()) {
                return fail<ExecutionAcceptanceContract>("acceptance.invalid_argument",
                    "Acceptance verification argument is invalid: " + argumentLocation);
            }
            requirement.verificationArguments.push_back(
                {std::move(name.value()), std::move(argumentValue.value())});
        }
        contract.requirements.push_back(std::move(requirement));
    }
    const auto valid = validateExecutionAcceptanceContract(contract);
    if (!valid.success()) return fail<ExecutionAcceptanceContract>(
        valid.error()->code, valid.error()->message);
    // AG-18.2: only the pre-binding DRAFT shape is checked here (which
    // selection ids the provider must/must not have supplied for this
    // kind) - verificationArguments is legitimately still unresolved
    // (empty, for cmake_build/qml_lint) at this point.
    // ManifestVerificationProfileBinder resolves selections into the final
    // verificationArguments later, immediately before manifests_.freeze(),
    // and re-validates the FINAL shape with
    // validateExecutionVerificationProfileArguments at that point.
    for (const auto& requirement : contract.requirements) {
        // Manifest verification reliability hardening: there is no
        // Backend-owned trusted catalog of admissible definition/
        // registration/target triples for cmake_target_consistency, so an
        // arbitrary provider submission must never be accepted as an
        // ordinary draft requirement - fail closed here, before this
        // requirement can ever reach freeze()/dispatch, even if the
        // provider bypassed the advertised tool schema (which no longer
        // advertises this kind at all). This is scoped to fresh submission
        // parsing only: already-persisted legacy Manifests are read via a
        // separate deserialization path (executionManifestFromJson) that
        // never calls this parser, and the internal verifier still knows
        // how to check a legacy/trusted cmake_target_consistency
        // requirement it already owns.
        if (requirement.verificationKind
            == ExecutionVerificationKind::CMakeTargetConsistency) {
            return fail<ExecutionAcceptanceContract>(
                "acceptance.untrusted_cmake_consistency_selection",
                "cmake_target_consistency requires a Backend-owned trusted catalog that does "
                "not yet exist; use cmake_build and/or qml_lint against a real catalog profile "
                "instead");
        }
        const auto draft = validateExecutionVerificationDraftSelections(requirement);
        if (!draft.success()) return fail<ExecutionAcceptanceContract>(
            draft.error()->code, draft.error()->message);
    }
    return common::Result<ExecutionAcceptanceContract>::ok(std::move(contract));
}

} // namespace ai::application::manifest_parsing
