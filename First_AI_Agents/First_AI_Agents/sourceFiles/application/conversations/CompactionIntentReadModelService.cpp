#include "ai/application/conversations/CompactionIntentReadModelService.hpp"

#include "ai/application/analytics/ProjectCostTypes.hpp"
#include "ai/application/conversations/CompactionCoveragePolicy.hpp"

namespace ai::application {

CompactionIntentReadModelService::CompactionIntentReadModelService(const IProjectMemoryCompactionIntentRepository& intents)
    : intents_(intents)
{
}

CompactionIntentReadSnapshot CompactionIntentReadModelService::read(const ProjectId& projectId,
    std::optional<std::chrono::system_clock::time_point> fromInclusive, std::optional<std::chrono::system_clock::time_point> toExclusive) const
{
    if (fromInclusive.has_value() && toExclusive.has_value() && *fromInclusive >= *toExclusive) {
        CompactionIntentReadSnapshot refused;
        refused.coverage = unavailableCompactionCoverage(
            common::Error {kProjectCostInvalidDateRangeCode, kProjectCostInvalidDateRangeMessage}, fromInclusive, toExclusive);
        return refused;   // before any read
    }
    const auto listed = intents_.listIntents(projectId, std::nullopt);
    if (!listed.success()) {
        CompactionIntentReadSnapshot failed;
        failed.coverage = unavailableCompactionCoverage(*listed.error(), fromInclusive, toExclusive);   // the original error
        return failed;
    }
    return composeCompactionIntentSnapshot(projectId, listed.value(), fromInclusive, toExclusive);
}

} // namespace ai::application
