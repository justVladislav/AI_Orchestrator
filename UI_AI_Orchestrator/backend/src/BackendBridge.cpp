#include <ai/ui/BackendBridge.hpp>

#include <QCoreApplication>
#include <QStandardPaths>

#include <utility>

std::filesystem::path BackendBridge::resolveDatabasePath(const QString& databasePathOverride)
{
    if (!databasePathOverride.isEmpty()) {
        // Tests pass an isolated path explicitly; production callers leave it
        // empty and use the stable per-user application data location below.
        return std::filesystem::path(databasePathOverride.toStdWString());
    }

    // Path resolution is a desktop concern. Opening, directory creation,
    // migrations, repositories, and failure handling all belong to the
    // backend-owned Runtime and remain invisible to this Qt adapter.
    const QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return std::filesystem::path(appDataDir.toStdWString()) / "orchestrator.sqlite";
}

BackendBridge::BackendBridge(QObject* parent)
    : BackendBridge(QString(), parent)
{
}

BackendBridge::BackendBridge(const QString& databasePathOverride, QObject* parent)
    : BackendBridge(databasePathOverride, nullptr, parent)
{
}

BackendBridge::BackendBridge(const QString& databasePathOverride,
    std::shared_ptr<ai::application::IMockWorkflowGate> mockWorkflowGate, QObject* parent)
    : QObject(parent)
    // Runtime must be initialized before every controller that references it.
    // enableAgentToolExecution defaults to false in BackendRuntimeConfig
    // (see its own doc comment: "Set to true only ... in future production
    // compositions that have passed the full workspace-security audit").
    // Turned on here for the desktop composition now that Stage 1-7 have
    // shipped and tested that audit: WorkspacePathGuard adversarial path
    // tests, FilesystemWorkspaceToolExecutor's read-only-only tool surface,
    // secret scanning before any content leaves the workspace, and the
    // fail-closed AgentToolExecutionService storage gate itself.
    , runtime_(ai::backend::BackendRuntimeConfig {
          .databasePath = resolveDatabasePath(databasePathOverride),
          .enableAgentToolExecution = true,
          .mockWorkflowGate = std::move(mockWorkflowGate)})
    , settingsController_(runtime_.applicationSettingsService(),
          runtime_.executionProfileService(), runtime_.providerCatalogService(),
          runtime_.verificationScratchService(), this)
    // BackendRuntime performs startup recovery before its constructor returns,
    // so every controller's constructor-time refresh sees recovered state.
    , workspaceContextController_(runtime_.workspaceContextService(), this)
    , providerCatalogController_(runtime_.providerCatalogService(), this)
    , executionProfileController_(runtime_.executionProfileService(), this)
    , runDraftController_(runtime_.runDraftService(), this)
    , credentialController_(
          runtime_.credentialManagementService(), providerCatalogController_, this)
    , executionAnalyticsController_(
          runtime_.executionAnalyticsService(), runtime_.providerCatalogService(), this)
    , comparisonController_(
          runtime_.comparisonService(), runtime_.comparisonReadModelService(),
          runtime_.providerCatalogService(),
          runtime_.comparisonExecutionManager(), this)
    , agentSessionController_(
          runtime_.agentToolReadModelService(), runtime_.changeProposalService(),
          runtime_.changeSetApplyService(), runtime_.proposalReviewService(),
          runtime_.manifestApplyVerificationGate(), this)
    , manifestController_(
          runtime_.manifestReadModelService(), runtime_.verificationBootstrapRetryService(),
          runtime_.manifestVerificationContinuationReadModelService(),
          runtime_.manifestVerificationContinuationPreparationService(),
          runtime_.manifestVerificationContinuationRepairService(), this)
    , manualHandoffController_(std::make_unique<ManifestVerificationManualHandoffController>(
          runtime_.manifestVerificationManualHandoffReadModelService(),
          runtime_.manifestVerificationManualHandoffService(),
          runtime_.changeProposalService(), runtime_.changeSetApplyService(),
          runtime_.proposalReviewService(), runtime_.manifestApplyVerificationGate(), this))
    , projectContextController_(std::make_unique<ProjectContextController>(
          runtime_.projectContextSnapshotService(), this))
    , chatController_(runtime_.chatService(), runtime_.providerCatalogService(), *projectContextController_, this)
    , projectMemoryController_(runtime_.projectMemoryService(),
          runtime_.projectMemoryCompactionService(), runtime_.providerCatalogService(), this)
    , projectController_(runtime_.projectService(), this)
    , projectVerificationSetupController_(
          std::make_unique<ProjectVerificationSetupController>(this))
    , promptWorkshopController_(std::make_unique<PromptWorkshopController>(
          runtime_.runDraftPlanService(), this))
    , outcomeBriefController_(std::make_unique<OutcomeBriefController>(runtime_.outcomeBriefReadModelService(), this))
    , followUpPlanController_(std::make_unique<FollowUpPlanController>(
          runtime_.followUpPlanService(), runtime_.followUpPlanReadModel(), this))
    , projectOutputApprovalController_(std::make_unique<ProjectOutputApprovalController>(
          runtime_.projectOutputApprovalService(), this))
    , taskController_(runtime_.taskService(), this)
    , runController_(runtime_.orchestrationService(), runtime_.artifactService(),
          runtime_.providerCatalogService(),
          runtime_.manifestVerificationManualHandoffReadModelService(), this)
{
    // AG-18.4B-R1: the selected project's durable verification readiness (status, "check again", the user's choice of a pair).
    projectVerificationSetupController_->setReadinessService(&runtime_.projectReadinessService());
    // PC-4C: brings the Outcome Briefs of older Runs up to date on a worker thread (bounded; it never delays the window and writes
    // only the brief repository). The runtime stops and joins it when it is destroyed.
    runtime_.startOutcomeBriefBackfill();

    // Archiving can invalidate the globally active project. Re-read through
    // WorkspaceContextService so its stale-reference self-healing is
    // immediately reflected in QML instead of waiting for an app restart.
    connect(
        &projectController_, &ProjectController::projectArchived,
        &workspaceContextController_, &WorkspaceContextController::refresh);
    connect(&projectController_, &ProjectController::projectCreated,
            &chatController_, &ChatController::notifyChoicesChanged);
    connect(&projectController_, &ProjectController::projectArchived,
            &chatController_, &ChatController::notifyChoicesChanged);
    connect(&projectController_, &ProjectController::projectRestored,
            &chatController_, &ChatController::notifyChoicesChanged);
    // A successfully completed run closes its linked open tasks, which also
    // removes them from the Overview Needs Attention panel.
    connect(&runController_, &RunController::runFinished,
            &taskController_, &TaskController::completeTasksForRun);
    // Run selection is application state, not a side effect of whether a
    // particular QML detail component happens to be visible. In particular,
    // a Prepared continuation child has no ordinary Manifest of its own yet;
    // gating ManifestPanel visibility on ManifestController state while QML
    // was also responsible for selecting that controller created a bootstrap
    // cycle and hid the paid-repair action. Synchronize both detail
    // controllers directly from the authoritative RunController selection.
    connect(&runController_, &RunController::selectedRunIdChanged, this, [this] {
        const QString runId = runController_.selectedRunId();
        // PC-4D: FIRST. The other controllers announce state changes while they switch (the agent session controller does so
        // synchronously), and the Outcome block listens to those announcements: if it were still on the PREVIOUS Run, the first one
        // would read that Run's brief with a synchronous SQLite lookup on the UI thread - exactly while the worker of the Run that has
        // just been started is writing. Switched first, the block is already empty and on the new Run, so those announcements read
        // nothing (it reads nothing while no block is shown). The read for the new Run follows later, see selectRunSoon().
        outcomeBriefController_->selectRunSoon(runId);
        // PC-5D: the follow-up status of the new Run is read later too (never synchronously while the worker of a Run that has just
        // been started is writing), and the previous Run's status is dropped at once.
        followUpPlanController_->selectRunSoon(runId);
        static_cast<void>(agentSessionController_.selectRun(runId));
        static_cast<void>(manifestController_.selectRun(runId));
        static_cast<void>(manualHandoffController_->selectRun(runId));
    });
    // PC-4D: the read-only Outcome block re-reads when something that can change the brief happens to the selected Run: it finishes,
    // a proposal is reviewed or applied, a handoff is created. The reconciler's own hook runs a moment AFTER these events, so the
    // controller reads now and once more shortly after. Progress events of a running Run deliberately do NOT read (a read is a
    // synchronous SQLite lookup on the UI thread that contends with the worker). It never writes and never asks the reconciler to run.
    connect(&runController_, &RunController::runFinished, this, [this](const QString& runId, const QString&) {
        outcomeBriefController_->onRunFinished(runId);
    });
    connect(&agentSessionController_, &AgentSessionController::stateChanged,
        outcomeBriefController_.get(), &OutcomeBriefController::scheduleRefresh);
    connect(manualHandoffController_.get(), &ManifestVerificationManualHandoffController::durableStateChanged,
        outcomeBriefController_.get(), [this](const QString&) { outcomeBriefController_->scheduleRefresh(); });
    // PC-5D: the follow-up block depends on the brief (the gate reads it), so it re-reads when the shown brief changes - and only then.
    connect(outcomeBriefController_.get(), &OutcomeBriefController::briefChanged,
        followUpPlanController_.get(), &FollowUpPlanController::refresh);
    // Drafts saved by a follow-up Confirm appear in the Drafts list without a restart (only a Confirm that really wrote them).
    connect(followUpPlanController_.get(), &FollowUpPlanController::draftsCreated,
        &runDraftController_, [this](const QString&, int) { runDraftController_.refresh(); });
    // Manual verification never rewrites the original Failed Run. Refresh
    // the separate Backend-owned resolution projection after every durable
    // handoff mutation (create/reverify/discard/review/apply), so RunsPage
    // updates immediately rather than only after an application restart.
    connect(manualHandoffController_.get(),
        &ManifestVerificationManualHandoffController::durableStateChanged,
        this, [this](const QString&) {
            runController_.refreshManualHandoffResolutions();
        });
    connect(&providerCatalogController_, &ProviderCatalogController::catalogChanged,
            &chatController_, &ChatController::notifyChoicesChanged);
    connect(&providerCatalogController_, &ProviderCatalogController::catalogChanged,
            &comparisonController_, &ComparisonController::notifyCatalogChanged);
    connect(&providerCatalogController_, &ProviderCatalogController::catalogChanged,
            &runController_, &RunController::refreshAgentChoices);
    connect(&providerCatalogController_, &ProviderCatalogController::catalogChanged,
            &settingsController_, &SettingsController::refreshChoices);
    connect(&executionProfileController_, &ExecutionProfileController::profilesChanged,
            &settingsController_, &SettingsController::refresh);
    // PC-3C: Drafts written by the Prompt Workshop appear in the Drafts list without a restart. Only a create that
    // really wrote drafts announces itself (a repeated create does not).
    connect(promptWorkshopController_.get(), &PromptWorkshopController::draftsCreated,
            &runDraftController_, [this](const QString&, int) { runDraftController_.refresh(); });
}

