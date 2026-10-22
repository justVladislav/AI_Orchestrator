#include "ai/application/InMemoryOutcomeBriefRepository.hpp"

#include <algorithm>

namespace ai::application {

InMemoryOutcomeBriefRepository::InMemoryOutcomeBriefRepository(OutcomeBriefRunLookup runLookup)
    : runLookup_(std::move(runLookup))
{
}

std::optional<OutcomeBriefRunOwner> InMemoryOutcomeBriefRepository::ownerOf(const RunId& runId) const
{
    return runLookup_ ? runLookup_(runId) : std::nullopt;
}

common::Result<void> InMemoryOutcomeBriefRepository::checkRead(const OutcomeBriefRevision& revision, bool isHead) const
{
    if (!runLookup_) return common::Result<void>::ok();
    return checkOutcomeBriefReadAuthority(revision.brief, ownerOf(revision.brief.runId), isHead);
}

common::Result<OutcomeBriefAppendResult> InMemoryOutcomeBriefRepository::append(
    OutcomeBrief brief, OutcomeBriefExpectedHead expected)
{
    using Result = common::Result<OutcomeBriefAppendResult>;
    const auto checked = checkOutcomeBriefAppendRequest(brief, expected);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(mutex_);
    if (runLookup_) {
        const auto authority = checkOutcomeBriefWriteAuthority(brief, ownerOf(brief.runId));
        if (!authority.success()) return Result::failure(*authority.error());
    }
    const auto existing = revisionsByRun_.find(brief.runId);
    std::optional<OutcomeBriefRevision> head;
    if (existing != revisionsByRun_.end() && !existing->second.empty()) head = existing->second.back();
    if (head.has_value()) {
        if (runLookup_) {
            // The head is compared with, not shown: a status that went stale is normal here, a foreign project is not.
            const auto authority = checkRead(*head, /*isHead=*/false);
            if (!authority.success()) return Result::failure(*authority.error());
        } else if (head->brief.projectId != brief.projectId) {
            return Result::failure({"outcome_brief.project_mismatch", "The Run belongs to another project"});
        }
    }
    const auto plan = planOutcomeBriefAppend(brief, expected, head);
    if (!plan.success()) return Result::failure(*plan.error());

    OutcomeBriefAppendResult result;
    if (!plan.value().appendRevision) {
        result.head = *head;
        result.appended = false;
        return Result::ok(std::move(result));
    }
    const RunId runId = brief.runId;
    auto& history = revisionsByRun_[runId];
    history.push_back(makeOutcomeBriefRevision(std::move(brief), plan.value().ordinal));
    result.head = history.back();
    result.appended = true;
    return Result::ok(std::move(result));
}

common::Result<std::optional<OutcomeBriefRevision>> InMemoryOutcomeBriefRepository::findHead(const RunId& runId) const
{
    using Result = common::Result<std::optional<OutcomeBriefRevision>>;
    std::lock_guard lock(mutex_);
    const auto found = revisionsByRun_.find(runId);
    if (found == revisionsByRun_.end() || found->second.empty()) return Result::ok(std::nullopt);
    const auto authority = checkRead(found->second.back(), /*isHead=*/true);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(found->second.back());
}

common::Result<std::optional<OutcomeBriefRevision>> InMemoryOutcomeBriefRepository::findRevision(
    const OutcomeBriefRevisionId& id) const
{
    using Result = common::Result<std::optional<OutcomeBriefRevision>>;
    std::lock_guard lock(mutex_);
    for (const auto& [runId, history] : revisionsByRun_) {
        for (const auto& revision : history) {
            if (revision.id != id) continue;
            const auto authority = checkRead(revision, /*isHead=*/revision.ordinal == history.back().ordinal);
            if (!authority.success()) return Result::failure(*authority.error());
            return Result::ok(revision);
        }
    }
    return Result::ok(std::nullopt);
}

common::Result<std::vector<OutcomeBriefRevision>> InMemoryOutcomeBriefRepository::listRevisions(
    const RunId& runId, std::size_t limit) const
{
    using Result = common::Result<std::vector<OutcomeBriefRevision>>;
    const auto checked = checkOutcomeBriefListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(mutex_);
    const auto found = revisionsByRun_.find(runId);
    if (found == revisionsByRun_.end()) return Result::ok(std::vector<OutcomeBriefRevision> {});
    const auto& history = found->second;
    const std::size_t first = history.size() > limit ? history.size() - limit : 0;
    std::vector<OutcomeBriefRevision> revisions(history.begin() + static_cast<std::ptrdiff_t>(first), history.end());
    for (const auto& revision : revisions) {
        const auto authority = checkRead(revision, /*isHead=*/revision.ordinal == history.back().ordinal);
        if (!authority.success()) return Result::failure(*authority.error());
    }
    return Result::ok(std::move(revisions));
}

common::Result<std::vector<OutcomeBriefRevision>> InMemoryOutcomeBriefRepository::listHeads(
    const ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<OutcomeBriefRevision>>;
    const auto checked = checkOutcomeBriefListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(mutex_);
    std::vector<OutcomeBriefRevision> heads;
    for (const auto& [runId, history] : revisionsByRun_) {
        if (history.empty()) continue;
        // A head belongs to the list of the project it NAMES and to the list of the project its Run belongs to: when the two
        // differ, both lists meet the row and both fail closed instead of one of them silently missing it.
        const auto owner = ownerOf(runId);
        const bool named = history.back().brief.projectId == projectId;
        const bool owned = owner.has_value() && owner->projectId == projectId;
        if (!named && !owned) continue;
        heads.push_back(history.back());
    }
    std::sort(heads.begin(), heads.end(), outcomeBriefHeadComesBefore);
    if (heads.size() > limit) heads.resize(limit);
    // A bounded page: the authority is checked for the heads of the returned window only (like the SQLite adapter, which applies
    // the LIMIT in the query and then reads the rows it got), so a stale head outside the window does not matter.
    for (const auto& head : heads) {
        const auto authority = checkRead(head, /*isHead=*/true);
        if (!authority.success()) return Result::failure(*authority.error());
    }
    return Result::ok(std::move(heads));
}

common::Result<std::optional<OutcomeBriefHeadVersion>> InMemoryOutcomeBriefRepository::findHeadVersionForAppend(
    const RunId& runId) const
{
    using Result = common::Result<std::optional<OutcomeBriefHeadVersion>>;
    std::lock_guard lock(mutex_);
    const auto found = revisionsByRun_.find(runId);
    if (found == revisionsByRun_.end() || found->second.empty()) {
        // No head: the Run must still exist (without a lookup the adapter knows no Runs and cannot tell).
        if (runLookup_) {
            const auto exists = checkOutcomeBriefRunExists(ownerOf(runId).has_value());
            if (!exists.success()) return Result::failure(*exists.error());
        }
        return Result::ok(std::nullopt);
    }
    // The same view of the head that `append` compares with: the project is checked, the status is not.
    const auto authority = checkRead(found->second.back(), /*isHead=*/false);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(OutcomeBriefHeadVersion {found->second.back().id, found->second.back().ordinal});
}

} // namespace ai::application
