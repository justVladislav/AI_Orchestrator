#include "ai/application/catalog/ExecutionProfileService.hpp"

#include "ai/application/execution/ExecutionAssignmentValidation.hpp"
#include "ai/application/IdGenerator.hpp"

#include <chrono>
#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

ExecutionProfileService::ExecutionProfileService(
    IExecutionProfileRepository& profiles,
    IProviderRepository& providers,
    IModelRepository& models)
    : profiles_(profiles)
    , providers_(providers)
    , models_(models)
{
}

common::Result<std::vector<ExecutionProfile>> ExecutionProfileService::listProfiles() const
{
    return profiles_.listAll();
}

common::Result<std::optional<ExecutionProfile>> ExecutionProfileService::findProfile(
    const ExecutionProfileId& id) const
{
    return profiles_.findById(id);
}

common::Result<void> ExecutionProfileService::validateAssignments(
    const std::vector<StageModelAssignment>& assignments) const
{
    // Delegates to the shared validator also used by WorkflowRouter's
    // resolve-time re-check, so both call sites reject exactly the same
    // conditions with exactly the same error codes.
    return validateExecutionAssignments(assignments, providers_, models_);
}

common::Result<void> ExecutionProfileService::validateWorkflow(
    std::string_view workflowId,
    const std::vector<StageModelAssignment>& assignments) const
{
    return validateExecutionProfileWorkflow(workflowId, assignments);
}

common::Result<ExecutionProfileId> ExecutionProfileService::createProfile(
    CreateExecutionProfileRequest request)
{
    if (request.name.empty()) {
        return common::Result<ExecutionProfileId>::failure(
            error("execution_profile.empty_name", "Execution profile name cannot be empty"));
    }
    if (request.workflowId.empty()) {
        return common::Result<ExecutionProfileId>::failure(
            error("execution_profile.empty_workflow_id", "Execution profile workflowId cannot be empty"));
    }
    const auto validated = validateAssignments(request.assignments);
    if (!validated.success()) {
        return common::Result<ExecutionProfileId>::failure(*validated.error());
    }
    const auto workflowValidated = validateWorkflow(request.workflowId, request.assignments);
    if (!workflowValidated.success()) {
        return common::Result<ExecutionProfileId>::failure(*workflowValidated.error());
    }
    const auto budget = resolveProviderExecutionBudget(request.providerBudget);
    if (!budget.success()) {
        return common::Result<ExecutionProfileId>::failure(*budget.error());
    }

    ExecutionProfile profile;
    profile.id = generateUuid();
    profile.name = std::move(request.name);
    profile.workflowId = std::move(request.workflowId);
    profile.enabled = request.enabled;
    profile.isDefault = false; // Only setDefaultProfile() may set this.
    profile.assignments = std::move(request.assignments);
    profile.providerBudget = std::move(request.providerBudget);
    profile.createdAt = std::chrono::system_clock::now();
    profile.updatedAt = profile.createdAt;

    const auto inserted = profiles_.insert(std::move(profile));
    if (!inserted.success()) {
        return common::Result<ExecutionProfileId>::failure(*inserted.error());
    }
    return common::Result<ExecutionProfileId>::ok(inserted.value().id);
}

common::Result<void> ExecutionProfileService::updateProfile(UpdateExecutionProfileRequest request)
{
    if (request.name.empty()) {
        return common::Result<void>::failure(
            error("execution_profile.empty_name", "Execution profile name cannot be empty"));
    }
    if (request.workflowId.empty()) {
        return common::Result<void>::failure(
            error("execution_profile.empty_workflow_id", "Execution profile workflowId cannot be empty"));
    }
    const auto validated = validateAssignments(request.assignments);
    if (!validated.success()) {
        return common::Result<void>::failure(*validated.error());
    }
    const auto workflowValidated = validateWorkflow(request.workflowId, request.assignments);
    if (!workflowValidated.success()) {
        return common::Result<void>::failure(*workflowValidated.error());
    }
    const auto budget = resolveProviderExecutionBudget(request.providerBudget);
    if (!budget.success()) {
        return common::Result<void>::failure(*budget.error());
    }

    const auto found = profiles_.findById(request.id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("execution_profile.not_found", "Execution profile not found"));
    }

    ExecutionProfile profile = *found.value();
    profile.name = std::move(request.name);
    profile.workflowId = std::move(request.workflowId);
    profile.assignments = std::move(request.assignments);
    profile.providerBudget = std::move(request.providerBudget);
    profile.updatedAt = std::chrono::system_clock::now();

    const auto updated = profiles_.update(std::move(profile));
    if (!updated.success()) {
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> ExecutionProfileService::setDefaultProfile(const ExecutionProfileId& id)
{
    const auto found = profiles_.findById(id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("execution_profile.not_found", "Execution profile not found"));
    }
    if (!found.value()->enabled) {
        return common::Result<void>::failure(error(
            "execution_profile.disabled_cannot_be_default",
            "A disabled execution profile cannot become the default"));
    }
    return profiles_.setDefaultProfile(id);
}

} // namespace ai::application
