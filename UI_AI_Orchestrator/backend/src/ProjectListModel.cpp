#include <ai/ui/ProjectListModel.hpp>

#include <QDateTime>
#include <QString>

namespace {

QDateTime toDateTime(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

} // namespace

ProjectListModel::ProjectListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ProjectListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(projects_.size());
}

QVariant ProjectListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& project = projects_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ProjectIdRole: return QString::fromStdString(project.id);
    case NameRole: return QString::fromStdString(project.name);
    case DescriptionRole: return QString::fromStdString(project.description);
    case RootPathRole: return QString::fromStdWString(project.rootPath.generic_wstring());
    case StatusRole: return QString::fromLatin1(ai::application::toString(project.status));
    case CreatedAtRole: return toDateTime(project.createdAt);
    case UpdatedAtRole: return toDateTime(project.updatedAt);
    default: return {};
    }
}

QHash<int, QByteArray> ProjectListModel::roleNames() const
{
    return {
        {ProjectIdRole, "projectId"},
        {NameRole, "name"},
        {DescriptionRole, "description"},
        {RootPathRole, "rootPath"},
        {StatusRole, "status"},
        {CreatedAtRole, "createdAt"},
        {UpdatedAtRole, "updatedAt"}
    };
}

void ProjectListModel::replace(std::vector<ai::application::Project> projects)
{
    beginResetModel();
    projects_ = std::move(projects);
    endResetModel();
}
