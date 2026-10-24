#include "SqliteRosterMapping.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::infrastructure::sqlite_detail {

using application::StageModelAssignment;

std::string serializeStringVector(const std::vector<std::string>& values)
{
    nlohmann::ordered_json encoded = nlohmann::ordered_json::array();
    for (const auto& value : values) encoded.push_back(value);
    return encoded.dump();
}

common::Result<std::vector<std::string>> parseStringVector(
    const std::string& encoded,
    std::string_view field)
{
    const auto value = nlohmann::json::parse(encoded, nullptr, false);
    if (value.is_discarded() || !value.is_array()) {
        return common::Result<std::vector<std::string>>::failure(error(
            "persistence.invalid_data", std::string(field) + " is not a JSON array"));
    }
    std::vector<std::string> result;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            return common::Result<std::vector<std::string>>::failure(error(
                "persistence.invalid_data", std::string(field) + " contains an invalid id"));
        }
        result.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<std::string>>::ok(std::move(result));
}

common::Result<StageModelAssignment> readRunAssignment(sqlite3_stmt* statement)
{
    auto result = readAssignment(statement);
    if (!result.success()) return result;
    result.value().agentName = columnText(statement, 5);
    result.value().role = columnText(statement, 6);
    result.value().instructions = columnText(statement, 7);
    const auto position = sqlite3_column_int64(statement, 8);
    if (position < 0) {
        return common::Result<StageModelAssignment>::failure(
            error("persistence.invalid_data", "Run assignment position is negative"));
    }
    result.value().position = static_cast<std::uint32_t>(position);
    result.value().leadArchitect = sqlite3_column_int(statement, 9) != 0;
    const auto dependencies = parseStringVector(
        columnText(statement, 10), "Run assignment dependencies");
    if (!dependencies.success()) {
        return common::Result<StageModelAssignment>::failure(*dependencies.error());
    }
    const auto maxAttempts = sqlite3_column_int64(statement, 11);
    if (maxAttempts <= 0 || maxAttempts > 10) {
        return common::Result<StageModelAssignment>::failure(error(
            "persistence.invalid_data", "Run assignment attempt bound is invalid"));
    }
    result.value().dependencies = dependencies.value();
    result.value().maxAttempts = static_cast<std::uint32_t>(maxAttempts);
    result.value().fallbackProviderId = optionalText(statement, 12);
    result.value().fallbackModelId = optionalText(statement, 13);
    result.value().workspaceAccess = sqlite3_column_int(statement, 14) != 0;
    return result;
}

} // namespace ai::infrastructure::sqlite_detail
