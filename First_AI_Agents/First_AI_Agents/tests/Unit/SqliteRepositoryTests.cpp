#include "RunRepositoryContractTests.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/conversations/ChatProjectContext.hpp"
#include "ai/application/settings/ApplicationSettingsService.hpp"
#include "ai/application/catalog/ExecutionProfileService.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/execution/MockWorkflowRunner.hpp"
#include "ai/application/execution/OrchestrationService.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/projects/TaskService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/projects/WorkspaceContextService.hpp"
#include "ai/infrastructure/FileArtifactStore.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

// Native SQLite only; this test must build without the Qt SDK exactly like
// the Persistence target it exercises.
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace ai::application;
using namespace ai::infrastructure;

void require(bool condition, const char* message)
{
    // These checks remain active in Release builds, unlike assert-based tests.
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string rawSql(const fs::path& databasePath, const char* sql);

struct TemporaryPersistenceWorkspace {
    fs::path root;
    fs::path projectRoot;
    fs::path databasePath;

    TemporaryPersistenceWorkspace()
    {
        root = fs::temp_directory_path()
            / ("ai-orchestrator-sqlite-test-"
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

Run waitForTerminalRun(IRunRepository& repository, const RunId& runId)
{
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        require(found.success(), "SQLite run lookup failed while waiting");
        require(found.value().has_value(), "SQLite run disappeared while waiting");
        if (isTerminal(found.value()->status)) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("SQLite-backed run did not finish before timeout");
}

class CapturingWorkflowRunner final : public IWorkflowRunner {
public:
    [[nodiscard]] std::vector<RunStage> createStages(
        const RunId& runId, const ResolvedExecutionPlan& /*plan*/) const override
    {
        // One deterministic stage keeps the context test focused while still
        // exercising RunManager's atomic aggregate creation path.
        RunStage stage;
        stage.id = runId + "-capture-stage";
        stage.runId = runId;
        stage.type = "capture-context";
        stage.title = "Capture context";
        stage.description = "Record the immutable WorkflowRunContext snapshot";
        return {std::move(stage)};
    }

    WorkflowResult run(
        const WorkflowRunContext& context,
        std::stop_token,
        IRunEventSink& eventSink) override
    {
        {
            // The worker writes and the test thread reads this snapshot, so the
            // copy is protected even though terminal persistence is used as the
            // external completion signal.
            std::lock_guard lock(mutex_);
            captured_ = context;
        }

        RunEvent started;
        started.type = RunEventType::StageStarted;
        started.runId = context.run.id;
        started.stageId = context.stages.front().id;
        eventSink.publish(started);

        RunEvent completed;
        completed.type = RunEventType::StageCompleted;
        completed.runId = context.run.id;
        completed.stageId = context.stages.front().id;
        completed.runProgress = 1.0;
        completed.stageProgress = 1.0;
        completed.usage = {7, 5};
        eventSink.publish(completed);

        return {true, RunStatus::Completed, "Context captured", completed.usage};
    }

    [[nodiscard]] std::optional<WorkflowRunContext> capturedContext() const
    {
        std::lock_guard lock(mutex_);
        return captured_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<WorkflowRunContext> captured_;
};

void testMigrationRejectsNewerSchema()
{
    TemporaryPersistenceWorkspace workspace;
    fs::create_directories(workspace.databasePath.parent_path());

    // Build the deliberately unsupported database through native sqlite3
    // directly, the same API the production repository uses, so this test
    // does not depend on Qt or on SqliteDatabase's own (not yet proven)
    // open() path to create the fixture.
    {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(
            sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
                == SQLITE_OK,
            "Could not create future-version DB through native sqlite3");
        require(sqlite3_exec(handle, "PRAGMA user_version = 999;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not set future schema version through native sqlite3");
        sqlite3_close_v2(handle);
    }

    const auto opened = SqliteDatabase::open(workspace.databasePath);
    require(!opened.success(), "A newer SQLite schema must be rejected");
    require(opened.error()->code == "migration.unsupported_version",
        "Unexpected error for a newer SQLite schema");
}

void testNewerSchemaRefusalPreservesFutureMarkerAndRows()
{
    TemporaryPersistenceWorkspace workspace;
    fs::create_directories(workspace.databasePath.parent_path());
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());

    {
        sqlite3* handle = nullptr;
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
                == SQLITE_OK,
            "Could not create future-version DB through native sqlite3");
        require(sqlite3_exec(handle,
                    "CREATE TABLE future_sentinel (id INTEGER PRIMARY KEY, note TEXT NOT NULL);"
                    "INSERT INTO future_sentinel(id, note) VALUES (1, 'kept');"
                    "PRAGMA user_version = 999;",
                    nullptr, nullptr, nullptr)
                == SQLITE_OK,
            "Could not prepare the future-version fixture");
        sqlite3_close_v2(handle);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(!opened.success() && opened.error()->code == "migration.unsupported_version",
            "A newer schema must be refused on every attempt");
    }

    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "Could not inspect the refused database");
    const auto scalar = [&](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK, "Inspection query failed");
        require(sqlite3_step(statement) == SQLITE_ROW, "Inspection query returned no row");
        const int value = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        return value;
    };
    require(scalar("PRAGMA user_version;") == 999, "The refused database lost its future version marker");
    require(scalar("SELECT COUNT(*) FROM future_sentinel WHERE note = 'kept';") == 1,
        "The refused database lost its sentinel row");
    require(scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name <> 'future_sentinel';") == 0,
        "Refusing a newer schema must not create any application table");
    sqlite3_close_v2(handle);
}

void testOpenRefusesDatabaseUnderNonDirectoryParent()
{
    TemporaryPersistenceWorkspace workspace;
    const fs::path blocker = workspace.root / "not-a-directory";
    {
        std::ofstream file(blocker, std::ios::binary);
        file << "plain file";
        require(file.good(), "Could not create the blocking file");
    }

    const auto opened = SqliteDatabase::open(blocker / "orchestrator.sqlite3");
    require(!opened.success() && opened.error()->code == "persistence.directory_failed",
        "A parent that is a file must be reported as persistence.directory_failed");
    require(fs::is_regular_file(blocker), "The blocking file must be left untouched");
}

void testUnicodeDatabaseLocationRoundTripsAndReopens()
{
    TemporaryPersistenceWorkspace workspace;
    // Escapes keep the source ASCII-only; the directory names are Cyrillic and CJK.
    const fs::path unicodeRoot = workspace.root
        / fs::path(std::u8string(u8"Данные-数据"));
    const fs::path unicodeProjectRoot = unicodeRoot / fs::path(std::u8string(u8"проект"));
    const fs::path databasePath = unicodeRoot / "state" / "orchestrator.sqlite3";
    fs::create_directories(unicodeProjectRoot);
    const std::string unicodeName = [] {
        const std::u8string value = u8"Проект 测试";
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    }();
    ProjectId projectId;

    {
        const auto opened = SqliteDatabase::open(databasePath);
        require(opened.success(), "Could not open a database in a Cyrillic/CJK directory");
        require(opened.value()->path() == databasePath, "The database must report the path it was opened with");
        SqliteProjectRepository projects(opened.value());
        ProjectService projectService(projects);
        const auto created = projectService.createProject({unicodeName, "Unicode location", unicodeProjectRoot});
        require(created.success(), "Could not persist a project under a Unicode location");
        projectId = created.value().id;
    }

    require(fs::is_regular_file(databasePath), "The database file must exist in the Unicode directory");

    const auto reopened = SqliteDatabase::open(databasePath);
    require(reopened.success(), "Could not reopen a database in a Cyrillic/CJK directory");
    SqliteProjectRepository projects(reopened.value());
    const auto project = projects.findById(projectId);
    require(project.success() && project.value().has_value(), "Project did not survive reopen in a Unicode location");
    require(project.value()->name == unicodeName, "Unicode project name did not round-trip");
    require(project.value()->rootPath == fs::weakly_canonical(unicodeProjectRoot),
        "Unicode project root did not round-trip");
}

void testRepositoriesPersistAndReopen()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    RunId runId;
    std::vector<StageId> stageIds;

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open fresh SQLite database");
        SqliteProjectRepository projects(opened.value());
        SqliteRunRepository runs(opened.value());
        ProjectService projectService(projects);

        const auto created = projectService.createProject(
            {"Persistent project", "SQLite repository test", workspace.projectRoot});
        require(created.success(), "Could not persist project");
        projectId = created.value().id;

        MockWorkflowRunner workflow(1ms, 2);
        SingleRunnerResolver resolver(workflow);
        RunManager manager(projects, runs, resolver);
        OrchestrationService orchestration(projects, runs, manager);
        const auto started = orchestration.startRun(
            {projectId, "Persistent run", "Complete through SQLite repositories"});
        require(started.success(), "Could not enqueue SQLite-backed run");
        runId = started.value();

        const Run completed = waitForTerminalRun(runs, runId);
        require(completed.status == RunStatus::Completed, "SQLite-backed run did not complete");
        require(completed.progress == 1.0, "Completed run progress was not persisted");
        require(completed.usage.inputTokens == 40 && completed.usage.outputTokens == 20,
            "Run usage was not persisted");

        const auto stages = runs.listStages(runId);
        require(stages.success() && stages.value().size() == 4,
            "Four SQLite-backed stages were expected");
        for (const auto& stage : stages.value()) {
            require(stage.status == StageStatus::Completed, "Completed stage status was not persisted");
            stageIds.push_back(stage.id);
        }

        // An invalid replacement fails after the DELETE and first INSERT. The
        // transaction must roll back so the original four stages remain intact.
        auto invalidStages = stages.value();
        invalidStages[1].id = invalidStages[0].id;
        const auto invalidReplacement = runs.replaceStages(runId, invalidStages);
        require(!invalidReplacement.success(), "Duplicate stage ids must reject replacement");
        const auto afterRollback = runs.listStages(runId);
        require(afterRollback.success() && afterRollback.value().size() == 4,
            "Failed stage replacement was not rolled back");
        require(afterRollback.value()[1].id == stageIds[1],
            "Stage transaction rollback did not restore original order/data");

        // Two snapshots model concurrent writers. Only the first matching
        // revision may commit; the stale second update must be rejected.
        const auto original = runs.findRun(runId);
        require(original.success() && original.value().has_value(), "Completed run was not found");
        Run firstWriter = *original.value();
        Run staleWriter = firstWriter;
        firstWriter.title = "Updated title";
        const auto updated = runs.updateRun(firstWriter);
        require(updated.success(), "First optimistic run update failed");
        staleWriter.title = "Stale overwrite";
        const auto conflict = runs.updateRun(staleWriter);
        require(!conflict.success() && conflict.error()->code == "run.revision_conflict",
            "Stale SQLite run revision was not rejected");
    }

    {
        // Reopen through a new connection to prove the test is not observing an
        // in-process cache owned by the first repository objects.
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen migrated SQLite database");
        SqliteProjectRepository projects(reopened.value());
        SqliteRunRepository runs(reopened.value());

        const auto project = projects.findById(projectId);
        require(project.success() && project.value().has_value(), "Project did not survive reopen");
        require(project.value()->rootPath == fs::weakly_canonical(workspace.projectRoot),
            "Project path did not round-trip through UTF-8 persistence");

        const auto run = runs.findRun(runId);
        require(run.success() && run.value().has_value(), "Run did not survive reopen");
        require(run.value()->status == RunStatus::Completed, "Run terminal state did not survive reopen");
        require(run.value()->title == "Updated title", "Latest run revision did not survive reopen");

        const auto stages = runs.listStages(runId);
        require(stages.success() && stages.value().size() == stageIds.size(),
            "Stages did not survive reopen");
        for (std::size_t index = 0; index < stageIds.size(); ++index) {
            require(stages.value()[index].id == stageIds[index], "Stage ordering changed after reopen");
        }

        const auto byProject = runs.listRuns(projectId);
        require(byProject.success() && byProject.value().size() == 1,
            "Project run filter did not return persisted run");
    }
}

void testSharedRunRepositoryContracts()
{
    // The exact same strict provider-neutral suite (with actionable failures) runs against both adapters;
    // future database implementations can reuse it without copying assertions.
    InMemoryRunRepository memoryRuns;
    ai::tests::runRunRepositoryContractTests(
        memoryRuns,
        "memory-contract",
        "memory-contract-project");
    // AG-17.7C: the legacy conversion aggregate is part of the same contract.
    ai::tests::runLegacyConversionRepositoryContractTests(
        memoryRuns, "memory-conversion", "memory-contract-project", "provider-x", "model-x");

    TemporaryPersistenceWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "Could not open SQLite contract database");
    SqliteProjectRepository projects(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteModelRepository models(opened.value());
    SqliteRunRepository sqliteRuns(opened.value());
    ProjectService projectService(projects);
    ProviderCatalogService catalog(providers, models);
    const auto project = projectService.createProject(
        {"Repository contract project", "Shared adapter contract", workspace.projectRoot});
    require(project.success(), "Could not create SQLite contract project");
    ai::tests::runRunRepositoryContractTests(
        sqliteRuns,
        "sqlite-contract",
        project.value().id);

    // The converted Run's roster references real catalog rows (foreign keys).
    const auto provider = catalog.createProvider(
        {"Contract Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
    require(provider.success(), "Could not create the contract provider");
    const auto model = catalog.createModel(
        {provider.value(), "contract-model", "Contract Model", 128000, 4096, 1000000, 3000000, 0, 0, true});
    require(model.success(), "Could not create the contract model");
    ai::tests::runLegacyConversionRepositoryContractTests(
        sqliteRuns, "sqlite-conversion", project.value().id, provider.value(), model.value());
}

void testLongContextPersistsReopensAndReachesWorkflowRunner()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    RunId runId;

    // Use inputs much larger than the current compact UI editors to prove the
    // Application/SQLite boundary has no accidental short-text truncation.
    std::string description = "Project-level architecture context\n";
    std::string task = "Run-specific objective and acceptance criteria\n";
    for (int index = 0; index < 2048; ++index) {
        description += "module/" + std::to_string(index)
            + ": preserve boundaries, tests, and reviewable artifacts\n";
        task += "criterion " + std::to_string(index)
            + ": inspect, implement, verify, and report without silent truncation\n";
    }
    // ProjectService intentionally trims only outer whitespace. End with
    // content so the equality checks target truncation/round-trip behavior,
    // while thousands of internal newlines remain part of the fixture.
    description += "final project context line";
    task += "final run acceptance line";

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open long-context SQLite database");
        SqliteProjectRepository projects(opened.value());
        ProjectService projectService(projects);
        const auto project = projectService.createProject(
            {"Long context project", description, workspace.projectRoot});
        require(project.success(), "Could not persist long project description");
        projectId = project.value().id;
        require(project.value().description == description, "ProjectService changed multiline description");
    }

    {
        // Reopen before execution so the captured Project proves that
        // WorkflowRunContext came from durable storage, not an in-process DTO.
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen long-context SQLite database");
        SqliteProjectRepository projects(reopened.value());
        SqliteRunRepository runs(reopened.value());
        const auto project = projects.findById(projectId);
        require(project.success() && project.value().has_value(), "Long-context project did not reopen");
        require(project.value()->description == description, "Long description changed after reopen");

        CapturingWorkflowRunner workflow;
        SingleRunnerResolver resolver(workflow);
        RunManager manager(projects, runs, resolver);
        OrchestrationService orchestration(projects, runs, manager);
        const auto started = orchestration.startRun(
            {projectId, "Long context run", task, "mock-sequential-v1"});
        require(started.success(), "Could not enqueue long-context run");
        runId = started.value();
        const auto completed = waitForTerminalRun(runs, runId);
        require(completed.status == RunStatus::Completed, "Capturing workflow did not complete");
        require(completed.task == task, "Persisted run task changed during execution");

        const auto captured = workflow.capturedContext();
        require(captured.has_value(), "Workflow runner did not receive a context snapshot");
        require(captured->project.description == description,
            "Workflow runner received a truncated project description");
        require(captured->run.task == task, "Workflow runner received a truncated run task");
        require(captured->stages.size() == 1, "Workflow runner received an incomplete stage snapshot");
    }

    {
        // A second reopen proves the full user input remains durable after the
        // asynchronous run and its revision/progress updates.
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen completed long-context database");
        SqliteProjectRepository projects(reopened.value());
        SqliteRunRepository runs(reopened.value());
        const auto project = projects.findById(projectId);
        const auto run = runs.findRun(runId);
        require(project.success() && project.value().has_value()
                && project.value()->description == description,
            "Long project context did not survive final restart");
        require(run.success() && run.value().has_value() && run.value()->task == task,
            "Long run task did not survive final restart");
    }
}

void testRecoveryUsesPersistedUnfinishedRows()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    constexpr const char* unfinishedId = "sqlite-recovery-run";

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open recovery SQLite database");
        SqliteProjectRepository projects(opened.value());
        SqliteRunRepository runs(opened.value());
        ProjectService projectService(projects);
        const auto project = projectService.createProject(
            {"Recovery project", "Persist unfinished state", workspace.projectRoot});
        require(project.success(), "Could not create recovery project");
        projectId = project.value().id;

        Run unfinished;
        unfinished.id = unfinishedId;
        unfinished.projectId = projectId;
        unfinished.title = "Interrupted session";
        unfinished.task = "Recover after reopen";
        unfinished.workflowId = "mock-sequential-v1";
        unfinished.status = RunStatus::Running;
        unfinished.progress = 0.25;
        unfinished.createdAt = std::chrono::system_clock::now();
        unfinished.startedAt = unfinished.createdAt;
        require(runs.insertRun(unfinished).success(), "Could not persist unfinished run");
    }

    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen recovery SQLite database");
        SqliteProjectRepository projects(reopened.value());
        SqliteRunRepository runs(reopened.value());
        MockWorkflowRunner workflow(1ms, 1);
        SingleRunnerResolver resolver(workflow);
        RunManager manager(projects, runs, resolver);
        OrchestrationService orchestration(projects, runs, manager);

        const auto unfinished = runs.listUnfinishedRuns();
        require(unfinished.success() && unfinished.value().size() == 1,
            "Persisted unfinished run was not listed");
        require(orchestration.recoverInterruptedRuns().success(),
            "Application recovery failed for SQLite run");

        const auto recovered = runs.findRun(unfinishedId);
        require(recovered.success() && recovered.value().has_value(), "Recovered run was not found");
        require(recovered.value()->status == RunStatus::Interrupted,
            "Recovered SQLite run was not marked interrupted");
        require(recovered.value()->finishedAt.has_value(), "Recovery did not persist finish time");
        require(recovered.value()->errorMessage.find("interrupted application session") != std::string::npos,
            "Recovery reason was not persisted");
        const auto remaining = runs.listUnfinishedRuns();
        require(remaining.success() && remaining.value().empty(),
            "Recovered run remained in the unfinished query");
    }
}

void testArtifactRepositoryAndFileStorePersistAndReopen()
{
    TemporaryPersistenceWorkspace workspace;
    const fs::path artifactRoot = workspace.root / "artifacts";
    ProjectId projectId;
    RunId runId;
    ArtifactId artifactId;
    ArtifactId verificationArtifactId;
    const std::string verificationRecord =
        "Schema: manifest-proposal-set-verification.v1\n"
        "Run: durable-run\n"
        "Baseline fingerprint: baseline-sha256\n"
        "Overlay fingerprint: overlay-sha256\n"
        "Ordered proposal: proposal-1 / proposal-content-sha256\n";

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open artifact SQLite database");
        SqliteProjectRepository projects(opened.value());
        SqliteRunRepository runs(opened.value());
        SqliteArtifactRepository artifacts(opened.value());
        ProjectService projectService(projects);

        const auto storeOpened = FileArtifactStore::open(artifactRoot);
        require(storeOpened.success(), "Could not open FileArtifactStore");
        ArtifactService artifactService(artifacts, *storeOpened.value());

        const auto project = projectService.createProject(
            {"Artifact SQLite project", "SQLite artifact repository test", workspace.projectRoot});
        require(project.success(), "Could not persist artifact project");
        projectId = project.value().id;

        MockWorkflowRunner workflow(1ms, 1, &artifactService);
        SingleRunnerResolver resolver(workflow);
        RunManager manager(projects, runs, resolver);
        OrchestrationService orchestration(projects, runs, manager);
        const auto started = orchestration.startRun(
            {projectId, "Artifact SQLite run", "Persist a reviewable artifact through SQLite"});
        require(started.success(), "Could not enqueue artifact-producing run");
        runId = started.value();

        const Run completed = waitForTerminalRun(runs, runId);
        require(completed.status == RunStatus::Completed, "Artifact-producing run did not complete");

        const auto verificationArtifact = artifactService.createArtifact(
            runId, std::nullopt, ArtifactKind::Report,
            "manifest-verification.md", "text/markdown", verificationRecord);
        require(verificationArtifact.success(),
            "Could not persist the verification restart fixture");
        verificationArtifactId = verificationArtifact.value().id;

        const auto listed = artifactService.listArtifacts(runId);
        require(listed.success() && listed.value().size() >= 3,
            "Mock run did not persist the expected artifacts through SQLite");
        artifactId = listed.value().front().id;

        const auto resolved = artifactService.resolveArtifactPath(artifactId);
        require(resolved.success(), "Could not resolve artifact absolute path");
        require(fs::exists(resolved.value()), "Resolved artifact path does not exist on disk");

        // Attempting to escape the artifact root through a hand-built relative
        // path (bypassing sanitization, as a stored row never would) must be
        // rejected by resolveAbsolutePath's own containment recheck.
        FileArtifactStore& concreteStore = *storeOpened.value();
        const auto escapeAttempt = concreteStore.resolveAbsolutePath("../../outside.txt");
        require(!escapeAttempt.success(), "Path traversal outside the artifact root was not rejected");
    }

    {
        // Reopen through new connections/store instances to prove both the
        // metadata row and the file content are durable, not held only by the
        // first process's in-memory objects.
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen artifact SQLite database");
        SqliteArtifactRepository artifacts(reopened.value());
        const auto storeReopened = FileArtifactStore::open(artifactRoot);
        require(storeReopened.success(), "Could not reopen FileArtifactStore");
        ArtifactService artifactService(artifacts, *storeReopened.value());

        const auto listed = artifactService.listArtifacts(runId);
        require(listed.success() && listed.value().size() >= 3,
            "Artifacts did not survive reopen");

        // Deterministic order must be stable across independent reads.
        const auto listedAgain = artifactService.listArtifacts(runId);
        require(listedAgain.success() && listedAgain.value().size() == listed.value().size(),
            "Artifact listing size changed between reopened reads");
        for (std::size_t index = 0; index < listed.value().size(); ++index) {
            require(listed.value()[index].id == listedAgain.value()[index].id,
                "Artifact listing order was not deterministic after reopen");
        }

        const auto artifact = artifactService.getArtifact(artifactId);
        require(artifact.success(), "Artifact metadata did not survive reopen");
        require(!artifact.value().sha256.empty(), "Artifact sha256 did not survive reopen");

        const auto resolved = artifactService.resolveArtifactPath(artifactId);
        require(resolved.success() && fs::exists(resolved.value()),
            "Artifact content file did not survive reopen");

        const auto content = artifactService.getArtifactContent(artifactId);
        require(content.success(), "Could not read artifact content through FileArtifactStore after reopen");
        require(!content.value().empty(), "Artifact content was empty after reopen");

        const auto verification = artifactService.getArtifactContent(
            verificationArtifactId);
        require(verification.success() && verification.value() == verificationRecord,
            "Verification Run/baseline/proposal identity changed after restart");

        const auto missingContent = artifactService.getArtifactContent("does-not-exist");
        require(!missingContent.success() && missingContent.error()->code == "artifact.not_found",
            "Reading content for a missing artifact id did not fail with artifact.not_found");
    }
}

