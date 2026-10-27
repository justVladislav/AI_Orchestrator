#include "ai/backend/BackendRuntime.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/DatabaseOwnerLock.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"
#include "FollowUpRequestTestSupport.hpp"
#include "OutcomeBriefRepositoryContract.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace ai::application;
using ai::backend::BackendRuntime;
using ai::backend::BackendRuntimeConfig;

void require(bool condition, const char* message)
{
    // These checks remain active in Release builds, unlike assert().
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// A fresh project directory plus a fresh database path per test, mirroring
// SqliteRepositoryTests.cpp's TemporaryPersistenceWorkspace.
struct TemporaryPersistenceWorkspace {
    fs::path root;
    fs::path projectRoot;
    fs::path databasePath;

    TemporaryPersistenceWorkspace()
    {
        root = fs::temp_directory_path()
            / ("ai-orchestrator-backend-runtime-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        projectRoot = root / "project";
        databasePath = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(projectRoot);
    }

    ~TemporaryPersistenceWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

Project createProject(ProjectService& service, const fs::path& path)
{
    const auto created = service.createProject(
        {"BackendRuntime test", "Exercises ai::backend::BackendRuntime", path});
    require(created.success(), "Could not create project through BackendRuntime::projectService()");
    return created.value();
}

// OrchestrationService has no single-run lookup; every test resolves a run
// through the same listRuns() a real caller would use, exactly like
// ApplicationPipelineTests.cpp/SqliteRepositoryTests.cpp do at their layers.
Run findRun(OrchestrationService& orchestration, const RunId& id)
{
    const auto runs = orchestration.listRuns();
    require(runs.success(), "OrchestrationService::listRuns failed");
    const auto it = std::find_if(runs.value().begin(), runs.value().end(),
        [&id](const Run& run) { return run.id == id; });
    require(it != runs.value().end(), "Run was not present in listRuns");
    return *it;
}

Run waitForTerminalRun(OrchestrationService& orchestration, const RunId& id)
{
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        const Run run = findRun(orchestration, id);
        if (isTerminal(run.status)) {
            return run;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Run did not reach a terminal state before timeout");
}

// RunManager writes the project memory segment (Task B, best-effort) only
// after the run's Completed status is already committed and its
// RunCompleted event already published - see RunManager::finishRun. A caller
// that only waits for the terminal run status can therefore observe
// Completed slightly before the segment exists; poll instead of asserting
// immediately.
std::vector<ProjectMemorySegment> waitForMemorySegments(
    ProjectMemoryService& service, const ProjectId& projectId, std::size_t expectedCount)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto segments = service.listMemorySegments(projectId);
        require(segments.success(), "listMemorySegments failed through BackendRuntime");
        if (segments.value().size() >= expectedCount) {
            return segments.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Project memory segment(s) did not appear before timeout");
}

// AG-10: RunManager::finishRun() reconciles dangling model_executions rows
// after already publishing the terminal RunEvent and committing the Run's
// own terminal status - the same "commit Run status first, best-effort work
// after" ordering waitForMemorySegments() above documents for project
// memory. A caller that only waits for the terminal Run must therefore poll
// the execution row too instead of asserting on it immediately.
ModelExecution waitForExecutionStatus(
    ModelExecutionService& service, const ModelExecutionId& id, ModelExecutionStatus expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = service.findExecution(id);
        require(found.success() && found.value().has_value(),
            "Seeded model execution disappeared while waiting for reconciliation");
        if (found.value()->status == expected) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Model execution did not reach the expected status before timeout");
}

void waitUntilRunning(OrchestrationService& orchestration, const RunId& id)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (findRun(orchestration, id).status == RunStatus::Running) {
            return;
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Run did not start before timeout");
}

// Scenarios 1-10: construct a runtime on a temp database, confirm storage is
// available, create a project, run a Mock workflow to completion, confirm
// its four stages persisted in order, destroy the runtime, then construct a
// second runtime on the same database file and confirm the project, run,
// and stages all survive with the same stable IDs and terminal state.
void testLifecycleCreateRunCompleteAndPersistAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    RunId runId;
    std::vector<StageId> stageIds;

    {
        BackendRuntime runtime({workspace.databasePath}); // 1
        require(runtime.storageAvailable(), "Fresh SQLite database must be available"); // 2
        require(!runtime.storageError().has_value(), "No storage error expected on a fresh database");
        require(!runtime.recoveryError().has_value(), "No recovery error expected on a fresh database");

        const Project project = createProject(runtime.projectService(), workspace.projectRoot); // 3
        projectId = project.id;

        const auto started = runtime.orchestrationService().startRun(
            {projectId, "Runtime lifecycle run", "Complete through BackendRuntime"}); // 4
        require(started.success(), "Could not start the Mock run");
        runId = started.value();

        const Run completed = waitForTerminalRun(runtime.orchestrationService(), runId); // 5
        require(completed.status == RunStatus::Completed, "Run did not complete");
        require(completed.progress == 1.0, "Completed run progress must be 1.0");

        const auto stages = runtime.orchestrationService().listStages(runId);
        require(stages.success() && stages.value().size() == 4, "Expected four Mock stages"); // 6
        static constexpr std::array<const char*, 4> expectedOrder {
            "context", "architect", "implementation", "documentation"};
        for (std::size_t index = 0; index < stages.value().size(); ++index) {
            require(stages.value()[index].type == expectedOrder[index], "Stage order was not preserved");
            require(stages.value()[index].status == StageStatus::Completed, "Every stage must complete");
            stageIds.push_back(stages.value()[index].id);
        }
        // runtime is destroyed here (7): its destructor must join
        // RunManager's worker before the SQLite connection it owns closes.
    }

    {
        BackendRuntime runtime({workspace.databasePath}); // 8: same path, a brand-new runtime instance
        require(runtime.storageAvailable(), "Reopened database must be available");

        const auto projects = runtime.projectService().listProjects();
        require(projects.success(), "listProjects failed after restart");
        const auto projectIt = std::find_if(projects.value().begin(), projects.value().end(),
            [&projectId](const Project& candidate) { return candidate.id == projectId; });
        require(projectIt != projects.value().end(), "Project id did not survive restart"); // 9

        const Run run = findRun(runtime.orchestrationService(), runId); // 9
        require(run.status == RunStatus::Completed, "Run terminal state did not survive restart"); // 10

        const auto stages = runtime.orchestrationService().listStages(runId);
        require(stages.success() && stages.value().size() == stageIds.size(), "Stages did not survive restart");
        for (std::size_t index = 0; index < stageIds.size(); ++index) {
            require(stages.value()[index].id == stageIds[index], "Stage IDs/order changed after restart"); // 9
            require(stages.value()[index].status == StageStatus::Completed,
                "Stage terminal state did not survive restart"); // 10
        }
    }
}

// Scenarios 11-12: a run written directly to SQLite as "Running" (simulating
// a crash - a clean BackendRuntime shutdown would already turn this into
// Interrupted itself through RunManager, which would not exercise
// OrchestrationService::recoverInterruptedRuns() the way an actual crash
// does) must already read back as Interrupted on the very first query after
// constructing the next BackendRuntime on that database.
void testRecoveryMarksUnfinishedRunInterruptedBeforeFirstRead()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    const RunId unfinishedId = "backend-runtime-recovery-run";

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh database must be available");
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;
        // Destroyed cleanly; no run was ever started, so RunManager's own
        // shutdown-to-Interrupted path has nothing to do here.
    }

    {
        // A raw repository connection stands in for "a prior process that
        // crashed mid-run": it writes a Running row and is closed without
        // ever going through RunManager's clean-shutdown path.
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open a raw connection to seed the unfinished run");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());

        Run unfinished;
        unfinished.id = unfinishedId;
        unfinished.projectId = projectId;
        unfinished.title = "Interrupted before this process could finish it";
        unfinished.task = "Recovered by the next BackendRuntime";
        unfinished.workflowId = "mock-sequential-v1";
        unfinished.status = RunStatus::Running;
        unfinished.progress = 0.5;
        unfinished.createdAt = std::chrono::system_clock::now();
        unfinished.startedAt = unfinished.createdAt;
        require(rawRuns.insertRun(unfinished).success(), "Could not persist the unfinished run fixture");
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Database with the unfinished run must be available");
        require(!runtime.recoveryError().has_value(), "Startup recovery must succeed");

        // Recovery already ran inside the constructor, so this first read
        // must already report Interrupted (11, 12).
        const Run recovered = findRun(runtime.orchestrationService(), unfinishedId);
        require(recovered.status == RunStatus::Interrupted, "Unfinished run was not recovered to Interrupted");
        require(recovered.finishedAt.has_value(), "Recovery must record a finish time");
    }
}

// Scenarios 13-16: an unusable database path must not crash BackendRuntime's
// constructor. storageAvailable() must read false, storageError() must
// carry the reason, and every Application Service command must return a
// controlled "storage.unavailable" Result instead of pretending to work.
void testStorageFailureIsControlledNotFatal()
{
    TemporaryPersistenceWorkspace workspace;
    // Force SqliteDatabase::open()'s create_directories() call to fail: put
    // a plain file where a directory is required, so the database's parent
    // path can never be created.
    const fs::path blockerFile = workspace.root / "blocked-by-a-file";
    {
        std::ofstream blocker(blockerFile);
        require(blocker.good(), "Could not create the blocker fixture file");
    }
    const fs::path unreachablePath = blockerFile / "state" / "orchestrator.sqlite3";

    BackendRuntime runtime({unreachablePath}); // 14: must not throw or crash
    require(!runtime.storageAvailable(), "storageAvailable must be false for an unusable path"); // 15
    require(runtime.storageError().has_value(), "storageError must be set"); // 13
    require(!runtime.storageError()->code.empty(), "storageError must carry a typed code");

    // Application Services stay constructible and callable (13): they
    // report the controlled failure instead of silently doing nothing.
    const auto created = runtime.projectService().createProject(
        {"Unreachable project", "Should never persist", workspace.projectRoot});
    require(!created.success(), "createProject must fail when storage is unavailable");
    require(created.error()->code == "storage.unavailable", "Expected storage.unavailable from ProjectService"); // 16

    const auto started = runtime.orchestrationService().startRun(
        {"missing-project-id", "Should never run", "Storage is unavailable"});
    require(!started.success(), "startRun must fail when storage is unavailable");
    require(started.error()->code == "storage.unavailable",
        "Expected storage.unavailable from OrchestrationService"); // 16

    const auto compactions = runtime.projectMemoryCompactionService().listCompactionRecords("missing-project");
    require(!compactions.success() && compactions.error()->code == "storage.unavailable",
        "ProjectMemoryCompactionService must preserve controlled storage failure semantics");

    // Stage 7's three new services must offer the exact same guarantee:
    // fully constructible and callable, reporting storage.unavailable
    // instead of crashing, hanging, or silently doing nothing.
    const auto activeProjectId = runtime.workspaceContextService().activeProjectId();
    require(!activeProjectId.success() && activeProjectId.error()->code == "storage.unavailable",
        "Expected storage.unavailable from WorkspaceContextService");
    const auto selectProject = runtime.workspaceContextService().selectProject("any-project-id");
    require(!selectProject.success() && selectProject.error()->code == "storage.unavailable",
        "Expected storage.unavailable from WorkspaceContextService::selectProject");

    const auto listProviders = runtime.providerCatalogService().listProviders();
    require(!listProviders.success() && listProviders.error()->code == "storage.unavailable",
        "Expected storage.unavailable from ProviderCatalogService");
    const auto createProvider = runtime.providerCatalogService().createProvider(
        {"Unreachable provider", "openai", "https://x.invalid", true, std::nullopt});
    require(!createProvider.success() && createProvider.error()->code == "storage.unavailable",
        "Expected storage.unavailable from ProviderCatalogService::createProvider");

    const auto listProfiles = runtime.executionProfileService().listProfiles();
    require(!listProfiles.success() && listProfiles.error()->code == "storage.unavailable",
        "Expected storage.unavailable from ExecutionProfileService");

    // The credential store is a separate boundary, independent of SQLite
    // availability: Windows Credential Manager and macOS Keychain adapters
    // remain usable even while SQLite storageAvailable() is false; builds
    // without a platform adapter use the controlled-unavailable stand-in.
    // Either way, the secret value must never appear in an error message.
    constexpr const char* credentialReference = "backend-runtime-test-storage-failure-credential";
    constexpr const char* sentinelSecret = "sk-do-not-leak-this-value";
    const auto storeSecret = runtime.credentialStore().store(credentialReference, sentinelSecret);
    if (storeSecret.success()) {
        // A real adapter is active and unaffected by SQLite being
        // unreachable - clean up the test credential immediately so this
        // test never leaves a stray secret behind on the host.
        const auto removed = runtime.credentialStore().remove(credentialReference);
        require(removed.success(), "Could not remove the test credential written by this scenario");
    } else {
        // A compiled OS adapter may still report an operational failure in
        // a locked/headless test session. Only builds without an adapter are
        // required to return credential_store.unavailable.
        require(!storeSecret.error()->code.empty(),
            "Credential store failure did not provide a typed error code");
        require(storeSecret.error()->message.find(sentinelSecret) == std::string::npos,
            "Credential store error message must never contain the secret value");
    }
}

// Global active project, provider/model catalog, and execution profile
// commands issued through the real BackendRuntime composition (SQLite +
// in-process services) must persist exactly like Project/Run/Artifact state
// already does.
void testWorkspaceAndCatalogPersistAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    ExecutionProfileId profileId;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh database must be available");
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;

        require(runtime.workspaceContextService().selectProject(projectId).success(),
            "Could not select the active project through BackendRuntime");

        const auto provider = runtime.providerCatalogService().createProvider(
            {"BackendRuntime Provider", "anthropic", "https://api.anthropic.invalid", true, std::nullopt});
        require(provider.success(), "Could not create provider through BackendRuntime");
        providerId = provider.value();

        const auto model = runtime.providerCatalogService().createModel(
            {providerId, "claude-test", "Claude Test", 200000, 8192, 1000000, 5000000, true});
        require(model.success(), "Could not create model through BackendRuntime");
        modelId = model.value();
        require(runtime.providerCatalogService().setDefaultModel(providerId, modelId).success(),
            "Could not set default model through BackendRuntime");

        StageModelAssignment documentation {"documentation", providerId, modelId, 4000ms};
        const auto profile = runtime.executionProfileService().createProfile(
            {"BackendRuntime Profile", "mock-sequential-v1", true, {documentation}});
        require(profile.success(), "Could not create execution profile through BackendRuntime");
        profileId = profile.value();
        require(runtime.executionProfileService().setDefaultProfile(profileId).success(),
            "Could not set default execution profile through BackendRuntime");
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Reopened database must be available");

        const auto activeProject = runtime.workspaceContextService().activeProject();
        require(activeProject.success() && activeProject.value().has_value()
                && activeProject.value()->id == projectId,
            "Active project did not survive restart through BackendRuntime");

        const auto provider = runtime.providerCatalogService().findProvider(providerId);
        require(provider.success() && provider.value().has_value(),
            "Provider did not survive restart through BackendRuntime");

        const auto model = runtime.providerCatalogService().findModel(modelId);
        require(model.success() && model.value().has_value() && model.value()->isDefault,
            "Default model did not survive restart through BackendRuntime");

        const auto profile = runtime.executionProfileService().findProfile(profileId);
        require(profile.success() && profile.value().has_value() && profile.value()->isDefault,
            "Default execution profile did not survive restart through BackendRuntime");
        require(profile.value()->assignments.size() == 1
                && profile.value()->assignments.front().stageType == "documentation",
            "Execution profile assignment did not survive restart through BackendRuntime");
    }
}

