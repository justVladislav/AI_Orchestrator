#pragma once

#include "ai/application/FollowUpPlanRequest.hpp"
#include "ai/application/RunDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// PC-5B: the persistence ports of the follow-up plan request. Every rule (the storage gate, the state machine, the
// compare-and-set, the one-open-request slot, the authority of the Run, the Confirm decision) lives in
// FollowUpPlanRequest.hpp and is shared; an adapter only stores what those functions decide, atomically. ONE contract suite
// runs against the in-memory and the SQLite adapter.
//
// Nothing here starts a Run, writes a Run, a proposal, an Apply, the Work Ledger or an Outcome Brief, or calls a provider.

struct FollowUpInsertResult {
    FollowUpPlanRequest request;
    // The `quoted` request of the same source Run that this insert replaced (a free discard, written in the SAME
    // transaction), if there was one.
    std::optional<FollowUpPlanRequestId> discardedQuoteId;
};

// Where the next page of listByStateAfter starts: the key of the last row of the previous page.
struct FollowUpStateCursor {
    std::chrono::system_clock::time_point createdAt;
    FollowUpPlanRequestId id;
};

class IFollowUpPlanRequestRepository {
public:
    virtual ~IFollowUpPlanRequestRepository() = default;

    // Gate (validateFollowUpPlanRequest, a `quoted` request) -> write authority (the source Run exists and belongs to the
    // request's project) -> the one-open slot (planFollowUpQuotedInsert) -> insert, and replace a quoted predecessor, in ONE
    // transaction. Failure codes: follow_up.invalid_request, follow_up.run_not_found, follow_up.project_mismatch,
    // follow_up.request_open (an authorized / composing / plan_ready request exists: never displaced),
    // follow_up.duplicate_id. A refusal writes nothing.
    virtual common::Result<FollowUpInsertResult> insertQuoted(FollowUpPlanRequest request) = 0;

    // ONE compare-and-set step (applyFollowUpTransition), stored together with the fields it sets, in one transaction.
    // Failure codes: follow_up.not_found, follow_up.state_changed and the others of applyFollowUpTransition (for a plan_ready step
    // also the codes of parseFollowUpAnswerPlan: a reply that is not ONE valid plan of the authorised size, with the stored
    // fingerprint, is never stored). A refusal writes nothing; a request is written once per step and a terminal request never again.
    virtual common::Result<FollowUpPlanRequest> transition(
        const FollowUpPlanRequestId& id, FollowUpTransition transition, std::chrono::system_clock::time_point now) = 0;

    // Every read below applies the READ authority and the storage gate: a row that is not exactly what the rules could
    // have produced, or that names another project than its source Run, is `persistence.invalid_data`, never shown.
    [[nodiscard]] virtual common::Result<std::optional<FollowUpPlanRequest>> findById(
        const FollowUpPlanRequestId& id) const = 0;
    // The request of the source Run in an open state (quoted | authorized | composing | plan_ready), if any.
    [[nodiscard]] virtual common::Result<std::optional<FollowUpPlanRequest>> findOpenForSourceRun(
        const RunId& runId) const = 0;
    // The newest request of the source Run in ANY state (createdAt, then id), if any: one row, whatever the Run's history. What the
    // Outcome block's status line shows (PC-5D). The same read authority and storage gate as every other read.
    [[nodiscard]] virtual common::Result<std::optional<FollowUpPlanRequest>> findLatestForSourceRun(
        const RunId& runId) const = 0;
    // The newest `limit` requests of the project (createdAt descending, ties by id descending). limit 1..500.
    [[nodiscard]] virtual common::Result<std::vector<FollowUpPlanRequest>> listForProject(
        const ProjectId& projectId, std::size_t limit) const = 0;
    // The oldest `limit` requests in `state` (createdAt ascending, ties by id): what the owner's processor walks at startup
    // (authorized -> claim or expire, composing -> interrupt). limit 1..500.
    [[nodiscard]] virtual common::Result<std::vector<FollowUpPlanRequest>> listByState(
        FollowUpPlanRequestState state, std::size_t limit) const = 0;
    // The same walk, one page at a time: the oldest `limit` requests in `state` that come STRICTLY AFTER `after` in the order
    // (createdAt ascending, then id ascending); no cursor = from the start. A page whose last row is the next cursor always makes
    // progress, whether or not the rows leave the state meanwhile, so a pass over every request of a state (startup recovery) has
    // no row-count ceiling. limit 1..500.
    [[nodiscard]] virtual common::Result<std::vector<FollowUpPlanRequest>> listByStateAfter(
        FollowUpPlanRequestState state, std::size_t limit, const std::optional<FollowUpStateCursor>& after) const = 0;
};

struct FollowUpConfirmResult {
    FollowUpPlanRequest request; // drafts_saved
    RunDraftGroup group;
    std::vector<RunDraft> drafts; // in ordinal order
    // false: an earlier Confirm of this request already saved the group; THIS call wrote nothing and returned that group.
    bool created {false};
};

// The atomic Confirm: ONE persistence operation, not two repository calls. In one transaction (SQLite) or with the request
// store's lock held and the Draft store's lock around the write (in-memory: the request changes inside the Draft store's own
// critical section, so no reader of the Draft store sees the group before the request says drafts_saved): the request is
// plan_ready and its stored plan fingerprint is the one previewed, the group is this request's, and the Drafts ARE the plan
// that the STORED reply parses to (planFollowUpConfirm) -> the Draft group, its Drafts and their rosters are inserted
// (validateRunDraftGroupInsertion), every source Run of the group exists in the project (checkFollowUpGroupSourceRuns) -> the
// request becomes drafts_saved with the group id. A second Confirm of the same request and plan returns the SAME group and
// writes nothing. It never creates a Run: its only product is a saved Draft group.
//
// Failure codes: follow_up.not_found, follow_up.not_plan_ready, follow_up.plan_changed, follow_up.group_mismatch,
// follow_up.drafts_not_the_plan (and the plan parser's codes if the stored reply no longer parses),
// follow_up.source_run_not_found, follow_up.source_run_wrong_project,
// follow_up.drafts_removed (drafts_saved, but the user removed every draft since: nothing is silently created again), the
// `run_draft_group.*` / `run_draft.*` codes of insertGroup, and persistence errors. A refusal leaves the request plan_ready
// and writes no group and no draft.
class IFollowUpPlanConfirmPort {
public:
    virtual ~IFollowUpPlanConfirmPort() = default;

    virtual common::Result<FollowUpConfirmResult> confirmDrafts(
        const FollowUpPlanRequestId& requestId,
        const std::string& expectedPlanFingerprint,
        RunDraftGroup group,
        std::vector<RunDraft> drafts,
        std::chrono::system_clock::time_point now) = 0;
};

} // namespace ai::application
