#pragma once

#include <ai/backend/BackendRuntime.hpp>
#include <ai/ui/ComparisonController.hpp>
#include <ai/ui/CredentialController.hpp>
#include <ai/ui/ChatController.hpp>
#include <ai/ui/ExecutionProfileController.hpp>
#include <ai/ui/ProjectController.hpp>
#include <ai/ui/ProviderCatalogController.hpp>
#include <ai/ui/RunController.hpp>
#include <ai/ui/WorkspaceContextController.hpp>

#include <QObject>
#include <QString>

#include <filesystem>

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
    Q_PROPERTY(QObject* projects READ projects CONSTANT)
    Q_PROPERTY(QObject* runs READ runs CONSTANT)
    Q_PROPERTY(QObject* workspace READ workspace CONSTANT)
    Q_PROPERTY(QObject* providerCatalog READ providerCatalog CONSTANT)
    Q_PROPERTY(QObject* executionProfiles READ executionProfiles CONSTANT)
    Q_PROPERTY(QObject* credentials READ credentials CONSTANT)
    Q_PROPERTY(QObject* chats READ chats CONSTANT)
    Q_PROPERTY(QObject* comparison READ comparison CONSTANT)

public:
    explicit BackendBridge(QObject* parent = nullptr);
    explicit BackendBridge(const QString& databasePathOverride, QObject* parent = nullptr);

    [[nodiscard]] QString status() const;
    [[nodiscard]] QObject* projects() noexcept;
    [[nodiscard]] QObject* runs() noexcept;
    [[nodiscard]] QObject* workspace() noexcept;
    [[nodiscard]] QObject* providerCatalog() noexcept;
    [[nodiscard]] QObject* executionProfiles() noexcept;
    [[nodiscard]] QObject* credentials() noexcept;
    [[nodiscard]] QObject* chats() noexcept;
    [[nodiscard]] QObject* comparison() noexcept;

    // Typed accessors for C++ integration tests; QML uses the properties.
    [[nodiscard]] ProjectController& projectController() noexcept;
    [[nodiscard]] RunController& runController() noexcept;
    [[nodiscard]] WorkspaceContextController& workspaceContextController() noexcept;
    [[nodiscard]] ProviderCatalogController& providerCatalogController() noexcept;
    [[nodiscard]] ExecutionProfileController& executionProfileController() noexcept;
    [[nodiscard]] CredentialController& credentialController() noexcept;
    [[nodiscard]] ChatController& chatController() noexcept;
    [[nodiscard]] ComparisonController& comparisonController() noexcept;

    [[nodiscard]] bool storageAvailable() const noexcept;

private:
    [[nodiscard]] static std::filesystem::path resolveDatabasePath(const QString& databasePathOverride);

    // runtime_ is declared first and therefore destroyed last.
    ai::backend::BackendRuntime runtime_;
    WorkspaceContextController workspaceContextController_;
    ProviderCatalogController providerCatalogController_;
    ExecutionProfileController executionProfileController_;
    CredentialController credentialController_;
    ChatController chatController_;
    ProjectController projectController_;
    RunController runController_;
    ComparisonController comparisonController_;
};
