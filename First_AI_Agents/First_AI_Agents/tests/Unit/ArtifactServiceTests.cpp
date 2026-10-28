#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/artifacts/ArtifactPathPolicy.hpp"
#include "ai/application/context/ContextSnapshot.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/execution/MockWorkflowRunner.hpp"
#include "ai/application/execution/OrchestrationService.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/FileArtifactStore.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace ai::application;

class RejectingArtifactRepository final : public IArtifactRepository {
public:
    ai::common::Result<Artifact> insertArtifact(Artifact) override
    {
        return ai::common::Result<Artifact>::failure(
            {"storage.unavailable", "metadata insert rejected for compensation test"});
    }
    ai::common::Result<std::optional<Artifact>> findArtifact(const ArtifactId&) const override
    {
        return ai::common::Result<std::optional<Artifact>>::ok(std::nullopt);
    }
    ai::common::Result<std::vector<Artifact>> listArtifacts(const RunId&) const override
    {
        return ai::common::Result<std::vector<Artifact>>::ok({});
    }
};

class CorruptingArtifactStore final : public IArtifactStore {
public:
    ai::common::Result<StoredArtifactFile> writeArtifact(
        const RunId& runId, std::string_view suggestedName,
        std::string_view content) override
    {
        return inner_.writeArtifact(runId, suggestedName, content);
    }

    ai::common::Result<void> removeArtifact(
        const std::filesystem::path& relativePath) override
    {
        return inner_.removeArtifact(relativePath);
    }

    ai::common::Result<std::filesystem::path> resolveAbsolutePath(
        const std::filesystem::path& relativePath) const override
    {
        return inner_.resolveAbsolutePath(relativePath);
    }

    ai::common::Result<std::string> readArtifact(
        const std::filesystem::path& relativePath) const override
    {
        auto content = inner_.readArtifact(relativePath);
        if (content.success() && corruptReads_ && !content.value().empty()) {
            content.value().front() = content.value().front() == 'x' ? 'y' : 'x';
        }
        return content;
    }

    void corruptReads() { corruptReads_ = true; }

private:
    InMemoryArtifactStore inner_;
    bool corruptReads_ {false};
};

struct TempProjectDirectory {
    fs::path path;

