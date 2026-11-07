#pragma once

#include "ai/application/conversations/CompactionCoverageTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace ai::application {

// B3-III pure rules of the compaction coverage (no I/O, no clock).

// The observation of a read that did not happen or failed: Unavailable, the ORIGINAL error, BOTH counts absent, and the requested
// period kept as the question that was asked.
[[nodiscard]] CompactionCoverageObservation unavailableCompactionCoverage(common::Error error,
    std::optional<std::chrono::system_clock::time_point> fromInclusive, std::optional<std::chrono::system_clock::time_point> toExclusive);

// From the strictly read intents of ONE project: the counts of Open and of Unresolved intents created in [from, to) and the very same
// rows. A real zero is a present zero. The data is refused - Unavailable with kIntentObservationInconsistentCode, never a silent
// filter - when a row belongs to another project or an id repeats. The period is not validated here (the callers do it before any read).
[[nodiscard]] CompactionIntentReadSnapshot composeCompactionIntentSnapshot(const ProjectId& projectId,
    const std::vector<ProjectMemoryCompactionIntent>& intents, std::optional<std::chrono::system_clock::time_point> fromInclusive,
    std::optional<std::chrono::system_clock::time_point> toExclusive);

} // namespace ai::application
