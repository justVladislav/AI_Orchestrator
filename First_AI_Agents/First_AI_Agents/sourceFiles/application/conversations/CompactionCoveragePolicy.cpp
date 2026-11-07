#include "ai/application/conversations/CompactionCoveragePolicy.hpp"

#include "ai/application/conversations/CompactionCreatedAtWindow.hpp"

#include <set>
#include <string>
#include <utility>

namespace ai::application {

CompactionCoverageObservation unavailableCompactionCoverage(common::Error error,
    std::optional<std::chrono::system_clock::time_point> fromInclusive, std::optional<std::chrono::system_clock::time_point> toExclusive)
{
    CompactionCoverageObservation observation;   // Unavailable, no counts
    observation.error = std::move(error);
    observation.windowed = fromInclusive.has_value() || toExclusive.has_value();
    observation.fromInclusive = fromInclusive;
    observation.toExclusive = toExclusive;
    return observation;
}

CompactionIntentReadSnapshot composeCompactionIntentSnapshot(const ProjectId& projectId,
    const std::vector<ProjectMemoryCompactionIntent>& intents, std::optional<std::chrono::system_clock::time_point> fromInclusive,
    std::optional<std::chrono::system_clock::time_point> toExclusive)
{
    std::set<ProjectMemoryCompactionId> seen;
    for (const auto& intent : intents) {
        if (intent.projectId != projectId) {
            CompactionIntentReadSnapshot refused;
            refused.coverage = unavailableCompactionCoverage(
                common::Error {kIntentObservationInconsistentCode, "The intent data contains a row of another project; it was not used"},
                fromInclusive, toExclusive);
            return refused;
        }
        if (!seen.insert(intent.id).second) {
            CompactionIntentReadSnapshot refused;
            refused.coverage = unavailableCompactionCoverage(
                common::Error {kIntentObservationInconsistentCode, "The intent data contains a repeated operation id; it was not used"},
                fromInclusive, toExclusive);
            return refused;
        }
    }

    CompactionIntentReadSnapshot snapshot;
    snapshot.coverage.state = CompactionCoverageState::Available;
    snapshot.coverage.windowed = fromInclusive.has_value() || toExclusive.has_value();
    snapshot.coverage.fromInclusive = fromInclusive;
    snapshot.coverage.toExclusive = toExclusive;
    std::uint64_t open = 0;
    std::uint64_t unresolved = 0;
    for (const auto& intent : intents) {
        if (!inCreatedAtWindow(intent.createdAt, fromInclusive, toExclusive)) continue;
        if (intent.state == CompactionIntentState::Open) ++open; else ++unresolved;
        snapshot.rows.push_back(intent);
    }
    snapshot.coverage.openOperations = open;
    snapshot.coverage.unresolvedOperations = unresolved;
    return snapshot;
}

} // namespace ai::application
