#pragma once

// The behavioural contract of IProjectOutputApprovalRepository, run against every implementation (SQLite in
// AIOrchestrator_SqliteProjects_tests, in-memory in First_AI_Agents_tests). Header-only; the checks throw, so they stay active
// under NDEBUG.
#include "ai/application/workspace/ProjectOutputApproval.hpp"
#include "ai/application/workspace/RecordedWorkspaceExclusions.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace output_approval_contract {

using namespace ai::application;

struct Fixture {
    std::shared_ptr<void> keepAlive;
    IProjectOutputApprovalRepository* repository {nullptr};
    // Makes a project exist where the implementation needs one (SQLite: the foreign key).
    std::function<void(const std::string&)> ensureProject;
};
using FixtureFactory = std::function<Fixture()>;

inline void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

inline ProjectOutputApproval makeApproval(const std::string& id, const std::string& project, const std::string& root,
    const std::string& path, bool complete = true, bool nested = false, const std::string& by = "tester")
{
    ProjectOutputApproval value;
    value.id = id;
    value.projectId = project;
    value.workspaceRoot = root;
    value.workspaceKey = outputApprovalWorkspaceKey(root);
    value.relativePath = path;
    value.pathKey = outputApprovalPathKey(path);
    value.inspectionComplete = complete;
    value.nestedSourcesAcknowledged = nested;
    value.approvedBy = by;
    value.approvedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'123));
    return value;
}

// The set rule the service uses: the strict parser over current + candidate (duplicates, nesting and covering are errors).
inline ProjectOutputApprovalSetValidator setRuleFor(const ProjectOutputApproval& candidate)
{
    return [candidate](const std::vector<ProjectOutputApproval>& current) -> ai::common::Result<void> {
        std::vector<std::string> exclusions;
        for (const auto& record : current) exclusions.push_back("path:" + record.relativePath);
        exclusions.push_back("path:" + candidate.relativePath);
        const auto parsed = RecordedWorkspaceExclusions::parsePaths(exclusions);
        if (!parsed.success()) return ai::common::Result<void>::failure({"output_approval.nested_or_duplicate", parsed.error()->message});
        return ai::common::Result<void>::ok();
    };
}

inline std::vector<std::string> pathsOf(const std::vector<ProjectOutputApproval>& records)
{
    std::vector<std::string> paths;
    for (const auto& record : records) paths.push_back(record.relativePath);
    return paths;
}

inline bool commitOk(IProjectOutputApprovalRepository& repository, const ProjectOutputApproval& candidate,
    ProjectOutputApprovalCommitOutcome* outcome = nullptr)
{
    const auto result = repository.commit(candidate, setRuleFor(candidate));
    if (result.success() && outcome != nullptr) *outcome = result.value().outcome;
    return result.success();
}

inline void testRoundTripIsolationAndRevoke(const FixtureFactory& make)
{
    auto fixture = make();
    auto& repository = *fixture.repository;
    fixture.ensureProject("p1");
    fixture.ensureProject("p2");
    const std::string utf8Path = "\xD0\xA1\xD0\xB1\xD0\xBE\xD1\x80\xD0\xBA\xD0\xB0/\xD0\xB2\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4"; // Сборка/выход

    auto first = makeApproval("id-1", "p1", "C:/ws", utf8Path, true, true, "");
    const auto inserted = repository.commit(first, setRuleFor(first));
    check(inserted.success() && inserted.value().outcome == ProjectOutputApprovalCommitOutcome::Inserted, "A new approval is inserted");

    const auto listed = repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws"));
    check(listed.success() && listed.value().size() == 1, "...and listed for its pair");
    const auto& record = listed.value().front();
    check(record.id == "id-1" && record.projectId == "p1" && record.workspaceRoot == "C:/ws" && record.relativePath == utf8Path
            && record.pathKey == first.pathKey && record.inspectionComplete && record.nestedSourcesAcknowledged
            && record.approvedBy.empty() && record.approvedAt == first.approvedAt,
        "Every field round-trips: UTF-8 path, both facts, an empty actor, the millisecond time");

    check(commitOk(repository, makeApproval("id-2", "p2", "C:/ws", utf8Path)), "Another project may approve the same path");
    check(commitOk(repository, makeApproval("id-3", "p1", "D:/other", utf8Path)), "...and so may another root of the same project");
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "A pair sees only its own records");
    check(repository.listForWorkspace("p2", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "...project 2 sees its own");
    const auto all = repository.listForProject("p1");
    check(all.success() && all.value().size() == 2, "listForProject shows the records of every root of the project");
    check(repository.listForProject("nobody").value().empty(), "...and nothing for an unknown project");

    check(repository.revoke("p2", "id-1").success() && !repository.revoke("p2", "id-1").value(),
        "A project cannot revoke another project's record");
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "...which stays");
    check(repository.revoke("p1", "id-1").value(), "The owner revokes its record");
    check(!repository.revoke("p1", "id-1").value(), "...once");
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().empty(), "A revoked record is gone");
    auto again = makeApproval("id-4", "p1", "C:/ws", utf8Path);
    ProjectOutputApprovalCommitOutcome outcome {};
    check(commitOk(repository, again, &outcome) && outcome == ProjectOutputApprovalCommitOutcome::Inserted,
        "Approving again after a revoke is a new record");
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().front().id == "id-4",
        "...with the new id");
}

