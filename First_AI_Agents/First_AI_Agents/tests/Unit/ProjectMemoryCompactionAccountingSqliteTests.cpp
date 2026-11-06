// C2-B/S2b-2: the REAL compaction service on a REAL SQLite database with a fake provider (no HTTP, no persistence-private fault
// seam). Failures are produced the way a real database produces them: a concurrent change of the memory between the provider call
// and the commit (an optimistic-concurrency conflict), and triggers created in the main schema of the temporary database through a
// second connection (RAISE(ABORT) on the INSERT of a record). The checks use throwing require().
#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
using ai::infrastructure::SqliteProjectMemoryRepository;
namespace policy = ai::application::compaction_accounting;
namespace common = ai::common;
using A = CompactionAccounting;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string rawExec(const fs::path& file, const char* sql)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    char* message = nullptr;
    const int rc = sqlite3_exec(handle, sql, nullptr, nullptr, &message);
    std::string text = rc == SQLITE_OK ? std::string {} : (message != nullptr ? message : "unknown SQLite error");
    sqlite3_free(message);
    sqlite3_close_v2(handle);
    return text;
}

long long rawCount(const fs::path& file, const std::string& sql)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "raw open failed");
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed");
    long long value = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return value;
}

// A provider that can change the world while the call is in flight (between the service's read and its commit).
class HookedClient final : public IProviderClient {
public:
    int observedCalls {0};
    std::string content {"CONSOLIDATED SUMMARY"};
    std::function<void()> whileInFlight;

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }
    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++observedCalls;
        if (whileInFlight) whileInFlight();
        ProviderChatResponse response;
        response.content = content;
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

struct Fixture {
    fs::path root;
    fs::path file;
    std::shared_ptr<SqliteDatabase> database;
    std::unique_ptr<SqliteProjectMemoryRepository> memory;
    std::unique_ptr<SqliteProjectMemoryCompactionRepository> compaction;
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    HookedClient client;
    Factory factory {client};
    std::unique_ptr<ProjectMemoryCompactionService> service;

    Fixture()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path() / ("aio-s2b2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                            + "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
        file = root / "state.sqlite3";
        open();
        require(rawExec(file,
                    "INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                    "VALUES('cp1','One','d','C:/fixture/1','active',1,1);")
                    .empty(),
            "seed the project");
        Project project;
        project.id = "cp1";
        project.name = "One";
        project.description = "d";
        project.rootPath = "C:/fixture/1";
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
        model.apiModelId = "fake-model-1";
        model.displayName = "Fake Model";
        model.contextWindowTokens = 128000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 5'000'000;
        model.outputPriceMicrosPerMillionTokens = 15'000'000;
        model.enabled = true;
        model.createdAt = project.createdAt;
        model.updatedAt = project.createdAt;
        require(models.insert(model).success(), "setup: model");
        for (const char* id : {"s1", "s2", "keep"}) {
            require(memory->insertMemorySegment(segment(id)).success(), "seed a segment");
        }
    }
    ~Fixture()
    {
        service.reset();
        compaction.reset();
        memory.reset();
        database.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    static ProjectMemorySegment segment(const std::string& id)
    {
        ProjectMemorySegment value;
        value.id = id;
        value.projectId = "cp1";
        value.runId = "run-" + id;
        value.summary = "Summary of " + id;
        value.sourceTokenCount = 11;
        value.summaryTokenCount = 7;
        value.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000 + static_cast<int>(id.size())));
        return value;
    }

    void open()
    {
        service.reset();
        compaction.reset();
        memory.reset();
        database.reset();
        auto opened = SqliteDatabase::open(file);
        require(opened.success(), "open the temporary database");
        database = std::move(opened.value());
        memory = std::make_unique<SqliteProjectMemoryRepository>(database);
        compaction = std::make_unique<SqliteProjectMemoryCompactionRepository>(database);
        service = std::make_unique<ProjectMemoryCompactionService>(*memory, *compaction, projects, providers, models, factory);
    }

    ProjectMemoryCompactionOutcome run() { return service->compactObserved("cp1", "prov1", "model1", {}); }
    long long segments(const std::string& where = "1=1") const
    {
        return rawCount(file, "SELECT count(*) FROM project_memory_segments WHERE " + where + ";");
    }
    long long records(const std::string& where = "1=1") const
    {
        return rawCount(file, "SELECT count(*) FROM project_memory_compaction_records WHERE " + where + ";");
    }
};

