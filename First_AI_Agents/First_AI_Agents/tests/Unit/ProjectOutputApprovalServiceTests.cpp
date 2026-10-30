// P3a: project output approvals. This file holds the contract run against the in-memory repository (the SQLite twin runs in
// AIOrchestrator_SqliteProjects_tests) and the tests of the service, the folder inspector and policyFor.
//
// The checks throw, so they stay active under NDEBUG. Every folder here is a temporary one; the junction checks create real
// junctions (or print an explicit SKIP when none can be made) - the absence of a NOTE is not evidence that they ran.
#include "ProjectOutputApprovalRepositoryContract.hpp"

#include "ai/application/inmemory/InMemoryProjectOutputApprovalRepository.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/workspace/ProjectOutputApprovalService.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ai::application;
namespace fs = std::filesystem;
namespace contract = output_approval_contract;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <class T>
void expectRefusal(const ai::common::Result<T>& result, const std::string& code, const std::string& what)
{
    if (result.success()) throw std::runtime_error(what + ": expected " + code + " but it succeeded");
    if (result.error()->code != code) {
        throw std::runtime_error(what + ": expected " + code + " but got " + result.error()->code + " (" + result.error()->message + ")");
    }
}

template <class T>
void expectOk(const ai::common::Result<T>& result, const std::string& what)
{
    if (!result.success()) throw std::runtime_error(what + ": " + result.error()->code + " (" + result.error()->message + ")");
}

// ---- the in-memory twin of the repository contract ----

contract::Fixture inMemoryApprovalFixture()
{
    auto repository = std::make_shared<InMemoryProjectOutputApprovalRepository>();
    return {repository, repository.get(), [](const std::string&) {}};
}

void testTheInMemoryRepositoryKeepsTheSameContract()
{
    contract::testRoundTripIsolationAndRevoke(inMemoryApprovalFixture);
    contract::testIdempotenceLimitAndValidator(inMemoryApprovalFixture);
    contract::testNestingRuleBothOrders(inMemoryApprovalFixture);
    contract::testRepeatOfTheSameObjectAndIdConflicts(inMemoryApprovalFixture);
    contract::testValidatorFailureRollsBackAndReadsAreConsistent(inMemoryApprovalFixture);
    InMemoryProjectOutputApprovalRepository repository;
    contract::testConcurrentCommitsStayValid(repository, repository, [](const std::string&) {}, 40);
}

// ---- fixtures for the service ----

