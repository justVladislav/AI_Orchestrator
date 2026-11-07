#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionIntent.hpp"
#include "ai/common/Error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace ai::application {

// B3-III: what the stored compaction intents of ONE project say about "operations without a confirmed final recording", as a read
// model. It is a statement about the ROWS that one read found, in the stated period - not an audit of every request, not a live status
// and not part of any amount. Neutral data only: no SQL, no Qt.

// Unavailable is the default: a default-constructed observation confirms nothing, in particular not a count of zero.
enum class CompactionCoverageState { Unavailable, Available };

struct CompactionCoverageObservation {
    CompactionCoverageState state {CompactionCoverageState::Unavailable};
    // The ORIGINAL error of the failed read (code and message unchanged); set exactly when the state is Unavailable because a read or
    // the data failed. A default observation has none.
    std::optional<common::Error> error;
    // Present exactly when the state is Available (a real zero stays a zero); BOTH absent when Unavailable - never zeros.
    std::optional<std::uint64_t> openOperations;
    std::optional<std::uint64_t> unresolvedOperations;
    // The REQUESTED period, kept also when the read failed (it is the question that was asked, not a measurement).
    // windowed is true when any bound was given; [fromInclusive, toExclusive) over the creation time of the intent.
    bool windowed {false};
    std::optional<std::chrono::system_clock::time_point> fromInclusive;
    std::optional<std::chrono::system_clock::time_point> toExclusive;
};

// ONE strict intent read of a project, split into what the surfaces show: the counts (coverage) and the very same rows they were
// counted from. Rows are those of the selected period in the order of the read (createdAt, id). They carry no amount, no usage and no
// price state: an intent has none.
struct CompactionIntentReadSnapshot {
    CompactionCoverageObservation coverage;
    std::vector<ProjectMemoryCompactionIntent> rows;
};

} // namespace ai::application
