#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// Persistence boundary for the change-set approval audit trail. Deliberately
// separate from IChangeProposalRepository: that interface's records are
// immutable except for the state field, while an approval row is written
// once by approve() and then updated once more by recordApplyOutcome() -
// a different write pattern for a different concern.
class IChangeApprovalRepository {
public:
    virtual ~IChangeApprovalRepository() = default;

    // Fails with "proposal.duplicate_id" if changeSetId already has an
    // approval row - callers should check findApproval() first (see
    // ChangeSetApplyService::approve()'s idempotent-on-matching-hash rule).
    virtual common::Result<void> recordApproval(const ApprovalRecord& record) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ApprovalRecord>> findApproval(
        const ChangeSetId& changeSetId) const = 0;

    // Updates the existing approval row with the outcome of an apply()
    // attempt. Fails with "proposal.not_found" if no approval row exists yet
    // for changeSetId - apply() cannot reach this without one (see the
    // approval gate at the top of ChangeSetApplyService::apply()).
    virtual common::Result<void> recordApplyOutcome(
        const ChangeSetId& changeSetId,
        const std::string& result,
        const std::optional<std::string>& errorCode,
        const std::vector<ApplyHashEntry>& hashLog,
        const std::optional<std::string>& postApplyWorkspaceFingerprint) = 0;
};

} // namespace ai::application
