#pragma once

#include <string>

namespace ai::application {

// PC-4C: how the producers of outcome facts tell the Outcome Brief reconciler that something happened. It is only a NUDGE: the brief
// is always recomputed from the durable facts, so a lost or failed call costs freshness, never correctness (the startup pass and the
// on-demand pull are the source of truth). Every call is best effort and must never throw, never fail the caller and never change a
// Run, a proposal, an Apply or the Work Ledger - which is why the methods return nothing.
class IOutcomeBriefTrigger {
public:
    virtual ~IOutcomeBriefTrigger() = default;

    // The Run (a RunId) reached a terminal status, got a continuation child or a manual handoff, or lost its handoff: its brief may
    // have changed.
    virtual void runMayHaveChanged(const std::string& runId) noexcept = 0;

    // A change set (a ChangeSetId) was approved, rejected or applied: the brief of its Run - and of the handoff anchor it belongs to,
    // if any - may have changed.
    virtual void proposalResolved(const std::string& changeSetId) noexcept = 0;
};

} // namespace ai::application
