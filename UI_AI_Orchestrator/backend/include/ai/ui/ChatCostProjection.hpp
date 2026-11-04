#pragma once

#include <ai/application/analytics/ExecutionAnalyticsTypes.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>
#include <ai/common/Result.hpp>

#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Qt projection of the Backend's model-execution ledger for ONE Chat conversation. Everything here only FORMATS what the
// Backend aggregated (ExecutionAnalyticsService::sourceSnapshot over ModelExecutionSource::Chat): the completeness state is
// the Backend's ExecutionAggregate::priceState(), money is never summed here, and nothing is read from the resettable
// conversation counters or from message rows. Split out of ChatController so every state is testable without a provider.

// The conversation's recorded cost: state (none|known|partial|unknown|unavailable), costText ("" unless the state carries an
// amount - an unknown or unavailable cost is never "$0"), costMicros, executionCount, unknownPriceExecutionCount and, for
// "unavailable" only, errorCode/errorMessage. A failed read never degrades into a guessed exact amount.
[[nodiscard]] QVariantMap chatRecordedCostMap(
    const QString& conversationId,
    const ai::common::Result<ai::application::ExecutionAnalyticsSnapshot>& snapshot);

// The same shape for "the ledger could not be asked at all" (no analytics service in the composition).
[[nodiscard]] QVariantMap chatRecordedCostUnavailable(
    const QString& conversationId, const ai::common::Error& error);

// Per provider/model rows over the same snapshot: providerId, modelId, providerName, modelName, inputTokens, outputTokens,
// totalTokens, executionCount (recorded attempts, NOT visible messages), costState, costText,
// unknownPriceExecutionCount, averageLatency.
[[nodiscard]] QVariantList chatModelMetricRows(
    const ai::application::ExecutionAnalyticsSnapshot& snapshot,
    ai::application::ProviderCatalogService& catalog);
