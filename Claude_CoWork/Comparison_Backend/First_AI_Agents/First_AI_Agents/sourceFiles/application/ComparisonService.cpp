#include "ai/application/ComparisonService.hpp"

#include <chrono>
#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

std::chrono::system_clock::time_point now()
{
    return std::chrono::system_clock::now();
}

} // namespace

ComparisonService::ComparisonService(
    IComparisonRepository& repository,
    IProjectRepository& projects,
    std::function<std::string()> idGenerator)
    : repository_(repository)
    , projects_(projects)
    , idGenerator_(std::move(idGenerator))
{
}

common::Result<ComparisonSession> ComparisonService::createSession(CreateComparisonRequest request)
{
    if (request.slots.empty()) {
        return common::Result<ComparisonSession>::failure(
            error("comparison.no_slots", "At least one provider slot is required"));
    }

    const auto projectResult = projects_.findById(request.projectId);
    if (!projectResult.success()) {
        return common::Result<ComparisonSession>::failure(*projectResult.error());
    }
    if (!projectResult.value().has_value()) {
        return common::Result<ComparisonSession>::failure(
            error("comparison.project_not_found", "Project not found: " + request.projectId));
    }

    const auto ts = now();

    ComparisonSession session;
    session.id = idGenerator_();
    session.projectId = std::move(request.projectId);
    session.title = std::move(request.title);
    session.task = std::move(request.task);
    session.status = ComparisonStatus::Active;
    session.createdAt = ts;
    session.updatedAt = ts;

    const auto insertedSession = repository_.insertSession(session);
    if (!insertedSession.success()) {
        return insertedSession;
    }

    for (const auto& slot : request.slots) {
        ComparisonResult result;
        result.id = idGenerator_();
        result.comparisonId = session.id;
        result.providerId = slot.providerId;
        result.providerName = slot.providerName;
        result.modelId = slot.modelId;
        result.modelName = slot.modelName;
        result.providerKind = slot.providerKind;
        result.apiModelId = slot.apiModelId;
        result.status = ComparisonResultStatus::Pending;
        result.isBest = false;
        result.selectedForSynthesis = false;

        const auto insertedResult = repository_.insertResult(std::move(result));
        if (!insertedResult.success()) {
            return common::Result<ComparisonSession>::failure(*insertedResult.error());
        }
    }

    return insertedSession;
}

common::Result<std::vector<ComparisonSession>> ComparisonService::listSessionsByProject(
    const ProjectId& projectId) const
{
    return repository_.listSessionsByProject(projectId);
}

common::Result<std::vector<ComparisonResult>> ComparisonService::listResultsBySession(
    const ComparisonId& comparisonId) const
{
    return repository_.listResultsBySession(comparisonId);
}

common::Result<std::optional<ComparisonResult>> ComparisonService::findResult(
    const ComparisonResultId& resultId) const
{
    return repository_.findResultById(resultId);
}

