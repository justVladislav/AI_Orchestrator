#include <ai/ui/BackendBridge.hpp>

#include <QStandardPaths>

std::filesystem::path BackendBridge::resolveDatabasePath(const QString& databasePathOverride)
{
    if (!databasePathOverride.isEmpty()) {
        return std::filesystem::path(databasePathOverride.toStdWString());
    }
    const QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return std::filesystem::path(appDataDir.toStdWString()) / "orchestrator.sqlite";
}

BackendBridge::BackendBridge(QObject* parent)
    : BackendBridge(QString(), parent)
{
}

BackendBridge::BackendBridge(const QString& databasePathOverride, QObject* parent)
    : QObject(parent)
    , runtime_(ai::backend::BackendRuntimeConfig {resolveDatabasePath(databasePathOverride)})
    , workspaceContextController_(runtime_.workspaceContextService(), this)
    , providerCatalogController_(runtime_.providerCatalogService(), this)
    , executionProfileController_(runtime_.executionProfileService(), this)
    , credentialController_(
          runtime_.credentialManagementService(), providerCatalogController_, this)
    , chatController_(runtime_.chatService(), runtime_.providerCatalogService(), this)
    , projectController_(runtime_.projectService(), this)
    , runController_(runtime_.orchestrationService(), runtime_.artifactService(), this)
    , comparisonController_(
          runtime_.comparisonService(),
          runtime_.projectService(),
          runtime_.providerCatalogService(),
          this)
{
    connect(
        &projectController_, &ProjectController::projectArchived,
        &workspaceContextController_, &WorkspaceContextController::refresh);
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

QObject* BackendBridge::projects()         noexcept { return &projectController_; }
QObject* BackendBridge::runs()             noexcept { return &runController_; }
QObject* BackendBridge::workspace()        noexcept { return &workspaceContextController_; }
QObject* BackendBridge::providerCatalog()  noexcept { return &providerCatalogController_; }
QObject* BackendBridge::executionProfiles()noexcept { return &executionProfileController_; }
QObject* BackendBridge::credentials()      noexcept { return &credentialController_; }
QObject* BackendBridge::chats()            noexcept { return &chatController_; }
QObject* BackendBridge::comparison()       noexcept { return &comparisonController_; }

ProjectController& BackendBridge::projectController() noexcept { return projectController_; }
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
CredentialController& BackendBridge::credentialController() noexcept
{
    return credentialController_;
}
ChatController& BackendBridge::chatController() noexcept { return chatController_; }
ComparisonController& BackendBridge::comparisonController() noexcept
{
    return comparisonController_;
}
bool BackendBridge::storageAvailable() const noexcept { return runtime_.storageAvailable(); }
