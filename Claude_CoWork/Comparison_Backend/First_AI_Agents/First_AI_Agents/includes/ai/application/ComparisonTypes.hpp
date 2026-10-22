#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

using ComparisonId = std::string;
using ComparisonResultId = std::string;

enum class ComparisonStatus { Active, Archived };

// Mirrors the four states visible in ComparisonResultCard.
enum class ComparisonResultStatus { Pending, Running, Completed, Failed, Partial };

// One comparison session groups multiple provider results for the same task.
struct ComparisonSession {
    ComparisonId id;
    ProjectId projectId;
    std::string title;
    std::string task;
    ComparisonStatus status { ComparisonStatus::Active };
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
};

// One provider/model result inside a ComparisonSession.
struct ComparisonResult {
    ComparisonResultId id;
    ComparisonId comparisonId;
    std::string providerId;
    std::string providerName;   // denormalized for display; captured at creation
    std::string modelId;
    std::string modelName;      // denormalized for display; captured at creation
    std::string providerKind;   // "openai" | "anthropic" | ... for client factory
    std::string apiModelId;     // API-level model identifier
    ComparisonResultStatus status { ComparisonResultStatus::Pending };
    std::string content;
    std::vector<std::string> strengths;
    std::vector<std::string> warnings;
    std::uint64_t inputTokens { 0 };
    std::uint64_t outputTokens { 0 };
    std::string totalCost;      // formatted, e.g. "$0.018"
    std::int64_t latencyMs { 0 };
    int qualityScore { 0 };
    bool isBest { false };
    bool selectedForSynthesis { false };
    std::string errorMessage;
    std::optional<std::chrono::system_clock::time_point> startedAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
};

// A provider/model slot that is handed to ComparisonService at session-creation
// time. Names and kind are resolved by the caller (ComparisonController) so
// ComparisonService depends only on IProjectRepository, not the provider catalog.
struct ComparisonProviderSlot {
    std::string providerId;
    std::string providerName;
    std::string modelId;
    std::string modelName;
    std::string providerKind;
    std::string apiModelId;
    // Pricing fields (micros per million tokens) for cost computation.
    std::uint64_t inputPriceMicrosPerMillion { 0 };
    std::uint64_t outputPriceMicrosPerMillion { 0 };
};

struct CreateComparisonRequest {
    ProjectId projectId;
    std::string title;
    std::string task;
    std::vector<ComparisonProviderSlot> slots;
};

struct StartSynthesisRequest {
    ComparisonId comparisonId;
    std::vector<ComparisonResultId> resultIds;
};

[[nodiscard]] const char* toString(ComparisonStatus status) noexcept;
[[nodiscard]] const char* toString(ComparisonResultStatus status) noexcept;

} // namespace ai::application