inline void testIdempotenceLimitAndValidator(const FixtureFactory& make)
{
    auto fixture = make();
    auto& repository = *fixture.repository;
    fixture.ensureProject("p1");
    fixture.ensureProject("p2");

    auto original = makeApproval("orig", "p1", "C:/ws", "Out", false, false, "first");
    check(commitOk(repository, original), "insert");
    auto repeat = makeApproval("other-id", "p1", "C:/ws", "Out", true, true, "second");
    repeat.approvedAt += std::chrono::seconds(60);
    const auto repeated = repository.commit(repeat, setRuleFor(repeat));
    check(repeated.success() && repeated.value().outcome == ProjectOutputApprovalCommitOutcome::AlreadyApproved
            && repeated.value().approval.id == "orig" && !repeated.value().approval.inspectionComplete
            && !repeated.value().approval.nestedSourcesAcknowledged && repeated.value().approval.approvedBy == "first"
            && repeated.value().approval.approvedAt == original.approvedAt,
        "A repeat returns the stored record unchanged: id, facts, actor and time");
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "...and adds no row");

    // The validator's failure leaves no trace, and it sees the pair's current set.
    std::vector<std::string> seen;
    const auto rejected = repository.commit(makeApproval("x", "p1", "C:/ws", "Other"),
        [&seen](const std::vector<ProjectOutputApproval>& current) -> ai::common::Result<void> {
            seen = pathsOf(current);
            return ai::common::Result<void>::failure({"output_approval.test_reject", "rejected"});
        });
    check(!rejected.success() && rejected.error()->code == "output_approval.test_reject", "A validator error is returned as it is");
    check(seen == std::vector<std::string> {"Out"}, "...it received the pair's current set");
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "...and nothing was stored");

    // Fill the pair to the limit.
    for (std::size_t index = 1; index < kProjectOutputApprovalLimit; ++index) {
        check(commitOk(repository, makeApproval("f" + std::to_string(index), "p1", "C:/ws", "Dir" + std::to_string(index))), "fill");
    }
    check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == kProjectOutputApprovalLimit,
        "The pair holds the limit");
    const auto ninth = repository.commit(makeApproval("ninth", "p1", "C:/ws", "Ninth"), setRuleFor(makeApproval("ninth", "p1", "C:/ws", "Ninth")));
    check(!ninth.success() && ninth.error()->code == kOutputApprovalLimitReachedCode, "One more is refused with the typed limit code");
    ProjectOutputApprovalCommitOutcome outcome {};
    check(commitOk(repository, makeApproval("again", "p1", "C:/ws", "Dir3"), &outcome)
            && outcome == ProjectOutputApprovalCommitOutcome::AlreadyApproved,
        "A repeat at the limit is still idempotent, not a ninth insert");
    check(commitOk(repository, makeApproval("other-root", "p1", "D:/ws", "Ninth")), "The limit is per (project, root)");
    check(commitOk(repository, makeApproval("other-project", "p2", "C:/ws", "Ninth")), "...and per project");
    check(repository.revoke("p1", "f1").value(), "A revoke frees a slot");
    check(commitOk(repository, makeApproval("ninth-b", "p1", "C:/ws", "Ninth")), "...which can be used");
}

