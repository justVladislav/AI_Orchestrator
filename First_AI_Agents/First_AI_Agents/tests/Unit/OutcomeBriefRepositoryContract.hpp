#pragma once

// PC-4B: ONE contract suite for IOutcomeBriefRepository. The SQLite adapter and the in-memory adapter run exactly the same
// scenarios, so a difference between them is a test failure, not a surprise. Anything that depends on SQLite (the Run and
// project rows, schema triggers, a lost head pointer, migrations, a second connection) lives in the SQLite tests instead.

#include "ai/application/planning/IOutcomeBriefRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace outcomebrief_contract {

using namespace ai::application;

struct Env {
    std::shared_ptr<void> keepAlive;          // owns the repository and whatever backs it
    IOutcomeBriefRepository* repository {nullptr};
    std::string projectId;                    // an existing project
    std::string otherProjectId;               // a second existing project
    // Makes the Run exist in `projectId`, Completed (SQLite: a `runs` row; in-memory: the entry its Run lookup answers from).
    // Idempotent by Run id.
    std::function<void(const std::string& runId, const std::string& projectId)> ensureRun;
    // The AUTHORITATIVE status of the Run changes behind the repository's back (SQLite: an UPDATE of `runs`).
    std::function<void(const std::string& runId, RunStatus status)> setRunStatus;
};
using EnvFactory = std::function<Env()>;

inline void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

inline std::string codeOf(const std::optional<ai::common::Error>& error)
{
    return error.has_value() ? error->code : std::string("<no error>");
}

// ---- briefs ---------------------------------------------------------------------------------------------------------------

inline OutcomeBriefProposalFacts proposal(std::string id, ChangeSetState state, bool humanRejected = false)
{
    OutcomeBriefProposalFacts value;
    value.changeSetId = std::move(id);
    value.state = state;
    value.humanRejected = humanRejected;
    return value;
}
inline OutcomeBriefProposalFacts ready(std::string id) { return proposal(std::move(id), ChangeSetState::ReadyForReview); }
inline OutcomeBriefProposalFacts applied(std::string id) { return proposal(std::move(id), ChangeSetState::Applied); }
inline OutcomeBriefProposalFacts rejected(std::string id)
{
    return proposal(std::move(id), ChangeSetState::ReadyForReview, true);
}

inline OutcomeBriefFacts terminalFacts(const std::string& runId, const std::string& projectId, RunStatus status,
    const std::vector<OutcomeBriefProposalFacts>& proposals,
    ManifestWorkLedgerDisposition disposition = ManifestWorkLedgerDisposition::ManifestReady)
{
    ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = projectId;
    entry.terminalStatus = status;
    entry.disposition = disposition;
    entry.baselineFingerprint = std::string(64, 'b');
    entry.terminalReason = "All tasks finished";
    for (const auto& item : proposals) {
        entry.proposalIds.push_back(item.changeSetId);
        ManifestWorkLedgerProposalLineage lineage;
        lineage.changeSetId = item.changeSetId;
        lineage.state = "ready_for_review";
        entry.proposalLineage.push_back(std::move(lineage));
    }
    ManifestWorkLedgerTaskDisposition task;
    task.taskId = "task-1";
    task.status = "completed";
    if (!proposals.empty()) task.changeSetId = proposals.front().changeSetId;
    entry.taskDispositions.push_back(std::move(task));
    entry.affectedRelativePaths = {"src/a.cpp"};
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;
    entry.verification = verification;
    entry.verificationSummary = "safety passed, build passed, acceptance passed";

    OutcomeBriefFacts facts;
    facts.runId = runId;
    facts.projectId = projectId;
    facts.workflowId = kExecutionManifestWorkflowId;
    facts.terminalStatus = status;
    facts.ledgerEntry = std::move(entry);
    facts.proposals = proposals;
    return facts;
}

inline OutcomeBriefFacts completedFacts(
    const std::string& runId, const std::string& projectId, const std::vector<OutcomeBriefProposalFacts>& proposals)
{
    return terminalFacts(runId, projectId, RunStatus::Completed, proposals);
}

// A derived (therefore storable) brief of a Completed Run. The Run is made to exist first.
inline OutcomeBrief makeBrief(const Env& env, const std::string& runId, const std::string& projectId,
    const std::vector<OutcomeBriefProposalFacts>& proposals)
{
    if (env.ensureRun) env.ensureRun(runId, projectId);
    const auto derived = deriveOutcomeBrief(completedFacts(runId, projectId, proposals));
    check(derived.success(), "setup: the brief must be derived");
    return derived.value();
}

