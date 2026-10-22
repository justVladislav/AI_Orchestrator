#include "ai/ui/ComparisonResultListModel.hpp"

#include <QLocale>

namespace {

// MW-P2: priceKnown=false means a usage category this execution actually
// consumed had an unset model price, so costMicros is a best-effort
// underestimate rather than a precise total - see
// ai::application::CostBreakdown's doc comment.
QString costText(std::uint64_t micros, bool priceKnown)
{
    const auto amount = QStringLiteral("$%1").arg(QLocale::c().toString(
        static_cast<double>(micros) / 1'000'000.0, 'f', 6));
    return priceKnown ? amount : amount + QStringLiteral(" (est.)");
}

QString latencyText(std::uint64_t milliseconds)
{
    return milliseconds == 0
        ? QStringLiteral("—")
        : QStringLiteral("%1 ms").arg(milliseconds);
}

bool reviewable(ai::application::ModelExecutionStatus status) noexcept
{
    return status == ai::application::ModelExecutionStatus::Completed
        || status == ai::application::ModelExecutionStatus::Partial;
}

QString uiStatus(ai::application::ModelExecutionStatus status)
{
    return status == ai::application::ModelExecutionStatus::Pending
        ? QStringLiteral("queued")
        : QString::fromLatin1(ai::application::toString(status));
}

} // namespace

ComparisonResultListModel::ComparisonResultListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ComparisonResultListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ComparisonResultListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    const auto& execution = row.execution;
    const bool qualityAvailable = row.latestEvaluation.has_value();
    switch (role) {
    case ResultIdRole: return QString::fromStdString(execution.id);
    case ProviderIdRole: return QString::fromStdString(execution.providerId);
    case ProviderNameRole: return QString::fromStdString(row.providerName);
    case ModelIdRole: return QString::fromStdString(execution.modelId);
    case ModelNameRole: return QString::fromStdString(row.modelName);
    case ExecutionRole: return QString::fromStdString(execution.role);
    case StatusRole: return uiStatus(execution.status);
    case ContentRole:
        return QString::fromStdString(
            execution.outputContent.empty()
                ? execution.outputPreview : execution.outputContent);
    case ErrorTextRole: return QString::fromStdString(execution.errorMessage);
    case StrengthsRole:
        return qualityAvailable
            ? QString::fromStdString(row.latestEvaluation->explanation) : QString();
    case WarningsRole:
        return execution.status == ai::application::ModelExecutionStatus::Partial
                || execution.status == ai::application::ModelExecutionStatus::Failed
            ? QString::fromStdString(execution.errorMessage) : QString();
    case InputTokensRole: return QVariant::fromValue<qulonglong>(execution.inputTokens);
    case OutputTokensRole: return QVariant::fromValue<qulonglong>(execution.outputTokens);
    case CostMicrosRole: return QVariant::fromValue<qulonglong>(execution.costMicros);
    case CostPriceKnownRole: return execution.costPriceKnown;
    case TotalCostTextRole: return costText(execution.costMicros, execution.costPriceKnown);
    case LatencyMsRole: return QVariant::fromValue<qulonglong>(execution.latencyMs);
    case LatencyTextRole: return latencyText(execution.latencyMs);
    case QualityAvailableRole: return qualityAvailable;
    case QualityScoreRole:
        return qualityAvailable
            ? static_cast<double>(row.latestEvaluation->scoreBasisPoints) / 100.0 : 0.0;
    case QualityBasisPointsRole:
        return qualityAvailable ? row.latestEvaluation->scoreBasisPoints : 0U;
    case QualitySourceRole:
        return qualityAvailable
            ? QStringLiteral("%1 · %2")
                  .arg(QString::fromLatin1(ai::application::toString(
                           row.latestEvaluation->evaluatorKind)),
                       QString::fromStdString(row.latestEvaluation->rubricVersion))
            : QString();
    case IsPreferredRole: return row.preferred;
    case SelectedForSynthesisRole: return row.selectedForSynthesis;
    case CanRetryRole: return false; // enabled only when the execution worker owns retry
    case CanSelectForSynthesisRole: return reviewable(execution.status);
    default: return {};
    }
}

QHash<int, QByteArray> ComparisonResultListModel::roleNames() const
{
    return {
        {ResultIdRole, "resultId"}, {ProviderIdRole, "providerId"},
        {ProviderNameRole, "providerName"}, {ModelIdRole, "modelId"},
        {ModelNameRole, "modelName"}, {ExecutionRole, "executionRole"},
        {StatusRole, "status"}, {ContentRole, "content"},
        {ErrorTextRole, "errorText"}, {StrengthsRole, "strengths"},
        {WarningsRole, "warnings"}, {InputTokensRole, "inputTokens"},
        {OutputTokensRole, "outputTokens"}, {CostMicrosRole, "costMicros"},
        {CostPriceKnownRole, "costPriceKnown"},
        {TotalCostTextRole, "totalCostText"}, {LatencyMsRole, "latencyMs"},
        {LatencyTextRole, "latencyText"}, {QualityAvailableRole, "qualityAvailable"},
        {QualityScoreRole, "qualityScore"}, {QualityBasisPointsRole, "qualityBasisPoints"},
        {QualitySourceRole, "qualitySource"}, {IsPreferredRole, "isPreferred"},
        {SelectedForSynthesisRole, "selectedForSynthesis"},
        {CanRetryRole, "canRetry"},
        {CanSelectForSynthesisRole, "canSelectForSynthesis"}
    };
}

void ComparisonResultListModel::replace(
    std::vector<ai::application::ComparisonResultReadModel> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}
