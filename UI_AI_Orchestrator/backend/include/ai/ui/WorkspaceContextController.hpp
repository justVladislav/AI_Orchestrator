#pragma once

#include <ai/application/projects/WorkspaceContextService.hpp>

#include <QObject>
#include <QString>

/**
 * Qt adapter for the Backend-owned application workspace selection.
 *
 * QML sees only a stable project ID and commands. Persistence, project
 * validation, and restart restoration remain inside WorkspaceContextService.
 */
class WorkspaceContextController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeProjectId READ activeProjectId NOTIFY activeProjectIdChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit WorkspaceContextController(
        ai::application::WorkspaceContextService& service,
        QObject* parent = nullptr);

    [[nodiscard]] QString activeProjectId() const;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE bool selectProject(const QString& projectId);
    Q_INVOKABLE bool clearActiveProject();
    Q_INVOKABLE void refresh();

signals:
    void activeProjectIdChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    void setActiveProjectId(QString projectId);
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::WorkspaceContextService& service_;
    QString activeProjectId_;
    QString lastError_;
};