// Three briefs of one Run with three different source states (awaiting review -> applied -> rejected) ...
inline OutcomeBrief briefA(const Env& env, const std::string& runId, const std::string& projectId)
{
    return makeBrief(env, runId, projectId, {ready("cs-1")});
}
inline OutcomeBrief briefB(const Env& env, const std::string& runId, const std::string& projectId)
{
    return makeBrief(env, runId, projectId, {applied("cs-1")});
}
inline OutcomeBrief briefC(const Env& env, const std::string& runId, const std::string& projectId)
{
    return makeBrief(env, runId, projectId, {rejected("cs-1")});
}
// ... and as many different ones as a test needs: `count` proposals awaiting review.
inline OutcomeBrief briefMany(const Env& env, const std::string& runId, const std::string& projectId, int count)
{
    std::vector<OutcomeBriefProposalFacts> proposals;
    for (int index = 1; index <= count; ++index) proposals.push_back(ready("cs-" + std::to_string(index)));
    return makeBrief(env, runId, projectId, proposals);
}

// Failed briefs with different source states (`count` applied proposals; 0: nothing produced). The Run's status is the caller's business.
inline OutcomeBrief failedBriefMany(const std::string& runId, const std::string& projectId, int count)
{
    std::vector<OutcomeBriefProposalFacts> proposals;
    for (int index = 1; index <= count; ++index) proposals.push_back(applied("cs-" + std::to_string(index)));
    const auto derived = deriveOutcomeBrief(terminalFacts(runId, projectId, RunStatus::Failed, proposals,
        count == 0 ? ManifestWorkLedgerDisposition::NotProduced : ManifestWorkLedgerDisposition::ManifestReady));
    check(derived.success(), "setup: a Failed brief");
    return derived.value();
}

inline bool sameBrief(const OutcomeBrief& left, const OutcomeBrief& right)
{
    return left.schemaVersion == right.schemaVersion && left.policyId == right.policyId && left.runId == right.runId
        && left.projectId == right.projectId && left.workflowId == right.workflowId
        && left.terminalStatus == right.terminalStatus && left.state == right.state && left.reasons == right.reasons
        && left.counts == right.counts && left.memberDecisions == right.memberDecisions
        && left.recoveryEligible == right.recoveryEligible && left.requiresInspection == right.requiresInspection
        && left.continuationSourceRunId == right.continuationSourceRunId
        && left.continuationChildRunId == right.continuationChildRunId && left.manualHandoffId == right.manualHandoffId
        && left.sourceStateJson == right.sourceStateJson && left.sourceStateFingerprint == right.sourceStateFingerprint
        && left.renderedText == right.renderedText && left.contentFingerprint == right.contentFingerprint;
}

inline bool sameRevision(const OutcomeBriefRevision& left, const OutcomeBriefRevision& right)
{
    return left.id == right.id && left.ordinal == right.ordinal && left.createdAt == right.createdAt
        && sameBrief(left.brief, right.brief);
}

// ---- helpers over the port ---------------------------------------------------------------------------------------------

inline OutcomeBriefAppendResult mustAppend(IOutcomeBriefRepository& repository, OutcomeBrief brief,
    OutcomeBriefExpectedHead expected, const std::string& what)
{
    auto result = repository.append(std::move(brief), std::move(expected));
    check(result.success(), what + ": the append must succeed but failed with " + codeOf(result.error()));
    return result.value();
}

inline void requireRefused(IOutcomeBriefRepository& repository, OutcomeBrief brief, OutcomeBriefExpectedHead expected,
    const std::string& code, const std::string& what)
{
    const auto result = repository.append(std::move(brief), std::move(expected));
    check(!result.success(), what + ": the append must be refused");
    check(codeOf(result.error()) == code, what + ": expected " + code + " but got " + codeOf(result.error()));
}

inline std::size_t revisionCount(IOutcomeBriefRepository& repository, const std::string& runId)
{
    const auto listed = repository.listRevisions(runId, kOutcomeBriefMaxListLimit);
    check(listed.success(), "listRevisions must succeed");
    return listed.value().size();
}

inline std::optional<OutcomeBriefRevision> headOf(IOutcomeBriefRepository& repository, const std::string& runId)
{
    const auto head = repository.findHead(runId);
    check(head.success(), "findHead must succeed");
    return head.value();
}

template <typename T>
inline void requireInvalidData(const ai::common::Result<T>& result, const std::string& what)
{
    check(!result.success() && codeOf(result.error()) == "persistence.invalid_data",
        what + ": expected persistence.invalid_data but got " + (result.success() ? std::string("a value") : codeOf(result.error())));
}

inline void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(4)); }

// Runs `work(index)` on `count` threads that are released together.
inline void runTogether(int count, const std::function<void(int)>& work)
{
    std::atomic<bool> go {false};
    std::atomic<int> failures {0};
    std::string firstFailure;
    std::mutex failureMutex;
    std::vector<std::thread> threads;
    for (int index = 0; index < count; ++index) {
        threads.emplace_back([&, index] {
            while (!go.load()) std::this_thread::yield();
            try {
                work(index);
            } catch (const std::exception& exception) {
                std::lock_guard lock(failureMutex);
                if (failures.fetch_add(1) == 0) firstFailure = exception.what();
            }
        });
    }
    go.store(true);
    for (auto& thread : threads) thread.join();
    check(failures.load() == 0, "a worker thread failed: " + firstFailure);
}

