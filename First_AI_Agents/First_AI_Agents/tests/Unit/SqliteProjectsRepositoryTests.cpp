#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
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
    fs::path root = fs::temp_directory_path() / ("aio-sqlite-projects-"
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

    // A project root that is never created: the repository stores the path text as given.
    fs::path projectRoot(const std::string& name) const { return root / "projects" / name; }
};

Project makeProject(std::string id, Time createdAt, fs::path rootPath)
{
    Project value;
    value.id = std::move(id);
    value.name = "Project \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.description = "Description with 'apostrophes'\nand a second line";
    value.rootPath = std::move(rootPath);
    value.status = ProjectStatus::Active;
    value.createdAt = createdAt;
    value.updatedAt = createdAt + std::chrono::milliseconds(7);
    return value;
}

void requireSameProject(const Project& actual, const Project& expected, const char* message)
{
    require(actual.id == expected.id && actual.name == expected.name
            && actual.description == expected.description && actual.rootPath == expected.rootPath
            && actual.status == expected.status && actual.createdAt == expected.createdAt
            && actual.updatedAt == expected.updatedAt,
        message);
}

void requireStoredProject(const SqliteProjectRepository& projects, const Project& expected, const char* message)
{
    const auto stored = projects.findById(expected.id);
    require(stored.success() && stored.value().has_value(), message);
    requireSameProject(*stored.value(), expected, message);
}

