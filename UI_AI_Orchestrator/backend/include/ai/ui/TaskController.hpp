#pragma once

#include <ai/application/projects/TaskService.hpp>
#include <ai/ui/TaskListModel.hpp>

#include <QAbstractItemModel>
#include <QDateTime>
#include <QObject>
#include <QString>

class TaskController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
public:
    explicit TaskController(ai::application::TaskService& service, QObject* parent = nullptr);
    QAbstractItemModel* model() noexcept;
    QString lastError() const;
    Q_INVOKABLE QString createTask(const QString& projectId, const QString& title,
        const QString& description, const QString& status, const QString& priority,
        bool requiresAttention, const QDateTime& dueAt, const QDateTime& plannedRunAt);
    Q_INVOKABLE bool updateTask(const QString& taskId, const QString& title,
        const QString& description, const QString& status, const QString& priority,
        bool requiresAttention, const QDateTime& dueAt, const QDateTime& plannedRunAt);
    Q_INVOKABLE bool cancelTask(const QString& taskId);
    Q_INVOKABLE bool linkRun(const QString& taskId, const QString& runId);
    Q_INVOKABLE void refresh();
    // Connected to RunController::runFinished by BackendBridge: a successfully
    // completed run marks every linked open task as done.
    void completeTasksForRun(const QString& runId, const QString& runStatus);
signals:
    void tasksChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();
private:
    void report(const ai::common::Error& error);
    void clearError();
    ai::application::TaskService& service_;
    TaskListModel model_;
    QString lastError_;
};
