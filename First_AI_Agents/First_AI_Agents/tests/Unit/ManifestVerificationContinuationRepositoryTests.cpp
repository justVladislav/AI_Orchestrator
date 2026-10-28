#include "ai/application/verification/ManifestVerificationAttributionService.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
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
        / ("ai-orchestrator-verification-continuation-test-"
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
    Project project {projectId, "Verification continuation test project",
        "Round-trip verification-continuation checkpoint persistence",
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
    run.title = "Verification continuation persistence";
    run.task = "Round-trip a verification-continuation checkpoint";
    run.workflowId = "execution-manifest-v1";
    run.status = RunStatus::Failed;
    run.createdAt = now;
    require(runs.insertRun(run).success(), "Could not seed Run");
}

ManifestVerificationContinuationCheckpoint makeAdmitted(const RunId& runId, const ProjectId& projectId)
{
    ManifestVerificationContinuationCheckpoint checkpoint;
    checkpoint.id = "verification-continuation:" + runId;
    checkpoint.sourceRunId = runId;
    checkpoint.sourceProjectId = projectId;
    checkpoint.status = ManifestVerificationContinuationStatus::Admitted;
    checkpoint.createdAt = std::chrono::system_clock::now();
    return checkpoint;
}

ManifestVerificationContinuationInput makeInput(
    const RunId& sourceRunId, const RunId& childRunId, const ProjectId& projectId)
{
    ManifestVerificationContinuationInput input;
    input.sourceRunId = sourceRunId;
    input.childRunId = childRunId;
    input.sourceProjectId = projectId;
    input.attributionId = "verification-attribution:" + sourceRunId;
    input.baselineFingerprint = std::string(64, 'a');
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = std::string(64, 'b');
    input.orderedProposalIds = {"proposal-1"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = std::chrono::system_clock::now();
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    return input;
}

void testInsertRoundTripsAndReopens()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open database");
        seedRun(opened.value(), "run-1", "continuation-project");
        SqliteManifestVerificationContinuationRepository repo(opened.value());
        require(repo.insert(makeAdmitted("run-1", "continuation-project")).success(),
            "Could not insert checkpoint");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Database did not reopen");
        SqliteManifestVerificationContinuationRepository repo(reopened.value());
        const auto found = repo.findBySourceRun("run-1");
        require(found.success() && found.value().has_value(),
            "Could not find checkpoint after reopen");
        const auto& checkpoint = *found.value();
        require(checkpoint.id == "verification-continuation:run-1",
            "id did not survive reopen");
        require(checkpoint.sourceProjectId == "continuation-project",
            "sourceProjectId did not survive reopen");
        require(checkpoint.status == ManifestVerificationContinuationStatus::Admitted,
            "status did not survive reopen");
        require(checkpoint.denialCode.empty() && checkpoint.denialMessage.empty(),
            "An Admitted checkpoint must not carry a denial code/message");
    }
}

void testDeniedCheckpointRoundTrips()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "continuation-project");
    SqliteManifestVerificationContinuationRepository repo(opened.value());

    ManifestVerificationContinuationCheckpoint denied = makeAdmitted("run-1", "continuation-project");
    denied.status = ManifestVerificationContinuationStatus::Denied;
    denied.denialCode = "verification_continuation.pre_freeze_failure";
    denied.denialMessage = "The source Run never froze a Manifest";
    require(repo.insert(denied).success(), "Could not insert denied checkpoint");

    const auto found = repo.findBySourceRun("run-1");
    require(found.success() && found.value().has_value(), "Could not find denied checkpoint");
    require(found.value()->status == ManifestVerificationContinuationStatus::Denied,
        "Denied status did not survive round trip");
    require(found.value()->denialCode == "verification_continuation.pre_freeze_failure",
        "denialCode did not survive round trip");
    require(found.value()->denialMessage == "The source Run never froze a Manifest",
        "denialMessage did not survive round trip");
}

void testFindBySourceRunReturnsEmptyWhenMissing()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    SqliteManifestVerificationContinuationRepository repo(opened.value());
    const auto found = repo.findBySourceRun("no-such-run");
    require(found.success() && !found.value().has_value(),
        "A missing checkpoint must return an empty optional, not an error");
}

