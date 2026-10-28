#pragma once

#include "ai/application/comparison/ComparisonQualityEvaluator.hpp"
#include "ai/application/comparison/ComparisonService.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/providers/IProviderClient.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace ai::application {

enum class ComparisonExecutionEventType {
    Queued,
    Started,
    CandidateFinished,
    Finished
};

struct ComparisonExecutionEvent {
    ComparisonExecutionEventType type {ComparisonExecutionEventType::Queued};
    ComparisonSessionId sessionId;
    std::optional<ModelExecutionId> executionId;
};

using ComparisonExecutionEventHandler =
    std::function<void(const ComparisonExecutionEvent&)>;

/**
 * Owns asynchronous Comparison execution.
 *
 * Sessions are serialized through one durable queue; candidates inside one
 * session execute in bounded parallel batches. Provider calls never run on
 * the Qt GUI thread, and cancellation is propagated with stop_token.
 */
class ComparisonExecutionManager final {
public:
    ComparisonExecutionManager(
        ComparisonService& comparisons,
        ModelExecutionService& executions,
        ProviderCatalogService& catalog,
        IProjectRepository& projects,
        IProviderClientFactory& clients,
        std::size_t maxCandidateConcurrency = 4);
    ~ComparisonExecutionManager();

    ComparisonExecutionManager(const ComparisonExecutionManager&) = delete;
    ComparisonExecutionManager& operator=(const ComparisonExecutionManager&) = delete;

    common::Result<void> enqueue(const ComparisonSessionId& sessionId);
    common::Result<void> requestCancellation(const ComparisonSessionId& sessionId);
    void setEventHandler(ComparisonExecutionEventHandler handler);
    [[nodiscard]] bool isActive(const ComparisonSessionId& sessionId) const;
    void shutdown();

private:
    void workerLoop(std::stop_token shutdownToken);
    void executeSession(const ComparisonSessionId& sessionId);
    void executeCandidate(
        const ComparisonSession& session,
        const Project& project,
        const ModelExecution& execution,
        std::stop_token stopToken);
    void cancelPendingCandidates(const ComparisonSessionId& sessionId);
    void notify(const ComparisonExecutionEvent& event);

    ComparisonService& comparisons_;
    ModelExecutionService& executions_;
    ProviderCatalogService& catalog_;
    IProjectRepository& projects_;
    IProviderClientFactory& clients_;
    ComparisonQualityEvaluator evaluator_;
    std::size_t maxCandidateConcurrency_;

    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<ComparisonSessionId> queue_;
    std::optional<ComparisonSessionId> currentSessionId_;
    std::stop_source currentStopSource_;
    ComparisonExecutionEventHandler eventHandler_;
    bool shuttingDown_ {false};
    std::jthread worker_;
};

} // namespace ai::application