// Scenarios 17-18: destroying a BackendRuntime while one run is actively
// executing and a second is still queued must not crash or hang.
// RunManager's destructor has to request cancellation, join the worker
// thread, and mark both rows Interrupted before Impl's storage member (and
// the sqlite3 connection/repositories it owns) is destroyed; if worker
// teardown instead raced repository destruction, this would be a
// use-after-free and would corrupt the heap or crash well before the
// assertions below run.
void testShutdownWithActiveAndQueuedRunsRecoversCleanly()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    RunId firstRunId;
    RunId secondRunId;

    {
        // A slower Mock workflow keeps the first run observably "running"
        // long enough to reliably destroy the runtime mid-execution instead
        // of racing a very short timing window.
        BackendRuntime runtime({workspace.databasePath, 20ms, 10});
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;

        const auto first = runtime.orchestrationService().startRun(
            {projectId, "Active at shutdown", "Still running when the runtime is destroyed"});
        require(first.success(), "Could not start the active run");
        firstRunId = first.value();
        const auto second = runtime.orchestrationService().startRun(
            {projectId, "Queued at shutdown", "Still queued when the runtime is destroyed"});
        require(second.success(), "Could not start the queued run");
        secondRunId = second.value();

        waitUntilRunning(runtime.orchestrationService(), firstRunId);
        // runtime destructs here with firstRunId Running and secondRunId
        // Queued (17, 18).
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Database must reopen after an unclean shutdown");
        const Run first = findRun(runtime.orchestrationService(), firstRunId);
        const Run second = findRun(runtime.orchestrationService(), secondRunId);
        require(first.status == RunStatus::Interrupted, "Active run must be Interrupted after shutdown");
        require(second.status == RunStatus::Interrupted, "Queued run must be Interrupted after shutdown");
    }
}

// A successful Mock run through the full BackendRuntime composition (real
// SQLite metadata + real filesystem content) must leave at least one
// reviewable artifact behind, and that artifact - both its metadata row and
// its file content - must survive destroying and reconstructing the runtime
// on the same database/artifact root, exactly like Project/Run state already
// does.
void testMockRunProducesReviewableArtifactAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    RunId runId;
    ArtifactId artifactId;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh database must be available");
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;

        const auto started = runtime.orchestrationService().startRun(
            {projectId, "Artifact runtime run", "Produce a reviewable artifact end to end"});
        require(started.success(), "Could not start the artifact-producing run");
        runId = started.value();

        const Run completed = waitForTerminalRun(runtime.orchestrationService(), runId);
        require(completed.status == RunStatus::Completed, "Artifact-producing run did not complete");

        const auto artifacts = runtime.artifactService().listArtifacts(runId);
        require(artifacts.success(), "listArtifacts failed through BackendRuntime");
        require(artifacts.value().size() >= 2,
            "A successful Mock run must produce at least a context_snapshot and a report artifact");

        bool hasContextSnapshot = false;
        for (const auto& artifact : artifacts.value()) {
            if (artifact.kind == ArtifactKind::ContextSnapshot) {
                hasContextSnapshot = true;
                artifactId = artifact.id;
            }
        }
        require(hasContextSnapshot, "Mock run did not produce a context_snapshot artifact");

        const auto resolved = runtime.artifactService().resolveArtifactPath(artifactId);
        require(resolved.success(), "Could not resolve the artifact's absolute path");
        require(fs::exists(ai::application::detail::longPath(resolved.value())), "Resolved artifact path does not exist on disk");   // longPath: the path may be longer than MAX_PATH
        require(!fs::path(resolved.value()).empty(), "Resolved artifact path was empty");

        std::ifstream content(ai::application::detail::longPath(resolved.value()));
        require(content.good(), "Could not open the resolved artifact file for reading");
        const std::string firstLine = [&content] {
            std::string line;
            std::getline(content, line);
            return line;
        }();
        require(!firstLine.empty(), "Context snapshot artifact file was empty");

        const auto readBack = runtime.artifactService().getArtifactContent(artifactId);
        require(readBack.success(), "Could not read artifact content through BackendRuntime");
        require(readBack.value().find(firstLine) != std::string::npos,
            "Content read through getArtifactContent did not match the file on disk");
        // runtime destructs here: worker thread stops before storage/artifact
        // store are torn down, exactly like the Project/Run lifecycle test.
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Reopened database must be available");

        const auto artifacts = runtime.artifactService().listArtifacts(runId);
        require(artifacts.success() && artifacts.value().size() >= 2,
            "Artifacts did not survive restart");

        const auto artifact = runtime.artifactService().getArtifact(artifactId);
        require(artifact.success(), "Artifact metadata did not survive restart");
        require(artifact.value().runId == runId, "Restarted artifact metadata changed run association");

        const auto resolved = runtime.artifactService().resolveArtifactPath(artifactId);
        require(resolved.success() && fs::exists(ai::application::detail::longPath(resolved.value())),
            "Artifact content file did not survive restart");
    }
}

// Run warnings through the real composition: the artifact root cannot be opened (a plain file sits where the directory would
// be), the database is fine. The Mock Run still completes, every artifact it was to leave is lost, and what remains of that is
// (1) a RunWarning event for the UI AFTER the warning was recorded, (2) the durable warnings, read again after a restart.
void testLostDiagnosticArtifactsLeaveDurableWarningsAndEventsAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    const fs::path blockerFile = workspace.root / "artifact-root-is-a-file";
    {
        fs::create_directories(workspace.root);
        std::ofstream blocker(blockerFile);
        require(blocker.good(), "Could not create the blocker fixture file");
    }
    BackendRuntimeConfig config {workspace.databasePath};
    config.artifactRoot = blockerFile / "artifacts";

    RunId runId;
    {
        BackendRuntime runtime(config);
        require(runtime.storageAvailable(), "The database itself must be available");

        struct Observed {
            std::mutex mutex;
            std::vector<RunEvent> warningEvents;
        } observed;
        runtime.orchestrationService().setEventHandler([&observed](const RunEvent& event) {
            if (event.type != RunEventType::RunWarning) return;
            std::lock_guard lock(observed.mutex);
            observed.warningEvents.push_back(event);
        });

        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        const auto started = runtime.orchestrationService().startRun(
            {project.id, "Lossy runtime run", "Lose every artifact"});
        require(started.success(), "Could not start the run");
        runId = started.value();
        const Run completed = waitForTerminalRun(runtime.orchestrationService(), runId);
        require(completed.status == RunStatus::Completed && completed.errorMessage.empty(),
            "A run that only lost diagnostic artifacts must still complete");

        const auto warnings = runtime.artifactService().listWarnings(runId);
        require(warnings.success() && warnings.value().size() == 2, "Both lost artifacts must leave a warning");

        {
            std::lock_guard lock(observed.mutex);
            require(observed.warningEvents.size() == 2, "Each recorded warning must be announced exactly once");
            for (const auto& event : observed.warningEvents) {
                require(event.runId == runId && !event.message.empty() && event.message.find("[not saved:") == std::string::npos,
                    "A recorded warning is announced with its own message and without the '[not saved: ...]' marker");
                const bool known = std::any_of(warnings.value().begin(), warnings.value().end(),
                    [&event](const RunWarning& warning) {
                        return warning.message == event.message && warning.stageId == event.stageId;
                    });
                require(known, "An announced warning must be the durable one (same text, same stage)");
            }
        }
        runtime.orchestrationService().setEventHandler({});
    }
    {
        BackendRuntime runtime(config);
        require(runtime.storageAvailable(), "Reopened database must be available");
        const auto warnings = runtime.artifactService().listWarnings(runId);
        require(warnings.success() && warnings.value().size() == 2, "The warnings did not survive the restart");
        require(findRun(runtime.orchestrationService(), runId).status == RunStatus::Completed,
            "The Run is still Completed: a warning does not change its outcome");
    }
}

void testModelExecutionLifecycleAndRecoveryAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ModelExecutionId completedId;
    ModelExecutionId interruptedId;
    ProjectId projectId;
    ProviderId providerId;

    {
        BackendRuntime runtime({workspace.databasePath});
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;
        const auto provider = runtime.providerCatalogService().createProvider(
            {"Execution Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
        require(provider.success(), "Could not create model-execution provider");
        providerId = provider.value();
        const auto model = runtime.providerCatalogService().createModel(
            {provider.value(), "execution-model", "Execution Model", 128000, 4096,
             1'000'000, 2'000'000, true});
        require(model.success(), "Could not create model-execution model");

        CreateModelExecutionRequest first;
        first.source = ModelExecutionSource::Comparison;
        first.sourceId = "comparison-live-1";
        first.projectId = projectId;
        first.providerId = provider.value();
        first.modelId = model.value();
        first.role = "reviewer";
        first.position = 0;
        first.requestHash = "request-hash";
        first.contextHash = "context-hash";
        const auto created = runtime.modelExecutionService().createExecution(first);
        require(created.success(), "Could not create model execution through BackendRuntime");
        completedId = created.value().id;
        require(runtime.modelExecutionService().startExecution(completedId).success(),
            "Could not start model execution");

        FinishModelExecutionRequest outcome;
        outcome.status = ModelExecutionStatus::Completed;
        outcome.outputPreview = "Reviewable provider output";
        outcome.inputTokens = 120;
        outcome.outputTokens = 42;
        outcome.cacheCreationInputTokens = 30;
        outcome.cacheReadInputTokens = 40;
        outcome.cacheCreationDuration = {true, 10, 20};
        // Modern rows carry cache audit fields, so their stored price is
        // authoritative even when it rounds to zero. Keep this lifecycle
        // fixture explicitly priced; legacy revaluation has its own tests.
        outcome.costMicros = 204;
        outcome.latencyMs = 800;
        const auto finished = runtime.modelExecutionService().finishExecution(completedId, outcome);
        require(finished.success(), "Could not finish model execution");
        require(finished.value().providerId == first.providerId
                && finished.value().modelId == first.modelId
                && finished.value().requestHash == first.requestHash,
            "Model execution identity changed while recording its outcome");
        require(finished.value().cacheCreationInputTokens == 30
                && finished.value().cacheReadInputTokens == 40
                && finished.value().cacheCreationDuration.known
                && finished.value().cacheCreationDuration.fiveMinuteTokens == 10
                && finished.value().cacheCreationDuration.oneHourTokens == 20,
            "Model execution service lost duration-aware cache usage");
        const auto secondFinish = runtime.modelExecutionService().finishExecution(completedId, outcome);
        require(!secondFinish.success()
                && secondFinish.error()->code == "model_execution.invalid_transition",
            "A terminal model execution was incorrectly rewritten");

        CreateQualityEvaluationRequest evaluation;
        evaluation.executionId = completedId;
        evaluation.evaluatorKind = QualityEvaluatorKind::Automated;
        evaluation.rubricVersion = "acceptance-v1";
        evaluation.scoreBasisPoints = 9100;
        evaluation.criteriaJson = R"({"correctness":94,"clarity":88})";
        require(runtime.modelExecutionService().addEvaluation(evaluation).success(),
            "Could not evaluate completed model output");

        CreateDeterministicQualityEvaluationRequest deterministic;
        deterministic.executionId = completedId;
        deterministic.evaluatorKind = QualityEvaluatorKind::Automated;
        deterministic.rubricVersion = "comparison-quality-v1";
        deterministic.rubric = {
            {"correctness", 6'000},
            {"requirements_coverage", 4'000}};
        deterministic.criteria = {
            {"correctness", 9'000, "The answer matches the verified result"},
            {"requirements_coverage", 8'000, "All required sections are present"}};
        deterministic.explanation = "Weighted, evidence-backed evaluation";
        const auto deterministicEvaluation =
            runtime.modelExecutionService().addDeterministicEvaluation(deterministic);
        require(deterministicEvaluation.success()
                && deterministicEvaluation.value().scoreBasisPoints == 8'600,
            "Deterministic quality score was not calculated from rubric weights");
        require(deterministicEvaluation.value().criteriaJson
                    == R"([{"id":"correctness","weight_basis_points":6000,"score_basis_points":9000},{"id":"requirements_coverage","weight_basis_points":4000,"score_basis_points":8000}])",
            "Deterministic criteria were not serialized canonically");

        deterministic.rubric[1].weightBasisPoints = 3'999;
        const auto invalidRubric =
            runtime.modelExecutionService().addDeterministicEvaluation(deterministic);
        require(!invalidRubric.success()
                && invalidRubric.error()->code == "quality_evaluation.invalid_rubric_weight",
            "An incomplete rubric weight was incorrectly accepted");

        first.position = 1;
        first.role = "implementer";
        const auto interrupted = runtime.modelExecutionService().createExecution(first);
        require(interrupted.success(), "Could not create execution used by recovery test");
        interruptedId = interrupted.value().id;
        require(runtime.modelExecutionService().startExecution(interruptedId).success(),
            "Could not start execution used by recovery test");
        FinishModelExecutionRequest invalidCacheOutcome;
        invalidCacheOutcome.status = ModelExecutionStatus::Completed;
        invalidCacheOutcome.inputTokens = 30;
        invalidCacheOutcome.cacheCreationInputTokens = 30;
        invalidCacheOutcome.cacheCreationDuration.fiveMinuteTokens = 30;
        const auto rejectedCacheOutcome = runtime.modelExecutionService().finishExecution(
            interruptedId, invalidCacheOutcome);
        require(!rejectedCacheOutcome.success()
                && rejectedCacheOutcome.error()->code
                    == "cache_duration.detail_without_partition",
            "Model execution service accepted hidden cache-duration detail");
        const auto stillRunning = runtime.modelExecutionService().findExecution(interruptedId);
        require(stillRunning.success() && stillRunning.value().has_value()
                && stillRunning.value()->status == ModelExecutionStatus::Running
                && stillRunning.value()->inputTokens == 0,
            "Refused cache usage partially changed the running execution");
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(!runtime.modelExecutionRecoveryError().has_value(),
            "Model execution startup recovery failed");

        const auto completed = runtime.modelExecutionService().findExecution(completedId);
        require(completed.success() && completed.value().has_value()
                && completed.value()->status == ModelExecutionStatus::Completed
                && completed.value()->outputTokens == 42,
            "Completed execution changed after restart");
        const auto evaluations = runtime.modelExecutionService().listEvaluations(completedId);
        require(evaluations.success() && evaluations.value().size() == 2
                && evaluations.value().front().scoreBasisPoints == 9100
                && evaluations.value().back().scoreBasisPoints == 8600,
            "Quality evaluation did not survive restart");

        const auto recovered = runtime.modelExecutionService().findExecution(interruptedId);
        require(recovered.success() && recovered.value().has_value()
                && recovered.value()->status == ModelExecutionStatus::Failed
                && recovered.value()->errorCode == "model_execution.interrupted"
                && recovered.value()->finishedAt.has_value(),
            "Interrupted model execution was not recovered before first read");
        const auto projectExecutions = runtime.modelExecutionService().listProjectExecutions(projectId);
        require(projectExecutions.success() && projectExecutions.value().size() == 2,
            "Project model-execution history did not survive restart");
        const auto analytics = runtime.executionAnalyticsService().projectSnapshot(projectId);
        require(analytics.success()
                && analytics.value().totals.executionCount == 2
                && analytics.value().totals.completedCount == 1
                && analytics.value().totals.failedCount == 1
                && analytics.value().totals.inputTokens == 120
                && analytics.value().totals.outputTokens == 42
                && analytics.value().totals.costMicros == 204
                && analytics.value().providers.size() == 1
                && analytics.value().providers.front().providerId == providerId
                && analytics.value().providers.front().models.size() == 1
                && analytics.value().providers.front().models.front().totals.costMicros == 204,
            "Execution analytics did not reproduce persisted totals and historical pricing");

        const auto rangeStart = projectExecutions.value().front().createdAt;
        const auto rangeEnd = projectExecutions.value().back().createdAt
            + std::chrono::milliseconds {1};
        const auto rangedAnalytics = runtime.executionAnalyticsService().projectSnapshot(
            projectId, rangeStart, rangeEnd);
        require(rangedAnalytics.success()
                && rangedAnalytics.value().totals.executionCount == 2,
            "Execution analytics date range dropped executions inside its bounds");

        const auto futureAnalytics = runtime.executionAnalyticsService().projectSnapshot(
            projectId, rangeEnd, rangeEnd + std::chrono::milliseconds {1});
        require(futureAnalytics.success()
                && futureAnalytics.value().totals.executionCount == 0
                && futureAnalytics.value().providers.empty(),
            "Execution analytics date range included executions outside its bounds");
    }
}

void testChatProviderAttemptsShareTheModelExecutionLedger()
{
    TemporaryPersistenceWorkspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    const Project project = createProject(runtime.projectService(), workspace.projectRoot);

    const auto providerId = runtime.providerCatalogService().createProvider(
        {"Unsupported chat provider", "openai_compatible", "https://example.invalid", true,
         std::nullopt});
    require(providerId.success(), "Could not create provider for chat-ledger test");
    const auto modelId = runtime.providerCatalogService().createModel(
        {providerId.value(), "chat-ledger-model", "Chat Ledger Model", 128000, 4096,
         100, 200, true});
    require(modelId.success(), "Could not create model for chat-ledger test");

    CreateConversationRequest create;
    create.projectId = project.id;
    create.providerId = providerId.value();
    create.modelId = modelId.value();
    create.title = "Execution ledger chat";
    const auto conversation = runtime.chatService().createConversation(create);
    require(conversation.success(), "Could not create conversation for chat-ledger test");

    const auto turn = runtime.chatService().prepareTurn(
        conversation.value().id, "Record this provider attempt in the shared ledger");
    require(turn.success(), "Could not prepare chat turn for ledger test");
    const auto generated = runtime.chatService().generate(
        turn.value().assistantMessage.id, std::stop_token {});
    require(generated.success()
            && generated.value().status == ChatMessageStatus::Failed
            && generated.value().role == ChatRole::Error,
        "Unsupported provider must produce a persisted failed assistant response");

    auto executions = runtime.modelExecutionService().listExecutions(
        ModelExecutionSource::Chat, conversation.value().id);
    require(executions.success() && executions.value().size() == 1,
        "Chat provider attempt was not recorded in the shared execution ledger");
    const auto& firstExecution = executions.value().front();
    require(firstExecution.status == ModelExecutionStatus::Failed,
        "Chat execution did not preserve failed terminal status");
    require(firstExecution.projectId == project.id,
        "Chat execution did not preserve its project association");
    require(firstExecution.providerId == providerId.value()
            && firstExecution.modelId == modelId.value(),
        "Chat execution did not preserve provider/model snapshot");
    require(firstExecution.position == 0,
        "First chat provider attempt did not receive position zero");
    require(!firstExecution.requestHash.empty() && !firstExecution.contextHash.empty(),
        "Chat execution request/context hashes are missing");
    require(!firstExecution.errorCode.empty() && !firstExecution.errorMessage.empty(),
        "Chat execution did not preserve the provider failure details");

    const auto retried = runtime.chatService().retry(
        turn.value().assistantMessage.id, std::stop_token {});
    require(retried.success() && retried.value().status == ChatMessageStatus::Failed,
        "Failed chat response could not be retried");
    executions = runtime.modelExecutionService().listExecutions(
        ModelExecutionSource::Chat, conversation.value().id);
    require(executions.success() && executions.value().size() == 2
            && executions.value()[1].position == 1
            && executions.value()[1].status == ModelExecutionStatus::Failed,
        "Chat retry must be preserved as a distinct provider attempt");
}

void testComparisonSessionUsesLedgerAndRecoversAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ComparisonSessionId sessionId;
    ProjectId projectId;

    {
        BackendRuntime runtime({workspace.databasePath});
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;
        const auto providerA = runtime.providerCatalogService().createProvider(
            {"Comparison OpenAI", "openai", "https://openai.invalid", true, std::nullopt});
        const auto providerB = runtime.providerCatalogService().createProvider(
            {"Comparison Anthropic", "anthropic", "https://anthropic.invalid", true, std::nullopt});
        require(providerA.success() && providerB.success(),
            "Could not create Comparison providers");
        const auto modelA = runtime.providerCatalogService().createModel(
            {providerA.value(), "comparison-a", "Comparison A", 100000, 4096, 1, 2, 0, 0, true});
        const auto modelB = runtime.providerCatalogService().createModel(
            {providerB.value(), "comparison-b", "Comparison B", 100000, 4096, 1, 2, 0, 0, true});
        require(modelA.success() && modelB.success(),
            "Could not create Comparison models");

        CreateComparisonSessionRequest request;
        request.projectId = project.id;
        request.title = "Durable comparison";
        request.prompt = "Solve the same task independently";
        request.contextHash = "comparison-context-hash";
        request.modelSlots = {
            {providerA.value(), modelA.value(), "candidate"},
            {providerB.value(), modelB.value(), "candidate"}};
        const auto created = runtime.comparisonService().createSession(request);
        require(created.success(), "Could not create durable Comparison session");
        sessionId = created.value().id;
        const auto results = runtime.comparisonService().listResults(sessionId);
        require(results.success() && results.value().size() == 2,
            "Comparison did not seed two shared-ledger executions");
        require(results.value()[0].sourceId == sessionId
                && results.value()[1].requestHash == created.value().requestHash,
            "Comparison execution identity/hash snapshot is inconsistent");
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(!runtime.comparisonRecoveryError().has_value(),
            "Comparison startup recovery failed");
        const auto session = runtime.comparisonService().findSession(sessionId);
        require(session.success() && session.value().has_value()
                && session.value()->status == ComparisonSessionStatus::Failed
                && session.value()->errorCode == "comparison.interrupted",
            "Interrupted Comparison session was not recovered truthfully");
        const auto results = runtime.comparisonService().listResults(sessionId);
        require(results.success() && results.value().size() == 2
                && results.value()[0].status == ModelExecutionStatus::Failed
                && results.value()[1].status == ModelExecutionStatus::Failed,
            "Interrupted Comparison candidate executions were not recovered");
        const auto projectSessions = runtime.comparisonService().listSessions(projectId);
        require(projectSessions.success() && projectSessions.value().size() == 1,
            "Project-scoped Comparison read failed after restart");
    }
}

// Task B (PROJECT_MEMORY_PLAN.md): a run completed through the real
// BackendRuntime composition (RunManager -> ProjectMemoryService ->
// SqliteProjectMemoryRepository) must leave exactly one project memory
// segment behind, and that segment must survive destroying and
// reconstructing the runtime on the same database file, exactly like
// Project/Run/Artifact state already does.
void testProjectMemorySegmentRecordedAndPersistsAcrossRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    RunId runId;
    std::string segmentId;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh database must be available");
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;

        const auto started = runtime.orchestrationService().startRun(
            {projectId, "Project memory runtime run", "Produce a project memory segment end to end"});
        require(started.success(), "Could not start the memory-producing run");
        runId = started.value();

        const Run completed = waitForTerminalRun(runtime.orchestrationService(), runId);
        require(completed.status == RunStatus::Completed, "Memory-producing run did not complete");

        const auto segments = waitForMemorySegments(runtime.projectMemoryService(), projectId, 1);
        require(segments.size() == 1,
            "Exactly one project memory segment must be recorded for one completed run");
        const auto& segment = segments.front();
        require(segment.runId == runId, "Segment must reference the completed run");
        require(segment.summaryTokenCount > 0, "Persisted segment must carry a nonzero summaryTokenCount");
        require(segment.sourceTokenCount > 0, "Persisted segment must carry a nonzero sourceTokenCount");
        segmentId = segment.id;
        // runtime destructs here: worker thread stops before storage is torn
        // down, exactly like every other BackendRuntime lifecycle test.
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Reopened database must be available");

        const auto segments = runtime.projectMemoryService().listMemorySegments(projectId);
        require(segments.success() && segments.value().size() == 1,
            "Project memory segment did not survive restart");
        require(segments.value().front().id == segmentId,
            "Project memory segment id changed after restart");
        require(segments.value().front().runId == runId,
            "Project memory segment run association changed after restart");
    }
}

void testExecutionManifestContractPersistsAcrossRuntimeRestart()
{
    TemporaryPersistenceWorkspace workspace;
    RunId runId;
    {
        BackendRuntime runtime({workspace.databasePath});
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        const auto started = runtime.orchestrationService().startRun(
            {project.id, "Manifest runtime run", "Freeze an execution contract"});
        require(started.success(), "Could not create Run for execution manifest");
        runId = started.value();

        ExecutionManifest manifest;
        manifest.runId = runId;
        manifest.requestedMaxParallelism = 1;
        manifest.policy.maxParallelism = 1;
        ExecutionManifestTask task;
        task.id = "contract-only";
        task.objective = "Prove BackendRuntime exposes the Phase 2 contract";
        task.role = "validator";
        task.ownedPaths = {"First_AI_Agents/contract"};
        task.acceptanceCriteria = {"Snapshot survives restart"};
        ExecutionAcceptanceRequirement acceptance;
        acceptance.id = "snapshot-survives";
        acceptance.description = "Snapshot survives restart";
        acceptance.requiredOutputs = {"README.md"};
        acceptance.verificationKind = ExecutionVerificationKind::InspectPaths;
        ExecutionAcceptanceContract contract;
        contract.requirements = {std::move(acceptance)};
        task.acceptanceContract = std::move(contract);
        task.contextReferences = {"REMAINING_GAPS_EXECUTION_PLAN.md"};
        task.requestedCapability = WorkspaceCapability::Inspect;
        manifest.tasks.push_back(std::move(task));

        const auto trustedPolicy = manifest.policy;
        const auto frozen = runtime.executionManifestService().freeze(
            std::move(manifest), trustedPolicy);
        require(frozen.success(), "BackendRuntime could not freeze execution manifest");
        const auto duplicate =
            runtime.executionManifestService().freeze(frozen.value(), trustedPolicy);
        require(!duplicate.success()
                && duplicate.error()->code == "manifest.already_frozen",
            "BackendRuntime did not preserve one-manifest-per-Run invariant");
        static_cast<void>(waitForTerminalRun(runtime.orchestrationService(), runId));
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Manifest database did not reopen");
        const auto read = runtime.executionManifestService().findForRun(runId);
        require(read.success() && read.value().has_value(),
            "BackendRuntime lost frozen manifest after restart");
        require(read.value()->tasks.size() == 1
                && read.value()->tasks.front().requestedCapability
                    == WorkspaceCapability::Inspect,
            "BackendRuntime changed manifest capability snapshot");
    }
}

// AG-10 end-to-end: proves RunManager::finishRun() actually invokes
// ModelExecutionService::reconcileRunExecutions() through the real
// BackendRuntime composition (BackendRuntime.cpp wires
// &modelExecutionService into RunManager's constructor) - not just the
// isolated service-level test in ModelExecutionRepositoryTests.cpp. A Mock
// run never creates a real model_execution row itself, so one is seeded
// directly (Pending -> Running) before the run is left to finish around it,
// reproducing the exact incident shape (a row left non-terminal by some
// other code path) without needing a real provider call.
void testDanglingModelExecutionReconciledWhenRunFinishes()
{
    TemporaryPersistenceWorkspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "Fresh SQLite database must be available");

    const Project project = createProject(runtime.projectService(), workspace.projectRoot);

    // model_executions.provider_id/model_id are real foreign keys - seed a
    // catalog entry first, exactly like
    // testModelExecutionLifecycleAndRecoveryAcrossRestart() above.
    const auto provider = runtime.providerCatalogService().createProvider(
        {"AG-10 Reconciliation Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
    require(provider.success(), "Could not create reconciliation provider");
    const auto model = runtime.providerCatalogService().createModel(
        {provider.value(), "reconciliation-model", "Reconciliation Model", 128000, 4096,
         1'000'000, 2'000'000, true});
    require(model.success(), "Could not create reconciliation model");

    const auto started = runtime.orchestrationService().startRun(
        {project.id, "AG-10 reconciliation run", "Prove dangling execution reconciliation"});
    require(started.success(), "Could not start the Mock run");
    const RunId runId = started.value();

    CreateModelExecutionRequest createRequest;
    createRequest.source = ModelExecutionSource::Run;
    createRequest.sourceId = runId;
    createRequest.projectId = project.id;
    createRequest.providerId = provider.value();
    createRequest.modelId = model.value();
    createRequest.role = "component-worker";
    createRequest.requestHash = "hash-1";
    createRequest.contextHash = "context-1";
    const auto created = runtime.modelExecutionService().createExecution(createRequest);
    require(created.success(), "Could not seed a model execution for reconciliation");
    const auto startedExecution =
        runtime.modelExecutionService().startExecution(created.value().id);
    require(startedExecution.success() && startedExecution.value().status == ModelExecutionStatus::Running,
        "Seeded execution must be Running before the Run finishes");

    const Run completed = waitForTerminalRun(runtime.orchestrationService(), runId);
    require(completed.status == RunStatus::Completed, "Run did not complete");

    const auto reconciled = waitForExecutionStatus(
        runtime.modelExecutionService(), created.value().id, ModelExecutionStatus::Failed);
    require(reconciled.errorCode == "model_execution.orphaned",
        "Reconciled execution did not use the stable AG-10 error code");
    require(reconciled.inputTokens == 0 && reconciled.outputTokens == 0
            && reconciled.costMicros == 0,
        "Reconciliation must never invent usage");
}

void testProjectMemoryCompactionServiceIsWiredThroughBackendRuntime()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh compaction database must be available");
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;

        const auto records = runtime.projectMemoryCompactionService().listCompactionRecords(projectId);
        require(records.success() && records.value().empty(),
            "Fresh project must expose an empty compaction provenance list");

        // Missing provider is rejected before any client/network call, while
        // still proving the runtime accessor is backed by the real service.
        const auto refused = runtime.projectMemoryCompactionService().compact(
            projectId, "missing-provider", "missing-model", std::stop_token {});
        require(!refused.success() && refused.error()->code == "project_memory.provider_unavailable",
            "BackendRuntime compaction service did not execute its typed preflight");
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        const auto records = runtime.projectMemoryCompactionService().listCompactionRecords(projectId);
        require(records.success() && records.value().empty(),
            "Compaction repository wiring did not survive a runtime restart");
    }
}

// AG-18.3 hardening (blocker 4, required test #1): the retry-service
// accessor must be backed by the runtime's own real, durable bootstrap
// repository - never a stub or a fresh in-memory instance constructed
// per-call. Exercised via a typed preflight (an unknown predecessor Run
// id), the same "proves the accessor is real, not merely present" pattern
// as testProjectMemoryCompactionServiceIsWiredThroughBackendRuntime above,
// and confirmed stable across a runtime restart so the same durable
// storage backs it both times.
void testVerificationBootstrapRetryServiceIsWiredThroughBackendRuntime()
{
    TemporaryPersistenceWorkspace workspace;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh bootstrap-retry database must be available");
        const auto refused = runtime.verificationBootstrapRetryService().createLinkedRetry(
            "does-not-exist");
        require(!refused.success()
                && refused.error()->code == "verification_environment.retry_source_invalid",
            "BackendRuntime's retry-service accessor did not execute its typed preflight "
            "against a real repository");
    }

    {
        BackendRuntime runtime({workspace.databasePath});
        const auto refused = runtime.verificationBootstrapRetryService().createLinkedRetry(
            "does-not-exist");
        require(!refused.success()
                && refused.error()->code == "verification_environment.retry_source_invalid",
            "The retry-service accessor's wiring to durable storage did not survive a runtime "
            "restart");
    }
}

// QML-P4R.3.3 hardening (blocker review): reconcileInterruptedAttempts()
// was implemented and proven correct at the service level, but was never
// actually composed into BackendRuntime's own startup sequence - so it
// never ran after a real application restart. This test proves the wiring
// itself: a real SQLite database left with a genuinely interrupted manual-
// handoff attempt (a Running attempt Run, a mid-attempt successor proposal,
// no Work Ledger entry) must already be fully reconciled by the time the
// NEXT BackendRuntime's constructor returns - the same "first read already
// reflects recovered state" guarantee recoveryError()'s own doc comment
// states for ordinary Run recovery - with zero direct calls to the manual-
// handoff service from this test (it is not yet a public BackendRuntime
// accessor - QML-P4R.5's job); only BackendRuntime's own constructor is
// exercised, exactly like every other restart test in this file.
void testManualHandoffReconciliationRunsOnBackendRuntimeRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    const RunId sourceRunId = "manual-handoff-restart-source";
    const RunId attemptRunId = "manual-handoff-restart-attempt";
    const std::string handoffId = "manual-handoff:restart-source";
    ChangeSetId successorId;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh manual-handoff-restart database must be available");
        const Project project = createProject(runtime.projectService(), workspace.projectRoot);
        projectId = project.id;
        // Destroyed cleanly here; the real crash is seeded below through a
        // raw connection, exactly like testRecoveryMarksUnfinishedRunInterruptedBeforeFirstRead
        // seeds its own unfinished run.
    }

    {
        // A raw connection stands in for a prior process that crashed
        // mid-reverify: it writes the exact shape importAndReverify() itself
        // would have left behind at that point - a Running attempt Run and
        // stage, a real successor proposal materialized mid-attempt, and NO
        // Work Ledger entry - then is closed without ever going through
        // ManifestVerificationManualHandoffService's own clean paths.
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open a raw connection to seed the interrupted attempt");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());
        ai::infrastructure::SqliteManifestVerificationManualHandoffRepository rawHandoffs(opened.value());
        ai::infrastructure::SqliteChangeProposalRepository rawProposalRepository(opened.value());
        // Real proposal store at the exact path BackendRuntime's own
        // openProposalStorage() derives (config.databasePath.parent_path() /
        // "proposals") so the NEXT BackendRuntime instance reads the same
        // published content this raw ChangeProposalService writes here.
        auto proposalStore = ai::infrastructure::FileChangeProposalStore::open(
            workspace.databasePath.parent_path() / "proposals");
        require(proposalStore.success(), "Could not open the real proposal store at BackendRuntime's own path");
        ChangeProposalService rawProposals(rawProposalRepository, *proposalStore.value());

        Run source;
        source.id = sourceRunId;
        source.projectId = projectId;
        source.title = "Stage 3 failure";
        source.task = "Repair qml lint";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(source).success(), "Could not seed the real source Run");

        CreateChangeProposalRequest componentRequest;
        componentRequest.runId = sourceRunId;
        componentRequest.projectId = projectId;
        componentRequest.providerId = "openai";
        componentRequest.modelId = "gpt-4";
        componentRequest.agentRole = "qml_component_author";
        componentRequest.rawModelOutput =
            R"({"schemaVersion":"proposal-submission.v2","summary":"Fixture",)"
            R"("changes":[{"change_id":"chg-component","operation":"create",)"
            R"("relative_path":"qml/App.qml","source_path":null,"proposed_content":"Item {}"}]})";
        componentRequest.policy.projectRoot = workspace.projectRoot;
        componentRequest.baseRevision = "main";
        const auto componentProposal = rawProposals.createFromModelOutput(componentRequest);
        require(componentProposal.success()
                && componentProposal.value().state == ChangeSetState::ReadyForReview,
            "Could not seed the real, ReadyForReview predecessor proposal");

        ManifestVerificationManualHandoff handoff;
        handoff.id = handoffId;
        handoff.originManifestRunId = sourceRunId;
        handoff.verificationAnchorRunId = sourceRunId;
        handoff.sourceProjectId = projectId;
        handoff.storageKey = "restart-key";
        handoff.workspaceIdentity = projectId;
        handoff.baselineFingerprint = std::string(64, 'a');
        handoff.sourceOverlayFingerprint = std::string(64, 'b');
        handoff.frozenMembers = {
            {componentProposal.value().changeSetId, sourceRunId, std::string(64, 'c'),
                componentProposal.value().changeSetId}};
        handoff.allowedPaths = {{"qml/App.qml", componentProposal.value().changeSetId, "component"}};
        WorkspaceBaselineSnapshot emptySnapshot;
        // Genuinely self-consistent (files empty -> fingerprint is sha256
        // of empty material), not an arbitrary placeholder -
        // WorkspaceBaselineProvenance::parseJson recomputes and rejects
        // any mismatch on every SQLite read (see computeBaselineFingerprint).
        // QML-P4R.3.5: enforceRetention()'s own listAllHandoffs() call is
        // the first thing in this test to ever read this handoff row back
        // through that check.
        emptySnapshot.fingerprint = sha256Hex("");
        handoff.originalSnapshot = emptySnapshot;
        handoff.createdAt = std::chrono::system_clock::now();
        require(rawHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

        Run attemptRun;
        attemptRun.id = attemptRunId;
        attemptRun.projectId = projectId;
        attemptRun.title = "Manual handoff reverify";
        attemptRun.task = "Manual handoff reverify";
        attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
        attemptRun.parentRunId = sourceRunId;
        // Genuinely Running - a clean shutdown never leaves this behind; a
        // real crash does. recoverInterruptedRuns() itself is what must
        // first turn this into Interrupted before reconciliation ever sees
        // it, exactly like testRecoveryMarksUnfinishedRunInterruptedBeforeFirstRead
        // proves for an ordinary Run.
        attemptRun.status = RunStatus::Running;
        attemptRun.progress = 0.3;
        attemptRun.createdAt = std::chrono::system_clock::now();
        attemptRun.startedAt = attemptRun.createdAt;
        attemptRun.currentStageId = attemptRunId + ":manual-handoff-reverify";
        RunStage stage;
        stage.id = *attemptRun.currentStageId;
        stage.runId = attemptRunId;
        stage.type = "manifest-verification-manual-handoff-reverify";
        stage.title = "Manual handoff reverify";
        stage.description = "Backend-verified user-edited candidate; no provider/model call";
        stage.status = StageStatus::Running;
        stage.progress = 0.3;
        stage.startedAt = attemptRun.createdAt;
        require(rawRuns.insertRunWithStages(attemptRun, {stage}).success(),
            "Could not seed the real Running attempt Run and stage");

        // A real successor, materialized mid-attempt before the simulated
        // crash - exactly what importAndReverify() itself does via
        // materializeRepairSuccessor().
        MaterializeRepairSuccessorRequest repairRequest;
        repairRequest.runId = attemptRunId;
        repairRequest.projectId = projectId;
        repairRequest.providerId = "manual-handoff";
        repairRequest.modelId = "manual-handoff";
        repairRequest.agentRole = "user";
        repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
        repairRequest.repairRawModelOutput = R"({
            "schemaVersion": "proposal-submission.v2",
            "summary": "Manual handoff edit",
            "changes": [
                {"change_id":"manual-1","operation":"create","relative_path":"qml/App.qml",
                 "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
            ]
        })";
        repairRequest.policy.projectRoot = workspace.projectRoot;
        repairRequest.policy.allowedRoots = {"qml/App.qml"};
        repairRequest.policy.allowedRootsAreExactPaths = true;
        repairRequest.expectedPredecessorRunId = sourceRunId;
        const auto successor = rawProposals.materializeRepairSuccessor(repairRequest);
        require(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
            "Could not seed the real mid-attempt successor proposal");
        successorId = successor.value().changeSetId;

        ManifestVerificationManualHandoffAttempt attempt;
        attempt.id = "manual-handoff-attempt:restart";
        attempt.handoffId = handoffId;
        attempt.verificationAnchorRunId = sourceRunId;
        attempt.attemptRunId = attemptRunId;
        attempt.attemptOrdinal = 1;
        attempt.editedSnapshotFingerprint = std::string(64, 'e');
        attempt.createdAt = std::chrono::system_clock::now();
        require(rawHandoffs.insertAttempt(attempt).success(),
            "Could not seed the real interrupted attempt record");

        // No Work Ledger entry for attemptRunId - the simulated crash
        // happened before that authoritative write.
    }

    {
        // The next real application start. Reconciliation must already have
        // run by the time this constructor returns - no test code calls the
        // manual-handoff service directly.
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Database with the interrupted attempt must be available");
        require(!runtime.recoveryError().has_value(),
            "Startup reconciliation of a real interrupted manual-handoff attempt (Running Run, "
            "a real mid-attempt successor, no Work Ledger entry) must succeed with zero "
            "recovery error - this is the exact gap the P4R.3.3 review found: reconciliation "
            "existed but was never actually composed into BackendRuntime's own startup path");

        const Run recoveredAttempt = findRun(runtime.orchestrationService(), attemptRunId);
        require(recoveredAttempt.status == RunStatus::Failed,
            "The interrupted attempt Run must reach reconcileInterruptedAttempts()'s own "
            "terminal Failed status, never left at the generic recoverInterruptedRuns() "
            "Interrupted status forever");

        const auto stages = runtime.orchestrationService().listStages(attemptRunId);
        require(stages.success() && stages.value().size() == 1
                && stages.value().front().status == StageStatus::Failed,
            "The attempt's own stage - never touched by the generic recoverInterruptedRuns() "
            "pass - must also reach a terminal Failed status");
    }

    {
        // BackendRuntime does not yet expose proposal reads publicly
        // (QML-P4R.5's job) - a fresh raw connection, opened only after the
        // runtime above has already closed, confirms the successor's own
        // missing-evidence annotation durably persisted through real SQLite.
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open a raw connection to verify the final state");
        ai::infrastructure::SqliteChangeProposalRepository rawProposalRepository(opened.value());
        const auto successorAfter = rawProposalRepository.findChangeSet(successorId);
        require(successorAfter.success() && successorAfter.value().has_value()
                && successorAfter.value()->missingEvidenceCode.has_value()
                && *successorAfter.value()->missingEvidenceCode == "manual_handoff.attempt_incomplete",
            "The mid-attempt successor must be durably marked missing-evidence by the real "
            "startup reconciliation pass, not merely in an in-memory test double");
    }
}

