#include <ai/application/proposals/ChangeProposalService.hpp>
#include <ai/application/proposals/ChangeProposalTypes.hpp>
#include <ai/application/ExecutionAcceptanceContract.hpp>
#include <ai/application/ExecutionAnalyticsService.hpp>
#include <ai/application/ExecutionManifestReadModelService.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>
#include <ai/application/IManifestWorkLedgerRepository.hpp>
#include <ai/application/InMemoryExecutionManifestRepository.hpp>
#include <ai/application/InMemoryManifestTaskExecutionRepository.hpp>
#include <ai/application/InMemoryManifestWorkLedgerRepository.hpp>
#include <ai/application/InMemoryRepositories.hpp>
#include <ai/application/InMemoryVerificationBootstrapRepository.hpp>
#include <ai/application/InMemoryVerificationEnvironmentRepository.hpp>
#include <ai/application/IRunRepository.hpp>
#include <ai/application/IWorkflowRunner.hpp>
#include <ai/application/ManifestApplyVerificationGate.hpp>
#include <ai/application/ManifestProposalContentHash.hpp>
#include <ai/application/ManifestRouting.hpp>
#include <ai/application/ManifestProposalOverlay.hpp>
#include <ai/application/ManifestWorkLedgerTypes.hpp>
#include <ai/application/MockWorkflowRunner.hpp>
#include <ai/application/OrchestrationService.hpp>
#include <ai/application/RunManager.hpp>
#include <ai/application/Sha256.hpp>
#include <ai/application/VerificationBootstrapRetryService.hpp>
#include <ai/application/WorkflowRouter.hpp>
#include <ai/application/WorkspaceBaselineProvenance.hpp>
#include <ai/application/WorkspaceReadPolicy.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/AgentSessionController.hpp>
#include <ai/ui/AgentProposalListModel.hpp>
#include <ai/ui/ArtifactListModel.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/ComparisonResultListModel.hpp>
#include <ai/ui/ComparisonSessionListModel.hpp>
#include <ai/ui/ExecutionAnalyticsController.hpp>
#include <ai/ui/ManifestController.hpp>
#include <ai/ui/ManifestVerificationManualHandoffController.hpp>
#include <ai/ui/ProjectListModel.hpp>
#include <ai/ui/ProjectMemoryController.hpp>
#include <ai/ui/ProviderCatalogController.hpp>
#include <ai/ui/RunContractVariant.hpp>
#include <ai/ui/RunListModel.hpp>
#include <ai/ui/RunStageModel.hpp>
#include "../../test_support/QtTestSupport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QVariantList>
#include <QVariantMap>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <limits>
#include <optional>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// One isolated temp directory per test: a project root SqliteDatabase can
// point ProjectService at, plus a "state/orchestrator.sqlite" path BackendBridge
// opens through its explicit-path constructor. Tests must never touch the real
// per-user database BackendBridge's default constructor resolves via
// QStandardPaths::AppDataLocation.
struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("aio-qt-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";

    TempWorkspace() { fs::create_directories(projectPath); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    [[nodiscard]] QString projectPathQt() const { return QString::fromStdWString(projectPath.wstring()); }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }

    // PC-4C: a BackendBridge is the ONE live owner of the database file. The second, short-lived runtime that a fixture seeds the file
    // through while the bridge is alive is therefore a GUEST: it takes no ownership lock and writes no Outcome Brief.
    [[nodiscard]] ai::backend::BackendRuntimeConfig guestRuntimeConfig() const
    {
        ai::backend::BackendRuntimeConfig config {databasePath};
        config.exclusiveDatabaseOwnership = false;
        return config;
    }
};

QString statusForRun(QAbstractItemModel* model, const QString& runId)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, RunListModel::RunIdRole).toString() == runId) {
            return model->data(index, RunListModel::StatusRole).toString();
        }
    }
    return {};
}

QString waitForStatus(
    RunController& controller,
    const QString& runId,
    const QStringList& expected,
    std::chrono::milliseconds timeout = 4s)
{
    ai::ui::test_support::waitForCondition("Run status", [&] {
        return expected.contains(statusForRun(controller.model(), runId));
    }, [&] {
        return ("runId=" + runId + ", status="
            + statusForRun(controller.model(), runId)).toStdString();
    }, timeout);
    return statusForRun(controller.model(), runId);
}

bool waitForRowCount(
    QAbstractItemModel* model,
    int expected,
    std::chrono::milliseconds timeout = 4s)
{
    return ai::ui::test_support::waitForCondition("Model row count", [&] {
        return model->rowCount() == expected;
    }, [&] {
        return "expected=" + std::to_string(expected)
            + ", actual=" + std::to_string(model->rowCount());
    }, timeout);
}

QString statusForComparisonSession(QAbstractItemModel* model, const QString& sessionId)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, ComparisonSessionListModel::SessionIdRole).toString()
            == sessionId) {
            return model->data(index, ComparisonSessionListModel::StatusRole).toString();
        }
    }
    return {};
}

QString waitForComparisonStatus(
    ComparisonController& controller,
    const QString& sessionId,
    const QStringList& expected,
    std::chrono::milliseconds timeout = 4s)
{
    ai::ui::test_support::waitForCondition("Comparison status", [&] {
        return expected.contains(statusForComparisonSession(
            controller.sessionsModel(), sessionId));
    }, [&] {
        return ("sessionId=" + sessionId + ", status="
            + statusForComparisonSession(controller.sessionsModel(), sessionId)).toStdString();
    }, timeout);
    return statusForComparisonSession(controller.sessionsModel(), sessionId);
}

// ---------------------------------------------------------------------
// AG-18.4A hardening: ManifestController's retry-after-setup and
// fail-closed error surfacing. Entirely in-memory (no SQLite/BackendBridge
// needed) - RunManager/WorkflowRouter/OrchestrationService are pure
// Application-layer, so this exercises the real Qt controller wired to a
// real (if in-memory) VerificationBootstrapRetryService/
// ExecutionManifestReadModelService, exactly the same composition
// BackendRuntime performs, just without SQLite.
// ---------------------------------------------------------------------
struct ManifestControllerHarness {
    std::filesystem::path workspacePath {std::filesystem::temp_directory_path()
        / ("ai-orchestrator-manifest-controller-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    ai::application::InMemoryProjectRepository projects;
    ai::application::InMemoryRunRepository runs;
    ai::application::InMemoryProviderRepository providers;
    ai::application::InMemoryModelRepository models;
    ai::application::InMemoryExecutionProfileRepository profiles;
    ai::application::InMemoryExecutionManifestRepository manifestRepository;
    ai::application::InMemoryManifestTaskExecutionRepository taskExecutions;
    ai::application::InMemoryVerificationEnvironmentRepository verificationEnvironmentRepository;
    ai::application::InMemoryVerificationBootstrapRepository verificationBootstrapRepository;
    // QML-lint advisory-severity policy (2026-09-28): the same
    // IManifestWorkLedgerRepository ExecutionManifestReadModelService now
    // optionally reads for its verificationAdvisory projection.
    ai::application::InMemoryManifestWorkLedgerRepository workLedger;
    // AG-17.7B: the ledger the member usage reconstruction reads.
    ai::application::InMemoryModelExecutionRepository modelExecutions;
    ai::application::MockWorkflowRunner mockRunner {1ms, 1};
    ai::application::WorkflowRouter router {profiles, providers, models};
    ai::application::RunManager manager {projects, runs, router};
    ai::application::OrchestrationService orchestration {projects, runs, manager};
    ai::application::ExecutionManifestService manifests {manifestRepository, runs};
    ai::application::ExecutionManifestReadModelService readModel {
        manifests, taskExecutions, nullptr, nullptr,
        &verificationEnvironmentRepository, &verificationBootstrapRepository, &workLedger,
        &providers, &models, &modelExecutions};
    ai::application::VerificationBootstrapRetryService retryService {
        verificationBootstrapRepository, runs, projects, orchestration};
    ManifestController controller {readModel, retryService};

    ManifestControllerHarness()
    {
        std::filesystem::create_directories(workspacePath);
        router.registerRunner(ai::application::kMockWorkflowId, mockRunner);
    }
    ~ManifestControllerHarness()
    {
        std::error_code ignored;
        std::filesystem::remove_all(workspacePath, ignored);
    }

    void writePreset() const
    {
        std::ofstream stream(workspacePath / "CMakeUserPresets.json", std::ios::binary);
        stream << "{\"version\":6,\"configurePresets\":[],\"buildPresets\":[]}";
        assert(stream.good());
    }

    ai::application::ProjectId makeProject(bool withPreset = true)
    {
        if (withPreset) writePreset();
        ai::application::Project project;
        project.name = "Manifest controller test project";
        project.description = "Test project";
        project.rootPath = workspacePath;
        const auto created = projects.insert(project);
        assert(created.success());
        return created.value().id;
    }

    ai::application::RunId seedActionRequiredRun(const ai::application::ProjectId& projectId)
    {
        ai::application::StartRunRequest request;
        request.projectId = projectId;
        request.title = "Predecessor needing manual setup";
        request.task = "Build the project";
        request.workflowId = ai::application::kMockWorkflowId;
        const auto started = orchestration.startRun(request);
        assert(started.success());
        const ai::application::RunId runId = started.value();
        ai::ui::test_support::waitForCondition("Predecessor Run reaches a terminal state", [&] {
            const auto found = runs.findRun(runId);
            return found.success() && found.value().has_value()
                && (found.value()->status == ai::application::RunStatus::Completed
                    || found.value()->status == ai::application::RunStatus::Failed);
        }, [] { return std::string("predecessor Run never finished"); }, 4s);

        ai::application::VerificationEnvironmentSnapshot snapshot;
        snapshot.runId = runId;
        snapshot.projectId = projectId;
        snapshot.baselineFingerprint = "baseline-fp";
        snapshot.state = ai::application::VerificationEnvironmentState::SetupRequired;
        snapshot.diagnostic =
            "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline";
        snapshot.discoveredAt = std::chrono::system_clock::now();
        assert(verificationEnvironmentRepository.recordSnapshot(snapshot).success());

        ai::application::VerificationBootstrapDecision decision;
        decision.runId = runId;
        decision.projectId = projectId;
        decision.baselineFingerprint = "baseline-fp";
        decision.verificationSnapshotSchemaVersion =
            std::string(ai::application::kVerificationEnvironmentSnapshotSchemaVersion);
        decision.disposition = ai::application::VerificationBootstrapDisposition::ActionRequired;
        decision.reasonCode = "verification_environment.action_required";
        decision.message = "No usable profile and no safe bootstrap was declared";
        decision.decidedAt = std::chrono::system_clock::now();
        assert(verificationBootstrapRepository.recordDecision(decision).success());
        return runId;
    }
};

// The rendered button text this test asserts against is exactly what
// ManifestPanel.qml renders - see that file's own "Retry after setup"
// RunActionButton. Kept in one named constant here so a future rename must
// update both intentionally rather than by accident.
constexpr const char* kExpectedRetryAfterSetupButtonText = "Retry after setup";

// A.1 (AG-18.4A hardening): a Run with a durable ActionRequired decision
// shows an eligible, active "Retry after setup" action, and invoking it
// creates a new, distinct Run linked to the predecessor via parentRunId -
// never something merely labeled a "scoped setup Run" (see
// ManifestController's own doc comment; AG-18.4C, the actual setup-Run
// launcher, is not implemented).
void testManifestControllerActionRequiredShowsActiveRetryAndCreatesLinkedRun()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = harness.seedActionRequiredRun(projectId);

    assert(harness.controller.selectRun(QString::fromStdString(predecessorId)));
    assert(harness.controller.errorText().isEmpty());
    assert(harness.controller.hasVerificationEnvironment());
    assert(harness.controller.verificationEnvironmentState() == QStringLiteral("setup_required"));
    assert(harness.controller.hasBootstrapDecision());
    assert(harness.controller.canRetryAfterSetup());
    assert(harness.controller.canRetryAfterSetupReason().isEmpty());
    // Never eligible/labeled as a "create setup Run" action. Creation-time
    // preset setup exists, while existing-Project configuration from Run
    // Details remains separately disabled.
    assert(!harness.controller.canConfigureBackendProfile());
    assert(!harness.controller.canConfigureBackendProfileReasonCode().isEmpty());

    const QString newRunId = harness.controller.retryAfterSetup();
    assert(!newRunId.isEmpty());
    assert(newRunId != QString::fromStdString(predecessorId));
    assert(harness.controller.errorText().isEmpty());

    const auto newRun = harness.runs.findRun(newRunId.toStdString());
    assert(newRun.success() && newRun.value().has_value());
    assert(newRun.value()->parentRunId.has_value()
        && *newRun.value()->parentRunId == predecessorId);
}

void testManifestControllerRetrySurfacesSetupStillRequiredWithoutCreatingRun()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject(false);
    const auto predecessorId = harness.seedActionRequiredRun(projectId);
    assert(harness.controller.selectRun(QString::fromStdString(predecessorId)));
    assert(harness.controller.canRetryAfterSetup());

    const auto before = harness.runs.listRuns(projectId);
    assert(before.success());
    const QString refused = harness.controller.retryAfterSetup();
    assert(refused.isEmpty());
    assert(harness.controller.errorText().startsWith(
        QStringLiteral("verification_environment.setup_still_required:")));
    const auto after = harness.runs.listRuns(projectId);
    assert(after.success() && after.value().size() == before.value().size());

    harness.writePreset();
    const QString created = harness.controller.retryAfterSetup();
    assert(!created.isEmpty());
    assert(harness.controller.errorText().isEmpty());
}

// A.2 (AG-18.4A hardening): a durable bootstrap decision whose own identity
// does not match its Run's verification-environment snapshot must fail the
// whole refresh() closed - the controller must never silently show "no
// bootstrap decision" or otherwise hide the corruption.
void testManifestControllerSurfacesReadModelCorruptionError()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject();
    const ai::application::RunId runId = "corrupt-run-1";
    ai::application::Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "Corrupt identity test run";
    run.task = "Exercise controller-level fail-closed surfacing";
    run.workflowId = "execution-manifest-v1";
    assert(harness.runs.insertRun(run).success());

    ai::application::VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = projectId;
    snapshot.baselineFingerprint = "baseline-fp";
    snapshot.state = ai::application::VerificationEnvironmentState::SetupRequired;
    snapshot.discoveredAt = std::chrono::system_clock::now();
    assert(harness.verificationEnvironmentRepository.recordSnapshot(snapshot).success());

    ai::application::VerificationBootstrapDecision mismatched;
    mismatched.runId = runId;
    mismatched.projectId = projectId;
    // Deliberately wrong: does not match the snapshot's own
    // baselineFingerprint above.
    mismatched.baselineFingerprint = "a-completely-different-baseline";
    mismatched.verificationSnapshotSchemaVersion =
        std::string(ai::application::kVerificationEnvironmentSnapshotSchemaVersion);
    mismatched.disposition = ai::application::VerificationBootstrapDisposition::ActionRequired;
    mismatched.reasonCode = "verification_environment.action_required";
    mismatched.decidedAt = std::chrono::system_clock::now();
    assert(harness.verificationBootstrapRepository.recordDecision(mismatched).success());

    const bool refreshed = harness.controller.selectRun(QString::fromStdString(runId));
    assert(!refreshed);
    assert(harness.controller.errorText().startsWith(
        QStringLiteral("verification_environment.corrupt_bootstrap_record:")));
    // The corruption must clear stale state, never leave a half-populated
    // projection behind.
    assert(!harness.controller.hasVerificationEnvironment());
    assert(!harness.controller.hasBootstrapDecision());
    assert(!harness.controller.canRetryAfterSetup());
}

// QML-lint advisory-severity policy (2026-09-28): the real Qt controller,
// wired to a real (in-memory) IManifestWorkLedgerRepository, projects the
// advisory summary/list from ExecutionManifestReadModelService's own
// verificationAdvisory exactly - proves the end-to-end Backend-to-QML wiring,
// not only the pure read-model unit test.
void testManifestControllerProjectsVerificationAdvisories()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject();

    ai::application::Run run;
    run.id = "run-advisory";
    run.projectId = projectId;
    run.title = "Advisory controller test run";
    run.task = "Exercise the advisory projection";
    run.workflowId = "execution-manifest-v1";
    assert(harness.runs.insertRun(run).success());

    ai::application::ExecutionManifestTask task;
    task.id = "component";
    task.objective = "Owns AlertsView.qml";
    task.role = "worker";
    task.requestedCapability = ai::application::WorkspaceCapability::Propose;
    ai::application::ExecutionManifest manifest;
    manifest.runId = run.id;
    manifest.tasks = {task};
    assert(harness.manifests.freeze(manifest, ai::application::ExecutionManifestPolicy {}).success());

    ai::application::ManifestVerificationDiagnosticFinding finding;
    finding.fileKey = "<candidate>/qml/AlertsView.qml";
    finding.line = 2;
    finding.column = 1;
    finding.tag = "unqualified";
    finding.text = "Unqualified access";
    ai::application::ManifestVerificationDiagnostic advisory;
    advisory.code = "verification.qml_warning_advisory";
    advisory.message = "advisory";
    advisory.requirementId = "lint";
    advisory.structuredFindings = {finding};

    ai::application::ManifestProposalSetVerification verification;
    // harness.makeProject() does not guarantee a non-blank generated id in
    // this in-memory fixture; verificationAdvisory never cross-checks this
    // field against the Run's real projectId (that identity binding is
    // AG-16's own job, not this read-model projection's), so any bounded
    // non-blank literal is sufficient and safe here.
    verification.workspaceIdentity = "workspace-1";
    verification.baselineFingerprint = "baseline-1";
    verification.overlayFingerprint = "overlay-1";
    verification.proposalSafetyStatus = ai::application::ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ai::application::ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ai::application::ExecutionVerificationStatus::Passed;
    verification.diagnostics = {advisory};
    verification.criteria = {
        {"lint", true, ai::application::ExecutionVerificationStatus::Passed, {}}};

    ai::application::ManifestWorkLedgerEntry entry;
    entry.runId = run.id;
    entry.projectId = projectId;
    entry.terminalStatus = ai::application::RunStatus::Completed;
    entry.disposition = ai::application::ManifestWorkLedgerDisposition::ManifestReady;
    entry.verification = verification;
    assert(harness.workLedger.insertEntry(entry).success());

    assert(harness.controller.selectRun(QString::fromStdString(run.id)));
    assert(harness.controller.errorText().isEmpty());
    assert(harness.controller.hasManifest());
    assert(harness.controller.hasVerificationAdvisory());
    assert(harness.controller.verificationAdvisoryPassed());
    assert(harness.controller.hasAdvisories());
    assert(harness.controller.advisoryCount() == 1);
    const auto advisories = harness.controller.advisories();
    assert(advisories.size() == 1);
    const auto item = advisories.front().toMap();
    assert(item.value("relativePath").toString() == QStringLiteral("qml/AlertsView.qml"));
    assert(item.value("tag").toString() == QStringLiteral("unqualified"));
    assert(item.value("line").toInt() == 2);
}

/// AG-17.6: ManifestController projects the frozen routing identity exactly as the
// Backend read model classified it - the roster members with their participation
// (an unused worker is a normal state, not an error) and each task's assignee.
void testManifestControllerProjectsRosterRoutingAndUnusedWorkers()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject();

    ai::application::Provider provider;
    provider.id = "p-qt";
    provider.name = "Qt Provider";
    provider.kind = "openai";
    assert(harness.providers.insert(provider).success());
    ai::application::Model model;
    model.id = "m-qt";
    model.providerId = provider.id;
    model.apiModelId = "qt-api";
    model.displayName = "Qt Model";
    assert(harness.models.insert(model).success());

    const auto member = [&](const char* id, const char* role, bool lead, bool workspace,
                            std::uint32_t position) {
        ai::application::StageModelAssignment value;
        value.stageType = id;
        value.role = role;
        value.leadArchitect = lead;
        value.workspaceAccess = workspace;
        value.providerId = provider.id;
        value.modelId = model.id;
        value.position = position;
        value.timeout = std::chrono::milliseconds(60000);
        return value;
    };
    const auto routing = ai::application::ManifestRouting::fromAssignments({
        member("architect", "Lead Architect", true, false, 0),
        member("qml-worker", "QML", false, true, 1),
        member("cpp-worker", "C++", false, true, 2)});

    ai::application::Run run;
    run.id = "run-roster-routing";
    run.projectId = projectId;
    run.title = "Roster routing controller test";
    run.task = "Exercise the routing projection";
    run.workflowId = "execution-manifest-v1";
    assert(harness.runs.insertRun(run).success());

    ai::application::ExecutionManifestTask task;
    task.id = "ui";
    task.objective = "Build the view";
    task.role = "worker";
    task.requestedCapability = ai::application::WorkspaceCapability::Propose;
    task.assigneeMemberId = "qml-worker";
    ai::application::ExecutionManifest manifest;
    manifest.runId = run.id;
    manifest.routing = routing;
    manifest.tasks = {task};
    assert(harness.manifests.freeze(manifest, ai::application::ExecutionManifestPolicy {}).success());

    assert(harness.controller.selectRun(QString::fromStdString(run.id)));
    assert(harness.controller.errorText().isEmpty());
    assert(harness.controller.hasManifest());
    assert(harness.controller.routingMode() == QStringLiteral("roster"));

    const auto members = harness.controller.routingMembers();
    assert(members.size() == 3);
    const auto byId = [&](const QString& memberId) {
        for (const auto& entry : members) {
            if (entry.toMap().value("memberId").toString() == memberId) return entry.toMap();
        }
        return QVariantMap {};
    };
    assert(byId("architect").value("participation").toString() == QStringLiteral("lead"));
    const auto qml = byId("qml-worker");
    assert(qml.value("participation").toString() == QStringLiteral("assigned"));
    assert(qml.value("assignedTaskCount").toUInt() == 1);
    assert(qml.value("label").toString() == QStringLiteral("Qt Provider / Qt Model"));
    assert(qml.value("role").toString() == QStringLiteral("QML"));
    const auto cpp = byId("cpp-worker");
    // An available worker without a task is Unused: a plain state, never a failure.
    assert(cpp.value("participation").toString() == QStringLiteral("unused"));
    assert(cpp.value("assignedTaskCount").toUInt() == 0);

    const auto tasks = harness.controller.tasks();
    assert(tasks.size() == 1);
    const auto taskMap = tasks.front().toMap();
    assert(taskMap.value("assigneeMemberId").toString() == QStringLiteral("qml-worker"));
    assert(taskMap.value("assigneeRole").toString() == QStringLiteral("QML"));
    assert(taskMap.value("assigneeLabel").toString() == QStringLiteral("Qt Provider / Qt Model"));

    // A Run without a Manifest resets the projection (no stale roster is shown).
    assert(harness.controller.selectRun(QString {}));
    assert(harness.controller.routingMode() == QStringLiteral("none"));
    assert(harness.controller.routingMembers().isEmpty());
}

/// AG-17.7B: ManifestController projects the Backend's member usage reconstruction
// verbatim - a nested `usage` map per member (with the price state that tells a precise
// total from a lower bound from an unknown), the Run total, and the unattributed rows as
// diagnostic debt. Nothing is summed or priced in the controller or in QML.
void testManifestControllerProjectsMemberUsageAndTheUnattributedDebt()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject();

    ai::application::Provider provider;
    provider.id = "p-usage";
    provider.name = "Usage Provider";
    provider.kind = "openai";
    assert(harness.providers.insert(provider).success());
    ai::application::Model model;
    model.id = "m-usage";
    model.providerId = provider.id;
    model.apiModelId = "usage-api";
    model.displayName = "Usage Model";
    assert(harness.models.insert(model).success());

    const auto member = [&](const char* id, const char* role, bool lead, bool workspace,
                            std::uint32_t position) {
        ai::application::StageModelAssignment value;
        value.stageType = id;
        value.role = role;
        value.leadArchitect = lead;
        value.workspaceAccess = workspace;
        value.providerId = provider.id;
        value.modelId = model.id;
        value.position = position;
        value.timeout = std::chrono::milliseconds(60000);
        return value;
    };
    ai::application::Run run;
    run.id = "run-member-usage";
    run.projectId = projectId;
    run.title = "Member usage controller test";
    run.task = "Exercise the usage projection";
    run.workflowId = "execution-manifest-v1";
    assert(harness.runs.insertRun(run).success());

    ai::application::ExecutionManifestTask task;
    task.id = "ui";
    task.objective = "Build the view";
    task.role = "worker";
    task.requestedCapability = ai::application::WorkspaceCapability::Propose;
    task.assigneeMemberId = "qml-worker";
    ai::application::ExecutionManifest manifest;
    manifest.runId = run.id;
    manifest.routing = ai::application::ManifestRouting::fromAssignments({
        member("architect", "Lead Architect", true, false, 0),
        member("qml-worker", "QML", false, true, 1),
        member("cpp-worker", "C++", false, true, 2)});
    manifest.tasks = {task};
    assert(harness.manifests.freeze(manifest, ai::application::ExecutionManifestPolicy {}).success());

    // By hand: the task "ui" is qml-worker's first task, at its frozen position 1.
    const auto insertRow = [&](const char* id, std::uint32_t position, std::uint32_t attempt,
                               const char* role, std::uint64_t cost, bool priceKnown) {
        ai::application::ModelExecution value;
        value.id = id;
        value.sourceId = run.id;
        value.position = position;
        value.attempt = attempt;
        value.role = role;
        value.status = ai::application::ModelExecutionStatus::Completed;
        value.inputTokens = 100;
        value.outputTokens = 10;
        value.costMicros = cost;
        value.costPriceKnown = priceKnown;
        assert(harness.modelExecutions.insertExecution(std::move(value)).success());
    };
    insertRow("planner", 0, 1, "planner", 5000, true);
    insertRow("ui-a1", 1, 1, "QML", 1000, true);
    insertRow("ui-a2", 1, 2, "QML", 2000, false);
    insertRow("stray", 50, 1, "stray", 70, true);

    assert(harness.controller.selectRun(QString::fromStdString(run.id)));
    assert(harness.controller.errorText().isEmpty());
    assert(harness.controller.usageAvailable());
    assert(harness.controller.usagePartitionExact()
        && "A stray row must stay inside the exact partition, as unattributed");
    assert(harness.controller.usageUnattributedCount() == 1);
    const auto unattributed = harness.controller.usageUnattributed();
    assert(unattributed.size() == 1);
    assert(unattributed.front().toMap().value("id").toString() == QStringLiteral("stray"));
    assert(unattributed.front().toMap().value("position").toUInt() == 50);
    assert(!unattributed.front().toMap().value("reason").toString().isEmpty());
    assert(!harness.controller.usageDiagnostics().isEmpty()
        && harness.controller.usageDiagnostics().front().startsWith(
               QStringLiteral("usage.unattributed: 1 execution(s)")));
    assert(harness.controller.usageRunTotal().value("executions").toULongLong() == 4);

    const auto members = harness.controller.routingMembers();
    const auto byId = [&](const QString& memberId) {
        for (const auto& entry : members) {
            if (entry.toMap().value("memberId").toString() == memberId) return entry.toMap();
        }
        return QVariantMap {};
    };
    const auto lead = byId("architect").value("usage").toMap();
    assert(lead.value("executions").toULongLong() == 1
        && lead.value("priceState").toString() == QStringLiteral("known")
        && lead.value("costText").toString() == QStringLiteral("$0.005000"));
    const auto qml = byId("qml-worker").value("usage").toMap();
    assert(qml.value("executions").toULongLong() == 2
        && qml.value("inputTokens").toULongLong() == 200
        && qml.value("unknownPriceExecutions").toULongLong() == 1
        && qml.value("priceState").toString() == QStringLiteral("partial")
        && qml.value("costText").toString() == QStringLiteral("$0.003000"));
    const auto cpp = byId("cpp-worker").value("usage").toMap();
    assert(cpp.value("executions").toULongLong() == 0
        && cpp.value("priceState").toString() == QStringLiteral("none")
        && cpp.value("costText").toString().isEmpty()
        && "An unused worker has no cost to show - not a known zero");

    // A Run without a Manifest resets the projection (no stale usage is shown).
    assert(harness.controller.selectRun(QString {}));
    assert(!harness.controller.usageAvailable() && !harness.controller.usagePartitionExact());
    assert(harness.controller.usageUnattributedCount() == 0
        && harness.controller.usageUnattributed().isEmpty()
        && harness.controller.usageDiagnostics().isEmpty()
        && harness.controller.usageRunTotal().isEmpty());
}

// AG-17.7B: an unknown price must never reach the screen as a zero. `costText` is empty
// for an unknown price, so the QML must word the three price states itself.
void testManifestControllerNeverRendersAnUnknownPriceAsZero()
{
    ManifestControllerHarness harness;
    const auto projectId = harness.makeProject();
    ai::application::StageModelAssignment lead;
    lead.stageType = "architect";
    lead.role = "Lead";
    lead.leadArchitect = true;
    lead.workspaceAccess = true;
    lead.providerId = "p";
    lead.modelId = "m";
    lead.position = 0;
    lead.timeout = std::chrono::milliseconds(60000);

    ai::application::Run run;
    run.id = "run-unknown-price";
    run.projectId = projectId;
    run.title = "Unknown price";
    run.task = "x";
    run.workflowId = "execution-manifest-v1";
    assert(harness.runs.insertRun(run).success());
    ai::application::ExecutionManifestTask task;
    task.id = "t";
    task.objective = "o";
    task.role = "worker";
    task.requestedCapability = ai::application::WorkspaceCapability::Propose;
    ai::application::ExecutionManifest manifest;
    manifest.runId = run.id;
    manifest.routing = ai::application::ManifestRouting::fromAssignments({lead});
    manifest.tasks = {task};
    assert(harness.manifests.freeze(manifest, ai::application::ExecutionManifestPolicy {}).success());

    ai::application::ModelExecution value;
    value.id = "only";
    value.sourceId = run.id;
    value.position = 1;
    value.attempt = 1;
    value.role = "worker";
    value.status = ai::application::ModelExecutionStatus::Completed;
    value.inputTokens = 10;
    value.outputTokens = 1;
    value.costMicros = 0;
    value.costPriceKnown = false;
    assert(harness.modelExecutions.insertExecution(std::move(value)).success());

    assert(harness.controller.selectRun(QString::fromStdString(run.id)));
    const auto usage = harness.controller.routingMembers().front().toMap().value("usage").toMap();
    assert(usage.value("priceState").toString() == QStringLiteral("unknown"));
    assert(usage.value("costText").toString().isEmpty()
        && "An unknown price must have no amount, never \"$0.000000\"");
    assert(usage.value("executions").toULongLong() == 1);
}

// A single-assignment Manifest Run (lead "architect" at position 0, one task at position 1)
// with the given ModelExecution rows, selectable through the harness controller.
void seedSingleAssignmentUsageRun(ManifestControllerHarness& harness, const std::string& runId,
    const std::vector<ai::application::ModelExecution>& rows)
{
    const auto projectId = harness.makeProject();
    ai::application::StageModelAssignment lead;
    lead.stageType = "architect";
    lead.role = "Lead";
    lead.leadArchitect = true;
    lead.workspaceAccess = true;
    lead.providerId = "p";
    lead.modelId = "m";
    lead.position = 0;
    lead.timeout = std::chrono::milliseconds(60000);

    ai::application::Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "Usage diagnostics";
    run.task = "x";
    run.workflowId = "execution-manifest-v1";
    assert(harness.runs.insertRun(run).success());
    ai::application::ExecutionManifestTask task;
    task.id = "t";
    task.objective = "o";
    task.role = "worker";
    task.requestedCapability = ai::application::WorkspaceCapability::Propose;
    ai::application::ExecutionManifest manifest;
    manifest.runId = run.id;
    manifest.routing = ai::application::ManifestRouting::fromAssignments({lead});
    manifest.tasks = {task};
    assert(harness.manifests.freeze(manifest, ai::application::ExecutionManifestPolicy {}).success());
    for (auto value : rows) {
        value.sourceId = run.id;
        assert(harness.modelExecutions.insertExecution(std::move(value)).success());
    }
}

ai::application::ModelExecution usageLedgerRow(const char* id, std::uint32_t attempt,
    std::uint64_t input, std::uint64_t cost, bool priceKnown)
{
    ai::application::ModelExecution value;
    value.id = id;
    value.position = 1;
    value.attempt = attempt;
    value.role = "worker";
    value.status = ai::application::ModelExecutionStatus::Completed;
    value.inputTokens = input;
    value.outputTokens = 1;
    value.costMicros = cost;
    value.costPriceKnown = priceKnown;
    return value;
}

// AG-17.7B: CostBreakdown records the PRICED part of an incompletely priced execution, so a
// positive amount with every execution unpriced is a lower bound ("partial"), not "unknown".
void testManifestControllerShowsIncompletePricingAsALowerBound()
{
    ManifestControllerHarness harness;
    seedSingleAssignmentUsageRun(harness, "run-lower-bound", {usageLedgerRow("only", 1, 10, 125, false)});
    assert(harness.controller.selectRun(QStringLiteral("run-lower-bound")));
    const auto usage = harness.controller.routingMembers().front().toMap().value("usage").toMap();
    assert(usage.value("priceState").toString() == QStringLiteral("partial"));
    assert(usage.value("costText").toString() == QStringLiteral("$0.000125")
        && "The priced part is shown, worded by the UI as a lower bound");
    assert(usage.value("unknownPriceExecutions").toULongLong() == 1
        && usage.value("executions").toULongLong() == 1);
}

class FailingUsageLedger final : public ai::application::IModelExecutionRepository {
public:
    ai::common::Result<ai::application::ModelExecution> insertExecution(
        ai::application::ModelExecution) override { return fail<ai::application::ModelExecution>(); }
    ai::common::Result<ai::application::ModelExecution> updateExecution(
        ai::application::ModelExecution) override { return fail<ai::application::ModelExecution>(); }
    ai::common::Result<std::optional<ai::application::ModelExecution>> findExecution(
        const ai::application::ModelExecutionId&) const override
    {
        return fail<std::optional<ai::application::ModelExecution>>();
    }
    ai::common::Result<std::vector<ai::application::ModelExecution>> listExecutions(
        ai::application::ModelExecutionSource, const std::string&) const override
    {
        return fail<std::vector<ai::application::ModelExecution>>();
    }
    ai::common::Result<std::vector<ai::application::ModelExecution>> listProjectExecutions(
        const ai::application::ProjectId&) const override
    {
        return fail<std::vector<ai::application::ModelExecution>>();
    }
    ai::common::Result<std::vector<ai::application::ModelExecution>> listUnfinishedExecutions() const override
    {
        return fail<std::vector<ai::application::ModelExecution>>();
    }
    ai::common::Result<ai::application::QualityEvaluation> insertEvaluation(
        ai::application::QualityEvaluation) override { return fail<ai::application::QualityEvaluation>(); }
    ai::common::Result<std::optional<ai::application::QualityEvaluation>> findEvaluation(
        const ai::application::QualityEvaluationId&) const override
    {
        return fail<std::optional<ai::application::QualityEvaluation>>();
    }
    ai::common::Result<std::vector<ai::application::QualityEvaluation>> listEvaluations(
        const ai::application::ModelExecutionId&) const override
    {
        return fail<std::vector<ai::application::QualityEvaluation>>();
    }

private:
    template <typename T>
    static ai::common::Result<T> fail()
    {
        return ai::common::Result<T>::failure({"persistence.read_failed", "usage rows unreadable"});
    }
};

// AG-17.7B: a usage diagnostic is not always attribution debt. A ledger that cannot be read
// (no attribution at all) and a saturated counter (the partition stays exact) must still reach
// the screen, and neither may be reported as "N executions could not be attributed".
void testManifestControllerExposesDiagnosticsThatAreNotAttributionDebt()
{
    // A saturated counter leaves the partition exact and nothing unattributed.
    {
        ManifestControllerHarness harness;
        // (Not std::numeric_limits<>::max(): the Windows `max` macro breaks that spelling here.)
        const std::uint64_t max = ~std::uint64_t {0};
        seedSingleAssignmentUsageRun(harness, "run-saturated",
            {usageLedgerRow("a", 1, max, 1, true), usageLedgerRow("b", 2, 5, 1, true)});
        assert(harness.controller.selectRun(QStringLiteral("run-saturated")));
        assert(harness.controller.usageAvailable() && harness.controller.usagePartitionExact());
        assert(harness.controller.usageUnattributedCount() == 0);
        const auto diagnostics = harness.controller.usageDiagnostics();
        assert(diagnostics.size() == 1 && diagnostics.front().startsWith(QStringLiteral("usage.counter_saturated")));
        assert(harness.controller.routingMembers().front().toMap().value("usage").toMap()
                   .value("saturated").toBool());
    }
    // An unreadable ledger: no attribution at all, but the Manifest view itself still projects.
    {
        ManifestControllerHarness harness;
        seedSingleAssignmentUsageRun(harness, "run-unreadable", {usageLedgerRow("a", 1, 10, 100, true)});
        FailingUsageLedger failing;
        ai::application::ExecutionManifestReadModelService readModel {
            harness.manifests, harness.taskExecutions, nullptr, nullptr,
            &harness.verificationEnvironmentRepository, &harness.verificationBootstrapRepository,
            &harness.workLedger, &harness.providers, &harness.models, &failing};
        ManifestController controller {readModel, harness.retryService};
        assert(controller.selectRun(QStringLiteral("run-unreadable")));
        assert(controller.errorText().isEmpty() && controller.hasManifest());
        assert(controller.routingMode() == QStringLiteral("single_assignment"));
        assert(!controller.usageAvailable() && controller.usageUnattributedCount() == 0);
        const auto diagnostics = controller.usageDiagnostics();
        assert(diagnostics.size() == 1
            && diagnostics.front() == QStringLiteral("usage.unavailable: persistence.read_failed"));
    }
}