void testWorkspaceProviderModelProfilePersistAndReopen()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    ProviderId providerId;
    ModelId firstModelId;
    ModelId secondModelId;
    ExecutionProfileId profileId;
    RunId graphRunId;

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open workspace/catalog SQLite database");
        SqliteProjectRepository projects(opened.value());
        SqliteWorkspaceStateRepository state(opened.value());
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteExecutionProfileRepository profiles(opened.value());
        SqliteApplicationSettingsRepository settings(opened.value());
        SqliteRunRepository runs(opened.value());

        ProjectService projectService(projects);
        WorkspaceContextService workspaceContext(state, projects);
        ProviderCatalogService catalog(providers, models);
        ExecutionProfileService profileService(profiles, providers, models);
        ApplicationSettingsService settingsService(settings, profiles, providers, models);

        const auto project = projectService.createProject(
            {"Workspace/catalog project", "SQLite workspace/catalog test", workspace.projectRoot});
        require(project.success(), "Could not create workspace/catalog project");
        projectId = project.value().id;

        require(workspaceContext.selectProject(projectId).success(), "Could not select active project");
        const auto activeBefore = workspaceContext.activeProjectId();
        require(activeBefore.success() && activeBefore.value().has_value()
                && *activeBefore.value() == projectId,
            "Active project id was not immediately readable after selection");

        const auto provider = catalog.createProvider(
            {"OpenAI Test", "openai", "https://api.openai.invalid", true, std::string {"cred-ref"}});
        require(provider.success(), "Could not create provider");
        providerId = provider.value();

        const auto firstModel = catalog.createModel(
            {providerId, "gpt-first", "GPT First", 128000, 4096, 1000000, 3000000, 0, 0, true});
        require(firstModel.success(), "Could not create first model");
        firstModelId = firstModel.value();
        const auto secondModel = catalog.createModel(
            {providerId, "gpt-second", "GPT Second", 128000, 4096, 1000000, 3000000, 0, 0, true});
        require(secondModel.success(), "Could not create second model");
        secondModelId = secondModel.value();

        require(catalog.setDefaultModel(providerId, firstModelId).success(), "Could not set first default model");
        require(catalog.setDefaultModel(providerId, secondModelId).success(), "Could not switch default model");

        StageModelAssignment architect {"architect", providerId, secondModelId, 5000ms};
        StageModelAssignment implementation {"implementation", providerId, secondModelId, 6000ms};
        CreateExecutionProfileRequest profileRequest {
            "SQLite Profile", "mock-sequential-v1", true, {architect, implementation}};
        profileRequest.providerBudget.maxInputTokens = 12'345;
        profileRequest.providerBudget.maxTurns = 9;
        const auto profile = profileService.createProfile(std::move(profileRequest));
        require(profile.success(), "Could not create execution profile");
        profileId = profile.value();
        require(profileService.setDefaultProfile(profileId).success(), "Could not set default execution profile");

        const auto defaultSettings = settingsService.get();
        require(defaultSettings.success()
                && defaultSettings.value().theme == ThemePreference::Dark
                && defaultSettings.value().defaultTimeoutMs == 300'000
                && defaultSettings.value().revision == 0,
            "Settings migration did not create the expected durable defaults");
        UpdateApplicationSettingsRequest invalidSettings;
        invalidSettings.defaultMaxParallelAgents = 0;
        require(!settingsService.update(invalidSettings).success(),
            "Settings service accepted zero parallel agents");

        UpdateApplicationSettingsRequest settingsUpdate;
        settingsUpdate.theme = ThemePreference::Transparent;
        settingsUpdate.interfaceScalePercent = 125;
        settingsUpdate.compactMode = true;
        settingsUpdate.reduceAnimations = true;
        settingsUpdate.runContextPolicy = ProjectContextPolicy::AskEveryTime;
        settingsUpdate.defaultExecutionProfileId = profileId;
        settingsUpdate.defaultTimeoutMs = 300'300;
        settingsUpdate.defaultMaxParallelAgents = 7;
        settingsUpdate.confirmManyModels = false;
        settingsUpdate.chatContextPolicy = ProjectContextPolicy::NoProject;
        settingsUpdate.defaultChatProviderId = providerId;
        settingsUpdate.defaultChatModelId = secondModelId;
        settingsUpdate.chatHistoryLimit = 500;
        const auto updatedSettings = settingsService.update(settingsUpdate);
        require(updatedSettings.success() && updatedSettings.value().revision == 1,
            "Could not persist typed application settings");

        StageModelAssignment graphArchitect = architect;
        graphArchitect.agentName = "Lead architect";
        graphArchitect.role = "architect";
        graphArchitect.position = 0;
        graphArchitect.leadArchitect = true;
        StageModelAssignment graphWorker = implementation;
        graphWorker.stageType = "worker-review";
        graphWorker.agentName = "Reviewer";
        graphWorker.role = "review";
        graphWorker.position = 1;
        graphWorker.dependencies = {"architect"};
        graphWorker.maxAttempts = 3;
        graphWorker.fallbackProviderId = providerId;
        graphWorker.fallbackModelId = firstModelId;
        graphWorker.workspaceAccess = true;

        Run graphRun;
        graphRun.id = "sqlite-graph-run";
        graphRunId = graphRun.id;
        graphRun.projectId = projectId;
        graphRun.title = "SQLite graph run";
        graphRun.task = "Persist graph policy";
        graphRun.workflowId = kExecutionProfileWorkflowId;
        graphRun.status = RunStatus::Queued;
        graphRun.createdAt = std::chrono::system_clock::now();
        graphRun.assignmentSnapshot = {graphArchitect, graphWorker};
        graphRun.maxParallelism = 7;
        graphRun.planningCompletionPolicy =
            PlanningCompletionPolicy::AllowEvidenceBackedNoChanges;
        graphRun.verificationRepairPolicy = VerificationRepairPolicy::Disabled;
        graphRun.providerBudget.maxInputTokens = 12'345;
        graphRun.providerBudget.maxTurns = 9;
        RunInterfaceContract graphContract;
        graphContract.id = "G1";
        graphContract.title = "Graph contract";
        graphContract.text = "property string metricId\nsignal activated(string id)";
        graphContract.appliesToPaths = {"qml/models"};
        graphRun.interfaceContracts = {graphContract};
        graphRun.providerBudgetUsage.turnsStarted = 3;
        graphRun.providerBudgetUsage.turnsCompleted = 3;
        graphRun.providerBudgetUsage.inputTokens = 900;
        graphRun.providerBudgetUsage.outputTokens = 75;
        graphRun.providerBudgetUsage.toolCalls = 4;
        graphRun.providerBudgetUsage.toolResultBytes = 1'024;
        graphRun.usage = {900, 75};
        RunStage architectStage;
        architectStage.id = graphRun.id + ":architect";
        architectStage.runId = graphRun.id;
        architectStage.type = "architect";
        architectStage.title = "Lead architect";
        architectStage.providerId = providerId;
        architectStage.modelId = secondModelId;
        RunStage workerStage = architectStage;
        workerStage.id = graphRun.id + ":worker-review";
        workerStage.type = "worker-review";
        workerStage.title = "Reviewer";
        require(runs.insertRunWithStages(
                    graphRun, {architectStage, workerStage}).success(),
            "Could not persist run graph policy");
    }

    {
        // Reopen through brand-new connections/services to prove durability.
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen workspace/catalog SQLite database");
        SqliteProjectRepository projects(reopened.value());
        SqliteWorkspaceStateRepository state(reopened.value());
        SqliteProviderRepository providers(reopened.value());
        SqliteModelRepository models(reopened.value());
        SqliteExecutionProfileRepository profiles(reopened.value());
        SqliteApplicationSettingsRepository settings(reopened.value());
        SqliteRunRepository runs(reopened.value());

        WorkspaceContextService workspaceContext(state, projects);
        ProviderCatalogService catalog(providers, models);
        ExecutionProfileService profileService(profiles, providers, models);
        ApplicationSettingsService settingsService(settings, profiles, providers, models);

        const auto activeProject = workspaceContext.activeProject();
        require(activeProject.success() && activeProject.value().has_value()
                && activeProject.value()->id == projectId,
            "Active project did not survive reopen");

        const auto provider = catalog.findProvider(providerId);
        require(provider.success() && provider.value().has_value(), "Provider did not survive reopen");
        require(provider.value()->credentialReference.has_value()
                && *provider.value()->credentialReference == "cred-ref",
            "Provider credentialReference did not survive reopen");

        const auto secondModel = catalog.findModel(secondModelId);
        require(secondModel.success() && secondModel.value().has_value()
                && secondModel.value()->isDefault,
            "Default model flag did not survive reopen");
        const auto firstModel = catalog.findModel(firstModelId);
        require(firstModel.success() && firstModel.value().has_value()
                && !firstModel.value()->isDefault,
            "Previous default model was not cleared/persisted correctly");

        const auto profile = profileService.findProfile(profileId);
        require(profile.success() && profile.value().has_value(), "Execution profile did not survive reopen");
        require(profile.value()->isDefault, "Default execution profile flag did not survive reopen");
        require(profile.value()->assignments.size() == 2, "Execution profile assignments did not survive reopen");
        require(profile.value()->assignments[0].stageType == "architect",
            "Execution profile assignment order changed after reopen");
        require(profile.value()->assignments[1].stageType == "implementation",
            "Execution profile assignment order changed after reopen");
        require(profile.value()->providerBudget.maxInputTokens == 12'345
                && profile.value()->providerBudget.maxTurns == 9,
            "Execution profile provider budget did not survive reopen");
        require(profile.value()->assignments[0].timeout == 5000ms,
            "Execution profile assignment timeout did not survive reopen");

        const auto graphRun = runs.findRun(graphRunId);
        require(graphRun.success() && graphRun.value().has_value(),
            "Run graph policy did not survive reopen");
        require(graphRun.value()->maxParallelism == 7,
            "Run max parallelism did not survive reopen");
        require(graphRun.value()->planningCompletionPolicy
                == PlanningCompletionPolicy::AllowEvidenceBackedNoChanges,
            "Run planning completion policy did not survive reopen");
        require(graphRun.value()->verificationRepairPolicy
                == VerificationRepairPolicy::Disabled,
            "Run verification repair policy did not survive reopen");
        require(graphRun.value()->providerBudget.maxInputTokens == 12'345
                && graphRun.value()->providerBudget.maxTurns == 9
                && graphRun.value()->providerBudgetUsage.turnsStarted == 3
                && graphRun.value()->providerBudgetUsage.inputTokens == 900
                && graphRun.value()->providerBudgetUsage.toolCalls == 4,
            "Run provider budget policy/usage did not survive reopen");
        require(graphRun.value()->interfaceContracts.size() == 1
                && graphRun.value()->interfaceContracts[0].id == "G1"
                && graphRun.value()->interfaceContracts[0].text
                    == "property string metricId\nsignal activated(string id)"
                && graphRun.value()->interfaceContracts[0].appliesToPaths
                    == std::vector<std::string>({"qml/models"}),
            "Run interface contracts did not survive reopen");
        require(graphRun.value()->assignmentSnapshot.size() == 2,
            "Run graph assignment snapshot changed after reopen");
        const auto& graphWorker = graphRun.value()->assignmentSnapshot[1];
        require(graphWorker.dependencies == std::vector<std::string> {"architect"}
                && graphWorker.maxAttempts == 3
                && graphWorker.fallbackProviderId == providerId
                && graphWorker.fallbackModelId == firstModelId
                && graphWorker.workspaceAccess,
            "Run dependency/retry/fallback/workspace-access policy did not survive reopen");
        const auto& graphArchitectRow = graphRun.value()->assignmentSnapshot[0];
        require(!graphArchitectRow.workspaceAccess,
            "Run assignment workspaceAccess defaulted to true unexpectedly");

        const auto persistedSettings = settingsService.get();
        require(persistedSettings.success(), "Application settings could not be read after reopen");
        require(persistedSettings.value().theme == ThemePreference::Transparent
                && persistedSettings.value().interfaceScalePercent == 125
                && persistedSettings.value().compactMode
                && persistedSettings.value().reduceAnimations
                && persistedSettings.value().runContextPolicy == ProjectContextPolicy::AskEveryTime
                && persistedSettings.value().defaultExecutionProfileId == profileId
                && persistedSettings.value().defaultTimeoutMs == 300'300
                && persistedSettings.value().defaultMaxParallelAgents == 7
                && !persistedSettings.value().confirmManyModels
                && persistedSettings.value().chatContextPolicy == ProjectContextPolicy::NoProject
                && persistedSettings.value().defaultChatProviderId == providerId
                && persistedSettings.value().defaultChatModelId == secondModelId
                && persistedSettings.value().chatHistoryLimit == 500
                && persistedSettings.value().revision == 1,
            "Typed application settings did not survive reopen intact");
    }
}

