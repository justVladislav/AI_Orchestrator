#include "ai/application/OutcomeBriefReconciler.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"

#include <algorithm>
#include <set>

namespace ai::application {

namespace {

common::Error reconcilerError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

void appendUniqueFragments(std::vector<std::string>& target, const std::vector<std::string>& fragments)
{
    for (const auto& fragment : fragments) {
        if (std::find(target.begin(), target.end(), fragment) == target.end()) target.push_back(fragment);
    }
}

void appendUniqueRun(std::vector<RunId>& target, const RunId& runId)
{
    if (!runId.empty() && std::find(target.begin(), target.end(), runId) == target.end()) target.push_back(runId);
}

bool isSettled(OutcomeBriefState state) noexcept
{
    return !isPendingOutcomeBriefState(state);
}

} // namespace

bool isOutcomeBriefWorkflow(const std::string& workflowId) noexcept
{
    return outcomeWorkflowKind(workflowId).has_value();
}

OutcomeBriefReconciler::OutcomeBriefReconciler(
    const IOutcomeBriefSources& sources, IOutcomeBriefRepository& briefs, std::optional<common::Error> refusal)
    : sources_(sources)
    , briefs_(briefs)
    , refusal_(std::move(refusal))
{
}

// One pass of reconcile() over one Run. It exists from BEFORE the first thing the pass reads until the pass is over, and remembers how many
// verdicts of the Run had been stored when it began. A verdict stored later may have been derived from facts that are NEWER than some this
// pass reads, so the pass must not write over it without looking again. The records belong to the Run and live only while a pass over THAT
// Run is in flight: the last pass over it erases them on its way out, so they follow the Runs in flight and never the history.
class OutcomeBriefReconciler::Attempt {
public:
    Attempt(OutcomeBriefReconciler& owner, const RunId& runId)
        : owner_(owner)
        , runId_(runId)
    {
        std::lock_guard lock(owner_.commitMutex_);
        auto& passes = owner_.passes_[runId_];
        ++passes.inFlight;
        verdictsWhenBegun_ = passes.verdicts;
    }
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;
    ~Attempt()
    {
        std::lock_guard lock(owner_.commitMutex_);
        const auto found = owner_.passes_.find(runId_);
        if (found != owner_.passes_.end() && --found->second.inFlight == 0) owner_.passes_.erase(found);
    }

    // To be called with commitMutex_ held, immediately before the write. False: another pass has stored a verdict for the Run since this
    // one began AND that verdict does not say what this one is about to say - what this pass holds may be older than what that one saw.
    [[nodiscard]] bool mayStore(const std::string& sourceStateFingerprint) const
    {
        const auto found = owner_.passes_.find(runId_);
        if (found == owner_.passes_.end()) return false;   // cannot happen while this pass is in flight; the safe answer is to look again
        if (found->second.verdicts == verdictsWhenBegun_) return true;
        return found->second.lastSourceStateFingerprint == sourceStateFingerprint;   // the same state: the write is a no-op, whoever saw it first
    }