// AG-17.6: the New Run dialog's roster launches through the converged Manifest workflow
// only when it says so; the Backend validates the roster and the Run reopens as a
// Manifest roster (never silently as the legacy fixed-stage runner).
void testManifestRosterRunSelectsTheManifestWorkflowAndReopensAsAManifestRoster()
{
    TempWorkspace workspace;
    auto bridgeOwner = std::make_unique<BackendBridge>(workspace.databasePathQt());
    BackendBridge& bridge = *bridgeOwner;
    assert(bridge.storageAvailable());

    const QString projectId = bridge.projectController().createProject(
        QStringLiteral("Manifest roster project"),
        QStringLiteral("Manifest roster Qt test"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());
    ProviderCatalogController& catalog = bridge.providerCatalogController();
    const QString providerId = catalog.createProvider(
        QStringLiteral("Roster test provider"), QStringLiteral("local"),
        QStringLiteral("http://127.0.0.1:11434/v1"));
    assert(!providerId.isEmpty());
    const QString modelId = catalog.createModel(
        providerId, QStringLiteral("roster-test-model"), QStringLiteral("Roster test model"),
        32768, 4096, 0, 0);
    assert(!modelId.isEmpty());

    const auto row = [&](const QString& stageType, const QString& role, bool lead,
                         bool workspaceAccess, int position) {
        return QVariantMap {
            {QStringLiteral("stageType"), stageType},
            {QStringLiteral("providerId"), providerId},
            {QStringLiteral("modelId"), modelId},
            {QStringLiteral("agentName"), role},
            {QStringLiteral("role"), role},
            {QStringLiteral("instructions"), QStringLiteral("Own %1 work.").arg(role)},
            {QStringLiteral("leadArchitect"), lead},
            {QStringLiteral("workspaceAccess"), workspaceAccess},
            {QStringLiteral("position"), position},
            {QStringLiteral("dependencies"), lead ? QVariantList {} : QVariantList {QStringLiteral("architect")}},
            {QStringLiteral("maxAttempts"), 1}};
    };
    RunController& runs = bridge.runController();

    // A roster with nobody who can take a workspace task is refused synchronously.
    QVariantList nobody;
    nobody.push_back(row(QStringLiteral("architect"), QStringLiteral("Lead"), true, false, 0));
    nobody.push_back(row(QStringLiteral("docs"), QStringLiteral("Docs"), false, false, 1));
    const QString refused = runs.startRunWithRoster(
        projectId, QStringLiteral("No worker"), QStringLiteral("Create a file"), nobody, 2,
        QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"));
    assert(refused.isEmpty());
    assert(runs.lastError().contains(QStringLiteral("non-lead worker with workspace access")));

    QVariantList roster;
    roster.push_back(row(QStringLiteral("architect"), QStringLiteral("Lead"), true, false, 0));
    roster.push_back(row(QStringLiteral("qml-implementer"), QStringLiteral("QML implementer"), false, true, 1));
    roster.push_back(row(QStringLiteral("cpp-implementer"), QStringLiteral("C++ implementer"), false, true, 2));
    const QString runId = runs.startRunWithRoster(
        projectId, QStringLiteral("Manifest roster run"), QStringLiteral("Create two files"),
        roster, 2, QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"));
    assert(!runId.isEmpty());
    // No provider is reachable, so the Run fails in planning - the point here is the
    // routing, not the outcome.
    static_cast<void>(waitForStatus(runs, runId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}));

    const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
    assert(database.success());
    ai::infrastructure::SqliteRunRepository storedRuns(database.value());
    const auto stored = storedRuns.findRun(runId.toStdString());
    assert(stored.success() && stored.value().has_value());
    assert(stored.value()->workflowId == "execution-manifest-v1");
    assert(!stored.value()->executionProfileId.has_value());
    assert(stored.value()->assignmentSnapshot.size() == 3);
    assert(stored.value()->assignmentSnapshot.front().leadArchitect);
    assert(stored.value()->assignmentSnapshot.at(1).stageType == "qml-implementer");
    assert(stored.value()->assignmentSnapshot.at(1).workspaceAccess);
    assert(stored.value()->assignmentSnapshot.at(1).maxAttempts == 1);

    // Edit & rerun reopens it as a Manifest roster, with every field intact.
    const QVariantMap draft = runs.rerunDraft(runId);
    assert(draft.value(QStringLiteral("workflowId")).toString() == QStringLiteral("execution-manifest-v1"));
    assert(draft.value(QStringLiteral("launchMode")).toString() == QStringLiteral("custom_roster"));
    assert(draft.value(QStringLiteral("roster")).toList().size() == 3);

    // Provider limits: no explicit limit = the Backend defaults with an automatic turn limit, and
    // Edit & rerun reopens with an EMPTY limits map; an explicit maxTurns pins it, is validated by
    // the Backend (never by QML) and reopens as exactly the values that were set.
    assert(stored.value()->providerBudget.autoExtendTurns);
    assert(draft.contains(QStringLiteral("providerBudget"))
           && draft.value(QStringLiteral("providerBudget")).toMap().isEmpty());
    const QString outOfRange = runs.startRunWithRoster(
        projectId, QStringLiteral("Too many turns"), QStringLiteral("Create two files"),
        roster, 2, QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"),
        QVariantMap {{QStringLiteral("maxTurns"), 65}});
    assert(outOfRange.isEmpty());
    assert(runs.lastError().contains(QStringLiteral("maxTurns")));
    const QString limitedRunId = runs.startRunWithRoster(
        projectId, QStringLiteral("Limited roster run"), QStringLiteral("Create two files"),
        roster, 2, QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"),
        QVariantMap {{QStringLiteral("maxTurns"), 12}, {QStringLiteral("maxInputTokens"), 100000}});
    assert(!limitedRunId.isEmpty());
    static_cast<void>(waitForStatus(runs, limitedRunId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}));
    const auto limited = storedRuns.findRun(limitedRunId.toStdString());
    assert(limited.success() && limited.value().has_value());
    assert(!limited.value()->providerBudget.autoExtendTurns);
    assert(limited.value()->providerBudget.maxTurns == 12);
    assert(limited.value()->providerBudget.maxInputTokens == 100000);
    const QVariantMap limitedDraft = runs.rerunDraft(limitedRunId);
    const QVariantMap limitedBudget = limitedDraft.value(QStringLiteral("providerBudget")).toMap();
    assert(limitedBudget.size() == 2
           && limitedBudget.value(QStringLiteral("maxTurns")).toULongLong() == 12
           && limitedBudget.value(QStringLiteral("maxInputTokens")).toULongLong() == 100000);

    // Drafts keep the same optional-key limits map and refuse it where it cannot apply.
    RunDraftController& drafts = bridge.runDraftController();
    QVariantList draftRoster;   // a draft row needs its timeout, like the dialog's payload carries
    for (const QVariant& entry : roster) {
        QVariantMap withTimeout = entry.toMap();
        withTimeout.insert(QStringLiteral("timeoutMs"), 60000);
        draftRoster.push_back(withTimeout);
    }
    const QString draftId = drafts.createDraft(
        QStringLiteral("Limited draft"), projectId, QStringLiteral("Title"), QStringLiteral("Task"),
        QStringLiteral("custom_roster"), QString {}, draftRoster, 2, QString {}, true,
        QStringLiteral("execution-manifest-v1"),
        QVariantMap {{QStringLiteral("maxTurns"), 40}});
    assert(!draftId.isEmpty());
    assert(drafts.openDraft(draftId).value(QStringLiteral("providerBudget")).toMap()
           == (QVariantMap {{QStringLiteral("maxTurns"), QVariant::fromValue<qulonglong>(40)}}));
    assert(drafts.updateDraft(draftId, QStringLiteral("Limited draft"), projectId,
        QStringLiteral("Title"), QStringLiteral("Task"), QStringLiteral("custom_roster"),
        QString {}, draftRoster, 2, QString {}, true, QStringLiteral("execution-manifest-v1"),
        QVariantMap {}));
    assert(drafts.openDraft(draftId).value(QStringLiteral("providerBudget")).toMap().isEmpty());
    assert(drafts.createDraft(
        QStringLiteral("Mock draft with limits"), projectId, QStringLiteral("Title"),
        QStringLiteral("Task"), QStringLiteral("mock"), QString {}, QVariantList {}, 2,
        QString {}, true, QString {},
        QVariantMap {{QStringLiteral("maxTurns"), 40}}).isEmpty());
    assert(drafts.lastError().contains(QStringLiteral("per-Run roster")));

    // PC-2: interface contracts travel as a list of maps, are validated by the Backend (never by QML), frozen
    // on the Run in their canonical form, reopened exactly by Edit & rerun and by drafts, and refused where no
    // worker prompt could carry them.
    const QVariantList contracts {
        QVariantMap {{QStringLiteral("id"), QStringLiteral("C1")},
            {QStringLiteral("kind"), QStringLiteral("interface_contract")},
            {QStringLiteral("title"), QStringLiteral(" Loss entry ")},
            {QStringLiteral("text"), QStringLiteral("property string metricId\r\nproperty real share")},
            {QStringLiteral("appliesToPaths"), QVariantList {QStringLiteral("qml\\models")}}},
        // The free-text scope form of the dialog: comma, semicolon and line-break separated.
        QVariantMap {{QStringLiteral("id"), QStringLiteral("K1")},
            {QStringLiteral("kind"), QStringLiteral("constraint")},
            {QStringLiteral("title"), QStringLiteral("Rule")},
            {QStringLiteral("text"), QStringLiteral("No file may exceed 300 lines.")},
            {QStringLiteral("appliesToPaths"), QStringLiteral("qml/pages, src;\n docs/ ,, ")}}};
    QVariantList badContracts = contracts;
    badContracts[0] = QVariantMap {{QStringLiteral("id"), QStringLiteral("1bad")},
        {QStringLiteral("title"), QStringLiteral("t")}, {QStringLiteral("text"), QStringLiteral("x")}};
    const QString refusedContracts = runs.startRunWithRoster(
        projectId, QStringLiteral("Bad contracts"), QStringLiteral("Create two files"),
        roster, 2, QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"), QVariantMap {}, badContracts);
    assert(refusedContracts.isEmpty());
    assert(runs.lastError().contains(QStringLiteral("contract")));
    const QString contractRunId = runs.startRunWithRoster(
        projectId, QStringLiteral("Contract roster run"), QStringLiteral("Create two files"),
        roster, 2, QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"), QVariantMap {}, contracts);
    assert(!contractRunId.isEmpty());
    static_cast<void>(waitForStatus(runs, contractRunId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}));
    const auto contractRun = storedRuns.findRun(contractRunId.toStdString());
    assert(contractRun.success() && contractRun.value().has_value());
    const auto& frozenContracts = contractRun.value()->interfaceContracts;
    assert(frozenContracts.size() == 2 && frozenContracts[0].id == "C1" && frozenContracts[0].title == "Loss entry"
           && frozenContracts[0].text == "property string metricId\nproperty real share"
           && frozenContracts[0].appliesToPaths == std::vector<std::string> {"qml/models"}
           && frozenContracts[1].kind == ai::application::RunContractKind::Constraint
           && frozenContracts[1].appliesToPaths
               == (std::vector<std::string> {"qml/pages", "src", "docs"}));
    const QVariantMap contractDraft = runs.rerunDraft(contractRunId);
    const QVariantList reopenedContracts = contractDraft.value(QStringLiteral("interfaceContracts")).toList();
    assert(reopenedContracts.size() == 2
           && reopenedContracts[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("C1")
           && reopenedContracts[0].toMap().value(QStringLiteral("kind")).toString()
               == QStringLiteral("interface_contract")
           && reopenedContracts[0].toMap().value(QStringLiteral("text")).toString()
               == QStringLiteral("property string metricId\nproperty real share")
           && reopenedContracts[1].toMap().value(QStringLiteral("kind")).toString()
               == QStringLiteral("constraint")
           && reopenedContracts[1].toMap().value(QStringLiteral("appliesToPaths")).toList().size() == 3);
    // A Run without contracts reopens with an empty list, never a missing key.
    assert(draft.contains(QStringLiteral("interfaceContracts"))
           && draft.value(QStringLiteral("interfaceContracts")).toList().isEmpty());

    // Drafts keep the same list and refuse a bad one with the Backend's own message.
    const QString contractDraftId = drafts.createDraft(
        QStringLiteral("Contract draft"), projectId, QStringLiteral("Title"), QStringLiteral("Task"),
        QStringLiteral("custom_roster"), QString {}, draftRoster, 2, QString {}, true,
        QStringLiteral("execution-manifest-v1"), QVariantMap {}, contracts);
    assert(!contractDraftId.isEmpty());
    assert(drafts.openDraft(contractDraftId).value(QStringLiteral("interfaceContracts")).toList().size() == 2);
    assert(!drafts.updateDraft(contractDraftId, QStringLiteral("Contract draft"), projectId,
        QStringLiteral("Title"), QStringLiteral("Task"), QStringLiteral("custom_roster"),
        QString {}, draftRoster, 2, QString {}, true, QStringLiteral("execution-manifest-v1"),
        QVariantMap {}, badContracts));
    assert(drafts.lastError().contains(QStringLiteral("contract")));
    assert(drafts.openDraft(contractDraftId).value(QStringLiteral("interfaceContracts")).toList().size() == 2);
    assert(drafts.updateDraft(contractDraftId, QStringLiteral("Contract draft"), projectId,
        QStringLiteral("Title"), QStringLiteral("Task"), QStringLiteral("custom_roster"),
        QString {}, draftRoster, 2, QString {}, true, QStringLiteral("execution-manifest-v1"),
        QVariantMap {}, QVariantList {}));
    assert(drafts.openDraft(contractDraftId).value(QStringLiteral("interfaceContracts")).toList().isEmpty());

    // A draft that could never start with its contracts is refused when it is saved, not stored and stripped by the
    // dialog later: contracts need a per-Run roster on the Manifest workflow.
    assert(drafts.createDraft(QStringLiteral("Profile contract draft"), projectId, QStringLiteral("Title"),
        QStringLiteral("Task"), QStringLiteral("custom_roster"), QString {}, draftRoster, 2, QString {}, true,
        QString {}, QVariantMap {}, contracts).isEmpty());
    assert(drafts.lastError().contains(QStringLiteral("Manifest workflow")));
    assert(drafts.createDraft(QStringLiteral("Mock contract draft"), projectId, QStringLiteral("Title"),
        QStringLiteral("Task"), QStringLiteral("mock"), QString {}, QVariantList {}, 2, QString {}, true,
        QString {}, QVariantMap {}, contracts).isEmpty());
    assert(drafts.updateDraft(contractDraftId, QStringLiteral("Contract draft"), projectId,
        QStringLiteral("Title"), QStringLiteral("Task"), QStringLiteral("custom_roster"),
        QString {}, draftRoster, 2, QString {}, true, QStringLiteral("execution-manifest-v1"),
        QVariantMap {}, contracts));
    assert(!drafts.updateDraft(contractDraftId, QStringLiteral("Contract draft"), projectId,
        QStringLiteral("Title"), QStringLiteral("Task"), QStringLiteral("custom_roster"),
        QString {}, draftRoster, 2, QString {}, true, QString {}, QVariantMap {}, contracts));
    assert(drafts.lastError().contains(QStringLiteral("Manifest workflow")));
    assert(drafts.openDraft(contractDraftId).value(QStringLiteral("workflowId")).toString()
               == QStringLiteral("execution-manifest-v1")
           && drafts.openDraft(contractDraftId).value(QStringLiteral("interfaceContracts")).toList().size() == 2);

    // An unknown kind is an error at the Qt boundary for a Run and for a draft, never the default kind.
    QVariantList unknownKind = contracts;
    unknownKind[1] = QVariantMap {{QStringLiteral("id"), QStringLiteral("K1")},
        {QStringLiteral("kind"), QStringLiteral("constaint")}, {QStringLiteral("title"), QStringLiteral("t")},
        {QStringLiteral("text"), QStringLiteral("x")}};
    assert(runs.startRunWithRoster(projectId, QStringLiteral("Unknown kind"), QStringLiteral("Create two files"),
        roster, 2, QString {}, QString {}, QString {}, false, false,
        QStringLiteral("execution-manifest-v1"), QVariantMap {}, unknownKind).isEmpty());
    assert(runs.lastError().contains(QStringLiteral("unknown kind")) && runs.lastError().contains(QStringLiteral("K1")));
    assert(drafts.createDraft(QStringLiteral("Unknown kind draft"), projectId, QStringLiteral("Title"),
        QStringLiteral("Task"), QStringLiteral("custom_roster"), QString {}, draftRoster, 2, QString {}, true,
        QStringLiteral("execution-manifest-v1"), QVariantMap {}, unknownKind).isEmpty());
    assert(drafts.lastError().contains(QStringLiteral("unknown kind")));
    assert(!drafts.updateDraft(contractDraftId, QStringLiteral("Contract draft"), projectId,
        QStringLiteral("Title"), QStringLiteral("Task"), QStringLiteral("custom_roster"),
        QString {}, draftRoster, 2, QString {}, true, QStringLiteral("execution-manifest-v1"),
        QVariantMap {}, unknownKind));
    assert(drafts.lastError().contains(QStringLiteral("unknown kind")));
    assert(drafts.openDraft(contractDraftId).value(QStringLiteral("interfaceContracts")).toList().size() == 2);

    // The legacy default is unchanged: no workflow id keeps the fixed-stage runner.
    const QString legacyRunId = runs.startRunWithRoster(
        projectId, QStringLiteral("Legacy roster run"), QStringLiteral("Legacy"), roster, 2,
        QString {}, QString {}, QString {}, false, false);
    assert(!legacyRunId.isEmpty());
    static_cast<void>(waitForStatus(runs, legacyRunId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}));
    const auto legacy = storedRuns.findRun(legacyRunId.toStdString());
    assert(legacy.success() && legacy.value().has_value());
    assert(legacy.value()->workflowId == "execution-profile-v1");
    // An unknown workflow id can never route a roster somewhere unintended.
    const QString unknownRunId = runs.startRunWithRoster(
        projectId, QStringLiteral("Unknown workflow roster run"), QStringLiteral("Unknown"), roster, 2,
        QString {}, QString {}, QString {}, false, false, QStringLiteral("something-else"));
    assert(!unknownRunId.isEmpty());
    static_cast<void>(waitForStatus(runs, unknownRunId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}));
    const auto unknown = storedRuns.findRun(unknownRunId.toStdString());
    assert(unknown.success() && unknown.value().has_value());
    assert(unknown.value()->workflowId == "execution-profile-v1");
}

// AG-17.7C-2: the Qt side of the audited legacy -> Manifest conversion. The controller shows the
// Backend's preview (never instruction text), routes a Retry that needs a decision to the
// conversion dialog, and starts a conversion on NOTHING but the opaque fingerprint it was shown.
void testLegacyConversionControllerShowsThePreviewAndConfirmsOnlyByFingerprint()
{
    TempWorkspace workspace;
    auto bridgeOwner = std::make_unique<BackendBridge>(workspace.databasePathQt());
    BackendBridge& bridge = *bridgeOwner;
    assert(bridge.storageAvailable());

    const QString projectId = bridge.projectController().createProject(
        QStringLiteral("Legacy conversion project"),
        QStringLiteral("Legacy conversion Qt test"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());
    ProviderCatalogController& catalog = bridge.providerCatalogController();
    const QString providerId = catalog.createProvider(
        QStringLiteral("Conversion test provider"), QStringLiteral("local"),
        QStringLiteral("http://127.0.0.1:11434/v1"));
    assert(!providerId.isEmpty());
    const QString modelId = catalog.createModel(
        providerId, QStringLiteral("conversion-test-model"), QStringLiteral("Conversion test model"),
        32768, 4096, 0, 0);
    assert(!modelId.isEmpty());

    RunController& runs = bridge.runController();
    runs.refreshAgentChoices();
    ai::ui::test_support::waitForCondition("agent choices loaded", [&] {
        return !runs.agentChoices().isEmpty();
    }, [] { return std::string("the catalog choices never loaded"); }, 4s);

    const QString secretInstructions = QStringLiteral("Plan carefully and keep tasks independent.");
    const auto row = [&](const QString& stageType, const QString& role, bool lead,
                         bool workspaceAccess, int position, const QString& instructions,
                         int attempts) {
        return QVariantMap {
            {QStringLiteral("stageType"), stageType},
            {QStringLiteral("providerId"), providerId},
            {QStringLiteral("modelId"), modelId},
            {QStringLiteral("agentName"), role},
            {QStringLiteral("role"), role},
            {QStringLiteral("instructions"), instructions},
            {QStringLiteral("leadArchitect"), lead},
            {QStringLiteral("workspaceAccess"), workspaceAccess},
            {QStringLiteral("position"), position},
            {QStringLiteral("dependencies"),
                lead ? QVariantList {} : QVariantList {QStringLiteral("architect")}},
            {QStringLiteral("maxAttempts"), attempts}};
    };
    const auto proposalRoster = [&] {
        QVariantList roster;
        roster.push_back(row(QStringLiteral("architect"), QStringLiteral("Lead"), true, false, 0,
            secretInstructions, 2));
        roster.push_back(row(QStringLiteral("qml-worker"), QStringLiteral("QML worker"), false, true, 1, {}, 1));
        roster.push_back(row(QStringLiteral("cpp-worker"), QStringLiteral("C++ worker"), false, true, 2, {}, 1));
        return roster;
    };
    const auto start = [&](const QString& title, const QVariantList& roster) {
        const QString runId = runs.startRunWithRoster(
            projectId, title, QStringLiteral("Legacy task"), roster, 2,
            QString {}, QString {}, QString {}, false, false);
        assert(!runId.isEmpty());
        static_cast<void>(waitForStatus(runs, runId,
            {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}, 20s));
        return runId;
    };

    // A lone workspace lead WITH instructions (needs confirmation), one WITHOUT (a safe Retry),
    // two identical proposal rosters, and a text roster (no Manifest counterpart).
    const QString loneWithInstructions = start(QStringLiteral("Lone lead with instructions"),
        {row(QStringLiteral("architect"), QStringLiteral("Analyst"), true, true, 0, secretInstructions, 1)});
    const QString loneSafe = start(QStringLiteral("Lone lead without instructions"),
        {row(QStringLiteral("architect"), QStringLiteral("Analyst"), true, true, 0, {}, 1)});
    const QString proposalA = start(QStringLiteral("Proposal roster A"), proposalRoster());
    const QString proposalB = start(QStringLiteral("Proposal roster B"), proposalRoster());
    const QString textRoster = start(QStringLiteral("Text roster"),
        {row(QStringLiteral("architect"), QStringLiteral("Lead"), true, false, 0, {}, 1),
         row(QStringLiteral("writer"), QStringLiteral("Writer"), false, false, 1, {}, 1)});

    const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
    assert(database.success());
    ai::infrastructure::SqliteRunRepository stored(database.value());
    const auto runCount = [&] {
        const auto all = stored.listRuns(std::nullopt);
        assert(all.success());
        return all.value().size();
    };
    const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));

    // ---- the preview: Backend-owned, instruction LENGTH only, roster rows without text ----
    const QVariantMap draftS4 = runs.rerunDraft(proposalA);
    assert(draftS4.contains(QStringLiteral("conversion")));
    const QVariantMap previewS4 = draftS4.value(QStringLiteral("conversion")).toMap();
    assert(previewS4.value(QStringLiteral("available")).toBool()
        && !previewS4.value(QStringLiteral("required")).toBool()
        && previewS4.value(QStringLiteral("legacyRerunAvailable")).toBool()
        && !previewS4.value(QStringLiteral("safeAutomaticRetry")).toBool());
    assert(previewS4.value(QStringLiteral("sourceRunId")).toString() == proposalA
        && previewS4.value(QStringLiteral("sourceWorkflowId")).toString() == QStringLiteral("execution-profile-v1")
        && previewS4.value(QStringLiteral("targetWorkflowId")).toString() == QStringLiteral("execution-manifest-v1"));
    assert(sha256.match(previewS4.value(QStringLiteral("confirmationFingerprint")).toString()).hasMatch()
        && "The confirmation fingerprint is the Backend's opaque 64-hex value");
    assert(previewS4.value(QStringLiteral("hasDroppedInstructions")).toBool());
    const QVariantList droppedS4 = previewS4.value(QStringLiteral("dropped")).toList();
    assert(!droppedS4.isEmpty());
    bool sawInstructionsLength = false;
    for (const QVariant& entry : droppedS4) {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("setting")).toString() == QStringLiteral("instructions")) {
            sawInstructionsLength = item.value(QStringLiteral("legacyValue")).toString()
                == QStringLiteral("%1 characters").arg(secretInstructions.size());
        }
    }
    assert(sawInstructionsLength && "An instructions entry is a length, never the text");
    const QVariantList rosterS4 = previewS4.value(QStringLiteral("roster")).toList();
    assert(rosterS4.size() == 3);
    for (const QVariant& member : rosterS4) {
        const QVariantMap item = member.toMap();
        assert(!item.contains(QStringLiteral("instructions"))
            && "A target roster row must carry no instruction text");
        assert(item.value(QStringLiteral("modelLabel")).toString()
            == QStringLiteral("Conversion test provider · Conversion test model"));
    }
    assert(rosterS4.front().toMap().value(QStringLiteral("leadArchitect")).toBool());
    assert(!QString::fromUtf8(QJsonDocument::fromVariant(QVariant(previewS4)).toJson())
            .contains(secretInstructions)
        && "The preview must not leak instruction text anywhere");

    // A lone workspace lead has NO legacy variant: the Backend marks the conversion required.
    const QVariantMap previewS2 = runs.rerunDraft(loneWithInstructions)
        .value(QStringLiteral("conversion")).toMap();
    assert(previewS2.value(QStringLiteral("available")).toBool()
        && previewS2.value(QStringLiteral("required")).toBool()
        && !previewS2.value(QStringLiteral("legacyRerunAvailable")).toBool()
        && !previewS2.value(QStringLiteral("safeAutomaticRetry")).toBool()
        && previewS2.value(QStringLiteral("roster")).toList().size() == 1);
    const QVariantMap previewSafe = runs.legacyConversionPreview(loneSafe);
    assert(previewSafe.value(QStringLiteral("safeAutomaticRetry")).toBool()
        && previewSafe.value(QStringLiteral("dropped")).toList().isEmpty()
        && !previewSafe.value(QStringLiteral("hasDroppedInstructions")).toBool());
    // The same preview, asked for directly, is the same decision.
    assert(runs.legacyConversionPreview(proposalA).value(QStringLiteral("confirmationFingerprint"))
        == previewS4.value(QStringLiteral("confirmationFingerprint")));
    // ... but a twin Run with an identical roster gets ITS OWN fingerprint.
    assert(runs.legacyConversionPreview(proposalB).value(QStringLiteral("confirmationFingerprint"))
        != previewS4.value(QStringLiteral("confirmationFingerprint")));
    // Defaults are the source Run's own editable fields.
    assert(previewS4.value(QStringLiteral("defaults")).toMap().value(QStringLiteral("title")).toString()
        == QStringLiteral("Proposal roster A"));

    // No conversion section where the Backend offers none: a text roster, a Manifest Run, Mock.
    assert(!runs.rerunDraft(textRoster).contains(QStringLiteral("conversion")));
    const QString mockRunId = runs.startRun(projectId, QStringLiteral("Mock"), QStringLiteral("Mock task"));
    assert(!mockRunId.isEmpty());
    static_cast<void>(waitForStatus(runs, mockRunId, {QStringLiteral("completed")}));
    assert(!runs.rerunDraft(mockRunId).contains(QStringLiteral("conversion")));
    // An unknown Run is an error, not an empty "preview".
    assert(runs.legacyConversionPreview(QStringLiteral("no-such-run")).isEmpty()
        && !runs.lastError().isEmpty());

    // ---- Retry: a safe conversion happens, a decision is routed, other shapes are unchanged ----
    QStringList requested;
    QObject::connect(&runs, &RunController::legacyConversionRequested, &runs,
        [&](const QString& sourceRunId) { requested.push_back(sourceRunId); });

    const std::size_t beforeRefusedRetry = runCount();
    assert(runs.retryRun(loneWithInstructions).isEmpty());
    assert(requested == QStringList {loneWithInstructions}
        && "A Retry that needs the user's decision must open the conversion dialog");
    assert(!runs.lastError().isEmpty());
    assert(runCount() == beforeRefusedRetry && "A routed Retry creates no Run");

    const QString safeChild = runs.retryRun(loneSafe);
    assert(!safeChild.isEmpty() && requested.size() == 1);
    static_cast<void>(waitForStatus(runs, safeChild,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}, 20s));
    const auto safeRun = stored.findRun(safeChild.toStdString());
    assert(safeRun.success() && safeRun.value().has_value()
        && safeRun.value()->workflowId == "execution-manifest-v1"
        && safeRun.value()->parentRunId == std::optional<ai::application::RunId>(loneSafe.toStdString()));
    const auto safeRecord = stored.findLegacyConversion(safeChild.toStdString());
    assert(safeRecord.success() && safeRecord.value().has_value()
        && safeRecord.value()->confirmationMode
            == ai::application::LegacyConversionConfirmationMode::NotRequiredSafeRetry
        && !safeRecord.value()->confirmedAt.has_value()
        && "A safe automatic Retry never claims that a user confirmed anything");

    for (const QString& exact : {proposalA, textRoster}) {
        const QString child = runs.retryRun(exact);
        assert(!child.isEmpty() && requested.size() == 1);
        static_cast<void>(waitForStatus(runs, child,
            {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}, 20s));
        const auto exactRun = stored.findRun(child.toStdString());
        assert(exactRun.success() && exactRun.value().has_value()
            && exactRun.value()->workflowId == "execution-profile-v1"
            && "An exact legacy Retry of every other shape stays on the legacy runner");
        const auto none = stored.findLegacyConversion(child.toStdString());
        assert(none.success() && !none.value().has_value());
    }

    // ---- explicit conversion: only the opaque fingerprint confirms ----
    const QString fingerprintA = previewS4.value(QStringLiteral("confirmationFingerprint")).toString();
    const std::size_t beforeConversion = runCount();
    const auto convert = [&](const QString& source, const QString& fingerprint) {
        return runs.startConvertedRerun(source, QStringLiteral("Converted title"),
            QStringLiteral("Converted task"), QString {}, 2, false, false, fingerprint);
    };

    const QVariantMap noConfirmation = convert(proposalA, QString {});
    assert(!noConfirmation.value(QStringLiteral("accepted")).toBool()
        && noConfirmation.value(QStringLiteral("confirmationRequired")).toBool()
        && !noConfirmation.value(QStringLiteral("confirmationStale")).toBool()
        && noConfirmation.value(QStringLiteral("runId")).toString().isEmpty());
    // A confirmation shown for ANOTHER Run is not portable, even for an identical roster.
    const QVariantMap foreign = convert(proposalB, fingerprintA);
    assert(!foreign.value(QStringLiteral("accepted")).toBool()
        && foreign.value(QStringLiteral("confirmationStale")).toBool());
    const QVariantMap garbage = convert(proposalA, QStringLiteral("not-a-fingerprint"));
    assert(!garbage.value(QStringLiteral("accepted")).toBool()
        && garbage.value(QStringLiteral("confirmationStale")).toBool());
    // A Run with no Manifest counterpart cannot be converted at all, typed.
    const QVariantMap notConvertible = convert(textRoster, QStringLiteral("anything"));
    assert(!notConvertible.value(QStringLiteral("accepted")).toBool()
        && notConvertible.value(QStringLiteral("errorCode")).toString().startsWith(QStringLiteral("legacy_roster.")));
    assert(runCount() == beforeConversion && "Every refused confirmation creates no Run");

    const QVariantMap accepted = convert(proposalA, fingerprintA);
    assert(accepted.value(QStringLiteral("accepted")).toBool()
        && accepted.value(QStringLiteral("errorCode")).toString().isEmpty());
    const QString convertedRunId = accepted.value(QStringLiteral("runId")).toString();
    assert(!convertedRunId.isEmpty());
    static_cast<void>(waitForStatus(runs, convertedRunId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}, 20s));
    const auto convertedRun = stored.findRun(convertedRunId.toStdString());
    assert(convertedRun.success() && convertedRun.value().has_value());
    assert(convertedRun.value()->workflowId == "execution-manifest-v1"
        && convertedRun.value()->parentRunId == std::optional<ai::application::RunId>(proposalA.toStdString())
        && convertedRun.value()->assignmentSnapshot.size() == 3
        && convertedRun.value()->title == "Converted title"
        && convertedRun.value()->task == "Converted task"
        && convertedRun.value()->maxParallelism == 2);
    const auto convertedRecord = stored.findLegacyConversion(convertedRunId.toStdString());
    assert(convertedRecord.success() && convertedRecord.value().has_value());
    assert(convertedRecord.value()->confirmationMode
            == ai::application::LegacyConversionConfirmationMode::ExplicitUser
        && convertedRecord.value()->confirmedAt.has_value()
        && QString::fromStdString(convertedRecord.value()->confirmationFingerprint) == fingerprintA
        && convertedRecord.value()->dropped.size() == static_cast<std::size_t>(droppedS4.size())
        && "The audit holds exactly what the preview showed, with the Backend's own time");
    // The historical Run is untouched.
    const auto source = stored.findRun(proposalA.toStdString());
    assert(source.success() && source.value().has_value()
        && source.value()->workflowId == "execution-profile-v1");
    assert(requested.size() == 1 && "Only a refused Retry is routed to the dialog");

    // The two run policies and the parallelism are the user's own choices and reach the new Run
    // unchanged, in BOTH directions (the first conversion above chose the opposite of each).
    assert(convertedRun.value()->planningCompletionPolicy
            == ai::application::PlanningCompletionPolicy::RequireManifest
        && convertedRun.value()->verificationRepairPolicy
            == ai::application::VerificationRepairPolicy::Disabled);
    const QString fingerprintB = runs.legacyConversionPreview(proposalB)
        .value(QStringLiteral("confirmationFingerprint")).toString();
    const QVariantMap acceptedB = runs.startConvertedRerun(proposalB, QStringLiteral("Policies on"),
        QStringLiteral("Second task"), QString {}, 3, true, true, fingerprintB);
    assert(acceptedB.value(QStringLiteral("accepted")).toBool());
    const QString convertedBId = acceptedB.value(QStringLiteral("runId")).toString();
    static_cast<void>(waitForStatus(runs, convertedBId,
        {QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")}, 20s));
    const auto convertedB = stored.findRun(convertedBId.toStdString());
    assert(convertedB.success() && convertedB.value().has_value()
        && convertedB.value()->planningCompletionPolicy
            == ai::application::PlanningCompletionPolicy::AllowEvidenceBackedNoChanges
        && convertedB.value()->verificationRepairPolicy
            == ai::application::VerificationRepairPolicy::OneBoundedWave
        && convertedB.value()->maxParallelism == 3
        && convertedB.value()->task == "Second task");

    // ---- AG-17.7C-3: the stored audit, as Run Details and the history list see it ----
    // An ordinary Run (the source, a Mock Run, an unknown id) has no record: show nothing.
    for (const QString& ordinary : {proposalA, mockRunId, QStringLiteral("no-such-run")}) {
        assert(runs.legacyConversionRecord(ordinary).value(QStringLiteral("state")).toString()
                == QStringLiteral("none")
            && "An ordinary Run has no conversion record");
    }
    // The explicit conversion: exactly what was confirmed, with the Backend's own time.
    const QVariantMap explicitRecord = runs.legacyConversionRecord(convertedRunId);
    assert(explicitRecord.value(QStringLiteral("state")).toString() == QStringLiteral("present")
        && explicitRecord.value(QStringLiteral("mode")).toString() == QStringLiteral("explicit_user")
        && explicitRecord.value(QStringLiteral("runId")).toString() == convertedRunId
        && explicitRecord.value(QStringLiteral("sourceRunId")).toString() == proposalA
        && explicitRecord.value(QStringLiteral("sourceRunTitle")).toString() == QStringLiteral("Proposal roster A")
        && explicitRecord.value(QStringLiteral("sourceRunAvailable")).toBool()
        && explicitRecord.value(QStringLiteral("sourceWorkflowId")).toString() == QStringLiteral("execution-profile-v1")
        && explicitRecord.value(QStringLiteral("reasonCode")).toString()
            == QStringLiteral("legacy_roster.converted_proposal_roster")
        && explicitRecord.value(QStringLiteral("policyId")).toString() == QStringLiteral("legacy-roster-adapter.v1")
        && explicitRecord.value(QStringLiteral("confirmationFingerprint")).toString() == fingerprintA);
    assert(explicitRecord.value(QStringLiteral("confirmedAt")).toDateTime().isValid()
        && "An explicit conversion records when the user confirmed it");
    assert(explicitRecord.value(QStringLiteral("droppedCount")).toInt() == droppedS4.size()
        && explicitRecord.value(QStringLiteral("dropped")).toList().size() == droppedS4.size()
        && explicitRecord.value(QStringLiteral("hasDroppedInstructions")).toBool());
    for (const char* key : {"sourceSnapshotFingerprint", "targetRosterFingerprint", "confirmationFingerprint"}) {
        assert(sha256.match(explicitRecord.value(QString::fromLatin1(key)).toString()).hasMatch());
    }
    assert(!QString::fromUtf8(QJsonDocument::fromVariant(QVariant(explicitRecord)).toJson())
            .contains(secretInstructions)
        && "The stored audit never shows instruction text, only its length");
    // The safe automatic Retry: no confirmation, and none may be claimed.
    const QVariantMap safeAudit = runs.legacyConversionRecord(safeChild);
    assert(safeAudit.value(QStringLiteral("state")).toString() == QStringLiteral("present")
        && safeAudit.value(QStringLiteral("mode")).toString() == QStringLiteral("not_required_safe_retry")
        && !safeAudit.value(QStringLiteral("confirmedAt")).toDateTime().isValid()
        && safeAudit.value(QStringLiteral("droppedCount")).toInt() == 0
        && !safeAudit.value(QStringLiteral("hasDroppedInstructions")).toBool()
        && safeAudit.value(QStringLiteral("reasonCode")).toString()
            == QStringLiteral("legacy_roster.converted_lone_workspace_lead")
        && "A safe automatic Retry never gains a confirmation time");

    // The history marker: the Backend's index, one value per row.
    runs.refresh(projectId);
    const auto markerOf = [&](const QString& id) {
        for (int row = 0; row < runs.model()->rowCount(); ++row) {
            const QModelIndex index = runs.model()->index(row, 0);
            if (runs.model()->data(index, RunListModel::RunIdRole).toString() == id) {
                return runs.model()->data(index, RunListModel::LegacyConversionRole).toString();
            }
        }
        return QStringLiteral("<missing row>");
    };
    ai::ui::test_support::waitForCondition("history markers loaded", [&] {
        return markerOf(convertedRunId) == QStringLiteral("explicit_user");
    }, [&] { return ("marker=" + markerOf(convertedRunId)).toStdString(); }, 5s);
    assert(markerOf(convertedRunId) == QStringLiteral("explicit_user")
        && markerOf(safeChild) == QStringLiteral("not_required_safe_retry")
        && markerOf(convertedBId) == QStringLiteral("explicit_user")
        && markerOf(proposalA).isEmpty() && markerOf(loneSafe).isEmpty() && markerOf(mockRunId).isEmpty()
        && "Only the converted Runs carry a marker, each with its own mode");
    assert(runs.model()->roleNames().values().contains("legacyConversion"));

    // A record that fails its integrity check is a fault of its own: typed, never a half record
    // and never "nothing". Here the Run's creation time is changed behind the record's back.
    auto tampered = stored.findRun(convertedBId.toStdString());
    assert(tampered.success() && tampered.value().has_value());
    ai::application::Run tamperedRun = *tampered.value();
    tamperedRun.createdAt += std::chrono::hours(1);
    assert(stored.updateRun(tamperedRun).success());
    const QVariantMap unreadable = runs.legacyConversionRecord(convertedBId);
    assert(unreadable.value(QStringLiteral("state")).toString() == QStringLiteral("unreadable")
        && unreadable.value(QStringLiteral("errorCode")).toString()
            == QStringLiteral("legacy_conversion.record_corrupt")
        && !unreadable.value(QStringLiteral("errorMessage")).toString().isEmpty());
    assert((unreadable.keys() == QStringList {QStringLiteral("errorCode"), QStringLiteral("errorMessage"),
                                              QStringLiteral("state")})
        && "A record that cannot be trusted exposes none of its fields");
    // The healthy neighbours are unaffected, and the marker (an index, not the record) still flags it.
    assert(runs.legacyConversionRecord(convertedRunId).value(QStringLiteral("state")).toString()
        == QStringLiteral("present"));
    assert(markerOf(convertedBId) == QStringLiteral("explicit_user"));
}

// AG-17.7C-2: static proof that the conversion dialog is a renderer over the Backend's preview and
// can never become a way to forge or edit a conversion.
void testLegacyConversionDialogQmlOnlyRendersThePreviewAndSendsTheOpaqueFingerprint()
{
    const std::filesystem::path runsDir = std::filesystem::path(AI_ORCHESTRATOR_QML_SOURCE_DIR)
        / "pages" / "Runs";
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        assert(stream.is_open() && "Could not open a QML file for the conversion contract");
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    };
    const std::string dialog = read(runsDir / "Runs_Objects" / "LegacyConversionDialog.qml");
    const std::string page = read(runsDir / "RunsPage.qml");
    const std::string newRun = read(runsDir / "Runs_Objects" / "NewRunDialog.qml");
    const auto count = [](const std::string& text, const std::string& needle) {
        std::size_t found = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
            ++found;
        return found;
    };
    const auto has = [](const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };

    // The one explicit act, and exactly what leaves the dialog: no roster, workflow, parent or flag.
    assert(has(dialog, "Convert and create Run"));
    assert(has(dialog, "signal convertRequested(string sourceRunId, string title, string task, string workspacePath,")
        && has(dialog, "bool automaticVerificationRepair, string expectedConfirmationFingerprint)"));
    assert(has(dialog, "String(dialog.preview.confirmationFingerprint))")
        && "The fingerprint goes back verbatim, never recomputed or built in QML");
    for (const char* forbidden : {"confirmed", "assignmentOverrides", "parentRunId", "workflowId:",
                                  "sha256", "Qt.md5", "CryptoJS", "rosterPayload"}) {
        assert(!has(dialog, forbidden)
            && "The conversion dialog must not mention a confirmation flag, a roster payload, a "
               "workflow or a parent, and must not hash anything");
    }
    // Only the user's own fields are editable: one title, one task, one spin box, two policies.
    assert(count(dialog, "TextField {") == 1 && count(dialog, "TextArea {") == 1
        && count(dialog, "SpinBox {") == 1 && count(dialog, "CheckBox {") == 2
        && count(dialog, "ComboBox {") == 0 && count(dialog, "Switch {") == 0
        && "The roster is locked: no control may edit it");
    // What the Backend returned is shown as plain text, never interpreted.
    assert(count(dialog, "textFormat: Text.PlainText") >= 8);
    // Convertibility is the Backend's call.
    assert(has(dialog, "Boolean(dialog.preview.available)")
        && has(dialog, "dialog.preview.hasDroppedInstructions"));

    // The page: the Backend's `required` flag decides, the typed refusals come back as flags.
    assert(has(page, "source.conversion && source.conversion.required")
        && has(page, "page.backend.runs.startConvertedRerun(")
        && has(page, "result.confirmationStale")
        && has(page, "page.backend.runs.legacyConversionPreview(")
        && has(page, "function onLegacyConversionRequested(sourceRunId)"));
    assert(!has(page, "legacy_roster.") && !has(page, "conversion_confirmation_required")
        && "The page reacts to Backend flags and signals, never to error code strings");
    // The legacy New Run dialog only forwards the source Run id.
    assert(has(newRun, "signal conversionRequested(string sourceRunId)")
        && has(newRun, "Convert to Manifest…")
        && has(newRun, "visible: dialog.conversionOffered"));
}

// AG-17.7C-3: static proof that the conversion audit is only ever RENDERED - read-only, read once
// per selected Run, shown as a fault when it cannot be trusted - in Run Details and the history.
void testLegacyConversionRecordQmlOnlyRendersTheStoredAudit()
{
    const std::filesystem::path runsDir = std::filesystem::path(AI_ORCHESTRATOR_QML_SOURCE_DIR)
        / "pages" / "Runs";
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        assert(stream.is_open() && "Could not open a QML file for the conversion audit contract");
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    };
    const std::string panel = read(runsDir / "Runs_Objects" / "LegacyConversionRecordPanel.qml");
    const std::string details = read(runsDir / "Runs_Objects" / "RunDetailsPanel.qml");
    const std::string list = read(runsDir / "Runs_Objects" / "RunListPanel.qml");
    const std::string page = read(runsDir / "RunsPage.qml");
    const std::string projections = read(runsDir / "Runs_Objects" / "RunModelSnapshots.qml");
    const auto count = [](const std::string& text, const std::string& needle) {
        std::size_t found = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
            ++found;
        return found;
    };
    const auto has = [](const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };

    // Read-only: the audit is immutable, so nothing in the panel can act on it.
    for (const char* control : {"TextField {", "TextArea {", "Button {", "MouseArea {", "CheckBox {",
                                "SpinBox {", "ComboBox {", "Switch {", "RunActionButton {"}) {
        assert(!has(panel, control) && "The conversion audit panel has no control: it is a record, not a form");
    }
    // It talks to the Backend through exactly one read, and never starts or confirms anything.
    assert(count(panel, "panel.runsController.legacyConversionRecord(") == 1);
    for (const char* forbidden : {"startConvertedRerun", "legacyConversionPreview", "retryRun",
                                  "assignmentOverrides", "parentRunId", "Qt.md5", "CryptoJS"}) {
        assert(!has(panel, forbidden) && "The audit panel must not start, confirm or hash anything");
    }
    // Read once per selected Run - never on every progress event of a running Run.
    assert(has(panel, "onRunIdChanged: panel.reload()") && !has(panel, "onRunDataChanged")
        && !has(panel, "Timer {") && !has(panel, "onStatusChanged"));
    // A record that cannot be trusted is a fault of its own, with the Backend's code and message,
    // and none of its fields are rendered; an ordinary Run shows nothing at all.
    assert(has(panel, "The conversion record of this Run could not be read, so none of it is shown.")
        && has(panel, "panel.record.errorCode") && has(panel, "panel.record.errorMessage")
        && has(panel, "visible: panel.present || panel.unreadable"));
    // Only an explicit confirmation is given a time; an automatic Retry never claims one.
    assert(has(panel, "panel.record.mode === \"explicit_user\"")
        && has(panel, "Created by an automatic Retry: nothing was dropped, so no confirmation was needed."));
    // Whatever the Backend returned is plain text.
    assert(count(panel, "textFormat: Text.PlainText") >= 6);

    // It sits in Run Details, fed by the selected Run's id only.
    assert(has(details, "LegacyConversionRecordPanel {")
        && has(details, "runsController: panel.runsController")
        && has(details, "runId: panel.runData ? panel.runData.runId : \"\""));
    // The history shows the Backend's marker and words it; a damaged record is flagged, not hidden.
    assert(has(list, "objectName: \"legacyConversionChip\"")
        && has(list, "String(runItem.modelData.legacyConversion || \"\") !== \"\"")
        && has(list, "runItem.modelData.legacyConversion === \"unreadable\""));
    assert(has(page, "RunModelSnapshots {") && has(page, "backend: page.backend"));
    assert(has(projections, "legacyConversion: it.legacyConversion")
        && has(projections, "required property string legacyConversion")
        && has(projections, "onLegacyConversionChanged: snapshots.scheduleRunsSnapshotRebuild()")
        && has(projections, "function conversionLabel(marker)"));
}

