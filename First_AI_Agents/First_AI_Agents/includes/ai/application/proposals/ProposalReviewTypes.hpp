#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace ai::application {

// Durable record that a human explicitly rejected/discarded a specific
// ReadyForReview change set instead of approving it.
//
// Deliberately separate from ChangeSetState::Rejected (a Backend validator
// safety finding produced before the proposal was ever shown for review) and
// from ApprovalRecord (the positive approve()/apply() path): AG-07 requires
// that "validation rejection cannot be relabeled as human rejection." A
// change set's own `state` field is therefore never mutated by this
// decision - it stays exactly what the Backend/validator produced. A reader
// distinguishes "safe but the human chose not to use it" from "unsafe" by
// checking for this record's presence, not by re-interpreting `state`.
//
// One row per change set: recording a rejection is the terminal human
// decision for that change set. It does not delete or alter any existing
// ApprovalRecord/apply audit trail; a change set rejected before it was ever
// approved simply never gets one.
struct ProposalReviewRejection {
    ChangeSetId changeSetId;
    std::chrono::system_clock::time_point rejectedAt;

    // Best-effort OS username annotation, matching ApprovalRecord::approvedBy -
    // not an authenticated identity.
    std::optional<std::string> rejectedBy;

    // Optional bounded human-entered reason. See
    // ProposalReviewService::kMaximumReasonBytes for the exact cap; the
    // service rejects an over-limit reason before this is ever constructed.
    std::optional<std::string> reason;
};

} // namespace ai::application