void testV2ToV3MigrationPreservesExistingData()
{
    TemporaryPersistenceWorkspace workspace;
    fs::create_directories(workspace.databasePath.parent_path());

    constexpr const char* projectIdFixture = "v2-fixture-project";
    constexpr const char* runIdFixture = "v2-fixture-run";
    constexpr const char* stageIdFixture = "v2-fixture-run:stage-0";
    constexpr const char* artifactIdFixture = "v2-fixture-artifact";

    // Build a database frozen at schema v2 using the exact v1+v2 migration
    // SQL this file's own SqliteDatabase::open() applies, issued directly
    // through native sqlite3 (not through open(), which would already apply
    // v3). This reproduces "an existing v2 database" without depending on
    // an old snapshot of production source.
    {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(
            sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
                == SQLITE_OK,
            "Could not create v2 fixture database");

        constexpr const char* v1v2Migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE projects (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    root_path TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK(status IN ('active','archived','unavailable')),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE TABLE runs (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    parent_run_id TEXT REFERENCES runs(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title TEXT NOT NULL,
    task TEXT NOT NULL,
    workflow_id TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('queued','running','cancelling','completed','failed','cancelled','interrupted')),
    progress REAL NOT NULL CHECK(progress >= 0.0 AND progress <= 1.0),
    current_stage_id TEXT,
    error_message TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    created_at_ms INTEGER NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    revision INTEGER NOT NULL CHECK(revision >= 0)
);
CREATE INDEX runs_project_created_idx ON runs(project_id, created_at_ms, id);
CREATE INDEX runs_status_idx ON runs(status);
CREATE TABLE run_stages (
    id TEXT PRIMARY KEY NOT NULL,
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    type TEXT NOT NULL,
    title TEXT NOT NULL,
    description TEXT NOT NULL,
    position INTEGER NOT NULL CHECK(position >= 0),
    status TEXT NOT NULL CHECK(status IN ('pending','running','completed','failed','skipped','cancelled')),
    progress REAL NOT NULL CHECK(progress >= 0.0 AND progress <= 1.0),
    provider_id TEXT NOT NULL,
    model_id TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    error_message TEXT NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    UNIQUE(run_id, position)
);
CREATE INDEX run_stages_run_idx ON run_stages(run_id, position);
CREATE TABLE artifacts (
    id TEXT PRIMARY KEY NOT NULL,
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    stage_id TEXT REFERENCES run_stages(id) ON UPDATE CASCADE ON DELETE SET NULL,
    kind TEXT NOT NULL CHECK(kind IN ('context_snapshot','report','patch','source','log','other')),
    name TEXT NOT NULL,
    media_type TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX artifacts_run_created_idx ON artifacts(run_id, created_at_ms, id);
INSERT INTO projects (id,name,description,root_path,status,created_at_ms,updated_at_ms)
    VALUES ('v2-fixture-project','V2 Fixture Project','Created before the v3 migration','/tmp/v2-fixture-project','active',1000,1000);
INSERT INTO runs (id,project_id,parent_run_id,title,task,workflow_id,status,progress,current_stage_id,
    error_message,input_tokens,output_tokens,created_at_ms,started_at_ms,finished_at_ms,revision)
    VALUES ('v2-fixture-run','v2-fixture-project',NULL,'V2 Fixture Run','Preserve me across the v3 migration',
    'mock-sequential-v1','completed',1.0,NULL,'',10,5,1000,1000,2000,1);
INSERT INTO run_stages (id,run_id,type,title,description,position,status,progress,provider_id,model_id,
    input_tokens,output_tokens,error_message,started_at_ms,finished_at_ms)
    VALUES ('v2-fixture-run:stage-0','v2-fixture-run','context','Prepare context','Fixture stage',0,'completed',
    1.0,'mock','mock-v1',10,5,'',1000,2000);
INSERT INTO artifacts (id,run_id,stage_id,kind,name,media_type,relative_path,byte_size,sha256,created_at_ms)
    VALUES ('v2-fixture-artifact','v2-fixture-run','v2-fixture-run:stage-0','report','fixture-report.md',
    'text/markdown','v2-fixture-run/fixture-report.md',42,'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd',1500);
PRAGMA user_version = 2;
COMMIT;
)sql";
        char* rawMessage = nullptr;
        const int result = sqlite3_exec(handle, v1v2Migration, nullptr, nullptr, &rawMessage);
        const bool migrated = result == SQLITE_OK;
        if (!migrated && rawMessage != nullptr) {
            std::fprintf(stderr, "v2 fixture migration failed: %s\n", rawMessage);
        }
        sqlite3_free(rawMessage);
        sqlite3_close_v2(handle);
        require(migrated, "Could not build the v2 fixture database");
    }

    // Opening through the real production path must apply only the v3
    // migration (schemaVersion starts at 2, so the v1/v2 blocks are skipped)
    // and must not lose any v2 data.
    const auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "Could not open the v2 fixture database through the v3-aware open()");

    SqliteProjectRepository projects(opened.value());
    SqliteRunRepository runs(opened.value());
    SqliteArtifactRepository artifacts(opened.value());
    SqliteWorkspaceStateRepository state(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteExecutionProfileRepository profiles(opened.value());

    const auto project = projects.findById(projectIdFixture);
    require(project.success() && project.value().has_value(), "v2 project did not survive the v3 migration");
    require(project.value()->name == "V2 Fixture Project", "v2 project data changed during the v3 migration");

    const auto run = runs.findRun(runIdFixture);
    require(run.success() && run.value().has_value(), "v2 run did not survive the v3 migration");
    require(run.value()->status == RunStatus::Completed, "v2 run status changed during the v3 migration");

    const auto stages = runs.listStages(runIdFixture);
    require(stages.success() && stages.value().size() == 1 && stages.value().front().id == stageIdFixture,
        "v2 run stage did not survive the v3 migration");

    const auto artifactList = artifacts.listArtifacts(runIdFixture);
    require(artifactList.success() && artifactList.value().size() == 1
            && artifactList.value().front().id == artifactIdFixture,
        "v2 artifact did not survive the v3 migration");

    // New v3 tables must exist and be usable: workspace_state's singleton
    // row is inserted by the migration itself with a NULL active project.
    const auto activeProjectId = state.activeProjectId();
    require(activeProjectId.success() && !activeProjectId.value().has_value(),
        "workspace_state singleton row was not created with a NULL active project by the v3 migration");

    const auto providerList = providers.listAll();
    require(providerList.success() && providerList.value().empty(),
        "Fresh v3 providers table was not empty after migrating a v2 database");
    const auto profileList = profiles.listAll();
    require(profileList.success() && profileList.value().empty(),
        "Fresh v3 execution_profiles table was not empty after migrating a v2 database");
}

void testV42ToV43ProviderBudgetMigrationDefaultsAndRestart()
{
    TemporaryPersistenceWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create v43 provider-budget fixture");
        SqliteProjectRepository projects(opened.value());
        Project project;
        project.id = "provider-budget-project";
        project.name = "Provider budget project";
        project.rootPath = workspace.projectRoot;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "Could not seed provider-budget project");

        SqliteRunRepository runs(opened.value());
        Run run;
        run.id = "provider-budget-run";
        run.projectId = project.id;
        run.title = "Legacy v42 run";
        run.task = "Verify migration defaults";
        run.workflowId = kExecutionManifestWorkflowId;
        run.createdAt = std::chrono::system_clock::now();
        require(runs.insertRun(run).success(), "Could not seed provider-budget run");

        SqliteExecutionProfileRepository profiles(opened.value());
        ExecutionProfile profile;
        profile.id = "provider-budget-profile";
        profile.name = "Legacy v42 profile";
        profile.workflowId = kExecutionManifestWorkflowId;
        profile.createdAt = std::chrono::system_clock::now();
        profile.updatedAt = profile.createdAt;
        require(profiles.insert(profile).success(), "Could not seed provider-budget profile");
    }

    sqlite3* handle = nullptr;
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open provider-budget fixture through native sqlite3");
    constexpr const char* rewind =
        "ALTER TABLE runs DROP COLUMN provider_budget_usage_json;"
        "ALTER TABLE runs DROP COLUMN provider_budget_json;"
        "ALTER TABLE execution_profiles DROP COLUMN provider_budget_json;"
        "PRAGMA user_version = 42;";
    require(sqlite3_exec(handle, rewind, nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not rewind provider-budget fixture to v42");
    sqlite3_close_v2(handle);

    const auto migrated = SqliteDatabase::open(workspace.databasePath);
    require(migrated.success(), "Could not migrate provider-budget fixture v42->v43");
    SqliteRunRepository runs(migrated.value());
    const auto run = runs.findRun("provider-budget-run");
    require(run.success() && run.value().has_value()
            && run.value()->providerBudget.maxTurns == 24
            && run.value()->providerBudgetUsage.turnsStarted == 0,
        "v42 Run did not receive safe provider-budget defaults");
    SqliteExecutionProfileRepository profiles(migrated.value());
    const auto profile = profiles.findById("provider-budget-profile");
    require(profile.success() && profile.value().has_value()
            && !profile.value()->providerBudget.maxTurns.has_value(),
        "v42 Execution Profile did not receive empty lower-limit overrides");

    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    require(reopened.success(), "Migrated v43 provider-budget database did not reopen");
}

void testV43ToV44CacheTokenMigrationDefaultsAndRestart()
{
    TemporaryPersistenceWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create v44 cache-token fixture");
        SqliteProjectRepository projects(opened.value());
        Project project;
        project.id = "cache-token-project";
        project.name = "Cache token project";
        project.rootPath = workspace.projectRoot;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "Could not seed cache-token project");

        SqliteRunRepository runs(opened.value());
        Run run;
        run.id = "cache-token-run";
        run.projectId = project.id;
        run.title = "Legacy v43 run";
        run.task = "Verify cache-token migration defaults";
        run.workflowId = kExecutionManifestWorkflowId;
        run.createdAt = std::chrono::system_clock::now();
        require(runs.insertRun(run).success(), "Could not seed cache-token run");
    }

    sqlite3* handle = nullptr;
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open cache-token fixture through native sqlite3");
    constexpr const char* rewind =
        "DROP TRIGGER runs_cache_duration_validate_insert;"
        "DROP TRIGGER runs_cache_duration_validate_update;"
        "ALTER TABLE runs DROP COLUMN cache_read_input_tokens;"
        "ALTER TABLE runs DROP COLUMN cache_creation_input_tokens;"
        "PRAGMA user_version = 43;";
    require(sqlite3_exec(handle, rewind, nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not rewind cache-token fixture to v43");
    sqlite3_close_v2(handle);

    const auto migrated = SqliteDatabase::open(workspace.databasePath);
    require(migrated.success(), "Could not migrate cache-token fixture v43->v44");
    SqliteRunRepository runs(migrated.value());
    const auto run = runs.findRun("cache-token-run");
    require(run.success() && run.value().has_value()
            && run.value()->usage.cacheCreationInputTokens == 0
            && run.value()->usage.cacheReadInputTokens == 0,
        "v43 Run did not receive truthful zero cache-token defaults");

    // A real, non-zero value must round-trip through the newly added columns,
    // not just the migration's zero default.
    Run withCacheUsage = *run.value();
    withCacheUsage.usage.inputTokens = 7'000;
    withCacheUsage.usage.cacheCreationInputTokens = 321;
    withCacheUsage.usage.cacheReadInputTokens = 6'500;
    const auto updated = runs.updateRun(withCacheUsage);
    require(updated.success()
            && updated.value().usage.inputTokens == 7'000
            && updated.value().usage.cacheCreationInputTokens == 321
            && updated.value().usage.cacheReadInputTokens == 6'500,
        "Migrated v44 database did not persist real cache-token usage");

    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    require(reopened.success(), "Migrated v44 cache-token database did not reopen");
}

// The automatic turn-limit mode (policy.autoExtendTurns) and the raised limit
// (usage.grantedMaxTurns) were added to existing JSON documents. New documents
// round-trip both. A policy frozen WITHOUT the flag cannot say whether its limit was typed or
// defaulted: the Backend default (24) reads as automatic, any other value stays pinned; an empty
// '{}' policy is the Backend defaults, automatic. A usage record without the raise reads as never
// raised.
void testTurnLimitExtensionFieldsRoundTripAndLegacyShapesAreReadByTheirLimit()
{
    TemporaryPersistenceWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create turn-limit fixture");
        SqliteProjectRepository projects(opened.value());
        Project project;
        project.id = "turn-limit-project";
        project.name = "Turn limit project";
        project.rootPath = workspace.projectRoot;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "Could not seed turn-limit project");

        SqliteRunRepository runs(opened.value());
        const auto seed = [&](const std::string& id, bool automatic, std::uint64_t granted) {
            Run run;
            run.id = id;
            run.projectId = project.id;
            run.title = id;
            run.task = "Verify the turn-limit fields";
            run.workflowId = kExecutionManifestWorkflowId;
            run.createdAt = std::chrono::system_clock::now();
            run.providerBudget.autoExtendTurns = automatic;
            run.providerBudgetUsage.grantedMaxTurns = granted;
            require(runs.insertRun(run).success(), "Could not seed a turn-limit run");
        };
        seed("automatic-run", /*automatic=*/true, /*granted=*/0);
        seed("pinned-run", /*automatic=*/false, /*granted=*/0);
        seed("legacy-run", /*automatic=*/true, /*granted=*/0);
        seed("legacy-custom-run", /*automatic=*/true, /*granted=*/0);
        seed("empty-policy-run", /*automatic=*/false, /*granted=*/0);

        auto automaticRun = runs.findRun("automatic-run");
        require(automaticRun.success() && automaticRun.value().has_value()
                && automaticRun.value()->providerBudget.autoExtendTurns
                && automaticRun.value()->providerBudgetUsage.grantedMaxTurns == 0,
            "A new automatic policy must round-trip with no raise recorded");
        Run raised = *automaticRun.value();
        raised.providerBudgetUsage.grantedMaxTurns = 25;
        raised.providerBudgetUsage.inputTokens = 60;
        raised.providerBudgetUsage.cacheCreationInputTokens = 60;
        raised.providerBudgetUsage.cacheCreationDuration = {true, 20, 40};
        require(runs.updateRun(raised).success(), "Could not record a raised turn limit");
    }

    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Turn-limit fixture did not reopen");
        SqliteRunRepository runs(reopened.value());
        const auto automaticRun = runs.findRun("automatic-run");
        require(automaticRun.success() && automaticRun.value().has_value()
                && automaticRun.value()->providerBudget.autoExtendTurns
                && automaticRun.value()->providerBudget.maxTurns == 24
                && automaticRun.value()->providerBudgetUsage.grantedMaxTurns == 25
                && automaticRun.value()->providerBudgetUsage.cacheCreationDuration.known
                && automaticRun.value()->providerBudgetUsage.cacheCreationDuration
                        .fiveMinuteTokens == 20
                && automaticRun.value()->providerBudgetUsage.cacheCreationDuration
                        .oneHourTokens == 40,
            "The raised turn limit and cache-duration usage must survive reopen next to the "
            "frozen policy");
        const auto pinnedRun = runs.findRun("pinned-run");
        require(pinnedRun.success() && pinnedRun.value().has_value()
                && !pinnedRun.value()->providerBudget.autoExtendTurns,
            "A pinned policy must stay pinned after reopen");
    }

    // Rewrite one Run to the shapes that existed before these fields: an
    // eight-field policy and a usage record without grantedMaxTurns.
    sqlite3* handle = nullptr;
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open the turn-limit fixture through native sqlite3");
    constexpr const char* legacyShape =
        "UPDATE runs SET provider_budget_json = "
        "'{\"schemaVersion\":\"provider-execution-budget.v1\",\"maxInputTokens\":500000,"
        "\"maxOutputTokens\":64000,\"maxTurns\":24,\"maxToolCalls\":128,"
        "\"maxToolResultBytes\":8388608,\"maxWallTimeMs\":900000,\"maxRequestTimeMs\":240000}', "
        "provider_budget_usage_json = "
        "'{\"turnsStarted\":3,\"turnsCompleted\":3,\"inputTokens\":10,\"outputTokens\":5,"
        "\"toolCalls\":0,\"toolResultBytes\":0,\"usageAuthoritative\":true,\"stopCode\":\"\"}' "
        "WHERE id = 'legacy-run';"
        "UPDATE runs SET provider_budget_json = "
        "'{\"schemaVersion\":\"provider-execution-budget.v1\",\"maxInputTokens\":500000,"
        "\"maxOutputTokens\":64000,\"maxTurns\":40,\"maxToolCalls\":128,"
        "\"maxToolResultBytes\":8388608,\"maxWallTimeMs\":900000,\"maxRequestTimeMs\":240000}' "
        "WHERE id = 'legacy-custom-run';"
        "UPDATE runs SET provider_budget_json = '{}' WHERE id = 'empty-policy-run';";
    require(sqlite3_exec(handle, legacyShape, nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not rewrite the Run to the legacy budget shapes");
    sqlite3_close_v2(handle);

    const auto migrated = SqliteDatabase::open(workspace.databasePath);
    require(migrated.success(), "Legacy turn-limit fixture did not reopen");
    SqliteRunRepository runs(migrated.value());
    const auto legacy = runs.findRun("legacy-run");
    require(legacy.success() && legacy.value().has_value()
            && legacy.value()->providerBudget.autoExtendTurns
            && legacy.value()->providerBudget.maxTurns == 24
            && legacy.value()->providerBudgetUsage.turnsStarted == 3
            && legacy.value()->providerBudgetUsage.grantedMaxTurns == 0
            && !legacy.value()->providerBudgetUsage.cacheCreationDuration.known,
        "A policy frozen without the flag at exactly the Backend default turn limit must read "
        "as automatic, and a usage record without the raise as never raised");
    const auto legacyCustom = runs.findRun("legacy-custom-run");
    require(legacyCustom.success() && legacyCustom.value().has_value()
            && !legacyCustom.value()->providerBudget.autoExtendTurns
            && legacyCustom.value()->providerBudget.maxTurns == 40,
        "A policy frozen without the flag at any other turn limit was chosen by somebody and "
        "must stay pinned");
    const auto emptyPolicy = runs.findRun("empty-policy-run");
    require(emptyPolicy.success() && emptyPolicy.value().has_value()
            && emptyPolicy.value()->providerBudget.autoExtendTurns
            && emptyPolicy.value()->providerBudget.maxTurns == 24,
        "An empty '{}' policy is the Backend defaults and must read as automatic");

    // The three duration fields are one atomic optional extension. A partially
    // written shape must not be interpreted by defaulting the missing member,
    // even when those defaults would accidentally make the arithmetic valid.
    sqlite3* corruptHandle = nullptr;
    require(sqlite3_open_v2(
                utf8Path.c_str(), &corruptHandle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open the duration-corruption fixture through native sqlite3");
    constexpr const char* corruptDurationShapes =
        "UPDATE runs SET provider_budget_usage_json = "
        "'{\"turnsStarted\":1,\"turnsCompleted\":1,\"inputTokens\":2,\"outputTokens\":0,"
        "\"toolCalls\":0,\"toolResultBytes\":0,\"usageAuthoritative\":true,"
        "\"stopCode\":\"\",\"cacheCreationInputTokens\":2,"
        "\"cacheCreationDurationPartitionKnown\":true,"
        "\"cacheCreationFiveMinuteInputTokens\":2}' WHERE id='automatic-run';"
        "UPDATE runs SET provider_budget_usage_json = "
        "'{\"turnsStarted\":1,\"turnsCompleted\":1,\"inputTokens\":1,\"outputTokens\":0,"
        "\"toolCalls\":0,\"toolResultBytes\":0,\"usageAuthoritative\":true,"
        "\"stopCode\":\"\",\"cacheCreationInputTokens\":2,"
        "\"cacheCreationDurationPartitionKnown\":true,"
        "\"cacheCreationFiveMinuteInputTokens\":2,"
        "\"cacheCreationOneHourInputTokens\":0}' WHERE id='pinned-run';";
    require(sqlite3_exec(
                corruptHandle, corruptDurationShapes, nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not write corrupt provider-budget duration shapes");
    sqlite3_close_v2(corruptHandle);
    const auto partialDuration = runs.findRun("automatic-run");
    require(!partialDuration.success()
            && partialDuration.error()->code == "persistence.invalid_data",
        "A partial provider-budget duration extension was accepted");
    const auto cacheCreationPastInput = runs.findRun("pinned-run");
    require(!cacheCreationPastInput.success()
            && cacheCreationPastInput.error()->code == "persistence.invalid_data",
        "Provider-budget cache creation larger than budget-countable input was accepted");
}

// v60 adds run_drafts.provider_budget_json (the explicit per-Run provider limits a draft carries).
// Every existing draft gets '{}' = no explicit limit; a real limit round-trips through the new
// column; and the migration is idempotent - a database whose version was rewound but whose column
// is already there reopens instead of failing on a duplicate column.
void testV59ToV60DraftProviderBudgetMigrationDefaultsAndRestart()
{
    TemporaryPersistenceWorkspace workspace;
    const auto openRaw = [&](const char* sql, const char* what) {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            what);
        require(sqlite3_exec(handle, sql, nullptr, nullptr, nullptr) == SQLITE_OK, what);
        sqlite3_close_v2(handle);
    };
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the v60 draft fixture");
        SqliteProjectRepository projects(opened.value());
        Project project;
        project.id = "draft-budget-project";
        project.name = "Draft budget project";
        project.rootPath = workspace.projectRoot;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "Could not seed the draft-budget project");

        SqliteRunDraftRepository drafts(opened.value());
        RunDraft draft;
        draft.id = "draft-budget-1";
        draft.name = "Old draft";
        draft.projectId = project.id;
        draft.title = "T";
        draft.task = "Task";
        draft.workflowId = kExecutionManifestWorkflowId;
        draft.launchMode = RunLaunchMode::CustomRoster;
        draft.createdAt = std::chrono::system_clock::now();
        draft.updatedAt = draft.createdAt;
        draft.providerBudget.maxTurns = 33;   // dropped with the column below, like a v59 draft
        require(drafts.insert(draft).success(), "Could not seed the v59 draft");
    }
    openRaw("ALTER TABLE run_drafts DROP COLUMN provider_budget_json; PRAGMA user_version = 59;",
        "Could not rewind the draft fixture to v59");

    {
        const auto migrated = SqliteDatabase::open(workspace.databasePath);
        require(migrated.success(), "Could not migrate the draft fixture v59->v60");
        SqliteRunDraftRepository drafts(migrated.value());
        const auto found = drafts.findById("draft-budget-1");
        require(found.success() && found.value().has_value()
                && !hasExplicitProviderBudgetLimits(found.value()->providerBudget),
            "A v59 draft must read as no explicit provider limit");
        RunDraft edited = *found.value();
        edited.providerBudget.maxTurns = 48;
        edited.providerBudget.maxOutputTokens = 32'000;
        require(drafts.update(edited).success(), "Could not store limits in the migrated draft");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "The migrated v60 database did not reopen");
        SqliteRunDraftRepository drafts(reopened.value());
        const auto found = drafts.findById("draft-budget-1");
        require(found.success() && found.value().has_value()
                && found.value()->providerBudget.maxTurns == 48
                && found.value()->providerBudget.maxOutputTokens == 32'000
                && !found.value()->providerBudget.maxToolCalls.has_value(),
            "Draft provider limits must survive a restart of the migrated database");
    }

    // A rewound version with the column still present: the migration inspects first.
    openRaw("PRAGMA user_version = 59;", "Could not rewind the version of the migrated fixture");
    const auto again = SqliteDatabase::open(workspace.databasePath);
    require(again.success(), "Re-running the v60 migration over an existing column must succeed");
    SqliteRunDraftRepository drafts(again.value());
    const auto kept = drafts.findById("draft-budget-1");
    require(kept.success() && kept.value().has_value()
            && kept.value()->providerBudget.maxTurns == 48,
        "A re-run migration must keep the stored draft limits");
}

// v61 adds runs.run_interface_contracts_json and run_drafts.run_interface_contracts_json (PC-2: the Run
// author's authoritative interface contracts). Every existing Run and draft gets '[]' = no contracts; the
// contracts round-trip byte for byte; a stale caller copy can never rewrite a Run's frozen contracts; a
// damaged or non-canonical column is reported instead of repaired; and the migration is idempotent.
void testV60ToV61InterfaceContractsMigrationRoundTripGuardAndDamage()
{
    TemporaryPersistenceWorkspace workspace;
    const auto openRaw = [&](const std::string& sql, const char* what) {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, what);
        require(sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, what);
        sqlite3_close_v2(handle);
    };
    const auto contract = [](std::string id, std::string text, std::vector<std::string> scopes = {}) {
        RunInterfaceContract value;
        value.id = std::move(id);
        value.title = "Contract " + value.id;
        value.text = std::move(text);
        value.appliesToPaths = std::move(scopes);
        return value;
    };
    const std::vector<RunInterfaceContract> original = {
        contract("C1", "line 1\nline 2 \"quoted\" \xC3\xA9", {"qml/models", "qml/pages/History"}),
        contract("K1", "No file may exceed 300 lines.")};
    const auto makeRun = [&](const std::string& id) {
        Run run;
        run.id = id;
        run.projectId = "contracts-project";
        run.title = "Contracts run";
        run.task = "Task";
        run.workflowId = kExecutionManifestWorkflowId;
        run.status = RunStatus::Queued;
        run.createdAt = std::chrono::system_clock::now();
        return run;
    };
    const auto makeDraft = [&](const std::string& id) {
        RunDraft draft;
        draft.id = id;
        draft.name = "Contracts draft";
        draft.projectId = "contracts-project";
        draft.title = "T";
        draft.task = "Task";
        draft.workflowId = kExecutionManifestWorkflowId;
        draft.launchMode = RunLaunchMode::CustomRoster;
        draft.createdAt = std::chrono::system_clock::now();
        draft.updatedAt = draft.createdAt;
        return draft;
    };
    const auto sameContracts = [](const std::vector<RunInterfaceContract>& left,
                                   const std::vector<RunInterfaceContract>& right) {
        return runInterfaceContractsFingerprint(left) == runInterfaceContractsFingerprint(right);
    };

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the v61 contracts fixture");
        SqliteProjectRepository projects(opened.value());
        Project project;
        project.id = "contracts-project";
        project.name = "Contracts project";
        project.rootPath = workspace.projectRoot;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        require(projects.insert(project).success(), "Could not seed the contracts project");

        SqliteRunRepository runs(opened.value());
        Run withContracts = makeRun("contracts-run");
        withContracts.interfaceContracts = original;
        require(runs.insertRunWithStages(withContracts, {}).success(), "Could not insert a Run with contracts");
        require(runs.insertRunWithStages(makeRun("plain-run"), {}).success(), "Could not insert a plain Run");

        const auto found = runs.findRun("contracts-run");
        require(found.success() && found.value().has_value()
                && sameContracts(found.value()->interfaceContracts, original),
            "The contracts of a Run must round-trip through SQLite");
        const auto plain = runs.findRun("plain-run");
        require(plain.success() && plain.value().has_value() && plain.value()->interfaceContracts.empty(),
            "A Run without contracts reads back empty");

        // A stale or hostile caller copy must never rewrite the contracts a Run froze.
        Run mutated = *found.value();
        mutated.status = RunStatus::Running;
        mutated.interfaceContracts = {contract("X9", "rewritten")};
        const auto updated = runs.updateRun(mutated);
        require(updated.success() && sameContracts(updated.value().interfaceContracts, original),
            "updateRun must hand back the frozen contracts, not the caller's");
        const auto reread = runs.findRun("contracts-run");
        require(reread.success() && reread.value().has_value()
                && sameContracts(reread.value()->interfaceContracts, original)
                && reread.value()->status == RunStatus::Running,
            "updateRun must not touch the stored contracts while it advances the lifecycle");

        SqliteRunDraftRepository drafts(opened.value());
        RunDraft draft = makeDraft("contracts-draft");
        draft.interfaceContracts = original;
        require(drafts.insert(draft).success(), "Could not insert a draft with contracts");
        const auto foundDraft = drafts.findById("contracts-draft");
        require(foundDraft.success() && foundDraft.value().has_value()
                && sameContracts(foundDraft.value()->interfaceContracts, original),
            "The contracts of a draft must round-trip through SQLite");
        RunDraft edited = *foundDraft.value();
        edited.interfaceContracts = {contract("E1", "edited")};
        require(drafts.update(edited).success(), "Could not update the draft contracts");
        const auto afterEdit = drafts.findById("contracts-draft");
        require(afterEdit.success() && afterEdit.value().has_value()
                && afterEdit.value()->interfaceContracts.size() == 1
                && afterEdit.value()->interfaceContracts[0].id == "E1",
            "A draft update replaces its contracts");
        edited.interfaceContracts.clear();
        require(drafts.update(edited).success(), "Could not clear the draft contracts");
        const auto cleared = drafts.findById("contracts-draft");
        require(cleared.success() && cleared.value().has_value() && cleared.value()->interfaceContracts.empty(),
            "Clearing the contracts of a draft sticks");
        edited.interfaceContracts = original;
        require(drafts.update(edited).success(), "Could not restore the draft contracts");
    }

    // Rewind to v60 (both columns gone): every existing Run and draft must read as "no contracts".
    openRaw("ALTER TABLE runs DROP COLUMN run_interface_contracts_json;"
            "ALTER TABLE run_drafts DROP COLUMN run_interface_contracts_json;"
            "PRAGMA user_version = 60;",
        "Could not rewind the contracts fixture to v60");
    {
        const auto migrated = SqliteDatabase::open(workspace.databasePath);
        require(migrated.success(), "Could not migrate the contracts fixture v60->v61");
        SqliteRunRepository runs(migrated.value());
        SqliteRunDraftRepository drafts(migrated.value());
        const auto oldRun = runs.findRun("contracts-run");
        require(oldRun.success() && oldRun.value().has_value() && oldRun.value()->interfaceContracts.empty(),
            "A v60 Run must read as having no contracts");
        const auto oldDraft = drafts.findById("contracts-draft");
        require(oldDraft.success() && oldDraft.value().has_value() && oldDraft.value()->interfaceContracts.empty(),
            "A v60 draft must read as having no contracts");
        Run fresh = makeRun("migrated-run");
        fresh.interfaceContracts = original;
        require(runs.insertRunWithStages(fresh, {}).success(), "Could not insert into the migrated schema");
        RunDraft edited = *oldDraft.value();
        edited.interfaceContracts = original;
        require(drafts.update(edited).success(), "Could not store contracts in the migrated draft");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "The migrated v61 database did not reopen");
        SqliteRunRepository runs(reopened.value());
        SqliteRunDraftRepository drafts(reopened.value());
        const auto fresh = runs.findRun("migrated-run");
        require(fresh.success() && fresh.value().has_value()
                && sameContracts(fresh.value()->interfaceContracts, original),
            "Contracts must survive a restart of the migrated database");
        const auto draft = drafts.findById("contracts-draft");
        require(draft.success() && draft.value().has_value()
                && sameContracts(draft.value()->interfaceContracts, original),
            "Draft contracts must survive a restart of the migrated database");
    }

    // A rewound version with the columns still present: the migration inspects first.
    openRaw("PRAGMA user_version = 60;", "Could not rewind the version of the migrated fixture");
    {
        const auto again = SqliteDatabase::open(workspace.databasePath);
        require(again.success(), "Re-running the v61 migration over existing columns must succeed");
        SqliteRunRepository runs(again.value());
        const auto kept = runs.findRun("migrated-run");
        require(kept.success() && kept.value().has_value()
                && sameContracts(kept.value()->interfaceContracts, original),
            "A re-run migration must keep the stored contracts");
    }

    // A damaged or non-canonical column is reported, never repaired and never read as "no contracts".
    for (const char* damaged : {"not json", "{}", "[1]",
             R"([{"id":"C1","kind":"interface_contract","title":" padded ","text":"x","appliesToPaths":[]}])"}) {
        openRaw(std::string("UPDATE runs SET run_interface_contracts_json = '") + damaged
                + "' WHERE id = 'migrated-run';"
                  "UPDATE run_drafts SET run_interface_contracts_json = '" + damaged
                + "' WHERE id = 'contracts-draft';",
            "Could not damage the contracts columns");
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "A damaged contracts column must not stop the database from opening");
        SqliteRunRepository runs(opened.value());
        SqliteRunDraftRepository drafts(opened.value());
        const auto run = runs.findRun("migrated-run");
        require(!run.success() && run.error()->code == "persistence.invalid_data",
            "A damaged Run contracts column must be reported as invalid data");
        const auto draft = drafts.findById("contracts-draft");
        require(!draft.success() && draft.error()->code == "persistence.invalid_data",
            "A damaged draft contracts column must be reported as invalid data");
    }
}

