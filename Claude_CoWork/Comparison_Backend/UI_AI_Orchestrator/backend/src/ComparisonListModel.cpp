#include <ai/ui/ComparisonListModel.hpp>

#include <QDateTime>
#include <chrono>

namespace {

QString relativeTime(std::chrono::system_clock::time_point tp)
{
    const auto nowMs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto tpMs = std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count();
    const auto diff = nowMs - tpMs;

    if (diff < 60)           return QObject::tr("Just now");
    if (diff < 3600)         return QObject::tr("%1 min ago").arg(diff / 60);
    if (diff < 86400)        return QObject::tr("%1 hr ago").arg(diff / 3600);
    if (diff < 86400 * 2)    return QObject::tr("Yesterday");
    if (diff < 86400 * 7)    return QObject::tr("%1 days ago").arg(diff / 86400);
    return QObject::tr("%1 weeks ago").arg(diff / (86400 * 7));
}

} // namespace

ComparisonListModel::ComparisonListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ComparisonListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(sessions_.size());
}

QVariant ComparisonListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto& s = sessions_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ComparisonIdRole:
    case ItemIdRole:
        return QString::fromStdString(s.id);
    case TitleRole:
    case ItemLabelRole:
        return QString::fromStdString(s.title);
    case ProjectIdRole:
        return QString::fromStdString(s.projectId);
    case UpdatedAtRole:
        return relativeTime(s.updatedAt);
    case StatusRole:
        return QString::fromStdString(std::string(ai::application::toString(s.status)));
    default:
        return {};
    }
}

QHash<int, QByteArray> ComparisonListModel::roleNames() const
{
    return {
        {ComparisonIdRole, "comparisonId"},
        {ItemIdRole,       "itemId"},
        {TitleRole,        "title"},
        {ItemLabelRole,    "itemLabel"},
        {ProjectIdRole,    "projectId"},
        {UpdatedAtRole,    "updatedAt"},
        {StatusRole,       "status"}
    };
}

void ComparisonListModel::replace(std::vector<ai::application::ComparisonSession> sessions)
{
    beginResetModel();
    sessions_ = std::move(sessions);
    endResetModel();
}
