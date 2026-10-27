#pragma once

#include "ai/application/artifacts/ArtifactTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

/**
 * Persistence port for artifact metadata only. Artifact contents are never
 * read or written through this interface - see IArtifactStore for that.
 * Absolute filesystem paths are never accepted or returned here.
 */
class IArtifactRepository {
public:
    virtual ~IArtifactRepository() = default;

    virtual common::Result<Artifact> insertArtifact(Artifact artifact) = 0;
    [[nodiscard]] virtual common::Result<std::optional<Artifact>> findArtifact(
        const ArtifactId& id) const = 0;
    // Deterministic order (createdAt, then id) so RunsPage's future
    // ArtifactListModel never reorders rows between refreshes.
    [[nodiscard]] virtual common::Result<std::vector<Artifact>> listArtifacts(
        const RunId& runId) const = 0;
};

} // namespace ai::application