// Schema v23 adds run_stage_assignments.workspace_access via
// "ALTER TABLE ... ADD COLUMN workspace_access INTEGER NOT NULL DEFAULT 0" -
// SQLite backfills that exact default into every pre-existing row the same
// way it fills a fresh INSERT that omits the column. A raw INSERT with an
// explicit column list omitting workspace_access is therefore a faithful,
// practical proxy for "a row written before this migration existed" without
// having to hand-reconstruct the full v1-v22 schema history as a fixture.
void testSchemaV23WorkspaceAccessDefaultsToFalseForRowsWithoutIt()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    constexpr const char* runIdFixture = "v23-fixture-run";

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open v23 fixture database");
        SqliteProjectRepository projects(opened.value());
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteRunRepository runs(opened.value());
        ProjectService projectService(projects);
        ProviderCatalogService catalog(providers, models);

        const auto project = projectService.createProject(
            {"v23 fixture project", "Schema v23 migration test", workspace.projectRoot});
        require(project.success(), "Could not create v23 fixture project");
        projectId = project.value().id;

        const auto provider = catalog.createProvider(
            {"Test Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
        require(provider.success(), "Could not create v23 fixture provider");
        providerId = provider.value();
        const auto model = catalog.createModel(
            {providerId, "gpt-fixture", "Fixture Model", 128000, 4096, 1000000, 3000000, 0, 0, true});
        require(model.success(), "Could not create v23 fixture model");
        modelId = model.value();

        StageModelAssignment architect {"architect", providerId, modelId, 5000ms};
        architect.leadArchitect = true;
        Run run;
        run.id = runIdFixture;
        run.projectId = projectId;
        run.title = "v23 fixture run";
        run.task = "Prove workspace_access defaults to false without the column";
        run.workflowId = kExecutionProfileWorkflowId;
        run.status = RunStatus::Queued;
        run.createdAt = std::chrono::system_clock::now();
        run.assignmentSnapshot = {architect};
        RunStage architectStage;
        architectStage.id = run.id + ":architect";
        architectStage.runId = run.id;
        architectStage.type = "architect";
        architectStage.title = "Lead architect";
        architectStage.providerId = providerId;
        architectStage.modelId = modelId;
        require(runs.insertRunWithStages(run, {architectStage}).success(),
            "Could not persist v23 fixture run");
    }

    {
        // Simulate a row written before this migration existed, via a
        // separate raw connection (the SqliteDatabase wrapper does not
        // expose its handle to callers): the column list below deliberately
        // does not mention workspace_access, so SQLite applies the column's
        // own DEFAULT 0 - the same mechanism that backfills genuinely
        // pre-existing rows when ALTER TABLE ... ADD COLUMN runs.
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open raw connection for legacy row insert");
        const char* rawInsert =
            "INSERT INTO run_stage_assignments "
            "(run_id, stage_type, provider_id, model_id, timeout_ms, position, lead_architect) "
            "VALUES (?, 'legacy-agent', ?, ?, 5000, 1, 0);";
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, rawInsert, -1, &statement, nullptr) == SQLITE_OK,
            "Could not prepare raw legacy run_stage_assignments insert");
        sqlite3_bind_text(statement, 1, runIdFixture, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, providerId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, modelId.c_str(), -1, SQLITE_TRANSIENT);
        require(sqlite3_step(statement) == SQLITE_DONE,
            "Could not insert raw legacy run_stage_assignments row");
        sqlite3_finalize(statement);

        // The fixture database was initially created by the current schema.
        // Rewind only its version marker so the next open executes the v24
        // backfill against the intentionally NULL Run.workspace_path row.
        require(sqlite3_exec(handle, "PRAGMA user_version = 23;",
                    nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not rewind fixture schema version to v23");
        sqlite3_close(handle);
    }

    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen v23 fixture database");
        SqliteRunRepository runs(reopened.value());
        const auto run = runs.findRun(runIdFixture);
        require(run.success() && run.value().has_value(),
            "v23 fixture run did not survive reopen");
        require(run.value()->assignmentSnapshot.size() == 2,
            "v23 fixture run lost the raw-inserted legacy assignment row");
        const auto canonicalProjectRoot = fs::weakly_canonical(workspace.projectRoot);
        require(run.value()->workspacePath.has_value()
                && *run.value()->workspacePath == canonicalProjectRoot,
            "Schema v24 did not snapshot the Project root for a legacy NULL workspace_path");
        const auto legacy = std::find_if(
            run.value()->assignmentSnapshot.begin(), run.value()->assignmentSnapshot.end(),
            [](const StageModelAssignment& a) { return a.stageType == "legacy-agent"; });
        require(legacy != run.value()->assignmentSnapshot.end(),
            "Raw-inserted legacy assignment row was not found after reopen");
        require(!legacy->workspaceAccess,
            "Pre-migration run_stage_assignments row did not default workspace_access to false");
    }
}

void testTaskRepositoryPersistsAndReopens()
{
    TemporaryPersistenceWorkspace workspace;
    auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "Task test database did not open");
    SqliteProjectRepository projects(opened.value());
    SqliteTaskRepository tasks(opened.value());
    ProjectService projectService(projects);
    TaskService taskService(tasks, projects);
    const auto project = projectService.createProject(
        {"Task project", "Shared context", workspace.projectRoot});
    require(project.success(), "Task project was not created");
    CreateTaskRequest request;
    request.projectId = project.value().id;
    request.title = "Persist task";
    request.description = "Task-specific context";
    request.status = TaskStatus::Blocked;
    request.priority = TaskPriority::High;
    request.requiresAttention = true;
    request.dueAt = std::chrono::system_clock::now() + 24h;
    const auto task = taskService.createTask(request);
    require(task.success(), "Task was not created");

    SqliteRunRepository runs(opened.value());
    Run sourceRun;
    sourceRun.id = "task-link-source-run";
    sourceRun.projectId = project.value().id;
    sourceRun.title = "Source run";
    sourceRun.task = "Exercise atomic task-link copy";
    sourceRun.workflowId = kMockWorkflowId;
    sourceRun.status = RunStatus::Completed;
    sourceRun.createdAt = std::chrono::system_clock::now();
    sourceRun.finishedAt = sourceRun.createdAt;
    sourceRun.workspacePath = project.value().rootPath;
    require(runs.insertRunWithStages(sourceRun, {}).success(),
        "Source run for task-link copy was not created");
    require(tasks.linkRun(task.value().id, sourceRun.id).success(),
        "Task was not linked to source run");

    Run retryRun = sourceRun;
    retryRun.id = "task-link-retry-run";
    retryRun.parentRunId = sourceRun.id;
    const auto insertedRetry = runs.insertRunWithStagesAndCopiedTaskLinks(
        retryRun, {}, sourceRun.id);
    if (!insertedRetry.success()) {
        throw std::runtime_error(
            insertedRetry.error()->code + ": " + insertedRetry.error()->message);
    }
    const auto linkedAfterRetry = tasks.linkedRuns(task.value().id);
    require(linkedAfterRetry.success() && linkedAfterRetry.value().size() == 2,
        "Rerun did not atomically inherit the source task link");

    const auto otherRoot = workspace.root / "other-project";
    std::filesystem::create_directories(otherRoot);
    const auto otherProject = projectService.createProject(
        {"Other project", "Must not inherit a foreign task", otherRoot});
    require(otherProject.success(), "Other task-link project was not created");
    Run crossProjectRun = sourceRun;
    crossProjectRun.id = "task-link-cross-project-run";
    crossProjectRun.projectId = otherProject.value().id;
    crossProjectRun.parentRunId = sourceRun.id;
    crossProjectRun.workspacePath = otherProject.value().rootPath;
    require(runs.insertRunWithStagesAndCopiedTaskLinks(
                crossProjectRun, {}, sourceRun.id).success(),
        "Cross-project edited rerun was not created");
    const auto linkedAfterCrossProject = tasks.linkedRuns(task.value().id);
    require(linkedAfterCrossProject.success() && linkedAfterCrossProject.value().size() == 2,
        "Cross-project rerun incorrectly inherited a foreign task link");

    // Phase 5: a brand-new Run's task link commits atomically with the Run
    // itself (insertRunWithStagesAndTaskLink), the same guarantee reruns
    // already had via insertRunWithStagesAndCopiedTaskLinks above.
    Run newRunWithTaskLink = sourceRun;
    newRunWithTaskLink.id = "task-link-new-run";
    newRunWithTaskLink.parentRunId.reset();
    require(runs.insertRunWithStagesAndTaskLink(
                newRunWithTaskLink, {}, task.value().id).success(),
        "New run with an atomic task link was not created");
    const auto linkedAfterNewRun = tasks.linkedRuns(task.value().id);
    require(linkedAfterNewRun.success() && linkedAfterNewRun.value().size() == 3,
        "New run did not atomically link to its requested task");

    // An unknown taskId must fail closed on the foreign-key constraint and
    // leave no partial Run row - never a Run silently missing its link.
    Run rejectedRun = sourceRun;
    rejectedRun.id = "task-link-rejected-run";
    rejectedRun.parentRunId.reset();
    const auto rejected = runs.insertRunWithStagesAndTaskLink(
        rejectedRun, {}, "task-does-not-exist");
    require(!rejected.success(), "Linking a new run to an unknown task must fail");
    const auto rejectedRunRow = runs.findRun(rejectedRun.id);
    require(rejectedRunRow.success() && !rejectedRunRow.value().has_value(),
        "A rejected task-link insert must not leave a partial Run row");

    auto reopened = SqliteDatabase::open(workspace.databasePath);
    require(reopened.success(), "Task database did not reopen");
    SqliteTaskRepository reopenedTasks(reopened.value());
    const auto listed = reopenedTasks.list(project.value().id);
    require(listed.success() && listed.value().size() == 1, "Task did not persist");
    require(listed.value().front().requiresAttention, "Task attention flag did not persist");
    require(listed.value().front().status == TaskStatus::Blocked, "Task status did not persist");
}

