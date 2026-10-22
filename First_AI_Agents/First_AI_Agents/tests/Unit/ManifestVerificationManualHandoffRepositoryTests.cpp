// QML-P4 package 1 (domain + persistence): round-trip and idempotency
// coverage for ManifestVerificationManualHandoff{,Attempt} across both the
// InMemory and SQLite repository implementations. Materialize()/
// importAndReverify() orchestration is covered by their own test files once
// those services exist - this file only proves the durable contract itself.

#include "ai/application/InMemoryManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using namespace ai::infrastructure;

std::string hex64(const std::string& seed)
{
    return sha256Hex(seed);
}

WorkspaceBaselineSnapshot makeSnapshot(const std::string& seed)
{
    WorkspaceBaselineSnapshot snapshot;
    WorkspaceBaselineEntry entry;
    entry.relativePath = "qml/AppSidebar.qml";
    entry.byteSize = 128;
    entry.sha256 = hex64("file:" + seed);
    snapshot.files.push_back(entry);
    snapshot.totalBytes = entry.byteSize;
    // WorkspaceBaselineProvenance::parseJson recomputes the fingerprint from
    // the (already sorted) file list and rejects any mismatch - see
    // computeBaselineFingerprint in WorkspaceBaselineProvenance.cpp. A fixture
    // snapshot must be genuinely self-consistent, not an arbitrary hash.
    std::ostringstream material;
    for (const auto& file : snapshot.files) {
        material << file.relativePath << '\0' << file.byteSize << '\0' << file.sha256 << '\n';
    }
    snapshot.fingerprint = sha256Hex(material.str());
    return snapshot;
}

ManifestVerificationManualHandoff makeHandoff(
    const std::string& verificationAnchorRunId, const std::string& seed)
{
    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:" + verificationAnchorRunId;
    handoff.originManifestRunId = verificationAnchorRunId;
    handoff.verificationAnchorRunId = verificationAnchorRunId;
    handoff.sourceProjectId = "project-1";
    handoff.storageKey = "h-" + seed;
    handoff.workspaceIdentity = "project-1";
    handoff.baselineFingerprint = hex64("baseline:" + seed);
    handoff.sourceOverlayFingerprint = hex64("overlay:" + seed);
    handoff.frozenMembers = {{
        "proposal-1", verificationAnchorRunId, hex64("proposal-1:" + seed), "proposal-1"}};
    ManifestVerificationManualHandoffAllowedPath allowed;
    allowed.relativePath = "qml/AppSidebar.qml";
    allowed.ownerProposalId = "proposal-1";
    allowed.ownerTaskId = "task-1";
    handoff.allowedPaths = {allowed};
    handoff.originalSnapshot = makeSnapshot(seed);
    handoff.createdAt = std::chrono::system_clock::now();
    return handoff;
}

ManifestVerificationManualHandoffAttempt makeAttempt(
    const ManifestVerificationManualHandoff& handoff, const std::string& attemptRunId,
    std::uint64_t ordinal, const std::string& seed)
{
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:" + attemptRunId;
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRunId;
    attempt.attemptOrdinal = ordinal;
    attempt.editedSnapshotFingerprint = hex64("edited:" + seed);
    attempt.createdAt = std::chrono::system_clock::now();
    return attempt;
}

ManifestVerificationManualHandoffAttemptResult makeAttemptResult(
    const ManifestVerificationManualHandoffAttempt& attempt,
    ManifestVerificationManualHandoffAttemptResultKind kind)
{
    ManifestVerificationManualHandoffAttemptResult result;
    result.id = "manual-handoff-attempt-result:" + attempt.id;
    result.attemptId = attempt.id;
    result.attemptRunId = attempt.attemptRunId;
    result.kind = kind;
    result.recordedAt = std::chrono::system_clock::now();
    return result;
}

void testValidateRejectsAllowedPathNotOwnedByAMemberProposal()
{
    auto handoff = makeHandoff("run-1", "a");
    handoff.allowedPaths.front().ownerProposalId = "not-a-member-proposal";
    assert(!validateManifestVerificationManualHandoff(handoff).success());
}

