#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

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
        / ("ai-orchestrator-manifest-task-exec-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }
};

// Idempotent: safe to call more than once against the same database (a test
// exercising several Runs shares one Project rather than colliding on a
// duplicate project id).
void seedProjectOnce(const std::shared_ptr<SqliteDatabase>& database)
{
    const auto now = std::chrono::system_clock::now();
    SqliteProjectRepository projects(database);
    const auto existing = projects.findById("manifest-task-exec-project");
    if (existing.success() && existing.value().has_value()) return;
    Project project {"manifest-task-exec-project", "Manifest task execution project", "Phase 5",
        database->path().parent_path() / "project", ProjectStatus::Active, now, now};
    require(projects.insert(project).success(), "Could not seed project");
}

void seedRun(const std::shared_ptr<SqliteDatabase>& database, const RunId& runId)
{
    seedProjectOnce(database);
    const auto now = std::chrono::system_clock::now();
    SqliteRunRepository runs(database);
    Run run;
    run.id = runId;
    run.projectId = "manifest-task-exec-project";
    run.title = "Manifest task execution persistence";
    run.task = "Round-trip per-task execution state";
    run.workflowId = "mock-sequential-v1";
    run.createdAt = now;
    require(runs.insertRun(run).success(), "Could not seed Run");
}

void testUpsertRoundTripsAndReopens()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1");
        SqliteManifestTaskExecutionRepository repo(opened.value());

        ManifestTaskState running;
        running.status = ManifestTaskStatus::Running;
        require(repo.upsert("run-1", "task-a", running).success(), "Could not upsert running state");

        // A later upsert for the same (runId, taskId) replaces the row, not
        // append a second one - a task legitimately moves through several
        // states across its lifetime.
        ManifestTaskState completed;
        completed.status = ManifestTaskStatus::Completed;
        completed.changeSetId = "cs-1";
        completed.attempts = 2;
        completed.outputSummary = "Created notes.md";
        require(repo.upsert("run-1", "task-a", completed).success(), "Could not upsert completed state");

        ManifestTaskState failed;
        failed.status = ManifestTaskStatus::Failed;
        failed.errorCode = "proposal.outside_allowed_roots";
        failed.errorMessage = "escaped ownership";
        require(repo.upsert("run-1", "task-b", failed).success(), "Could not upsert failed state");
    }

    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestTaskExecutionRepository repo(reopened.value());
        const auto listed = repo.listForRun("run-1");
        require(listed.success(), "Could not list manifest task executions after reopen");
        require(listed.value().size() == 2, "Expected exactly one row per task, not per upsert");

        const auto& taskA = listed.value()[0];
        require(taskA.taskId == "task-a", "Unexpected task ordering (expected task-a first)");
        require(taskA.state.status == ManifestTaskStatus::Completed,
            "task-a must reflect its latest upserted state, not the first one");
        require(taskA.state.changeSetId.has_value() && *taskA.state.changeSetId == "cs-1",
            "task-a's change set id did not survive reopen");
        require(taskA.state.attempts == 2, "task-a's attempts did not survive reopen");
        require(taskA.state.outputSummary == "Created notes.md",
            "task-a's output summary did not survive reopen");

        const auto& taskB = listed.value()[1];
        require(taskB.taskId == "task-b", "Unexpected task ordering (expected task-b second)");
        require(taskB.state.status == ManifestTaskStatus::Failed, "task-b must be Failed");
        require(taskB.state.errorCode == "proposal.outside_allowed_roots",
            "task-b's error code did not survive reopen");
        require(!taskB.state.changeSetId.has_value(),
            "task-b never produced a proposal and must have no change set id");
    }
}

void testListForRunIsScopedToItsOwnRun()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-a");
    seedRun(opened.value(), "run-b");
    SqliteManifestTaskExecutionRepository repo(opened.value());

    ManifestTaskState state;
    state.status = ManifestTaskStatus::Completed;
    require(repo.upsert("run-a", "task-1", state).success(), "Could not upsert for run-a");
    require(repo.upsert("run-b", "task-1", state).success(), "Could not upsert for run-b");

    const auto listedA = repo.listForRun("run-a");
    require(listedA.success() && listedA.value().size() == 1,
        "run-a's list must not include run-b's rows");
}

// AG-04: ManifestWorkerNoChanges evidence persistence, kept in its own
// table (manifest_task_no_changes) rather than nullable columns on
// manifest_task_executions - see IManifestTaskExecutionRepository.hpp.
ManifestWorkerNoChanges makeNoChanges(const std::string& requirementId, const std::string& path)
{
    ManifestWorkerNoChanges evidence;
    evidence.reason = ManifestWorkerNoChangesReason::AlreadySatisfiedInBaseline;
    evidence.summary = "The requested output already exists with the expected content.";
    ManifestWorkerCriterionEvidence item;
    item.requirementId = requirementId;
    item.inspectedPath = path;
    item.observation = "Inspected file already contains the required content.";
    evidence.criterionEvidence.push_back(std::move(item));
    evidence.inspectedRelativePaths = {path};
    return evidence;
}