void testChatProjectContextAttachmentIsAtomicImmutableAndRoundTrips()
{
    TemporaryPersistenceWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "Chat context database did not open");
    SqliteProjectRepository projects(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteModelRepository models(opened.value());
    SqliteChatRepository chats(opened.value());
    ProviderCatalogService catalog(providers, models);
    const auto now = std::chrono::system_clock::now();

    Project project;
    project.id = "context-project";
    project.name = "Context project";
    project.rootPath = workspace.projectRoot;
    project.status = ProjectStatus::Active;
    project.createdAt = project.updatedAt = now;
    require(projects.insert(project).success(), "Context project was not inserted");
    const auto provider = catalog.createProvider(
        {"Context Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
    require(provider.success(), "Context provider was not created");
    const auto model = catalog.createModel(
        {provider.value(), "context-model", "Context Model", 128000, 4096, 1000000, 3000000, 0, 0, true});
    require(model.success(), "Context model was not created");

    ChatConversation conversation;
    conversation.id = "context-conversation";
    conversation.projectId = project.id;
    conversation.providerId = provider.value();
    conversation.modelId = model.value();
    conversation.title = "Context chat";
    conversation.createdAt = conversation.updatedAt = now;
    require(chats.insertConversation(conversation).success(), "Context conversation was not inserted");

    const std::string workspaceFingerprint(64, 'a');
    ProjectContextAssemblyInput input;
    input.projectId = project.id;
    input.workspaceFingerprint = workspaceFingerprint;
    ProjectContextItem projectItem;
    projectItem.text = "Project: Context project";
    projectItem.source.kind = "project";
    projectItem.source.id = project.id;
    projectItem.mandatory = true;
    ProjectContextItem workspaceItem;
    workspaceItem.text = "Workspace baseline";
    workspaceItem.source.kind = "workspace";
    workspaceItem.source.id = "current";
    workspaceItem.source.digest = workspaceFingerprint;
    workspaceItem.mandatory = true;
    input.sections = {{ProjectContextCategory::ProjectMetadata, {projectItem}},
        {ProjectContextCategory::WorkspaceCatalog, {workspaceItem}}};
    const auto snapshot = assembleProjectContextSnapshot(input);
    require(snapshot.success(), "Context snapshot did not assemble");

    PreparedChatTurn turn;
    turn.userMessage.id = "context-user";
    turn.userMessage.conversationId = conversation.id;
    turn.userMessage.role = ChatRole::User;
    turn.userMessage.status = ChatMessageStatus::Sent;
    turn.userMessage.content = "Use this context";
    turn.userMessage.createdAt = now;
    turn.assistantMessage.id = "context-assistant";
    turn.assistantMessage.conversationId = conversation.id;
    turn.assistantMessage.role = ChatRole::Assistant;
    turn.assistantMessage.status = ChatMessageStatus::Generating;
    turn.assistantMessage.providerId = provider.value();
    turn.assistantMessage.modelId = model.value();
    turn.assistantMessage.createdAt = now + 1ms;
    const auto attachment = makeChatProjectContextAttachment(
        turn.assistantMessage.id, conversation.id, snapshot.value());
    require(attachment.success(), "Context attachment did not seal");
    turn.assistantMessage.projectContext = attachment.value();
    conversation.preview = turn.userMessage.content;
    conversation.updatedAt = now;
    const auto inserted = chats.insertPreparedTurn(conversation, turn);
    require(inserted.success(), "Prepared context turn was not inserted");

    const auto read = chats.findMessage(turn.assistantMessage.id);
    require(read.success() && read.value().has_value() && read.value()->projectContext == attachment.value(),
        "The exact immutable context attachment did not round-trip");
    auto answer = *read.value();
    answer.status = ChatMessageStatus::Sent;
    answer.content = "Answer";
    require(chats.updateMessage(answer).success(), "Assistant output could not be updated");
    const auto afterOutput = chats.findMessage(answer.id);
    require(afterOutput.success() && afterOutput.value()->projectContext == attachment.value(),
        "Updating Assistant output changed its context attachment");
    // A failed generation turns the Assistant row into an Error row, and Retry turns it back: both carry the
    // attachment and stay readable. A row that is not an Assistant turn can never carry one.
    require(rawSql(workspace.databasePath, "UPDATE chat_messages SET role='error' WHERE id='context-assistant';").empty(),
        "The Assistant row could not become an Error row");
    const auto asError = chats.findMessage(turn.assistantMessage.id);
    require(asError.success() && asError.value().has_value() && asError.value()->role == ChatRole::Error
            && asError.value()->projectContext == attachment.value(),
        "An Error row must stay readable together with its context attachment");
    const auto listedAsError = chats.listMessages(conversation.id);
    require(listedAsError.success() && listedAsError.value().size() == 2,
        "A conversation with an Error row that carries a context must stay listable");
    for (const char* role : {"user", "system"}) {
        require(rawSql(workspace.databasePath,
                    (std::string("UPDATE chat_messages SET role='") + role + "' WHERE id='context-assistant';").c_str())
                    .find("chat_project_context_message_role_guard") != std::string::npos,
            "Raw SQL turned a message with a context attachment into a User or System row");
    }
    require(rawSql(workspace.databasePath, "UPDATE chat_messages SET role='assistant' WHERE id='context-assistant';").empty(),
        "The row could not become an Assistant row again (Retry)");
    require(!chats.insertMessage(turn.assistantMessage).success(),
        "A non-atomic message insert accepted a context attachment");
    require(rawSql(workspace.databasePath,
                "UPDATE chat_project_context_attachments SET rendered_text='changed' "
                "WHERE assistant_message_id='context-assistant';")
                .find("chat_project_context_immutable") != std::string::npos,
        "Raw SQL could mutate an immutable Chat context attachment");

    ChatConversation otherConversation = conversation;
    otherConversation.id = "context-other-conversation";
    otherConversation.title = "Other context chat";
    otherConversation.preview.clear();
    require(chats.insertConversation(otherConversation).success(), "Second context conversation was not inserted");
    ChatMessage otherAssistant = turn.assistantMessage;
    otherAssistant.id = "context-other-assistant";
    otherAssistant.conversationId = otherConversation.id;
    otherAssistant.projectContext.reset();
    require(chats.insertMessage(otherAssistant).success(), "Second Assistant message was not inserted");
    Project otherProject = project;
    otherProject.id = "context-other-project";
    otherProject.name = "Other context project";
    otherProject.rootPath = workspace.projectRoot / "other-context-project";
    fs::create_directories(otherProject.rootPath);
    require(projects.insert(otherProject).success(), "Second context project was not inserted");
    const auto copyRawAttachment = [&](std::string_view messageId, std::string_view conversationId,
                                       std::string_view projectId) {
        const std::string sql =
            "INSERT INTO chat_project_context_attachments SELECT '" + std::string(messageId) + "','"
            + std::string(conversationId) + "','" + std::string(projectId)
            + "',schema_version,snapshot_fingerprint,rendered_content_fingerprint,snapshot_policy_id,"
              "workspace_fingerprint,selected_run_id,includes_derived_project_memory,snapshot_estimated_tokens,"
              "rendered_estimated_tokens,rendered_text FROM chat_project_context_attachments "
              "WHERE assistant_message_id='context-assistant';";
        return rawSql(workspace.databasePath, sql.c_str());
    };
    require(copyRawAttachment(turn.userMessage.id, conversation.id, project.id)
                .find("chat_project_context_invalid_identity") != std::string::npos,
        "Raw SQL attached project context to a non-Assistant message");
    require(copyRawAttachment(otherAssistant.id, conversation.id, project.id)
                .find("chat_project_context_invalid_identity") != std::string::npos,
        "Raw SQL attached project context using a different conversation identity");
    require(copyRawAttachment(otherAssistant.id, otherConversation.id, otherProject.id)
                .find("chat_project_context_invalid_identity") != std::string::npos,
        "Raw SQL attached project context from a different project");

    require(rawSql(workspace.databasePath, "DROP TRIGGER chat_project_context_message_role_guard;").empty(),
        "Could not disable the role guard for defensive-read coverage");
    for (const char* role : {"user", "system"}) {
        require(rawSql(workspace.databasePath,
                    (std::string("UPDATE chat_messages SET role='") + role + "' WHERE id='context-assistant';").c_str()).empty(),
            "Could not inject a wrong role for defensive-read coverage");
        const auto wrongRole = chats.findMessage(turn.assistantMessage.id);
        require(!wrongRole.success() && wrongRole.error()->code == "chat_context.invalid_identity",
            "The read model concealed a context attachment on a User or System row");
        require(!chats.listMessages(conversation.id).success(), "The conversation listing concealed the damaged row");
    }
    require(rawSql(workspace.databasePath, "UPDATE chat_messages SET role='assistant' WHERE id='context-assistant';").empty(),
        "Could not restore the Assistant row");
    require(rawSql(workspace.databasePath, "DROP TRIGGER chat_project_context_validate_insert;").empty(),
        "Could not disable the insert trigger for defensive-read coverage");
    require(copyRawAttachment(otherAssistant.id, conversation.id, project.id).empty(),
        "Could not inject a mismatched attachment for defensive-read coverage");
    const auto corrupted = chats.findMessage(otherAssistant.id);
    require(!corrupted.success() && corrupted.error()->code == "chat_context.invalid_identity",
        "The Chat read model concealed a persisted attachment/conversation mismatch");

    PreparedChatTurn rejected = turn;
    rejected.assistantMessage.id = "context-assistant-rejected";
    rejected.assistantMessage.projectContext.reset();
    conversation.preview = "must not commit";
    const auto duplicate = chats.insertPreparedTurn(conversation, rejected);
    require(!duplicate.success(), "A duplicate user row unexpectedly committed");
    const auto absent = chats.findMessage(rejected.assistantMessage.id);
    const auto unchangedConversation = chats.findConversation(conversation.id);
    require(absent.success() && !absent.value().has_value()
            && unchangedConversation.success() && unchangedConversation.value()->preview == "Use this context",
        "A failed prepared-turn transaction left partial state");
}

void testChatMemorySegmentsPersistOrderCascadeAndMessageIsolation()
{
    TemporaryPersistenceWorkspace workspace;
    const auto now = std::chrono::system_clock::now();
    ConversationId conversationId = "conv-memory";
    ConversationId keptConversationId = "conv-kept";

    const auto makeConversation = [&](const ConversationId& id, const ProviderId& providerId,
                                      const ModelId& modelId) {
        ChatConversation value;
        value.id = id;
        value.providerId = providerId;
        value.modelId = modelId;
        value.title = "Memory test";
        value.preview = "";
        value.createdAt = now;
        value.updatedAt = now;
        return value;
    };
    const auto makeMessage = [&](const std::string& id, const ConversationId& conversation,
                                 const std::string& content) {
        ChatMessage value;
        value.id = id;
        value.conversationId = conversation;
        value.role = ChatRole::User;
        value.status = ChatMessageStatus::Sent;
        value.content = content;
        value.createdAt = now;
        return value;
    };
    const auto makeSegment = [&](const std::string& id, const ConversationId& conversation,
                                 std::chrono::milliseconds offset) {
        ChatMemorySegment value;
        value.id = id;
        value.conversationId = conversation;
        value.firstMessageId = "msg-1";
        value.lastMessageId = "msg-2";
        value.summary = "Summary " + id;
        value.sourceTokenCount = 400;
        value.summaryTokenCount = 40;
        value.createdAt = now + offset;
        return value;
    };

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Chat memory database did not open");
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteChatRepository chats(opened.value());
        SqliteChatMemoryRepository memory(opened.value());
        ProviderCatalogService catalog(providers, models);

        const auto provider = catalog.createProvider(
            {"Memory Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
        require(provider.success(), "Chat memory provider was not created");
        const auto model = catalog.createModel(
            {provider.value(), "gpt-memory", "GPT Memory", 128000, 4096, 1000000, 3000000, 0, 0, true});
        require(model.success(), "Chat memory model was not created");

        require(chats.insertConversation(
                    makeConversation(conversationId, provider.value(), model.value())).success(),
            "Memory conversation was not inserted");
        require(chats.insertConversation(
                    makeConversation(keptConversationId, provider.value(), model.value())).success(),
            "Kept conversation was not inserted");
        require(chats.insertMessage(makeMessage("msg-1", conversationId, "first")).success(),
            "First chat message was not inserted");
        require(chats.insertMessage(makeMessage("msg-2", conversationId, "second")).success(),
            "Second chat message was not inserted");
        require(chats.insertMessage(makeMessage("msg-kept", keptConversationId, "kept")).success(),
            "Kept chat message was not inserted");

        // Insert deliberately out of chronological order; listMemorySegments
        // must return them sorted by created_at_ms with id as tiebreak.
        require(memory.insertMemorySegment(makeSegment("seg-b", conversationId, 2000ms)).success(),
            "Second segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-a", conversationId, 1000ms)).success(),
            "First segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-c", conversationId, 3000ms)).success(),
            "Third segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-kept", keptConversationId, 1000ms)).success(),
            "Kept segment was not inserted");

        require(!memory.insertMemorySegment(makeSegment("seg-orphan", "missing-conversation", 0ms)).success(),
            "Segment for a missing conversation was accepted");
    }

    // Everything above must survive a full database reopen.
    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    require(reopened.success(), "Chat memory database did not reopen");
    SqliteChatRepository chats(reopened.value());
    SqliteChatMemoryRepository memory(reopened.value());

    const auto listed = memory.listMemorySegments(conversationId);
    require(listed.success() && listed.value().size() == 3, "Segments did not persist across reopen");
    require(listed.value()[0].id == "seg-a" && listed.value()[1].id == "seg-b"
            && listed.value()[2].id == "seg-c",
        "Segments are not in stable chronological order");
    require(listed.value()[0].summary == "Summary seg-a"
            && listed.value()[0].sourceTokenCount == 400
            && listed.value()[0].summaryTokenCount == 40,
        "Segment payload did not persist");

    const auto found = memory.findMemorySegment("seg-b");
    require(found.success() && found.value().has_value()
            && found.value()->conversationId == conversationId
            && found.value()->firstMessageId == "msg-1"
            && found.value()->lastMessageId == "msg-2",
        "findMemorySegment did not return the stored segment");

    // Summary layer must not touch the source history.
    const auto messagesBefore = chats.listMessages(conversationId);
    require(messagesBefore.success() && messagesBefore.value().size() == 2
            && messagesBefore.value()[0].content == "first"
            && messagesBefore.value()[1].content == "second",
        "chat_messages were affected by memory segments");

    // Cascade: deleting the conversation removes its segments…
    require(chats.removeConversation(conversationId).success(), "Conversation was not removed");
    const auto afterCascade = memory.listMemorySegments(conversationId);
    require(afterCascade.success() && afterCascade.value().empty(),
        "Segments were not cascade-deleted with the conversation");

    // …while the other conversation keeps both its message and its segment.
    const auto keptMessages = chats.listMessages(keptConversationId);
    require(keptMessages.success() && keptMessages.value().size() == 1,
        "Cascade delete affected another conversation's messages");
    const auto keptSegments = memory.listMemorySegments(keptConversationId);
    require(keptSegments.success() && keptSegments.value().size() == 1
            && keptSegments.value().front().id == "seg-kept",
        "Cascade delete affected another conversation's segments");

    // removeMemorySegments: explicit purge, zero rows is a valid no-op.
    require(memory.removeMemorySegments(keptConversationId).success(),
        "Could not remove segments explicitly");
    require(memory.removeMemorySegments(keptConversationId).success(),
        "Removing zero segments was not a no-op success");
    const auto keptMessagesAfter = chats.listMessages(keptConversationId);
    require(keptMessagesAfter.success() && keptMessagesAfter.value().size() == 1,
        "removeMemorySegments touched chat_messages");
}

void testProjectMemorySegmentsPersistOrderCascadeAndProjectIsolation()
{
    TemporaryPersistenceWorkspace workspace;
    const auto now = std::chrono::system_clock::now();

    const auto makeProject = [&](const ProjectId& id, const std::string& name) {
        Project value;
        value.id = id;
        value.name = name;
        value.description = "";
        value.rootPath = workspace.projectRoot / name;
        value.status = ProjectStatus::Active;
        value.createdAt = now;
        value.updatedAt = now;
        return value;
    };
    const auto makeSegment = [&](const std::string& id, const ProjectId& projectId,
                                 std::chrono::milliseconds offset) {
        ProjectMemorySegment value;
        value.id = id;
        value.projectId = projectId;
        value.runId = "run-1";
        value.summary = "Summary " + id;
        value.sourceTokenCount = 500;
        value.summaryTokenCount = 50;
        value.createdAt = now + offset;
        return value;
    };

    ProjectId projectId = "proj-memory";
    ProjectId keptProjectId = "proj-kept";

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Project memory database did not open");
        SqliteProjectRepository projects(opened.value());
        SqliteProjectMemoryRepository memory(opened.value());

        require(projects.insert(makeProject(projectId, "memory-project")).success(),
            "Memory project was not inserted");
        require(projects.insert(makeProject(keptProjectId, "kept-project")).success(),
            "Kept project was not inserted");

        // Insert deliberately out of chronological order; listMemorySegments
        // must return them sorted by created_at_ms with id as tiebreak.
        require(memory.insertMemorySegment(makeSegment("seg-b", projectId, 2000ms)).success(),
            "Second segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-a", projectId, 1000ms)).success(),
            "First segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-c", projectId, 3000ms)).success(),
            "Third segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-kept", keptProjectId, 1000ms)).success(),
            "Kept segment was not inserted");

        // Two segments sharing the exact same createdAt must still resolve
        // deterministically via the id tiebreak in ORDER BY created_at_ms,id —
        // insert "seg-tie-z" before "seg-tie-a" so a naive insertion-order
        // read would fail this check.
        require(memory.insertMemorySegment(makeSegment("seg-tie-z", projectId, 5000ms)).success(),
            "Tie segment z was not inserted");
        require(memory.insertMemorySegment(makeSegment("seg-tie-a", projectId, 5000ms)).success(),
            "Tie segment a was not inserted");

        const auto orphanResult = memory.insertMemorySegment(makeSegment("seg-orphan", "missing-project", 0ms));
        require(!orphanResult.success(), "Segment for a missing project was accepted");
        require(orphanResult.error()->code == "project_memory.project_not_found",
            "Missing-project insert did not report project_memory.project_not_found");

        const auto duplicateResult = memory.insertMemorySegment(makeSegment("seg-a", projectId, 1000ms));
        require(!duplicateResult.success(), "Duplicate segment id was accepted");
        require(duplicateResult.error()->code == "project_memory.duplicate_id",
            "Duplicate segment id did not report project_memory.duplicate_id");
    }

    // Everything above must survive a full database reopen.
    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Project memory database did not reopen");
        SqliteProjectRepository projects(reopened.value());
        SqliteProjectMemoryRepository memory(reopened.value());

        const auto listed = memory.listMemorySegments(projectId);
        require(listed.success() && listed.value().size() == 5, "Segments did not persist across reopen");
        require(listed.value()[0].id == "seg-a" && listed.value()[1].id == "seg-b"
                && listed.value()[2].id == "seg-c",
            "Segments are not in stable chronological order");
        // Tie-break: "seg-tie-a" and "seg-tie-z" share createdAt (5000ms offset)
        // and were inserted z-then-a, so only the id order proves the ORDER BY
        // created_at_ms,id tiebreak — insertion order alone would fail this.
        require(listed.value()[3].id == "seg-tie-a" && listed.value()[4].id == "seg-tie-z",
            "Segments sharing createdAt are not ordered by id as the tiebreak");

        // Bounded newest-first read used by the project context snapshot: at most `limit` rows, newest first with
        // the id tiebreak DESCENDING, the project's total count, and never another project's rows.
        const auto recent = memory.listRecentMemorySegments(projectId, 3);
        require(recent.success() && recent.value().totalCount == 5 && recent.value().newest.size() == 3
                && recent.value().newest[0].id == "seg-tie-z" && recent.value().newest[1].id == "seg-tie-a"
                && recent.value().newest[2].id == "seg-c",
            "listRecentMemorySegments did not return the newest rows first, with the id tiebreak and the total");
        require(recent.value().newest[0].summary == "Summary seg-tie-z"
                && recent.value().newest[0].summaryTokenCount == 50 && recent.value().newest[0].runId == "run-1",
            "listRecentMemorySegments did not read the full row");
        const auto wide = memory.listRecentMemorySegments(projectId, 100);
        require(wide.success() && wide.value().totalCount == 5 && wide.value().newest.size() == 5
                && wide.value().newest.front().id == "seg-tie-z" && wide.value().newest.back().id == "seg-a",
            "A limit above the row count must return every row, newest first");
        const auto zero = memory.listRecentMemorySegments(projectId, 0);
        require(zero.success() && zero.value().totalCount == 5 && zero.value().newest.empty(),
            "A zero limit must return no rows but the real total");
        const auto keptRecent = memory.listRecentMemorySegments(keptProjectId, 10);
        require(keptRecent.success() && keptRecent.value().totalCount == 1 && keptRecent.value().newest.size() == 1
                && keptRecent.value().newest.front().id == "seg-kept",
            "listRecentMemorySegments leaked or miscounted another project's rows");
        const auto unknown = memory.listRecentMemorySegments("no-such-project", 10);
        require(unknown.success() && unknown.value().totalCount == 0 && unknown.value().newest.empty(),
            "An unknown project has no recent segments and a zero total");
        require(listed.value()[0].summary == "Summary seg-a"
                && listed.value()[0].sourceTokenCount == 500
                && listed.value()[0].summaryTokenCount == 50
                && listed.value()[0].runId == "run-1",
            "Segment payload did not persist");

        const auto found = memory.findMemorySegment("seg-b");
        require(found.success() && found.value().has_value()
                && found.value()->projectId == projectId
                && found.value()->runId == "run-1",
            "findMemorySegment did not return the stored segment");

        const auto stillThere = projects.findById(projectId);
        require(stillThere.success() && stillThere.value().has_value(), "Project lookup failed before delete");
    }

    // Cascade: deleting the project removes its segments. IProjectRepository
    // has no delete API, so this drives the cascade through a fresh native
    // sqlite3 connection, the same approach testMigrationRejectsNewerSchema
    // uses to reach state outside the repository ports, then reopens through
    // SqliteDatabase again to observe the effect the production code sees.
    {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open project memory database through native sqlite3");
        require(sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not enable foreign keys through native sqlite3");
        require(sqlite3_exec(handle, "DELETE FROM projects WHERE id='proj-memory';", nullptr, nullptr, nullptr)
                == SQLITE_OK,
            "Could not delete memory project through native sqlite3");
        sqlite3_close_v2(handle);
    }

    const auto reopenedAfterDelete = SqliteDatabase::open(workspace.databasePath);
    require(reopenedAfterDelete.success(), "Project memory database did not reopen after delete");
    SqliteProjectMemoryRepository memoryAfterDelete(reopenedAfterDelete.value());

    const auto afterCascade = memoryAfterDelete.listMemorySegments(projectId);
    require(afterCascade.success() && afterCascade.value().empty(),
        "Segments were not cascade-deleted with the project");

    // …while the other project keeps its segment.
    const auto keptSegments = memoryAfterDelete.listMemorySegments(keptProjectId);
    require(keptSegments.success() && keptSegments.value().size() == 1
            && keptSegments.value().front().id == "seg-kept",
        "Cascade delete affected another project's segments");

    // removeMemorySegments: explicit purge, zero rows is a valid no-op.
    require(memoryAfterDelete.removeMemorySegments(keptProjectId).success(),
        "Could not remove segments explicitly");
    require(memoryAfterDelete.removeMemorySegments(keptProjectId).success(),
        "Removing zero segments was not a no-op success");
    const auto keptAfterPurge = memoryAfterDelete.listMemorySegments(keptProjectId);
    require(keptAfterPurge.success() && keptAfterPurge.value().empty(),
        "removeMemorySegments did not remove the kept project's segments");
}

