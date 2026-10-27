#pragma once

#include "ai/application/artifacts/ArtifactTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ai::application {

/**
 * Assembles a provider-neutral RunContextSnapshot from durable inputs.
 *
 * maxTotalBytes is an explicit, caller-opted-in budget. When set and the
 * assembled total (project description + selected project memory + run task
 * + every selected file's byteSize) would exceed it, this returns a typed
 * "context_snapshot.budget_exceeded" failure instead of truncating anything -
 * silent truncation of user-provided context is forbidden by the product
 * contract. When maxTotalBytes is std::nullopt, no budget is enforced.
 */
[[nodiscard]] common::Result<RunContextSnapshot> buildRunContextSnapshot(
    const RunId& runId,
    const ProjectId& projectId,
    std::string projectDescription,
    std::string runTask,
    std::vector<ContextSnapshotFile> selectedFiles,
    std::optional<std::uint64_t> maxTotalBytes = std::nullopt,
    std::string projectMemorySection = {},
    std::vector<ProjectMemorySegmentId> includedProjectMemorySegmentIds = {});

// Deterministic, human-readable text rendering of a snapshot. Used as the
// actual stored artifact content, so a reviewer can open it directly without
// a JSON viewer.
[[nodiscard]] std::string renderRunContextSnapshot(const RunContextSnapshot& snapshot);

} // namespace ai::application