void testInsertRejectsDuplicateSourceRun()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "continuation-project");
    SqliteManifestVerificationContinuationRepository repo(opened.value());
    require(repo.insert(makeAdmitted("run-1", "continuation-project")).success(),
        "Could not insert first checkpoint");

    auto duplicate = makeAdmitted("run-1", "continuation-project");
    duplicate.status = ManifestVerificationContinuationStatus::Denied;
    duplicate.denialCode = "should-never-be-visible";
    const auto rejected = repo.insert(duplicate);
    require(!rejected.success(), "A duplicate sourceRunId insert must fail, not replace");
    require(rejected.error()->code == "manifest_verification_continuation.already_exists",
        "Expected the already_exists failure code");

    const auto found = repo.findBySourceRun("run-1");
    require(found.success() && found.value().has_value()
            && found.value()->status == ManifestVerificationContinuationStatus::Admitted,
        "The original checkpoint must remain untouched by a rejected duplicate insert");
}

void testCheckpointRowCannotBeUpdated()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open database");
    seedRun(opened.value(), "run-1", "continuation-project");
    SqliteManifestVerificationContinuationRepository repo(opened.value());
    require(repo.insert(makeAdmitted("run-1", "continuation-project")).success(),
        "Could not insert checkpoint");

    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw sqlite3 connection");
    const int result = sqlite3_exec(raw,
        "UPDATE manifest_verification_continuations SET status = 'denied' "
        "WHERE source_run_id = 'run-1';",
        nullptr, nullptr, nullptr);
    require(result != SQLITE_OK, "The immutability trigger did not reject an UPDATE");
    sqlite3_close(raw);
}

ManifestVerificationAttributionCheckpoint makeAttribution(
    const RunId& runId, const ProjectId& projectId)
{
    ManifestVerificationAttributionCheckpoint checkpoint;
    checkpoint.id = "verification-attribution:" + runId;
    checkpoint.sourceRunId = runId;
    checkpoint.sourceProjectId = projectId;
    checkpoint.status = ManifestVerificationAttributionStatus::Attributed;
    checkpoint.baselineFingerprint = "baseline-1";
    checkpoint.sourceOverlayFingerprint = "overlay-1";
    ManifestVerificationFindingAttribution finding;
    finding.diagnosticCode = "verification.qml_warning";
    finding.requirementId = "qml-lint";
    finding.relativePath = "qml/app.qml";
    finding.line = 10;
    finding.column = 3;
    finding.tag = "unqualified";
    finding.kind = ManifestVerificationFindingAttributionKind::SourceProposalOwner;
    finding.ownerTaskId = "component";
    finding.ownerProposalId = "cs-1";
    checkpoint.findings.push_back(std::move(finding));
    checkpoint.createdAt = std::chrono::system_clock::now();
    return checkpoint;
}

void testAttributionRoundTripsAcrossReopenAndRejectsDuplicate()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open attribution database");
        seedRun(opened.value(), "run-1", "continuation-project");
        SqliteManifestVerificationContinuationRepository repo(opened.value());
        require(repo.insertAttribution(makeAttribution("run-1", "continuation-project")).success(),
            "Could not insert attribution checkpoint");
        const auto duplicate = repo.insertAttribution(makeAttribution("run-1", "continuation-project"));
        require(!duplicate.success()
                && duplicate.error()->code == "manifest_verification_attribution.already_exists",
            "A duplicate attribution must fail without replacing the original");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Attribution database did not reopen");
        SqliteManifestVerificationContinuationRepository repo(reopened.value());
        const auto found = repo.findAttributionBySourceRun("run-1");
        require(found.success() && found.value().has_value(),
            "Attribution was not durable across reopen");
        const auto& value = *found.value();
        require(value.status == ManifestVerificationAttributionStatus::Attributed
                && value.baselineFingerprint == "baseline-1"
                && value.sourceOverlayFingerprint == "overlay-1"
                && value.findings.size() == 1,
            "Attribution identity did not round-trip");
        require(value.findings.front().relativePath == "qml/app.qml"
                && value.findings.front().ownerTaskId == "component"
                && value.findings.front().ownerProposalId == "cs-1",
            "Typed finding attribution did not round-trip");
    }
}

