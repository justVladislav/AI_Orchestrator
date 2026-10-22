#include "ai/application/ComparisonReadModelService.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

common::Error makeError(std::string code, std::string message)
{
    return common::Error {std::move(code), std::move(message)};
}

bool isReviewable(ModelExecutionStatus status) noexcept
{
    return status == ModelExecutionStatus::Completed
        || status == ModelExecutionStatus::Partial;
}

} // namespace

ComparisonReadModelService::ComparisonReadModelService(
    ComparisonService& comparisons,
    ModelExecutionService& executions,
    ProviderCatalogService& catalog)
    : comparisons_(comparisons), executions_(executions), catalog_(catalog)
{
}

common::Result<ComparisonSessionReadModel> ComparisonReadModelService::session(
    const ComparisonSessionId& id) const
{
    const auto foundSession = comparisons_.findSession(id);
    if (!foundSession.success())
        return common::Result<ComparisonSessionReadModel>::failure(*foundSession.error());
    if (!foundSession.value().has_value()) {
        return common::Result<ComparisonSessionReadModel>::failure(
            makeError("comparison.not_found", "Comparison session was not found"));
    }

    const auto executions = comparisons_.listResults(id);
    if (!executions.success())
        return common::Result<ComparisonSessionReadModel>::failure(*executions.error());
    const auto providers = catalog_.listProviders();
    if (!providers.success())
        return common::Result<ComparisonSessionReadModel>::failure(*providers.error());
    const auto models = catalog_.listModels();
    if (!models.success())
        return common::Result<ComparisonSessionReadModel>::failure(*models.error());

    std::unordered_map<ProviderId, std::string> providerNames;
    for (const auto& provider : providers.value())
        providerNames.emplace(provider.id, provider.name);
    std::unordered_map<ModelId, std::string> modelNames;
    for (const auto& model : models.value())
        modelNames.emplace(model.id, model.displayName);

    ComparisonSessionReadModel result;
    result.session = *foundSession.value();
    result.results.reserve(executions.value().size());
    const std::unordered_set<ModelExecutionId> selected(
        result.session.synthesisSelection.begin(),
        result.session.synthesisSelection.end());

    std::uint64_t latencyTotal = 0;
    std::uint64_t latencyCount = 0;
    std::vector<const ComparisonResultReadModel*> reviewable;
    for (const auto& execution : executions.value()) {
        ComparisonResultReadModel row;
        row.execution = execution;
        const auto provider = providerNames.find(execution.providerId);
        row.providerName = provider == providerNames.end()
            ? execution.providerId : provider->second;
        const auto model = modelNames.find(execution.modelId);
        row.modelName = model == modelNames.end()
            ? execution.modelId : model->second;
        row.preferred = result.session.preferredExecutionId == execution.id;
        row.selectedForSynthesis = selected.contains(execution.id);

        const auto evaluations = executions_.listEvaluations(execution.id);
        if (!evaluations.success())
            return common::Result<ComparisonSessionReadModel>::failure(*evaluations.error());
        if (!evaluations.value().empty())
            row.latestEvaluation = evaluations.value().back();

        result.summary.inputTokens += execution.inputTokens;
        result.summary.outputTokens += execution.outputTokens;
        result.summary.costMicros += execution.costMicros;
        if (execution.latencyMs > 0) {
            latencyTotal += execution.latencyMs;
            ++latencyCount;
        }
        result.results.push_back(std::move(row));
    }
    result.summary.averageLatencyMs = latencyCount == 0
        ? 0 : latencyTotal / latencyCount;

    for (const auto& row : result.results) {
        if (isReviewable(row.execution.status)) reviewable.push_back(&row);
    }
    result.summary.qualityComplete = reviewable.size() >= 2
        && std::all_of(reviewable.begin(), reviewable.end(), [](const auto* row) {
            return row->latestEvaluation.has_value();
        });
    result.summary.synthesisEnabled = result.session.synthesisSelection.size() >= 2
        && std::all_of(result.session.synthesisSelection.begin(),
            result.session.synthesisSelection.end(), [&](const auto& selectedId) {
                return std::any_of(reviewable.begin(), reviewable.end(),
                    [&](const auto* row) { return row->execution.id == selectedId; });
            });

    if (result.session.preferredExecutionId.has_value()) {
        result.summary.recommendedExecutionId = result.session.preferredExecutionId;
        result.summary.recommendationSource = "preferred";
    } else if (result.summary.qualityComplete) {
        std::sort(reviewable.begin(), reviewable.end(), [](const auto* left, const auto* right) {
            const auto leftScore = left->latestEvaluation->scoreBasisPoints;
            const auto rightScore = right->latestEvaluation->scoreBasisPoints;
            return leftScore == rightScore
                ? left->execution.position < right->execution.position
                : leftScore > rightScore;
        });
        const bool uniqueWinner = reviewable.size() == 1
            || reviewable[0]->latestEvaluation->scoreBasisPoints
                != reviewable[1]->latestEvaluation->scoreBasisPoints;
        if (uniqueWinner) {
            result.summary.recommendedExecutionId = reviewable.front()->execution.id;
            result.summary.recommendationSource = "quality";
        }
    }
    return common::Result<ComparisonSessionReadModel>::ok(std::move(result));
}

} // namespace ai::application