QString BackendBridge::applicationVersion() const
{
    return QCoreApplication::applicationVersion();
}

QString BackendBridge::status() const
{
    if (runtime_.storageError().has_value()) {
        return QStringLiteral("SQLite storage unavailable: %1")
            .arg(QString::fromStdString(runtime_.storageError()->message));
    }
    if (runtime_.recoveryError().has_value()) {
        return QStringLiteral("Application Layer and Qt Adapter are ready; startup recovery failed: %1")
            .arg(QString::fromStdString(runtime_.recoveryError()->message));
    }
    return QStringLiteral("Application Layer and Qt Adapter are ready");
}

QObject* BackendBridge::projects() noexcept { return &projectController_; }
QObject* BackendBridge::projectVerificationSetup() noexcept
{
    return projectVerificationSetupController_.get();
}
QObject* BackendBridge::projectOutputApprovals() noexcept { return projectOutputApprovalController_.get(); }
QObject* BackendBridge::tasks() noexcept { return &taskController_; }
QObject* BackendBridge::runs() noexcept { return &runController_; }
QObject* BackendBridge::workspace() noexcept { return &workspaceContextController_; }
QObject* BackendBridge::providerCatalog() noexcept { return &providerCatalogController_; }
QObject* BackendBridge::executionProfiles() noexcept { return &executionProfileController_; }
QObject* BackendBridge::runDrafts() noexcept { return &runDraftController_; }
QObject* BackendBridge::credentials() noexcept { return &credentialController_; }
QObject* BackendBridge::chats() noexcept { return &chatController_; }
QObject* BackendBridge::executionAnalytics() noexcept { return &executionAnalyticsController_; }
QObject* BackendBridge::comparisons() noexcept { return &comparisonController_; }
QObject* BackendBridge::agentSessions() noexcept { return &agentSessionController_; }
QObject* BackendBridge::manifest() noexcept { return &manifestController_; }
QObject* BackendBridge::manualHandoff() noexcept { return manualHandoffController_.get(); }
QObject* BackendBridge::outcomeBrief() noexcept { return outcomeBriefController_.get(); }
QObject* BackendBridge::settings() noexcept { return &settingsController_; }
QObject* BackendBridge::projectMemory() noexcept { return &projectMemoryController_; }

