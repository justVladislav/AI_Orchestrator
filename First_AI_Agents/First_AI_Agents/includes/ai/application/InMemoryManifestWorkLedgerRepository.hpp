#pragma once

#include "ai/application/IManifestWorkLedgerRepository.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace ai::application {

class InMemoryManifestWorkLedgerRepository final
    : public IManifestWorkLedgerRepository {
public:
    common::Result<void> insertEntry(ManifestWorkLedgerEntry entry) override;
    [[nodiscard]] common::Result<std::optional<ManifestWorkLedgerEntry>> findEntry(
        const RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<ManifestWorkLedgerEntry>>
    listRecentForProject(const ProjectId& projectId, std::size_t limit) const override;

    common::Result<void> appendTransition(
        ManifestWorkLedgerReviewTransition transition) override;
    [[nodiscard]] common::Result<std::vector<ManifestWorkLedgerReviewTransition>>
    listTransitionsForRun(const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    // Ordered by runId for deterministic listing; entries are not otherwise
    // orderable by "recency" without a timestamp comparison performed at
    // read time (see listRecentForProject's implementation).
    std::map<RunId, ManifestWorkLedgerEntry> entries_;
    // Insertion order preserved (a std::vector, not a map) since
    // listTransitionsForRun's contract is "append-only, in the order they
    // occurred" - a map keyed only by runId would silently reorder same-run
    // transitions with identical timestamps.
    std::vector<ManifestWorkLedgerReviewTransition> transitions_;
};

} // namespace ai::application