void testAttributionRowCannotBeUpdated()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open attribution database");
    seedRun(opened.value(), "run-1", "continuation-project");
    SqliteManifestVerificationContinuationRepository repo(opened.value());
    require(repo.insertAttribution(makeAttribution("run-1", "continuation-project")).success(),
        "Could not insert attribution checkpoint");
    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw attribution sqlite connection");
    const int result = sqlite3_exec(raw,
        "UPDATE manifest_verification_attributions SET status = 'blocked' "
        "WHERE source_run_id = 'run-1';", nullptr, nullptr, nullptr);
    require(result != SQLITE_OK, "The attribution immutability trigger did not reject UPDATE");
    sqlite3_close(raw);
}

void testV48ToV49MigrationAndCorruptFindingFailClosed()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not create current-schema fixture");
        seedRun(opened.value(), "run-1", "continuation-project");
    }
    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw migration fixture");
    require(sqlite3_exec(raw,
        "DROP TABLE manifest_verification_attributions; PRAGMA user_version = 48;",
        nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not rewind fixture to the genuine v48 shape");
    sqlite3_close(raw);

    const auto upgraded = SqliteDatabase::open(workspace.database);
    require(upgraded.success(), "The v48 database did not upgrade to v49");
    SqliteManifestVerificationContinuationRepository repo(upgraded.value());
    require(repo.insertAttribution(makeAttribution("run-1", "continuation-project")).success(),
        "The migrated attribution table is not usable");

    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not reopen raw corruption fixture");
    require(sqlite3_exec(raw,
        "DROP TRIGGER manifest_verification_attributions_reject_update; "
        "UPDATE manifest_verification_attributions SET findings_json='{}' "
        "WHERE source_run_id='run-1';",
        nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not inject a corrupt attribution row");
    sqlite3_close(raw);
    const auto corrupt = repo.findAttributionBySourceRun("run-1");
    require(!corrupt.success()
            && corrupt.error()->code == "manifest_verification_attribution.corrupt_findings",
        "A corrupt persisted attribution must fail closed");
}

void testContinuationInputRoundTripsReopensAndIsImmutable()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open continuation-input database");
        seedRun(opened.value(), "source-run", "continuation-project");
        SqliteManifestVerificationContinuationRepository repo(opened.value());
        require(repo.insertInput(makeInput(
            "source-run", "child-run", "continuation-project")).success(),
            "Could not insert continuation input");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Continuation-input database did not reopen");
        SqliteManifestVerificationContinuationRepository repo(reopened.value());
        const auto found = repo.findInputBySourceRun("source-run");
        require(found.success() && found.value().has_value()
                && found.value()->childRunId == "child-run"
                && found.value()->plannerRequests == 0
                && found.value()->orderedProposalIds == std::vector<ChangeSetId>{"proposal-1"},
            "Continuation input did not round-trip exactly");
        const auto listed = repo.listInputs();
        require(listed.success() && listed.value().size() == 1,
            "Continuation input was not listed after reopen");

        sqlite3* raw = nullptr;
        require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
            "Could not open raw input database");
        const int updated = sqlite3_exec(raw,
            "UPDATE manifest_verification_continuation_inputs SET planner_requests=1;",
            nullptr, nullptr, nullptr);
        require(updated != SQLITE_OK, "Continuation input UPDATE was not rejected");
        sqlite3_close(raw);
    }
}

void testContinuationInputRejectsDuplicateAndCorruptFingerprint()
{
    Workspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    require(opened.success(), "Could not open input validation database");
    seedRun(opened.value(), "source-run", "continuation-project");
    SqliteManifestVerificationContinuationRepository repo(opened.value());
    auto input = makeInput("source-run", "child-run", "continuation-project");
    require(repo.insertInput(input).success(), "Could not seed continuation input");
    const auto duplicate = repo.insertInput(input);
    require(!duplicate.success()
            && duplicate.error()->code
                == "manifest_verification_continuation.input_already_exists",
        "Duplicate continuation input did not fail with its stable code");

    auto corrupt = makeInput("source-run-2", "child-run-2", "continuation-project");
    corrupt.inputFingerprint = std::string(64, '0');
    const auto rejected = repo.insertInput(corrupt);
    require(!rejected.success()
            && rejected.error()->code
                == "manifest_verification_continuation.invalid_input_fingerprint",
        "A mismatched continuation fingerprint was accepted");
}

