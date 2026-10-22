#include "ai/ui/AgentToolCallListModel.hpp"

#include <QDateTime>
#include <QLocale>

namespace {

QString toLocalTimeText(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC).toLocalTime(),
        QLocale::ShortFormat);
}

QString statusText(ai::application::AgentToolCallState state)
{
    switch (state) {
    case ai::application::AgentToolCallState::Requested: return QStringLiteral("requested");
    case ai::application::AgentToolCallState::Running: return QStringLiteral("running");
    case ai::application::AgentToolCallState::Completed: return QStringLiteral("completed");
    case ai::application::AgentToolCallState::Failed: return QStringLiteral("failed");
    case ai::application::AgentToolCallState::Interrupted: return QStringLiteral("interrupted");
    }
    return QStringLiteral("unknown");
}

} // namespace

AgentToolCallListModel::AgentToolCallListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AgentToolCallListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant AgentToolCallListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case AuditIdRole: return QString::fromStdString(row.auditId);
    case ToolNameRole: return QString::fromStdString(row.toolName);
    case StatusRole: return statusText(row.state);
    // A failed call with an errorCode means the executor refused the
    // request (path outside the workspace, denied directory, secret
    // pattern, size limit, etc.) - surfaced to the UI as "denied" rather
    // than a generic failure so a reviewer can tell policy enforcement
    // apart from a transport/internal error.
    case DeniedRole:
        return row.state == ai::application::AgentToolCallState::Failed
            && !row.errorCode.empty();
    case TruncatedRole: return row.truncated;
    case ReturnedBytesRole: return QVariant::fromValue<qulonglong>(row.returnedBytes);
    case ErrorCodeRole: return QString::fromStdString(row.errorCode);
    case CreatedAtTextRole: return toLocalTimeText(row.createdAt);
    case FinishedAtTextRole:
        return row.finishedAt.has_value() ? toLocalTimeText(*row.finishedAt) : QString();
    default: return {};
    }
}

QHash<int, QByteArray> AgentToolCallListModel::roleNames() const
{
    return {
        {AuditIdRole, "auditId"}, {ToolNameRole, "toolName"},
        {StatusRole, "status"}, {DeniedRole, "denied"},
        {TruncatedRole, "truncated"}, {ReturnedBytesRole, "returnedBytes"},
        {ErrorCodeRole, "errorCode"}, {CreatedAtTextRole, "createdAtText"},
        {FinishedAtTextRole, "finishedAtText"}
    };
}

void AgentToolCallListModel::replace(std::vector<ai::application::AgentToolAuditRecord> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}
