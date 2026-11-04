#include <ai/ui/ChatCostProjection.hpp>

#include <ai/ui/ProviderExecutionAnalyticsModel.hpp>

#include <QCoreApplication>
#include <QVariant>

namespace {

QString costStateOf(const ai::application::ExecutionAggregate& totals)
{
    return QString::fromLatin1(toString(totals.priceState()));
}

QVariantMap emptyRecordedCost(const QString& conversationId, const QString& state)
{
    return QVariantMap {
        {QStringLiteral("conversationId"), conversationId},
        {QStringLiteral("state"), state},
        {QStringLiteral("costText"), QString {}},
        {QStringLiteral("costMicros"), QVariant::fromValue<qulonglong>(0)},
        {QStringLiteral("executionCount"), QVariant::fromValue<qulonglong>(0)},
        {QStringLiteral("unknownPriceExecutionCount"), QVariant::fromValue<qulonglong>(0)},
        {QStringLiteral("errorCode"), QString {}},
        {QStringLiteral("errorMessage"), QString {}}};
}

} // namespace

QVariantMap chatRecordedCostUnavailable(const QString& conversationId, const ai::common::Error& error)
{
    QVariantMap result = emptyRecordedCost(conversationId, QStringLiteral("unavailable"));
    result[QStringLiteral("errorCode")] = QString::fromStdString(error.code);
    result[QStringLiteral("errorMessage")] = QString::fromStdString(error.message);
    return result;
}

QVariantMap chatRecordedCostMap(
    const QString& conversationId,
    const ai::common::Result<ai::application::ExecutionAnalyticsSnapshot>& snapshot)
{
    if (!snapshot.success()) return chatRecordedCostUnavailable(conversationId, *snapshot.error());
    const auto& totals = snapshot.value().totals;
    QVariantMap result = emptyRecordedCost(conversationId, costStateOf(totals));
    result[QStringLiteral("costText")] = executionCostText(totals);
    result[QStringLiteral("costMicros")] = QVariant::fromValue<qulonglong>(totals.costMicros);
    result[QStringLiteral("executionCount")] = QVariant::fromValue<qulonglong>(totals.executionCount);
    result[QStringLiteral("unknownPriceExecutionCount")] =
        QVariant::fromValue<qulonglong>(totals.unknownPriceExecutionCount);
    return result;
}

QVariantList chatModelMetricRows(
    const ai::application::ExecutionAnalyticsSnapshot& snapshot,
    ai::application::ProviderCatalogService& catalog)
{
    QVariantList rows;
    for (const auto& provider : snapshot.providers) {
        const auto providerFound = catalog.findProvider(provider.providerId);
        const QString providerName = providerFound.success() && providerFound.value()
            ? QString::fromStdString(providerFound.value()->name)
            : QCoreApplication::translate("ChatCostProjection", "Unknown provider");
        for (const auto& modelAggregate : provider.models) {
            const auto& totals = modelAggregate.totals;
            const auto modelFound = catalog.findModel(modelAggregate.modelId);
            rows.push_back(QVariantMap {
                {QStringLiteral("providerId"), QString::fromStdString(provider.providerId)},
                {QStringLiteral("modelId"), QString::fromStdString(modelAggregate.modelId)},
                {QStringLiteral("providerName"), providerName},
                {QStringLiteral("modelName"), modelFound.success() && modelFound.value()
                    ? QString::fromStdString(modelFound.value()->displayName)
                    : QCoreApplication::translate("ChatCostProjection", "Unknown model")},
                {QStringLiteral("inputTokens"), QVariant::fromValue<qulonglong>(totals.inputTokens)},
                {QStringLiteral("outputTokens"), QVariant::fromValue<qulonglong>(totals.outputTokens)},
                {QStringLiteral("totalTokens"),
                    QVariant::fromValue<qulonglong>(totals.inputTokens + totals.outputTokens)},
                {QStringLiteral("executionCount"), QVariant::fromValue<qulonglong>(totals.executionCount)},
                {QStringLiteral("costState"), costStateOf(totals)},
                {QStringLiteral("costText"), executionCostText(totals)},
                {QStringLiteral("unknownPriceExecutionCount"),
                    QVariant::fromValue<qulonglong>(totals.unknownPriceExecutionCount)},
                {QStringLiteral("averageLatency"), totals.averageLatencyMs > 0
                    ? QCoreApplication::translate("ChatCostProjection", "%1 ms").arg(totals.averageLatencyMs)
                    : QString {}}});
        }
    }
    return rows;
}
