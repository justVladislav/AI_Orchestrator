#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/ManifestProposalSetVerifier.hpp"

#include <filesystem>
#include <stop_token>
#include <vector>

namespace ai::application {

struct ManifestVerificationEnvironmentPreflightRequest {
    std::filesystem::path workspaceRoot;
    std::vector<ExecutionManifestTask> tasks;
};

// Read-only, provider-free feasibility gate. It answers only whether the
// frozen manifest's external verification profiles can be attempted against
// the current baseline. Product correctness remains the later combined-overlay
// verifier's responsibility.
class ManifestVerificationEnvironmentPreflight final {
public:
    [[nodiscard]] static common::Result<void> validate(
        const ManifestVerificationEnvironmentPreflightRequest& request,
        IManifestVerificationCommandRunner& commandRunner,
        std::stop_token stopToken);
};

} // namespace ai::application
