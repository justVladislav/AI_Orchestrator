#pragma once

#include "ai/application/execution/ExecutionManifestValidator.hpp"
#include "ai/application/execution/IExecutionManifestRepository.hpp"
#include "ai/application/IRunRepository.hpp"

namespace ai::application {

class ExecutionManifestService final {
public:
    ExecutionManifestService(
        IExecutionManifestRepository& manifests,
        IRunRepository& runs);

    // The policy is a separate trusted Backend/user input. Any policy fields
    // supplied inside the planner-owned candidate are ignored.
    [[nodiscard]] std::vector<ExecutionManifestFinding> validate(
        ExecutionManifest candidate,
        const ExecutionManifestPolicy& trustedPolicy) const;
    common::Result<ExecutionManifest> freeze(
        ExecutionManifest candidate,
        const ExecutionManifestPolicy& trustedPolicy);
    [[nodiscard]] common::Result<std::optional<ExecutionManifest>> findForRun(
        const RunId& runId) const;

private:
    IExecutionManifestRepository& manifests_;
    IRunRepository& runs_;
};

} // namespace ai::application
