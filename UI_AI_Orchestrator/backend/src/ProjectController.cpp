#include <ai/ui/ProjectController.hpp>

#include <filesystem>

ProjectController::ProjectController(ai::application::ProjectService& service, QObject* parent)
    : QObject(parent)
    , service_(service)
    , model_(this)
{
    refresh();
}

QAbstractItemModel* ProjectController::model() noexcept { return &model_; }
QString ProjectController::lastError() const { return lastError_; }

QString ProjectController::createProject(
    const QString& name,
    const QString& description,
    const QString& rootPath)
{
    const auto result = service_.createProject({
        name.toStdString(),
        description.toStdString(),
        std::filesystem::path(rootPath.toStdWString())
    });
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }

    clearError();
    refresh();
    const QString projectId = QString::fromStdString(result.value().id);
    emit projectCreated(projectId);
    return projectId;
}

bool ProjectController::archiveProject(const QString& projectId)
{
    const auto result = service_.archiveProject(projectId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    emit projectArchived(projectId);
    return true;
}

bool ProjectController::restoreProject(const QString& projectId)
{
    const auto result = service_.restoreProject(projectId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    emit projectRestored(projectId);
    return true;
}

bool ProjectController::updateProjectPath(const QString& projectId, const QString& rootPath)
{
    const auto result = service_.updateProjectPath(
        projectId.toStdString(), std::filesystem::path(rootPath.toStdWString()));
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProjectController::updateProjectDescription(
    const QString& projectId, const QString& description)
{
    const auto result = service_.updateProjectDescription(
        projectId.toStdString(), description.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

void ProjectController::refresh()
{
    const auto result = service_.listProjects();
    if (!result.success()) {
        reportError(*result.error());
        return;
    }
    model_.replace(result.value());
}

void ProjectController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void ProjectController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
