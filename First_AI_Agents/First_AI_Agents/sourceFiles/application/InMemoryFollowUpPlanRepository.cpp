#include "ai/application/InMemoryFollowUpPlanRepository.hpp"

#include "ai/application/RunDraftGroup.hpp"

#include <algorithm>

namespace ai::application {
namespace {

template <typename T>
common::Result<T> failWith(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

} // namespace

InMemoryFollowUpPlanRepository::InMemoryFollowUpPlanRepository(FollowUpRunProjectLookup runLookup, InMemoryRunDraftRepository* drafts)
    : runLookup_(std::move(runLookup))
    , drafts_(drafts)
{
}

std::optional<ProjectId> InMemoryFollowUpPlanRepository::projectOfRun(const RunId& runId) const
{
    return runLookup_ ? runLookup_(runId) : std::nullopt;
}

common::Result<void> InMemoryFollowUpPlanRepository::checkRead(const FollowUpPlanRequest& request) const
{
    if (!runLookup_) return common::Result<void>::ok();
    return checkFollowUpRequestReadAuthority(request, projectOfRun(request.sourceRunId));
}

std::optional<FollowUpPlanRequest> InMemoryFollowUpPlanRepository::openForRunLocked(const RunId& runId) const
{
    for (const auto& [id, request] : requests_) {
        static_cast<void>(id);
        if (request.sourceRunId == runId && isOpenFollowUpPlanRequestState(request.state)) return request;
    }
    return std::nullopt;
}

common::Result<FollowUpInsertResult> InMemoryFollowUpPlanRepository::insertQuoted(FollowUpPlanRequest request)
{
    using Result = common::Result<FollowUpInsertResult>;
    const auto gate = validateFollowUpPlanRequest(request);
    if (!gate.success()) return Result::failure(*gate.error());
    if (request.state != FollowUpPlanRequestState::Quoted) {
        return failWith<FollowUpInsertResult>("follow_up.invalid_request", "A request is inserted as quoted");
    }
    if (runLookup_) {
        const auto authority = checkFollowUpRequestWriteAuthority(request, projectOfRun(request.sourceRunId));
        if (!authority.success()) return Result::failure(*authority.error());
    }

    std::lock_guard lock(mutex_);
    if (requests_.contains(request.id)) {
        return failWith<FollowUpInsertResult>("follow_up.duplicate_id", "A follow-up request with this id already exists");
    }
    const auto open = openForRunLocked(request.sourceRunId);
    const auto replaced = planFollowUpQuotedInsert(request, open);
    if (!replaced.success()) return Result::failure(*replaced.error());

    // Everything is computed before the first write: a refusal leaves the store as it was.
    std::optional<FollowUpPlanRequest> discarded;
    if (replaced.value().has_value()) {
        FollowUpTransition discard;
        discard.event = FollowUpPlanEvent::Discard;
        discard.expectedState = FollowUpPlanRequestState::Quoted;
        const auto next = applyFollowUpTransition(*open, discard, request.createdAt);
        if (!next.success()) return Result::failure(*next.error());
        discarded = next.value();
    }
    FollowUpInsertResult result;
    result.request = request;
    result.discardedQuoteId = replaced.value();
    if (discarded.has_value()) requests_[discarded->id] = std::move(*discarded);
    requests_.emplace(request.id, std::move(request));
    return Result::ok(std::move(result));
}

common::Result<FollowUpPlanRequest> InMemoryFollowUpPlanRepository::transition(
    const FollowUpPlanRequestId& id, FollowUpTransition transition, std::chrono::system_clock::time_point now)
{
    using Result = common::Result<FollowUpPlanRequest>;
    std::lock_guard lock(mutex_);
    const auto found = requests_.find(id);
    if (found == requests_.end()) return failWith<FollowUpPlanRequest>("follow_up.not_found", "No such follow-up request");
    const auto authority = checkRead(found->second);
    if (!authority.success()) return Result::failure(*authority.error());
    auto next = applyFollowUpTransition(found->second, transition, now);
    if (!next.success()) return Result::failure(*next.error());
    found->second = next.value();
    return Result::ok(std::move(next.value()));
}

common::Result<std::optional<FollowUpPlanRequest>> InMemoryFollowUpPlanRepository::findById(
    const FollowUpPlanRequestId& id) const
{
    using Result = common::Result<std::optional<FollowUpPlanRequest>>;
    std::lock_guard lock(mutex_);
    const auto found = requests_.find(id);
    if (found == requests_.end()) return Result::ok(std::nullopt);
    const auto authority = checkRead(found->second);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(found->second);
}

common::Result<std::optional<FollowUpPlanRequest>> InMemoryFollowUpPlanRepository::findOpenForSourceRun(const RunId& runId) const
{
    using Result = common::Result<std::optional<FollowUpPlanRequest>>;
    std::lock_guard lock(mutex_);
    const auto open = openForRunLocked(runId);
    if (!open.has_value()) return Result::ok(std::nullopt);
    const auto authority = checkRead(*open);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(open);
}

common::Result<std::optional<FollowUpPlanRequest>> InMemoryFollowUpPlanRepository::findLatestForSourceRun(const RunId& runId) const
{
    using Result = common::Result<std::optional<FollowUpPlanRequest>>;
    std::lock_guard lock(mutex_);
    const FollowUpPlanRequest* latest = nullptr;
    for (const auto& [id, request] : requests_) {
        static_cast<void>(id);
        if (request.sourceRunId != runId) continue;
        if (latest == nullptr || request.createdAt > latest->createdAt
            || (request.createdAt == latest->createdAt && request.id > latest->id)) {
            latest = &request;
        }
    }
    if (latest == nullptr) return Result::ok(std::nullopt);
    const auto authority = checkRead(*latest);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(*latest);
}

common::Result<std::vector<FollowUpPlanRequest>> InMemoryFollowUpPlanRepository::listForProject(
    const ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<FollowUpPlanRequest>>;
    const auto checked = checkFollowUpListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());
    std::lock_guard lock(mutex_);
    std::vector<FollowUpPlanRequest> rows;
    for (const auto& [id, request] : requests_) {
        static_cast<void>(id);
        // A request belongs to the list of the project it NAMES and to the list of the project its Run belongs to: when the
        // two differ, both lists meet the row and both fail closed (the SQLite adapter joins `runs` for the same reason).
        const bool named = request.projectId == projectId;
        const bool owned = runLookup_ && projectOfRun(request.sourceRunId) == std::optional<ProjectId>(projectId);
        if (named || owned) rows.push_back(request);
    }
    std::sort(rows.begin(), rows.end(), [](const FollowUpPlanRequest& left, const FollowUpPlanRequest& right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.id > right.id;
    });
    if (rows.size() > limit) rows.resize(limit);
    for (const auto& row : rows) {
        const auto authority = checkRead(row);
        if (!authority.success()) return Result::failure(*authority.error());
    }
    return Result::ok(std::move(rows));
}

common::Result<std::vector<FollowUpPlanRequest>> InMemoryFollowUpPlanRepository::listByState(
    FollowUpPlanRequestState state, std::size_t limit) const
{
    return listByStateAfter(state, limit, std::nullopt);
}

common::Result<std::vector<FollowUpPlanRequest>> InMemoryFollowUpPlanRepository::listByStateAfter(
    FollowUpPlanRequestState state, std::size_t limit, const std::optional<FollowUpStateCursor>& after) const
{
    using Result = common::Result<std::vector<FollowUpPlanRequest>>;
    const auto checked = checkFollowUpListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());
    std::lock_guard lock(mutex_);
    std::vector<FollowUpPlanRequest> rows;
    for (const auto& [id, request] : requests_) {
        static_cast<void>(id);
        if (request.state != state) continue;
        // Strictly after the cursor in (createdAt, id) order.
        if (after.has_value() && (request.createdAt < after->createdAt
                || (request.createdAt == after->createdAt && request.id <= after->id))) {
            continue;
        }
        rows.push_back(request);
    }
    std::sort(rows.begin(), rows.end(), [](const FollowUpPlanRequest& left, const FollowUpPlanRequest& right) {
        if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
        return left.id < right.id;
    });
    if (rows.size() > limit) rows.resize(limit);
    for (const auto& row : rows) {
        const auto authority = checkRead(row);
        if (!authority.success()) return Result::failure(*authority.error());
    }
    return Result::ok(std::move(rows));
}

