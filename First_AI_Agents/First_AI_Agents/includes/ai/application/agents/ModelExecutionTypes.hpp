#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/catalog/ProviderCatalogTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ai::application {

using ModelExecutionId = std::string;
using QualityEvaluationId = std::string;

// The two error codes ModelExecutionService writes on an execution it had to fail because the
// attempt never finished: the application restarted under it
// (recoverInterruptedExecutions) or its owning Run reached a terminal state while the row was
// still Pending or Running (reconcileRunExecutions). Read-side projections count both kinds
// as "interrupted"; the writer and the readers share these literals.
inline constexpr const char* kModelExecutionInterruptedErrorCode = "model_execution.interrupted";
inline constexpr const char* kModelExecutionOrphanedErrorCode = "model_execution.orphaned";

// FollowUp (PC-5): the one paid composer call of a follow-up plan request. sourceId is the request id, projectId the
// request's project, and there is no link to the source Run, so the cost never changes the cost of that Run.
enum class ModelExecutionSource { Run, Comparison, Chat, FollowUp };
enum class ModelExecutionStatus { Pending, Running, Completed, Partial, Failed, Cancelled };
enum class QualityEvaluatorKind { Automated, BlindedModel, Human };

struct ModelExecution {
    ModelExecutionId id;
    ModelExecutionSource source {ModelExecutionSource::Run};
    std::string sourceId;
    std::optional<ProjectId> projectId;
    ProviderId providerId;
    ModelId modelId;
    std::string role;
    std::uint32_t position {0};
    std::uint32_t attempt {1};
    ModelExecutionStatus status {ModelExecutionStatus::Pending};
    std::string requestHash;
    std::string contextHash;
    std::optional<std::string> outputArtifactId;
    // Durable complete provider response. outputPreview remains a compact
    // list/read-model projection; quality evaluation and synthesis must use
    // this full value so they remain reproducible after process restart.
    std::string outputContent;
    std::string outputPreview;
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
    // MW-P2: false when costMicros was computed against a model price that
    // was unset (0, "not entered") for a usage category this execution
    // actually consumed - see ai::application::CostBreakdown::priceKnown.
    // costMicros still holds the best-effort number in that case; this flag
    // is what lets the UI show it as an estimate instead of a precise total.
    bool costPriceKnown {true};
    std::uint64_t latencyMs {0};
    std::string errorCode;
    std::string errorMessage;
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> startedAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
    // ACD-2 durable cache usage. Existing rows default to zero and an unknown
    // duration partition; ACD-3 populates these from provider responses.
    std::uint64_t cacheCreationInputTokens {0};
    std::uint64_t cacheReadInputTokens {0};
    models::TokenUsage::CacheCreationDurationShare cacheCreationDuration;
};

struct QualityEvaluation {
    QualityEvaluationId id;
    ModelExecutionId executionId;
    QualityEvaluatorKind evaluatorKind {QualityEvaluatorKind::Automated};
    std::optional<ProviderId> evaluatorProviderId;
    std::optional<ModelId> evaluatorModelId;
    std::string rubricVersion;
    std::uint32_t scoreBasisPoints {0};
    std::string criteriaJson;
    std::string explanation;
    std::string evidence;
    std::chrono::system_clock::time_point createdAt;
};

[[nodiscard]] std::string_view toString(ModelExecutionSource value) noexcept;
[[nodiscard]] std::string_view toString(ModelExecutionStatus value) noexcept;
[[nodiscard]] std::string_view toString(QualityEvaluatorKind value) noexcept;
[[nodiscard]] std::optional<ModelExecutionSource> modelExecutionSourceFromString(std::string_view value) noexcept;
[[nodiscard]] std::optional<ModelExecutionStatus> modelExecutionStatusFromString(std::string_view value) noexcept;
[[nodiscard]] std::optional<QualityEvaluatorKind> qualityEvaluatorKindFromString(std::string_view value) noexcept;
[[nodiscard]] bool isTerminal(ModelExecutionStatus value) noexcept;

} // namespace ai::application
