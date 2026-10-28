#include "ai/application/comparison/ComparisonTypes.hpp"

namespace ai::application {

std::string_view toString(ComparisonSessionStatus value) noexcept
{
    switch (value) {
    case ComparisonSessionStatus::Pending: return "pending";
    case ComparisonSessionStatus::Running: return "running";
    case ComparisonSessionStatus::Completed: return "completed";
    case ComparisonSessionStatus::Partial: return "partial";
    case ComparisonSessionStatus::Failed: return "failed";
    case ComparisonSessionStatus::Cancelled: return "cancelled";
    }
    return "pending";
}

std::optional<ComparisonSessionStatus> comparisonSessionStatusFromString(
    std::string_view value) noexcept
{
    if (value == "pending") return ComparisonSessionStatus::Pending;
    if (value == "running") return ComparisonSessionStatus::Running;
    if (value == "completed") return ComparisonSessionStatus::Completed;
    if (value == "partial") return ComparisonSessionStatus::Partial;
    if (value == "failed") return ComparisonSessionStatus::Failed;
    if (value == "cancelled") return ComparisonSessionStatus::Cancelled;
    return std::nullopt;
}

bool isTerminal(ComparisonSessionStatus value) noexcept
{
    return value == ComparisonSessionStatus::Completed
        || value == ComparisonSessionStatus::Partial
        || value == ComparisonSessionStatus::Failed
        || value == ComparisonSessionStatus::Cancelled;
}

} // namespace ai::application
