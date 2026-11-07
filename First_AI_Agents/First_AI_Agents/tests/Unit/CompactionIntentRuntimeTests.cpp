// B3-II-A: the composition of the compaction intent in BackendRuntime.
//  1. The REAL runtime on a REAL temporary database builds the service with the SQLite intent port (it could not be constructed
//     without one). What is reachable offline through the real runtime is checked: a call that ends before a client exists leaves the
//     intent table untouched and writes the same Failed terminal record as before (the production provider factory cannot be replaced,
//     so an admitted request through the real runtime would be a real provider call and is NOT exercised here).
//  2. The fail-closed intent port the runtime composes for an unavailable database: it never admits, so a service built over it sends
//     nothing and writes no record, whatever else is available.
// No network, no provider call, no credential, no live database. The checks use throwing require().
#include "detail/UnavailableProjectMemoryCompactionIntentRepository.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/backend/BackendRuntime.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using ai::backend::BackendRuntime;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryRepository;
namespace common = ai::common;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Workspace {
    fs::path root;
    fs::path projectRoot;
    fs::path databasePath;
    Workspace()
    {
        root = fs::temp_directory_path()
            / ("aio-b3iia-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        projectRoot = root / "project";
        databasePath = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(projectRoot);
    }
    ~Workspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

long long rawCount(const fs::path& file, const std::string& sql)
{
    const auto text = file.generic_u8string();
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(std::string(reinterpret_cast<const char*>(text.data()), text.size()).c_str(), &handle,
                SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed: " + sql);
    long long value = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return value;
}

ProjectMemorySegment segment(const std::string& id, const ProjectId& project)
{
    ProjectMemorySegment value;
    value.id = id;
    value.projectId = project;
    value.runId = "";
    value.summary = "Summary " + id;
    value.sourceTokenCount = 100;
    value.summaryTokenCount = 10;
    value.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
    return value;
}

void testTheRealRuntimeComposesTheServiceAndAnEarlyEndLeavesTheIntentAlone()
{
    Workspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "durable storage");
    const auto project = runtime.projectService().createProject({"Project", "intent", workspace.projectRoot});
    require(project.success(), "project");
    // A provider whose client cannot be built offline: no credential is configured, so no client and no request exist.
    const auto provider = runtime.providerCatalogService().createProvider(
        {"Intent Provider", "anthropic", "https://api.anthropic.invalid", true, std::nullopt});
    require(provider.success(), "provider");
    const auto model = runtime.providerCatalogService().createModel({provider.value(), "claude-test", "Claude Test", 200000, 8192, 1000000, 5000000, true});
    require(model.success(), "model");
    {   // two sources, written through a SECOND connection with the real repository (no production seam)
        auto second = SqliteDatabase::open(workspace.databasePath);
        require(second.success(), "second connection");
        SqliteProjectMemoryRepository memory(second.value());
        require(memory.insertMemorySegment(segment("m1", project.value().id)).success(), "segment 1");
        require(memory.insertMemorySegment(segment("m2", project.value().id)).success(), "segment 2");
    }

    const auto outcome = runtime.projectMemoryCompactionService().compactObserved(project.value().id, provider.value(), model.value(), {});
    require(outcome.result.success() && outcome.result.value().status == ProjectMemoryCompactionStatus::Failed,
        "no client: the same Failed terminal record as before the intent existed");
    require(!outcome.responseObserved && outcome.accounting == CompactionAccounting::NotApplicable, "nothing was received, nothing to account for");
    require(outcome.intent.admission == CompactionIntentAdmission::NotApplicable && !outcome.intent.writeState.has_value()
                && outcome.intent.observation == CompactionIntentObservation::NotObserved,
        "admission was never attempted");
    require(rawCount(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_intents;") == 0
                && rawCount(workspace.databasePath, "SELECT count(*) FROM project_memory_compaction_records;") == 1
                && rawCount(workspace.databasePath, "SELECT count(*) FROM project_memory_segments;") == 2,
        "the schema with the intent table is there (the runtime opened it); no intent row, one Failed record, the sources untouched");
}

// ---- 2. the fail-closed port ---------------------------------------------------------------------------------------------------------

class Memory final : public IProjectMemoryRepository {
public:
    int writes {0};
    common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment value) override
    {
        ++writes;
        return common::Result<ProjectMemorySegment>::ok(std::move(value));
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId& project) const override
    {
        return common::Result<std::vector<ProjectMemorySegment>>::ok({segment("a", project), segment("b", project)});
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(const ProjectMemorySegmentId&) const override
    {
        return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }
    common::Result<void> removeMemorySegments(const ProjectId&) override
    {
        ++writes;
        return common::Result<void>::ok();
    }
};

class Records final : public IProjectMemoryCompactionRepository {
public:
    int writes {0};
    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord&) override { ++writes; return common::Result<void>::ok(); }
    common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        ++writes;
        return common::Result<void>::ok();
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId&) const override
    {
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok({});
    }
    CompactionPersistOutcome commitSuccessfulCompactionObserved(const ProjectMemoryCompactionRecord&, ProjectMemorySegment) override
    {
        ++writes;
        return {};
    }
    CompactionPersistOutcome insertCompactionRecordObserved(const ProjectMemoryCompactionRecord&) override
    {
        ++writes;
        return {};
    }
};

class Client final : public IProviderClient {
public:
    int calls {0};
    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        ++calls;
        return common::Result<ProviderChatResponse>::failure({"test.must_not_send", "the unavailable intent port must stop the call"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++calls;
        return {common::Result<ProviderChatResponse>::failure({"test.must_not_send", "the unavailable intent port must stop the call"}), std::nullopt};
    }
};

class Factory final : public IProviderClientFactory {
public:
    explicit Factory(IProviderClient& client) : client_(client) {}
    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        return common::Result<std::shared_ptr<IProviderClient>>::ok(std::shared_ptr<IProviderClient>(&client_, [](IProviderClient*) {}));
    }

private:
    IProviderClient& client_;
};

void testTheUnavailablePortNeverAdmitsAndTheServiceSendsNothing()
{
    ai::backend::detail::UnavailableProjectMemoryCompactionIntentRepository intents("database is unavailable (test)");
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    Memory memory;
    Records records;
    Client client;
    Factory factory {client};
    Project project;
    project.id = "p1";
    project.name = "P";
    project.description = "d";
    project.rootPath = "C:/fixture";
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    require(projects.insert(project).success(), "setup: project");
    Provider provider;
    provider.id = "prov1";
    provider.name = "Fake";
    provider.kind = "openai_compatible";
    provider.enabled = true;
    provider.createdAt = project.createdAt;
    provider.updatedAt = project.createdAt;
    require(providers.insert(provider).success(), "setup: provider");
    Model model;
    model.id = "model1";
    model.providerId = "prov1";
    model.apiModelId = "fake";
    model.displayName = "Fake";
    model.contextWindowTokens = 1000;
    model.maxOutputTokens = 100;
    model.enabled = true;
    model.createdAt = project.createdAt;
    model.updatedAt = project.createdAt;
    require(models.insert(model).success(), "setup: model");

    ProjectMemoryCompactionService service(memory, records, intents, projects, providers, models, factory);
    const auto outcome = service.compactObserved("p1", "prov1", "model1", {});
    require(client.calls == 0, "the unavailable intent port stops the request");
    require(!outcome.result.success() && outcome.result.error()->code == kIntentNotRecordedCode, "the typed refusal");
    require(records.writes == 0 && memory.writes == 0, "no record, no fallback, no memory write");
    require(outcome.intent.admission == CompactionIntentAdmission::NotAdmitted && outcome.intent.writeState == CompactionPersistState::NotCommitted,
        "NotAdmitted; nothing was ever started");
    require(outcome.intent.observation == CompactionIntentObservation::Unreadable,
        "and the proof of an unavailable storage proves nothing: Unreadable, never 'absent'");
    bool keptTheStorageReason = false;
    for (const auto& entry : outcome.diagnostics) {
        if (entry.stage == CompactionDiagnosticStage::IntentWrite && entry.error.code == "storage.unavailable"
            && entry.error.message == "database is unavailable (test)") {
            keptTheStorageReason = true;
        }
    }
    require(keptTheStorageReason, "the storage's own reason is kept as the IntentWrite cause");

    // The port is closed in every other direction too.
    CompactionIntentIdentity identity;
    require(!intents.findIntent("x").success() && !intents.listIntentsByState(CompactionIntentState::Open).success(), "reads fail");
    require(intents.markUnresolvedObserved(identity, std::chrono::system_clock::now()).mutation == CompactionIntentMutation::Unconfirmed
                && intents.closeIntentObserved(identity).mutation == CompactionIntentMutation::Unconfirmed,
        "mutations are never Applied");
}

} // namespace

int runCompactionIntentRuntimeTests()
{
    try {
        testTheRealRuntimeComposesTheServiceAndAnEarlyEndLeavesTheIntentAlone();
        testTheUnavailablePortNeverAdmitsAndTheServiceSendsNothing();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentRuntimeTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentRuntimeTests: OK\n";
    return 0;
}