// PC-4C: the Outcome Brief reconciler is wired into the runtime. A Completed Manifest Run with one reviewable proposal, left behind
// by an earlier process, gets its brief from the history pass; a human rejection through the REAL review service moves it on at
// once (the producer's nudge); and another restart produces no new revision - duplicate recovery is one revision with identical bytes.
void testOutcomeBriefIsReconciledThroughBackendRuntime()
{
    namespace oc = outcomebrief_contract;
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    const RunId runId = "outcome-brief-run";
    const RunId attemptRunId = "outcome-brief-attempt";
    ChangeSetId changeSetId;
    ChangeSetId secondChangeSetId;

    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "Fresh outcome-brief database must be available");
        projectId = createProject(runtime.projectService(), workspace.projectRoot).id;
    }
    {
        // A raw connection stands in for the earlier process: a finished Manifest Run, its proposal and its Work Ledger entry.
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open a raw connection to seed the finished Run");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());
        ai::infrastructure::SqliteChangeProposalRepository rawProposalRepository(opened.value());
        ai::infrastructure::SqliteManifestWorkLedgerRepository rawLedger(opened.value());
        auto proposalStore = ai::infrastructure::FileChangeProposalStore::open(workspace.databasePath.parent_path() / "proposals");
        require(proposalStore.success(), "Could not open the real proposal store at BackendRuntime's own path");
        ChangeProposalService rawProposals(rawProposalRepository, *proposalStore.value());

        Run run;
        run.id = runId;
        run.projectId = projectId;
        run.title = "Outcome brief";
        run.task = "Produce a reviewable proposal";
        run.workflowId = kExecutionManifestWorkflowId;
        run.status = RunStatus::Completed;
        run.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(run).success(), "Could not seed the finished Run");

        CreateChangeProposalRequest request;
        request.runId = runId;
        request.projectId = projectId;
        request.providerId = "openai";
        request.modelId = "gpt-4";
        request.agentRole = "qml_component_author";
        request.rawModelOutput =
            R"({"schemaVersion":"proposal-submission.v2","summary":"Fixture",)"
            R"("changes":[{"change_id":"chg-1","operation":"create",)"
            R"("relative_path":"qml/App.qml","source_path":null,"proposed_content":"Item {}"}]})";
        request.policy.projectRoot = workspace.projectRoot;
        request.baseRevision = "main";
        const auto proposal = rawProposals.createFromModelOutput(request);
        require(proposal.success() && proposal.value().state == ChangeSetState::ReadyForReview, "Could not seed the reviewable proposal");
        changeSetId = proposal.value().changeSetId;

        // A second reviewable proposal of the same Run: the first is rejected below, the second approved.
        CreateChangeProposalRequest secondRequest = request;
        secondRequest.rawModelOutput =
            R"({"schemaVersion":"proposal-submission.v2","summary":"Second fixture",)"
            R"("changes":[{"change_id":"chg-2","operation":"create",)"
            R"("relative_path":"qml/Other.qml","source_path":null,"proposed_content":"Item {}"}]})";
        const auto secondProposal = rawProposals.createFromModelOutput(secondRequest);
        require(secondProposal.success() && secondProposal.value().state == ChangeSetState::ReadyForReview, "Could not seed the second proposal");
        secondChangeSetId = secondProposal.value().changeSetId;

        auto built = oc::terminalFacts(runId, projectId, RunStatus::Completed, {oc::ready(changeSetId), oc::ready(secondChangeSetId)});
        require(built.ledgerEntry.has_value(), "setup: a ledger entry");
        // The stored verification identity is validated on every read, so it must be a real one (the shape the combined verification
        // leaves behind for the one reviewable proposal).
        ManifestProposalSetVerification verification = *built.ledgerEntry->verification;
        verification.workspaceIdentity = projectId;
        verification.baselineFingerprint = std::string(64, 'b');
        verification.overlayFingerprint = std::string(64, 'd');
        verification.orderedProposalIds = {changeSetId, secondChangeSetId};
        verification.orderedProposalContentHashes = {std::string(64, 'c'), std::string(64, 'd')};
        built.ledgerEntry->verification = verification;
        const auto inserted = rawLedger.insertEntry(*built.ledgerEntry);
        require(inserted.success(), "Could not seed the Work Ledger entry");
        const auto readBack = rawLedger.findEntry(runId);
        require(readBack.success() && readBack.value().has_value(),
            (std::string("The seeded Work Ledger entry must read back: ") + (readBack.error().has_value() ? readBack.error()->message : std::string())).c_str());

        // A handoff attempt Run is never an outcome Run: the history pass must leave it alone.
        Run attempt;
        attempt.id = attemptRunId;
        attempt.projectId = projectId;
        attempt.title = "Attempt";
        attempt.task = "Attempt";
        attempt.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
        attempt.status = RunStatus::Completed;
        attempt.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(attempt).success(), "Could not seed the attempt Run");
    }

    std::string firstBytes;
    {
        BackendRuntime runtime({workspace.databasePath});
        require(runtime.storageAvailable(), "The seeded database must open");
        require(!runtime.waitForOutcomeBriefBackfill().has_value(), "Nothing runs until the pass is started");
        runtime.startOutcomeBriefBackfill();
        runtime.startOutcomeBriefBackfill();   // a second start does nothing
        const auto pass = runtime.waitForOutcomeBriefBackfill();
        {
            std::string detail = "The history pass must finish cleanly";
            if (!pass.has_value()) {
                detail += ": it has no result";
            } else {
                detail += ": stopped=" + std::to_string(pass->stopped) + " failed=" + std::to_string(pass->failed) + " examined="
                    + std::to_string(pass->examined);
                for (const auto& failedRunId : pass->failedRunIds) {
                    const auto why = runtime.outcomeBriefReconciler().reconcileRun(failedRunId);
                    detail += " [" + failedRunId + ": " + (why.success() ? std::string("now ok") : why.error()->code + " " + why.error()->message) + "]";
                }
            }
            require(pass.has_value() && !pass->stopped && pass->failed == 0, detail.c_str());
        }
        require(pass->examined == 1 && pass->appended == 1, "It examines the one outcome Run (not the attempt) and writes its brief");

        const auto head = runtime.outcomeBriefRepository().findHead(runId);
        require(head.success() && head.value().has_value() && head.value()->brief.state == OutcomeBriefState::AwaitingReview,
            "The finished Run is awaiting review");
        require(!runtime.outcomeBriefRepository().findHead(attemptRunId).value().has_value(), "The attempt Run has no brief");
        firstBytes = head.value()->brief.renderedText;

        // The REAL review service rejects; its nudge updates the brief synchronously, with no pass and no pull.
        const auto rejected = runtime.proposalReviewService().reject(changeSetId, std::string("reviewer"), std::string("not wanted"));
        require(rejected.success(), "The review service must reject the proposal");
        const auto after = runtime.outcomeBriefRepository().findHead(runId);
        require(after.success() && after.value().has_value() && after.value()->brief.state == OutcomeBriefState::AwaitingReview
                && after.value()->ordinal == 2 && after.value()->brief.counts.humanRejected == 1,
            "A rejection is in the brief at once (the review service's nudge): one rejected, one still awaiting review");

        // (The apply service's approve / apply nudges are proved at their own level: through the runtime a Manifest proposal can only
        // be approved after the AG-16 verification authority says so, which needs a verified Run.)
        require(runtime.outcomeBriefReconciler().reconcileRun(runId).value().kind
                == OutcomeBriefReconcileOutcome::Kind::Unchanged,
            "The on-demand pull finds nothing left to change");
    }
    {
        // Another restart: duplicate recovery changes nothing.
        BackendRuntime runtime({workspace.databasePath});
        runtime.startOutcomeBriefBackfill();
        const auto pass = runtime.waitForOutcomeBriefBackfill();
        require(pass.has_value() && pass->examined == 1 && pass->appended == 0 && pass->unchanged == 1,
            "A restart appends nothing: the head already says exactly this");
        const auto revisions = runtime.outcomeBriefRepository().listRevisions(runId, 10);
        require(revisions.success() && revisions.value().size() == 2, "Still exactly the two revisions");
        require(revisions.value().front().brief.renderedText == firstBytes, "...and the first one is byte for byte what it was");
        // Destroyed right after starting a pass: the worker is stopped and joined before the storage goes away.
        runtime.startOutcomeBriefBackfill();
    }
    {
        BackendRuntime runtime({workspace.databasePath});
        runtime.startOutcomeBriefBackfill();   // no wait: the destructor must stop and join the worker
    }
}

