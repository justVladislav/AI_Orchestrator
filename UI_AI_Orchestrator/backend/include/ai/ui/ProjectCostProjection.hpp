#pragma once

#include <ai/application/analytics/ProjectCostTypes.hpp>

#include <QString>
#include <QVariantMap>

// A1-min: how the Qt layer words the project cost estimate for QML. Pure functions over the Application aggregates: no QObject, no
// I/O, no pricing. QML receives these values and the counters and chooses the phrases; it never reads a raw amount or formats money.
namespace ai::ui::project_cost {

// "none" | "known" | "partial" | "unknown": the same vocabulary as every other cost surface.
[[nodiscard]] QString priceStateName(const ai::application::ProjectCostAggregate& aggregate);

// The Backend-formatted amount ("$0.1234", two decimals from $100). EMPTY when the price is unknown or there is nothing recorded: an
// unknown price is never rendered as "$0". For a partial aggregate it is the recorded part of an incomplete estimate.
[[nodiscard]] QString costText(const ai::application::ProjectCostAggregate& aggregate);

// True for partial and unknown: the amount (if any) is not a precise total.
[[nodiscard]] bool priceIncomplete(const ai::application::ProjectCostAggregate& aggregate);

// One aggregate as a QML map: the counters of BOTH sources kept apart (ledger executions are not compactions), the token total of
// both, the price state and text. Used for the model rows of a provider.
[[nodiscard]] QVariantMap aggregateMap(const ai::application::ProjectCostAggregate& aggregate);

} // namespace ai::ui::project_cost