void testProjectMemoryReplaceSegmentsAndCompactionRecordsPersistOrderAndCascade()
{
    TemporaryPersistenceWorkspace workspace;
    const auto now = std::chrono::system_clock::now();

    const auto makeProject = [&](const ProjectId& id, const std::string& name) {
        Project value;
        value.id = id;
        value.name = name;
        value.description = "";
        value.rootPath = workspace.projectRoot / name;
        value.status = ProjectStatus::Active;
        value.createdAt = now;
        value.updatedAt = now;
        return value;
    };
    const auto makeSegment = [&](const std::string& id, const ProjectId& projectId,
                                 std::chrono::milliseconds offset) {
        ProjectMemorySegment value;
        value.id = id;
        value.projectId = projectId;
        value.runId = "run-1";
        value.summary = "Summary " + id;
        value.sourceTokenCount = 500;
        value.summaryTokenCount = 50;
        value.createdAt = now + offset;
        return value;
    };
    const auto makeCompactedSegment = [&](const std::string& id, const ProjectId& projectId,
                                          std::chrono::milliseconds offset) {
        ProjectMemorySegment value;
        value.id = id;
        value.projectId = projectId;
        value.runId = ""; // not tied to a single run - see ProjectMemoryCompactionService.cpp
        value.summary = "Consolidated summary";
        value.sourceTokenCount = 1500;
        value.summaryTokenCount = 120;
        value.createdAt = now + offset;
        return value;
    };
    const auto makeCompletedRecord = [&](const std::string& id, const ProjectId& projectId,
                                         std::vector<ProjectMemorySegmentId> sourceIds,
                                         const ProjectMemorySegmentId& resultId,
                                         std::chrono::milliseconds offset) {
        ProjectMemoryCompactionRecord record;
        record.id = id;
        record.projectId = projectId;
        record.providerId = "prov-1";
        record.modelId = "model-1";
        record.sourceSegmentIds = std::move(sourceIds);
        record.sourceContentHash = "hash-" + id;
        record.resultSegmentId = resultId;
        record.inputTokens = 40;
        record.outputTokens = 15;
        record.costMicros = 425;
        record.status = ProjectMemoryCompactionStatus::Completed;
        record.createdAt = now + offset;
        return record;
    };
    const auto makeFailedRecord = [&](const std::string& id, const ProjectId& projectId,
                                      std::vector<ProjectMemorySegmentId> sourceIds,
                                      std::chrono::milliseconds offset) {
        ProjectMemoryCompactionRecord record;
        record.id = id;
        record.projectId = projectId;
        record.providerId = "prov-1";
        record.modelId = "model-1";
        record.sourceSegmentIds = std::move(sourceIds);
        record.sourceContentHash = "hash-" + id;
        record.status = ProjectMemoryCompactionStatus::Failed;
        record.errorCode = "provider.rate_limited";
        record.errorMessage = "Simulated rate limit";
        record.createdAt = now + offset;
        return record;
    };

    ProjectId projectId = "proj-compaction";
    ProjectId keptProjectId = "proj-compaction-kept";

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Compaction database did not open");
        SqliteProjectRepository projects(opened.value());
        SqliteProjectMemoryRepository memory(opened.value());
        SqliteProjectMemoryCompactionRepository compactions(opened.value());

        require(projects.insert(makeProject(projectId, "compaction-project")).success(),
            "Compaction project was not inserted");
        require(projects.insert(makeProject(keptProjectId, "compaction-kept-project")).success(),
            "Kept project was not inserted");

        require(memory.insertMemorySegment(makeSegment("cseg-a", projectId, 1000ms)).success(),
            "First source segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("cseg-b", projectId, 2000ms)).success(),
            "Second source segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("cseg-c", projectId, 3000ms)).success(),
            "Third source segment was not inserted");
        require(memory.insertMemorySegment(makeSegment("cseg-kept", keptProjectId, 1000ms)).success(),
            "Kept project's segment was not inserted");

        // Log a failed attempt first (source segments untouched) so the
        // ordering/reopen assertions below have two records to check.
        require(compactions
                    .insertCompactionRecord(makeFailedRecord(
                        "rec-failed", projectId, {"cseg-a", "cseg-b", "cseg-c"}, 4000ms))
                    .success(),
            "Failed compaction record was not inserted");
        const auto stillThreeSegments = memory.listMemorySegments(projectId);
        require(stillThreeSegments.success() && stillThreeSegments.value().size() == 3,
            "Logging a failed compaction record must never touch the segments themselves");

        // A successful commit atomically removes all sources, inserts the
        // consolidated segment, and records its immutable provenance.
        const auto completedRecord = makeCompletedRecord(
            "rec-completed", projectId, {"cseg-a", "cseg-b", "cseg-c"}, "cseg-result", 5000ms);
        const auto replaced = compactions.commitSuccessfulCompaction(
            completedRecord, makeCompactedSegment("cseg-result", projectId, 5000ms));
        require(replaced.success(), "Atomic compaction commit failed");
        const auto afterReplace = memory.listMemorySegments(projectId);
        require(afterReplace.success() && afterReplace.value().size() == 1
                && afterReplace.value().front().id == "cseg-result",
            "Atomic compaction did not replace the three sources");

        // A stale second provider response cannot create a duplicate summary
        // after the first compaction has consumed its source rows.
        const auto staleRecord = makeCompletedRecord(
            "rec-stale", projectId, {"cseg-a", "cseg-b", "cseg-c"}, "cseg-stale", 6000ms);
        const auto stale = compactions.commitSuccessfulCompaction(
            staleRecord, makeCompactedSegment("cseg-stale", projectId, 6000ms));
        require(!stale.success() && stale.error()->code == "project_memory.compaction_conflict",
            "A stale compaction did not fail closed with compaction_conflict");
        const auto afterStale = memory.listMemorySegments(projectId);
        require(afterStale.success() && afterStale.value().size() == 1
                && afterStale.value().front().id == "cseg-result",
            "A stale compaction changed the committed memory state");
        require(compactions
                    .insertCompactionRecord(makeFailedRecord(
                        "rec-kept", keptProjectId, {"cseg-kept"}, 1000ms))
                    .success(),
            "Kept project's failed compaction record was not inserted");

        const auto nonAtomicCompleted = compactions.insertCompactionRecord(makeCompletedRecord(
            "rec-invalid-completed", projectId, {"cseg-result"}, "result-invalid", 7000ms));
        require(!nonAtomicCompleted.success()
                && nonAtomicCompleted.error()->code == "project_memory.invalid_compaction_commit",
            "A Completed provenance row was accepted without an atomic memory commit");

        const auto duplicateRecord = compactions.insertCompactionRecord(
            makeFailedRecord("rec-failed", projectId, {"cseg-a"}, 7000ms));
        require(!duplicateRecord.success(), "Duplicate compaction record id was accepted");
        require(duplicateRecord.error()->code == "project_memory.duplicate_id",
            "Duplicate compaction record id did not report project_memory.duplicate_id");
    }

    // Everything above must survive a full database reopen.
    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Compaction database did not reopen");
        SqliteProjectMemoryRepository memory(reopened.value());
        SqliteProjectMemoryCompactionRepository compactions(reopened.value());

        const auto segments = memory.listMemorySegments(projectId);
        require(segments.success() && segments.value().size() == 1
                && segments.value().front().id == "cseg-result",
            "The atomic compaction result did not persist across reopen");

        const auto records = compactions.listCompactionRecords(projectId);
        require(records.success() && records.value().size() == 2,
            "Both compaction records must persist across reopen");
        require(records.value()[0].id == "rec-failed" && records.value()[1].id == "rec-completed",
            "Compaction records are not in stable chronological order");

        const auto& failed = records.value()[0];
        require(failed.status == ProjectMemoryCompactionStatus::Failed
                && failed.errorCode == "provider.rate_limited"
                && !failed.resultSegmentId.has_value(),
            "Failed record's status/error/absent resultSegmentId did not persist");
        require(failed.sourceSegmentIds == std::vector<ProjectMemorySegmentId> {"cseg-a", "cseg-b", "cseg-c"},
            "Failed record's ordered sourceSegmentIds did not persist");

        const auto& completed = records.value()[1];
        require(completed.status == ProjectMemoryCompactionStatus::Completed
                && completed.resultSegmentId.has_value() && *completed.resultSegmentId == "cseg-result"
                && completed.inputTokens == 40 && completed.outputTokens == 15
                && completed.costMicros == 425,
            "Completed record's status/resultSegmentId/usage did not persist");

        const auto keptRecords = compactions.listCompactionRecords(keptProjectId);
        require(keptRecords.success() && keptRecords.value().size() == 1,
            "The other project's compaction record must be isolated from projectId's");
    }

    // Cascade: deleting the project removes its compaction records too.
    {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open compaction database through native sqlite3");
        require(sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not enable foreign keys through native sqlite3");
        require(sqlite3_exec(handle, "DELETE FROM projects WHERE id='proj-compaction';", nullptr, nullptr, nullptr)
                == SQLITE_OK,
            "Could not delete compaction project through native sqlite3");
        sqlite3_close_v2(handle);
    }

    const auto reopenedAfterDelete = SqliteDatabase::open(workspace.databasePath);
    require(reopenedAfterDelete.success(), "Compaction database did not reopen after delete");
    SqliteProjectMemoryCompactionRepository compactionsAfterDelete(reopenedAfterDelete.value());

    const auto afterCascade = compactionsAfterDelete.listCompactionRecords(projectId);
    require(afterCascade.success() && afterCascade.value().empty(),
        "Compaction records were not cascade-deleted with the project");

    const auto keptAfterCascade = compactionsAfterDelete.listCompactionRecords(keptProjectId);
    require(keptAfterCascade.success() && keptAfterCascade.value().size() == 1,
        "Cascade delete affected another project's compaction records");
}

// Runs raw SQL on the database file through the native API (foreign keys on, like production),
// returning the SQLite error message, or an empty string on success.
std::string rawSql(const fs::path& databasePath, const char* sql)
{
    sqlite3* handle = nullptr;
    const std::u8string path = databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open the database through native sqlite3");
    require(sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not enable foreign keys through native sqlite3");
    char* rawMessage = nullptr;
    const int result = sqlite3_exec(handle, sql, nullptr, nullptr, &rawMessage);
    std::string message = result == SQLITE_OK ? std::string {}
        : (rawMessage != nullptr ? std::string(rawMessage) : std::string("unknown SQLite error"));
    sqlite3_free(rawMessage);
    sqlite3_close_v2(handle);
    return message;
}

// AG-17.7C (SQLite specifics): v58 migration, the immutable trigger, the CHECK constraints, the
// transaction rollback, fail-closed reads of a corrupted record and reopen persistence. The shared
// aggregate contract runs in testSharedRunRepositoryContracts.
void testLegacyConversionRecordSqliteSpecifics()
{
    TemporaryPersistenceWorkspace workspace;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    RunId sourceRunId;
    ai::tests::ContractConversion good;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open the conversion test database");
        SqliteProjectRepository projects(opened.value());
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteRunRepository runs(opened.value());
        ProjectService projectService(projects);
        ProviderCatalogService catalog(providers, models);
        const auto project = projectService.createProject(
            {"Conversion project", "Conversion SQLite specifics", workspace.projectRoot});
        require(project.success(), "Could not create the conversion project");
        projectId = project.value().id;
        const auto provider = catalog.createProvider(
            {"Conversion Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
        require(provider.success(), "Could not create the conversion provider");
        providerId = provider.value();
        const auto model = catalog.createModel(
            {providerId, "conversion-model", "Conversion Model", 128000, 4096, 1000000, 3000000, 0, 0, true});
        require(model.success(), "Could not create the conversion model");
        modelId = model.value();
        sourceRunId = ai::tests::insertContractLegacySource(runs, "conv", projectId);

        // 1. v58 stores a converted Run with its record.
        good = ai::tests::contractConversion("good", projectId, providerId, modelId, sourceRunId, false);
        require(runs.insertRunWithStagesAndLegacyConversion(
                    good.run, good.stages, good.record, std::nullopt).success(),
            "A conversion aggregate was refused");
    }

    // 2. The record survives a reopen exactly.
    {
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "The conversion database did not reopen");
        SqliteRunRepository runs(reopened.value());
        const auto found = runs.findLegacyConversion(good.run.id);
        require(found.success() && found.value().has_value()
                && ai::tests::sameConversionRecord(*found.value(), good.record),
            "The conversion record did not survive a reopen");
    }

    // 3. The record is immutable and its CHECK constraints hold.
    require(rawSql(workspace.databasePath,
                "UPDATE run_legacy_conversions SET policy_id='tampered' WHERE run_id='good-converted-run';")
                .find("run_legacy_conversion_immutable") != std::string::npos,
        "A conversion record must be immutable");
    const std::string hex(64, 'a');
    const auto row = [&](const std::string& workflow, const std::string& reason, const std::string& dropped,
                         const std::string& mode, const std::string& confirmedAt, const std::string& sourceRun) {
        return "INSERT INTO run_legacy_conversions(run_id,source_run_id,source_workflow_id,policy_id,reason_code,"
               "source_snapshot_fingerprint,target_roster_fingerprint,confirmation_fingerprint,dropped_json,"
               "confirmation_mode,confirmed_at_ms,created_at_ms) VALUES ('conv-legacy-source-run','" + sourceRun
               + "','" + workflow + "','p','" + reason + "','" + hex + "','" + hex + "','" + hex + "','"
               + dropped + "','" + mode + "'," + confirmedAt + ",1);";
    };
    const char* lone = "legacy_roster.converted_lone_workspace_lead";
    // Each row below must be refused BY A CHECK CONSTRAINT: the run_id is an existing Run (so no
    // foreign-key problem) and no earlier row used it (so no key conflict).
    const auto refusedByCheck = [&](const std::string& sql) {
        return rawSql(workspace.databasePath, sql.c_str()).find("CHECK constraint failed") != std::string::npos;
    };
    require(refusedByCheck(row("manifest-v9", lone, "[]", "explicit_user", "5", "other")),
        "An unknown source workflow must be refused");
    require(refusedByCheck(row("execution-profile-v1", "legacy_roster.empty_snapshot", "[]", "explicit_user", "5", "other")),
        "A reason code that is not a conversion must be refused");
    require(refusedByCheck(row("execution-profile-v1", lone, "[]", "confirmed", "5", "other")),
        "An unknown confirmation mode must be refused");
    require(refusedByCheck(row("execution-profile-v1", lone, "[]", "explicit_user", "NULL", "other")),
        "An explicit confirmation without a time must be refused");
    require(refusedByCheck(row("execution-profile-v1", lone, "[]", "not_required_safe_retry", "5", "other")),
        "A safe retry with a confirmation time must be refused");
    require(refusedByCheck(row("execution-profile-v1", lone, "[]", "explicit_user", "5", "conv-legacy-source-run")),
        "A record that is its own source must be refused");

    // 4. A failure of the record insert rolls the WHOLE aggregate back (the Run included).
    require(rawSql(workspace.databasePath,
                "CREATE TRIGGER block_conversion_insert BEFORE INSERT ON run_legacy_conversions "
                "BEGIN SELECT RAISE(ABORT,'blocked_for_test'); END;").empty(),
        "Could not install the blocking trigger");
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not reopen the database for the rollback test");
        SqliteRunRepository runs(opened.value());
        auto blocked = ai::tests::contractConversion("blocked", projectId, providerId, modelId, sourceRunId, false);
        const auto refused = runs.insertRunWithStagesAndLegacyConversion(
            blocked.run, blocked.stages, blocked.record, std::nullopt);
        require(!refused.success(), "The blocked record insert must fail the aggregate");
        const auto run = runs.findRun(blocked.run.id);
        require(run.success() && !run.value().has_value(),
            "A failed record insert must roll the Run back");
        const auto stages = runs.listStages(blocked.run.id);
        require(stages.success() && stages.value().empty(),
            "A failed record insert must roll the stages back");
    }
    require(rawSql(workspace.databasePath, "DROP TRIGGER block_conversion_insert;").empty(),
        "Could not remove the blocking trigger");

    // 5. A corrupted record fails closed with a typed error and is never returned half trusted.
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not reopen the database for the corruption test");
        SqliteRunRepository runs(opened.value());
        const char* cases[] = {"c1", "c2", "c3", "c4", "c5", "c6"};
        for (const char* name : cases) {
            auto aggregate = ai::tests::contractConversion(name, projectId, providerId, modelId, sourceRunId, false);
            require(runs.insertRunWithStagesAndLegacyConversion(
                        aggregate.run, aggregate.stages, aggregate.record, std::nullopt).success(),
                "Could not store a conversion to corrupt");
        }
    }
    require(rawSql(workspace.databasePath, "DROP TRIGGER run_legacy_conversions_reject_update;").empty(),
        "Could not drop the immutable trigger to simulate external corruption");
    const std::string flipped(64, 'b');
    require(rawSql(workspace.databasePath,
                "UPDATE run_legacy_conversions SET dropped_json='not json' WHERE run_id='c1-converted-run';").empty()
            && rawSql(workspace.databasePath,
                ("UPDATE run_legacy_conversions SET target_roster_fingerprint='" + flipped
                    + "' WHERE run_id='c2-converted-run';").c_str()).empty()
            && rawSql(workspace.databasePath,
                "UPDATE run_legacy_conversions SET source_run_id='someone-else' WHERE run_id='c3-converted-run';").empty()
            && rawSql(workspace.databasePath,
                "UPDATE run_legacy_conversions SET reason_code='legacy_roster.converted_lone_workspace_lead' "
                "WHERE run_id='c4-converted-run';").empty()
            && rawSql(workspace.databasePath,
                "UPDATE run_legacy_conversions SET dropped_json='[{\"memberId\":\"architect\",\"setting\":"
                "\"instructions\",\"legacyValue\":\"the secret plan\",\"reason\":\"r\"}]' "
                "WHERE run_id='c5-converted-run';").empty()
            && rawSql(workspace.databasePath,
                "UPDATE run_legacy_conversions SET confirmation_mode='not_required_safe_retry', "
                "confirmed_at_ms=NULL WHERE run_id='c6-converted-run';").empty(),
        "Could not corrupt the conversion records");
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not reopen the database after the corruption");
        SqliteRunRepository runs(opened.value());
        for (const char* name : {"c1", "c2", "c3", "c4", "c5", "c6"}) {
            const auto read = runs.findLegacyConversion(std::string(name) + "-converted-run");
            require(!read.success() && read.error()->code == kLegacyConversionRecordCorruptCode,
                (std::string("A corrupted conversion record was not reported as corrupt: ") + name).c_str());
        }
        // The healthy neighbour is untouched.
        const auto healthy = runs.findLegacyConversion("good-converted-run");
        require(healthy.success() && healthy.value().has_value(), "A healthy record must still read");
    }

    // AG-17.7C-3: the history INDEX is a marker, not the record. It lists every conversion row -
    // corrupt ones included, so a damaged audit is flagged and never hidden - with the stored
    // mode, and reports a mode it does not recognise as such instead of dropping the row.
    require(rawSql(workspace.databasePath,
                "PRAGMA ignore_check_constraints=ON; "
                "UPDATE run_legacy_conversions SET confirmation_mode='bogus' "
                "WHERE run_id='c2-converted-run';").empty(),
        "Could not plant an unrecognised confirmation mode");
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not reopen the database for the index test");
        SqliteRunRepository runs(opened.value());
        const auto index = runs.listLegacyConversionIndex();
        require(index.success(), "The conversion index must be readable");
        const auto entryFor = [&](const std::string& id) -> const LegacyConversionIndexEntry* {
            for (const auto& entry : index.value()) {
                if (entry.runId == id) return &entry;
            }
            return nullptr;
        };
        require(index.value().size() == 7, "The index lists every conversion row, damaged or not");
        const auto* good = entryFor("good-converted-run");
        require(good != nullptr && good->mode == std::optional<LegacyConversionConfirmationMode>(
                    LegacyConversionConfirmationMode::ExplicitUser),
            "A healthy conversion is indexed with its mode");
        const auto* safeMode = entryFor("c6-converted-run");
        require(safeMode != nullptr && safeMode->mode == std::optional<LegacyConversionConfirmationMode>(
                    LegacyConversionConfirmationMode::NotRequiredSafeRetry),
            "The index reports the stored mode even when the record itself fails its integrity check");
        const auto* bogus = entryFor("c2-converted-run");
        require(bogus != nullptr && !bogus->mode.has_value(),
            "An unrecognised mode is reported as such, never hidden");
        // Sorted, so the marker projection is deterministic.
        for (std::size_t position = 1; position < index.value().size(); ++position) {
            require(index.value()[position - 1].runId < index.value()[position].runId,
                "The index must come back ordered by Run id");
        }
    }

    // 6. Deleting the converted Run deletes its record with it (cascade).
    require(rawSql(workspace.databasePath, "DELETE FROM runs WHERE id='good-converted-run';").empty(),
        "Could not delete the converted Run");
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not reopen the database after the delete");
        SqliteRunRepository runs(opened.value());
        const auto gone = runs.findLegacyConversion("good-converted-run");
        require(gone.success() && !gone.value().has_value(), "The record must go with its Run");
        const auto index = runs.listLegacyConversionIndex();
        require(index.success() && index.value().size() == 6
                && std::none_of(index.value().begin(), index.value().end(),
                    [](const LegacyConversionIndexEntry& entry) { return entry.runId == "good-converted-run"; }),
            "A deleted converted Run leaves the index with its record");
    }
}

// AG-17.7C: a database at schema v57 gains the conversion table (and stays usable) on open.
void testV57ToV58ConversionTableMigration()
{
    TemporaryPersistenceWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the fixture database");
    }
    // Rewind exactly what the v58 migration adds, then let open() migrate again.
    require(rawSql(workspace.databasePath,
                "DROP TRIGGER run_legacy_conversions_reject_update;"
                "DROP TABLE run_legacy_conversions;"
                "PRAGMA user_version = 57;").empty(),
        "Could not rewind the fixture database to schema v57");
    const auto migrated = SqliteDatabase::open(workspace.databasePath);
    require(migrated.success(), "A v57 database did not migrate to v58");
    // The table exists again, and the immutable trigger came back with it.
    require(rawSql(workspace.databasePath, "SELECT count(*) FROM run_legacy_conversions;").empty(),
        "The conversion table was not recreated by the migration");
    require(!rawSql(workspace.databasePath,
                "CREATE TRIGGER run_legacy_conversions_reject_update BEFORE UPDATE ON run_legacy_conversions "
                "BEGIN SELECT 1; END;").empty(),
        "The migration must have created the immutable trigger");
    // Migrating twice is a no-op, and a database newer than v58 is still refused.
    const auto again = SqliteDatabase::open(workspace.databasePath);
    require(again.success(), "A v58 database must reopen");
}

// An empty string when the database is at exactly this schema version (an integer-overflow error
// is the failure signal of the probe).
std::string schemaVersionIs(const fs::path& databasePath, int version)
{
    const std::string sql =
        "SELECT CASE WHEN (SELECT user_version FROM pragma_user_version) = "
        + std::to_string(version) + " THEN 1 ELSE abs(-9223372036854775808) END;";
    return rawSql(databasePath, sql.c_str());
}

// The schema version a database that opened successfully ends at.
constexpr int kLatestSchemaVersion = 70;

// The same probe for a single numeric value: empty when `expression` evaluates to `expected`.
std::string sqlValueIs(const fs::path& databasePath, const std::string& expression, long long expected)
{
    const std::string sql = "SELECT CASE WHEN (" + expression + ") = " + std::to_string(expected)
        + " THEN 1 ELSE abs(-9223372036854775808) END;";
    return rawSql(databasePath, sql.c_str());
}

// Run warnings (schema step 68). An artifact store that loses every write: the Mock workflow then ends Completed with NO artifact
// and the only trace of what it was to leave behind is the warnings it recorded.
class AlwaysLosingArtifactStore final : public IArtifactStore {
public:
    ai::common::Result<StoredArtifactFile> writeArtifact(const RunId&, std::string_view, std::string_view) override
    {
        return ai::common::Result<StoredArtifactFile>::failure({"artifact_store.write_failed", "disk is full"});
    }
    ai::common::Result<void> removeArtifact(const std::filesystem::path&) override
    {
        return ai::common::Result<void>::ok();
    }
    ai::common::Result<std::filesystem::path> resolveAbsolutePath(const std::filesystem::path&) const override
    {
        return ai::common::Result<std::filesystem::path>::failure({"artifact.not_found", "none"});
    }
    ai::common::Result<std::string> readArtifact(const std::filesystem::path&) const override
    {
        return ai::common::Result<std::string>::failure({"artifact.not_found", "none"});
    }
};