// ---- scenarios -----------------------------------------------------------------------------------------------------------

inline void testFirstAppendAndReadBack(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    check(!headOf(repository, "run-1").has_value(), "A Run without a brief has no head");
    check(revisionCount(repository, "run-1") == 0, "...and no revisions");
    check(!repository.findRevision("no-such-revision").value().has_value(), "An unknown revision id finds nothing");

    const OutcomeBrief brief = briefA(env, "run-1", env.projectId);
    const auto first = mustAppend(repository, brief, OutcomeBriefExpectedHead::none(), "first append");
    check(first.appended, "The first brief is a new revision");
    check(first.head.ordinal == 1 && !first.head.id.empty(), "...the first of the Run");
    check(sameBrief(first.head.brief, brief), "...which holds the brief exactly as it was given");

    const auto head = headOf(repository, "run-1");
    check(head.has_value() && sameRevision(*head, first.head), "findHead returns the revision that was appended");
    const auto byId = repository.findRevision(first.head.id);
    check(byId.success() && byId.value().has_value() && sameRevision(*byId.value(), first.head),
        "findRevision returns the very same revision");
    const auto heads = repository.listHeads(env.projectId, 10);
    check(heads.success() && heads.value().size() == 1 && sameRevision(heads.value().front(), first.head),
        "listHeads lists it");
}

inline void testRepeatingTheHeadWritesNothing(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const auto first = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::any(), "first");
    pause();
    const auto again = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::any(), "again");
    check(!again.appended, "The same source state as the head is not a new revision");
    check(sameRevision(again.head, first.head), "...and the answer is the head as it was stored");
    const auto withHead = mustAppend(repository, briefA(env, "run-1", env.projectId),
        OutcomeBriefExpectedHead::revision(first.head.id), "again with the expected head");
    check(!withHead.appended && sameRevision(withHead.head, first.head), "...also when the expected head is named");
    check(revisionCount(repository, "run-1") == 1, "Repeating a brief never adds a revision");
}

// A -> B -> A: the third brief equals an OLDER revision, but not the head, so it is a real change and a new revision.
inline void testReturningToAnEarlierStateIsANewRevision(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const auto a1 = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "A");
    pause();
    const auto b = mustAppend(repository, briefB(env, "run-1", env.projectId),
        OutcomeBriefExpectedHead::revision(a1.head.id), "B");
    pause();
    const auto a2 = mustAppend(repository, briefA(env, "run-1", env.projectId),
        OutcomeBriefExpectedHead::revision(b.head.id), "A again");
    check(a1.appended && b.appended && a2.appended, "A, B and A again are three revisions");
    check(a1.head.ordinal == 1 && b.head.ordinal == 2 && a2.head.ordinal == 3, "...numbered 1, 2, 3");
    check(a2.head.id != a1.head.id, "...the return to A is not the first A");
    check(a2.head.brief.sourceStateFingerprint == a1.head.brief.sourceStateFingerprint
            && a2.head.brief.sourceStateFingerprint != b.head.brief.sourceStateFingerprint,
        "...with the same source state as the first A");
    check(headOf(repository, "run-1")->id == a2.head.id, "The head is the third revision");
    const auto repeated = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::any(), "A once more");
    check(!repeated.appended && repeated.head.id == a2.head.id, "A fourth A is a repeat of the head");

    const auto all = repository.listRevisions("run-1", 10);
    check(all.success() && all.value().size() == 3, "The history holds the three revisions");
    check(all.value()[0].id == a1.head.id && all.value()[1].id == b.head.id && all.value()[2].id == a2.head.id,
        "...oldest first");
    check(sameRevision(all.value()[0], a1.head), "...and the first one was not changed by the later ones");
    const auto newest = repository.listRevisions("run-1", 2);
    check(newest.success() && newest.value().size() == 2 && newest.value()[0].id == b.head.id
            && newest.value()[1].id == a2.head.id,
        "A limit keeps the NEWEST revisions, still oldest first");
    check(repository.listRevisions("run-1", 1).value().front().id == a2.head.id, "A limit of one is the head");
    check(repository.listHeads(env.projectId, 10).value().size() == 1, "A Run has one head however long its history is");
}

