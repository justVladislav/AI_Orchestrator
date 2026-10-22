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
        / ("ai-orchestrator-manifest-work-ledger-test-"
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
    Project project {projectId, "Work Ledger test project", "Round-trip Work Ledger persistence",
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
    run.title = "Work Ledger persistence";
    run.task = "Round-trip a durable ledger entry";
    run.workflowId = "execution-manifest-v1";
    run.createdAt = now;
    require(runs.insertRun(run).success(), "Could not seed Run");
}

ManifestWorkLedgerEntry makeEntry(const RunId& runId, const ProjectId& projectId)
{
    ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = projectId;
    entry.terminalStatus = RunStatus::Completed;
    entry.terminalReason = "Manifest execution completed";
    entry.baselineFingerprint = "deadbeef";
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    ManifestWorkLedgerTaskDisposition taskA;
    taskA.taskId = "task-a";
    taskA.status = "completed";
    taskA.changeSetId = "cs-1";
    entry.taskDispositions.push_back(taskA);
    ManifestWorkLedgerTaskDisposition taskB;
    taskB.taskId = "task-b";
    taskB.status = "skipped";
    taskB.noChangesReason = "already_satisfied_in_baseline";
    entry.taskDispositions.push_back(taskB);
    entry.proposalIds = {"cs-1"};
    entry.affectedRelativePaths = {"src/a.cpp", "src/b.cpp"};
    entry.verificationSummary = "passed";
    entry.remainingWorkNote = "";
    entry.planningInputFingerprint = std::string(64, 'a');
    entry.planningFailureSignature = std::string(64, 'b');
    ManifestWorkLedgerWorkspaceSnapshot snapshot;
    RunWorkspaceScopeEntry scopeEntry;
    scopeEntry.relativePath = "src/a.cpp";
    scopeEntry.role = WorkspaceScopeRole::OwnedWrite;
    scopeEntry.reason = "Declared as owned_write by task 'task-a'";
    scopeEntry.taskId = "task-a";
    scopeEntry.baselineHash = "hash-a";
    snapshot.entries.push_back(std::move(scopeEntry));
    snapshot.dependencyEdges.push_back({"task-b", "task-a", "src/a.cpp"});
    ManifestWorkLedgerTaskWorkspaceActivity activity;
    activity.taskId = "task-a";
    activity.inspectedRelativePaths = {"src/a.cpp"};
    activity.proposedModifiedPaths = {"src/a.cpp"};
    snapshot.taskActivities.push_back(std::move(activity));
    entry.workspaceSnapshot = std::move(snapshot);
    entry.createdAt = std::chrono::system_clock::now();
    return entry;
}

// AG-14.2: simulates a database that was already opened by the original
// AG-14 binary (7d229c9), whose v34 migration created
// manifest_work_ledger_entries WITHOUT manifest_schema_version/
// proposal_lineage/verification_identity_json - the exact bug the AG-14.1
// review found (those 3 columns were mistakenly folded into the v34 CREATE
// TABLE in place instead of being their own v35 migration). Proves the
// fixed v35 ALTER TABLE migration upgrades such a database correctly
// instead of silently skipping (schemaVersion already >= supportedSchema-
// Version) or failing outright ("duplicate column name").
void testUpgradeFromOriginalV34SchemaAddsNewColumns()
{
    Workspace workspace;
    {
        // A normal open brings runs/projects (needed for the FK below and
        // for seedRun's own bookkeeping) all the way to the current schema.
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "ledger-project");
    }
    {
        // Replace manifest_work_ledger_entries with the exact original v34
        // shape (3 columns fewer) and hand-write one row using only those
        // old columns, then roll user_version back to 34 - faithfully
        // reproducing "a database last touched by 7d229c9" without having
        // to hand-reconstruct the full v1-v33 migration history.
        sqlite3* raw = nullptr;
        require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
            "Could not open raw sqlite3 connection");
        const char* downgrade = R"sql(
DROP TABLE manifest_work_ledger_entries;
CREATE TABLE manifest_work_ledger_entries (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    project_id TEXT NOT NULL,
    schema_version TEXT NOT NULL,
    terminal_status TEXT NOT NULL,
    terminal_reason TEXT NOT NULL DEFAULT '',
    baseline_fingerprint TEXT NOT NULL DEFAULT '',
    disposition TEXT NOT NULL,
    task_dispositions TEXT NOT NULL DEFAULT '[]',
    proposal_ids TEXT NOT NULL DEFAULT '[]',
    affected_relative_paths TEXT NOT NULL DEFAULT '[]',
    verification_summary TEXT NOT NULL DEFAULT '',
    remaining_work_note TEXT NOT NULL DEFAULT '',
    created_at_ms INTEGER NOT NULL
);
INSERT INTO manifest_work_ledger_entries
    (run_id, project_id, schema_version, terminal_status, terminal_reason,
     baseline_fingerprint, disposition, task_dispositions, proposal_ids,
     affected_relative_paths, verification_summary, remaining_work_note, created_at_ms)
    VALUES
    ('run-1', 'ledger-project', 'manifest-work-ledger.v1', 'completed', '',
     'old-baseline-fingerprint', 'manifest_ready', '[]', '[]',
     '[]', 'passed', '', 1700000000000);
PRAGMA user_version = 34;
)sql";
        require(sqlite3_exec(raw, downgrade, nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not downgrade to the original v34 schema shape");
        sqlite3_close(raw);
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not upgrade from the original v34 shape");
        SqliteManifestWorkLedgerRepository repo(reopened.value());
        const auto found = repo.findEntry("run-1");
        require(found.success() && found.value().has_value(),
            "The pre-existing v34-shaped row did not survive the v35 upgrade");
        const auto& entry = *found.value();
        require(entry.baselineFingerprint == "old-baseline-fingerprint",
            "Pre-existing column data did not survive the v35 upgrade");
        require(entry.manifestSchemaVersion.empty(),
            "manifest_schema_version must default to '' for a pre-existing v34 row");
        require(entry.proposalLineage.empty(),
            "proposal_lineage must default to an empty list for a pre-existing v34 row");
        require(!entry.verification.has_value(),
            "verification_identity_json must default to NULL for a pre-existing v34 row");

        // The upgrade must also be a genuine no-op on a third open - proves
        // the hasManifestSchemaVersionColumn guard does not re-run (and
        // fail on) the ALTER TABLE once the columns already exist.
        const auto reopenedAgain = SqliteDatabase::open(workspace.database);
        require(reopenedAgain.success(), "A stable v35 database must reopen cleanly");
    }
}

