#pragma once

#include <ai/application/ExecutionAnalyticsTypes.hpp>

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <vector>

struct ProviderExecutionAnalyticsRow {
    ai::application::ProviderExecutionAggregate aggregate;
    QString name;
    QHash<QString, QString> modelNames;
};

// The amount QML shows next to an analytics aggregate, formatted by the Qt layer so QML never reads
// or formats raw cost. Empty when the price is Unknown (or there is nothing): an unknown price is
// never rendered as "$0". For a Partial aggregate the amount is a lower bound (the caller words it
// as such). Same rule as the former QML formatter: 4 decimals, 2 from $100.
[[nodiscard]] QString executionCostText(const ai::application::ExecutionAggregate& aggregate);

class ProviderExecutionAnalyticsModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ProviderIdRole = Qt::UserRole + 1,
        ProviderNameRole,
        ExecutionCountRole,
        CompletedCountRole,
        FailedCountRole,
        InputTokensRole,
        OutputTokensRole,
        CostMicrosRole,
        AverageLatencyMsRole,
        ModelsRole,
        // Price state of the row's aggregate ("none" | "known" | "partial" | "unknown"), the
        // Backend-formatted amount and whether the data is incomplete (partial or unknown).
        PriceStateRole,
        UnknownPriceExecutionCountRole,
        CostTextRole,
        PriceIncompleteRole,
        // The row's share of the chart. Relative shares are only meaningful between EXACT
        // amounts: with any Partial/Unknown row the basis is "incomplete" and the share is 0 -
        // lower bounds of different executions are never normalised against each other.
        // Otherwise "cost" (largest provider = 1) or, when every cost is a known zero, "tokens".
        CostShareRole,
        ShareBasisRole
    };
    Q_ENUM(Role)

    explicit ProviderExecutionAnalyticsModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ProviderExecutionAnalyticsRow> rows);

private:
    void recomputeShares();

    std::vector<ProviderExecutionAnalyticsRow> rows_;
    std::vector<double> shares_;
    QString shareBasis_;
};
