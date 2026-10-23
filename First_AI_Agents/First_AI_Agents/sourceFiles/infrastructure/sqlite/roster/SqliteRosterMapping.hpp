#pragma once

// Mapping shared by the Run code that stays in the aggregate and by the run draft repository: the JSON string
// list used by several columns, and the rich roster assignment row of run_stage_assignments and
// run_draft_assignments (the narrow profile row is read by readAssignment in catalog/SqliteProfileMapping).
#include "ai/application/ExecutionProfileTypes.hpp"
#include "ai/common/Result.hpp"

#include <sqlite3.h>

#include <string>
#include <string_view>
#include <vector>

namespace ai::infrastructure::sqlite_detail {

std::string serializeStringVector(const std::vector<std::string>& values);
common::Result<std::vector<std::string>> parseStringVector(const std::string& encoded, std::string_view field);
common::Result<application::StageModelAssignment> readRunAssignment(sqlite3_stmt* statement);

} // namespace ai::infrastructure::sqlite_detail
