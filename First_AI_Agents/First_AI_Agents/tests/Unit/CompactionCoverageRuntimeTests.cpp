// B3-III: the compaction recording coverage on the REAL BackendRuntime over a REAL temporary SQLite database. Intents are seeded through
// a SECOND connection with the real repositories (before the owner starts for the Unresolved case, after it for the Open one).
//  - an intent changes no stored total, tokens, price state, evidence count or provider row; the read never writes (the money tables are
//    byte-for-byte equal before and after every read);
//  - the matching final record closes the intent (the storage's own trigger) and is counted exactly ONCE; the coverage falls to a real 0;
//  - another project and a model that is not in the catalog do not distort the selection;
//  - after the owner's startup the Unresolved intent is read by BOTH Application surfaces (the project cost and the history reader) with
//    the same scope, while the startup report stays what it was at startup.
// No provider, no client, no credential, no live database. The checks use throwing require().
#include "ai/application/conversations/CompactionCoverageTypes.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/backend/BackendRuntime.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using ai::backend::BackendRuntime;
using ai::infrastructure::SqliteDatabase;
using ai::infrastructure::SqliteProjectMemoryCompactionIntentRepository;
using ai::infrastructure::SqliteProjectMemoryCompactionRepository;
using Cov = CompactionCoverageState;

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
        root = fs::temp_directory_path() / ("aio-b3iii-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        projectRoot = root / "project";
        databasePath = root / "state" / "orchestrator.sqlite3";
        fs::create_directories(projectRoot);
        fs::create_directories(root / "project-b");
    }
    ~Workspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string moneySnapshot(const fs::path& file)
{
    sqlite3* handle = nullptr;
    require(sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "read-only open failed");
    sqlite3_busy_timeout(handle, 5000);
    std::string out;
    for (const char* table : {"project_memory_compaction_records", "model_executions", "project_memory_segments"}) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, (std::string("SELECT * FROM ") + table + " ORDER BY rowid;").c_str(), -1, &statement, nullptr) == SQLITE_OK,
            "dump prepare failed");
        out += std::string(table) + "\n";
        while (sqlite3_step(statement) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                const unsigned char* raw = sqlite3_column_text(statement, i);
                out += raw != nullptr ? reinterpret_cast<const char*>(raw) : "<NULL>";
                out += '\x1f';
            }
            out += '\n';
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close_v2(handle);
    return out;
}

const auto kCreated = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'000));

ProjectMemoryCompactionIntent intentFor(const std::string& id, const ProjectId& project, const std::string& modelId = "model-1")
{
    ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = project;
    value.providerId = "provider-1";
    value.modelId = modelId;
    value.sourceSegmentIds = {"s1", "s2"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = kCreated;
    return value;
}

void seedIntent(const Workspace& workspace, const ProjectMemoryCompactionIntent& intent)
{
    auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "seed: open");
    SqliteProjectMemoryCompactionIntentRepository intents(opened.value());
    require(mayDispatch(intents.recordIntentObserved(intent)), "seed: the intent is admitted");
}

// A stored final record, seeded through the second connection (the real repository), for the money side.
void seedRecord(const Workspace& workspace, const std::string& id, const ProjectId& project, bool completed)
{
    auto opened = SqliteDatabase::open(workspace.databasePath);
    require(opened.success(), "seed record: open");
    SqliteProjectMemoryCompactionRepository records(opened.value());
    ProjectMemoryCompactionRecord record;
    record.id = id;
    record.projectId = project;
    record.providerId = "provider-1";
    record.modelId = "model-1";
    record.sourceSegmentIds = {"s1", "s2"};
    record.sourceContentHash = std::string(64, 'a');
    record.inputTokens = 40;
    record.outputTokens = 15;
    record.costMicros = 425;
    record.costPriceKnown = true;
    record.status = completed ? ProjectMemoryCompactionStatus::Completed : ProjectMemoryCompactionStatus::Failed;
    if (completed) {
        record.resultSegmentId = "result-" + id;
    } else {
        record.errorCode = "provider.failed";
        record.errorMessage = "scripted";
    }
    record.createdAt = kCreated;
    if (completed) {
        // The Completed path stores the result segment too; the sources must exist for the commit to replace them.
        ProjectMemorySegment s1;
        s1.id = "s1"; s1.projectId = project; s1.runId = ""; s1.summary = "one"; s1.sourceTokenCount = 1; s1.summaryTokenCount = 1; s1.createdAt = kCreated;
        ProjectMemorySegment s2 = s1;
        s2.id = "s2";
        ai::infrastructure::SqliteProjectMemoryRepository memory(opened.value());
        require(memory.insertMemorySegment(s1).success() && memory.insertMemorySegment(s2).success(), "seed: sources");
        ProjectMemorySegment result = s1;
        result.id = *record.resultSegmentId;
        result.summary = "consolidated";
        require(records.commitSuccessfulCompaction(record, result).success(), "seed: the Completed record");
    } else {
        require(records.insertCompactionRecord(record).success(), "seed: the Failed record");
    }
}