    // To be called with commitMutex_ held, right after a write has succeeded (a no-op write is a verdict too: it is what makes the
    // head say "I looked, and it is still this").
    void recordStored(const OutcomeBriefRevision& head) const
    {
        auto& passes = owner_.passes_[runId_];
        ++passes.verdicts;
        passes.lastSourceStateFingerprint = head.brief.sourceStateFingerprint;
    }

private:
    OutcomeBriefReconciler& owner_;
    RunId runId_;
    std::uint64_t verdictsWhenBegun_ {0};
};

common::Result<OutcomeBriefFacts> OutcomeBriefReconciler::gatherFacts(const Run& run) const
{
    using Result = common::Result<OutcomeBriefFacts>;
    OutcomeBriefFacts facts;
    facts.runId = run.id;
    facts.projectId = run.projectId;
    facts.workflowId = run.workflowId;
    facts.terminalStatus = run.status;

    // Absolute paths of the project and of the Run's own workspace must never appear in a brief.
    const auto project = sources_.findProject(run.projectId);
    if (!project.success()) return Result::failure(*project.error());
    if (!project.value().has_value()) {
        return Result::failure(reconcilerError("outcome_brief.project_not_found", "The project of the Run does not exist"));
    }
    facts.forbiddenPathFragments = projectContextPathFragments(project.value()->rootPath);
    if (run.workspacePath.has_value()) {
        appendUniqueFragments(facts.forbiddenPathFragments, projectContextPathFragments(*run.workspacePath));
    }

    // The immutable ledger entry says WHICH proposals belong to the Run; their CURRENT decisions come from the specialized
    // repositories. Every read is fail-closed: no partial facts ever reach the resolver.
    const auto entry = sources_.findLedgerEntry(run.id);
    if (!entry.success()) return Result::failure(*entry.error());
    facts.ledgerEntry = entry.value();
    if (entry.value().has_value()) {
        const auto transitions = sources_.listLedgerTransitions(run.id);
        if (!transitions.success()) return Result::failure(*transitions.error());

        std::set<ChangeSetId> seen;
        const std::size_t listed = std::min(entry.value()->proposalIds.size(), kManifestWorkLedgerMaxProposalIds);
        for (std::size_t index = 0; index < listed; ++index) {
            const ChangeSetId& id = entry.value()->proposalIds[index];
            if (!seen.insert(id).second) continue;   // a repeated id is the resolver's finding, not a second fact
            const auto changeSet = sources_.findChangeSet(id);
            if (!changeSet.success()) return Result::failure(*changeSet.error());
            if (!changeSet.value().has_value()) continue;   // a member without facts: the resolver says so

            OutcomeBriefProposalFacts proposal;
            proposal.changeSetId = id;
            proposal.state = changeSet.value()->state;
            const auto rejection = sources_.findRejection(id);
            if (!rejection.success()) return Result::failure(*rejection.error());
            proposal.humanRejected = rejection.value().has_value();
            const auto approval = sources_.findApproval(id);
            if (!approval.success()) return Result::failure(*approval.error());
            proposal.approved = approval.value().has_value();

            // Transitions only ENRICH the typed decision (time, reason, post-apply fingerprint); they never override it.
            for (const auto& transition : transitions.value()) {
                if (transition.changeSetId != id) continue;
                OutcomeBriefDecisionEvidence evidence;
                evidence.occurredAt = transition.occurredAt;
                switch (transition.kind) {
                case ManifestWorkLedgerTransitionKind::Approved:
                    proposal.approvedEvidence = std::move(evidence);
                    break;
                case ManifestWorkLedgerTransitionKind::Rejected:
                    evidence.reason = transition.reason;
                    proposal.rejectedEvidence = std::move(evidence);
                    break;
                case ManifestWorkLedgerTransitionKind::Applied:
                    evidence.postActionWorkspaceFingerprint = transition.postActionWorkspaceFingerprint;
                    proposal.appliedEvidence = std::move(evidence);
                    break;
                }
            }
            facts.proposals.push_back(std::move(proposal));
        }
    }

    // Typed relations only (never Run::parentRunId). Whether they fit the kind of Run is the resolver's verdict.
    const auto child = sources_.continuationChildOf(run.id);
    if (!child.success()) return Result::failure(*child.error());
    facts.continuationChildRunId = child.value();
    const auto source = sources_.continuationSourceOf(run.id);
    if (!source.success()) return Result::failure(*source.error());
    facts.continuationSourceRunId = source.value();
    const auto handoffId = sources_.handoffIdOfAnchor(run.id);
    if (!handoffId.success()) return Result::failure(*handoffId.error());
    if (handoffId.value().has_value()) {
        const auto resolution = sources_.handoffResolutionOf(run);
        if (!resolution.success()) return Result::failure(*resolution.error());
        facts.manualHandoff = OutcomeBriefHandoffFacts {*handoffId.value(), resolution.value()};
    }
    return Result::ok(std::move(facts));
}

common::Result<OutcomeBriefReconcileOutcome> OutcomeBriefReconciler::reconcile(const RunId& runId, Mode mode)
{
    using Result = common::Result<OutcomeBriefReconcileOutcome>;
    const auto skipped = [](std::string because) {
        OutcomeBriefReconcileOutcome outcome;
        outcome.kind = OutcomeBriefReconcileOutcome::Kind::Skipped;
        outcome.skippedBecause = std::move(because);
        return Result::ok(std::move(outcome));
    };

    // The one door every write goes through (the pull, the nudges and the history pass all end here): a reconciler that must not write
    // says so before it reads a single source.
    if (refusal_.has_value()) return Result::failure(*refusal_);

    for (int attemptNumber = 0; attemptNumber < kMaxAttempts; ++attemptNumber) {
        // The pass is registered BEFORE anything is read (see Attempt): a verdict that another pass stores from now on is one this pass
        // has to look at before it writes.
        Attempt attempt(*this, runId);

        // Everything is read afresh on every attempt: a lost race means the world moved. The Run is read first (it decides whether
        // there is anything to do; a change of its STATUS after this read is caught by the write authority of the append).
        const auto run = sources_.findRun(runId);
        if (!run.success()) return Result::failure(*run.error());
        if (!run.value().has_value()) return skipped("unknown Run");
        if (!isTerminal(run.value()->status)) return skipped("the Run is not terminal");
        if (!isOutcomeBriefWorkflow(run.value()->workflowId)) return skipped("not an outcome Run");

        // The head the compare-and-set is made against is read BEFORE the facts the brief is derived from - also when it is stale because
        // the Run moved on. Read after them, a reconciler holding OLD facts could pick up the id of a head that a newer one wrote in
        // between and then successfully overwrite it with the old state; read before, the newer head makes this append fail with
        // head_changed and the retry reads everything again.
        const auto version = briefs_.findHeadVersionForAppend(runId);
        if (!version.success()) return Result::failure(*version.error());

        const auto facts = gatherFacts(*run.value());
        if (!facts.success()) return Result::failure(*facts.error());
        const auto derived = deriveOutcomeBrief(facts.value());
        if (!derived.success()) return Result::failure(*derived.error());

        if (mode == Mode::OnlyUnsettledOrAlreadyStored && !version.value().has_value() && isSettled(derived.value().state)) {
            return skipped("the outcome is settled and the Run is old");
        }
        const auto expected = version.value().has_value() ? OutcomeBriefExpectedHead::revision(version.value()->revisionId)
                                                          : OutcomeBriefExpectedHead::none();

        // The final check and the write are ONE critical section: nothing can be stored between them. The compare-and-set below knows
        // only the head id, which a verdict that found nothing to change does not move; this check is what sees such a verdict (an
        // A -> B -> A seen by the second pass while this one was still holding B). A read of the sources never happens in here.
        std::unique_lock commitLock(commitMutex_);
        if (!attempt.mayStore(derived.value().sourceStateFingerprint)) continue;   // read everything again
        const auto appended = briefs_.append(derived.value(), expected);
        if (appended.success()) attempt.recordStored(appended.value().head);
        commitLock.unlock();
        if (appended.success()) {
            OutcomeBriefReconcileOutcome outcome;
            outcome.kind = appended.value().appended ? OutcomeBriefReconcileOutcome::Kind::Appended
                                                     : OutcomeBriefReconcileOutcome::Kind::Unchanged;
            outcome.state = appended.value().head.brief.state;
            outcome.ordinal = appended.value().head.ordinal;
            return Result::ok(std::move(outcome));
        }
        const std::string& code = appended.error()->code;
        if (code == "outcome_brief.head_changed") continue;   // somebody else wrote first: read again and compare
        if (code == "outcome_brief.run_status_mismatch") return skipped("the Run left its terminal status");
        return Result::failure(*appended.error());
    }
    return Result::failure(reconcilerError("outcome_brief.reconcile_contended",
        "The head of the Outcome Brief kept changing while it was being reconciled"));
}

common::Result<OutcomeBriefReconcileOutcome> OutcomeBriefReconciler::reconcileRun(const RunId& runId)
{
    return reconcile(runId, Mode::Always);
}

std::size_t OutcomeBriefReconciler::trackedRunCount() const
{
    std::lock_guard lock(commitMutex_);
    return passes_.size();
}

common::Result<std::vector<RunId>> OutcomeBriefReconciler::affectedByRun(const RunId& runId) const
{
    using Result = common::Result<std::vector<RunId>>;
    if (refusal_.has_value()) return Result::failure(*refusal_);
    std::vector<RunId> affected;
    appendUniqueRun(affected, runId);
    // A handoff attempt Run is never a brief itself; its anchor's resolution is what it can change.
    const auto anchor = sources_.handoffAnchorOfAttemptRun(runId);
    if (!anchor.success()) return Result::failure(*anchor.error());
    if (anchor.value().has_value()) appendUniqueRun(affected, *anchor.value());
    return Result::ok(std::move(affected));
}

common::Result<std::vector<RunId>> OutcomeBriefReconciler::affectedByProposal(const ChangeSetId& changeSetId) const
{
    using Result = common::Result<std::vector<RunId>>;
    if (refusal_.has_value()) return Result::failure(*refusal_);
    std::vector<RunId> affected;
    const auto changeSet = sources_.findChangeSet(changeSetId);
    if (!changeSet.success()) return Result::failure(*changeSet.error());
    if (!changeSet.value().has_value()) return Result::ok(std::move(affected));
    appendUniqueRun(affected, changeSet.value()->runId);

    // A frozen member of a handoff, and a proposal of a handoff attempt Run, decide the resolution of the handoff's ANCHOR.
    const auto memberAnchor = sources_.handoffAnchorOfMemberProposal(changeSetId);
    if (!memberAnchor.success()) return Result::failure(*memberAnchor.error());
    if (memberAnchor.value().has_value()) appendUniqueRun(affected, *memberAnchor.value());
    const auto attemptAnchor = sources_.handoffAnchorOfAttemptRun(changeSet.value()->runId);
    if (!attemptAnchor.success()) return Result::failure(*attemptAnchor.error());
    if (attemptAnchor.value().has_value()) appendUniqueRun(affected, *attemptAnchor.value());
    return Result::ok(std::move(affected));
}

common::Result<OutcomeBriefHistoryResult> OutcomeBriefReconciler::reconcileHistory(const OutcomeBriefHistoryOptions& options)
{
    using Result = common::Result<OutcomeBriefHistoryResult>;
    if (refusal_.has_value()) return Result::failure(*refusal_);
    if (options.pageSize == 0 || options.pageSize > kMaxRunPageSize) {
        return Result::failure(reconcilerError("outcome_brief.invalid_request", "The page size must be between 1 and 1000"));
    }
    const auto projectIds = sources_.listProjectIds();
    if (!projectIds.success()) return Result::failure(*projectIds.error());

    OutcomeBriefHistoryResult result;
    for (const ProjectId& projectId : projectIds.value()) {
        // The Runs of one project arrive newest first, page after page, each page strictly after the last Run of the one before (the
        // keyset (createdAt, id)); the k-th Run seen is therefore the k-th newest of the project.
        std::optional<RunPageCursor> cursor;
        std::size_t index = 0;
        for (;;) {
            if (options.shouldStop && options.shouldStop()) {
                result.stopped = true;
                return Result::ok(std::move(result));
            }
            const auto page = sources_.listOutcomeRunsPage(projectId, cursor, options.pageSize);
            if (!page.success()) {
                ++result.failed;
                result.failedProjectIds.push_back(projectId);
                break;
            }
            ++result.pagesRead;
            for (const Run& run : page.value()) {
                if (options.shouldStop && options.shouldStop()) {
                    result.stopped = true;
                    return Result::ok(std::move(result));
                }
                ++result.examined;
                const Mode mode = index < options.newestPerProject ? Mode::Always : Mode::OnlyUnsettledOrAlreadyStored;
                ++index;
                const auto outcome = reconcile(run.id, mode);
                if (!outcome.success()) {
                    ++result.failed;
                    if (result.failedRunIds.size() < kMaxReportedFailures) result.failedRunIds.push_back(run.id);
                    continue;
                }
                switch (outcome.value().kind) {
                case OutcomeBriefReconcileOutcome::Kind::Appended: ++result.appended; break;
                case OutcomeBriefReconcileOutcome::Kind::Unchanged: ++result.unchanged; break;
                case OutcomeBriefReconcileOutcome::Kind::Skipped: ++result.skipped; break;
                }
            }
            if (page.value().size() < options.pageSize) break;   // the last page of this project
            cursor = RunPageCursor {page.value().back().createdAt, page.value().back().id};
        }
    }
    return Result::ok(std::move(result));
}

OutcomeBriefNudgeCounts OutcomeBriefReconciler::nudgeCounts() const noexcept
{
    return {runNudges_.load(), proposalNudges_.load()};
}

void OutcomeBriefReconciler::runMayHaveChanged(const std::string& runId) noexcept
{
    ++runNudges_;
    try {
        const auto affected = affectedByRun(runId);
        if (!affected.success()) return;
        for (const auto& id : affected.value()) static_cast<void>(reconcile(id, Mode::Always));
    } catch (...) {
        // A nudge never fails its caller.
    }
}

void OutcomeBriefReconciler::proposalResolved(const std::string& changeSetId) noexcept
{
    ++proposalNudges_;
    try {
        const auto affected = affectedByProposal(changeSetId);
        if (!affected.success()) return;
        for (const auto& id : affected.value()) static_cast<void>(reconcile(id, Mode::Always));
    } catch (...) {
        // A nudge never fails its caller.
    }
}

} // namespace ai::application
