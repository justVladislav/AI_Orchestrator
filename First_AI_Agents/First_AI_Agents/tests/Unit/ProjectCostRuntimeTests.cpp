// A1-min: the project cost estimate on the REAL BackendRuntime over a REAL temporary SQLite database. The compaction history and the ledger
// are seeded through a SECOND connection with the real repositories (no production seam, no new public insert): the real mapping of the
// nullable cost_price_known (NULL / 0 / 1), a paid Failed and Cancelled record, a record of a provider that is not in the catalog, a
// record of another project and a time window - then BackendRuntime::projectCostService() is asked and must agree with the rules of
// the pure composition. The estimate never writes. The checks use throwing require().
#include "ai/backend/BackendRuntime.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using ai::backend::BackendRuntime;
using ai::infrastructure::SqliteDatabase;
using S = ProjectMemoryCompactionStatus;

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
            / ("aio-project-cost-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "raw prepare failed");
    long long value = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return value;
}

const auto kBase = std::chrono::system_clock::time_point {std::chrono::milliseconds {1'700'000'000'000}};
std::chrono::system_clock::time_point at(int seconds) { return kBase + std::chrono::seconds {seconds}; }

ProjectMemoryCompactionRecord record(const std::string& id, const ProjectId& project, const ProviderId& provider, const ModelId& model,
    S status, std::optional<bool> priceKnown, std::uint64_t cost, std::uint64_t in, std::uint64_t out, int seconds)
{
    ProjectMemoryCompactionRecord value;
    value.id = id;
    value.projectId = project;
    value.providerId = provider;
    value.modelId = model;
    value.sourceSegmentIds = {"s1"};
    value.sourceContentHash = "hash-" + id;
    value.inputTokens = in;
    value.outputTokens = out;
    value.costMicros = cost;
    value.costPriceKnown = priceKnown;
    value.status = status;
    if (status != S::Completed) {
        value.errorCode = "provider.failed";
        value.errorMessage = "scripted";
    }
    value.createdAt = at(seconds);
    return value;
}

ProjectMemorySegment segment(const std::string& id, const ProjectId& project, int seconds)
{
    ProjectMemorySegment value;
    value.id = id;
    value.projectId = project;
    value.runId = "";
    value.summary = "Summary " + id;
    value.sourceTokenCount = 100;
    value.summaryTokenCount = 10;
    value.createdAt = at(seconds);
    return value;
}

ModelExecution ledgerRow(const std::string& id, const ProjectId& project, std::uint64_t cost, bool priceKnown, int seconds)
{
    ModelExecution row;
    row.id = id;
    row.source = ModelExecutionSource::Chat;
    row.sourceId = "chat-" + id;
    row.projectId = project;
    row.providerId = "prov-1";
    row.modelId = "model-1";
    row.role = "assistant";
    row.position = 0;
    row.status = ModelExecutionStatus::Completed;
    row.inputTokens = 10;
    row.outputTokens = 20;
    row.costMicros = cost;
    row.costPriceKnown = priceKnown;
    row.latencyMs = 120;
    row.createdAt = at(seconds);
    return row;
}

