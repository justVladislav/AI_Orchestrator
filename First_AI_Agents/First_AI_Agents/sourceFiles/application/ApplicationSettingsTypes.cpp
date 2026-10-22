#include "ai/application/ApplicationSettingsTypes.hpp"

namespace ai::application {

std::string_view toString(ThemePreference value) noexcept
{
    switch (value) {
    case ThemePreference::Light: return "light";
    case ThemePreference::Dark: return "dark";
    case ThemePreference::Transparent: return "transparent";
    }
    return "dark";
}

std::optional<ThemePreference> themePreferenceFromString(std::string_view value) noexcept
{
    if (value == "light") return ThemePreference::Light;
    if (value == "dark") return ThemePreference::Dark;
    if (value == "transparent") return ThemePreference::Transparent;
    return std::nullopt;
}

std::string_view toString(ProjectContextPolicy value) noexcept
{
    switch (value) {
    case ProjectContextPolicy::ActiveProject: return "active_project";
    case ProjectContextPolicy::AskEveryTime: return "ask_every_time";
    case ProjectContextPolicy::NoProject: return "no_project";
    }
    return "active_project";
}

std::optional<ProjectContextPolicy> projectContextPolicyFromString(
    std::string_view value) noexcept
{
    if (value == "active_project") return ProjectContextPolicy::ActiveProject;
    if (value == "ask_every_time") return ProjectContextPolicy::AskEveryTime;
    if (value == "no_project") return ProjectContextPolicy::NoProject;
    return std::nullopt;
}

} // namespace ai::application