inline void testTheExpectedHeadIsACompareAndSet(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    requireRefused(repository, briefA(env, "run-1", env.projectId),
        OutcomeBriefExpectedHead::revision("some-revision"), "outcome_brief.head_changed",
        "a revision was expected but the Run has no brief");
    check(revisionCount(repository, "run-1") == 0, "...and nothing was written");

    const auto a = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "A");
    requireRefused(repository, briefB(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(),
        "outcome_brief.head_changed", "none was expected but the Run has a head");
    requireRefused(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(),
        "outcome_brief.head_changed", "none was expected, even for the very same brief");
    requireRefused(repository, briefB(env, "run-1", env.projectId), OutcomeBriefExpectedHead::revision("not-the-head"),
        "outcome_brief.head_changed", "another revision was expected");
    check(revisionCount(repository, "run-1") == 1 && headOf(repository, "run-1")->id == a.head.id,
        "A refused compare-and-set leaves the history alone");

    const auto b = mustAppend(repository, briefB(env, "run-1", env.projectId),
        OutcomeBriefExpectedHead::revision(a.head.id), "B on the expected head");
    check(b.appended && b.head.ordinal == 2, "The expected head lets the write through");
    requireRefused(repository, briefC(env, "run-1", env.projectId), OutcomeBriefExpectedHead::revision(a.head.id),
        "outcome_brief.head_changed", "an OLDER revision is not the head");
    mustAppend(repository, briefC(env, "run-1", env.projectId), OutcomeBriefExpectedHead::any(), "C without a check");
    check(revisionCount(repository, "run-1") == 3, "Any skips the check");
}

inline void testMalformedExpectedHeadsAreRefused(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    requireRefused(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::revision(""),
        "outcome_brief.invalid_request", "an expected revision without an id");
    OutcomeBriefExpectedHead none = OutcomeBriefExpectedHead::none();
    none.revisionId = "stray";
    requireRefused(repository, briefA(env, "run-1", env.projectId), none, "outcome_brief.invalid_request",
        "an id next to none");
    OutcomeBriefExpectedHead any = OutcomeBriefExpectedHead::any();
    any.revisionId = "stray";
    requireRefused(repository, briefA(env, "run-1", env.projectId), any, "outcome_brief.invalid_request",
        "an id next to any");
    OutcomeBriefExpectedHead unknown;
    unknown.kind = static_cast<OutcomeBriefExpectedHead::Kind>(99);
    requireRefused(repository, briefA(env, "run-1", env.projectId), unknown, "outcome_brief.invalid_request",
        "an unknown kind");
    check(revisionCount(repository, "run-1") == 0, "Nothing was written by any of them");
}

// The gate is mandatory: a brief the resolver could not have produced never reaches storage, not even as a "repeat" of the head.
inline void testTheWriteGateRefusesEveryEditedBrief(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const OutcomeBrief good = briefA(env, "run-1", env.projectId);
    const auto edit = [&](const std::function<void(OutcomeBrief&)>& change) {
        OutcomeBrief copy = good;
        change(copy);
        return copy;
    };
    const auto refuse = [&](const OutcomeBrief& brief, const std::string& what) {
        requireRefused(repository, brief, OutcomeBriefExpectedHead::any(), "outcome_brief.invalid_brief", what);
    };

    refuse(OutcomeBrief {}, "an empty brief");
    refuse(edit([](OutcomeBrief& b) { b.renderedText += " "; }), "a rendering that does not match its fingerprint");
    refuse(edit([](OutcomeBrief& b) { b.renderedText += "x"; b.contentFingerprint = ai::application::sha256Hex(b.renderedText); }),
        "a re-sealed rendering that is not the canonical one");
    refuse(edit([](OutcomeBrief& b) { b.state = OutcomeBriefState::ResolvedApplied; }), "a state the content does not lead to");
    refuse(edit([](OutcomeBrief& b) { b.counts.applied += 1; }), "counts that are not the arithmetic of the rows");
    refuse(edit([](OutcomeBrief& b) { b.reasons = {OutcomeBriefReason::ApplyFailed}; }), "a reason without a proof");
    refuse(edit([](OutcomeBrief& b) { b.sourceStateFingerprint = std::string(64, '0'); }), "a source state fingerprint that is not its hash");
    refuse(edit([](OutcomeBrief& b) { b.runId = "another-run"; }), "a Run id that is not the source state's");
    refuse(edit([](OutcomeBrief& b) { b.memberDecisions.clear(); }), "member decisions that are not the rows");
    check(!headOf(repository, "run-1").has_value() && revisionCount(repository, "run-1") == 0
            && !headOf(repository, "another-run").has_value(),
        "None of the edited briefs was stored");

    // The same edits against a Run that already has a head: still refused, and the head is untouched. The edited
    // rendering has the head's own source state, so a comparison without the gate would call it a repeat.
    const auto head = mustAppend(repository, good, OutcomeBriefExpectedHead::none(), "the good brief");
    refuse(edit([](OutcomeBrief& b) { b.renderedText += "x"; b.contentFingerprint = ai::application::sha256Hex(b.renderedText); }),
        "an edited copy of the head");
    check(revisionCount(repository, "run-1") == 1 && sameRevision(*headOf(repository, "run-1"), head.head),
        "...the head is still the good brief");
}

