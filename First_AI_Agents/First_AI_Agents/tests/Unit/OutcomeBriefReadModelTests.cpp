#include "OutcomeBriefRepositoryContract.hpp"

#include "ai/application/inmemory/InMemoryOutcomeBriefRepository.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/verification/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/application/planning/OutcomeBriefReadModelService.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace {

namespace oc = outcomebrief_contract;
using namespace ai::application;
namespace common = ai::common;
using Availability = OutcomeBriefView::Availability;
using Resolution = ManifestVerificationManualHandoffResolution;

const std::string kProject = "project-1";

// The Run repository and the brief repository over the same Runs - the brief repository answers its authority questions
// (a head follows the status of its Run) from the very repository the read model reads.
struct Rig {
    InMemoryRunRepository runs;
    InMemoryOutcomeBriefRepository briefs {[this](const RunId& runId) -> std::optional<OutcomeBriefRunOwner> {
        const auto found = runs.findRun(runId);
        if (!found.success() || !found.value().has_value()) return std::nullopt;
        return OutcomeBriefRunOwner {found.value()->projectId, found.value()->status};
    }};
    OutcomeBriefReadModelService service {runs, briefs};
    int ticks {0};

    void addRun(const std::string& id, RunStatus status, const std::string& workflow = std::string(kExecutionManifestWorkflowId))
    {
        Run run;
        run.id = id;
        run.projectId = kProject;
        run.title = id;
        run.task = id;
        run.workflowId = workflow;
        run.status = status;
        run.createdAt = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000 + (++ticks)));
        oc::check(runs.insertRun(run).success(), "setup: the Run " + id);
    }

    void setStatus(const std::string& id, RunStatus status)
    {
        auto run = runs.findRun(id);
        oc::check(run.success() && run.value().has_value(), "setup: the Run to move on");
        run.value()->status = status;
        oc::check(runs.updateRun(*run.value()).success(), "setup: the Run moves on");
    }

    // The brief the reconciler would have stored for these facts (the read model is not about deriving it).
    OutcomeBriefRevision store(const OutcomeBriefFacts& facts)
    {
        const auto derived = deriveOutcomeBrief(facts);
        oc::check(derived.success(), "setup: the facts derive a brief");
        const auto appended = briefs.append(derived.value(), OutcomeBriefExpectedHead::any());
        oc::check(appended.success(), "setup: the brief is stored: " + (appended.error().has_value() ? appended.error()->code : std::string()));
        return appended.value().head;
    }
};

OutcomeBriefFacts failedFacts(const std::string& runId, RunStatus status = RunStatus::Failed)
{
    return oc::terminalFacts(runId, kProject, status, {}, ManifestWorkLedgerDisposition::NotProduced);
}

// ---- nothing to show ---------------------------------------------------------------------------------------------------------

void testNothingIsShownForARunThatIsNotAnOutcomeRun()
{
    Rig rig;
    rig.addRun("mock", RunStatus::Completed, std::string(kMockWorkflowId));
    rig.addRun("running", RunStatus::Running);
    rig.addRun("attempt", RunStatus::Completed, std::string(kManifestVerificationManualHandoffWorkflowId));
    rig.addRun("prepared-child", RunStatus::Prepared, std::string(kManifestVerificationContinuationWorkflowId));
    for (const std::string id : {"mock", "running", "attempt", "prepared-child", "no-such-run", ""}) {
        const auto view = rig.service.viewForRun(id);
        oc::check(view.availability == Availability::NotApplicable, "not an outcome Run, so no block: '" + id + "'");
        oc::check(view.errorCode.empty() && view.state.empty() && view.renderedText.empty(), "...and nothing else is filled in");
    }
}

void testATerminalOutcomeRunWithoutABriefIsNotRecordedYet()
{
    Rig rig;
    rig.addRun("completed", RunStatus::Completed);
    rig.addRun("failed", RunStatus::Failed);
    rig.addRun("child", RunStatus::Completed, std::string(kManifestVerificationContinuationWorkflowId));
    for (const std::string id : {"completed", "failed", "child"}) {
        const auto view = rig.service.viewForRun(id);
        oc::check(view.availability == Availability::NotRecorded, id + ": a terminal outcome Run whose brief is not written yet says so");
        oc::check(view.runId == id && view.state.empty() && view.revision == 0, id + ": and shows no state or revision of its own");
    }
}

// ---- a brief ------------------------------------------------------------------------------------------------------------------