std::string readManifestPresentationQml()
{
    const auto directory = std::filesystem::path(AI_ORCHESTRATOR_QML_SOURCE_DIR)
        / "pages" / "Runs" / "Runs_Objects";
    std::ostringstream source;
    for (const char* file : {"ManifestPanel.qml", "ManifestEnvironmentSection.qml",
                             "ManifestRosterSection.qml", "ManifestTaskCard.qml"}) {
        std::ifstream stream(directory / file, std::ios::binary);
        assert(stream.is_open() && "Could not open a deployed Manifest presentation component");
        source << stream.rdbuf() << '\n';
    }
    return source.str();
}

// AG-17.6: static proof that the shipped QML presents the roster as a pool of available
// workers and never styles an unused worker as a problem.
void testManifestPanelQmlPresentsUnusedWorkersWithoutWarning()
{
    const std::string qml = readManifestPresentationQml();
    assert(qml.find("Available workers") != std::string::npos);
    assert(qml.find("qsTranslate(\"ManifestPanel\", \"Unused\")") != std::string::npos);
    // The participation text is coloured only success (assigned) or muted: a worker
    // without a task is never rendered with the warning or error colour.
    const auto text = qml.find("objectName: \"rosterParticipationText\"");
    assert(text != std::string::npos);
    const auto end = qml.find("font.weight", text);
    assert(end != std::string::npos);
    const std::string block = qml.substr(text, end - text);
    assert(block.find("colors.warning") == std::string::npos);
    assert(block.find("colors.error") == std::string::npos);
}

// AG-17.7B: static proof that the shipped QML words the three price states itself, never
// renders an unknown price as an amount, never computes cost or tokens (it only renders what
// the Backend projected), and shows unattributed usage as diagnostic debt - with the warning
// colour reserved for that debt block.
void testManifestPanelQmlWordsPriceStatesAndNeverComputesCost()
{
    const std::string qml = readManifestPresentationQml();

    const auto functionStart = qml.find("function usageSummary(usage)");
    assert(functionStart != std::string::npos);
    const auto functionEnd = qml.find("\n    }", functionStart);
    assert(functionEnd != std::string::npos);
    const std::string summary = qml.substr(functionStart, functionEnd - functionStart);
    assert(summary.find("usage.priceState === \"none\"") != std::string::npos
        && summary.find("usage.priceState === \"known\"") != std::string::npos
        && summary.find("usage.priceState === \"partial\"") != std::string::npos);
    assert(summary.find("qsTranslate(\"ManifestPanel\", \"price unknown\")") != std::string::npos
        && "An unknown price must be worded, not rendered as an amount");
    // costText is only used by the known and partial branches; the unknown branch is the
    // final else and never touches it.
    const auto unknownBranch = summary.find("else cost = qsTranslate(\"ManifestPanel\", \"price unknown\")");
    assert(unknownBranch != std::string::npos);
    const auto unknownLineEnd = summary.find('\n', unknownBranch);
    assert(summary.substr(unknownBranch, unknownLineEnd - unknownBranch).find("costText")
        == std::string::npos);
    assert(qml.find("costMicros") == std::string::npos
        && "QML must never read or compute raw cost: the Backend projects the amount text");
    assert(qml.find("objectName: \"rosterUsageText\"") != std::string::npos
        && qml.find("objectName: \"usageIncompleteText\"") != std::string::npos
        && qml.find("objectName: \"usageDiagnosticsSection\"") != std::string::npos);
    // A partial amount is worded as a lower bound ("at least ...").
    const auto partialBranch = summary.find("usage.priceState === \"partial\"");
    assert(partialBranch != std::string::npos
        && summary.find("qsTranslate(\"ManifestPanel\", \"at least %1", partialBranch) != std::string::npos);

    // The warning colour appears in the incomplete-attribution line, never in the per-member
    // usage line.
    const auto usageText = qml.find("objectName: \"rosterUsageText\"");
    const auto usageTextEnd = qml.find("font.pixelSize", usageText);
    assert(qml.substr(usageText, usageTextEnd - usageText).find("colors.warning") == std::string::npos);

    // Incomplete attribution speaks only about attribution: it is shown for unattributed rows or
    // an inexact partition, and says "N executions could not be attributed" only when N > 0.
    const auto incomplete = qml.find("objectName: \"usageIncompleteText\"");
    const auto incompleteEnd = qml.find("font.weight", incomplete);
    assert(incompleteEnd != std::string::npos);
    const std::string incompleteBlock = qml.substr(incomplete, incompleteEnd - incomplete);
    assert(incompleteBlock.find("usageAvailable") != std::string::npos
        && incompleteBlock.find("usageUnattributedCount > 0") != std::string::npos
        && incompleteBlock.find("!panel.controller.usagePartitionExact") != std::string::npos
        && incompleteBlock.find("could not be attributed") != std::string::npos
        && incompleteBlock.find("inexact") != std::string::npos
        && incompleteBlock.find("colors.warning") != std::string::npos);

    // Every Backend usage diagnostic is shown whenever there is one, independently of whether
    // usage could be attributed (an unreadable ledger) or the partition is exact (a saturated
    // counter): its visibility depends on the diagnostics alone.
    const auto diagnostics = qml.find("objectName: \"usageDiagnosticsSection\"");
    const auto diagnosticsEnd = qml.find("spacing:", diagnostics);
    assert(diagnosticsEnd != std::string::npos);
    const std::string diagnosticsBlock = qml.substr(diagnostics, diagnosticsEnd - diagnostics);
    assert(diagnosticsBlock.find("usageDiagnostics.length > 0") != std::string::npos
        && diagnosticsBlock.find("usageAvailable") == std::string::npos
        && diagnosticsBlock.find("usagePartitionExact") == std::string::npos
        && diagnosticsBlock.find("usageUnattributedCount") == std::string::npos
        && diagnosticsBlock.find("colors.warning") == std::string::npos);
}

// A.3 (AG-18.4A hardening): static proof that the real deployed QML source
// never labels this action "Create setup Run" (the earlier, semantically
// wrong name this hardening pass corrected) and does render the real
// "Retry after setup" text - reading the actual file on disk rather than
// only asserting the renamed Qt property/method names above, so a stray
// leftover string in the QML itself would be caught too.
void testManifestPanelQmlNeverLabelsRetryAsCreateSetupRun()
{
    const std::string content = readManifestPresentationQml();

    assert(content.find("Create setup Run") == std::string::npos
        && "ManifestPanel.qml must never label the retry action \"Create setup Run\" - it is a "
           "retry after manual/external setup, not a setup-Run launcher (AG-18.4C)");
    assert(content.find(kExpectedRetryAfterSetupButtonText) != std::string::npos
        && "ManifestPanel.qml must render the real \"Retry after setup\" button text");
    assert(content.find("not available yet") != std::string::npos
        && content.find("externally") != std::string::npos
        && content.find("checks that a preset file exists") != std::string::npos
        && "ManifestPanel.qml must disclose existing-Project setup limits and the click-time check");
    assert(content.find("canCreateSetupRun") == std::string::npos
        && content.find("createSetupRun") == std::string::npos
        && "ManifestPanel.qml must not reference the old canCreateSetupRun/createSetupRun names");
}

// PC-5D: the deployed QML of the follow-up planner is a renderer of Backend authority and has NO way to start a Run. These are
// static facts about the shipped files: the paid planner's QML talks only to backend.followUpPlan (the controller it is handed), never
// to the Run, orchestration or provider controllers, and the agreed relabel of the paid verification repair is in place (label only: the
// command and its eligibility are unchanged, see testManifestPanelContinuationActionUsesBackendEligibilityOnly).
std::string readQml(const std::filesystem::path& relative)
{
    const std::filesystem::path path = std::filesystem::path(AI_ORCHESTRATOR_QML_SOURCE_DIR) / relative;
    std::ifstream stream(path, std::ios::binary);
    assert(stream.is_open() && "Could not open a QML file for the follow-up planner contract test");
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void testTheFollowUpPlannerQmlCannotStartAnythingAndTheRelabelIsInPlace()
{
    const std::filesystem::path runsObjects = std::filesystem::path("pages") / "Runs" / "Runs_Objects";
    const std::string panel = readQml(runsObjects / "FollowUpPlanPanel.qml");
    const std::string dialog = readQml(runsObjects / "FollowUpPlanDialog.qml");
    const std::string reply = readQml(runsObjects / "FollowUpReplyDialog.qml");

    for (const auto* file : {&panel, &dialog, &reply}) {
        // No start API, no Run write, no provider call from these files: only the follow-up controller and its dialogs.
        for (const char* forbidden : {"backend.runs", "backend.orchestration", "startRun", "createRun", "retryRun", "backend.chats",
                 "backend.comparisons", "backend.providerCatalog", "backend.credentials", "backend.runDrafts", "backend.manifest"}) {
            assert(file->find(forbidden) == std::string::npos
                && "The follow-up planner's QML must not reach a Run, a provider or the Drafts store directly");
        }
    }
    // The ONE paid command is the dialog's Authorize, and it only asks the controller to authorize a quote it has just shown.
    assert(panel.find("controller.authorize(") != std::string::npos && dialog.find("authorizeRequested(") != std::string::npos
        && "The paid command crosses the typed controller boundary");
    assert(dialog.find("controller.") == std::string::npos && reply.find("controller.") == std::string::npos
        && "The dialogs relay signals; only the panel owns Backend calls");
    assert(panel.find("controller.confirm(") != std::string::npos && panel.find("controller.preview(") != std::string::npos
        && "Confirm and the stored-plan preview are the Backend's");
    // The Backend decides what is allowed, free or charged; the panel only words it.
    assert(panel.find("allowed === true") != std::string::npos && panel.find("mayBeCharged") != std::string::npos
        && "Eligibility and cost honesty come from the Backend's maps");
    assert(dialog.find("worstCaseCostMicros") != std::string::npos && dialog.find("Authorize and send") != std::string::npos
        && "The paid button states the worst-case cost from the quote");
    // The standing promises are on screen.
    assert(dialog.find("Nothing is sent until you press Authorize") != std::string::npos
        && dialog.find("CONTENT of the") != std::string::npos
        && "The dialog must say that nothing is sent before Authorize and that file CONTENT is sent");
    // QML never receives file content: the dialog renders metadata fields only.
    assert(dialog.find("modelData.content") == std::string::npos && dialog.find(".renderedText") == std::string::npos
        && "The planner dialog never touches file content");
    // The Outcome block itself stays read-only: no planner action lives in it.
    const std::string outcome = readQml(runsObjects / "OutcomeBriefPanel.qml");
    assert(outcome.find("followUp") == std::string::npos && outcome.find("authorize") == std::string::npos
        && "The read-only Outcome block carries no planner action");

    // The agreed relabel, and only the label.
    const std::string manifest = readQml(runsObjects / "ManifestPanel.qml");
    assert(manifest.find("Prepare paid verification repair") != std::string::npos
        && manifest.find("qsTr(\"Prepare continuation\")") == std::string::npos
        && manifest.find("panel.controller.prepareVerificationContinuation()") != std::string::npos
        && "Prepare Continuation is relabelled Prepare paid verification repair; the command is unchanged");

    // The panel is hosted in Run Details under the Outcome block, wired to backend.followUpPlan only.
    const std::string details = readQml(runsObjects / "RunDetailsPanel.qml");
    const std::string runsPage = readQml(std::filesystem::path("pages") / "Runs" / "RunsPage.qml");
    assert(details.find("FollowUpPlanPanel {") != std::string::npos && details.find("controller: panel.followUpPlanController") != std::string::npos
        && runsPage.find("followUpPlanController: page.backend.followUpPlan") != std::string::npos
        && "Run Details hosts the follow-up panel and RunsPage hands it backend.followUpPlan");
}

// MW-P5.5: the deployed QML is a renderer of Backend authority. The action
// must bind to the typed canContinue projection and invoke the controller;
// it must not derive eligibility from Run status, verification strings, or
// proposal state in JavaScript/QML.
void testManifestPanelContinuationActionUsesBackendEligibilityOnly()
{
    const std::filesystem::path qmlSourceDir(AI_ORCHESTRATOR_QML_SOURCE_DIR);
    const std::filesystem::path qmlPath = qmlSourceDir
        / "pages" / "Runs" / "Runs_Objects" / "ManifestPanel.qml";
    std::ifstream stream(qmlPath, std::ios::binary);
    assert(stream.is_open() && "Could not open ManifestPanel.qml for the P5.5 contract test");
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();

    assert(content.find("prepareVerificationContinuationButton") != std::string::npos);
    assert(content.find("continuationSection.detail.selectedRole === \"child\"")
            != std::string::npos
        && content.find("!continuationSection.selectedContinuationChild")
            != std::string::npos
        && "A continuation child must not render the source Run's stale Prepare action");
    assert(content.find("continuationSection.detail.canContinue === true") != std::string::npos
        && "Continuation enablement must use the Backend-owned canContinue projection");
    assert(content.find("panel.controller.prepareVerificationContinuation()") != std::string::npos
        && "Preparation must cross the typed Qt controller boundary");
    assert(content.find("verificationContinuationPlannerSkipped") != std::string::npos
        && content.find("verificationContinuationBlockReason") != std::string::npos
        && "The continuation UI must disclose Planner skipping and Backend denial reasons");

    // MW-P5.6B: prepare (free) and the paid repair dispatch are two
    // separate, explicitly confirmed actions - the deployed QML must never
    // wire the paid call directly to a single click. hasPreparedChild gates
    // the paid step on the durable childRunId/childStatus projection plus
    // the Backend-owned canStartRepair decision, never on anything QML
    // derives itself; a local-only confirmation gate must
    // sit between "Start paid repair" and the actual controller call.
    assert(content.find("readonly property bool hasPreparedChild") != std::string::npos
        && content.find("continuationSection.detail.childStatus === \"prepared\"")
            != std::string::npos
        && "The paid-repair step must gate on the durable Prepared child projection");
    assert(content.find("property bool confirmingPaidRepair") != std::string::npos
        && "The paid repair click must not fire the controller call directly");
    assert(content.find("readonly property string preparedChildId") != std::string::npos
        && content.find("onPreparedChildIdChanged: confirmingPaidRepair = false")
            != std::string::npos
        && "Paid confirmation must reset when switching between distinct Prepared children");
    assert(content.find("startVerificationContinuationRepairButton") != std::string::npos
        && content.find("confirmPaidRepairButton") != std::string::npos
        && "Starting paid repair must go through an explicit confirmation step");
    assert(content.find("continuationSection.detail.canStartRepair === true")
            != std::string::npos
        && "Paid repair must use the Backend-owned repair eligibility decision");
    assert(content.find("panel.controller.startVerificationContinuationRepair()") != std::string::npos
        && "Confirmed paid repair must cross the typed Qt controller boundary");
    assert(content.find("startVerificationContinuationRepairButton")
            < content.find("model: continuationSection.detail.findings")
        && "The paid continuation action must stay above the potentially long findings list");
    assert(content.find("panel.controller.continueFailedVerification") == std::string::npos
        && content.find("continueFailedVerificationButton") == std::string::npos
        && "The old combined continueFailedVerification one-click action must be fully removed");
    assert(content.find("materializes new proposed changes") != std::string::npos
        && content.find("apply them to the live workspace") != std::string::npos
        && content.find("create new repair") != std::string::npos
        && content.find("reviewed action") != std::string::npos
        && "Paid-repair confirmation must not falsely claim that repair itself applies files");

    // MW-P5.6A: command-time failures are separate from the read-model's
    // eligibility/block projection. ManifestController already preserves the
    // exact Backend code/message in errorText after its durable refresh; the
    // deployed QML must render that property rather than making a failed
    // prepare()/repair() invocation look like an inert button click.
    assert(content.find("verificationContinuationErrorText") != std::string::npos
        && "ManifestPanel.qml must expose a dedicated continuation command-error item");
    assert(content.find("panel.controller.errorText.length > 0") != std::string::npos
        && content.find("text: panel.controller ? panel.controller.errorText : \"\"")
            != std::string::npos
        && "Continuation command errors must be rendered verbatim from Backend authority");
    assert(content.find("color: panel.colors.error") != std::string::npos
        && "Continuation command errors must use the central error theme color");

    // QML-P0: a terminal Failed child's own honest content disposition -
    // never a substitute for childStatus, never Apply-eligible. Must be
    // rendered from the Backend-owned contentOutcome/remainingFindings
    // projection, never re-derived in QML.
    assert(content.find("verificationContinuationNeedsManualAction") != std::string::npos
        && content.find("continuationSection.detail.contentOutcome === \"needs_manual_action\"")
            != std::string::npos
        && "The continuation UI must disclose the Backend-owned NeedsManualAction projection");
    assert(content.find("model: continuationSection.detail.remainingFindings") != std::string::npos
        && "The NeedsManualAction disclosure must render the Backend-owned remaining findings");
    assert(content.find("modelData.line") != std::string::npos
        && content.find("modelData.column") != std::string::npos
        && content.find("modelData.message") != std::string::npos
        && "A remaining finding must show enough evidence (line/column/bounded message) for a "
           "human to actually act on it, not just its path and tag");
    assert(content.find("proposal safety and all other configured checks") != std::string::npos
        && "The NeedsManualAction text must not claim a specific check (e.g. build) ran when the "
           "classifier only proves every PRESENT non-lint check passed");

    // A Prepared child is an operator decision point. The continuation panel
    // must be near the Run header, not buried after stages/artifacts/logs, and
    // Prepared must remain visible in the list's action-required section.
    const std::filesystem::path detailsPath = qmlSourceDir
        / "pages" / "Runs" / "Runs_Objects" / "RunDetailsPanel.qml";
    std::ifstream detailsStream(detailsPath, std::ios::binary);
    assert(detailsStream.is_open());
    std::ostringstream detailsBuffer;
    detailsBuffer << detailsStream.rdbuf();
    const std::string detailsSource = detailsBuffer.str();
    assert(detailsSource.find("ManifestPanel {") < detailsSource.find("GridLayout {")
        && "Continuation actions must be projected before the long Run detail sections");

    const std::filesystem::path listPath = qmlSourceDir
        / "pages" / "Runs" / "Runs_Objects" / "RunListPanel.qml";
    std::ifstream listStream(listPath, std::ios::binary);
    assert(listStream.is_open());
    std::ostringstream listBuffer;
    listBuffer << listStream.rdbuf();
    const std::string listSource = listBuffer.str();
    assert(listSource.find("r.status === \"prepared\"") != std::string::npos
        && listSource.find("Active / needs action") != std::string::npos
        && "Prepared continuation children must remain visibly actionable in the Run list");

    // The paid boundary must refresh its authority from durable storage,
    // not merely trust the QVariantMap that happened to be projected when
    // the user opened the confirmation UI. Keep this source-level contract
    // beside the deployed-QML contract so a future refactor cannot silently
    // reintroduce cached eligibility across the free/paid gap.
    const std::filesystem::path controllerPath = qmlSourceDir.parent_path()
        / "backend" / "src" / "ManifestController.cpp";
    std::ifstream controllerStream(controllerPath, std::ios::binary);
    assert(controllerStream.is_open()
        && "Could not open ManifestController.cpp for the P5.6B paid-boundary contract test");
    std::ostringstream controllerBuffer;
    controllerBuffer << controllerStream.rdbuf();
    const std::string controllerSource = controllerBuffer.str();
    const auto durableRead = controllerSource.find(
        "continuationReadModel_->viewForRun(selectedRunId_.toStdString())");
    const auto paidDispatch = controllerSource.find(
        "repair->repair(childRunIdStd, stopSource->get_token())");
    assert(durableRead != std::string::npos && paidDispatch != std::string::npos
        && durableRead < paidDispatch
        && "Paid repair must re-read durable continuation authority before provider dispatch");
}

// ---------------------------------------------------------------------
// AG-16.2: Manifest-workflow fixture helpers. Seed a Manifest-workflow Run
// (workflowId=kExecutionManifestWorkflowId, status=Completed) directly via
// IRunRepository - no real Planner/provider execution, matching this
// file's own established "second short-lived BackendRuntime on the same
// on-disk database" pattern - plus a minimal AgentToolSession row (so
// AgentToolReadModelService::session() actually joins in the proposal by
// runId, exactly like a real agent session would) and one real
// ReadyForReview Create proposal via the real ChangeProposalService
// pipeline. seedManifestWorkLedgerVerification() then durably records a
// terminal Work Ledger entry whose ManifestProposalSetVerification is
// built with the exact same canonical computeManifestProposalContentHash
// every production code path uses - never a hand-rolled hash.
// ---------------------------------------------------------------------
struct SeededManifestProposal {
    std::string runId;
    std::string changeSetId;
};

SeededManifestProposal seedManifestRunWithProposal(
    const TempWorkspace& workspace, const std::string& projectId,
    const std::string& relativePath, const std::string& content)
{
    ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
    assert(seed.storageAvailable());

    static int counter = 0;
    const std::string runId = "manifest-run-" + std::to_string(++counter) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    ai::application::Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "AG-16.2 Manifest fixture run";
    run.task = "Exercise the apply-verification gate projection";
    run.workflowId = ai::application::kExecutionManifestWorkflowId;
    run.workspacePath = workspace.projectPath;
    run.status = ai::application::RunStatus::Completed;
    run.createdAt = std::chrono::system_clock::now();
    const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
    assert(database.success());
    ai::infrastructure::SqliteRunRepository runs(database.value());
    const auto insertedRun = runs.insertRun(run);
    assert(insertedRun.success());

    // A minimal, real AgentToolSession row so AgentToolReadModelService::
    // session() actually surfaces this Run's proposal - it joins by
    // session.runId, not by any explicit session<->proposal link (see
    // AgentToolReadModelService.cpp).
    ai::application::AgentToolSession toolSession;
    toolSession.sessionId = "manifest-session-" + runId;
    toolSession.runId = runId;
    toolSession.providerId = "test-provider";
    toolSession.modelId = "test-model";
    toolSession.state = ai::application::AgentToolSessionState::Completed;
    toolSession.createdAt = run.createdAt;
    const auto insertedSession = seed.agentToolSessionRepository().insertSession(toolSession);
    assert(insertedSession.success());

    ai::application::CreateChangeProposalRequest request;
    request.runId = runId;
    request.projectId = projectId;
    request.providerId = "test-provider";
    request.modelId = "test-model";
    request.agentRole = "workspace-access";
    request.rawModelOutput = "```json\n{\"schemaVersion\":\"proposal-submission.v2\","
        "\"summary\":\"Add a file\",\"changes\":[{\"change_id\":\"c1\",\"operation\":\"create\","
        "\"relative_path\":\"" + relativePath + "\",\"source_path\":null,"
        "\"proposed_content\":\"" + content + "\"}]}\n```";
    request.policy.projectRoot = workspace.projectPath;
    const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
    assert(created.success());
    assert(created.value().state == ai::application::ChangeSetState::ReadyForReview);

    return {runId, created.value().changeSetId};
}

// acceptanceStatus/corruptContentHash let a caller build a deliberately
// failing verification (required status not passed / content-hash
// mismatch) while otherwise reusing the exact same seeding logic as the
// eligible case.
void seedManifestWorkLedgerVerification(
    const TempWorkspace& workspace, const std::string& runId, const std::string& projectId,
    const std::string& changeSetId,
    ai::application::ExecutionVerificationStatus acceptanceStatus =
        ai::application::ExecutionVerificationStatus::Passed,
    bool corruptContentHash = false)
{
    ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
    assert(seed.storageAvailable());

    const auto changes = seed.changeProposalService().listChanges(changeSetId);
    assert(changes.success());
    std::vector<ai::application::ManifestProposalContentHashChangeInput> hashInputs;
    for (const auto& change : changes.value()) {
        ai::application::ManifestProposalContentHashChangeInput input;
        input.operation = change.operation;
        input.relativePath = change.relativePath;
        input.sourcePath = change.sourcePath;
        if (change.operation == ai::application::FileChangeOperation::Create
                || change.operation == ai::application::FileChangeOperation::Modify) {
            const auto content = seed.changeProposalService().readProposedContent(
                changeSetId, change.changeId);
            assert(content.success());
            input.contentSha256 = ai::application::sha256Hex(content.value());
        }
        hashInputs.push_back(std::move(input));
    }
    const std::string realHash =
        ai::application::computeManifestProposalContentHash(changeSetId, hashInputs);

    constexpr const char* kBaselineFingerprint = "aio-qt-test-baseline-fingerprint";

    ai::application::ManifestProposalSetVerification verification;
    verification.workspaceIdentity = projectId;
    verification.baselineFingerprint = kBaselineFingerprint;
    verification.overlayFingerprint = "aio-qt-test-overlay-fingerprint";
    verification.orderedProposalIds = {changeSetId};
    verification.orderedProposalContentHashes = {corruptContentHash ? std::string(64, 'f') : realHash};
    verification.proposalSafetyStatus = ai::application::ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ai::application::ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = acceptanceStatus;

    ai::application::ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = projectId;
    entry.terminalStatus = ai::application::RunStatus::Completed;
    entry.baselineFingerprint = kBaselineFingerprint;
    entry.disposition = ai::application::ManifestWorkLedgerDisposition::ManifestReady;
    ai::application::ManifestWorkLedgerTaskDisposition taskDisposition;
    taskDisposition.taskId = "task-1";
    taskDisposition.status = "completed";
    taskDisposition.changeSetId = changeSetId;
    entry.taskDispositions = {taskDisposition};
    entry.verification = verification;
    entry.createdAt = std::chrono::system_clock::now();
    const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
    assert(database.success());
    ai::infrastructure::SqliteExecutionManifestRepository manifests(database.value());
    ai::application::ExecutionManifest manifest;
    manifest.runId = runId;
    assert(manifests.insert(std::move(manifest)).success());
    ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database.value());
    const auto inserted = ledger.insertEntry(entry);
    assert(inserted.success());
}

// A.1: an exactly-verified, fully-passed Manifest proposal projects
// applyAllowed=true with the gate's own kEligibleCode - never computed by
// the controller, always the real ManifestApplyVerificationGate decision.
void testEligibleManifestProposalProjectsApplyAllowedTrue()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 eligible test"),
        QStringLiteral("Verify an eligible Manifest proposal projects applyAllowed=true"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "eligible.txt", "eligible content");
    seedManifestWorkLedgerVerification(
        workspace, seeded.runId, projectId.toStdString(), seeded.changeSetId);

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));

    const QVariantMap detail = agentSessions.selectedProposalDetail();
    assert(detail.value(QStringLiteral("applyAllowed")).toBool());
    assert(detail.value(QStringLiteral("applyBlockCode")).toString()
        == QStringLiteral("manifest_apply.eligible"));
    assert(detail.value(QStringLiteral("applyBlockMessage")).toString().isEmpty());
}

// A.2: a Manifest Run with no Work Ledger verification at all projects
// applyAllowed=false with the expected code/message.
void testMissingVerificationProjectsApplyAllowedFalse()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 missing verification test"),
        QStringLiteral("Verify a Manifest proposal without a verification projects applyAllowed=false"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "missing-verification.txt", "content");
    // Deliberately no seedManifestWorkLedgerVerification() call.

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));

    const QVariantMap detail = agentSessions.selectedProposalDetail();
    assert(!detail.value(QStringLiteral("applyAllowed")).toBool());
    assert(detail.value(QStringLiteral("applyBlockCode")).toString()
        == QStringLiteral("manifest_apply.verification_missing"));
    assert(!detail.value(QStringLiteral("applyBlockMessage")).toString().isEmpty());
}

// A.3: a verification whose required acceptance status did not pass
// projects applyAllowed=false.
void testFailedRequiredVerificationProjectsApplyAllowedFalse()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 failed verification test"),
        QStringLiteral("Verify a failed required verification projects applyAllowed=false"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "failed-verification.txt", "content");
    seedManifestWorkLedgerVerification(
        workspace, seeded.runId, projectId.toStdString(), seeded.changeSetId,
        ai::application::ExecutionVerificationStatus::Failed);

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));

    const QVariantMap detail = agentSessions.selectedProposalDetail();
    assert(!detail.value(QStringLiteral("applyAllowed")).toBool());
    assert(detail.value(QStringLiteral("applyBlockCode")).toString()
        == QStringLiteral("manifest_apply.verification_not_passed"));
}

