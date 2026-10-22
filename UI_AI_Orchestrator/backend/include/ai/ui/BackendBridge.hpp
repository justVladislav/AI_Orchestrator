#pragma once

#include <ai/backend/BackendRuntime.hpp>
#include <ai/ui/AgentSessionController.hpp>
#include <ai/ui/CredentialController.hpp>
#include <ai/ui/ChatController.hpp>
#include <ai/ui/ComparisonController.hpp>
#include <ai/ui/ExecutionProfileController.hpp>
#include <ai/ui/ExecutionAnalyticsController.hpp>
#include <ai/ui/FollowUpPlanController.hpp>
#include <ai/ui/ManifestController.hpp>
#include <ai/ui/ManifestVerificationManualHandoffController.hpp>
#include <ai/ui/OutcomeBriefController.hpp>
#include <ai/ui/ProjectContextController.hpp>
#include <ai/ui/ProjectController.hpp>
#include <ai/ui/ProjectMemoryController.hpp>
#include <ai/ui/ProjectVerificationSetupController.hpp>
#include <ai/ui/PromptWorkshopController.hpp>
#include <ai/ui/TaskController.hpp>
#include <ai/ui/ProviderCatalogController.hpp>
#include <ai/ui/RunController.hpp>
#include <ai/ui/RunDraftController.hpp>
#include <ai/ui/SettingsController.hpp>
#include <ai/ui/WorkspaceContextController.hpp>

#include <QObject>
#include <QString>

#include <filesystem>
#include <memory>

/**
 * Thin Qt adapter and the only root object injected into QML.
 *
 * BackendRuntime owns persistence, repositories, workers, and Application
 * Services. This class only resolves the desktop-specific data path, exposes
 * QObject controllers, and translates backend startup state to QString.
 */
class BackendBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString status READ status CONSTANT)
    // The version the executable was built as ("1.0.0-rc.1"): what main() set with
    // QCoreApplication::setApplicationVersion(). Empty when nothing set it (tests, embedding), and QML then
    // says so instead of inventing a number.
    Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)
    Q_PROPERTY(QObject* projects READ projects CONSTANT)
    Q_PROPERTY(QObject* projectVerificationSetup READ projectVerificationSetup CONSTANT)
    Q_PROPERTY(QObject* tasks READ tasks CONSTANT)
    Q_PROPERTY(QObject* runs READ runs CONSTANT)
    Q_PROPERTY(QObject* workspace READ workspace CONSTANT)
    Q_PROPERTY(QObject* providerCatalog READ providerCatalog CONSTANT)
    Q_PROPERTY(QObject* executionProfiles READ executionProfiles CONSTANT)
    Q_PROPERTY(QObject* runDrafts READ runDrafts CONSTANT)
    Q_PROPERTY(QObject* credentials READ credentials CONSTANT)
    Q_PROPERTY(QObject* chats READ chats CONSTANT)
    Q_PROPERTY(QObject* executionAnalytics READ executionAnalytics CONSTANT)
    Q_PROPERTY(QObject* comparisons READ comparisons CONSTANT)
    Q_PROPERTY(QObject* agentSessions READ agentSessions CONSTANT)
    Q_PROPERTY(QObject* manifest READ manifest CONSTANT)
    Q_PROPERTY(QObject* manualHandoff READ manualHandoff CONSTANT)
    Q_PROPERTY(QObject* outcomeBrief READ outcomeBrief CONSTANT)
    Q_PROPERTY(QObject* settings READ settings CONSTANT)
    Q_PROPERTY(QObject* projectMemory READ projectMemory CONSTANT)
    Q_PROPERTY(QObject* projectContext READ projectContext CONSTANT)
    Q_PROPERTY(QObject* promptWorkshop READ promptWorkshop CONSTANT)
    Q_PROPERTY(QObject* followUpPlan READ followUpPlan CONSTANT)

