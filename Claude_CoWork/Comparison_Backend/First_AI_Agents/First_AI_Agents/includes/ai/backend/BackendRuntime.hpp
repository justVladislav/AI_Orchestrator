#pragma once

#include "ai/application/ArtifactService.hpp"
#include "ai/application/ComparisonService.hpp"
#include "ai/application/CredentialManagementService.hpp"
#include "ai/application/ChatService.hpp"
#include "ai/application/ExecutionProfileService.hpp"
#include "ai/application/ICredentialStore.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/application/ProjectService.hpp"
#include "ai/application/ProviderCatalogService.hpp"
#include "ai/application/WorkspaceContextService.hpp"
#include "ai/common/Error.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

namespace ai::backend {

struct BackendRuntimeConfig {
    std::filesystem::path databasePath;
    std::chrono::milliseconds mockStepDelay {std::chrono::milliseconds {20}};
    std::size_t mockProgressSteps {4};
    std::filesystem::path artifactRoot;
};

class BackendRuntime final {
public:
    explicit BackendRuntime(BackendRuntimeConfig config);
    ~BackendRuntime();

    BackendRuntime(const BackendRuntime&) = delete;
    BackendRuntime& operator=(const BackendRuntime&) = delete;
    BackendRuntime(BackendRuntime&&) = delete;
    BackendRuntime& operator=(BackendRuntime&&) = delete;

    [[nodiscard]] ai::application::ProjectService& projectService() noexcept;
    [[nodiscard]] ai::application::OrchestrationService& orchestrationService() noexcept;
    [[nodiscard]] ai::application::ArtifactService& artifactService() noexcept;
    [[nodiscard]] ai::application::WorkspaceContextService& workspaceContextService() noexcept;
    [[nodiscard]] ai::application::ProviderCatalogService& providerCatalogService() noexcept;
    [[nodiscard]] ai::application::ExecutionProfileService& executionProfileService() noexcept;
    [[nodiscard]] ai::application::ICredentialStore& credentialStore() noexcept;
    [[nodiscard]] ai::application::CredentialManagementService& credentialManagementService() noexcept;
    [[nodiscard]] ai::application::ChatService& chatService() noexcept;

    // ── Comparison ──────────────────────────────────────────────────────────
    /// Comparison session and result commands/queries. Same always-constructible,
    /// controlled-failure guarantee as the other services: when storage is
    /// unavailable every operation returns "storage.unavailable" instead of
    /// throwing or returning a null reference.
    [[nodiscard]] ai::application::ComparisonService& comparisonService() noexcept;

    [[nodiscard]] bool storageAvailable() const noexcept;
    [[nodiscard]] const std::optional<ai::common::Error>& storageError() const noexcept;
    [[nodiscard]] const std::optional<ai::common::Error>& recoveryError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai::backend
