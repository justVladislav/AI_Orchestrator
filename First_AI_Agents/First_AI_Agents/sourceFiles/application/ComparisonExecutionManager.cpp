#include "ai/application/ComparisonExecutionManager.hpp"

#include "ai/application/CostCalculation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <sstream>
#include <utility>

namespace ai::application {
namespace {

common::Error managerError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

CostBreakdown executionCost(const Model& model, const models::TokenUsage& usage)
{
    // MW-P2: see CostCalculation.hpp - correctly bills any cache-write/
    // cache-read tokens at their own rate instead of the plain input rate.
    // A candidate is ONE provider response, so it is classified against the model's
    // long-context threshold by its own input_tokens (M2b).
    return computeSingleResponseCostMicros(model, usage);
}

std::string previewOf(std::string_view content)
{
    constexpr std::size_t limit = 1'000;
    return std::string(content.substr(0, std::min(limit, content.size())));
}

} // namespace

ComparisonExecutionManager::ComparisonExecutionManager(
    ComparisonService& comparisons,
    ModelExecutionService& executions,
    ProviderCatalogService& catalog,
    IProjectRepository& projects,
    IProviderClientFactory& clients,
    std::size_t maxCandidateConcurrency)
    : comparisons_(comparisons)
    , executions_(executions)
    , catalog_(catalog)
    , projects_(projects)
    , clients_(clients)
    , maxCandidateConcurrency_(std::max<std::size_t>(1, maxCandidateConcurrency))
    , worker_([this](std::stop_token token) { workerLoop(token); })
{
}

ComparisonExecutionManager::~ComparisonExecutionManager()
{
    shutdown();
}

common::Result<void> ComparisonExecutionManager::enqueue(
    const ComparisonSessionId& sessionId)
{
    if (sessionId.empty()) {
        return common::Result<void>::failure(
            managerError("comparison.invalid_id", "Comparison session id is required"));
    }
    const auto session = comparisons_.findSession(sessionId);
    if (!session.success()) return common::Result<void>::failure(*session.error());
    if (!session.value().has_value()) {
        return common::Result<void>::failure(
            managerError("comparison.not_found", "Comparison session not found"));
    }
    if (session.value()->status != ComparisonSessionStatus::Pending) {
        return common::Result<void>::failure(managerError(
            "comparison.invalid_transition", "Only a pending comparison can be queued"));
    }

    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            return common::Result<void>::failure(managerError(
                "comparison_manager.shutting_down", "Comparison manager is shutting down"));
        }
        if ((currentSessionId_.has_value() && *currentSessionId_ == sessionId)
            || std::find(queue_.begin(), queue_.end(), sessionId) != queue_.end()) {
            return common::Result<void>::failure(
                managerError("comparison.already_queued", "Comparison is already queued"));
        }
        queue_.push_back(sessionId);
    }
    condition_.notify_one();
    notify({ComparisonExecutionEventType::Queued, sessionId, std::nullopt});
    return common::Result<void>::ok();
}

common::Result<void> ComparisonExecutionManager::requestCancellation(
    const ComparisonSessionId& sessionId)
{
    bool queuedCancellation = false;
    {
        std::lock_guard lock(mutex_);
        const auto queued = std::find(queue_.begin(), queue_.end(), sessionId);
        if (queued != queue_.end()) {
            queue_.erase(queued);
            queuedCancellation = true;
        } else if (currentSessionId_.has_value() && *currentSessionId_ == sessionId) {
            currentStopSource_.request_stop();
            return common::Result<void>::ok();
        } else {
            return common::Result<void>::failure(managerError(
                "comparison.not_active", "Comparison is not queued or running"));
        }
    }

    if (queuedCancellation) {
        cancelPendingCandidates(sessionId);
        const auto finished = comparisons_.finishSession(
            sessionId,
            ComparisonSessionStatus::Cancelled,
            "comparison.cancelled",
            "Comparison cancelled before execution");
        if (!finished.success()) return common::Result<void>::failure(*finished.error());
        notify({ComparisonExecutionEventType::Finished, sessionId, std::nullopt});
    }
    return common::Result<void>::ok();
}

void ComparisonExecutionManager::setEventHandler(ComparisonExecutionEventHandler handler)
{
    std::lock_guard lock(mutex_);
    eventHandler_ = std::move(handler);
}

bool ComparisonExecutionManager::isActive(const ComparisonSessionId& sessionId) const
{
    std::lock_guard lock(mutex_);
    return (currentSessionId_.has_value() && *currentSessionId_ == sessionId)
        || std::find(queue_.begin(), queue_.end(), sessionId) != queue_.end();
}