// A.4: a verification whose stored content hash no longer matches this
// proposal's real durable content projects applyAllowed=false.
void testContentHashMismatchProjectsApplyAllowedFalse()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 hash mismatch test"),
        QStringLiteral("Verify a content hash mismatch projects applyAllowed=false"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "hash-mismatch.txt", "content");
    seedManifestWorkLedgerVerification(
        workspace, seeded.runId, projectId.toStdString(), seeded.changeSetId,
        ai::application::ExecutionVerificationStatus::Passed, /*corruptContentHash=*/true);

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));

    const QVariantMap detail = agentSessions.selectedProposalDetail();
    assert(!detail.value(QStringLiteral("applyAllowed")).toBool());
    assert(detail.value(QStringLiteral("applyBlockCode")).toString()
        == QStringLiteral("manifest_apply.content_hash_mismatch"));
}

// A.5: terminal states (validator-Rejected, and Applied after a successful
// apply) project applyAllowed=false through the gate's existing
// "target must be ReadyForReview" check - no new/replacement gate code is
// invented for either state (see ManifestApplyVerificationGate.cpp's own
// doc comment on proposal.not_ready).
void testTerminalStatesProjectApplyAllowedFalse()
{
    namespace fs = std::filesystem;
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 terminal states test"),
        QStringLiteral("Verify Rejected/Applied proposals project applyAllowed=false"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    // Rejected (validator truth, ChangeSetState::Rejected) - a Modify
    // against a target that does not exist is guaranteed to be rejected
    // with a hash-evidence finding, the same fixture shape
    // ChangeSetApplyServiceTests.cpp's own testApplyNotReady uses.
    std::string rejectedChangeSetId;
    {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());
        ai::application::Run run;
        run.id = "manifest-run-rejected";
        run.projectId = projectId.toStdString();
        run.title = "AG-16.2 rejected fixture run";
        run.task = "Exercise a validator-rejected proposal";
        run.workflowId = ai::application::kExecutionManifestWorkflowId;
        run.workspacePath = workspace.projectPath;
        run.status = ai::application::RunStatus::Completed;
        run.createdAt = std::chrono::system_clock::now();
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteRunRepository runs(database.value());
        assert(runs.insertRun(run).success());
        ai::application::AgentToolSession toolSession;
        toolSession.sessionId = "manifest-session-rejected";
        toolSession.runId = run.id;
        toolSession.providerId = "test-provider";
        toolSession.modelId = "test-model";
        toolSession.state = ai::application::AgentToolSessionState::Completed;
        toolSession.createdAt = run.createdAt;
        assert(seed.agentToolSessionRepository().insertSession(toolSession).success());

        ai::application::CreateChangeProposalRequest request;
        request.runId = run.id;
        request.projectId = projectId.toStdString();
        request.providerId = "test-provider";
        request.modelId = "test-model";
        request.agentRole = "workspace-access";
        request.rawModelOutput = "```json\n{\"schemaVersion\":\"proposal-submission.v2\","
            "\"summary\":\"Modify a missing file\",\"changes\":[{\"change_id\":\"m1\","
            "\"operation\":\"modify\",\"relative_path\":\"missing.txt\",\"source_path\":null,"
            "\"proposed_content\":\"y\"}]}\n```";
        request.policy.projectRoot = workspace.projectPath;
        const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
        assert(created.success());
        assert(created.value().state == ai::application::ChangeSetState::Rejected);
        rejectedChangeSetId = created.value().changeSetId;
    }

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QStringLiteral("manifest-run-rejected")));
    assert(ai::ui::test_support::waitForCondition("Rejected session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(rejectedChangeSetId)));
    QVariantMap rejectedDetail = agentSessions.selectedProposalDetail();
    assert(!rejectedDetail.value(QStringLiteral("applyAllowed")).toBool());

    // Applied - a fully eligible proposal that has already been applied.
    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "applied.txt", "applied content");
    seedManifestWorkLedgerVerification(
        workspace, seeded.runId, projectId.toStdString(), seeded.changeSetId);
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Applied-fixture session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    // Matches the real UI flow: the Apply button only ever exists for the
    // currently *selected* proposal's detail panel, so a genuine
    // approveAndApply() call is always preceded by selectProposal().
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));
    assert(agentSessions.approveAndApply(QString::fromStdString(seeded.changeSetId)));
    assert(fs::exists(workspace.projectPath / "applied.txt"));

    // approveAndApply()'s own refresh() reloads asynchronously; wait for
    // that background reload (and the resulting selectedProposalDetail_
    // re-derivation) to actually land before inspecting it - matches how a
    // real running app's event loop delivers the same completion signal.
    assert(ai::ui::test_support::waitForCondition("Post-apply detail reflects Applied", [&] {
        return agentSessions.selectedProposalDetail().value(QStringLiteral("state")).toString()
            == QStringLiteral("applied");
    }, [&] {
        return agentSessions.selectedProposalDetail().value(QStringLiteral("state"))
            .toString().toStdString();
    }, 4s));

    const QVariantMap appliedDetail = agentSessions.selectedProposalDetail();
    assert(appliedDetail.value(QStringLiteral("changeSetId")).toString()
        == QString::fromStdString(seeded.changeSetId));
    assert(!appliedDetail.value(QStringLiteral("applyAllowed")).toBool());
}

// A.6: a race condition (or any other reason) that makes the real Backend
// apply() call fail after the UI believed the proposal was eligible must
// be handled gracefully - the controller surfaces the authoritative
// Backend error, and the workspace is never mutated.
void testBackendApplyRejectionHandledGracefully()
{
    namespace fs = std::filesystem;
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 race condition test"),
        QStringLiteral("Verify a Backend apply() rejection after UI eligibility is handled gracefully"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "race.txt", "race content");
    seedManifestWorkLedgerVerification(
        workspace, seeded.runId, projectId.toStdString(), seeded.changeSetId);

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));
    assert(agentSessions.selectedProposalDetail().value(QStringLiteral("applyAllowed")).toBool());

    // Simulate the race: another process changes the durable proposal state
    // after the UI projected applyAllowed=true but before confirmation. The
    // controller must publish both the Backend's exact refusal and a freshly
    // evaluated detail instead of leaving a stale enabled action behind.
    {
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteChangeProposalRepository proposals(database.value());
        assert(proposals.updateChangeSetState(
            seeded.changeSetId, ai::application::ChangeSetState::Rejected).success());
    }

    assert(!agentSessions.approveAndApply(QString::fromStdString(seeded.changeSetId)));
    assert(agentSessions.errorCode() == QStringLiteral("proposal.not_ready"));
    assert(!agentSessions.errorText().isEmpty());
    const QVariantMap refreshedDetail = agentSessions.selectedProposalDetail();
    assert(!refreshedDetail.value(QStringLiteral("applyAllowed")).toBool());
    assert(refreshedDetail.value(QStringLiteral("applyBlockCode")).toString()
        == QStringLiteral("proposal.not_ready"));
    assert(!fs::exists(workspace.projectPath / "race.txt"));
}

// A.7: a successful apply immediately updates the read model in place -
// selectedProposalDetail reflects Applied without a fresh selectProposal()
// call, and re-applying is impossible (state is no longer ReadyForReview).
void testSuccessfulApplyUpdatesSelectedProposalDetailInPlace()
{
    namespace fs = std::filesystem;
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-16.2 post-apply projection test"),
        QStringLiteral("Verify a successful apply updates selectedProposalDetail in place"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "post-apply.txt", "post-apply content");
    seedManifestWorkLedgerVerification(
        workspace, seeded.runId, projectId.toStdString(), seeded.changeSetId);

    AgentSessionController& agentSessions = bridge.agentSessionController();
    assert(agentSessions.selectRun(QString::fromStdString(seeded.runId)));
    assert(ai::ui::test_support::waitForCondition("Session load", [&] {
        return agentSessions.hasSession();
    }, [&] { return agentSessions.errorText().toStdString(); }, 4s));
    assert(agentSessions.selectProposal(QString::fromStdString(seeded.changeSetId)));
    assert(agentSessions.selectedProposalDetail().value(QStringLiteral("state")).toString()
        == QStringLiteral("ready_for_review"));

    assert(agentSessions.approveAndApply(QString::fromStdString(seeded.changeSetId)));
    assert(fs::exists(workspace.projectPath / "post-apply.txt"));

    // No selectProposal() call here - approveAndApply()'s own refresh()
    // (asynchronous, like every AgentSessionController reload) must have
    // re-derived the detail for the same changeSetId once it lands, the
    // same way a real running app's event loop would deliver it.
    assert(ai::ui::test_support::waitForCondition("Post-apply detail reflects Applied in place", [&] {
        return agentSessions.selectedProposalDetail().value(QStringLiteral("state")).toString()
            == QStringLiteral("applied");
    }, [&] {
        return agentSessions.selectedProposalDetail().value(QStringLiteral("state"))
            .toString().toStdString();
    }, 4s));

    const QVariantMap detail = agentSessions.selectedProposalDetail();
    assert(detail.value(QStringLiteral("changeSetId")).toString()
        == QString::fromStdString(seeded.changeSetId));
    assert(!detail.value(QStringLiteral("applyAllowed")).toBool());
}

// Exercises one BackendBridge lifetime end to end: SQLite-backed project
// creation, a completed Mock run observed through the queued Qt event
// boundary, and cooperative cancellation of a second run.
void testSingleSessionLifecycle()
{
    TempWorkspace workspace;
    // This scenario intentionally keeps a large number of Qt value objects
    // alive at once. Keep the equally large composition root off the Windows
    // test thread's stack: small, unrelated controller-layout changes used to
    // expose a deterministic stack-corruption crash only while unwinding this
    // function, after every assertion had already passed.
    auto bridgeOwner = std::make_unique<BackendBridge>(workspace.databasePathQt());
    BackendBridge& bridge = *bridgeOwner;
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Qt adapter test"),
        QStringLiteral("End-to-end Mock run"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    ExecutionAnalyticsController& analytics = bridge.executionAnalyticsController();
    assert(bridge.executionAnalytics() == &analytics);
    assert(analytics.refreshProject(projectId));
    assert(analytics.scopeId() == projectId);
    assert(analytics.executionCount() == 0);
    assert(analytics.providers()->rowCount() == 0);
    const QDateTime analyticsNow = QDateTime::currentDateTimeUtc();
    assert(analytics.refreshProjectRange(
        projectId, analyticsNow.addDays(-1), analyticsNow.addDays(1)));
    assert(!analytics.refreshProjectRange(
        projectId, analyticsNow, analyticsNow.addDays(-1)));
    assert(!analytics.refreshSource(QStringLiteral("unknown"), QStringLiteral("scope")));
    assert(projects.model()->rowCount() == 1);
    assert(projects.model()->data(
        projects.model()->index(0, 0),
        ProjectListModel::ProjectIdRole).toString() == projectId);

    // The Qt workspace adapter must persist only through the Application
    // Service; it never writes SQLite or owns selection state independently.
    assert(bridge.workspaceContextController().selectProject(projectId));
    assert(bridge.workspaceContextController().activeProjectId() == projectId);

    // Provider/model metadata travels through ProviderCatalogService and the
    // read-only Qt models. Credentials have a separate write-only controller.
    ProviderCatalogController& catalog = bridge.providerCatalogController();
    assert(catalog.hasBuiltInModelCatalog(QStringLiteral("openai")));
    assert(catalog.hasBuiltInModelCatalog(QStringLiteral("anthropic")));
    assert(!catalog.hasBuiltInModelCatalog(QStringLiteral("local")));
    const QVariantList openAiCatalog = catalog.catalogModelsForProvider(QStringLiteral("openai"));
    const QVariantList anthropicCatalog = catalog.catalogModelsForProvider(QStringLiteral("anthropic"));
    assert(openAiCatalog.size() == 8);
    assert(anthropicCatalog.size() == 4);
    assert(openAiCatalog.front().toMap().value(QStringLiteral("apiModelId")).toString()
        == QStringLiteral("gpt-6-sol"));
    assert(anthropicCatalog.back().toMap().value(QStringLiteral("apiModelId")).toString()
        == QStringLiteral("claude-haiku-4-5-20251001"));
    const QString providerId = catalog.createProvider(
        QStringLiteral("Local test provider"),
        QStringLiteral("local"),
        QStringLiteral("http://127.0.0.1:11434/v1"));
    assert(!providerId.isEmpty());
    assert(catalog.providers()->rowCount() == 1);
    const QString modelId = catalog.createModel(
        providerId,
        QStringLiteral("local-test-model"),
        QStringLiteral("Local test model"),
        32768,
        4096,
        0,
        0);
    assert(!modelId.isEmpty());
    assert(catalog.models()->rowCount() == 1);
    assert(catalog.updateModelPricing(modelId, 2'500'000, 10'000'000, 3'000'000, 250'000));
    const int inputPriceRole = catalog.models()->roleNames().key("inputPriceMicros");
    const int outputPriceRole = catalog.models()->roleNames().key("outputPriceMicros");
    const int cacheWritePriceRole = catalog.models()->roleNames().key("cacheWritePriceMicros");
    const int cacheReadPriceRole = catalog.models()->roleNames().key("cacheReadPriceMicros");
    assert(catalog.models()->data(catalog.models()->index(0, 0), inputPriceRole).toULongLong()
        == 2'500'000);
    assert(catalog.models()->data(catalog.models()->index(0, 0), outputPriceRole).toULongLong()
        == 10'000'000);
    assert(catalog.models()->data(catalog.models()->index(0, 0), cacheWritePriceRole).toULongLong()
        == 3'000'000);
    assert(catalog.models()->data(catalog.models()->index(0, 0), cacheReadPriceRole).toULongLong()
        == 250'000);
    assert(catalog.updateModelPricing(modelId, 0, 0, 0, 0));
    assert(catalog.setDefaultModel(providerId, modelId));

    // Chats consumes the same persisted provider/model ids and attaches the
    // active project automatically. No demo provider names or model ids are
    // manufactured in QML.
    ChatController& chats = bridge.chatController();
    const QVariantList chatChoices = chats.providerChoices();
    assert(chatChoices.size() == 1);
    assert(chatChoices.constFirst().toMap().value(QStringLiteral("modelId")).toString() == modelId);
    const QString conversationId = chats.createConversation(providerId, modelId);
    assert(!conversationId.isEmpty());
    const QVariantList conversations = chats.conversations();
    assert(conversations.size() == 1);
    assert(conversations.constFirst().toMap().value(QStringLiteral("conversationId")).toString()
        == conversationId);
    assert(conversations.constFirst().toMap().value(QStringLiteral("projectId")).toString()
        == projectId);
    assert(chats.messages(conversationId).isEmpty());

    // Comparison is projected from durable sessions plus the shared model-
    // execution ledger. The Qt adapter supplies choices and forwards intent;
    // it does not manufacture result rows or quality scores.
    // Two models of the same provider is the common UI path and exercises
    // parallel client resolution without relying on provider identity to keep
    // candidate calls isolated.
    const QString secondProviderId = providerId;
    const QString secondModelId = catalog.createModel(
        secondProviderId,
        QStringLiteral("second-test-model"),
        QStringLiteral("Second test model"),
        32768,
        4096,
        0,
        0);
    assert(!secondModelId.isEmpty());
    ComparisonController& comparisons = bridge.comparisonController();
    assert(bridge.comparisons() == &comparisons);
    assert(comparisons.modelChoices().size() == 2);
    assert(comparisons.selectProject(projectId));
    QVariantList comparisonSlots;
    comparisonSlots.push_back(QVariantMap {
        {QStringLiteral("providerId"), providerId},
        {QStringLiteral("modelId"), modelId},
        {QStringLiteral("agentName"), QStringLiteral("Lead Architect")},
        {QStringLiteral("role"), QStringLiteral("Software Architect")},
        {QStringLiteral("role"), QStringLiteral("candidate")}});
    comparisonSlots.push_back(QVariantMap {
        {QStringLiteral("providerId"), secondProviderId},
        {QStringLiteral("modelId"), secondModelId},
        {QStringLiteral("role"), QStringLiteral("candidate")}});
    const QString comparisonId = comparisons.createComparison(QVariantMap {
        {QStringLiteral("projectId"), projectId},
        {QStringLiteral("title"), QStringLiteral("Qt comparison")},
        {QStringLiteral("prompt"), QStringLiteral("Solve independently")},
        {QStringLiteral("slots"), comparisonSlots}});
    assert(!comparisonId.isEmpty());
    assert(comparisons.sessionsModel()->rowCount() == 1);
    assert(comparisons.resultsModel()->rowCount() == 2);
    // Creating a comparison now immediately hands the durable candidate rows
    // to ComparisonExecutionManager. These deliberately unreachable local
    // providers therefore fail asynchronously; remaining Queued is no longer
    // the controller contract.
    const QString comparisonStatus = waitForComparisonStatus(
        comparisons, comparisonId, {QStringLiteral("failed")});
    assert(comparisonStatus == QStringLiteral("failed"));
    assert(comparisons.currentSessionStatus() == QStringLiteral("failed"));
    assert(comparisons.summaryData().value(QStringLiteral("totalTokensText")).toString()
        == QStringLiteral("0"));
    for (int row = 0; row < comparisons.resultsModel()->rowCount(); ++row) {
        const QModelIndex index = comparisons.resultsModel()->index(row, 0);
        assert(comparisons.resultsModel()->data(
                   index, ComparisonResultListModel::StatusRole).toString()
            == QStringLiteral("failed"));
        assert(comparisons.resultsModel()->data(
                   index, ComparisonResultListModel::ContentRole).toString().isEmpty());
        assert(!comparisons.resultsModel()->data(
                    index, ComparisonResultListModel::ErrorTextRole).toString().isEmpty());
        assert(!comparisons.resultsModel()->data(
                    index, ComparisonResultListModel::QualityAvailableRole).toBool());
        assert(!comparisons.resultsModel()->data(
                    index, ComparisonResultListModel::CanSelectForSynthesisRole).toBool());
    }

    QVariantList assignments;
    for (const QString& stageType : {
             QStringLiteral("architect"),
             QStringLiteral("implementation"),
             QStringLiteral("documentation")}) {
        QVariantMap assignment;
        assignment.insert(QStringLiteral("stageType"), stageType);
        assignment.insert(QStringLiteral("providerId"), providerId);
        assignment.insert(QStringLiteral("modelId"), modelId);
        assignment.insert(QStringLiteral("timeoutMs"), 30000);
        assignments.push_back(assignment);
    }

    ExecutionProfileController& profiles = bridge.executionProfileController();
    const QString profileId = profiles.createProfile(
        QStringLiteral("Qt production profile"),
        QStringLiteral("execution-profile-v1"),
        assignments);
    assert(!profileId.isEmpty());
    assert(profiles.profiles()->rowCount() == 1);
    assert(profiles.runnableProfiles()->rowCount() == 1);
    assert(profiles.setDefaultProfile(profileId));
    assert(profiles.defaultProfileId() == profileId);
    assert(bridge.settingsController().executionProfileChoices().size() == 2);
    assert(bridge.settingsController().chatModelChoices().size() == 3);

    // Status never reads the secret. A fresh provider has no opaque
    // credentialReference regardless of which OS store backs the runtime.
    assert(!bridge.credentialController().isConfigured(providerId));

    // Worker events must cross the queued Qt boundary and refresh both models.
    RunController& runs = bridge.runController();
    const QString completedRunId = runs.startRun(
        projectId,
        QStringLiteral("Complete Qt adapter run"),
        QStringLiteral("Publish progress through Qt models"));
    assert(!completedRunId.isEmpty());
    assert(waitForStatus(runs, completedRunId, {QStringLiteral("completed")}) == QStringLiteral("completed"));
    assert(runs.stageModel()->rowCount() == 4);
    for (int row = 0; row < runs.stageModel()->rowCount(); ++row) {
        const QModelIndex index = runs.stageModel()->index(row, 0);
        assert(runs.stageModel()->data(index, RunStageModel::StatusRole).toString()
            == QStringLiteral("completed"));
    }
    // A successful Mock run produces a persisted reviewable context artifact.
    // The Qt projection exposes metadata, never a relative/absolute path.
    assert(runs.artifactModel()->rowCount() >= 1);
    const QModelIndex artifactIndex = runs.artifactModel()->index(0, 0);
    assert(!runs.artifactModel()->data(
        artifactIndex, ArtifactListModel::ArtifactIdRole).toString().isEmpty());
    assert(runs.artifactModel()->data(
        artifactIndex, ArtifactListModel::RunIdRole).toString() == completedRunId);
    assert(runs.artifactModel()->data(
        artifactIndex, ArtifactListModel::ByteSizeRole).toULongLong() > 0);
    assert(!runs.artifactModel()->roleNames().values().contains("relativePath"));

    // Cancellation travels from the Qt command API to RunManager's stop token.
    const QString cancelledRunId = runs.startRun(
        projectId,
        QStringLiteral("Cancel Qt adapter run"),
        QStringLiteral("Verify cooperative cancellation"));
    assert(!cancelledRunId.isEmpty());
    assert(waitForStatus(runs, cancelledRunId, {QStringLiteral("running")}) == QStringLiteral("running"));
    assert(runs.cancelRun(cancelledRunId));
    assert(waitForStatus(runs, cancelledRunId, {QStringLiteral("cancelled")}) == QStringLiteral("cancelled"));

    // The custom-roster Qt command must preserve the execution graph policy
    // in SQLite instead of flattening it into a sequential QML-only list.
    QVariantList graphRoster;
    graphRoster.push_back(QVariantMap {
        {QStringLiteral("stageType"), QStringLiteral("architect")},
        {QStringLiteral("providerId"), providerId},
        {QStringLiteral("modelId"), modelId},
        {QStringLiteral("agentName"), QStringLiteral("Lead Architect")},
        {QStringLiteral("role"), QStringLiteral("Software Architect")},
        {QStringLiteral("leadArchitect"), true},
        {QStringLiteral("dependencies"), QVariantList {}},
        {QStringLiteral("maxAttempts"), 2},
        {QStringLiteral("fallbackProviderId"), providerId},
        {QStringLiteral("fallbackModelId"), secondModelId}});
    graphRoster.push_back(QVariantMap {
        {QStringLiteral("stageType"), QStringLiteral("implementation")},
        {QStringLiteral("providerId"), providerId},
        {QStringLiteral("modelId"), modelId},
        {QStringLiteral("agentName"), QStringLiteral("Implementation Agent")},
        {QStringLiteral("role"), QStringLiteral("Implementation specialist")},
        {QStringLiteral("leadArchitect"), false},
        {QStringLiteral("dependencies"), QVariantList {QStringLiteral("architect")}},
        {QStringLiteral("maxAttempts"), 3},
        {QStringLiteral("fallbackProviderId"), providerId},
        {QStringLiteral("fallbackModelId"), secondModelId}});
    // No maxAttempts/fallback here (both default to the no-retry-friendly
    // values): a workspaceAccess agent does not support them (see
    // ExecutionAssignmentValidation's run.workspace_access_no_retry rule).
    graphRoster.push_back(QVariantMap {
        {QStringLiteral("stageType"), QStringLiteral("documentation")},
        {QStringLiteral("providerId"), providerId},
        {QStringLiteral("modelId"), modelId},
        {QStringLiteral("agentName"), QStringLiteral("Workspace Agent")},
        {QStringLiteral("role"), QStringLiteral("Workspace-access specialist")},
        {QStringLiteral("leadArchitect"), false},
        {QStringLiteral("dependencies"), QVariantList {QStringLiteral("architect")}},
        {QStringLiteral("workspaceAccess"), true}});
    const QString graphRunId = runs.startRunWithRoster(
        projectId,
        QStringLiteral("Qt graph contract run"),
        QStringLiteral("Persist graph, retry, fallback and parallelism"),
        graphRoster,
        2,
        QString {},
        QString {},
        QString {},
        false,
        false);
    assert(!graphRunId.isEmpty());
    assert(waitForStatus(runs, graphRunId, {QStringLiteral("failed")})
        == QStringLiteral("failed"));

    const auto graphDatabase = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
    assert(graphDatabase.success());
    ai::infrastructure::SqliteRunRepository graphRuns(graphDatabase.value());
    const auto storedGraphRun = graphRuns.findRun(graphRunId.toStdString());
    assert(storedGraphRun.success() && storedGraphRun.value().has_value());
    assert(storedGraphRun.value()->maxParallelism == 2);
    assert(storedGraphRun.value()->planningCompletionPolicy
        == ai::application::PlanningCompletionPolicy::RequireManifest);
    assert(storedGraphRun.value()->verificationRepairPolicy
        == ai::application::VerificationRepairPolicy::Disabled);
    assert(storedGraphRun.value()->assignmentSnapshot.size() == 3);
    const auto& storedWorker = storedGraphRun.value()->assignmentSnapshot.at(1);
    assert(storedWorker.dependencies == std::vector<std::string> {"architect"});
    assert(storedWorker.maxAttempts == 3);
    assert(storedWorker.fallbackProviderId == providerId.toStdString());
    assert(storedWorker.fallbackModelId == secondModelId.toStdString());
    const auto& storedWorkspaceAgent = storedGraphRun.value()->assignmentSnapshot.at(2);
    assert(storedWorkspaceAgent.workspaceAccess);
    assert(!storedWorker.workspaceAccess);

    // Edit & rerun must consume a Backend-owned draft rather than rebuilding
    // an execution plan from the lossy Run list projection. The Qt mapping
    // therefore has to preserve graph, retry/fallback, workspace-access and
    // immutable workspace fields end to end.
    const QVariantMap graphDraft = runs.rerunDraft(graphRunId);
    assert(graphDraft.value(QStringLiteral("sourceRunId")).toString() == graphRunId);
    // Package 3 (planner-terminal-budget-hardening, 2026-09-28): real-Run
    // incident 7385ad56-1720-4835-b717-123bcff298f9 left only a short
    // planning constraint in the durable Run's task after an Edit & rerun of
    // ade2ca50-0c15-4762-a2b8-3ffb0e6f0b3e. This is the exact assertion the
    // original coverage above never made: the Backend-owned draft QML
    // prefills from must itself carry the SOURCE Run's full, untruncated
    // title/task verbatim - never a shortened summary or a synthesized
    // constraint - so NewRunDialog.qml's titleField/taskField (see
    // NewRunDialog.qml's `prefillData` handling) show the user the complete
    // original text before they edit or launch anything.
    assert(graphDraft.value(QStringLiteral("title")).toString()
        == QStringLiteral("Qt graph contract run"));
    assert(graphDraft.value(QStringLiteral("task")).toString()
        == QStringLiteral("Persist graph, retry, fallback and parallelism"));
    assert(graphDraft.value(QStringLiteral("launchMode")).toString()
        == QStringLiteral("custom_roster"));
    assert(graphDraft.value(QStringLiteral("maxParallelism")).toUInt() == 2);
    assert(!graphDraft.value(
        QStringLiteral("allowEvidenceBackedNoChanges")).toBool());
    assert(!graphDraft.value(
        QStringLiteral("automaticVerificationRepair")).toBool());
    assert(graphDraft.value(QStringLiteral("roster")).toList().size() == 3);
    const QVariantMap workerDraft =
        graphDraft.value(QStringLiteral("roster")).toList().at(1).toMap();
    assert(workerDraft.value(QStringLiteral("dependencies")).toList()
        == QVariantList {QStringLiteral("architect")});
    assert(workerDraft.value(QStringLiteral("maxAttempts")).toUInt() == 3);
    assert(workerDraft.value(QStringLiteral("fallbackProviderId")).toString() == providerId);
    assert(workerDraft.value(QStringLiteral("fallbackModelId")).toString() == secondModelId);
    const QVariantMap workspaceAgentDraft =
        graphDraft.value(QStringLiteral("roster")).toList().at(2).toMap();
    assert(workspaceAgentDraft.value(QStringLiteral("workspaceAccess")).toBool());

    QVariantList editedRoster = graphDraft.value(QStringLiteral("roster")).toList();
    const QString editedGraphRunId = runs.startRunWithRoster(
        projectId,
        QStringLiteral("Edited Qt graph contract run"),
        QStringLiteral("Keep the durable source lineage and full graph"),
        editedRoster,
        graphDraft.value(QStringLiteral("maxParallelism")).toInt(),
        QString {},
        graphRunId,
        QString {},
        false,
        graphDraft.value(QStringLiteral("automaticVerificationRepair")).toBool());
    assert(!editedGraphRunId.isEmpty());
    assert(waitForStatus(runs, editedGraphRunId, {QStringLiteral("failed")})
        == QStringLiteral("failed"));
    const auto storedEditedRun = graphRuns.findRun(editedGraphRunId.toStdString());
    assert(storedEditedRun.success() && storedEditedRun.value().has_value());
    // Package 3: the child Run's durable title/task must equal exactly what
    // this call submitted - not the original title/task, not a
    // concatenation of the two, and not any other hidden merge/append. This
    // is what proves a user's replacement text in NewRunDialog.qml's
    // taskField genuinely becomes the new Run's complete task rather than a
    // silent continuation of the old one.
    assert(storedEditedRun.value()->title == "Edited Qt graph contract run");
    assert(storedEditedRun.value()->task == "Keep the durable source lineage and full graph");
    assert(storedEditedRun.value()->parentRunId == graphRunId.toStdString());
    assert(storedEditedRun.value()->verificationRepairPolicy
        == ai::application::VerificationRepairPolicy::Disabled);
    assert(storedEditedRun.value()->workspacePath == storedGraphRun.value()->workspacePath);
    assert(storedEditedRun.value()->assignmentSnapshot.size() == 3);
    assert(storedEditedRun.value()->assignmentSnapshot.at(1).dependencies
        == std::vector<std::string> {"architect"});
    assert(storedEditedRun.value()->assignmentSnapshot.at(1).maxAttempts == 3);
    assert(storedEditedRun.value()->assignmentSnapshot.at(2).workspaceAccess);

    // Saved-profile identity must remain explicit in the draft. Seed a
    // terminal durable snapshot directly so this adapter-contract test never
    // waits on a real provider timeout merely to obtain historical state.
    // Exact Retry and live provider routing remain covered by Application
    // tests and the existing production-run cases above.
    ai::application::Run savedProfileRun;
    savedProfileRun.id = "qt-saved-profile-draft-run";
    savedProfileRun.projectId = projectId.toStdString();
    savedProfileRun.title = "Qt saved profile run";
    savedProfileRun.task = "Expose saved profile rerun mode";
    savedProfileRun.workflowId = ai::application::kExecutionProfileWorkflowId;
    savedProfileRun.executionProfileId = profileId.toStdString();
    savedProfileRun.assignmentSnapshot = storedGraphRun.value()->assignmentSnapshot;
    savedProfileRun.maxParallelism = 2;
    savedProfileRun.workspacePath = storedGraphRun.value()->workspacePath;
    savedProfileRun.status = ai::application::RunStatus::Failed;
    savedProfileRun.createdAt = std::chrono::system_clock::now();
    savedProfileRun.finishedAt = savedProfileRun.createdAt;
    assert(graphRuns.insertRunWithStages(savedProfileRun, {}).success());
    runs.refresh(projectId);

    const QVariantMap profileDraft = runs.rerunDraft(
        QString::fromStdString(savedProfileRun.id));
    assert(profileDraft.value(QStringLiteral("launchMode")).toString()
        == QStringLiteral("saved_execution_profile"));
    assert(profileDraft.value(QStringLiteral("executionProfileId")).toString() == profileId);
    assert(profileDraft.value(QStringLiteral("roster")).toList().isEmpty());

    const QVariantMap mockDraft = runs.rerunDraft(completedRunId);
    assert(mockDraft.value(QStringLiteral("launchMode")).toString()
        == QStringLiteral("mock"));
    assert(!mockDraft.value(QStringLiteral("workspacePath")).toString().isEmpty());
}

// Proves the actual requirement behind SQLite integration: a second
// BackendBridge (a fresh composition root - new repositories, new
// RunManager, new controllers, exactly what happens on a real app restart)
// opened on the same database file sees the first bridge's project and
// completed run under the same stable IDs, with the run still reporting its
// terminal "completed" state rather than anything reset or left "running".
void testPersistenceAcrossCompositionRootRestart()
{
    TempWorkspace workspace;
    QString projectId;
    QString runId;

    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        projectId = bridge.projectController().createProject(
            QStringLiteral("Restart persistence test"),
            QStringLiteral("Survives a composition-root restart"),
            workspace.projectPathQt());
        assert(!projectId.isEmpty());
        assert(bridge.workspaceContextController().selectProject(projectId));

        SettingsController& settings = bridge.settingsController();
        assert(bridge.settings() == &settings);
        assert(settings.themeMode() == QStringLiteral("dark"));
        assert(settings.setThemeMode(QStringLiteral("transparent")));
        assert(settings.setInterfaceScalePercent(125));
        assert(settings.setCompactMode(true));
        assert(settings.setReduceAnimations(true));
        assert(settings.setRunContextPolicy(QStringLiteral("ask_every_time")));
        assert(settings.setDefaultTimeoutMs(300300));
        assert(settings.setDefaultMaxParallelAgents(7));
        assert(settings.setConfirmManyModels(false));
        assert(settings.setChatContextPolicy(QStringLiteral("no_project")));
        assert(settings.setChatHistoryLimit(500));
        assert(!settings.setInterfaceScalePercent(10));
        assert(settings.interfaceScalePercent() == 125);

        runId = bridge.runController().startRun(
            projectId,
            QStringLiteral("Run before restart"),
            QStringLiteral("Must still read back as completed after restart"));
        assert(!runId.isEmpty());
        assert(waitForStatus(bridge.runController(), runId, {QStringLiteral("completed")})
            == QStringLiteral("completed"));
        // bridge is destroyed here: RunManager's destructor joins the worker
        // thread and SqliteDatabase's destructor closes the connection,
        // exactly like process shutdown.
    }

    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());

        QAbstractItemModel* projectModel = bridge.projectController().model();
        assert(projectModel->rowCount() == 1);
        assert(projectModel->data(projectModel->index(0, 0), ProjectListModel::ProjectIdRole).toString()
            == projectId);
        assert(bridge.workspaceContextController().activeProjectId() == projectId);

        SettingsController& settings = bridge.settingsController();
        assert(settings.themeMode() == QStringLiteral("transparent"));
        assert(settings.interfaceScalePercent() == 125);
        assert(settings.compactMode());
        assert(settings.reduceAnimations());
        assert(settings.runContextPolicy() == QStringLiteral("ask_every_time"));
        assert(settings.defaultTimeoutMs() == 300300);
        assert(settings.defaultMaxParallelAgents() == 7);
        assert(!settings.confirmManyModels());
        assert(settings.chatContextPolicy() == QStringLiteral("no_project"));
        assert(settings.chatHistoryLimit() == 500);

        QAbstractItemModel* runModel = bridge.runController().model();
        // RunController read projections are intentionally asynchronous so
        // a real database cannot block the GUI thread during composition.
        assert(waitForRowCount(runModel, 1));
        const QModelIndex runIndex = runModel->index(0, 0);
        assert(runModel->data(runIndex, RunListModel::RunIdRole).toString() == runId);
        assert(runModel->data(runIndex, RunListModel::StatusRole).toString() == QStringLiteral("completed"));
        assert(runModel->data(runIndex, RunListModel::ProgressRole).toDouble() == 1.0);
        // No run may come back Queued/Running/Cancelling: recovery only
        // touches rows a crash could have left active, and this run already
        // finished cleanly before the restart.
        const QString status = runModel->data(runIndex, RunListModel::StatusRole).toString();
        assert(status != QStringLiteral("running") && status != QStringLiteral("queued")
            && status != QStringLiteral("cancelling"));

        bridge.runController().selectRun(runId);
        assert(bridge.manifestController().selectedRunId() == runId
            && "Run selection must reach ManifestController even when ManifestPanel is hidden");
        assert(bridge.agentSessionController().selectedRunId() == runId
            && "Run selection must reach every Backend-owned Run detail controller");
        // QML-P4R.5 package 2: the new manual-handoff controller must be
        // wired into the SAME Run-selection broadcast as every other
        // Run-detail controller - never a separately (and possibly
        // forgotten) selected Run.
        assert(bridge.manualHandoffController().selectedRunId() == runId
            && "Run selection must reach ManifestVerificationManualHandoffController too");
        // This Run's workflowId is "mock" (MockWorkflowRunner), outside the
        // two anchor workflows materialize() itself ever accepts - the
        // manual-handoff panel must stay hidden (isApplicable=false), never
        // silently show a stale or default-populated Create action.
        assert(!bridge.manualHandoffController().isApplicable()
            && "A non-Manifest/continuation Run must never look applicable for a manual handoff");
        assert(!bridge.manualHandoffController().hasHandoff());
        assert(!bridge.manualHandoffController().canCreate());
        // Guard rails must refuse synchronously - no busy transition, no
        // real Backend dispatch - for a Run the read model itself already
        // ruled out.
        assert(!bridge.manualHandoffController().create());
        assert(!bridge.manualHandoffController().reverify());
        assert(!bridge.manualHandoffController().discard());
        assert(!bridge.manualHandoffController().busy());
        assert(!bridge.manualHandoffController().cancel());
        assert(!bridge.manualHandoffController().openCandidateLocation());
        assert(bridge.manualHandoffController().errorText().startsWith(
            QStringLiteral("manual_handoff.not_found:")));
        assert(!bridge.manualHandoffController().copyCandidatePathToClipboard());
        assert(bridge.manualHandoffController().errorText().startsWith(
            QStringLiteral("manual_handoff.not_found:")));
        // Post-P4R.6 human review/Apply hardening: with no authoritative
        // review member ever loaded, every member-scoped command must refuse
        // synchronously rather than reach the Backend with an arbitrary id.
        assert(bridge.manualHandoffController().reviewMembers().isEmpty());
        assert(!bridge.manualHandoffController().approveAndApplyMember(QStringLiteral("anything")));
        assert(bridge.manualHandoffController().errorText().startsWith(
            QStringLiteral("manual_handoff.review_member_not_found:")));
        assert(!bridge.manualHandoffController().rejectMember(QStringLiteral("anything"), QString()));
        QAbstractItemModel* stageModel = bridge.runController().stageModel();
        assert(waitForRowCount(stageModel, 4));
        for (int row = 0; row < stageModel->rowCount(); ++row) {
            assert(stageModel->data(stageModel->index(row, 0), RunStageModel::StatusRole).toString()
                == QStringLiteral("completed"));
        }

        // Artifact metadata and its stable ID survive a fresh Runtime and Qt
        // controller composition just like the Run/Stage aggregate.
        QAbstractItemModel* artifactModel = bridge.runController().artifactModel();
        assert(artifactModel->rowCount() >= 1);
        const QModelIndex artifactIndex = artifactModel->index(0, 0);
        assert(artifactModel->data(
            artifactIndex, ArtifactListModel::RunIdRole).toString() == runId);
        assert(!artifactModel->data(
            artifactIndex, ArtifactListModel::ArtifactIdRole).toString().isEmpty());
    }
}

// QML-P4R.5 package 3: the deployed manual-handoff surface must remain a
// direct renderer/command adapter over the dedicated controller. In
// particular it may display only relative allowed paths, must label local
// verification provider-free, and must never invent an Apply action or start
// work from a timer/file event.
void testManualHandoffPanelUsesBackendProjectionOnly()
{
    const std::filesystem::path qmlSourceDir(AI_ORCHESTRATOR_QML_SOURCE_DIR);
    const std::filesystem::path panelPath = qmlSourceDir
        / "pages" / "Runs" / "Runs_Objects" / "ManualHandoffPanel.qml";
    std::ifstream panelStream(panelPath, std::ios::binary);
    assert(panelStream.is_open());
    std::ostringstream panelBuffer;
    panelBuffer << panelStream.rdbuf();
    const std::string panelRoot = panelBuffer.str();
    std::string panel = panelRoot;
    for (const char* component : {"ManualHandoffCandidateChanges", "ManualHandoffAttemptHistory",
                                  "ManualHandoffReviewSection"}) {
        assert(panelRoot.find(std::string(component) + " {") != std::string::npos
            && "The handoff panel must instantiate each extracted section");
        const auto componentPath = panelPath.parent_path() / (std::string(component) + ".qml");
        std::ifstream componentStream(componentPath, std::ios::binary);
        assert(componentStream.is_open());
        std::ostringstream componentBuffer;
        componentBuffer << componentStream.rdbuf();
        const std::string source = componentBuffer.str();
        assert(source.find("required property var controller") != std::string::npos);
        panel += "\n" + source;
    }
    assert(panelRoot.find("onResetRequested:") != std::string::npos
        && panelRoot.find("onMemberSelectionRequested:") != std::string::npos
        && panelRoot.find("onDiffToggleRequested:") != std::string::npos
        && panelRoot.find("onFindingsRequested:") != std::string::npos
        && "Section actions must be relayed to the panel's existing handlers");

    assert(panel.find("panel.controller.isApplicable") != std::string::npos
        && panel.find("panel.controller.canCreate") != std::string::npos
        && panel.find("panel.controller.canApply") != std::string::npos
        && "Visibility/Create/Apply truth must come from Backend projections");
    assert(panel.find("panel.controller.create()") != std::string::npos
        && panel.find("panel.controller.reverify()") != std::string::npos
        && panel.find("panel.controller.discard()") != std::string::npos
        && panel.find("panel.controller.cancel()") != std::string::npos
        && "Every mutating action must cross the dedicated Qt controller boundary");
    assert(panel.find("panel.controller.openCandidateLocation()") != std::string::npos
        && panel.find("panel.controller.copyCandidatePathToClipboard()") != std::string::npos
        && "Native path actions must remain inside the C++ controller");
    assert(panel.find("panel.controller.allowedRelativePaths") != std::string::npos
        && panel.find("candidatePath") == std::string::npos
        && panel.find("absolutePath") == std::string::npos
        && panel.find("file://") == std::string::npos
        && "QML may render safe relative paths but must never receive an absolute path");
    assert(panel.find("panel.controller.attempts") != std::string::npos
        && panel.find("panel.controller.hasFinalVerification") != std::string::npos
        && panel.find("panel.controller.finalProposalSafetyStatus") != std::string::npos
        && panel.find("panel.controller.finalBuildLintTestStatus") != std::string::npos
        && panel.find("panel.controller.finalAcceptanceStatus") != std::string::npos
        && "Attempt history and verification must be rendered from the read model");
    assert(panel.find("attemptRow.modelData.hasVerification") != std::string::npos
        && panel.find("attemptRow.modelData.buildLintTestStatus") != std::string::npos
        && "Failed verified attempts must expose their exact Backend statuses in handoff history");
    // Findings of an attempt: the Backend's buckets are only COUNTED in one line per attempt and listed in the findings window;
    // the six inline small-type lists were a second copy of the same findings and are gone. The legacy flat
    // verificationDiagnostics list is still never rendered.
    assert(panel.find("function attemptFindingSummary(attempt)") != std::string::npos
        && panel.find("attempt.actionableDiagnostics") != std::string::npos
        && panel.find("attempt.outsideScopeDiagnostics") != std::string::npos
        && panel.find("attempt.generalDiagnostics") != std::string::npos
        && panel.find("attempt.newActionableDiagnostics") != std::string::npos
        && panel.find("attempt.newOutsideManifestOrContextDiagnostics") != std::string::npos
        && panel.find("attempt.unchangedBaselineDebtDiagnostics") != std::string::npos
        && panel.find("attemptRow.modelData.verificationDiagnostics") == std::string::npos
        && "Attempt findings must be summarised from the Backend buckets, never from the legacy flat list");
    assert(panel.find("manualHandoffAttemptFindingSummary") != std::string::npos
        && panel.find("manualHandoffShowFindingsButton") != std::string::npos
        && panel.find("manualHandoffActionableDiagnostics") == std::string::npos
        && panel.find("manualHandoffOutsideScopeDiagnostics") == std::string::npos
        && panel.find("manualHandoffGeneralDiagnostics") == std::string::npos
        && panel.find("manualHandoffNewActionableDiagnostics") == std::string::npos
        && panel.find("manualHandoffNewOutsideManifestOrContextDiagnostics") == std::string::npos
        && panel.find("manualHandoffUnchangedBaselineDebtDiagnostics") == std::string::npos
        && "Attempt findings are counted in one line and listed in the window, not printed a second time inline");
    assert(panel.find("panel.controller.errorText") != std::string::npos
        && panel.find("panel.controller.applyResultText") != std::string::npos
        && panel.find("panel.controller.createReasonCode") != std::string::npos
        && panel.find("panel.controller.applyReasonCode") != std::string::npos
        && "Bounded Backend refusal/error text must stay visible");
    assert(panel.find("provider-free") != std::string::npos
        && panel.find("property bool confirmingDiscard") != std::string::npos
        && panel.find("manualHandoffConfirmDiscardButton") != std::string::npos
        && "Local verification needs honest labeling and destructive discard confirmation");
    assert(panel.find("function onRunUpdated(runId)") != std::string::npos
        && panel.find("runId === panel.runId") != std::string::npos
        && panel.find("panel.controller.refresh()") != std::string::npos
        && "The selected Run becoming terminal must refresh handoff eligibility without reselection");
    assert(panel.find("Timer {") == std::string::npos
        && panel.find("FolderListModel") == std::string::npos
        && panel.find("FileSystemWatcher") == std::string::npos
        && "No timer or file watcher may auto-start/drive manual handoff work");
    assert(panel.find("Continue in proposal review") == std::string::npos
        && "The old dead-end text must be removed now that exact-member review/Apply lives here");
    assert(panel.find("panel.controller.reviewMembers") != std::string::npos
        && panel.find("panel.controller.selectMember(") != std::string::npos
        && panel.find("panel.controller.selectedMemberProposalId") != std::string::npos
        && panel.find("panel.controller.selectedMemberDetail") != std::string::npos
        && "The exact-member review surface must read only the dedicated controller projection");
    assert(panel.find("panel.controller.approveAndApplyMember(") != std::string::npos
        && panel.find("panel.controller.rejectMember(") != std::string::npos
        && "Apply/Reject must cross the dedicated controller boundary, never a synthesized session");
    assert(panel.find("panel.controller.readOriginalContent(") != std::string::npos
        && panel.find("panel.controller.readProposedContent(") != std::string::npos
        && "Diff content must be fetched through the controller by typed (proposalId, changeId)");
    assert(panel.find("selectedMemberDetail.applyAllowed") != std::string::npos
        && "Per-member Apply eligibility must come from the real AG-16 projection, never be inferred");
    assert(panel.find("manualHandoffApplyResultText") != std::string::npos
        && panel.find("&& !panel.controller.busy") != std::string::npos
        && "Review mutations must be serialized and successful Apply warnings must be rendered");

    // Provider-free reset hardening: the candidate-changes section, its two
    // Backend-projected path lists, the explicit Refresh/Reset actions, and
    // the two-step Reset confirmation must all be present with stable
    // objectNames a real-QML test can locate without searching by text.
    assert(panel.find("manualHandoffCandidateChanges") != std::string::npos
        && panel.find("panel.controller.editableChangedRelativePaths") != std::string::npos
        && panel.find("panel.controller.resetRequiredRelativePaths") != std::string::npos
        && "Candidate diff lists must be a direct, unmodified Backend projection");
    assert(panel.find("manualHandoffRefreshCandidateButton") != std::string::npos
        && panel.find("panel.controller.refresh()") != std::string::npos
        && "An explicit Refresh action must exist beside the candidate-changes section");
    assert(panel.find("manualHandoffResetButton") != std::string::npos
        && panel.find("property bool confirmingReset") != std::string::npos
        && panel.find("manualHandoffConfirmResetButton") != std::string::npos
        && panel.find("manualHandoffCancelResetButton") != std::string::npos
        && panel.find("panel.controller.resetNonEditableChanges()") != std::string::npos
        && "Reset must require explicit two-step confirmation before calling the Backend command");
    assert(panel.find("manualHandoffResetResultText") != std::string::npos
        && panel.find("panel.controller.resetResultText") != std::string::npos
        && "A successful reset's Backend summary must remain visible");
    assert(panel.find("FileSystemWatcher") == std::string::npos
        && panel.find("FolderListModel") == std::string::npos
        && "Reset/refresh must never be driven by a filesystem watcher");

    const std::filesystem::path detailsPath = qmlSourceDir
        / "pages" / "Runs" / "Runs_Objects" / "RunDetailsPanel.qml";
    std::ifstream detailsStream(detailsPath, std::ios::binary);
    assert(detailsStream.is_open());
    std::ostringstream detailsBuffer;
    detailsBuffer << detailsStream.rdbuf();
    const std::string details = detailsBuffer.str();
    assert(details.find("property var manualHandoffController: null") != std::string::npos
        && details.find("ManualHandoffPanel {") != std::string::npos
        && details.find("controller: panel.manualHandoffController") != std::string::npos
        && "Run Details must host the dedicated controller without merging it into ManifestPanel");

    const std::filesystem::path runsPagePath = qmlSourceDir
        / "pages" / "Runs" / "RunsPage.qml";
    std::ifstream runsPageStream(runsPagePath, std::ios::binary);
    assert(runsPageStream.is_open());
    std::ostringstream runsPageBuffer;
    runsPageBuffer << runsPageStream.rdbuf();
    const std::string runsPage = runsPageBuffer.str();
    assert(runsPage.find(
        "manualHandoffController: page.backend.manualHandoff") != std::string::npos
        && "RunsPage must pass BackendBridge's real manual-handoff controller");
    assert(runsPage.find("page.backend.manualHandoff.refresh()") != std::string::npos
        && "The explicit Runs refresh must refresh the separate handoff read model too");

    const std::filesystem::path cmakePath = qmlSourceDir / "CMakeLists.txt";
    std::ifstream cmakeStream(cmakePath, std::ios::binary);
    assert(cmakeStream.is_open());
    std::ostringstream cmakeBuffer;
    cmakeBuffer << cmakeStream.rdbuf();
    assert(cmakeBuffer.str().find(
        "pages/Runs/Runs_Objects/ManualHandoffPanel.qml") != std::string::npos
        && "The deployed QML module must compile/register ManualHandoffPanel.qml");
    for (const char* component : {"ManualHandoffCandidateChanges", "ManualHandoffAttemptHistory",
                                  "ManualHandoffReviewSection"}) {
        assert(cmakeBuffer.str().find(std::string("pages/Runs/Runs_Objects/") + component + ".qml")
                != std::string::npos
            && "Each extracted handoff section must be registered in the deployed QML module");
    }

    // REMAIN_TO_DO 6a (b)+(c): the recorded findings stand in ONE shared block above BOTH repair panels, are
    // registered in the module, are not rendered a second time inside the handoff panel, and the block gates
    // nothing (no command call, no enabled binding, no timer).
    assert(cmakeBuffer.str().find(
        "pages/Runs/Runs_Objects/RecordedFindingsBlock.qml") != std::string::npos
        && "The deployed QML module must compile/register RecordedFindingsBlock.qml");
    const auto blockAt = details.find("RecordedFindingsBlock {");
    assert(blockAt != std::string::npos && blockAt < details.find("ManifestPanel {")
        && blockAt < details.find("ManualHandoffPanel {")
        && "The recorded findings must be placed above both the paid and the free repair panel");
    assert(panel.find("manualHandoffInitialScopeFindings") == std::string::npos
        && panel.find("panel.controller.initialScopeDiagnostics") == std::string::npos
        && "The handoff panel must not render the same findings a second time");
    const std::filesystem::path blockPath = qmlSourceDir
        / "pages" / "Runs" / "Runs_Objects" / "RecordedFindingsBlock.qml";
    std::ifstream blockStream(blockPath, std::ios::binary);
    assert(blockStream.is_open());
    std::ostringstream blockBuffer;
    blockBuffer << blockStream.rdbuf();
    const std::string block = blockBuffer.str();
    assert(block.find("block.controller.recordedFindings") != std::string::npos
        && block.find("Recorded findings from the failed verification") != std::string::npos
        && block.find("ManualHandoffFindingsDialog {") != std::string::npos
        && block.find("recordedFindingsShowAllButton") != std::string::npos
        && block.find("maximumInlineFindingsPerGroup") != std::string::npos
        && block.find("all findings") == std::string::npos
        && "The block reads the Backend projection, bounds the inline preview, reuses the findings "
           "window and never calls the list complete");
    assert(block.find("enabled:") == std::string::npos
        && block.find(".create(") == std::string::npos
        && block.find(".reverify(") == std::string::npos
        && block.find("Timer {") == std::string::npos
        && "The informational block must not gate or start anything");
}

// QML-P4R.5 package-2 hardening: an async command remains bound to the Run
// that launched it even when the global selection changes before the queued
// QFutureWatcher completion reaches the GUI thread. The old Run's error may
// be reported by commandFinished(commandRunId, succeeded), but must never be
// pasted into the newly selected Run's errorText/read-model projection.
void testManualHandoffCommandCompletionDoesNotPolluteNewRunSelection()
{
    TempWorkspace workspace;
    QString projectId;
    {
        BackendBridge seed(workspace.databasePathQt());
        assert(seed.storageAvailable());
        projectId = seed.projectController().createProject(
            QStringLiteral("Manual handoff command identity"),
            QStringLiteral("Proves stale async completion isolation"),
            workspace.projectPathQt());
        assert(!projectId.isEmpty());
    }

    const QString anchorRunId = QStringLiteral("manual-handoff-command-anchor");
    const QString otherRunId = QStringLiteral("manual-handoff-command-other");
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(opened.success());
        ai::infrastructure::SqliteRunRepository runs(opened.value());
        ai::infrastructure::SqliteManifestVerificationManualHandoffRepository handoffs(
            opened.value());
        const auto now = std::chrono::system_clock::now();

        ai::application::Run anchor;
        anchor.id = anchorRunId.toStdString();
        anchor.projectId = projectId.toStdString();
        anchor.title = "Failed Manifest anchor";
        anchor.task = "Repair QML lint";
        anchor.workflowId = ai::application::kExecutionManifestWorkflowId;
        anchor.status = ai::application::RunStatus::Failed;
        anchor.createdAt = now;
        assert(runs.insertRun(anchor).success());

        ai::application::Run other;
        other.id = otherRunId.toStdString();
        other.projectId = projectId.toStdString();
        other.title = "Unrelated selected Run";
        other.task = "Stay isolated from the old command";
        other.workflowId = ai::application::kMockWorkflowId;
        other.status = ai::application::RunStatus::Completed;
        other.createdAt = now;
        assert(runs.insertRun(other).success());

        ai::application::ManifestVerificationManualHandoff handoff;
        handoff.id = "manual-handoff:" + anchor.id;
        handoff.originManifestRunId = anchor.id;
        handoff.verificationAnchorRunId = anchor.id;
        handoff.sourceProjectId = anchor.projectId;
        handoff.storageKey = "stale-completion-candidate";
        handoff.workspaceIdentity = anchor.projectId;
        handoff.baselineFingerprint = std::string(64, 'a');
        handoff.sourceOverlayFingerprint = std::string(64, 'b');
        handoff.frozenMembers = {{"stale-member", anchor.id,
            std::string(64, 'c'), "stale-member"}};
        handoff.allowedPaths = {{"qml/App.qml", "stale-member", "component"}};
        // Empty snapshot must be self-consistent: SQLite parsing recomputes
        // the fingerprint instead of accepting an arbitrary SHA-shaped value.
        handoff.originalSnapshot.fingerprint = ai::application::sha256Hex("");
        handoff.createdAt = now;
        assert(handoffs.insertHandoff(handoff).success());
    }

    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());
    auto& controller = bridge.manualHandoffController();
    if (!controller.selectRun(anchorRunId)) {
        std::fprintf(stderr, "manual-handoff select failed: %s\n",
            controller.errorText().toUtf8().constData());
        assert(false);
    }
    assert(controller.isApplicable() && controller.hasHandoff());
    {
        // No Work Ledger verification is seeded here: the recorded-findings projection must exist with
        // every key, say nothing (no findings) and stay "unknown" - never an invented verdict.
        const auto recorded = controller.recordedFindings();
        assert(recorded.contains(QStringLiteral("hasFindings"))
            && recorded.contains(QStringLiteral("findings"))
            && recorded.contains(QStringLiteral("debtCount"))
            && recorded.contains(QStringLiteral("verdict"))
            && recorded.contains(QStringLiteral("verdictCode"))
            && recorded.contains(QStringLiteral("verdictMessage"))
            && recorded.contains(QStringLiteral("outsideScopePaths"))
            && recorded.contains(QStringLiteral("classificationErrorCode"))
            && recorded.contains(QStringLiteral("classificationErrorMessage")));
        assert(!recorded.value(QStringLiteral("hasFindings")).toBool());
        assert(recorded.value(QStringLiteral("verdict")).toString() == QStringLiteral("unknown"));
    }

    QString finishedRunId;
    bool finishedSucceeded = true;
    int finishedCount = 0;
    QObject::connect(&controller,
        &ManifestVerificationManualHandoffController::commandFinished,
        [&](const QString& commandRunId, bool succeeded) {
            finishedRunId = commandRunId;
            finishedSucceeded = succeeded;
            ++finishedCount;
        });

    // The candidate directory was deliberately not created, so Reverify
    // will fail in the worker. Request cancellation too: the controller must
    // expose a genuine stop_source while busy irrespective of which failure
    // wins the worker race.
    assert(controller.reverify());
    assert(controller.busy());
    assert(controller.canCancel());
    assert(controller.activeCommandRunId() == anchorRunId);

    // Hardening: synchronous review mutations must share the controller's
    // command-serialization boundary. Otherwise the previously-authoritative
    // member could be Applied/Rejected while Reverify is constructing a new
    // authoritative attempt in the background.
    assert(!controller.approveAndApplyMember(QStringLiteral("stale-member")));
    assert(controller.errorText().startsWith(
        QStringLiteral("manual_handoff.command_busy:")));
    assert(!controller.rejectMember(
        QStringLiteral("stale-member"), QStringLiteral("must stay serialized")));
    assert(controller.errorText().startsWith(
        QStringLiteral("manual_handoff.command_busy:")));
    assert(controller.cancel());
    assert(!controller.canCancel());

    // The queued finished callback cannot execute on this GUI thread until
    // control returns to the event loop, so this deterministically changes
    // selection before finishCommand() projects the outcome.
    assert(controller.selectRun(otherRunId));
    assert(controller.selectedRunId() == otherRunId);
    assert(!controller.isApplicable());
    assert(controller.errorText().isEmpty());
    assert(controller.activeCommandRunId() == anchorRunId);

    assert(ai::ui::test_support::waitForCondition("Manual-handoff command completion", [&] {
        return !controller.busy() && finishedCount == 1;
    }, [&] {
        return ("busy=" + std::to_string(controller.busy())
            + ", finishedCount=" + std::to_string(finishedCount));
    }, 4s));
    assert(controller.selectedRunId() == otherRunId);
    assert(!controller.isApplicable());
    assert(controller.errorText().isEmpty()
        && "The old Run's worker error must never pollute the new selection");
    assert(controller.activeCommandRunId().isEmpty());
    assert(!controller.canCancel());
    assert(finishedRunId == anchorRunId && !finishedSucceeded
        && "Completion notification must retain the originating Run identity");
}

