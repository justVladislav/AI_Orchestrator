#include "ai/application/VerificationEnvironmentSnapshotTypes.hpp"

namespace ai::application {

const char* toString(VerificationEnvironmentState value) noexcept
{
    switch (value) {
    case VerificationEnvironmentState::ExistingProfile: return "existing_profile";
    case VerificationEnvironmentState::ProposedBootstrap: return "proposed_bootstrap";
    case VerificationEnvironmentState::SetupRequired: return "setup_required";
    }
    return "setup_required";
}

std::optional<VerificationEnvironmentState>
verificationEnvironmentStateFromString(std::string_view value) noexcept
{
    if (value == "existing_profile") return VerificationEnvironmentState::ExistingProfile;
    if (value == "proposed_bootstrap") return VerificationEnvironmentState::ProposedBootstrap;
    if (value == "setup_required") return VerificationEnvironmentState::SetupRequired;
    return std::nullopt;
}

const char* toString(VerificationPresetFileSource value) noexcept
{
    switch (value) {
    case VerificationPresetFileSource::ProjectPresets: return "project_presets";
    case VerificationPresetFileSource::UserPresets: return "user_presets";
    case VerificationPresetFileSource::Unknown: return "unknown";
    }
    return "unknown";
}

std::optional<VerificationPresetFileSource>
verificationPresetFileSourceFromString(std::string_view value) noexcept
{
    if (value == "project_presets") return VerificationPresetFileSource::ProjectPresets;
    if (value == "user_presets") return VerificationPresetFileSource::UserPresets;
    if (value == "unknown") return VerificationPresetFileSource::Unknown;
    return std::nullopt;
}

const char* toString(VerificationProfileOrigin value) noexcept
{
    switch (value) {
    case VerificationProfileOrigin::Discovered: return "discovered";
    case VerificationProfileOrigin::BackendConfigured: return "backend_configured";
    }
    return "discovered";
}

std::optional<VerificationProfileOrigin>
verificationProfileOriginFromString(std::string_view value) noexcept
{
    if (value == "discovered") return VerificationProfileOrigin::Discovered;
    if (value == "backend_configured") return VerificationProfileOrigin::BackendConfigured;
    return std::nullopt;
}

} // namespace ai::application
