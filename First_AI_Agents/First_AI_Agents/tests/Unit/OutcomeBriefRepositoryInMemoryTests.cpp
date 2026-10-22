#include "OutcomeBriefRepositoryContract.hpp"

#include "ai/application/InMemoryOutcomeBriefRepository.hpp"

#include <iostream>
#include <limits>
#include <map>
#include <mutex>

namespace {

namespace oc = outcomebrief_contract;
using namespace ai::application;

// The Runs the in-memory adapter's lookup answers from: the stand-in of the `runs` table.
struct FakeRuns {
    std::mutex mutex;
    std::map<std::string, OutcomeBriefRunOwner> runs;

    std::optional<OutcomeBriefRunOwner> find(const std::string& runId)
    {
        std::lock_guard lock(mutex);
        const auto found = runs.find(runId);
        if (found == runs.end()) return std::nullopt;
        return found->second;
    }
};

struct InMemoryState {
    FakeRuns runs;
    InMemoryOutcomeBriefRepository repository {[this](const RunId& runId) { return runs.find(runId); }};
};

oc::Env inMemoryEnv()
{
    auto state = std::make_shared<InMemoryState>();
    oc::Env env;
    env.keepAlive = state;
    env.repository = &state->repository;
    env.projectId = "project-1";
    env.otherProjectId = "project-2";
    env.ensureRun = [state](const std::string& runId, const std::string& projectId) {
        std::lock_guard lock(state->runs.mutex);
        state->runs.runs.try_emplace(runId, OutcomeBriefRunOwner {projectId, RunStatus::Completed});
    };
    env.setRunStatus = [state](const std::string& runId, RunStatus status) {
        std::lock_guard lock(state->runs.mutex);
        oc::check(state->runs.runs.count(runId) == 1, "setup: the Run must exist before its status changes");
        state->runs.runs[runId].status = status;
    };
    return env;
}

void testTheInMemoryRepositoryHonoursTheContract()
{
    oc::runOutcomeBriefRepositoryContract([] { return inMemoryEnv(); });
}

// The values handed out are copies: changing one never changes what the repository holds.
void testTheRepositoryHandsOutCopies()
{
    const auto env = inMemoryEnv();
    auto& repository = *env.repository;
    const auto first = oc::mustAppend(repository, oc::briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "first");
    auto answer = first;
    answer.head.brief.renderedText = "tampered";
    auto head = oc::headOf(repository, "run-1");
    head->brief.state = OutcomeBriefState::ResolvedApplied;
    auto listed = repository.listRevisions("run-1", 10).value();
    listed.front().brief.sourceStateJson = "tampered";
    const auto again = oc::headOf(repository, "run-1");
    oc::check(oc::sameRevision(*again, first.head), "A copy taken from the repository cannot rewrite it");
}

void testOutcomeProposalDecisionsRoundTripThroughText()
{
    for (int index = 0; index <= static_cast<int>(OutcomeProposalDecision::Inconsistent); ++index) {
        const auto decision = static_cast<OutcomeProposalDecision>(index);
        const auto parsed = outcomeProposalDecisionFromString(toString(decision));
        oc::check(parsed.has_value() && *parsed == decision, std::string("The decision spelling of ") + toString(decision) + " reads back");
    }
    oc::check(!outcomeProposalDecisionFromString("").has_value() && !outcomeProposalDecisionFromString("Applied").has_value()
            && !outcomeProposalDecisionFromString("unknown").has_value() && !outcomeProposalDecisionFromString("applied ").has_value(),
        "Only the exact spellings are decisions");
}

// The shared compare-and-set / idempotency rule, on its own: both adapters call this function.
void testThePlanOfAnAppend()
{
    const auto env = inMemoryEnv();
    const OutcomeBrief a = oc::briefA(env, "run-1", env.projectId);
    const OutcomeBrief b = oc::briefB(env, "run-1", env.projectId);
    const auto revisionOf = [](const OutcomeBrief& brief, std::uint64_t ordinal) {
        return makeOutcomeBriefRevision(brief, ordinal);
    };
    const OutcomeBriefRevision headA = revisionOf(a, 4);

    const auto plan = planOutcomeBriefAppend(a, OutcomeBriefExpectedHead::any(), std::nullopt);
    oc::check(plan.success() && plan.value().appendRevision && plan.value().ordinal == 1, "No head: the first ordinal");
    const auto onHead = planOutcomeBriefAppend(b, OutcomeBriefExpectedHead::any(), headA);
    oc::check(onHead.success() && onHead.value().appendRevision && onHead.value().ordinal == 5, "A change continues the numbering");
    const auto repeat = planOutcomeBriefAppend(a, OutcomeBriefExpectedHead::any(), headA);
    oc::check(repeat.success() && !repeat.value().appendRevision && repeat.value().ordinal == 4,
        "The head's own source state is a repeat that keeps the head's ordinal");

    oc::check(planOutcomeBriefAppend(a, OutcomeBriefExpectedHead::none(), std::nullopt).success(), "none and no head");
    const auto noneButHead = planOutcomeBriefAppend(b, OutcomeBriefExpectedHead::none(), headA);
    oc::check(!noneButHead.success() && noneButHead.error()->code == "outcome_brief.head_changed", "none but a head");
    const auto namedButNone = planOutcomeBriefAppend(a, OutcomeBriefExpectedHead::revision(headA.id), std::nullopt);
    oc::check(!namedButNone.success() && namedButNone.error()->code == "outcome_brief.head_changed", "a named head but none");
    const auto named = planOutcomeBriefAppend(b, OutcomeBriefExpectedHead::revision(headA.id), headA);
    oc::check(named.success() && named.value().ordinal == 5, "the named head is the head");
    const auto wrong = planOutcomeBriefAppend(b, OutcomeBriefExpectedHead::revision("other"), headA);
    oc::check(!wrong.success() && wrong.error()->code == "outcome_brief.head_changed", "another named head");
    // The check comes first: even a repeat of the head is refused when the caller expected something else.
    const auto repeatWrong = planOutcomeBriefAppend(a, OutcomeBriefExpectedHead::revision("other"), headA);
    oc::check(!repeatWrong.success() && repeatWrong.error()->code == "outcome_brief.head_changed",
        "A repeat does not excuse a wrong expectation");

    OutcomeBriefRevision last = headA;
    last.ordinal = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto exhausted = planOutcomeBriefAppend(b, OutcomeBriefExpectedHead::any(), last);
    oc::check(!exhausted.success() && exhausted.error()->code == "outcome_brief.revision_limit", "The ordinal range is bounded");
}

// The READ authority, project, in memory: the Run's project is not the one the stored briefs name (the case a hand-made SQLite row is
// in the SQLite tests). Every read that meets the briefs fails closed, from the side of either project, and nothing is built on them.
void testAForeignProjectIsNeverReadFromMemory()
{
    const auto env = inMemoryEnv();
    auto& repository = *env.repository;
    const auto first = oc::mustAppend(repository, oc::briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "A");
    oc::mustAppend(repository, oc::briefB(env, "run-1", env.projectId), OutcomeBriefExpectedHead::any(), "B");
    {
        const auto state = std::static_pointer_cast<InMemoryState>(env.keepAlive);
        std::lock_guard lock(state->runs.mutex);
        state->runs.runs["run-1"].projectId = env.otherProjectId;   // the briefs name project-1, the Run is project-2's
    }
    oc::requireInvalidData(repository.findHead("run-1"), "findHead");
    oc::requireInvalidData(repository.findRevision(first.head.id), "findRevision of an OLDER revision (a project never changes)");
    oc::requireInvalidData(repository.listRevisions("run-1", 10), "listRevisions");
    oc::requireInvalidData(repository.listHeads(env.projectId, 10), "listHeads of the project the briefs name");
    oc::requireInvalidData(repository.listHeads(env.otherProjectId, 10), "listHeads of the project the Run belongs to");
    oc::requireInvalidData(repository.findHeadVersionForAppend("run-1"), "the writer's read of a foreign head");

    const auto aboutTheOwner = deriveOutcomeBrief(oc::completedFacts("run-1", env.otherProjectId, {oc::ready("cs-1"), oc::ready("cs-2")}));
    oc::check(aboutTheOwner.success(), "setup: a brief about the project the Run belongs to");
    const auto refused = repository.append(aboutTheOwner.value(), OutcomeBriefExpectedHead::any());
    oc::check(!refused.success() && refused.error()->code == "persistence.invalid_data", "A new brief is not built on a foreign history");
    {
        const auto state = std::static_pointer_cast<InMemoryState>(env.keepAlive);
        std::lock_guard lock(state->runs.mutex);
        state->runs.runs["run-1"].projectId = env.projectId;   // the Run is project-1's again: the history reads
    }
    oc::check(oc::revisionCount(repository, "run-1") == 2, "...and it wrote nothing: the history is still the two revisions");
}

// Without a lookup the adapter knows no Runs: it keeps only briefs and checks no authority (a double for tests of the shared rules).
void testWithoutALookupOnlyTheHistoryKnowsTheProject()
{
    InMemoryOutcomeBriefRepository repository;
    oc::Env env;
    env.projectId = "project-1";
    env.otherProjectId = "project-2";
    oc::mustAppend(repository, oc::briefA(env, "run-1", env.projectId), OutcomeBriefExpectedHead::none(), "no Run table at all");
    oc::check(oc::headOf(repository, "run-1").has_value(), "...and the brief is read without any authority");
    oc::requireRefused(repository, oc::briefB(env, "run-1", env.otherProjectId), OutcomeBriefExpectedHead::any(),
        "outcome_brief.project_mismatch", "a Run that already has a history keeps its project");
}

// The shared authority rules, on their own: both adapters call these two functions.
void testTheAuthorityRules()
{
    const auto env = inMemoryEnv();
    const OutcomeBrief brief = oc::briefA(env, "run-1", env.projectId);   // a Completed brief of project-1
    const auto owner = [&](const std::string& project, RunStatus status) {
        return std::optional<OutcomeBriefRunOwner>(OutcomeBriefRunOwner {project, status});
    };
    const auto codeOf = [](const ai::common::Result<void>& result) { return result.success() ? std::string("ok") : result.error()->code; };

    oc::check(codeOf(checkOutcomeBriefWriteAuthority(brief, owner("project-1", RunStatus::Completed))) == "ok", "write: all agree");
    oc::check(codeOf(checkOutcomeBriefWriteAuthority(brief, std::nullopt)) == "outcome_brief.run_not_found", "write: no Run");
    oc::check(codeOf(checkOutcomeBriefWriteAuthority(brief, owner("project-2", RunStatus::Completed))) == "outcome_brief.project_mismatch",
        "write: another project");
    oc::check(codeOf(checkOutcomeBriefWriteAuthority(brief, owner("project-1", RunStatus::Running))) == "outcome_brief.run_status_mismatch",
        "write: another status");
    oc::check(codeOf(checkOutcomeBriefWriteAuthority(brief, owner("project-2", RunStatus::Running))) == "outcome_brief.project_mismatch",
        "write: the project is judged first");

    for (const bool isHead : {true, false}) {
        oc::check(codeOf(checkOutcomeBriefReadAuthority(brief, owner("project-1", RunStatus::Completed), isHead)) == "ok", "read: all agree");
        oc::check(codeOf(checkOutcomeBriefReadAuthority(brief, std::nullopt, isHead)) == "persistence.invalid_data", "read: no Run");
        oc::check(codeOf(checkOutcomeBriefReadAuthority(brief, owner("project-2", RunStatus::Completed), isHead)) == "persistence.invalid_data",
            "read: another project, head or not");
    }
    oc::check(codeOf(checkOutcomeBriefReadAuthority(brief, owner("project-1", RunStatus::Running), true)) == "persistence.invalid_data",
        "read: the current revision must have the Run's status");
    oc::check(codeOf(checkOutcomeBriefReadAuthority(brief, owner("project-1", RunStatus::Running), false)) == "ok",
        "read: an older revision describes the past");
}

void testTheRequestIsCheckedBeforeStorage()
{
    const auto env = inMemoryEnv();
    const OutcomeBrief good = oc::briefA(env, "run-1", env.projectId);
    oc::check(checkOutcomeBriefAppendRequest(good, OutcomeBriefExpectedHead::any()).success(), "a good request");
    OutcomeBrief bad = good;
    bad.state = OutcomeBriefState::ResolvedApplied;
    const auto gate = checkOutcomeBriefAppendRequest(bad, OutcomeBriefExpectedHead::any());
    oc::check(!gate.success() && gate.error()->code == "outcome_brief.invalid_brief", "the gate");
    const auto shape = checkOutcomeBriefAppendRequest(good, OutcomeBriefExpectedHead::revision(""));
    oc::check(!shape.success() && shape.error()->code == "outcome_brief.invalid_request", "the expected head");
    oc::check(checkOutcomeBriefListLimit(1).success() && checkOutcomeBriefListLimit(kOutcomeBriefMaxListLimit).success()
            && !checkOutcomeBriefListLimit(0).success() && !checkOutcomeBriefListLimit(kOutcomeBriefMaxListLimit + 1).success(),
        "the list limit is 1..1000");
}

void testTheHeadOrdering()
{
    const auto env = inMemoryEnv();
    const auto at = [](std::int64_t ms) { return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)); };
    auto older = makeOutcomeBriefRevision(oc::briefA(env, "run-a", env.projectId), 1);
    auto newer = makeOutcomeBriefRevision(oc::briefA(env, "run-b", env.projectId), 1);
    older.createdAt = at(1000);
    newer.createdAt = at(2000);
    oc::check(outcomeBriefHeadComesBefore(newer, older) && !outcomeBriefHeadComesBefore(older, newer), "newest first");
    older.createdAt = at(2000);
    oc::check(outcomeBriefHeadComesBefore(newer, older) && !outcomeBriefHeadComesBefore(older, newer),
        "equal times: the greater Run id first");
    oc::check(!outcomeBriefHeadComesBefore(newer, newer), "an element is not before itself");
}

} // namespace

int main()
{
    try {
        testTheInMemoryRepositoryHonoursTheContract();
        testTheRepositoryHandsOutCopies();
        testAForeignProjectIsNeverReadFromMemory();
        testWithoutALookupOnlyTheHistoryKnowsTheProject();
        testTheAuthorityRules();
        testOutcomeProposalDecisionsRoundTripThroughText();
        testThePlanOfAnAppend();
        testTheRequestIsCheckedBeforeStorage();
        testTheHeadOrdering();
        std::cout << "OutcomeBriefRepository_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
