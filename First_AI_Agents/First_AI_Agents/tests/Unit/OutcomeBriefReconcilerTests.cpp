#include "OutcomeBriefRepositoryContract.hpp"
#include "RunPagingContract.hpp"

#include "ai/application/proposals/IChangeApprovalRepository.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/proposals/IProposalReviewRepository.hpp"
#include "ai/application/InMemoryManifestVerificationContinuationRepository.hpp"
#include "ai/application/InMemoryManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryOutcomeBriefRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/application/planning/OutcomeBriefReconciler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace {

namespace oc = outcomebrief_contract;
using namespace ai::application;
using Resolution = ManifestVerificationManualHandoffResolution;
namespace common = ai::common;

const std::string kProject = "project-1";
const std::string kOtherProject = "project-2";
const std::string kRootPath = "C:\\Users\\someone\\secret-workspace\\project-one";

// ---- a fake world behind the read-only sources -------------------------------------------------------------------------------

struct World {
    mutable std::mutex mutex;
    mutable std::vector<std::string> calls;
    std::map<std::string, Run> runs;
    std::map<std::string, Project> projects;
    std::map<std::string, ManifestWorkLedgerEntry> entries;
    std::map<std::string, std::vector<ManifestWorkLedgerReviewTransition>> transitions;
    std::map<std::string, ProposedChangeSet> changeSets;
    std::set<std::string> approvals;
    std::map<std::string, ProposalReviewRejection> rejections;
    std::map<std::string, std::string> childOfSource;
    std::map<std::string, std::string> sourceOfChild;
    std::map<std::string, std::string> handoffOfAnchor;
    std::map<std::string, std::string> anchorOfMember;
    std::map<std::string, std::string> anchorOfAttemptRun;
    std::map<std::string, Resolution> resolutionOfAnchor;
    std::set<std::string> failing;   // the names of the reads that fail
    std::map<std::string, std::function<void()>> afterRead;   // by read name: runs ONCE, right after the next such read has answered
    struct PageRequest {
        std::string project;
        std::string afterId;
        std::size_t limit {0};
    };
    mutable std::vector<PageRequest> pageRequests;
    bool throwing {false};           // every read throws

    void record(const char* call) const
    {
        std::lock_guard lock(mutex);
        calls.push_back(call);
    }
};

class FakeSources final : public IOutcomeBriefSources {
public:
    explicit FakeSources(World& world) : world_(world) {}

    template <typename T>
    common::Result<T> guarded(const char* call, const std::function<T()>& read) const
    {
        world_.record(call);
        if (world_.throwing) throw std::runtime_error("the source threw");
        if (world_.failing.count(call) != 0) return common::Result<T>::failure({"fake.read_failed", std::string(call) + " failed"});
        auto answer = common::Result<T>::ok(read());
        // The world may move on right after a read has answered - what a late reconciler would then act on is what it ALREADY read.
        const auto hook = world_.afterRead.find(call);
        if (hook != world_.afterRead.end()) {
            const auto moveTheWorld = std::move(hook->second);
            world_.afterRead.erase(hook);
            moveTheWorld();
        }
        return answer;
    }

