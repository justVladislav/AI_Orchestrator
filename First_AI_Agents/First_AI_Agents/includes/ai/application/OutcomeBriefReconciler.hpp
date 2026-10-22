#pragma once

#include "ai/application/IOutcomeBriefRepository.hpp"
#include "ai/application/IOutcomeBriefTrigger.hpp"
#include "ai/application/OutcomeBrief.hpp"
#include "ai/application/OutcomeBriefSources.hpp"
#include "ai/common/Result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::application {

// What one reconcile of one Run did.
struct OutcomeBriefReconcileOutcome {
    enum class Kind {
        Skipped,     // not an outcome Run (yet): unknown, not terminal, not an ordinary Manifest Run / continuation child
        Unchanged,   // the head already says exactly this
        Appended,    // a new revision (the first brief of the Run, or a changed outcome)
    };
    Kind kind {Kind::Skipped};
    std::string skippedBecause;                 // Kind::Skipped only
    std::optional<OutcomeBriefState> state;     // Unchanged / Appended: the state of the head
    std::optional<std::uint64_t> ordinal;       // Unchanged / Appended: the ordinal of the head
};

// How many nudges the reconciler has received (whatever came of them): a diagnostic that also lets a test see that a producer is wired.
struct OutcomeBriefNudgeCounts {
    std::uint64_t runs {0};
    std::uint64_t proposals {0};
};

struct OutcomeBriefHistoryOptions {
    // The newest terminal outcome Runs of each project that always get a brief; older ones only while their outcome is not settled.
    std::size_t newestPerProject {200};
    // Polled before every Run and every page; true stops the pass (a shutting-down runtime).
    std::function<bool()> shouldStop;
    // How many Runs of a project are read at once (1..kMaxRunPageSize): the pass never holds more than one page of Runs.
    std::size_t pageSize {64};
};

struct OutcomeBriefHistoryResult {
    std::size_t examined {0};     // terminal outcome Runs looked at
    std::size_t appended {0};
    std::size_t unchanged {0};
    std::size_t skipped {0};      // older Runs whose outcome is settled and that have no brief: left alone
    std::size_t failed {0};       // a Run whose facts could not be read or whose brief could not be stored, or a project whose page could not
    std::vector<RunId> failedRunIds;   // the first few Runs of them
    std::vector<ProjectId> failedProjectIds;   // projects whose pages could not be read (their remaining Runs were not visited)
    std::size_t pagesRead {0};
    bool stopped {false};
};

// PC-4C: the pull-based reconciler of the Outcome Brief. It reads durable facts through the READ-ONLY IOutcomeBriefSources, derives the
// brief (PC-4A) and stores a revision when the outcome changed (PC-4B); the brief repository is the ONLY thing it writes. It never
// changes a Run, a proposal, an Apply, a review decision or the Work Ledger, calls no provider and starts nothing.
//
// Idempotent and restart-safe: the same facts always give the same source state, a revision is added only when it differs from the
// CURRENT head, and the write is a compare-and-set on the head id the reconciler read, so a hook and the startup pass that race on the
// same Run yield one winner and a retry that finds the other's result unchanged. IOutcomeBriefTrigger is the best-effort entry the
// producers use; reconcileRun / reconcileHistory are the entries that report what happened.
//
// The compare-and-set alone cannot see an A -> B -> A: a reconcile that read the head (A) and then facts that say B, while the world
// goes back to A and a second reconcile finds the head already saying A (nothing to write, so the head id does not change), would
// still pass the compare-and-set and store B for good. So the verdicts of ONE reconciler are also validated optimistically: an attempt
// notes how many verdicts of its Run had been stored when it began and, in the one critical section that also holds the write, gives up
// and reads everything again when another attempt has stored one since - unless that verdict says exactly what it was about to say.
// That needs this reconciler to be the ONLY writer of the briefs, and it is made so outside this class: the runtime that composes it holds
// the ownership lock of its database file (infrastructure::DatabaseOwnerLock - one live owner per file, another instance or runtime is
// refused) and hands the repository out as a const view. A second reconciler on the same repository (only a test can build one) is still
// protected by the compare-and-set and by the order of the reads, but not against an A -> B -> A.
class OutcomeBriefReconciler final : public IOutcomeBriefTrigger {
public:
    // How often a lost race (a compare-and-set, or a verdict stored by another attempt meanwhile) is retried with freshly read facts.
    static constexpr int kMaxAttempts = 3;
    // How many failed Run ids a history result keeps.
    static constexpr std::size_t kMaxReportedFailures = 16;

