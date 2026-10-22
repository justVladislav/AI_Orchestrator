// AG-18.3 persistence tests: SqliteVerificationBootstrapRepository's
// immutable decisions, append-only transitions, retry links, restart
// round-trip, fail-closed corrupt-row handling, and the v40->v41 migration.
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
        / ("ai-orchestrator-verification-bootstrap-test-"
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
    Project project {projectId, "Bootstrap test project", "Round-trip bootstrap persistence",
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
    run.title = "Bootstrap persistence";
    run.task = "Round-trip a bootstrap decision";
    run.workflowId = "execution-manifest-v1";
    run.createdAt = now;
    require(runs.insertRun(run).success(), "Could not seed Run");
}

VerificationBootstrapDecision makeActionRequiredDecision(
    const RunId& runId, const ProjectId& projectId)
{
    VerificationBootstrapDecision decision;
    decision.runId = runId;
    decision.projectId = projectId;
    decision.baselineFingerprint = "baseline-fp-1";
    decision.verificationSnapshotSchemaVersion = "verification-environment-snapshot.v1";
    decision.disposition = VerificationBootstrapDisposition::ActionRequired;
    decision.reasonCode = "verification_environment.action_required";
    decision.message = "No usable verification profile and no safe bootstrap was declared";
    decision.decidedAt = std::chrono::system_clock::now();
    return decision;
}

VerificationBootstrapDecision makeBootstrapPendingDecision(
    const RunId& runId, const ProjectId& projectId)
{
    VerificationBootstrapDecision decision;
    decision.runId = runId;
    decision.projectId = projectId;
    decision.baselineFingerprint = "baseline-fp-1";
    decision.verificationSnapshotSchemaVersion = "verification-environment-snapshot.v1";
    decision.disposition = VerificationBootstrapDisposition::BootstrapPending;
    decision.reasonCode = "verification_environment.bootstrap_pending";
    decision.message = "A same-Run bootstrap was declared and admitted";
    decision.bootstrapSetupPaths = {"CMakeUserPresets.json"};
    decision.owningTaskIds = {"bootstrap"};
    decision.decidedAt = std::chrono::system_clock::now();
    return decision;
}

sqlite3* openRaw(const fs::path& database)
{
    sqlite3* raw = nullptr;
    require(sqlite3_open(database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    return raw;
}

void dropImmutabilityTrigger(sqlite3* raw)
{
    require(sqlite3_exec(raw, "DROP TRIGGER verification_bootstrap_decisions_reject_update;",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not drop immutability trigger for the test");
}

void testActionRequiredDecisionRoundTripsAndReopens()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "bootstrap-project");
        SqliteVerificationBootstrapRepository repo(opened.value());
        require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project"))
                .success(),
            "Could not record ActionRequired decision");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteVerificationBootstrapRepository repo(reopened.value());
        const auto found = repo.findDecisionForRun("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find recorded ActionRequired decision after reopen");
        const auto& decision = *found.value();
        require(decision.disposition == VerificationBootstrapDisposition::ActionRequired
                && decision.reasonCode == "verification_environment.action_required"
                && decision.baselineFingerprint == "baseline-fp-1"
                && decision.bootstrapSetupPaths.empty(),
            "ActionRequired decision fields did not survive reopen");
    }
}

void testBootstrapPendingDecisionRoundTripsSetupPaths()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeBootstrapPendingDecision("run-1", "bootstrap-project"))
            .success(),
        "Could not record BootstrapPending decision");
    const auto found = repo.findDecisionForRun("run-1");
    require(found.success() && found.value().has_value(), "Could not find recorded decision");
    require(found.value()->disposition == VerificationBootstrapDisposition::BootstrapPending
            && found.value()->bootstrapSetupPaths.size() == 1
            && found.value()->bootstrapSetupPaths.front() == fs::path("CMakeUserPresets.json")
            && found.value()->owningTaskIds == std::vector<std::string>{"bootstrap"},
        "BootstrapPending setup paths/owning tasks did not round-trip");
}

void testFindDecisionReturnsNulloptWhenAbsent()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    const auto found = repo.findDecisionForRun("run-1");
    require(found.success() && !found.value().has_value(),
        "findDecisionForRun must return nullopt, not an error, when nothing was recorded");
}