    common::Result<std::optional<Run>> findRun(const RunId& runId) const override
    {
        return guarded<std::optional<Run>>("findRun", [&]() -> std::optional<Run> {
            const auto found = world_.runs.find(runId);
            if (found == world_.runs.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::vector<ProjectId>> listProjectIds() const override
    {
        return guarded<std::vector<ProjectId>>("listProjectIds", [&] {
            std::vector<ProjectId> out;
            for (const auto& [id, project] : world_.projects) out.push_back(id);
            return out;
        });
    }
    common::Result<std::vector<Run>> listOutcomeRunsPage(
        const ProjectId& projectId, const std::optional<RunPageCursor>& after, std::size_t limit) const override
    {
        return guarded<std::vector<Run>>("listOutcomeRunsPage", [&] {
            {
                std::lock_guard lock(world_.mutex);
                world_.pageRequests.push_back({projectId, after.has_value() ? after->id : std::string(), limit});
            }
            std::vector<Run> mine;
            for (const auto& [id, run] : world_.runs) {
                if (run.projectId == projectId) mine.push_back(run);
            }
            return selectTerminalRunsPage(std::move(mine),
                {std::string(kExecutionManifestWorkflowId), std::string(kManifestVerificationContinuationWorkflowId)}, after, limit);
        });
    }
    common::Result<std::optional<Project>> findProject(const ProjectId& projectId) const override
    {
        return guarded<std::optional<Project>>("findProject", [&]() -> std::optional<Project> {
            const auto found = world_.projects.find(projectId);
            if (found == world_.projects.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<ManifestWorkLedgerEntry>> findLedgerEntry(const RunId& runId) const override
    {
        return guarded<std::optional<ManifestWorkLedgerEntry>>("findLedgerEntry", [&]() -> std::optional<ManifestWorkLedgerEntry> {
            const auto found = world_.entries.find(runId);
            if (found == world_.entries.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::vector<ManifestWorkLedgerReviewTransition>> listLedgerTransitions(const RunId& runId) const override
    {
        return guarded<std::vector<ManifestWorkLedgerReviewTransition>>("listLedgerTransitions", [&] {
            const auto found = world_.transitions.find(runId);
            return found == world_.transitions.end() ? std::vector<ManifestWorkLedgerReviewTransition> {} : found->second;
        });
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId& changeSetId) const override
    {
        return guarded<std::optional<ProposedChangeSet>>("findChangeSet", [&]() -> std::optional<ProposedChangeSet> {
            const auto found = world_.changeSets.find(changeSetId);
            if (found == world_.changeSets.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<ApprovalRecord>> findApproval(const ChangeSetId& changeSetId) const override
    {
        return guarded<std::optional<ApprovalRecord>>("findApproval", [&]() -> std::optional<ApprovalRecord> {
            if (world_.approvals.count(changeSetId) == 0) return std::nullopt;
            ApprovalRecord record;
            record.changeSetId = changeSetId;
            return record;
        });
    }
    common::Result<std::optional<ProposalReviewRejection>> findRejection(const ChangeSetId& changeSetId) const override
    {
        return guarded<std::optional<ProposalReviewRejection>>("findRejection", [&]() -> std::optional<ProposalReviewRejection> {
            const auto found = world_.rejections.find(changeSetId);
            if (found == world_.rejections.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<RunId>> continuationChildOf(const RunId& sourceRunId) const override
    {
        return guarded<std::optional<RunId>>("continuationChildOf", [&]() -> std::optional<RunId> {
            const auto found = world_.childOfSource.find(sourceRunId);
            if (found == world_.childOfSource.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<RunId>> continuationSourceOf(const RunId& childRunId) const override
    {
        return guarded<std::optional<RunId>>("continuationSourceOf", [&]() -> std::optional<RunId> {
            const auto found = world_.sourceOfChild.find(childRunId);
            if (found == world_.sourceOfChild.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<std::string>> handoffIdOfAnchor(const RunId& anchorRunId) const override
    {
        return guarded<std::optional<std::string>>("handoffIdOfAnchor", [&]() -> std::optional<std::string> {
            const auto found = world_.handoffOfAnchor.find(anchorRunId);
            if (found == world_.handoffOfAnchor.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<RunId>> handoffAnchorOfMemberProposal(const ChangeSetId& changeSetId) const override
    {
        return guarded<std::optional<RunId>>("handoffAnchorOfMemberProposal", [&]() -> std::optional<RunId> {
            const auto found = world_.anchorOfMember.find(changeSetId);
            if (found == world_.anchorOfMember.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<std::optional<RunId>> handoffAnchorOfAttemptRun(const RunId& attemptRunId) const override
    {
        return guarded<std::optional<RunId>>("handoffAnchorOfAttemptRun", [&]() -> std::optional<RunId> {
            const auto found = world_.anchorOfAttemptRun.find(attemptRunId);
            if (found == world_.anchorOfAttemptRun.end()) return std::nullopt;
            return found->second;
        });
    }
    common::Result<Resolution> handoffResolutionOf(const Run& anchor) const override
    {
        return guarded<Resolution>("handoffResolutionOf", [&] {
            const auto found = world_.resolutionOfAnchor.find(anchor.id);
            return found == world_.resolutionOfAnchor.end() ? Resolution::NotApplicable : found->second;
        });
    }

private:
    World& world_;
};

// The brief repository and the Run lookup it answers its authority questions from: the same world.
struct Rig {
    World world;
    FakeSources sources {world};
    InMemoryOutcomeBriefRepository briefs {[this](const RunId& runId) -> std::optional<OutcomeBriefRunOwner> {
        std::lock_guard lock(world.mutex);
        const auto found = world.runs.find(runId);
        if (found == world.runs.end()) return std::nullopt;
        return OutcomeBriefRunOwner {found->second.projectId, found->second.status};
    }};
    OutcomeBriefReconciler reconciler {sources, briefs};

    Rig()
    {
        for (const std::string& id : {kProject, kOtherProject}) {
            Project project;
            project.id = id;
            project.name = id;
            project.rootPath = id == kProject ? std::filesystem::path(kRootPath) : std::filesystem::path("C:\\other\\root");
            world.projects[id] = project;
        }
    }

    int createdTicks {0};

    // A terminal outcome Run with its ledger entry; the proposals are (id, state) pairs that really exist.
    void addRun(const std::string& id, RunStatus status, const std::vector<std::pair<std::string, ChangeSetState>>& proposals = {},
        const std::string& project = kProject, const std::string& workflow = kExecutionManifestWorkflowId,
        bool withLedger = true)
    {
        Run run;
        run.id = id;
        run.projectId = project;
        run.workflowId = workflow;
        run.status = status;
        run.createdAt = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000 + (++createdTicks)));
        world.runs[id] = run;
        std::vector<OutcomeBriefProposalFacts> facts;
        for (const auto& [csId, state] : proposals) {
            facts.push_back(oc::proposal(csId, state));
            ProposedChangeSet changeSet;
            changeSet.changeSetId = csId;
            changeSet.runId = id;
            changeSet.projectId = project;
            changeSet.state = state;
            world.changeSets[csId] = changeSet;
        }
        if (withLedger) {
            const auto disposition = status == RunStatus::Completed ? ManifestWorkLedgerDisposition::ManifestReady
                                                                    : ManifestWorkLedgerDisposition::NotProduced;
            auto built = oc::terminalFacts(id, project, status, facts, proposals.empty() && status == RunStatus::Completed
                                                                       ? ManifestWorkLedgerDisposition::NoChangesNeeded : disposition);
            world.entries[id] = *built.ledgerEntry;
        }
    }

    void setProposalState(const std::string& csId, ChangeSetState state) { world.changeSets[csId].state = state; }
    void approve(const std::string& csId) { world.approvals.insert(csId); }
    void reject(const std::string& csId)
    {
        ProposalReviewRejection record;
        record.changeSetId = csId;
        record.reason = "not wanted";
        world.rejections[csId] = record;
    }
    void setStatus(const std::string& id, RunStatus status) { world.runs[id].status = status; }

    OutcomeBriefReconcileOutcome reconcile(const std::string& runId)
    {
        const auto result = reconciler.reconcileRun(runId);
        oc::check(result.success(), "reconcileRun must succeed: " + (result.error().has_value() ? result.error()->code + " " + result.error()->message : std::string()));
        return result.value();
    }

    std::optional<OutcomeBriefRevision> head(const std::string& runId)
    {
        const auto found = briefs.findHead(runId);
        oc::check(found.success(), "findHead must succeed");
        return found.value();
    }
    std::size_t revisions(const std::string& runId) { return oc::revisionCount(briefs, runId); }
};

using Kind = OutcomeBriefReconcileOutcome::Kind;

// How often the Run row has been read so far: once per pass of a reconcile.
std::size_t runReads(const World& world)
{
    std::lock_guard lock(world.mutex);
    return static_cast<std::size_t>(std::count(world.calls.begin(), world.calls.end(), std::string("findRun")));
}

// ---- tests -------------------------------------------------------------------------------------------------------------------

void testOnlyOutcomeRunsGetABrief()
{
    Rig rig;
    rig.addRun("running", RunStatus::Running);
    rig.addRun("prepared-child", RunStatus::Prepared, {}, kProject, std::string(kManifestVerificationContinuationWorkflowId), false);
    rig.addRun("attempt", RunStatus::Completed, {}, kProject, std::string(kManifestVerificationManualHandoffWorkflowId));
    rig.addRun("mock", RunStatus::Completed, {}, kProject, kMockWorkflowId);
    for (const std::string id : {"running", "prepared-child", "attempt", "mock", "no-such-run"}) {
        const auto outcome = rig.reconcile(id);
        oc::check(outcome.kind == Kind::Skipped && !outcome.skippedBecause.empty(), id + ": not an outcome Run, so skipped with a reason");
        oc::check(!rig.head(id).has_value(), id + ": and no brief");
    }
}

// The lifecycle of a Completed Run, and the idempotency: duplicate recovery is ONE revision with identical bytes.
void testTheLifecycleOfACompletedRun()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});

    const auto first = rig.reconcile("run-1");
    oc::check(first.kind == Kind::Appended && first.state == OutcomeBriefState::AwaitingReview && first.ordinal == 1u, "awaiting review");
    const auto bytes = rig.head("run-1")->brief.renderedText;
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto again = rig.reconcile("run-1");
        oc::check(again.kind == Kind::Unchanged && again.ordinal == 1u, "duplicate recovery changes nothing");
    }
    oc::check(rig.revisions("run-1") == 1 && rig.head("run-1")->brief.renderedText == bytes, "...one revision with identical bytes");

    rig.approve("cs-1");
    const auto approved = rig.reconcile("run-1");
    oc::check(approved.kind == Kind::Appended && approved.state == OutcomeBriefState::AwaitingApply && approved.ordinal == 2u, "approved, not applied");
    rig.setProposalState("cs-1", ChangeSetState::Applied);
    const auto applied = rig.reconcile("run-1");
    oc::check(applied.kind == Kind::Appended && applied.state == OutcomeBriefState::ResolvedApplied && applied.ordinal == 3u, "applied");
    oc::check(isResolvedOutcomeBriefState(rig.head("run-1")->brief.state), "...resolved");
    oc::check(rig.reconcile("run-1").kind == Kind::Unchanged, "and stable");
}

void testAReturnToAnEarlierOutcomeIsANewRevision()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.reconcile("run-1");                                         // awaiting_review
    rig.setProposalState("cs-1", ChangeSetState::ApplyFailed);
    rig.reconcile("run-1");                                         // attention_required (apply failed)
    rig.setProposalState("cs-1", ChangeSetState::ReadyForReview);
    const auto third = rig.reconcile("run-1");                      // awaiting_review again
    oc::check(third.kind == Kind::Appended && third.ordinal == 3u && third.state == OutcomeBriefState::AwaitingReview, "A -> B -> A is three revisions");
}

void testHumanRejectionAndTheOtherStates()
{
    Rig rig;
    rig.addRun("rejected", RunStatus::Completed, {{"cs-r", ChangeSetState::ReadyForReview}});
    rig.reject("cs-r");
    oc::check(rig.reconcile("rejected").state == OutcomeBriefState::ResolvedRejected, "a human rejection");

    rig.addRun("none", RunStatus::Completed);
    oc::check(rig.reconcile("none").state == OutcomeBriefState::ResolvedNoChange, "nothing produced");

    rig.addRun("no-ledger", RunStatus::Completed, {}, kProject, kExecutionManifestWorkflowId, /*withLedger=*/false);
    const auto missing = rig.reconcile("no-ledger");
    oc::check(missing.state == OutcomeBriefState::AttentionRequired, "a missing ledger entry is attention, not a skip");
    oc::check(rig.head("no-ledger")->brief.reasons == std::vector<OutcomeBriefReason> {OutcomeBriefReason::SourceLedgerMissing}, "...with its typed reason");
    rig.world.entries["no-ledger"] = rig.world.entries["none"];
    rig.world.entries["no-ledger"].runId = "no-ledger";
    const auto repaired = rig.reconcile("no-ledger");
    oc::check(repaired.kind == Kind::Appended && repaired.state == OutcomeBriefState::ResolvedNoChange, "an entry that appears later yields a new revision");

    rig.addRun("cancelled", RunStatus::Cancelled);
    rig.addRun("interrupted", RunStatus::Interrupted);
    rig.addRun("failed", RunStatus::Failed);
    for (const std::string id : {"cancelled", "interrupted", "failed"}) {
        oc::check(rig.reconcile(id).state == OutcomeBriefState::RecoveryOnly, id + ": recovery only");
    }
    oc::check(!rig.head("cancelled")->brief.recoveryEligible && !rig.head("cancelled")->brief.requiresInspection, "cancelled: neither flag");
    oc::check(rig.head("interrupted")->brief.recoveryEligible && rig.head("interrupted")->brief.requiresInspection, "interrupted: both flags");
    oc::check(rig.head("failed")->brief.recoveryEligible && !rig.head("failed")->brief.requiresInspection, "failed: eligible");
}

void testContinuationAndManualHandoff()
{
    Rig rig;
    rig.addRun("source", RunStatus::Failed);
    oc::check(rig.reconcile("source").state == OutcomeBriefState::RecoveryOnly, "a Failed source nobody took over");
    rig.world.childOfSource["source"] = "child";
    rig.world.sourceOfChild["child"] = "source";
    oc::check(rig.reconcile("source").state == OutcomeBriefState::SupersededByContinuation, "a typed continuation child supersedes it");
    oc::check(rig.head("source")->brief.continuationChildRunId == std::optional<std::string>("child"), "...and names the child");
    rig.addRun("child", RunStatus::Completed, {{"cs-c", ChangeSetState::Applied}}, kProject, std::string(kManifestVerificationContinuationWorkflowId));
    const auto child = rig.reconcile("child");
    oc::check(child.state == OutcomeBriefState::ResolvedApplied, "the child has its own brief");
    oc::check(rig.head("child")->brief.continuationSourceRunId == std::optional<std::string>("source"), "...which names its source");

    rig.addRun("anchor", RunStatus::Failed, {{"cs-a", ChangeSetState::ReadyForReview}});
    rig.world.handoffOfAnchor["anchor"] = "handoff-1";
    rig.world.resolutionOfAnchor["anchor"] = Resolution::Unresolved;
    oc::check(rig.reconcile("anchor").state == OutcomeBriefState::AwaitingManualResolution, "a handoff waits for its resolution");
    rig.world.resolutionOfAnchor["anchor"] = Resolution::VerifiedAwaitingApply;
    oc::check(rig.reconcile("anchor").state == OutcomeBriefState::AwaitingManualResolution, "verified is not yet applied");
    rig.world.resolutionOfAnchor["anchor"] = Resolution::ResolvedManuallyApplied;
    oc::check(rig.reconcile("anchor").state == OutcomeBriefState::ResolvedManuallyApplied, "resolved manually");
    oc::check(rig.head("anchor")->brief.manualHandoffId == std::optional<std::string>("handoff-1"), "...naming the handoff");
}

// A stale head (the Run moved on) is replaced by the brief that is true now.
void testAStaleHeadIsReplaced()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Interrupted);
    rig.reconcile("run-1");
    rig.setStatus("run-1", RunStatus::Failed);
    rig.world.entries["run-1"].terminalStatus = RunStatus::Failed;
    oc::check(!rig.briefs.findHead("run-1").success(), "setup: the stored head is stale and unreadable");
    const auto outcome = rig.reconcile("run-1");
    oc::check(outcome.kind == Kind::Appended && outcome.ordinal == 2u, "the reconciler replaces it through the writer's read");
    oc::check(rig.head("run-1")->brief.terminalStatus == RunStatus::Failed, "...and the head reads again");
}

void testFactsAreFailClosed()
{
    for (const std::string readName : {"findProject", "findLedgerEntry", "listLedgerTransitions", "findChangeSet", "findRejection",
             "findApproval", "continuationChildOf", "continuationSourceOf", "handoffIdOfAnchor", "findRun"}) {
        Rig rig;
        rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
        rig.world.failing.insert(readName);
        const auto result = rig.reconciler.reconcileRun("run-1");
        oc::check(!result.success() && result.error()->code == "fake.read_failed", readName + ": a failing read fails the reconcile");
        oc::check(!rig.briefs.findHead("run-1").value().has_value(), readName + ": and nothing is written");
    }
    Rig rig;
    rig.addRun("anchor", RunStatus::Failed);
    rig.world.handoffOfAnchor["anchor"] = "h";
    rig.world.failing.insert("handoffResolutionOf");
    oc::check(!rig.reconciler.reconcileRun("anchor").success() && !rig.briefs.findHead("anchor").value().has_value(),
        "a failing resolution reader fails the reconcile too");
}

// Transitions are audit evidence only: they enrich the decision with its time, reason and post-apply fingerprint.
void testTransitionsOnlyEnrichTheBrief()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-r", ChangeSetState::ReadyForReview}, {"cs-a", ChangeSetState::Applied}});
    rig.reject("cs-r");
    rig.reconcile("run-1");
    const std::string bare = rig.head("run-1")->brief.renderedText;
    oc::check(bare.find("redundant refactor") == std::string::npos, "setup: no reason without a transition");

    const auto when = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'500));
    ManifestWorkLedgerReviewTransition rejected;
    rejected.id = "t-r";
    rejected.runId = "run-1";
    rejected.changeSetId = "cs-r";
    rejected.kind = ManifestWorkLedgerTransitionKind::Rejected;
    rejected.occurredAt = when;
    rejected.reason = "redundant refactor";
    ManifestWorkLedgerReviewTransition applied;
    applied.id = "t-a";
    applied.runId = "run-1";
    applied.changeSetId = "cs-a";
    applied.kind = ManifestWorkLedgerTransitionKind::Applied;
    applied.occurredAt = when;
    applied.postActionWorkspaceFingerprint = std::string(64, 'e');
    rig.world.transitions["run-1"] = {rejected, applied};
    const auto outcome = rig.reconcile("run-1");
    const std::string text = rig.head("run-1")->brief.renderedText;   // a copy: head() returns a temporary
    oc::check(outcome.kind == Kind::Appended, "the evidence changes the source state, so it is a new revision");
    oc::check(text.find("redundant refactor") != std::string::npos, "the human reason is carried (as data)");
    oc::check(text.find(std::string(64, 'e')) != std::string::npos, "the post-apply fingerprint is carried");
    oc::check(text.find(formatOutcomeBriefTimestamp(when)) != std::string::npos, "the time of the decision is carried");
    oc::check(rig.head("run-1")->brief.state == OutcomeBriefState::ResolvedMixed, "...while the typed decisions alone decide the state");
}

void testApprovalEvidenceCarriesItsTime()
{
    Rig rig;
    rig.addRun("run-2", RunStatus::Completed, {{"cs-p", ChangeSetState::ReadyForReview}});
    rig.approve("cs-p");
    rig.reconcile("run-2");
    const auto when = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'900));
    ManifestWorkLedgerReviewTransition approved;
    approved.id = "t-p";
    approved.runId = "run-2";
    approved.changeSetId = "cs-p";
    approved.kind = ManifestWorkLedgerTransitionKind::Approved;
    approved.occurredAt = when;
    rig.world.transitions["run-2"] = {approved};
    oc::check(rig.reconcile("run-2").kind == Kind::Appended, "the approval's time is new evidence");
    oc::check(rig.head("run-2")->brief.renderedText.find(formatOutcomeBriefTimestamp(when)) != std::string::npos, "...and it is carried");
    oc::check(rig.head("run-2")->brief.state == OutcomeBriefState::AwaitingApply, "the state is the typed one");
}

void testAMemberWithoutAChangeSetIsAFinding()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.world.changeSets.erase("cs-1");
    oc::check(rig.reconcile("run-1").state == OutcomeBriefState::AttentionRequired, "a ledger member nobody can find is attention");
    oc::check(rig.head("run-1")->brief.reasons == std::vector<OutcomeBriefReason> {OutcomeBriefReason::InconsistentSourceFacts},
        "...with the typed reason");
}

void testARepeatedMemberIsReadOnce()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.world.entries["run-1"].proposalIds.push_back("cs-1");
    rig.reconcile("run-1");
    oc::check(std::count(rig.world.calls.begin(), rig.world.calls.end(), std::string("findChangeSet")) == 1,
        "a repeated id is one fact, read once; the repetition itself is the resolver's finding");
    oc::check(rig.head("run-1")->brief.state == OutcomeBriefState::AttentionRequired, "...and the verdict is attention");
}

// The generic "looks like an absolute path" guard cannot see a root that is spelled relatively; the known spellings of the project's
// and the Run's own roots are the second line of defence, and the reconciler is what hands them to the resolver.
void testKnownRootsAreForbiddenEvenWhenTheyDoNotLookAbsolute()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    rig.world.projects[kProject].rootPath = std::filesystem::path("secret-root/inner");
    rig.world.runs["run-1"].workspacePath = std::filesystem::path("scratch-area/ws");
    rig.world.entries["run-1"].terminalReason = "files went to secret-root/inner/src";
    rig.world.entries["run-1"].remainingWorkNote = "and some to scratch-area/ws/out";
    rig.reconcile("run-1");
    const auto text = rig.head("run-1")->brief.renderedText + rig.head("run-1")->brief.sourceStateJson;
    oc::check(text.find("secret-root") == std::string::npos, "the project's own root is not in the brief, however it is spelled");
    oc::check(text.find("scratch-area") == std::string::npos, "...nor the Run's own workspace root");
}

void testTheRunWorkspacePathNeverReachesABriefEither()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    rig.world.runs["run-1"].workspacePath = std::filesystem::path("C:\\sandbox\\elsewhere\\ws");
    rig.world.entries["run-1"].terminalReason = "worked in C:\\sandbox\\elsewhere\\ws\\src and finished";
    rig.reconcile("run-1");
    const auto text = rig.head("run-1")->brief.renderedText + rig.head("run-1")->brief.sourceStateJson;
    oc::check(text.find("elsewhere") == std::string::npos, "the absolute path of the Run's own workspace is not in the brief");
}

// The Run leaves its terminal status between the read of its facts and the write of the brief: the brief is not written, and that
// is a skip, not an error.
void testARunThatMovesOnWhileBeingReconciledIsSkipped()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    rig.world.afterRead["findRun"] = [&] { rig.world.runs["run-1"].status = RunStatus::Running; };
    const auto outcome = rig.reconcile("run-1");
    oc::check(outcome.kind == Kind::Skipped && !outcome.skippedBecause.empty(), "skipped with a reason");
    oc::check(!rig.head("run-1").has_value(), "...and nothing is written");
}

// The production sources are thin READ views over the repositories: every answer is the repository's own.
class ProposalsStub final : public IChangeProposalRepository {
public:
    std::map<std::string, ProposedChangeSet> sets;
    common::Result<void> insertChangeSet(const ProposedChangeSet&) override { return unsupported<void>(); }
    common::Result<ProposedChangeSet> updateChangeSetState(const ChangeSetId&, ChangeSetState) override { return unsupported<ProposedChangeSet>(); }
    common::Result<void> markMissingEvidence(const ChangeSetId&, std::string, std::string) override { return unsupported<void>(); }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId& id) const override
    {
        const auto found = sets.find(id);
        if (found == sets.end()) return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(found->second);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId&) const override { return unsupported<std::vector<ProposedChangeSet>>(); }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override { return unsupported<std::vector<ProposedChangeSet>>(); }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId&) const override { return unsupported<std::vector<ProposedFileChange>>(); }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId&) const override { return unsupported<std::vector<ValidationFinding>>(); }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>> listChangesForSets(const std::vector<ChangeSetId>&) const override
    {
        return unsupported<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>();
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>> listValidationFindingsForSets(const std::vector<ChangeSetId>&) const override
    {
        return unsupported<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>();
    }

private:
    template <typename T> static common::Result<T> unsupported() { return common::Result<T>::failure({"stub.unsupported", "not used"}); }
};

class ApprovalsStub final : public IChangeApprovalRepository {
public:
    std::set<std::string> approved;
    common::Result<void> recordApproval(const ApprovalRecord&) override { return common::Result<void>::failure({"stub.unsupported", "not used"}); }
    common::Result<std::optional<ApprovalRecord>> findApproval(const ChangeSetId& id) const override
    {
        if (approved.count(id) == 0) return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
        ApprovalRecord record;
        record.changeSetId = id;
        return common::Result<std::optional<ApprovalRecord>>::ok(record);
    }
    common::Result<void> recordApplyOutcome(const ChangeSetId&, const std::string&, const std::optional<std::string>&,
        const std::vector<ApplyHashEntry>&, const std::optional<std::string>&) override
    {
        return common::Result<void>::failure({"stub.unsupported", "not used"});
    }
};

class ReviewsStub final : public IProposalReviewRepository {
public:
    std::set<std::string> rejected;
    common::Result<void> recordRejection(const ProposalReviewRejection&) override { return common::Result<void>::failure({"stub.unsupported", "not used"}); }
    common::Result<std::optional<ProposalReviewRejection>> findRejection(const ChangeSetId& id) const override
    {
        if (rejected.count(id) == 0) return common::Result<std::optional<ProposalReviewRejection>>::ok(std::nullopt);
        ProposalReviewRejection record;
        record.changeSetId = id;
        return common::Result<std::optional<ProposalReviewRejection>>::ok(record);
    }
};

class ResolutionsStub final : public IManifestVerificationManualHandoffResolutionReader {
public:
    std::map<std::string, Resolution> byRun;
    common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>> resolutionsForRuns(const std::vector<Run>& runs) const override
    {
        std::vector<ManifestVerificationManualHandoffRunResolution> answer;
        for (const Run& run : runs) {
            const auto found = byRun.find(run.id);
            if (found != byRun.end()) answer.push_back({run.id, found->second});
        }
        return common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>::ok(std::move(answer));
    }
};

// A project repository that answers in DESCENDING id order: the sources' own order is what the history pass relies on.
class DescendingProjects final : public IProjectRepository {
public:
    common::Result<Project> insert(Project) override { return common::Result<Project>::failure({"stub.unsupported", "not used"}); }
    common::Result<Project> update(Project) override { return common::Result<Project>::failure({"stub.unsupported", "not used"}); }
    common::Result<std::optional<Project>> findById(const ProjectId&) const override { return common::Result<std::optional<Project>>::ok(std::nullopt); }
    common::Result<std::optional<Project>> findByRootPath(const std::filesystem::path&) const override { return common::Result<std::optional<Project>>::ok(std::nullopt); }
    common::Result<std::vector<Project>> listAll() const override
    {
        std::vector<Project> projects;
        for (const char* id : {"c-project", "b-project", "a-project"}) {
            Project project;
            project.id = id;
            projects.push_back(project);
        }
        return common::Result<std::vector<Project>>::ok(std::move(projects));
    }
};

void testTheSourcesListProjectsSortedWhateverOrderTheRepositoryAnswersIn()
{
    DescendingProjects projects;
    InMemoryRunRepository runs;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository handoffs;
    ProposalsStub proposals;
    ApprovalsStub approvals;
    ReviewsStub reviews;
    ResolutionsStub resolutions;
    const RepositoryOutcomeBriefSources sources(runs, projects, ledger, proposals, approvals, reviews, continuations, handoffs, resolutions);
    const auto ids = sources.listProjectIds();
    oc::check(ids.success() && ids.value() == (std::vector<std::string> {"a-project", "b-project", "c-project"}),
        "the history pass visits the projects in id order, whatever order the repository lists them in");
}

void testTheRepositorySourcesAnswerFromTheRepositories()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository handoffs;
    ProposalsStub proposals;
    ApprovalsStub approvals;
    ReviewsStub reviews;
    ResolutionsStub resolutions;
    const RepositoryOutcomeBriefSources sources(runs, projects, ledger, proposals, approvals, reviews, continuations, handoffs, resolutions);
    const auto must = [](const auto& result, const std::string& what) {
        oc::check(result.success(), what + ": the read must succeed" + (result.error().has_value() ? ": " + result.error()->code + " " + result.error()->message : std::string()));
    };

    Project project;
    project.id = kProject;
    project.name = "Sources";
    project.rootPath = std::filesystem::path("C:\\root");
    oc::check(projects.insert(project).success(), "setup: a project");
    Project earlier = project;
    earlier.id = "a-project";
    earlier.name = "Listed first";
    earlier.rootPath = std::filesystem::path("C:\\other-root");
    oc::check(projects.insert(earlier).success(), "setup: a project whose id sorts first, inserted second");
    for (const std::string id : {"source", "child", "anchor", "attempt", "plain"}) {
        Run run;
        run.id = id;
        run.projectId = kProject;
        run.title = id;
        run.task = id;
        run.workflowId = id == "attempt" ? std::string(kManifestVerificationManualHandoffWorkflowId)
            : id == "child"              ? std::string(kManifestVerificationContinuationWorkflowId)
                                         : std::string(kExecutionManifestWorkflowId);
        run.status = RunStatus::Failed;
        run.createdAt = std::chrono::system_clock::now();
        oc::check(runs.insertRun(run).success(), "setup: the Run " + id);
    }

    // Runs and projects.
    const auto run = sources.findRun("source");
    must(run, "findRun");
    oc::check(run.value().has_value() && run.value()->id == "source", "findRun answers with the Run");
    oc::check(!sources.findRun("nobody").value().has_value(), "...and with nothing for an unknown id");
    const auto projectIds = sources.listProjectIds();
    must(projectIds, "listProjectIds");
    oc::check(projectIds.value() == (std::vector<std::string> {"a-project", kProject}), "listProjectIds lists the projects, sorted by id");
    // The outcome Runs of a project, a page at a time, newest first; the handoff attempt Run is never one.
    const auto page1 = sources.listOutcomeRunsPage(kProject, std::nullopt, 3);
    must(page1, "listOutcomeRunsPage");
    oc::check(page1.value().size() == 3, "a page holds at most its limit");
    const auto page2 = sources.listOutcomeRunsPage(kProject, RunPageCursor {page1.value().back().createdAt, page1.value().back().id}, 3);
    must(page2, "the second page");
    std::set<std::string> seen;
    for (const auto& listed : page1.value()) seen.insert(listed.id);
    for (const auto& listed : page2.value()) seen.insert(listed.id);
    oc::check(page2.value().size() == 1 && seen == std::set<std::string> {"source", "child", "anchor", "plain"},
        "two pages are the four outcome Runs, once each (the attempt Run is not one)");
    oc::check(sources.findProject(kProject).value().has_value() && !sources.findProject("nobody").value().has_value(), "findProject");

    // Ledger and transitions.
    auto built = oc::terminalFacts("source", kProject, RunStatus::Failed, {}, ManifestWorkLedgerDisposition::NotProduced);
    oc::check(ledger.insertEntry(*built.ledgerEntry).success(), "setup: a ledger entry");
    ManifestWorkLedgerReviewTransition transition;
    transition.id = "t-1";
    transition.runId = "source";
    transition.changeSetId = "cs-1";
    transition.kind = ManifestWorkLedgerTransitionKind::Approved;
    transition.occurredAt = std::chrono::system_clock::now();
    oc::check(ledger.appendTransition(transition).success(), "setup: a transition");
    oc::check(sources.findLedgerEntry("source").value().has_value() && !sources.findLedgerEntry("plain").value().has_value(), "findLedgerEntry");
    oc::check(sources.listLedgerTransitions("source").value().size() == 1 && sources.listLedgerTransitions("plain").value().empty(),
        "listLedgerTransitions");

    // Proposal, approval and rejection.
    ProposedChangeSet set;
    set.changeSetId = "cs-1";
    set.runId = "source";
    set.projectId = kProject;
    set.state = ChangeSetState::ReadyForReview;
    proposals.sets["cs-1"] = set;
    approvals.approved.insert("cs-1");
    reviews.rejected.insert("cs-2");
    oc::check(sources.findChangeSet("cs-1").value()->state == ChangeSetState::ReadyForReview && !sources.findChangeSet("cs-9").value().has_value(), "findChangeSet");
    oc::check(sources.findApproval("cs-1").value().has_value() && !sources.findApproval("cs-2").value().has_value(), "findApproval");
    oc::check(sources.findRejection("cs-2").value().has_value() && !sources.findRejection("cs-1").value().has_value(), "findRejection");

    // Continuation relation: the child and the source are not interchangeable.
    ManifestVerificationContinuationInput input;
    input.sourceRunId = "source";
    input.childRunId = "child";
    input.sourceProjectId = kProject;
    input.attributionId = "attribution-1";
    input.baselineFingerprint = std::string(64, 'a');
    input.sourceWorkspaceIdentity = kProject;
    input.sourceOverlayFingerprint = std::string(64, 'b');
    input.orderedProposalIds = {"cs-1"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = std::chrono::system_clock::now();
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    const auto insertedInput = continuations.insertInput(input);
    oc::check(insertedInput.success(), "setup: a continuation input" + (insertedInput.error().has_value() ? ": " + insertedInput.error()->message : std::string()));
    oc::check(sources.continuationChildOf("source").value() == std::optional<std::string>("child"), "the child of a source");
    oc::check(sources.continuationSourceOf("child").value() == std::optional<std::string>("source"), "the source of a child");
    oc::check(!sources.continuationChildOf("child").value().has_value() && !sources.continuationSourceOf("source").value().has_value(),
        "...and never the other way round");

    // Manual handoff: anchor, frozen member and attempt Run.
    ManifestVerificationManualHandoff handoff;
    handoff.id = "handoff-1";
    handoff.originManifestRunId = "origin-run";   // not the anchor: the adapter must answer with the ANCHOR
    handoff.verificationAnchorRunId = "anchor";
    handoff.sourceProjectId = kProject;
    handoff.storageKey = "key-1";
    handoff.workspaceIdentity = kProject;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {{"cs-m", "anchor", std::string(64, 'c'), "cs-m"}};
    handoff.allowedPaths = {{"qml/App.qml", "cs-m", "component"}};
    WorkspaceBaselineSnapshot snapshot;
    snapshot.fingerprint = sha256Hex("");
    handoff.originalSnapshot = snapshot;
    handoff.createdAt = std::chrono::system_clock::now();
    const auto insertedHandoff = handoffs.insertHandoff(handoff);
    oc::check(insertedHandoff.success(), "setup: a handoff" + (insertedHandoff.error().has_value() ? ": " + insertedHandoff.error()->message : std::string()));
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "attempt-1";
    attempt.handoffId = "handoff-1";
    attempt.verificationAnchorRunId = "anchor";
    attempt.attemptRunId = "attempt";
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'd');
    attempt.createdAt = std::chrono::system_clock::now();
    const auto insertedAttempt = handoffs.insertAttempt(attempt);
    oc::check(insertedAttempt.success(), "setup: an attempt" + (insertedAttempt.error().has_value() ? ": " + insertedAttempt.error()->message : std::string()));
    oc::check(sources.handoffIdOfAnchor("anchor").value() == std::optional<std::string>("handoff-1") && !sources.handoffIdOfAnchor("plain").value().has_value(),
        "the handoff of an anchor");
    oc::check(sources.handoffAnchorOfMemberProposal("cs-m").value() == std::optional<std::string>("anchor") && !sources.handoffAnchorOfMemberProposal("cs-1").value().has_value(),
        "the anchor of a frozen member");
    oc::check(sources.handoffAnchorOfAttemptRun("attempt").value() == std::optional<std::string>("anchor") && !sources.handoffAnchorOfAttemptRun("plain").value().has_value(),
        "the anchor of an attempt Run");

    // The resolution comes from the reader; a Run it says nothing about has none to resolve.
    resolutions.byRun["anchor"] = Resolution::PartiallyApplied;
    const auto anchorRun = sources.findRun("anchor").value().value();
    oc::check(sources.handoffResolutionOf(anchorRun).value() == Resolution::PartiallyApplied, "the resolution of an anchor");
    oc::check(sources.handoffResolutionOf(sources.findRun("plain").value().value()).value() == Resolution::NotApplicable, "...and none for a Run without a handoff");
}

// The brief never carries the absolute path of the project, even when provider prose does.
void testTheProjectPathNeverReachesABrief()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed);
    rig.world.entries["run-1"].terminalReason = "wrote files under " + kRootPath + "\\src and finished";
    rig.reconcile("run-1");
    const auto text = rig.head("run-1")->brief.renderedText + rig.head("run-1")->brief.sourceStateJson;
    oc::check(text.find("secret-workspace") == std::string::npos, "the absolute project path is not in the brief");
}

// The reconciler cannot change the world: the sources have no mutator, and the world is byte-for-byte the same afterwards.
void testTheReconcilerOnlyReads()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.addRun("run-2", RunStatus::Failed);
    const auto runsBefore = rig.world.runs.size();
    const auto stateBefore = rig.world.changeSets["cs-1"].state;
    const auto entryBefore = rig.world.entries["run-1"].verificationSummary;
    rig.reconcile("run-1");
    rig.reconcile("run-2");
    static_cast<void>(rig.reconciler.reconcileHistory());
    rig.reconciler.proposalResolved("cs-1");
    rig.reconciler.runMayHaveChanged("run-1");
    oc::check(rig.world.runs.size() == runsBefore && rig.world.changeSets["cs-1"].state == stateBefore
            && rig.world.entries["run-1"].verificationSummary == entryBefore && rig.world.approvals.empty() && rig.world.rejections.empty(),
        "Run, proposal, review and ledger state are untouched");
    for (const std::string& call : rig.world.calls) {
        oc::check(call.rfind("find", 0) == 0 || call.rfind("list", 0) == 0 || call.rfind("continuation", 0) == 0
                || call.rfind("handoff", 0) == 0,
            "only read calls were made, got " + call);
    }
}

// The best-effort entries: they find the affected Runs, never throw and never fail their caller.
void testTheTriggerEntries()
{
    Rig rig;
    rig.addRun("anchor", RunStatus::Failed, {{"cs-a", ChangeSetState::Applied}});
    rig.world.handoffOfAnchor["anchor"] = "h";
    rig.world.resolutionOfAnchor["anchor"] = Resolution::PartiallyApplied;
    rig.addRun("attempt", RunStatus::Completed, {{"cs-s", ChangeSetState::Applied}}, kProject, std::string(kManifestVerificationManualHandoffWorkflowId));
    rig.world.anchorOfAttemptRun["attempt"] = "anchor";
    rig.world.anchorOfMember["cs-m"] = "anchor";
    rig.addRun("plain", RunStatus::Completed, {{"cs-p", ChangeSetState::ReadyForReview}});
    rig.addRun("holder", RunStatus::Completed, {{"cs-m", ChangeSetState::Applied}});

    rig.reconciler.runMayHaveChanged("plain");
    oc::check(rig.head("plain").has_value() && rig.head("plain")->brief.state == OutcomeBriefState::AwaitingReview, "runMayHaveChanged writes the brief");
    rig.setProposalState("cs-p", ChangeSetState::Applied);
    rig.reconciler.proposalResolved("cs-p");
    oc::check(rig.head("plain")->brief.state == OutcomeBriefState::ResolvedApplied, "proposalResolved moves it on");

    // A proposal of a handoff attempt Run, and a frozen member proposal, move the ANCHOR; the attempt Run itself gets no brief.
    rig.reconciler.proposalResolved("cs-s");
    oc::check(rig.head("anchor").has_value() && rig.head("anchor")->brief.state == OutcomeBriefState::AwaitingManualResolution, "the anchor of an attempt proposal");
    oc::check(!rig.head("attempt").has_value(), "...and the attempt Run has no brief");
    rig.world.resolutionOfAnchor["anchor"] = Resolution::ResolvedManuallyApplied;
    rig.reconciler.proposalResolved("cs-m");
    oc::check(rig.head("anchor")->brief.state == OutcomeBriefState::ResolvedManuallyApplied, "the anchor of a frozen member proposal");
    rig.world.resolutionOfAnchor["anchor"] = Resolution::Unresolved;
    rig.reconciler.runMayHaveChanged("attempt");
    oc::check(rig.head("anchor")->brief.state == OutcomeBriefState::AwaitingManualResolution, "a finished attempt Run moves its anchor");

    // Unknown ids, failing reads and throwing reads are all swallowed.
    const auto before = rig.reconciler.nudgeCounts();
    rig.reconciler.proposalResolved("no-such-proposal");
    rig.reconciler.runMayHaveChanged("no-such-run");
    rig.world.failing.insert("findRun");
    rig.reconciler.runMayHaveChanged("plain");
    rig.world.failing.clear();
    rig.world.throwing = true;
    rig.reconciler.runMayHaveChanged("plain");
    rig.reconciler.proposalResolved("cs-p");
    rig.world.throwing = false;
    const auto after = rig.reconciler.nudgeCounts();
    oc::check(after.runs - before.runs == 3 && after.proposals - before.proposals == 2,
        "every nudge is counted, whatever came of it (two proposals, three Runs)");
}

// The race that identical facts cannot show: a reconciler that read its facts BEFORE the world moved on must not be able to write them
// after a newer reconciler has written the new state - it has to lose the compare-and-set, read everything again and change nothing.
// The newer one is a SEPARATE reconciler on the same repository here: one reconciler would catch this itself (see the A -> B -> A test
// below), and what is proven in this test is the compare-and-set and the order of the reads, which are all a second one can rely on.
void testALateReconcilerCannotOverwriteANewerHead()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    OutcomeBriefReconciler otherReconciler {rig.sources, rig.briefs};
    bool raced = false;
    // The last read of the first reconciler's facts (the handoff of the anchor) answers - and then the world moves on and a second
    // reconciler runs to the end: the proposal is applied and the new outcome is stored.
    rig.world.afterRead["handoffIdOfAnchor"] = [&] {
        rig.setProposalState("cs-1", ChangeSetState::Applied);
        const auto second = otherReconciler.reconcileRun("run-1");
        oc::check(second.success() && second.value().kind == Kind::Appended && second.value().state == OutcomeBriefState::ResolvedApplied,
            "the second reconciler writes the new outcome");
        raced = true;
    };
    const auto late = rig.reconciler.reconcileRun("run-1");   // the first one, holding the OLD facts
    oc::check(raced, "setup: the race happened");
    oc::check(late.success() && late.value().kind == Kind::Unchanged && late.value().state == OutcomeBriefState::ResolvedApplied,
        "the late reconciler lost the compare-and-set, read everything again and found nothing to change");
    oc::check(rig.revisions("run-1") == 1 && rig.head("run-1")->brief.state == OutcomeBriefState::ResolvedApplied,
        "the head is the NEW state, with no later correction: the old state was never written");
}

// The race the compare-and-set cannot see (review 2): the head id only moves when something is WRITTEN, and a reconcile that finds the
// head already saying what it sees writes nothing. A -> B -> A: the first reconcile holds B (its facts were read while the world was in
// B), the world goes back to A, and the second reconcile finds the head - A, revision 1 - exactly right and leaves it alone. The first
// one still holds the id of revision 1, so its compare-and-set would pass and B would be the head for good. It must notice that a verdict
// was stored since it began, read everything again and find A. (recovery_only -> a handoff is created -> the handoff is deleted.)
void testAnOutcomeThatIsOverBeforeItIsStoredNeverBecomesTheHead()
{
    Rig rig;
    rig.addRun("probe", RunStatus::Failed);
    rig.world.handoffOfAnchor["probe"] = "handoff-probe";
    rig.world.resolutionOfAnchor["probe"] = Resolution::Unresolved;
    oc::check(rig.reconcile("probe").state == OutcomeBriefState::AwaitingManualResolution, "setup: a handoff on a Failed anchor is the state B");

    rig.addRun("run-1", RunStatus::Failed);
    const auto first = rig.reconcile("run-1");
    oc::check(first.kind == Kind::Appended && first.state == OutcomeBriefState::RecoveryOnly && first.ordinal == 1u,
        "setup: A (nobody took the Run over) is the head");
    const std::string headOfA = rig.head("run-1")->id;
    oc::check(rig.reconciler.trackedRunCount() == 0, "setup: nothing is remembered while no reconcile runs");

    // The world moves to B: a manual handoff appears for the anchor.
    rig.world.handoffOfAnchor["run-1"] = "handoff-1";
    rig.world.resolutionOfAnchor["run-1"] = Resolution::Unresolved;
    bool raced = false;
    // The LAST read of the first reconcile's facts answers - B - and only then is the handoff gone again (A) and a second reconcile of the
    // same Run run to the end.
    rig.world.afterRead["handoffResolutionOf"] = [&] {
        rig.world.handoffOfAnchor.erase("run-1");
        rig.world.resolutionOfAnchor.erase("run-1");
        const auto second = rig.reconciler.reconcileRun("run-1");
        oc::check(second.success() && second.value().kind == Kind::Unchanged && second.value().state == OutcomeBriefState::RecoveryOnly
                && second.value().ordinal == 1u,
            "the second reconcile finds A stored and exactly right");
        oc::check(rig.head("run-1")->id == headOfA, "...so it wrote nothing, and the head id did not move");
        oc::check(rig.reconciler.trackedRunCount() == 1, "...but its verdict is remembered for as long as the first reconcile is in flight");
        raced = true;
    };
    const auto late = rig.reconciler.reconcileRun("run-1");   // the first one, holding B
    oc::check(raced, "setup: the race happened");
    oc::check(late.success() && late.value().kind == Kind::Unchanged && late.value().state == OutcomeBriefState::RecoveryOnly
            && late.value().ordinal == 1u,
        "the first reconcile did not store B: it read everything again and found A");
    oc::check(rig.revisions("run-1") == 1 && rig.head("run-1")->id == headOfA && rig.head("run-1")->brief.state == OutcomeBriefState::RecoveryOnly,
        "B was never stored: the head is still revision 1, A");
    oc::check(rig.reconciler.trackedRunCount() == 0, "...and nothing is remembered once no reconcile is in flight");
}

// The other half of the same rule: a verdict that stored the SAME thing this reconcile is about to store is no reason to read everything
// again - writing the same state is a no-op whoever saw it first. Counted in the reads of the Run: the first reconcile and the one it
// races with are two, a needless third would be a second pass of the first.
void testAVerdictThatAgreesCostsNoSecondPass()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.reconcile("run-1");
    const std::size_t before = runReads(rig.world);
    bool raced = false;
    rig.world.afterRead["handoffIdOfAnchor"] = [&] {
        const auto second = rig.reconciler.reconcileRun("run-1");
        oc::check(second.success() && second.value().kind == Kind::Unchanged, "the second reconcile finds the same state");
        raced = true;
    };
    const auto first = rig.reconciler.reconcileRun("run-1");
    oc::check(raced, "setup: the race happened");
    oc::check(first.success() && first.value().kind == Kind::Unchanged && first.value().ordinal == 1u, "the first reconcile finds it unchanged too");
    oc::check(runReads(rig.world) - before == 2, "...in one pass: two reads of the Run in all, the first reconcile's and the second's");
    oc::check(rig.revisions("run-1") == 1 && rig.reconciler.trackedRunCount() == 0, "one revision, and nothing left remembered");
}

// ...and a reconcile that BEGAN after a verdict was stored is not held up by it: that verdict is older than everything it reads. The first
// reconcile holds A; inside it a second one stores nothing (A is right), then the world moves to B and a third one - which began after the
// second's verdict - stores B at once. The first must then look again (B is newer than what it holds) and find nothing to change.
void testAReconcileThatBeginsAfterAVerdictIsNotHeldUpByIt()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.reconcile("run-1");   // A: awaiting review, revision 1
    const std::size_t before = runReads(rig.world);
    bool raced = false;
    rig.world.afterRead["handoffIdOfAnchor"] = [&] {
        const auto second = rig.reconciler.reconcileRun("run-1");
        oc::check(second.success() && second.value().kind == Kind::Unchanged, "the second reconcile stores nothing: A is right");
        rig.setProposalState("cs-1", ChangeSetState::Applied);
        const auto third = rig.reconciler.reconcileRun("run-1");
        oc::check(third.success() && third.value().kind == Kind::Appended && third.value().state == OutcomeBriefState::ResolvedApplied
                && third.value().ordinal == 2u,
            "the third reconcile began after that verdict and stores the new outcome without a second pass");
        raced = true;
    };
    const auto first = rig.reconciler.reconcileRun("run-1");   // holding A
    oc::check(raced, "setup: the race happened");
    oc::check(first.success() && first.value().kind == Kind::Unchanged && first.value().state == OutcomeBriefState::ResolvedApplied
            && first.value().ordinal == 2u,
        "the first reconcile looked again and found the new outcome stored");
    oc::check(runReads(rig.world) - before == 4, "two passes of the first reconcile and one each of the other two: nobody retried needlessly");
    oc::check(rig.revisions("run-1") == 2 && rig.reconciler.trackedRunCount() == 0, "two revisions, and nothing left remembered");
}

// The records follow the Runs in flight, not the history (review 3): while one Run hangs on a read, however many others are reconciled
// meanwhile, only the hanging one is remembered - each of the others is forgotten the moment its last pass is over.
void testTheBookkeepingFollowsTheRunsInFlightNotTheHistory()
{
    Rig rig;
    rig.addRun("slow", RunStatus::Completed, {{"cs-slow", ChangeSetState::ReadyForReview}});
    const std::vector<std::string> others = {"run-a", "run-b", "run-c", "run-d", "run-e"};
    for (const std::string& id : others) rig.addRun(id, RunStatus::Completed, {{"cs-" + id, ChangeSetState::ReadyForReview}});
    oc::check(rig.reconciler.trackedRunCount() == 0, "setup: nothing is remembered while no reconcile runs");
    bool raced = false;
    rig.world.afterRead["handoffIdOfAnchor"] = [&] {   // the first reconcile hangs here, right after a read has answered
        oc::check(rig.reconciler.trackedRunCount() == 1, "the Run that hangs is remembered");
        for (const std::string& id : others) {
            const auto outcome = rig.reconciler.reconcileRun(id);
            oc::check(outcome.success() && outcome.value().kind == Kind::Appended, id + ": reconciled while the first one hangs");
            oc::check(rig.reconciler.trackedRunCount() == 1,
                id + ": a Run whose reconcile is over is forgotten at once; only the one that hangs is remembered");
        }
        raced = true;
    };
    const auto slow = rig.reconciler.reconcileRun("slow");
    oc::check(raced && slow.success() && slow.value().kind == Kind::Appended, "setup: the race happened, and the hanging Run is reconciled too");
    oc::check(rig.reconciler.trackedRunCount() == 0, "...and nothing is remembered once no reconcile runs");
}

// A reconciler constructed with a refusal - what a guest of a database that another runtime owns gets - reads nothing and writes nothing:
// every entry fails closed with the refusal BEFORE a single source is read (review 3), while an ordinary reconciler over the same world and
// the same repository is not affected.
void testAReconcilerThatMustNotWriteReadsAndWritesNothing()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    const common::Error refusal {"outcome_brief.not_the_owner", "a guest does not write briefs"};
    OutcomeBriefReconciler refusing {rig.sources, rig.briefs, refusal};
    const auto readsSoFar = [&] {
        std::lock_guard lock(rig.world.mutex);
        return rig.world.calls.size();
    };
    const std::size_t before = readsSoFar();

    const auto refused = [](const auto& result, const std::string& entry) {
        oc::check(!result.success() && result.error()->code == "outcome_brief.not_the_owner", entry + ": refused, with the reason");
    };
    refused(refusing.reconcileRun("run-1"), "reconcileRun");
    refused(refusing.reconcileHistory(), "reconcileHistory");
    refused(refusing.affectedByRun("run-1"), "affectedByRun");
    refused(refusing.affectedByProposal("cs-1"), "affectedByProposal");
    refusing.runMayHaveChanged("run-1");
    refusing.proposalResolved("cs-1");
    oc::check(refusing.nudgeCounts().runs == 1 && refusing.nudgeCounts().proposals == 1, "the nudges are still counted");
    oc::check(readsSoFar() == before, "...and not one source was read by any of the entries");
    oc::check(!rig.head("run-1").has_value() && rig.revisions("run-1") == 0, "...and nothing was written");

    const auto ordinary = rig.reconcile("run-1");   // the same world, the same repository, no refusal
    oc::check(ordinary.kind == Kind::Appended && ordinary.ordinal == 1u, "an ordinary reconciler over them is not affected");
}

// A repository that lets a test act while a write is in progress: `beforeAppend` runs ONCE, at the start of the next append - that is, in
// whatever critical section the caller has put the write in.
class BriefsWithHook final : public IOutcomeBriefRepository {
public:
    explicit BriefsWithHook(IOutcomeBriefRepository& inner) : inner_(inner) {}

    std::function<void()> beforeAppend;

    common::Result<OutcomeBriefAppendResult> append(OutcomeBrief brief, OutcomeBriefExpectedHead expected) override
    {
        if (beforeAppend) {
            const auto hook = std::move(beforeAppend);
            beforeAppend = nullptr;
            hook();
        }
        return inner_.append(std::move(brief), std::move(expected));
    }
    common::Result<std::optional<OutcomeBriefRevision>> findHead(const RunId& runId) const override { return inner_.findHead(runId); }
    common::Result<std::optional<OutcomeBriefRevision>> findRevision(const OutcomeBriefRevisionId& id) const override
    {
        return inner_.findRevision(id);
    }
    common::Result<std::vector<OutcomeBriefRevision>> listRevisions(const RunId& runId, std::size_t limit) const override
    {
        return inner_.listRevisions(runId, limit);
    }
    common::Result<std::vector<OutcomeBriefRevision>> listHeads(const ProjectId& projectId, std::size_t limit) const override
    {
        return inner_.listHeads(projectId, limit);
    }
    common::Result<std::optional<OutcomeBriefHeadVersion>> findHeadVersionForAppend(const RunId& runId) const override
    {
        return inner_.findHeadVersionForAppend(runId);
    }

private:
    IOutcomeBriefRepository& inner_;
};

// The check and the write are ONE step. The first reconcile has passed its final check (nothing was stored since it began) and is about to
// write B when the handoff disappears and a second reconcile starts on another thread. If the check and the write were two steps, the
// second would find A already stored, store its verdict and the first would then still write B over it. With one critical section it
// cannot even begin before the first has written; it then reads B, finds A in the world and corrects the head. (The wait below only
// gives a broken implementation the time to show itself: a correct one never depends on it.)
void testTheFinalCheckAndTheWriteAreOneStep()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Failed);
    rig.reconcile("run-1");   // A is the head

    BriefsWithHook hooked {rig.briefs};
    OutcomeBriefReconciler reconciler {rig.sources, hooked};
    rig.world.handoffOfAnchor["run-1"] = "handoff-1";   // the world is in B
    rig.world.resolutionOfAnchor["run-1"] = Resolution::Unresolved;

    std::atomic<bool> secondFinished {false};
    bool secondFinishedBeforeTheFirstWrote = false;
    std::optional<common::Result<OutcomeBriefReconcileOutcome>> secondResult;
    std::thread second;
    hooked.beforeAppend = [&] {
        rig.world.handoffOfAnchor.erase("run-1");   // the world is back in A
        rig.world.resolutionOfAnchor.erase("run-1");
        second = std::thread([&] {
            secondResult = reconciler.reconcileRun("run-1");
            secondFinished = true;
        });
        for (int waited = 0; waited < 100 && !secondFinished.load(); ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        secondFinishedBeforeTheFirstWrote = secondFinished.load();
    };
    const auto first = reconciler.reconcileRun("run-1");   // holding B
    if (second.joinable()) second.join();

    oc::check(secondResult.has_value(), "setup: the second reconcile ran to the end");
    oc::check(!secondFinishedBeforeTheFirstWrote, "the second reconcile could not finish while the first was writing");
    oc::check(first.success() && first.value().kind == Kind::Appended && first.value().state == OutcomeBriefState::AwaitingManualResolution,
        "the first reconcile wrote what it had read (the world had not been seen to change when it passed its check)");
    oc::check(secondResult.has_value() && secondResult->success() && secondResult->value().kind == Kind::Appended
            && secondResult->value().state == OutcomeBriefState::RecoveryOnly && secondResult->value().ordinal == 3u,
        "the second reconcile came after that write, read B, found A in the world and corrected the head");
    oc::check(rig.revisions("run-1") == 3 && rig.head("run-1")->brief.state == OutcomeBriefState::RecoveryOnly,
        "the final head is A: recovery_only -> awaiting_manual_resolution -> recovery_only");
}

// A hook and the startup pass race on one Run: one revision, no matter who wins.
void testRacingReconcilesAreOneRevision()
{
    Rig rig;
    rig.addRun("run-1", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    std::atomic<int> appended {0};
    oc::runTogether(8, [&](int) {
        const auto result = rig.reconciler.reconcileRun("run-1");
        oc::check(result.success(), "a racing reconcile must succeed");
        if (result.value().kind == Kind::Appended) ++appended;
    });
    oc::check(appended.load() == 1 && rig.revisions("run-1") == 1, "exactly one writer appended, everybody else found it unchanged");
}

void testRunPagingOnTheInMemoryRepository()
{
    InMemoryRunRepository runs;
    runpaging_contract::Env env;
    env.repository = &runs;
    env.insert = [&](const Run& run) { oc::check(runs.insertRun(run).success(), "setup: a Run"); };
    env.ensureProject = [](const std::string&) {};
    runpaging_contract::runRunPagingContract(env);
}

void testTheHistoryPass()
{
    Rig rig;
    // project-1: five terminal outcome Runs, oldest first. The newest two always get a brief; of the older three, the one with a
    // proposal still awaiting review (p1-b) gets one whatever its age, and the settled ones (p1-a and p1-c, the first Run just
    // outside the window) are left alone.
    rig.addRun("p1-a", RunStatus::Completed);
    rig.addRun("p1-b", RunStatus::Completed, {{"cs-b", ChangeSetState::ReadyForReview}});
    rig.addRun("p1-c", RunStatus::Completed);
    rig.addRun("p1-d", RunStatus::Completed);
    rig.addRun("p1-e", RunStatus::Completed);
    rig.addRun("p2-a", RunStatus::Completed, {}, kOtherProject);
    rig.addRun("p2-b", RunStatus::Failed, {}, kOtherProject);
    rig.addRun("busy", RunStatus::Running);
    rig.addRun("attempt", RunStatus::Completed, {}, kProject, std::string(kManifestVerificationManualHandoffWorkflowId));

    OutcomeBriefHistoryOptions options;
    options.newestPerProject = 2;
    options.pageSize = 2;   // project-1 is read in three pages, project-2 in two (the second one empty)
    const auto first = rig.reconciler.reconcileHistory(options);
    oc::check(first.success(), "the history pass succeeds");
    const auto& result = first.value();
    oc::check(result.examined == 7, "seven terminal outcome Runs (the running one and the attempt are not)");
    {
        std::vector<std::string> afterIds;
        std::vector<std::size_t> limits;
        for (const auto& request : rig.world.pageRequests) {
            if (request.project != kProject) continue;
            afterIds.push_back(request.afterId);
            limits.push_back(request.limit);
        }
        oc::check(afterIds == std::vector<std::string> {"", "p1-d", "p1-b"} && limits == std::vector<std::size_t> {2, 2, 2},
            "project-1 is read in stable pages: each strictly after the last Run of the one before");
        oc::check(result.pagesRead == 5 && rig.world.pageRequests.size() == 5, "five pages in all, never the whole history at once");
    }
    oc::check(rig.head("p1-e").has_value() && rig.head("p1-d").has_value(), "the newest two of project-1 always get a brief");
    oc::check(rig.head("p1-b").has_value() && rig.head("p1-b")->brief.state == OutcomeBriefState::AwaitingReview, "an old pending outcome gets one too");
    oc::check(!rig.head("p1-a").has_value() && !rig.head("p1-c").has_value(), "old settled outcomes are left alone");
    oc::check(rig.head("p2-a").has_value() && rig.head("p2-b").has_value(), "each project has its own newest window");
    oc::check(!rig.head("busy").has_value() && !rig.head("attempt").has_value(), "a running Run and an attempt Run get nothing");
    oc::check(result.appended == 5 && result.skipped == 2 && result.failed == 0 && !result.stopped, "the counts");

    const auto second = rig.reconciler.reconcileHistory(options);
    oc::check(second.success() && second.value().appended == 0 && second.value().unchanged == 5 && second.value().skipped == 2,
        "a second pass changes nothing");

    // An old Run that already has a brief keeps following its outcome.
    oc::check(rig.reconcile("p1-a").kind == Kind::Appended, "setup: an explicit request gives an old settled Run a brief");
    rig.world.entries["p1-a"].baselineFingerprint = std::string(64, 'd');
    const auto third = rig.reconciler.reconcileHistory(options);
    oc::check(third.success() && third.value().appended == 1, "an old Run that has a brief is followed when its outcome changes");
}

void testTheHistoryPassCountsFailuresAndStops()
{
    Rig rig;
    rig.addRun("ok-1", RunStatus::Completed);
    rig.addRun("bad", RunStatus::Completed, {{"cs-1", ChangeSetState::ReadyForReview}});
    rig.addRun("ok-2", RunStatus::Completed);
    rig.world.changeSets.erase("cs-1");
    rig.world.failing.insert("findChangeSet");
    auto result = rig.reconciler.reconcileHistory();
    oc::check(result.success() && result.value().failed == 1 && result.value().failedRunIds == std::vector<std::string> {"bad"},
        "a failing Run is counted and named");
    oc::check(result.value().appended == 2 && rig.head("ok-1").has_value() && rig.head("ok-2").has_value(), "...and the pass goes on");

    rig.world.failing.clear();
    OutcomeBriefHistoryOptions options;
    int polls = 0;
    options.pageSize = 1;
    // polled before each page and before each Run: page (no), Run (no) -> one Run examined; then the next page (yes)
    options.shouldStop = [&] { return ++polls > 2; };
    rig.world.runs["extra"] = rig.world.runs["ok-1"];
    rig.world.runs["extra"].id = "extra";
    rig.world.entries["extra"] = rig.world.entries["ok-1"];
    rig.world.entries["extra"].runId = "extra";
    result = rig.reconciler.reconcileHistory(options);
    oc::check(result.success() && result.value().stopped && result.value().examined == 1 && result.value().pagesRead == 1,
        "a stop request ends the pass between two pages");

    // A page that cannot be read fails ITS project, not the pass.
    rig.world.failing.insert("listOutcomeRunsPage");
    result = rig.reconciler.reconcileHistory();
    oc::check(result.success() && result.value().failed == 2
            && result.value().failedProjectIds == std::vector<std::string> {kProject, kOtherProject} && result.value().examined == 0,
        "a project whose page cannot be read is counted and named; the next project is still tried");
    rig.world.failing.clear();
    rig.world.failing.insert("listProjectIds");
    oc::check(!rig.reconciler.reconcileHistory().success(), "a project list that cannot be read fails the pass");
    rig.world.failing.clear();
    OutcomeBriefHistoryOptions invalid;
    invalid.pageSize = 0;
    oc::check(!rig.reconciler.reconcileHistory(invalid).success(), "a page size of zero is refused");
    invalid.pageSize = kMaxRunPageSize + 1;
    oc::check(!rig.reconciler.reconcileHistory(invalid).success(), "...and so is one above the ceiling");
}

} // namespace

int main()
{
    try {
        testOnlyOutcomeRunsGetABrief();
        testTheLifecycleOfACompletedRun();
        testAReturnToAnEarlierOutcomeIsANewRevision();
        testHumanRejectionAndTheOtherStates();
        testContinuationAndManualHandoff();
        testAStaleHeadIsReplaced();
        testFactsAreFailClosed();
        testTransitionsOnlyEnrichTheBrief();
        testApprovalEvidenceCarriesItsTime();
        testAMemberWithoutAChangeSetIsAFinding();
        testARepeatedMemberIsReadOnce();
        testKnownRootsAreForbiddenEvenWhenTheyDoNotLookAbsolute();
        testTheRunWorkspacePathNeverReachesABriefEither();
        testARunThatMovesOnWhileBeingReconciledIsSkipped();
        testTheRepositorySourcesAnswerFromTheRepositories();
        testTheProjectPathNeverReachesABrief();
        testTheReconcilerOnlyReads();
        testTheTriggerEntries();
        testRacingReconcilesAreOneRevision();
        testRunPagingOnTheInMemoryRepository();
        testTheSourcesListProjectsSortedWhateverOrderTheRepositoryAnswersIn();
        testALateReconcilerCannotOverwriteANewerHead();
        testAnOutcomeThatIsOverBeforeItIsStoredNeverBecomesTheHead();
        testAVerdictThatAgreesCostsNoSecondPass();
        testAReconcileThatBeginsAfterAVerdictIsNotHeldUpByIt();
        testTheBookkeepingFollowsTheRunsInFlightNotTheHistory();
        testAReconcilerThatMustNotWriteReadsAndWritesNothing();
        testTheFinalCheckAndTheWriteAreOneStep();
        testTheHistoryPass();
        testTheHistoryPassCountsFailuresAndStops();
        std::cout << "OutcomeBriefReconciler_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
