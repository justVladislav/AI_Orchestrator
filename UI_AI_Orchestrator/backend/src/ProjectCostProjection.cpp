#include "ai/ui/ProjectCostProjection.hpp"

#include <QLocale>
#include <QVariant>

namespace ai::ui::project_cost {

using ai::application::ExecutionPriceState;
using ai::application::ProjectCostAggregate;

QString priceStateName(const ProjectCostAggregate& aggregate)
{
    return QString::fromLatin1(toString(aggregate.priceState()));
}

QString costText(const ProjectCostAggregate& aggregate)
{
    const auto state = aggregate.priceState();
    if (state != ExecutionPriceState::Known && state != ExecutionPriceState::Partial) return {};
    // Same rule as the other cost surfaces: 4 decimals, 2 from $100 (a separate copy of four lines on purpose: the older
    // projections are not touched by this package).
    const double dollars = static_cast<double>(aggregate.costMicros()) / 1'000'000.0;
    return QStringLiteral("$%1").arg(QLocale::c().toString(dollars, 'f', dollars >= 100.0 ? 2 : 4));
}

bool priceIncomplete(const ProjectCostAggregate& aggregate)
{
    const auto state = aggregate.priceState();
    return state == ExecutionPriceState::Partial || state == ExecutionPriceState::Unknown;
}

QVariantMap aggregateMap(const ProjectCostAggregate& aggregate)
{
    const auto count = [](std::uint64_t value) { return QVariant::fromValue<qulonglong>(value); };
    QVariantMap map;
    map.insert(QStringLiteral("ledgerExecutionCount"), count(aggregate.ledgerExecutionCount()));
    map.insert(QStringLiteral("compactionRecordCount"), count(aggregate.compactionRecordCount()));
    map.insert(QStringLiteral("compactionNoneCount"), count(aggregate.compactionNoneCount()));
    map.insert(QStringLiteral("pricingEvidenceCount"), count(aggregate.pricingEvidenceCount()));
    map.insert(QStringLiteral("incompletePriceCount"), count(aggregate.incompletePriceCount()));
    map.insert(QStringLiteral("inputTokens"), count(aggregate.inputTokens()));
    map.insert(QStringLiteral("outputTokens"), count(aggregate.outputTokens()));
    map.insert(QStringLiteral("ledgerAverageLatencyMs"), count(aggregate.ledgerAverageLatencyMs()));
    map.insert(QStringLiteral("priceState"), priceStateName(aggregate));
    map.insert(QStringLiteral("costText"), costText(aggregate));
    map.insert(QStringLiteral("priceIncomplete"), priceIncomplete(aggregate));
    return map;
}

} // namespace ai::ui::project_cost
