#pragma once

#include "ai/application/ExecutionProfileTypes.hpp"
#include "ai/application/ProviderCatalogTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ai::application {

enum class ThemePreference { Light, Dark, Transparent };
enum class ProjectContextPolicy { ActiveProject, AskEveryTime, NoProject };

[[nodiscard]] std::string_view toString(ThemePreference value) noexcept;
[[nodiscard]] std::optional<ThemePreference> themePreferenceFromString(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view toString(ProjectContextPolicy value) noexcept;
[[nodiscard]] std::optional<ProjectContextPolicy> projectContextPolicyFromString(
    std::string_view value) noexcept;

struct ApplicationSettings {
    ThemePreference theme {ThemePreference::Dark};
    // Zero means responsive/automatic. Explicit values are percentages.
    std::uint32_t interfaceScalePercent {0};
    bool compactMode {false};
    bool reduceAnimations {false};

    ProjectContextPolicy runContextPolicy {ProjectContextPolicy::ActiveProject};
    std::optional<ExecutionProfileId> defaultExecutionProfileId;
    std::uint64_t defaultTimeoutMs {300'000};
    std::uint32_t defaultMaxParallelAgents {4};
    bool confirmManyModels {true};

    ProjectContextPolicy chatContextPolicy {ProjectContextPolicy::ActiveProject};
    std::optional<ProviderId> defaultChatProviderId;
    std::optional<ModelId> defaultChatModelId;
    std::uint32_t chatHistoryLimit {200};

    std::uint64_t revision {0};
    std::chrono::system_clock::time_point updatedAt {};
};

struct UpdateApplicationSettingsRequest {
    ThemePreference theme {ThemePreference::Dark};
    std::uint32_t interfaceScalePercent {0};
    bool compactMode {false};
    bool reduceAnimations {false};
    ProjectContextPolicy runContextPolicy {ProjectContextPolicy::ActiveProject};
    std::optional<ExecutionProfileId> defaultExecutionProfileId;
    std::uint64_t defaultTimeoutMs {300'000};
    std::uint32_t defaultMaxParallelAgents {4};
    bool confirmManyModels {true};
    ProjectContextPolicy chatContextPolicy {ProjectContextPolicy::ActiveProject};
    std::optional<ProviderId> defaultChatProviderId;
    std::optional<ModelId> defaultChatModelId;
    std::uint32_t chatHistoryLimit {200};
};

} // namespace ai::application
