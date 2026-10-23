#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;
using Time = std::chrono::system_clock::time_point;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Time at(std::int64_t milliseconds)
{
    return Time(std::chrono::milliseconds(milliseconds));
}

struct Workspace {
    fs::path root = fs::temp_directory_path() / ("aio-sqlite-settings-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path databasePath = root / "state.sqlite3";

    Workspace() { fs::create_directories(root); }
    ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }

    std::shared_ptr<SqliteDatabase> open() const
    {
        auto result = SqliteDatabase::open(databasePath);
        require(result.success(), "Could not open test database");
        return std::move(result.value());
    }

    // Raw access for corrupting rows the public API would never write.
    void sql(const char* statement) const
    {
        sqlite3* handle = nullptr;
        const auto path = databasePath.generic_u8string();
        const auto opened = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),
            &handle, SQLITE_OPEN_READWRITE, nullptr);
        if (opened != SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            throw std::runtime_error("Could not open test SQL connection");
        }
        const int result = sqlite3_exec(handle, statement, nullptr, nullptr, nullptr);
        sqlite3_close(handle);
        require(result == SQLITE_OK, "Test SQL statement failed");
    }
};

std::optional<fs::path> storedScratch(const SqliteVerificationScratchSettingRepository& setting)
{
    const auto loaded = setting.load();
    require(loaded.success(), "Could not read the verification folder setting");
    return loaded.value();
}

void testScratchSettingStoreClearAndRefusals()
{
    Workspace workspace;
    const fs::path first = workspace.root / "scratch-a";
    const fs::path unicode = workspace.root / fs::path(std::u8string(u8"папка-測試"));
    {
        const auto database = workspace.open();
        SqliteVerificationScratchSettingRepository setting(database);
        require(!storedScratch(setting).has_value(), "A new database must have no verification folder");

        require(setting.store(first).success(), "Could not store a path");
        require(storedScratch(setting) == std::optional<fs::path>(first), "The stored path must read back");
        require(setting.store(unicode).success(), "Could not overwrite the stored path");
        require(storedScratch(setting) == std::optional<fs::path>(unicode),
            "A Unicode path must replace the previous one and read back unchanged");

        SqliteVerificationScratchSettingRepository other(database);
        require(storedScratch(other) == std::optional<fs::path>(unicode),
            "A second repository on the same connection must see the setting");

        require(setting.store(std::nullopt).success(), "Could not clear the setting");
        require(!storedScratch(setting).has_value(), "A cleared setting must read as absent");
        require(setting.store(first).success(), "Could not store after clearing");

        // The schema bounds the stored text to 1..2048 bytes; a refusal must keep the previous value.
        const fs::path longest = fs::path(std::string(2048, 'a'));
        require(setting.store(longest).success(), "A path of exactly 2048 bytes must be accepted");
        require(storedScratch(setting) == std::optional<fs::path>(longest), "The longest path must read back");
        const auto tooLong = setting.store(fs::path(std::string(2049, 'a')));
        require(!tooLong.success() && tooLong.error()->code == "persistence.write_failed",
            "A path over 2048 bytes must be refused as a failed write");
        require(storedScratch(setting) == std::optional<fs::path>(longest), "A refused path changed the setting");
        const auto empty = setting.store(fs::path());
        require(!empty.success() && empty.error()->code == "persistence.write_failed",
            "An empty path must be refused as a failed write");
        require(storedScratch(setting) == std::optional<fs::path>(longest), "A refused empty path changed the setting");
        require(setting.store(unicode).success(), "The connection must be usable after refused writes");
    }

    {
        const auto reopened = workspace.open();
        SqliteVerificationScratchSettingRepository setting(reopened);
        require(storedScratch(setting) == std::optional<fs::path>(unicode), "The setting must survive reopening");
    }

    // A missing singleton row reads as no setting, and the next store recreates it.
    workspace.sql("DELETE FROM verification_scratch_setting;");
    const auto database = workspace.open();
    SqliteVerificationScratchSettingRepository setting(database);
    require(!storedScratch(setting).has_value(), "A missing row must read as no setting");
    require(setting.store(first).success(), "Storing must recreate the missing row");
    require(storedScratch(setting) == std::optional<fs::path>(first), "The recreated row must read back");
}
} // namespace

int main()
{
    try {
        testScratchSettingStoreClearAndRefusals();
        std::cout << "SQLite Settings contracts: 1 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Settings contracts failed: " << error.what() << '\n';
        return 1;
    }
}
