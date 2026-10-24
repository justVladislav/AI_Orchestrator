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

void addCatalogRows(const std::shared_ptr<SqliteDatabase>& database)
{
    SqliteProviderRepository providers(database);
    Provider provider;
    provider.id = "provider-1";
    provider.name = "Provider";
    provider.kind = "openai_compatible";
    provider.baseUrl = "https://example.invalid/v1";
    provider.enabled = true;
    provider.createdAt = at(500);
    provider.updatedAt = at(500);
    require(providers.insert(provider).success(), "Could not insert a provider for the settings tests");

    SqliteModelRepository models(database);
    Model model;
    model.id = "model-1";
    model.providerId = "provider-1";
    model.apiModelId = "api-1";
    model.displayName = "Model";
    model.contextWindowTokens = 200'000;
    model.maxOutputTokens = 16'384;
    model.inputPriceMicrosPerMillionTokens = 3'000'000;
    model.outputPriceMicrosPerMillionTokens = 15'000'000;
    model.enabled = true;
    model.createdAt = at(500);
    model.updatedAt = at(500);
    require(models.insert(model).success(), "Could not insert a model for the settings tests");

    SqliteExecutionProfileRepository profiles(database);
    ExecutionProfile profile;
    profile.id = "profile-1";
    profile.name = "Profile";
    profile.workflowId = "settings-test-workflow";
    profile.createdAt = at(500);
    profile.updatedAt = at(500);
    require(profiles.insert(profile).success(), "Could not insert a profile for the settings tests");
}

void requireSameSettings(const ApplicationSettings& actual, const ApplicationSettings& expected, const char* message)
{
    require(actual.theme == expected.theme && actual.interfaceScalePercent == expected.interfaceScalePercent
            && actual.compactMode == expected.compactMode && actual.reduceAnimations == expected.reduceAnimations
            && actual.runContextPolicy == expected.runContextPolicy
            && actual.defaultExecutionProfileId == expected.defaultExecutionProfileId
            && actual.defaultTimeoutMs == expected.defaultTimeoutMs
            && actual.defaultMaxParallelAgents == expected.defaultMaxParallelAgents
            && actual.confirmManyModels == expected.confirmManyModels
            && actual.chatContextPolicy == expected.chatContextPolicy
            && actual.defaultChatProviderId == expected.defaultChatProviderId
            && actual.defaultChatModelId == expected.defaultChatModelId
            && actual.chatHistoryLimit == expected.chatHistoryLimit && actual.revision == expected.revision
            && actual.updatedAt == expected.updatedAt,
        message);
}

ApplicationSettings loadSettings(const SqliteApplicationSettingsRepository& settings)
{
    const auto loaded = settings.load();
    require(loaded.success(), "Could not load the application settings");
    return loaded.value();
}

void testApplicationSettingsDefaultsAndRoundTrip()
{
    Workspace workspace;
    ApplicationSettings full;
    full.theme = ThemePreference::Light;
    full.interfaceScalePercent = 75;
    full.compactMode = true;
    full.reduceAnimations = true;
    full.runContextPolicy = ProjectContextPolicy::AskEveryTime;
    full.defaultExecutionProfileId = "profile-1";
    full.defaultTimeoutMs = 1000;
    full.defaultMaxParallelAgents = 1;
    full.confirmManyModels = false;
    full.chatContextPolicy = ProjectContextPolicy::NoProject;
    full.defaultChatProviderId = "provider-1";
    full.defaultChatModelId = "model-1";
    full.chatHistoryLimit = 1;
    full.revision = 1;
    full.updatedAt = at(5000);
    ApplicationSettings extremes;
    extremes.theme = ThemePreference::Transparent;
    extremes.interfaceScalePercent = 200;
    extremes.compactMode = false;
    extremes.reduceAnimations = true;
    extremes.runContextPolicy = ProjectContextPolicy::NoProject;
    extremes.defaultTimeoutMs = 86'400'000;
    extremes.defaultMaxParallelAgents = 32;
    extremes.confirmManyModels = true;
    extremes.chatContextPolicy = ProjectContextPolicy::AskEveryTime;
    extremes.chatHistoryLimit = 10'000;
    extremes.revision = 2;
    extremes.updatedAt = at(6000);
    {
        const auto database = workspace.open();
        addCatalogRows(database);
        SqliteApplicationSettingsRepository settings(database);

        // The seeded row carries the documented defaults.
        const ApplicationSettings defaults = loadSettings(settings);
        requireSameSettings(defaults, ApplicationSettings {}, "A new database must hold the default settings");
        require(defaults.revision == 0, "The seeded settings must start at revision 0");

        const auto saved = settings.save(full);
        require(saved.success(), "Could not save settings that set every field");
        requireSameSettings(saved.value(), full, "save must return the settings as written");
        requireSameSettings(loadSettings(settings), full, "Every field must round-trip, including set optionals");

        const auto savedAgain = settings.save(extremes);
        require(savedAgain.success(), "Could not save the next revision");
        requireSameSettings(loadSettings(settings), extremes,
            "The next revision must replace every field and clear the optionals");
    }

    const auto reopened = workspace.open();
    SqliteApplicationSettingsRepository settings(reopened);
    requireSameSettings(loadSettings(settings), extremes, "The settings must survive reopening");
}

