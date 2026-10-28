#pragma once

#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/projects/IWorkspaceStateRepository.hpp"

namespace ai::application {

/**
 * Owns the single globally active project the desktop UI's "Continue"
 * action will eventually set before navigating to Runs. This service never
 * starts a run and never copies Project fields into workspace state; it
 * only validates and stores a stable ProjectId, then re-resolves the full
 * Project through IProjectRepository on every read so Description/name/path
 * always reflect the live aggregate.
 */
class WorkspaceContextService {
public:
    WorkspaceContextService(
        IWorkspaceStateRepository& stateRepository,
        IProjectRepository& projectRepository);

    // Rejects an unknown project id ("workspace.project_not_found") and a
    // project that is not ProjectStatus::Active ("workspace.project_not_active"
    // - covers both Archived and Unavailable). Re-selecting the same already
    // active project id is safe: it re-validates and re-writes the same value.
    common::Result<void> selectProject(const ProjectId& projectId);

    common::Result<void> clearActiveProject();

    // Both accessors self-heal a stale reference: if the stored active
    // project id no longer resolves to an existing, Active project (deleted,
    // archived, or made unavailable since it was selected), the stored state
    // is cleared and std::nullopt is returned instead of surfacing a
    // dangling id or crashing - the "controlled" behavior the stale-startup
    // requirement asks for. A repository-level failure (e.g.
    // storage.unavailable) is still propagated as a Result failure.
    [[nodiscard]] common::Result<std::optional<ProjectId>> activeProjectId() const;
    [[nodiscard]] common::Result<std::optional<Project>> activeProject() const;

private:
    [[nodiscard]] common::Result<std::optional<Project>> resolveActiveProject() const;

    IWorkspaceStateRepository& stateRepository_;
    IProjectRepository& projectRepository_;
};

} // namespace ai::application