    TempProjectDirectory()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-artifact-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempProjectDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

Run waitForTerminalRun(IRunRepository& repository, const RunId& runId, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        assert(found.success());
        assert(found.value().has_value());
        if (isTerminal(found.value()->status)) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    assert(false && "Run did not reach a terminal state before timeout");
    return {};
}

void testCreateListGetAndResolveArtifact()
{
    InMemoryArtifactRepository repository;
    InMemoryArtifactStore store;
    ArtifactService service(repository, store);

    const auto empty = service.listArtifacts("no-such-run");
    assert(empty.success() && empty.value().empty());

    const auto created = service.createArtifact(
        "run-1", std::nullopt, ArtifactKind::Report, "notes.md", "text/markdown", "hello artifact");
    assert(created.success());
    assert(created.value().runId == "run-1");
    assert(created.value().kind == ArtifactKind::Report);
    assert(created.value().byteSize == std::string("hello artifact").size());
    assert(!created.value().sha256.empty());
    assert(!created.value().id.empty());

    const auto listed = service.listArtifacts("run-1");
    assert(listed.success() && listed.value().size() == 1);
    assert(listed.value().front().id == created.value().id);

    const auto fetched = service.getArtifact(created.value().id);
    assert(fetched.success() && fetched.value().name == "notes.md");

    const auto missing = service.getArtifact("does-not-exist");
    assert(!missing.success() && missing.error()->code == "artifact.not_found");

    const auto resolved = service.resolveArtifactPath(created.value().id);
    assert(resolved.success());
    assert(resolved.value() == created.value().relativePath);
}

void testGetArtifactContentRoundTripsThroughStore()
{
    InMemoryArtifactRepository repository;
    InMemoryArtifactStore store;
    ArtifactService service(repository, store);

    const auto created = service.createArtifact(
        "run-content", std::nullopt, ArtifactKind::ContextSnapshot,
        "context.md", "text/markdown", "the full context text");
    assert(created.success());

    const auto content = service.getArtifactContent(created.value().id);
    assert(content.success());
    assert(content.value() == "the full context text");

    const auto missing = service.getArtifactContent("does-not-exist");
    assert(!missing.success() && missing.error()->code == "artifact.not_found");
}

void testGetArtifactContentRejectsCorruptedPayload()
{
    InMemoryArtifactRepository repository;
    CorruptingArtifactStore store;
    ArtifactService service(repository, store);

    const auto created = service.createArtifact(
        "run-corrupt", std::nullopt, ArtifactKind::Report,
        "verification.md", "text/markdown", "trusted verification record");
    assert(created.success());

    store.corruptReads();
    const auto content = service.getArtifactContent(created.value().id);
    assert(!content.success());
    assert(content.error()->code == "artifact.integrity_mismatch");
}

void testListArtifactsIsDeterministicallyOrdered()
{
    InMemoryArtifactRepository repository;
    InMemoryArtifactStore store;
    ArtifactService service(repository, store);

    for (int index = 0; index < 5; ++index) {
        const auto created = service.createArtifact(
            "ordered-run", std::nullopt, ArtifactKind::Log,
            "log-" + std::to_string(index) + ".txt", "text/plain", "content " + std::to_string(index));
        assert(created.success());
    }

    // Two independent listArtifacts calls against the same unmodified data
    // must return the exact same order every time.
    const auto first = service.listArtifacts("ordered-run");
    const auto second = service.listArtifacts("ordered-run");
    assert(first.success() && second.success());
    assert(first.value().size() == 5 && second.value().size() == 5);
    for (std::size_t index = 0; index < first.value().size(); ++index) {
        assert(first.value()[index].id == second.value()[index].id);
    }
}

void testArtifactNameSanitizationRejectsTraversal()
{
    InMemoryArtifactRepository repository;
    InMemoryArtifactStore store;
    ArtifactService service(repository, store);

    const auto created = service.createArtifact(
        "traversal-run", std::nullopt, ArtifactKind::Other,
        "../../etc/passwd", "text/plain", "should not escape");
    assert(created.success());
    // Every '/' and '\' in the suggested name is sanitized to '_' before it
    // ever reaches a path, so the built relative path can contain exactly
    // one directory separator: the one this policy inserts itself between
    // the run id segment and the file name segment. A literal ".." can
    // still appear as harmless text inside the single sanitized filename
    // component (e.g. "..__.._etc_passwd") without creating any traversal,
    // because there is no separator left for it to act as a path component.
    const std::string relative = created.value().relativePath.generic_string();
    assert(std::count(relative.begin(), relative.end(), '/') == 1);
    assert(relative.find('\\') == std::string::npos);
    const std::string nameSegment = relative.substr(relative.find('/') + 1);
    assert(nameSegment.find('/') == std::string::npos && nameSegment.find('\\') == std::string::npos);
}

void testMetadataFailureRemovesWrittenFile()
{
    RejectingArtifactRepository repository;
    InMemoryArtifactStore store;
    ArtifactService service(repository, store);

    const auto created = service.createArtifact(
        "rollback-run", std::nullopt, ArtifactKind::Report,
        "rollback.md", "text/markdown", "must be removed");
    assert(!created.success());
    assert(created.error()->code == "storage.unavailable");

    const auto relativePath = buildArtifactRelativePath("rollback-run", "rollback.md", 0);
    const auto resolved = store.resolveAbsolutePath(relativePath);
    assert(!resolved.success() && resolved.error()->code == "artifact.not_found");
}

void testContextSnapshotBudgetIsExplicitNotSilentTruncation()
{
    std::string description(500, 'd');
    std::string task(500, 't');

    const auto withinBudget = buildRunContextSnapshot(
        "run-x", "project-x", description, task, {}, 2000);
    assert(withinBudget.success());
    assert(withinBudget.value().totalByteCount == description.size() + task.size());

    const auto overBudget = buildRunContextSnapshot(
        "run-x", "project-x", description, task, {}, 100);
    assert(!overBudget.success());
    assert(overBudget.error()->code == "context_snapshot.budget_exceeded");

    const auto noBudget = buildRunContextSnapshot("run-x", "project-x", description, task, {});
    assert(noBudget.success());

    const std::string rendered = renderRunContextSnapshot(noBudget.value());
    assert(rendered.find(description) != std::string::npos);
    assert(rendered.find(task) != std::string::npos);

    const std::string memorySection =
        "# Project Memory\n\nA durable prior decision";
    const auto withMemory = buildRunContextSnapshot(
        "run-memory", "project-memory", description, task, {}, std::nullopt,
        memorySection, {"segment-a", "segment-b"});
    assert(withMemory.success());
    assert(withMemory.value().totalByteCount
        == description.size() + task.size() + memorySection.size());
    const std::string memoryRendered =
        renderRunContextSnapshot(withMemory.value());
    assert(memoryRendered.find(memorySection) != std::string::npos);
    assert(memoryRendered.find("segment-a") != std::string::npos);
    assert(memoryRendered.find("segment-b") != std::string::npos);
}

void testMockRunProducesReviewableArtifacts()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifactService(artifactRepository, artifactStore);