fs::path makeTempRoot()
{
    static std::atomic<int> counter {0};
    const fs::path root = fs::temp_directory_path()
        / ("aio-approvals-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
            + std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    return root;
}

void touch(const fs::path& path, const std::string& text = "x")
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// A directory link made the way this machine allows: a junction on Windows, otherwise a directory symlink. False when none can be made.
bool makeDirectoryLink(const fs::path& link, const fs::path& target)
{
#ifdef _WIN32
    const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    return std::system(command.c_str()) == 0 && fs::exists(link);
#else
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    return !error;
#endif
}

// Delegates to the real inspector, optionally runs a hook first or answers with a prepared result.
class HookInspector final : public IOutputDirectoryInspector {
public:
    mutable std::function<void(std::stop_token)> beforeInspect;
    mutable std::optional<OutputDirectoryInspection> forced;
    mutable std::atomic<int> calls {0};

    [[nodiscard]] OutputDirectoryInspection inspect(
        const fs::path& directory, const OutputDirectoryInspectionLimits& limits, std::stop_token stopToken) const override
    {
        ++calls;
        if (beforeInspect) beforeInspect(stopToken);
        if (forced.has_value()) return *forced;
        return real.inspect(directory, limits, stopToken);
    }

private:
    FilesystemOutputDirectoryInspector real;
};

// A repository that can be made to fail, over the in-memory one.
class FailingApprovalRepository final : public IProjectOutputApprovalRepository {
public:
    InMemoryProjectOutputApprovalRepository inner;
    bool failReads {false};
    int commitCalls {0};

    [[nodiscard]] ai::common::Result<ProjectOutputApprovalCommitResult> commit(
        const ProjectOutputApproval& candidate, const ProjectOutputApprovalSetValidator& validator) override
    {
        ++commitCalls;
        return inner.commit(candidate, validator);
    }
    [[nodiscard]] ai::common::Result<std::vector<ProjectOutputApproval>> listForWorkspace(
        const ProjectId& projectId, const std::string& workspaceKey) const override
    {
        if (failReads) {
            return ai::common::Result<std::vector<ProjectOutputApproval>>::failure({kOutputApprovalStorageFailedCode, "disk gone"});
        }
        return inner.listForWorkspace(projectId, workspaceKey);
    }
    [[nodiscard]] ai::common::Result<std::vector<ProjectOutputApproval>> listForProject(const ProjectId& projectId) const override
    {
        return inner.listForProject(projectId);
    }
    ai::common::Result<bool> revoke(const ProjectId& projectId, const std::string& approvalId) override
    {
        return inner.revoke(projectId, approvalId);
    }
};

struct World {
    fs::path root;
    fs::path ws;
    InMemoryProjectRepository projects;
    FailingApprovalRepository approvals;
    std::vector<ProtectedStoragePath> protectedList;
    bool protectedFails {false};
    FunctionProtectedStoragePathSource protectedSource;
    HookInspector inspector;
    ProjectOutputApprovalService service;
    Project project;

    explicit World(OutputDirectoryInspectionLimits limits = {})
        : root(makeTempRoot())
        , ws(root / "ws")
        , protectedSource([this](const fs::path&) -> ai::common::Result<std::vector<ProtectedStoragePath>> {
            if (protectedFails) return ai::common::Result<std::vector<ProtectedStoragePath>>::failure({"test.provider", "no answer"});
            return ai::common::Result<std::vector<ProtectedStoragePath>>::ok(protectedList);
        })
        , service(projects, approvals, protectedSource, inspector, limits)
    {
        fs::create_directories(ws);
        project.id = "p1";
        project.name = "Fixture";
        project.rootPath = fs::canonical(ws);
        project.status = ProjectStatus::Active;
        check(projects.insert(project).success(), "could not create the project");
    }

    ~World()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void updateProject(const std::function<void(Project&)>& change)
    {
        change(project);
        check(projects.update(project).success(), "could not update the project");
    }

    OutputApprovalRequest request(const std::string& path, bool nested = false, bool incomplete = false,
        const std::string& by = "tester") const
    {
        OutputApprovalRequest value;
        value.projectId = project.id;
        value.relativePath = path;
        value.acknowledgeNestedSources = nested;
        value.acknowledgeIncomplete = incomplete;
        value.approvedBy = by;
        return value;
    }

    ai::common::Result<ProjectOutputApprovalCommitResult> approve(const std::string& path, bool nested = false, bool incomplete = false)
    {
        return service.approve(request(path, nested, incomplete));
    }

    std::size_t storedCount() const
    {
        return approvals.inner.listForProject(project.id).value().size();
    }
};

// ---- tests ----

void testAnOutputThatDoesNotExistYetCanBeApproved()
{
    World w;
    fs::create_directories(w.ws / "Builds");
    const auto preview = w.service.preview("p1", "Builds/x64-ninja");
    expectOk(preview, "preview");
    check(!preview.value().exists && preview.value().existingPrefix == "Builds", "The existing part of the path is reported");
    check(!preview.value().requiresNestedConfirmation && !preview.value().requiresIncompleteConfirmation && !preview.value().alreadyApproved,
        "A folder that is not there needs no confirmation");
    check(preview.value().workspaceRoot == detail::genericUtf8(w.project.rootPath), "The root is the project's own record");
    check(!fs::exists(w.ws / "Builds" / "x64-ninja"), "A preview creates nothing");
    check(w.storedCount() == 0, "...and stores nothing");
    check(w.inspector.calls == 0, "A folder that does not exist is not walked");

    const auto approved = w.approve("Builds/x64-ninja");
    expectOk(approved, "approve");
    check(approved.value().outcome == ProjectOutputApprovalCommitOutcome::Inserted, "...and is inserted");
    check(!fs::exists(w.ws / "Builds" / "x64-ninja"), "An approval creates no folder either");
    const auto& record = approved.value().approval;
    check(record.inspectionComplete && !record.nestedSourcesAcknowledged && record.approvedBy == "tester" && !record.id.empty(),
        "The record carries the facts: complete, no nested sources, the actor");
    const auto again = w.approve("Builds/x64-ninja");
    check(again.success() && again.value().outcome == ProjectOutputApprovalCommitOutcome::AlreadyApproved
            && again.value().approval.id == record.id && again.value().approval.approvedAt == record.approvedAt,
        "A repeat is idempotent: the stored id and time are returned");
    check(w.service.preview("p1", "Builds/x64-ninja").value().alreadyApproved, "...and the preview says so");
    const auto root = w.service.preview("p1", "Out2");
    check(root.success() && root.value().existingPrefix.empty(), "Only the root exists for a first-level missing folder");
}

void testHardRefusalsAndTheirIndependenceOfTheConfirmations()
{
    World w;
    for (const char* bad : {".", "..", "a/../b", "/abs", "C:/x", "a\\b", "", "a/", "./a", "a//b", "CON", "bad:name"}) {
        expectRefusal(w.service.preview("p1", bad), kOutputApprovalInvalidPathCode, std::string("invalid path '") + bad + "'");
        expectRefusal(w.service.approve(w.request(bad, true, true)), kOutputApprovalInvalidPathCode,
            std::string("approve of invalid path '") + bad + "'");
    }
    check(w.storedCount() == 0, "Nothing was stored for any invalid path");

    // A CMakeLists.txt directly in the folder: a hard refusal that no confirmation lifts.
    touch(w.ws / "Src" / "CMakeLists.txt");
    expectRefusal(w.approve("Src", true, true), kOutputDirContainsCMakeSourcesCode, "root CMakeLists.txt");
    expectRefusal(w.service.preview("p1", "Src"), kOutputDirContainsCMakeSourcesCode, "preview of a folder with CMakeLists.txt");

    // A file instead of a folder, and a file as a parent.
    touch(w.ws / "file.txt");
    expectRefusal(w.approve("file.txt", true, true), kOutputApprovalNotADirectoryCode, "file instead of folder");
    expectRefusal(w.approve("file.txt/inner", true, true), kOutputApprovalNotADirectoryCode, "file as a parent");

    // Links: neither as the folder nor as a component of the path.
    fs::create_directories(w.root / "outside" / "inner");
    if (makeDirectoryLink(w.ws / "lnk", w.root / "outside")) {
        expectRefusal(w.approve("lnk", true, true), kOutputApprovalReparsePointCode, "link as the folder");
        expectRefusal(w.approve("lnk/inner", true, true), kOutputApprovalReparsePointCode, "link as a component");
        expectRefusal(w.service.preview("p1", "lnk/never"), kOutputApprovalReparsePointCode, "link before a missing tail");
    } else {
        std::cout << "SKIP: no junction or symbolic link can be made here; the link refusals were NOT exercised\n";
    }
    check(w.storedCount() == 0, "Nothing was stored for any refusal");

    // The project: missing, and archived.
    expectRefusal(w.service.preview("ghost", "Out"), "project.not_found", "unknown project");
    w.updateProject([](Project& p) { p.status = ProjectStatus::Archived; });
    expectRefusal(w.approve("Out"), kOutputApprovalProjectNotActiveCode, "archived project");
    w.updateProject([](Project& p) { p.status = ProjectStatus::Active; });
    // A root that is gone.
    w.updateProject([&w](Project& p) { p.rootPath = w.root / "vanished"; });
    expectRefusal(w.approve("Out"), kOutputApprovalWorkspaceUnavailableCode, "vanished root");
}

void testNestedSourcesNeedTheirOwnConfirmationAndNothingIsSkippedByName()
{
    World w;
    touch(w.ws / "Build" / "CMakeCache.txt");
    touch(w.ws / "Build" / "_deps" / "fmt-src" / "CMakeLists.txt");
    touch(w.ws / "Build" / "CMakeFiles" / "CMakeScratch" / "TryCompile-1" / "CMakeLists.txt");
    touch(w.ws / "Build" / "other.bin", "0123456789");

    const auto preview = w.service.preview("p1", "Build");
    expectOk(preview, "preview");
    check(preview.value().exists && preview.value().requiresNestedConfirmation && !preview.value().requiresIncompleteConfirmation,
        "Nested sources are found by content, not skipped by the names _deps / CMakeFiles");
    check(preview.value().inspection.status == OutputInspectionStatus::Complete && preview.value().inspection.nestedSourceCount == 2
            && preview.value().inspection.nestedSourceSamples.size() == 2,
        "...both are counted and shown");
    const auto& samples = preview.value().inspection.nestedSourceSamples;
    check(std::find(samples.begin(), samples.end(), "_deps/fmt-src/CMakeLists.txt") != samples.end()
            && std::find(samples.begin(), samples.end(), "CMakeFiles/CMakeScratch/TryCompile-1/CMakeLists.txt") != samples.end(),
        "...as relative generic UTF-8 paths");
    check(preview.value().inspection.bytes >= 10, "The size is counted from metadata");

    expectRefusal(w.approve("Build"), kOutputDirNestedSourcesUnconfirmedCode, "no confirmation");
    expectRefusal(w.approve("Build", false, true), kOutputDirNestedSourcesUnconfirmedCode,
        "the other confirmation does not confirm nested sources");
    check(w.storedCount() == 0, "Nothing stored yet");
    const auto approved = w.approve("Build", true, false);
    expectOk(approved, "confirmed");
    check(approved.value().approval.nestedSourcesAcknowledged && approved.value().approval.inspectionComplete,
        "The record keeps what was found and confirmed");

    // The same names without sources are nothing special.
    touch(w.ws / "Plain" / "_deps" / "readme.txt");
    const auto plain = w.service.preview("p1", "Plain");
    check(plain.success() && !plain.value().requiresNestedConfirmation, "_deps alone proves nothing");
}

void testAnIncompleteWalkIsNeverCompleteAndNeedsItsOwnConfirmation()
{
    OutputDirectoryInspectionLimits small;
    small.maxEntries = 5;
    World w(small);
    for (int index = 0; index < 30; ++index) touch(w.ws / "Big" / ("f" + std::to_string(index) + ".o"));
    const auto preview = w.service.preview("p1", "Big");
    expectOk(preview, "preview");
    check(preview.value().inspection.status == OutputInspectionStatus::Incomplete && preview.value().inspection.hitEntryLimit
            && preview.value().requiresIncompleteConfirmation && preview.value().inspection.entries > 0,
        "A limit makes the result Incomplete");
    check(preview.value().inspection.entries <= small.maxEntries, "...and the walk stopped at the limit");
    expectRefusal(w.approve("Big"), kOutputDirInspectionIncompleteCode, "no confirmation");
    expectRefusal(w.approve("Big", true, false), kOutputDirInspectionIncompleteCode, "the nested confirmation does not confirm completeness");
    const auto approved = w.approve("Big", false, true);
    expectOk(approved, "confirmed");
    check(!approved.value().approval.inspectionComplete && !approved.value().approval.nestedSourcesAcknowledged,
        "The record says the walk was incomplete and that no nested sources were confirmed");

    // The depth limit alone is also Incomplete.
    OutputDirectoryInspectionLimits shallow;
    shallow.maxDepth = 2;
    World deep(shallow);
    touch(deep.ws / "D" / "a" / "b" / "c" / "file.txt");
    const auto deepPreview = deep.service.preview("p1", "D");
    check(deepPreview.success() && deepPreview.value().inspection.hitDepthLimit && deepPreview.value().requiresIncompleteConfirmation,
        "A folder below the depth limit is reported, not entered");

    // A link inside the tree is not entered and makes the result Incomplete (a link is a fact, not a proof).
    World linked;
    fs::create_directories(linked.root / "target" / "x");
    touch(linked.ws / "L" / "file.txt");
    if (makeDirectoryLink(linked.ws / "L" / "lnk", linked.root / "target")) {
        const auto linkedPreview = linked.service.preview("p1", "L");
        check(linkedPreview.success() && linkedPreview.value().inspection.skippedLinks == 1
                && linkedPreview.value().inspection.status == OutputInspectionStatus::Incomplete
                && linkedPreview.value().requiresIncompleteConfirmation,
            "A link inside the tree is not entered and the walk is Incomplete");
        check(linkedPreview.value().inspection.files == 1, "...and nothing behind it is counted");
    } else {
        std::cout << "SKIP: no junction or symbolic link can be made here; the in-tree link check was NOT exercised\n";
    }
}

void testTheTwoConfirmationsAreIndependentAndNeverLiftAHardRefusal()
{
    World w;
    touch(w.ws / "Mixed" / "marker.txt");
    OutputDirectoryInspection both;
    both.status = OutputInspectionStatus::Incomplete;
    both.hitEntryLimit = true;
    both.entries = 10;
    both.nestedSourceCount = 1;
    both.nestedSourceSamples = {"sub/CMakeLists.txt"};
    w.inspector.forced = both;

    const auto preview = w.service.preview("p1", "Mixed");
    check(preview.success() && preview.value().requiresNestedConfirmation && preview.value().requiresIncompleteConfirmation,
        "Both facts are reported");
    expectRefusal(w.approve("Mixed", true, false), kOutputDirInspectionIncompleteCode, "only the nested confirmation");
    expectRefusal(w.approve("Mixed", false, true), kOutputDirNestedSourcesUnconfirmedCode, "only the incomplete confirmation");
    expectRefusal(w.approve("Mixed", false, false), kOutputDirNestedSourcesUnconfirmedCode, "neither");
    const auto approved = w.approve("Mixed", true, true);
    expectOk(approved, "both confirmations");
    check(!approved.value().approval.inspectionComplete && approved.value().approval.nestedSourcesAcknowledged,
        "Both facts are stored as they are");

    // A hard failure of the walk is not a thing to confirm.
    OutputDirectoryInspection failed;
    failed.status = OutputInspectionStatus::Failed;
    failed.failure = "a folder could not be listed";
    w.inspector.forced = failed;
    touch(w.ws / "Locked" / "x.txt");
    expectRefusal(w.approve("Locked", true, true), kOutputDirInspectionFailedCode, "an inspection failure");
    expectRefusal(w.service.preview("p1", "Locked"), kOutputDirInspectionFailedCode, "...also in a preview");

    // Neither confirmation lifts a protected path, a link, a file or a root CMakeLists.txt.
    w.inspector.forced.reset();
    w.protectedList = {{w.ws / "Locked", true}};
    expectRefusal(w.approve("Locked", true, true), kOutputApprovalProtectedStorageCode, "protected storage");
    w.protectedList.clear();
    touch(w.ws / "Src" / "CMakeLists.txt");
    expectRefusal(w.approve("Src", true, true), kOutputDirContainsCMakeSourcesCode, "root sources");
    check(w.storedCount() == 1, "Only the confirmed approval was stored");
}

void testCancellationNeverWrites()
{
    {
        World w;
        touch(w.ws / "Out" / "a.txt");
        std::stop_source source;
        source.request_stop();
        expectRefusal(w.service.approve(w.request("Out"), source.get_token()), kOutputApprovalCancelledCode, "cancelled before the walk");
        check(w.storedCount() == 0, "Nothing stored after a cancelled approval");
        expectRefusal(w.service.preview("p1", "Out", source.get_token()), kOutputApprovalCancelledCode, "a cancelled preview");
    }
    {   // the real inspector honours a token that is already stopped, before it sees an entry
        FilesystemOutputDirectoryInspector inspector;
        World w;
        touch(w.ws / "Out" / "a.txt");
        std::stop_source source;
        source.request_stop();
        const auto result = inspector.inspect(w.ws / "Out", OutputDirectoryInspectionLimits {}, source.get_token());
        check(result.status == OutputInspectionStatus::Cancelled && result.entries == 0, "The walk checks the token first");
    }
    {   // a stop that arrives while the walk is running: the answer is Complete, but nothing is written
        World w;
        touch(w.ws / "Out" / "a.txt");
        std::stop_source source;
        w.inspector.beforeInspect = [&source](std::stop_token) { source.request_stop(); };
        expectRefusal(w.service.approve(w.request("Out"), source.get_token()), kOutputApprovalCancelledCode,
            "stop requested during the walk");
        check(w.storedCount() == 0, "...and nothing was stored");
    }
    {   // a cancelled walk is not a result the user can confirm
        World w;
        touch(w.ws / "Out" / "a.txt");
        OutputDirectoryInspection cancelled;
        cancelled.status = OutputInspectionStatus::Cancelled;
        w.inspector.forced = cancelled;
        expectRefusal(w.approve("Out", true, true), kOutputApprovalCancelledCode, "a Cancelled walk with both confirmations");
        check(w.storedCount() == 0, "...stores nothing");
    }
}

// The in-memory project repository whose n-th findById runs a hook first (the repository or the file system may wait there).
class HookProjectRepository final : public IProjectRepository {
public:
    InMemoryProjectRepository inner;
    mutable int reads {0};
    mutable int hookAtRead {0};
    mutable std::function<void()> hook;

    ai::common::Result<Project> insert(Project project) override { return inner.insert(std::move(project)); }
    ai::common::Result<Project> update(Project project) override { return inner.update(std::move(project)); }
    [[nodiscard]] ai::common::Result<std::optional<Project>> findById(const ProjectId& id) const override
    {
        if (++reads == hookAtRead && hook) hook();
        return inner.findById(id);
    }
    [[nodiscard]] ai::common::Result<std::optional<Project>> findByRootPath(const fs::path& rootPath) const override
    {
        return inner.findByRootPath(rootPath);
    }
    [[nodiscard]] ai::common::Result<std::vector<Project>> listAll() const override { return inner.listAll(); }
};

// A stop that arrives while the project is read AGAIN - after the last earlier check, before the commit - must not reach the commit.
void testACancelDuringTheSecondProjectReadNeverCommits()
{
    const fs::path root = makeTempRoot();
    const fs::path ws = root / "ws";
    touch(ws / "Out" / "a.txt");
    HookProjectRepository projects;
    FailingApprovalRepository approvals;
    FunctionProtectedStoragePathSource protectedSource([](const fs::path&) {
        return ai::common::Result<std::vector<ProtectedStoragePath>>::ok(std::vector<ProtectedStoragePath> {});
    });
    HookInspector inspector;
    ProjectOutputApprovalService service(projects, approvals, protectedSource, inspector, OutputDirectoryInspectionLimits {});
    Project project;
    project.id = "p1";
    project.name = "Fixture";
    project.rootPath = fs::canonical(ws);
    project.status = ProjectStatus::Active;
    check(projects.insert(project).success(), "could not create the project");
    OutputApprovalRequest request;
    request.projectId = "p1";
    request.relativePath = "Out";
    request.approvedBy = "tester";

    std::stop_source source;
    projects.hookAtRead = 2;   // read 1 opens the approve, read 2 is the one just before the commit
    projects.hook = [&source] { source.request_stop(); };
    expectRefusal(service.approve(request, source.get_token()), kOutputApprovalCancelledCode, "a stop during the second project read");
    check(projects.reads == 2, "the hook ran on the read it was meant for");
    check(approvals.commitCalls == 0 && approvals.inner.listForProject("p1").value().empty(), "the commit was never called and nothing is stored");

    // The same wiring with no stop requested commits: the hook alone is not what refuses.
    projects.reads = 0;
    projects.hook = [] {};
    expectOk(service.approve(request, std::stop_token {}), "the same approve without a stop");
    check(approvals.commitCalls == 1 && approvals.inner.listForProject("p1").value().size() == 1, "...writes exactly one row");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void testTheInspectorCountsMetadataExactlyWhenComplete()
{
    World w;
    touch(w.ws / "T" / "a.bin", "12345");
    touch(w.ws / "T" / "sub" / "b.bin", "123");
    touch(w.ws / "T" / "sub" / "CMakeLists.txt", "x");
    touch(w.ws / "T" / "sub" / "deeper" / "cmakelists.TXT", "x");
    fs::create_directories(w.ws / "T" / "emptyDir");
    FilesystemOutputDirectoryInspector inspector;
    const auto result = inspector.inspect(w.ws / "T", OutputDirectoryInspectionLimits {}, {});
    check(result.status == OutputInspectionStatus::Complete && !result.hitEntryLimit && !result.hitDepthLimit && result.skippedLinks == 0,
        "A small tree is Complete");
    check(result.files == 4 && result.entries == 7 && result.bytes == 5 + 3 + 1 + 1, "Entries, files and bytes are exact");
#ifdef _WIN32
    check(result.nestedSourceCount == 2, "Names are compared case-insensitively on Windows");
#else
    check(result.nestedSourceCount == 1, "Names are exact elsewhere");
#endif
    OutputDirectoryInspectionLimits samples;
    samples.maxSamples = 1;
    const auto limited = inspector.inspect(w.ws / "T", samples, {});
    check(limited.nestedSourceSamples.size() == 1 && limited.nestedSourceCount == result.nestedSourceCount,
        "The samples are capped, the count is not");
    // A CMakeLists.txt directly in the inspected folder is not 'nested'.
    touch(w.ws / "R" / "CMakeLists.txt");
    check(inspector.inspect(w.ws / "R", OutputDirectoryInspectionLimits {}, {}).nestedSourceCount == 0,
        "The one in the folder itself is the caller's separate refusal");
    // A folder that cannot be listed is Failed, never Complete.
    const auto missing = inspector.inspect(w.ws / "does-not-exist", OutputDirectoryInspectionLimits {}, {});
    check(missing.status == OutputInspectionStatus::Failed && !missing.failure.empty(), "An unlistable folder is a failure");
}

void testProtectedStoragePathsAreNeverOutput()
{
    World w;
    fs::create_directories(w.ws / "data" / "artifacts" / "run1");
    fs::create_directories(w.ws / "data" / "other");
    fs::create_directories(w.ws / "data" / "artifacts2");
    touch(w.ws / "data" / "app.db");
    w.protectedList = {
        {w.ws / "data" / "app.db", false},
        {fs::path(w.ws / "data" / "app.db").concat("-wal"), false},
        {w.ws / "data" / "artifacts", true},
        {w.ws / "data" / "proposals", true},
    };
    expectRefusal(w.approve("data"), kOutputApprovalProtectedStorageCode, "an ancestor of the database file");
    // An existing file is refused as a file before storage is even asked; a protected file that is absent is refused as storage.
    expectRefusal(w.approve("data/app.db"), kOutputApprovalNotADirectoryCode, "the existing database file itself");
    expectRefusal(w.approve("data/app.db-wal"), kOutputApprovalProtectedStorageCode, "the absent write-ahead log file of the database");
    expectRefusal(w.approve("data/artifacts"), kOutputApprovalProtectedStorageCode, "a storage folder");
    expectRefusal(w.approve("data/artifacts/run1"), kOutputApprovalProtectedStorageCode, "inside a storage folder");
    expectRefusal(w.approve("data/proposals"), kOutputApprovalProtectedStorageCode, "a storage folder that does not exist yet");
    expectRefusal(w.approve("data/proposals/p/q"), kOutputApprovalProtectedStorageCode, "inside a storage folder that does not exist yet");
    check(w.approve("data/other").success(), "A sibling of the storage is not protected: the whole parent of the database is not");
    check(w.approve("data/artifacts2").success(), "artifacts2 is not artifacts");
#ifdef _WIN32
    expectRefusal(w.approve("DATA/ARTIFACTS"), kOutputApprovalProtectedStorageCode, "the same folder spelled in another case");
#endif
    // The list is never silently empty.
    w.protectedFails = true;
    expectRefusal(w.approve("Anything"), kOutputApprovalProtectedUnavailableCode, "a provider that cannot answer");
    expectRefusal(w.service.preview("p1", "Anything"), kOutputApprovalProtectedUnavailableCode, "...also for a preview");
    w.protectedFails = false;
    w.protectedList = {{fs::path("relative/path"), true}};
    expectRefusal(w.approve("Anything"), kOutputApprovalProtectedUnavailableCode, "a relative entry in the list");
    // Storage outside the workspace changes nothing.
    w.protectedList = {{w.root / "elsewhere" / "app.db", false}, {w.root / "elsewhere" / "artifacts", true}};
    check(w.approve("Anything").success(), "Storage outside the workspace protects nothing inside it");
    // A link to a storage folder is refused as a link.
    w.protectedList = {{w.ws / "data" / "artifacts", true}};
    if (makeDirectoryLink(w.ws / "alias", w.ws / "data" / "artifacts")) {
        expectRefusal(w.approve("alias"), kOutputApprovalReparsePointCode, "a link to a storage folder");
    } else {
        std::cout << "SKIP: no junction or symbolic link can be made here; the storage-link check was NOT exercised\n";
    }
}

void testTheSetRulesAtTheServiceLevel()
{
    World w;
    check(w.approve("a").success(), "a");
    expectRefusal(w.approve("a/b"), kOutputApprovalNestedCode, "a then a/b");
    check(w.approve("ab").success(), "a and ab are compatible");
    World reverse;
    check(reverse.approve("x/y").success(), "x/y");
    expectRefusal(reverse.approve("x"), kOutputApprovalCoversExistingCode, "x/y then x");
    check(reverse.storedCount() == 1, "...and nothing was added");

    // The limit, and the idempotent repeat at the limit.
    World full;
    for (int index = 0; index < 8; ++index) check(full.approve("D" + std::to_string(index)).success(), "fill");
    const auto first = full.approvals.inner.listForProject("p1").value().front();
    expectRefusal(full.approve("D9"), kOutputApprovalLimitReachedCode, "the ninth");
    const auto repeat = full.approve(first.relativePath);
    check(repeat.success() && repeat.value().outcome == ProjectOutputApprovalCommitOutcome::AlreadyApproved
            && repeat.value().approval.id == first.id,
        "A repeat at the limit is the stored record");
    check(full.storedCount() == 8, "...and the set is still eight");

    // The preview the user saw is information, not a pass.
    World stale;
    touch(stale.ws / "Out" / "a.txt");
    auto request = stale.request("Out");
    request.expectedWorkspaceRoot = detail::genericUtf8(stale.project.rootPath);
    request.expectedRelativePath = "Out";
    check(stale.service.approve(request).success(), "A matching preview is accepted");
    auto other = stale.request("Out2");
    other.expectedRelativePath = "Out";
    expectRefusal(stale.service.approve(other), kOutputApprovalStaleCode, "a different path than previewed");
    auto elsewhere = stale.request("Out3");
    elsewhere.expectedWorkspaceRoot = detail::genericUtf8(stale.root / "another");
    expectRefusal(stale.service.approve(elsewhere), kOutputApprovalStaleCode, "a different root than previewed");
    check(stale.storedCount() == 1, "Nothing stale was written");
}

void testARootChangeDuringTheWalkWritesNothingAndRootsAreIndependent()
{
    World w;
    fs::create_directories(w.root / "ws2");
    touch(w.ws / "Out" / "a.txt");
    // AT5: the project moves to another folder while this one is being walked.
    w.inspector.beforeInspect = [&w](std::stop_token) {
        w.updateProject([&w](Project& p) { p.rootPath = fs::canonical(w.root / "ws2"); });
    };
    expectRefusal(w.approve("Out"), kOutputApprovalStaleCode, "the root changed during the walk");
    w.inspector.beforeInspect = nullptr;
    check(w.storedCount() == 0, "...and nothing was written");

    // AT6: approvals belong to the root they were made for.
    w.updateProject([&w](Project& p) { p.rootPath = fs::canonical(w.ws); });
    check(w.approve("Out").success(), "approve for the first root");
    w.updateProject([&w](Project& p) { p.rootPath = fs::canonical(w.root / "ws2"); });
    const auto dormant = w.service.list("p1");
    check(dormant.success() && dormant.value().size() == 1 && !dormant.value().front().appliesToCurrentRoot,
        "An approval of the earlier root is listed as dormant");
    check(w.service.policyFor("p1", fs::canonical(w.root / "ws2")).value().deniedPaths.empty(),
        "...and it does not apply to the new root");
    check(w.approve("Out").success() && w.storedCount() == 2, "The same relative path can be approved for the new root too");
    w.updateProject([&w](Project& p) { p.rootPath = fs::canonical(w.ws); });
    const auto back = w.service.list("p1");
    check(back.success() && back.value().size() == 2
            && std::count_if(back.value().begin(), back.value().end(), [](const OutputApprovalView& v) { return v.appliesToCurrentRoot; }) == 1,
        "Back on the first root exactly its own approval applies again");
    check(w.service.policyFor("p1", fs::canonical(w.ws)).value().deniedPaths.size() == 1, "...in the policy too");
}

void testRevokeListActorAndUtf8()
{
    World w;
    const std::string utf8Path = "\xD0\xA1\xD0\xB1\xD0\xBE\xD1\x80\xD0\xBA\xD0\xB0/\xD0\xB2\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4";
    const auto approved = w.service.approve(w.request(utf8Path, false, false, ""));
    expectOk(approved, "UTF-8 path with an empty actor");
    check(approved.value().approval.approvedBy.empty(), "An empty actor is stored as empty");
    check(approved.value().approval.relativePath == utf8Path, "The UTF-8 path is stored as it is");

    // A long actor is cut on a character boundary.
    std::string longActor;
    for (int index = 0; index < 200; ++index) longActor += "\xD0\xB6"; // 400 bytes
    const auto cut = w.service.approve(w.request("Other", false, false, longActor));
    expectOk(cut, "long actor");
    const std::string& stored = cut.value().approval.approvedBy;
    check(stored.size() <= 256 && stored.size() % 2 == 0 && !stored.empty(), "The actor is bounded without splitting a character");

    // Another project's record cannot be revoked through this project, and the project must exist.
    Project second = w.project;
    second.id = "p2";
    second.rootPath = fs::canonical(w.root);
    check(w.projects.insert(second).success(), "second project");
    OutputApprovalRequest foreign;
    foreign.projectId = "p2";
    foreign.relativePath = "ForeignOut";
    check(w.service.approve(foreign).success(), "p2 approves its own folder");
    const auto p2Id = w.service.list("p2").value().front().approval.id;
    const auto notMine = w.service.revoke("p1", p2Id);
    check(notMine.success() && !notMine.value(), "A record of another project is not revoked");
    check(w.service.list("p2").value().size() == 1, "...and stays");
    expectRefusal(w.service.revoke("ghost", p2Id), "project.not_found", "revoke for an unknown project");
    expectRefusal(w.service.list("ghost"), "project.not_found", "list for an unknown project");

    const auto id = approved.value().approval.id;
    const auto removed = w.service.revoke("p1", id);
    check(removed.success() && removed.value(), "The owner revokes its record");
    check(w.service.revoke("p1", id).value() == false, "...once");
    w.updateProject([](Project& p) { p.status = ProjectStatus::Archived; });
    check(w.service.revoke("p1", w.service.list("p1").value().front().approval.id).value(), "An archived project can still revoke");
    check(w.service.list("p1").value().empty(), "...and the list is empty");
}

void testPolicyForIsConsistentDeterministicAndFailsClosed()
{
    {   // empty set = exactly the defaults
        World w;
        const auto policy = w.service.policyFor("p1", w.project.rootPath);
        expectOk(policy, "empty set");
        const auto defaults = WorkspaceReadPolicy::secureDefaults(w.project.rootPath);
        check(policy.value().deniedPaths.empty() && policy.value().workspaceRoot == defaults.workspaceRoot
                && policy.value().deniedDirectoryNames == defaults.deniedDirectoryNames
                && policy.value().deniedDirectorySequences == defaults.deniedDirectorySequences
                && policy.value().deniedFileNames == defaults.deniedFileNames
                && policy.value().deniedExtensions == defaults.deniedExtensions,
            "An empty set is exactly secureDefaults");
    }
    {   // the order does not depend on the order of approval; a missing folder is fine
        World a;
        World b;
        for (const char* path : {"Zeta", "Alpha", "Mid/dle"}) check(a.approve(path).success(), "approve");
        for (const char* path : {"Mid/dle", "Zeta", "Alpha"}) check(b.approve(path).success(), "approve");
        const auto first = a.service.policyFor("p1", a.project.rootPath);
        const auto second = b.service.policyFor("p1", b.project.rootPath);
        expectOk(first, "policy a");
        expectOk(second, "policy b");
        std::vector<std::string> firstPaths;
        std::vector<std::string> secondPaths;
        for (const auto& p : first.value().deniedPaths) firstPaths.push_back(detail::genericUtf8(p));
        for (const auto& p : second.value().deniedPaths) secondPaths.push_back(detail::genericUtf8(p));
        check(firstPaths == secondPaths && firstPaths.size() == 3, "The set is ordered by key, whatever the order of approval was");
        check(!fs::exists(a.ws / "Zeta"), "The producer creates nothing");
    }
    {   // a storage failure is an error, never "no exclusions"
        World w;
        check(w.approve("Out").success(), "approve");
        w.approvals.failReads = true;
        const auto failed = w.service.policyFor("p1", w.project.rootPath);
        expectRefusal(failed, kOutputApprovalStorageFailedCode, "a failing read");
        w.approvals.failReads = false;
        check(w.service.policyFor("p1", w.project.rootPath).value().deniedPaths.size() == 1, "...and it works again afterwards");
    }
    {   // an unusable workspace
        World w;
        expectRefusal(w.service.policyFor("p1", w.root / "nowhere"), kOutputApprovalWorkspaceUnavailableCode, "missing root");
        expectRefusal(w.service.policyFor("p1", fs::path("relative")), kOutputApprovalWorkspaceUnavailableCode, "relative root");
    }
    {   // a broken row and a broken set, written behind the service's back
        World w;
        ProjectOutputApproval row;
        row.id = "raw1";
        row.projectId = "p1";
        row.workspaceRoot = detail::genericUtf8(w.project.rootPath);
        row.workspaceKey = outputApprovalWorkspaceKey(w.project.rootPath);
        row.relativePath = "Out";
        row.pathKey = "not-the-key";
        row.inspectionComplete = true;
        check(w.approvals.inner.commit(row, nullptr).success(), "raw row");
        expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "a row whose key does not match its path");
    }
    {
        World w;
        for (const char* path : {"a", "a/b"}) {
            ProjectOutputApproval row;
            row.id = std::string("raw-") + path;
            row.projectId = "p1";
            row.workspaceRoot = detail::genericUtf8(w.project.rootPath);
            row.workspaceKey = outputApprovalWorkspaceKey(w.project.rootPath);
            row.relativePath = path;
            row.pathKey = outputApprovalPathKey(path);
            row.inspectionComplete = true;
            check(w.approvals.inner.commit(row, nullptr).success(), "raw row");
        }
        expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "a nested set");
    }
}