// Post-P4R.6 human review/Apply hardening: end-to-end real proof that an
// authoritative manual-handoff member is reviewable and Apply-eligible
// through ManifestVerificationManualHandoffController alone - never through
// AgentSessionController/AgentToolSession. Mirrors seedManifestRunWithProposal/
// seedManifestWorkLedgerVerification's own established pattern (a real
// ChangeProposalService-created proposal, a real canonical content hash), but
// keys the Work Ledger entry to a SEPARATE manual-handoff ATTEMPT Run - which
// deliberately gets NO AgentToolSession row at all (unlike the origin Run,
// which incidentally has one only because it shares seedManifestRunWithProposal's
// own generic fixture helper) - exactly the real production shape: the
// attempt Run that actually carries Apply authority never runs a provider
// session. Proves, against the real SQLite-backed ManifestApplyVerificationGate:
// the member is listed with real change/finding/apply-eligibility detail,
// selectMember()/read*Content() work without a session, an unknown proposal
// id fails closed, Approve-and-apply genuinely writes the file and the
// second, unrelated Reject action never mutates a member it was not called
// on, and the member re-reads as Applied afterward.
void testManualHandoffReviewMemberRealApplyEndToEnd()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Manual handoff review/Apply"),
        QStringLiteral("Proves exact-member human review/Apply without an AgentToolSession"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "reviewed.txt", "reviewed content");
    const std::string attemptRunId = seeded.runId + "-manual-handoff-attempt";

    // Real canonical content hash - the exact same algorithm/inputs
    // seedManifestWorkLedgerVerification uses, computed independently here
    // because that helper keys its ledger entry to the SAME Run as the
    // proposal, while this handoff's authoritative ledger entry must be
    // keyed to the separate attempt Run instead.
    std::string realHash;
    {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());
        const auto changes = seed.changeProposalService().listChanges(seeded.changeSetId);
        assert(changes.success() && changes.value().size() == 1);
        ai::application::ManifestProposalContentHashChangeInput input;
        input.operation = changes.value().front().operation;
        input.relativePath = changes.value().front().relativePath;
        input.sourcePath = changes.value().front().sourcePath;
        const auto content = seed.changeProposalService().readProposedContent(
            seeded.changeSetId, changes.value().front().changeId);
        assert(content.success());
        input.contentSha256 = ai::application::sha256Hex(content.value());
        realHash = ai::application::computeManifestProposalContentHash(
            seeded.changeSetId, {input});
    }

    {
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteRunRepository runs(database.value());

        ai::application::Run attempt;
        attempt.id = attemptRunId;
        attempt.projectId = projectId.toStdString();
        attempt.title = "Manual handoff attempt";
        attempt.task = "Provider-free reverify";
        attempt.workflowId = std::string(ai::application::kManifestVerificationManualHandoffWorkflowId);
        attempt.parentRunId = seeded.runId;
        attempt.status = ai::application::RunStatus::Completed;
        attempt.createdAt = std::chrono::system_clock::now();
        assert(runs.insertRun(attempt).success());
        // Deliberately NO AgentToolSession row for attemptRunId - the real
        // production shape this test proves: a manual-handoff attempt never
        // creates a provider/agent-tool session.

        ai::infrastructure::SqliteManifestVerificationManualHandoffRepository handoffs(
            database.value());
        ai::application::ManifestVerificationManualHandoff handoff;
        handoff.id = "manual-handoff:" + seeded.runId;
        handoff.originManifestRunId = seeded.runId;
        handoff.verificationAnchorRunId = seeded.runId;
        handoff.sourceProjectId = projectId.toStdString();
        handoff.storageKey = "review-apply-e2e-candidate";
        handoff.workspaceIdentity = projectId.toStdString();
        handoff.baselineFingerprint = std::string(64, 'a');
        handoff.sourceOverlayFingerprint = std::string(64, 'b');
        handoff.frozenMembers = {
            {seeded.changeSetId, seeded.runId, realHash, seeded.changeSetId}};
        handoff.allowedPaths = {{"reviewed.txt", seeded.changeSetId, "task-1"}};
        handoff.originalSnapshot.fingerprint = ai::application::sha256Hex("");
        handoff.createdAt = std::chrono::system_clock::now();
        assert(handoffs.insertHandoff(handoff).success());

        ai::application::ManifestVerificationManualHandoffAttempt attemptRow;
        attemptRow.id = "attempt-1:" + handoff.id;
        attemptRow.handoffId = handoff.id;
        attemptRow.verificationAnchorRunId = seeded.runId;
        attemptRow.attemptRunId = attemptRunId;
        attemptRow.attemptOrdinal = 1;
        attemptRow.editedSnapshotFingerprint = ai::application::sha256Hex("edited-e2e");
        attemptRow.createdAt = std::chrono::system_clock::now();
        assert(handoffs.insertAttempt(attemptRow).success());

        ai::infrastructure::SqliteExecutionManifestRepository manifests(database.value());
        ai::application::ExecutionManifest manifest;
        manifest.runId = attemptRunId;
        assert(manifests.insert(std::move(manifest)).success());

        ai::application::ManifestProposalSetVerification verification;
        verification.workspaceIdentity = projectId.toStdString();
        verification.baselineFingerprint = handoff.baselineFingerprint;
        verification.overlayFingerprint = "review-apply-e2e-overlay";
        verification.orderedProposalIds = {seeded.changeSetId};
        verification.orderedProposalContentHashes = {realHash};
        verification.proposalSafetyStatus = ai::application::ExecutionVerificationStatus::Passed;
        verification.buildLintTestStatus = ai::application::ExecutionVerificationStatus::Passed;
        verification.acceptanceStatus = ai::application::ExecutionVerificationStatus::Passed;

        ai::application::ManifestWorkLedgerEntry entry;
        entry.runId = attemptRunId;
        entry.projectId = projectId.toStdString();
        entry.terminalStatus = ai::application::RunStatus::Completed;
        entry.baselineFingerprint = handoff.baselineFingerprint;
        entry.disposition = ai::application::ManifestWorkLedgerDisposition::ManifestReady;
        entry.proposalIds = {seeded.changeSetId};
        ai::application::ManifestWorkLedgerTaskDisposition taskDisposition;
        taskDisposition.taskId = "task-1";
        taskDisposition.status = "completed";
        taskDisposition.changeSetId = seeded.changeSetId;
        entry.taskDispositions = {taskDisposition};
        entry.verification = verification;
        entry.createdAt = std::chrono::system_clock::now();
        ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database.value());
        assert(ledger.insertEntry(entry).success());
    }

    // Selecting the ANCHOR Run (never the internal attempt Run, which stays
    // out of the top-level Runs projection - see RunController's own
    // manual-handoff filtering) is all that is needed - no
    // AgentSessionController/AgentToolSession call anywhere in this test.
    auto& controller = bridge.manualHandoffController();
    assert(controller.selectRun(QString::fromStdString(seeded.runId)));
    assert(controller.isApplicable() && controller.hasFinalVerification() && controller.canApply());

    const QVariantList members = controller.reviewMembers();
    assert(members.size() == 1);
    const QVariantMap member = members.first().toMap();
    const QString proposalId = member.value(QStringLiteral("proposalId")).toString();
    assert(proposalId == QString::fromStdString(seeded.changeSetId));
    assert(member.value(QStringLiteral("owningRunId")).toString()
        == QString::fromStdString(seeded.runId));
    assert(!member.value(QStringLiteral("alreadyApplied")).toBool());
    assert(member.value(QStringLiteral("applyAllowed")).toBool());
    assert(member.value(QStringLiteral("applyBlockCode")).toString()
        == QStringLiteral("manifest_apply.eligible"));
    assert(member.value(QStringLiteral("changes")).toList().size() == 1);

    // Requirement 4/6: an unknown/stale proposal id must fail closed
    // synchronously, never reach the Backend approve()/apply() pipeline.
    assert(!controller.selectMember(QStringLiteral("not-a-real-proposal-id")));
    assert(controller.errorText().startsWith(QStringLiteral("manual_handoff.review_member_not_found")));
    assert(!controller.approveAndApplyMember(QStringLiteral("not-a-real-proposal-id")));
    assert(!controller.rejectMember(QStringLiteral("not-a-real-proposal-id"), QString()));

    // Requirement 10 (Cancel leaves no mutation): selecting and merely
    // inspecting the diff never applies or rejects anything.
    assert(controller.selectMember(proposalId));
    assert(controller.selectedMemberProposalId() == proposalId);
    const QString changeId = controller.selectedMemberDetail()
        .value(QStringLiteral("changes")).toList().first().toMap()
        .value(QStringLiteral("changeId")).toString();
    assert(!changeId.isEmpty());
    assert(controller.readOriginalContent(proposalId, changeId).isEmpty());
    assert(controller.readProposedContent(proposalId, changeId) == QStringLiteral("reviewed content"));
    {
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteChangeProposalRepository proposalsRepo(database.value());
        const auto unchanged = proposalsRepo.findChangeSet(seeded.changeSetId);
        assert(unchanged.success() && unchanged.value().has_value()
            && unchanged.value()->state == ai::application::ChangeSetState::ReadyForReview
            && "Inspecting the diff must never mutate the proposal");
    }

    // Force only the best-effort post-Apply artifact write to fail. The
    // workspace mutation must remain successful and the controller must
    // surface ApplyChangeSetResult::provenanceErrorCode rather than silently
    // discarding it or pretending the file write rolled back.
    const fs::path artifactRoot = workspace.databasePath.parent_path() / "artifacts";
    const fs::path artifactBackup = workspace.databasePath.parent_path() / "artifacts-before-warning";
    assert(fs::is_directory(artifactRoot));
    std::error_code artifactMoveError;
    fs::rename(artifactRoot, artifactBackup, artifactMoveError);
    assert(!artifactMoveError);
    {
        std::ofstream blocker(artifactRoot, std::ios::binary);
        assert(blocker.is_open());
        blocker << "deliberately not an artifact directory";
    }

    // The real Approve-and-apply pipeline: ProposalReviewService::
    // assertNotRejected() -> ChangeSetApplyService::approve()/apply() ->
    // the real workspace file is written -> refresh() re-reads durable state.
    assert(!std::filesystem::exists(workspace.projectPath / "reviewed.txt"));
    assert(controller.approveAndApplyMember(proposalId));
    assert(controller.errorText().isEmpty());
    assert(controller.applyResultText().startsWith(
        QStringLiteral("Applied 1 file(s) to workspace")));
    assert(controller.applyResultText().contains(
        QStringLiteral("Warning: post-apply provenance was not recorded"))
        && "A successful Apply's provenance warning must remain visible");
    assert(std::filesystem::exists(workspace.projectPath / "reviewed.txt"));
    {
        std::ifstream written(workspace.projectPath / "reviewed.txt", std::ios::binary);
        std::ostringstream buffer;
        buffer << written.rdbuf();
        assert(buffer.str() == "reviewed content");
    }

    // AG-16 rechecked, not just rendered: the read model re-reads the durable
    // Applied state, never trusting the pre-apply cached projection.
    const QVariantList membersAfterApply = controller.reviewMembers();
    assert(membersAfterApply.size() == 1);
    const QVariantMap memberAfterApply = membersAfterApply.first().toMap();
    assert(memberAfterApply.value(QStringLiteral("alreadyApplied")).toBool());
    assert(memberAfterApply.value(QStringLiteral("state")).toString() == QStringLiteral("applied"));
    assert(!memberAfterApply.value(QStringLiteral("applyAllowed")).toBool()
        && "An already-Applied member must never be re-offered for Apply");
    // canApply now reflects that no member remains eligible - never a stale
    // true left over from before the Apply.
    assert(!controller.canApply());

    // A second Approve-and-apply on the same already-Applied member must
    // fail closed through the real Backend pipeline (ChangeSetApplyService::
    // apply() itself refuses a non-ReadyForReview state) rather than
    // silently no-op or re-write the file.
    assert(!controller.approveAndApplyMember(proposalId));
    assert(!controller.errorText().isEmpty());
}

// Provider-free reset hardening: the two cheapest guards -
// resetNonEditableChanges() must refuse synchronously (no busy transition,
// no filesystem access) when no Run is selected at all, and again when a
// selected Run genuinely has no manual handoff yet.
void testManualHandoffResetGuardsNoRunAndNoHandoff()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());
    auto& controller = bridge.manualHandoffController();

    assert(!controller.resetNonEditableChanges());
    assert(!controller.busy());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Manual handoff reset guards"),
        QStringLiteral("Proves resetNonEditableChanges() refuses before any filesystem work"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const auto seeded = seedManifestRunWithProposal(
        workspace, projectId.toStdString(), "qml/App.qml", "Item {}");
    {
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteRunRepository runs(database.value());
        const auto run = runs.findRun(seeded.runId);
        assert(run.success() && run.value().has_value());
        auto failed = *run.value();
        failed.status = ai::application::RunStatus::Failed;
        assert(runs.updateRun(failed).success());
    }

    assert(controller.selectRun(QString::fromStdString(seeded.runId)));
    assert(controller.isApplicable() && !controller.hasHandoff());
    assert(!controller.resetNonEditableChanges());
    assert(controller.errorText().startsWith(QStringLiteral("manual_handoff.not_found")));
    assert(!controller.busy());
}