void testValidateRejectsDuplicateAllowedPaths()
{
    auto handoff = makeHandoff("run-1", "a");
    handoff.allowedPaths.push_back(handoff.allowedPaths.front());
    assert(!validateManifestVerificationManualHandoff(handoff).success());
}

void testValidateRejectsEmptyAllowedPaths()
{
    auto handoff = makeHandoff("run-1", "a");
    handoff.allowedPaths.clear();
    assert(!validateManifestVerificationManualHandoff(handoff).success());
}

// QML-P4R.1 hardening: two frozen members sharing a lineage root would mean
// the exact set contains two different repair generations of the same
// original proposal - never a valid exact set.
void testValidateRejectsDuplicateLineageRoots()
{
    auto handoff = makeHandoff("run-1", "a");
    ManifestVerificationManualHandoffFrozenMember second = handoff.frozenMembers.front();
    second.proposalId = "proposal-2";
    second.lineageRootProposalId = handoff.frozenMembers.front().lineageRootProposalId;
    handoff.frozenMembers.push_back(second);
    assert(!validateManifestVerificationManualHandoff(handoff).success());
}

void testValidateAttemptRejectsSameSourceAndAttemptRun()
{
    auto handoff = makeHandoff("run-1", "a");
    auto attempt = makeAttempt(handoff, "run-1", 1, "a");
    assert(!validateManifestVerificationManualHandoffAttempt(attempt).success());
}

void testInMemoryHandoffRoundTripAndDuplicateRejection()
{
    InMemoryManifestVerificationManualHandoffRepository repository;
    auto handoff = makeHandoff("run-1", "a");
    assert(repository.insertHandoff(handoff).success());

    const auto duplicate = repository.insertHandoff(makeHandoff("run-1", "b"));
    assert(!duplicate.success());
    assert(duplicate.error()->code == "manifest_verification_manual_handoff.already_exists");

    const auto bySource = repository.findHandoffByAnchorRun("run-1");
    assert(bySource.success() && bySource.value().has_value());
    assert(bySource.value()->storageKey == handoff.storageKey);
    assert(bySource.value()->allowedPaths.size() == 1);
    assert(bySource.value()->originalSnapshot.files.size() == 1);

    const auto byId = repository.findHandoffById(handoff.id);
    assert(byId.success() && byId.value().has_value());
    assert(byId.value()->verificationAnchorRunId == "run-1");

    assert(!repository.findHandoffByAnchorRun("run-missing").value().has_value());
    assert(!repository.findHandoffById("missing-id").value().has_value());
}

// QML-P4R.1 hardening: AG-16's reverse lookup must resolve a frozen member by
// its own proposal id alone, never by guessing which Run is "the anchor" -
// and a proposal id can be frozen into at most one handoff.
void testInMemoryFindHandoffByMemberProposalIdAndClaimUniqueness()
{
    InMemoryManifestVerificationManualHandoffRepository repository;
    auto handoff = makeHandoff("run-4", "member-a");
    assert(repository.insertHandoff(handoff).success());

    const auto found = repository.findHandoffByMemberProposalId("proposal-1");
    assert(found.success() && found.value().has_value() && found.value()->id == handoff.id);

    const auto missing = repository.findHandoffByMemberProposalId("proposal-missing");
    assert(missing.success() && !missing.value().has_value());

    // A different handoff (different anchor Run) whose frozen member reuses
    // the SAME proposal id must be rejected, not silently accepted.
    const auto rejected = repository.insertHandoff(makeHandoff("run-5", "member-b"));
    assert(!rejected.success());
    assert(rejected.error()->code == "manifest_verification_manual_handoff.member_already_claimed");
    assert(!repository.findHandoffByAnchorRun("run-5").value().has_value());
}

