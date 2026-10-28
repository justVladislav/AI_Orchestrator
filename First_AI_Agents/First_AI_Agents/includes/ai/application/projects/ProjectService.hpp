#pragma once

#include "ai/application/projects/IProjectRepository.hpp"

namespace ai::application {

/** Validates project commands and owns non-destructive project lifecycle rules. */
class ProjectService {
public:
    explicit ProjectService(IProjectRepository& repository);

    common::Result<Project> createProject(const CreateProjectRequest& request);
    [[nodiscard]] common::Result<std::vector<Project>> listProjects() const;
    common::Result<Project> archiveProject(const ProjectId& projectId);
    common::Result<Project> restoreProject(const ProjectId& projectId);
    common::Result<Project> updateProjectDescription(const ProjectId& projectId,
                                                     const std::string& description);
    common::Result<Project> updateProjectPath(const ProjectId& projectId,
                                              const std::filesystem::path& rootPath);

private:
    IProjectRepository& repository_;
};

} // namespace ai::application
