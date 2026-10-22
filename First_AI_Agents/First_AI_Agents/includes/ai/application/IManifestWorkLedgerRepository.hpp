#pragma once

#include "ai/application/ManifestWorkLedgerTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace ai::application {

// Persistence port for AG-14's durable Project Work Ledger. Deliberately
// two separate append-only stores, the same "hot task-execution row vs. cold
// terminal evidence row" split IManifestTaskExecutionRepository.hpp already
// documents for manifest_task_no_changes: an entry is written exactly once,
// when its Run goes terminal, while transitions accumulate afterward as a
// human reviews/applies the Run's proposals - folding both into one mutable
// aggregate would make the entry not-actually-immutable.
class IManifestWorkLedgerRepository {
public:
    virtual ~IManifestWorkLedgerRepository() = default;

    // Immutable on runId, not idempotent-by-replacement: a second call for
    // the same Run fails with "manifest_work_ledger.duplicate_entry" rather
    // than silently overwriting a possibly-more-complete original entry
    // (AG-14.2 - both the Sqlite and InMemory implementations enforce this
    // identically). ManifestWorkLedgerService::recordTerminalRun treats
    // that specific code as a benign idempotent no-op.
    virtual common::Result<void> insertEntry(ManifestWorkLedgerEntry entry) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ManifestWorkLedgerEntry>> findEntry(
        const RunId& runId) const = 0;

    // Newest-first, bounded to limit - the caller (ManifestWorkLedgerService::
    // buildPlannerContext) owns the actual injection budget; this is only a
    // hard ceiling against an unbounded scan.
    [[nodiscard]] virtual common::Result<std::vector<ManifestWorkLedgerEntry>>
    listRecentForProject(const ProjectId& projectId, std::size_t limit) const = 0;

    // Append-only, and immutable per (changeSetId, kind): a change set has
    // at most one Approved, one Rejected, and one Applied transition ever
    // (see ManifestWorkLedgerService::recordReviewTransition's deterministic
    // id). A second append for the same (changeSetId, kind) fails with
    // "manifest_work_ledger.duplicate_transition" rather than creating a
    // duplicate row; recordReviewTransition treats that as a benign
    // idempotent no-op.
    virtual common::Result<void> appendTransition(
        ManifestWorkLedgerReviewTransition transition) = 0;

    [[nodiscard]] virtual common::Result<std::vector<ManifestWorkLedgerReviewTransition>>
    listTransitionsForRun(const RunId& runId) const = 0;
};

} // namespace ai::application
