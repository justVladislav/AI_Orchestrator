#include "ai/application/RunWorkspaceScopeTypes.hpp"

namespace ai::application {

const char* toString(WorkspaceScopeRole value) noexcept
{
    switch (value) {
    case WorkspaceScopeRole::OwnedWrite: return "owned_write";
    case WorkspaceScopeRole::PlannedCreate: return "planned_create";
    case WorkspaceScopeRole::RequiredRead: return "required_read";
    case WorkspaceScopeRole::ReferenceOnly: return "reference_only";
    case WorkspaceScopeRole::Excluded: return "excluded";
    case WorkspaceScopeRole::Deferred: return "deferred";
    }
    return "reference_only";
}

std::optional<WorkspaceScopeRole> workspaceScopeRoleFromString(std::string_view value) noexcept
{
    if (value == "owned_write") return WorkspaceScopeRole::OwnedWrite;
    if (value == "planned_create") return WorkspaceScopeRole::PlannedCreate;
    if (value == "required_read") return WorkspaceScopeRole::RequiredRead;
    if (value == "reference_only") return WorkspaceScopeRole::ReferenceOnly;
    if (value == "excluded") return WorkspaceScopeRole::Excluded;
    if (value == "deferred") return WorkspaceScopeRole::Deferred;
    return std::nullopt;
}

const char* toString(WorkspaceScopeOrigin value) noexcept
{
    switch (value) {
    case WorkspaceScopeOrigin::FrozenInitial: return "frozen_initial";
    case WorkspaceScopeOrigin::ApprovedExpansion: return "approved_expansion";
    }
    return "frozen_initial";
}

std::optional<WorkspaceScopeOrigin> workspaceScopeOriginFromString(std::string_view value) noexcept
{
    if (value == "frozen_initial") return WorkspaceScopeOrigin::FrozenInitial;
    if (value == "approved_expansion") return WorkspaceScopeOrigin::ApprovedExpansion;
    return std::nullopt;
}

} // namespace ai::application