// Provider-free reset hardening real end-to-end proof: a genuine two-file
// materialized candidate (built the same way materialize() itself would -
// a real ManifestProposalOverlay over a real multi-change proposal, its
// exact snapshot captured and copied into the real candidate storage root)
// where the user tampers the non-editable file. Proves the "empty reset
// set" guard on the untouched candidate, that resetNonEditableChanges()
// shares the busy/command-serialization boundary with reverify(), and that
// a successful async reset genuinely restores the forbidden edit on disk,
// preserves the authorized edit, updates resetResultText, and leaves
// resetRequiredRelativePaths empty after the controller's own authoritative
// refresh() - never the worker's own in-memory result.
void testManualHandoffResetNonEditableChangesRealCandidateEndToEnd()
{
    TempWorkspace workspace;
    fs::create_directories(workspace.projectPath / "qml");
    {
        std::ofstream a(workspace.projectPath / "qml" / "A.qml", std::ios::binary | std::ios::trunc);
        a << "original-a";
    }
    {
        std::ofstream b(workspace.projectPath / "qml" / "B.qml", std::ios::binary | std::ios::trunc);
        b << "original-b";
    }

    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());
    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Manual handoff reset real candidate"),
        QStringLiteral("Proves resetNonEditableChanges() end to end through the real controller"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    const std::string runId = "manual-handoff-reset-anchor";
    ai::application::ManifestVerificationManualHandoff handoff;
    handoff.storageKey = "reset-e2e-candidate";
    ai::application::ManifestProposalOverlayDescriptor overlay;
    std::string overlayFingerprint;
    std::string contentHash;
    ai::application::ChangeSetId proposalId;
    {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());

        ai::application::Run run;
        run.id = runId;
        run.projectId = projectId.toStdString();
        run.title = "Stage 3 failure";
        run.task = "Repair qml lint";
        run.workflowId = ai::application::kExecutionManifestWorkflowId;
        run.workspacePath = workspace.projectPath;
        run.status = ai::application::RunStatus::Failed;
        run.createdAt = std::chrono::system_clock::now();
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteRunRepository runs(database.value());
        assert(runs.insertRun(run).success());

        ai::application::CreateChangeProposalRequest request;
        request.runId = runId;
        request.projectId = projectId.toStdString();
        request.providerId = "test-provider";
        request.modelId = "test-model";
        request.agentRole = "workspace-access";
        request.baseRevision = "main";
        request.policy.projectRoot = workspace.projectPath;
        request.requireHashEvidence = true;
        request.observedFileHashes = {
            {"qml/a.qml", ai::application::sha256Hex("original-a")},
            {"qml/b.qml", ai::application::sha256Hex("original-b")}};
        request.rawModelOutput = R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Fix qml lint","changes":[)"
            R"({"change_id":"fix-a","operation":"modify","relative_path":"qml/A.qml",)"
            R"("source_path":null,"proposed_content":"fixed-a"},)"
            R"({"change_id":"fix-b","operation":"modify","relative_path":"qml/B.qml",)"
            R"("source_path":null,"proposed_content":"fixed-b"}]})";
        const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
        assert(created.success()
            && created.value().state == ai::application::ChangeSetState::ReadyForReview);
        proposalId = created.value().changeSetId;

        const auto policy = ai::application::WorkspaceReadPolicy::secureDefaults(
            workspace.projectPath);
        const auto baselineSnapshot = ai::application::WorkspaceBaselineProvenance::capture(policy);
        assert(baselineSnapshot.success());
        handoff.baselineFingerprint = baselineSnapshot.value().fingerprint;

        ai::application::BuildManifestProposalOverlayRequest overlayRequest;
        overlayRequest.workspaceRoot = workspace.projectPath;
        overlayRequest.trustedScratchRoot = workspace.root / "scratch";
        overlayRequest.workspaceIdentity = projectId.toStdString();
        overlayRequest.baselinePolicy = policy;
        overlayRequest.baselineSnapshot = baselineSnapshot.value();
        overlayRequest.orderedProposalIds = {proposalId};
        const auto built = ai::application::ManifestProposalOverlay::build(
            overlayRequest, seed.changeProposalService());
        assert(built.success());
        overlay = built.value();
        overlayFingerprint = overlay.overlayFingerprint;
        assert(overlay.orderedProposalContentHashes.size() == 1);
        contentHash = overlay.orderedProposalContentHashes.front();

        const auto overlaySnapshot = ai::application::WorkspaceBaselineProvenance::capture(
            ai::application::WorkspaceReadPolicy::secureDefaults(overlay.root));
        assert(overlaySnapshot.success());
        handoff.originalSnapshot = overlaySnapshot.value();

        // Copy the real, freshly-built overlay tree into the real candidate
        // storage root (databasePath.parent_path() / "manual-handoff" /
        // storageKey) - exactly the durable location materialize() itself
        // would have used, so this test exercises the SAME resolveCandidateRoot()
        // containment logic resetNonEditableChanges() genuinely relies on.
        const fs::path candidateRoot =
            workspace.databasePath.parent_path() / "manual-handoff" / handoff.storageKey;
        std::error_code copyError;
        fs::create_directories(candidateRoot.parent_path(), copyError);
        assert(!copyError);
        fs::copy(overlay.root, candidateRoot, fs::copy_options::recursive, copyError);
        assert(!copyError);
        assert(ai::application::ManifestProposalOverlay::cleanup(overlay).success());
    }

    handoff.id = "manual-handoff:" + runId;
    handoff.originManifestRunId = runId;
    handoff.verificationAnchorRunId = runId;
    handoff.sourceProjectId = projectId.toStdString();
    handoff.workspaceIdentity = projectId.toStdString();
    handoff.sourceOverlayFingerprint = overlayFingerprint;
    handoff.frozenMembers = {{proposalId, runId, contentHash, proposalId}};
    handoff.allowedPaths = {{"qml/A.qml", proposalId, "component"}};
    handoff.createdAt = std::chrono::system_clock::now();
    {
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteManifestVerificationManualHandoffRepository handoffs(
            database.value());
        assert(handoffs.insertHandoff(handoff).success());
    }

    auto& controller = bridge.manualHandoffController();
    assert(controller.selectRun(QString::fromStdString(runId)));
    assert(controller.isApplicable() && controller.hasHandoff());
    assert(controller.resetRequiredRelativePaths().isEmpty()
        && "A freshly materialized, untouched candidate must have nothing to reset");
    assert(controller.editableChangedRelativePaths().isEmpty());

    // Guard: nothing to reset yet - refused synchronously, never busy.
    assert(!controller.resetNonEditableChanges());
    assert(controller.errorText().startsWith(
        QStringLiteral("manual_handoff.no_non_editable_changes")));
    assert(!controller.busy());

    // Guard: resetNonEditableChanges() shares the same busy/command-
    // serialization boundary reverify() already uses. The untouched
    // candidate makes reverify() itself fail fast with
    // manual_handoff.no_changes_to_reverify once its worker runs, but the
    // synchronous refusal below only needs busy_ to be true right now.
    assert(controller.reverify());
    assert(controller.busy());
    assert(!controller.resetNonEditableChanges());
    assert(ai::ui::test_support::waitForCondition("Reverify-on-untouched-candidate completion",
        [&] { return !controller.busy(); },
        [&] { return "busy=" + std::to_string(controller.busy()); }, 4s));

    // Now the user tampers the non-editable file and also authors the one
    // authorized edit.
    {
        std::ofstream b(workspace.databasePath.parent_path() / "manual-handoff"
                / handoff.storageKey / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        assert(b.is_open());
        b << "tampered-b";
    }
    {
        std::ofstream a(workspace.databasePath.parent_path() / "manual-handoff"
                / handoff.storageKey / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        assert(a.is_open());
        a << "fixed-a-by-user";
    }
    assert(controller.refresh());
    assert(controller.resetRequiredRelativePaths() == QStringList {QStringLiteral("qml/B.qml")});
    assert(controller.editableChangedRelativePaths() == QStringList {QStringLiteral("qml/A.qml")});
    assert(controller.resetResultText().isEmpty());

    QString finishedRunId;
    bool finishedSucceeded = false;
    QObject::connect(&controller,
        &ManifestVerificationManualHandoffController::commandFinished,
        [&](const QString& commandRunId, bool succeeded) {
            finishedRunId = commandRunId;
            finishedSucceeded = succeeded;
        });

    assert(controller.resetNonEditableChanges());
    assert(controller.busy());
    assert(ai::ui::test_support::waitForCondition("Manual-handoff reset completion",
        [&] { return !controller.busy(); },
        [&] { return "busy=" + std::to_string(controller.busy())
            + ", error=" + controller.errorText().toStdString(); }, 4s));

    assert(finishedRunId == QString::fromStdString(runId) && finishedSucceeded);
    assert(controller.errorText().isEmpty());
    assert(!controller.resetResultText().isEmpty());
    assert(controller.resetResultText().contains(QStringLiteral("1")));
    assert(controller.resetRequiredRelativePaths().isEmpty());
    assert(controller.editableChangedRelativePaths() == QStringList {QStringLiteral("qml/A.qml")});

    {
        std::ifstream restoredB(workspace.databasePath.parent_path() / "manual-handoff"
                / handoff.storageKey / "qml" / "B.qml",
            std::ios::binary);
        assert(restoredB.is_open());
        std::ostringstream buffer;
        buffer << restoredB.rdbuf();
        assert(buffer.str() == "fixed-b"
            && "The forbidden edit must be restored to the frozen candidate's own bytes");
    }
    {
        std::ifstream preservedA(workspace.databasePath.parent_path() / "manual-handoff"
                / handoff.storageKey / "qml" / "A.qml",
            std::ios::binary);
        assert(preservedA.is_open());
        std::ostringstream buffer;
        buffer << preservedA.rdbuf();
        assert(buffer.str() == "fixed-a-by-user"
            && "The authorized edit must survive reset byte-for-byte");
    }

    // Live project must never be touched by reset.
    {
        std::ifstream liveA(workspace.projectPath / "qml" / "A.qml", std::ios::binary);
        std::ostringstream buffer;
        buffer << liveA.rdbuf();
        assert(buffer.str() == "original-a");
    }
}

// Simulates a hard crash: a run is written directly to the database as
// "Running" through a raw SqliteRunRepository, bypassing RunManager entirely
// (a clean BackendBridge shutdown would already turn an active run into
// Interrupted itself, which would not exercise OrchestrationService::
// recoverInterruptedRuns() - the path a real crash actually leaves behind).
// The next BackendBridge constructed on that database must recover it to
// Interrupted before any Qt model is first read.
void testRecoveryOfPersistedUnfinishedRun()
{
    TempWorkspace workspace;
    QString projectId;
    const ai::application::RunId unfinishedRunId = "qt-adapter-recovery-run";

    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        projectId = bridge.projectController().createProject(
            QStringLiteral("Recovery test project"),
            QStringLiteral("Holds a run left Running by a simulated crash"),
            workspace.projectPathQt());
        assert(!projectId.isEmpty());
    }

    {
        // A raw repository connection stands in for "a prior process that
        // crashed mid-run": it writes a Running row and is closed without
        // ever going through RunManager's own clean-shutdown-to-Interrupted path.
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(opened.success());
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());

        ai::application::Run unfinished;
        unfinished.id = unfinishedRunId;
        unfinished.projectId = projectId.toStdString();
        unfinished.title = "Interrupted before this process could finish it";
        unfinished.task = "Recovered by the next BackendBridge";
        unfinished.workflowId = "mock-sequential-v1";
        unfinished.status = ai::application::RunStatus::Running;
        unfinished.progress = 0.5;
        unfinished.createdAt = std::chrono::system_clock::now();
        unfinished.startedAt = unfinished.createdAt;
        assert(rawRuns.insertRun(unfinished).success());
    }

    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());

        QAbstractItemModel* runModel = bridge.runController().model();
        assert(waitForRowCount(runModel, 1));
        const QModelIndex runIndex = runModel->index(0, 0);
        assert(runModel->data(runIndex, RunListModel::RunIdRole).toString()
            == QString::fromStdString(unfinishedRunId));
        assert(runModel->data(runIndex, RunListModel::StatusRole).toString()
            == QStringLiteral("interrupted"));
        assert(!runModel->data(runIndex, RunListModel::FinishedAtRole).isNull());
    }
}

// Manual-handoff attempts remain durable Runs for crash recovery and typed
// retry/replay, but they are subordinate audit rows rendered inside their
// anchor's ManualHandoffPanel. They must never appear as empty duplicate
// top-level rows in the shared Runs model.
void testManualHandoffAttemptRunIsHiddenFromTopLevelRunModel()
{
    TempWorkspace workspace;
    QString projectId;
    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        projectId = bridge.projectController().createProject(
            QStringLiteral("Internal attempt visibility"),
            QStringLiteral("Keep manual verification attempts in handoff history"),
            workspace.projectPathQt());
        assert(!projectId.isEmpty());
    }

    const ai::application::RunId anchorRunId = "visible-manifest-anchor";
    const ai::application::RunId attemptRunId = "internal-manual-handoff-attempt";
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(opened.success());
        ai::infrastructure::SqliteRunRepository runs(opened.value());
        const auto now = std::chrono::system_clock::now();

        ai::application::Run anchor;
        anchor.id = anchorRunId;
        anchor.projectId = projectId.toStdString();
        anchor.title = "Visible failed Manifest Run";
        anchor.task = "Own the user-visible orchestration history";
        anchor.workflowId = ai::application::kExecutionManifestWorkflowId;
        anchor.status = ai::application::RunStatus::Failed;
        anchor.createdAt = now;
        anchor.finishedAt = now;
        assert(runs.insertRun(anchor).success());

        ai::application::Run attempt = anchor;
        attempt.id = attemptRunId;
        attempt.parentRunId = anchorRunId;
        attempt.title += " - manual handoff reverify";
        attempt.workflowId =
            std::string(ai::application::kManifestVerificationManualHandoffWorkflowId);
        attempt.errorMessage = "Manual handoff verification did not pass";
        assert(runs.insertRun(attempt).success());
    }

    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());
    QAbstractItemModel* model = bridge.runController().model();
    assert(waitForRowCount(model, 1));
    assert(model->data(model->index(0, 0), RunListModel::RunIdRole).toString()
        == QString::fromStdString(anchorRunId));
}

void testTaskLifecycleAndPersistence()
{
    TempWorkspace workspace;
    QString projectId;
    QString taskId;
    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        projectId = bridge.projectController().createProject(
            QStringLiteral("Task persistence"), QStringLiteral("Shared context"),
            workspace.projectPathQt());
        assert(!projectId.isEmpty());
        TaskController& tasks = bridge.taskController();
        assert(bridge.tasks() == &tasks);
        taskId = tasks.createTask(projectId, QStringLiteral("Review orchestration"),
            QStringLiteral("Check the persisted task contract"), QStringLiteral("blocked"),
            QStringLiteral("high"), true, QDateTime::currentDateTimeUtc().addDays(1),
            QDateTime {});
        assert(!taskId.isEmpty());
        assert(tasks.model()->rowCount() == 1);
        const QModelIndex index = tasks.model()->index(0, 0);
        assert(tasks.model()->data(index, TaskListModel::TaskIdRole).toString() == taskId);
        assert(tasks.model()->data(index, TaskListModel::RequiresAttentionRole).toBool());
        assert(tasks.updateTask(taskId, QStringLiteral("Review orchestration contract"),
            QStringLiteral("Updated context"), QStringLiteral("in_progress"),
            QStringLiteral("critical"), false, QDateTime {}, QDateTime {}));
    }
    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        QAbstractItemModel* model = bridge.taskController().model();
        assert(model->rowCount() == 1);
        const QModelIndex index = model->index(0, 0);
        assert(model->data(index, TaskListModel::TaskIdRole).toString() == taskId);
        assert(model->data(index, TaskListModel::TitleRole).toString()
            == QStringLiteral("Review orchestration contract"));
        assert(model->data(index, TaskListModel::StatusRole).toString()
            == QStringLiteral("in_progress"));
        assert(!model->data(index, TaskListModel::RequiresAttentionRole).toBool());
        assert(bridge.taskController().cancelTask(taskId));
    }
}

// PC-2: the Run's frozen interface contracts and their fingerprint are two more roles. Contracts are immutable
// after enqueue, so a status update republishes neither, and a Run without contracts has an empty list and an
// empty fingerprint (never a made-up hash).
void testRunListModelExposesTheFrozenInterfaceContracts()
{
    ai::application::Run run;
    run.id = "run-with-contracts";
    run.projectId = "project-contracts";
    run.title = "Contracts";
    run.task = "Task";
    run.workflowId = "execution-manifest-v1";
    run.status = ai::application::RunStatus::Running;
    run.createdAt = std::chrono::system_clock::now();
    ai::application::RunInterfaceContract contract;
    contract.id = "C1";
    contract.title = "Shape";
    contract.text = "property string metricId";
    contract.appliesToPaths = {"qml/models"};
    ai::application::Run plain = run;
    plain.id = "plain-run";
    run.interfaceContracts = {contract};

    RunListModel model;
    assert(model.roleNames().value(RunListModel::InterfaceContractsRole)
               == QByteArrayLiteral("interfaceContracts")
           && model.roleNames().value(RunListModel::InterfaceContractsFingerprintRole)
               == QByteArrayLiteral("interfaceContractsFingerprint"));
    model.replace({run, plain}, {});
    const auto listAt = [&](int row) {
        return model.data(model.index(row, 0), RunListModel::InterfaceContractsRole).toList();
    };
    const auto fingerprintAt = [&](int row) {
        return model.data(model.index(row, 0), RunListModel::InterfaceContractsFingerprintRole).toString();
    };
    assert(listAt(0).size() == 1
           && listAt(0).front().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("C1")
           && listAt(0).front().toMap().value(QStringLiteral("appliesToPaths")).toList().size() == 1);
    assert(fingerprintAt(0).size() == 64
           && fingerprintAt(0).toStdString()
               == ai::application::runInterfaceContractsFingerprint(run.interfaceContracts));
    assert(listAt(1).isEmpty() && fingerprintAt(1).isEmpty());

    QList<int> publishedRoles;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) { publishedRoles = roles; });
    auto finished = run;
    finished.status = ai::application::RunStatus::Completed;
    model.updateOne(finished);
    assert(!publishedRoles.isEmpty()
           && !publishedRoles.contains(RunListModel::InterfaceContractsRole)
           && !publishedRoles.contains(RunListModel::InterfaceContractsFingerprintRole));
}

// The scopes of a frozen contract go to the editor as a list, the editor shows them as the text "a, b" and sends
// that text back on Edit & rerun. The Backend and the converter must agree so that nothing changes on the way:
// the Backend refuses a scope the text form could not carry (',' ';' or a leading space), and the converter trims
// only the spaces around a piece.
void testRunContractScopesSurviveTheEditorTextRoundTrip()
{
    using ai::application::RunInterfaceContract;
    RunInterfaceContract contract;
    contract.id = "C1";
    contract.title = "Shape";
    contract.text = "property string metricId";
    contract.appliesToPaths = {"my dir/sub dir", "qml/models", "a b", "Project Files/x y.qml"};
    const auto frozen = ai::application::normalizeRunInterfaceContracts({contract});
    assert(frozen.success());

    // What InterfaceContractsEditor.apply() shows and payload() returns.
    QVariantList toEditor = ai::ui::runContractsToVariant(frozen.value());
    QVariantMap entry = toEditor.front().toMap();
    QStringList shown;
    for (const QVariant& scope : entry.value(QStringLiteral("appliesToPaths")).toList()) {
        shown.push_back(scope.toString());
    }
    entry.insert(QStringLiteral("appliesToPaths"), shown.join(QStringLiteral(", ")));
    const auto back = ai::ui::runContractsFromVariant(QVariantList {entry});
    assert(back.success() && back.value().size() == 1);
    assert(back.value().front().appliesToPaths == frozen.value().front().appliesToPaths);
    assert(ai::application::runInterfaceContractsFingerprint(
               ai::application::normalizeRunInterfaceContracts(back.value()).value())
           == ai::application::runInterfaceContractsFingerprint(frozen.value()));

    // A separator or a leading space is refused by the Backend instead of being split or trimmed on the way back.
    for (const char* bad : {"data/a,b", "data/a;b", " data"}) {
        RunInterfaceContract refused = contract;
        refused.appliesToPaths = {bad};
        const auto result = ai::application::normalizeRunInterfaceContracts({refused});
        assert(!result.success() && result.error()->code == "run_contracts.invalid_scope");
    }

    // Only spaces and tabs are trimmed around a piece (a Unicode space is part of the name); blank pieces vanish.
    const auto split = ai::ui::runContractsFromVariant(QVariantList {QVariantMap {
        {QStringLiteral("id"), QStringLiteral("C2")},
        {QStringLiteral("appliesToPaths"), QString::fromUtf8(" a/b ,\t c/d ;;\n e f \n,\xC2\xA0g/h\xC2\xA0 ")}}});
    assert(split.success() && split.value().size() == 1);
    assert(split.value().front().appliesToPaths
           == (std::vector<std::string> {"a/b", "c/d", "e f", "\xC2\xA0g/h\xC2\xA0"}));
}

// A kind the Backend does not know is an error at the Qt boundary, never the default kind.
void testRunContractUnknownKindIsRefusedAtTheQtBoundary()
{
    const auto entry = [](const char* kind) {
        QVariantMap map {{QStringLiteral("id"), QStringLiteral("C1")},
            {QStringLiteral("title"), QStringLiteral("t")}, {QStringLiteral("text"), QStringLiteral("x")}};
        if (kind != nullptr) map.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
        return QVariantList {map};
    };
    const auto absent = ai::ui::runContractsFromVariant(entry(nullptr));
    assert(absent.success()
           && absent.value().front().kind == ai::application::RunContractKind::InterfaceContract);
    assert(ai::ui::runContractsFromVariant(entry("")).success());
    const auto constraint = ai::ui::runContractsFromVariant(entry("constraint"));
    assert(constraint.success()
           && constraint.value().front().kind == ai::application::RunContractKind::Constraint);
    for (const char* bad : {"Constraint", "interface-contract", "constaint", " constraint", "5"}) {
        const auto refused = ai::ui::runContractsFromVariant(entry(bad));
        assert(!refused.success() && refused.error()->code == "run_contracts.invalid_kind"
               && refused.error()->message.find("C1") != std::string::npos);
    }
    // Position matters: one bad entry in the middle refuses the whole list.
    QVariantList list = entry("constraint");
    list.append(entry("nope").front());
    assert(!ai::ui::runContractsFromVariant(list).success());
}

// AG-17.7C-3: the history marker is one more parallel projection. It starts empty for every Run,
// is published ALONE (no other role) when only it changes, is never applied to the wrong row when
// the projection does not line up with the Runs, and a shape change carries it along.
void testRunListModelPublishesTheConversionMarkerAndRefusesAMisalignedProjection()
{
    const auto makeRun = [](const char* id) {
        ai::application::Run run;
        run.id = id;
        run.projectId = "project-marker-test";
        run.title = id;
        run.task = "Marker task";
        run.workflowId = "execution-manifest-v1";
        run.status = ai::application::RunStatus::Completed;
        run.createdAt = std::chrono::system_clock::now();
        return run;
    };
    const auto first = makeRun("run-a");
    const auto second = makeRun("run-b");
    const auto markerAt = [](const RunListModel& model, int row) {
        return model.data(model.index(row, 0), RunListModel::LegacyConversionRole).toString();
    };

    RunListModel model;
    assert(model.roleNames().value(RunListModel::LegacyConversionRole) == QByteArrayLiteral("legacyConversion"));
    model.replace({first, second}, {});
    assert(markerAt(model, 0).isEmpty() && markerAt(model, 1).isEmpty()
        && "A Run with no projection has no marker");

    int changeCount = 0;
    int resetCount = 0;
    QList<int> publishedRoles;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            ++changeCount;
            publishedRoles = roles;
        });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resetCount; });

    // Only the marker changes: it is published alone, on the right row, without a reset.
    model.replace({first, second}, {}, {"", "explicit_user"});
    assert(changeCount == 1 && resetCount == 0);
    assert((publishedRoles == QList<int> {RunListModel::LegacyConversionRole}));
    assert(markerAt(model, 0).isEmpty() && markerAt(model, 1) == QStringLiteral("explicit_user"));
    // The same projection again is a no-op (QML would otherwise rebuild its snapshots for nothing).
    model.replace({first, second}, {}, {"", "explicit_user"});
    assert(changeCount == 1);

    // A projection that does not line up with the Runs is dropped as a whole: no marker is better
    // than a marker on the wrong Run.
    model.replace({first, second}, {}, {"not_required_safe_retry"});
    assert(markerAt(model, 0).isEmpty() && markerAt(model, 1).isEmpty());
    assert(changeCount == 2 && (publishedRoles == QList<int> {RunListModel::LegacyConversionRole}));

    // A shape change is a reset, and it carries the markers of the new rows.
    model.replace({second}, {}, {"unreadable"});
    assert(resetCount == 1 && model.rowCount() == 1);
    assert(markerAt(model, 0) == QStringLiteral("unreadable"));
}

// The Backend can raise an automatic turn limit while a Run is live. The frozen policy keeps the
// original limit; the model shows the limit the Run is actually held to, says what it was raised
// from, publishes exactly those two roles when the raise happens, and ignores a recorded raise on
// a pinned policy (it can never have been applied).
void testRunListModelShowsAnAutomaticallyRaisedTurnLimit()
{
    ai::application::Run run;
    run.id = "run-turn-limit";
    run.projectId = "project-turn-limit";
    run.title = "Turn limit";
    run.task = "Turn limit task";
    run.workflowId = "execution-manifest-v1";
    run.status = ai::application::RunStatus::Running;
    run.createdAt = std::chrono::system_clock::now();
    run.providerBudget.autoExtendTurns = true;
    run.providerBudgetUsage.turnsStarted = 3;

    const auto maxTurns = [](const RunListModel& model) {
        return model.data(model.index(0, 0), RunListModel::ProviderBudgetMaxTurnsRole).toULongLong();
    };
    const auto raisedFrom = [](const RunListModel& model) {
        return model.data(model.index(0, 0), RunListModel::ProviderBudgetTurnsRaisedFromRole).toULongLong();
    };

    RunListModel model;
    assert(model.roleNames().value(RunListModel::ProviderBudgetTurnsRaisedFromRole)
        == QByteArrayLiteral("providerBudgetTurnsRaisedFrom"));
    model.replace({run});
    assert(maxTurns(model) == 24 && raisedFrom(model) == 0);

    QList<int> publishedRoles;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            publishedRoles = roles;
        });
    run.providerBudgetUsage.grantedMaxTurns = 25;
    model.updateOne(run);
    assert(maxTurns(model) == 25 && raisedFrom(model) == 24);
    assert(publishedRoles.contains(RunListModel::ProviderBudgetMaxTurnsRole)
        && publishedRoles.contains(RunListModel::ProviderBudgetTurnsRaisedFromRole));

    // A recorded raise on a pinned policy is not shown as if it had been applied.
    run.providerBudget.autoExtendTurns = false;
    model.updateOne(run);
    assert(maxTurns(model) == 24 && raisedFrom(model) == 0);
}

// Run Details shows the durable Run warnings: the artifact root is unusable (a plain file sits where the store's directory would
// be), so the Mock Run completes without any artifact and the only trace is its warnings - read from the database, selected
// run only, and the same after a restart. The projection exposes names and messages, never a path.
void testRunWarningsReachTheRunDetailsModelAndSurviveARestart()
{
    TempWorkspace workspace;
    fs::create_directories(workspace.databasePath.parent_path());
    {
        std::ofstream blocker(workspace.databasePath.parent_path() / "artifacts");
        assert(blocker.good());
    }

    QString runId;
    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        const QString projectId = bridge.projectController().createProject(
            QStringLiteral("Warning project"), QStringLiteral("Lose every artifact"), workspace.projectPathQt());
        assert(!projectId.isEmpty());

        RunController& runs = bridge.runController();
        runId = runs.startRun(projectId, QStringLiteral("Lossy run"), QStringLiteral("Lose every artifact"));
        assert(!runId.isEmpty());
        // The Run did its work: the lost artifacts do not make it Failed.
        assert(waitForStatus(runs, runId, {QStringLiteral("completed")}) == QStringLiteral("completed"));
        assert(waitForRowCount(runs.warningModel(), 2));
        assert(runs.artifactModel()->rowCount() == 0);

        QStringList artifactNames;
        for (int row = 0; row < runs.warningModel()->rowCount(); ++row) {
            const QModelIndex index = runs.warningModel()->index(row, 0);
            assert(runs.warningModel()->data(index, RunWarningListModel::RunIdRole).toString() == runId);
            assert(runs.warningModel()->data(index, RunWarningListModel::CodeRole).toString()
                == QStringLiteral("run_warning.diagnostic_artifact_lost"));
            const QString name = runs.warningModel()->data(index, RunWarningListModel::ArtifactNameRole).toString();
            artifactNames.push_back(name);
            assert(runs.warningModel()->data(index, RunWarningListModel::MessageRole).toString().contains(name));
            assert(runs.warningModel()->data(index, RunWarningListModel::CreatedAtRole).toDateTime().isValid());
        }
        artifactNames.sort();
        assert(artifactNames == (QStringList {QStringLiteral("context-snapshot.md"), QStringLiteral("mock-run-report.md")}));
        assert(!runs.warningModel()->roleNames().values().contains("relativePath"));
        assert(!runs.warningModel()->roleNames().values().contains("path"));

        // Another selection never shows this Run's warnings.
        runs.selectRun(QStringLiteral("some-other-run"));
        assert(waitForRowCount(runs.warningModel(), 0));
    }
    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        RunController& runs = bridge.runController();
        runs.selectRun(runId);
        assert(waitForRowCount(runs.warningModel(), 2));
        assert(runs.warningModel()->data(runs.warningModel()->index(0, 0), RunWarningListModel::RunIdRole).toString() == runId);
    }
}

// The folder of the temporary verification copies is validated by the Backend, not by the page: a refused folder is reported in the
// Backend's own words and leaves the previous setting; an accepted one is created and survives a restart; empty goes back to the
// default.
void testVerificationFolderSettingIsValidatedByTheBackendAndSurvivesARestart()
{
    TempWorkspace workspace;
    const fs::path chosen = workspace.root / "vf";
    const QString chosenText = QString::fromStdWString(chosen.wstring());
    {
        BackendBridge bridge(workspace.databasePathQt());
        assert(bridge.storageAvailable());
        SettingsController& settings = bridge.settingsController();
        assert(settings.verificationScratchRoot().isEmpty());
        assert(settings.verificationScratchSource() == QStringLiteral("default"));
        assert(settings.verificationScratchUsable() && settings.verificationScratchProblem().isEmpty());
        assert(!settings.verificationScratchEffective().isEmpty());
        assert(settings.verificationScratchMaxLength() == 64);

        assert(!settings.setVerificationScratchRoot(QStringLiteral("relative/folder")));
        assert(settings.lastError().contains(QStringLiteral("absolute path")));
        assert(settings.verificationScratchRoot().isEmpty() && "a refused folder is not stored");

        assert(settings.setVerificationScratchRoot(chosenText));
        assert(fs::is_directory(chosen) && "the Backend creates the folder");
        assert(settings.verificationScratchSource() == QStringLiteral("setting"));
        assert(settings.verificationScratchUsable());
        assert(QDir::fromNativeSeparators(settings.verificationScratchRoot()).compare(
                   QDir::fromNativeSeparators(chosenText), Qt::CaseInsensitive) == 0);

        // A refusal leaves the folder that is in force.
        assert(!settings.setVerificationScratchRoot(QStringLiteral("relative/folder")));
        assert(QDir::fromNativeSeparators(settings.verificationScratchRoot()).compare(
                   QDir::fromNativeSeparators(chosenText), Qt::CaseInsensitive) == 0);
    }
    {
        BackendBridge bridge(workspace.databasePathQt());
        SettingsController& settings = bridge.settingsController();
        assert(settings.verificationScratchSource() == QStringLiteral("setting")
            && "the folder must survive a restart");
        assert(settings.setVerificationScratchRoot(QString {}) && "empty goes back to the default");
        assert(settings.verificationScratchSource() == QStringLiteral("default")
            && settings.verificationScratchRoot().isEmpty());
    }
}

// A project folder whose name the ANSI code page of the machine cannot hold (Chinese on a Russian Windows, Cyrillic on an English one;
// the name has both, so the test does not depend on the machine): path::generic_string() throws std::system_error for it, and an
// exception out of a Qt model's data() ends the application. The page must show the name as it is.
void testProjectListModelShowsRootPathsTheAnsiCodePageCannotHold()
{
    ProjectListModel model;
    ai::application::Project project;
    project.id = "project-nonansi";
    project.name = "Non-ANSI root";
    project.rootPath = std::filesystem::path(L"C:/\u6e2c\u8a66/\u043f\u0440\u043e\u0435\u043a\u0442");
    project.status = ai::application::ProjectStatus::Active;
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    model.replace({project});
    const QString shown = model.data(model.index(0, 0), ProjectListModel::RootPathRole).toString();
    assert(shown == QString::fromWCharArray(L"C:/\u6e2c\u8a66/\u043f\u0440\u043e\u0435\u043a\u0442"));
}

void testRunWarningListModelProjectsRowsAndHidesAMissingStage()
{
    RunWarningListModel model;
    assert(model.rowCount() == 0);
    ai::application::RunWarning withStage;
    withStage.id = "w-1";
    withStage.runId = "run-1";
    withStage.stageId = "stage-1";
    withStage.code = "run_warning.diagnostic_artifact_lost";
    withStage.message = "Diagnostic artifact 'a.md' was not saved";
    withStage.artifactName = "a.md";
    withStage.createdAt = std::chrono::system_clock::now();
    ai::application::RunWarning withoutStage = withStage;
    withoutStage.id = "w-2";
    withoutStage.stageId.reset();
    withoutStage.artifactName = "b.md";
    model.replace({withStage, withoutStage});
    assert(model.rowCount() == 2);
    assert(model.data(model.index(0, 0), RunWarningListModel::WarningIdRole).toString() == QStringLiteral("w-1"));
    assert(model.data(model.index(0, 0), RunWarningListModel::StageIdRole).toString() == QStringLiteral("stage-1"));
    assert(!model.data(model.index(1, 0), RunWarningListModel::StageIdRole).isValid());
    assert(model.data(model.index(1, 0), RunWarningListModel::ArtifactNameRole).toString() == QStringLiteral("b.md"));
    assert(!model.data(model.index(2, 0), RunWarningListModel::MessageRole).isValid());
    model.replace({});
    assert(model.rowCount() == 0);
}

void testRunListModelPublishesOnlyChangedRoles()
{
    RunListModel model;
    ai::application::Run initial;
    initial.id = "run-role-test";
    initial.projectId = "project-role-test";
    initial.title = "Initial title";
    initial.task = "Initial task";
    initial.workflowId = "mock-sequential-v1";
    initial.status = ai::application::RunStatus::Queued;
    initial.createdAt = std::chrono::system_clock::now();
    model.replace({initial}, {"unresolved"});
    assert(model.data(model.index(0, 0), RunListModel::ManualHandoffResolutionRole)
        .toString() == QStringLiteral("unresolved"));

    int changeCount = 0;
    int resetCount = 0;
    QList<int> publishedRoles;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
        [&changeCount, &publishedRoles](
            const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            ++changeCount;
            publishedRoles = roles;
        });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model,
        [&resetCount] { ++resetCount; });

    auto progressUpdate = initial;
    progressUpdate.status = ai::application::RunStatus::Running;
    progressUpdate.progress = 0.5;
    progressUpdate.usage.inputTokens = 17;
    model.updateOne(progressUpdate);
    assert(changeCount == 1);
    assert(resetCount == 0);
    // Package 4: with zero cache activity, NonCacheInputTokensRole's
    // Backend-computed value is mathematically identical to InputTokensRole
    // (see computeUsageTokenBreakdown()), so it changes in lockstep here.
    assert((publishedRoles == QList<int> {
        RunListModel::StatusRole,
        RunListModel::ProgressRole,
        RunListModel::InputTokensRole,
        RunListModel::NonCacheInputTokensRole}));

    // An identical projection must be a no-op: otherwise QML would rebuild
    // JavaScript snapshots even though no visible state changed.
    model.updateOne(progressUpdate);
    assert(changeCount == 1);
    assert(resetCount == 0);

    auto sameShapeUpdate = progressUpdate;
    sameShapeUpdate.title = "Updated title";
    sameShapeUpdate.usage.outputTokens = 23;
    model.replace({sameShapeUpdate}, {"unresolved"});
    assert(changeCount == 2);
    assert(resetCount == 0);
    assert((publishedRoles == QList<int> {
        RunListModel::TitleRole,
        RunListModel::OutputTokensRole}));

    model.replace({sameShapeUpdate}, {"resolved_manually_applied"});
    assert(changeCount == 3);
    assert(resetCount == 0);
    assert((publishedRoles == QList<int> {RunListModel::ManualHandoffResolutionRole}));
    assert(model.data(model.index(0, 0), RunListModel::ManualHandoffResolutionRole)
        .toString() == QStringLiteral("resolved_manually_applied"));
}

