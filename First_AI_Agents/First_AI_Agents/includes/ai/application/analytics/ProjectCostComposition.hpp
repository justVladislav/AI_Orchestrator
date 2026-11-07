#pragma once

#include "ai/application/analytics/ExecutionAnalyticsTypes.hpp"
#include "ai/application/analytics/ProjectCostTypes.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace ai::application {

// Adds ONE stored compaction record to an aggregate. The record's contribution is decided only by compactionPriceState(record) -
// never by its status or by whether it has tokens: a positive recorded amount counts even with zero tokens, a known zero is
// known, and only a state of none is kept apart (noneCount). Nothing is read from the catalog.
void addCompactionRecord(CompactionCostAggregate& aggregate, const ProjectMemoryCompactionRecord& record);

// Composes the ledger summary (taken as it is) with the compaction records of the same project (pure, no I/O).
//   - A repeated compaction record id anywhere in `records` is a data error: the whole result fails with
//     kProjectCostDuplicateCompactionRecordCode (nothing is skipped or summed twice). Equal ids in the ledger and in the
//     compaction history are NOT duplicates; they are different sources.
//   - The window [fromInclusive, toExclusive) is applied to the compaction records by createdAt, the same way the ledger applied it.
//   - Provider and model rows come from the same data as the total, sorted by cost, then by the number of recorded items, then id.
[[nodiscard]] common::Result<ProjectCostSnapshot> composeProjectCost(
    const ExecutionAnalyticsSnapshot& ledger,
    const std::vector<ProjectMemoryCompactionRecord>& records,
    std::optional<std::chrono::system_clock::time_point> fromInclusive = std::nullopt,
    std::optional<std::chrono::system_clock::time_point> toExclusive = std::nullopt);

} // namespace ai::application