void testPolicyForRefusesAnApprovalThatBecameUnsafe()
{
    // PS6 at the service level: the real Run producer arrives later; here the refusal itself is pinned.
    {
        World w;
        check(w.approve("Out").success(), "approve a folder that does not exist yet");
        check(w.service.policyFor("p1", w.project.rootPath).success(), "...it is fine while it is absent");
        fs::create_directories(w.ws / "Out" / "deeper");
        touch(w.ws / "Out" / "deeper" / "CMakeLists.txt");
        check(w.service.policyFor("p1", w.project.rootPath).success(),
            "Nested sources found later are NOT re-asked: policyFor does not walk");
        touch(w.ws / "Out" / "CMakeLists.txt");
        expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "a CMakeLists.txt appeared in the folder");
    }
    {
        World w;
        check(w.approve("Out").success(), "approve");
        touch(w.ws / "Out");
        const auto result = w.service.policyFor("p1", w.project.rootPath);
        expectRefusal(result, kOutputApprovalInvalidCode, "the folder became a file");
        check(result.error()->message.find(kOutputApprovalNotADirectoryCode) != std::string::npos, "...and the reason is named");
    }
    {
        World w;
        fs::create_directories(w.ws / "Out");
        check(w.approve("Out").success(), "approve");
        fs::remove_all(w.ws / "Out");
        fs::create_directories(w.root / "elsewhere");
        if (makeDirectoryLink(w.ws / "Out", w.root / "elsewhere")) {
            const auto result = w.service.policyFor("p1", w.project.rootPath);
            expectRefusal(result, kOutputApprovalInvalidCode, "the folder became a link");
            check(result.error()->message.find(kOutputApprovalReparsePointCode) != std::string::npos, "...and the reason is named");
        } else {
            std::cout << "SKIP: no junction or symbolic link can be made here; the became-a-link check was NOT exercised\n";
        }
    }
    {   // the protected storage grew over an approval
        World w;
        check(w.approve("Out").success(), "approve");
        w.protectedList = {{w.ws / "Out", true}};
        expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "an approval over protected storage");
        w.protectedList.clear();
        w.protectedFails = true;
        expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "a provider that cannot answer");
    }
}