void testThePresentBriefIsProjectedFaithfully()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    OutcomeBriefFacts facts = oc::terminalFacts("run-1", kProject, RunStatus::Completed,
        {oc::ready("cs-undecided"), oc::applied("cs-applied"), oc::proposal("cs-rejected", ChangeSetState::ReadyForReview, true)});
    const OutcomeBriefRevision stored = rig.store(facts);

    const auto view = rig.service.viewForRun("run-1");
    oc::check(view.availability == Availability::Present && view.runId == "run-1", "the head is shown");
    oc::check(view.state == "awaiting_review" && !view.resolved && view.pending, "awaiting review: pending, not resolved");
    oc::check(view.terminalStatus == "completed", "the terminal status the brief describes");
    oc::check(view.reasons.empty(), "no reason: it is not attention_required");
    oc::check(view.counts == stored.brief.counts && view.counts.members == 3 && view.counts.reviewable == 3 && view.counts.applied == 1
            && view.counts.humanRejected == 1 && view.counts.undecided == 1,
        "the typed counts are the brief's");
    oc::check(view.members.size() == 3 && view.members[0].changeSetId == "cs-undecided" && view.members[1].changeSetId == "cs-applied"
            && view.members[2].changeSetId == "cs-rejected",
        "every member, in ledger order");
    oc::check(view.members[0].decision == toString(OutcomeProposalDecision::Undecided)
            && view.members[1].decision == toString(OutcomeProposalDecision::Applied)
            && view.members[2].decision == toString(OutcomeProposalDecision::HumanRejected),
        "...each with its typed decision");
    oc::check(!view.recoveryEligible && !view.requiresInspection, "a Completed Run needs no recovery");
    oc::check(!view.continuationSourceRunId.has_value() && !view.continuationChildRunId.has_value() && !view.manualHandoffId.has_value(),
        "and has no relation");
    oc::check(view.revision == 1 && view.recordedAt == stored.createdAt, "revision 1, recorded when the revision was stored");
    oc::check(view.sourceStateFingerprint == stored.brief.sourceStateFingerprint && view.contentFingerprint == stored.brief.contentFingerprint
            && view.sourceStateFingerprint.size() == 64 && view.contentFingerprint.size() == 64,
        "both fingerprints are the stored ones");
    oc::check(view.renderedText == stored.brief.renderedText && !view.renderedText.empty(), "and the text is the stored bytes, unchanged");
}

void testTheCurrentRevisionIsTheOneShown()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    rig.store(oc::terminalFacts("run-1", kProject, RunStatus::Completed, {oc::ready("cs-1")}));
    const auto first = rig.service.viewForRun("run-1");
    oc::check(first.revision == 1 && first.state == "awaiting_review", "revision 1: awaiting review");

    const OutcomeBriefRevision second = rig.store(oc::terminalFacts("run-1", kProject, RunStatus::Completed, {oc::applied("cs-1")}));
    const auto now = rig.service.viewForRun("run-1");
    oc::check(now.revision == 2 && now.state == "resolved_applied" && now.resolved && !now.pending,
        "revision 2 is shown once the proposal is applied: resolved, nothing pending");
    oc::check(now.contentFingerprint == second.brief.contentFingerprint && now.contentFingerprint != first.contentFingerprint,
        "and it is the new content");

    // A -> B -> A is three revisions and the head is the last.
    rig.store(oc::terminalFacts("run-1", kProject, RunStatus::Completed, {oc::ready("cs-1")}));
    const auto back = rig.service.viewForRun("run-1");
    oc::check(back.revision == 3 && back.state == "awaiting_review" && back.sourceStateFingerprint == first.sourceStateFingerprint,
        "a return to an earlier outcome is a third revision, shown as the current one");
}

void testAttentionRelationsAndRecoveryFlags()
{
    Rig rig;

    rig.addRun("no-ledger", RunStatus::Completed);
    OutcomeBriefFacts missing = oc::terminalFacts("no-ledger", kProject, RunStatus::Completed, {});
    missing.ledgerEntry.reset();
    rig.store(missing);
    const auto attention = rig.service.viewForRun("no-ledger");
    oc::check(attention.state == "attention_required" && attention.pending && !attention.resolved
            && attention.reasons == std::vector<std::string> {"source_ledger_missing"},
        "a missing ledger entry: attention, with its typed reason");

    rig.addRun("source", RunStatus::Failed);
    OutcomeBriefFacts superseded = failedFacts("source");
    superseded.continuationChildRunId = "child-1";
    rig.store(superseded);
    const auto taken = rig.service.viewForRun("source");
    oc::check(taken.state == "superseded_by_continuation" && !taken.pending && !taken.resolved, "neither pending nor resolved");
    oc::check(taken.continuationChildRunId == std::optional<std::string>("child-1") && !taken.continuationSourceRunId.has_value(),
        "it names its child");
    oc::check(taken.recoveryEligible && !taken.requiresInspection, "a Failed Run is recovery eligible");

    rig.addRun("child-1", RunStatus::Completed, std::string(kManifestVerificationContinuationWorkflowId));
    OutcomeBriefFacts child = oc::terminalFacts("child-1", kProject, RunStatus::Completed, {oc::applied("cs-c")});
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.continuationSourceRunId = "source";
    rig.store(child);
    const auto fromSource = rig.service.viewForRun("child-1");
    oc::check(fromSource.state == "resolved_applied" && fromSource.continuationSourceRunId == std::optional<std::string>("source"),
        "a continuation child names its source");

    rig.addRun("anchor", RunStatus::Failed);
    OutcomeBriefFacts handoff = failedFacts("anchor");
    handoff.manualHandoff = OutcomeBriefHandoffFacts {"handoff-1", Resolution::Unresolved};
    rig.store(handoff);
    const auto waiting = rig.service.viewForRun("anchor");
    oc::check(waiting.state == "awaiting_manual_resolution" && waiting.pending && waiting.manualHandoffId == std::optional<std::string>("handoff-1"),
        "a handoff waits for its resolution, and is named");

    rig.addRun("interrupted", RunStatus::Interrupted);
    rig.store(failedFacts("interrupted", RunStatus::Interrupted));
    const auto interrupted = rig.service.viewForRun("interrupted");
    oc::check(interrupted.state == "recovery_only" && interrupted.terminalStatus == "interrupted" && interrupted.recoveryEligible
            && interrupted.requiresInspection,
        "an Interrupted Run: recovery allowed, but it must be inspected");
}