    ProjectService projectService(projects);
    TempProjectDirectory directory;
    const auto project = projectService.createProject(
        {"Artifact project", "Verify Mock run artifacts", directory.path});
    assert(project.success());

    MockWorkflowRunner workflow(1ms, 1, &artifactService);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver);
    OrchestrationService orchestration(projects, runs, manager);
    const auto started = orchestration.startRun(
        {project.value().id, "Artifact run", "Produce a reviewable artifact"});
    assert(started.success());
    const RunId runId = started.value();

    const Run finished = waitForTerminalRun(runs, runId);
    assert(finished.status == RunStatus::Completed);

    const auto artifacts = artifactService.listArtifacts(runId);
    assert(artifacts.success());
    // At least the context-snapshot (after stage 0) and the final report.
    assert(artifacts.value().size() >= 2);

    bool hasContextSnapshot = false;
    bool hasReport = false;
    for (const auto& artifact : artifacts.value()) {
        assert(artifact.runId == runId);
        assert(artifact.byteSize > 0);
        assert(!artifact.sha256.empty());
        if (artifact.kind == ArtifactKind::ContextSnapshot) hasContextSnapshot = true;
        if (artifact.kind == ArtifactKind::Report) hasReport = true;
    }
    assert(hasContextSnapshot && "Mock run did not produce a context_snapshot artifact");
    assert(hasReport && "Mock run did not produce a report artifact");
}

void testMockRunWithoutArtifactServiceStillCompletes()
{
    // ArtifactService is optional: existing/older composition roots that
    // never pass one must keep working exactly as before.
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ProjectService projectService(projects);
    TempProjectDirectory directory;
    const auto project = projectService.createProject(
        {"No artifact project", "No artifact service configured", directory.path});
    assert(project.success());

    MockWorkflowRunner workflow(1ms, 1);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver);
    OrchestrationService orchestration(projects, runs, manager);
    const auto started = orchestration.startRun(
        {project.value().id, "No artifact run", "Complete without an artifact service"});
    assert(started.success());

    const Run finished = waitForTerminalRun(runs, started.value());
    assert(finished.status == RunStatus::Completed);
}

// ---------------------------------------------------------------------------
// Run warnings: a lost DIAGNOSTIC artifact leaves a durable, deduplicated warning; a mandatory one does not
// ---------------------------------------------------------------------------

// A store that loses every write (or only some names), with an error message of a chosen size.
class LosingArtifactStore final : public IArtifactStore {
public:
    bool failAll {true};
    std::string errorMessage {"disk is full"};