ProjectId createProject(BackendRuntime& runtime, const fs::path& root, const char* name)
{
    const auto project = runtime.projectService().createProject({name, "coverage", root});
    require(project.success(), std::string("project ") + name);
    return project.value().id;
}

void requireSameMoney(const ProjectCostSnapshot& a, const ProjectCostSnapshot& b, const std::string& what)
{
    require(a.totals.costMicros() == b.totals.costMicros() && a.totals.inputTokens() == b.totals.inputTokens()
                && a.totals.outputTokens() == b.totals.outputTokens() && a.totals.priceState() == b.totals.priceState()
                && a.totals.pricingEvidenceCount() == b.totals.pricingEvidenceCount() && a.totals.compactionRecordCount() == b.totals.compactionRecordCount()
                && a.totals.ledgerExecutionCount() == b.totals.ledgerExecutionCount() && a.providers.size() == b.providers.size(),
        what + ": the money is the same");
}

void testAnIntentChangesNoMoneyAndTheReadNeverWrites()
{
    Workspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    require(runtime.storageAvailable(), "durable storage");
    const ProjectId project = createProject(runtime, workspace.projectRoot, "A");
    seedRecord(workspace, "record-1", project, /*completed=*/false);

    const auto before = runtime.projectCostService().projectCost(project);
    require(before.success() && before.value().compactionCoverage.state == Cov::Available && before.value().compactionCoverage.openOperations == 0u
                && before.value().compactionCoverage.unresolvedOperations == 0u && before.value().totals.compactionRecordCount() == 1u,
        "setup: one stored record, the coverage is a real zero");

    seedIntent(workspace, intentFor("op-live", project));   // after the owner's startup: an Open protocol
    const std::string moneyBefore = moneySnapshot(workspace.databasePath);
    const auto after = runtime.projectCostService().projectCost(project);
    const auto again = runtime.projectCostService().projectCost(project);
    require(after.success() && again.success(), "the estimate succeeds");
    requireSameMoney(before.value(), after.value(), "an Open intent");
    requireSameMoney(after.value(), again.value(), "a repeated refresh");
    require(after.value().compactionCoverage.state == Cov::Available && after.value().compactionCoverage.openOperations == 1u
                && after.value().compactionCoverage.unresolvedOperations == 0u && again.value().compactionCoverage.openOperations == 1u,
        "the coverage says one Open protocol, and a repeated refresh does not double it");
    const auto section = runtime.compactionIntentReadModel().read(project);
    require(section.rows.size() == 1 && section.rows[0].state == CompactionIntentState::Open && section.coverage.openOperations == 1u,
        "the history reader sees the same row");
    require(moneySnapshot(workspace.databasePath) == moneyBefore, "records, ledger and segments are byte-for-byte equal after the reads: reading never writes");
}

void testTheMatchingFinalRecordClosesTheIntentAndIsCountedOnce()
{
    Workspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    const ProjectId project = createProject(runtime, workspace.projectRoot, "A");
    ProjectMemoryCompactionIntent intent = intentFor("op-1", project);
    seedIntent(workspace, intent);
    const auto open = runtime.projectCostService().projectCost(project);
    require(open.success() && open.value().compactionCoverage.openOperations == 1u && open.value().totals.compactionRecordCount() == 0u
                && open.value().totals.pricingEvidenceCount() == 0u,
        "setup: an Open protocol and no stored record");

    seedRecord(workspace, "op-1", project, /*completed=*/false);   // the same id and identity: the trigger closes the intent
    const auto closed = runtime.projectCostService().projectCost(project);
    const auto repeated = runtime.projectCostService().projectCost(project);
    require(closed.success() && closed.value().totals.compactionRecordCount() == 1u && closed.value().totals.costMicros() == 425u
                && closed.value().compactionCoverage.openOperations == 0u && closed.value().compactionCoverage.unresolvedOperations == 0u
                && closed.value().compactionCoverage.state == Cov::Available,
        "the record is counted ONCE and the coverage fell to a real zero");
    requireSameMoney(closed.value(), repeated.value(), "a repeated refresh after the closure");
}