inline void testRunsAndProjectsAreIndependent(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const auto oldest = mustAppend(repository, briefA(env, "run-old", env.projectId), OutcomeBriefExpectedHead::none(), "old");
    pause();
    const auto middle = mustAppend(repository, briefA(env, "run-mid", env.projectId), OutcomeBriefExpectedHead::none(), "mid");
    pause();
    const auto other = mustAppend(repository, briefA(env, "run-other", env.otherProjectId), OutcomeBriefExpectedHead::none(), "other");
    pause();
    const auto newest = mustAppend(repository, briefA(env, "run-new", env.projectId), OutcomeBriefExpectedHead::none(), "new");

    auto heads = repository.listHeads(env.projectId, 10);
    check(heads.success() && heads.value().size() == 3, "A project lists its own Runs only");
    check(heads.value()[0].id == newest.head.id && heads.value()[1].id == middle.head.id && heads.value()[2].id == oldest.head.id,
        "...newest head first");
    const auto limited = repository.listHeads(env.projectId, 2);
    check(limited.success() && limited.value().size() == 2 && limited.value()[0].id == newest.head.id
            && limited.value()[1].id == middle.head.id,
        "...a limit keeps the newest");
    const auto others = repository.listHeads(env.otherProjectId, 10);
    check(others.success() && others.value().size() == 1 && others.value()[0].id == other.head.id,
        "The other project lists its own Run");
    check(repository.listHeads("no-such-project", 10).value().empty(), "An unknown project lists nothing");

    // A new revision makes the Run's head the newest of its project, and the older revision is not a head any more.
    pause();
    const auto moved = mustAppend(repository, briefB(env, "run-old", env.projectId), OutcomeBriefExpectedHead::revision(oldest.head.id), "old moves");
    heads = repository.listHeads(env.projectId, 10);
    check(heads.value().size() == 3 && heads.value()[0].id == moved.head.id && heads.value()[0].ordinal == 2,
        "A Run that changed is listed with its new head, first");
    check(std::none_of(heads.value().begin(), heads.value().end(),
              [&](const OutcomeBriefRevision& revision) { return revision.id == oldest.head.id; }),
        "...and its old revision is no head");
    check(headOf(repository, "run-mid")->id == middle.head.id, "The other Runs keep their heads");
    check(revisionCount(repository, "run-new") == 1 && revisionCount(repository, "run-old") == 2,
        "Each Run has its own numbering");
}

// The WRITE authority: the Run must exist, be the brief's project's and still have the status the brief describes.
inline void testTheRunMustExistAndMatch(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const auto ghost = deriveOutcomeBrief(completedFacts("ghost-run", env.projectId, {ready("cs-1")}));
    check(ghost.success(), "setup: a brief of a Run nobody created");
    requireRefused(repository, ghost.value(), OutcomeBriefExpectedHead::any(), "outcome_brief.run_not_found", "an unknown Run");

    env.ensureRun("run-1", env.projectId);
    const auto foreign = deriveOutcomeBrief(completedFacts("run-1", env.otherProjectId, {ready("cs-1")}));
    check(foreign.success(), "setup: a brief of the Run with another project");
    requireRefused(repository, foreign.value(), OutcomeBriefExpectedHead::any(), "outcome_brief.project_mismatch",
        "the first brief of a Run names another project than the Run's");
    const auto failed = deriveOutcomeBrief(
        terminalFacts("run-1", env.projectId, RunStatus::Failed, {}, ManifestWorkLedgerDisposition::NotProduced));
    check(failed.success(), "setup: a Failed brief");
    requireRefused(repository, failed.value(), OutcomeBriefExpectedHead::any(), "outcome_brief.run_status_mismatch",
        "a brief that describes another status than the Run has");
    check(revisionCount(repository, "run-1") == 0 && !headOf(repository, "ghost-run").has_value(), "None of them wrote anything");

    const OutcomeBrief good = briefA(env, "run-1", env.projectId);
    env.setRunStatus("run-1", RunStatus::Running);
    requireRefused(repository, good, OutcomeBriefExpectedHead::any(), "outcome_brief.run_status_mismatch", "a Run that runs again");
    env.setRunStatus("run-1", RunStatus::Completed);
    check(repository.append(good, OutcomeBriefExpectedHead::any()).success(), "...and with the status back the brief is stored");
}

// The READ authority, status: a Run that left its terminal status after the brief was written has no readable brief.
inline void testAStaleHeadIsNeverRead(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const auto stored = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "run-1");
    const auto healthy = mustAppend(repository, briefA(env, "run-2", env.projectId), OutcomeBriefExpectedHead::none(), "run-2");

    env.setRunStatus("run-1", RunStatus::Running);   // the stored brief says Completed
    requireInvalidData(repository.findHead("run-1"), "findHead of a stale head");
    requireInvalidData(repository.findRevision(stored.head.id), "findRevision of a stale head");
    requireInvalidData(repository.listRevisions("run-1", 10), "listRevisions that contains a stale head");
    requireInvalidData(repository.listHeads(env.projectId, 10), "listHeads that contains a stale head");
    const auto other = repository.findHead("run-2");
    check(other.success() && other.value().has_value() && sameRevision(*other.value(), healthy.head),
        "A Run that did not move is still readable");
    check(repository.listHeads(env.otherProjectId, 10).success(), "...and so is another project's list");
    requireRefused(repository, briefB(env, "run-1", env.projectId), OutcomeBriefExpectedHead::any(),
        "outcome_brief.run_status_mismatch", "a brief of the old status cannot be written either");

    env.setRunStatus("run-1", RunStatus::Completed);
    const auto again = headOf(repository, "run-1");
    check(again.has_value() && sameRevision(*again, stored.head), "With the status back the very same brief is readable again");
    check(repository.listHeads(env.projectId, 10).value().size() == 2, "...and so is the list");
}