// Package 4 (2026-09-28 planner-terminal-budget-hardening): the CSV
// reconciliation fixture proven at the pure computeUsageTokenBreakdown()
// level in CostCalculationTests.cpp, exercised end to end through the real
// Qt role RunDetailsPanel.qml actually reads - proves the Backend-owned
// breakdown reaches QML unmodified rather than being re-derived there.
void testRunListModelProjectsNonCacheInputTokensFromBackendBreakdown()
{
    RunListModel model;
    ai::application::Run run;
    run.id = "run-usage-breakdown-test";
    run.projectId = "project-usage-breakdown-test";
    run.title = "Usage breakdown";
    run.task = "Prove the non-cache projection";
    run.workflowId = "mock-sequential-v1";
    run.status = ai::application::RunStatus::Completed;
    run.createdAt = std::chrono::system_clock::now();
    run.usage.inputTokens = 852 + 284'235 + 3'518'953;
    run.usage.cacheCreationInputTokens = 284'235;
    run.usage.cacheReadInputTokens = 3'518'953;
    run.usage.outputTokens = 19'782;
    model.replace({run}, {"not_applicable"});

    const QModelIndex index = model.index(0, 0);
    assert(model.data(index, RunListModel::InputTokensRole).toULongLong()
        == 852ULL + 284'235ULL + 3'518'953ULL);
    assert(model.data(index, RunListModel::NonCacheInputTokensRole).toULongLong() == 852ULL);
    assert(model.data(index, RunListModel::CacheCreationInputTokensRole).toULongLong()
        == 284'235ULL);
    assert(model.data(index, RunListModel::CacheReadInputTokensRole).toULongLong()
        == 3'518'953ULL);
    assert(model.data(index, RunListModel::OutputTokensRole).toULongLong() == 19'782ULL);
    assert(model.roleNames().value(RunListModel::NonCacheInputTokensRole)
        == QByteArray("nonCacheInputTokens"));
}

QVariantMap runRowSnapshot(QAbstractItemModel* model, const QString& runId)
{
    QVariantMap row;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex index = model->index(r, 0);
        if (model->data(index, RunListModel::RunIdRole).toString() == runId) {
            row.insert(QStringLiteral("title"), model->data(index, RunListModel::TitleRole));
            row.insert(QStringLiteral("status"), model->data(index, RunListModel::StatusRole));
            row.insert(QStringLiteral("inputTokens"),
                model->data(index, RunListModel::InputTokensRole));
            row.insert(QStringLiteral("outputTokens"),
                model->data(index, RunListModel::OutputTokensRole));
            break;
        }
    }
    return row;
}

// Regression test for the findRun()-per-event swap in RunController's async
// refresh (RunController.cpp startPendingRefresh()/finishRefresh()): a
// RunEvent stream for one run must never touch another, already-settled
// run's row. Two runs complete sequentially, then only the second is
// retried (a brand-new run id, its own independent RunEvent stream) - the
// first run's row must stay byte-for-byte identical across that retry, not
// just "still present". Before this swap the per-event path re-fetched
// every run via listRuns() on each event, which would have made this test
// pass just as well - the point of this test is to keep it passing when a
// future change narrows what a single event is allowed to touch even
// further, not to detect the swap itself (that isn't observable from the Qt
// adapter's public surface without mocking OrchestrationService).
void testUnrelatedRunRowUnaffectedByAnotherRunsEvent()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("findRun swap test"),
        QStringLiteral("Verify per-event refresh does not disturb other rows"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());
    assert(bridge.workspaceContextController().selectProject(projectId));

    RunController& runs = bridge.runController();
    const QString runAId = runs.startRun(
        projectId, QStringLiteral("Run A"), QStringLiteral("First run"));
    assert(!runAId.isEmpty());
    assert(waitForStatus(runs, runAId, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    const QString runBId = runs.startRun(
        projectId, QStringLiteral("Run B"), QStringLiteral("Second run"));
    assert(!runBId.isEmpty());
    assert(waitForStatus(runs, runBId, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    const QVariantMap runABefore = runRowSnapshot(runs.model(), runAId);
    assert(!runABefore.isEmpty());

    const QString retriedBId = runs.retryRun(runBId);
    assert(!retriedBId.isEmpty());
    assert(waitForStatus(runs, retriedBId, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    const QVariantMap runAAfter = runRowSnapshot(runs.model(), runAId);
    assert(runAAfter == runABefore);
}

bool modelRowsBelongToRun(QAbstractItemModel* model, int runIdRole, const QString& runId)
{
    if (model->rowCount() == 0) return false;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->data(model->index(row, 0), runIdRole).toString() != runId)
            return false;
    }
    return true;
}

// AG-12 focused stress: a stale async detail refresh for Run A must never
// overwrite a newer Run B selection, and an immediate cancellation must not
// be lost while queued Run events and list refreshes cross the Qt thread
// boundary. The environment-controlled 100-iteration mode runs only this
// bounded scenario, keeping the normal CTest fast while retaining a cheap,
// reproducible release-gate command.
void testSelectionAndCancellationRace(int iterations)
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());
    const QString projectId = bridge.projectController().createProject(
        QStringLiteral("AG-12 race fixture"),
        QStringLiteral("Selection and cancellation stress"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    RunController& runs = bridge.runController();
    const QString runA = runs.startRun(
        projectId, QStringLiteral("Selection A"), QStringLiteral("First stable detail set"));
    const QString runB = runs.startRun(
        projectId, QStringLiteral("Selection B"), QStringLiteral("Second stable detail set"));
    assert(!runA.isEmpty() && !runB.isEmpty());
    assert(waitForStatus(runs, runA, {QStringLiteral("completed")})
        == QStringLiteral("completed"));
    assert(waitForStatus(runs, runB, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    for (int iteration = 0; iteration < iterations; ++iteration) {
        runs.selectRun(runA);
        // Start A's queued refresh before superseding it with B. Whether the
        // SQLite read has already finished or is still in flight is
        // intentionally scheduler-dependent; both outcomes must preserve B.
        ai::ui::test_support::pumpOneEventCycle();
        runs.selectRun(runB);
        const bool selectedProjectionSettled =
            ai::ui::test_support::waitForCondition("Selected Run projection", [&] {
                return runs.selectedRunId() == runB
                    && modelRowsBelongToRun(
                        runs.stageModel(), RunStageModel::RunIdRole, runB)
                    && modelRowsBelongToRun(
                        runs.artifactModel(), ArtifactListModel::RunIdRole, runB);
            }, [&] {
                return "iteration=" + std::to_string(iteration)
                    + ", selected=" + runs.selectedRunId().toStdString()
                    + ", stages=" + std::to_string(runs.stageModel()->rowCount())
                    + ", artifacts=" + std::to_string(runs.artifactModel()->rowCount());
            }, 4s);
        assert(selectedProjectionSettled);

        const QString cancelledRun = runs.startRun(
            projectId,
            QStringLiteral("Cancellation %1").arg(iteration),
            QStringLiteral("Cancel immediately while Qt events cross threads"));
        assert(!cancelledRun.isEmpty());
        assert(runs.selectedRunId() == cancelledRun);
        assert(runs.cancelRun(cancelledRun));
        assert(waitForStatus(runs, cancelledRun, {QStringLiteral("cancelled")})
            == QStringLiteral("cancelled"));
        assert(runs.selectedRunId() == cancelledRun);
    }
}

// Regression test for the approval audit trail
// (ChangeSetApplyService::approve()/apply(), AgentSessionController::
// approveAndApply()): apply() must refuse to run without a prior approval,
// and approveAndApply() must both record one and then write the file.
//
// The Qt adapter has no user-facing "create a proposal" action - proposals
// are only ever produced by a workspaceAccess agent's propose_change_set
// tool call during a real agent session, which needs a scripted-provider
// harness this UI test binary doesn't have (that lives in the Backend's own
// test suite). Instead, a real ReadyForReview change set is seeded for the
// bridge's own, already-persisted Run through a second, short-lived
// BackendRuntime opened on the same on-disk database file - once seeded,
// AgentSessionController::applyProposal()/approveAndApply() operate purely
// on a changeSetId and need no session/proposal-list UI state, so this still
// exercises the real SqliteChangeApprovalRepository/ChangeSetApplyService
// through the bridge under test, not a mock.
void testApproveAndApplyRequiresApprovalBeforeWritingFiles()
{
    namespace fs = std::filesystem;
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Approval test"),
        QStringLiteral("Verify apply requires approval"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    RunController& runs = bridge.runController();
    const QString runId = runs.startRun(
        projectId, QStringLiteral("Approval fixture run"),
        QStringLiteral("Produce a change set to approve"));
    assert(!runId.isEmpty());
    assert(waitForStatus(runs, runId, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    std::string changeSetId;
    {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());

        ai::application::CreateChangeProposalRequest request;
        request.runId = runId.toStdString();
        request.projectId = projectId.toStdString();
        request.providerId = "test-provider";
        request.modelId = "test-model";
        request.agentRole = "workspace-access";
        request.rawModelOutput =
            "```json\n"
            "{\"schemaVersion\":\"proposal-submission.v2\",\"summary\":\"Add a file\",\"changes\":[{"
            "\"change_id\":\"c1\",\"operation\":\"create\",\"relative_path\":\"approved.txt\","
            "\"source_path\":null,\"proposed_content\":\"hello from approval test\"}]}\n"
            "```";
        request.policy.projectRoot = workspace.projectPath;

        const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
        assert(created.success());
        assert(created.value().state == ai::application::ChangeSetState::ReadyForReview);
        changeSetId = created.value().changeSetId;
        const auto seededContent = seed.changeProposalService().readProposedContent(
            changeSetId, "c1");
        assert(seededContent.success());
        assert(seededContent.value() == "hello from approval test");
    }

    AgentSessionController& agentSessions = bridge.agentSessionController();
    const QString qChangeSetId = QString::fromStdString(changeSetId);
    const QString persistedContent = agentSessions.readProposedContent(
        qChangeSetId, QStringLiteral("c1"));
    assert(persistedContent == QStringLiteral("hello from approval test"));

    // apply() must refuse to run without a prior approve() call.
    assert(!agentSessions.applyProposal(qChangeSetId));
    assert(agentSessions.errorText()
        == QStringLiteral("This change set has not been approved for apply"));
    assert(!fs::exists(workspace.projectPath / "approved.txt"));

    // approveAndApply() records the approval and then writes the file.
    assert(agentSessions.approveAndApply(qChangeSetId));
    assert(agentSessions.applyResultText().startsWith(
        QStringLiteral("Applied 1 file(s) to workspace")));
    assert(fs::exists(workspace.projectPath / "approved.txt"));
}

// AG-07: a reviewer's reject/discard decision is a durable, separate fact
// from ChangeSetState/ApprovalRecord (see ProposalReviewTypes.hpp) that must
// still block AgentSessionController::approveAndApply() through the real
// BackendBridge/ProposalReviewService wiring - not just the pure-logic
// ProposalReviewService tests in ChangeProposalTests.cpp.
void testRejectProposalBlocksApproveAndApply()
{
    namespace fs = std::filesystem;
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Reject test"),
        QStringLiteral("Verify reviewer rejection blocks approve and apply"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    RunController& runs = bridge.runController();
    const QString runId = runs.startRun(
        projectId, QStringLiteral("Reject fixture run"),
        QStringLiteral("Produce a change set to reject"));
    assert(!runId.isEmpty());
    assert(waitForStatus(runs, runId, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    std::string changeSetId;
    {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());

        ai::application::CreateChangeProposalRequest request;
        request.runId = runId.toStdString();
        request.projectId = projectId.toStdString();
        request.providerId = "test-provider";
        request.modelId = "test-model";
        request.agentRole = "workspace-access";
        request.rawModelOutput =
            "```json\n"
            "{\"schemaVersion\":\"proposal-submission.v2\",\"summary\":\"Add a file\",\"changes\":[{"
            "\"change_id\":\"c1\",\"operation\":\"create\",\"relative_path\":\"rejected.txt\","
            "\"source_path\":null,\"proposed_content\":\"should never be written\"}]}\n"
            "```";
        request.policy.projectRoot = workspace.projectPath;

        const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
        assert(created.success());
        assert(created.value().state == ai::application::ChangeSetState::ReadyForReview);
        changeSetId = created.value().changeSetId;
    }

    AgentSessionController& agentSessions = bridge.agentSessionController();
    const QString qChangeSetId = QString::fromStdString(changeSetId);

    assert(agentSessions.rejectProposal(qChangeSetId, QStringLiteral("Not needed")));

    // Idempotent: rejecting again succeeds and does not error.
    assert(agentSessions.rejectProposal(qChangeSetId, QStringLiteral("A different reason")));

    // approveAndApply() must now refuse before recording any approval.
    assert(!agentSessions.approveAndApply(qChangeSetId));
    assert(agentSessions.errorText()
        == QStringLiteral(
            "This proposal was rejected by a reviewer and cannot be approved or applied"));
    assert(!fs::exists(workspace.projectPath / "rejected.txt"));
}

// Reject/discard must also block a bare applyProposal() call when an
// approval already exists from before the rejection (e.g. a failed-and-
// rolled-back apply attempt the reviewer then decides to abandon instead of
// retrying) - the ordering-independence AgentSessionController::
// applyProposal()'s own ProposalReviewService gate exists for.
void testRejectProposalBlocksApplyEvenWithPriorApproval()
{
    namespace fs = std::filesystem;
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("Reject after approval test"),
        QStringLiteral("Verify reviewer rejection blocks apply after a prior approval"),
        workspace.projectPathQt());
    assert(!projectId.isEmpty());

    RunController& runs = bridge.runController();
    const QString runId = runs.startRun(
        projectId, QStringLiteral("Reject after approval fixture run"),
        QStringLiteral("Produce a change set to approve then reject"));
    assert(!runId.isEmpty());
    assert(waitForStatus(runs, runId, {QStringLiteral("completed")})
        == QStringLiteral("completed"));

    std::string changeSetId;
    {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());

        ai::application::CreateChangeProposalRequest request;
        request.runId = runId.toStdString();
        request.projectId = projectId.toStdString();
        request.providerId = "test-provider";
        request.modelId = "test-model";
        request.agentRole = "workspace-access";
        request.rawModelOutput =
            "```json\n"
            "{\"schemaVersion\":\"proposal-submission.v2\",\"summary\":\"Add a file\",\"changes\":[{"
            "\"change_id\":\"c1\",\"operation\":\"create\",\"relative_path\":\"never.txt\","
            "\"source_path\":null,\"proposed_content\":\"should never be written either\"}]}\n"
            "```";
        request.policy.projectRoot = workspace.projectPath;

        const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
        assert(created.success());
        changeSetId = created.value().changeSetId;

        // Seed a prior approval directly, the same way the approval test
        // above seeds its proposal - before the reviewer ever rejects it.
        const auto approved = seed.changeSetApplyService().approve(changeSetId, std::nullopt);
        assert(approved.success());
    }

    AgentSessionController& agentSessions = bridge.agentSessionController();
    const QString qChangeSetId = QString::fromStdString(changeSetId);

    assert(agentSessions.rejectProposal(qChangeSetId, QString()));

    // applyProposal() (not approveAndApply()) is the path that would have
    // reused the pre-existing approval - it must still be refused.
    assert(!agentSessions.applyProposal(qChangeSetId));
    assert(agentSessions.errorText()
        == QStringLiteral(
            "This proposal was rejected by a reviewer and cannot be approved or applied"));
    assert(!fs::exists(workspace.projectPath / "never.txt"));
}

void testProposalLineageRolesLinkPredecessorAndSuccessor()
{
    AgentProposalListModel model;
    ai::application::AgentProposalReadModel successor;
    successor.header.changeSetId = "proposal-successor";
    successor.header.summary = "Repaired proposal";
    successor.header.supersedesChangeSetId = "proposal-predecessor";
    successor.header.state = ai::application::ChangeSetState::ReadyForReview;
    ai::application::AgentProposalReadModel predecessor;
    predecessor.header.changeSetId = "proposal-predecessor";
    predecessor.header.summary = "Rejected attempt";
    predecessor.header.state = ai::application::ChangeSetState::Rejected;
    model.replace({successor, predecessor});

    const auto successorIndex = model.index(0, 0);
    const auto predecessorIndex = model.index(1, 0);
    assert(model.data(successorIndex,
               AgentProposalListModel::SupersedesChangeSetIdRole).toString()
        == QStringLiteral("proposal-predecessor"));
    assert(!model.data(successorIndex,
                AgentProposalListModel::SupersededRole).toBool());
    assert(model.data(predecessorIndex,
               AgentProposalListModel::SupersededRole).toBool());
    assert(model.data(predecessorIndex,
               AgentProposalListModel::SupersededByChangeSetIdRole).toString()
        == QStringLiteral("proposal-successor"));
}

// AG-11 remaining slice: ProjectMemoryController end to end through the real
// BackendBridge -> BackendRuntime chain. Segments only ever come from a
// completed Run (ProjectMemoryService::recordRunOutcome, called best-effort
// by RunManager), so this drives two real Mock runs to completion instead of
// fabricating segment rows directly. Both compact() paths exercised here
// (insufficient segments, provider not found) fail before ever reaching
// IProviderClientFactory, so - like the rest of this file - no real or fake
// provider network call is made.
bool waitForSegmentCount(
    ProjectMemoryController& controller,
    const QString& projectId,
    int expected,
    std::chrono::milliseconds timeout = 4s)
{
    return ai::ui::test_support::waitForCondition("Project memory segment count", [&] {
        return controller.segments(projectId).size() == expected;
    }, [&] {
        return "expected=" + std::to_string(expected)
            + ", actual=" + std::to_string(controller.segments(projectId).size());
    }, timeout);
}

void testProjectMemoryControllerViewerCompactionValidationAndClear()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());

    const fs::path projectARoot = workspace.projectPath / "a";
    const fs::path projectBRoot = workspace.projectPath / "b";
    fs::create_directories(projectARoot);
    fs::create_directories(projectBRoot);

    ProjectController& projects = bridge.projectController();
    const QString projectAId = projects.createProject(
        QStringLiteral("Memory project A"), QStringLiteral("Viewer/compact target"),
        QString::fromStdWString(projectARoot.wstring()));
    const QString projectBId = projects.createProject(
        QStringLiteral("Memory project B"), QStringLiteral("Isolation control"),
        QString::fromStdWString(projectBRoot.wstring()));
    assert(!projectAId.isEmpty() && !projectBId.isEmpty());

    ProjectMemoryController& memory = bridge.projectMemoryController();
    assert(bridge.projectMemory() == &memory);
    assert(memory.segments(projectAId).isEmpty());
    assert(memory.compactionHistory(projectAId).isEmpty());
    assert(!memory.compacting());

    QString lastErrorCode;
    QString lastErrorMessage;
    int compactingChangedCount = 0;
    QObject::connect(&memory, &ProjectMemoryController::errorOccurred,
        [&](const QString& code, const QString& message) {
            lastErrorCode = code;
            lastErrorMessage = message;
        });
    QObject::connect(&memory, &ProjectMemoryController::compactingChanged,
        [&] { ++compactingChangedCount; });

    RunController& runs = bridge.runController();
    const QString firstRunId = runs.startRun(
        projectAId, QStringLiteral("Memory run 1"), QStringLiteral("Produce the first segment"));
    assert(!firstRunId.isEmpty());
    assert(waitForStatus(runs, firstRunId, {QStringLiteral("completed")}) == QStringLiteral("completed"));
    assert(waitForSegmentCount(memory, projectAId, 1));

    const QVariantList firstSegments = memory.segments(projectAId);
    const QVariantMap firstSegment = firstSegments.constFirst().toMap();
    assert(firstSegment.value(QStringLiteral("segmentId")).toString().size() > 0);
    assert(firstSegment.value(QStringLiteral("sourceRunId")).toString() == firstRunId);
    assert(firstSegment.value(QStringLiteral("sourceTokenCount")).toULongLong() > 0);
    assert(firstSegment.value(QStringLiteral("summaryTokenCount")).toULongLong() > 0);
    assert(!firstSegment.value(QStringLiteral("createdAt")).toString().isEmpty());
    assert(memory.compactionHistory(projectAId).isEmpty());

    // Empty provider/model must never start a worker at all.
    assert(!memory.compact(projectAId, QString(), QString()));
    assert(!memory.compacting());
    assert(compactingChangedCount == 0);

    // compact() checks project/provider/model before the segment-count floor
    // (see ProjectMemoryCompactionService::compact()), so an unknown provider
    // is refused there first even though this project also has too few
    // segments. The busy state is synchronously true immediately after
    // compact() returns, since the queued completion callback cannot run
    // before this thread returns to the event loop.
    assert(memory.compact(projectAId, QStringLiteral("nonexistent-provider"), QStringLiteral("nonexistent-model")));
    assert(memory.compacting());
    assert(memory.compactingProjectId() == projectAId);
    assert(ai::ui::test_support::waitForCondition("Compaction settles (provider unavailable)", [&] {
        return !memory.compacting();
    }, [&] { return lastErrorCode.toStdString(); }, 4s));
    assert(lastErrorCode == QStringLiteral("project_memory.provider_unavailable"));
    assert(!lastErrorMessage.isEmpty());
    assert(compactingChangedCount == 2);
    // A refused attempt before the provider check must never alter memory or
    // leave a compaction record behind.
    assert(memory.segments(projectAId).size() == 1);
    assert(memory.compactionHistory(projectAId).isEmpty());

    // A real, enabled provider/model clears that first check, so the very
    // next refusal proves the independent segment-count floor - still without
    // ever reaching IProviderClientFactory.
    ProviderCatalogController& catalog = bridge.providerCatalogController();
    const QString providerId = catalog.createProvider(
        QStringLiteral("Memory test provider"), QStringLiteral("local"),
        QStringLiteral("http://127.0.0.1:11434/v1"));
    assert(!providerId.isEmpty());
    const QString modelId = catalog.createModel(
        providerId, QStringLiteral("memory-test-model"), QStringLiteral("Memory test model"),
        32768, 4096, 0, 0);
    assert(!modelId.isEmpty());

    lastErrorCode.clear();
    lastErrorMessage.clear();
    compactingChangedCount = 0;
    assert(memory.compact(projectAId, providerId, modelId));
    assert(ai::ui::test_support::waitForCondition("Compaction settles (insufficient segments)", [&] {
        return !memory.compacting();
    }, [&] { return lastErrorCode.toStdString(); }, 4s));
    assert(lastErrorCode == QStringLiteral("project_memory.insufficient_segments_to_compact"));
    assert(memory.segments(projectAId).size() == 1);
    assert(memory.compactionHistory(projectAId).isEmpty());

    const QVariantList choices = memory.providerChoices();
    assert(choices.size() == 1);
    assert(choices.constFirst().toMap().value(QStringLiteral("modelId")).toString() == modelId);

    // A second, busy compaction must be refused outright rather than queued
    // or silently dropped - it must not disturb the compacting state either.
    assert(memory.compact(projectAId, providerId, modelId));
    assert(memory.compacting());
    assert(!memory.compact(projectAId, providerId, modelId));
    assert(ai::ui::test_support::waitForCondition("Compaction settles (second insufficient-segments attempt)", [&] {
        return !memory.compacting();
    }, [&] { return lastErrorCode.toStdString(); }, 4s));

    // Isolation control: give the second project its own segment before
    // clearing the first, so a scoping bug would show up as project B losing
    // data it never should have touched.
    const QString controlRunId = runs.startRun(
        projectBId, QStringLiteral("Memory run B"), QStringLiteral("Isolation control segment"));
    assert(!controlRunId.isEmpty());
    assert(waitForStatus(runs, controlRunId, {QStringLiteral("completed")}) == QStringLiteral("completed"));
    assert(waitForSegmentCount(memory, projectBId, 1));

    assert(!memory.clearMemory(QString()));
    assert(memory.clearMemory(projectAId));
    assert(waitForSegmentCount(memory, projectAId, 0));
    assert(memory.segments(projectBId).size() == 1);
    // Both earlier refusals were pre-attempt validation failures (insufficient
    // segments, provider not found) - per ProjectMemoryCompactionService's
    // contract neither one ever reaches the provider, so neither persists a
    // compaction record. clearMemory() is destructive to segments only; this
    // asserts it did not somehow fabricate provenance either.
    assert(memory.compactionHistory(projectAId).isEmpty());
}

// ---------------------------------------------------------------------------------------------
// M2c: the long-context tier in the catalog controller, and the analytics price state.
// ---------------------------------------------------------------------------------------------

struct TierCatalogFixture {
    ai::application::InMemoryProviderRepository providers;
    ai::application::InMemoryModelRepository models;
    ai::application::ProviderCatalogService service {providers, models};
    ProviderCatalogController controller {service};
    QString providerId;
    QString modelId;

    TierCatalogFixture()
    {
        providerId = controller.createProvider(
            QStringLiteral("Tier provider"), QStringLiteral("local"),
            QStringLiteral("http://127.0.0.1:11434/v1"));
        assert(!providerId.isEmpty());
        // A large-window model, created flat ($2 in / $10 out): the tier is entered afterwards.
        modelId = controller.createModel(
            providerId, QStringLiteral("tier-model"), QStringLiteral("Tier model"),
            1'050'000, 128'000, 2'000'000, 10'000'000);
        assert(!modelId.isEmpty());
    }

    [[nodiscard]] QVariant role(const char* name)
    {
        const int roleId = controller.models()->roleNames().key(name);
        assert(roleId != 0 && "the model list does not expose this role");
        return controller.models()->data(controller.models()->index(0, 0), roleId);
    }

    [[nodiscard]] ai::application::Model stored()
    {
        const auto found = service.findModel(modelId.toStdString());
        assert(found.success() && found.value().has_value());
        return *found.value();
    }

    // The tier as the Qt model projects it AND as the Backend stores it must both be `expected`.
    void requireTier(const ai::application::LongContextPricing& expected)
    {
        assert(role("longContextEnabled").toBool() == expected.enabled());
        assert(role("longContextThresholdInputTokens").toULongLong() == expected.thresholdInputTokens);
        assert(role("longContextInputPriceMicros").toULongLong() == expected.inputPriceMicrosPerMillionTokens);
        assert(role("longContextOutputPriceMicros").toULongLong() == expected.outputPriceMicrosPerMillionTokens);
        assert(role("longContextCacheWritePriceMicros").toULongLong() == expected.cacheWritePriceMicrosPerMillionTokens);
        assert(role("longContextCacheReadPriceMicros").toULongLong() == expected.cacheReadPriceMicrosPerMillionTokens);
        assert(role("longContextCacheWriteOneHourPriceMicros").toULongLong()
            == expected.cacheWriteOneHourPriceMicrosPerMillionTokens);
        assert(stored().longContextPricing == expected);
    }
};

const ai::application::LongContextPricing kCompleteTier {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};

void testRunChoicesFollowCatalogChangesWithoutRestart()
{
    TempWorkspace workspace;
    auto bridge = std::make_unique<BackendBridge>(workspace.databasePathQt(), nullptr);
    auto& runs = bridge->runController();
    auto& catalog = bridge->providerCatalogController();
    const auto waitForChoices = [&](const QString& expectedModel) {
        return ai::ui::test_support::waitForCondition("live Run model choices", [&] {
            if (runs.agentChoicesLoading()) return false;
            const auto choices = runs.agentChoices();
            return expectedModel.isEmpty() ? choices.isEmpty()
                : choices.size() == 1
                    && choices.constFirst().toMap().value(QStringLiteral("modelId")).toString() == expectedModel;
        }, 5s);
    };
    // Finish the initial empty read before adding the first model: otherwise
    // a late startup read could hide a missing catalog-change connection.
    assert(waitForChoices({}));
    const QString provider = catalog.createProvider(QStringLiteral("First-use OpenAI"),
        QStringLiteral("openai"), QStringLiteral("https://api.openai.invalid"));
    assert(!provider.isEmpty());
    const QString model = catalog.createCatalogModel(provider, QStringLiteral("gpt-6-sol"));
    assert(!model.isEmpty());
    assert(waitForChoices(model));
    assert(catalog.setModelEnabled(model, false));
    assert(waitForChoices({}));
    assert(catalog.setModelEnabled(model, true));
    assert(waitForChoices(model));
    assert(catalog.setProviderEnabled(provider, false));
    assert(waitForChoices({}));
    assert(catalog.setProviderEnabled(provider, true));
    assert(waitForChoices(model));
    assert(catalog.deleteModel(model));
    assert(waitForChoices({}));
}

void testCatalogChangesRefreshChoicesOnceWithoutRefreshingSettings()
{
    TempWorkspace workspace;
    auto bridge = std::make_unique<BackendBridge>(workspace.databasePathQt(), nullptr);
    auto& catalog = bridge->providerCatalogController();
    auto& settings = bridge->settingsController();
    const QString provider = catalog.createProvider(QStringLiteral("Catalog refresh"),
        QStringLiteral("openai"), QStringLiteral("https://api.openai.invalid"));
    const QString model = catalog.createCatalogModel(provider, QStringLiteral("gpt-6-sol"));
    assert(!provider.isEmpty() && !model.isEmpty());

    int catalogChanges = 0;
    int choiceChanges = 0;
    int settingsChanges = 0;
    int scratchChecks = 0;
    int chatChanges = 0;
    int comparisonChanges = 0;
    QObject::connect(&catalog, &ProviderCatalogController::catalogChanged, bridge.get(), [&] {
        ++catalogChanges;
        // Consumers see the complete catalog, never the interval between its two resets.
        assert(catalog.providers()->rowCount() == 1);
        assert(catalog.models()->rowCount() == (catalogChanges < 5 ? 1 : 0));
    });
    QObject::connect(&settings, &SettingsController::choicesChanged, bridge.get(), [&] { ++choiceChanges; });
    QObject::connect(&settings, &SettingsController::settingsChanged, bridge.get(), [&] { ++settingsChanges; });
    QObject::connect(&settings, &SettingsController::verificationScratchChanged, bridge.get(), [&] { ++scratchChecks; });
    QObject::connect(&bridge->chatController(), &ChatController::choicesChanged, bridge.get(), [&] { ++chatChanges; });
    QObject::connect(&bridge->comparisonController(), &ComparisonController::stateChanged, bridge.get(), [&] { ++comparisonChanges; });
    const auto requireRefreshes = [&](int expected, int expectedChoices) {
        assert(catalogChanges == expected);
        assert(choiceChanges == expected);
        assert(chatChanges == expected);
        assert(comparisonChanges == expected);
        assert(settingsChanges == 0);
        assert(scratchChecks == 0);
        assert(settings.chatModelChoices().size() == expectedChoices);
    };
    assert(catalog.setProviderEnabled(provider, false));
    requireRefreshes(1, 1); // Only the "first available" placeholder remains.
    assert(catalog.setProviderEnabled(provider, true));
    requireRefreshes(2, 2);
    assert(catalog.setModelEnabled(model, false));
    requireRefreshes(3, 1);
    assert(catalog.setModelEnabled(model, true));
    requireRefreshes(4, 2);
    assert(catalog.deleteModel(model));
    requireRefreshes(5, 1);
    assert(!catalog.setProviderEnabled(QStringLiteral("missing-provider"), false));
    requireRefreshes(5, 1); // A rejected command does not announce a catalog change.
    catalog.refresh();
    requireRefreshes(6, 1);
}

// create: a model is created flat - no tier, nothing auto-filled - and the model list says so.
void testCatalogControllerCreatesAFlatModelWithAnEmptyTier()
{
    TierCatalogFixture fixture;
    fixture.requireTier({});
    assert(!fixture.role("longContextEnabled").toBool());
    assert(fixture.role("contextWindowTokens").toULongLong() == 1'050'000);
}

// update: ONE atomic save carries the flat prices, the cache prices and the whole tier.
void testCatalogControllerSavesThePricingFormWithTheTierInOneUpdate()
{
    TierCatalogFixture fixture;
    assert(fixture.controller.saveModelPricingWithCacheDuration(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000,
        4'000'000, 272'000, 4'000'000, 15'000'000, 5'000'000, 400'000,
        8'000'000));
    fixture.requireTier({272'000, 4'000'000, 15'000'000, 5'000'000, 400'000, 8'000'000});
    assert(fixture.role("inputPriceMicros").toULongLong() == 2'000'000);
    assert(fixture.role("cacheWritePriceMicros").toULongLong() == 2'500'000);
    assert(fixture.role("cacheReadPriceMicros").toULongLong() == 200'000);
    assert(fixture.role("cacheWriteOneHourPriceMicros").toULongLong() == 4'000'000);
    assert(fixture.controller.lastError().isEmpty());

    // The tier can be edited like any other value.
    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000,
        300'000, 4'500'000, 16'000'000, 6'000'000, 500'000));
    fixture.requireTier({300'000, 4'500'000, 16'000'000, 6'000'000, 500'000, 8'000'000});
    assert(fixture.role("cacheWriteOneHourPriceMicros").toULongLong() == 4'000'000);
}

// preserve: edits that do not name the tier never lose it.
void testCatalogEditsThatDoNotNameTheTierPreserveIt()
{
    TierCatalogFixture fixture;
    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000,
        272'000, 4'000'000, 15'000'000, 5'000'000, 400'000));

    // The full model edit (name, limits, flat prices) - the form ModelFormDialog drives.
    assert(fixture.controller.updateModel(
        fixture.modelId, QStringLiteral("tier-model"), QStringLiteral("Renamed model"),
        1'050'000, 64'000, 3'000'000, 11'000'000));
    assert(fixture.role("displayName").toString() == QStringLiteral("Renamed model"));
    assert(fixture.role("inputPriceMicros").toULongLong() == 3'000'000);
    fixture.requireTier(kCompleteTier);

    // The flat-only pricing command (the older form) changes only what it names.
    assert(fixture.controller.updateModelPricing(fixture.modelId, 1'000'000, 2'000'000, 3'000'000, 4'000'000));
    assert(fixture.role("cacheReadPriceMicros").toULongLong() == 4'000'000);
    fixture.requireTier(kCompleteTier);

    // Enabling / disabling the model and the default flag are not pricing edits either.
    assert(fixture.controller.setModelEnabled(fixture.modelId, false));
    assert(fixture.controller.setModelEnabled(fixture.modelId, true));
    fixture.requireTier(kCompleteTier);
}

// clear: the tier is removed only by an explicit save with a zero threshold and zero prices.
void testCatalogControllerRemovesTheTierOnlyExplicitly()
{
    TierCatalogFixture fixture;
    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000,
        272'000, 4'000'000, 15'000'000, 5'000'000, 400'000));
    fixture.requireTier(kCompleteTier);

    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000, 0, 0, 0, 0, 0));
    fixture.requireTier({});
    assert(!fixture.role("longContextEnabled").toBool());
    // The flat and cache prices of the same save are intact.
    assert(fixture.role("inputPriceMicros").toULongLong() == 2'000'000);
    assert(fixture.role("cacheWritePriceMicros").toULongLong() == 2'500'000);
}

// partial: a long-context price that was not entered stays 0 ("not entered"); the tier is accepted.
void testCatalogControllerAcceptsPartiallyEnteredLongContextPrices()
{
    TierCatalogFixture fixture;
    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 0, 0, 272'000, 4'000'000, 0, 0, 0));
    fixture.requireTier({272'000, 4'000'000, 0, 0, 0});
    assert(fixture.role("longContextEnabled").toBool());
    assert(fixture.role("longContextOutputPriceMicros").toULongLong() == 0);
}

// built-in catalog: a model added from it is created with EVERY catalog value (a fresh install must
// not start with zero cache prices or without the long-context tier), and the catalog itself is
// internally consistent: each Anthropic entry prices the 5-minute cache at 1.25x write / 0.1x read.
void testCatalogControllerCreatesCatalogModelsWithCachePricesAndTheTier()
{
    ai::application::InMemoryProviderRepository providers;
    ai::application::InMemoryModelRepository models;
    ai::application::ProviderCatalogService service {providers, models};
    ProviderCatalogController controller {service};
    const QString openAi = controller.createProvider(
        QStringLiteral("OpenAI"), QStringLiteral("openai"), QStringLiteral("https://api.openai.invalid"));
    const QString anthropic = controller.createProvider(
        QStringLiteral("Anthropic"), QStringLiteral("anthropic"), QStringLiteral("https://api.anthropic.invalid"));
    const QString local = controller.createProvider(
        QStringLiteral("Local"), QStringLiteral("local"), QStringLiteral("http://127.0.0.1:11434/v1"));
    assert(!openAi.isEmpty() && !anthropic.isEmpty() && !local.isEmpty());

    const auto storedModel = [&](const QString& modelId) {
        const auto found = service.findModel(modelId.toStdString());
        assert(found.success() && found.value().has_value());
        return *found.value();
    };

    // gpt-6-sol: the official Standard rates and the long-context tier, from the catalog alone.
    const QString sol = controller.createCatalogModel(openAi, QStringLiteral("gpt-6-sol"));
    assert(!sol.isEmpty() && controller.lastError().isEmpty());
    const auto solModel = storedModel(sol);
    assert(solModel.contextWindowTokens == 1'050'000 && solModel.maxOutputTokens == 128'000);
    assert(solModel.inputPriceMicrosPerMillionTokens == 2'000'000);
    assert(solModel.outputPriceMicrosPerMillionTokens == 10'000'000);
    assert(solModel.cacheWritePriceMicrosPerMillionTokens == 2'500'000);
    assert(solModel.cacheReadPriceMicrosPerMillionTokens == 200'000);
    assert(solModel.longContextPricing == kCompleteTier);

    // Every entry of both catalogs is creatable and matches what the entry itself advertises.
    QString sonnetId;
    for (const QString& provider : {openAi, anthropic}) {
        const QString kind = provider == openAi ? QStringLiteral("openai") : QStringLiteral("anthropic");
        for (const QVariant& entryValue : controller.catalogModelsForProvider(kind)) {
            const QVariantMap entry = entryValue.toMap();
            const QString apiId = entry.value(QStringLiteral("apiModelId")).toString();
            const QString created = provider == openAi && apiId == QStringLiteral("gpt-6-sol")
                ? sol : controller.createCatalogModel(provider, apiId);
            assert(!created.isEmpty());
            if (apiId == QStringLiteral("claude-sonnet-5"))
                sonnetId = created;
            const auto model = storedModel(created);
            assert(model.inputPriceMicrosPerMillionTokens == entry.value(QStringLiteral("inputPriceMicros")).toULongLong());
            assert(model.outputPriceMicrosPerMillionTokens == entry.value(QStringLiteral("outputPriceMicros")).toULongLong());
            assert(model.cacheWritePriceMicrosPerMillionTokens == entry.value(QStringLiteral("cacheWritePriceMicros")).toULongLong());
            assert(model.cacheReadPriceMicrosPerMillionTokens == entry.value(QStringLiteral("cacheReadPriceMicros")).toULongLong());
            assert(model.longContextPricing.thresholdInputTokens
                == entry.value(QStringLiteral("longContextThresholdInputTokens")).toULongLong());
            if (kind == QStringLiteral("anthropic")) {
                assert(model.cacheWritePriceMicrosPerMillionTokens * 4 == model.inputPriceMicrosPerMillionTokens * 5);
                assert(model.cacheReadPriceMicrosPerMillionTokens * 10 == model.inputPriceMicrosPerMillionTokens);
            }
        }
    }
    // The superseded Sonnet 4.6 rate ($3 / $15) must not come back.
    assert(!sonnetId.isEmpty());
    const auto sonnet = storedModel(sonnetId);
    assert(sonnet.inputPriceMicrosPerMillionTokens == 2'000'000 && sonnet.outputPriceMicrosPerMillionTokens == 10'000'000);

    // Refusals change nothing and say why: an id outside the provider's catalog (also another
    // provider's), a provider without a catalog, an unknown provider.
    const int rowsBefore = controller.models()->rowCount();
    assert(controller.createCatalogModel(anthropic, QStringLiteral("gpt-6-sol")).isEmpty());
    assert(controller.lastError().contains(QStringLiteral("catalog")));
    assert(controller.createCatalogModel(local, QStringLiteral("gpt-6-sol")).isEmpty());
    assert(controller.createCatalogModel(QStringLiteral("no-such-provider"), QStringLiteral("gpt-6-sol")).isEmpty());
    assert(controller.models()->rowCount() == rowsBefore);
}

// validation errors: every rule is the Backend's; the controller shows its typed code and text,
// and a refused save changes nothing - not even the flat prices the same form carried.
void testCatalogControllerShowsBackendValidationErrorsAndChangesNothing()
{
    TierCatalogFixture fixture;
    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000,
        272'000, 4'000'000, 15'000'000, 5'000'000, 400'000));

    QString lastCode;
    QString lastMessage;
    QObject::connect(&fixture.controller, &ProviderCatalogController::operationFailed,
        &fixture.controller, [&](const QString& code, const QString& message) {
            lastCode = code;
            lastMessage = message;
        });

    const auto expectRefusal = [&](ai::application::LongContextPricingProblem problem) {
        assert(lastCode == QString::fromLatin1(ai::application::longContextPricingProblemCode(problem)));
        const QString backendText =
            QString::fromLatin1(ai::application::longContextPricingProblemMessage(problem));
        assert(lastMessage == backendText);
        assert(fixture.controller.lastError() == backendText);
        // Nothing changed: the previous tier AND the previous flat prices are still stored.
        fixture.requireTier(kCompleteTier);
        assert(fixture.role("inputPriceMicros").toULongLong() == 2'000'000);
        lastCode.clear();
        lastMessage.clear();
    };

    // Long-context prices without a threshold.
    assert(!fixture.controller.saveModelPricing(
        fixture.modelId, 9'000'000, 9'000'000, 9'000'000, 9'000'000, 0, 4'000'000, 0, 0, 0));
    expectRefusal(ai::application::LongContextPricingProblem::PricesWithoutThreshold);
    assert(!fixture.controller.saveModelPricingWithCacheDuration(
        fixture.modelId, 9'000'000, 9'000'000, 9'000'000, 9'000'000,
        9'000'000, 0, 0, 0, 0, 0, 1));
    expectRefusal(ai::application::LongContextPricingProblem::PricesWithoutThreshold);

    // A threshold at the context window, and above it.
    assert(!fixture.controller.saveModelPricing(
        fixture.modelId, 9'000'000, 9'000'000, 9'000'000, 9'000'000,
        1'050'000, 4'000'000, 15'000'000, 5'000'000, 400'000));
    expectRefusal(ai::application::LongContextPricingProblem::ThresholdOutOfRange);
    assert(!fixture.controller.saveModelPricing(
        fixture.modelId, 9'000'000, 9'000'000, 9'000'000, 9'000'000,
        2'000'000, 4'000'000, 15'000'000, 5'000'000, 400'000));
    expectRefusal(ai::application::LongContextPricingProblem::ThresholdOutOfRange);

    // An ordinary edit that would leave the stored tier at or above the context window is refused
    // too (the Backend validates the RESULTING model), and the model keeps its window.
    assert(!fixture.controller.updateModel(
        fixture.modelId, QStringLiteral("tier-model"), QStringLiteral("Tier model"),
        272'000, 128'000, 2'000'000, 10'000'000));
    expectRefusal(ai::application::LongContextPricingProblem::ThresholdOutOfRange);
    assert(fixture.role("contextWindowTokens").toULongLong() == 1'050'000);

    // An unknown model.
    assert(!fixture.controller.saveModelPricing(
        QStringLiteral("no-such-model"), 1, 1, 1, 1, 0, 0, 0, 0, 0));
    assert(lastCode == QStringLiteral("model.not_found"));
    assert(fixture.controller.lastError() == QStringLiteral("Model not found"));

    // A successful save clears the error again.
    assert(fixture.controller.saveModelPricing(
        fixture.modelId, 2'000'000, 10'000'000, 2'500'000, 200'000,
        272'000, 4'000'000, 15'000'000, 5'000'000, 400'000));
    assert(fixture.controller.lastError().isEmpty());
}

struct AnalyticsStateFixture {
    ai::application::InMemoryProviderRepository providers;
    ai::application::InMemoryModelRepository models;
    ai::application::InMemoryModelExecutionRepository executions;
    ai::application::ProviderCatalogService catalog {providers, models};
    ai::application::ExecutionAnalyticsService analytics {executions, models};
    ExecutionAnalyticsController controller {analytics, catalog};
    std::uint32_t nextPosition {0};

    [[nodiscard]] QString addProvider(const char* name)
    {
        const auto created = catalog.createProvider(
            {name, "local", "http://127.0.0.1:11434/v1", true, std::nullopt});
        assert(created.success());
        return QString::fromStdString(created.value());
    }

    [[nodiscard]] QString addModel(const QString& providerId, const char* apiModelId)
    {
        const auto created = catalog.createModel(
            {providerId.toStdString(), apiModelId, apiModelId, 128'000, 4096,
             1'000'000, 2'000'000, 0, 0, true});
        assert(created.success());
        return QString::fromStdString(created.value());
    }

    // One finished execution with a STORED cost and a stored price state.
    void addRow(const QString& providerId, const QString& modelId, std::uint64_t input,
        std::uint64_t output, std::uint64_t costMicros, bool priceKnown,
        const char* sourceId = "run-m2c")
    {
        ai::application::ModelExecution execution;
        execution.id = "exec-" + std::to_string(nextPosition);
        execution.source = ai::application::ModelExecutionSource::Run;
        execution.sourceId = sourceId;
        execution.projectId = "project-m2c";
        execution.providerId = providerId.toStdString();
        execution.modelId = modelId.toStdString();
        execution.role = "worker";
        execution.position = nextPosition++;
        execution.status = ai::application::ModelExecutionStatus::Completed;
        execution.inputTokens = input;
        execution.outputTokens = output;
        execution.costMicros = costMicros;
        execution.costPriceKnown = priceKnown;
        execution.createdAt = std::chrono::system_clock::now();
        assert(executions.insertExecution(execution).success());
    }

    [[nodiscard]] QVariant providerRole(const QString& providerId, const char* name)
    {
        QAbstractItemModel* model = controller.providers();
        const int providerIdRole = model->roleNames().key("providerId");
        const int role = model->roleNames().key(name);
        assert(role != 0);
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), providerIdRole).toString() == providerId)
                return model->data(model->index(row, 0), role);
        }
        assert(false && "the provider row was not projected");
        return {};
    }

    [[nodiscard]] QVariantMap modelEntry(const QString& providerId, const QString& modelId)
    {
        const QVariantList entries = providerRole(providerId, "models").toList();
        for (const QVariant& entry : entries) {
            if (entry.toMap().value(QStringLiteral("modelId")).toString() == modelId)
                return entry.toMap();
        }
        assert(false && "the model entry was not projected");
        return {};
    }
};

// The four price states at every level the Overview shows: the total, each provider and each model.
void testAnalyticsControllerProjectsTheFourPriceStatesAtEveryLevel()
{
    AnalyticsStateFixture fixture;
    const QString knownProvider = fixture.addProvider("Known provider");
    const QString knownModel = fixture.addModel(knownProvider, "known-model");
    const QString partialProvider = fixture.addProvider("Partial provider");
    const QString partialModel = fixture.addModel(partialProvider, "partial-model");
    const QString unknownProvider = fixture.addProvider("Unknown provider");
    const QString unknownModel = fixture.addModel(unknownProvider, "unknown-model");

    // Known: a stored amount with a known price ($0.0007).
    fixture.addRow(knownProvider, knownModel, 1'000, 100, 700, true);
    // Partial: a stored LOWER BOUND ($0.25) with an incomplete price.
    fixture.addRow(partialProvider, partialModel, 1'000, 100, 250'000, false);
    // Unknown: an unpriced execution with no amount at all.
    fixture.addRow(unknownProvider, unknownModel, 1'000, 100, 0, false);
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-m2c")));

    // Total: something was priced and something was not - a lower bound.
    assert(fixture.controller.executionCount() == 3);
    assert(fixture.controller.priceState() == QStringLiteral("partial"));
    assert(fixture.controller.unknownPriceExecutionCount() == 2);
    assert(fixture.controller.priceIncomplete());
    assert(fixture.controller.costText() == QStringLiteral("$0.2507"));

    // Provider and model levels carry their own state, amount text and incomplete flag.
    struct Expected { QString provider; QString model; QString state; QString text; bool incomplete; };
    const Expected expected[] = {
        {knownProvider, knownModel, QStringLiteral("known"), QStringLiteral("$0.0007"), false},
        {partialProvider, partialModel, QStringLiteral("partial"), QStringLiteral("$0.2500"), true},
        {unknownProvider, unknownModel, QStringLiteral("unknown"), QString(), true},
    };
    for (const auto& item : expected) {
        assert(fixture.providerRole(item.provider, "priceState").toString() == item.state);
        assert(fixture.providerRole(item.provider, "costText").toString() == item.text);
        assert(fixture.providerRole(item.provider, "priceIncomplete").toBool() == item.incomplete);
        const QVariantMap entry = fixture.modelEntry(item.provider, item.model);
        assert(entry.value(QStringLiteral("priceState")).toString() == item.state);
        assert(entry.value(QStringLiteral("costText")).toString() == item.text);
        assert(entry.value(QStringLiteral("priceIncomplete")).toBool() == item.incomplete);
    }
    // An unknown price has NO amount text: it can never be rendered as "$0".
    assert(fixture.providerRole(unknownProvider, "costText").toString().isEmpty());
    assert(fixture.providerRole(unknownProvider, "unknownPriceExecutionCount").toULongLong() == 1);

    // A scope with only the unknown execution: the total itself is unknown, with no amount.
    fixture.addRow(unknownProvider, unknownModel, 500, 50, 0, false, "run-only-unknown");
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-only-unknown")));
    assert(fixture.controller.priceState() == QStringLiteral("unknown"));
    assert(fixture.controller.costText().isEmpty());
    assert(fixture.controller.priceIncomplete());

    // A scope with only the known execution: a plain amount; $100 and above use 2 decimals.
    fixture.addRow(knownProvider, knownModel, 1'000, 100, 250'000'000, true, "run-only-known");
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-only-known")));
    assert(fixture.controller.priceState() == QStringLiteral("known"));
    assert(fixture.controller.costText() == QStringLiteral("$250.00"));
    assert(!fixture.controller.priceIncomplete());
    assert(fixture.controller.unknownPriceExecutionCount() == 0);

    // No execution at all: ordinary absence of data, not an unknown price.
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-empty")));
    assert(fixture.controller.priceState() == QStringLiteral("none"));
    assert(fixture.controller.executionCount() == 0);
    assert(fixture.controller.costText().isEmpty());
    assert(!fixture.controller.priceIncomplete());
    assert(fixture.controller.providers()->rowCount() == 0);

    // clear() resets the state too.
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-m2c")));
    fixture.controller.clear();
    assert(fixture.controller.priceState() == QStringLiteral("none"));
}

// The chart compares providers only between EXACT amounts: with any partial/unknown provider no
// share is shown for any row, and the lower bounds are never normalised against each other.
void testAnalyticsChartSharesExistOnlyBetweenExactAmounts()
{
    AnalyticsStateFixture fixture;
    const QString bigProvider = fixture.addProvider("Big provider");
    const QString bigModel = fixture.addModel(bigProvider, "big-model");
    const QString smallProvider = fixture.addProvider("Small provider");
    const QString smallModel = fixture.addModel(smallProvider, "small-model");
    const QString partialProvider = fixture.addProvider("Partial provider");
    const QString partialModel = fixture.addModel(partialProvider, "partial-model");

    // Two exact providers: the largest is 1, the other is the exact ratio.
    fixture.addRow(bigProvider, bigModel, 1'000, 100, 1'000'000, true);
    fixture.addRow(smallProvider, smallModel, 1'000, 100, 250'000, true);
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-m2c")));
    assert(fixture.providerRole(bigProvider, "shareBasis").toString() == QStringLiteral("cost"));
    assert(fixture.providerRole(bigProvider, "costShare").toDouble() == 1.0);
    assert(fixture.providerRole(smallProvider, "costShare").toDouble() == 0.25);

    // One more provider with an incomplete price: nobody gets a share.
    fixture.addRow(partialProvider, partialModel, 1'000, 100, 900'000, false);
    assert(fixture.controller.refreshSource(QStringLiteral("run"), QStringLiteral("run-m2c")));
    for (const QString& provider : {bigProvider, smallProvider, partialProvider}) {
        assert(fixture.providerRole(provider, "shareBasis").toString() == QStringLiteral("incomplete"));
        assert(fixture.providerRole(provider, "costShare").toDouble() == 0.0);
    }
}

QString readQmlSource(const char* relativePath)
{
    const std::filesystem::path path =
        std::filesystem::path(AI_ORCHESTRATOR_QML_SOURCE_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    assert(stream.is_open() && "Could not open a QML source for a presentation contract");
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return QString::fromStdString(buffer.str());
}

// Static proof: the Overview words the four states itself, never renders an unknown price as an
// amount, never reads or formats a raw cost, and draws no share bar for incomplete pricing.
void testOverviewQmlWordsPriceStatesAndNeverReadsRawCost()
{
    const QString overview = readQmlSource("pages/Overview/OverviewPage.qml");
    const auto functionStart = overview.indexOf(QStringLiteral("function costWording(priceState, costText)"));
    assert(functionStart >= 0);
    const auto functionEnd = overview.indexOf(QStringLiteral("function "), functionStart + 10);
    assert(functionEnd > functionStart);
    const QString wording = overview.mid(functionStart, functionEnd - functionStart);
    assert(wording.contains(QStringLiteral("priceState === \"known\""))
        && wording.contains(QStringLiteral("priceState === \"partial\""))
        && wording.contains(QStringLiteral("priceState === \"unknown\"")));
    assert(wording.contains(QStringLiteral("qsTr(\"At least %1\")")));
    assert(wording.contains(QStringLiteral("qsTr(\"Cost unavailable\")")));
    // The unknown branch never touches the amount text.
    const auto unknownBranch = wording.indexOf(QStringLiteral("priceState === \"unknown\""));
    const auto unavailable = wording.indexOf(QStringLiteral("Cost unavailable"), unknownBranch);
    assert(unavailable > unknownBranch);
    assert(!wording.mid(unknownBranch, unavailable - unknownBranch).contains(QStringLiteral("costText")));

    assert(!overview.contains(QStringLiteral("costMicros"))
        && "QML must never read raw cost: the Qt layer projects state and amount text");
    assert(!overview.contains(QStringLiteral("formatCost"))
        && !overview.contains(QStringLiteral("Pricing not set"))
        && !overview.contains(QStringLiteral("Not configured")));
    assert(overview.contains(QStringLiteral("page.costWording(page.backend.executionAnalytics.priceState,")));
    assert(overview.contains(QStringLiteral("incomplete: page.backend.executionAnalytics.priceIncomplete")));

    const QString chart = readQmlSource("pages/Overview/Overview_Objects/OverviewProviderChart.qml");
    assert(chart.contains(QStringLiteral("visible: providerRow.modelData.shareBasis !== \"incomplete\"")));
    assert(chart.contains(QStringLiteral("visible: providerRow.modelData.shareBasis === \"incomplete\"")));
    assert(chart.contains(QStringLiteral("qsTr(\"Incomplete pricing\")")));
    assert(chart.contains(QStringLiteral("providerRow.modelData.priceIncomplete")));
    assert(!chart.contains(QStringLiteral("costMicros")));

    const QString popup = readQmlSource("pages/Overview/Overview_Objects/OverviewProviderDetailsPopup.qml");
    assert(popup.contains(QStringLiteral("modelRow.modelData.priceIncomplete")));
    assert(!popup.contains(QStringLiteral("costMicros")));
}

// Static proof for the catalog UI: the long-context fields are explicit, nothing is auto-filled,
// the hint is provider-neutral (GPT-6 Sol appears once, as a reference sentence, never in a
// condition), the Backend's own error text is shown and its rules are not duplicated in QML.
void testPricingDialogQmlHasExplicitTierFieldsNoAutofillAndNoModelSpecificLogic()
{
    const QString dialog = readQmlSource("pages/Providers/Providers_Objects/ModelPricingDialog.qml");
    assert(dialog.contains(QStringLiteral(
        "Long-context prices apply when a single Response reports more than this number of input tokens.")));
    assert(dialog.contains(QStringLiteral("Reference only, never applied automatically: GPT-6 Sol")));
    assert(dialog.count(QStringLiteral("gpt"), Qt::CaseInsensitive) == 1
        && "the model name may appear only in the reference sentence, never in logic");
    assert(dialog.count(QStringLiteral("272")) == 1
        && "the reference threshold may appear only in the reference sentence, never as a value");
    for (const char* field : {"longContextThresholdField", "longContextInputPriceField",
             "longContextOutputPriceField", "longContextCacheWritePriceField",
             "longContextCacheReadPriceField", "longContextCacheWriteOneHourPriceField",
             "longContextCheck", "cacheWriteOneHourPriceField"}) {
        assert(dialog.contains(QStringLiteral("objectName: \"%1\"").arg(QLatin1String(field))));
    }
    // A model without a tier leaves the tier fields EMPTY - they are filled from the Backend only.
    assert(dialog.contains(QStringLiteral("const tierEnabled = Boolean(modelData.longContextEnabled)")));
    assert(dialog.contains(QStringLiteral(": \"\"")));
    assert(!dialog.contains(QStringLiteral("text = \"")));

    const QString page = readQmlSource("pages/Providers/ProvidersPage.qml");
    assert(page.contains(QStringLiteral(
        "backend.providerCatalog.saveModelPricingWithCacheDuration(")));
    assert(page.contains(QStringLiteral(
        "function saveModelPricing(modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,")));
    assert(page.contains(QStringLiteral("function saveModelPricingWithCacheDuration(")));
    assert(page.contains(QStringLiteral(
        "page.saveModelPricingWithCacheDuration(")));
    assert(page.contains(QStringLiteral("backend.providerCatalog.saveModelPricing(")));
    assert(page.contains(QStringLiteral("pricingDialog.errorText = backend.providerCatalog.lastError")));
    for (const QString* source : {&dialog, &page}) {
        // The Backend's rule texts are shown verbatim, never re-implemented here.
        assert(!source->contains(QStringLiteral("strictly below")));
        assert(!source->contains(QStringLiteral("require a long-context threshold")));
        assert(!source->contains(QStringLiteral("model.long_context")));
    }

    // The tier chip is data-driven: it follows longContextEnabled, no provider/model conditions.
    const QString panel = readQmlSource("pages/Providers/Providers_Objects/ProviderDetailsPanel.qml");
    assert(panel.contains(QStringLiteral("visible: modelRow.longContextEnabled")));
    assert(panel.contains(QStringLiteral("qsTr(\"Long-context tier\")")));
    assert(!panel.contains(QStringLiteral("gpt"), Qt::CaseInsensitive));
}

// Release version: Settings shows the version the executable was built as. The Backend bridge hands out exactly
// what main() put into QCoreApplication (nothing when it was not set), and the Settings row reads only that - no
// version number is spelled in QML, and "unavailable" is the answer for an empty version, not a placeholder number.
void testSettingsShowsTheApplicationVersionFromTheBridgeOnly()
{
    TempWorkspace workspace;
    auto bridgeOwner = std::make_unique<BackendBridge>(workspace.databasePathQt());
    const QString previous = QCoreApplication::applicationVersion();

    QCoreApplication::setApplicationVersion(QString());
    assert(bridgeOwner->applicationVersion().isEmpty());
    QCoreApplication::setApplicationVersion(QStringLiteral("7.8.9-test.2"));
    assert(bridgeOwner->applicationVersion() == QStringLiteral("7.8.9-test.2"));
    assert(bridgeOwner->property("applicationVersion").toString() == QStringLiteral("7.8.9-test.2"));
    QCoreApplication::setApplicationVersion(previous);

    const QString settings = readQmlSource("pages/Settings/SettingsPage.qml");
    assert(settings.contains(QStringLiteral("readonly property string versionText: page.backend.applicationVersion")));
    assert(settings.contains(QStringLiteral("versionText.length > 0 ? versionText : qsTr(\"Version information unavailable\")")));
    assert(settings.contains(QStringLiteral("objectName: \"applicationVersionText\"")));
    assert(!settings.contains(QStringLiteral("rc.1")) && !settings.contains(QStringLiteral("1.0.0")));
}

// AG-18.4B-R1: the Projects page's setup status, "Check again" and the user's choice of a preset pair go through the bridge only:
// the recorded state is read (nothing starts), an explicit check records its outcome durably, a choice is validated against the
// project, and the dialog's "Test configuration" answers with the real check's reason instead of a bare "confirmed".
void testProjectSetupStatusCheckAndChoiceGoThroughTheBridge()
{
    TempWorkspace workspace;
    BackendBridge bridge(workspace.databasePathQt());
    assert(bridge.storageAvailable());
    ProjectController& projects = bridge.projectController();
    const QString projectId = projects.createProject(
        QStringLiteral("AG-18.4B-R1 setup"), QStringLiteral("A project without CMake presets"), workspace.projectPathQt());
    assert(!projectId.isEmpty());
    ProjectVerificationSetupController& setup = bridge.projectVerificationSetupController();

    QVariantMap status = setup.projectSetupStatus(projectId);
    assert(status.value(QStringLiteral("exists")).toBool() == false);
    assert(status.value(QStringLiteral("candidates")).toStringList().isEmpty());

    // "Check again" on a project that has no preset: a typed, recorded reason; no cmake is even started.
    const quint64 checkId = setup.checkProjectSetup(projectId, workspace.projectPathQt());
    assert(checkId != 0);
    assert(setup.checkingProjectSetup());
    assert(setup.checkProjectSetup(projectId, workspace.projectPathQt()) == 0); // one check at a time
    QString finishedCode;
    bool finishedReady = true;
    quint64 finishedId = 0;
    QObject::connect(&setup, &ProjectVerificationSetupController::projectSetupCheckFinished, &setup,
        [&](quint64 id, const QString&, bool ready, const QString& code, const QString&) {
            finishedId = id;
            finishedReady = ready;
            finishedCode = code;
        });
    assert(ai::ui::test_support::waitForCondition("the setup check finishes", [&] { return !setup.checkingProjectSetup(); },
        [] { return std::string(); }, 10s));
    assert(ai::ui::test_support::waitForCondition("the finish is announced", [&] { return finishedId == checkId; },
        [] { return std::string(); }, 4s));
    assert(!finishedReady && finishedCode == QStringLiteral("project_setup.preset_not_found"));
    status = setup.projectSetupStatus(projectId);
    assert(status.value(QStringLiteral("exists")).toBool());
    assert(status.value(QStringLiteral("state")).toString() == QStringLiteral("failed"));
    assert(status.value(QStringLiteral("failureCode")).toString() == QStringLiteral("project_setup.preset_not_found"));
    assert(status.value(QStringLiteral("failureMessage")).toString().contains(QStringLiteral("check again")));

    // The user's choice: refused when incomplete or for an unknown project, recorded (unverified) otherwise.
    assert(!setup.selectProjectSetupPair(projectId, workspace.projectPathQt(), QString(), QStringLiteral("b"))
                .value(QStringLiteral("success")).toBool());
    assert(!setup.selectProjectSetupPair(QStringLiteral("no-such-project"), workspace.projectPathQt(),
                QStringLiteral("a"), QStringLiteral("b")).value(QStringLiteral("success")).toBool());
    const QVariantMap chosen = setup.selectProjectSetupPair(
        projectId, workspace.projectPathQt(), QStringLiteral("local"), QStringLiteral("local-build"));
    assert(chosen.value(QStringLiteral("success")).toBool());
    status = setup.projectSetupStatus(projectId);
    assert(status.value(QStringLiteral("state")).toString() == QStringLiteral("unverified"));
    assert(status.value(QStringLiteral("configurePreset")).toString() == QStringLiteral("local"));
    assert(status.value(QStringLiteral("buildPreset")).toString() == QStringLiteral("local-build"));

    // The dialog's "Test configuration": a build preset is required (the check configures AND builds the pair), and the answer
    // is the check's own reason - never "configure preset confirmed" without a configure.
    QString probeCode;
    QString probeText;
    bool probeSuccess = true;
    quint64 probeFinishedId = 0;
    QObject::connect(&setup, &ProjectVerificationSetupController::testConfigurationFinished, &setup,
        [&](quint64 id, bool success, const QString&, const QString&, bool, bool, const QString& code, const QString& text) {
            probeFinishedId = id;
            probeSuccess = success;
            probeCode = code;
            probeText = text;
        });
    const quint64 probeId = setup.testExistingConfiguration(
        workspace.projectPathQt(), QStringLiteral("local"), QString());
    assert(probeId != 0);
    assert(ai::ui::test_support::waitForCondition("the probe finishes", [&] { return probeFinishedId == probeId; },
        [] { return std::string(); }, 10s));
    assert(!probeSuccess && probeCode == QStringLiteral("project_setup.preset_not_found")
        && probeText.contains(QStringLiteral("build preset")));
    const quint64 secondProbeId = setup.testExistingConfiguration(
        workspace.projectPathQt(), QStringLiteral("local"), QStringLiteral("local-build"));
    assert(secondProbeId != 0);
    assert(ai::ui::test_support::waitForCondition("the second probe finishes", [&] { return probeFinishedId == secondProbeId; },
        [] { return std::string(); }, 10s));
    assert(!probeSuccess && probeCode == QStringLiteral("project_setup.preset_not_found")
        && probeText.contains(QStringLiteral("preset")));

    // The panel only talks to the bridge: no process, no database, no provider.
    const QString panel = readQmlSource("pages/Projects/Projects_Objets/ProjectSetupPanel.qml");
    assert(panel.contains(QStringLiteral("backend.projectVerificationSetup.projectSetupStatus")));
    assert(panel.contains(QStringLiteral("backend.projectVerificationSetup.checkProjectSetup")));
    assert(panel.contains(QStringLiteral("backend.projectVerificationSetup.selectProjectSetupPair")));
    assert(!panel.contains(QStringLiteral("cmake")) && !panel.contains(QStringLiteral("sqlite")));
    const QString details = readQmlSource("pages/Projects/Projects_Objets/ProjectDetailsPanel.qml");
    assert(details.contains(QStringLiteral("ProjectSetupPanel {")));
}

} // namespace

// PC-3D: static proof that the Prompt Workshop UI only RENDERS the Backend's preview and RELAYS the user's
// choices. QML never reads a plan, never decides whether a reply holds one, and writes only by confirming the
// Backend's own preview token; New Run keeps an empty-roster plan draft a Manifest roster instead of Mock.
void testPromptWorkshopQmlOnlyRendersAndRelaysTheBackendsPreview()
{
    const QString delegate = readQmlSource("pages/Chats/Chat_Objects/ChatMessageDelegate.qml");
    const QString dialog = readQmlSource("pages/Chats/Chat_Objects/NextRunPlanDialog.qml");
    const QString chatsPage = readQmlSource("pages/Chats/ChatsPage.qml");
    const QString header = readQmlSource("pages/Chats/Chat_Objects/ChatHeader.qml");
    const QString drafts = readQmlSource("pages/Runs/Runs_Objects/DraftsDialog.qml");
    const QString newRun = readQmlSource("pages/Runs/Runs_Objects/NewRunDialog.qml").replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QString rosterSetup = readQmlSource("pages/Runs/Runs_Objects/RosterSetupSection.qml");
    const QString rosterMember = readQmlSource("pages/Runs/Runs_Objects/RosterMemberRow.qml");
    const QString runsPage = readQmlSource("pages/Runs/RunsPage.qml");
    const QString projections = readQmlSource("pages/Runs/Runs_Objects/RunModelSnapshots.qml");
    const QString pageHost = readQmlSource("pages/PageHost.qml");

    // The per-message action: the Backend decides, QML only looks at its answer. The reply text is never scanned.
    assert(delegate.contains(QStringLiteral("required property string planEnvelope"))
        && delegate.contains(QStringLiteral("delegateRoot.isAssistant && delegateRoot.planEnvelope === \"one\"")));
    assert(!delegate.contains(QStringLiteral("next-run-plan")) && !delegate.contains(QStringLiteral("JSON."))
        && !delegate.contains(QStringLiteral("content.indexOf")) && !delegate.contains(QStringLiteral("content.match"))
        && !delegate.contains(QStringLiteral("content.includes")) && !delegate.contains(QStringLiteral("content.search"))
        && "QML must not look for a plan in a message");
    // No QML file parses a plan or asks for a direct write.
    for (const QString* source : {&delegate, &dialog, &chatsPage, &header, &drafts, &newRun,
                                  &rosterSetup, &rosterMember, &runsPage, &projections, &pageHost}) {
        assert(!source->contains(QStringLiteral("createFromMessage"))
            && "There is no write that does not go through a preview token");
        assert(!source->contains(QStringLiteral("JSON.parse")) && "QML does not parse a plan");
    }
    assert(!dialog.contains(QStringLiteral("JSON.")) && !dialog.contains(QStringLiteral("schemaVersion")));

    // The one write: only the preview's own token, an optional template draft and optional source Runs.
    assert(dialog.contains(QStringLiteral(
        "signal confirmRequested(string previewToken, string templateDraftId, var sourceRunIds, bool applySuggestedParallelism)")));
    // 6a (a): the staffing hint is rendered from the Backend's map; the copy tick exists only where the host switches it on (the
    // follow-up page), never in the Prompt Workshop, and it can only ever pass a boolean.
    assert(dialog.contains(QStringLiteral("nextRunPlanApplyParallelismCheck"))
        && dialog.contains(QStringLiteral("property bool offersSuggestedParallelism: false"))
        && dialog.contains(QStringLiteral("card.modelData.staffing.summary"))
        && dialog.contains(QStringLiteral("dialog.chosenTemplateWorkerCount()"))
        && !chatsPage.contains(QStringLiteral("offersSuggestedParallelism"))
        && "The Prompt Workshop never offers to copy the suggested parallelism");
    assert(dialog.contains(QStringLiteral("dialog.confirmRequested(String(dialog.preview.previewToken)")));
    assert(chatsPage.contains(QStringLiteral("backend.promptWorkshop.createFromPreview(previewToken, templateDraftId, sourceRunIds)")));
    assert(chatsPage.contains(QStringLiteral("backend.promptWorkshop.previewFromMessage("))
        && chatsPage.contains(QStringLiteral("backend.promptWorkshop.previewFromPaste(")));
    // A preview that is no longer good can only be repeated, never confirmed.
    assert(chatsPage.contains(QStringLiteral("run_draft_plan.stale_preview"))
        && chatsPage.contains(QStringLiteral("run_draft_plan.preview_expired"))
        && dialog.contains(QStringLiteral("offerPreviewAgain")));
    // The preview shows tasks and contracts in full and selectable, and nothing there is editable.
    assert(dialog.count(QStringLiteral("readOnly: true")) >= 2 && dialog.count(QStringLiteral("selectByMouse: true")) >= 3);
    assert(dialog.contains(QStringLiteral("Nothing starts from here")));
    // The header action needs a project; the dialog is hosted by the page.
    assert(header.contains(QStringLiteral("enabled: header.canImportPlan"))
        && chatsPage.contains(QStringLiteral("canImportPlan: page.selectedProjectId.length > 0")));
    // Navigation to the drafts goes through the host, like every other cross-page request.
    assert(chatsPage.contains(QStringLiteral("signal runDraftsRequested()"))
        && pageHost.contains(QStringLiteral("runs.openDrafts()")));

    // The Drafts list draws the Backend's projection of a plan draft.
    for (const char* badge : {"draftGroupBadge", "draftOriginBadge", "draftDependencyBadge", "draftSourceRunsBadge", "draftEditedBadge"}) {
        assert(drafts.contains(QStringLiteral("objectName: \"%1\"").arg(QLatin1String(badge))));
    }
    assert(drafts.contains(QStringLiteral("draftRow.modelData.edited")) && drafts.contains(QStringLiteral("draftRow.modelData.dependsOnTitles")));
    // A draft keeps its own number after a sibling is deleted: "Draft #n" and how many remain, never "n of m".
    assert(drafts.contains(QStringLiteral("Draft #%2")) && drafts.contains(QStringLiteral("draft remaining"))
        && !drafts.contains(QStringLiteral(" of %3")));
    // A picker that fails to load is shown with a retry, never taken for an empty choice.
    assert(chatsPage.contains(QStringLiteral("templates.ok ? templates.templates : []"))
        && chatsPage.contains(QStringLiteral("runs.ok ? runs.runs : []"))
        && chatsPage.contains(QStringLiteral("planDialog.choiceErrors = problems"))
        && chatsPage.contains(QStringLiteral("onReloadChoicesRequested: page.loadPlanChoices()"))
        && dialog.contains(QStringLiteral("signal reloadChoicesRequested()"))
        && dialog.contains(QStringLiteral("nextRunPlanChoiceError")));
    assert(projections.contains(QStringLiteral("required property bool edited")) && projections.contains(QStringLiteral("edited: it.edited")));

    // New Run: an empty-roster per-Run draft stays a real-provider Manifest roster (its contracts stay visible);
    // it never switches to Mock, and it cannot start before the models are chosen.
    const qsizetype emptyBranch = newRun.indexOf(QStringLiteral("if (roster.length === 0) {"));
    assert(emptyBranch >= 0);
    const qsizetype emptyBranchEnd = newRun.indexOf(QStringLiteral("return\n        }"), emptyBranch);
    assert(emptyBranchEnd > emptyBranch);
    const QString emptyBranchText = newRun.mid(emptyBranch, emptyBranchEnd - emptyBranch);
    assert(!emptyBranchText.contains(QStringLiteral("productionCheck.checked = false"))
        && !emptyBranchText.contains(QStringLiteral("customRosterCheck.checked = false"))
        && !emptyBranchText.contains(QStringLiteral("rosterSection.manifestChecked = false"))
        && "An empty-roster draft must not be switched to Mock");
    assert(emptyBranchText.contains(QStringLiteral("productionCheck.checked = true"))
        && emptyBranchText.contains(QStringLiteral("customRosterCheck.checked = true"))
        && emptyBranchText.contains(QStringLiteral("\"modelChoiceIndex\", -1"))
        && emptyBranchText.contains(QStringLiteral("rosterNeedsModels = true")));
    assert(rosterSetup.contains(QStringLiteral("objectName: \"newRunRosterSetupHint\""))
        && newRun.contains(QStringLiteral("Choose a model for the lead and for every worker before starting this Run.")));
    // A draft is written back in place from the same fields as a new one; the page owns the Backend call.
    assert(newRun.contains(QStringLiteral("signal updateDraftRequested("))
        && runsPage.contains(QStringLiteral("page.backend.runDrafts.updateDraft(")));
}

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();
    QCoreApplication application(argc, argv);
    bool stressConfigured = false;
    const int stressIterations = qEnvironmentVariableIntValue(
        "AI_ORCHESTRATOR_B6_STRESS_ITERATIONS", &stressConfigured);
    if (stressConfigured && stressIterations > 0) {
        testSelectionAndCancellationRace(stressIterations);
        return 0;
    }
    testSingleSessionLifecycle();
    testPersistenceAcrossCompositionRootRestart();
    testManualHandoffCommandCompletionDoesNotPolluteNewRunSelection();
    testManualHandoffReviewMemberRealApplyEndToEnd();
    testManualHandoffResetGuardsNoRunAndNoHandoff();
    testManualHandoffResetNonEditableChangesRealCandidateEndToEnd();
    testRecoveryOfPersistedUnfinishedRun();
    testManualHandoffAttemptRunIsHiddenFromTopLevelRunModel();
    testTaskLifecycleAndPersistence();
    testRunListModelPublishesOnlyChangedRoles();
    testProjectListModelShowsRootPathsTheAnsiCodePageCannotHold();
    testRunWarningListModelProjectsRowsAndHidesAMissingStage();
    testVerificationFolderSettingIsValidatedByTheBackendAndSurvivesARestart();
    testRunWarningsReachTheRunDetailsModelAndSurviveARestart();
    testRunListModelPublishesTheConversionMarkerAndRefusesAMisalignedProjection();
    testRunListModelExposesTheFrozenInterfaceContracts();
    testRunContractScopesSurviveTheEditorTextRoundTrip();
    testRunContractUnknownKindIsRefusedAtTheQtBoundary();
    testRunListModelShowsAnAutomaticallyRaisedTurnLimit();
    testRunListModelProjectsNonCacheInputTokensFromBackendBreakdown();
    testUnrelatedRunRowUnaffectedByAnotherRunsEvent();
    testSelectionAndCancellationRace(3);
    testApproveAndApplyRequiresApprovalBeforeWritingFiles();
    testRejectProposalBlocksApproveAndApply();
    testRejectProposalBlocksApplyEvenWithPriorApproval();
    testProposalLineageRolesLinkPredecessorAndSuccessor();
    testProjectMemoryControllerViewerCompactionValidationAndClear();
    testEligibleManifestProposalProjectsApplyAllowedTrue();
    testMissingVerificationProjectsApplyAllowedFalse();
    testFailedRequiredVerificationProjectsApplyAllowedFalse();
    testContentHashMismatchProjectsApplyAllowedFalse();
    testTerminalStatesProjectApplyAllowedFalse();
    testBackendApplyRejectionHandledGracefully();
    testSuccessfulApplyUpdatesSelectedProposalDetailInPlace();
    testManifestControllerActionRequiredShowsActiveRetryAndCreatesLinkedRun();
    testManifestControllerRetrySurfacesSetupStillRequiredWithoutCreatingRun();
    testManifestControllerSurfacesReadModelCorruptionError();
    testManifestControllerProjectsVerificationAdvisories();
    testManifestControllerProjectsRosterRoutingAndUnusedWorkers();
    testManifestControllerProjectsMemberUsageAndTheUnattributedDebt();
    testManifestControllerNeverRendersAnUnknownPriceAsZero();
    testManifestControllerShowsIncompletePricingAsALowerBound();
    testManifestControllerExposesDiagnosticsThatAreNotAttributionDebt();
    testManifestRosterRunSelectsTheManifestWorkflowAndReopensAsAManifestRoster();
    testLegacyConversionControllerShowsThePreviewAndConfirmsOnlyByFingerprint();
    testLegacyConversionDialogQmlOnlyRendersThePreviewAndSendsTheOpaqueFingerprint();
    testLegacyConversionRecordQmlOnlyRendersTheStoredAudit();
    testManifestPanelQmlPresentsUnusedWorkersWithoutWarning();
    testManifestPanelQmlWordsPriceStatesAndNeverComputesCost();
    testRunChoicesFollowCatalogChangesWithoutRestart();
    testCatalogChangesRefreshChoicesOnceWithoutRefreshingSettings();
    testCatalogControllerCreatesAFlatModelWithAnEmptyTier();
    testCatalogControllerSavesThePricingFormWithTheTierInOneUpdate();
    testCatalogEditsThatDoNotNameTheTierPreserveIt();
    testCatalogControllerRemovesTheTierOnlyExplicitly();
    testCatalogControllerAcceptsPartiallyEnteredLongContextPrices();
    testCatalogControllerShowsBackendValidationErrorsAndChangesNothing();
    testCatalogControllerCreatesCatalogModelsWithCachePricesAndTheTier();
    testSettingsShowsTheApplicationVersionFromTheBridgeOnly();
    testProjectSetupStatusCheckAndChoiceGoThroughTheBridge();
    testAnalyticsControllerProjectsTheFourPriceStatesAtEveryLevel();
    testAnalyticsChartSharesExistOnlyBetweenExactAmounts();
    testOverviewQmlWordsPriceStatesAndNeverReadsRawCost();
    testPricingDialogQmlHasExplicitTierFieldsNoAutofillAndNoModelSpecificLogic();
    testManifestPanelQmlNeverLabelsRetryAsCreateSetupRun();
    testManifestPanelContinuationActionUsesBackendEligibilityOnly();
    testTheFollowUpPlannerQmlCannotStartAnythingAndTheRelabelIsInPlace();
    testPromptWorkshopQmlOnlyRendersAndRelaysTheBackendsPreview();
    testManualHandoffPanelUsesBackendProjectionOnly();
    return 0;
}
