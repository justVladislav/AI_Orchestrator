#include "ai/application/execution/ExecutionManifestDraftTypes.hpp"

namespace ai::application {

const char* toString(ManifestDraftCapability capability) noexcept
{
    switch (capability) {
    case ManifestDraftCapability::Inspect: return "inspect";
    case ManifestDraftCapability::Propose: return "propose";
    }
    return "inspect";
}

std::optional<ManifestDraftCapability> manifestDraftCapabilityFromString(
    std::string_view value) noexcept
{
    if (value == "inspect") return ManifestDraftCapability::Inspect;
    if (value == "propose") return ManifestDraftCapability::Propose;
    return std::nullopt;
}

} // namespace ai::application
