#include "ai/application/ComparisonTypes.hpp"

namespace ai::application {

const char* toString(ComparisonStatus status) noexcept
{
    switch (status) {
    case ComparisonStatus::Active:   return "active";
    case ComparisonStatus::Archived: return "archived";
    }
    return "active";
}

const char* toString(ComparisonResultStatus status) noexcept
{
    switch (status) {
    case ComparisonResultStatus::Pending:   return "pending";
    case ComparisonResultStatus::Running:   return "running";
    case ComparisonResultStatus::Completed: return "completed";
    case ComparisonResultStatus::Failed:    return "failed";
    case ComparisonResultStatus::Partial:   return "partial";
    }
    return "pending";
}

} // namespace ai::application