void testInsertEntryRoundTripsAndReopens()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "ledger-project");
        SqliteManifestWorkLedgerRepository repo(opened.value());
        require(repo.insertEntry(makeEntry("run-1", "ledger-project")).success(),
            "Could not insert Work Ledger entry");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestWorkLedgerRepository repo(reopened.value());
        const auto found = repo.findEntry("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find Work Ledger entry after reopen");
        const auto& entry = *found.value();
        require(entry.projectId == "ledger-project", "projectId did not survive reopen");
        require(entry.terminalStatus == RunStatus::Completed, "terminalStatus did not survive reopen");
        require(entry.terminalReason == "Manifest execution completed",
            "terminalReason did not survive reopen");
        require(entry.baselineFingerprint == "deadbeef", "baselineFingerprint did not survive reopen");
        require(entry.disposition == ManifestWorkLedgerDisposition::ManifestReady,
            "disposition did not survive reopen");
        require(entry.taskDispositions.size() == 2, "taskDispositions did not survive reopen");
        require(entry.taskDispositions[0].taskId == "task-a"
                && entry.taskDispositions[0].changeSetId.has_value()
                && *entry.taskDispositions[0].changeSetId == "cs-1",
            "task-a disposition did not survive reopen");
        require(entry.taskDispositions[1].taskId == "task-b"
                && entry.taskDispositions[1].noChangesReason.has_value()
                && *entry.taskDispositions[1].noChangesReason == "already_satisfied_in_baseline",
            "task-b disposition did not survive reopen");
        require(entry.proposalIds == std::vector<ChangeSetId> {"cs-1"},
            "proposalIds did not survive reopen");
        require((entry.affectedRelativePaths
                == std::vector<std::string> {"src/a.cpp", "src/b.cpp"}),
            "affectedRelativePaths did not survive reopen");
        require(entry.verificationSummary == "passed", "verificationSummary did not survive reopen");
        require(entry.workspaceSnapshot.has_value()
                && entry.workspaceSnapshot->entries.size() == 1
                && entry.workspaceSnapshot->entries.front().baselineHash
                    == std::optional<std::string> {"hash-a"}
                && entry.workspaceSnapshot->dependencyEdges.size() == 1
                && entry.workspaceSnapshot->taskActivities.size() == 1
                && entry.workspaceSnapshot->taskActivities.front().proposedModifiedPaths
                    == std::vector<std::string> {"src/a.cpp"},
            "Typed workspace scope/activity snapshot did not survive reopen");
    }
}

