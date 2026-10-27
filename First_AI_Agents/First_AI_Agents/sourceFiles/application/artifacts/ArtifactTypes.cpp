#include "ai/application/artifacts/ArtifactTypes.hpp"

namespace ai::application {

const char* toString(ArtifactKind kind) noexcept
{
    switch (kind) {
    case ArtifactKind::ContextSnapshot: return "context_snapshot";
    case ArtifactKind::Report: return "report";
    case ArtifactKind::Patch: return "patch";
    case ArtifactKind::Source: return "source";
    case ArtifactKind::Log: return "log";
    case ArtifactKind::Other: return "other";
    }
    return "unknown";
}

std::optional<ArtifactKind> artifactKindFromString(std::string_view value) noexcept
{
    if (value == "context_snapshot") return ArtifactKind::ContextSnapshot;
    if (value == "report") return ArtifactKind::Report;
    if (value == "patch") return ArtifactKind::Patch;
    if (value == "source") return ArtifactKind::Source;
    if (value == "log") return ArtifactKind::Log;
    if (value == "other") return ArtifactKind::Other;
    return std::nullopt;
}

} // namespace ai::application
