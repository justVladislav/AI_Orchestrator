#include <ai/ui/TaskListModel.hpp>

#include <QDateTime>
#include <QString>

namespace {
QDateTime dateTime(std::chrono::system_clock::time_point value)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(ms.count(), Qt::UTC);
}
QVariant optionalDate(const std::optional<std::chrono::system_clock::time_point>& value)
{
    return value.has_value() ? QVariant::fromValue(dateTime(*value)) : QVariant {};
}
}

TaskListModel::TaskListModel(QObject* parent) : QAbstractListModel(parent) {}
int TaskListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(tasks_.size());
}
QVariant TaskListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& task = tasks_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TaskIdRole: return QString::fromStdString(task.id);
    case ProjectIdRole: return QString::fromStdString(task.projectId);
    case TitleRole: return QString::fromStdString(task.title);
    case DescriptionRole: return QString::fromStdString(task.description);
    case StatusRole: return QString::fromLatin1(ai::application::toString(task.status));
    case PriorityRole: return QString::fromLatin1(ai::application::toString(task.priority));
    case RequiresAttentionRole: return task.requiresAttention;
    case DueAtRole: return optionalDate(task.dueAt);
    case PlannedRunAtRole: return optionalDate(task.plannedRunAt);
    case CreatedAtRole: return dateTime(task.createdAt);
    case UpdatedAtRole: return dateTime(task.updatedAt);
    default: return {};
    }
}
QHash<int, QByteArray> TaskListModel::roleNames() const
{
    return {{TaskIdRole,"taskId"},{ProjectIdRole,"projectId"},{TitleRole,"title"},
        {DescriptionRole,"description"},{StatusRole,"status"},{PriorityRole,"priority"},
        {RequiresAttentionRole,"requiresAttention"},{DueAtRole,"dueAt"},
        {PlannedRunAtRole,"plannedRunAt"},{CreatedAtRole,"createdAt"},{UpdatedAtRole,"updatedAt"}};
}
void TaskListModel::replace(std::vector<ai::application::ProjectTask> tasks)
{
    beginResetModel(); tasks_ = std::move(tasks); endResetModel();
}