void testInsertEntryRejectsDuplicateRunId()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "ledger-project");
    SqliteManifestWorkLedgerRepository repo(opened.value());

    require(repo.insertEntry(makeEntry("run-1", "ledger-project")).success(),
        "Could not insert first Work Ledger entry");

    // AG-14.1: a Work Ledger entry is immutable once written - a second
    // insertEntry for the same runId (a retry, a reconciliation pass racing
    // the original write) must be rejected, never silently overwrite a
    // possibly-more-complete original entry.
    auto replacement = makeEntry("run-1", "ledger-project");
    replacement.terminalStatus = RunStatus::Failed;
    replacement.terminalReason = "Manifest verification failed";
    const auto rejected = repo.insertEntry(replacement);
    require(!rejected.success(), "A duplicate runId insert must fail, not replace");
    require(rejected.error()->code == "manifest_work_ledger.duplicate_entry",
        "A duplicate runId insert must fail with the duplicate_entry code");

    const auto found = repo.findEntry("run-1");
    require(found.success() && found.value().has_value(), "Could not find original entry");
    require(found.value()->terminalStatus == RunStatus::Completed,
        "The original entry must remain untouched by a rejected duplicate insert");
    require(found.value()->terminalReason == "Manifest execution completed",
        "The original entry's terminalReason must remain untouched");
}

void testListRecentForProjectOrdersNewestFirstAndRespectsLimit()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "ledger-project");
    seedRun(opened.value(), "run-2", "ledger-project");
    seedRun(opened.value(), "run-3", "ledger-project");
    seedRun(opened.value(), "run-other", "other-project");
    SqliteManifestWorkLedgerRepository repo(opened.value());

    auto entry1 = makeEntry("run-1", "ledger-project");
    entry1.createdAt = std::chrono::system_clock::now();
    require(repo.insertEntry(entry1).success(), "Could not insert run-1 entry");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto entry2 = makeEntry("run-2", "ledger-project");
    entry2.createdAt = std::chrono::system_clock::now();
    require(repo.insertEntry(entry2).success(), "Could not insert run-2 entry");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto entry3 = makeEntry("run-3", "ledger-project");
    entry3.createdAt = std::chrono::system_clock::now();
    require(repo.insertEntry(entry3).success(), "Could not insert run-3 entry");

    require(repo.insertEntry(makeEntry("run-other", "other-project")).success(),
        "Could not insert other-project entry");

    const auto listed = repo.listRecentForProject("ledger-project", 2);
    require(listed.success(), "Could not list recent Work Ledger entries");
    require(listed.value().size() == 2, "listRecentForProject must respect the limit");
    require(listed.value()[0].runId == "run-3", "Expected the newest entry first");
    require(listed.value()[1].runId == "run-2", "Expected the second-newest entry second");
}

void testListRecentForProjectBreaksSameMillisecondTiesByRunId()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-a", "ledger-project");
    seedRun(opened.value(), "run-b", "ledger-project");
    SqliteManifestWorkLedgerRepository repo(opened.value());

    // Same createdAt for both entries - listRecentForProject must still be
    // deterministic (ORDER BY created_at_ms DESC, run_id DESC), never
    // dependent on SQLite's own unspecified tie-break order (AG-14.1).
    const auto sameInstant = std::chrono::system_clock::now();
    auto entryA = makeEntry("run-a", "ledger-project");
    entryA.createdAt = sameInstant;
    require(repo.insertEntry(entryA).success(), "Could not insert run-a entry");
    auto entryB = makeEntry("run-b", "ledger-project");
    entryB.createdAt = sameInstant;
    require(repo.insertEntry(entryB).success(), "Could not insert run-b entry");

    const auto listed = repo.listRecentForProject("ledger-project", 10);
    require(listed.success() && listed.value().size() == 2,
        "Could not list same-millisecond Work Ledger entries");
    require(listed.value()[0].runId == "run-b" && listed.value()[1].runId == "run-a",
        "Same-millisecond entries must break ties by run_id DESC, deterministically");
}