// R3 of the P3a review: a stored record must agree with itself and with the root asked for - workspace_root, workspace_key and the
// requested root name ONE root - or it is rejected, never taken as valid because one of the three matches.
ProjectOutputApproval rawRecord(const World& w, const std::string& id, const std::string& path, const std::string& root,
    const std::string& key)
{
    ProjectOutputApproval row;
    row.id = id;
    row.projectId = "p1";
    row.workspaceRoot = root;
    row.workspaceKey = key;
    row.relativePath = path;
    row.pathKey = outputApprovalPathKey(path);
    row.inspectionComplete = true;
    (void)w;
    return row;
}

void testPolicyForRejectsARecordThatDisagreesWithItself()
{
    World w;
    const std::string realRoot = detail::genericUtf8(w.project.rootPath);
    const std::string realKey = outputApprovalWorkspaceKey(w.project.rootPath);

    // The root names another folder, the key is the requested one: the reported failure.
    check(w.approvals.inner.commit(rawRecord(w, "bad1", "Out", detail::genericUtf8(w.root / "other-workspace"), realKey), nullptr).success(),
        "raw row");
    auto mismatch = w.service.policyFor("p1", w.project.rootPath);
    expectRefusal(mismatch, kOutputApprovalInvalidCode, "root and key naming different folders");
    check(mismatch.error()->message.find("workspace root") != std::string::npos, "...and the reason is the root/key disagreement");
    check(w.approvals.inner.revoke("p1", "bad1").value(), "clean up the raw row");

    // Data that got past the table's own checks: an empty and a relative root.
    check(w.approvals.inner.commit(rawRecord(w, "bad2", "Out", "", realKey), nullptr).success(), "raw row");
    expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "an empty root");
    check(w.approvals.inner.revoke("p1", "bad2").value(), "clean up");
    check(w.approvals.inner.commit(rawRecord(w, "bad3", "Out", "ws", realKey), nullptr).success(), "raw row");
    expectRefusal(w.service.policyFor("p1", w.project.rootPath), kOutputApprovalInvalidCode, "a relative root");
    check(w.approvals.inner.revoke("p1", "bad3").value(), "clean up");

    // The key is right but the root names another folder (not the key's), and the root is right but the key is another one.
    check(w.approvals.inner.commit(rawRecord(w, "bad4", "Out", realRoot, outputApprovalWorkspaceKey(w.root / "elsewhere")), nullptr).success(),
        "raw row");
    check(w.service.policyFor("p1", w.project.rootPath).value().deniedPaths.empty(),
        "A record listed under another key is not the requested root's at all");
    check(w.approvals.inner.revoke("p1", "bad4").value(), "clean up");

    // A record of another project cannot be listed for this one.
    ProjectOutputApproval foreign = rawRecord(w, "bad5", "Out", realRoot, realKey);
    foreign.projectId = "p2";
    check(w.approvals.inner.commit(foreign, nullptr).success(), "raw row");
    check(w.service.policyFor("p1", w.project.rootPath).value().deniedPaths.empty(), "Another project's record is not read");
    check(w.approvals.inner.revoke("p2", "bad5").value(), "clean up");

    // An honest record still passes, also after the root moved and came back.
    check(w.approvals.inner.commit(rawRecord(w, "ok1", "Out", realRoot, realKey), nullptr).success(), "honest row");
    check(w.service.policyFor("p1", w.project.rootPath).value().deniedPaths.size() == 1, "An honest record is used");
    fs::create_directories(w.root / "moved");
    check(w.service.policyFor("p1", fs::canonical(w.root / "moved")).value().deniedPaths.empty(),
        "...and is not used for another root (it is dormant there)");
    check(w.service.policyFor("p1", w.project.rootPath).value().deniedPaths.size() == 1, "...and again for the original root");