    ai::common::Result<StoredArtifactFile> writeArtifact(
        const RunId& runId, std::string_view suggestedName, std::string_view content) override
    {
        if (failAll) {
            return ai::common::Result<StoredArtifactFile>::failure({"artifact_store.write_failed", errorMessage});
        }
        return inner_.writeArtifact(runId, suggestedName, content);
    }
    ai::common::Result<void> removeArtifact(const std::filesystem::path& relativePath) override
    {
        return inner_.removeArtifact(relativePath);
    }
    ai::common::Result<std::filesystem::path> resolveAbsolutePath(const std::filesystem::path& relativePath) const override
    {
        return inner_.resolveAbsolutePath(relativePath);
    }
    ai::common::Result<std::string> readArtifact(const std::filesystem::path& relativePath) const override
    {
        return inner_.readArtifact(relativePath);
    }

private:
    InMemoryArtifactStore inner_;
};

class RejectingWarningRepository final : public IRunWarningRepository {
public:
    ai::common::Result<bool> recordWarning(RunWarning) override
    {
        return ai::common::Result<bool>::failure({"storage.unavailable", "warning insert rejected"});
    }
    ai::common::Result<std::vector<RunWarning>> listWarnings(const RunId&) const override
    {
        return ai::common::Result<std::vector<RunWarning>>::ok({});
    }
};

struct ObservedWarning {
    RunWarning warning;
    std::optional<ai::common::Error> recordError;
};

void testLostDiagnosticArtifactIsRecordedOnceAndAnnounced()
{
    InMemoryArtifactRepository repository;
    LosingArtifactStore store;
    InMemoryRunWarningRepository warnings;
    ArtifactService service(repository, store, &warnings);
    std::vector<ObservedWarning> observed;
    service.setWarningObserver([&observed](const RunWarning& warning, const std::optional<ai::common::Error>& recordError) {
        observed.push_back({warning, recordError});
    });

    // A diagnostic artifact that IS stored leaves no warning and no announcement.
    store.failAll = false;
    const auto stored = service.createDiagnosticArtifact(
        "run-w", "stage-1", ArtifactKind::Report, "kept.md", "text/markdown", "kept");
    assert(stored.success());
    assert(service.listWarnings("run-w").value().empty() && observed.empty());

    store.failAll = true;
    const auto lost = service.createDiagnosticArtifact(
        "run-w", "stage-1", ArtifactKind::Report, "lost.md", "text/markdown", "lost");
    // The caller still sees the store's own error (diagnostic callers discard it; tests and logs do not).
    assert(!lost.success() && lost.error()->code == "artifact_store.write_failed");
    const auto listed = service.listWarnings("run-w");
    assert(listed.success() && listed.value().size() == 1);
    const RunWarning& warning = listed.value().front();
    assert(warning.code == kRunWarningDiagnosticArtifactLostCode);
    assert(warning.runId == "run-w" && warning.stageId == std::optional<StageId>("stage-1"));
    assert(warning.artifactName == "lost.md" && !warning.id.empty());
    assert(warning.message.find("lost.md") != std::string::npos
        && warning.message.find("artifact_store.write_failed") != std::string::npos
        && warning.message.find("disk is full") != std::string::npos);
    // Recorded first, announced after: the observer saw the very warning that is durable, with no record error.
    assert(observed.size() == 1 && !observed.front().recordError.has_value());
    assert(observed.front().warning.id == warning.id);

    // The same loss again (a retried write, a re-processed stage) is neither a second row nor a second announcement.
    const auto again = service.createDiagnosticArtifact(
        "run-w", "stage-1", ArtifactKind::Report, "lost.md", "text/markdown", "lost");
    assert(!again.success() && again.error()->code == "artifact_store.write_failed");
    assert(service.listWarnings("run-w").value().size() == 1 && observed.size() == 1);

    // A different artifact, another stage of the same artifact name, and another Run are different losses.
    assert(!service.createDiagnosticArtifact(
        "run-w", "stage-1", ArtifactKind::Report, "other.md", "text/markdown", "x").success());
    assert(!service.createDiagnosticArtifact(
        "run-w", "stage-2", ArtifactKind::Report, "lost.md", "text/markdown", "x").success());
    assert(!service.createDiagnosticArtifact(
        "run-w", std::nullopt, ArtifactKind::Report, "lost.md", "text/markdown", "x").success());
    assert(!service.createDiagnosticArtifact(
        "run-v", "stage-1", ArtifactKind::Report, "lost.md", "text/markdown", "x").success());
    assert(service.listWarnings("run-w").value().size() == 4);
    assert(service.listWarnings("run-v").value().size() == 1);
    assert(observed.size() == 5);

    // A MANDATORY artifact keeps createArtifact(): its failure is returned to the stage that needs it and is NOT a warning.
    assert(!service.createArtifact(
        "run-m", "stage-1", ArtifactKind::Report, "needed.json", "application/json", "{}").success());
    assert(service.listWarnings("run-m").value().empty() && observed.size() == 5);
}