void testInsertEntryRoundTripsVerificationAndProposalLineage()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "ledger-project");
        SqliteManifestWorkLedgerRepository repo(opened.value());

        auto entry = makeEntry("run-1", "ledger-project");
        entry.manifestSchemaVersion = "execution-manifest.v1";
        ManifestWorkLedgerProposalLineage lineage;
        lineage.changeSetId = "cs-1";
        lineage.state = "applied";
        lineage.supersedesChangeSetId = "cs-0";
        entry.proposalLineage = {lineage};

        ManifestProposalSetVerification verification;
        verification.workspaceIdentity = "workspace-1";
        verification.baselineFingerprint = "baseline-fingerprint-1";
        verification.orderedProposalIds = {"cs-1"};
        verification.orderedProposalContentHashes = {"hash-1"};
        verification.overlayFingerprint = "overlay-fingerprint-1";
        verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
        verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
        verification.acceptanceStatus = ExecutionVerificationStatus::Failed;
        ManifestVerificationDiagnostic diagnostic;
        diagnostic.code = "acceptance.requirement_not_met";
        diagnostic.message = "Requirement req-1 was not satisfied";
        diagnostic.requirementId = "req-1";
        verification.diagnostics = {diagnostic};
        entry.verification = verification;

        require(repo.insertEntry(entry).success(),
            "Could not insert Work Ledger entry with verification/lineage");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestWorkLedgerRepository repo(reopened.value());
        const auto found = repo.findEntry("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find entry with verification/lineage after reopen");
        const auto& entry = *found.value();
        require(entry.manifestSchemaVersion == "execution-manifest.v1",
            "manifestSchemaVersion did not survive reopen");
        require(entry.proposalLineage.size() == 1
                && entry.proposalLineage.front().changeSetId == "cs-1"
                && entry.proposalLineage.front().state == "applied"
                && entry.proposalLineage.front().supersedesChangeSetId.has_value()
                && *entry.proposalLineage.front().supersedesChangeSetId == "cs-0",
            "proposalLineage did not survive reopen");
        require(entry.verification.has_value(), "verification identity did not survive reopen");
        const auto& verification = *entry.verification;
        require(verification.workspaceIdentity == "workspace-1",
            "verification.workspaceIdentity did not survive reopen");
        require(verification.baselineFingerprint == "baseline-fingerprint-1",
            "verification.baselineFingerprint did not survive reopen");
        require(verification.orderedProposalIds == std::vector<ChangeSetId> {"cs-1"},
            "verification.orderedProposalIds did not survive reopen");
        require(verification.acceptanceStatus == ExecutionVerificationStatus::Failed,
            "verification.acceptanceStatus did not survive reopen");
        require(verification.diagnostics.size() == 1
                && verification.diagnostics.front().code == "acceptance.requirement_not_met"
                && verification.diagnostics.front().requirementId.has_value()
                && *verification.diagnostics.front().requirementId == "req-1",
            "verification.diagnostics did not survive reopen");
    }
}

