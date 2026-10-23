#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
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
void addProvider(const std::shared_ptr<SqliteDatabase>& database, const std::string& id)
{
    SqliteProviderRepository providers(database);
    require(providers.insert(provider(id, at(500))).success(), "Could not insert a provider for the model tests");
}

Model model(std::string id, std::string providerId, std::string apiModelId, Time createdAt)
{
    Model value;
    value.id = std::move(id);
    value.providerId = std::move(providerId);
    value.apiModelId = std::move(apiModelId);
    value.displayName = "Model \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.contextWindowTokens = 200'000;
    value.maxOutputTokens = 16'384;
    value.inputPriceMicrosPerMillionTokens = 3'000'000;
    value.outputPriceMicrosPerMillionTokens = 15'000'000;
    value.enabled = true;
    value.isDefault = false;
    value.createdAt = createdAt;
    value.updatedAt = createdAt + std::chrono::milliseconds(7);
    return value;
}

// Every price column carries a distinct value so a swapped column or binding is visible.
Model pricedModel(std::string id, std::string providerId, std::string apiModelId, Time createdAt)
{
    Model value = model(std::move(id), std::move(providerId), std::move(apiModelId), createdAt);
    value.cacheWritePriceMicrosPerMillionTokens = 3'750'000;
    value.cacheReadPriceMicrosPerMillionTokens = 300'000;
    value.cacheWriteOneHourPriceMicrosPerMillionTokens = 6'000'000;
    value.longContextPricing.thresholdInputTokens = 100'000;
    value.longContextPricing.inputPriceMicrosPerMillionTokens = 6'000'000;
    value.longContextPricing.outputPriceMicrosPerMillionTokens = 22'500'000;
    value.longContextPricing.cacheWritePriceMicrosPerMillionTokens = 7'500'000;
    value.longContextPricing.cacheReadPriceMicrosPerMillionTokens = 600'000;
    value.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens = 12'000'000;
    return value;
}

void requireSameModel(const Model& actual, const Model& expected, const char* message)
{
    require(actual.id == expected.id && actual.providerId == expected.providerId
            && actual.apiModelId == expected.apiModelId && actual.displayName == expected.displayName
            && actual.contextWindowTokens == expected.contextWindowTokens
            && actual.maxOutputTokens == expected.maxOutputTokens
            && actual.inputPriceMicrosPerMillionTokens == expected.inputPriceMicrosPerMillionTokens
            && actual.outputPriceMicrosPerMillionTokens == expected.outputPriceMicrosPerMillionTokens
            && actual.cacheWritePriceMicrosPerMillionTokens == expected.cacheWritePriceMicrosPerMillionTokens
            && actual.cacheReadPriceMicrosPerMillionTokens == expected.cacheReadPriceMicrosPerMillionTokens
            && actual.cacheWriteOneHourPriceMicrosPerMillionTokens
                == expected.cacheWriteOneHourPriceMicrosPerMillionTokens
            && actual.longContextPricing == expected.longContextPricing
            && actual.enabled == expected.enabled && actual.isDefault == expected.isDefault
            && actual.createdAt == expected.createdAt && actual.updatedAt == expected.updatedAt,
        message);
}

std::vector<std::string> ids(const std::vector<Model>& models)
{
    std::vector<std::string> result;
    for (const auto& value : models) result.push_back(value.id);
    return result;
}

void testModelRoundTripPricingOrderingAndFilter()
{
    Workspace workspace;
    const Model flat = model("model-flat", "provider-1", "api-flat", at(3000));
    const Model priced = pricedModel("model-long", "provider-1", "api-long", at(1000));
    Model cached = model("model-cache", "provider-2", "api-cache", at(2000));
    cached.enabled = false;
    cached.cacheWritePriceMicrosPerMillionTokens = 1'250'000;
    cached.cacheReadPriceMicrosPerMillionTokens = 125'000;
    cached.cacheWriteOneHourPriceMicrosPerMillionTokens = 2'000'000;
    const Model tied = model("model-a", "provider-1", "api-tied", at(3000));

    {
        const auto database = workspace.open();
        addProvider(database, "provider-1");
        addProvider(database, "provider-2");
        SqliteModelRepository models(database);
        SqliteModelRepository sameConnection(database);
        for (const Model& value : {flat, priced, cached, tied}) {
            const auto inserted = models.insert(value);
            require(inserted.success(), "Model insert failed");
            requireSameModel(inserted.value(), value, "Insert must return the model it stored");
        }
        const auto missing = models.findById("missing-model");
        require(missing.success() && !missing.value().has_value(), "A missing model must be an empty result");
        const auto seen = sameConnection.findById("model-long");
        require(seen.success() && seen.value().has_value(), "A repository sharing the database must see the insert");
        requireSameModel(*seen.value(), priced, "Flat, cache, one-hour and long-context prices must round-trip");
    }

    const auto reopened = workspace.open();
    SqliteModelRepository models(reopened);
    const auto all = models.listAll();
    require(all.success() && all.value().size() == 4, "Four models must survive reopen");
    require((ids(all.value()) == std::vector<std::string> {"model-long", "model-cache", "model-a", "model-flat"}),
        "Models are ordered by creation time, then id");
    requireSameModel(all.value()[0], priced, "Priced model differs after reopen");
    requireSameModel(all.value()[1], cached, "Cache-priced model differs after reopen");
    requireSameModel(all.value()[2], tied, "Tied model differs after reopen");
    requireSameModel(all.value()[3], flat, "Flat model differs after reopen");

    const auto first = models.listAll(std::optional<ProviderId>("provider-1"));
    require(first.success()
            && (ids(first.value()) == std::vector<std::string> {"model-long", "model-a", "model-flat"}),
        "A provider filter must keep only its models, in the same order");
    const auto second = models.listAll(std::optional<ProviderId>("provider-2"));
    require(second.success() && (ids(second.value()) == std::vector<std::string> {"model-cache"}),
        "The second provider's filter differs");
    const auto none = models.listAll(std::optional<ProviderId>("provider-unknown"));
    require(none.success() && none.value().empty(), "An unknown provider filter must be empty, not an error");
}

