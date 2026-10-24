#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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
    fs::path root = fs::temp_directory_path() / ("aio-sqlite-comparison-"
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

void insertProject(const std::shared_ptr<SqliteDatabase>& database,
    const Workspace& workspace, const std::string& id)
{
    SqliteProjectRepository projects(database);
    require(projects.insert(Project {id, "Test project", "Synthetic context",
        workspace.root / id, ProjectStatus::Active, at(1000), at(1000)}).success(),
        "Could not insert project through the other repository");
}

ComparisonSession session(std::string id, std::string projectId, Time createdAt)
{
    ComparisonSession value;
    value.id = std::move(id);
    value.projectId = std::move(projectId);
    value.title = "Comparison \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.prompt = "Line one\nLine two";
    value.requestHash = "request-hash";
    value.contextHash = "context-hash";
    value.createdAt = createdAt;
    return value;
}

void testRoundTripOrderingAndLifetime()
{
    Workspace workspace;
    {
        auto database = workspace.open();
        insertProject(database, workspace, "project-a");
        insertProject(database, workspace, "project-b");
        SqliteComparisonSessionRepository comparisons(database);
        auto pending = session("z-pending", "project-a", at(2000));
        pending.synthesisSelection = {"execution-\"one\"", "execution-\\two"};
        require(comparisons.insertSession(pending).success(), "Pending insert failed");
        auto running = session("a-running", "project-a", at(1000));
        running.status = ComparisonSessionStatus::Running;
        running.startedAt = at(1500);
        require(comparisons.insertSession(running).success(), "Running insert failed");
        auto completed = session("m-completed", "project-b", at(1000));
        completed.status = ComparisonSessionStatus::Completed;
        completed.startedAt = at(1100);
        completed.finishedAt = at(1400);
        require(comparisons.insertSession(completed).success(), "Completed insert failed");

        // The repository must retain the shared connection after its caller lets go.
        database.reset();
        const auto found = comparisons.findSession(pending.id);
        require(found.success() && found.value().has_value(), "Session disappeared");
        const auto& stored = *found.value();
        require(stored.title == pending.title && stored.prompt == pending.prompt
                && stored.requestHash == pending.requestHash && stored.contextHash == pending.contextHash
                && stored.synthesisSelection == pending.synthesisSelection
                && stored.createdAt == pending.createdAt && !stored.startedAt && !stored.finishedAt
                && !stored.preferredExecutionId && !stored.synthesisExecutionId && stored.revision == 0,
            "Comparison fields/optional values did not round-trip");
        const auto all = comparisons.listSessions(std::nullopt);
        require(all.success() && all.value().size() == 3
                && all.value()[0].id == running.id && all.value()[1].id == completed.id
                && all.value()[2].id == pending.id, "List order must be created_at_ms then id");
        const auto project = comparisons.listSessions("project-a");
        require(project.success() && project.value().size() == 2
                && project.value()[0].id == running.id && project.value()[1].id == pending.id,
            "Project filtering/order changed");
        const auto unfinished = comparisons.listUnfinishedSessions();
        require(unfinished.success() && unfinished.value().size() == 2
                && unfinished.value()[0].id == running.id && unfinished.value()[1].id == pending.id,
            "Terminal sessions leaked into unfinished sessions");
    }
    auto database = workspace.open();
    SqliteComparisonSessionRepository comparisons(database);
    const auto stored = comparisons.findSession("m-completed");
    require(stored.success() && stored.value() && stored.value()->startedAt == at(1100)
            && stored.value()->finishedAt == at(1400), "Session/timestamps did not survive reopen");
    const auto missing = comparisons.findSession("missing");
    require(missing.success() && !missing.value(), "Missing session must be an empty optional");
}

void testRevisionConflictsAndBindingFailures()
{
    Workspace workspace;
    auto database = workspace.open();
    insertProject(database, workspace, "project");
    SqliteComparisonSessionRepository comparisons(database);
    auto value = session("comparison", "project", at(1000));
    require(comparisons.insertSession(value).success(), "Insert failed");
    const auto duplicate = comparisons.insertSession(value);
    require(!duplicate.success() && duplicate.error()->code == "persistence.write_failed",
        "Duplicate must keep the existing write-failure code");
    const auto zero = comparisons.updateSession(value);
    require(!zero.success() && zero.error()->code == "comparison.revision_conflict",
        "Revision zero update must fail");
    value.revision = 1;
    value.status = ComparisonSessionStatus::Failed;
    value.errorCode = "example.failure";
    value.errorMessage = "Synthetic failure";
    value.startedAt = at(1200);
    value.finishedAt = at(1300);
    require(comparisons.updateSession(value).success(), "Versioned update failed");
    const auto stale = comparisons.updateSession(value);
    require(!stale.success() && stale.error()->code == "comparison.revision_conflict",
        "Stale update must fail without overwriting persisted state");
    value.revision = std::numeric_limits<std::uint64_t>::max();
    const auto overflow = comparisons.updateSession(value);
    require(!overflow.success() && overflow.error()->code == "persistence.value_out_of_range",
        "Unsigned binding overflow changed its error code");
    const auto found = comparisons.findSession(value.id);
    require(found.success() && found.value() && found.value()->revision == 1
            && found.value()->errorCode == "example.failure"
            && found.value()->errorMessage == "Synthetic failure"
            && found.value()->finishedAt == at(1300), "Failed writes altered the stored session");
}

void testForeignKeyFailureDoesNotPoisonConnection()
{
    Workspace workspace;
    auto database = workspace.open();
    SqliteComparisonSessionRepository comparisons(database);
    const auto value = session("comparison", "project", at(1000));
    const auto rejected = comparisons.insertSession(value);
    require(!rejected.success() && rejected.error()->code == "persistence.write_failed",
        "Missing project must fail the foreign-key constraint");
    insertProject(database, workspace, "project");
    require(comparisons.insertSession(value).success(), "Failed statement poisoned the shared connection");
}

void testInvalidSelectionIsRejectedByEveryReader()
{
    Workspace workspace;
    auto database = workspace.open();
    insertProject(database, workspace, "project");
    SqliteComparisonSessionRepository comparisons(database);
    require(comparisons.insertSession(session("comparison", "project", at(1000))).success(), "Insert failed");
    for (const char* sql : {
             "UPDATE comparison_sessions SET synthesis_selection_json='{}';",
             "UPDATE comparison_sessions SET synthesis_selection_json='[\"\"]';",
             "UPDATE comparison_sessions SET synthesis_selection_json='[';"}) {
        workspace.sql(sql);
        const auto found = comparisons.findSession("comparison");
        const auto all = comparisons.listSessions(std::nullopt);
        const auto unfinished = comparisons.listUnfinishedSessions();
        require(!found.success() && found.error()->code == "persistence.invalid_data"
                && !all.success() && all.error()->code == "persistence.invalid_data"
                && !unfinished.success() && unfinished.error()->code == "persistence.invalid_data",
            "A comparison reader accepted corrupt selection data");
    }
    workspace.sql("UPDATE comparison_sessions SET synthesis_selection_json='[]';");
    require(comparisons.findSession("comparison").success(), "Readers did not recover after fixture repair");
}

void testConcurrentRepositoriesHaveOneRevisionWinner()
{
    Workspace workspace;
    auto database = workspace.open();
    insertProject(database, workspace, "project");
    SqliteComparisonSessionRepository first(database), second(database);
    auto current = session("comparison", "project", at(1000));
    require(first.insertSession(current).success(), "Insert failed");
    for (std::uint64_t revision = 1; revision <= 32; ++revision) {
        auto left = current, right = current;
        left.revision = right.revision = revision;
        left.title = "Left";
        right.title = "Right";
        bool leftWon = false, rightWon = false;
        std::string leftError, rightError;
        std::barrier ready(3);
        std::thread a([&] { ready.arrive_and_wait(); const auto result = first.updateSession(left);
            leftWon = result.success(); if (!leftWon) leftError = result.error()->code; });
        std::thread b([&] { ready.arrive_and_wait(); const auto result = second.updateSession(right);
            rightWon = result.success(); if (!rightWon) rightError = result.error()->code; });
        ready.arrive_and_wait();
        a.join();
        b.join();
        require(leftWon != rightWon && (leftWon ? rightError : leftError) == "comparison.revision_conflict",
            "Exactly one concurrent update must win each revision");
        const auto stored = first.findSession(current.id);
        require(stored.success() && stored.value() && stored.value()->revision == revision
                && stored.value()->title == (leftWon ? "Left" : "Right"), "Persisted revision winner differs");
        current = *stored.value();
    }
}
} // namespace

int main()
{
    try {
        testRoundTripOrderingAndLifetime();
        testRevisionConflictsAndBindingFailures();
        testForeignKeyFailureDoesNotPoisonConnection();
        testInvalidSelectionIsRejectedByEveryReader();
        testConcurrentRepositoriesHaveOneRevisionWinner();
        std::cout << "SQLite Comparison contracts: 5 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Comparison contracts failed: " << error.what() << '\n';
        return 1;
    }
}