// AG-15.3: workspace_scope_schema_version/workspace_scope_summary (v38)
// round-trip, and a pre-v38 row (simulated the same way
// testUpgradeFromOriginalV34SchemaAddsNewColumns simulates a pre-v35 row)
// upgrades cleanly with both new columns defaulting to '' - v34-v37
// untouched.
void testWorkspaceScopeSummaryRoundTripsAndDefaultsEmptyOnUpgrade()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "ledger-project");
        SqliteManifestWorkLedgerRepository repo(opened.value());

        auto entry = makeEntry("run-1", "ledger-project");
        entry.workspaceScopeSchemaVersion = "run-workspace-scope.v1";
        entry.workspaceScopeSummary =
            "planned write: 2, required read: 1, reference: 0, excluded: 0, "
            "deferred: 1, approved expansions: 1 [deferred: docs/todo.md]";
        require(repo.insertEntry(entry).success(),
            "Could not insert Work Ledger entry with a workspace scope summary");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestWorkLedgerRepository repo(reopened.value());
        const auto found = repo.findEntry("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find entry with a workspace scope summary after reopen");
        require(found.value()->workspaceScopeSchemaVersion == "run-workspace-scope.v1",
            "workspaceScopeSchemaVersion did not survive reopen");
        require(found.value()->workspaceScopeSummary.find("approved expansions: 1")
                != std::string::npos,
            "workspaceScopeSummary did not survive reopen");
        require(found.value()->planningInputFingerprint == std::string(64, 'a')
                && found.value()->planningFailureSignature == std::string(64, 'b'),
            "Planning retry identities did not survive reopen");
    }

    // Simulate a pre-v38 row (no workspace_scope_* columns at all) and prove
    // the v38 ALTER TABLE upgrades it with both new columns defaulting to
    // '', never failing or leaving the row unreadable.
    Workspace legacyWorkspace;
    {
        const auto opened = SqliteDatabase::open(legacyWorkspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-legacy", "ledger-project");
    }
    {
        sqlite3* raw = nullptr;
        require(sqlite3_open(legacyWorkspace.database.string().c_str(), &raw) == SQLITE_OK,
            "Could not open raw sqlite3 connection");
        const char* downgrade = R"sql(
DROP TABLE manifest_work_ledger_entries;
CREATE TABLE manifest_work_ledger_entries (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    project_id TEXT NOT NULL,
    schema_version TEXT NOT NULL,
    terminal_status TEXT NOT NULL,
    terminal_reason TEXT NOT NULL DEFAULT '',
    baseline_fingerprint TEXT NOT NULL DEFAULT '',
    manifest_schema_version TEXT NOT NULL DEFAULT '',
    disposition TEXT NOT NULL,
    task_dispositions TEXT NOT NULL DEFAULT '[]',
    proposal_ids TEXT NOT NULL DEFAULT '[]',
    proposal_lineage TEXT NOT NULL DEFAULT '[]',
    affected_relative_paths TEXT NOT NULL DEFAULT '[]',
    verification_identity_json TEXT,
    verification_summary TEXT NOT NULL DEFAULT '',
    remaining_work_note TEXT NOT NULL DEFAULT '',
    created_at_ms INTEGER NOT NULL
);
INSERT INTO manifest_work_ledger_entries
    (run_id, project_id, schema_version, terminal_status, terminal_reason,
     baseline_fingerprint, manifest_schema_version, disposition, task_dispositions,
     proposal_ids, proposal_lineage, affected_relative_paths, verification_summary,
     remaining_work_note, created_at_ms)
    VALUES
    ('run-legacy', 'ledger-project', 'manifest-work-ledger.v1', 'completed', '',
     'legacy-baseline-fingerprint', '', 'manifest_ready', '[]', '[]', '[]', '[]',
     'passed', '', 1700000000000);
PRAGMA user_version = 37;
)sql";
        require(sqlite3_exec(raw, downgrade, nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not downgrade to a pre-v38 schema shape");
        sqlite3_close(raw);
    }
    {
        const auto reopened = SqliteDatabase::open(legacyWorkspace.database);
        require(reopened.success(), "Database did not upgrade from the pre-v38 shape");
        SqliteManifestWorkLedgerRepository repo(reopened.value());
        const auto found = repo.findEntry("run-legacy");
        require(found.success() && found.value().has_value(),
            "The pre-existing v37-shaped row did not survive the v38 upgrade");
        require(found.value()->baselineFingerprint == "legacy-baseline-fingerprint",
            "Pre-existing column data did not survive the v38 upgrade");
        require(found.value()->workspaceScopeSchemaVersion.empty(),
            "workspace_scope_schema_version must default to '' for a pre-existing v37 row");
        require(found.value()->workspaceScopeSummary.empty(),
            "workspace_scope_summary must default to '' for a pre-existing v37 row");
        require(!found.value()->workspaceSnapshot.has_value(),
            "workspace snapshot must default to NULL for a pre-existing row");
        require(found.value()->planningInputFingerprint.empty()
                && found.value()->planningFailureSignature.empty(),
            "Planning retry identities must default empty for a pre-v47 row");

        const auto reopenedAgain = SqliteDatabase::open(legacyWorkspace.database);
        require(reopenedAgain.success(), "A stable v38 database must reopen cleanly");
    }
}

void testFindEntryFailsOnCorruptJsonColumn()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "ledger-project");
    SqliteManifestWorkLedgerRepository repo(opened.value());
    require(repo.insertEntry(makeEntry("run-1", "ledger-project")).success(),
        "Could not insert Work Ledger entry");

    // Hand-corrupt a JSON column directly through the raw connection - a
    // malformed column must fail the whole row read (AG-14.1), never be
    // silently reinterpreted as an empty/absent value.
    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    require(sqlite3_exec(raw,
        "UPDATE manifest_work_ledger_entries SET task_dispositions = '{not valid json' "
        "WHERE run_id = 'run-1';", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not corrupt task_dispositions column");
    sqlite3_close(raw);

    const auto found = repo.findEntry("run-1");
    require(!found.success(), "findEntry must fail on a corrupt JSON column, not return an empty value");
}

void testFindEntryFailsOnCorruptWorkspaceSnapshot()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "ledger-project");
    SqliteManifestWorkLedgerRepository repo(opened.value());
    require(repo.insertEntry(makeEntry("run-1", "ledger-project")).success(),
        "Could not insert Work Ledger entry with workspace snapshot");

    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    require(sqlite3_exec(raw,
        "UPDATE manifest_work_ledger_entries SET workspace_scope_snapshot_json = '{bad' "
        "WHERE run_id = 'run-1';", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not corrupt workspace snapshot column");
    sqlite3_close(raw);

    const auto found = repo.findEntry("run-1");
    require(!found.success(),
        "A corrupt typed workspace snapshot was silently treated as absent");
}

void testAppendTransitionRoundTripsInOccurredOrder()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "ledger-project");
        SqliteManifestWorkLedgerRepository repo(opened.value());

        ManifestWorkLedgerReviewTransition approved;
        approved.id = "transition-1";
        approved.runId = "run-1";
        approved.changeSetId = "cs-1";
        approved.kind = ManifestWorkLedgerTransitionKind::Approved;
        approved.occurredAt = std::chrono::system_clock::now();
        approved.actor = "operator";
        require(repo.appendTransition(approved).success(), "Could not append Approved transition");

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        ManifestWorkLedgerReviewTransition applied;
        applied.id = "transition-2";
        applied.runId = "run-1";
        applied.changeSetId = "cs-1";
        applied.kind = ManifestWorkLedgerTransitionKind::Applied;
        applied.occurredAt = std::chrono::system_clock::now();
        applied.postActionWorkspaceFingerprint = "cafef00d";
        require(repo.appendTransition(applied).success(), "Could not append Applied transition");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestWorkLedgerRepository repo(reopened.value());
        const auto listed = repo.listTransitionsForRun("run-1");
        require(listed.success(), "Could not list transitions after reopen");
        require(listed.value().size() == 2, "Expected both transitions to survive reopen");
        require(listed.value()[0].kind == ManifestWorkLedgerTransitionKind::Approved
                && listed.value()[0].actor.has_value() && *listed.value()[0].actor == "operator",
            "Approved transition did not survive reopen in occurred order");
        require(listed.value()[1].kind == ManifestWorkLedgerTransitionKind::Applied
                && listed.value()[1].postActionWorkspaceFingerprint.has_value()
                && *listed.value()[1].postActionWorkspaceFingerprint == "cafef00d",
            "Applied transition did not survive reopen in occurred order");
    }
}

