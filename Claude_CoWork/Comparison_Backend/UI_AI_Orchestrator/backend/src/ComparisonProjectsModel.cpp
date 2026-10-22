#include <ai/ui/ComparisonProjectsModel.hpp>

#include <QDateTime>

ComparisonProjectsModel::ComparisonProjectsModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ComparisonProjectsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(projects_.size());
}

QVariant ComparisonProjectsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto& p = projects_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ProjectIdRole:
    case ItemIdRole:
        return QString::fromStdString(p.id);
    case NameRole:
    case ItemLabelRole:
        return QString::fromStdString(p.name);
    default:
        return {};
    }
}

QHash<int, QByteArray> ComparisonProjectsModel::roleNames() const
{
    return {
        {ProjectIdRole, "projectId"},
        {ItemIdRole,    "itemId"},
        {NameRole,      "name"},
        {ItemLabelRole, "itemLabel"}
    };
}

void ComparisonProjectsModel::replace(std::vector<ai::application::Project> projects)
{
    beginResetModel();
    projects_ = std::move(projects);
    endResetModel();
}