void testModelWriteErrorsAndStableDefault()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProvider(database, "provider-1");
    addProvider(database, "provider-2");
    SqliteModelRepository models(database);
    const Model first = model("model-1", "provider-1", "api-1", at(1000));
    const Model second = model("model-2", "provider-1", "api-2", at(2000));
    require(models.insert(first).success() && models.insert(second).success(), "Could not insert models");
    require(models.setDefaultModel("provider-1", "model-1").success(), "Could not set the default model");

    Model duplicateId = model("model-1", "provider-1", "api-other", at(3000));
    const auto refusedId = models.insert(duplicateId);
    require(!refusedId.success() && refusedId.error()->code == "model.duplicate_id",
        "A duplicate model id must report model.duplicate_id");
    const Model duplicateApi = model("model-3", "provider-1", "api-1", at(3000));
    const auto refusedApi = models.insert(duplicateApi);
    require(!refusedApi.success() && refusedApi.error()->code == "model.duplicate_api_model_id",
        "A duplicate apiModelId under one provider must report model.duplicate_api_model_id");
    require(models.insert(model("model-4", "provider-2", "api-1", at(4000))).success(),
        "The same apiModelId under another provider is allowed");

    Model collide = second;
    collide.apiModelId = "api-1";
    const auto refusedUpdate = models.update(collide);
    require(!refusedUpdate.success() && refusedUpdate.error()->code == "model.duplicate_api_model_id",
        "An update onto a taken apiModelId must report model.duplicate_api_model_id");
    const auto unchanged = models.findById("model-2");
    require(unchanged.success() && unchanged.value().has_value(), "Second model disappeared");
    requireSameModel(*unchanged.value(), second, "A refused update must leave the row untouched");

    Model absent = model("model-absent", "provider-1", "api-absent", at(5000));
    const auto missingUpdate = models.update(absent);
    require(!missingUpdate.success() && missingUpdate.error()->code == "model.not_found",
        "Updating a missing model must report model.not_found");

    Model unknownProvider = model("model-5", "provider-unknown", "api-5", at(6000));
    const auto fkInsert = models.insert(unknownProvider);
    require(!fkInsert.success() && fkInsert.error()->code == "persistence.write_failed",
        "A model under an unknown provider must be a write failure");
    Model moved = second;
    moved.providerId = "provider-unknown";
    const auto fkUpdate = models.update(moved);
    require(!fkUpdate.success() && fkUpdate.error()->code == "persistence.write_failed",
        "Moving a model to an unknown provider must be a write failure");

    Model huge = model("model-huge", "provider-1", "api-huge", at(7000));
    huge.inputPriceMicrosPerMillionTokens = std::numeric_limits<std::uint64_t>::max();
    const auto overflow = models.insert(huge);
    require(!overflow.success() && overflow.error()->code == "persistence.value_out_of_range",
        "A price above SQLite's integer range must be refused before writing");
    Model hugeUpdate = second;
    hugeUpdate.longContextPricing.cacheReadPriceMicrosPerMillionTokens = std::numeric_limits<std::uint64_t>::max();
    const auto overflowUpdate = models.update(hugeUpdate);
    require(!overflowUpdate.success() && overflowUpdate.error()->code == "persistence.value_out_of_range",
        "An out-of-range price in an update must be refused");

    // update() may change every price, but never the provider's default flag.
    Model repriced = pricedModel("model-1", "provider-1", "api-1-renamed", at(1000));
    repriced.displayName = "Renamed";
    repriced.enabled = false;
    repriced.isDefault = false;
    require(models.update(repriced).success(), "Could not update the default model");
    const auto stored = models.findById("model-1");
    require(stored.success() && stored.value().has_value(), "Default model disappeared");
    Model expected = repriced;
    expected.isDefault = true;
    requireSameModel(*stored.value(), expected, "An update must replace prices and keep the stored default flag");

    require(models.insert(model("model-6", "provider-1", "api-6", at(8000))).success(),
        "The connection must stay usable after refused writes");
    const auto all = models.listAll();
    require(all.success() && all.value().size() == 4, "Only the valid models may exist");
}

