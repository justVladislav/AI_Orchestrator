#pragma once

// The stored text of a Run status. Shared by the Run reader, the work ledger and the Outcome Brief reader.
#include "ai/application/ApplicationTypes.hpp"

#include <optional>
#include <string_view>

namespace ai::infrastructure::sqlite_detail {

std::optional<application::RunStatus> runStatusFromString(std::string_view value);

} // namespace ai::infrastructure::sqlite_detail