// ---- a fault is never an empty block --------------------------------------------------------------------------------------------

void testAStaleHeadIsAFaultNotAnEmptyBlock()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    rig.store(oc::terminalFacts("run-1", kProject, RunStatus::Completed, {oc::ready("cs-1")}));
    oc::check(rig.service.viewForRun("run-1").availability == Availability::Present, "setup: the brief is readable");

    rig.setStatus("run-1", RunStatus::Failed);   // the Run left the status the brief describes
    const auto stale = rig.service.viewForRun("run-1");
    oc::check(stale.availability == Availability::Unreadable && stale.errorCode == "persistence.invalid_data" && !stale.errorMessage.empty(),
        "a head that no longer matches its Run is refused with the repository's own code and message");
    oc::check(stale.state.empty() && stale.renderedText.empty() && stale.revision == 0, "...and nothing of the stale brief is shown");
}

// A Run repository that cannot answer.
class FailingRuns final : public IRunRepository {
public:
    common::Result<Run> insertRun(Run) override { return fail<Run>(); }
    common::Result<Run> insertRunWithStages(Run, std::vector<RunStage>) override { return fail<Run>(); }
    common::Result<Run> updateRun(Run) override { return fail<Run>(); }
    common::Result<std::optional<Run>> findRun(const RunId&) const override { return fail<std::optional<Run>>(); }
    common::Result<std::vector<Run>> listRuns(const std::optional<ProjectId>&) const override { return fail<std::vector<Run>>(); }
    common::Result<std::vector<Run>> listUnfinishedRuns() const override { return fail<std::vector<Run>>(); }
    common::Result<void> replaceStages(const RunId&, std::vector<RunStage>) override { return fail<void>(); }
    common::Result<RunStage> updateStage(RunStage) override { return fail<RunStage>(); }
    common::Result<std::vector<RunStage>> listStages(const RunId&) const override { return fail<std::vector<RunStage>>(); }

private:
    template <typename T> static common::Result<T> fail() { return common::Result<T>::failure({"run.read_failed", "the Run could not be read"}); }
};

void testAFailingRunRepositoryIsAFaultToo()
{
    FailingRuns runs;
    InMemoryOutcomeBriefRepository briefs;
    const OutcomeBriefReadModelService service {runs, briefs};
    const auto view = service.viewForRun("run-1");
    oc::check(view.availability == Availability::Unreadable && view.errorCode == "run.read_failed" && view.errorMessage == "the Run could not be read",
        "a Run that cannot be read is a fault with the repository's code and message, not 'nothing to show'");
}

// ---- it only reads ------------------------------------------------------------------------------------------------------------

void testTheServiceCanOnlyRead()
{
    // Constructible from CONST views of both repositories: it holds no way to write, reconcile or start anything.
    static_assert(std::is_constructible_v<OutcomeBriefReadModelService, const IRunRepository&, const IOutcomeBriefRepository&>);
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    const std::size_t before = oc::revisionCount(rig.briefs, "run-1");
    for (int repeat = 0; repeat < 3; ++repeat) static_cast<void>(rig.service.viewForRun("run-1"));
    oc::check(before == 0 && oc::revisionCount(rig.briefs, "run-1") == 0, "reading a Run without a brief never creates one");
}

} // namespace

int main()
{
    try {
        testNothingIsShownForARunThatIsNotAnOutcomeRun();
        testATerminalOutcomeRunWithoutABriefIsNotRecordedYet();
        testThePresentBriefIsProjectedFaithfully();
        testTheCurrentRevisionIsTheOneShown();
        testAttentionRelationsAndRecoveryFlags();
        testAStaleHeadIsAFaultNotAnEmptyBlock();
        testAFailingRunRepositoryIsAFaultToo();
        testTheServiceCanOnlyRead();
        std::cout << "OutcomeBriefReadModel_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