void testInMemoryAttemptRoundTripFingerprintIdempotencyAndOrdering()
{
    InMemoryManifestVerificationManualHandoffRepository repository;
    auto handoff = makeHandoff("run-2", "a");
    assert(repository.insertHandoff(handoff).success());

    const auto first = makeAttempt(handoff, "attempt-1", 1, "state-1");
    assert(repository.insertAttempt(first).success());
    const auto second = makeAttempt(handoff, "attempt-2", 2, "state-2");
    assert(repository.insertAttempt(second).success());

    const auto duplicateOrdinal = repository.insertAttempt(
        makeAttempt(handoff, "attempt-3", 2, "state-3"));
    assert(!duplicateOrdinal.success());
    assert(duplicateOrdinal.error()->code
        == "manifest_verification_manual_handoff.attempt_already_exists");

    const auto duplicateRunId = repository.insertAttempt(
        makeAttempt(handoff, "attempt-1", 3, "state-3"));
    assert(!duplicateRunId.success());
    assert(duplicateRunId.error()->code
        == "manifest_verification_manual_handoff.attempt_already_exists");

    const auto byFingerprint = repository.findAttemptByFingerprint(
        handoff.id, first.editedSnapshotFingerprint);
    assert(byFingerprint.success() && byFingerprint.value().has_value());
    assert(byFingerprint.value()->attemptRunId == "attempt-1");

    const auto missingFingerprint = repository.findAttemptByFingerprint(
        handoff.id, hex64("never-seen"));
    assert(missingFingerprint.success() && !missingFingerprint.value().has_value());

    const auto listed = repository.listAttemptsForHandoff(handoff.id);
    assert(listed.success() && listed.value().size() == 2);
    assert(listed.value()[0].attemptOrdinal == 1);
    assert(listed.value()[1].attemptOrdinal == 2);
}

struct TemporaryPersistenceWorkspace {
    fs::path root;
    fs::path databasePath;