void testSetDefaultModelIsAtomic()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProvider(database, "provider-1");
    addProvider(database, "provider-2");
    SqliteModelRepository models(database);
    require(models.insert(model("model-1", "provider-1", "api-1", at(1000))).success()
            && models.insert(model("model-2", "provider-1", "api-2", at(2000))).success()
            && models.insert(model("model-3", "provider-2", "api-3", at(3000))).success(),
        "Could not insert models");

    const auto defaultIds = [&](const char* owner) {
        std::vector<std::string> result;
        const auto listed = models.listAll(std::optional<ProviderId>(owner));
        require(listed.success(), "Could not list models");
        for (const auto& value : listed.value()) if (value.isDefault) result.push_back(value.id);
        return result;
    };
    using Ids = std::vector<std::string>;

    require(models.setDefaultModel("provider-1", "model-1").success()
            && models.setDefaultModel("provider-2", "model-3").success(),
        "Could not set initial defaults");
    require(defaultIds("provider-1") == Ids {"model-1"} && defaultIds("provider-2") == Ids {"model-3"},
        "Each provider must have its own default");

    require(models.setDefaultModel("provider-1", "model-2").success(), "Could not move the default");
    require(defaultIds("provider-1") == Ids {"model-2"} && defaultIds("provider-2") == Ids {"model-3"},
        "Moving a default must leave exactly one for that provider and not touch another provider");

    // Each refusal first clears the old default inside the transaction; it must be restored.
    const auto missing = models.setDefaultModel("provider-1", "model-missing");
    require(!missing.success() && missing.error()->code == "model.provider_mismatch",
        "A missing model must report model.provider_mismatch");
    require(defaultIds("provider-1") == Ids {"model-2"}, "A refused default change lost the previous default");

    const auto foreign = models.setDefaultModel("provider-1", "model-3");
    require(!foreign.success() && foreign.error()->code == "model.provider_mismatch",
        "A model of another provider must report model.provider_mismatch");
    require(defaultIds("provider-1") == Ids {"model-2"} && defaultIds("provider-2") == Ids {"model-3"},
        "A refused cross-provider change altered a default");

    const auto unknown = models.setDefaultModel("provider-unknown", "model-1");
    require(!unknown.success() && unknown.error()->code == "model.provider_mismatch",
        "An unknown provider must report model.provider_mismatch");
    require(defaultIds("provider-1") == Ids {"model-2"}, "An unknown-provider refusal altered a default");

    require(models.setDefaultModel("provider-1", "model-2").success(), "Setting the current default again must succeed");
    require(defaultIds("provider-1") == Ids {"model-2"}, "Re-setting the default must keep it");

    // A rolled-back transaction must not leave the connection inside one.
    require(models.insert(model("model-4", "provider-1", "api-4", at(4000))).success(),
        "The connection must be usable after a refused default change");
    require(models.setDefaultModel("provider-1", "model-4").success(), "A later default change must work");
    require(defaultIds("provider-1") == Ids {"model-4"}, "The later default change did not apply");
}

void testModelRemoveNotFoundAndInUse()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProvider(database, "provider-1");
    SqliteModelRepository models(database);
    require(models.insert(model("model-free", "provider-1", "api-free", at(1000))).success()
            && models.insert(model("model-used", "provider-1", "api-used", at(2000))).success(),
        "Could not insert models");

    SqliteExecutionProfileRepository profiles(database);
    ExecutionProfile profile;
    profile.id = "profile-1";
    profile.name = "References a model";
    profile.workflowId = "catalog-test-workflow";
    profile.createdAt = at(3000);
    profile.updatedAt = at(3000);
    StageModelAssignment assignment;
    assignment.stageType = "architect";
    assignment.providerId = "provider-1";
    assignment.modelId = "model-used";
    assignment.timeout = std::chrono::milliseconds(60'000);
    profile.assignments.push_back(assignment);
    const auto savedProfile = profiles.insert(profile);
    require(savedProfile.success(), "Could not insert the profile that references a model");

    const auto inUse = models.remove("model-used");
    require(!inUse.success() && inUse.error()->code == "model.in_use",
        "Deleting a referenced model must report model.in_use");
    const auto stillThere = models.findById("model-used");
    require(stillThere.success() && stillThere.value().has_value(), "A refused removal deleted the model");

    const auto missing = models.remove("model-missing");
    require(!missing.success() && missing.error()->code == "model.not_found",
        "Removing a missing model must report model.not_found");

    const auto removed = models.remove("model-free");
    require(removed.success(), "An unreferenced model must be removable");
    const auto gone = models.findById("model-free");
    require(gone.success() && !gone.value().has_value(), "A removed model must disappear");
    const auto again = models.remove("model-free");
    require(!again.success() && again.error()->code == "model.not_found", "A second removal must report model.not_found");
    const auto all = models.listAll();
    require(all.success() && all.value().size() == 1 && all.value()[0].id == "model-used",
        "Only the referenced model may remain");
}