QObject* BackendBridge::projectContext() noexcept { return projectContextController_.get(); }
QObject* BackendBridge::promptWorkshop() noexcept { return promptWorkshopController_.get(); }
QObject* BackendBridge::followUpPlan() noexcept { return followUpPlanController_.get(); }
ProjectController& BackendBridge::projectController() noexcept { return projectController_; }
ProjectVerificationSetupController& BackendBridge::projectVerificationSetupController() noexcept
{
    return *projectVerificationSetupController_;
}
ProjectOutputApprovalController& BackendBridge::projectOutputApprovalController() noexcept
{
    return *projectOutputApprovalController_;
}
TaskController& BackendBridge::taskController() noexcept { return taskController_; }
RunController& BackendBridge::runController() noexcept { return runController_; }
WorkspaceContextController& BackendBridge::workspaceContextController() noexcept
{
    return workspaceContextController_;
}
ProviderCatalogController& BackendBridge::providerCatalogController() noexcept
{
    return providerCatalogController_;
}
ExecutionProfileController& BackendBridge::executionProfileController() noexcept
{
    return executionProfileController_;
}
RunDraftController& BackendBridge::runDraftController() noexcept
{
    return runDraftController_;
}
CredentialController& BackendBridge::credentialController() noexcept
{
    return credentialController_;
}
ChatController& BackendBridge::chatController() noexcept { return chatController_; }
ExecutionAnalyticsController& BackendBridge::executionAnalyticsController() noexcept
{
    return executionAnalyticsController_;
}
ComparisonController& BackendBridge::comparisonController() noexcept
{
    return comparisonController_;
}
AgentSessionController& BackendBridge::agentSessionController() noexcept
{
    return agentSessionController_;
}
ManifestController& BackendBridge::manifestController() noexcept
{
    return manifestController_;
}
ManifestVerificationManualHandoffController& BackendBridge::manualHandoffController() noexcept
{
    return *manualHandoffController_;
}
OutcomeBriefController& BackendBridge::outcomeBriefController() noexcept
{
    return *outcomeBriefController_;
}
SettingsController& BackendBridge::settingsController() noexcept
{
    return settingsController_;
}
ProjectMemoryController& BackendBridge::projectMemoryController() noexcept
{
    return projectMemoryController_;
}

ProjectContextController& BackendBridge::projectContextController() noexcept
{
    return *projectContextController_;
}

PromptWorkshopController& BackendBridge::promptWorkshopController() noexcept
{
    return *promptWorkshopController_;
}

FollowUpPlanController& BackendBridge::followUpPlanController() noexcept
{
    return *followUpPlanController_;
}

bool BackendBridge::storageAvailable() const noexcept { return runtime_.storageAvailable(); }