// Only the CURRENT revision describes the Run's status now: an older one is history, and a stale head never blocks the brief that
// is true now (otherwise a Run that was Interrupted and then Failed could never get its new brief).
inline void testAStaleHeadCanBeReplacedAndHistoryIsNotCompared(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    env.ensureRun("run-1", env.projectId);
    env.setRunStatus("run-1", RunStatus::Interrupted);
    const auto interrupted = deriveOutcomeBrief(
        terminalFacts("run-1", env.projectId, RunStatus::Interrupted, {}, ManifestWorkLedgerDisposition::NotProduced));
    check(interrupted.success(), "setup: an Interrupted brief");
    const auto first = mustAppend(repository, interrupted.value(), OutcomeBriefExpectedHead::none(), "Interrupted");

    env.setRunStatus("run-1", RunStatus::Failed);   // the Run went on and ended another way
    requireInvalidData(repository.findHead("run-1"), "the Interrupted head of a Failed Run");
    const auto failed = deriveOutcomeBrief(
        terminalFacts("run-1", env.projectId, RunStatus::Failed, {}, ManifestWorkLedgerDisposition::NotProduced));
    check(failed.success(), "setup: a Failed brief");
    const auto version = repository.findHeadVersionForAppend("run-1");
    check(version.success() && version.value().has_value() && version.value()->revisionId == first.head.id && version.value()->ordinal == 1,
        "The writer still sees which revision the stale head is");
    requireRefused(repository, failed.value(), OutcomeBriefExpectedHead::revision("not-the-stale-head"), "outcome_brief.head_changed",
        "a wrong expectation of the stale head");
    const auto second = mustAppend(
        repository, failed.value(), OutcomeBriefExpectedHead::revision(version.value()->revisionId), "Failed replaces the stale head");
    check(second.appended && second.head.ordinal == 2, "The brief that is true now becomes the head");

    const auto head = headOf(repository, "run-1");
    check(head.has_value() && sameRevision(*head, second.head), "...and is read");
    const auto history = repository.listRevisions("run-1", 10);
    check(history.success() && history.value().size() == 2 && history.value()[0].brief.terminalStatus == RunStatus::Interrupted,
        "The older revision describes the Run as it was and is still listed");
    const auto old = repository.findRevision(first.head.id);
    check(old.success() && old.value().has_value() && sameRevision(*old.value(), first.head), "...and found by id");
    check(repository.listHeads(env.projectId, 10).success(), "The list reads again");
}

// The WRITER's read of the head: which revision it is - never what it says - and for a stale head too.
inline void testTheWriterReadOfTheHead(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    // "No head" must not hide "no Run": a Run that does not exist is invalid data, a Run that exists but has no brief is just empty.
    requireInvalidData(repository.findHeadVersionForAppend("ghost-run"), "the writer's read for a Run that does not exist");
    env.ensureRun("run-1", env.projectId);
    const auto none = repository.findHeadVersionForAppend("run-1");
    check(none.success() && !none.value().has_value(), "A Run without a brief has no head version");

    const auto a = mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "A");
    auto version = repository.findHeadVersionForAppend("run-1");
    check(version.success() && version.value().has_value() && version.value()->revisionId == a.head.id && version.value()->ordinal == 1,
        "The version of the first head");
    const auto b = mustAppend(repository, briefB(env, "run-1", env.projectId), OutcomeBriefExpectedHead::revision(version.value()->revisionId), "B");
    version = repository.findHeadVersionForAppend("run-1");
    check(version.success() && version.value().has_value() && version.value()->revisionId == b.head.id && version.value()->ordinal == 2,
        "...and of the second one");

    env.setRunStatus("run-1", RunStatus::Running);   // the head is stale now
    requireInvalidData(repository.findHead("run-1"), "a consumer cannot read the stale head");
    version = repository.findHeadVersionForAppend("run-1");
    check(version.success() && version.value().has_value() && version.value()->revisionId == b.head.id && version.value()->ordinal == 2,
        "...but the writer still sees which revision it is");
}

