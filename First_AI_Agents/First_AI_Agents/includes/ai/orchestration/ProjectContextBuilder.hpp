#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ai::orchestration {

struct ProjectFileSnapshot {
    std::string relativePath;
    std::string content;
};

struct ProjectContext {
    std::vector<ProjectFileSnapshot> files;
    std::string manifest;
    std::string context;
};

struct ProjectContextSelection {
    std::string title;
    std::vector<std::string> editableFiles;
    std::vector<std::string> readOnlyFiles;
    std::vector<std::string> notes;
};

/**
 * @brief Builds the read-only source snapshot passed to the Architect stage.
 *
 * The builder deliberately excludes Workspace/run output, build folders,
 * binary files, local secret files, and vendored dependencies. This prevents a
 * new AI run from ingesting previous AI artifacts or credentials as context.
 */
class ProjectContextBuilder {
public:
    explicit ProjectContextBuilder(std::filesystem::path projectRoot);

    [[nodiscard]] ProjectContext build() const;
    [[nodiscard]] ProjectContext buildSelected(const ProjectContextSelection& selection) const;

private:
    std::filesystem::path projectRoot_;
};

} // namespace ai::orchestration