// PC-4C: the producers that cannot be driven to a brief without a provider or a verified Run are still observably wired - each one nudges
// the runtime's reconciler (the counts), through the REAL service the runtime composes.
void testTheProducersAreWiredToTheOutcomeBriefReconciler()
{
    TemporaryPersistenceWorkspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "Fresh wiring database must be available");
    const Project project = createProject(runtime.projectService(), workspace.projectRoot);
    const auto waitForRunNudges = [&](std::uint64_t atLeast) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (runtime.outcomeBriefReconciler().nudgeCounts().runs >= atLeast) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    };
    require(runtime.outcomeBriefReconciler().nudgeCounts().runs == 0 && runtime.outcomeBriefReconciler().nudgeCounts().proposals == 0,
        "Nothing has been nudged yet (the startup recovery does not nudge)");

    // RunManager::finishRun: a Run that finishes (here a Mock one) nudges once.
    const auto started = runtime.orchestrationService().startRun({project.id, "Wired run", "Finish and be briefed"});
    require(started.success(), "startRun must succeed");
    static_cast<void>(waitForTerminalRun(runtime.orchestrationService(), started.value()));
    require(waitForRunNudges(1), "A finished Run must nudge the Outcome Brief reconciler");
    const std::uint64_t afterRun = runtime.outcomeBriefReconciler().nudgeCounts().runs;

    // Continuation preparation and the paid child's finish nudge only at the end of a verified, paid flow: their wiring is asserted.
    IOutcomeBriefTrigger* const reconciler = &runtime.outcomeBriefReconciler();
    require(runtime.manifestVerificationContinuationPreparationService().outcomeBriefTrigger() == reconciler,
        "Continuation preparation must be wired to the Outcome Brief reconciler");
    require(runtime.manifestVerificationContinuationRepairService().outcomeBriefTrigger() == reconciler,
        "The continuation repair (child finish) must be wired to the Outcome Brief reconciler");

    // ManualHandoffService: materialize nudges its anchor on every exit, also when it fails.
    const auto materialized = runtime.manifestVerificationManualHandoffService().materialize("no-such-anchor-run");
    require(!materialized.success(), "A handoff of an unknown Run cannot be materialized");
    require(runtime.outcomeBriefReconciler().nudgeCounts().runs == afterRun + 1,
        "...but the attempt still nudges the brief of its anchor (the nudge is idempotent)");

    // ChangeSetApplyService::approve: a proposal of a non-Manifest Run needs no verification authority.
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open a raw connection");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());
        Run run;
        run.id = "wired-profile-run";
        run.projectId = project.id;
        run.title = "Profile run";
        run.task = "Propose";
        run.workflowId = kExecutionProfileWorkflowId;
        run.status = RunStatus::Completed;
        run.workspacePath = workspace.projectRoot;
        run.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(run).success(), "Could not seed the profile Run");
    }
    auto proposalStore = ai::infrastructure::FileChangeProposalStore::open(workspace.databasePath.parent_path() / "proposals");
    require(proposalStore.success(), "Could not open the proposal store");
    CreateChangeProposalRequest request;
    request.runId = "wired-profile-run";
    request.projectId = project.id;
    request.providerId = "openai";
    request.modelId = "gpt-4";
    request.agentRole = "coder";
    request.rawModelOutput =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Wired",)"
        R"("changes":[{"change_id":"chg-w","operation":"create",)"
        R"("relative_path":"wired.txt","source_path":null,"proposed_content":"hi"}]})";
    request.policy.projectRoot = workspace.projectRoot;
    request.baseRevision = "main";
    const auto proposal = runtime.changeProposalService().createFromModelOutput(request);
    require(proposal.success() && proposal.value().state == ChangeSetState::ReadyForReview, "Could not create the proposal through the runtime");
    const auto proposalNudgesBefore = runtime.outcomeBriefReconciler().nudgeCounts().proposals;
    const auto approved = runtime.changeSetApplyService().approve(proposal.value().changeSetId, std::string("reviewer"));
    require(approved.success(), "The apply service must approve a non-Manifest proposal");
    require(runtime.outcomeBriefReconciler().nudgeCounts().proposals == proposalNudgesBefore + 1,
        "An approval nudges the Outcome Brief reconciler once");
}