#ifdef _WIN32
    // The same root spelled in another case is the same root on Windows: the record stays valid.
    std::string upper = realRoot;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    check(w.approvals.inner.revoke("p1", "ok1").value(), "replace the honest row");
    check(w.approvals.inner.commit(rawRecord(w, "ok2", "Out", upper, outputApprovalWorkspaceKey(detail::pathFromUtf8(upper))), nullptr).success(),
        "row with the root in another case");
    check(w.service.policyFor("p1", w.project.rootPath).value().deniedPaths.size() == 1,
        "A root spelled in another case is accepted on Windows");
#endif
}

void testPolicyForOverARealSqliteRow()
{
    World w;
    const fs::path database = w.root / "approvals.sqlite3";
    const auto opened = ai::infrastructure::SqliteDatabase::open(database);
    check(opened.success(), "could not open the temporary database");
    ai::infrastructure::SqliteProjectRepository projects(opened.value());
    ai::infrastructure::SqliteProjectOutputApprovalRepository approvals(opened.value());
    Project project = w.project;
    project.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(500));
    project.updatedAt = project.createdAt;
    check(projects.insert(project).success(), "could not store the project");
    ProjectOutputApprovalService service(projects, approvals, w.protectedSource, w.inspector);

    check(service.approve([&] { auto r = w.request("Out"); return r; }()).success(), "approve through the service");
    const auto good = service.policyFor("p1", project.rootPath);
    expectOk(good, "policyFor over the stored row");
    check(good.value().deniedPaths.size() == 1, "...uses it");

    // A real row whose root and key disagree (valid for the table's CHECKs, wrong for the record).
    const auto realKey = outputApprovalWorkspaceKey(project.rootPath);
    check(approvals.commit(rawRecord(w, "bad", "Other", detail::genericUtf8(w.root / "other-workspace"), realKey), nullptr).success(),
        "raw row in SQLite");
    expectRefusal(service.policyFor("p1", project.rootPath), kOutputApprovalInvalidCode, "a SQLite row with a root/key disagreement");
    check(approvals.revoke("p1", "bad").value(), "clean up");
    check(service.policyFor("p1", project.rootPath).value().deniedPaths.size() == 1, "...and the honest row works again");

    // Root change and return, over SQLite.
    fs::create_directories(w.root / "second");
    Project moved = project;
    moved.rootPath = fs::canonical(w.root / "second");
    check(projects.update(moved).success(), "move the project");
    check(service.policyFor("p1", moved.rootPath).value().deniedPaths.empty(), "The dormant row does not apply to the new root");
    check(service.list("p1").value().front().appliesToCurrentRoot == false, "...and is listed as dormant");
    check(projects.update(project).success(), "move the project back");
    check(service.policyFor("p1", project.rootPath).value().deniedPaths.size() == 1, "...and applies again when the root is back");
}

