#include "ai/ui/ProjectCostProviderModel.hpp"

#include "ai/ui/ProjectCostProjection.hpp"

#include <QVariantList>

#include <algorithm>

using ai::application::ExecutionPriceState;
using ai::application::ProviderProjectCost;
namespace project_cost = ai::ui::project_cost;

ProjectCostShares computeProjectCostShares(const std::vector<ProviderProjectCost>& providers)
{
    ProjectCostShares result;
    result.shares.assign(providers.size(), 0.0);
    if (providers.empty()) {
        result.basis = QStringLiteral("empty");
        return result;
    }
    const auto anyRowIs = [&providers](ExecutionPriceState state) {
        return std::any_of(providers.begin(), providers.end(),
            [state](const ProviderProjectCost& provider) { return provider.totals.priceState() == state; });
    };
    if (anyRowIs(ExecutionPriceState::Partial) || anyRowIs(ExecutionPriceState::Unknown)) {
        result.basis = QStringLiteral("incomplete");
        return result;
    }
    if (anyRowIs(ExecutionPriceState::None)) {
        result.basis = QStringLiteral("unavailable");
        return result;
    }
    double maximumCost = 0.0;
    double maximumTokens = 0.0;
    for (const auto& provider : providers) {
        maximumCost = std::max(maximumCost, static_cast<double>(provider.totals.costMicros()));
        maximumTokens = std::max(maximumTokens,
            static_cast<double>(provider.totals.inputTokens()) + static_cast<double>(provider.totals.outputTokens()));
    }
    const bool byCost = maximumCost > 0.0;
    result.basis = byCost ? QStringLiteral("cost") : QStringLiteral("tokens");
    const double denominator = byCost ? maximumCost : maximumTokens;
    if (denominator <= 0.0) return result;
    for (std::size_t i = 0; i < providers.size(); ++i) {
        const auto& totals = providers[i].totals;
        const double numerator = byCost
            ? static_cast<double>(totals.costMicros())
            : static_cast<double>(totals.inputTokens()) + static_cast<double>(totals.outputTokens());
        result.shares[i] = numerator / denominator;
    }
    return result;
}

ProjectCostProviderModel::ProjectCostProviderModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ProjectCostProviderModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ProjectCostProviderModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    const auto& totals = row.cost.totals;
    const auto count = [](std::uint64_t value) { return QVariant::fromValue<qulonglong>(value); };
    switch (role) {
    case ProviderIdRole: return QString::fromStdString(row.cost.providerId);
    case ProviderNameRole: return row.name;
    case LedgerExecutionCountRole: return count(totals.ledgerExecutionCount());
    case CompactionRecordCountRole: return count(totals.compactionRecordCount());
    case CompactionNoneCountRole: return count(totals.compactionNoneCount());
    case InputTokensRole: return count(totals.inputTokens());
    case OutputTokensRole: return count(totals.outputTokens());
    case LedgerAverageLatencyMsRole: return count(totals.ledgerAverageLatencyMs());
    case PriceStateRole: return project_cost::priceStateName(totals);
    case CostTextRole: return project_cost::costText(totals);
    case PriceIncompleteRole: return project_cost::priceIncomplete(totals);
    case PricingEvidenceCountRole: return count(totals.pricingEvidenceCount());
    case IncompletePriceCountRole: return count(totals.incompletePriceCount());
    case CostShareRole: return shares_[static_cast<std::size_t>(index.row())];
    case ShareBasisRole: return shareBasis_;
    case ModelsRole: {
        QVariantList models;
        models.reserve(static_cast<qsizetype>(row.cost.models.size()));
        for (const auto& model : row.cost.models) {
            const auto modelId = QString::fromStdString(model.modelId);
            QVariantMap item = project_cost::aggregateMap(model.totals);
            item.insert(QStringLiteral("modelId"), modelId);
            item.insert(QStringLiteral("modelName"), row.modelNames.value(modelId, modelId));
            models.push_back(item);
        }
        return models;
    }
    default: return {};
    }
}

QHash<int, QByteArray> ProjectCostProviderModel::roleNames() const
{
    return {
        {ProviderIdRole, "providerId"},
        {ProviderNameRole, "providerName"},
        {LedgerExecutionCountRole, "ledgerExecutionCount"},
        {CompactionRecordCountRole, "compactionRecordCount"},
        {CompactionNoneCountRole, "compactionNoneCount"},
        {InputTokensRole, "inputTokens"},
        {OutputTokensRole, "outputTokens"},
        {LedgerAverageLatencyMsRole, "ledgerAverageLatencyMs"},
        {ModelsRole, "models"},
        {PriceStateRole, "priceState"},
        {CostTextRole, "costText"},
        {PriceIncompleteRole, "priceIncomplete"},
        {PricingEvidenceCountRole, "pricingEvidenceCount"},
        {IncompletePriceCountRole, "incompletePriceCount"},
        {CostShareRole, "costShare"},
        {ShareBasisRole, "shareBasis"}
    };
}

void ProjectCostProviderModel::replace(std::vector<ProjectCostProviderRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    std::vector<ProviderProjectCost> costs;
    costs.reserve(rows_.size());
    for (const auto& row : rows_) costs.push_back(row.cost);
    auto computed = computeProjectCostShares(costs);
    shares_ = std::move(computed.shares);
    shareBasis_ = std::move(computed.basis);
    endResetModel();
}
