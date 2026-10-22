#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <sstream>
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
        / ("ai-orchestrator-verification-environment-snapshot-test-"
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
    Project project {projectId, "Verification environment test project",
        "Round-trip verification environment snapshot persistence",
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
    run.title = "Verification environment persistence";
    run.task = "Round-trip a verification environment snapshot";
    run.workflowId = "execution-manifest-v1";
    run.createdAt = now;
    require(runs.insertRun(run).success(), "Could not seed Run");
}

VerificationEnvironmentSnapshot makeSnapshot(const RunId& runId, const ProjectId& projectId)
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = projectId;
    snapshot.baselineFingerprint = "baseline-fp-1";
    snapshot.state = VerificationEnvironmentState::ExistingProfile;

    VerificationProfile profile;
    profile.profileId = "local-qt-debug";
    profile.configurePresetName = "local-qt-debug";
    profile.configureDisplayName = "Desktop Qt Debug";
    profile.configureSource = VerificationPresetFileSource::UserPresets;
    profile.declaredBinaryDir = "${sourceDir}/build/local-qt-debug";
    profile.resolvedBinaryDir = "build/local-qt-debug";
    profile.origin = VerificationProfileOrigin::Discovered;
    profile.buildPresetsConfirmed = true;
    VerificationBuildPresetDescriptor buildPreset;
    buildPreset.name = "local-qt-debug";
    buildPreset.displayName = "Desktop Qt Debug";
    buildPreset.source = VerificationPresetFileSource::UserPresets;
    buildPreset.declaredConfigurePreset = "local-qt-debug";
    buildPreset.declaredTargets = {"all_qmllint", "all"};
    buildPreset.allowedTargetsConfirmed = true;
    buildPreset.allowedTargets = {"all", "all_qmllint"};
    profile.buildPresets.push_back(buildPreset);
    snapshot.profiles.push_back(profile);

    VerificationBuildPresetDescriptor unlinked;
    unlinked.name = "orphan-build";
    unlinked.source = VerificationPresetFileSource::Unknown;
    snapshot.unlinkedBuildPresets.push_back(unlinked);

    snapshot.discoveredAt = std::chrono::system_clock::now();
    return snapshot;
}

sqlite3* openRaw(const fs::path& database)
{
    sqlite3* raw = nullptr;
    require(sqlite3_open(database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    return raw;
}

// Bypasses the immutability trigger the same way AG-15's own corrupt-column
// test does: a raw connection performing a schema-level operation, never
// production code.
void dropImmutabilityTrigger(sqlite3* raw)
{
    require(sqlite3_exec(raw, "DROP TRIGGER verification_environment_snapshots_reject_update;",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not drop immutability trigger for the test");
}

void testRecordSnapshotRoundTripsAndReopens()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "verification-environment-project");
        SqliteVerificationEnvironmentRepository repo(opened.value());
        require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
            "Could not record verification environment snapshot");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteVerificationEnvironmentRepository repo(reopened.value());
        const auto found = repo.findForRun("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find recorded verification environment snapshot after reopen");
        const auto& snapshot = *found.value();
        require(snapshot.projectId == "verification-environment-project",
            "projectId did not survive reopen");
        require(snapshot.baselineFingerprint == "baseline-fp-1",
            "baselineFingerprint did not survive reopen");
        require(snapshot.state == VerificationEnvironmentState::ExistingProfile,
            "state did not survive reopen");
        require(snapshot.profiles.size() == 1, "Expected exactly one profile to survive reopen");
        const auto& profile = snapshot.profiles[0];
        require(profile.profileId == "local-qt-debug"
                && profile.configurePresetName == "local-qt-debug"
                && profile.configureDisplayName == "Desktop Qt Debug"
                && profile.configureSource == VerificationPresetFileSource::UserPresets
                && profile.declaredBinaryDir == "${sourceDir}/build/local-qt-debug"
                && profile.resolvedBinaryDir == "build/local-qt-debug"
                && profile.origin == VerificationProfileOrigin::Discovered
                && profile.buildPresetsConfirmed,
            "Profile identity/provenance did not survive reopen");
        require(profile.buildPresets.size() == 1
                && profile.buildPresets[0].declaredConfigurePreset == "local-qt-debug"
                && profile.buildPresets[0].declaredTargets.size() == 2
                && profile.buildPresets[0].declaredTargets[0] == "all_qmllint"
                && profile.buildPresets[0].declaredTargets[1] == "all"
                && profile.buildPresets[0].allowedTargetsConfirmed
                && profile.buildPresets[0].allowedTargets
                    == std::vector<std::string>({"all", "all_qmllint"}),
            "Linked build preset did not survive reopen");
        require(snapshot.unlinkedBuildPresets.size() == 1
                && snapshot.unlinkedBuildPresets[0].name == "orphan-build"
                && snapshot.unlinkedBuildPresets[0].source == VerificationPresetFileSource::Unknown,
            "Unlinked build preset did not survive reopen");
    }
}

void testFindForRunReturnsNulloptWhenAbsent()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    const auto found = repo.findForRun("run-1");
    require(found.success() && !found.value().has_value(),
        "findForRun must return nullopt, not an error, when nothing was recorded");
}