// Two writers that saw the same STALE head: one winner, the other hears head_changed. This is what `Any` cannot give.
inline void testWritersOfOneStaleHeadHaveOneWinner(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    env.ensureRun("run-1", env.projectId);
    env.setRunStatus("run-1", RunStatus::Interrupted);
    const auto interrupted = deriveOutcomeBrief(
        terminalFacts("run-1", env.projectId, RunStatus::Interrupted, {}, ManifestWorkLedgerDisposition::NotProduced));
    check(interrupted.success(), "setup: an Interrupted brief");
    mustAppend(repository, interrupted.value(), OutcomeBriefExpectedHead::none(), "Interrupted");
    env.setRunStatus("run-1", RunStatus::Failed);

    constexpr int kWriters = 6;
    std::vector<OutcomeBrief> briefs;
    std::vector<std::string> seen;
    for (int index = 0; index < kWriters; ++index) {
        briefs.push_back(failedBriefMany("run-1", env.projectId, index));   // six different calculations of the same Run
        const auto version = repository.findHeadVersionForAppend("run-1");
        check(version.success() && version.value().has_value(), "every writer reads the stale head");
        seen.push_back(version.value()->revisionId);
    }
    check(std::set<std::string>(seen.begin(), seen.end()).size() == 1, "...and all of them see the same one");

    std::atomic<int> won {0};
    std::atomic<int> lost {0};
    runTogether(kWriters, [&](int index) {
        const auto result = repository.append(briefs[static_cast<std::size_t>(index)],
            OutcomeBriefExpectedHead::revision(seen[static_cast<std::size_t>(index)]));
        if (result.success()) {
            check(result.value().appended && result.value().head.ordinal == 2, "the winner writes the second revision");
            ++won;
        } else {
            check(codeOf(result.error()) == "outcome_brief.head_changed", "a loser hears head_changed, got " + codeOf(result.error()));
            ++lost;
        }
    });
    check(won.load() == 1 && lost.load() == kWriters - 1, "Exactly one of the writers replaces the stale head");
    check(revisionCount(repository, "run-1") == 2, "...and only its revision was added");
    const auto head = headOf(repository, "run-1");
    check(head.has_value() && head->ordinal == 2, "The winner's brief is true now, so the head reads again");
}

// `limit` is a bounded page: the authority is checked for the heads of the returned window only.
inline void testListHeadsChecksOnlyTheReturnedWindow(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    mustAppend(repository, briefA(env, "run-old", env.projectId), OutcomeBriefExpectedHead::none(), "old");
    pause();
    const auto fresh = mustAppend(repository, briefA(env, "run-new", env.projectId), OutcomeBriefExpectedHead::none(), "new");

    env.setRunStatus("run-old", RunStatus::Running);   // the OLDER head is stale
    const auto page = repository.listHeads(env.projectId, 1);
    check(page.success() && page.value().size() == 1 && sameRevision(page.value()[0], fresh.head),
        "A stale head outside the returned window does not matter");
    requireInvalidData(repository.listHeads(env.projectId, 2), "a stale head inside the window");

    // The stale head is the NEWEST one: it is in every window.
    env.setRunStatus("run-old", RunStatus::Completed);
    pause();
    mustAppend(repository, briefB(env, "run-old", env.projectId), OutcomeBriefExpectedHead::any(), "old moves on");
    env.setRunStatus("run-old", RunStatus::Running);
    requireInvalidData(repository.listHeads(env.projectId, 1), "a stale newest head in a window of one");
    requireInvalidData(repository.listHeads(env.projectId, 2), "...and in a window of two");
}

inline void testAProjectCannotChangeUnderARun(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    mustAppend(repository, briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "A");
    // The brief is valid for the other project (its Run is made to exist there), but this Run belongs to the first one.
    OutcomeBriefFacts facts = completedFacts("run-1", env.otherProjectId, {applied("cs-1")});
    const auto derived = deriveOutcomeBrief(facts);
    check(derived.success(), "setup: a brief of the Run with another project");
    requireRefused(repository, derived.value(), OutcomeBriefExpectedHead::any(), "outcome_brief.project_mismatch",
        "a Run cannot move to another project");
    check(revisionCount(repository, "run-1") == 1, "...and nothing was written");
}

inline void testListLimitsAreBounded(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    const auto refused = [](const auto& result, const std::string& what) {
        check(!result.success() && codeOf(result.error()) == "outcome_brief.invalid_request",
            what + ": the limit must be refused");
    };
    refused(repository.listRevisions("run-1", 0), "revisions, zero");
    refused(repository.listRevisions("run-1", kOutcomeBriefMaxListLimit + 1), "revisions, above the ceiling");
    refused(repository.listHeads(env.projectId, 0), "heads, zero");
    refused(repository.listHeads(env.projectId, kOutcomeBriefMaxListLimit + 1), "heads, above the ceiling");
    check(repository.listRevisions("run-1", kOutcomeBriefMaxListLimit).success()
            && repository.listHeads(env.projectId, kOutcomeBriefMaxListLimit).success(),
        "The ceiling itself is allowed");
}