void ComparisonExecutionManager::shutdown()
{
    std::vector<ComparisonSessionId> queued;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) return;
        shuttingDown_ = true;
        queued.assign(queue_.begin(), queue_.end());
        queue_.clear();
        currentStopSource_.request_stop();
    }
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();

    for (const auto& sessionId : queued) {
        cancelPendingCandidates(sessionId);
        static_cast<void>(comparisons_.finishSession(
            sessionId,
            ComparisonSessionStatus::Cancelled,
            "comparison.interrupted",
            "Application shutdown before comparison execution"));
    }
}

void ComparisonExecutionManager::workerLoop(std::stop_token shutdownToken)
{
    while (!shutdownToken.stop_requested()) {
        ComparisonSessionId sessionId;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, shutdownToken, [this] {
                return shuttingDown_ || !queue_.empty();
            });
            if (shutdownToken.stop_requested() || shuttingDown_) break;
            sessionId = queue_.front();
            queue_.pop_front();
            currentSessionId_ = sessionId;
            currentStopSource_ = std::stop_source {};
        }

        executeSession(sessionId);

        {
            std::lock_guard lock(mutex_);
            if (currentSessionId_ == sessionId) currentSessionId_.reset();
        }
    }
}

void ComparisonExecutionManager::executeSession(
    const ComparisonSessionId& sessionId)
{
    const auto started = comparisons_.startSession(sessionId);
    if (!started.success()) {
        notify({ComparisonExecutionEventType::Finished, sessionId, std::nullopt});
        return;
    }
    notify({ComparisonExecutionEventType::Started, sessionId, std::nullopt});

    const auto project = projects_.findById(started.value().projectId);
    const auto candidates = comparisons_.listResults(sessionId);
    if (!project.success() || !project.value().has_value() || !candidates.success()) {
        const std::string message = !project.success() ? project.error()->message
            : !project.value().has_value() ? "Project not found"
            : candidates.error()->message;
        cancelPendingCandidates(sessionId);
        static_cast<void>(comparisons_.finishSession(
            sessionId, ComparisonSessionStatus::Failed,
            "comparison.preflight_failed", message));
        notify({ComparisonExecutionEventType::Finished, sessionId, std::nullopt});
        return;
    }

    const std::stop_token stopToken = currentStopSource_.get_token();
    for (std::size_t offset = 0; offset < candidates.value().size();
         offset += maxCandidateConcurrency_) {
        if (stopToken.stop_requested()) break;
        const std::size_t end = std::min(
            candidates.value().size(), offset + maxCandidateConcurrency_);
        std::vector<std::future<void>> tasks;
        tasks.reserve(end - offset);
        for (std::size_t index = offset; index < end; ++index) {
            const ModelExecution candidate = candidates.value()[index];
            tasks.push_back(std::async(std::launch::async,
                [this, session = started.value(), value = *project.value(),
                 candidate, stopToken] {
                    executeCandidate(session, value, candidate, stopToken);
                }));
        }
        for (auto& task : tasks) {
            try {
                task.get();
            } catch (...) {
                // Candidate failures are persisted inside executeCandidate.
                // This guard keeps an unexpected adapter exception from
                // terminating the manager worker.
            }
        }
    }

    if (stopToken.stop_requested()) cancelPendingCandidates(sessionId);

    const auto finalCandidates = comparisons_.listResults(sessionId);
    std::size_t reviewable = 0;
    std::size_t failed = 0;
    if (finalCandidates.success()) {
        for (const auto& candidate : finalCandidates.value()) {
            if (candidate.status == ModelExecutionStatus::Completed
                || candidate.status == ModelExecutionStatus::Partial) {
                ++reviewable;
                const auto evaluation = executions_.addDeterministicEvaluation(
                    evaluator_.evaluate(candidate, started.value().prompt));
                if (!evaluation.success()) ++failed;
            } else if (candidate.status == ModelExecutionStatus::Failed) {
                ++failed;
            }
        }
    }

    ComparisonSessionStatus status = ComparisonSessionStatus::Failed;
    std::string errorCode;
    std::string errorMessage;
    if (stopToken.stop_requested()) {
        status = ComparisonSessionStatus::Cancelled;
        errorCode = "comparison.cancelled";
        errorMessage = "Comparison cancelled";
    } else if (finalCandidates.success()
        && reviewable == finalCandidates.value().size() && failed == 0) {
        status = ComparisonSessionStatus::Completed;
    } else if (reviewable >= 2) {
        status = ComparisonSessionStatus::Partial;
        errorCode = "comparison.partial";
        errorMessage = "Some candidates or quality evaluations failed";
    } else {
        errorCode = "comparison.insufficient_results";
        errorMessage = "Fewer than two candidates produced reviewable results";
    }

    static_cast<void>(comparisons_.finishSession(
        sessionId, status, std::move(errorCode), std::move(errorMessage)));
    notify({ComparisonExecutionEventType::Finished, sessionId, std::nullopt});
}