void testUpsertNoChangesRoundTripsReopensAndReplaces()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1");
        SqliteManifestTaskExecutionRepository repo(opened.value());

        require(repo.upsertNoChanges("run-1", "task-a", makeNoChanges("req-1", "notes/file.md"))
                    .success(),
            "Could not upsert no-op evidence");

        // A later upsert for the same (runId, taskId) replaces the row,
        // matching upsert()'s own semantics for ManifestTaskState - a task
        // revisiting no-op across a bounded retry must not accumulate stale
        // evidence rows.
        auto replacement = makeNoChanges("req-1", "notes/file.md");
        replacement.reason = ManifestWorkerNoChangesReason::SatisfiedByDependencyProposals;
        replacement.summary = "A dependency's proposal already covers this requirement.";
        require(repo.upsertNoChanges("run-1", "task-a", replacement).success(),
            "Could not upsert replacement no-op evidence");
    }

    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestTaskExecutionRepository repo(reopened.value());
        const auto listed = repo.listNoChangesForRun("run-1");
        require(listed.success(), "Could not list no-op evidence after reopen");
        require(listed.value().size() == 1,
            "Expected exactly one row per task, not per upsert");

        const auto& record = listed.value().front();
        require(record.taskId == "task-a", "Unexpected task id for no-op evidence");
        require(record.evidence.reason == ManifestWorkerNoChangesReason::SatisfiedByDependencyProposals,
            "No-op evidence must reflect its latest upserted reason, not the first one");
        require(record.evidence.summary == "A dependency's proposal already covers this requirement.",
            "No-op evidence summary did not survive reopen");
        require(record.evidence.criterionEvidence.size() == 1
                && record.evidence.criterionEvidence.front().requirementId == "req-1"
                && record.evidence.criterionEvidence.front().inspectedPath == "notes/file.md",
            "No-op evidence criterion evidence did not survive reopen");
        require(record.evidence.inspectedRelativePaths == std::vector<std::string> {"notes/file.md"},
            "No-op evidence inspected paths did not survive reopen");
    }
}

void testListNoChangesForRunIsScopedToItsOwnRun()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-a");
    seedRun(opened.value(), "run-b");
    SqliteManifestTaskExecutionRepository repo(opened.value());

    require(repo.upsertNoChanges("run-a", "task-1", makeNoChanges("req-1", "a.md")).success(),
        "Could not upsert no-op evidence for run-a");
    require(repo.upsertNoChanges("run-b", "task-1", makeNoChanges("req-1", "b.md")).success(),
        "Could not upsert no-op evidence for run-b");

    const auto listedA = repo.listNoChangesForRun("run-a");
    require(listedA.success() && listedA.value().size() == 1,
        "run-a's no-op evidence must not include run-b's rows");
    require(listedA.value().front().evidence.criterionEvidence.front().inspectedPath == "a.md",
        "run-a's no-op evidence must be its own, not run-b's");
}

void testNoChangesCascadeDeletesWithItsRun()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-cascade");
        seedRun(opened.value(), "run-kept");
        SqliteManifestTaskExecutionRepository repo(opened.value());
        require(repo.upsertNoChanges("run-cascade", "task-1", makeNoChanges("req-1", "a.md"))
                    .success(),
            "Could not seed no-op evidence for the run to be deleted");
        require(repo.upsertNoChanges("run-kept", "task-1", makeNoChanges("req-1", "b.md")).success(),
            "Could not seed no-op evidence for the kept run");
    }
    {
        sqlite3* handle = nullptr;
        const std::u8string path = workspace.database.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open database through native sqlite3");
        require(sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not enable foreign keys through native sqlite3");
        require(sqlite3_exec(handle, "DELETE FROM runs WHERE id='run-cascade';", nullptr, nullptr, nullptr)
                    == SQLITE_OK,
            "Could not delete run-cascade through native sqlite3");
        sqlite3_close_v2(handle);
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen after delete");
        SqliteManifestTaskExecutionRepository repo(reopened.value());
        const auto afterCascade = repo.listNoChangesForRun("run-cascade");
        require(afterCascade.success() && afterCascade.value().empty(),
            "No-op evidence was not cascade-deleted with its Run");
        const auto keptAfterCascade = repo.listNoChangesForRun("run-kept");
        require(keptAfterCascade.success() && keptAfterCascade.value().size() == 1,
            "Cascade delete affected another Run's no-op evidence");
    }
}

} // namespace

int runManifestTaskExecutionRepositoryTests()
{
    testUpsertRoundTripsAndReopens();
    testListForRunIsScopedToItsOwnRun();
    testUpsertNoChangesRoundTripsReopensAndReplaces();
    testListNoChangesForRunIsScopedToItsOwnRun();
    testNoChangesCascadeDeletesWithItsRun();
    return 0;
}
