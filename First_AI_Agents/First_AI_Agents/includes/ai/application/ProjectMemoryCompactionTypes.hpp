#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

using ProjectMemoryCompactionId = std::string;

// A compaction attempt's terminal disposition. There is no Pending row: one
// synchronous compact() call produces exactly one durable record with its
// final status - see ProjectMemoryCompactionService.hpp for why this differs
// from ChatService's create-then-finish pattern.
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

    ProjectMemoryCompactionStatus status {ProjectMemoryCompactionStatus::Failed};
    // Populated for Failed/Cancelled; empty for Completed.
    std::string errorCode;
    std::string errorMessage;

    std::chrono::system_clock::time_point createdAt;
};

} // namespace ai::application