void testRecordSnapshotRejectsDuplicateRunId()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
        "Could not record first verification environment snapshot");

    auto duplicate = makeSnapshot("run-1", "verification-environment-project");
    duplicate.baselineFingerprint = "should-never-be-visible";
    const auto rejected = repo.recordSnapshot(duplicate);
    require(!rejected.success(), "A duplicate runId record must fail, not replace");
    require(rejected.error()->code == "verification_environment.already_recorded",
        "Expected the already_recorded failure code");

    const auto found = repo.findForRun("run-1");
    require(found.success() && found.value().has_value()
            && found.value()->baselineFingerprint == "baseline-fp-1",
        "The original recorded snapshot must remain untouched by a rejected duplicate");
}

void testRecordedSnapshotRowCannotBeUpdated()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
        "Could not record verification environment snapshot");

    sqlite3* raw = openRaw(workspace.database);
    const int result = sqlite3_exec(raw,
        "UPDATE verification_environment_snapshots SET state = 'setup_required' WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr);
    require(result != SQLITE_OK, "The immutability trigger did not reject an UPDATE");
    sqlite3_close(raw);
}

void testFindForRunFailsOnCorruptProfilesColumn()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
        "Could not record verification environment snapshot");

    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_environment_snapshots SET profiles_json = '{not valid json' "
        "WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt profiles_json column");
    sqlite3_close(raw);

    const auto found = repo.findForRun("run-1");
    require(!found.success(), "findForRun must fail on a corrupt profiles_json column");
}

void testFindForRunFailsClosedOnSchemaVersionMismatch()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
        "Could not record verification environment snapshot");

    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_environment_snapshots SET schema_version = 'some-future-schema.v2' "
        "WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not overwrite schema_version column");
    sqlite3_close(raw);

    const auto found = repo.findForRun("run-1");
    require(!found.success(),
        "findForRun must fail closed on a snapshot whose schema_version does not match the current contract");
}

void testFindForRunFailsClosedOnStateContentMismatch()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    // ExistingProfile with a non-empty profiles list, then corrupted to
    // claim SetupRequired while the profiles_json content is left untouched
    // - state and content must agree, or the row is corrupt.
    require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
        "Could not record verification environment snapshot");

    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_environment_snapshots SET state = 'setup_required' WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt state column");
    sqlite3_close(raw);

    const auto found = repo.findForRun("run-1");
    require(!found.success(),
        "findForRun must fail closed when state and stored profile content disagree");
}

void testFindForRunFailsClosedOnOversizedProfilesArray()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
        "Could not record verification environment snapshot");

    // Build a well-formed but out-of-bound profiles array (one more entry
    // than kVerificationEnvironmentMaxProfiles allows).
    std::ostringstream oversized;
    oversized << "[";
    for (std::size_t i = 0; i <= kVerificationEnvironmentMaxProfiles; ++i) {
        if (i != 0) oversized << ",";
        oversized << R"({"profileId":"p)" << i << R"(","configurePresetName":"p)" << i
                   << R"(","configureDisplayName":"","configureSource":"unknown",)"
                   << R"("declaredBinaryDir":"","resolvedBinaryDir":"","origin":"discovered",)"
                   << R"("buildPresetsConfirmed":true,"buildPresets":[]})";
    }
    oversized << "]";

    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(raw,
        "UPDATE verification_environment_snapshots SET profiles_json = ? WHERE run_id = 'run-1';",
        -1, &statement, nullptr) == SQLITE_OK, "Could not prepare oversized-profiles UPDATE");
    const std::string payload = oversized.str();
    require(sqlite3_bind_text(statement, 1, payload.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK,
        "Could not bind oversized profiles_json payload");
    require(sqlite3_step(statement) == SQLITE_DONE, "Could not write oversized profiles_json payload");
    sqlite3_finalize(statement);
    sqlite3_close(raw);

    const auto found = repo.findForRun("run-1");
    require(!found.success(),
        "findForRun must fail closed on a profiles array exceeding the bounded cap");
}