void testRunWarningMessageIsBoundedOnACharacterBoundary()
{
    InMemoryArtifactRepository repository;
    LosingArtifactStore store;
    store.errorMessage.clear();
    for (int index = 0; index < 3000; ++index) store.errorMessage += "\xD1\x8F";   // 6000 bytes of the letter "ya"
    InMemoryRunWarningRepository warnings;
    ArtifactService service(repository, store, &warnings);

    assert(!service.createDiagnosticArtifact(
        "run-b", std::nullopt, ArtifactKind::Report, "big.md", "text/markdown", "x").success());
    const auto listed = service.listWarnings("run-b");
    assert(listed.success() && listed.value().size() == 1);
    const std::string& message = listed.value().front().message;
    assert(!message.empty() && message.size() <= kRunWarningMessageMaxBytes);
    // Never half of a two-byte letter: the cut falls where a character ends.
    std::size_t continuationBytes = 0;
    for (const char byte : message) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) == 0x80U) ++continuationBytes;
    }
    const std::size_t lead = std::count_if(message.begin(), message.end(),
        [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) == 0xC0U; });
    assert(lead == continuationBytes && "a letter was cut in half");

    assert(boundedUtf8("abcdef", 3) == "abc");
    assert(boundedUtf8("abc", 10) == "abc");
    assert(boundedUtf8("a\xD1\x8F", 2) == "a");   // the second byte of the letter would not fit: drop it whole
    assert(boundedUtf8("", 4).empty());
}

void testWarningThatCannotBeRecordedIsNeverSilent()
{
    InMemoryArtifactRepository repository;
    LosingArtifactStore store;
    RejectingWarningRepository rejecting;
    ArtifactService service(repository, store, &rejecting);
    std::vector<ObservedWarning> observed;
    service.setWarningObserver([&observed](const RunWarning& warning, const std::optional<ai::common::Error>& recordError) {
        observed.push_back({warning, recordError});
    });

    const auto lost = service.createDiagnosticArtifact(
        "run-u", "stage-1", ArtifactKind::Report, "lost.md", "text/markdown", "x");
    // Both the loss and the failure to record it are in the answer, and the observer is told that the warning is NOT durable.
    assert(!lost.success() && lost.error()->code == "artifact.diagnostic_warning_unrecorded");
    assert(lost.error()->message.find("disk is full") != std::string::npos
        && lost.error()->message.find("warning insert rejected") != std::string::npos);
    assert(observed.size() == 1 && observed.front().recordError.has_value()
        && observed.front().recordError->code == "storage.unavailable");
    assert(observed.front().warning.artifactName == "lost.md");

    // Without any warning store the loss is still announced, and still reported as not recorded.
    ArtifactService withoutStore(repository, store);
    std::vector<ObservedWarning> observedWithout;
    withoutStore.setWarningObserver(
        [&observedWithout](const RunWarning& warning, const std::optional<ai::common::Error>& recordError) {
            observedWithout.push_back({warning, recordError});
        });
    const auto lostWithout = withoutStore.createDiagnosticArtifact(
        "run-u", std::nullopt, ArtifactKind::Report, "lost.md", "text/markdown", "x");
    assert(!lostWithout.success() && lostWithout.error()->code == "artifact.diagnostic_warning_unrecorded");
    assert(observedWithout.size() == 1 && observedWithout.front().recordError.has_value());
    assert(withoutStore.listWarnings("run-u").success() && withoutStore.listWarnings("run-u").value().empty());
}

