#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"

#include <algorithm>

namespace ai::application {

common::Result<void> InMemoryManifestWorkLedgerRepository::insertEntry(
    ManifestWorkLedgerEntry entry)
{
    std::lock_guard lock(mutex_);
    const RunId runId = entry.runId;
    if (entries_.contains(runId)) {
        return common::Result<void>::failure({"manifest_work_ledger.duplicate_entry",
            "A Work Ledger entry already exists for this Run"});
    }
    entries_[runId] = std::move(entry);
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestWorkLedgerEntry>>
InMemoryManifestWorkLedgerRepository::findEntry(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(runId);
    if (it == entries_.end()) {
        return common::Result<std::optional<ManifestWorkLedgerEntry>>::ok(std::nullopt);
    }
    return common::Result<std::optional<ManifestWorkLedgerEntry>>::ok(it->second);
}

common::Result<std::vector<ManifestWorkLedgerEntry>>
InMemoryManifestWorkLedgerRepository::listRecentForProject(
    const ProjectId& projectId, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestWorkLedgerEntry> matches;
    for (const auto& [runId, entry] : entries_) {
        if (entry.projectId == projectId) matches.push_back(entry);
    }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.createdAt > b.createdAt;
    });
    if (matches.size() > limit) matches.resize(limit);
    return common::Result<std::vector<ManifestWorkLedgerEntry>>::ok(std::move(matches));
}

common::Result<void> InMemoryManifestWorkLedgerRepository::appendTransition(
    ManifestWorkLedgerReviewTransition transition)
{
    std::lock_guard lock(mutex_);
    const bool duplicate = std::any_of(transitions_.begin(), transitions_.end(),
        [&](const ManifestWorkLedgerReviewTransition& existing) {
            return existing.id == transition.id;
        });
    if (duplicate) {
        return common::Result<void>::failure({"manifest_work_ledger.duplicate_transition",
            "A Work Ledger transition with this id already exists"});
    }
    transitions_.push_back(std::move(transition));
    return common::Result<void>::ok();
}

common::Result<std::vector<ManifestWorkLedgerReviewTransition>>
InMemoryManifestWorkLedgerRepository::listTransitionsForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestWorkLedgerReviewTransition> results;
    for (const auto& transition : transitions_) {
        if (transition.runId == runId) results.push_back(transition);
    }
    return common::Result<std::vector<ManifestWorkLedgerReviewTransition>>::ok(std::move(results));
}

} // namespace ai::application
