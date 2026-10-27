#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/ModelExecutionTypes.hpp"
#include "ai/application/catalog/ProviderCatalogTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

using ComparisonSessionId = std::string;

enum class ComparisonSessionStatus {
    Pending,
    Running,
    Completed,
    Partial,
    Failed,
    Cancelled
};

struct ComparisonSession {
    ComparisonSessionId id;
    ProjectId projectId;
    std::string title;
    std::string prompt;
    ComparisonSessionStatus status {ComparisonSessionStatus::Pending};
    std::string requestHash;
    std::string contextHash;
    std::optional<ModelExecutionId> preferredExecutionId;
    std::vector<ModelExecutionId> synthesisSelection;
    std::optional<ModelExecutionId> synthesisExecutionId;
    std::string errorCode;
    std::string errorMessage;
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> startedAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;
    std::uint64_t revision {0};
};

struct ComparisonModelSlot {
    ProviderId providerId;
    ModelId modelId;
    std::string role {"candidate"};
};

struct CreateComparisonSessionRequest {
    ProjectId projectId;
    std::string title;
    std::string prompt;
    std::string contextHash;
    std::vector<ComparisonModelSlot> modelSlots;
};

[[nodiscard]] std::string_view toString(ComparisonSessionStatus value) noexcept;
[[nodiscard]] std::optional<ComparisonSessionStatus> comparisonSessionStatusFromString(
    std::string_view value) noexcept;
[[nodiscard]] bool isTerminal(ComparisonSessionStatus value) noexcept;

} // namespace ai::application