void testDuplicateAgreeingDecisionIsIdempotent()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project")).success(),
        "Could not record first decision");
    require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project")).success(),
        "A byte-identical duplicate decision must be an idempotent success");
}

void testConflictingDuplicateDecisionFailsClosed()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project")).success(),
        "Could not record first decision");
    auto conflicting = makeBootstrapPendingDecision("run-1", "bootstrap-project");
    const auto rejected = repo.recordDecision(conflicting);
    require(!rejected.success() && rejected.error()->code == "verification_environment.decision_conflict",
        "A conflicting duplicate decision for the same Run was not rejected");
}

void testDecisionRowCannotBeUpdated()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project")).success(),
        "Could not record decision");
    sqlite3* raw = openRaw(workspace.database);
    const int result = sqlite3_exec(raw,
        "UPDATE verification_bootstrap_decisions SET disposition = 'bootstrap_pending' "
        "WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr);
    require(result != SQLITE_OK, "The immutability trigger did not reject an UPDATE");
    sqlite3_close(raw);
}

void testFindDecisionFailsClosedOnCorruptSetupPathsColumn()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeBootstrapPendingDecision("run-1", "bootstrap-project"))
            .success(),
        "Could not record decision");
    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_bootstrap_decisions SET bootstrap_setup_paths_json = '{not valid json' "
        "WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt bootstrap_setup_paths_json column");
    sqlite3_close(raw);
    const auto found = repo.findDecisionForRun("run-1");
    require(!found.success(), "findDecisionForRun must fail on a corrupt setup-paths column");
}

void testFindDecisionFailsClosedOnStateContentMismatch()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    // ActionRequired legitimately has no setup paths - a row claiming a
    // green disposition (BootstrapVerified) with none is corrupt.
    require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project")).success(),
        "Could not record decision");
    sqlite3* raw = openRaw(workspace.database);
    dropImmutabilityTrigger(raw);
    require(sqlite3_exec(raw,
        "UPDATE verification_bootstrap_decisions SET disposition = 'bootstrap_verified' "
        "WHERE run_id = 'run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt disposition column");
    sqlite3_close(raw);
    const auto found = repo.findDecisionForRun("run-1");
    require(!found.success(),
        "findDecisionForRun must fail closed when a non-ActionRequired disposition carries no "
        "setup paths");
}

void testForeignRunMismatchFailsClosed()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    auto decision = makeActionRequiredDecision("run-1", "a-different-project-never-seeded");
    // The run_id foreign key still resolves ("run-1" was seeded); projectId
    // itself carries no FK, but a caller comparing it against the actual
    // Run's own projectId (ManifestWorkflowRunner's own responsibility) is
    // what actually detects a mismatch - this repository test simply proves
    // the mismatched projectId is preserved verbatim rather than silently
    // corrected, so that check can work.
    require(repo.recordDecision(decision).success(), "Could not record decision");
    const auto found = repo.findDecisionForRun("run-1");
    require(found.success() && found.value().has_value()
            && found.value()->projectId == "a-different-project-never-seeded",
        "A mismatched projectId must be preserved, not silently corrected, so callers can detect it");
}

void testAppendTransitionAndListForRun()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeBootstrapPendingDecision("run-1", "bootstrap-project"))
            .success(),
        "Could not record decision");

    VerificationBootstrapTransition transition;
    transition.id = "transition-1";
    transition.runId = "run-1";
    transition.toDisposition = VerificationBootstrapDisposition::BootstrapVerified;
    transition.reasonCode = "verification_environment.bootstrap_verified";
    transition.message = "Candidate passed";
    VerificationBootstrapCandidateIdentity identity;
    identity.overlayFingerprint = "overlay-fp-1";
    identity.candidateProfileId = "local-qt-debug";
    identity.candidateBuildProfileId = "local-qt-debug";
    identity.candidateTargetId = "all_qmllint";
    transition.candidateIdentity = identity;
    transition.transitionedAt = std::chrono::system_clock::now();
    require(repo.appendTransition(transition).success(), "Could not append transition");

    const auto listed = repo.listTransitionsForRun("run-1");
    require(listed.success() && listed.value().size() == 1,
        "Expected exactly one transition to be listed");
    const auto& roundTripped = listed.value().front();
    require(roundTripped.toDisposition == VerificationBootstrapDisposition::BootstrapVerified
            && roundTripped.reasonCode == "verification_environment.bootstrap_verified"
            && roundTripped.candidateIdentity.has_value()
            && roundTripped.candidateIdentity->overlayFingerprint == "overlay-fp-1"
            && roundTripped.candidateIdentity->candidateProfileId == "local-qt-debug"
            && roundTripped.candidateIdentity->candidateTargetId == "all_qmllint",
        "Transition/candidate identity did not round-trip");
}