void testUpgradeAddsVerificationEnvironmentSnapshotsTable()
{
    // AG-18.1 hardening: a fresh SqliteDatabase::open() already migrates a
    // brand-new file straight to the current version, so it alone never
    // proves the v40 step specifically. This test opens fully, seeds
    // pre-existing data, then rolls back ONLY the v40-created objects (its
    // table/trigger/index - nothing else v40 touches) and its version
    // marker via a raw connection, and reopens through the real code path -
    // genuinely re-triggering just the v39->v40 migration step and proving
    // it both adds the table back and leaves unrelated pre-existing data
    // (the seeded manifest_task_executions row) untouched.
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "verification-environment-project");
        SqliteManifestTaskExecutionRepository taskExecutions(opened.value());
        ManifestTaskState state;
        state.status = ManifestTaskStatus::Completed;
        require(taskExecutions.upsert("run-1", "task-a", state).success(),
            "Could not seed a manifest task execution row");
    }
    {
        sqlite3* raw = openRaw(workspace.database);
        require(sqlite3_exec(raw,
            "DROP TRIGGER verification_environment_snapshots_reject_update;\n"
            "DROP INDEX idx_verification_environment_snapshots_project;\n"
            "DROP TABLE verification_environment_snapshots;\n"
            "PRAGMA user_version = 39;",
            nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not roll back the database to a pre-v40 shape for the test");
        sqlite3_close(raw);
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen after re-running the v40 upgrade");
        SqliteVerificationEnvironmentRepository repo(reopened.value());
        require(repo.recordSnapshot(makeSnapshot("run-1", "verification-environment-project")).success(),
            "Could not record a verification environment snapshot after the v40 upgrade re-ran");
        // The immutability trigger must also have been recreated, not just
        // the bare table.
        sqlite3* raw = openRaw(workspace.database);
        const int result = sqlite3_exec(raw,
            "UPDATE verification_environment_snapshots SET state = 'setup_required' WHERE run_id = 'run-1';",
            nullptr, nullptr, nullptr);
        require(result != SQLITE_OK, "The re-run v40 migration must recreate the immutability trigger");
        sqlite3_close(raw);
        SqliteManifestTaskExecutionRepository taskExecutions(reopened.value());
        const auto rows = taskExecutions.listForRun("run-1");
        require(rows.success() && rows.value().size() == 1,
            "Pre-existing manifest_task_executions row did not survive the re-run v40 upgrade");
    }
}

} // namespace

// AG-17.0: allowedTargetsComplete is additive. It round-trips, and a row
// written before the field existed reads as NOT complete (unknown), never as
// complete.
void testAllowedTargetsCompleteRoundTripsAndLegacyRowsReadAsUnknown()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    seedRun(opened.value(), "run-2", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());

    auto complete = makeSnapshot("run-1", "verification-environment-project");
    complete.profiles[0].buildPresets[0].allowedTargetsComplete = true;
    auto incomplete = makeSnapshot("run-2", "verification-environment-project");
    incomplete.profiles[0].buildPresets[0].allowedTargetsComplete = false;
    require(repo.recordSnapshot(complete).success() && repo.recordSnapshot(incomplete).success(),
        "Could not record snapshots");

    const auto readComplete = repo.findForRun("run-1");
    const auto readIncomplete = repo.findForRun("run-2");
    require(readComplete.success() && readComplete.value().has_value()
            && readComplete.value()->profiles[0].buildPresets[0].allowedTargetsComplete,
        "allowedTargetsComplete=true did not survive a round trip");
    require(readIncomplete.success() && readIncomplete.value().has_value()
            && !readIncomplete.value()->profiles[0].buildPresets[0].allowedTargetsComplete,
        "allowedTargetsComplete=false did not survive a round trip");

    // Strip the key from run-1's stored JSON: the pre-AG-17.0 row shape.
    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_environment_snapshots SET profiles_json = "
        "replace(profiles_json, '\"allowedTargetsComplete\":true,', '') WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not strip allowedTargetsComplete");
    sqlite3_close(raw);
    const auto legacy = repo.findForRun("run-1");
    require(legacy.success() && legacy.value().has_value(),
        "A pre-AG-17.0 row without allowedTargetsComplete must still read");
    require(!legacy.value()->profiles[0].buildPresets[0].allowedTargetsComplete
            && legacy.value()->profiles[0].buildPresets[0].allowedTargetsConfirmed,
        "A legacy row must read as confirmed but NOT provably complete");
}

void testCompleteWithoutConfirmedIsCorrupt()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "verification-environment-project");
    SqliteVerificationEnvironmentRepository repo(opened.value());
    auto snapshot = makeSnapshot("run-1", "verification-environment-project");
    snapshot.profiles[0].buildPresets[0].allowedTargetsComplete = true;
    require(repo.recordSnapshot(snapshot).success(), "Could not record snapshot");

    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_environment_snapshots SET profiles_json = "
        "replace(replace(profiles_json, '\"allowedTargetsConfirmed\":true', "
        "'\"allowedTargetsConfirmed\":false'), '\"allowedTargets\":[\"all\",\"all_qmllint\"],', "
        "'\"allowedTargets\":[],') WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not make the row self-contradictory");
    sqlite3_close(raw);
    require(!repo.findForRun("run-1").success(),
        "A row claiming a complete but unconfirmed catalog must fail closed");
}

int runVerificationEnvironmentSnapshotRepositoryTests()
{
    testRecordSnapshotRoundTripsAndReopens();
    testFindForRunReturnsNulloptWhenAbsent();
    testRecordSnapshotRejectsDuplicateRunId();
    testRecordedSnapshotRowCannotBeUpdated();
    testFindForRunFailsOnCorruptProfilesColumn();
    testFindForRunFailsClosedOnSchemaVersionMismatch();
    testFindForRunFailsClosedOnStateContentMismatch();
    testFindForRunFailsClosedOnOversizedProfilesArray();
    testUpgradeAddsVerificationEnvironmentSnapshotsTable();
    testAllowedTargetsCompleteRoundTripsAndLegacyRowsReadAsUnknown();
    testCompleteWithoutConfirmedIsCorrupt();
    return 0;
}