void ComparisonExecutionManager::executeCandidate(
    const ComparisonSession& session,
    const Project& project,
    const ModelExecution& execution,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        FinishModelExecutionRequest cancelled;
        cancelled.status = ModelExecutionStatus::Cancelled;
        cancelled.errorCode = "comparison.cancelled";
        cancelled.errorMessage = "Comparison cancelled before candidate execution";
        static_cast<void>(executions_.finishExecution(execution.id, cancelled));
        notify({ComparisonExecutionEventType::CandidateFinished, session.id, execution.id});
        return;
    }

    const auto begun = executions_.startExecution(execution.id);
    if (!begun.success()) {
        notify({ComparisonExecutionEventType::CandidateFinished, session.id, execution.id});
        return;
    }
    const auto startedAt = std::chrono::steady_clock::now();

    FinishModelExecutionRequest finish;
    const auto provider = catalog_.findProvider(execution.providerId);
    const auto model = catalog_.findModel(execution.modelId);
    if (!provider.success() || !model.success()
        || !provider.value().has_value() || !model.value().has_value()) {
        finish.status = ModelExecutionStatus::Failed;
        finish.errorCode = "comparison.catalog_entry_missing";
        finish.errorMessage = "Comparison provider or model is no longer available";
    } else if (!provider.value()->enabled || !model.value()->enabled
        || model.value()->providerId != provider.value()->id) {
        finish.status = ModelExecutionStatus::Failed;
        finish.errorCode = "comparison.model_unavailable";
        finish.errorMessage = "Comparison provider or model is disabled or mismatched";
    } else {
        ProviderChatRequest request;
        request.providerId = provider.value()->id;
        request.modelId = model.value()->id;
        request.apiModelId = model.value()->apiModelId;
        request.systemPrompt =
            "You are an independent candidate in a blinded model comparison. "
            "Answer the requested task directly, include concrete reasoning or "
            "implementation details, and do not discuss the comparison itself.";
        std::ostringstream content;
        content << "# Project\n" << project.name << "\n\n"
                << "# Project context\n" << project.description << "\n\n"
                << "# Task\n" << session.prompt;
        request.userContent = content.str();
        request.maxOutputTokens = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                model.value()->maxOutputTokens > 0
                    ? model.value()->maxOutputTokens : 4096,
                16'384));

        const auto client = clients_.clientFor(provider.value()->id, provider.value()->kind);
        const auto response = client.success()
            ? client.value()->send(request, stopToken)
            : common::Result<ProviderChatResponse>::failure(*client.error());
        if (stopToken.stop_requested()) {
            finish.status = ModelExecutionStatus::Cancelled;
            finish.errorCode = "comparison.cancelled";
            finish.errorMessage = "Comparison candidate cancelled";
        } else if (!response.success()) {
            finish.status = ModelExecutionStatus::Failed;
            finish.errorCode = response.error()->code;
            finish.errorMessage = response.error()->message;
        } else {
            finish.status = ModelExecutionStatus::Completed;
            finish.outputContent = response.value().content;
            finish.outputPreview = previewOf(response.value().content);
            finish.inputTokens = response.value().usage.inputTokens;
            finish.outputTokens = response.value().usage.outputTokens;
            finish.cacheCreationInputTokens = response.value().usage.cacheCreationInputTokens;
            finish.cacheReadInputTokens = response.value().usage.cacheReadInputTokens;
            finish.cacheCreationDuration = response.value().usage.cacheCreationDuration;
            const auto costBreakdown = executionCost(*model.value(), response.value().usage);
            finish.costMicros = costBreakdown.costMicros;
            finish.costPriceKnown = costBreakdown.priceKnown;
        }
    }
    finish.latencyMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count());
    static_cast<void>(executions_.finishExecution(execution.id, finish));
    notify({ComparisonExecutionEventType::CandidateFinished, session.id, execution.id});
}

void ComparisonExecutionManager::cancelPendingCandidates(
    const ComparisonSessionId& sessionId)
{
    const auto candidates = comparisons_.listResults(sessionId);
    if (!candidates.success()) return;
    for (const auto& candidate : candidates.value()) {
        if (candidate.status != ModelExecutionStatus::Pending
            && candidate.status != ModelExecutionStatus::Running) continue;
        FinishModelExecutionRequest cancelled;
        cancelled.status = ModelExecutionStatus::Cancelled;
        cancelled.errorCode = "comparison.cancelled";
        cancelled.errorMessage = "Comparison execution cancelled";
        static_cast<void>(executions_.finishExecution(candidate.id, cancelled));
    }
}

void ComparisonExecutionManager::notify(const ComparisonExecutionEvent& event)
{
    ComparisonExecutionEventHandler handler;
    {
        std::lock_guard lock(mutex_);
        handler = eventHandler_;
    }
    if (handler) handler(event);
}

} // namespace ai::application