    // With a `refusal` the reconciler is READ-NOTHING and WRITE-NOTHING: every entry (reconcileRun, reconcileHistory, the fan-out reads, the
    // nudges) fails closed with that error before it reads a source or touches the repository. That is what the runtime gives a GUEST of a
    // database another runtime owns (see BackendRuntimeConfig::exclusiveDatabaseOwnership): the guest has a reconciler object, because
    // the services are composed the same way, but nothing it is asked can produce a brief.
    OutcomeBriefReconciler(const IOutcomeBriefSources& sources, IOutcomeBriefRepository& briefs,
        std::optional<common::Error> refusal = std::nullopt);

    // Recomputes the brief of one Run now and stores a revision if it changed. Fails when the facts cannot be read or the brief cannot
    // be stored (nothing is written then); a Run that is not (yet) an outcome Run is a Skipped outcome, not an error.
    [[nodiscard]] common::Result<OutcomeBriefReconcileOutcome> reconcileRun(const RunId& runId);

    // The Runs whose brief an event on this Run / this proposal can change (the Run, the anchor of its handoff, ...). Read-only.
    [[nodiscard]] common::Result<std::vector<RunId>> affectedByRun(const RunId& runId) const;
    [[nodiscard]] common::Result<std::vector<RunId>> affectedByProposal(const ChangeSetId& changeSetId) const;

    // Bounded history pass (startup recovery, backfill): project by project and page by page (stable keyset pages, newest first), so
    // neither the memory nor a single query grows with the history. The newest `newestPerProject` Runs of each project always get a
    // brief, an older one only while its outcome is not settled (or when it already has a brief that must follow). A failing Run, or a
    // project whose page cannot be read, is counted and the pass goes on.
    [[nodiscard]] common::Result<OutcomeBriefHistoryResult> reconcileHistory(const OutcomeBriefHistoryOptions& options = {});

    // IOutcomeBriefTrigger: best effort, never throws, errors are dropped.
    void runMayHaveChanged(const std::string& runId) noexcept override;
    void proposalResolved(const std::string& changeSetId) noexcept override;
    [[nodiscard]] OutcomeBriefNudgeCounts nudgeCounts() const noexcept;

    // A diagnostic: how many Runs are being reconciled right now. The bookkeeping that validates the attempts against each other exists
    // per Run and only while an attempt over THAT Run is in flight - the last one out erases it - so it follows the Runs in flight and
    // never the history: one Run that hangs on a read does not keep the records of the thousands that finish meanwhile.
    [[nodiscard]] std::size_t trackedRunCount() const;

private:
    enum class Mode { Always, OnlyUnsettledOrAlreadyStored };

    // What the attempts over one Run need to know about each other: how many are in flight, how many verdicts were stored while any was
    // (a write that found nothing to change counts), and the source state the head had after the last of them.
    struct RunPasses {
        std::size_t inFlight {0};
        std::uint64_t verdicts {0};
        std::string lastSourceStateFingerprint;
    };
    class Attempt;   // one pass of reconcile() over one Run; see the .cpp

    [[nodiscard]] common::Result<OutcomeBriefReconcileOutcome> reconcile(const RunId& runId, Mode mode);
    [[nodiscard]] common::Result<OutcomeBriefFacts> gatherFacts(const Run& run) const;

    const IOutcomeBriefSources& sources_;
    IOutcomeBriefRepository& briefs_;
    const std::optional<common::Error> refusal_;
    std::atomic<std::uint64_t> runNudges_ {0};
    std::atomic<std::uint64_t> proposalNudges_ {0};

    // Guards `passes_` AND is held across the final check and the write of one attempt, so that no verdict can be stored between the
    // check and the write. Nothing is read from a source or a repository for facts while it is held.
    mutable std::mutex commitMutex_;
    std::unordered_map<RunId, RunPasses> passes_;
};

// True for the workflows that bear a ledger entry and so a brief: an ordinary Manifest Run and a paid continuation child.
// A manual-handoff attempt Run is never an outcome Run. A thin wrapper over the one table, outcomeWorkflowKind().
[[nodiscard]] bool isOutcomeBriefWorkflow(const std::string& workflowId) noexcept;

} // namespace ai::application