public:
    // Opens (or creates) the SQLite database at the default per-user
    // application data location. See resolveDatabasePath().
    explicit BackendBridge(QObject* parent = nullptr);
    // Opens (or creates) the SQLite database at an explicit path instead of
    // the default location. Tests use this so they read and write an
    // isolated, disposable database and never touch the real user's data.
    explicit BackendBridge(const QString& databasePathOverride, QObject* parent = nullptr);
    // TEST-ONLY composition: the same wiring, with a barrier the Mock workflow calls before every step
    // (ai::application::IMockWorkflowGate), so that a test can hold a Mock Run at a known step instead of racing its clock.
    // Nothing else of the runtime is configurable from here. `parent` is deliberately required, so that a call with two arguments
    // can only ever mean the constructor above; the constructors above pass no gate.
    BackendBridge(const QString& databasePathOverride,
        std::shared_ptr<ai::application::IMockWorkflowGate> mockWorkflowGate, QObject* parent);

    [[nodiscard]] QString status() const;
    [[nodiscard]] QString applicationVersion() const;
    [[nodiscard]] QObject* projects() noexcept;
    [[nodiscard]] QObject* projectVerificationSetup() noexcept;
    [[nodiscard]] QObject* tasks() noexcept;
    [[nodiscard]] QObject* runs() noexcept;
    [[nodiscard]] QObject* workspace() noexcept;
    [[nodiscard]] QObject* providerCatalog() noexcept;
    [[nodiscard]] QObject* executionProfiles() noexcept;
    [[nodiscard]] QObject* runDrafts() noexcept;
    [[nodiscard]] QObject* credentials() noexcept;
    [[nodiscard]] QObject* chats() noexcept;
    [[nodiscard]] QObject* executionAnalytics() noexcept;
    [[nodiscard]] QObject* comparisons() noexcept;
    [[nodiscard]] QObject* agentSessions() noexcept;
    [[nodiscard]] QObject* manifest() noexcept;
    [[nodiscard]] QObject* manualHandoff() noexcept;
    [[nodiscard]] QObject* outcomeBrief() noexcept;
    [[nodiscard]] QObject* settings() noexcept;
    [[nodiscard]] QObject* projectMemory() noexcept;
    [[nodiscard]] QObject* projectContext() noexcept;
    [[nodiscard]] QObject* promptWorkshop() noexcept;
    [[nodiscard]] QObject* followUpPlan() noexcept;

    // Typed accessors support C++ integration tests; QML uses the properties.
    [[nodiscard]] ProjectController& projectController() noexcept;
    [[nodiscard]] ProjectVerificationSetupController& projectVerificationSetupController() noexcept;
    [[nodiscard]] TaskController& taskController() noexcept;
    [[nodiscard]] RunController& runController() noexcept;
    [[nodiscard]] WorkspaceContextController& workspaceContextController() noexcept;
    [[nodiscard]] ProviderCatalogController& providerCatalogController() noexcept;
    [[nodiscard]] ExecutionProfileController& executionProfileController() noexcept;
    [[nodiscard]] RunDraftController& runDraftController() noexcept;
    [[nodiscard]] CredentialController& credentialController() noexcept;
    [[nodiscard]] ChatController& chatController() noexcept;
    [[nodiscard]] ExecutionAnalyticsController& executionAnalyticsController() noexcept;
    [[nodiscard]] ComparisonController& comparisonController() noexcept;
    [[nodiscard]] AgentSessionController& agentSessionController() noexcept;
    [[nodiscard]] ManifestController& manifestController() noexcept;
    [[nodiscard]] ManifestVerificationManualHandoffController& manualHandoffController() noexcept;
    [[nodiscard]] OutcomeBriefController& outcomeBriefController() noexcept;
    [[nodiscard]] SettingsController& settingsController() noexcept;
    [[nodiscard]] ProjectMemoryController& projectMemoryController() noexcept;
    [[nodiscard]] ProjectContextController& projectContextController() noexcept;
    [[nodiscard]] PromptWorkshopController& promptWorkshopController() noexcept;
    [[nodiscard]] FollowUpPlanController& followUpPlanController() noexcept;

    // False after a controlled storage-open/migration failure (see status()
    // for the reason). Not exposed to QML - only C++ tests need to assert
    // that persistence actually engaged rather than the safe-failure path.
    [[nodiscard]] bool storageAvailable() const noexcept;

private:
    [[nodiscard]] static std::filesystem::path resolveDatabasePath(const QString& databasePathOverride);

    // Runtime is declared before the controllers and therefore destroyed
    // after them. It owns every backend dependency the controllers reference,
    // including the worker thread and native SQLite persistence stack.
    ai::backend::BackendRuntime runtime_;
    SettingsController settingsController_;
    WorkspaceContextController workspaceContextController_;
    ProviderCatalogController providerCatalogController_;
    ExecutionProfileController executionProfileController_;
    RunDraftController runDraftController_;
    CredentialController credentialController_;
    ExecutionAnalyticsController executionAnalyticsController_;
    ComparisonController comparisonController_;
    AgentSessionController agentSessionController_;
    ManifestController manifestController_;
    // Heap-owned, unlike this class's other controller members: this
    // controller grew a whole second command surface (post-P4R.6 human
    // review/Apply hardening: reviewMembers/selectMember/
    // approveAndApplyMember/rejectMember, four additional held application-
    // service references) on top of its original Create/Reverify/Discard
    // shape - the same reasoning that already made
    // ProjectVerificationSetupController heap-owned in this class (see its
    // own member doc comment): BackendBridge is already a large, Windows-
    // stack-layout-sensitive object in several fixtures that construct it as
    // a plain stack-local variable.
    std::unique_ptr<ManifestVerificationManualHandoffController> manualHandoffController_;
    // Created before ChatController: it owns the in-memory reviewed snapshot that Chat may explicitly attach.
    std::unique_ptr<ProjectContextController> projectContextController_;
    ChatController chatController_;
    ProjectMemoryController projectMemoryController_;
    ProjectController projectController_;
    // Heap-owned, unlike this class's other controller members: BackendBridge
    // is already a large, Windows-stack-layout-sensitive object in several
    // fixtures that construct it as a plain stack-local variable (see
    // QtAdapterTests.cpp's own many `BackendBridge bridge(...)` locals, and
    // QML_PROJECT_CONTEXT.md's own account of the historical
    // stack-layout-sensitive destructor crash this exact pattern caused
    // before). Adding this controller as a plain stack-resident member
    // reproduced that crash (AIOrchestrator_QtAdapter_tests segfaulting
    // immediately on construction); a single pointer-sized member does not.
    std::unique_ptr<ProjectVerificationSetupController> projectVerificationSetupController_;
    // PC-3C: heap-owned like the controllers above (BackendBridge is stack-layout sensitive). Holds a reference to
    // the runtime's RunDraftPlanService only; declared after runtime_, so it is destroyed before it.
    std::unique_ptr<PromptWorkshopController> promptWorkshopController_;
    // PC-4D: heap-owned like the controllers above (BackendBridge is stack-layout sensitive). Holds a const reference to the
    // runtime's OutcomeBriefReadModelService only; declared after runtime_, so it is destroyed before it.
    std::unique_ptr<OutcomeBriefController> outcomeBriefController_;
    // PC-5D: heap-owned like the controllers above (BackendBridge is stack-layout sensitive). Holds the runtime's FollowUpPlanService
    // and FollowUpPlanReadModel only; declared after runtime_, so it is destroyed before it (and waits for a running call).
    std::unique_ptr<FollowUpPlanController> followUpPlanController_;
    TaskController taskController_;
    RunController runController_;
};