void testV49ToV50MigrationCreatesUsableInputTable()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not create current-schema fixture");
        seedRun(opened.value(), "source-run", "continuation-project");
    }
    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw v49 fixture");
    require(sqlite3_exec(raw,
        "DROP TABLE manifest_verification_continuation_inputs; PRAGMA user_version=49;",
        nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not rewind fixture to v49");
    sqlite3_close(raw);
    const auto upgraded = SqliteDatabase::open(workspace.database);
    require(upgraded.success(), "v49 database did not upgrade to v50");
    SqliteManifestVerificationContinuationRepository repo(upgraded.value());
    require(repo.insertInput(makeInput(
        "source-run", "child-run", "continuation-project")).success(),
        "Migrated v50 continuation-input table is unusable");
}

// MW-P5.6C restart integration test: proves the whole real-SQLite chain
// (Manifest + frozen AG-15 RunWorkspaceScope + Work Ledger + admission, all
// through the actual repositories, closed and reopened to simulate a real
// process restart) resolves ownership via the frozen scope fallback when the
// Manifest's own path-role fields carry no owner for the path - exactly the
// shape a Manifest persisted before dc0391a's fix would read back as (see
// ManifestVerificationAttributionService.hpp's class comment). The "component"
// task deliberately has an EMPTY ownedPaths to simulate that data loss; only
// the separately-frozen, immutable run_workspace_scopes row still knows
// "component" owns qml/App.qml, since that row was derived from the Manifest
// while it still had the field, before any persistence round-trip.
void testAttributionServiceUsesFrozenScopeFallbackAfterRestart()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open scope-fallback database");
        seedRun(opened.value(), "source-run", "continuation-project");

        ExecutionAcceptanceRequirement requirement;
        requirement.id = "qml-lint";
        requirement.description = "QML lint is clean";
        requirement.verificationKind = ExecutionVerificationKind::QmlLint;
        requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
        ExecutionAcceptanceContract contract;
        contract.requirements.push_back(requirement);

        // Simulates a Manifest that lost its AG-15 ownedPaths for this task
        // (the exact shape the pre-dc0391a persistence bug produced).
        ExecutionManifestTask component;
        component.id = "component";
        component.requestedCapability = WorkspaceCapability::Propose;
        ExecutionManifestTask integration;
        integration.id = "integration";
        integration.ownedPaths = {"qml/CMakeLists.txt"};
        integration.acceptanceContract = contract;
        integration.requestedCapability = WorkspaceCapability::Propose;
        ExecutionManifest manifest;
        manifest.runId = "source-run";
        manifest.tasks = {component, integration};
        SqliteExecutionManifestRepository manifests(opened.value());
        require(manifests.insert(manifest).success(), "Could not seed scope-fallback Manifest");

        RunWorkspaceScopeEntry entry;
        entry.relativePath = "qml/app.qml";
        entry.role = WorkspaceScopeRole::OwnedWrite;
        entry.reason = "Declared as owned_write by task component";
        entry.taskId = "component";
        entry.baselineHash = std::string(64, 'a');
        entry.origin = WorkspaceScopeOrigin::FrozenInitial;
        RunWorkspaceScopeEntry integrationEntry;
        integrationEntry.relativePath = "qml/cmakelists.txt";
        integrationEntry.role = WorkspaceScopeRole::OwnedWrite;
        integrationEntry.reason = "Declared as owned_write by task integration";
        integrationEntry.taskId = "integration";
        integrationEntry.baselineHash = std::string(64, 'b');
        integrationEntry.origin = WorkspaceScopeOrigin::FrozenInitial;
        RunWorkspaceScope scope;
        scope.runId = "source-run";
        scope.manifestSchemaVersion = manifest.schemaVersion;
        scope.entries = {entry, integrationEntry};
        scope.frozenAt = std::chrono::system_clock::now();
        SqliteRunWorkspaceScopeRepository workspaceScopes(opened.value());
        require(workspaceScopes.freeze(scope).success(), "Could not freeze scope-fallback scope");

        ManifestProposalSetVerification candidate;
        candidate.workspaceIdentity = "workspace-1";
        candidate.baselineFingerprint = "baseline-1";
        candidate.orderedProposalIds = {"cs-component", "cs-integration"};
        candidate.orderedProposalContentHashes = {"hash-component", "hash-integration"};
        candidate.overlayFingerprint = "candidate-overlay";
        candidate.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
        candidate.buildLintTestStatus = ExecutionVerificationStatus::Failed;
        candidate.acceptanceStatus = ExecutionVerificationStatus::Failed;
        candidate.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
        ManifestVerificationDiagnostic diagnostic;
        diagnostic.code = "verification.qml_warning";
        diagnostic.message = "QML lint reported a warning";
        diagnostic.requirementId = "qml-lint";
        diagnostic.structuredFindings.push_back(
            {"<candidate>/qml/App.qml", 12, 4, "unqualified", "Unqualified access"});
        candidate.diagnostics.push_back(std::move(diagnostic));

        ManifestWorkLedgerEntry entry2;
        entry2.runId = "source-run";
        entry2.projectId = "continuation-project";
        entry2.terminalStatus = RunStatus::Failed;
        entry2.disposition = ManifestWorkLedgerDisposition::ManifestReady;
        entry2.verification = candidate;
        entry2.taskDispositions = {
            {"component", "succeeded", ChangeSetId("cs-component"), std::nullopt},
            {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}};
        entry2.createdAt = std::chrono::system_clock::now();
        SqliteManifestWorkLedgerRepository ledger(opened.value());
        require(ledger.insertEntry(entry2).success(), "Could not seed scope-fallback Work Ledger");

        SqliteManifestVerificationContinuationRepository continuations(opened.value());
        require(continuations.insert(makeAdmitted("source-run", "continuation-project")).success(),
            "Could not seed scope-fallback admission");
    }

    const auto reopened = SqliteDatabase::open(workspace.database);
    require(reopened.success(), "Scope-fallback database did not reopen");
    SqliteExecutionManifestRepository manifests(reopened.value());
    SqliteManifestWorkLedgerRepository ledger(reopened.value());
    SqliteManifestVerificationContinuationRepository continuations(reopened.value());
    SqliteRunWorkspaceScopeRepository workspaceScopes(reopened.value());
    SqliteManifestVerificationBaselineNormalizationRepository baselineNormalizations(reopened.value());
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);

    ManifestProposalSetVerification baseline;
    baseline.workspaceIdentity = "workspace-1";
    baseline.baselineFingerprint = "baseline-1";
    baseline.overlayFingerprint = "baseline-overlay";
    baseline.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    baseline.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    baseline.acceptanceStatus = ExecutionVerificationStatus::Passed;
    baseline.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Passed, {}});

    ManifestProposalSetVerification candidateAgain;
    candidateAgain.workspaceIdentity = "workspace-1";
    candidateAgain.baselineFingerprint = "baseline-1";
    candidateAgain.orderedProposalIds = {"cs-component", "cs-integration"};
    candidateAgain.orderedProposalContentHashes = {"hash-component", "hash-integration"};
    candidateAgain.overlayFingerprint = "candidate-overlay";
    candidateAgain.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    candidateAgain.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    candidateAgain.acceptanceStatus = ExecutionVerificationStatus::Failed;
    candidateAgain.criteria.push_back(
        {"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnosticAgain;
    diagnosticAgain.code = "verification.qml_warning";
    diagnosticAgain.message = "QML lint reported a warning";
    diagnosticAgain.requirementId = "qml-lint";
    diagnosticAgain.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 12, 4, "unqualified", "Unqualified access"});
    candidateAgain.diagnostics.push_back(std::move(diagnosticAgain));

    const auto result = attribution.attributeOrEvaluate("source-run", baseline, candidateAgain);
    require(result.success(), "attributeOrEvaluate failed against the reopened database");
    require(result.value().status == ManifestVerificationAttributionStatus::Attributed,
        "A Manifest that lost its owner for this path must still attribute via the frozen "
        "workspace scope fallback after a restart");
    require(result.value().findings.size() == 1
            && result.value().findings.front().ownerTaskId == "component"
            && result.value().findings.front().ownerProposalId == "cs-component",
        "The scope-fallback owner must be the real frozen owner, not left unattributed");
}

