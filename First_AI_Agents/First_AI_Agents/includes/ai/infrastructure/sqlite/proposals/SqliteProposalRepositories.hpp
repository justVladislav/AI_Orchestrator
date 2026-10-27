#pragma once

#include "ai/application/proposals/IChangeApprovalRepository.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/proposals/IProposalReviewRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::infrastructure {

/** SQLite implementation of the Stage 1 change-proposal persistence port. */
class SqliteChangeProposalRepository final : public application::IChangeProposalRepository {
public:
    explicit SqliteChangeProposalRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertChangeSet(
        const application::ProposedChangeSet& changeSet) override;

    common::Result<application::ProposedChangeSet> updateChangeSetState(
        const application::ChangeSetId& id,
        application::ChangeSetState newState) override;

    common::Result<void> markMissingEvidence(
        const application::ChangeSetId& id, std::string code, std::string message) override;

    [[nodiscard]] common::Result<std::optional<application::ProposedChangeSet>> findChangeSet(
        const application::ChangeSetId& id) const override;

    [[nodiscard]] common::Result<std::vector<application::ProposedChangeSet>> listForRun(
        const application::RunId& runId) const override;

    [[nodiscard]] common::Result<std::vector<application::ProposedChangeSet>> listForProject(
        const application::ProjectId& projectId) const override;

    [[nodiscard]] common::Result<std::vector<application::ProposedFileChange>> listChanges(
        const application::ChangeSetId& changeSetId) const override;

    [[nodiscard]] common::Result<std::vector<application::ValidationFinding>> listValidationFindings(
        const application::ChangeSetId& changeSetId) const override;

    [[nodiscard]] common::Result<
        std::unordered_map<application::ChangeSetId, std::vector<application::ProposedFileChange>>>
    listChangesForSets(const std::vector<application::ChangeSetId>& changeSetIds) const override;

    [[nodiscard]] common::Result<
        std::unordered_map<application::ChangeSetId, std::vector<application::ValidationFinding>>>
    listValidationFindingsForSets(
        const std::vector<application::ChangeSetId>& changeSetIds) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the change-set approval audit trail. */
class SqliteChangeApprovalRepository final : public application::IChangeApprovalRepository {
public:
    explicit SqliteChangeApprovalRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordApproval(
        const application::ApprovalRecord& record) override;

    [[nodiscard]] common::Result<std::optional<application::ApprovalRecord>> findApproval(
        const application::ChangeSetId& changeSetId) const override;

    common::Result<void> recordApplyOutcome(
        const application::ChangeSetId& changeSetId,
        const std::string& result,
        const std::optional<std::string>& errorCode,
        const std::vector<application::ApplyHashEntry>& hashLog,
        const std::optional<std::string>& postApplyWorkspaceFingerprint) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the durable human proposal-rejection port (AG-07). */
class SqliteProposalReviewRepository final : public application::IProposalReviewRepository {
public:
    explicit SqliteProposalReviewRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordRejection(
        const application::ProposalReviewRejection& record) override;

    [[nodiscard]] common::Result<std::optional<application::ProposalReviewRejection>>
    findRejection(const application::ChangeSetId& changeSetId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