void testMockRunWithLostDiagnosticArtifactsCompletesWithWarnings()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryArtifactRepository artifactRepository;
    LosingArtifactStore artifactStore;
    InMemoryRunWarningRepository warnings;
    ArtifactService artifactService(artifactRepository, artifactStore, &warnings);

    ProjectService projectService(projects);
    TempProjectDirectory directory;
    const auto project = projectService.createProject(
        {"Warning project", "Verify warnings of a Mock run", directory.path});
    assert(project.success());

    MockWorkflowRunner workflow(1ms, 1, &artifactService);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver);
    OrchestrationService orchestration(projects, runs, manager);
    const auto started = orchestration.startRun(
        {project.value().id, "Lossy run", "Lose every artifact"});
    assert(started.success());

    const Run finished = waitForTerminalRun(runs, started.value());
    // Neither the lost context snapshot nor the lost report fails the Run ...
    assert(finished.status == RunStatus::Completed && finished.errorMessage.empty());
    assert(artifactService.listArtifacts(started.value()).value().empty());
    // ... and both leave a warning (the one thing that remains of them).
    const auto listed = artifactService.listWarnings(started.value());
    assert(listed.success() && listed.value().size() == 2);
    bool snapshot = false;
    bool report = false;
    for (const auto& warning : listed.value()) {
        if (warning.artifactName == "context-snapshot.md") snapshot = warning.stageId.has_value();
        if (warning.artifactName == "mock-run-report.md") report = !warning.stageId.has_value();
    }
    assert(snapshot && report);
}

// ---------------------------------------------------------------------------
// FileArtifactStore: paths longer than the legacy 260-character limit
//
// An artifact lives at <root>/<run id>/<ms>-<n>-<name>. The whole cycle - open, write, read, reopen, write again, remove - must
// work when the file path, then a directory path, then the root itself is longer than MAX_PATH, and the containment check must
// still refuse a path that leaves the root. Before the store used the extended-length form (ai::application::detail::longPath)
// it failed with artifact_store.write_failed / directory_failed for this very structure from a root of about 200 characters on
// (measured for THIS path structure only: no universal "safe length" follows from it).
// ---------------------------------------------------------------------------

struct LongArtifactRoot {
    fs::path base;
    fs::path root;