void requireReceivedAmount(const ProjectMemoryCompactionOutcome& outcome, const std::string& what)
{
    require(outcome.responseObserved && outcome.observedRecord.has_value() && outcome.observedRecord->inputTokens == 40
                && outcome.observedRecord->outputTokens == 15 && outcome.observedRecord->costMicros > 0
                && outcome.observedRecord->costPriceKnown == std::optional<bool> {true},
        what + ": the received usage, amount and flag are visible in the outcome");
}

// A normal success: one Completed row, no fallback; the control for everything below.
void testASuccessfulCompactionIsOneDurableCompletedRow()
{
    Fixture f;
    const auto outcome = f.run();
    require(outcome.result.success() && outcome.accounting == A::Saved && outcome.connection == CompactionConnectionState::Healthy,
        "success: Saved on a healthy connection");
    require(f.client.observedCalls == 1 && f.records() == 1 && f.records("status='completed'") == 1 && f.records("status='failed'") == 0,
        "one provider call and exactly ONE durable Completed row, no fallback");
    require(f.segments("id IN ('s1','s2')") == 0 && f.segments("id='keep'") == 0 && f.segments() == 1,
        "the sources were replaced by the result segment");
    f.open();   // reopen
    const auto found = f.compaction->findCompactionRecord(outcome.result.value().id);
    require(found.success() && found.value().has_value() && policy::identityDifference(outcome.result.value(), *found.value()) == nullptr,
        "after a reopen the stored row is the returned record, field for field");
}

// An optimistic-concurrency conflict produced by a real concurrent change of the memory while the provider call is in flight.
void testAConcurrentMemoryChangeLeavesAFailedAccountingRowAndTheNewStateAlone()
{
    Fixture f;
    f.client.whileInFlight = [&] {
        // somebody clears the project memory and writes a newer segment while the provider is thinking
        require(f.memory->removeMemorySegments("cp1").success(), "the concurrent clear");
        require(f.memory->insertMemorySegment(Fixture::segment("newer")).success(), "the concurrent newer segment");
    };
    const auto outcome = f.run();
    require(!outcome.result.success() && outcome.result.error()->code == "project_memory.compaction_conflict",
        "the PRIMARY failure is the optimistic conflict");
    require(outcome.accounting == A::Saved && outcome.responseObserved, "the received usage is Saved by the Failed accounting row");
    require(outcome.connection == CompactionConnectionState::Healthy && !outcome.primaryNative.has_value() && !outcome.fallbackNative.has_value(),
        "healthy; the conflict is not a SQLite failure, so no native codes are invented");
    requireReceivedAmount(outcome, "conflict");
    require(f.client.observedCalls == 1, "no second provider call");
    require(f.segments() == 1 && f.segments("id='newer'") == 1,
        "the newer state is NOT overwritten and no result segment appeared");
    require(f.records() == 1 && f.records("status='failed'") == 1 && f.records("status='completed'") == 0, "exactly one Failed accounting row");

    f.open();   // reopen: it is durable
    const auto stored = f.compaction->findCompactionRecord(outcome.observedRecord->id);
    require(stored.success() && stored.value().has_value() && policy::identityDifference(*outcome.observedRecord, *stored.value()) == nullptr,
        "after a reopen the Failed row is readable and identical to the fallback the service reported");
    require(stored.value()->status == ProjectMemoryCompactionStatus::Failed && !stored.value()->resultSegmentId.has_value()
                && stored.value()->errorCode == "project_memory.compaction_conflict" && stored.value()->costMicros == outcome.observedRecord->costMicros,
        "...Failed, no result segment, the primary category as its code, the received amount");
    const auto listed = f.compaction->listCompactionRecords("cp1");
    require(listed.success() && listed.value().size() == 1 && compactionPriceState(listed.value()[0]) == ExecutionPriceState::Known,
        "the history shows the known amount");
}

