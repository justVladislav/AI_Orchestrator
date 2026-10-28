#include "ai/application/comparison/ComparisonService.hpp"

#include "ai/application/IdGenerator.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

namespace ai::application {
namespace {

common::Error comparisonError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

template <typename T>
common::Result<T> failure(std::string code, std::string message)
{
    return common::Result<T>::failure(comparisonError(std::move(code), std::move(message)));
}

} // namespace

ComparisonService::ComparisonService(
    IComparisonSessionRepository& sessions,
    IProjectRepository& projects,
    ProviderCatalogService& catalog,
    ModelExecutionService& executions)
    : sessions_(sessions)
    , projects_(projects)
    , catalog_(catalog)
    , executions_(executions)
{
}

common::Result<ComparisonSession> ComparisonService::createSession(
    const CreateComparisonSessionRequest& request)
{
    if (request.projectId.empty() || request.title.empty() || request.prompt.empty()) {
        return failure<ComparisonSession>(
            "comparison.invalid_request", "Project, title and prompt are required");
    }
    if (request.modelSlots.size() < 2) {
        return failure<ComparisonSession>(
            "comparison.insufficient_candidates", "A comparison requires at least two model slots");
    }

    const auto project = projects_.findById(request.projectId);
    if (!project.success()) return common::Result<ComparisonSession>::failure(*project.error());
    if (!project.value().has_value())
        return failure<ComparisonSession>("comparison.project_not_found", "Project not found");
    if (project.value()->status != ProjectStatus::Active)
        return failure<ComparisonSession>("comparison.project_unavailable", "Project is not active");

    std::unordered_set<std::string> uniqueSlots;
    for (const auto& slot : request.modelSlots) {
        const auto provider = catalog_.findProvider(slot.providerId);
        if (!provider.success())
            return common::Result<ComparisonSession>::failure(*provider.error());
        const auto model = catalog_.findModel(slot.modelId);
        if (!model.success()) return common::Result<ComparisonSession>::failure(*model.error());
        if (!provider.value().has_value() || !model.value().has_value()
            || model.value()->providerId != slot.providerId) {
            return failure<ComparisonSession>(
                "comparison.invalid_model_slot", "Every model must belong to its selected provider");
        }
        if (!provider.value()->enabled || !model.value()->enabled) {
            return failure<ComparisonSession>(
                "comparison.model_unavailable", "Comparison models and providers must be enabled");
        }
        const std::string identity = slot.providerId + "\n" + slot.modelId;
        if (!uniqueSlots.insert(identity).second) {
            return failure<ComparisonSession>(
                "comparison.duplicate_model_slot", "The same provider/model slot cannot be compared twice");
        }
    }

    ComparisonSession session;
    session.id = generateUuid();
    session.projectId = request.projectId;
    session.title = request.title;
    session.prompt = request.prompt;
    session.status = ComparisonSessionStatus::Pending;
    session.requestHash = sha256Hex(request.prompt);
    session.contextHash = request.contextHash.empty()
        ? sha256Hex(project.value()->description)
        : request.contextHash;
    session.createdAt = std::chrono::system_clock::now();

    auto inserted = sessions_.insertSession(session);
    if (!inserted.success()) return inserted;

    for (std::size_t index = 0; index < request.modelSlots.size(); ++index) {
        const auto& slot = request.modelSlots[index];
        CreateModelExecutionRequest execution;
        execution.source = ModelExecutionSource::Comparison;
        execution.sourceId = session.id;
        execution.projectId = session.projectId;
        execution.providerId = slot.providerId;
        execution.modelId = slot.modelId;
        execution.role = slot.role.empty() ? "candidate" : slot.role;
        execution.position = static_cast<std::uint32_t>(index);
        execution.requestHash = session.requestHash;
        execution.contextHash = session.contextHash;
        const auto created = executions_.createExecution(execution);
        if (!created.success()) {
            session.status = ComparisonSessionStatus::Failed;
            session.errorCode = "comparison.execution_seed_failed";
            session.errorMessage = created.error()->message;
            session.finishedAt = std::chrono::system_clock::now();
            ++session.revision;
            static_cast<void>(sessions_.updateSession(session));
            return common::Result<ComparisonSession>::failure(*created.error());
        }
    }
    return inserted;
}

common::Result<ComparisonSession> ComparisonService::loadRequired(
    const ComparisonSessionId& id) const
{
    const auto found = sessions_.findSession(id);
    if (!found.success()) return common::Result<ComparisonSession>::failure(*found.error());
    if (!found.value().has_value())
        return failure<ComparisonSession>("comparison.not_found", "Comparison session not found");
    return common::Result<ComparisonSession>::ok(*found.value());
}

common::Result<ComparisonSession> ComparisonService::startSession(
    const ComparisonSessionId& id)
{
    auto loaded = loadRequired(id);
    if (!loaded.success()) return loaded;
    auto value = loaded.value();
    if (value.status != ComparisonSessionStatus::Pending)
        return failure<ComparisonSession>(
            "comparison.invalid_transition", "Only a pending comparison can start");
    value.status = ComparisonSessionStatus::Running;
    value.startedAt = std::chrono::system_clock::now();
    ++value.revision;
    return sessions_.updateSession(std::move(value));
}

common::Result<ComparisonSession> ComparisonService::finishSession(
    const ComparisonSessionId& id,
    ComparisonSessionStatus status,
    std::string errorCode,
    std::string errorMessage)
{
    if (!isTerminal(status))
        return failure<ComparisonSession>(
            "comparison.invalid_outcome", "Comparison outcome must be terminal");
    auto loaded = loadRequired(id);
    if (!loaded.success()) return loaded;
    auto value = loaded.value();
    if (isTerminal(value.status))
        return failure<ComparisonSession>(
            "comparison.invalid_transition", "A terminal comparison cannot be rewritten");
    if ((status == ComparisonSessionStatus::Completed
            || status == ComparisonSessionStatus::Partial)
        && value.status != ComparisonSessionStatus::Running) {
        return failure<ComparisonSession>(
            "comparison.invalid_transition", "A successful comparison must be running first");
    }
    value.status = status;
    value.errorCode = std::move(errorCode);
    value.errorMessage = std::move(errorMessage);
    value.finishedAt = std::chrono::system_clock::now();
    ++value.revision;
    return sessions_.updateSession(std::move(value));
}

common::Result<void> ComparisonService::validateSelectableExecutions(
    const ComparisonSessionId& id,
    const std::vector<ModelExecutionId>& executionIds) const
{
    const auto results = executions_.listExecutions(ModelExecutionSource::Comparison, id);
    if (!results.success()) return common::Result<void>::failure(*results.error());
    std::unordered_set<ModelExecutionId> allowed;
    for (const auto& result : results.value()) {
        if (result.status == ModelExecutionStatus::Completed
            || result.status == ModelExecutionStatus::Partial) {
            allowed.insert(result.id);
        }
    }
    std::unordered_set<ModelExecutionId> requested;
    for (const auto& executionId : executionIds) {
        if (!allowed.contains(executionId) || !requested.insert(executionId).second) {
            return common::Result<void>::failure(comparisonError(
                "comparison.execution_not_selectable",
                "Selections must be unique completed or partial results from this comparison"));
        }
    }
    return common::Result<void>::ok();
}

common::Result<ComparisonSession> ComparisonService::setPreferredExecution(
    const ComparisonSessionId& id,
    const std::optional<ModelExecutionId>& executionId)
{
    auto loaded = loadRequired(id);
    if (!loaded.success()) return loaded;
    if (executionId.has_value()) {
        const auto valid = validateSelectableExecutions(id, {*executionId});
        if (!valid.success()) return common::Result<ComparisonSession>::failure(*valid.error());
    }
    auto value = loaded.value();
    value.preferredExecutionId = executionId;
    ++value.revision;
    return sessions_.updateSession(std::move(value));
}

common::Result<ComparisonSession> ComparisonService::setSynthesisSelection(
    const ComparisonSessionId& id,
    std::vector<ModelExecutionId> executionIds)
{
    auto loaded = loadRequired(id);
    if (!loaded.success()) return loaded;
    if (executionIds.size() == 1)
        return failure<ComparisonSession>(
            "comparison.synthesis_requires_multiple", "Synthesis requires at least two results");
    const auto valid = validateSelectableExecutions(id, executionIds);
    if (!valid.success()) return common::Result<ComparisonSession>::failure(*valid.error());
    auto value = loaded.value();
    value.synthesisSelection = std::move(executionIds);
    ++value.revision;
    return sessions_.updateSession(std::move(value));
}

common::Result<ComparisonSession> ComparisonService::setSynthesisExecution(
    const ComparisonSessionId& id,
    const std::optional<ModelExecutionId>& executionId)
{
    auto loaded = loadRequired(id);
    if (!loaded.success()) return loaded;
    if (executionId.has_value()) {
        const auto found = executions_.findExecution(*executionId);
        if (!found.success()) return common::Result<ComparisonSession>::failure(*found.error());
        if (!found.value().has_value() || found.value()->source != ModelExecutionSource::Comparison
            || found.value()->sourceId != id || found.value()->role != "synthesis") {
            return failure<ComparisonSession>(
                "comparison.invalid_synthesis_execution",
                "Synthesis execution must belong to this comparison and use the synthesis role");
        }
    }
    auto value = loaded.value();
    value.synthesisExecutionId = executionId;
    ++value.revision;
    return sessions_.updateSession(std::move(value));
}

common::Result<std::optional<ComparisonSession>> ComparisonService::findSession(
    const ComparisonSessionId& id) const
{
    return sessions_.findSession(id);
}

common::Result<std::vector<ComparisonSession>> ComparisonService::listSessions(
    const std::optional<ProjectId>& projectId) const
{
    return sessions_.listSessions(projectId);
}

common::Result<std::vector<ModelExecution>> ComparisonService::listResults(
    const ComparisonSessionId& id) const
{
    return executions_.listExecutions(ModelExecutionSource::Comparison, id);
}

common::Result<void> ComparisonService::recoverInterruptedSessions()
{
    const auto unfinished = sessions_.listUnfinishedSessions();
    if (!unfinished.success()) return common::Result<void>::failure(*unfinished.error());
    for (auto value : unfinished.value()) {
        value.status = ComparisonSessionStatus::Failed;
        value.errorCode = "comparison.interrupted";
        value.errorMessage = "Recovered after an interrupted application session";
        value.finishedAt = std::chrono::system_clock::now();
        ++value.revision;
        const auto updated = sessions_.updateSession(std::move(value));
        if (!updated.success()) return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