void testCorruptModelRowsFailClosed()
{
    Workspace workspace;
    {
        const auto database = workspace.open();
        addProvider(database, "provider-good");
        addProvider(database, "provider-bad");
        SqliteModelRepository models(database);
        require(models.insert(model("model-good", "provider-good", "api-good", at(1000))).success()
                && models.insert(model("model-orphan-tier", "provider-bad", "api-1", at(2000))).success()
                && models.insert(model("model-bad-threshold", "provider-bad", "api-2", at(3000))).success(),
            "Could not insert models");
    }

    workspace.sql("UPDATE models SET long_context_input_price_micros_per_million_tokens=5 "
                  "WHERE id='model-orphan-tier';");
    workspace.sql("UPDATE models SET long_context_threshold_input_tokens=200000 "
                  "WHERE id='model-bad-threshold';");

    const auto database = workspace.open();
    SqliteModelRepository models(database);
    const auto good = models.findById("model-good");
    require(good.success() && good.value().has_value(), "A valid model must stay readable");

    for (const char* id : {"model-orphan-tier", "model-bad-threshold"}) {
        const auto bad = models.findById(id);
        require(!bad.success() && bad.error()->code == "persistence.invalid_data",
            "A row with an invalid long-context tier must fail closed on findById");
    }
    const auto everything = models.listAll();
    require(!everything.success() && everything.error()->code == "persistence.invalid_data",
        "A listing that contains an invalid row must fail rather than skip it");
    const auto onlyGood = models.listAll(std::optional<ProviderId>("provider-good"));
    require(onlyGood.success() && onlyGood.value().size() == 1,
        "A listing filtered to a healthy provider must still work");
}

void addCatalog(const std::shared_ptr<SqliteDatabase>& database)
{
    addProvider(database, "provider-1");
    addProvider(database, "provider-2");
    SqliteModelRepository models(database);
    require(models.insert(model("model-1", "provider-1", "api-1", at(1000))).success()
            && models.insert(model("model-2", "provider-1", "api-2", at(2000))).success()
            && models.insert(model("model-3", "provider-2", "api-3", at(3000))).success(),
        "Could not insert models for the profile tests");
}

StageModelAssignment makeAssignment(
    std::string stage, std::string providerId, std::string modelId, std::int64_t timeoutMs)
{
    StageModelAssignment value;
    value.stageType = std::move(stage);
    value.providerId = std::move(providerId);
    value.modelId = std::move(modelId);
    value.timeout = std::chrono::milliseconds(timeoutMs);
    return value;
}

ExecutionProfile makeProfile(std::string id, Time createdAt, std::vector<StageModelAssignment> roster = {})
{
    ExecutionProfile value;
    value.id = std::move(id);
    value.name = "Profile \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.workflowId = "catalog-test-workflow";
    value.enabled = true;
    value.isDefault = false;
    value.assignments = std::move(roster);
    value.createdAt = createdAt;
    value.updatedAt = createdAt + std::chrono::milliseconds(7);
    return value;
}

// Every limit carries a distinct value below its Backend ceiling so a swapped field is visible.
ProviderExecutionBudgetOverrides fullBudget()
{
    ProviderExecutionBudgetOverrides value;
    value.maxInputTokens = 400'000;
    value.maxOutputTokens = 32'000;
    value.maxTurns = 12;
    value.maxToolCalls = 100;
    value.maxToolResultBytes = 4ULL * 1024 * 1024;
    value.maxWallTimeMs = 600'000;
    value.maxRequestTimeMs = 120'000;
    return value;
}

ExecutionProfile sortedByStage(ExecutionProfile value)
{
    std::sort(value.assignments.begin(), value.assignments.end(),
        [](const StageModelAssignment& a, const StageModelAssignment& b) { return a.stageType < b.stageType; });
    return value;
}

