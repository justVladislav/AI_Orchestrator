#include "ai/ui/ProviderExecutionAnalyticsModel.hpp"

#include <QLocale>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

using ai::application::ExecutionPriceState;

QString executionCostText(const ai::application::ExecutionAggregate& aggregate)
{
    const auto state = aggregate.priceState();
    if (state != ExecutionPriceState::Known && state != ExecutionPriceState::Partial) return {};
    const double dollars = static_cast<double>(aggregate.costMicros) / 1'000'000.0;
    return QStringLiteral("$%1").arg(
        QLocale::c().toString(dollars, 'f', dollars >= 100.0 ? 2 : 4));
}

namespace {

bool priceIncomplete(const ai::application::ExecutionAggregate& aggregate)
{
    const auto state = aggregate.priceState();
    return state == ExecutionPriceState::Partial || state == ExecutionPriceState::Unknown;
}

} // namespace

ProviderExecutionAnalyticsModel::ProviderExecutionAnalyticsModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ProviderExecutionAnalyticsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ProviderExecutionAnalyticsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    const auto& totals = row.aggregate.totals;
    switch (role) {
    case ProviderIdRole: return QString::fromStdString(row.aggregate.providerId);
    case ProviderNameRole: return row.name;
    case ExecutionCountRole: return QVariant::fromValue<qulonglong>(totals.executionCount);
    case CompletedCountRole: return QVariant::fromValue<qulonglong>(totals.completedCount);
    case FailedCountRole: return QVariant::fromValue<qulonglong>(totals.failedCount);
    case InputTokensRole: return QVariant::fromValue<qulonglong>(totals.inputTokens);
    case OutputTokensRole: return QVariant::fromValue<qulonglong>(totals.outputTokens);
    case CostMicrosRole: return QVariant::fromValue<qulonglong>(totals.costMicros);
    case AverageLatencyMsRole: return QVariant::fromValue<qulonglong>(totals.averageLatencyMs);
    case PriceStateRole: return QString::fromLatin1(toString(totals.priceState()));
    case UnknownPriceExecutionCountRole:
        return QVariant::fromValue<qulonglong>(totals.unknownPriceExecutionCount);
    case CostTextRole: return executionCostText(totals);
    case PriceIncompleteRole: return priceIncomplete(totals);
    case CostShareRole: return shares_[static_cast<std::size_t>(index.row())];
    case ShareBasisRole: return shareBasis_;
    case ModelsRole: {
        QVariantList models;
        models.reserve(static_cast<qsizetype>(row.aggregate.models.size()));
        for (const auto& model : row.aggregate.models) {
            const auto modelId = QString::fromStdString(model.modelId);
            const auto& modelTotals = model.totals;
            QVariantMap item;
            item.insert(QStringLiteral("modelId"), modelId);
            item.insert(QStringLiteral("modelName"), row.modelNames.value(modelId, modelId));
            item.insert(QStringLiteral("executionCount"), QVariant::fromValue<qulonglong>(modelTotals.executionCount));
            item.insert(QStringLiteral("completedCount"), QVariant::fromValue<qulonglong>(modelTotals.completedCount));
            item.insert(QStringLiteral("failedCount"), QVariant::fromValue<qulonglong>(modelTotals.failedCount));
            item.insert(QStringLiteral("inputTokens"), QVariant::fromValue<qulonglong>(modelTotals.inputTokens));
            item.insert(QStringLiteral("outputTokens"), QVariant::fromValue<qulonglong>(modelTotals.outputTokens));
            item.insert(QStringLiteral("costMicros"), QVariant::fromValue<qulonglong>(modelTotals.costMicros));
            item.insert(QStringLiteral("averageLatencyMs"), QVariant::fromValue<qulonglong>(modelTotals.averageLatencyMs));
            item.insert(QStringLiteral("priceState"), QString::fromLatin1(toString(modelTotals.priceState())));
            item.insert(QStringLiteral("unknownPriceExecutionCount"),
                        QVariant::fromValue<qulonglong>(modelTotals.unknownPriceExecutionCount));
            item.insert(QStringLiteral("costText"), executionCostText(modelTotals));
            item.insert(QStringLiteral("priceIncomplete"), priceIncomplete(modelTotals));
            models.push_back(item);
        }
        return models;
    }
    default: return {};
    }
}

QHash<int, QByteArray> ProviderExecutionAnalyticsModel::roleNames() const
{
    return {
        {ProviderIdRole, "providerId"},
        {ProviderNameRole, "providerName"},
        {ExecutionCountRole, "executionCount"},
        {CompletedCountRole, "completedCount"},
        {FailedCountRole, "failedCount"},
        {InputTokensRole, "inputTokens"},
        {OutputTokensRole, "outputTokens"},
        {CostMicrosRole, "costMicros"},
        {AverageLatencyMsRole, "averageLatencyMs"},
        {ModelsRole, "models"},
        {PriceStateRole, "priceState"},
        {UnknownPriceExecutionCountRole, "unknownPriceExecutionCount"},
        {CostTextRole, "costText"},
        {PriceIncompleteRole, "priceIncomplete"},
        {CostShareRole, "costShare"},
        {ShareBasisRole, "shareBasis"}
    };
}

void ProviderExecutionAnalyticsModel::replace(std::vector<ProviderExecutionAnalyticsRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    recomputeShares();
    endResetModel();
}

// Relative shares of different providers are only meaningful between EXACT amounts. A Partial
// amount is a lower bound and an Unknown one is no amount at all, so as soon as one row is not
// Known no cost share is shown for any row (nothing is normalised between lower bounds). With
// every row Known the largest provider is 1; if every known cost is zero the chart falls back to
// token volume, which is exact data.
void ProviderExecutionAnalyticsModel::recomputeShares()
{
    shares_.assign(rows_.size(), 0.0);
    const bool allKnown = std::all_of(rows_.begin(), rows_.end(), [](const auto& row) {
        return row.aggregate.totals.priceState() == ExecutionPriceState::Known;
    });
    if (!allKnown) {
        shareBasis_ = QStringLiteral("incomplete");
        return;
    }
    double maximumCost = 0.0;
    double maximumTokens = 0.0;
    for (const auto& row : rows_) {
        maximumCost = std::max(maximumCost, static_cast<double>(row.aggregate.totals.costMicros));
        maximumTokens = std::max(maximumTokens,
            static_cast<double>(row.aggregate.totals.inputTokens)
                + static_cast<double>(row.aggregate.totals.outputTokens));
    }
    const bool byCost = maximumCost > 0.0;
    shareBasis_ = byCost ? QStringLiteral("cost") : QStringLiteral("tokens");
    const double denominator = byCost ? maximumCost : maximumTokens;
    if (denominator <= 0.0) return;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto& totals = rows_[i].aggregate.totals;
        const double numerator = byCost
            ? static_cast<double>(totals.costMicros)
            : static_cast<double>(totals.inputTokens) + static_cast<double>(totals.outputTokens);
        shares_[i] = numerator / denominator;
    }
}
