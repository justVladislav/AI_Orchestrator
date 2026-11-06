#pragma once

#include <ai/application/analytics/ProjectCostTypes.hpp>

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <vector>

struct ProjectCostProviderRow {
    ai::application::ProviderProjectCost cost;
    QString name;
    QHash<QString, QString> modelNames;
};

// The chart shares of the provider rows and the reason they are what they are. A share is a ratio to the LARGEST row (the largest is
// 1), not a percentage of the total, and only between EXACT amounts. The basis tells QML why a bar is or is not drawn:
//   "cost"        every row is Known and some cost is positive: share = row cost / the largest row cost,
//   "tokens"      every row is Known and every cost is a known zero: share = row tokens / the largest row tokens,
//   "incomplete"  some row is Partial or Unknown: no share (incomplete estimates are never normalised against each other),
//   "unavailable" no row is Partial or Unknown but some row has no price data at all (for example only cancelled compactions):
//                 that row is not a known zero, so no share is shown for any row,
//   "empty"       there are no rows: nothing to compare (NOT an incomplete price).
// The older ProviderExecutionAnalyticsModel keeps its own rule; this one is equivalent to it on all-Known and on Partial/Unknown rows
// (tested) and adds the two last cases.
struct ProjectCostShares {
    std::vector<double> shares;
    QString basis;
};
[[nodiscard]] ProjectCostShares computeProjectCostShares(const std::vector<ai::application::ProviderProjectCost>& providers);

class ProjectCostProviderModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ProviderIdRole = Qt::UserRole + 1,
        ProviderNameRole,
        LedgerExecutionCountRole,
        CompactionRecordCountRole,
        CompactionNoneCountRole,
        InputTokensRole,
        OutputTokensRole,
        LedgerAverageLatencyMsRole,
        ModelsRole,
        PriceStateRole,
        CostTextRole,
        PriceIncompleteRole,
        PricingEvidenceCountRole,
        IncompletePriceCountRole,
        CostShareRole,
        ShareBasisRole
    };
    Q_ENUM(Role)

    explicit ProjectCostProviderModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ProjectCostProviderRow> rows);
    [[nodiscard]] QString sharesBasis() const { return shareBasis_; }

private:
    std::vector<ProjectCostProviderRow> rows_;
    std::vector<double> shares_;
    QString shareBasis_ {QStringLiteral("empty")};
};
