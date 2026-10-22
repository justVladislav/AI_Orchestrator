#include "ai/application/ExecutionManifestTypes.hpp"

namespace ai::application {

const char* toString(ManifestRoutingMode value) noexcept
{
    switch (value) {
    case ManifestRoutingMode::SingleAssignment: return "single_assignment";
    case ManifestRoutingMode::Roster: return "roster";
    }
    return "single_assignment";
}

std::optional<ManifestRoutingMode> manifestRoutingModeFromString(
    std::string_view value) noexcept
{
    if (value == "single_assignment") return ManifestRoutingMode::SingleAssignment;
    if (value == "roster") return ManifestRoutingMode::Roster;
    return std::nullopt;
}

const char* toString(MandatoryVerificationDecisionStatus value) noexcept
{
    switch (value) {
    case MandatoryVerificationDecisionStatus::NotTriggered: return "not_triggered";
    case MandatoryVerificationDecisionStatus::Injected: return "injected";
    case MandatoryVerificationDecisionStatus::SatisfiedByExisting: return "satisfied_by_existing";
    case MandatoryVerificationDecisionStatus::NotApplicable: return "not_applicable";
    }
    return "not_triggered";
}

std::optional<MandatoryVerificationDecisionStatus>
mandatoryVerificationDecisionStatusFromString(std::string_view value) noexcept
{
    if (value == "not_triggered") return MandatoryVerificationDecisionStatus::NotTriggered;
    if (value == "injected") return MandatoryVerificationDecisionStatus::Injected;
    if (value == "satisfied_by_existing")
        return MandatoryVerificationDecisionStatus::SatisfiedByExisting;
    if (value == "not_applicable") return MandatoryVerificationDecisionStatus::NotApplicable;
    return std::nullopt;
}

const char* toString(WorkspaceCapability capability) noexcept
{
    switch (capability) {
    case WorkspaceCapability::None: return "none";
    case WorkspaceCapability::Inspect: return "inspect";
    case WorkspaceCapability::Propose: return "propose";
    }
    return "none";
}

std::optional<WorkspaceCapability> workspaceCapabilityFromString(
    std::string_view value) noexcept
{
    if (value == "none") return WorkspaceCapability::None;
    if (value == "inspect") return WorkspaceCapability::Inspect;
    if (value == "propose") return WorkspaceCapability::Propose;
    return std::nullopt;
}

} // namespace ai::application