// R4: the SAME object committed again is the exact repeat, in every state; only a NEW path may collide on an id.
inline void testRepeatOfTheSameObjectAndIdConflicts(const FixtureFactory& make)
{
    {
        auto fixture = make();
        auto& repository = *fixture.repository;
        fixture.ensureProject("p1");
        const auto same = makeApproval("same-id", "p1", "C:/ws", "A", false, true, "first");
        ProjectOutputApprovalCommitOutcome outcome {};
        check(commitOk(repository, same, &outcome) && outcome == ProjectOutputApprovalCommitOutcome::Inserted, "first commit");
        const auto repeated = repository.commit(same, setRuleFor(same));
        check(repeated.success() && repeated.value().outcome == ProjectOutputApprovalCommitOutcome::AlreadyApproved
                && repeated.value().approval.id == "same-id" && repeated.value().approval.approvedBy == "first",
            "The same object twice: the second is AlreadyApproved with the stored record, not a conflict");
        check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "...and one row");
    }
    {   // at the limit
        auto fixture = make();
        auto& repository = *fixture.repository;
        fixture.ensureProject("p1");
        std::vector<ProjectOutputApproval> all;
        for (std::size_t index = 0; index < kProjectOutputApprovalLimit; ++index) {
            all.push_back(makeApproval("lim-" + std::to_string(index), "p1", "C:/ws", "L" + std::to_string(index)));
            check(commitOk(repository, all.back()), "fill");
        }
        for (const auto& candidate : all) {
            const auto again = repository.commit(candidate, setRuleFor(candidate));
            check(again.success() && again.value().outcome == ProjectOutputApprovalCommitOutcome::AlreadyApproved
                    && again.value().approval.id == candidate.id,
                "The same object again at the limit is still AlreadyApproved");
        }
        check(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == kProjectOutputApprovalLimit,
            "...and the set stays at the limit");
    }
    {   // a real id conflict: another NEW path with a used id, while there is room
        auto fixture = make();
        auto& repository = *fixture.repository;
        fixture.ensureProject("p1");
        fixture.ensureProject("p2");
        check(commitOk(repository, makeApproval("taken", "p1", "C:/ws", "B")), "first use of the id");
        const auto clash = makeApproval("taken", "p1", "C:/ws", "C");
        const auto sameProject = repository.commit(clash, setRuleFor(clash));
        check(!sameProject.success() && sameProject.error()->code == kOutputApprovalConflictCode,
            "A used id for another new path is a conflict");
        const auto otherClash = makeApproval("taken", "p2", "C:/other", "Z");
        const auto otherProject = repository.commit(otherClash, setRuleFor(otherClash));
        check(!otherProject.success() && otherProject.error()->code == kOutputApprovalConflictCode,
            "...also for another project: ids are global");
        check(pathsOf(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value()) == std::vector<std::string> {"B"},
            "...and the set did not change");
        check(repository.listForProject("p2").value().empty(), "...nothing for the other project either");
    }
}

inline void testNestingRuleBothOrders(const FixtureFactory& make)
{
    {
        auto fixture = make();
        fixture.ensureProject("p1");
        check(commitOk(*fixture.repository, makeApproval("a", "p1", "C:/ws", "a")), "a");
        check(!commitOk(*fixture.repository, makeApproval("ab", "p1", "C:/ws", "a/b")), "a then a/b: refused");
        check(commitOk(*fixture.repository, makeApproval("sib", "p1", "C:/ws", "ab")), "a and ab are compatible");
        check(pathsOf(fixture.repository->listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value())
                == std::vector<std::string> {"a", "ab"},
            "Only the compatible records are stored");
    }
    {
        auto fixture = make();
        fixture.ensureProject("p1");
        check(commitOk(*fixture.repository, makeApproval("ab", "p1", "C:/ws", "a/b")), "a/b");
        check(!commitOk(*fixture.repository, makeApproval("a", "p1", "C:/ws", "a")), "a/b then a: refused");
        check(fixture.repository->listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 1, "...nothing added");
    }
}

// Runs `jobs` concurrently released by a barrier (never a sleep); returns the results in job order.
template <class Result>
std::vector<Result> runTogether(std::vector<std::function<Result()>> jobs)
{
    std::vector<Result> results(jobs.size());
    std::barrier gate(static_cast<std::ptrdiff_t>(jobs.size()));
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        threads.emplace_back([&, index] {
            gate.arrive_and_wait();
            results[index] = jobs[index]();
        });
    }
    for (auto& thread : threads) thread.join();
    return results;
}

