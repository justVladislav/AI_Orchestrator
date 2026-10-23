#include "SqliteRunStatusMapping.hpp"

#include <optional>
#include <string_view>

namespace ai::infrastructure::sqlite_detail {

using application::RunStatus;

std::optional<RunStatus> runStatusFromString(std::string_view value)
{
    if (value == "prepared") return RunStatus::Prepared;
    if (value == "queued") return RunStatus::Queued;
    if (value == "running") return RunStatus::Running;
    if (value == "cancelling") return RunStatus::Cancelling;
    if (value == "completed") return RunStatus::Completed;
    if (value == "failed") return RunStatus::Failed;
    if (value == "cancelled") return RunStatus::Cancelled;
    if (value == "interrupted") return RunStatus::Interrupted;
    return std::nullopt;
}

} // namespace ai::infrastructure::sqlite_detail
