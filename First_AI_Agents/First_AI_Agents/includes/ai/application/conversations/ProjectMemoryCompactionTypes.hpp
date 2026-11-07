#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/analytics/ExecutionAnalyticsTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

using ProjectMemoryCompactionId = std::string;

// A compaction attempt's terminal disposition. There is no Pending row: one
// synchronous compact() call tries to write exactly one record with its
// final status (durable when the write succeeds; storage can fail) - see
// ProjectMemoryCompactionService.hpp for why this differs from ChatService's
// create-then-finish pattern.
enum class ProjectMemoryCompactionStatus { Completed, Failed, Cancelled };

// Header-only: a three-value enum does not warrant its own translation unit
// (and therefore its own new CMake source-list entry - see the exclusive
// file-ownership split this package respects).
[[nodiscard]] inline const char* toString(ProjectMemoryCompactionStatus status) noexcept
{
    switch (status) {
    case ProjectMemoryCompactionStatus::Completed: return "completed";
    case ProjectMemoryCompactionStatus::Failed: return "failed";
    case ProjectMemoryCompactionStatus::Cancelled: return "cancelled";
    }
    return "failed";
}

// Durable, immutable audit/provenance record for one AG-11 compaction
// attempt - kept even when the attempt failed or was cancelled, so "we tried
// to compact and it was refused/cancelled" is never silently lost. Never
// mutated after insertCompactionRecord(); a retry produces its own new row.
struct ProjectMemoryCompactionRecord {
    ProjectMemoryCompactionId id;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;

    // Exactly which existing segments this attempt read, in the order they
    // were included in the prompt - immutable even though those segment rows
    // are deleted on success (replaced by resultSegmentId). This is the
    // "provenance" AG-11 requires: an auditor can see what was compacted
    // without the original rows still existing.
    std::vector<ProjectMemorySegmentId> sourceSegmentIds;
    // SHA-256 of the exact source text this attempt actually sent to the
    // provider (ids + summaries, in order) - lets a reader detect whether a
    // segment set changed between two compaction attempts without storing
    // the full source text a second time.
    std::string sourceContentHash;

    // Set only when status == Completed.
    std::optional<ProjectMemorySegmentId> resultSegmentId;

    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
    // C2-B/S1: whether costMicros is a COMPLETE price for the usage above, as computeSingleResponseCostMicros reported it
    // (CostBreakdown::priceKnown). This record is the only owner of the amount and of this flag.
    //   true     the price was complete (a true zero included),
    //   false    the price was incomplete: costMicros is only what could be priced,
    //   nullopt  no evidence of completeness: a record written before this field existed, or an attempt that never got a
    //            response to price. NEVER defaulted to true and never inferred from today's catalog.
    std::optional<bool> costPriceKnown;

    ProjectMemoryCompactionStatus status {ProjectMemoryCompactionStatus::Failed};
    // Populated for Failed/Cancelled; empty for Completed.
    std::string errorCode;
    std::string errorMessage;

    std::chrono::system_clock::time_point createdAt;
};

// How completely the stored cost of ONE compaction record is known - the same four states as every other cost surface
// (ExecutionPriceState: none | known | partial | unknown). A pure function of the stored record: it never reads the
// catalog, so a later price change cannot rewrite history, and the status alone never hides or proves a cost.
// Order matters; false and nullopt are NOT the same thing:
//   1. costPriceKnown == true                      known (a true zero included; it proves the calculation was complete for the
//                                                  usage received, not that the provider billed nothing)
//   2. false or nullopt, costMicros > 0           partial: the recorded amount is an incomplete estimate, whatever the tokens/status
//   3. false, costMicros == 0                     unknown, also with zero tokens (an explicit false is not "no operation")
//   4. nullopt, costMicros == 0, tokens > 0       unknown
//   5. nullopt, costMicros == 0, no tokens, Completed   unknown: an old successful answer does not prove an exact zero
//   6. nullopt, costMicros == 0, no tokens, Failed/Cancelled   none: nothing was recorded (not a claim that it was free)
[[nodiscard]] inline ExecutionPriceState compactionPriceState(const ProjectMemoryCompactionRecord& record) noexcept
{
    if (record.costPriceKnown.has_value() && *record.costPriceKnown) return ExecutionPriceState::Known;
    if (record.costMicros > 0) return ExecutionPriceState::Partial;
    if (record.costPriceKnown.has_value()) return ExecutionPriceState::Unknown;
    if (record.inputTokens > 0 || record.outputTokens > 0) return ExecutionPriceState::Unknown;
    if (record.status == ProjectMemoryCompactionStatus::Completed) return ExecutionPriceState::Unknown;
    return ExecutionPriceState::None;
}

} // namespace ai::application