    explicit LongArtifactRoot(std::size_t minimumLength)
    {
        base = fs::temp_directory_path()
            / ("aio-artifact-long-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base;
        int segment = 0;
        while (root.native().size() + 12 < minimumLength) root /= "seg" + std::to_string(segment++) + std::string(36, 'x');
        root /= "artifacts";
    }
    ~LongArtifactRoot()
    {
        std::error_code ec;
        fs::remove_all(detail::longPath(base), ec);
        assert(!fs::exists(detail::longPath(base)) && "the long test tree must be removed completely");
    }
};

template <typename ResultType>
void requireArtifactOk(const ResultType& result, const char* step, std::size_t rootLength)
{
    if (!result.success()) {
        std::fprintf(stderr, "[long-path artifact store, root >= %zu] %s failed: %s / %s\n", rootLength, step,
            result.error()->code.c_str(), result.error()->message.c_str());
    }
    assert(result.success() && "see the line above for the failing step and its error");
}

void runArtifactStoreLongPathCycle(std::size_t minimumRootLength)
{
    LongArtifactRoot tree(minimumRootLength);
    const std::string runId = "11111111-aaaa-bbbb-cccc-000000000001";
    fs::path firstPath;
    {
        auto opened = ai::infrastructure::FileArtifactStore::open(tree.root);
        requireArtifactOk(opened, "open", minimumRootLength);
        auto& store = *opened.value();
        const auto written = store.writeArtifact(runId, "context-snapshot.md", "first content");
        requireArtifactOk(written, "write", minimumRootLength);
        firstPath = written.value().relativePath;
        assert(written.value().byteSize == 13 && written.value().sha256 == sha256Hex("first content"));
        const auto read = store.readArtifact(firstPath);
        requireArtifactOk(read, "read", minimumRootLength);
        assert(read.value() == "first content");
        const auto resolved = store.resolveAbsolutePath(firstPath);
        requireArtifactOk(resolved, "resolve", minimumRootLength);
    }
    {
        // A fresh store object over the same root (an application restart) reads what was written and keeps writing.
        auto reopened = ai::infrastructure::FileArtifactStore::open(tree.root);
        requireArtifactOk(reopened, "reopen", minimumRootLength);
        auto& store = *reopened.value();
        const auto read = store.readArtifact(firstPath);
        requireArtifactOk(read, "read after reopen", minimumRootLength);
        assert(read.value() == "first content");

        const auto second = store.writeArtifact(runId, "mock-run-report.md", "second content");
        requireArtifactOk(second, "write after reopen", minimumRootLength);
        assert((tree.root / second.value().relativePath).native().size() > 0);
        const auto secondRead = store.readArtifact(second.value().relativePath);
        requireArtifactOk(secondRead, "read the second artifact", minimumRootLength);
        assert(secondRead.value() == "second content");

        // Removing is real, and a removed artifact is "not found" - never a made-up success.
        requireArtifactOk(store.removeArtifact(second.value().relativePath), "remove", minimumRootLength);
        const auto gone = store.readArtifact(second.value().relativePath);
        assert(!gone.success() && gone.error()->code == "artifact.not_found");

        // Containment on a long path: a real file OUTSIDE the root, reached through "..", must be refused, and so must an absolute path.
        const fs::path outside = tree.root.parent_path() / "outside";
        std::error_code ec;
        fs::create_directories(detail::longPath(outside), ec);
        assert(!ec);
        { std::ofstream(detail::longPath(outside / "stolen.txt"), std::ios::binary) << "stolen"; }
        const auto escaped = store.readArtifact(fs::path("..") / "outside" / "stolen.txt");
        assert(!escaped.success() && "a path that leaves the artifact root must not be read");
        assert(escaped.error()->code == "artifact_store.path_escape");
        const auto absolute = store.readArtifact(outside / "stolen.txt");
        assert(!absolute.success() && absolute.error()->code == "artifact_store.invalid_path");
    }
}

void testFileArtifactStoreSupportsPathsLongerThanMaxPath()
{
    runArtifactStoreLongPathCycle(100);   // everything under the legacy limit: the baseline
    runArtifactStoreLongPathCycle(200);   // the file path is longer than 260
    runArtifactStoreLongPathCycle(250);   // the artifact directory is longer than 260 too
    runArtifactStoreLongPathCycle(330);   // the root itself is longer than 260
}
} // namespace

int runArtifactServiceTests()
{
    testCreateListGetAndResolveArtifact();
    testGetArtifactContentRoundTripsThroughStore();
    testGetArtifactContentRejectsCorruptedPayload();
    testListArtifactsIsDeterministicallyOrdered();
    testArtifactNameSanitizationRejectsTraversal();
    testMetadataFailureRemovesWrittenFile();
    testContextSnapshotBudgetIsExplicitNotSilentTruncation();
    testMockRunProducesReviewableArtifacts();
    testMockRunWithoutArtifactServiceStillCompletes();
    testLostDiagnosticArtifactIsRecordedOnceAndAnnounced();
    testRunWarningMessageIsBoundedOnACharacterBoundary();
    testWarningThatCannotBeRecordedIsNeverSilent();
    testMockRunWithLostDiagnosticArtifactsCompletesWithWarnings();
    testFileArtifactStoreSupportsPathsLongerThanMaxPath();
    return 0;
}