ManifestVerificationBaselineNormalization makeBaselineNormalization(
    const RunId& runId)
{
    ManifestVerificationBaselineNormalization normalization;
    normalization.id = "verification-baseline-normalization:" + runId;
    normalization.sourceRunId = runId;
    normalization.policyId = "legacy-normalization.v1";
    normalization.durableBaselineFingerprint = std::string(64, 'a');
    normalization.normalizedBaselineFingerprint = std::string(64, 'b');
    normalization.excludedPrefixes = {".qtcreator/"};
    normalization.normalizedPaths = {".qtcreator/CMakeLists.txt.user"};
    normalization.createdAt = std::chrono::system_clock::now();
    return normalization;
}

void testBaselineNormalizationRoundTripsAcrossReopenRejectsDuplicateAndIsImmutable()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not open baseline-normalization database");
        seedRun(opened.value(), "run-1", "continuation-project");
        SqliteManifestVerificationBaselineNormalizationRepository repo(opened.value());
        require(repo.insert(makeBaselineNormalization("run-1")).success(),
            "Could not insert baseline-normalization proof");
        const auto duplicate = repo.insert(makeBaselineNormalization("run-1"));
        require(!duplicate.success()
                && duplicate.error()->code
                    == "manifest_verification_baseline_normalization.already_exists",
            "A duplicate baseline-normalization proof must fail without replacing the original");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        require(reopened.success(), "Baseline-normalization database did not reopen");
        SqliteManifestVerificationBaselineNormalizationRepository repo(reopened.value());
        const auto found = repo.findBySourceRun("run-1");
        require(found.success() && found.value().has_value(),
            "Baseline-normalization proof was not durable across reopen");
        const auto& value = *found.value();
        require(value.policyId == "legacy-normalization.v1"
                && value.durableBaselineFingerprint == std::string(64, 'a')
                && value.normalizedBaselineFingerprint == std::string(64, 'b')
                && value.excludedPrefixes == std::vector<std::string> {".qtcreator/"}
                && value.normalizedPaths
                    == std::vector<std::string> {".qtcreator/CMakeLists.txt.user"},
            "Baseline-normalization proof identity did not round-trip");

        const auto missing = repo.findBySourceRun("no-such-run");
        require(missing.success() && !missing.value().has_value(),
            "A missing baseline-normalization proof must return an empty optional, not an error");

        sqlite3* raw = nullptr;
        require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
            "Could not open raw baseline-normalization connection");
        const int updateResult = sqlite3_exec(raw,
            "UPDATE manifest_verification_baseline_normalizations SET policy_id='tampered' "
            "WHERE source_run_id='run-1';", nullptr, nullptr, nullptr);
        require(updateResult != SQLITE_OK,
            "The baseline-normalization immutability trigger did not reject UPDATE");
        sqlite3_close(raw);
    }
}

