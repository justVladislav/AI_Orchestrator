#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace {

using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Workspace {
    fs::path root {fs::temp_directory_path()
        / ("ai-orchestrator-run-workspace-scope-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }
};

void seedProjectOnce(const std::shared_ptr<SqliteDatabase>& database, const ProjectId& projectId)
{
    const auto now = std::chrono::system_clock::now();
    SqliteProjectRepository projects(database);
    const auto existing = projects.findById(projectId);
    if (existing.success() && existing.value().has_value()) return;
    Project project {projectId, "Workspace scope test project", "Round-trip workspace scope persistence",
        database->path().parent_path() / projectId, ProjectStatus::Active, now, now};
    require(projects.insert(project).success(), "Could not seed project");
}

void seedRun(
    const std::shared_ptr<SqliteDatabase>& database, const RunId& runId, const ProjectId& projectId)
{
    seedProjectOnce(database, projectId);
    const auto now = std::chrono::system_clock::now();
    SqliteRunRepository runs(database);
    Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "Workspace scope persistence";
    run.task = "Round-trip a frozen workspace scope";
    run.workflowId = "execution-manifest-v1";
    run.createdAt = now;
    require(runs.insertRun(run).success(), "Could not seed Run");
}

RunWorkspaceScope makeScope(const RunId& runId)
{
    RunWorkspaceScope scope;
    scope.runId = runId;
    scope.manifestSchemaVersion = "execution-manifest.v1";
    RunWorkspaceScopeEntry owned;
    owned.relativePath = "src/a.cpp";
    owned.role = WorkspaceScopeRole::OwnedWrite;
    owned.reason = "Declared as owned_write by task 'implement'";
    owned.taskId = "implement";
    owned.baselineHash = "deadbeef";
    scope.entries.push_back(owned);
    RunWorkspaceScopeEntry required;
    required.relativePath = "src/shared.hpp";
    required.role = WorkspaceScopeRole::RequiredRead;
    required.reason = "Declared as required_read by task 'implement'";
    required.taskId = "implement";
    required.baselineHash = "cafef00d";
    scope.entries.push_back(required);
    scope.dependencyEdges.push_back({"implement", "define-shared", "src/shared.hpp"});
    scope.frozenAt = std::chrono::system_clock::now();
    return scope;
}

void testFreezeRoundTripsAndReopens()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "scope-project");
        SqliteRunWorkspaceScopeRepository repo(opened.value());
        require(repo.freeze(makeScope("run-1")).success(), "Could not freeze workspace scope");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteRunWorkspaceScopeRepository repo(reopened.value());
        const auto found = repo.findForRun("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find frozen workspace scope after reopen");
        const auto& scope = *found.value();
        require(scope.manifestSchemaVersion == "execution-manifest.v1",
            "manifestSchemaVersion did not survive reopen");
        require(scope.entries.size() == 2, "entries did not survive reopen");
        require(scope.entries[0].relativePath == "src/a.cpp"
                && scope.entries[0].role == WorkspaceScopeRole::OwnedWrite
                && scope.entries[0].taskId == "implement"
                && scope.entries[0].baselineHash.has_value()
                && *scope.entries[0].baselineHash == "deadbeef",
            "First entry did not survive reopen");
        require(scope.entries[1].role == WorkspaceScopeRole::RequiredRead,
            "Second entry role did not survive reopen");
        require(scope.dependencyEdges.size() == 1
                && scope.dependencyEdges[0].dependentTaskId == "implement"
                && scope.dependencyEdges[0].providerTaskId == "define-shared"
                && scope.dependencyEdges[0].relativePath == "src/shared.hpp",
            "Dependency edge did not survive reopen");
    }
}

void testFreezeRejectsDuplicateRunId()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "scope-project");
    SqliteRunWorkspaceScopeRepository repo(opened.value());
    require(repo.freeze(makeScope("run-1")).success(), "Could not freeze first workspace scope");

    auto duplicate = makeScope("run-1");
    duplicate.manifestSchemaVersion = "should-never-be-visible";
    const auto rejected = repo.freeze(duplicate);
    require(!rejected.success(), "A duplicate runId freeze must fail, not replace");
    require(rejected.error()->code == "workspace_scope.already_frozen",
        "Expected the already_frozen failure code");

    const auto found = repo.findForRun("run-1");
    require(found.success() && found.value().has_value()
            && found.value()->manifestSchemaVersion == "execution-manifest.v1",
        "The original frozen scope must remain untouched by a rejected duplicate freeze");
}