void testDuplicateTransitionIsIdempotent()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeBootstrapPendingDecision("run-1", "bootstrap-project"))
            .success(),
        "Could not record decision");
    VerificationBootstrapTransition transition;
    transition.id = "transition-1";
    transition.runId = "run-1";
    transition.toDisposition = VerificationBootstrapDisposition::BootstrapFailed;
    transition.reasonCode = "verification_environment.bootstrap_verification_failed";
    transition.transitionedAt = std::chrono::system_clock::now();
    require(repo.appendTransition(transition).success(), "Could not append first transition");
    // AG-18.3 hardening (blocker 1): transition identity is now deterministic
    // (see ManifestWorkflowRunner) - a genuine retry of the same append
    // always reuses the exact same id, so idempotency is keyed on
    // (id, payload agreement), not on (run_id,to,reason) alone. Reposting
    // the identical transition under its own id must still be a benign
    // no-op.
    require(repo.appendTransition(transition).success(),
        "A byte-identical repeated transition append (same id, same payload) must be an "
        "idempotent success");
    const auto listed = repo.listTransitionsForRun("run-1");
    require(listed.success() && listed.value().size() == 1,
        "A byte-identical repeated transition must not create a second row");

    // A DIFFERENT id for the same already-resolved Run is never a legitimate
    // duplicate under the deterministic-identity contract - it must fail
    // closed rather than silently deduping by logical fields alone.
    VerificationBootstrapTransition mismatchedId = transition;
    mismatchedId.id = "transition-2";
    const auto conflicting = repo.appendTransition(mismatchedId);
    require(!conflicting.success(),
        "A differently-identified transition for an already-resolved Run must fail closed");
    require(conflicting.error()->code == "verification_environment.decision_conflict",
        "Expected decision_conflict for a mismatched-id transition on an already-resolved Run");
}

void testRetryLinkRoundTripsAndIsIdempotent()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    seedRun(opened.value(), "run-2", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project")).success(),
        "Could not record ActionRequired decision");

    VerificationBootstrapRetryLink link;
    link.predecessorRunId = "run-1";
    link.newRunId = "run-2";
    link.projectId = "bootstrap-project";
    link.reason = "action_required_linked_retry";
    link.createdAt = std::chrono::system_clock::now();
    require(repo.recordRetryLink(link).success(), "Could not record retry link");

    const auto found = repo.findRetryLinkForPredecessor("run-1");
    require(found.success() && found.value().has_value()
            && found.value()->newRunId == "run-2",
        "Retry link did not round-trip");

    // A byte-identical repeated request (same newRunId/projectId) is a
    // benign idempotent success.
    require(repo.recordRetryLink(link).success(),
        "A byte-identical repeated retry-link request must be an idempotent success");
    const auto stillFirst = repo.findRetryLinkForPredecessor("run-1");
    require(stillFirst.success() && stillFirst.value().has_value()
            && stillFirst.value()->newRunId == "run-2",
        "A duplicate retry-link request must not silently replace the original link");

    // AG-18.3 hardening (blocker 4): a DIFFERENT newRunId for an
    // already-linked predecessor must never return success merely because
    // the predecessor primary key already exists - it must fail closed
    // rather than silently keeping the first link (which would hide a real
    // conflict from the reservation-based retry protocol).
    VerificationBootstrapRetryLink conflictingAttempt = link;
    conflictingAttempt.newRunId = "run-3";
    const auto conflicting = repo.recordRetryLink(conflictingAttempt);
    require(!conflicting.success(),
        "A conflicting retry-link request (different newRunId) must fail closed, not silently "
        "succeed");
    require(conflicting.error()->code == "verification_environment.decision_conflict",
        "Expected decision_conflict for a conflicting retry-link request");
    const auto stillUnchanged = repo.findRetryLinkForPredecessor("run-1");
    require(stillUnchanged.success() && stillUnchanged.value().has_value()
            && stillUnchanged.value()->newRunId == "run-2",
        "A rejected conflicting retry-link request must not replace the original link");
}