    TemporaryPersistenceWorkspace()
    {
        root = fs::temp_directory_path()
            / ("ai-orchestrator-manual-handoff-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        databasePath = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(root);
    }

    ~TemporaryPersistenceWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void testSqliteHandoffAndAttemptRoundTripAndDuplicateRejection()
{
    TemporaryPersistenceWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.databasePath);
    assert(opened.success());
    const auto database = opened.value();

    SqliteProjectRepository projects(database);
    Project project;
    project.id = "project-1";
    project.name = "Project";
    project.rootPath = workspace.root / "project";
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    assert(projects.insert(project).success());

    SqliteRunRepository runs(database);
    Run source;
    source.id = "run-3";
    source.projectId = "project-1";
    source.title = "Source Run";
    source.task = "task";
    source.workflowId = std::string(kExecutionManifestWorkflowId);
    source.status = RunStatus::Failed;
    source.createdAt = std::chrono::system_clock::now();
    assert(runs.insertRun(source).success());

    SqliteManifestVerificationManualHandoffRepository repository(database);
    auto handoff = makeHandoff("run-3", "sql-a");
    assert(repository.insertHandoff(handoff).success());

    const auto duplicate = repository.insertHandoff(makeHandoff("run-3", "sql-b"));
    assert(!duplicate.success());

    const auto reloaded = repository.findHandoffByAnchorRun("run-3");
    assert(reloaded.success() && reloaded.value().has_value());
    const auto& loaded = *reloaded.value();
    assert(loaded.id == handoff.id);
    assert(loaded.originManifestRunId == handoff.originManifestRunId);
    assert(loaded.verificationAnchorRunId == handoff.verificationAnchorRunId);
    assert(loaded.storageKey == handoff.storageKey);
    assert(loaded.workspaceIdentity == handoff.workspaceIdentity);
    assert(loaded.baselineFingerprint == handoff.baselineFingerprint);
    assert(loaded.sourceOverlayFingerprint == handoff.sourceOverlayFingerprint);
    assert(loaded.frozenMembers.size() == handoff.frozenMembers.size());
    assert(loaded.frozenMembers.front().proposalId == handoff.frozenMembers.front().proposalId);
    assert(loaded.frozenMembers.front().owningRunId == handoff.frozenMembers.front().owningRunId);
    assert(loaded.frozenMembers.front().contentHash == handoff.frozenMembers.front().contentHash);
    assert(loaded.frozenMembers.front().lineageRootProposalId
        == handoff.frozenMembers.front().lineageRootProposalId);
    assert(loaded.allowedPaths.size() == 1);
    assert(loaded.allowedPaths.front().relativePath == "qml/AppSidebar.qml");
    assert(loaded.allowedPaths.front().ownerProposalId == "proposal-1");
    assert(loaded.allowedPaths.front().ownerTaskId == "task-1");
    assert(loaded.originalSnapshot.fingerprint == handoff.originalSnapshot.fingerprint);
    assert(loaded.originalSnapshot.files.size() == 1);
    assert(loaded.originalSnapshot.files.front().relativePath
        == handoff.originalSnapshot.files.front().relativePath);
    assert(loaded.originalSnapshot.files.front().sha256
        == handoff.originalSnapshot.files.front().sha256);

    Run attemptRun = source;
    attemptRun.id = "attempt-run-1";
    attemptRun.parentRunId = "run-3";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.status = RunStatus::Completed;
    assert(runs.insertRun(attemptRun).success());

    const auto attempt = makeAttempt(loaded, "attempt-run-1", 1, "sql-state-1");
    assert(repository.insertAttempt(attempt).success());

    const auto duplicateAttempt = repository.insertAttempt(
        makeAttempt(loaded, "attempt-run-1", 2, "sql-state-2"));
    assert(!duplicateAttempt.success());

    const auto reloadedAttempt = repository.findAttemptByRunId("attempt-run-1");
    assert(reloadedAttempt.success() && reloadedAttempt.value().has_value());
    assert(reloadedAttempt.value()->handoffId == handoff.id);
    assert(reloadedAttempt.value()->editedSnapshotFingerprint
        == attempt.editedSnapshotFingerprint);

    const auto byFingerprint = repository.findAttemptByFingerprint(
        handoff.id, attempt.editedSnapshotFingerprint);
    assert(byFingerprint.success() && byFingerprint.value().has_value());
    assert(byFingerprint.value()->attemptRunId == "attempt-run-1");

    // A terminal/incomplete attempt may be explicitly retried without any
    // edit change. The append-only ordinal, not the candidate fingerprint,
    // is the durable command identity; fingerprint lookup returns newest.
    // Reservation is intentionally durable before its Run exists. This is
    // the crash-safe v54 ordering used by importAndReverify().
    auto sameFingerprintDifferentRun = makeAttempt(loaded, "attempt-run-2", 2, "sql-state-1");
    assert(sameFingerprintDifferentRun.editedSnapshotFingerprint
        == attempt.editedSnapshotFingerprint);
    assert(repository.insertAttempt(sameFingerprintDifferentRun).success());

    const auto newestByFingerprint = repository.findAttemptByFingerprint(
        handoff.id, attempt.editedSnapshotFingerprint);
    assert(newestByFingerprint.success() && newestByFingerprint.value().has_value());
    assert(newestByFingerprint.value()->attemptRunId == "attempt-run-2");

    const auto duplicateOrdinal = repository.insertAttempt(
        makeAttempt(loaded, "attempt-run-3", 2, "sql-state-3"));
    assert(!duplicateOrdinal.success());

    const auto listed = repository.listAttemptsForHandoff(handoff.id);
    assert(listed.success() && listed.value().size() == 2);
}

// QML-P4R.1 hardening: the SQLite counterpart of
// testInMemoryFindHandoffByMemberProposalIdAndClaimUniqueness - proves the
// normalized manifest_verification_manual_handoff_members table (indexed by
// proposal_id, never scanned from JSON) and its UNIQUE constraint actually
// work end to end, and that a rejected insert leaves no partial row behind
// (the whole insertHandoff() call is one transaction).
void testSqliteFindHandoffByMemberProposalIdAndClaimUniqueness()
{
    TemporaryPersistenceWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.databasePath);
    assert(opened.success());
    const auto database = opened.value();

    SqliteProjectRepository projects(database);
    Project project;
    project.id = "project-member";
    project.name = "Member lookup project";
    project.rootPath = workspace.root / "project-member";
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    assert(projects.insert(project).success());

    SqliteRunRepository runs(database);
    Run runA;
    runA.id = "run-member-a";
    runA.projectId = "project-member";
    runA.title = "Run A";
    runA.task = "task";
    runA.workflowId = std::string(kExecutionManifestWorkflowId);
    runA.status = RunStatus::Failed;
    runA.createdAt = std::chrono::system_clock::now();
    assert(runs.insertRun(runA).success());
    Run runB = runA;
    runB.id = "run-member-b";
    assert(runs.insertRun(runB).success());

