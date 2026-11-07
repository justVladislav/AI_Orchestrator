#include "SqlitePrimitives.hpp"
#include "SqliteConnectionHealth.hpp"

#include <limits>
#include <utility>

namespace ai::infrastructure::sqlite_detail {

namespace {

// A refusal by THIS connection's quarantine: SQLITE_AUTH, and the connection is known (client data) to be quarantined. The same code
// on a connection that is not quarantined is some other authorization error and is not renamed. Covers the refusal at prepare AND the
// one that surfaces at sqlite3_step when a statement prepared earlier is re-prepared under the authorizer.
std::optional<common::Error> quarantineRefusal(sqlite3* handle, std::string_view operation)
{
    if (handle == nullptr || sqlite3_extended_errcode(handle) != SQLITE_AUTH) return std::nullopt;
    const ConnectionHealth* health = connectionHealthOf(handle);
    if (health == nullptr || !health->quarantined()) return std::nullopt;
    std::string message = "The SQLite connection is isolated (";
    message += toString(health->reason());
    message += health->isolationFailed() ? "; isolation incomplete" : "";
    message += ") and refuses every statement until its owner closes the open transaction; nothing was performed: ";
    message += operation;
    return error("persistence.connection_unusable", std::move(message));
}

} // namespace

NativeError captureNativeError(sqlite3* handle)
{
    NativeError native;
    if (handle == nullptr) {
        native.code = SQLITE_MISUSE;
        native.extendedCode = SQLITE_MISUSE;
        native.message = "SQLite handle is unavailable";
        return native;
    }
    // Extended result codes are enabled on these connections, so sqlite3_errcode() already returns the EXTENDED value (787).
    // The primary code is its low byte (19), whichever way the handle is configured.
    native.extendedCode = sqlite3_extended_errcode(handle);
    native.code = native.extendedCode & 0xFF;
    native.message = sqlite3_errmsg(handle);
    return native;
}

std::optional<NativeError> tryCaptureNativeError(sqlite3* handle) noexcept
{
    try {
        return captureNativeError(handle);
    } catch (...) {
        return std::nullopt;
    }
}

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

common::Error databaseError(sqlite3* handle, std::string code, std::string_view operation)
{
    if (auto refusal = quarantineRefusal(handle, operation)) return std::move(*refusal);
    std::string message(operation);
    message += ": ";
    message += handle != nullptr ? sqlite3_errmsg(handle) : "SQLite handle is unavailable";
    return error(std::move(code), std::move(message));
}

common::Result<void> execute(sqlite3* handle, const char* sql, std::string code)
{
    char* rawMessage = nullptr;
    const int result = sqlite3_exec(handle, sql, nullptr, nullptr, &rawMessage);
    if (result == SQLITE_OK) {
        return common::Result<void>::ok();
    }

    // The extended code is read BEFORE sqlite3_free/anything else touches the handle.
    if (auto refusal = quarantineRefusal(handle, "an SQL statement")) {
        sqlite3_free(rawMessage);
        return common::Result<void>::failure(std::move(*refusal));
    }
    std::string message = rawMessage != nullptr ? rawMessage : sqlite3_errmsg(handle);
    sqlite3_free(rawMessage);
    return common::Result<void>::failure(error(std::move(code), std::move(message)));
}

common::Result<void> prepare(sqlite3* handle, const char* sql, Statement& statement)
{
    if (sqlite3_prepare_v2(handle, sql, -1, statement.output(), nullptr) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.prepare_failed", "Could not prepare SQLite statement"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindText(sqlite3* handle, sqlite3_stmt* statement, int index, const std::string& value)
{
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind text value"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindNullableText(
    sqlite3* handle,
    sqlite3_stmt* statement,
    int index,
    const std::optional<std::string>& value)
{
    if (value.has_value()) {
        return bindText(handle, statement, index, *value);
    }
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind null text value"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindInt64(sqlite3* handle, sqlite3_stmt* statement, int index, std::int64_t value)
{
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind integer value"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindUnsigned(sqlite3* handle, sqlite3_stmt* statement, int index, std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return common::Result<void>::failure(
            error("persistence.value_out_of_range", "Unsigned value exceeds SQLite INTEGER range"));
    }
    return bindInt64(handle, statement, index, static_cast<std::int64_t>(value));
}

std::int64_t toMilliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromMilliseconds(std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

common::Result<void> bindOptionalTime(
    sqlite3* handle,
    sqlite3_stmt* statement,
    int index,
    const std::optional<std::chrono::system_clock::time_point>& value)
{
    if (value.has_value()) {
        return bindInt64(handle, statement, index, toMilliseconds(*value));
    }
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind null timestamp"));
    }
    return common::Result<void>::ok();
}

std::optional<std::chrono::system_clock::time_point> optionalTime(sqlite3_stmt* statement, int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return fromMilliseconds(sqlite3_column_int64(statement, column));
}

std::string columnText(sqlite3_stmt* statement, int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    if (value == nullptr) {
        return {};
    }
    return reinterpret_cast<const char*>(value);
}

std::optional<std::string> optionalText(sqlite3_stmt* statement, int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return columnText(statement, column);
}

// std::filesystem::path has no portable narrow encoding across platforms
// (native() is UTF-16 on Windows). Persist an explicit UTF-8 encoding so the
// database file itself is portable and non-ASCII project paths round-trip.
std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path pathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

} // namespace ai::infrastructure::sqlite_detail
