#pragma once

#include <ai/application/catalog/ProviderCatalogTypes.hpp>

#include <QAbstractListModel>

#include <vector>

/** Read-only Qt projection of Backend model metadata. */
class ModelListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ModelIdRole = Qt::UserRole + 1,
        ProviderIdRole,
        ApiModelIdRole,
        DisplayNameRole,
        ContextWindowTokensRole,
        MaxOutputTokensRole,
        InputPriceMicrosRole,
        OutputPriceMicrosRole,
        CacheWritePriceMicrosRole,
        CacheReadPriceMicrosRole,
        EnabledRole,
        DefaultRole,
        CreatedAtRole,
        UpdatedAtRole,
        // Long-context tier (appended last so no existing role value shifts). Every value is the
        // Backend's own: QML never derives "enabled" from the threshold itself.
        LongContextEnabledRole,
        LongContextThresholdInputTokensRole,
        LongContextInputPriceMicrosRole,
        LongContextOutputPriceMicrosRole,
        LongContextCacheWritePriceMicrosRole,
        LongContextCacheReadPriceMicrosRole,
        CacheWriteOneHourPriceMicrosRole,
        LongContextCacheWriteOneHourPriceMicrosRole
    };
    Q_ENUM(Role)

    explicit ModelListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::Model> models);

private:
    std::vector<ai::application::Model> models_;
};
