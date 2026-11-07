#include <ai/ui/ComparisonCostProjection.hpp>

#include <QLocale>
#include <QVariant>

QVariantMap comparisonSummaryCostFields(const ai::application::ComparisonSummaryReadModel& summary)
{
    using ai::application::ExecutionPriceState;
    const ExecutionPriceState state = summary.priceState();
    const bool hasAmount = state == ExecutionPriceState::Known || state == ExecutionPriceState::Partial;
    return QVariantMap {
        {QStringLiteral("totalCostText"), hasAmount
            ? QStringLiteral("$%1").arg(QLocale::c().toString(
                  static_cast<double>(summary.costMicros) / 1'000'000.0, 'f', 6))
            : QString {}},
        {QStringLiteral("costState"), QString::fromLatin1(toString(state))},
        {QStringLiteral("costResultCount"), QVariant::fromValue<qulonglong>(summary.executionCount)},
        {QStringLiteral("unknownPriceResultCount"),
            QVariant::fromValue<qulonglong>(summary.unknownPriceExecutionCount)}};
}