void requireSameProfile(const ExecutionProfile& actual, const ExecutionProfile& expected, const char* message)
{
    const auto& a = actual.providerBudget;
    const auto& e = expected.providerBudget;
    bool same = actual.id == expected.id && actual.name == expected.name
        && actual.workflowId == expected.workflowId && actual.enabled == expected.enabled
        && actual.isDefault == expected.isDefault && actual.createdAt == expected.createdAt
        && actual.updatedAt == expected.updatedAt
        && a.maxInputTokens == e.maxInputTokens && a.maxOutputTokens == e.maxOutputTokens
        && a.maxTurns == e.maxTurns && a.maxToolCalls == e.maxToolCalls
        && a.maxToolResultBytes == e.maxToolResultBytes && a.maxWallTimeMs == e.maxWallTimeMs
        && a.maxRequestTimeMs == e.maxRequestTimeMs
        && actual.assignments.size() == expected.assignments.size();
    for (std::size_t i = 0; same && i < actual.assignments.size(); ++i) {
        const auto& x = actual.assignments[i];
        const auto& y = expected.assignments[i];
        same = x.stageType == y.stageType && x.providerId == y.providerId && x.modelId == y.modelId
            && x.timeout == y.timeout;
    }
    require(same, message);
}

// The profile as stored, with its roster in the stored (stage type) order.
void requireStoredProfile(const SqliteExecutionProfileRepository& profiles, const ExecutionProfile& expected,
    const char* message)
{
    const auto stored = profiles.findById(expected.id);
    require(stored.success() && stored.value().has_value(), message);
    requireSameProfile(*stored.value(), sortedByStage(expected), message);
}

