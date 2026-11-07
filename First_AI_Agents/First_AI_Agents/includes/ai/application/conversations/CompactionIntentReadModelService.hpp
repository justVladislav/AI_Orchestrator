#pragma once

#include "ai/application/conversations/CompactionCoverageTypes.hpp"
#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"

#include <chrono>
#include <optional>

namespace ai::application {

// B3-III: the ONE const reader of the compaction intents for the surfaces that show them (the Overview cost card and the project
// memory history). It holds the intent port only as a const reference to its read side: it cannot admit, mark, close, recover or send
// anything, and it has no provider, no record repository and no money.
//
// One call = ONE strict listIntents(projectId): the counts and the rows of the same call come from the same list, so they cannot
// disagree. A read that fails (or data that is refused) is an Unavailable coverage with the ORIGINAL error and no counts - never a
// zero and never a failure of whoever asked. An empty or inverted period is Unavailable with kProjectCostInvalidDateRangeCode and the
// port is not touched. The reads of the surfaces are separate, best-effort reads, not one atomic snapshot of the database.
class CompactionIntentReadModelService final {
public:
    explicit CompactionIntentReadModelService(const IProjectMemoryCompactionIntentRepository& intents);

    [[nodiscard]] CompactionIntentReadSnapshot read(const ProjectId& projectId,
        std::optional<std::chrono::system_clock::time_point> fromInclusive = std::nullopt,
        std::optional<std::chrono::system_clock::time_point> toExclusive = std::nullopt) const;

private:
    const IProjectMemoryCompactionIntentRepository& intents_;
};

} // namespace ai::application
