#pragma once

#include "ai/application/analytics/ExecutionAnalyticsService.hpp"
#include "ai/application/analytics/ProjectCostTypes.hpp"
#include "ai/application/conversations/CompactionIntentReadModelService.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>

namespace ai::application {

/**
 * Read-only project cost estimate: the execution ledger (through ExecutionAnalyticsService) composed with the stored memory
 * compaction records (through ProjectMemoryCompactionService), by the pure rules in ProjectCostComposition.hpp.
 *
 * It owns no data and writes nothing: no execution row is created, no compaction record is changed or repriced.
 *
 * Consistency, stated plainly: the reads are SEQUENTIAL and BEST-EFFORT. The ledger rows, the model catalog and the compaction
 * records are three separate reads, each locking the shared connection by itself; this is NOT an atomic database snapshot. The
 * B3-III: AFTER a successful monetary composition the project's compaction intents are read ONCE (best-effort) and attached as
 * snapshot.compactionCoverage. A failed intent read never fails or changes the money: it is an Unavailable coverage with the original
 * error and no counts. The intents are in no amount, no price state and no evidence count.
 *
 * The fields of the ONE returned snapshot agree with each other (total == sum of providers == sum of models), not with a single
 * instant of the sources. The failure of ANY required read fails the whole result with that read's original error; a ledger
 * without its compactions is never returned as a complete estimate.
 */
class ProjectCostService final {
public:
    ProjectCostService(const ExecutionAnalyticsService& analytics,
                       const ProjectMemoryCompactionService& compactions,
                       const CompactionIntentReadModelService& intents);

    // [fromInclusive, toExclusive) over the creation time of both sources. An empty or inverted window is
    // kProjectCostInvalidDateRangeCode (no read is made).
    [[nodiscard]] common::Result<ProjectCostSnapshot> projectCost(
        const ProjectId& projectId,
        std::optional<std::chrono::system_clock::time_point> fromInclusive = std::nullopt,
        std::optional<std::chrono::system_clock::time_point> toExclusive = std::nullopt) const;

private:
    const ExecutionAnalyticsService& analytics_;
    const ProjectMemoryCompactionService& compactions_;
    const CompactionIntentReadModelService& intents_;
};

} // namespace ai::application