// AT1 / AT3 / AT4 on one repository; `second` may be another CONNECTION to the same store (SQLite) or the same object.
inline void testConcurrentCommitsStayValid(IProjectOutputApprovalRepository& first, IProjectOutputApprovalRepository& second,
    const std::function<void(const std::string&)>& ensureProject, int rounds)
{
    for (int round = 0; round < rounds; ++round) {
        const std::string project = "c" + std::to_string(round);
        ensureProject(project);
        const std::string rootA = "C:/ws";
        {   // AT1: a and a/b at the same time
            const auto results = runTogether<bool>({
                [&] { return commitOk(first, makeApproval("n1-" + project, project, rootA, "a")); },
                [&] { return commitOk(second, makeApproval("n2-" + project, project, rootA, "a/b")); }});
            check(static_cast<int>(results[0]) + static_cast<int>(results[1]) == 1, "AT1: exactly one of a and a/b is stored");
            const auto stored = first.listForWorkspace(project, outputApprovalWorkspaceKey(rootA));
            check(stored.success() && stored.value().size() == 1, "AT1: the stored set holds one record");
            std::vector<std::string> exclusions;
            for (const auto& record : stored.value()) exclusions.push_back("path:" + record.relativePath);
            check(RecordedWorkspaceExclusions::parsePaths(exclusions).success(), "AT1: the stored set is valid");
        }
        {   // AT4: the same path twice
            const auto results = runTogether<int>({
                [&] {
                    ProjectOutputApprovalCommitOutcome outcome {};
                    return commitOk(first, makeApproval("s1-" + project, project, rootA, "same"), &outcome) ? static_cast<int>(outcome) : -1;
                },
                [&] {
                    ProjectOutputApprovalCommitOutcome outcome {};
                    return commitOk(second, makeApproval("s2-" + project, project, rootA, "same"), &outcome) ? static_cast<int>(outcome) : -1;
                }});
            check(results[0] >= 0 && results[1] >= 0 && results[0] != results[1], "AT4: one inserted, one already approved, no error");
            int rows = 0;
            const auto afterSame = first.listForWorkspace(project, outputApprovalWorkspaceKey(rootA));
            check(afterSame.success(), "AT4: the set reads");
            for (const auto& record : afterSame.value()) {
                if (record.relativePath == "same") ++rows;
            }
            check(rows == 1, "AT4: one row");
        }
    }
    {   // AT3: seven rows, two different last ones
        ensureProject("limit");
        for (int index = 0; index < 6; ++index) {
            check(commitOk(first, makeApproval("l" + std::to_string(index), "limit", "C:/ws", "D" + std::to_string(index))), "seed");
        }
        // 6 rows + "a" = 7
        check(commitOk(first, makeApproval("l-a", "limit", "C:/ws", "A")), "seed 7");
        const auto results = runTogether<bool>({
            [&] { return commitOk(first, makeApproval("e1", "limit", "C:/ws", "E1")); },
            [&] { return commitOk(second, makeApproval("e2", "limit", "C:/ws", "E2")); }});
        check(static_cast<int>(results[0]) + static_cast<int>(results[1]) == 1, "AT3: only one of two last inserts passes");
        check(first.listForWorkspace("limit", outputApprovalWorkspaceKey("C:/ws")).value().size() == kProjectOutputApprovalLimit,
            "AT3: the pair holds exactly the limit");
    }
}

inline void testValidatorFailureRollsBackAndReadsAreConsistent(const FixtureFactory& make)
{
    auto fixture = make();
    auto& repository = *fixture.repository;
    fixture.ensureProject("p1");
    check(commitOk(repository, makeApproval("keep", "p1", "C:/ws", "Keep")), "seed");
    // AT7: a failing validator rolls the command back completely, also when it runs next to a concurrent revoke/read.
    std::atomic<bool> stop {false};
    std::atomic<int> torn {0};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto listed = repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws"));
            if (!listed.success()) { ++torn; continue; }
            for (const auto& record : listed.value()) {
                if (record.relativePath != "Keep" && record.relativePath != "Extra") ++torn;
            }
        }
    });
    for (int index = 0; index < 50; ++index) {
        const auto failed = repository.commit(makeApproval("bad" + std::to_string(index), "p1", "C:/ws", "Bad"),
            [](const std::vector<ProjectOutputApproval>&) -> ai::common::Result<void> {
                return ai::common::Result<void>::failure({"output_approval.test_reject", "no"});
            });
        check(!failed.success(), "the validator refuses");
        check(commitOk(repository, makeApproval("extra" + std::to_string(index), "p1", "C:/ws", "Extra")), "a good commit still works");
        check(repository.revoke("p1", "extra" + std::to_string(index)).value(), "...and its revoke");
    }
    stop = true;
    reader.join();
    check(torn.load() == 0, "A concurrent reader never sees a failed commit, a partial record or a read failure");
    check(pathsOf(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value()) == std::vector<std::string> {"Keep"},
        "The refused commits left nothing behind");
}

} // namespace output_approval_contract
