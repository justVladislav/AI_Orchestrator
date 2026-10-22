#include "ai/ui/ComparisonSessionListModel.hpp"

#include <QDateTime>
#include <QLocale>

namespace {

QDateTime toDateTime(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

QDateTime updatedAt(const ai::application::ComparisonSession& session)
{
    if (session.finishedAt.has_value()) return toDateTime(*session.finishedAt);
    if (session.startedAt.has_value()) return toDateTime(*session.startedAt);
    return toDateTime(session.createdAt);
}

QString uiStatus(ai::application::ComparisonSessionStatus status)
{
    return status == ai::application::ComparisonSessionStatus::Pending
        ? QStringLiteral("queued")
        : QString::fromLatin1(ai::application::toString(status));
}

} // namespace

ComparisonSessionListModel::ComparisonSessionListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ComparisonSessionListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ComparisonSessionListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    const auto& session = row.session;
    switch (role) {
    case SessionIdRole: return QString::fromStdString(session.id);
    case ProjectIdRole: return QString::fromStdString(session.projectId);
    case TitleRole: return QString::fromStdString(session.title);
    case StatusRole: return uiStatus(session.status);
    case UpdatedAtRole: return updatedAt(session);
    case UpdatedAtTextRole:
        return QLocale().toString(updatedAt(session).toLocalTime(), QLocale::ShortFormat);
    case ResultCountRole: return row.resultCount;
    default: return {};
    }
}

QHash<int, QByteArray> ComparisonSessionListModel::roleNames() const
{
    return {
        {SessionIdRole, "sessionId"},
        {ProjectIdRole, "projectId"},
        {TitleRole, "title"},
        {StatusRole, "status"},
        {UpdatedAtRole, "updatedAt"},
        {UpdatedAtTextRole, "updatedAtText"},
        {ResultCountRole, "resultCount"}
    };
}

void ComparisonSessionListModel::replace(std::vector<ComparisonSessionRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}