// PC-4C (review 2): a database file has ONE live owner. The guards the application keeps in memory (the path claims, the manual-handoff
// import lock, the single Outcome Brief reconciler whose verdicts are validated against each other) assume it, so a second runtime - another
// instance of the application, or another runtime of this process - is refused instead of racing the first, and the Outcome Brief
// repository is handed out read-only: the reconciler is its only writer, by type and by lock.
void testTheRuntimeIsTheOneLiveOwnerOfItsDatabase()
{
    using ai::infrastructure::DatabaseOwnerLock;
    TemporaryPersistenceWorkspace workspace;

    // The lock itself.
    {
        auto first = DatabaseOwnerLock::acquire(workspace.databasePath);
        require(first.success() && first.value() != nullptr, "The first owner takes the lock");
        const fs::path lockFile = first.value()->lockFilePath();
        require(!lockFile.empty() && fs::exists(lockFile), "...which is a companion file next to the database");

        const auto second = DatabaseOwnerLock::acquire(workspace.databasePath);
        require(!second.success() && second.error()->code == "persistence.owned_by_another_instance",
            "A second owner of the same file is refused");
        require(second.error()->message.find(workspace.root.string()) == std::string::npos, "...with a message that carries no path");

        const auto another = DatabaseOwnerLock::acquire(workspace.root / "state" / "another.sqlite3");
        require(another.success(), "Another file has its own owner");

        first.value().reset();
        require(fs::exists(lockFile),
            "The companion file stays when its owner goes (a deleted and re-created file would let two owners hold two files)");
        const auto again = DatabaseOwnerLock::acquire(workspace.databasePath);
        require(again.success(), "The lock is free the moment its owner is gone");
    }
    {
        const auto memoryA = DatabaseOwnerLock::acquire(":memory:");
        const auto memoryB = DatabaseOwnerLock::acquire(":memory:");
        require(memoryA.success() && memoryB.success() && memoryA.value()->lockFilePath().empty(),
            "An in-memory database has no other process to share with, so it needs no lock");
        const auto empty = DatabaseOwnerLock::acquire(fs::path());
        require(!empty.success() && empty.error()->code == "persistence.empty_path", "An empty path is refused, as it always was");
    }

    // The runtime.
    {
        BackendRuntime owner({workspace.databasePath});
        require(owner.storageAvailable(), "The first runtime owns the database");
        const Project project = createProject(owner.projectService(), workspace.projectRoot);
        {
            BackendRuntime intruder({workspace.databasePath});
            require(!intruder.storageAvailable() && intruder.storageError().has_value()
                    && intruder.storageError()->code == "persistence.owned_by_another_instance",
                "A second runtime over the same file gets unavailable storage, with the reason");
            require(!intruder.outcomeBriefReconciler().reconcileRun("any-run").success(),
                "...so its reconciler has nothing to read and nothing to write");
            const auto created = intruder.projectService().createProject({"Intruder", "Must not persist", workspace.projectRoot});
            require(!created.success() && created.error()->code == "storage.unavailable", "...and nothing it does reaches the database");
        }
        require(!DatabaseOwnerLock::acquire(workspace.databasePath).success(),
            "The refused runtime took nothing from the owner: the file is still owned when it is gone");
        const auto listed = owner.projectService().listProjects();
        require(listed.success() && listed.value().size() == 1 && listed.value().front().id == project.id,
            "The owner is not disturbed: its data is what it was");
    }
    {
        BackendRuntime next({workspace.databasePath});
        require(next.storageAvailable(), "Once the owner is gone the file has an owner again");
        const auto listed = next.projectService().listProjects();
        require(listed.success() && listed.value().size() == 1, "...and its data is intact");
    }

    static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<BackendRuntime&>().outcomeBriefRepository())>>,
        "The Outcome Brief repository is handed out read-only: the reconciler is the only writer");
}

