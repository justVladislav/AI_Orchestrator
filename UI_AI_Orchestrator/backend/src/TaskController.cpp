#include <ai/ui/TaskController.hpp>

#include <algorithm>

namespace {
std::optional<ai::application::TaskStatus> status(const QString& value)
{
    using S = ai::application::TaskStatus;
    if (value == "backlog") return S::Backlog;
    if (value == "planned") return S::Planned;
    if (value == "in_progress") return S::InProgress;
    if (value == "blocked") return S::Blocked;
    if (value == "done") return S::Done;
    if (value == "cancelled") return S::Cancelled;
    return std::nullopt;
}
std::optional<ai::application::TaskPriority> priority(const QString& value)
{
    using P = ai::application::TaskPriority;
    if (value == "low") return P::Low;
    if (value == "normal") return P::Normal;
    if (value == "high") return P::High;
    if (value == "critical") return P::Critical;
    return std::nullopt;
}
std::optional<std::chrono::system_clock::time_point> time(const QDateTime& value)
{
    if (!value.isValid()) return std::nullopt;
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value.toMSecsSinceEpoch()));
}
}

TaskController::TaskController(ai::application::TaskService& service, QObject* parent)
    : QObject(parent), service_(service), model_(this) { refresh(); }
QAbstractItemModel* TaskController::model() noexcept { return &model_; }
QString TaskController::lastError() const { return lastError_; }

QString TaskController::createTask(const QString& projectId, const QString& title,
    const QString& description, const QString& statusValue, const QString& priorityValue,
    bool requiresAttention, const QDateTime& dueAt, const QDateTime& plannedRunAt)
{
    const auto s = status(statusValue); const auto p = priority(priorityValue);
    if (!s || !p) { report({"task.invalid_value","Unknown task status or priority"}); return {}; }
    ai::application::CreateTaskRequest request;
    request.projectId = projectId.toStdString(); request.title = title.toStdString();
    request.description = description.toStdString(); request.status = *s; request.priority = *p;
    request.requiresAttention = requiresAttention; request.dueAt = time(dueAt);
    request.plannedRunAt = time(plannedRunAt);
    const auto result = service_.createTask(request);
    if (!result.success()) { report(*result.error()); return {}; }
    clearError(); refresh(); return QString::fromStdString(result.value().id);
}

bool TaskController::updateTask(const QString& taskId, const QString& title,
    const QString& description, const QString& statusValue, const QString& priorityValue,
    bool requiresAttention, const QDateTime& dueAt, const QDateTime& plannedRunAt)
{
    const auto s = status(statusValue); const auto p = priority(priorityValue);
    if (!s || !p) { report({"task.invalid_value","Unknown task status or priority"}); return false; }
    ai::application::UpdateTaskRequest request;
    request.taskId = taskId.toStdString(); request.title = title.toStdString();
    request.description = description.toStdString(); request.status = *s; request.priority = *p;
    request.requiresAttention = requiresAttention; request.dueAt = time(dueAt);
    request.plannedRunAt = time(plannedRunAt);
    const auto result = service_.updateTask(request);
    if (!result.success()) { report(*result.error()); return false; }
    clearError(); refresh(); return true;
}
bool TaskController::cancelTask(const QString& taskId)
{
    const auto result = service_.cancelTask(taskId.toStdString());
    if (!result.success()) { report(*result.error()); return false; }
    clearError(); refresh(); return true;
}
bool TaskController::linkRun(const QString& taskId, const QString& runId)
{
    const auto result = service_.linkRun(taskId.toStdString(), runId.toStdString());
    if (!result.success()) { report(*result.error()); return false; }
    clearError(); return true;
}
void TaskController::completeTasksForRun(const QString& runId, const QString& runStatus)
{
    // Only a successful run closes its tasks; failed/cancelled/interrupted
    // outcomes leave them open so they stay visible as needing action.
    if (runStatus != QStringLiteral("completed"))
        return;
    const auto tasks = service_.listTasks();
    if (!tasks.success()) { report(*tasks.error()); return; }
    const std::string run = runId.toStdString();
    bool changed = false;
    for (const auto& task : tasks.value()) {
        using S = ai::application::TaskStatus;
        if (task.status == S::Done || task.status == S::Cancelled)
            continue;
        const auto links = service_.linkedRuns(task.id);
        if (!links.success()) continue;
        if (std::find(links.value().begin(), links.value().end(), run) == links.value().end())
            continue;
        ai::application::UpdateTaskRequest request;
        request.taskId = task.id;
        request.title = task.title;
        request.description = task.description;
        request.status = S::Done;
        request.priority = task.priority;
        // A finished task no longer needs attention, so it also leaves the
        // Overview Needs Attention panel (which filters done/cancelled).
        request.requiresAttention = false;
        request.dueAt = task.dueAt;
        request.plannedRunAt = task.plannedRunAt;
        const auto updated = service_.updateTask(request);
        if (updated.success()) changed = true;
        else report(*updated.error());
    }
    if (changed) { clearError(); refresh(); }
}

void TaskController::refresh()
{
    const auto result = service_.listTasks();
    if (!result.success()) { report(*result.error()); return; }
    model_.replace(result.value()); emit tasksChanged();
}
void TaskController::report(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message); emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}
void TaskController::clearError()
{
    if (!lastError_.isEmpty()) { lastError_.clear(); emit lastErrorChanged(); }
}
