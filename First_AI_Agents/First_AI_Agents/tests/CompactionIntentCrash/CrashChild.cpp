// The CHILD of the crash tests. It builds the real compaction service over real SQLite with a fake provider, holds the real database
// owner lock until it dies, reaches ONE of five test-only points and ends its own process with TerminateProcess (no destructors).
//
//   P0 intent_committed_before_dispatch   a decorator of the intent port, right after the REAL Committed admission, before any client call
//   P1 inside_fake_send                   inside the fake provider, after its durable call counter reached 1
//   P2 response_ready_before_record_write the fake returned a paid response; a decorator of the record port, before the final write
//   P3 before_final_commit                update_hook of the FINAL record insert arms a flag; commit_hook of that transaction dies
//   P4 after_final_commit                 the same gate; wal_hook (after the commit) dies
//
// The hooks are installed on the child's connection only after the fixture was seeded, and the gate is armed only inside the fake send,
// i.e. after the admission commit: the intent's own commit and the seed never trigger them. Inside a callback no SQL runs - only the
// prepared marker is written through the OS and the process ends. Test-only; nothing of this exists in production code.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CrashCommon.hpp"

#include "SqliteFaultForTesting.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/infrastructure/DatabaseOwnerLock.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <windows.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

namespace crash {

namespace {

using namespace ai::application;
namespace common = ai::common;
namespace detail = ai::infrastructure::sqlite_detail;
using ai::infrastructure::DatabaseOwnerLock;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionIntentRepository;
using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
using ai::infrastructure::SqliteProjectMemoryRepository;

std::string g_phase;
std::atomic<int> g_clientCalls {0};
std::atomic<int> g_responseConsumed {0};
std::atomic<int> g_hookFlag {0};
std::atomic<bool> g_gateArmed {false};
HANDLE g_counter = INVALID_HANDLE_VALUE;

[[noreturn]] void die() { dieAtPhase(g_phase.c_str(), g_clientCalls.load(), g_responseConsumed.load(), g_hookFlag.load()); }

struct SetupFailure {
    std::string what;
};

void need(bool condition, const char* what)
{
    if (!condition) throw SetupFailure {what};
}

std::string narrow(const wchar_t* text)
{
    std::string out;
    for (; *text != L'\0'; ++text) out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return out;
}

// ---- the decorators (test-only) ----------------------------------------------------------------------------------------------------
class DyingIntents final : public IProjectMemoryCompactionIntentRepository {
public:
    explicit DyingIntents(IProjectMemoryCompactionIntentRepository& inner) : inner_(inner) {}

    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent& intent) override
    {
        const CompactionIntentWriteOutcome outcome = inner_.recordIntentObserved(intent);
        // After the REAL Committed admission, before the service can reach the provider.
        if (g_phase == kP0 && mayDispatch(outcome)) die();
        return outcome;
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId& id) const override
    {
        return inner_.findIntent(id);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(
        const ProjectId& project, std::optional<CompactionIntentState> state) const override
    {
        return inner_.listIntents(project, state);
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState state) const override
    {
        return inner_.listIntentsByState(state);
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(const CompactionIntentIdentity& e, std::chrono::system_clock::time_point t) override
    {
        return inner_.markUnresolvedObserved(e, t);
    }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity& e) override { return inner_.closeIntentObserved(e); }
    [[nodiscard]] CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity& e) const override
    {
        return inner_.observeOperation(e);
    }

private:
    IProjectMemoryCompactionIntentRepository& inner_;
};

class DyingRecords final : public IProjectMemoryCompactionRepository {
public:
    explicit DyingRecords(IProjectMemoryCompactionRepository& inner) : inner_(inner) {}

    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord& record) override { return inner_.insertCompactionRecord(record); }
    common::Result<void> commitSuccessfulCompaction(const ProjectMemoryCompactionRecord& record, ProjectMemorySegment segment) override
    {
        return inner_.commitSuccessfulCompaction(record, std::move(segment));
    }
    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(const ProjectId& project) const override
    {
        return inner_.listCompactionRecords(project);
    }
    CompactionPersistOutcome commitSuccessfulCompactionObserved(const ProjectMemoryCompactionRecord& record, ProjectMemorySegment segment) override
    {
        beforeFinalWrite();
        return inner_.commitSuccessfulCompactionObserved(record, std::move(segment));
    }
    CompactionPersistOutcome insertCompactionRecordObserved(const ProjectMemoryCompactionRecord& record) override
    {
        beforeFinalWrite();
        return inner_.insertCompactionRecordObserved(record);
    }
    [[nodiscard]] common::Result<std::optional<ProjectMemoryCompactionRecord>> findCompactionRecord(const ProjectMemoryCompactionId& id) const override
    {
        return inner_.findCompactionRecord(id);
    }

private:
    static void beforeFinalWrite()
    {
        if (g_phase == kP2) {
            g_responseConsumed = 1;   // the service holds the paid response and is about to store it
            die();
        }
    }
    IProjectMemoryCompactionRepository& inner_;
};

