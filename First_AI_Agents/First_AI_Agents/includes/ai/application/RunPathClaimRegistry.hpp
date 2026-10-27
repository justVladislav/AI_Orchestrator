#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/proposals/ChangeProposalTypes.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::application {

// In-memory, run-scoped arbiter of which relative workspace path each
// concurrently-executing agent's ProposedChangeSet has claimed inside one
// still-running Run. Exists only to catch the cross-agent race the intra-
// changeset ChangeProposalValidator::validate check (proposal.conflicting_path)
// cannot see: two DIFFERENT proposals (different changeSetId, produced by two
// agents racing in the same parallel batch inside
// ExecutionProfileWorkflowRunner::run()) each individually valid, targeting
// the same relative path.
//
// Not a persistence port: claims are pure in-process arbitration state with
// no durable meaning of their own (IChangeProposalRepository/Store remain the
// sole source of truth for a proposal's actual review/apply status).
//
// Bounded lifetime: every claim for a runId MUST be dropped via releaseRun()
// once that Run reaches any terminal state (RunManager::finishRun), or
// process memory grows without bound across the app's lifetime.
class RunPathClaimRegistry final {
public:
    struct PathClaim {
        // Must be pre-canonicalized exactly like ChangeProposalValidator does
        // (std::filesystem::path::generic_string()), so both conflict checks
        // agree on what "the same path" means.
        std::string relativePathGeneric;
        // The specific change owning this path, for the finding's changeId.
        ChangeId changeId;
    };

    struct ClaimOutcome {
        bool claimed {false};
        // Set only when claimed == false: identifies the already-registered
        // change and change set that own the colliding path. Returning the
        // winner (not the rejected request) makes persisted conflict
        // diagnostics actionable in Run Details.
        ChangeId conflictingChangeId;
        ChangeSetId conflictingChangeSetId;
        // Manifest verification reliability hardening follow-up: set ONLY by
        // transferClaim (always empty from tryClaim) to the EXACT path set
        // fromChangeSetId held immediately before that call - empty if it
        // held none. A caller that later needs to roll a successful transfer
        // back (e.g. a downstream persistence failure discovered after this
        // call already returned claimed == true) must pass THIS snapshot to
        // the reverse transferClaim call, never a separately reconstructed
        // approximation: only this snapshot is guaranteed to match what was
        // actually transferred, so passing anything else risks fabricating a
        // claim for fromChangeSetId that it never actually held.
        std::vector<PathClaim> previousClaims;
    };

    // All-or-nothing: attempts to claim every path in `claims` for
    // (runId, changeSetId). If ANY path is already claimed by a DIFFERENT
    // changeSetId in the same run, NOTHING is claimed and the first
    // already-registered colliding claim is returned. Re-claiming paths this same
    // changeSetId already owns is a no-op success (idempotent - matters if
    // ChangeProposalService is ever called twice for the same changeSetId).
    [[nodiscard]] ClaimOutcome tryClaim(
        const RunId& runId,
        const ChangeSetId& changeSetId,
        const std::vector<PathClaim>& claims);

    // Releases every path this changeSetId owns within runId (no-op if it
    // owns none). Called when a claim was taken but the changeset does not
    // end up durably ReadyForReview, so a legitimate same-agent retry never
    // self-conflicts against its own abandoned attempt.
    void release(const RunId& runId, const ChangeSetId& changeSetId);

    // Manifest verification reliability hardening (lineage-aware repair
    // materialization): atomically replaces fromChangeSetId's claim with
    // toChangeSetId's, under ONE lock acquisition - unlike a caller doing
    // release(from) followed by tryClaim(to, ...) as two separate calls,
    // this never has a window where neither changeset holds the paths, and a
    // failed transfer restores fromChangeSetId's exact original claim set
    // instead of losing it. Typically used when toChangeSetId's `claims`
    // substantially overlap fromChangeSetId's own already-held paths (a
    // repair replacing its own task's predecessor proposal) - tryClaim alone
    // could never succeed there, since fromChangeSetId's still-held claim
    // would itself look like a conflict.
    //
    // On success: every path fromChangeSetId held is released, and every
    // path in `claims` now belongs to toChangeSetId (a path claim can name a
    // different set than fromChangeSetId's own - e.g. one carried-forward
    // path replaced by a differently-named repair change, or a genuinely new
    // path added by the repair - so this is a general "claim exactly this
    // set for toChangeSetId, releasing fromChangeSetId's former claim as
    // part of the same atomic step", not a literal rename).
    // On failure (some `claims` entry is already held by a THIRD changeSetId
    // that is neither fromChangeSetId nor toChangeSetId): fromChangeSetId's
    // original claims are restored exactly as they were before this call,
    // and toChangeSetId ends up owning nothing new. Idempotent/no-op-safe
    // when fromChangeSetId never actually held a claim (its snapshot is then
    // simply empty and nothing is restored on failure) - the exact same
    // "safe no-op" contract release() already documents. Either way, the
    // returned ClaimOutcome::previousClaims is the exact snapshot this call
    // captured - see its own doc comment for why a caller doing its OWN
    // later rollback must reuse it verbatim.
    [[nodiscard]] ClaimOutcome transferClaim(
        const RunId& runId,
        const ChangeSetId& fromChangeSetId,
        const ChangeSetId& toChangeSetId,
        const std::vector<PathClaim>& claims);

    // RunManager::finishRun cleanup hook: drops every claim for runId
    // regardless of state. Safe to call for a run that was never claimed
    // against (Mock runs, non-workspace rosters) - a no-op lookup miss.
    void releaseRun(const RunId& runId);

private:
    struct Claim {
        ChangeSetId changeSetId;
        ChangeId changeId;
    };

    std::mutex mutex_;
    std::unordered_map<RunId, std::unordered_map<std::string, Claim>> claimsByRun_;
};

} // namespace ai::application