    SqliteManifestVerificationManualHandoffRepository repository(database);
    auto handoffA = makeHandoff("run-member-a", "member-sql-a");
    assert(repository.insertHandoff(handoffA).success());

    const auto found = repository.findHandoffByMemberProposalId("proposal-1");
    assert(found.success() && found.value().has_value() && found.value()->id == handoffA.id);

    const auto missing = repository.findHandoffByMemberProposalId("proposal-missing");
    assert(missing.success() && !missing.value().has_value());

    // handoffB's own frozen member reuses "proposal-1" (makeHandoff's own
    // default) - the members table's UNIQUE(proposal_id) must reject it, and
    // the rejection must not leave a partial handoff row behind.
    const auto rejected = repository.insertHandoff(makeHandoff("run-member-b", "member-sql-b"));
    assert(!rejected.success());
    assert(rejected.error()->code == "manifest_verification_manual_handoff.member_already_claimed");
    const auto notPersisted = repository.findHandoffByAnchorRun("run-member-b");
    assert(notPersisted.success() && !notPersisted.value().has_value());
}

constexpr const char* rawV54ManualHandoffTablesSql =
    "DROP TABLE IF EXISTS manifest_verification_manual_handoff_members;"
    "DROP TABLE IF EXISTS manifest_verification_manual_handoff_attempts;"
    "DROP TABLE IF EXISTS manifest_verification_manual_handoffs;"
    "CREATE TABLE manifest_verification_manual_handoffs ("
    "    id TEXT PRIMARY KEY,"
    "    source_run_id TEXT NOT NULL UNIQUE REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,"
    "    schema_version TEXT NOT NULL,"
    "    source_project_id TEXT NOT NULL,"
    "    storage_key TEXT NOT NULL,"
    "    workspace_identity TEXT NOT NULL,"
    "    baseline_fingerprint TEXT NOT NULL,"
    "    source_overlay_fingerprint TEXT NOT NULL,"
    "    proposal_ids_json TEXT NOT NULL,"
    "    proposal_hashes_json TEXT NOT NULL,"
    "    allowed_paths_json TEXT NOT NULL,"
    "    original_snapshot_json TEXT NOT NULL,"
    "    created_at_ms INTEGER NOT NULL"
    ");"
    "CREATE TABLE manifest_verification_manual_handoff_attempts ("
    "    id TEXT PRIMARY KEY,"
    "    handoff_id TEXT NOT NULL REFERENCES manifest_verification_manual_handoffs(id)"
    "        ON UPDATE CASCADE ON DELETE CASCADE,"
    "    source_run_id TEXT NOT NULL,"
    "    attempt_run_id TEXT NOT NULL UNIQUE,"
    "    schema_version TEXT NOT NULL,"
    "    attempt_ordinal INTEGER NOT NULL CHECK(attempt_ordinal >= 1),"
    "    previous_attempt_id TEXT,"
    "    edited_snapshot_fingerprint TEXT NOT NULL,"
    "    created_at_ms INTEGER NOT NULL,"
    "    UNIQUE(handoff_id, attempt_ordinal)"
    ");"
    "CREATE INDEX idx_manual_handoff_attempts_handoff"
    "    ON manifest_verification_manual_handoff_attempts(handoff_id, attempt_ordinal);";

