#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace ai::application {

/**
 * Persistence port for project metadata.
 *
 * Application services depend on this interface; in-memory and future SQLite
 * implementations remain replaceable infrastructure details.
 */
class IProjectRepository {
public:
    virtual ~IProjectRepository() = default;
    // Insert/update return the stored representation so implementations may
    // normalize fields or advance persistence metadata.
    virtual common::Result<Project> insert(Project project) = 0;
    virtual common::Result<Project> update(Project project) = 0;
    [[nodiscard]] virtual common::Result<std::optional<Project>> findById(const ProjectId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<Project>> findByRootPath(
        const std::filesystem::path& rootPath) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<Project>> listAll() const = 0;
};

} // namespace ai::application