// The fake provider: its call counter is DURABLE (one byte appended per call) so the parent can see it after the process is gone.
class Client final : public IProviderClient {
public:
    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++g_clientCalls;
        DWORD written = 0;
        const char byte = 'x';
        if (g_counter == INVALID_HANDLE_VALUE || !WriteFile(g_counter, &byte, 1, &written, nullptr) || written != 1 || !FlushFileBuffers(g_counter)) {
            TerminateProcess(GetCurrentProcess(), kSetupFailed);
        }
        if (g_phase == kP1) die();
        if (g_phase == kP3 || g_phase == kP4) g_gateArmed = true;   // from here on, a record insert is THE final write
        ProviderChatResponse response;
        response.content = "CONSOLIDATED SUMMARY";
        response.usage.inputTokens = 40;
        response.usage.outputTokens = 15;
        return {common::Result<ProviderChatResponse>::ok(std::move(response)), std::nullopt};
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

// ---- the SQLite hooks: no SQL, no allocation of consequence, only the prepared marker and the end of the process ----------------------
void onUpdate(void*, int operation, const char*, const char* table, sqlite3_int64)
{
    if (g_gateArmed.load() && operation == SQLITE_INSERT && table != nullptr && std::strcmp(table, "project_memory_compaction_records") == 0) {
        g_hookFlag = 1;   // the FINAL record insert has begun
    }
}

int onCommit(void*)
{
    if (g_hookFlag.load() != 0) die();   // P3: the final transaction is about to commit and never will
    return 0;
}

int onWal(void*, sqlite3*, const char*, int)
{
    if (g_hookFlag.load() != 0) die();   // P4: the final transaction IS committed (it is in the WAL)
    return SQLITE_OK;
}

std::string exec(const detail::ConnectionHandles& handles, const std::string& sql)
{
    std::lock_guard lock(*handles.mutex);
    char* message = nullptr;
    const int rc = sqlite3_exec(handles.handle, sql.c_str(), nullptr, nullptr, &message);
    std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
    sqlite3_free(message);
    return text;
}

std::string pragma(const detail::ConnectionHandles& handles, const char* sql)
{
    std::lock_guard lock(*handles.mutex);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(handles.handle, sql, -1, &statement, nullptr) != SQLITE_OK) return "<error>";
    std::string value = "<none>";
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char* raw = sqlite3_column_text(statement, 0);
        value = raw != nullptr ? reinterpret_cast<const char*>(raw) : "<null>";
    }
    sqlite3_finalize(statement);
    return value;
}

} // namespace