void testTheRealRuntimeComposesTheLedgerAndTheStoredCompactionsAndNeverWrites()
{
    Workspace workspace;
    ProjectId projectA;
    ProjectId projectB;
    {
        BackendRuntime seed({workspace.databasePath});
        require(seed.storageAvailable(), "durable storage");
        const auto a = seed.projectService().createProject({"Project A", "cost", workspace.projectRoot});
        require(a.success(), "project A");
        projectA = a.value().id;
        fs::create_directories(workspace.root / "project-b");
        const auto b = seed.projectService().createProject({"Project B", "cost", workspace.root / "project-b"});
        require(b.success(), "project B");
        projectB = b.value().id;
    }
    {
        const auto opened = SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "second connection");
        const auto database = opened.value();
        ai::infrastructure::SqliteProviderRepository providers(database);
        ai::infrastructure::SqliteModelRepository models(database);
        ai::infrastructure::SqliteModelExecutionRepository ledger(database);
        ai::infrastructure::SqliteProjectMemoryRepository memory(database);
        ai::infrastructure::SqliteProjectMemoryCompactionRepository compactions(database);

        const auto now = std::chrono::system_clock::now();
        Provider provider;
        provider.id = "prov-1";
        provider.name = "Provider one";
        provider.kind = "openai_compatible";
        provider.baseUrl = "http://127.0.0.1:1/v1";
        provider.enabled = true;
        provider.createdAt = now;
        provider.updatedAt = now;
        require(providers.insert(provider).success(), "provider");
        Model model;
        model.id = "model-1";
        model.providerId = "prov-1";
        model.apiModelId = "api-1";
        model.displayName = "Model one";
        model.contextWindowTokens = 100'000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 5'000'000;
        model.outputPriceMicrosPerMillionTokens = 15'000'000;
        model.enabled = true;
        model.createdAt = now;
        model.updatedAt = now;
        require(models.insert(model).success(), "model");

        // The ledger: one priced execution of project A, one of project B.
        require(ledger.insertExecution(ledgerRow("l-a", projectA, 200, true, 10)).success(), "ledger A");
        require(ledger.insertExecution(ledgerRow("l-b", projectB, 8888, true, 10)).success(), "ledger B");

        // The history of project A: a Completed record (committed with its segments), then Failed/Cancelled ones with every flag.
        require(memory.insertMemorySegment(segment("s1", projectA, 1)).success(), "source segment");
        auto completed = record("c-completed", projectA, "prov-1", "model-1", S::Completed, true, 300, 50, 10, 11);
        completed.sourceSegmentIds = {"s1"};
        completed.resultSegmentId = "s-result";
        require(compactions.commitSuccessfulCompaction(completed, segment("s-result", projectA, 11)).success(), "completed record");
        require(compactions.insertCompactionRecord(record("c-paid-failed", projectA, "prov-1", "model-1", S::Failed, false, 150, 5, 0, 12)).success(),
            "paid Failed, flag false");
        require(compactions.insertCompactionRecord(record("c-none", projectA, "prov-1", "model-1", S::Cancelled, std::nullopt, 0, 0, 0, 13)).success(),
            "NULL Cancelled 0/0/0");
        require(compactions.insertCompactionRecord(record("c-known-zero", projectA, "prov-1", "model-1", S::Failed, true, 0, 3, 0, 14)).success(),
            "known zero");
        require(compactions.insertCompactionRecord(record("c-null-partial", projectA, "prov-1", "model-1", S::Failed, std::nullopt, 70, 0, 0, 15)).success(),
            "NULL with an amount");
        require(compactions.insertCompactionRecord(record("c-gone", projectA, "prov-gone", "model-gone", S::Cancelled, true, 40, 2, 2, 16)).success(),
            "a record of a provider that is not in the catalog");
        // Another project.
        require(compactions.insertCompactionRecord(record("c-other", projectB, "prov-1", "model-1", S::Failed, true, 9999, 1, 1, 12)).success(),
            "project B");
    }

    const fs::path file = workspace.databasePath;
    const long long ledgerRowsBefore = rawCount(file, "SELECT COUNT(*) FROM model_executions");
    const long long compactionRowsBefore = rawCount(file, "SELECT COUNT(*) FROM project_memory_compaction_records");

    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "durable storage");

    // The nullable flag survives the real round trip.
    const auto stored = runtime.projectMemoryCompactionService().listCompactionRecords(projectA);
    require(stored.success() && stored.value().size() == 6, "six stored records for project A");
    const auto flagOf = [&](const std::string& id) -> std::optional<std::optional<bool>> {
        for (const auto& item : stored.value()) {
            if (item.id == id) return item.costPriceKnown;
        }
        return std::nullopt;
    };
    require(flagOf("c-completed") == std::optional<std::optional<bool>> {std::optional<bool> {true}}, "true round-trips");
    require(flagOf("c-paid-failed") == std::optional<std::optional<bool>> {std::optional<bool> {false}}, "false round-trips");
    require(flagOf("c-none") == std::optional<std::optional<bool>> {std::optional<bool> {}}, "NULL round-trips");

    const auto cost = runtime.projectCostService().projectCost(projectA);
    require(cost.success(), "the estimate is produced");
    const auto& totals = cost.value().totals;
    require(totals.ledgerExecutionCount() == 1 && totals.compactionRecordCount() == 6, "one ledger row and six compaction records");
    require(totals.compaction.knownCount == 3 && totals.compaction.partialCount == 2 && totals.compaction.unknownCount == 0
            && totals.compaction.noneCount == 1,
        "known (completed, known zero, deleted provider), partial (paid false, NULL with an amount), none (NULL Cancelled 0/0)");
    require(totals.costMicros() == 200 + 300 + 150 + 0 + 70 + 40, "the stored amounts of both sources are summed: 760");
    require(totals.pricingEvidenceCount() == 6 && totals.incompletePriceCount() == 2 && totals.priceState() == ExecutionPriceState::Partial,
        "evidence 1 + 5, incomplete 2, partial");
    require(totals.compaction.completedCount == 1 && totals.compaction.failedCount == 3 && totals.compaction.cancelledCount == 2, "the statuses");
    require(cost.value().providers.size() == 2, "prov-1 and the provider that is not in the catalog");
    require(cost.value().providers[0].providerId == "prov-1" && cost.value().providers[0].totals.costMicros() == 720
            && cost.value().providers[1].providerId == "prov-gone" && cost.value().providers[1].totals.costMicros() == 40,
        "the rows are keyed by the stored ids, ordered by cost");

    // Another project is isolated; an empty one is none.
    const auto other = runtime.projectCostService().projectCost(projectB);
    require(other.success() && other.value().totals.costMicros() == 8888 + 9999 && other.value().totals.compactionRecordCount() == 1,
        "project B has only its own two sources");
    const auto none = runtime.projectCostService().projectCost("no-such-project");
    require(none.success() && none.value().totals.priceState() == ExecutionPriceState::None && none.value().providers.empty(),
        "a project with nothing recorded is none, not a zero");

    // The same window for both sources: [12 s, 15 s) keeps the paid Failed (12), the NULL Cancelled (13) and the known zero (14).
    const auto windowed = runtime.projectCostService().projectCost(projectA, at(12), at(15));
    require(windowed.success() && windowed.value().totals.ledgerExecutionCount() == 0 && windowed.value().totals.compactionRecordCount() == 3
            && windowed.value().totals.costMicros() == 150,
        "the half-open window applies to the compactions and to the ledger alike");

    // Reading is repeatable and writes nothing.
    const auto again = runtime.projectCostService().projectCost(projectA);
    require(again.success() && again.value().totals.costMicros() == totals.costMicros(), "a second read is equal");
    require(rawCount(file, "SELECT COUNT(*) FROM model_executions") == ledgerRowsBefore
            && rawCount(file, "SELECT COUNT(*) FROM project_memory_compaction_records") == compactionRowsBefore,
        "no ledger row and no compaction record was created or removed by reading");
}

} // namespace

int runProjectCostRuntimeTests()
{
    try {
        testTheRealRuntimeComposesTheLedgerAndTheStoredCompactionsAndNeverWrites();
    } catch (const std::exception& exception) {
        std::cerr << "FAILED testTheRealRuntimeComposesTheLedgerAndTheStoredCompactionsAndNeverWrites: " << exception.what() << std::endl;
        return 1;
    }
    std::cerr << "[project-cost-runtime] 1 case passed" << std::endl;
    return 0;
}
