#pragma once

#include <ai/application/ProviderCatalogTypes.hpp>

#include <QAbstractListModel>

#include <vector>

/** Read-only Qt projection of Backend provider metadata. */
class ProviderListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ProviderIdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        BaseUrlRole,
        EnabledRole,
        CredentialConfiguredRole,
        CreatedAtRole,
        UpdatedAtRole
    };
    Q_ENUM(Role)

    explicit ProviderListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::Provider> providers);

private:
    std::vector<ai::application::Provider> providers_;
};
