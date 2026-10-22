#include <ai/ui/ProviderListModel.hpp>

#include <QDateTime>
#include <QString>

#include <utility>

namespace {

QDateTime toDateTime(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

} // namespace

ProviderListModel::ProviderListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ProviderListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(providers_.size());
}

QVariant ProviderListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& provider = providers_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ProviderIdRole: return QString::fromStdString(provider.id);
    case NameRole: return QString::fromStdString(provider.name);
    case KindRole: return QString::fromStdString(provider.kind);
    case BaseUrlRole: return QString::fromStdString(provider.baseUrl);
    case EnabledRole: return provider.enabled;
    case CredentialConfiguredRole: return provider.credentialReference.has_value();
    case CreatedAtRole: return toDateTime(provider.createdAt);
    case UpdatedAtRole: return toDateTime(provider.updatedAt);
    default: return {};
    }
}

QHash<int, QByteArray> ProviderListModel::roleNames() const
{
    return {
        {ProviderIdRole, "providerId"},
        {NameRole, "name"},
        {KindRole, "kind"},
        {BaseUrlRole, "baseUrl"},
        {EnabledRole, "enabled"},
        {CredentialConfiguredRole, "credentialConfigured"},
        {CreatedAtRole, "createdAt"},
        {UpdatedAtRole, "updatedAt"}
    };
}

void ProviderListModel::replace(std::vector<ai::application::Provider> providers)
{
    beginResetModel();
    providers_ = std::move(providers);
    endResetModel();
}
