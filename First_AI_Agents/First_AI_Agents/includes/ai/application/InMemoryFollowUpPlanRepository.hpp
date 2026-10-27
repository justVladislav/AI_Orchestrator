#pragma once

#include "ai/application/planning/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace ai::application {

// The in-memory twin of SqliteFollowUpPlanRepository: it runs the very same contract suite. It has no `runs` table, so the
// Run authority comes from the injected lookup and goes through the very same shared checks; WITHOUT a lookup it checks no
// authority (a double for tests of the shared rules).
//
// The Confirm port needs the Draft store: `drafts` is the in-memory Draft repository the group and its Drafts are saved into. The
// request lock is held for the whole Confirm; the request's next state is computed (and validated) BEFORE the insert, and the
// request is replaced INSIDE the Draft store's own lock (a callback run by insertConfirmedFollowUpGroup after the rows are in),
// so a Confirm is either complete or leaves everything as it was, and a reader of the Draft store never sees the group while
// the request is still plan_ready. The two locks are only ever taken request-then-drafts. Without `drafts` every Confirm is
// refused (`follow_up.confirm_unavailable`).
class InMemoryFollowUpPlanRepository final : public IFollowUpPlanRequestRepository, public IFollowUpPlanConfirmPort {
public:
    explicit InMemoryFollowUpPlanRepository(FollowUpRunProjectLookup runLookup = {}, InMemoryRunDraftRepository* drafts = nullptr);

    common::Result<FollowUpInsertResult> insertQuoted(FollowUpPlanRequest request) override;
    common::Result<FollowUpPlanRequest> transition(
        const FollowUpPlanRequestId& id, FollowUpTransition transition, std::chrono::system_clock::time_point now) override;
    [[nodiscard]] common::Result<std::optional<FollowUpPlanRequest>> findById(const FollowUpPlanRequestId& id) const override;
    [[nodiscard]] common::Result<std::optional<FollowUpPlanRequest>> findOpenForSourceRun(const RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<FollowUpPlanRequest>> findLatestForSourceRun(const RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<FollowUpPlanRequest>> listForProject(
        const ProjectId& projectId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<FollowUpPlanRequest>> listByState(
        FollowUpPlanRequestState state, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<FollowUpPlanRequest>> listByStateAfter(
        FollowUpPlanRequestState state, std::size_t limit, const std::optional<FollowUpStateCursor>& after) const override;

    common::Result<FollowUpConfirmResult> confirmDrafts(
        const FollowUpPlanRequestId& requestId,
        const std::string& expectedPlanFingerprint,
        RunDraftGroup group,
        std::vector<RunDraft> drafts,
        std::chrono::system_clock::time_point now) override;

private:
    [[nodiscard]] std::optional<ProjectId> projectOfRun(const RunId& runId) const;
    // The read authority of one stored request; a no-op without a lookup.
    [[nodiscard]] common::Result<void> checkRead(const FollowUpPlanRequest& request) const;
    [[nodiscard]] std::optional<FollowUpPlanRequest> openForRunLocked(const RunId& runId) const;

    FollowUpRunProjectLookup runLookup_;
    InMemoryRunDraftRepository* drafts_ {nullptr};
    mutable std::mutex mutex_;
    std::map<FollowUpPlanRequestId, FollowUpPlanRequest> requests_;
};

} // namespace ai::application
