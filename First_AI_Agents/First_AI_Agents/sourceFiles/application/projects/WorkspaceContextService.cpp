#include "ai/application/projects/WorkspaceContextService.hpp"

namespace ai::application {

WorkspaceContextService::WorkspaceContextService(
    IWorkspaceStateRepository& stateRepository,
    IProjectRepository& projectRepository)
    : stateRepository_(stateRepository)
    , projectRepository_(projectRepository)
{
}

common::Result<void> WorkspaceContextService::selectProject(const ProjectId& projectId)
{
    const auto project = projectRepository_.findById(projectId);
    if (!project.success()) {
        return common::Result<void>::failure(*project.error());
    }
    if (!project.value().has_value()) {
        return common::Result<void>::failure(
            {"workspace.project_not_found", "Project not found"});
    }
    if (project.value()->status != ProjectStatus::Active) {
        // One stable code covers both Archived and Unavailable: from the
        // workspace's point of view, neither is a valid active project.
        return common::Result<void>::failure(
            {"workspace.project_not_active", "Only an active project can become the workspace's active project"});
    }
    return stateRepository_.setActiveProjectId(projectId);
}

common::Result<void> WorkspaceContextService::clearActiveProject()
{
    return stateRepository_.setActiveProjectId(std::nullopt);
}

common::Result<std::optional<ProjectId>> WorkspaceContextService::activeProjectId() const
{
    const auto project = resolveActiveProject();
    if (!project.success()) {
        return common::Result<std::optional<ProjectId>>::failure(*project.error());
    }
    if (!project.value().has_value()) {
        return common::Result<std::optional<ProjectId>>::ok(std::nullopt);
    }
    return common::Result<std::optional<ProjectId>>::ok(project.value()->id);
}

common::Result<std::optional<Project>> WorkspaceContextService::activeProject() const
{
    return resolveActiveProject();
}

common::Result<std::optional<Project>> WorkspaceContextService::resolveActiveProject() const
{
    const auto storedId = stateRepository_.activeProjectId();
    if (!storedId.success()) {
        return common::Result<std::optional<Project>>::failure(*storedId.error());
    }
    if (!storedId.value().has_value()) {
        return common::Result<std::optional<Project>>::ok(std::nullopt);
    }

    const auto project = projectRepository_.findById(*storedId.value());
    if (!project.success()) {
        return common::Result<std::optional<Project>>::failure(*project.error());
    }

    const bool staleReference = !project.value().has_value()
        || project.value()->status != ProjectStatus::Active;
    if (staleReference) {
        // Self-heal instead of returning a dangling/invalid reference: a
        // previously active project may have been deleted, archived, or
        // marked unavailable since it was selected. This never fails
        // startup - the caller simply observes "no active project" - unless
        // clearing the stale state itself hits a storage failure, which is
        // still surfaced as a controlled Result failure.
        const auto cleared = stateRepository_.setActiveProjectId(std::nullopt);
        if (!cleared.success()) {
            return common::Result<std::optional<Project>>::failure(*cleared.error());
        }
        return common::Result<std::optional<Project>>::ok(std::nullopt);
    }

    return common::Result<std::optional<Project>>::ok(project.value());
}

} // namespace ai::application