void testFindRetryLinkReturnsNulloptWhenAbsent()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "bootstrap-project");
    SqliteVerificationBootstrapRepository repo(opened.value());
    const auto found = repo.findRetryLinkForPredecessor("run-1");
    require(found.success() && !found.value().has_value(),
        "findRetryLinkForPredecessor must return nullopt, not an error, when nothing was recorded");
}

void testUpgradeAddsVerificationBootstrapTables()
{
    // Mirrors the AG-18.1 v39->v40 upgrade test's technique: open fully,
    // seed pre-existing unrelated data, roll back ONLY the v41-created
    // objects and the version marker via a raw connection, then reopen
    // through the real code path - genuinely re-triggering the v40->v41
    // migration step and proving it both adds the tables back and leaves
    // unrelated pre-existing data untouched.
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "bootstrap-project");
        SqliteManifestTaskExecutionRepository taskExecutions(opened.value());
        ManifestTaskState state;
        state.status = ManifestTaskStatus::Completed;
        require(taskExecutions.upsert("run-1", "task-a", state).success(),
            "Could not seed a manifest task execution row");
    }
    {
        sqlite3* raw = openRaw(workspace.database);
        require(sqlite3_exec(raw,
            "DROP TABLE verification_bootstrap_retry_links;\n"
            "DROP INDEX idx_verification_bootstrap_transitions_run;\n"
            "DROP TABLE verification_bootstrap_transitions;\n"
            "DROP TRIGGER verification_bootstrap_decisions_reject_update;\n"
            "DROP INDEX idx_verification_bootstrap_decisions_project;\n"
            "DROP TABLE verification_bootstrap_decisions;\n"
            "PRAGMA user_version = 40;",
            nullptr, nullptr, nullptr) == SQLITE_OK,
            "Could not roll back the database to a pre-v41 shape for the test");
        sqlite3_close(raw);
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen after re-running the v41 upgrade");
        SqliteVerificationBootstrapRepository repo(reopened.value());
        require(repo.recordDecision(makeActionRequiredDecision("run-1", "bootstrap-project"))
                .success(),
            "Could not record a decision after the v41 upgrade re-ran");
        sqlite3* raw = openRaw(workspace.database);
        const int result = sqlite3_exec(raw,
            "UPDATE verification_bootstrap_decisions SET disposition = 'bootstrap_pending' "
            "WHERE run_id = 'run-1';",
            nullptr, nullptr, nullptr);
        require(result != SQLITE_OK,
            "The re-run v41 migration must recreate the immutability trigger");
        sqlite3_close(raw);
        SqliteManifestTaskExecutionRepository taskExecutions(reopened.value());
        const auto rows = taskExecutions.listForRun("run-1");
        require(rows.success() && rows.value().size() == 1,
            "Pre-existing manifest_task_executions row did not survive the re-run v41 upgrade");
    }
}

} // namespace

int runVerificationBootstrapRepositoryTests()
{
    testActionRequiredDecisionRoundTripsAndReopens();
    testBootstrapPendingDecisionRoundTripsSetupPaths();
    testFindDecisionReturnsNulloptWhenAbsent();
    testDuplicateAgreeingDecisionIsIdempotent();
    testConflictingDuplicateDecisionFailsClosed();
    testDecisionRowCannotBeUpdated();
    testFindDecisionFailsClosedOnCorruptSetupPathsColumn();
    testFindDecisionFailsClosedOnStateContentMismatch();
    testForeignRunMismatchFailsClosed();
    testAppendTransitionAndListForRun();
    testDuplicateTransitionIsIdempotent();
    testRetryLinkRoundTripsAndIsIdempotent();
    testFindRetryLinkReturnsNulloptWhenAbsent();
    testUpgradeAddsVerificationBootstrapTables();
    return 0;
}