// PC-4C (review 2), test fixtures only: a fixture that seeds a database through a short-lived second runtime while the first is alive
// builds the second one as a GUEST (BackendRuntimeConfig::exclusiveDatabaseOwnership = false). A guest opens the file the owner holds,
// but it is not the owner: it takes no lock and writes no brief - none of its producers is wired to its reconciler and its history pass
// does not run - so there is never a second reconciler writing to one file, not even in a test. The public accessor of the guest's
// reconciler is no way around that: asked by hand (review 3), it refuses, before it reads a source, and the owner can still reconcile.
void testAGuestRuntimeOwnsNothingAndWritesNoBrief()
{
    namespace oc = outcomebrief_contract;
    TemporaryPersistenceWorkspace workspace;
    BackendRuntime owner({workspace.databasePath});
    require(owner.storageAvailable(), "The owner opens the database");
    const Project project = createProject(owner.projectService(), workspace.projectRoot);

    // A terminal outcome Run - a Failed Manifest Run with its Work Ledger entry - as an earlier process would have left it.
    const RunId outcomeRunId = "guest-outcome-run";
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open a raw connection to seed the terminal Run");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());
        ai::infrastructure::SqliteManifestWorkLedgerRepository rawLedger(opened.value());
        Run run;
        run.id = outcomeRunId;
        run.projectId = project.id;
        run.title = "Guest outcome";
        run.task = "A finished Manifest Run";
        run.workflowId = kExecutionManifestWorkflowId;
        run.status = RunStatus::Failed;
        run.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(run).success(), "Could not seed the terminal Run");
        auto built = oc::terminalFacts(outcomeRunId, project.id, RunStatus::Failed, {}, ManifestWorkLedgerDisposition::NotProduced);
        require(built.ledgerEntry.has_value(), "setup: a Work Ledger entry");
        // A Failed Run whose combined verification never ran: the column is empty, which is legitimate (an early failure) and reads back.
        built.ledgerEntry->verification.reset();
        built.ledgerEntry->verificationSummary = {};
        require(rawLedger.insertEntry(*built.ledgerEntry).success(), "Could not seed the Work Ledger entry");
    }

    BackendRuntimeConfig guestConfig {workspace.databasePath};
    guestConfig.exclusiveDatabaseOwnership = false;
    {
        BackendRuntime guest(guestConfig);
        require(guest.storageAvailable() && !guest.storageError().has_value(), "A guest opens the file the owner holds");
        const auto started = guest.orchestrationService().startRun({project.id, "Guest run", "Finish, unbriefed"});
        require(started.success(), "startRun must succeed in the guest");
        static_cast<void>(waitForTerminalRun(guest.orchestrationService(), started.value()));
        std::this_thread::sleep_for(300ms);   // a producer that was wired would have nudged long ago
        require(guest.outcomeBriefReconciler().nudgeCounts().runs == 0 && guest.outcomeBriefReconciler().nudgeCounts().proposals == 0,
            "A guest's producers are not wired to its reconciler: the Run that finished in it nudged nothing");
        guest.startOutcomeBriefBackfill();
        require(!guest.waitForOutcomeBriefBackfill().has_value(), "...and its history pass does not run");
        require(!guest.outcomeBriefRepository().findHead(started.value()).value().has_value(), "...so there is no brief of that Run");

        // The accessor of the guest's reconciler is public: asked BY HAND, about a terminal outcome Run that really exists, it refuses.
        const auto pulled = guest.outcomeBriefReconciler().reconcileRun(outcomeRunId);
        require(!pulled.success() && pulled.error()->code == "outcome_brief.not_the_owner",
            "A guest's reconciler refuses an explicit reconcile of an existing terminal Run");
        guest.outcomeBriefReconciler().runMayHaveChanged(outcomeRunId);   // the nudge entries too: they produce nothing
        guest.outcomeBriefReconciler().proposalResolved("any-proposal");
        const auto history = guest.outcomeBriefReconciler().reconcileHistory();
        require(!history.success() && history.error()->code == "outcome_brief.not_the_owner", "...and so does an explicit history pass");
        require(!guest.outcomeBriefRepository().findHead(outcomeRunId).value().has_value(), "...so no head appeared (seen from the guest)");
        require(!owner.outcomeBriefRepository().findHead(outcomeRunId).value().has_value(), "...nor from the owner");

        const auto followUp = guest.followUpPlanService().listForProject(project.id, 10);
        require(!followUp.success() && followUp.error()->code == "follow_up.not_the_owner",
            "A guest's follow-up surface refuses before it can read or write the owner's requests");

        // The guest took no lock, and giving it up releases none: the file is still the owner's.
        require(!ai::infrastructure::DatabaseOwnerLock::acquire(workspace.databasePath).success(), "The guest holds no ownership lock");
    }
    require(!ai::infrastructure::DatabaseOwnerLock::acquire(workspace.databasePath).success(), "...and the owner still holds its own");

    // The owner can reconcile that very Run: the guest took nothing from it.
    const auto owned = owner.outcomeBriefReconciler().reconcileRun(outcomeRunId);
    std::string ownedDetail = "The owner reconciles the terminal Run and writes its brief";
    if (!owned.success()) {
        ownedDetail += ": failed with " + owned.error()->code + " " + owned.error()->message;
    } else {
        ownedDetail += ": kind=" + std::to_string(static_cast<int>(owned.value().kind)) + " state="
            + (owned.value().state.has_value() ? std::string(toString(*owned.value().state)) : std::string("none"))
            + " ordinal=" + std::to_string(owned.value().ordinal.value_or(0)) + " skipped=" + owned.value().skippedBecause;
    }
    require(owned.success() && owned.value().kind == OutcomeBriefReconcileOutcome::Kind::Appended
            && owned.value().state == OutcomeBriefState::RecoveryOnly && owned.value().ordinal == 1u,
        ownedDetail.c_str());
    const auto stored = owner.outcomeBriefRepository().findHead(outcomeRunId);
    require(stored.success() && stored.value().has_value() && stored.value()->ordinal == 1u, "...which is the one and only revision");

    // The owner, for contrast, is the one whose history pass runs.
    owner.startOutcomeBriefBackfill();
    const auto pass = owner.waitForOutcomeBriefBackfill();
    require(pass.has_value() && !pass->stopped && pass->failed == 0, "The owner's history pass runs and finishes cleanly");
}