void testApplicationSettingsRevisionConflicts()
{
    Workspace workspace;
    const auto database = workspace.open();
    SqliteApplicationSettingsRepository settings(database);
    SqliteApplicationSettingsRepository otherWriter(database);

    ApplicationSettings next = loadSettings(settings);
    next.theme = ThemePreference::Light;
    next.revision = 1;
    next.updatedAt = at(1000);
    require(settings.save(next).success(), "Could not save the first revision");
    const ApplicationSettings stored = loadSettings(settings);
    requireSameSettings(stored, next, "The first revision must be stored");

    const auto refuse = [&](std::uint64_t revision, const char* message) {
        ApplicationSettings attempt = stored;
        attempt.theme = ThemePreference::Transparent;
        attempt.revision = revision;
        attempt.updatedAt = at(9999);
        const auto result = settings.save(attempt);
        require(!result.success() && result.error()->code == "settings.revision_conflict", message);
        requireSameSettings(loadSettings(settings), stored, "A refused save changed the stored settings");
    };
    refuse(0, "Revision 0 must be refused as a revision conflict");
    refuse(1, "Repeating the stored revision must be refused as a revision conflict");
    refuse(3, "Skipping a revision must be refused as a revision conflict");

    // Two writers that both started from the stored revision: only the first one wins.
    ApplicationSettings first = stored;
    first.compactMode = true;
    first.revision = 2;
    first.updatedAt = at(2000);
    ApplicationSettings second = stored;
    second.reduceAnimations = true;
    second.revision = 2;
    second.updatedAt = at(3000);
    require(settings.save(first).success(), "The first writer must win");
    const auto lost = otherWriter.save(second);
    require(!lost.success() && lost.error()->code == "settings.revision_conflict",
        "The second writer from the same revision must get settings.revision_conflict");
    requireSameSettings(loadSettings(otherWriter), first, "The losing writer must not change the winner's settings");

    ApplicationSettings retried = first;
    retried.reduceAnimations = true;
    retried.revision = 3;
    retried.updatedAt = at(4000);
    require(otherWriter.save(retried).success(), "A retry on top of the winner's revision must succeed");
    requireSameSettings(loadSettings(settings), retried, "The retried revision must be stored");
}

