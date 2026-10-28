#include "ai/application/projects/ProjectService.hpp"

#include "ai/application/IdGenerator.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

std::string trim(std::string value)
{
    const auto isText = [](unsigned char character) { return std::isspace(character) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), isText);
    const auto last = std::find_if(value.rbegin(), value.rend(), isText).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

} // namespace

ProjectService::ProjectService(IProjectRepository& repository)
    : repository_(repository)
{
}

common::Result<Project> ProjectService::createProject(const CreateProjectRequest& request)
{
    // Validate and canonicalize at the service boundary so every repository
    // adapter receives stable paths and cannot store duplicate spellings.
    const std::string name = trim(request.name);
    if (name.empty()) {
        return common::Result<Project>::failure(error("project.empty_name", "Project name cannot be empty"));
    }
    if (request.rootPath.empty()) {
        return common::Result<Project>::failure(error("project.empty_path", "Project path cannot be empty"));
    }

    std::error_code pathError;
    if (!std::filesystem::exists(request.rootPath, pathError) || pathError) {
        return common::Result<Project>::failure(error("project.path_not_found", "Project path does not exist"));
    }
    if (!std::filesystem::is_directory(request.rootPath, pathError) || pathError) {
        return common::Result<Project>::failure(error("project.path_not_directory", "Project path is not a directory"));
    }

    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(request.rootPath, pathError);
    if (pathError) {
        return common::Result<Project>::failure(error("project.path_unavailable", "Project path cannot be resolved"));
    }

    const auto duplicate = repository_.findByRootPath(canonicalPath);
    if (!duplicate.success()) {
        return common::Result<Project>::failure(*duplicate.error());
    }
    if (duplicate.value().has_value()) {
        // The canonical path is unique across all statuses. Point the user at
        // Restore instead of a dead-end error when the owner is archived.
        if (duplicate.value()->status == ProjectStatus::Archived) {
            return common::Result<Project>::failure(error("project.duplicate_path_archived",
                "This path belongs to an archived project. Restore that project instead."));
        }
        return common::Result<Project>::failure(error("project.duplicate_path", "Project path is already registered"));
    }

    // Generate identity only after validation; rejected commands leave no rows.
    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = generateUuid();
    project.name = name;
    project.description = trim(request.description);
    project.rootPath = canonicalPath;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    return repository_.insert(std::move(project));
}

common::Result<std::vector<Project>> ProjectService::listProjects() const
{
    return repository_.listAll();
}

common::Result<Project> ProjectService::archiveProject(const ProjectId& projectId)
{
    // Archive rather than delete so historical runs keep a valid project link.
    const auto found = repository_.findById(projectId);
    if (!found.success()) {
        return common::Result<Project>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<Project>::failure(error("project.not_found", "Project not found"));
    }

    Project project = *found.value();
    project.status = ProjectStatus::Archived;
    project.updatedAt = std::chrono::system_clock::now();
    return repository_.update(std::move(project));
}

common::Result<Project> ProjectService::restoreProject(const ProjectId& projectId)
{
    const auto found = repository_.findById(projectId);
    if (!found.success()) {
        return common::Result<Project>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<Project>::failure(error("project.not_found", "Project not found"));
    }

    Project project = *found.value();
    if (project.status != ProjectStatus::Archived) {
        return common::Result<Project>::failure(
            error("project.not_archived", "Only archived projects can be restored"));
    }
    project.status = ProjectStatus::Active;
    project.updatedAt = std::chrono::system_clock::now();
    return repository_.update(std::move(project));
}

common::Result<Project> ProjectService::updateProjectPath(
    const ProjectId& projectId, const std::filesystem::path& rootPath)
{
    const auto found = repository_.findById(projectId);
    if (!found.success())
        return common::Result<Project>::failure(*found.error());
    if (!found.value().has_value())
        return common::Result<Project>::failure(error("project.not_found", "Project not found"));

    if (rootPath.empty())
        return common::Result<Project>::failure(error("project.empty_path", "Project path cannot be empty"));

    std::error_code pathError;
    if (!std::filesystem::exists(rootPath, pathError) || pathError)
        return common::Result<Project>::failure(error("project.path_not_found", "Project path does not exist"));
    if (!std::filesystem::is_directory(rootPath, pathError) || pathError)
        return common::Result<Project>::failure(error("project.path_not_directory", "Project path is not a directory"));

    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(rootPath, pathError);
    if (pathError)
        return common::Result<Project>::failure(error("project.path_unavailable", "Project path cannot be resolved"));

    Project project = *found.value();
    if (canonicalPath == project.rootPath) {
        // No-op: same canonical path, nothing to validate or persist.
        return common::Result<Project>::ok(std::move(project));
    }

    const auto duplicate = repository_.findByRootPath(canonicalPath);
    if (!duplicate.success())
        return common::Result<Project>::failure(*duplicate.error());
    if (duplicate.value().has_value() && duplicate.value()->id != projectId) {
        if (duplicate.value()->status == ProjectStatus::Archived) {
            return common::Result<Project>::failure(error("project.duplicate_path_archived",
                "This path belongs to an archived project. Restore that project instead."));
        }
        return common::Result<Project>::failure(error("project.duplicate_path", "Project path is already registered"));
    }

    project.rootPath = canonicalPath;
    project.updatedAt = std::chrono::system_clock::now();
    return repository_.update(std::move(project));
}

common::Result<Project> ProjectService::updateProjectDescription(
    const ProjectId& projectId, const std::string& description)
{
    const auto found = repository_.findById(projectId);
    if (!found.success())
        return common::Result<Project>::failure(*found.error());
    if (!found.value().has_value())
        return common::Result<Project>::failure(
            error("project.not_found", "Project not found"));

    Project project = *found.value();
    project.description = trim(description);
    project.updatedAt = std::chrono::system_clock::now();
    return repository_.update(std::move(project));
}

} // namespace ai::application