std::vector<std::string> profileIds(const SqliteExecutionProfileRepository& profiles)
{
    const auto listed = profiles.listAll();
    require(listed.success(), "Could not list execution profiles");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

void testProfileRoundTripOrderingAndReopen()
{
    Workspace workspace;
    ExecutionProfile rich = makeProfile("profile-a", at(1000), {
        makeAssignment("implementation", "provider-2", "model-3", 90'000),
        makeAssignment("architect", "provider-1", "model-1", 60'000),
        makeAssignment("documentation", "provider-1", "model-2", 30'000)});
    rich.enabled = false;
    rich.providerBudget = fullBudget();
    const ExecutionProfile bare = makeProfile("profile-b", at(1000));
    const ExecutionProfile late = makeProfile("profile-late", at(2000),
        {makeAssignment("architect", "provider-1", "model-1", 15'000)});
    using Ids = std::vector<std::string>;
    {
        const auto database = workspace.open();
        addCatalog(database);
        SqliteExecutionProfileRepository profiles(database);
        const auto savedLate = profiles.insert(late);
        require(savedLate.success(), "Could not insert a profile");
        const auto savedBare = profiles.insert(bare);
        require(savedBare.success(), "Could not insert a profile without a roster");
        const auto savedRich = profiles.insert(rich);
        require(savedRich.success(), "Could not insert a profile with a full roster and budget");
        requireSameProfile(savedRich.value(), rich, "insert must return the profile as written");

        requireStoredProfile(profiles, rich, "A stored profile must round-trip every field");
        const auto stored = profiles.findById("profile-a");
        require(stored.value()->assignments.size() == 3
                && stored.value()->assignments[0].stageType == "architect"
                && stored.value()->assignments[1].stageType == "documentation"
                && stored.value()->assignments[2].stageType == "implementation",
            "A roster must be read in stage type order whatever the insertion order was");
        requireStoredProfile(profiles, bare, "A profile without a roster or budget must round-trip");
        requireStoredProfile(profiles, late, "A later profile must round-trip");
        require(profileIds(profiles) == Ids {"profile-a", "profile-b", "profile-late"},
            "Profiles must be listed by creation time and then id");

        const auto missing = profiles.findById("profile-missing");
        require(missing.success() && !missing.value().has_value(), "A missing profile must read as absent");
    }

    const auto reopened = workspace.open();
    SqliteExecutionProfileRepository profiles(reopened);
    requireStoredProfile(profiles, rich, "A profile must survive reopening the database");
    requireStoredProfile(profiles, late, "A roster must survive reopening the database");
    require(profileIds(profiles) == Ids {"profile-a", "profile-b", "profile-late"},
        "The listing order must survive reopening the database");
    const auto listed = profiles.listAll();
    require(listed.value()[0].assignments.size() == 3 && listed.value()[1].assignments.empty()
            && listed.value()[2].assignments.size() == 1,
        "A listing must attach each profile's own roster");
}

void testProfileWriteErrorsAreAtomic()
{
    Workspace workspace;
    const auto database = workspace.open();
    addCatalog(database);
    SqliteExecutionProfileRepository profiles(database);
    const ExecutionProfile first = makeProfile("profile-1", at(1000),
        {makeAssignment("architect", "provider-1", "model-1", 60'000)});
    require(profiles.insert(first).success(), "Could not insert the first profile");

    const auto refuse = [&](ExecutionProfile attempt, const char* code, const char* message) {
        const auto result = profiles.insert(std::move(attempt));
        require(!result.success() && result.error()->code == code, message);
        require(profileIds(profiles) == std::vector<std::string> {"profile-1"},
            "A refused insert must not leave a profile or a partial roster behind");
        requireStoredProfile(profiles, first, "A refused insert altered an existing profile");
    };

    ExecutionProfile duplicateId = makeProfile("profile-1", at(2000),
        {makeAssignment("implementation", "provider-2", "model-3", 1000)});
    duplicateId.name = "Must not replace the first profile";
    refuse(duplicateId, "execution_profile.duplicate_id", "A repeated profile id must report execution_profile.duplicate_id");

    refuse(makeProfile("profile-2", at(2000), {
               makeAssignment("architect", "provider-1", "model-1", 1000),
               makeAssignment("architect", "provider-1", "model-2", 2000)}),
        "execution_profile.duplicate_stage_type",
        "A repeated stage type must report execution_profile.duplicate_stage_type");
    refuse(makeProfile("profile-2", at(2000), {
               makeAssignment("architect", "provider-1", "model-1", 1000),
               makeAssignment("implementation", "provider-1", "model-missing", 2000)}),
        "persistence.write_failed", "An unknown model must be refused as a failed write");
    refuse(makeProfile("profile-2", at(2000), {
               makeAssignment("architect", "provider-missing", "model-1", 1000)}),
        "persistence.write_failed", "An unknown provider must be refused as a failed write");
    refuse(makeProfile("profile-2", at(2000), {
               makeAssignment("architect", "provider-1", "model-1", 1000),
               makeAssignment("implementation", "provider-1", "model-2", 0)}),
        "persistence.write_failed", "A zero timeout must be refused as a failed write");
    refuse(makeProfile("profile-2", at(2000), {
               makeAssignment("architect", "provider-1", "model-1", -5)}),
        "persistence.write_failed", "A negative timeout must be refused as a failed write");
    refuse(makeProfile("profile-2", at(2000), {
               makeAssignment("not-a-stage", "provider-1", "model-1", 1000)}),
        "persistence.write_failed", "An unknown stage type must be refused as a failed write");

    // Only one profile may be the default.
    ExecutionProfile defaultProfile = makeProfile("profile-d1", at(3000));
    defaultProfile.isDefault = true;
    require(profiles.insert(defaultProfile).success(), "Could not insert a default profile");
    ExecutionProfile secondDefault = makeProfile("profile-d2", at(4000));
    secondDefault.isDefault = true;
    const auto clash = profiles.insert(secondDefault);
    require(!clash.success() && clash.error()->code == "persistence.write_failed",
        "A second default profile must be refused as a failed write");
    require(profileIds(profiles) == std::vector<std::string> {"profile-1", "profile-d1"},
        "A refused default must not be stored");

    // Every refusal rolled back; the connection must be outside a transaction.
    const ExecutionProfile ok = makeProfile("profile-3", at(5000),
        {makeAssignment("documentation", "provider-1", "model-2", 3000)});
    require(profiles.insert(ok).success(), "The connection must be usable after refused inserts");
    requireStoredProfile(profiles, ok, "A profile inserted after refusals must round-trip");
}

void testProfileUpdateReplacesRosterAtomically()
{
    Workspace workspace;
    const auto database = workspace.open();
    addCatalog(database);
    SqliteExecutionProfileRepository profiles(database);
    const ExecutionProfile original = [] {
        ExecutionProfile value = makeProfile("profile-1", at(1000), {
            makeAssignment("architect", "provider-1", "model-1", 60'000),
            makeAssignment("implementation", "provider-1", "model-2", 45'000),
            makeAssignment("documentation", "provider-2", "model-3", 30'000)});
        value.providerBudget = fullBudget();
        return value;
    }();
    ExecutionProfile other = makeProfile("profile-2", at(2000),
        {makeAssignment("architect", "provider-1", "model-1", 11'000)});
    require(profiles.insert(original).success() && profiles.insert(other).success(), "Could not insert profiles");

    // A successful update replaces the profile and its whole roster.
    ExecutionProfile replaced = original;
    replaced.name = "Renamed \xE6\xB8\xAC";
    replaced.workflowId = "other-workflow";
    replaced.enabled = false;
    replaced.providerBudget = {};
    replaced.createdAt = at(1500);
    replaced.updatedAt = at(9000);
    replaced.assignments = {makeAssignment("implementation", "provider-2", "model-3", 12'345)};
    const auto updated = profiles.update(replaced);
    require(updated.success(), "Could not update a profile");
    requireSameProfile(updated.value(), replaced, "update must return the profile as written");
    requireStoredProfile(profiles, replaced, "An update must replace the fields and the whole roster");
    requireStoredProfile(profiles, other, "An update must not touch another profile or its roster");

    replaced.assignments.clear();
    require(profiles.update(replaced).success(), "Could not update a profile to an empty roster");
    requireStoredProfile(profiles, replaced, "An empty roster must clear the stored assignments");

    replaced.assignments = {
        makeAssignment("documentation", "provider-1", "model-2", 5000),
        makeAssignment("architect", "provider-1", "model-1", 7000)};
    require(profiles.update(replaced).success(), "Could not grow the roster again");
    requireStoredProfile(profiles, replaced, "A regrown roster must be stored in full");
    requireStoredProfile(profiles, other, "Rosters of other profiles must stay intact");

    const ExecutionProfile before = replaced;
    const auto refuse = [&](ExecutionProfile attempt, const char* code, const char* message) {
        attempt.name = "Must not persist";
        attempt.updatedAt = at(99'999);
        const auto result = profiles.update(std::move(attempt));
        require(!result.success() && result.error()->code == code, message);
        requireStoredProfile(profiles, before, "A refused update changed the profile or its roster");
        requireStoredProfile(profiles, other, "A refused update changed another profile");
    };

    // The old roster is deleted before the new one is written, so each of these fails after the delete.
    ExecutionProfile attempt = before;
    attempt.assignments = {
        makeAssignment("architect", "provider-1", "model-1", 1000),
        makeAssignment("architect", "provider-1", "model-2", 2000)};
    refuse(attempt, "execution_profile.duplicate_stage_type",
        "A repeated stage type in the new roster must report execution_profile.duplicate_stage_type");
    attempt.assignments = {
        makeAssignment("implementation", "provider-1", "model-1", 1000),
        makeAssignment("documentation", "provider-1", "model-missing", 2000)};
    refuse(attempt, "persistence.write_failed", "An unknown model in the new roster must be refused");
    attempt.assignments = {
        makeAssignment("architect", "provider-1", "model-1", 1000),
        makeAssignment("implementation", "provider-1", "model-2", 0)};
    refuse(attempt, "persistence.write_failed", "A zero timeout in the new roster must be refused");

    // A clash on the single default fails on the profile row itself, before the roster is touched.
    require(profiles.setDefaultProfile("profile-2").success(), "Could not set a default profile");
    other.isDefault = true;
    attempt = before;
    attempt.isDefault = true;
    refuse(attempt, "persistence.write_failed", "A second default through update must be refused");

    const auto missing = profiles.update(makeProfile("profile-missing", at(1000),
        {makeAssignment("architect", "provider-1", "model-1", 1000)}));
    require(!missing.success() && missing.error()->code == "execution_profile.not_found",
        "Updating a missing profile must report execution_profile.not_found");
    const auto absent = profiles.findById("profile-missing");
    require(absent.success() && !absent.value().has_value(), "A refused update must not create a profile");
    require(profileIds(profiles) == std::vector<std::string> {"profile-1", "profile-2"},
        "A refused update must not change the set of profiles");

    // Every refusal rolled back; the connection must be outside a transaction.
    ExecutionProfile finalState = before;
    finalState.name = "Final";
    finalState.assignments = {makeAssignment("implementation", "provider-1", "model-2", 6000)};
    require(profiles.update(finalState).success(), "The connection must be usable after refused updates");
    requireStoredProfile(profiles, finalState, "An update after refusals must apply in full");
    requireStoredProfile(profiles, other, "The other profile must still be intact");
}

void testSetDefaultProfileIsAtomic()
{
    Workspace workspace;
    const auto database = workspace.open();
    addCatalog(database);
    SqliteExecutionProfileRepository profiles(database);
    const ExecutionProfile a = makeProfile("profile-a", at(1000),
        {makeAssignment("architect", "provider-1", "model-1", 60'000)});
    const ExecutionProfile b = makeProfile("profile-b", at(2000),
        {makeAssignment("implementation", "provider-1", "model-2", 45'000)});
    const ExecutionProfile c = makeProfile("profile-c", at(3000));
    require(profiles.insert(a).success() && profiles.insert(b).success() && profiles.insert(c).success(),
        "Could not insert profiles");

    using Ids = std::vector<std::string>;
    const auto defaults = [&] {
        Ids result;
        const auto listed = profiles.listAll();
        require(listed.success(), "Could not list profiles");
        for (const auto& value : listed.value()) if (value.isDefault) result.push_back(value.id);
        return result;
    };
    require(defaults().empty(), "No profile may be the default initially");

    require(profiles.setDefaultProfile("profile-a").success(), "Could not set the first default");
    require(defaults() == Ids {"profile-a"}, "The first default did not apply");
    require(profiles.setDefaultProfile("profile-b").success(), "Could not move the default");
    require(defaults() == Ids {"profile-b"}, "Moving the default must leave exactly one");

    // The refusal first clears the old default inside the transaction; it must be restored.
    const auto missing = profiles.setDefaultProfile("profile-missing");
    require(!missing.success() && missing.error()->code == "execution_profile.not_found",
        "A missing profile must report execution_profile.not_found");
    require(defaults() == Ids {"profile-b"}, "A refused default change lost the previous default");

    require(profiles.setDefaultProfile("profile-b").success(), "Setting the current default again must succeed");
    require(defaults() == Ids {"profile-b"}, "Re-setting the default must keep it");

    // Only the flag changes: the rest of the profile and its roster stay as written.
    ExecutionProfile expectedA = a;
    ExecutionProfile expectedB = b;
    expectedB.isDefault = true;
    requireStoredProfile(profiles, expectedA, "Moving the default altered the former default's data");
    requireStoredProfile(profiles, expectedB, "Setting the default altered the profile's data");

    // A rolled-back transaction must not leave the connection inside one.
    require(profiles.insert(makeProfile("profile-d", at(4000))).success(),
        "The connection must be usable after a refused default change");
    require(profiles.setDefaultProfile("profile-c").success(), "A later default change must work");
    require(defaults() == Ids {"profile-c"}, "The later default change did not apply");
}

void testCorruptProfileRowsFailClosed()
{
    Workspace workspace;
    {
        const auto database = workspace.open();
        addCatalog(database);
        SqliteExecutionProfileRepository profiles(database);
        for (const char* id : {"profile-good", "profile-bad-json", "profile-bad-type", "profile-bad-limit",
                 "profile-bad-timeout", "profile-negative-timeout"}) {
            require(profiles.insert(makeProfile(id, at(1000),
                        {makeAssignment("architect", "provider-1", "model-1", 60'000)})).success(),
                "Could not insert profiles");
        }
    }

    workspace.sql("UPDATE execution_profiles SET provider_budget_json='{broken' WHERE id='profile-bad-json';");
    workspace.sql("UPDATE execution_profiles SET provider_budget_json='{\"maxTurns\":\"many\"}' "
                  "WHERE id='profile-bad-type';");
    workspace.sql("UPDATE execution_profiles SET provider_budget_json='{\"maxTurns\":0}' "
                  "WHERE id='profile-bad-limit';");
    // The schema refuses these values, so the checks are switched off on the corrupting connection only.
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE execution_profile_assignments SET timeout_ms=0 "
                  "WHERE profile_id='profile-bad-timeout';");
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE execution_profile_assignments SET timeout_ms=-1 "
                  "WHERE profile_id='profile-negative-timeout';");

    const auto database = workspace.open();
    SqliteExecutionProfileRepository profiles(database);
    const auto good = profiles.findById("profile-good");
    require(good.success() && good.value().has_value() && good.value()->assignments.size() == 1,
        "A valid profile must stay readable");

    for (const char* id : {"profile-bad-json", "profile-bad-type"}) {
        const auto bad = profiles.findById(id);
        require(!bad.success() && bad.error()->code == "persistence.invalid_data",
            "A profile with unreadable budget JSON must fail closed on findById");
    }
    const auto overLimit = profiles.findById("profile-bad-limit");
    require(!overLimit.success() && overLimit.error()->code == "provider_budget.invalid_policy",
        "A stored budget that the Backend would refuse must fail closed on findById");
    for (const char* id : {"profile-bad-timeout", "profile-negative-timeout"}) {
        const auto bad = profiles.findById(id);
        require(!bad.success() && bad.error()->code == "persistence.invalid_data",
            "A non-positive stored timeout must fail closed on findById");
    }
    const auto everything = profiles.listAll();
    require(!everything.success(), "A listing that contains an invalid profile must fail rather than skip it");
}
} // namespace

int main()
{
    try {
        testProviderRoundTripOrderingAndLifetime();
        testProviderDuplicateUpdateAndMissing();
        testProviderConstraintFailuresDoNotPoisonConnection();
        testConcurrentProviderWritersShareOneConnection();
        testModelRoundTripPricingOrderingAndFilter();
        testModelWriteErrorsAndStableDefault();
        testSetDefaultModelIsAtomic();
        testModelRemoveNotFoundAndInUse();
        testCorruptModelRowsFailClosed();
        testProfileRoundTripOrderingAndReopen();
        testProfileWriteErrorsAreAtomic();
        testProfileUpdateReplacesRosterAtomically();
        testSetDefaultProfileIsAtomic();
        testCorruptProfileRowsFailClosed();
        std::cout << "SQLite Catalog contracts: 14 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Catalog contracts failed: " << error.what() << '\n';
        return 1;
    }
}
