#include "ai/infrastructure/SqliteRepositories.hpp"

#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
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
    fs::path root = fs::temp_directory_path() / ("aio-sqlite-catalog-"
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
};

Provider provider(std::string id, Time createdAt, std::optional<std::string> credential = std::nullopt)
{
    Provider value;
    value.id = std::move(id);
    value.name = "Provider \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.kind = "openai_compatible";
    value.baseUrl = "https://example.invalid/v1";
    value.enabled = true;
    value.credentialReference = std::move(credential);
    value.createdAt = createdAt;
    value.updatedAt = createdAt + std::chrono::milliseconds(7);
    return value;
}

void requireSame(const Provider& actual, const Provider& expected, const char* message)
{
    require(actual.id == expected.id && actual.name == expected.name && actual.kind == expected.kind
            && actual.baseUrl == expected.baseUrl && actual.enabled == expected.enabled
            && actual.credentialReference == expected.credentialReference
            && actual.createdAt == expected.createdAt && actual.updatedAt == expected.updatedAt,
        message);
}

void testProviderRoundTripOrderingAndLifetime()
{
    Workspace workspace;
    Provider plain = provider("provider-b", at(3000));
    Provider keyed = provider("provider-a", at(3000), std::string("credential-reference-1"));
    keyed.kind = "anthropic";
    keyed.enabled = false;
    Provider earliest = provider("provider-z", at(1000));
    earliest.kind = "local";

    {
        const auto database = workspace.open();
        SqliteProviderRepository providers(database);
        SqliteProviderRepository sameConnection(database);
        for (const Provider& value : {plain, keyed, earliest}) {
            const auto inserted = providers.insert(value);
            require(inserted.success(), "Provider insert failed");
            requireSame(inserted.value(), value, "Insert must return the provider it stored");
        }

        const auto missing = providers.findById("missing-provider");
        require(missing.success() && !missing.value().has_value(), "A missing provider must be an empty result");
        const auto seenElsewhere = sameConnection.findById("provider-a");
        require(seenElsewhere.success() && seenElsewhere.value().has_value(),
            "A repository sharing the database must see the insert");
        requireSame(*seenElsewhere.value(), keyed, "Credential reference, kind and enabled flag must round-trip");
    }

    const auto reopened = workspace.open();
    SqliteProviderRepository providers(reopened);
    const auto all = providers.listAll();
    require(all.success() && all.value().size() == 3, "Three providers must survive reopen");
    require(all.value()[0].id == "provider-z" && all.value()[1].id == "provider-a"
            && all.value()[2].id == "provider-b",
        "Providers are ordered by creation time, then id");
    requireSame(all.value()[0], earliest, "Earliest provider differs after reopen");
    requireSame(all.value()[1], keyed, "Keyed provider differs after reopen");
    requireSame(all.value()[2], plain, "Plain provider differs after reopen");
    require(!all.value()[2].credentialReference.has_value(), "An absent credential reference must stay absent");
}

void testProviderDuplicateUpdateAndMissing()
{
    Workspace workspace;
    const auto database = workspace.open();
    SqliteProviderRepository providers(database);
    const Provider original = provider("provider-1", at(2000), std::string("credential-reference-1"));
    require(providers.insert(original).success(), "Could not insert provider");

    Provider duplicate = original;
    duplicate.name = "Different name";
    const auto refused = providers.insert(duplicate);
    require(!refused.success() && refused.error()->code == "provider.duplicate_id",
        "A duplicate provider id must report provider.duplicate_id");
    const auto untouched = providers.findById("provider-1");
    require(untouched.success() && untouched.value().has_value(), "Original provider disappeared");
    requireSame(*untouched.value(), original, "A refused insert must not change the stored provider");

    Provider changed = original;
    changed.name = "Renamed";
    changed.kind = "openai";
    changed.baseUrl = "https://example.invalid/v2";
    changed.enabled = false;
    changed.credentialReference = std::nullopt;
    changed.createdAt = at(2500);
    changed.updatedAt = at(9000);
    const auto updated = providers.update(changed);
    require(updated.success(), "Provider update failed");
    requireSame(updated.value(), changed, "Update must return the provider it stored");
    const auto stored = providers.findById("provider-1");
    require(stored.success() && stored.value().has_value(), "Updated provider disappeared");
    requireSame(*stored.value(), changed, "Every column, including created time and a cleared credential, is replaced");

    Provider absent = provider("provider-absent", at(1));
    const auto missing = providers.update(absent);
    require(!missing.success() && missing.error()->code == "provider.not_found",
        "Updating a missing provider must report provider.not_found");
    const auto none = providers.findById("provider-absent");
    require(none.success() && !none.value().has_value(), "A refused update must not create a provider");
    const auto all = providers.listAll();
    require(all.success() && all.value().size() == 1, "Refused writes must not add rows");
}

void testProviderConstraintFailuresDoNotPoisonConnection()
{
    Workspace workspace;
    const auto database = workspace.open();
    SqliteProviderRepository providers(database);
    const Provider original = provider("provider-1", at(2000));
    require(providers.insert(original).success(), "Could not insert provider");

    Provider invalidKind = provider("provider-2", at(3000));
    invalidKind.kind = "not-a-supported-kind";
    const auto badInsert = providers.insert(invalidKind);
    require(!badInsert.success() && badInsert.error()->code == "persistence.write_failed",
        "A provider kind outside the schema CHECK must be a write failure, not a duplicate id");

    Provider badUpdate = original;
    badUpdate.kind = "not-a-supported-kind";
    badUpdate.name = "Must not be stored";
    const auto refusedUpdate = providers.update(badUpdate);
    require(!refusedUpdate.success() && refusedUpdate.error()->code == "persistence.write_failed",
        "An update violating the schema CHECK must be a write failure");

    const auto stored = providers.findById("provider-1");
    require(stored.success() && stored.value().has_value(), "Original provider disappeared");
    requireSame(*stored.value(), original, "A refused update must leave the row untouched");

    require(providers.insert(provider("provider-3", at(4000))).success(),
        "The connection must stay usable after refused writes");
    const auto all = providers.listAll();
    require(all.success() && all.value().size() == 2, "Only the valid providers may exist");
}

void testConcurrentProviderWritersShareOneConnection()
{
    Workspace workspace;
    const auto database = workspace.open();
    SqliteProviderRepository first(database);
    SqliteProviderRepository second(database);
    constexpr int perWriter = 25;
    std::barrier ready(2);
    bool firstOk = true;
    bool secondOk = true;
    std::thread a([&] {
        ready.arrive_and_wait();
        for (int i = 0; i < perWriter; ++i) {
            firstOk = first.insert(provider("a-" + std::to_string(i), at(1000 + i))).success() && firstOk;
        }
    });
    std::thread b([&] {
        ready.arrive_and_wait();
        for (int i = 0; i < perWriter; ++i) {
            secondOk = second.insert(provider("b-" + std::to_string(i), at(1000 + i))).success() && secondOk;
        }
    });
    a.join();
    b.join();
    require(firstOk && secondOk, "Concurrent provider inserts through the shared connection must all succeed");
    const auto all = first.listAll();
    require(all.success() && all.value().size() == 2 * perWriter, "Every concurrent insert must be stored once");
}
} // namespace

int main()
{
    try {
        testProviderRoundTripOrderingAndLifetime();
        testProviderDuplicateUpdateAndMissing();
        testProviderConstraintFailuresDoNotPoisonConnection();
        testConcurrentProviderWritersShareOneConnection();
        std::cout << "SQLite Catalog contracts: 4 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Catalog contracts failed: " << error.what() << '\n';
        return 1;
    }
}
