#pragma once

#include "ai/application/proposals/ProposalReviewTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

// Persistence boundary for durable human reject/discard decisions. See
// ProposalReviewTypes.hpp for why this is a separate concern from both
// IChangeProposalRepository (validator truth, immutable) and
// IChangeApprovalRepository (the positive approve/apply audit trail).
class IProposalReviewRepository {
public:
    virtual ~IProposalReviewRepository() = default;

    // Fails with "proposal.duplicate_id" if changeSetId already has a
    // rejection row - ProposalReviewService::reject() checks findRejection()
    // first and returns the existing record instead of calling this twice
    // for the same change set.
    virtual common::Result<void> recordRejection(
        const ProposalReviewRejection& record) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ProposalReviewRejection>>
    findRejection(const ChangeSetId& changeSetId) const = 0;
};

} // namespace ai::application
