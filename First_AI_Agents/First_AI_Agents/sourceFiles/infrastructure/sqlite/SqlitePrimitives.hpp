#pragma once

// Persistence-private building blocks. No feature DTOs, schema definitions or
// repository-specific SQL belong here; callers use the public repository ports.
#include "ai/common/Result.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ai::infrastructure::sqlite_detail {

// sqlite3_stmt has one owner and must be finalized on every early return,
// including the many early-return branches these repository methods take.
class Statement final {
public:
    Statement() = default;
    ~Statement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt** output() noexcept { return &statement_; }
    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ {nullptr};
};

common::Error error(std::string code, std::string message);
common::Error databaseError(sqlite3* handle, std::string code, std::string_view operation);
common::Result<void> execute(sqlite3* handle, const char* sql, std::string code);
common::Result<void> prepare(sqlite3* handle, const char* sql, Statement& statement);
common::Result<void> bindText(sqlite3* handle, sqlite3_stmt* statement, int index, const std::string& value);
common::Result<void> bindNullableText(sqlite3* handle, sqlite3_stmt* statement, int index,
    const std::optional<std::string>& value);
common::Result<void> bindInt64(sqlite3* handle, sqlite3_stmt* statement, int index, std::int64_t value);
common::Result<void> bindUnsigned(sqlite3* handle, sqlite3_stmt* statement, int index, std::uint64_t value);
std::int64_t toMilliseconds(std::chrono::system_clock::time_point value);
std::chrono::system_clock::time_point fromMilliseconds(std::int64_t value);
common::Result<void> bindOptionalTime(sqlite3* handle, sqlite3_stmt* statement, int index,
    const std::optional<std::chrono::system_clock::time_point>& value);
std::optional<std::chrono::system_clock::time_point> optionalTime(sqlite3_stmt* statement, int column);
std::string columnText(sqlite3_stmt* statement, int column);
std::optional<std::string> optionalText(sqlite3_stmt* statement, int column);

} // namespace ai::infrastructure::sqlite_detail
