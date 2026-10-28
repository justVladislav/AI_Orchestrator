#pragma once

// Mapping shared by the execution profile repository and the run draft code that
// still lives in the aggregate: the provider budget override JSON and the narrow
// (stage, provider, model, timeout) assignment row of execution_profile_assignments.
#include "ai/application/catalog/ExecutionProfileTypes.hpp"
#include "ai/application/budget/ProviderExecutionBudget.hpp"
#include "ai/common/Result.hpp"

#include <sqlite3.h>

#include <string>
#include <string_view>

namespace ai::infrastructure::sqlite_detail {

std::string providerBudgetOverridesJson(const application::ProviderExecutionBudgetOverrides& value);
common::Result<application::ProviderExecutionBudgetOverrides> parseProviderBudgetOverrides(
    std::string_view raw);
common::Result<application::StageModelAssignment> readAssignment(sqlite3_stmt* statement);

} // namespace ai::infrastructure::sqlite_detail