void testBaselineNormalizationRejectsAuthorizationIdentityDrift()
{
    auto wrongId = makeBaselineNormalization("run-1");
    wrongId.id = "arbitrary-id";
    require(!validateManifestVerificationBaselineNormalization(wrongId).success(),
        "A normalization proof id must be deterministically bound to sourceRunId");

    auto wrongPolicy = makeBaselineNormalization("run-1");
    wrongPolicy.policyId = "future-or-provider-policy";
    require(!validateManifestVerificationBaselineNormalization(wrongPolicy).success(),
        "An unknown normalization policy must fail closed");

    auto wrongPrefixes = makeBaselineNormalization("run-1");
    wrongPrefixes.excludedPrefixes = {".qtcreator/", ".idea/"};
    require(!validateManifestVerificationBaselineNormalization(wrongPrefixes).success(),
        "A proof may name only the exact Backend-owned prefix set");

    auto malformedFingerprint = makeBaselineNormalization("run-1");
    malformedFingerprint.durableBaselineFingerprint = "not-a-sha256";
    require(!validateManifestVerificationBaselineNormalization(malformedFingerprint).success(),
        "Both normalization fingerprints must be canonical SHA-256 identities");

    auto noTransition = makeBaselineNormalization("run-1");
    noTransition.normalizedBaselineFingerprint = noTransition.durableBaselineFingerprint;
    require(!validateManifestVerificationBaselineNormalization(noTransition).success(),
        "A normalization proof must represent a real fingerprint transition");

    auto unorderedPaths = makeBaselineNormalization("run-1");
    unorderedPaths.normalizedPaths = {
        ".qtcreator/z.user", ".qtcreator/a.user"};
    require(!validateManifestVerificationBaselineNormalization(unorderedPaths).success(),
        "Normalized evidence paths must be unique and deterministically ordered");
}

