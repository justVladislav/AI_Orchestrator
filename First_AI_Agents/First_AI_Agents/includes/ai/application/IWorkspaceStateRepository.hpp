#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

/**
 * Persistence port for exactly one piece of durable global UI state: which
 * project (if any) is currently "active" in the workspace. This is
 * deliberately the only thing this port stores - Project's own name,
 * description, root path, and status are never copied into workspace state;
 * a resolved Project is always re-read from IProjectRepository through the
 * stored, stable ProjectId.
 */
class IWorkspaceStateRepository {
public:
    virtual ~IWorkspaceStateRepository() = default;

    [[nodiscard]] virtual common::Result<std::optional<ProjectId>> activeProjectId() const = 0;
    virtual common::Result<void> setActiveProjectId(const std::optional<ProjectId>& projectId) = 0;
};

} // namespace ai::application