void testTransitionsAreScopedToTheirOwnRun()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-a", "ledger-project");
    seedRun(opened.value(), "run-b", "ledger-project");
    SqliteManifestWorkLedgerRepository repo(opened.value());

    ManifestWorkLedgerReviewTransition forA;
    forA.id = "transition-a";
    forA.runId = "run-a";
    forA.changeSetId = "cs-a";
    forA.kind = ManifestWorkLedgerTransitionKind::Rejected;
    forA.occurredAt = std::chrono::system_clock::now();
    forA.reason = "duplicated work";
    require(repo.appendTransition(forA).success(), "Could not append run-a transition");

    ManifestWorkLedgerReviewTransition forB;
    forB.id = "transition-b";
    forB.runId = "run-b";
    forB.changeSetId = "cs-b";
    forB.kind = ManifestWorkLedgerTransitionKind::Approved;
    forB.occurredAt = std::chrono::system_clock::now();
    require(repo.appendTransition(forB).success(), "Could not append run-b transition");

    const auto listedA = repo.listTransitionsForRun("run-a");
    require(listedA.success() && listedA.value().size() == 1,
        "run-a's transitions must not include run-b's rows");
    require(listedA.value().front().changeSetId == "cs-a",
        "run-a's transition must be its own, not run-b's");
}

} // namespace

int runManifestWorkLedgerRepositoryTests()
{
    testUpgradeFromOriginalV34SchemaAddsNewColumns();
    testInsertEntryRoundTripsAndReopens();
    testInsertEntryRejectsDuplicateRunId();
    testListRecentForProjectOrdersNewestFirstAndRespectsLimit();
    testListRecentForProjectBreaksSameMillisecondTiesByRunId();
    testInsertEntryRoundTripsVerificationAndProposalLineage();
    testWorkspaceScopeSummaryRoundTripsAndDefaultsEmptyOnUpgrade();
    testFindEntryFailsOnCorruptJsonColumn();
    testFindEntryFailsOnCorruptWorkspaceSnapshot();
    testAppendTransitionRoundTripsInOccurredOrder();
    testTransitionsAreScopedToTheirOwnRun();
    return 0;
}