void testConcurrentApprovesRevokesAndReadsStayConsistent()
{
    for (int round = 0; round < 20; ++round) {
        World w;
        const auto results = contract::runTogether<bool>({
            [&] { return w.approve("a").success(); },
            [&] { return w.approve("a/b").success(); }});
        check(static_cast<int>(results[0]) + static_cast<int>(results[1]) == 1, "AT1 at the service: exactly one of a and a/b");
        check(w.service.policyFor("p1", w.project.rootPath).success(), "...and the stored set is a valid policy");
    }
    World w;
    std::atomic<bool> stop {false};
    std::atomic<int> bad {0};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto policy = w.service.policyFor("p1", w.project.rootPath);
            if (!policy.success() || policy.value().deniedPaths.size() > 1) ++bad;
        }
    });
    for (int index = 0; index < 100; ++index) {
        const auto approved = w.approve("Out");
        check(approved.success(), "approve while another thread reads");
        check(w.service.revoke("p1", approved.value().approval.id).success(), "revoke while another thread reads");
    }
    stop = true;
    reader.join();
    check(bad.load() == 0, "A concurrent reader only ever sees a consistent set (none or one), never an error");
}

} // namespace

int runProjectOutputApprovalServiceTests()
{
    try {
        testTheInMemoryRepositoryKeepsTheSameContract();
        testAnOutputThatDoesNotExistYetCanBeApproved();
        testHardRefusalsAndTheirIndependenceOfTheConfirmations();
        testNestedSourcesNeedTheirOwnConfirmationAndNothingIsSkippedByName();
        testAnIncompleteWalkIsNeverCompleteAndNeedsItsOwnConfirmation();
        testTheTwoConfirmationsAreIndependentAndNeverLiftAHardRefusal();
        testCancellationNeverWrites();
        testACancelDuringTheSecondProjectReadNeverCommits();
        testTheInspectorCountsMetadataExactlyWhenComplete();
        testProtectedStoragePathsAreNeverOutput();
        testTheSetRulesAtTheServiceLevel();
        testARootChangeDuringTheWalkWritesNothingAndRootsAreIndependent();
        testRevokeListActorAndUtf8();
        testPolicyForIsConsistentDeterministicAndFailsClosed();
        testPolicyForRefusesAnApprovalThatBecameUnsafe();
        testPolicyForRejectsARecordThatDisagreesWithItself();
        testPolicyForOverARealSqliteRow();
        testConcurrentApprovesRevokesAndReadsStayConsistent();
    } catch (const std::exception& exception) {
        std::cerr << "ProjectOutputApproval tests failed: " << exception.what() << std::endl;
        return 1;
    }
    std::cout << "ProjectOutputApproval tests passed" << std::endl;
    return 0;
}