void testApplicationSettingsRefusedWritesKeepStoredSettings()
{
    Workspace workspace;
    const auto database = workspace.open();
    addCatalogRows(database);
    SqliteApplicationSettingsRepository settings(database);
    ApplicationSettings good = loadSettings(settings);
    good.compactMode = true;
    good.defaultExecutionProfileId = "profile-1";
    good.revision = 1;
    good.updatedAt = at(1000);
    require(settings.save(good).success(), "Could not save the baseline settings");

    const auto refuse = [&](const char* code, const char* message, auto&& change) {
        ApplicationSettings attempt = good;
        attempt.revision = good.revision + 1;
        attempt.updatedAt = at(9999);
        change(attempt);
        const auto result = settings.save(attempt);
        require(!result.success() && result.error()->code == code, message);
        requireSameSettings(loadSettings(settings), good, "A refused save changed the stored settings");
    };
    constexpr const char* writeFailed = "persistence.write_failed";
    refuse(writeFailed, "A scale below the allowed range must be refused", [](auto& s) { s.interfaceScalePercent = 50; });
    refuse(writeFailed, "A scale above the allowed range must be refused", [](auto& s) { s.interfaceScalePercent = 201; });
    refuse(writeFailed, "A timeout below the allowed range must be refused", [](auto& s) { s.defaultTimeoutMs = 999; });
    refuse(writeFailed, "A timeout above the allowed range must be refused", [](auto& s) { s.defaultTimeoutMs = 86'400'001; });
    refuse(writeFailed, "Zero parallel agents must be refused", [](auto& s) { s.defaultMaxParallelAgents = 0; });
    refuse(writeFailed, "Too many parallel agents must be refused", [](auto& s) { s.defaultMaxParallelAgents = 33; });
    refuse(writeFailed, "A zero history limit must be refused", [](auto& s) { s.chatHistoryLimit = 0; });
    refuse(writeFailed, "A history limit above the range must be refused", [](auto& s) { s.chatHistoryLimit = 10'001; });
    refuse(writeFailed, "An unknown default profile must be refused", [](auto& s) { s.defaultExecutionProfileId = "profile-unknown"; });
    refuse(writeFailed, "An unknown chat provider must be refused", [](auto& s) { s.defaultChatProviderId = "provider-unknown"; });
    refuse(writeFailed, "An unknown chat model must be refused", [](auto& s) { s.defaultChatModelId = "model-unknown"; });
    refuse("persistence.value_out_of_range", "A timeout beyond the signed 64-bit range must be refused",
        [](auto& s) { s.defaultTimeoutMs = std::numeric_limits<std::uint64_t>::max(); });

    ApplicationSettings next = good;
    next.theme = ThemePreference::Light;
    next.revision = 2;
    next.updatedAt = at(2000);
    require(settings.save(next).success(), "The connection must be usable after refused saves");
    requireSameSettings(loadSettings(settings), next, "A save after refusals must apply in full");
}

void testApplicationSettingsCorruptRowsFailClosed()
{
    Workspace workspace;
    { const auto database = workspace.open(); }
    const auto loadCode = [&]() -> std::string {
        const auto database = workspace.open();
        SqliteApplicationSettingsRepository settings(database);
        const auto loaded = settings.load();
        return loaded.success() ? std::string() : loaded.error()->code;
    };
    require(loadCode().empty(), "The seeded settings must load");

    const struct { const char* corrupt; const char* repair; } cases[] = {
        {"theme_mode='neon'", "theme_mode='dark'"},
        {"run_context_policy='sometimes'", "run_context_policy='active_project'"},
        {"chat_context_policy='sometimes'", "chat_context_policy='active_project'"}};
    for (const auto& item : cases) {
        workspace.sql((std::string("PRAGMA ignore_check_constraints=1; UPDATE application_settings SET ")
            + item.corrupt + " WHERE id=1;").c_str());
        require(loadCode() == "persistence.invalid_data", "An unknown enum value must fail closed on load");
        workspace.sql((std::string("UPDATE application_settings SET ") + item.repair + " WHERE id=1;").c_str());
        require(loadCode().empty(), "Repairing the value must make the settings readable again");
    }

    // Without the singleton row there is nothing to read and nothing to update.
    workspace.sql("DELETE FROM application_settings;");
    require(loadCode() == "persistence.read_failed", "A missing settings row must be reported as a failed read");
    const auto database = workspace.open();
    SqliteApplicationSettingsRepository settings(database);
    ApplicationSettings attempt;
    attempt.revision = 1;
    const auto saved = settings.save(attempt);
    require(!saved.success() && saved.error()->code == "settings.revision_conflict",
        "Saving without the settings row must report a revision conflict");
}
} // namespace

int main()
{
    try {
        testScratchSettingStoreClearAndRefusals();
        testApplicationSettingsDefaultsAndRoundTrip();
        testApplicationSettingsRevisionConflicts();
        testApplicationSettingsRefusedWritesKeepStoredSettings();
        testApplicationSettingsCorruptRowsFailClosed();
        std::cout << "SQLite Settings contracts: 5 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Settings contracts failed: " << error.what() << '\n';
        return 1;
    }
}