common::Result<void> ComparisonService::setResultRunning(const ComparisonResultId& resultId)
{
    const auto found = repository_.findResultById(resultId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("comparison.result_not_found", "Result not found: " + resultId));
    }

    auto result = *found.value();
    result.status = ComparisonResultStatus::Running;
    result.startedAt = now();
    result.content = {};
    result.errorMessage = {};
    result.warnings = {};
    result.inputTokens = 0;
    result.outputTokens = 0;
    result.latencyMs = 0;

    const auto updated = repository_.updateResult(std::move(result));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::setResultCompleted(
    const ComparisonResultId& resultId,
    const std::string& content,
    std::uint64_t inputTokens,
    std::uint64_t outputTokens,
    std::int64_t latencyMs,
    const std::string& totalCost)
{
    const auto found = repository_.findResultById(resultId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("comparison.result_not_found", "Result not found: " + resultId));
    }

    auto result = *found.value();
    result.status = ComparisonResultStatus::Completed;
    result.content = content;
    result.inputTokens = inputTokens;
    result.outputTokens = outputTokens;
    result.latencyMs = latencyMs;
    result.totalCost = totalCost;
    result.finishedAt = now();
    result.errorMessage = {};

    const auto updated = repository_.updateResult(std::move(result));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::setResultFailed(
    const ComparisonResultId& resultId,
    const std::string& errorMessage)
{
    const auto found = repository_.findResultById(resultId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("comparison.result_not_found", "Result not found: " + resultId));
    }

    auto result = *found.value();
    result.status = ComparisonResultStatus::Failed;
    result.errorMessage = errorMessage;
    result.warnings = {errorMessage};
    result.finishedAt = now();

    const auto updated = repository_.updateResult(std::move(result));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::setResultPartial(
    const ComparisonResultId& resultId,
    const std::string& content,
    std::uint64_t inputTokens,
    std::uint64_t outputTokens,
    std::int64_t latencyMs,
    const std::string& totalCost)
{
    const auto found = repository_.findResultById(resultId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("comparison.result_not_found", "Result not found: " + resultId));
    }

    auto result = *found.value();
    result.status = ComparisonResultStatus::Partial;
    result.content = content;
    result.inputTokens = inputTokens;
    result.outputTokens = outputTokens;
    result.latencyMs = latencyMs;
    result.totalCost = totalCost;
    result.warnings = {"Response was truncated before completion"};
    result.finishedAt = now();
    result.errorMessage = {};

    const auto updated = repository_.updateResult(std::move(result));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::resetResultPending(const ComparisonResultId& resultId)
{
    const auto found = repository_.findResultById(resultId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("comparison.result_not_found", "Result not found: " + resultId));
    }

    auto result = *found.value();
    result.status = ComparisonResultStatus::Pending;
    result.content = {};
    result.errorMessage = {};
    result.warnings = {};
    result.inputTokens = 0;
    result.outputTokens = 0;
    result.latencyMs = 0;
    result.totalCost = {};
    result.startedAt = std::nullopt;
    result.finishedAt = std::nullopt;
    result.selectedForSynthesis = false;

    const auto updated = repository_.updateResult(std::move(result));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::setResultBest(
    const ComparisonId& comparisonId,
    const ComparisonResultId& resultId)
{
    const auto allResults = repository_.listResultsBySession(comparisonId);
    if (!allResults.success()) return common::Result<void>::failure(*allResults.error());

    for (auto result : *allResults.value()) {
        const bool shouldBeBest = result.id == resultId;
        if (result.isBest != shouldBeBest) {
            result.isBest = shouldBeBest;
            const auto updated = repository_.updateResult(std::move(result));
            if (!updated.success()) return common::Result<void>::failure(*updated.error());
        }
    }
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::clearResultBest(const ComparisonId& comparisonId)
{
    const auto allResults = repository_.listResultsBySession(comparisonId);
    if (!allResults.success()) return common::Result<void>::failure(*allResults.error());

    for (auto result : *allResults.value()) {
        if (result.isBest) {
            result.isBest = false;
            const auto updated = repository_.updateResult(std::move(result));
            if (!updated.success()) return common::Result<void>::failure(*updated.error());
        }
    }
    return common::Result<void>::ok();
}

common::Result<void> ComparisonService::setResultSynthesisSelected(
    const ComparisonResultId& resultId,
    bool selected)
{
    const auto found = repository_.findResultById(resultId);
    if (!found.success()) return common::Result<void>::failure(*found.error());
    if (!found.value().has_value()) {
        return common::Result<void>::failure(
            error("comparison.result_not_found", "Result not found: " + resultId));
    }

    auto result = *found.value();
    if (result.status != ComparisonResultStatus::Completed) {
        return common::Result<void>::failure(
            error("comparison.not_completed", "Only completed results can be selected for synthesis"));
    }
    result.selectedForSynthesis = selected;

    const auto updated = repository_.updateResult(std::move(result));
    if (!updated.success()) return common::Result<void>::failure(*updated.error());
    return common::Result<void>::ok();
}

} // namespace ai::application