int runChild(int argc, wchar_t** argv)
{
    if (argc != 5) return kBadArguments;
    g_phase = narrow(argv[0]);
    const std::string nonce = narrow(argv[1]);
    if (g_phase != kP0 && g_phase != kP1 && g_phase != kP2 && g_phase != kP3 && g_phase != kP4) return kBadArguments;
    const std::wstring databasePath = argv[2];
    const std::wstring markerPath = argv[3];
    const std::wstring counterPath = argv[4];

    try {
        need(prepareMarker(markerPath, nonce), "marker file");
        g_counter = CreateFileW(counterPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        need(g_counter != INVALID_HANDLE_VALUE, "counter file");

        // The REAL owner lock, before SQLite, held until the process dies (the lock object is never destroyed on purpose).
        auto ownerLock = DatabaseOwnerLock::acquire(std::filesystem::path(databasePath));
        need(ownerLock.success(), "owner lock");
        DatabaseOwnerLock* const heldUntilDeath = ownerLock.value().release();
        (void)heldUntilDeath;

        auto opened = SqliteDatabase::open(std::filesystem::path(databasePath));
        need(opened.success(), "database open");
        std::shared_ptr<SqliteDatabase> database = std::move(opened.value());
        const detail::ConnectionHandles handles = detail::ConnectionAccess::of(*database);
        need(pragma(handles, "PRAGMA journal_mode;") == "wal", "journal_mode is wal");
        need(pragma(handles, "PRAGMA synchronous;") == "1", "synchronous is NORMAL");

        need(exec(handles, "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                           "VALUES('p1','One','d','C:/fixture/1','active',1,1);")
                .empty(),
            "seed project");
        SqliteProjectMemoryRepository memory(database);
        for (const char* id : {"s1", "s2", "s3"}) {
            ProjectMemorySegment segment;
            segment.id = id;
            segment.projectId = "p1";
            segment.runId = std::string("run-") + id;
            segment.summary = std::string("Summary of ") + id;
            segment.sourceTokenCount = 11;
            segment.summaryTokenCount = 7;
            segment.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
            need(memory.insertMemorySegment(segment).success(), "seed segment");
        }

        InMemoryProjectRepository projects;
        InMemoryProviderRepository providers;
        InMemoryModelRepository models;
        Project project;
        project.id = "p1";
        project.name = "One";
        project.description = "d";
        project.rootPath = "C:/fixture/1";
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        need(projects.insert(project).success(), "project");
        Provider provider;
        provider.id = "prov1";
        provider.name = "Fake";
        provider.kind = "openai_compatible";
        provider.enabled = true;
        provider.createdAt = project.createdAt;
        provider.updatedAt = project.createdAt;
        need(providers.insert(provider).success(), "provider");
        Model model;
        model.id = "model1";
        model.providerId = "prov1";
        model.apiModelId = "fake-model-1";
        model.displayName = "Fake Model";
        model.contextWindowTokens = 128000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 5'000'000;
        model.outputPriceMicrosPerMillionTokens = 15'000'000;
        model.enabled = true;
        model.createdAt = project.createdAt;
        model.updatedAt = project.createdAt;
        need(models.insert(model).success(), "model");

        SqliteProjectMemoryCompactionRepository realRecords(database);
        SqliteProjectMemoryCompactionIntentRepository realIntents(database);
        DyingRecords records(realRecords);
        DyingIntents intents(realIntents);
        Client client;
        Factory factory(client);
        ProjectMemoryCompactionService service(memory, records, intents, projects, providers, models, factory);

        // Hooks last: the seed and (for P3/P4) the admission commit happen with the gate unarmed and, for the hooks, before they exist.
        if (g_phase == kP3 || g_phase == kP4) {
            std::lock_guard lock(*handles.mutex);
            sqlite3_update_hook(handles.handle, &onUpdate, nullptr);
            if (g_phase == kP3) sqlite3_commit_hook(handles.handle, &onCommit, nullptr);
            if (g_phase == kP4) sqlite3_wal_hook(handles.handle, &onWal, nullptr);   // replaces the auto-checkpoint: the child dies anyway
        }

        (void)service.compactObserved("p1", "prov1", "model1", {});
        return static_cast<int>(kPhaseNotReached);   // no hook ended the process
    } catch (const SetupFailure&) {
        return static_cast<int>(kSetupFailed);
    } catch (...) {
        return static_cast<int>(kSetupFailed);
    }
}

} // namespace crash