void testAnotherProjectAndAModelOutsideTheCatalogDoNotDistortTheSelection()
{
    Workspace workspace;
    BackendRuntime runtime({workspace.databasePath});
    const ProjectId a = createProject(runtime, workspace.projectRoot, "A");
    const ProjectId b = createProject(runtime, workspace.root / "project-b", "B");
    seedIntent(workspace, intentFor("op-a", a, "model-that-is-not-in-the-catalog"));
    seedIntent(workspace, intentFor("op-b", b));
    const auto forA = runtime.projectCostService().projectCost(a);
    const auto forB = runtime.projectCostService().projectCost(b);
    require(forA.success() && forA.value().compactionCoverage.openOperations == 1u && forB.value().compactionCoverage.openOperations == 1u,
        "each project counts only its own protocol");
    const auto rowsA = runtime.compactionIntentReadModel().read(a);
    require(rowsA.rows.size() == 1 && rowsA.rows[0].id == "op-a" && rowsA.rows[0].modelId == "model-that-is-not-in-the-catalog" && rowsA.rows[0].projectId == a,
        "a model that is not in the catalog stays in the row as an id; the other project's row is not there");
    require(forA.value().providers.empty() && forA.value().totals.pricingEvidenceCount() == 0, "and neither creates a provider or model row");
}

void testTheOwnersUnresolvedIntentIsReadByBothSurfacesAndTheReportStaysAStartupSnapshot()
{
    Workspace workspace;
    ProjectId project;
    {
        BackendRuntime first({workspace.databasePath});
        project = createProject(first, workspace.projectRoot, "A");
    }
    seedIntent(workspace, intentFor("op-old", project));   // before the owner starts: a previous process left it open

    BackendRuntime runtime({workspace.databasePath});
    require(runtime.compactionIntentRecoveryReport().has_value() && runtime.compactionIntentRecoveryReport()->markedUnresolved == 1,
        "the owner's startup marked it");
    const auto cost = runtime.projectCostService().projectCost(project);
    const auto history = runtime.compactionIntentReadModel().read(project);
    require(cost.success() && cost.value().compactionCoverage.unresolvedOperations == 1u && cost.value().compactionCoverage.openOperations == 0u
                && history.coverage.unresolvedOperations == 1u && history.coverage.openOperations == 0u && history.rows.size() == 1
                && history.rows[0].state == CompactionIntentState::Unresolved && history.rows[0].unresolvedMarkedAt.has_value()
                && !cost.value().compactionCoverage.windowed && !history.coverage.windowed,
        "BOTH surfaces read the Unresolved intent with the same (all-time) scope");

    seedIntent(workspace, intentFor("op-new", project));   // a new Open protocol after the startup
    const auto later = runtime.projectCostService().projectCost(project);
    require(later.value().compactionCoverage.openOperations == 1u && later.value().compactionCoverage.unresolvedOperations == 1u
                && runtime.compactionIntentRecoveryReport()->markedUnresolved == 1 && runtime.compactionIntentRecoveryReport()->inspected == 1,
        "the coverage is current; the startup report is still the report of the startup (it did not become the current count)");

    // The same project in a window that excludes the intents: windowed, a real zero, and the money untouched.
    const auto windowed = runtime.projectCostService().projectCost(project, kCreated + std::chrono::hours(1), kCreated + std::chrono::hours(2));
    require(windowed.success() && windowed.value().compactionCoverage.windowed && windowed.value().compactionCoverage.openOperations == 0u
                && windowed.value().compactionCoverage.unresolvedOperations == 0u,
        "a period that holds none of them is a windowed real zero");
}

} // namespace

int runCompactionCoverageRuntimeTests()
{
    try {
        testAnIntentChangesNoMoneyAndTheReadNeverWrites();
        testTheMatchingFinalRecordClosesTheIntentAndIsCountedOnce();
        testAnotherProjectAndAModelOutsideTheCatalogDoNotDistortTheSelection();
        testTheOwnersUnresolvedIntentIsReadByBothSurfacesAndTheReportStaysAStartupSnapshot();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionCoverageRuntimeTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionCoverageRuntimeTests: OK\n";
    return 0;
}