// Both writes fail for real: the INSERT of a 'completed' record and the INSERT of a 'failed' one are aborted by triggers.
void testWhenBothWritesFailTheKnownAmountIsStillReportedAndNothingIsStored()
{
    Fixture f;
    require(rawExec(f.file,
                "CREATE TRIGGER s2b2_block_completed BEFORE INSERT ON project_memory_compaction_records WHEN NEW.status = 'completed' "
                "BEGIN SELECT RAISE(ABORT, 'completed blocked'); END;"
                "CREATE TRIGGER s2b2_block_failed BEFORE INSERT ON project_memory_compaction_records WHEN NEW.status = 'failed' "
                "BEGIN SELECT RAISE(ABORT, 'failed blocked'); END;")
                .empty(),
        "install the triggers");
    const auto outcome = f.run();
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed"
                && outcome.result.error()->message.find("completed blocked") != std::string::npos,
        "the PRIMARY failure is the commit's, with its own text (the fallback's text does not replace it)");
    require(outcome.accounting == A::NotSaved && outcome.responseObserved, "the usage was NOT saved, and that is proven (NotSaved, not Unknown)");
    requireReceivedAmount(outcome, "both writes failed");
    require(outcome.observedRecord->status == ProjectMemoryCompactionStatus::Failed, "observedRecord is the fallback that failed");
    require(outcome.primaryNative.has_value() && outcome.primaryNative->extendedCode == SQLITE_CONSTRAINT_TRIGGER
                && outcome.fallbackNative.has_value() && outcome.fallbackNative->extendedCode == SQLITE_CONSTRAINT_TRIGGER
                && outcome.fallbackNative->message.find("failed blocked") != std::string::npos,
        "the primary and the fallback native codes are two separate values");
    // the fallback insert's OWN error, as the repository returned it, is a separate cause apart from the commit error and from the codes
    bool fallbackWriteError = false;
    for (const auto& cause : outcome.diagnostics) {
        if (cause.stage == CompactionDiagnosticStage::FallbackWrite && cause.error.code == "persistence.write_failed"
            && cause.error.message.find("failed blocked") != std::string::npos
            && cause.error.message.find("completed blocked") == std::string::npos) {
            fallbackWriteError = true;
        }
    }
    require(fallbackWriteError, "the fallback insert's own error (code and message) is a separate FallbackWrite cause, not only a native detail");
    for (const auto& cause : outcome.diagnostics) {
        require(cause.stage != CompactionDiagnosticStage::Rollback && cause.stage != CompactionDiagnosticStage::ProofRead
                    ? true : cause.error.message.find("failed blocked") == std::string::npos,
            "nothing of the fallback write is labelled with the primary write's phases");
    }
    require(outcome.connection == CompactionConnectionState::Healthy, "the connection is healthy after both real rollbacks");
    require(f.client.observedCalls == 1, "no second provider call");
    require(f.records() == 0 && f.segments() == 3, "nothing is stored; the sources are intact (no result segment)");
    // the connection is still usable
    require(rawExec(f.file, "DROP TRIGGER s2b2_block_completed; DROP TRIGGER s2b2_block_failed;").empty(), "drop the triggers");
    const auto again = f.run();
    require(again.result.success() && again.accounting == A::Saved, "the next compaction on the same connection works");
}

// A paid terminal Failed record (empty summary) whose single insert fails for real.
void testAPaidFailedRecordWhoseInsertFailsIsNotSavedAndKeepsItsCause()
{
    Fixture f;
    f.client.content = "   ";
    require(rawExec(f.file,
                "CREATE TRIGGER s2b2_block_terminal BEFORE INSERT ON project_memory_compaction_records WHEN NEW.status = 'failed' "
                "BEGIN SELECT RAISE(ABORT, 'terminal blocked'); END;")
                .empty(),
        "install the trigger");
    const auto outcome = f.run();
    require(!outcome.result.success() && outcome.result.error()->code == "persistence.write_failed" && outcome.accounting == A::NotSaved,
        "the result is the failure of that insert; NotSaved");
    requireReceivedAmount(outcome, "empty summary");
    require(outcome.observedRecord->errorCode == "project_memory.empty_summary" && outcome.primaryNative.has_value()
                && !outcome.fallbackNative.has_value(),
        "the terminal cause stays in the record; there is no fallback for a terminal record");
    require(f.client.observedCalls == 1 && f.records() == 0 && f.segments() == 3, "one call, nothing stored, sources intact");
}

} // namespace

int runProjectMemoryCompactionAccountingSqliteTests()
{
    try {
        testASuccessfulCompactionIsOneDurableCompletedRow();
        testAConcurrentMemoryChangeLeavesAFailedAccountingRowAndTheNewStateAlone();
        testWhenBothWritesFailTheKnownAmountIsStillReportedAndNothingIsStored();
        testAPaidFailedRecordWhoseInsertFailsIsNotSavedAndKeepsItsCause();
    } catch (const std::exception& exception) {
        std::cerr << "ProjectMemoryCompactionAccountingSqliteTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "ProjectMemoryCompactionAccountingSqliteTests: OK\n";
    return 0;
}
