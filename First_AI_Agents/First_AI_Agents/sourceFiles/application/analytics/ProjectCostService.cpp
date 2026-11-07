#include "ai/application/analytics/ProjectCostService.hpp"

#include "ai/application/analytics/ProjectCostComposition.hpp"

namespace ai::application {

ProjectCostService::ProjectCostService(const ExecutionAnalyticsService& analytics,
                                       const ProjectMemoryCompactionService& compactions,
                                       const CompactionIntentReadModelService& intents)
    : analytics_(analytics), compactions_(compactions), intents_(intents)
{
}

common::Result<ProjectCostSnapshot> ProjectCostService::projectCost(
    const ProjectId& projectId,
    std::optional<std::chrono::system_clock::time_point> fromInclusive,
    std::optional<std::chrono::system_clock::time_point> toExclusive) const
{
    if (fromInclusive.has_value() && toExclusive.has_value() && *fromInclusive >= *toExclusive) {
        return common::Result<ProjectCostSnapshot>::failure(
            {kProjectCostInvalidDateRangeCode, kProjectCostInvalidDateRangeMessage});
    }

    // Read 1 (ledger rows + model catalog, inside the analytics service). Its failure is returned as it came.
    const auto ledger = analytics_.projectSnapshot(projectId, fromInclusive, toExclusive);
    if (!ledger.success()) {
        return common::Result<ProjectCostSnapshot>::failure(*ledger.error());
    }
    // Read 2 (the compaction history). Its failure fails the whole estimate: a ledger-only total would pass for a complete one.
    const auto records = compactions_.listCompactionRecords(projectId);
    if (!records.success()) {
        return common::Result<ProjectCostSnapshot>::failure(*records.error());
    }
    auto composed = composeProjectCost(ledger.value(), records.value(), fromInclusive, toExclusive);
    if (!composed.success()) return composed;
    // Only now, with the money settled, ONE best-effort read of the intents. Its failure is a coverage state, never a money failure.
    composed.value().compactionCoverage = intents_.read(projectId, fromInclusive, toExclusive).coverage;
    return composed;
}

} // namespace ai::application
