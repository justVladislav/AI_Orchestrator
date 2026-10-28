#pragma once

#include <ai/application/projects/ProjectService.hpp>
#include <ai/ui/ProjectListModel.hpp>

#include <QAbstractItemModel>
#include <QObject>
#include <QString>

/** Qt command/read-model adapter for ProjectService. */
class ProjectController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ProjectController(ai::application::ProjectService& service, QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE QString createProject(
        const QString& name,
        const QString& description,
        const QString& rootPath);
    Q_INVOKABLE bool archiveProject(const QString& projectId);
    Q_INVOKABLE bool restoreProject(const QString& projectId);
    Q_INVOKABLE bool updateProjectPath(const QString& projectId, const QString& rootPath);
    Q_INVOKABLE bool updateProjectDescription(const QString& projectId,
                                              const QString& description);
    Q_INVOKABLE void refresh();

signals:
    void projectCreated(const QString& projectId);
    void projectArchived(const QString& projectId);
    void projectRestored(const QString& projectId);
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::ProjectService& service_;
    ProjectListModel model_;
    QString lastError_;
};