// A completed Mock Run whose artifacts were all lost; returns its id.
RunId runMockWorkflowThatLosesItsArtifacts(const std::shared_ptr<SqliteDatabase>& database, const fs::path& projectRoot)
{
    SqliteProjectRepository projects(database);
    SqliteRunRepository runs(database);
    SqliteArtifactRepository artifacts(database);
    SqliteRunWarningRepository warnings(database);
    AlwaysLosingArtifactStore store;
    ArtifactService artifactService(artifacts, store, &warnings);
    ProjectService projectService(projects);
    const auto project = projectService.createProject({"Warning project", "Run warnings through SQLite", projectRoot});
    require(project.success(), "Could not persist the warning project");

    MockWorkflowRunner workflow(1ms, 1, &artifactService);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver);
    OrchestrationService orchestration(projects, runs, manager);
    const auto started = orchestration.startRun({project.value().id, "Lossy run", "Lose every artifact"});
    require(started.success(), "Could not enqueue the lossy run");
    const Run finished = waitForTerminalRun(runs, started.value());
    require(finished.status == RunStatus::Completed, "A run that only lost diagnostic artifacts must still complete");
    return started.value();
}

void testRunWarningsPersistDeduplicateRejectBadRowsAndSurviveReopen()
{
    TemporaryPersistenceWorkspace workspace;
    RunId runId;
    std::vector<RunWarning> firstRead;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not open the warning database");
        runId = runMockWorkflowThatLosesItsArtifacts(opened.value(), workspace.projectRoot);

        SqliteRunWarningRepository warnings(opened.value());
        const auto listed = warnings.listWarnings(runId);
        require(listed.success() && listed.value().size() == 2,
            "The two lost Mock artifacts must leave two warnings in SQLite");
        firstRead = listed.value();
        require(firstRead[0].code == kRunWarningDiagnosticArtifactLostCode && firstRead[0].runId == runId,
            "A warning must keep its code and Run");
        require((firstRead[0].artifactName == "context-snapshot.md" && firstRead[0].stageId.has_value()
                    && firstRead[1].artifactName == "mock-run-report.md" && !firstRead[1].stageId.has_value())
                || (firstRead[1].artifactName == "context-snapshot.md" && firstRead[1].stageId.has_value()
                    && firstRead[0].artifactName == "mock-run-report.md" && !firstRead[0].stageId.has_value()),
            "A warning must keep the artifact name and whether it belonged to a stage (NULL stage is its own identity)");

        // The same failure again - same Run, stage, code and artifact name - is not a second row, and says so.
        RunWarning repeated = firstRead[0];
        repeated.id = "another-id";
        const auto duplicate = warnings.recordWarning(repeated);
        require(duplicate.success() && !duplicate.value(), "Recording the same failure again must report 'already there'");
        // ... also for a warning without a stage (a NULL stage must not make every repeat look new).
        const auto& noStage = firstRead[0].stageId.has_value() ? firstRead[1] : firstRead[0];
        RunWarning repeatedNoStage = noStage;
        repeatedNoStage.id = "another-id-2";
        const auto duplicateNoStage = warnings.recordWarning(repeatedNoStage);
        require(duplicateNoStage.success() && !duplicateNoStage.value(),
            "Recording the same stage-less failure again must report 'already there'");
        require(warnings.listWarnings(runId).value().size() == 2, "A repeated failure must not add a row");

        // A different artifact is a new warning.
        RunWarning other = firstRead[0];
        other.id = "other-id";
        other.artifactName = "other.md";
        const auto added = warnings.recordWarning(other);
        require(added.success() && added.value(), "A different artifact must be a new warning");

        // Anything that is not 'the same warning again' is a FAILURE the caller hears about - never an ignored row.
        RunWarning emptyMessage = firstRead[0];
        emptyMessage.id = "bad-1";
        emptyMessage.artifactName = "bad-1.md";
        emptyMessage.message.clear();
        require(!warnings.recordWarning(emptyMessage).success(), "An empty message must be refused");
        RunWarning oversized = emptyMessage;
        oversized.id = "bad-2";
        oversized.artifactName = "bad-2.md";
        oversized.message.assign(kRunWarningMessageMaxBytes + 1, 'x');
        require(!warnings.recordWarning(oversized).success(), "An oversized message must be refused");
        RunWarning orphan = firstRead[0];
        orphan.id = "bad-3";
        orphan.runId = "no-such-run";
        require(!warnings.recordWarning(orphan).success(), "A warning for a Run that does not exist must be refused");
        require(warnings.listWarnings(runId).value().size() == 3, "Refused rows must leave nothing behind");
    }
    {
        // A new connection: the warnings are the same after a restart, in the same order.
        const auto reopened = SqliteDatabase::open(workspace.databasePath);
        require(reopened.success(), "Could not reopen the warning database");
        SqliteRunWarningRepository warnings(reopened.value());
        const auto listed = warnings.listWarnings(runId);
        require(listed.success() && listed.value().size() == 3, "Warnings did not survive reopen");
        for (std::size_t index = 0; index < 2; ++index) {
            const auto& expected = firstRead[index];
            const auto found = std::find_if(listed.value().begin(), listed.value().end(),
                [&expected](const RunWarning& row) { return row.id == expected.id; });
            require(found != listed.value().end() && found->message == expected.message
                    && found->artifactName == expected.artifactName && found->stageId == expected.stageId
                    && found->createdAt == expected.createdAt,
                "A warning changed across a restart");
        }
        require(warnings.listWarnings("another-run").value().empty(), "Another Run has no warnings");
    }
}

void testV67ToV68RunWarningsMigrationIsAdditiveAndRepeatable()
{
    TemporaryPersistenceWorkspace workspace;
    RunId runId;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the fixture database");
        runId = runMockWorkflowThatLosesItsArtifacts(opened.value(), workspace.projectRoot);
    }
    require(schemaVersionIs(workspace.databasePath, kLatestSchemaVersion).empty(), "A new database is at the latest schema");
    require(sqlValueIs(workspace.databasePath, "SELECT count(*) FROM run_warnings", 2).empty(), "setup: two warnings");

    // A database whose version was only set back (a rewound test fixture) re-runs the step and keeps what is there.
    require(rawSql(workspace.databasePath, "PRAGMA user_version = 67;").empty(), "Could not rewind to v67");
    {
        const auto again = SqliteDatabase::open(workspace.databasePath);
        require(again.success(), ("Step 68 must be repeatable over its own table: "
            + (again.success() ? std::string() : again.error()->message)).c_str());
        SqliteRunWarningRepository warnings(again.value());
        require(warnings.listWarnings(runId).value().size() == 2, "Step 68 must not touch existing warnings");
    }
    require(schemaVersionIs(workspace.databasePath, kLatestSchemaVersion).empty(), "...and ends at the latest schema");

    // A v67 database made before the step existed: no table at all. Its Run and artifacts are untouched.
    require(rawSql(workspace.databasePath, "DROP TABLE run_warnings; PRAGMA user_version = 67;").empty(),
        "Could not rewind to the v67 shape");
    const auto runsBefore = rawSql(workspace.databasePath, "SELECT count(*) FROM runs;");
    require(runsBefore.empty(), "setup: the runs table is readable");
    const auto migrated = SqliteDatabase::open(workspace.databasePath);
    require(migrated.success(), ("A v67 database did not migrate to v68: "
        + (migrated.success() ? std::string() : migrated.error()->message)).c_str());
    require(schemaVersionIs(workspace.databasePath, kLatestSchemaVersion).empty(), "A v67 database must end at v68");
    require(sqlValueIs(workspace.databasePath, "SELECT count(*) FROM run_warnings", 0).empty(),
        "The step creates an empty table");
    require(sqlValueIs(workspace.databasePath, "SELECT count(*) FROM runs WHERE id = '" + runId + "'", 1).empty(),
        "The step must not touch the Runs");
    // The identity index is there (the same failure cannot be stored twice) and the Run is protected from deletion.
    require(sqlValueIs(workspace.databasePath,
                "SELECT count(*) FROM sqlite_master WHERE name = 'run_warnings_identity_idx' AND sql LIKE '%UNIQUE%'", 1)
                .empty(),
        "The identity index of the warnings must be UNIQUE");
    SqliteRunWarningRepository warnings(migrated.value());
    RunWarning warning;
    warning.id = "w-1";
    warning.runId = runId;
    warning.code = kRunWarningDiagnosticArtifactLostCode;
    warning.message = "lost";
    warning.artifactName = "x.md";
    warning.createdAt = std::chrono::system_clock::now();
    require(warnings.recordWarning(warning).success(), "The migrated table must accept a warning");
    require(sqlValueIs(workspace.databasePath,
                "SELECT count(*) FROM pragma_foreign_key_list('run_warnings') "
                "WHERE \"table\" = 'runs' AND on_delete = 'RESTRICT' AND on_update = 'RESTRICT'", 1)
                .empty(),
        "A Run that has warnings must be neither deletable nor renamable (RESTRICT, like the other evidence tables)");
}