// PC-5C-2b: this is the REAL production startup sequence, reached by constructing BackendRuntime.
// Follow-up rows are settled by their request owner before the generic execution sweep: durable
// paid accounting is preserved, a stopped paid response remains Cancelled, and a call with no
// response becomes an interruption with zero usage. An unrelated execution still follows the
// generic path. No provider is configured or called.
void testFollowUpAccountingOwnsItsExecutionsDuringProductionStartupRecovery()
{
    using namespace followup_test;
    TemporaryPersistenceWorkspace workspace;
    Project project;
    {
        BackendRuntime seed({workspace.databasePath});
        project = createProject(seed.projectService(), workspace.projectRoot);
    }

    ModelExecutionId paidExecutionId;
    ModelExecutionId cancelledExecutionId;
    ModelExecutionId unansweredExecutionId;
    ModelExecutionId blockedPaidExecutionId;
    ModelExecutionId orphanedExecutionId;
    ModelExecutionId genericExecutionId;
    const Catalog catalog;
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open the database for the follow-up recovery fixture");
        const auto database = opened.value();
        ai::infrastructure::SqliteProviderRepository providers(database);
        ai::infrastructure::SqliteModelRepository models(database);
        ai::infrastructure::SqliteRunRepository runs(database);
        ai::infrastructure::SqliteFollowUpPlanRepository requests(database);
        ai::infrastructure::SqliteModelExecutionRepository executionRows(database);
        ModelExecutionService executions(executionRows);

        const auto now = std::chrono::system_clock::now();
        Provider provider {catalog.providerId, "Recovery provider", catalog.providerKind, catalog.baseUrl, true,
            catalog.credentialReference, now, now};
        require(providers.insert(provider).success(), "Could not seed the follow-up provider");
        Model model {catalog.modelId, catalog.providerId, "model-api", "Recovery model", 200'000, 64'000, 3'000'000,
            15'000'000, 3'750'000, 300'000, true, true, now, now};
        require(models.insert(model).success(), "Could not seed the follow-up model");

        const auto seedRun = [&](const std::string& id) {
            Run run;
            run.id = id;
            run.projectId = project.id;
            run.title = id;
            run.task = "Recovery fixture";
            run.workflowId = kExecutionManifestWorkflowId;
            run.status = RunStatus::Completed;
            run.createdAt = now;
            require(runs.insertRun(run).success(), "Could not seed a follow-up source Run");
        };
        const auto driveToComposing = [&](const std::string& requestId, const std::string& runId) {
            seedRun(runId);
            auto request = quotedRequest(catalog, requestId, project.id, runId, at(0));
            require(requests.insertQuoted(request).success(), "Could not insert a quoted follow-up request");
            require(requests.transition(requestId, authorizeStep(request), at(1)).success(), "Could not authorize the request");
            require(requests.transition(requestId,
                plainStep(FollowUpPlanEvent::Claim, FollowUpPlanRequestState::Authorized), at(2)).success(),
                "Could not claim the request");
        };
        const auto executionFor = [&](const std::string& requestId, ModelExecutionSource source, bool start) {
            CreateModelExecutionRequest create;
            create.source = source;
            create.sourceId = requestId;
            create.projectId = project.id;
            create.providerId = catalog.providerId;
            create.modelId = catalog.modelId;
            create.role = source == ModelExecutionSource::FollowUp ? "follow_up_composer" : "chat_assistant";
            create.requestHash = hex64('d');
            create.contextHash = hex64('e');
            const auto row = executions.createExecution(create);
            require(row.success(), "Could not seed an execution row");
            if (start) require(executions.startExecution(row.value().id).success(), "Could not start an execution row");
            return row.value().id;
        };

        driveToComposing("follow-paid", "follow-run-paid");
        require(requests.transition("follow-paid", planStep(answerOf()), at(3)).success(),
            "Could not store the paid follow-up result");
        paidExecutionId = executionFor("follow-paid", ModelExecutionSource::FollowUp, true);

        driveToComposing("follow-cancelled", "follow-run-cancelled");
        auto cancel = plainStep(FollowUpPlanEvent::Cancel, FollowUpPlanRequestState::Composing);
        cancel.accounting = accountingOf();
        require(requests.transition("follow-cancelled", std::move(cancel), at(3)).success(),
            "Could not store the cancelled paid follow-up result");
        cancelledExecutionId = executionFor("follow-cancelled", ModelExecutionSource::FollowUp, true);

        driveToComposing("follow-unanswered", "follow-run-unanswered");
        unansweredExecutionId = executionFor("follow-unanswered", ModelExecutionSource::FollowUp, false);
        driveToComposing("follow-blocked-paid", "follow-run-blocked-paid");
        require(requests.transition("follow-blocked-paid", planStep(answerOf()), at(3)).success(),
            "Could not store the paid result whose execution write will fail");
        blockedPaidExecutionId = executionFor("follow-blocked-paid", ModelExecutionSource::FollowUp, true);
        orphanedExecutionId = executionFor("missing-follow-up-request", ModelExecutionSource::FollowUp, true);
        genericExecutionId = executionFor("generic-chat", ModelExecutionSource::Chat, true);

        sqlite3* raw = nullptr;
        const auto utf8 = workspace.databasePath.generic_u8string();
        const std::string databasePath(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        require(sqlite3_open_v2(databasePath.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open the raw trigger connection");
        const std::string trigger = "CREATE TRIGGER block_one_follow_up_settlement BEFORE UPDATE ON model_executions "
            "WHEN OLD.id='" + blockedPaidExecutionId
            + "' BEGIN SELECT RAISE(ABORT,'injected follow-up settlement failure'); END;";
        const int triggerResult = sqlite3_exec(raw, trigger.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close_v2(raw);
        require(triggerResult == SQLITE_OK, "Could not install the follow-up settlement failure trigger");
    }

    BackendRuntime recovered({workspace.databasePath});
    const auto recoveryError = recovered.followUpComposerError();
    require(recoveryError.has_value() && recoveryError->code == "persistence.write_failed"
            && recoveryError->message.find("[persistence.invalid_data]") != std::string::npos,
        "The production error channel must expose both failed accounting repair and orphaned identity");
    const auto paid = recovered.modelExecutionService().findExecution(paidExecutionId);
    const auto cancelled = recovered.modelExecutionService().findExecution(cancelledExecutionId);
    const auto unanswered = recovered.modelExecutionService().findExecution(unansweredExecutionId);
    const auto blockedPaid = recovered.modelExecutionService().findExecution(blockedPaidExecutionId);
    const auto orphaned = recovered.modelExecutionService().findExecution(orphanedExecutionId);
    const auto generic = recovered.modelExecutionService().findExecution(genericExecutionId);
    require(paid.success() && paid.value().has_value() && paid.value()->status == ModelExecutionStatus::Completed
            && paid.value()->inputTokens == accountingOf().inputTokens
            && paid.value()->outputTokens == accountingOf().outputTokens
            && paid.value()->costMicros == accountingOf().costMicros,
        "The production order must restore paid follow-up usage instead of flattening it to interrupted");
    require(cancelled.success() && cancelled.value().has_value()
            && cancelled.value()->status == ModelExecutionStatus::Cancelled
            && cancelled.value()->inputTokens == accountingOf().inputTokens
            && cancelled.value()->costMicros == accountingOf().costMicros,
        "The production order must restore the exact cancelled paid outcome");
    require(unanswered.success() && unanswered.value().has_value()
            && unanswered.value()->status == ModelExecutionStatus::Failed
            && unanswered.value()->errorCode == kModelExecutionInterruptedErrorCode
            && unanswered.value()->inputTokens == 0 && unanswered.value()->costMicros == 0,
        "The follow-up owner must close a no-response row as an unpriced interruption");
    require(blockedPaid.success() && blockedPaid.value().has_value()
            && blockedPaid.value()->status == ModelExecutionStatus::Running,
        "A paid row whose exact accounting cannot be written remains retryable and is not flattened by generic recovery");
    require(orphaned.success() && orphaned.value().has_value()
            && orphaned.value()->status == ModelExecutionStatus::Running,
        "Neither recovery owner may flatten a corrupt orphaned follow-up row");
    require(generic.success() && generic.value().has_value()
            && generic.value()->status == ModelExecutionStatus::Failed
            && generic.value()->errorCode == kModelExecutionInterruptedErrorCode,
        "The generic startup sweep must still close unrelated unfinished executions");
    const auto interruptedRequest = recovered.followUpPlanService().find("follow-unanswered");
    require(interruptedRequest.success() && interruptedRequest.value().has_value()
            && interruptedRequest.value()->state == FollowUpPlanRequestState::Interrupted,
        "The orphaned composing request must be interrupted before the runtime is returned");
}

// Seeds a provider, a model and (optionally) one source Run into the database a runtime will open next.
struct FollowUpStartupSeed {
    explicit FollowUpStartupSeed(const TemporaryPersistenceWorkspace& workspace)
    {
        {
            BackendRuntime seed({workspace.databasePath});
            project = createProject(seed.projectService(), workspace.projectRoot);
        }
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open the database for the follow-up startup fixture");
        database = opened.value();
        ai::infrastructure::SqliteProviderRepository providers(database);
        ai::infrastructure::SqliteModelRepository models(database);
        const auto now = std::chrono::system_clock::now();
        const followup_test::Catalog catalog;
        Provider provider {catalog.providerId, "Recovery provider", catalog.providerKind, catalog.baseUrl, true,
            catalog.credentialReference, now, now};
        require(providers.insert(provider).success(), "Could not seed the follow-up provider");
        Model model {catalog.modelId, catalog.providerId, "model-api", "Recovery model", 200'000, 64'000, 3'000'000,
            15'000'000, 3'750'000, 300'000, true, true, now, now};
        require(models.insert(model).success(), "Could not seed the follow-up model");
    }

    // A source Run WITHOUT an Outcome Brief: a request on it can never reach the provider (its sources read as `outcome_missing`).
    void seedRun(const std::string& id) const
    {
        ai::infrastructure::SqliteRunRepository runs(database);
        Run run;
        run.id = id;
        run.projectId = project.id;
        run.title = id;
        run.task = "Recovery fixture";
        run.workflowId = kExecutionManifestWorkflowId;
        run.status = RunStatus::Completed;
        run.createdAt = std::chrono::system_clock::now();
        require(runs.insertRun(run).success(), "Could not seed a follow-up source Run");
    }

    Project project;
    std::shared_ptr<ai::infrastructure::SqliteDatabase> database;
};

// When every unfinished execution has an owner, startup reports NOTHING: the follow-up pass must not mistake the rows of other
// sources for orphaned follow-up rows (that would raise a spurious error at every start of the application).
void testAnOwnedFollowUpStartupReportsNoError()
{
    using namespace followup_test;
    TemporaryPersistenceWorkspace workspace;
    const FollowUpStartupSeed seed(workspace);
    ModelExecutionId paidId;
    ModelExecutionId chatId;
    {
        ai::infrastructure::SqliteFollowUpPlanRepository requests(seed.database);
        ai::infrastructure::SqliteModelExecutionRepository executionRows(seed.database);
        ModelExecutionService executions(executionRows);
        const Catalog catalog;
        seed.seedRun("owned-run");
        auto request = quotedRequest(catalog, "owned-request", seed.project.id, "owned-run", at(0));
        require(requests.insertQuoted(request).success(), "Could not insert a quoted follow-up request");
        require(requests.transition("owned-request", authorizeStep(request), at(1)).success(), "Could not authorize");
        require(requests.transition("owned-request", plainStep(FollowUpPlanEvent::Claim, FollowUpPlanRequestState::Authorized), at(2)).success(),
            "Could not claim");
        require(requests.transition("owned-request", planStep(answerOf()), at(3)).success(), "Could not store the paid result");
        const auto create = [&](ModelExecutionSource source, const std::string& sourceId) {
            CreateModelExecutionRequest row;
            row.source = source;
            row.sourceId = sourceId;
            row.projectId = seed.project.id;
            row.providerId = catalog.providerId;
            row.modelId = catalog.modelId;
            row.role = source == ModelExecutionSource::FollowUp ? "follow_up_composer" : "chat_assistant";
            row.requestHash = hex64('d');
            row.contextHash = hex64('e');
            const auto created = executions.createExecution(row);
            require(created.success() && executions.startExecution(created.value().id).success(), "Could not seed an unfinished execution");
            return created.value().id;
        };
        paidId = create(ModelExecutionSource::FollowUp, "owned-request");
        chatId = create(ModelExecutionSource::Chat, "owned-chat");
    }
    BackendRuntime recovered({workspace.databasePath});
    require(!recovered.followUpComposerError().has_value(), "Nothing to report when every follow-up row has its request");
    require(!recovered.modelExecutionRecoveryError().has_value(), "...and the generic pass reports nothing either");
    const auto paid = recovered.modelExecutionService().findExecution(paidId);
    const auto chat = recovered.modelExecutionService().findExecution(chatId);
    require(paid.success() && paid.value().has_value() && paid.value()->status == ModelExecutionStatus::Completed
            && paid.value()->costMicros == accountingOf().costMicros,
        "The paid follow-up row is restored from its request");
    require(chat.success() && chat.value().has_value() && chat.value()->status == ModelExecutionStatus::Failed
            && chat.value()->errorCode == kModelExecutionInterruptedErrorCode,
        "...and the other source is still closed by the generic pass");
}

// A request the owner authorised and that was still live at shutdown is claimed ONCE, by the owner, only after the shared startup
// pass: it leaves `authorized` without the test calling anything. Its source Run has no Outcome Brief, so it ends as a FREE stale and
// no provider is ever reached (this fixture has no credential and no network).
void testALiveAuthorizedRequestIsProcessedOnceAfterTheSharedStartupPass()
{
    using namespace followup_test;
    TemporaryPersistenceWorkspace workspace;
    const FollowUpStartupSeed seed(workspace);
    const auto now = followUpFloorToMilliseconds(std::chrono::system_clock::now());
    {
        ai::infrastructure::SqliteFollowUpPlanRepository requests(seed.database);
        const Catalog catalog;
        seed.seedRun("live-run");
        auto request = quotedRequest(catalog, "live-request", seed.project.id, "live-run", now);
        require(requests.insertQuoted(request).success(), "Could not insert a quoted follow-up request");
        require(requests.transition("live-request", authorizeStep(request), now).success(), "Could not authorize");
    }
    BackendRuntime recovered({workspace.databasePath});
    std::optional<FollowUpPlanRequest> settled;
    for (int attempt = 0; attempt < 200; ++attempt) {
        const auto found = recovered.followUpPlanService().find("live-request");
        require(found.success() && found.value().has_value(), "The request is still there");
        if (found.value()->state != FollowUpPlanRequestState::Authorized) {
            settled = *found.value();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    require(settled.has_value(), "The owner claimed the live authorised request after startup recovery");
    require(settled->state == FollowUpPlanRequestState::Stale && !followUpRequestMayBeCharged(*settled),
        "...it ended as a free stale (no Outcome Brief), never reaching a provider");
}

} // namespace

int runBackendRuntimeTests()
{
    testLifecycleCreateRunCompleteAndPersistAcrossRestart();
    testRecoveryMarksUnfinishedRunInterruptedBeforeFirstRead();
    testStorageFailureIsControlledNotFatal();
    testShutdownWithActiveAndQueuedRunsRecoversCleanly();
    testMockRunProducesReviewableArtifactAcrossRestart();
    testLostDiagnosticArtifactsLeaveDurableWarningsAndEventsAcrossRestart();
    testWorkspaceAndCatalogPersistAcrossRestart();
    testModelExecutionLifecycleAndRecoveryAcrossRestart();
    testChatProviderAttemptsShareTheModelExecutionLedger();
    testComparisonSessionUsesLedgerAndRecoversAcrossRestart();
    testProjectMemorySegmentRecordedAndPersistsAcrossRestart();
    testExecutionManifestContractPersistsAcrossRuntimeRestart();
    testDanglingModelExecutionReconciledWhenRunFinishes();
    testProjectMemoryCompactionServiceIsWiredThroughBackendRuntime();
    testVerificationBootstrapRetryServiceIsWiredThroughBackendRuntime();
    testManualHandoffReconciliationRunsOnBackendRuntimeRestart();
    testOutcomeBriefIsReconciledThroughBackendRuntime();
    testTheProducersAreWiredToTheOutcomeBriefReconciler();
    testTheRuntimeIsTheOneLiveOwnerOfItsDatabase();
    testAGuestRuntimeOwnsNothingAndWritesNoBrief();
    testFollowUpAccountingOwnsItsExecutionsDuringProductionStartupRecovery();
    testAnOwnedFollowUpStartupReportsNoError();
    testALiveAuthorizedRequestIsProcessedOnceAfterTheSharedStartupPass();
    return 0;
}