std::vector<std::string> projectIds(const SqliteProjectRepository& projects)
{
    const auto listed = projects.listAll();
    require(listed.success(), "Could not list projects");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

void testProjectRoundTripOrderingAndLookup()
{
    Workspace workspace;
    const fs::path unicodeRoot = workspace.root
        / fs::path(std::u8string(u8"Данные-数据"))
        / fs::path(std::u8string(u8"проект"));
    Project archived = makeProject("project-b", at(1000), workspace.projectRoot("beta"));
    archived.status = ProjectStatus::Archived;
    Project unavailable = makeProject("project-a", at(1000), unicodeRoot);
    unavailable.status = ProjectStatus::Unavailable;
    const Project late = makeProject("project-late", at(2000), workspace.projectRoot("late"));
    using Ids = std::vector<std::string>;
    {
        const auto database = workspace.open();
        SqliteProjectRepository projects(database);
        require(projects.insert(late).success(), "Could not insert a project");
        require(projects.insert(archived).success(), "Could not insert an archived project");
        const auto saved = projects.insert(unavailable);
        require(saved.success(), "Could not insert an unavailable project");
        requireSameProject(saved.value(), unavailable, "insert must return the project as written");

        requireStoredProject(projects, archived, "An archived project must round-trip");
        requireStoredProject(projects, unavailable, "A project under a Unicode path must round-trip");
        requireStoredProject(projects, late, "An active project must round-trip");
        require(projectIds(projects) == Ids {"project-a", "project-b", "project-late"},
            "Projects must be listed by creation time and then id");

        const auto byPath = projects.findByRootPath(unicodeRoot);
        require(byPath.success() && byPath.value().has_value() && byPath.value()->id == "project-a",
            "findByRootPath must find a project by its exact root path");
        requireSameProject(*byPath.value(), unavailable, "findByRootPath must return the whole project");
        const auto parent = projects.findByRootPath(unicodeRoot.parent_path());
        require(parent.success() && !parent.value().has_value(),
            "A path that is only a prefix of a root must not match");
        const auto missingId = projects.findById("project-missing");
        require(missingId.success() && !missingId.value().has_value(), "A missing project must read as absent");
    }

    const auto reopened = workspace.open();
    SqliteProjectRepository projects(reopened);
    requireStoredProject(projects, archived, "A project must survive reopening the database");
    requireStoredProject(projects, unavailable, "A Unicode root must survive reopening the database");
    require(projectIds(projects) == Ids {"project-a", "project-b", "project-late"},
        "The listing order must survive reopening the database");
    const auto byPath = projects.findByRootPath(unicodeRoot);
    require(byPath.success() && byPath.value().has_value() && byPath.value()->id == "project-a",
        "findByRootPath must work after reopening");
}

void testProjectWriteErrorsAndUpdate()
{
    Workspace workspace;
    const auto database = workspace.open();
    SqliteProjectRepository projects(database);
    const Project first = makeProject("project-1", at(1000), workspace.projectRoot("one"));
    const Project second = makeProject("project-2", at(2000), workspace.projectRoot("two"));
    require(projects.insert(first).success() && projects.insert(second).success(), "Could not insert projects");

    const auto sameId = projects.insert(makeProject("project-1", at(3000), workspace.projectRoot("other")));
    require(!sameId.success() && sameId.error()->code == "project.duplicate_id",
        "A repeated project id must report project.duplicate_id");
    const auto samePath = projects.insert(makeProject("project-3", at(3000), workspace.projectRoot("one")));
    require(!samePath.success() && samePath.error()->code == "project.duplicate_path",
        "A repeated root path must report project.duplicate_path");
    require(projectIds(projects) == std::vector<std::string> {"project-1", "project-2"},
        "Refused inserts must not add rows");
    requireStoredProject(projects, first, "A refused insert altered an existing project");

    // update() replaces every field, including the creation time and the root path.
    Project changed = first;
    changed.name = "Renamed \xE6\xB8\xAC";
    changed.description = "New description";
    changed.rootPath = workspace.projectRoot("one-moved");
    changed.status = ProjectStatus::Archived;
    changed.createdAt = at(500);
    changed.updatedAt = at(9000);
    const auto updated = projects.update(changed);
    require(updated.success(), "Could not update a project");
    requireSameProject(updated.value(), changed, "update must return the project as written");
    requireStoredProject(projects, changed, "An update must replace every field");
    requireStoredProject(projects, second, "An update must not touch another project");
    const auto movedOut = projects.findByRootPath(workspace.projectRoot("one"));
    require(movedOut.success() && !movedOut.value().has_value(), "The old root path must no longer match");

    // Taking another project's root path is refused as a plain write failure and changes nothing.
    Project clash = changed;
    clash.name = "Must not persist";
    clash.rootPath = second.rootPath;
    const auto clashed = projects.update(clash);
    require(!clashed.success() && clashed.error()->code == "persistence.write_failed",
        "An update onto another project's root path must be refused as a failed write");
    requireStoredProject(projects, changed, "A refused update changed the project");
    requireStoredProject(projects, second, "A refused update changed another project");

    const auto missing = projects.update(makeProject("project-missing", at(1000), workspace.projectRoot("none")));
    require(!missing.success() && missing.error()->code == "project.not_found",
        "Updating a missing project must report project.not_found");
    require(projectIds(projects) == std::vector<std::string> {"project-1", "project-2"},
        "A refused update must not create a project");

    for (const ProjectStatus status : {ProjectStatus::Unavailable, ProjectStatus::Active}) {
        changed.status = status;
        require(projects.update(changed).success(), "Could not change the project status");
        requireStoredProject(projects, changed, "Every project status must be storable");
    }
    require(projects.insert(makeProject("project-4", at(4000), workspace.projectRoot("four"))).success(),
        "The connection must be usable after refused writes");
}

void testCorruptProjectRowsFailClosed()
{
    Workspace workspace;
    {
        const auto database = workspace.open();
        SqliteProjectRepository projects(database);
        require(projects.insert(makeProject("project-good", at(1000), workspace.projectRoot("good"))).success()
                && projects.insert(makeProject("project-bad", at(2000), workspace.projectRoot("bad"))).success(),
            "Could not insert projects");
    }
    // The schema refuses an unknown status, so the check is switched off on the corrupting connection only.
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE projects SET status='mystery' WHERE id='project-bad';");

    const auto database = workspace.open();
    SqliteProjectRepository projects(database);
    const auto good = projects.findById("project-good");
    require(good.success() && good.value().has_value(), "A valid project must stay readable");
    const auto bad = projects.findById("project-bad");
    require(!bad.success() && bad.error()->code == "persistence.invalid_data",
        "A project with an unknown status must fail closed on findById");
    const auto byPath = projects.findByRootPath(workspace.projectRoot("bad"));
    require(!byPath.success() && byPath.error()->code == "persistence.invalid_data",
        "A project with an unknown status must fail closed on findByRootPath");
    const auto everything = projects.listAll();
    require(!everything.success() && everything.error()->code == "persistence.invalid_data",
        "A listing that contains an invalid project must fail rather than skip it");
}

void testConcurrentProjectWritersShareOneConnection()
{
    Workspace workspace;
    const auto database = workspace.open();
    SqliteProjectRepository first(database);
    SqliteProjectRepository second(database);
    constexpr int perWriter = 25;
    std::barrier ready(2);
    bool firstOk = true;
    bool secondOk = true;
    std::thread a([&] {
        ready.arrive_and_wait();
        for (int i = 0; i < perWriter; ++i) {
            firstOk = first.insert(makeProject("a-" + std::to_string(i), at(1000 + i),
                          workspace.projectRoot("a-" + std::to_string(i)))).success() && firstOk;
        }
    });
    std::thread b([&] {
        ready.arrive_and_wait();
        for (int i = 0; i < perWriter; ++i) {
            secondOk = second.insert(makeProject("b-" + std::to_string(i), at(1000 + i),
                           workspace.projectRoot("b-" + std::to_string(i)))).success() && secondOk;
        }
    });
    a.join();
    b.join();
    require(firstOk && secondOk, "Concurrent project inserts through the shared connection must all succeed");
    const auto all = first.listAll();
    require(all.success() && all.value().size() == 2 * perWriter, "Every concurrent insert must be stored once");
}

void addProject(const std::shared_ptr<SqliteDatabase>& database, const Workspace& workspace, const std::string& id)
{
    SqliteProjectRepository projects(database);
    require(projects.insert(makeProject(id, at(500), workspace.projectRoot(id))).success(),
        "Could not insert a project for the task tests");
}

void addRun(const std::shared_ptr<SqliteDatabase>& database, const std::string& runId, const std::string& projectId)
{
    SqliteRunRepository runs(database);
    Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "Linked run";
    run.task = "Run linked to a task";
    run.workflowId = "projects-test-workflow";
    run.createdAt = at(600);
    require(runs.insertRun(run).success(), "Could not insert a run for the task tests");
}

ProjectTask makeTask(std::string id, std::string projectId, std::int64_t updatedMs)
{
    ProjectTask value;
    value.id = std::move(id);
    value.projectId = std::move(projectId);
    value.title = "Task \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.description = "Task description\nsecond line";
    value.status = TaskStatus::Backlog;
    value.priority = TaskPriority::Normal;
    value.requiresAttention = false;
    value.createdAt = at(500);
    value.updatedAt = at(updatedMs);
    return value;
}

void requireSameTask(const ProjectTask& actual, const ProjectTask& expected, const char* message)
{
    require(actual.id == expected.id && actual.projectId == expected.projectId && actual.title == expected.title
            && actual.description == expected.description && actual.status == expected.status
            && actual.priority == expected.priority && actual.requiresAttention == expected.requiresAttention
            && actual.dueAt == expected.dueAt && actual.plannedRunAt == expected.plannedRunAt
            && actual.createdAt == expected.createdAt && actual.updatedAt == expected.updatedAt,
        message);
}

void requireStoredTask(const SqliteTaskRepository& tasks, const ProjectTask& expected, const char* message)
{
    const auto stored = tasks.findById(expected.id);
    require(stored.success() && stored.value().has_value(), message);
    requireSameTask(*stored.value(), expected, message);
}

std::vector<std::string> taskIds(const SqliteTaskRepository& tasks, const std::optional<ProjectId>& projectId)
{
    const auto listed = tasks.list(projectId);
    require(listed.success(), "Could not list tasks");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

void testTaskRoundTripOrderingAndFilter()
{
    Workspace workspace;
    const TaskStatus statuses[] = {TaskStatus::Backlog, TaskStatus::Planned, TaskStatus::InProgress,
        TaskStatus::Blocked, TaskStatus::Done, TaskStatus::Cancelled};
    const TaskPriority priorities[] = {TaskPriority::Low, TaskPriority::Normal, TaskPriority::High,
        TaskPriority::Critical, TaskPriority::Low, TaskPriority::Normal};
    // task-2 and task-3 share an update time, so the id decides between them.
    const struct { const char* id; const char* project; std::int64_t updated; } layout[] = {
        {"task-1", "project-1", 3000}, {"task-2", "project-1", 2000}, {"task-3", "project-1", 2000},
        {"task-4", "project-1", 1000}, {"task-5", "project-2", 4000}, {"task-6", "project-2", 500}};
    std::vector<ProjectTask> expected;
    for (int i = 0; i < 6; ++i) {
        ProjectTask task = makeTask(layout[i].id, layout[i].project, layout[i].updated);
        task.status = statuses[i];
        task.priority = priorities[i];
        task.requiresAttention = (i % 2) == 0;
        if (i % 3 == 0) task.dueAt = at(7000 + i);
        if (i % 2 == 1) task.plannedRunAt = at(8000 + i);
        if (i == 3) { task.dueAt = at(7003); task.plannedRunAt = at(8003); }
        expected.push_back(task);
    }
    using Ids = std::vector<std::string>;
    {
        const auto database = workspace.open();
        addProject(database, workspace, "project-1");
        addProject(database, workspace, "project-2");
        SqliteTaskRepository tasks(database);
        for (int i = 5; i >= 0; --i) {
            const auto saved = tasks.insert(expected[i]);
            require(saved.success(), "Could not insert a task");
            requireSameTask(saved.value(), expected[i], "insert must return the task as written");
        }
        for (const auto& task : expected) requireStoredTask(tasks, task, "A task must round-trip every field");
        require(taskIds(tasks, std::nullopt) == Ids {"task-5", "task-1", "task-2", "task-3", "task-4", "task-6"},
            "Tasks must be listed by update time, newest first, and then id");
        require(taskIds(tasks, ProjectId("project-1")) == Ids {"task-1", "task-2", "task-3", "task-4"},
            "A project filter must list only that project's tasks in the same order");
        require(taskIds(tasks, ProjectId("project-2")) == Ids {"task-5", "task-6"},
            "A project filter must list only that project's tasks");
        require(taskIds(tasks, ProjectId("project-unknown")).empty(), "An unknown project must list no tasks");
        const auto missing = tasks.findById("task-missing");
        require(missing.success() && !missing.value().has_value(), "A missing task must read as absent");
    }

    const auto reopened = workspace.open();
    SqliteTaskRepository tasks(reopened);
    for (const auto& task : expected) requireStoredTask(tasks, task, "A task must survive reopening the database");
    require(taskIds(tasks, std::nullopt) == Ids {"task-5", "task-1", "task-2", "task-3", "task-4", "task-6"},
        "The listing order must survive reopening the database");
}

void testTaskWriteErrorsAndUpdate()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProject(database, workspace, "project-1");
    addProject(database, workspace, "project-2");
    SqliteTaskRepository tasks(database);
    ProjectTask original = makeTask("task-1", "project-1", 1000);
    original.dueAt = at(7000);
    original.plannedRunAt = at(8000);
    original.requiresAttention = true;
    require(tasks.insert(original).success(), "Could not insert a task");

    const auto orphan = tasks.insert(makeTask("task-2", "project-unknown", 2000));
    require(!orphan.success() && orphan.error()->code == "persistence.write_failed",
        "A task of an unknown project must be refused as a failed write");
    ProjectTask repeated = makeTask("task-1", "project-2", 3000);
    repeated.title = "Must not replace the first task";
    const auto again = tasks.insert(repeated);
    require(!again.success() && again.error()->code == "persistence.write_failed",
        "A repeated task id must be refused as a failed write");
    require(taskIds(tasks, std::nullopt) == std::vector<std::string> {"task-1"}, "Refused inserts must not add rows");
    requireStoredTask(tasks, original, "A refused insert altered an existing task");

    // update() replaces the editable fields and leaves the owning project and the creation time alone.
    ProjectTask changed = original;
    changed.title = "Renamed \xE6\xB8\xAC";
    changed.description = "New description";
    changed.status = TaskStatus::Blocked;
    changed.priority = TaskPriority::Critical;
    changed.requiresAttention = false;
    changed.dueAt = std::nullopt;
    changed.plannedRunAt = at(8500);
    changed.updatedAt = at(9000);
    ProjectTask attempt = changed;
    attempt.projectId = "project-2";
    attempt.createdAt = at(1);
    const auto updated = tasks.update(attempt);
    require(updated.success(), "Could not update a task");
    requireSameTask(updated.value(), attempt, "update must return the task as written");
    requireStoredTask(tasks, changed, "An update must replace the editable fields and keep project and creation time");

    const auto missing = tasks.update(makeTask("task-missing", "project-1", 1000));
    require(!missing.success() && missing.error()->code == "task.not_found",
        "Updating a missing task must report task.not_found");
    require(taskIds(tasks, std::nullopt) == std::vector<std::string> {"task-1"},
        "A refused update must not create a task");
    require(tasks.insert(makeTask("task-3", "project-2", 4000)).success(),
        "The connection must be usable after refused writes");
}

void testCorruptTaskRowsFailClosed()
{
    Workspace workspace;
    {
        const auto database = workspace.open();
        addProject(database, workspace, "project-good");
        addProject(database, workspace, "project-bad");
        SqliteTaskRepository tasks(database);
        require(tasks.insert(makeTask("task-good", "project-good", 1000)).success()
                && tasks.insert(makeTask("task-bad-status", "project-bad", 2000)).success()
                && tasks.insert(makeTask("task-bad-priority", "project-bad", 3000)).success(),
            "Could not insert tasks");
    }
    workspace.sql("PRAGMA ignore_check_constraints=1; "
                  "UPDATE project_tasks SET status='mystery' WHERE id='task-bad-status';");
    workspace.sql("PRAGMA ignore_check_constraints=1; "
                  "UPDATE project_tasks SET priority='urgent' WHERE id='task-bad-priority';");

    const auto database = workspace.open();
    SqliteTaskRepository tasks(database);
    const auto good = tasks.findById("task-good");
    require(good.success() && good.value().has_value(), "A valid task must stay readable");
    for (const char* id : {"task-bad-status", "task-bad-priority"}) {
        const auto bad = tasks.findById(id);
        require(!bad.success() && bad.error()->code == "persistence.invalid_data",
            "A task with an unknown status or priority must fail closed on findById");
    }
    const auto everything = tasks.list(std::nullopt);
    require(!everything.success() && everything.error()->code == "persistence.invalid_data",
        "A listing that contains an invalid task must fail rather than skip it");
    const auto onlyGood = tasks.list(ProjectId("project-good"));
    require(onlyGood.success() && onlyGood.value().size() == 1,
        "A listing filtered to a healthy project must still work");
}

void testTaskRunLinks()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProject(database, workspace, "project-1");
    SqliteTaskRepository tasks(database);
    require(tasks.insert(makeTask("task-1", "project-1", 1000)).success()
            && tasks.insert(makeTask("task-2", "project-1", 2000)).success(),
        "Could not insert tasks");
    for (const char* id : {"run-z", "run-a", "run-m"}) addRun(database, id, "project-1");

    const auto none = tasks.linkedRuns("task-1");
    require(none.success() && none.value().empty(), "A task without links must list no runs");
    const auto unknownTask = tasks.linkedRuns("task-unknown");
    require(unknownTask.success() && unknownTask.value().empty(), "An unknown task must list no runs");

    using Ids = std::vector<std::string>;
    // Links are listed in the order they were made, not by run id.
    require(tasks.linkRun("task-1", "run-z").success() && tasks.linkRun("task-1", "run-a").success()
            && tasks.linkRun("task-1", "run-m").success(),
        "Could not link runs");
    const auto linked = tasks.linkedRuns("task-1");
    require(linked.success() && linked.value() == Ids {"run-z", "run-a", "run-m"},
        "Linked runs must be listed in link order");

    // A run belongs to one task: linking it elsewhere moves it, and linking it again re-adds it last.
    require(tasks.linkRun("task-2", "run-a").success(), "Could not move a run to another task");
    const auto afterMove = tasks.linkedRuns("task-1");
    require(afterMove.success() && afterMove.value() == Ids {"run-z", "run-m"},
        "A run linked to another task must leave the first task");
    const auto second = tasks.linkedRuns("task-2");
    require(second.success() && second.value() == Ids {"run-a"}, "The second task must own the moved run");
    require(tasks.linkRun("task-1", "run-z").success(), "Could not link the same run again");
    const auto relinked = tasks.linkedRuns("task-1");
    require(relinked.success() && relinked.value() == Ids {"run-m", "run-z"},
        "Linking the same run again must keep a single link");

    const auto badTask = tasks.linkRun("task-unknown", "run-m");
    require(!badTask.success() && badTask.error()->code == "persistence.write_failed",
        "Linking an unknown task must be refused as a failed write");
    const auto badRun = tasks.linkRun("task-1", "run-unknown");
    require(!badRun.success() && badRun.error()->code == "persistence.write_failed",
        "Linking an unknown run must be refused as a failed write");
    const auto unchanged = tasks.linkedRuns("task-1");
    require(unchanged.success() && unchanged.value() == Ids {"run-m", "run-z"},
        "Refused links must not change the existing ones");
}
} // namespace

int main()
{
    try {
        testProjectRoundTripOrderingAndLookup();
        testProjectWriteErrorsAndUpdate();
        testCorruptProjectRowsFailClosed();
        testConcurrentProjectWritersShareOneConnection();
        testTaskRoundTripOrderingAndFilter();
        testTaskWriteErrorsAndUpdate();
        testCorruptTaskRowsFailClosed();
        testTaskRunLinks();
        std::cout << "SQLite Projects contracts: 8 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Projects contracts failed: " << error.what() << '\n';
        return 1;
    }
}
