#pragma once

// PC-4C: one scenario for IRunRepository::listTerminalRunsPage, run on every adapter that has its own implementation (SQLite) and on the
// default one (in-memory): the same order, the same keyset, the same filters.

#include "ai/application/IRunRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/verification/ManifestVerificationManualHandoffTypes.hpp"

#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace runpaging_contract {

using namespace ai::application;

struct Env {
    IRunRepository* repository {nullptr};
    std::function<void(const Run&)> insert;                       // stores a Run (the adapter makes its project exist)
    std::function<void(const std::string&)> ensureProject;
};

inline void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

inline Run makeRun(const std::string& id, const std::string& project, const std::string& workflow, RunStatus status, long long ms)
{
    Run run;
    run.id = id;
    run.projectId = project;
    run.title = id;
    run.task = id;
    run.workflowId = workflow;
    run.status = status;
    run.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'000LL + ms));
    return run;
}

inline std::vector<std::string> ids(const std::vector<Run>& runs)
{
    std::vector<std::string> out;
    for (const auto& run : runs) out.push_back(run.id);
    return out;
}

inline RunPageCursor cursorOf(const Run& run) { return RunPageCursor {run.createdAt, run.id}; }

inline void runRunPagingContract(const Env& env)
{
    const std::string manifest = kExecutionManifestWorkflowId;
    const std::string continuation(kManifestVerificationContinuationWorkflowId);
    const std::vector<std::string> outcome = {manifest, continuation};
    env.ensureProject("p1");
    env.ensureProject("p2");
    // r05 and r06 share a creation time: the id breaks the tie (descending). Running / Prepared / other workflows / other projects
    // are never in a page.
    for (const Run& run : {makeRun("r01", "p1", manifest, RunStatus::Completed, 100), makeRun("r02", "p1", manifest, RunStatus::Failed, 200),
             makeRun("r03", "p1", manifest, RunStatus::Cancelled, 300), makeRun("r04", "p1", manifest, RunStatus::Interrupted, 400),
             makeRun("r05", "p1", continuation, RunStatus::Completed, 500), makeRun("r06", "p1", manifest, RunStatus::Completed, 500),
             makeRun("r07", "p1", manifest, RunStatus::Running, 600), makeRun("r08", "p1", manifest, RunStatus::Prepared, 650),
             makeRun("r09", "p1", std::string(kManifestVerificationManualHandoffWorkflowId), RunStatus::Completed, 700),
             makeRun("r10", "p1", kMockWorkflowId, RunStatus::Completed, 710), makeRun("r11", "p1", manifest, RunStatus::Completed, 800),
             makeRun("x01", "p2", manifest, RunStatus::Completed, 900)}) {
        env.insert(run);
    }
    const std::vector<std::string> everything = {"r11", "r06", "r05", "r04", "r03", "r02", "r01"};

    // The whole history, in pages of every size, is always the same sequence.
    for (const std::size_t size : {std::size_t {1}, std::size_t {2}, std::size_t {3}, std::size_t {7}, std::size_t {50}}) {
        std::vector<std::string> collected;
        std::optional<RunPageCursor> cursor;
        for (int guard = 0; guard < 20; ++guard) {
            const auto page = env.repository->listTerminalRunsPage("p1", outcome, cursor, size);
            check(page.success(), "a page must be readable");
            check(page.value().size() <= size, "a page never exceeds its limit");
            for (const auto& id : ids(page.value())) collected.push_back(id);
            if (page.value().size() < size) break;
            cursor = cursorOf(page.value().back());
        }
        check(collected == everything, "pages of " + std::to_string(size) + " give the whole history, newest first, without a gap or a repeat");
    }

    // A page strictly follows its cursor, also in the middle of a tie.
    const auto afterTie = env.repository->listTerminalRunsPage("p1", outcome, cursorOf(makeRun("r06", "p1", manifest, RunStatus::Completed, 500)), 10);
    check(afterTie.success() && ids(afterTie.value()) == std::vector<std::string>({"r05", "r04", "r03", "r02", "r01"}), "after r06 comes r05, not r06 again");
    const auto afterSecond = env.repository->listTerminalRunsPage("p1", outcome, cursorOf(makeRun("r05", "p1", manifest, RunStatus::Completed, 500)), 10);
    check(afterSecond.success() && ids(afterSecond.value()) == std::vector<std::string>({"r04", "r03", "r02", "r01"}), "after r05 comes r04");
    // A cursor that points at nothing that exists still works (a keyset, not an offset).
    const auto afterGhost = env.repository->listTerminalRunsPage("p1", outcome, cursorOf(makeRun("zzz", "p1", manifest, RunStatus::Completed, 450)), 2);
    check(afterGhost.success() && ids(afterGhost.value()) == std::vector<std::string>({"r04", "r03"}), "a cursor between two Runs continues at the next older one");

    // The filters.
    const auto manifestOnly = env.repository->listTerminalRunsPage("p1", {manifest}, std::nullopt, 50);
    check(manifestOnly.success() && ids(manifestOnly.value()) == std::vector<std::string>({"r11", "r06", "r04", "r03", "r02", "r01"}), "only the asked workflow");
    check(env.repository->listTerminalRunsPage("p1", {}, std::nullopt, 50).value().empty(), "no workflow asked, nothing returned");
    check(env.repository->listTerminalRunsPage("nobody", outcome, std::nullopt, 50).value().empty(), "an unknown project has no Runs");
    const auto other = env.repository->listTerminalRunsPage("p2", outcome, std::nullopt, 50);
    check(other.success() && ids(other.value()) == std::vector<std::string>({"x01"}), "another project's Runs stay in its own pages");
    for (const std::size_t bad : {std::size_t {0}, kMaxRunPageSize + 1}) {
        const auto refused = env.repository->listTerminalRunsPage("p1", outcome, std::nullopt, bad);
        check(!refused.success() && refused.error()->code == "run.invalid_page", "a page size of " + std::to_string(bad) + " is refused");
    }
    check(env.repository->listTerminalRunsPage("p1", outcome, std::nullopt, kMaxRunPageSize).success(), "the largest page is allowed");

    // Stable: a Run created while a pass is under way does not move the pages it has not reached; an older one shows up at the end.
    const auto firstPage = env.repository->listTerminalRunsPage("p1", outcome, std::nullopt, 3);
    check(firstPage.success() && ids(firstPage.value()) == std::vector<std::string>({"r11", "r06", "r05"}), "setup: the first page");
    env.insert(makeRun("r12", "p1", manifest, RunStatus::Completed, 950));   // newer than everything
    env.insert(makeRun("r00", "p1", manifest, RunStatus::Completed, 50));    // older than everything
    const auto rest = env.repository->listTerminalRunsPage("p1", outcome, cursorOf(firstPage.value().back()), 10);
    check(rest.success() && ids(rest.value()) == std::vector<std::string>({"r04", "r03", "r02", "r01", "r00"}),
        "the pages after the cursor ignore the newer Run and include the older one");
}

} // namespace runpaging_contract
