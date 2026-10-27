#pragma once

#include <ai/application/comparison/ComparisonReadModelService.hpp>

#include <QAbstractListModel>
#include <vector>

class ComparisonResultListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ResultIdRole = Qt::UserRole + 1,
        ProviderIdRole,
        ProviderNameRole,
        ModelIdRole,
        ModelNameRole,
        ExecutionRole,
        StatusRole,
        ContentRole,
        ErrorTextRole,
        StrengthsRole,
        WarningsRole,
        InputTokensRole,
        OutputTokensRole,
        CostMicrosRole,
        CostPriceKnownRole,
        TotalCostTextRole,
        LatencyMsRole,
        LatencyTextRole,
        QualityAvailableRole,
        QualityScoreRole,
        QualityBasisPointsRole,
        QualitySourceRole,
        IsPreferredRole,
        SelectedForSynthesisRole,
        CanRetryRole,
        CanSelectForSynthesisRole
    };
    Q_ENUM(Role)

    explicit ComparisonResultListModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ai::application::ComparisonResultReadModel> rows);

private:
    std::vector<ai::application::ComparisonResultReadModel> rows_;
};
