#include <ai/ui/WorkspaceContextController.hpp>

#include <utility>

WorkspaceContextController::WorkspaceContextController(
    ai::application::WorkspaceContextService& service,
    QObject* parent)
    : QObject(parent)
    , service_(service)
{
    // Restore the persisted selection before QML reads the adapter for the
    // first time. BackendRuntime has already completed its startup work.
    refresh();
}

QString WorkspaceContextController::activeProjectId() const { return activeProjectId_; }
QString WorkspaceContextController::lastError() const { return lastError_; }

bool WorkspaceContextController::selectProject(const QString& projectId)
{
    const auto result = service_.selectProject(projectId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }

    clearError();
    setActiveProjectId(projectId);
    return true;
}

bool WorkspaceContextController::clearActiveProject()
{
    const auto result = service_.clearActiveProject();
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }

    clearError();
    setActiveProjectId({});
    return true;
}

void WorkspaceContextController::refresh()
{
    const auto result = service_.activeProjectId();
    if (!result.success()) {
        reportError(*result.error());
        return;
    }

    clearError();
    const auto& projectId = result.value();
    setActiveProjectId(projectId.has_value()
            ? QString::fromStdString(*projectId)
            : QString {});
}

void WorkspaceContextController::setActiveProjectId(QString projectId)
{
    if (activeProjectId_ == projectId) {
        return;
    }
    activeProjectId_ = std::move(projectId);
    emit activeProjectIdChanged();
}

void WorkspaceContextController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void WorkspaceContextController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
