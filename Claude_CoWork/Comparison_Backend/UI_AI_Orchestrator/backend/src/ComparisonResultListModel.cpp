#include <ai/ui/ComparisonResultListModel.hpp>

#include <QStringList>

namespace {

QString latencyText(std::int64_t latencyMs)
{
    if (latencyMs <= 0) return QStringLiteral("—");
    if (latencyMs < 1000) return QStringLiteral("%1ms").arg(latencyMs);
    return QStringLiteral("%1s").arg(QString::number(latencyMs / 1000.0, 'f', 1));
}

QStringList toQStringList(const std::vector<std::string>& v)
{
    QStringList out;
    out.reserve(static_cast<qsizetype>(v.size()));
    for (const auto& s : v) {
        out.append(QString::fromStdString(s));
    }
    return out;
}

QString statusString(ai::application::ComparisonResultStatus s)
{
    return QString::fromLatin1(ai::application::toString(s));
}

} // namespace

ComparisonResultListModel::ComparisonResultListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ComparisonResultListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

QVariant ComparisonResultListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto& r = results_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ResultIdRole:           return QString::fromStdString(r.id);
    case ProviderIdRole:         return QString::fromStdString(r.providerId);
    case ProviderNameRole:       return QString::fromStdString(r.providerName);
    case ModelNameRole:          return QString::fromStdString(r.modelName);
    case StatusRole:             return statusString(r.status);
    case ContentRole:            return QString::fromStdString(r.content);
    case StrengthsRole:          return toQStringList(r.strengths);
    case WarningsRole:           return toQStringList(r.warnings);
    case InputTokensRole:        return static_cast<int>(r.inputTokens);
    case OutputTokensRole:       return static_cast<int>(r.outputTokens);
    case TotalCostRole:          return QString::fromStdString(r.totalCost.empty() ? "$0.000" : r.totalCost);
    case LatencyTextRole:        return latencyText(r.latencyMs);
    case QualityScoreRole:       return r.qualityScore;
    case IsBestRole:             return r.isBest;
    case SelectedForSynthesisRole: return r.selectedForSynthesis;
    default:                     return {};
    }
}

QHash<int, QByteArray> ComparisonResultListModel::roleNames() const
{
    return {
        {ResultIdRole,              "resultId"},
        {ProviderIdRole,            "providerId"},
        {ProviderNameRole,          "providerName"},
        {ModelNameRole,             "modelName"},
        {StatusRole,                "status"},
        {ContentRole,               "content"},
        {StrengthsRole,             "strengths"},
        {WarningsRole,              "warnings"},
        {InputTokensRole,           "inputTokens"},
        {OutputTokensRole,          "outputTokens"},
        {TotalCostRole,             "totalCost"},
        {LatencyTextRole,           "latencyText"},
        {QualityScoreRole,          "qualityScore"},
        {IsBestRole,                "isBest"},
        {SelectedForSynthesisRole,  "selectedForSynthesis"}
    };
}

void ComparisonResultListModel::replace(std::vector<ai::application::ComparisonResult> results)
{
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

void ComparisonResultListModel::updateResult(const ai::application::ComparisonResult& updated)
{
    for (std::size_t i = 0; i < results_.size(); ++i) {
        if (results_[i].id == updated.id) {
            results_[i] = updated;
            const QModelIndex idx = createIndex(static_cast<int>(i), 0);
            emit dataChanged(idx, idx);
            return;
        }
    }
}