common::Result<FollowUpConfirmResult> InMemoryFollowUpPlanRepository::confirmDrafts(
    const FollowUpPlanRequestId& requestId,
    const std::string& expectedPlanFingerprint,
    RunDraftGroup group,
    std::vector<RunDraft> drafts,
    std::chrono::system_clock::time_point now)
{
    using Result = common::Result<FollowUpConfirmResult>;
    if (drafts_ == nullptr) {
        return failWith<FollowUpConfirmResult>("follow_up.confirm_unavailable", "This repository has no Draft store to confirm into");
    }
    std::lock_guard lock(mutex_);
    const auto found = requests_.find(requestId);
    if (found == requests_.end()) return failWith<FollowUpConfirmResult>("follow_up.not_found", "No such follow-up request");
    const auto authority = checkRead(found->second);
    if (!authority.success()) return Result::failure(*authority.error());

    const auto action = planFollowUpConfirm(found->second, expectedPlanFingerprint, group, drafts);
    if (!action.success()) return Result::failure(*action.error());

    if (action.value() == FollowUpConfirmAction::ReturnExisting) {
        const auto storedGroup = drafts_->findGroupById(*found->second.draftGroupId);
        if (!storedGroup.success()) return Result::failure(*storedGroup.error());
        if (!storedGroup.value().has_value() || storedGroup.value()->sourceRequestId != std::optional<std::string>(requestId)) {
            return failWith<FollowUpConfirmResult>("follow_up.drafts_removed",
                "The drafts of this request were removed after they were saved");
        }
        auto stored = drafts_->listGroupDrafts(storedGroup.value()->id);
        if (!stored.success()) return Result::failure(*stored.error());
        FollowUpConfirmResult result;
        result.request = found->second;
        result.group = std::move(*storedGroup.value());
        result.drafts = std::move(stored.value());
        result.created = false;
        return Result::ok(std::move(result));
    }

    // The next state is validated first: if the request cannot become drafts_saved, no group is written.
    auto next = applyFollowUpConfirm(found->second, group.id, now);
    if (!next.success()) return Result::failure(*next.error());
    // The same order as the SQLite adapter: the group's own validation, then its source Runs, then the write.
    const auto valid = validateRunDraftGroupInsertion(group, drafts);
    if (!valid.success()) return Result::failure(*valid.error());
    // The extra source Runs of the group must exist in the project (without a lookup there is no Run authority to apply).
    if (runLookup_) {
        const auto sources = checkFollowUpGroupSourceRuns(group, found->second.projectId, [&](const RunId& runId) {
            return common::Result<std::optional<ProjectId>>::ok(projectOfRun(runId));
        });
        if (!sources.success()) return Result::failure(*sources.error());
    }
    // The request changes INSIDE the Draft store's lock, right after the rows go in: the store never shows the group while
    // the request is still plan_ready. (The request lock is held around it, and nothing takes the two in the other order.)
    auto inserted = drafts_->insertConfirmedFollowUpGroup(group, std::move(drafts), [&] { found->second = next.value(); });
    if (!inserted.success()) return Result::failure(*inserted.error());

    FollowUpConfirmResult result;
    result.request = std::move(next.value());
    result.group = std::move(group);
    result.drafts = std::move(inserted.value());
    result.created = true;
    return Result::ok(std::move(result));
}

} // namespace ai::application
