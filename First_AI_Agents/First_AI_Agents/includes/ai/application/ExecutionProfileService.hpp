#pragma once

#include "ai/application/IExecutionProfileRepository.hpp"
#include "ai/application/IModelRepository.hpp"
#include "ai/application/IProviderRepository.hpp"

namespace ai::application {

/**
 * Validates and coordinates ExecutionProfile commands. Every assignment is
 * checked against the live Provider/Model catalog at write time so a saved
 * profile can never reference a deleted-in-spirit (disabled), unknown, or
 * mismatched provider/model pair. Workflow-specific profile shape is also
 * validated here (for example Manifest requires one Architect assignment),
 * before the profile can become a runnable persisted configuration.
 */
class ExecutionProfileService {
public:
    ExecutionProfileService(
        IExecutionProfileRepository& profiles,
        IProviderRepository& providers,
        IModelRepository& models);

    [[nodiscard]] common::Result<std::vector<ExecutionProfile>> listProfiles() const;
    [[nodiscard]] common::Result<std::optional<ExecutionProfile>> findProfile(
        const ExecutionProfileId& id) const;
    common::Result<ExecutionProfileId> createProfile(CreateExecutionProfileRequest request);
    common::Result<void> updateProfile(UpdateExecutionProfileRequest request);
    common::Result<void> setDefaultProfile(const ExecutionProfileId& id);

private:
    [[nodiscard]] common::Result<void> validateAssignments(
        const std::vector<StageModelAssignment>& assignments) const;
    [[nodiscard]] common::Result<void> validateWorkflow(
        std::string_view workflowId,
        const std::vector<StageModelAssignment>& assignments) const;

    IExecutionProfileRepository& profiles_;
    IProviderRepository& providers_;
    IModelRepository& models_;
};

} // namespace ai::application
