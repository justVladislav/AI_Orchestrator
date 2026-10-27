#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ai::application {

// The newest segments of a project, newest first (created_at descending, id descending), plus how many the
// project has in total.
struct ProjectMemoryRecentPage {
    std::vector<ProjectMemorySegment> newest;
    std::uint64_t totalCount {0};
};

// Persistence port for the project-level memory layer. Segments summarize
// run outcomes without ever mutating Project::description; removing a
// project removes its segments (enforced by ON DELETE CASCADE in the SQLite
// adapter). run_id is not a foreign key: memory outlives the deletion of any
// single run.
class IProjectMemoryRepository {
public:
    virtual ~IProjectMemoryRepository() = default;
    virtual common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment value) = 0;
    [[nodiscard]] virtual common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(
        const ProjectId& projectId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(
        const ProjectMemorySegmentId& id) const = 0;
    virtual common::Result<void> removeMemorySegments(const ProjectId& projectId) = 0;

    // Bounded read for callers that need only the newest segments (the project context snapshot): at most
    // `limit` segments, newest first, and the project's total count. The default builds it from
    // listMemorySegments() (so every existing implementation and test double keeps working); the SQLite
    // adapter overrides it with COUNT + ORDER BY ... DESC LIMIT so a long-lived project is never read in full
    // under the database mutex.
    [[nodiscard]] virtual common::Result<ProjectMemoryRecentPage> listRecentMemorySegments(
        const ProjectId& projectId, std::size_t limit) const
    {
        auto all = listMemorySegments(projectId);
        if (!all.success()) return common::Result<ProjectMemoryRecentPage>::failure(*all.error());
        ProjectMemoryRecentPage page;
        page.totalCount = all.value().size();
        std::sort(all.value().begin(), all.value().end(),
            [](const ProjectMemorySegment& left, const ProjectMemorySegment& right) {
                if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
                return left.id > right.id;
            });
        if (all.value().size() > limit) all.value().resize(limit);
        page.newest = std::move(all.value());
        return common::Result<ProjectMemoryRecentPage>::ok(std::move(page));
    }
};

} // namespace ai::application