// QML-P4R.1 hardening: a GENUINE raw-v54-shaped database (the actual v53/v54
// column names this migration used to replay against, not merely a fresh
// database that happens to report schema version 54) must still upgrade
// cleanly to v55 when both tables are empty - proving the row-count guard
// does not itself become a false-positive refusal on the legitimate empty
// case.
void testRawV54ManualHandoffTablesUpgradeCleanlyWhenEmpty()
{
    TemporaryPersistenceWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        assert(opened.success());
    }
    sqlite3* handle = nullptr;
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    assert(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const std::string rewind = std::string("BEGIN IMMEDIATE;") + rawV54ManualHandoffTablesSql
        + "PRAGMA user_version = 54;COMMIT;";
    assert(sqlite3_exec(handle, rewind.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close_v2(handle);

    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    assert(reopened.success());

    // Confirm the v55 shape and the new members table actually work end to
    // end after a genuine raw-v54 upgrade, not merely that open() returned
    // success.
    SqliteProjectRepository projects(reopened.value());
    Project project;
    project.id = "raw-v54-project";
    project.name = "Raw v54 upgrade project";
    project.rootPath = workspace.root / "raw-v54-project";
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    assert(projects.insert(project).success());
    SqliteRunRepository runs(reopened.value());
    Run run;
    run.id = "raw-v54-run";
    run.projectId = project.id;
    run.title = "Raw v54 upgrade run";
    run.task = "task";
    run.workflowId = std::string(kExecutionManifestWorkflowId);
    run.status = RunStatus::Failed;
    run.createdAt = std::chrono::system_clock::now();
    assert(runs.insertRun(run).success());
    SqliteManifestVerificationManualHandoffRepository repository(reopened.value());
    auto handoff = makeHandoff("raw-v54-run", "raw-v54-upgrade");
    assert(repository.insertHandoff(handoff).success());
    const auto found = repository.findHandoffByMemberProposalId("proposal-1");
    assert(found.success() && found.value().has_value() && found.value()->id == handoff.id);
}

// QML-P4R.1 hardening: the inverse case - a genuine raw-v54-shaped database
// with an EXISTING row must refuse the v55 upgrade with a stable error code
// rather than silently dropping it, and the row must remain exactly as it
// was (the refusal happens before the drop/recreate transaction even
// begins).
void testRawV54ManualHandoffTableWithARowRefusesUpgradeWithoutDataLoss()
{
    TemporaryPersistenceWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        assert(opened.success());
        SqliteProjectRepository projects(opened.value());
        Project project;
        project.id = "raw-v54-row-project";
        project.name = "Raw v54 row project";
        project.rootPath = workspace.root / "raw-v54-row-project";
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        assert(projects.insert(project).success());
        SqliteRunRepository runs(opened.value());
        Run run;
        run.id = "raw-v54-row-run";
        run.projectId = project.id;
        run.title = "Raw v54 row run";
        run.task = "task";
        run.workflowId = std::string(kExecutionManifestWorkflowId);
        run.createdAt = std::chrono::system_clock::now();
        assert(runs.insertRun(run).success());
    }
    sqlite3* handle = nullptr;
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    assert(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const std::string rewind = std::string("BEGIN IMMEDIATE;") + rawV54ManualHandoffTablesSql
        + "INSERT INTO manifest_verification_manual_handoffs "
          "(id,source_run_id,schema_version,source_project_id,storage_key,workspace_identity,"
          "baseline_fingerprint,source_overlay_fingerprint,proposal_ids_json,proposal_hashes_json,"
          "allowed_paths_json,original_snapshot_json,created_at_ms) VALUES "
          "('h-raw','raw-v54-row-run','manifest-verification-manual-handoff.v1',"
          "'raw-v54-row-project','k1','w1','b1','o1','[]','[]','[]','{}',0);"
          "PRAGMA user_version = 54;COMMIT;";
    assert(sqlite3_exec(handle, rewind.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close_v2(handle);

    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    assert(!reopened.success());
    assert(reopened.error()->code == "migration.v55_refused_data_loss");

    sqlite3* verifyHandle = nullptr;
    assert(sqlite3_open_v2(utf8Path.c_str(), &verifyHandle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_stmt* countStatement = nullptr;
    assert(sqlite3_prepare_v2(verifyHandle,
        "SELECT COUNT(*) FROM manifest_verification_manual_handoffs;", -1, &countStatement, nullptr)
        == SQLITE_OK);
    assert(sqlite3_step(countStatement) == SQLITE_ROW);
    assert(sqlite3_column_int(countStatement, 0) == 1);
    sqlite3_finalize(countStatement);
    sqlite3_close_v2(verifyHandle);
}

// QML-P4R.3.4: the typed retry/replay classification's InMemory round trip
// - insert, find, at-most-one-per-attempt rejection, and cascade removal
// when its owning attempt/handoff is discarded (the same cascade the
// attempt row itself already gets).
void testInMemoryAttemptResultRoundTripDuplicateRejectionAndCascade()
{
    InMemoryManifestVerificationManualHandoffRepository repository;
    auto handoff = makeHandoff("run-6", "result-a");
    assert(repository.insertHandoff(handoff).success());
    const auto attempt = makeAttempt(handoff, "attempt-result-1", 1, "result-state-1");
    assert(repository.insertAttempt(attempt).success());

    assert(!repository.findAttemptResultByAttemptId(attempt.id).value().has_value());

    const auto result = makeAttemptResult(
        attempt, ManifestVerificationManualHandoffAttemptResultKind::Incomplete);
    assert(repository.insertAttemptResult(result).success());

    const auto duplicate = repository.insertAttemptResult(makeAttemptResult(
        attempt, ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess));
    assert(!duplicate.success());
    assert(duplicate.error()->code
        == "manifest_verification_manual_handoff.attempt_result_already_exists");

    const auto found = repository.findAttemptResultByAttemptId(attempt.id);
    assert(found.success() && found.value().has_value());
    assert(found.value()->kind == ManifestVerificationManualHandoffAttemptResultKind::Incomplete);
    assert(found.value()->attemptRunId == attempt.attemptRunId);

    assert(repository.discardHandoff(handoff.id).success());
    assert(!repository.findAttemptResultByAttemptId(attempt.id).value().has_value());
}

// The SQLite counterpart: real round trip, the schema's own UNIQUE(attempt_id)
// rejection, and a genuine ON DELETE CASCADE through the attempts table when
// the owning handoff is discarded - never a row left pointing at a deleted
// attempt.
void testSqliteAttemptResultRoundTripDuplicateRejectionAndCascade()
{
    TemporaryPersistenceWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.databasePath);
    assert(opened.success());
    const auto database = opened.value();

    SqliteProjectRepository projects(database);
    Project project;
    project.id = "project-result";
    project.name = "Attempt result project";
    project.rootPath = workspace.root / "project-result";
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    assert(projects.insert(project).success());

    SqliteRunRepository runs(database);
    Run source;
    source.id = "run-result-1";
    source.projectId = "project-result";
    source.title = "Source Run";
    source.task = "task";
    source.workflowId = std::string(kExecutionManifestWorkflowId);
    source.status = RunStatus::Failed;
    source.createdAt = std::chrono::system_clock::now();
    assert(runs.insertRun(source).success());

    SqliteManifestVerificationManualHandoffRepository repository(database);
    auto handoff = makeHandoff("run-result-1", "sql-result-a");
    assert(repository.insertHandoff(handoff).success());
    const auto attempt = makeAttempt(handoff, "attempt-result-run-1", 1, "sql-result-state-1");
    assert(repository.insertAttempt(attempt).success());

    assert(!repository.findAttemptResultByAttemptId(attempt.id).value().has_value());

    const auto result = makeAttemptResult(
        attempt, ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure);
    assert(repository.insertAttemptResult(result).success());

    const auto duplicate = repository.insertAttemptResult(makeAttemptResult(
        attempt, ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess));
    assert(!duplicate.success());
    assert(duplicate.error()->code
        == "manifest_verification_manual_handoff.attempt_result_already_exists");

    const auto found = repository.findAttemptResultByAttemptId(attempt.id);
    assert(found.success() && found.value().has_value());
    assert(found.value()->kind
        == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure);
    assert(found.value()->attemptRunId == attempt.attemptRunId);

    assert(repository.discardHandoff(handoff.id).success());
    const auto afterDiscard = repository.findAttemptResultByAttemptId(attempt.id);
    assert(afterDiscard.success() && !afterDiscard.value().has_value());

    sqlite3* verifyHandle = nullptr;
    const std::u8string path = workspace.databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    assert(sqlite3_open_v2(utf8Path.c_str(), &verifyHandle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_stmt* countStatement = nullptr;
    assert(sqlite3_prepare_v2(verifyHandle,
        "SELECT COUNT(*) FROM manifest_verification_manual_handoff_attempt_results;", -1,
        &countStatement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(countStatement) == SQLITE_ROW);
    assert(sqlite3_column_int(countStatement, 0) == 0);
    sqlite3_finalize(countStatement);
    sqlite3_close_v2(verifyHandle);
}

// QML-P4R.3.4 mandated coverage: the typed classification must survive a
// genuine SQLite restart - not merely a fresh read through the SAME open
// connection. Closes the database (drops every shared_ptr<SqliteDatabase>),
// reopens a brand-new connection on the same file, and proves the row - and
// specifically which typed kind it recorded - reads back unchanged.
void testSqliteAttemptResultClassificationSurvivesRestart()
{
    TemporaryPersistenceWorkspace workspace;
    ManifestVerificationManualHandoff handoff;
    ManifestVerificationManualHandoffAttempt attempt;
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        assert(opened.success());
        const auto database = opened.value();

        SqliteProjectRepository projects(database);
        Project project;
        project.id = "project-restart";
        project.name = "Attempt result restart project";
        project.rootPath = workspace.root / "project-restart";
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        assert(projects.insert(project).success());

        SqliteRunRepository runs(database);
        Run source;
        source.id = "run-restart-1";
        source.projectId = "project-restart";
        source.title = "Source Run";
        source.task = "task";
        source.workflowId = std::string(kExecutionManifestWorkflowId);
        source.status = RunStatus::Failed;
        source.createdAt = std::chrono::system_clock::now();
        assert(runs.insertRun(source).success());

        SqliteManifestVerificationManualHandoffRepository repository(database);
        handoff = makeHandoff("run-restart-1", "sql-restart-a");
        assert(repository.insertHandoff(handoff).success());
        attempt = makeAttempt(handoff, "attempt-restart-run-1", 1, "sql-restart-state-1");
        assert(repository.insertAttempt(attempt).success());
        const auto result = makeAttemptResult(
            attempt, ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess);
        assert(repository.insertAttemptResult(result).success());
        // database (and every repository built on it) goes out of scope
        // here - a genuine close, not merely a second handle on the same
        // open connection.
    }

    const auto reopened = SqliteDatabase::open(workspace.databasePath);
    assert(reopened.success());
    SqliteManifestVerificationManualHandoffRepository restarted(reopened.value());
    const auto found = restarted.findAttemptResultByAttemptId(attempt.id);
    assert(found.success() && found.value().has_value());
    assert(found.value()->kind == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess);
    assert(found.value()->attemptRunId == attempt.attemptRunId);

    // The attempt and handoff themselves must have survived identically -
    // the typed result's own restart survival is only meaningful alongside
    // the durable state it classifies.
    const auto restartedAttempt = restarted.findAttemptByRunId(attempt.attemptRunId);
    assert(restartedAttempt.success() && restartedAttempt.value().has_value());
    assert(restartedAttempt.value()->editedSnapshotFingerprint == attempt.editedSnapshotFingerprint);
}

} // namespace

int runManifestVerificationManualHandoffRepositoryTests()
{
    testValidateRejectsAllowedPathNotOwnedByAMemberProposal();
    testValidateRejectsDuplicateAllowedPaths();
    testValidateRejectsEmptyAllowedPaths();
    testValidateRejectsDuplicateLineageRoots();
    testValidateAttemptRejectsSameSourceAndAttemptRun();
    testInMemoryHandoffRoundTripAndDuplicateRejection();
    testInMemoryFindHandoffByMemberProposalIdAndClaimUniqueness();
    testInMemoryAttemptRoundTripFingerprintIdempotencyAndOrdering();
    testSqliteHandoffAndAttemptRoundTripAndDuplicateRejection();
    testSqliteFindHandoffByMemberProposalIdAndClaimUniqueness();
    testRawV54ManualHandoffTablesUpgradeCleanlyWhenEmpty();
    testRawV54ManualHandoffTableWithARowRefusesUpgradeWithoutDataLoss();
    testInMemoryAttemptResultRoundTripDuplicateRejectionAndCascade();
    testSqliteAttemptResultRoundTripDuplicateRejectionAndCascade();
    testSqliteAttemptResultClassificationSurvivesRestart();
    return 0;
}
