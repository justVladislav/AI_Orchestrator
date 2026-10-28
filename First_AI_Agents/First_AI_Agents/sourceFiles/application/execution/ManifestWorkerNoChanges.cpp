#include "ai/application/execution/ManifestWorkerNoChanges.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ai::application {

namespace {

constexpr std::size_t kMaximumPayloadBytes = 256 * 1024;
constexpr std::size_t kMaximumEvidenceItems = 64;
constexpr std::size_t kMaximumStringBytes = 4096;

bool blank(std::string_view value)
{
    return value.empty() || std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

std::string normalizedPath(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    while (value.starts_with("./")) value.erase(0, 2);
    return value;
}

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

} // namespace

const char* toString(ManifestWorkerNoChangesReason value) noexcept
{
    switch (value) {
    case ManifestWorkerNoChangesReason::AlreadySatisfiedInBaseline:
        return "already_satisfied_in_baseline";
    case ManifestWorkerNoChangesReason::SatisfiedByDependencyProposals:
        return "satisfied_by_dependency_proposals";
    }
    return "already_satisfied_in_baseline";
}

std::optional<ManifestWorkerNoChangesReason>
manifestWorkerNoChangesReasonFromString(std::string_view value) noexcept
{
    if (value == "already_satisfied_in_baseline")
        return ManifestWorkerNoChangesReason::AlreadySatisfiedInBaseline;
    if (value == "satisfied_by_dependency_proposals")
        return ManifestWorkerNoChangesReason::SatisfiedByDependencyProposals;
    return std::nullopt;
}

common::Result<ManifestWorkerNoChanges> parseManifestWorkerNoChanges(
    const ParseManifestWorkerNoChangesRequest& request)
{
    using json = nlohmann::json;
    if (request.rawJson.size() > kMaximumPayloadBytes) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.output_too_large",
            "Worker no-change submission exceeds the payload limit");
    }

    json object;
    try {
        object = json::parse(request.rawJson);
    } catch (const std::exception&) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.parse_error",
            "Worker no-change submission is not valid JSON");
    }
    if (!object.is_object()) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.invalid_shape",
            "Worker no-change submission must be an object");
    }
    static const std::unordered_set<std::string> exactFields = {
        "schemaVersion", "reason", "summary", "criterionEvidence"};
    bool unexpectedField = object.size() != exactFields.size();
    for (const auto& [key, unused] : object.items()) {
        static_cast<void>(unused);
        if (!exactFields.contains(key)) unexpectedField = true;
    }
    if (unexpectedField) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.unexpected_field",
            "Worker no-change submission fields do not match the schema");
    }
    for (const auto* field : {"schemaVersion", "reason", "summary"}) {
        if (!object.contains(field) || !object.at(field).is_string()) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.invalid_field",
                "Worker no-change string field is missing or invalid");
        }
        if (object.at(field).get_ref<const std::string&>().size() > kMaximumStringBytes) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.field_too_large",
                "Worker no-change string exceeds the field limit");
        }
    }
    if (!object.contains("criterionEvidence")
        || !object.at("criterionEvidence").is_array()
        || object.at("criterionEvidence").empty()
        || object.at("criterionEvidence").size() > kMaximumEvidenceItems) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.invalid_evidence",
            "Worker no-change evidence count is outside the supported range");
    }

    ManifestWorkerNoChanges result;
    result.schemaVersion = object.at("schemaVersion").get<std::string>();
    if (result.schemaVersion != kManifestWorkerNoChangesSchemaVersion) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.unsupported_schema",
            "Worker no-change schema version is not supported");
    }
    const auto reason = manifestWorkerNoChangesReasonFromString(
        object.at("reason").get_ref<const std::string&>());
    if (!reason.has_value()) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.invalid_reason",
            "Worker no-change reason is not supported");
    }
    if (*reason == ManifestWorkerNoChangesReason::SatisfiedByDependencyProposals
        && !request.dependencyProposalContextPresent) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.missing_dependency_context",
            "Dependency-proposal satisfaction requires dependency proposal context");
    }
    result.reason = *reason;
    result.summary = object.at("summary").get<std::string>();
    if (blank(result.summary)) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.empty_summary",
            "Worker no-change summary must be nonblank");
    }

    std::unordered_set<std::string> inspected;
    for (const auto& path : request.sameAttemptInspectedRelativePaths)
        inspected.emplace(normalizedPath(path));
    if (inspected.empty()) {
        return fail<ManifestWorkerNoChanges>("worker_no_changes.missing_inspection",
            "Worker no-change submission requires a successful same-attempt inspection");
    }
    result.inspectedRelativePaths.assign(inspected.begin(), inspected.end());
    std::sort(result.inspectedRelativePaths.begin(), result.inspectedRelativePaths.end());

    std::unordered_set<std::string> evidencedRequirements;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        evidencedOutputsByRequirement;
    std::unordered_set<std::string> knownMissing;
    for (const auto& path : request.sameAttemptKnownMissingRelativePaths)
        knownMissing.emplace(normalizedPath(path));
    for (const auto& item : object.at("criterionEvidence")) {
        if (!item.is_object() || item.size() != 3
            || !item.contains("requirementId") || !item.at("requirementId").is_string()
            || !item.contains("inspectedPath") || !item.at("inspectedPath").is_string()
            || !item.contains("observation") || !item.at("observation").is_string()) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.invalid_evidence",
                "Worker no-change evidence item does not match the schema");
        }
        ManifestWorkerCriterionEvidence evidence;
        evidence.requirementId = item.at("requirementId").get<std::string>();
        evidence.inspectedPath = item.at("inspectedPath").get<std::string>();
        evidence.observation = item.at("observation").get<std::string>();
        if (blank(evidence.requirementId) || blank(evidence.inspectedPath)
            || blank(evidence.observation)
            || evidence.requirementId.size() > kMaximumStringBytes
            || evidence.inspectedPath.size() > kMaximumStringBytes
            || evidence.observation.size() > kMaximumStringBytes) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.invalid_evidence",
                "Worker no-change evidence strings must be nonblank and bounded");
        }
        const auto evidencePath = normalizedPath(evidence.inspectedPath);
        if (knownMissing.contains(evidencePath)) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.required_output_missing",
                "Worker no-change evidence identifies a missing required output");
        }
        if (!inspected.contains(evidencePath)) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.uninspected_evidence_path",
                "Worker no-change evidence path was not inspected in the same attempt");
        }
        const auto requiredOutputs = request.requiredOutputsByRequirement.find(
            evidence.requirementId);
        if (requiredOutputs != request.requiredOutputsByRequirement.end()
            && !requiredOutputs->second.empty()
            && std::none_of(requiredOutputs->second.begin(), requiredOutputs->second.end(),
                [&](const std::string& output) {
                    return normalizedPath(output) == evidencePath;
                })) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.unmapped_output",
                "Worker no-change evidence does not inspect a required output for its criterion");
        }
        evidencedRequirements.emplace(evidence.requirementId);
        evidencedOutputsByRequirement[evidence.requirementId].emplace(evidencePath);
        result.criterionEvidence.push_back(std::move(evidence));
    }
    for (const auto& id : request.requiredRequirementIds) {
        if (!evidencedRequirements.contains(id)) {
            return fail<ManifestWorkerNoChanges>("worker_no_changes.unmapped_requirement",
                "A required acceptance requirement has no no-change evidence");
        }
        const auto requiredOutputs = request.requiredOutputsByRequirement.find(id);
        if (requiredOutputs == request.requiredOutputsByRequirement.end()) continue;
        const auto evidenced = evidencedOutputsByRequirement.find(id);
        for (const auto& output : requiredOutputs->second) {
            if (evidenced == evidencedOutputsByRequirement.end()
                || !evidenced->second.contains(normalizedPath(output))) {
                return fail<ManifestWorkerNoChanges>(
                    "worker_no_changes.required_output_uninspected",
                    "Every required inspect_paths output needs its own successful "
                    "same-attempt evidence item before a no-change outcome");
            }
        }
    }
    return common::Result<ManifestWorkerNoChanges>::ok(std::move(result));
}

} // namespace ai::application
