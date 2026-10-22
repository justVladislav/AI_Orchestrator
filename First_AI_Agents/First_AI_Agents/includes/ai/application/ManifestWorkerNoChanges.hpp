#pragma once

#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kManifestWorkerNoChangesSchemaVersion =
    "manifest-worker-no-changes.v1";

enum class ManifestWorkerNoChangesReason {
    AlreadySatisfiedInBaseline,
    SatisfiedByDependencyProposals,
};

[[nodiscard]] const char* toString(ManifestWorkerNoChangesReason value) noexcept;
[[nodiscard]] std::optional<ManifestWorkerNoChangesReason>
manifestWorkerNoChangesReasonFromString(std::string_view value) noexcept;

struct ManifestWorkerCriterionEvidence {
    std::string requirementId;
    std::string inspectedPath;
    std::string observation;
};

struct ManifestWorkerNoChanges {
    std::string schemaVersion {std::string(kManifestWorkerNoChangesSchemaVersion)};
    ManifestWorkerNoChangesReason reason {
        ManifestWorkerNoChangesReason::AlreadySatisfiedInBaseline};
    std::string summary;
    std::vector<ManifestWorkerCriterionEvidence> criterionEvidence;
    // Backend-derived from successful read-only calls in this exact provider
    // attempt. Model JSON cannot populate or override it.
    std::vector<std::string> inspectedRelativePaths;
};

struct ParseManifestWorkerNoChangesRequest {
    std::string_view rawJson;
    std::vector<std::string> requiredRequirementIds;
    std::unordered_map<std::string, std::vector<std::string>>
        requiredOutputsByRequirement;
    std::vector<std::string> sameAttemptInspectedRelativePaths;
    std::vector<std::string> sameAttemptKnownMissingRelativePaths;
    bool dependencyProposalContextPresent {false};
};

[[nodiscard]] common::Result<ManifestWorkerNoChanges>
parseManifestWorkerNoChanges(const ParseManifestWorkerNoChangesRequest& request);

} // namespace ai::application