void testV50UpgradeCreatesNormalizationAndAcceptsPreparedChildRuns()
{
    Workspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        require(opened.success(), "Could not create the v51 rewind fixture");
        seedRun(opened.value(), "run-1", "continuation-project");
    }
    sqlite3* raw = nullptr;
    require(sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw v50 fixture");
    require(sqlite3_exec(raw,
        "DROP TRIGGER IF EXISTS manifest_verification_baseline_normalizations_reject_update;"
        "DROP TABLE manifest_verification_baseline_normalizations;"
        "PRAGMA user_version=50;", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not rewind fixture to the genuine v50 shape");
    sqlite3_close(raw);

    const auto upgraded = SqliteDatabase::open(workspace.database);
    require(upgraded.success(), "v50 database did not upgrade to v51");
    SqliteManifestVerificationBaselineNormalizationRepository repo(upgraded.value());
    require(repo.insert(makeBaselineNormalization("run-1")).success(),
        "Migrated v51 normalization table is unusable");
    const auto found = repo.findBySourceRun("run-1");
    require(found.success() && found.value().has_value(),
        "A proof did not round-trip through the migrated v51 table");

    SqliteRunRepository runs(upgraded.value());
    Run child;
    child.id = "prepared-child";
    child.projectId = "continuation-project";
    child.parentRunId = "run-1";
    child.title = "Prepared verification continuation";
    child.task = "Await explicit paid repair";
    child.workflowId = kManifestVerificationContinuationWorkflowId;
    child.status = RunStatus::Prepared;
    child.createdAt = std::chrono::system_clock::now();
    require(runs.insertRun(child).success(),
        "Migrated runs table still rejects the durable Prepared child status");
    const auto preparedChild = runs.findRun(child.id);
    require(preparedChild.success() && preparedChild.value().has_value()
            && preparedChild.value()->status == RunStatus::Prepared,
        "Prepared child status did not round-trip through the v52 runs table");
}

} // namespace

int runManifestVerificationContinuationRepositoryTests()
{
    testInsertRoundTripsAndReopens();
    testDeniedCheckpointRoundTrips();
    testFindBySourceRunReturnsEmptyWhenMissing();
    testInsertRejectsDuplicateSourceRun();
    testCheckpointRowCannotBeUpdated();
    testAttributionRoundTripsAcrossReopenAndRejectsDuplicate();
    testAttributionRowCannotBeUpdated();
    testV48ToV49MigrationAndCorruptFindingFailClosed();
    testContinuationInputRoundTripsReopensAndIsImmutable();
    testContinuationInputRejectsDuplicateAndCorruptFingerprint();
    testV49ToV50MigrationCreatesUsableInputTable();
    testAttributionServiceUsesFrozenScopeFallbackAfterRestart();
    testBaselineNormalizationRoundTripsAcrossReopenRejectsDuplicateAndIsImmutable();
    testBaselineNormalizationRejectsAuthorizationIdentityDrift();
    testV50UpgradeCreatesNormalizationAndAcceptsPreparedChildRuns();
    return 0;
}
