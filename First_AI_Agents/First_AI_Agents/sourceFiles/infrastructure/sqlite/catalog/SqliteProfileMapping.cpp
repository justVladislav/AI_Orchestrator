#include "SqliteProfileMapping.hpp"
#include "../SqlitePrimitives.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ai::infrastructure::sqlite_detail {

using application::ProviderExecutionBudgetOverrides;
using application::StageModelAssignment;

std::string providerBudgetOverridesJson(const ProviderExecutionBudgetOverrides& value)
{
    nlohmann::json document = nlohmann::json::object();
    const auto put = [&](const char* name, const std::optional<std::uint64_t>& field) {
        if (field) document[name] = *field;
    };
    put("maxInputTokens", value.maxInputTokens);
    put("maxOutputTokens", value.maxOutputTokens);
    put("maxTurns", value.maxTurns);
    put("maxToolCalls", value.maxToolCalls);
    put("maxToolResultBytes", value.maxToolResultBytes);
    put("maxWallTimeMs", value.maxWallTimeMs);
    put("maxRequestTimeMs", value.maxRequestTimeMs);
    return document.dump();
}

common::Result<ProviderExecutionBudgetOverrides> parseProviderBudgetOverrides(
    std::string_view raw)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        if (!document.is_object() || document.size() > 7)
            throw std::runtime_error("invalid object");
        ProviderExecutionBudgetOverrides value;
        const auto get = [&](const char* name, std::optional<std::uint64_t>& field) {
            if (document.contains(name)) field = document.at(name).get<std::uint64_t>();
        };
        get("maxInputTokens", value.maxInputTokens);
        get("maxOutputTokens", value.maxOutputTokens);
        get("maxTurns", value.maxTurns);
        get("maxToolCalls", value.maxToolCalls);
        get("maxToolResultBytes", value.maxToolResultBytes);
        get("maxWallTimeMs", value.maxWallTimeMs);
        get("maxRequestTimeMs", value.maxRequestTimeMs);
        const auto valid = resolveProviderExecutionBudget(value);
        if (!valid.success()) {
            return common::Result<ProviderExecutionBudgetOverrides>::failure(*valid.error());
        }
        return common::Result<ProviderExecutionBudgetOverrides>::ok(std::move(value));
    } catch (...) {
        return common::Result<ProviderExecutionBudgetOverrides>::failure(error(
            "persistence.invalid_data", "Execution profile contains corrupt provider budget limits"));
    }
}

common::Result<StageModelAssignment> readAssignment(sqlite3_stmt* statement)
{
    // executionProfileAssignmentColumns order: profile_id(0), stage_type(1),
    // provider_id(2), model_id(3), timeout_ms(4).
    const std::int64_t timeoutMs = sqlite3_column_int64(statement, 4);
    if (timeoutMs <= 0) {
        return common::Result<StageModelAssignment>::failure(
            error("persistence.invalid_data", "Assignment row contains a non-positive timeout"));
    }
    StageModelAssignment assignment;
    assignment.stageType = columnText(statement, 1);
    assignment.providerId = columnText(statement, 2);
    assignment.modelId = columnText(statement, 3);
    assignment.timeout = std::chrono::milliseconds(timeoutMs);
    return common::Result<StageModelAssignment>::ok(std::move(assignment));
}

} // namespace ai::infrastructure::sqlite_detail