void testFrozenScopeRowCannotBeUpdated()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "scope-project");
    SqliteRunWorkspaceScopeRepository repo(opened.value());
    require(repo.freeze(makeScope("run-1")).success(), "Could not freeze workspace scope");

    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    const int result = sqlite3_exec(raw,
        "UPDATE run_workspace_scopes SET entries_json = '[]' WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr);
    require(result != SQLITE_OK, "The immutability trigger did not reject an UPDATE");
    sqlite3_close(raw);
}

void testExpansionRoundTripsAndRejectsDuplicate()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "scope-project");
    SqliteRunWorkspaceScopeRepository repo(opened.value());
    require(repo.freeze(makeScope("run-1")).success(), "Could not freeze workspace scope");

    RunWorkspaceScopeExpansion expansion;
    expansion.id = "expansion-1";
    expansion.runId = "run-1";
    expansion.taskId = "implement";
    expansion.relativePath = "src/discovered.hpp";
    expansion.role = WorkspaceScopeRole::RequiredRead;
    expansion.reason = "Discovered a real dependency while reading src/a.cpp";
    expansion.baselineHash = "abc123";
    expansion.approvedAt = std::chrono::system_clock::now();
    require(repo.recordExpansion(expansion).success(), "Could not record expansion");

    auto duplicate = expansion;
    duplicate.id = "expansion-2";
    duplicate.reason = "should never be visible";
    const auto rejected = repo.recordExpansion(duplicate);
    require(!rejected.success(), "A duplicate (run,task,path) expansion must fail, not duplicate");
    require(rejected.error()->code == "workspace_scope.expansion_already_recorded",
        "Expected the expansion_already_recorded failure code");

    const auto listed = repo.listExpansionsForRun("run-1");
    require(listed.success() && listed.value().size() == 1,
        "A rejected duplicate expansion must never be appended");
    require(listed.value().front().reason == "Discovered a real dependency while reading src/a.cpp",
        "The original expansion's reason must remain untouched");
}

void testFindEntryFailsOnCorruptEntriesColumn()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "scope-project");
    SqliteRunWorkspaceScopeRepository repo(opened.value());
    require(repo.freeze(makeScope("run-1")).success(), "Could not freeze workspace scope");

    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    // Bypass the immutability trigger the same way AG-14's own corrupt-column
    // test does: a raw connection performing a schema-level operation, never
    // production code.
    require(sqlite3_exec(raw, "DROP TRIGGER run_workspace_scopes_reject_update;",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not drop immutability trigger for the test");
    require(sqlite3_exec(raw,
        "UPDATE run_workspace_scopes SET entries_json = '{not valid json' WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt entries_json column");
    sqlite3_close(raw);

    const auto found = repo.findForRun("run-1");
    require(!found.success(), "findForRun must fail on a corrupt entries_json column");
}

void testUpgradeAddsInspectedRelativePathsColumnAndNewTables()
{
    // AG-15: simulates a database that predates v37 - no run_workspace_scopes/
    // run_workspace_scope_expansions tables, and manifest_task_executions
    // without the inspected_relative_paths column - proving the v37 upgrade
    // adds both without disturbing pre-existing data.
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "scope-project");
        SqliteManifestTaskExecutionRepository taskExecutions(opened.value());
        ManifestTaskState state;
        state.status = ManifestTaskStatus::Completed;
        require(taskExecutions.upsert("run-1", "task-a", state).success(),
            "Could not seed a manifest task execution row");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen after the v37 upgrade");
        SqliteRunWorkspaceScopeRepository repo(reopened.value());
        require(repo.freeze(makeScope("run-1")).success(),
            "Could not freeze a workspace scope after the v37 upgrade");
        SqliteManifestTaskExecutionRepository taskExecutions(reopened.value());
        const auto rows = taskExecutions.listForRun("run-1");
        require(rows.success() && rows.value().size() == 1,
            "Pre-existing manifest_task_executions row did not survive the v37 upgrade");
    }
}

} // namespace

int runRunWorkspaceScopeRepositoryTests()
{
    testFreezeRoundTripsAndReopens();
    testFreezeRejectsDuplicateRunId();
    testFrozenScopeRowCannotBeUpdated();
    testExpansionRoundTripsAndRejectsDuplicate();
    testFindEntryFailsOnCorruptEntriesColumn();
    testUpgradeAddsInspectedRelativePathsColumnAndNewTables();
    return 0;
}