// AG-17 gpt-6-sol checkpoint M2a (review fix): the tier invariant - threshold 0 means flat so every
// long price must be 0; threshold > 0 must be strictly below the context window - is enforced by the
// v59 migration for EVERY existing row (before the version is raised, never by silently zeroing a
// stored price) and by every Model read, not only by the catalog service.
void testModelLongContextInvariantIsEnforcedByTheMigrationAndEveryRead()
{
    const LongContextPricing tier {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};
    struct Fixture {
        ProviderId providerId;
        ModelId tieredId;
        ModelId flatId;
    };
    const auto seed = [&](const TemporaryPersistenceWorkspace& workspace) {
        Fixture fixture;
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the invariant fixture");
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        ProviderCatalogService catalog(providers, models);
        const auto provider = catalog.createProvider(
            {"Tier Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
        require(provider.success(), "Could not create the tier provider");
        fixture.providerId = provider.value();
        CreateModelRequest request;
        request.providerId = fixture.providerId;
        request.apiModelId = "flat-model";
        request.displayName = "Flat";
        request.contextWindowTokens = 400'000;
        request.maxOutputTokens = 128'000;
        request.inputPriceMicrosPerMillionTokens = 1'750'000;
        const auto flat = catalog.createModel(request);
        require(flat.success(), "Could not create the flat model");
        fixture.flatId = flat.value();
        request.apiModelId = "tiered-model";
        request.displayName = "Tiered";
        request.contextWindowTokens = 1'050'000;
        request.longContextPricing = tier;
        const auto tiered = catalog.createModel(request);
        require(tiered.success(), "Could not create the tiered model");
        fixture.tieredId = tiered.value();
        return fixture;
    };
    const auto refusedWithCode = [](const fs::path& path, const char* code, const char* what) {
        const auto refused = SqliteDatabase::open(path);
        require(!refused.success() && refused.error()->code == code, what);
        return refused.error()->message;
    };
    const char* longPriceColumns =
        "long_context_input_price_micros_per_million_tokens = 0, "
        "long_context_output_price_micros_per_million_tokens = 0, "
        "long_context_cache_write_price_micros_per_million_tokens = 0, "
        "long_context_cache_read_price_micros_per_million_tokens = 0";

    // 1. A partially migrated database lost the THRESHOLD column but kept non-zero long prices.
    //    Re-adding the column with DEFAULT 0 would turn them into forbidden "prices without a
    //    threshold" - read as flat pricing, an understated cost - so the migration must refuse.
    {
        TemporaryPersistenceWorkspace workspace;
        seed(workspace);
        require(rawSql(workspace.databasePath,
                    "ALTER TABLE models DROP COLUMN long_context_threshold_input_tokens;"
                    "PRAGMA user_version = 58;").empty(),
            "Could not simulate a database that lost only the threshold column");
        const std::string message = refusedWithCode(workspace.databasePath, "migration.v59_failed",
            "The v59 migration must refuse long prices that lost their threshold");
        require(message.find("long-context price tier") != std::string::npos
                && message.find("repair it and reopen") != std::string::npos,
            "The refusal must say what to do");
        // Nothing was changed: still v58, the threshold column is still missing, the prices are
        // untouched (never zeroed silently), and a second attempt is refused the same way.
        require(schemaVersionIs(workspace.databasePath, 58).empty(),
            "A refused migration must not raise the schema version");
        require(!rawSql(workspace.databasePath,
                    "SELECT long_context_threshold_input_tokens FROM models;").empty(),
            "A refused migration must roll its added columns back");
        require(sqlValueIs(workspace.databasePath,
                    "SELECT long_context_input_price_micros_per_million_tokens FROM models "
                    "WHERE api_model_id = 'tiered-model'", 4'000'000).empty(),
            "A refused migration changed a stored price");
        refusedWithCode(workspace.databasePath, "migration.v59_failed",
            "A second attempt must be refused as well");
        // The operator's explicit repair (clear the orphan prices) lets it complete.
        require(rawSql(workspace.databasePath,
                    (std::string("UPDATE models SET ") + longPriceColumns + ";").c_str()).empty(),
            "Could not repair the orphan prices");
        require(SqliteDatabase::open(workspace.databasePath).success(),
            "A repaired database must migrate");
        require(schemaVersionIs(workspace.databasePath, kLatestSchemaVersion).empty(),
            "The repaired database did not reach the latest schema");
    }

    // 2. The threshold column survived, but its value is not strictly below the context window.
    {
        TemporaryPersistenceWorkspace workspace;
        seed(workspace);
        require(rawSql(workspace.databasePath,
                    "UPDATE models SET long_context_threshold_input_tokens = context_window_tokens "
                    "WHERE api_model_id = 'tiered-model'; PRAGMA user_version = 58;").empty(),
            "Could not simulate an unreachable threshold");
        refusedWithCode(workspace.databasePath, "migration.v59_failed",
            "The v59 migration must refuse a threshold that the window cannot reach");
        require(schemaVersionIs(workspace.databasePath, 58).empty(),
            "A refused migration must not raise the schema version");
        require(sqlValueIs(workspace.databasePath,
                    "SELECT long_context_threshold_input_tokens FROM models "
                    "WHERE api_model_id = 'tiered-model'", 1'050'000).empty(),
            "A refused migration changed the stored threshold");
        require(rawSql(workspace.databasePath,
                    "UPDATE models SET long_context_threshold_input_tokens = 272000 "
                    "WHERE api_model_id = 'tiered-model';").empty(),
            "Could not repair the threshold");
        require(SqliteDatabase::open(workspace.databasePath).success(),
            "A repaired database must migrate");
        require(schemaVersionIs(workspace.databasePath, kLatestSchemaVersion).empty(),
            "The repaired database did not reach the latest schema");
    }

    // 3. A flat model row that is ALSO consistent is fine: only offending rows refuse the migration.
    {
        TemporaryPersistenceWorkspace workspace;
        seed(workspace);
        require(rawSql(workspace.databasePath,
                    "ALTER TABLE models DROP COLUMN long_context_threshold_input_tokens;"
                    "ALTER TABLE models DROP COLUMN long_context_cache_read_price_micros_per_million_tokens;"
                    "UPDATE models SET long_context_input_price_micros_per_million_tokens = 0, "
                    "long_context_output_price_micros_per_million_tokens = 0, "
                    "long_context_cache_write_price_micros_per_million_tokens = 0;"
                    "PRAGMA user_version = 58;").empty(),
            "Could not simulate a partial database with no stored prices");
        require(SqliteDatabase::open(workspace.databasePath).success(),
            "A partial database whose rows are consistent must migrate");
        require(schemaVersionIs(workspace.databasePath, kLatestSchemaVersion).empty(),
            "It must reach the latest schema");
    }

    // 3b. A negative stored value (a database whose long-context column was created WITHOUT the
    //     CHECK) refuses the migration, and - once the database is marked v59 - every read of that row.
    {
        TemporaryPersistenceWorkspace workspace;
        const Fixture fixture = seed(workspace);
        require(rawSql(workspace.databasePath,
                    "ALTER TABLE models DROP COLUMN long_context_input_price_micros_per_million_tokens;"
                    "ALTER TABLE models ADD COLUMN long_context_input_price_micros_per_million_tokens "
                    "INTEGER NOT NULL DEFAULT 0;"
                    "UPDATE models SET long_context_input_price_micros_per_million_tokens = -1 "
                    "WHERE api_model_id = 'tiered-model';"
                    "PRAGMA user_version = 58;").empty(),
            "Could not simulate a database with an unchecked negative long-context price");
        const std::string message = refusedWithCode(workspace.databasePath, "migration.v59_failed",
            "The v59 migration must refuse a negative long-context value");
        require(message.find("negative value") != std::string::npos,
            "The refusal must name the negative value");
        require(schemaVersionIs(workspace.databasePath, 58).empty(),
            "A refused migration must not raise the schema version");
        require(rawSql(workspace.databasePath, "PRAGMA user_version = 59;").empty(),
            "Could not mark the fixture as v59");
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "A v59 database must open even with an invalid row");
        SqliteModelRepository models(opened.value());
        const auto read = models.findById(fixture.tieredId);
        require(!read.success() && read.error()->code == "persistence.invalid_data",
            "A negative stored value must not be readable");
    }

    // 4. READS: a v59 row that is valid SQL (every CHECK passes) but logically forbidden is never
    //    handed out - the Model read fails closed, one row at a time.
    {
        TemporaryPersistenceWorkspace workspace;
        const Fixture fixture = seed(workspace);
        const auto readTiered = [&] {
            const auto opened = SqliteDatabase::open(workspace.databasePath);
            require(opened.success(), "A v59 database must open even with an invalid row");
            SqliteModelRepository models(opened.value());
            return models.findById(fixture.tieredId);
        };
        require(readTiered().success() && readTiered().value()->longContextPricing == tier,
            "The fixture must start valid");

        // 4a. Long prices without a threshold (reads as flat = understated cost).
        require(rawSql(workspace.databasePath,
                    "UPDATE models SET long_context_threshold_input_tokens = 0 "
                    "WHERE api_model_id = 'tiered-model';").empty(),
            "Could not corrupt the threshold");
        {
            const auto read = readTiered();
            require(!read.success() && read.error()->code == "persistence.invalid_data",
                "Long prices without a threshold must not be readable");
            const auto opened = SqliteDatabase::open(workspace.databasePath);
            SqliteModelRepository models(opened.value());
            const auto listed = models.listAll(fixture.providerId);
            require(!listed.success() && listed.error()->code == "persistence.invalid_data",
                "listAll must fail closed too");
            const auto flat = models.findById(fixture.flatId);
            require(flat.success() && flat.value().has_value()
                    && flat.value()->longContextPricing == LongContextPricing {},
                "Only the offending row may fail");
        }
        // 4b. A threshold the window cannot reach.
        require(rawSql(workspace.databasePath,
                    "UPDATE models SET long_context_threshold_input_tokens = context_window_tokens "
                    "WHERE api_model_id = 'tiered-model';").empty(),
            "Could not corrupt the threshold again");
        {
            const auto read = readTiered();
            require(!read.success() && read.error()->code == "persistence.invalid_data",
                "An unreachable threshold must not be readable");
        }
        // 4c. One token below the window is valid again: the repair path works.
        require(rawSql(workspace.databasePath,
                    "UPDATE models SET long_context_threshold_input_tokens = context_window_tokens - 1 "
                    "WHERE api_model_id = 'tiered-model';").empty(),
            "Could not repair the threshold");
        const auto repaired = readTiered();
        require(repaired.success() && repaired.value()->longContextPricing.thresholdInputTokens == 1'049'999
                && repaired.value()->longContextPricing.inputPriceMicrosPerMillionTokens == 4'000'000,
            "A repaired row must read back with its stored prices");
    }
}

// AG-17 gpt-6-sol checkpoint M2a (SQLite): the long-context price tier persists through each of
// its five columns, survives a reopen, migrates from v58 additively and idempotently (fully or
// partially rewound), keeps every existing flat price, and the database itself refuses a
// negative value.
void testModelLongContextPricingPersistsAndMigratesV58ToV59()
{
    TemporaryPersistenceWorkspace workspace;
    // A distinct value per column, so a swapped binding or column cannot pass.
    const LongContextPricing tier {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};
    ProviderId providerId;
    ModelId tieredId;
    ModelId flatId;

    const auto readModel = [&](const ModelId& id) {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not reopen the long-context fixture");
        SqliteModelRepository models(opened.value());
        const auto found = models.findById(id);
        require(found.success() && found.value().has_value(), "A fixture model disappeared");
        return *found.value();
    };

    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the long-context fixture");
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        ProviderCatalogService catalog(providers, models);
        const auto provider = catalog.createProvider(
            {"Tier Provider", "openai", "https://api.openai.invalid", true, std::nullopt});
        require(provider.success(), "Could not create the tier provider");
        providerId = provider.value();

        CreateModelRequest request;
        request.providerId = providerId;
        request.apiModelId = "flat-model";
        request.displayName = "Flat";
        request.contextWindowTokens = 400'000;
        request.maxOutputTokens = 128'000;
        request.inputPriceMicrosPerMillionTokens = 1'750'000;
        request.outputPriceMicrosPerMillionTokens = 14'000'000;
        request.cacheWritePriceMicrosPerMillionTokens = 1'900'000;
        request.cacheReadPriceMicrosPerMillionTokens = 175'000;
        const auto flat = catalog.createModel(request);
        require(flat.success(), "Could not create the flat model");
        flatId = flat.value();

        request.apiModelId = "tiered-model";
        request.displayName = "Tiered";
        request.contextWindowTokens = 1'050'000;
        request.inputPriceMicrosPerMillionTokens = 2'000'000;
        request.outputPriceMicrosPerMillionTokens = 10'000'000;
        request.cacheWritePriceMicrosPerMillionTokens = 2'500'000;
        request.cacheReadPriceMicrosPerMillionTokens = 200'000;
        request.longContextPricing = tier;
        const auto tiered = catalog.createModel(request);
        require(tiered.success(), "Could not create the tiered model");
        tieredId = tiered.value();
    }

    // Reopened: every column round-trips, a plain model stays flat, flat prices are untouched.
    {
        const Model tiered = readModel(tieredId);
        require(tiered.longContextPricing == tier
                && tiered.longContextPricing.thresholdInputTokens == 272'000
                && tiered.longContextPricing.inputPriceMicrosPerMillionTokens == 4'000'000
                && tiered.longContextPricing.outputPriceMicrosPerMillionTokens == 15'000'000
                && tiered.longContextPricing.cacheWritePriceMicrosPerMillionTokens == 5'000'000
                && tiered.longContextPricing.cacheReadPriceMicrosPerMillionTokens == 400'000,
            "The long-context tier did not round-trip through SQLite column by column");
        require(tiered.inputPriceMicrosPerMillionTokens == 2'000'000
                && tiered.cacheReadPriceMicrosPerMillionTokens == 200'000,
            "Storing a tier changed the flat prices");
        const Model flat = readModel(flatId);
        require(flat.longContextPricing == LongContextPricing {}
                && flat.inputPriceMicrosPerMillionTokens == 1'750'000,
            "A model without a tier must stay flat");
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        SqliteModelRepository models(opened.value());
        const auto listed = models.listAll(providerId);
        require(listed.success() && listed.value().size() == 2,
            "The catalog listing must read the tier columns too");
        for (const Model& model : listed.value()) {
            require(model.longContextPricing == (model.id == tieredId ? tier : LongContextPricing {}),
                "listAll returned a different tier than findById");
        }
    }

    // UPDATE writes every column: a blind edit keeps the tier, a replacement changes all five.
    const LongContextPricing replacement {500'000, 8'000'000, 20'000'000, 9'000'000, 700'000};
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        ProviderCatalogService catalog(providers, models);
        require(catalog.updateModel({tieredId, "tiered-model", "Tiered", 1'050'000, 128'000,
                                     2'100'000, 10'500'000, 2'600'000, 210'000}).success(),
            "A price edit that does not mention the tier must succeed");
        require(readModel(tieredId).longContextPricing == tier,
            "A blind price edit wiped the stored tier");
        UpdateModelRequest replace {tieredId, "tiered-model", "Tiered", 1'050'000, 128'000,
                                    2'100'000, 10'500'000, 2'600'000, 210'000, replacement};
        require(catalog.updateModel(replace).success(), "Could not replace the tier");
        require(readModel(tieredId).longContextPricing == replacement,
            "UPDATE did not write all five long-context columns");
        replace.longContextPricing = tier;
        require(catalog.updateModel(replace).success(), "Could not restore the tier");
    }
    require(readModel(tieredId).longContextPricing == tier, "The restored tier was lost");

    const std::string rewindAll =
        "ALTER TABLE models DROP COLUMN long_context_threshold_input_tokens;"
        "ALTER TABLE models DROP COLUMN long_context_input_price_micros_per_million_tokens;"
        "ALTER TABLE models DROP COLUMN long_context_output_price_micros_per_million_tokens;"
        "ALTER TABLE models DROP COLUMN long_context_cache_write_price_micros_per_million_tokens;"
        "ALTER TABLE models DROP COLUMN long_context_cache_read_price_micros_per_million_tokens;"
        "PRAGMA user_version = 58;";

    // v58 -> v59: the columns come back, every row keeps its flat prices and becomes flat.
    require(rawSql(workspace.databasePath, rewindAll.c_str()).empty(),
        "Could not rewind the fixture to schema v58");
    require(!rawSql(workspace.databasePath,
                "SELECT long_context_threshold_input_tokens FROM models;").empty(),
        "The rewind must really have removed the tier columns");
    {
        const auto migrated = SqliteDatabase::open(workspace.databasePath);
        require(migrated.success(), "A v58 database did not migrate to v59");
    }
    // The migration recorded the new version (an integer-overflow error is the failure signal).
    require(rawSql(workspace.databasePath,
                "SELECT CASE WHEN (SELECT user_version FROM pragma_user_version) >= 59 "
                "THEN 1 ELSE abs(-9223372036854775808) END;").empty(),
        "The v59 migration did not record schema version 59");
    require(rawSql(workspace.databasePath,
                "SELECT long_context_threshold_input_tokens, "
                "long_context_input_price_micros_per_million_tokens, "
                "long_context_output_price_micros_per_million_tokens, "
                "long_context_cache_write_price_micros_per_million_tokens, "
                "long_context_cache_read_price_micros_per_million_tokens FROM models;").empty(),
        "The migration did not add all five tier columns");
    {
        const Model tiered = readModel(tieredId);
        require(tiered.longContextPricing == LongContextPricing {},
            "A migrated row must be flat: its tier was never stored in v58");
        require(tiered.apiModelId == "tiered-model" && tiered.contextWindowTokens == 1'050'000
                && tiered.inputPriceMicrosPerMillionTokens == 2'100'000
                && tiered.cacheWritePriceMicrosPerMillionTokens == 2'600'000,
            "The v58 -> v59 migration changed a flat model field");
        require(readModel(flatId).inputPriceMicrosPerMillionTokens == 1'750'000,
            "The migration changed another model's flat price");
    }
    {   // The migrated columns really store values.
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        ProviderCatalogService catalog(providers, models);
        require(catalog.updateModel({tieredId, "tiered-model", "Tiered", 1'050'000, 128'000,
                                     2'100'000, 10'500'000, 2'600'000, 210'000, tier}).success(),
            "Could not store a tier in a migrated database");
    }
    require(readModel(tieredId).longContextPricing == tier,
        "A migrated database did not persist a tier");

    // Idempotent: a database already at the columns but marked v58 reopens, nothing is lost.
    require(rawSql(workspace.databasePath, "PRAGMA user_version = 58;").empty(),
        "Could not mark the fixture as v58");
    {
        const auto again = SqliteDatabase::open(workspace.databasePath);
        require(again.success(), "Re-running the v59 migration over existing columns must be a no-op");
    }
    require(readModel(tieredId).longContextPricing == tier,
        "Re-running the migration lost a stored tier");

    // Partial: only the missing columns are added; the surviving ones keep their values.
    require(rawSql(workspace.databasePath,
                "ALTER TABLE models DROP COLUMN long_context_output_price_micros_per_million_tokens;"
                "ALTER TABLE models DROP COLUMN long_context_cache_read_price_micros_per_million_tokens;"
                "PRAGMA user_version = 58;").empty(),
        "Could not partially rewind the fixture");
    {
        const auto partial = SqliteDatabase::open(workspace.databasePath);
        require(partial.success(), "A partially migrated database did not complete the v59 migration");
    }
    {
        const Model tiered = readModel(tieredId);
        require(tiered.longContextPricing.thresholdInputTokens == 272'000
                && tiered.longContextPricing.inputPriceMicrosPerMillionTokens == 4'000'000
                && tiered.longContextPricing.cacheWritePriceMicrosPerMillionTokens == 5'000'000
                && tiered.longContextPricing.outputPriceMicrosPerMillionTokens == 0
                && tiered.longContextPricing.cacheReadPriceMicrosPerMillionTokens == 0,
            "A partial migration must keep the surviving columns and zero only the re-added ones");
    }

    // The database itself refuses a negative value in any tier column.
    for (const char* column : {
             "long_context_threshold_input_tokens",
             "long_context_input_price_micros_per_million_tokens",
             "long_context_output_price_micros_per_million_tokens",
             "long_context_cache_write_price_micros_per_million_tokens",
             "long_context_cache_read_price_micros_per_million_tokens"}) {
        const std::string sql = std::string("UPDATE models SET ") + column + " = -1;";
        require(!rawSql(workspace.databasePath, sql.c_str()).empty(),
            "SQLite accepted a negative long-context value");
    }
}

void testV64ToV65CacheDurationMigrationAndRoundTrip()
{
    TemporaryPersistenceWorkspace workspace;
    const auto now = std::chrono::system_clock::now();
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "Could not create the cache-duration fixture");
        SqliteProjectRepository projects(opened.value());
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteRunRepository runs(opened.value());
        SqliteModelExecutionRepository executions(opened.value());

        Project project {"duration-project", "Duration project", "", workspace.projectRoot,
            ProjectStatus::Active, now, now};
        require(projects.insert(project).success(), "Could not seed duration project");
        Provider provider {"duration-provider", "Anthropic", "anthropic", "https://example.invalid",
            true, std::nullopt, now, now};
        require(providers.insert(provider).success(), "Could not seed duration provider");
        Model model {"duration-model", provider.id, "claude-duration", "Claude duration",
            200'000, 16'384, 3'000'000, 15'000'000, 3'750'000, 300'000,
            true, true, now, now,
            LongContextPricing {100'000, 6'000'000, 22'500'000, 7'500'000, 600'000}};
        require(models.insert(model).success(), "Could not seed duration model");
        Run run;
        run.id = "duration-run";
        run.projectId = project.id;
        run.title = "Legacy duration run";
        run.task = "Preserve aggregate cache usage";
        run.workflowId = kExecutionManifestWorkflowId;
        run.usage.inputTokens = 100;
        run.usage.cacheCreationInputTokens = 60;
        run.usage.cacheReadInputTokens = 20;
        run.createdAt = now;
        require(runs.insertRun(run).success(), "Could not seed duration run");
        ModelExecution execution;
        execution.id = "duration-execution";
        execution.source = ModelExecutionSource::Run;
        execution.sourceId = run.id;
        execution.projectId = project.id;
        execution.providerId = provider.id;
        execution.modelId = model.id;
        execution.role = "worker";
        execution.status = ModelExecutionStatus::Completed;
        execution.inputTokens = 100;
        execution.outputTokens = 10;
        execution.createdAt = now;
        execution.startedAt = now;
        execution.finishedAt = now;
        require(executions.insertExecution(execution).success(), "Could not seed duration execution");
    }

    const char* rewind = R"sql(
DROP TRIGGER runs_cache_duration_validate_insert;
DROP TRIGGER runs_cache_duration_validate_update;
DROP TRIGGER model_executions_cache_duration_validate_insert;
DROP TRIGGER model_executions_cache_duration_validate_update;
ALTER TABLE runs DROP COLUMN cache_creation_duration_partition_known;
ALTER TABLE runs DROP COLUMN cache_creation_five_minute_input_tokens;
ALTER TABLE runs DROP COLUMN cache_creation_one_hour_input_tokens;
ALTER TABLE model_executions DROP COLUMN cache_creation_input_tokens;
ALTER TABLE model_executions DROP COLUMN cache_read_input_tokens;
ALTER TABLE model_executions DROP COLUMN cache_creation_duration_partition_known;
ALTER TABLE model_executions DROP COLUMN cache_creation_five_minute_input_tokens;
ALTER TABLE model_executions DROP COLUMN cache_creation_one_hour_input_tokens;
ALTER TABLE models DROP COLUMN cache_write_one_hour_price_micros_per_million_tokens;
ALTER TABLE models DROP COLUMN long_context_cache_write_one_hour_price_micros_per_million_tokens;
PRAGMA user_version = 64;
)sql";
    require(rawSql(workspace.databasePath, rewind).empty(), "Could not build a real v64 fixture");

    {
        const auto migrated = SqliteDatabase::open(workspace.databasePath);
        require(migrated.success(), "Could not migrate the v64 cache-duration fixture");
        SqliteModelRepository models(migrated.value());
        SqliteRunRepository runs(migrated.value());
        SqliteModelExecutionRepository executions(migrated.value());
        const auto model = models.findById("duration-model");
        const auto run = runs.findRun("duration-run");
        const auto execution = executions.findExecution("duration-execution");
        require(model.success() && model.value().has_value()
                && model.value()->cacheWritePriceMicrosPerMillionTokens == 3'750'000
                && model.value()->longContextPricing.cacheWritePriceMicrosPerMillionTokens == 7'500'000
                && model.value()->cacheWriteOneHourPriceMicrosPerMillionTokens == 0
                && model.value()->longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens == 0,
            "v65 changed old prices or invented one-hour prices");
        require(run.success() && run.value().has_value()
                && run.value()->usage.cacheCreationInputTokens == 60
                && !run.value()->usage.cacheCreationDuration.known
                && run.value()->usage.cacheCreationDuration.fiveMinuteTokens == 0
                && run.value()->usage.cacheCreationDuration.oneHourTokens == 0,
            "v65 did not preserve an old Run as explicitly unpartitioned");
        require(execution.success() && execution.value().has_value()
                && execution.value()->cacheCreationInputTokens == 0
                && !execution.value()->cacheCreationDuration.known,
            "v65 invented detailed usage for an old model execution");

        Model updatedModel = *model.value();
        updatedModel.cacheWriteOneHourPriceMicrosPerMillionTokens = 6'000'000;
        updatedModel.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens = 12'000'000;
        require(models.update(updatedModel).success(), "Could not store one-hour prices");
        Run updatedRun = *run.value();
        updatedRun.usage.cacheCreationDuration = {true, 20, 40};
        require(runs.updateRun(updatedRun).success(), "Could not store Run duration usage");
        ModelExecution updatedExecution = *execution.value();
        updatedExecution.cacheCreationInputTokens = 30;
        updatedExecution.cacheReadInputTokens = 20;
        updatedExecution.cacheCreationDuration = {true, 10, 20};
        require(executions.updateExecution(updatedExecution).success(),
            "Could not store model-execution duration usage");
    }

    require(!rawSql(workspace.databasePath,
                 "UPDATE runs SET cache_creation_one_hour_input_tokens=39 "
                 "WHERE id='duration-run';")
                 .empty(),
        "Run duration trigger accepted an incomplete partition");
    require(!rawSql(workspace.databasePath,
                 "UPDATE model_executions SET cache_creation_duration_partition_known=0 "
                 "WHERE id='duration-execution';")
                 .empty(),
        "Model-execution duration trigger accepted hidden detail");
    require(!rawSql(workspace.databasePath,
                 "UPDATE models SET cache_write_one_hour_price_micros_per_million_tokens=-1 "
                 "WHERE id='duration-model';")
                 .empty(),
        "SQLite accepted a negative flat one-hour cache price");
    require(!rawSql(workspace.databasePath,
                 "UPDATE models SET long_context_cache_write_one_hour_price_micros_per_million_tokens=-1 "
                 "WHERE id='duration-model';")
                 .empty(),
        "SQLite accepted a negative long-context one-hour cache price");

    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    require(reopened.success(), "The v65 database did not reopen");
    SqliteModelRepository models(reopened.value());
    SqliteRunRepository runs(reopened.value());
    SqliteModelExecutionRepository executions(reopened.value());
    const auto model = models.findById("duration-model");
    const auto run = runs.findRun("duration-run");
    const auto execution = executions.findExecution("duration-execution");
    require(model.success() && model.value()->cacheWriteOneHourPriceMicrosPerMillionTokens == 6'000'000
            && model.value()->longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens == 12'000'000,
        "One-hour prices did not survive reopen");
    require(run.success() && run.value()->usage.cacheCreationDuration.known
            && run.value()->usage.cacheCreationDuration.fiveMinuteTokens == 20
            && run.value()->usage.cacheCreationDuration.oneHourTokens == 40,
        "Run duration usage did not survive reopen");
    require(execution.success() && execution.value()->cacheCreationDuration.known
            && execution.value()->cacheCreationDuration.fiveMinuteTokens == 10
            && execution.value()->cacheCreationDuration.oneHourTokens == 20,
        "Model-execution duration usage did not survive reopen");

    require(rawSql(workspace.databasePath,
                "UPDATE runs SET input_tokens=10,cache_creation_input_tokens=8,"
                "cache_read_input_tokens=8,cache_creation_duration_partition_known=1,"
                "cache_creation_five_minute_input_tokens=3,"
                "cache_creation_one_hour_input_tokens=5 WHERE id='duration-run';")
                .empty(),
        "Could not write the corrupt Run cache-breakdown fixture");
    const auto corruptRunBreakdown = runs.findRun("duration-run");
    require(!corruptRunBreakdown.success()
            && corruptRunBreakdown.error()->code == "persistence.invalid_data",
        "A raw Run row whose cache subsets exceed input was accepted");

    // A partially applied v65 may already have the new price columns while
    // still advertising v64. Never raise its version if the newly introduced
    // long-context one-hour rate would be silently ignored without a tier.
    TemporaryPersistenceWorkspace partial;
    {
        const auto opened = SqliteDatabase::open(partial.databasePath);
        require(opened.success(), "Could not create partial-v65 price fixture");
        SqliteProviderRepository partialProviders(opened.value());
        SqliteModelRepository partialModels(opened.value());
        Provider provider {"partial-duration-provider", "Anthropic", "anthropic",
            "https://example.invalid", true, std::nullopt, now, now};
        require(partialProviders.insert(provider).success(),
            "Could not seed partial-v65 provider");
        Model flat {"partial-duration-model", provider.id, "claude-flat", "Claude flat",
            200'000, 16'384, 3'000'000, 15'000'000, 3'750'000, 300'000,
            true, false, now, now};
        require(partialModels.insert(flat).success(), "Could not seed partial-v65 model");
    }
    require(rawSql(partial.databasePath,
                "UPDATE models SET long_context_cache_write_one_hour_price_micros_per_million_tokens=1 "
                "WHERE id='partial-duration-model'; PRAGMA user_version=64;")
                .empty(),
        "Could not create partial-v65 invalid one-hour price");
    const auto refusedOpen = SqliteDatabase::open(partial.databasePath);
    require(!refusedOpen.success() && refusedOpen.error()->code == "migration.v65_failed",
        "v65 must refuse a long-context one-hour price without a threshold");
    const std::string refused = refusedOpen.error()->message;
    require(refused.find("require a long-context threshold") != std::string::npos,
        "v65 refusal did not explain the new one-hour price invariant");
    require(schemaVersionIs(partial.databasePath, 64).empty(),
        "A refused v65 migration raised the schema version");

    // Removing the blocker must let the very same database upgrade: the refused
    // step left nothing behind, and the seeded rows survive the retry.
    require(rawSql(partial.databasePath,
                "UPDATE models SET long_context_cache_write_one_hour_price_micros_per_million_tokens=0 "
                "WHERE id='partial-duration-model';")
                .empty(),
        "Could not remove the v65 blocker from the fixture");
    const auto recovered = SqliteDatabase::open(partial.databasePath);
    require(recovered.success(), "The v65 database did not upgrade after its blocker was removed");
    SqliteModelRepository recoveredModels(recovered.value());
    const auto recoveredModel = recoveredModels.findById("partial-duration-model");
    require(recoveredModel.success() && recoveredModel.value().has_value()
            && recoveredModel.value()->providerId == "partial-duration-provider",
        "The seeded model did not survive the recovered upgrade");
    require(schemaVersionIs(partial.databasePath, 70).empty(),
        "The recovered database did not reach the current schema version");
}

} // namespace

int runSqliteRepositoryTests()
{
    testMigrationRejectsNewerSchema();
    testNewerSchemaRefusalPreservesFutureMarkerAndRows();
    testOpenRefusesDatabaseUnderNonDirectoryParent();
    testUnicodeDatabaseLocationRoundTripsAndReopens();
    testSharedRunRepositoryContracts();
    testRepositoriesPersistAndReopen();
    testLongContextPersistsReopensAndReachesWorkflowRunner();
    testRecoveryUsesPersistedUnfinishedRows();
    testArtifactRepositoryAndFileStorePersistAndReopen();
    testWorkspaceProviderModelProfilePersistAndReopen();
    testV2ToV3MigrationPreservesExistingData();
    testV42ToV43ProviderBudgetMigrationDefaultsAndRestart();
    testV43ToV44CacheTokenMigrationDefaultsAndRestart();
    testTurnLimitExtensionFieldsRoundTripAndLegacyShapesAreReadByTheirLimit();
    testV59ToV60DraftProviderBudgetMigrationDefaultsAndRestart();
    testV60ToV61InterfaceContractsMigrationRoundTripGuardAndDamage();
    testSchemaV23WorkspaceAccessDefaultsToFalseForRowsWithoutIt();
    testTaskRepositoryPersistsAndReopens();
    testChatProjectContextAttachmentIsAtomicImmutableAndRoundTrips();
    testChatMemorySegmentsPersistOrderCascadeAndMessageIsolation();
    testProjectMemorySegmentsPersistOrderCascadeAndProjectIsolation();
    testLegacyConversionRecordSqliteSpecifics();
    testV57ToV58ConversionTableMigration();
    testRunWarningsPersistDeduplicateRejectBadRowsAndSurviveReopen();
    testV67ToV68RunWarningsMigrationIsAdditiveAndRepeatable();
    testModelLongContextPricingPersistsAndMigratesV58ToV59();
    testV64ToV65CacheDurationMigrationAndRoundTrip();
    testModelLongContextInvariantIsEnforcedByTheMigrationAndEveryRead();
    return 0;
}