// Several writers expect the same head (none): exactly one wins, the others learn that the head changed.
inline void testOnlyOneWriterWinsTheSameExpectedHead(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    constexpr int kWriters = 8;
    std::vector<OutcomeBrief> briefs;
    for (int index = 1; index <= kWriters; ++index) briefs.push_back(briefMany(env, "run-1", env.projectId, index));

    std::atomic<int> won {0};
    std::atomic<int> lost {0};
    runTogether(kWriters, [&](int index) {
        const auto result = repository.append(briefs[static_cast<std::size_t>(index)], OutcomeBriefExpectedHead::none());
        if (result.success()) {
            check(result.value().appended && result.value().head.ordinal == 1, "the winner writes the first revision");
            ++won;
        } else {
            check(codeOf(result.error()) == "outcome_brief.head_changed", "a loser must hear head_changed, got " + codeOf(result.error()));
            ++lost;
        }
    });
    check(won.load() == 1 && lost.load() == kWriters - 1, "Exactly one of the writers wins");
    check(revisionCount(repository, "run-1") == 1, "...and only its revision exists");
}

// Writers that read the head, write against it and retry when it moved: every brief lands exactly once, in a gapless order.
inline void testOptimisticWritersConverge(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    constexpr int kWriters = 6;
    std::vector<OutcomeBrief> briefs;
    for (int index = 1; index <= kWriters; ++index) briefs.push_back(briefMany(env, "run-1", env.projectId, index));

    runTogether(kWriters, [&](int index) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto head = repository.findHead("run-1");
            check(head.success(), "findHead must succeed");
            const auto expected = head.value().has_value() ? OutcomeBriefExpectedHead::revision(head.value()->id)
                                                           : OutcomeBriefExpectedHead::none();
            const auto result = repository.append(briefs[static_cast<std::size_t>(index)], expected);
            if (result.success()) {
                check(result.value().appended, "a unique brief is always a new revision");
                return;
            }
            check(codeOf(result.error()) == "outcome_brief.head_changed", "only head_changed may be retried, got " + codeOf(result.error()));
        }
        throw std::runtime_error("a writer never got through");
    });

    const auto history = repository.listRevisions("run-1", kOutcomeBriefMaxListLimit);
    check(history.success() && history.value().size() == static_cast<std::size_t>(kWriters),
        "Every writer's brief was stored once");
    std::set<std::string> sources;
    for (std::size_t index = 0; index < history.value().size(); ++index) {
        check(history.value()[index].ordinal == index + 1, "The ordinals are 1..n without gaps");
        sources.insert(history.value()[index].brief.sourceStateFingerprint);
    }
    check(sources.size() == static_cast<std::size_t>(kWriters), "...each with its own source state");
    check(headOf(repository, "run-1")->ordinal == static_cast<std::uint64_t>(kWriters), "The head is the last of them");
}

// Writers that all bring the same brief without an expectation: one revision, the rest are repeats of the head.
inline void testRacingRepeatsAreOneRevision(const EnvFactory& factory)
{
    const Env env = factory();
    auto& repository = *env.repository;
    constexpr int kWriters = 8;
    const OutcomeBrief brief = briefA(env, "run-1", env.projectId);
    std::atomic<int> appended {0};
    runTogether(kWriters, [&](int) {
        const auto result = repository.append(brief, OutcomeBriefExpectedHead::any());
        check(result.success(), "a repeat must succeed, got " + codeOf(result.error()));
        if (result.value().appended) ++appended;
        check(result.value().head.ordinal == 1, "...and answer with the one revision");
    });
    check(appended.load() == 1, "Exactly one of them appended");
    check(revisionCount(repository, "run-1") == 1, "...and the history has one revision");
}

inline void runOutcomeBriefRepositoryContract(const EnvFactory& factory)
{
    testFirstAppendAndReadBack(factory);
    testRepeatingTheHeadWritesNothing(factory);
    testReturningToAnEarlierStateIsANewRevision(factory);
    testTheExpectedHeadIsACompareAndSet(factory);
    testMalformedExpectedHeadsAreRefused(factory);
    testTheWriteGateRefusesEveryEditedBrief(factory);
    testRunsAndProjectsAreIndependent(factory);
    testAProjectCannotChangeUnderARun(factory);
    testTheRunMustExistAndMatch(factory);
    testAStaleHeadIsNeverRead(factory);
    testAStaleHeadCanBeReplacedAndHistoryIsNotCompared(factory);
    testTheWriterReadOfTheHead(factory);
    testWritersOfOneStaleHeadHaveOneWinner(factory);
    testListHeadsChecksOnlyTheReturnedWindow(factory);
    testListLimitsAreBounded(factory);
    testOnlyOneWriterWinsTheSameExpectedHead(factory);
    testOptimisticWritersConverge(factory);
    testRacingRepeatsAreOneRevision(factory);
}

} // namespace outcomebrief_contract
