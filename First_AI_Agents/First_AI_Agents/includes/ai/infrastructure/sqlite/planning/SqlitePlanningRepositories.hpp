#pragma once

#include "ai/application/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/IOutcomeBriefRepository.hpp"
#include "ai/application/planning/IRunDraftRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ai::infrastructure {

class SqliteRunDraftRepository final : public application::IRunDraftRepository {
public:
    explicit SqliteRunDraftRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::RunDraft> insert(application::RunDraft draft) override;
    common::Result<application::RunDraft> update(application::RunDraft draft) override;
    [[nodiscard]] common::Result<std::optional<application::RunDraft>> findById(
        const application::RunDraftId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::RunDraft>> listAll() const override;
    // Also removes the draft's group in the same transaction when it was the group's last draft (PC-3B).
    common::Result<void> remove(const application::RunDraftId& id) override;
    // PC-3B: the group row and every draft with its roster rows in ONE transaction (schema v62).
    common::Result<std::vector<application::RunDraft>> insertGroup(
        application::RunDraftGroup group, std::vector<application::RunDraft> drafts) override;
    [[nodiscard]] common::Result<std::optional<application::RunDraftGroup>> findGroupById(
        const application::RunDraftGroupId& id) const override;
    [[nodiscard]] common::Result<std::optional<application::RunDraftGroup>> findGroupByIdempotencyKey(
        const std::string& idempotencyKey) const override;
    [[nodiscard]] common::Result<std::vector<application::RunDraft>> listGroupDrafts(
        const application::RunDraftGroupId& id) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

// PC-4B: the append-only revisions of the Outcome Brief and the derived head pointer (schema v64). The write gate is
// validateOutcomeBrief; a revision and its head row are written in ONE transaction, and every read passes the gate again.
class SqliteOutcomeBriefRepository final : public application::IOutcomeBriefRepository {
public:
    explicit SqliteOutcomeBriefRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::OutcomeBriefAppendResult> append(
        application::OutcomeBrief brief, application::OutcomeBriefExpectedHead expected) override;
    [[nodiscard]] common::Result<std::optional<application::OutcomeBriefRevision>> findHead(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<application::OutcomeBriefRevision>> findRevision(
        const application::OutcomeBriefRevisionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::OutcomeBriefRevision>> listRevisions(
        const application::RunId& runId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::OutcomeBriefRevision>> listHeads(
        const application::ProjectId& projectId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::optional<application::OutcomeBriefHeadVersion>> findHeadVersionForAppend(
        const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

// PC-5B: the durable follow-up plan request (schema v66) and the atomic Confirm of its plan. Every rule is the shared one of
// FollowUpPlanRequest.hpp; this adapter stores what those functions decide. A transition is a compare-and-set written in ONE
// transaction (BEGIN IMMEDIATE), and the Confirm writes the Draft group, its Drafts and drafts_saved in ONE transaction. The
// Run authority is read from `runs` inside the transaction and again on every read; the schema triggers keep the identity
// and the quote immutable and the states on the agreed edges even for a writer that bypasses this class.
class SqliteFollowUpPlanRepository final : public application::IFollowUpPlanRequestRepository,
                                           public application::IFollowUpPlanConfirmPort {
public:
    explicit SqliteFollowUpPlanRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::FollowUpInsertResult> insertQuoted(application::FollowUpPlanRequest request) override;
    common::Result<application::FollowUpPlanRequest> transition(
        const application::FollowUpPlanRequestId& id,
        application::FollowUpTransition transition,
        std::chrono::system_clock::time_point now) override;
    [[nodiscard]] common::Result<std::optional<application::FollowUpPlanRequest>> findById(
        const application::FollowUpPlanRequestId& id) const override;
    [[nodiscard]] common::Result<std::optional<application::FollowUpPlanRequest>> findOpenForSourceRun(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<application::FollowUpPlanRequest>> findLatestForSourceRun(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<application::FollowUpPlanRequest>> listForProject(
        const application::ProjectId& projectId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::FollowUpPlanRequest>> listByState(
        application::FollowUpPlanRequestState state, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::FollowUpPlanRequest>> listByStateAfter(
        application::FollowUpPlanRequestState state, std::size_t limit,
        const std::optional<application::FollowUpStateCursor>& after) const override;

    common::Result<application::FollowUpConfirmResult> confirmDrafts(
        const application::FollowUpPlanRequestId& requestId,
        const std::string& expectedPlanFingerprint,
        application::RunDraftGroup group,
        std::vector<application::RunDraft> drafts,
        std::chrono::system_clock::time_point now) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
