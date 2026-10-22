#include <ai/ui/ModelListModel.hpp>

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

ModelListModel::ModelListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ModelListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(models_.size());
}

QVariant ModelListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& model = models_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ModelIdRole: return QString::fromStdString(model.id);
    case ProviderIdRole: return QString::fromStdString(model.providerId);
    case ApiModelIdRole: return QString::fromStdString(model.apiModelId);
    case DisplayNameRole: return QString::fromStdString(model.displayName);
    case ContextWindowTokensRole: return QVariant::fromValue<qulonglong>(model.contextWindowTokens);
    case MaxOutputTokensRole: return QVariant::fromValue<qulonglong>(model.maxOutputTokens);
    case InputPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.inputPriceMicrosPerMillionTokens);
    case OutputPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.outputPriceMicrosPerMillionTokens);
    case CacheWritePriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.cacheWritePriceMicrosPerMillionTokens);
    case CacheReadPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.cacheReadPriceMicrosPerMillionTokens);
    case EnabledRole: return model.enabled;
    case DefaultRole: return model.isDefault;
    case CreatedAtRole: return toDateTime(model.createdAt);
    case UpdatedAtRole: return toDateTime(model.updatedAt);
    case LongContextEnabledRole: return model.longContextPricing.enabled();
    case LongContextThresholdInputTokensRole:
        return QVariant::fromValue<qulonglong>(model.longContextPricing.thresholdInputTokens);
    case LongContextInputPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.longContextPricing.inputPriceMicrosPerMillionTokens);
    case LongContextOutputPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.longContextPricing.outputPriceMicrosPerMillionTokens);
    case LongContextCacheWritePriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.longContextPricing.cacheWritePriceMicrosPerMillionTokens);
    case LongContextCacheReadPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.longContextPricing.cacheReadPriceMicrosPerMillionTokens);
    case CacheWriteOneHourPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(model.cacheWriteOneHourPriceMicrosPerMillionTokens);
    case LongContextCacheWriteOneHourPriceMicrosRole:
        return QVariant::fromValue<qulonglong>(
            model.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens);
    default: return {};
    }
}

QHash<int, QByteArray> ModelListModel::roleNames() const
{
    return {
        {ModelIdRole, "modelId"},
        {ProviderIdRole, "providerId"},
        {ApiModelIdRole, "apiModelId"},
        {DisplayNameRole, "displayName"},
        {ContextWindowTokensRole, "contextWindowTokens"},
        {MaxOutputTokensRole, "maxOutputTokens"},
        {InputPriceMicrosRole, "inputPriceMicros"},
        {OutputPriceMicrosRole, "outputPriceMicros"},
        {CacheWritePriceMicrosRole, "cacheWritePriceMicros"},
        {CacheReadPriceMicrosRole, "cacheReadPriceMicros"},
        {EnabledRole, "enabled"},
        {DefaultRole, "isDefault"},
        {CreatedAtRole, "createdAt"},
        {UpdatedAtRole, "updatedAt"},
        {LongContextEnabledRole, "longContextEnabled"},
        {LongContextThresholdInputTokensRole, "longContextThresholdInputTokens"},
        {LongContextInputPriceMicrosRole, "longContextInputPriceMicros"},
        {LongContextOutputPriceMicrosRole, "longContextOutputPriceMicros"},
        {LongContextCacheWritePriceMicrosRole, "longContextCacheWritePriceMicros"},
        {LongContextCacheReadPriceMicrosRole, "longContextCacheReadPriceMicros"},
        {CacheWriteOneHourPriceMicrosRole, "cacheWriteOneHourPriceMicros"},
        {LongContextCacheWriteOneHourPriceMicrosRole,
            "longContextCacheWriteOneHourPriceMicros"}
    };
}

void ModelListModel::replace(std::vector<ai::application::Model> models)
{
    beginResetModel();
    models_ = std::move(models);
    endResetModel();
}
