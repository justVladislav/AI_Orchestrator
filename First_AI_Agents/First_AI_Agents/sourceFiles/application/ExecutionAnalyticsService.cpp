#include "ai/application/ExecutionAnalyticsService.hpp"

#include "ai/application/CostCalculation.hpp"

#include <algorithm>
#include <iterator>
#include <unordered_map>

namespace ai::application {
namespace {

struct EffectiveCost {
    std::uint64_t costMicros {0};
    // False when costMicros is only what could be priced (a lower bound), or when no amount can
    // be established at all.
    bool priceKnown {true};
};

// The cost one execution row contributes to an aggregate.
//
// The row's stored (costMicros, costPriceKnown) pair is the durable pricing result recorded when
// the execution finished, and it is AUTHORITATIVE: a later catalog edit must not rewrite history.
//   1. costMicros > 0              the stored amount with the stored price state.
//   2. costPriceKnown == false     the stored lower bound - including ZERO - never revalued: an
//                                  unpriced execution is not free, and the catalog may differ now.
//   3. cost 0, price known, no usage   an exact zero (the row never reached a provider).
// Only a row that carries real usage yet no durable amount (a historical row from before costs
// were stored; costPriceKnown defaults to true) is derived again, and only where that is exact:
//   4. the model is flat           the flat formula over the row's input/output tokens.
//   5. the model has a long-context tier, or is gone    0 and UNKNOWN - never a known zero. A row
//                                  holds ONE aggregated input_tokens, which says nothing about which
//                                  of its Responses were long: ten short Responses of 50,000 do not
//                                  become one long Response of 500,000, and a flat price would
//                                  understate any that were.
EffectiveCost effectiveCost(const ModelExecution& execution,
    const std::unordered_map<ModelId, const Model*>& models)
{
    if (execution.costMicros > 0) return {execution.costMicros, execution.costPriceKnown};
    if (!execution.costPriceKnown) return {0, false};
    if (execution.inputTokens == 0 && execution.outputTokens == 0) return {0, true};
    // Cache audit fields did not exist on the historical rows handled below.
    // Their presence proves that a known zero is a durable modern result (for
    // example a sub-micro cache read), not a missing amount to revalue through
    // today's catalog.
    if (execution.cacheCreationInputTokens > 0 || execution.cacheReadInputTokens > 0
        || execution.cacheCreationDuration.known) {
        return {0, true};
    }
    const auto found = models.find(execution.modelId);
    if (found == models.end() || found->second->longContextPricing.enabled()) return {0, false};
    // A legacy row carries no cache-token breakdown. With the tier disabled nothing is ever
    // classified long, so the single-response function is exactly the flat formula.
    models::TokenUsage usage;
    usage.inputTokens = execution.inputTokens;
    usage.outputTokens = execution.outputTokens;
    const auto flat = computeSingleResponseCostMicros(*found->second, usage);
    return {flat.costMicros, flat.priceKnown};
}

void addExecution(ExecutionAggregate& aggregate, const ModelExecution& execution,
    const EffectiveCost& cost)
{
    ++aggregate.executionCount;
    switch (execution.status) {
    case ModelExecutionStatus::Pending: ++aggregate.pendingCount; break;
    case ModelExecutionStatus::Running: ++aggregate.runningCount; break;
    case ModelExecutionStatus::Completed: ++aggregate.completedCount; break;
    case ModelExecutionStatus::Partial: ++aggregate.partialCount; break;
    case ModelExecutionStatus::Failed: ++aggregate.failedCount; break;
    case ModelExecutionStatus::Cancelled: ++aggregate.cancelledCount; break;
    }
    aggregate.inputTokens += execution.inputTokens;
    aggregate.outputTokens += execution.outputTokens;
    aggregate.costMicros += cost.costMicros;
    if (!cost.priceKnown) ++aggregate.unknownPriceExecutionCount;
    aggregate.totalLatencyMs += execution.latencyMs;
}

void finishAverage(ExecutionAggregate& aggregate)
{
    aggregate.averageLatencyMs = aggregate.executionCount == 0
        ? 0
        : aggregate.totalLatencyMs / aggregate.executionCount;
}

} // namespace

ExecutionAnalyticsService::ExecutionAnalyticsService(
    IModelExecutionRepository& repository, IModelRepository& models)
    : repository_(repository), models_(models)
{
}

common::Result<ExecutionAnalyticsSnapshot> ExecutionAnalyticsService::projectSnapshot(
    const ProjectId& projectId,
    std::optional<std::chrono::system_clock::time_point> fromInclusive,
    std::optional<std::chrono::system_clock::time_point> toExclusive) const
{
    const auto executions = repository_.listProjectExecutions(projectId);
    if (!executions.success()) {
        return common::Result<ExecutionAnalyticsSnapshot>::failure(*executions.error());
    }
    const auto models = models_.listAll();
    if (!models.success()) {
        return common::Result<ExecutionAnalyticsSnapshot>::failure(*models.error());
    }

    std::vector<ModelExecution> filteredExecutions;
    const auto* selectedExecutions = &executions.value();
    if (fromInclusive.has_value() || toExclusive.has_value()) {
        filteredExecutions.reserve(executions.value().size());
        std::copy_if(executions.value().begin(), executions.value().end(),
            std::back_inserter(filteredExecutions),
            [fromInclusive, toExclusive](const ModelExecution& execution) {
                return (!fromInclusive.has_value() || execution.createdAt >= *fromInclusive)
                    && (!toExclusive.has_value() || execution.createdAt < *toExclusive);
            });
        selectedExecutions = &filteredExecutions;
    }
    return common::Result<ExecutionAnalyticsSnapshot>::ok(
        aggregate(*selectedExecutions, models.value()));
}

common::Result<ExecutionAnalyticsSnapshot> ExecutionAnalyticsService::sourceSnapshot(
    ModelExecutionSource source, const std::string& sourceId) const
{
    const auto executions = repository_.listExecutions(source, sourceId);
    if (!executions.success()) {
        return common::Result<ExecutionAnalyticsSnapshot>::failure(*executions.error());
    }
    const auto models = models_.listAll();
    if (!models.success()) {
        return common::Result<ExecutionAnalyticsSnapshot>::failure(*models.error());
    }
    return common::Result<ExecutionAnalyticsSnapshot>::ok(
        aggregate(executions.value(), models.value()));
}

ExecutionAnalyticsSnapshot ExecutionAnalyticsService::aggregate(
    const std::vector<ModelExecution>& executions,
    const std::vector<Model>& models)
{
    ExecutionAnalyticsSnapshot snapshot;
    std::unordered_map<ModelId, const Model*> modelsById;
    modelsById.reserve(models.size());
    for (const auto& model : models) modelsById.emplace(model.id, &model);
    std::unordered_map<ProviderId, std::size_t> providerIndexes;
    std::vector<std::unordered_map<ModelId, std::size_t>> modelIndexes;
    for (const auto& execution : executions) {
        const auto cost = effectiveCost(execution, modelsById);
        addExecution(snapshot.totals, execution, cost);
        const auto [it, inserted] = providerIndexes.emplace(
            execution.providerId, snapshot.providers.size());
        if (inserted) {
            ProviderExecutionAggregate provider;
            provider.providerId = execution.providerId;
            snapshot.providers.push_back(std::move(provider));
            modelIndexes.emplace_back();
        }
        auto& provider = snapshot.providers[it->second];
        addExecution(provider.totals, execution, cost);
        auto& indexes = modelIndexes[it->second];
        const auto [modelIt, modelInserted] = indexes.emplace(
            execution.modelId, provider.models.size());
        if (modelInserted) {
            ProviderExecutionAggregate::ModelAggregate model;
            model.modelId = execution.modelId;
            provider.models.push_back(std::move(model));
        }
        addExecution(provider.models[modelIt->second].totals, execution, cost);
    }

    finishAverage(snapshot.totals);
    for (auto& provider : snapshot.providers) {
        finishAverage(provider.totals);
        for (auto& model : provider.models) finishAverage(model.totals);
        std::sort(provider.models.begin(), provider.models.end(),
            [](const auto& left, const auto& right) {
                if (left.totals.costMicros != right.totals.costMicros)
                    return left.totals.costMicros > right.totals.costMicros;
                if (left.totals.executionCount != right.totals.executionCount)
                    return left.totals.executionCount > right.totals.executionCount;
                return left.modelId < right.modelId;
            });
    }
    std::sort(snapshot.providers.begin(), snapshot.providers.end(),
        [](const auto& left, const auto& right) {
            if (left.totals.costMicros != right.totals.costMicros)
                return left.totals.costMicros > right.totals.costMicros;
            if (left.totals.executionCount != right.totals.executionCount)
                return left.totals.executionCount > right.totals.executionCount;
            return left.providerId < right.providerId;
        });
    return snapshot;
}

} // namespace ai::application
