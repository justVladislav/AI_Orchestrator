#pragma once

#include <ai/application/comparison/ComparisonReadModelService.hpp>

#include <QVariantMap>

// The cost fields of the Comparison summary map, projected from the Backend's summary and nothing else:
//   totalCostText            the recorded amount ("$0.000012") ONLY for a known or partial total; "" for an unknown total
//                            (it is not "$0") and for no results at all
//   costState                none | known | partial | unknown - the Backend's ComparisonSummaryReadModel::priceState(), over
//                            exactly the results the amount was summed from (sticky: one unpriced result keeps it incomplete)
//   costResultCount          how many results the sum covers (every result, whatever its status)
//   unknownPriceResultCount  how many of them have an incomplete stored price
// The QML only chooses the wording; it neither sums nor classifies.
[[nodiscard]] QVariantMap comparisonSummaryCostFields(
    const ai::application::ComparisonSummaryReadModel& summary);
