#include "FollowUpPlanRequestRepositoryContract.hpp"

#include "ai/application/InMemoryFollowUpPlanRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"

#include <iostream>
#include <map>
#include <mutex>

namespace {

namespace fc = followup_contract;
using namespace ai::application;
using namespace followup_test;

// The Runs the in-memory adapter's lookup answers from: the stand-in of the `runs` table.
struct InMemoryState {
    std::mutex mutex;
    std::map<std::string, std::string> runs;   // Run id -> project id
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository repository {
        [this](const RunId& runId) -> std::optional<ProjectId> {
            std::lock_guard lock(mutex);
            const auto found = runs.find(runId);
            if (found == runs.end()) return std::nullopt;
            return found->second;
        },
        &drafts};
};

fc::Env inMemoryEnv()
{
    auto state = std::make_shared<InMemoryState>();
    fc::Env env;
    env.keepAlive = state;
    env.repository = &state->repository;
    env.confirm = &state->repository;
    env.drafts = &state->drafts;
    env.projectId = "project-1";
    env.otherProjectId = "project-2";
    env.ensureRun = [state](const std::string& runId, const std::string& projectId) {
        std::lock_guard lock(state->mutex);
        state->runs.try_emplace(runId, projectId);
    };
    env.moveRun = [state](const std::string& runId, const std::string& projectId) {
        std::lock_guard lock(state->mutex);
        check(state->runs.count(runId) == 1, "setup: the Run must exist before it moves");
        state->runs[runId] = projectId;
    };
    return env;
}

void testTheInMemoryRepositoryHonoursTheContract()
{
    fc::runFollowUpPlanRequestRepositoryContract([] { return inMemoryEnv(); });
}

// The values handed out are copies: changing one never changes what the repository holds.
void testTheRepositoryHandsOutCopies()
{
    const auto env = inMemoryEnv();
    const auto first = fc::mustInsert(env, fc::quoted(env, "req-1", "run-1"));
    auto answer = first;
    answer.goal = "tampered";
    answer.quote.capMicros = 1;
    auto found = fc::stored(env, "req-1");
    found.state = FollowUpPlanRequestState::PlanReady;
    auto listed = env.repository->listForProject(env.projectId, 10).value();
    listed.front().files.clear();
    check(sameRequest(fc::stored(env, "req-1"), first), "A copy taken from the repository cannot rewrite it");
}

// Without a lookup the adapter knows no Runs: it checks no authority (a double for tests of the shared rules).
void testWithoutALookupNoAuthorityIsChecked()
{
    InMemoryFollowUpPlanRepository repository;
    const Catalog catalog;
    const auto request = quotedRequest(catalog, "req-1", "project-1", "any-run", at(0));
    check(repository.insertQuoted(request).success(), "No Run table, no write authority");
    check(repository.findById("req-1").value().has_value(), "...and no read authority either");
    // It still enforces the one-open slot and the gate.
    const auto replaced = repository.insertQuoted(quotedRequest(catalog, "req-2", "project-1", "any-run", at(1000)));
    check(replaced.success() && replaced.value().discardedQuoteId == std::optional<std::string>("req-1"),
        "...but the one-open slot still replaces a quote");
    auto broken = quotedRequest(catalog, "req-3", "project-1", "other-run", at(0));
    broken.maxDrafts = 9;
    requireCode(repository.insertQuoted(broken), "follow_up.invalid_request", "...and the gate still runs");
}

// Confirm needs the Draft store; without it every confirm is refused instead of pretending to save something.
void testConfirmNeedsADraftStore()
{
    InMemoryFollowUpPlanRepository repository;
    requireCode(repository.confirmDrafts("req-1", hex64('c'), RunDraftGroup {}, {}, at(0)), "follow_up.confirm_unavailable",
        "a repository without a Draft store");
}

} // namespace

int main()
{
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAILED " << name << ": " << error.what() << '\n';
        }
    };
    run("testTheInMemoryRepositoryHonoursTheContract", testTheInMemoryRepositoryHonoursTheContract);
    run("testTheRepositoryHandsOutCopies", testTheRepositoryHandsOutCopies);
    run("testWithoutALookupNoAuthorityIsChecked", testWithoutALookupNoAuthorityIsChecked);
    run("testConfirmNeedsADraftStore", testConfirmNeedsADraftStore);
    if (failures != 0) {
        std::cerr << "FollowUpPlanRequestRepository tests failed: " << failures << " test(s)\n";
        return 1;
    }
    std::cout << "FollowUpPlanRequestRepository tests passed\n";
    return 0;
}
