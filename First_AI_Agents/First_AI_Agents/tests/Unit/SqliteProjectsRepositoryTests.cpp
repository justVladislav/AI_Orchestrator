#include "ai/infrastructure/SqliteRepositories.hpp"

#include "ProjectOutputApprovalRepositoryContract.hpp"

#include <sqlite3.h>

#include <algorithm>
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

void testWorkspaceActiveProject()
{
    Workspace workspace;
    const auto activeId = [](const SqliteWorkspaceStateRepository& state) -> std::optional<ProjectId> {
        const auto active = state.activeProjectId();
        require(active.success(), "Could not read the active project");
        return active.value();
    };
    {
        const auto database = workspace.open();
        addProject(database, workspace, "project-1");
        addProject(database, workspace, "project-2");
        SqliteWorkspaceStateRepository state(database);
        require(!activeId(state).has_value(), "A new workspace must have no active project");

        require(state.setActiveProjectId(ProjectId("project-1")).success(), "Could not select a project");
        require(activeId(state) == std::optional<ProjectId>("project-1"), "The selected project must be active");
        require(state.setActiveProjectId(ProjectId("project-2")).success(), "Could not switch the project");
        require(activeId(state) == std::optional<ProjectId>("project-2"), "Switching must replace the selection");

        // A refused selection keeps the previous one and leaves the connection usable.
        const auto unknown = state.setActiveProjectId(ProjectId("project-unknown"));
        require(!unknown.success() && unknown.error()->code == "workspace.project_not_found",
            "An unknown project must report workspace.project_not_found");
        require(activeId(state) == std::optional<ProjectId>("project-2"),
            "A refused selection must keep the previous active project");

        // The state is shared by every repository object on the same connection.
        SqliteWorkspaceStateRepository other(database);
        require(activeId(other) == std::optional<ProjectId>("project-2"), "A second repository must see the selection");

        require(state.setActiveProjectId(std::nullopt).success(), "Could not clear the selection");
        require(!activeId(state).has_value() && !activeId(other).has_value(), "Clearing must leave no active project");
        require(state.setActiveProjectId(ProjectId("project-1")).success(), "Could not select after clearing");
    }

    const auto reopened = workspace.open();
    SqliteWorkspaceStateRepository state(reopened);
    require(activeId(state) == std::optional<ProjectId>("project-1"), "The selection must survive reopening");
    require(state.setActiveProjectId(ProjectId("project-2")).success(), "The reopened state must accept a change");
    require(activeId(state) == std::optional<ProjectId>("project-2"), "The change after reopening must apply");
}

ProjectReadinessRecord makeReadiness(std::string projectId, ProjectReadinessState state)
{
    ProjectReadinessRecord value;
    value.projectId = std::move(projectId);
    value.projectRoot = "C:/work/" + value.projectId;
    value.state = state;
    value.checkedAt = at(10'000);
    return value;
}

// A record that satisfies the schema's rule for Ready: the whole proof is present.
ProjectReadinessRecord makeReadyReadiness(std::string projectId)
{
    ProjectReadinessRecord value = makeReadiness(std::move(projectId), ProjectReadinessState::Ready);
    value.configurePreset = "local";
    value.buildPreset = "local-build";
    value.binaryDir = "build/local";
    value.inputsFingerprint = std::string(64, 'a');
    value.toolchainIdentity = "msvc=14.51;sdk=10.0.26100.0";
    value.targets = {"all", "app", "\xE6\xB8\xAC\xE8\xA9\xA6"};
    value.targetsComplete = true;
    value.checkedAt = at(20'000);
    value.lastSuccessAt = at(20'000);
    return value;
}

void requireSameReadiness(const ProjectReadinessRecord& actual, const ProjectReadinessRecord& expected,
    const char* message)
{
    require(actual.projectId == expected.projectId && actual.projectRoot == expected.projectRoot
            && actual.configurePreset == expected.configurePreset && actual.buildPreset == expected.buildPreset
            && actual.binaryDir == expected.binaryDir && actual.state == expected.state
            && actual.inputsFingerprint == expected.inputsFingerprint
            && actual.toolchainIdentity == expected.toolchainIdentity && actual.targets == expected.targets
            && actual.targetsComplete == expected.targetsComplete && actual.checkedAt == expected.checkedAt
            && actual.lastSuccessAt == expected.lastSuccessAt && actual.failureCode == expected.failureCode
            && actual.failureMessage == expected.failureMessage,
        message);
}

void requireStoredReadiness(const SqliteProjectReadinessRepository& readiness, const ProjectReadinessRecord& expected,
    const char* message)
{
    const auto found = readiness.find(expected.projectId);
    require(found.success() && found.value().has_value(), message);
    requireSameReadiness(*found.value(), expected, message);
}

void testReadinessEveryStateRoundTripsAndProjectsAreIsolated()
{
    Workspace workspace;
    const ProjectReadinessRecord unverified = makeReadiness("project-1", ProjectReadinessState::Unverified);
    const ProjectReadinessRecord ready = makeReadyReadiness("project-2");
    ProjectReadinessRecord stale = makeReadyReadiness("project-3");
    stale.state = ProjectReadinessState::Stale;
    stale.targetsComplete = false;
    stale.targets.clear();
    stale.checkedAt = at(30'000);
    stale.lastSuccessAt = at(20'000);
    ProjectReadinessRecord failed = makeReadiness("project-4", ProjectReadinessState::Failed);
    failed.configurePreset = "local";
    failed.buildPreset = "local-build";
    failed.binaryDir = "build/local";
    failed.inputsFingerprint = std::string(64, 'b');
    failed.toolchainIdentity = "msvc=14.51";
    failed.checkedAt = at(40'000);
    failed.failureCode = "project_setup.configure_failed";
    failed.failureMessage = "LNK1104: cannot open \"file\"\nsecond line \xE6\xB8\xAC";
    const ProjectReadinessRecord* all[] = {&unverified, &ready, &stale, &failed};
    {
        const auto database = workspace.open();
        for (const auto* record : all) addProject(database, workspace, record->projectId);
        SqliteProjectReadinessRepository readiness(database);
        const auto none = readiness.find("project-1");
        require(none.success() && !none.value().has_value(), "A project starts with no readiness record");
        const auto unknown = readiness.find("project-unknown");
        require(unknown.success() && !unknown.value().has_value(), "An unknown project has no readiness record");

        for (const auto* record : all) require(readiness.upsert(*record).success(), "Could not store a readiness record");
        for (const auto* record : all) requireStoredReadiness(readiness, *record, "A readiness record must round-trip every field");

        // Upserting one project replaces only its own record, whatever state it moves through.
        ProjectReadinessRecord moved = ready;
        moved.state = ProjectReadinessState::Failed;
        moved.lastSuccessAt.reset();
        moved.failureCode = "project_setup.build_failed";
        moved.failureMessage = "C1083";
        moved.checkedAt = at(50'000);
        require(readiness.upsert(moved).success(), "Could not move a record to Failed");
        requireStoredReadiness(readiness, moved, "An upsert must replace the whole record");
        for (const auto* record : all) {
            if (record == &ready) continue;
            requireStoredReadiness(readiness, *record, "An upsert for one project changed another project's record");
        }
        require(readiness.upsert(ready).success(), "Could not move the record back to Ready");
        requireStoredReadiness(readiness, ready, "The record must be replaceable again, clearing the failure");
    }

    const auto reopened = workspace.open();
    SqliteProjectReadinessRepository readiness(reopened);
    for (const auto* record : all) requireStoredReadiness(readiness, *record, "Readiness must survive reopening the database");
}

void testReadinessRefusedWritesKeepTheStoredRecord()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProject(database, workspace, "project-1");
    SqliteProjectReadinessRepository readiness(database);
    const ProjectReadinessRecord ready = makeReadyReadiness("project-1");
    require(readiness.upsert(ready).success(), "Could not store a readiness record");

    const auto ghost = readiness.upsert(makeReadyReadiness("project-unknown"));
    require(!ghost.success() && ghost.error()->code == "project.not_found",
        "A record for an unknown project must report project.not_found");
    const auto absent = readiness.find("project-unknown");
    require(absent.success() && !absent.value().has_value(), "A refused record must not be stored");

    // The schema refuses a Ready record whose proof is incomplete.
    for (const char* missing : {"fingerprint", "toolchain", "preset", "success"}) {
        ProjectReadinessRecord unproven = ready;
        unproven.failureMessage = "Must not persist";
        if (std::string(missing) == "fingerprint") unproven.inputsFingerprint.clear();
        if (std::string(missing) == "toolchain") unproven.toolchainIdentity.clear();
        if (std::string(missing) == "preset") unproven.buildPreset.clear();
        if (std::string(missing) == "success") unproven.lastSuccessAt.reset();
        const auto refused = readiness.upsert(unproven);
        require(!refused.success() && refused.error()->code == "persistence.write_failed",
            "A Ready record without its full proof must be refused as a failed write");
        requireStoredReadiness(readiness, ready, "A refused upsert changed the stored record");
    }

    ProjectReadinessRecord next = ready;
    next.state = ProjectReadinessState::Stale;
    next.checkedAt = at(60'000);
    require(readiness.upsert(next).success(), "The connection must be usable after refused writes");
    requireStoredReadiness(readiness, next, "An upsert after refusals must apply in full");
}

void testReadinessCorruptRowsAreHandledAsPinned()
{
    Workspace workspace;
    {
        const auto database = workspace.open();
        for (const char* id : {"project-good", "project-bad-state", "project-bad-targets"}) {
            addProject(database, workspace, id);
        }
        SqliteProjectReadinessRepository readiness(database);
        require(readiness.upsert(makeReadyReadiness("project-good")).success()
                && readiness.upsert(makeReadyReadiness("project-bad-state")).success()
                && readiness.upsert(makeReadyReadiness("project-bad-targets")).success(),
            "Could not store readiness records");
    }
    workspace.sql("PRAGMA ignore_check_constraints=1; "
                  "UPDATE project_verification_readiness SET state='maybe' WHERE project_id='project-bad-state';");
    // Pinned as the current behaviour, not as a design goal: an unreadable target list reads as empty and
    // non-string entries are skipped.
    workspace.sql("UPDATE project_verification_readiness SET targets_json='not json' "
                  "WHERE project_id='project-bad-targets';");

    const auto database = workspace.open();
    SqliteProjectReadinessRepository readiness(database);
    requireStoredReadiness(readiness, makeReadyReadiness("project-good"), "A valid record must stay readable");
    const auto bad = readiness.find("project-bad-state");
    require(!bad.success() && bad.error()->code == "persistence.invalid_data",
        "An unknown stored state must fail closed");

    const auto garbled = readiness.find("project-bad-targets");
    require(garbled.success() && garbled.value().has_value() && garbled.value()->targets.empty(),
        "An unreadable target list currently reads as empty");
    workspace.sql("UPDATE project_verification_readiness SET targets_json='[\"a\",1,\"b\",null]' "
                  "WHERE project_id='project-bad-targets';");
    const auto mixed = readiness.find("project-bad-targets");
    require(mixed.success() && mixed.value().has_value()
            && mixed.value()->targets == std::vector<std::string> {"a", "b"},
        "Non-string target entries are currently skipped");
}
} // namespace

// ---- P3a: project output approvals (schema step 71) ----

struct ApprovalStore {
    Workspace workspace;
    std::shared_ptr<SqliteDatabase> database = workspace.open();
    SqliteProjectOutputApprovalRepository repository {database};
    std::vector<std::string> projects;

    void ensureProject(const std::string& id)
    {
        if (std::find(projects.begin(), projects.end(), id) != projects.end()) return;
        addProject(database, workspace, id);
        projects.push_back(id);
    }
};

output_approval_contract::Fixture sqliteApprovalFixture()
{
    auto store = std::make_shared<ApprovalStore>();
    return {store, &store->repository, [store](const std::string& id) { store->ensureProject(id); }};
}

void testOutputApprovalContractOnSqlite()
{
    output_approval_contract::testRoundTripIsolationAndRevoke(sqliteApprovalFixture);
    output_approval_contract::testIdempotenceLimitAndValidator(sqliteApprovalFixture);
    output_approval_contract::testNestingRuleBothOrders(sqliteApprovalFixture);
    output_approval_contract::testRepeatOfTheSameObjectAndIdConflicts(sqliteApprovalFixture);
    output_approval_contract::testValidatorFailureRollsBackAndReadsAreConsistent(sqliteApprovalFixture);
    {   // one connection, two threads
        ApprovalStore store;
        output_approval_contract::testConcurrentCommitsStayValid(store.repository, store.repository,
            [&store](const std::string& id) { store.ensureProject(id); }, 40);
    }
}

// A local mutex of one handle proves nothing about SQL atomicity: two CONNECTIONS to one file race here.
void testOutputApprovalTwoConnectionsToOneFile()
{
    Workspace workspace;
    auto first = workspace.open();
    auto second = workspace.open();
    SqliteProjectOutputApprovalRepository firstRepository(first);
    SqliteProjectOutputApprovalRepository secondRepository(second);
    std::vector<std::string> projects;
    output_approval_contract::testConcurrentCommitsStayValid(firstRepository, secondRepository,
        [&](const std::string& id) {
            if (std::find(projects.begin(), projects.end(), id) == projects.end()) {
                addProject(first, workspace, id);
                projects.push_back(id);
            }
        },
        40);
}

void testOutputApprovalStorageGuards()
{
    ApprovalStore store;
    store.ensureProject("p1");
    using output_approval_contract::makeApproval;

    // An unknown project is refused with its own code, not stored.
    const auto unknown = store.repository.commit(makeApproval("u", "ghost", "C:/ws", "Out"), nullptr);
    require(!unknown.success() && unknown.error()->code == "project.not_found", "An unknown project is refused");

    // Persists across a reopen.
    require(output_approval_contract::commitOk(store.repository, makeApproval("keep", "p1", "C:/ws", "Out")), "approve");
    {
        auto reopened = store.workspace.open();
        SqliteProjectOutputApprovalRepository again(reopened);
        const auto listed = again.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws"));
        require(listed.success() && listed.value().size() == 1 && listed.value().front().id == "keep", "An approval survives a reopen");
    }

    // The trigger is a net below the transaction: a raw ninth insert is refused by SQLite itself.
    for (int index = 1; index < 8; ++index) {
        require(output_approval_contract::commitOk(store.repository, makeApproval("t" + std::to_string(index), "p1", "C:/ws", "T" + std::to_string(index))), "fill");
    }
    const std::string ninth = "INSERT INTO project_output_approvals (id,project_id,workspace_root,workspace_key,relative_path,path_key,"
                              "inspection_complete,nested_sources_acknowledged,approved_by,approved_at_ms) "
                              "VALUES ('raw9','p1','C:/ws','" + outputApprovalWorkspaceKey("C:/ws") + "','Raw9','raw9',1,0,'',1);";
    bool refused = false;
    try { store.workspace.sql(ninth.c_str()); } catch (const std::runtime_error&) { refused = true; }
    require(refused, "SQLite's own trigger refuses a ninth row");
    require(store.repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 8, "...and the set is intact");

    // A row that breaks the table's own guarantees is an ERROR, never a shorter list.
    const std::string corruptSql = std::string("PRAGMA ignore_check_constraints = ON; DELETE FROM project_output_approvals WHERE id = 't1';"
                        "INSERT INTO project_output_approvals (id,project_id,workspace_root,workspace_key,relative_path,path_key,"
                        "inspection_complete,nested_sources_acknowledged,approved_by,approved_at_ms) "
                        "VALUES ('bad','p1','C:/ws','") + outputApprovalWorkspaceKey("C:/ws") + "','Bad','bad',2,0,'',1);";
    store.workspace.sql(corruptSql.c_str());
    const auto corrupt = store.repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws"));
    require(!corrupt.success() && corrupt.error()->code == kOutputApprovalStorageFailedCode, "A corrupt row fails the read");
    const auto corruptCommit = store.repository.commit(makeApproval("after", "p1", "C:/ws", "After"), nullptr);
    require(!corruptCommit.success(), "...and a commit over it fails closed instead of adding to a partial set");
    store.workspace.sql("DELETE FROM project_output_approvals WHERE id = 'bad';");
    require(store.repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).success(), "A repaired table reads again");

    // Deleting the project removes its approvals (and only its).
    store.ensureProject("p2");
    require(output_approval_contract::commitOk(store.repository, makeApproval("other", "p2", "C:/ws", "Out")), "p2 approves");
    store.workspace.sql("PRAGMA foreign_keys = ON; DELETE FROM projects WHERE id = 'p1';");
    require(store.repository.listForProject("p1").value().empty(), "A removed project takes its approvals with it");
    require(store.repository.listForProject("p2").value().size() == 1, "...and nobody else's");
}

std::string columnNamesOfApprovals(const Workspace& workspace)
{
    sqlite3* handle = nullptr;
    const auto path = workspace.databasePath.generic_u8string();
    require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, "PRAGMA table_info(project_output_approvals);", -1, &statement, nullptr) == SQLITE_OK, "prepare");
    std::string names;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        names += reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        names += ",";
    }
    sqlite3_finalize(statement);
    sqlite3_close(handle);
    return names;
}

long long numberOf(const Workspace& workspace, const char* sql)
{
    sqlite3* handle = nullptr;
    const auto path = workspace.databasePath.generic_u8string();
    require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK, "prepare");
    require(sqlite3_step(statement) == SQLITE_ROW, "row");
    const long long value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(handle);
    return value;
}

void testSchemaStep71IsRepeatableAndNeverDestroysData()
{
    using output_approval_contract::makeApproval;
    // A fresh database has the step's table, index and trigger in the documented shape.
    {
        Workspace workspace;
        { auto database = workspace.open(); }
        require(columnNamesOfApprovals(workspace)
                == "id,project_id,workspace_root,workspace_key,relative_path,path_key,inspection_complete,nested_sources_acknowledged,"
                   "approved_by,approved_at_ms,",
            "The table has the documented columns");
        require(numberOf(workspace, "PRAGMA user_version;") == 71, "A new database is at version 71");
        require(numberOf(workspace, "SELECT count(*) FROM sqlite_master WHERE name IN ('project_output_approvals_pair_idx','project_output_approvals_limit');") == 2,
            "...with its index and its trigger");
    }
    // Rewound to 70 WITH data: the step runs again over its own populated table and keeps every row; its trigger is recreated.
    {
        ApprovalStore store;
        store.ensureProject("p1");
        require(output_approval_contract::commitOk(store.repository, makeApproval("a", "p1", "C:/ws", "A")), "approve");
        require(output_approval_contract::commitOk(store.repository, makeApproval("b", "p1", "C:/ws", "B")), "approve");
        store.workspace.sql("DROP TRIGGER project_output_approvals_limit; PRAGMA user_version = 70;");
        auto again = SqliteDatabase::open(store.workspace.databasePath);
        require(again.success(), "Step 71 runs again over its own populated table");
        SqliteProjectOutputApprovalRepository repository(again.value());
        require(repository.listForWorkspace("p1", outputApprovalWorkspaceKey("C:/ws")).value().size() == 2, "...and keeps every row");
        require(numberOf(store.workspace, "SELECT count(*) FROM sqlite_master WHERE name = 'project_output_approvals_limit';") == 1,
            "...and its trigger is back");
        require(numberOf(store.workspace, "PRAGMA user_version;") == 71, "...at version 71");
    }
    // Something of that name that is not the table (or has another shape) refuses the step and survives it.
    {
        Workspace workspace;
        { auto database = workspace.open(); }
        workspace.sql("DROP TABLE project_output_approvals; CREATE TABLE project_output_approvals (x INTEGER); "
                      "INSERT INTO project_output_approvals VALUES (7); PRAGMA user_version = 70;");
        const auto refused = SqliteDatabase::open(workspace.databasePath);
        require(!refused.success() && refused.error()->code == "migration.v71_failed", "Another shape refuses the step");
        require(numberOf(workspace, "SELECT x FROM project_output_approvals;") == 7 && numberOf(workspace, "PRAGMA user_version;") == 70,
            "...and the foreign table and the version are untouched");
    }
    {
        Workspace workspace;
        { auto database = workspace.open(); }
        workspace.sql("DROP TABLE project_output_approvals; CREATE VIEW project_output_approvals AS SELECT 1 AS x; PRAGMA user_version = 70;");
        const auto refused = SqliteDatabase::open(workspace.databasePath);
        require(!refused.success() && refused.error()->code == "migration.v71_failed", "A view of that name refuses the step");
        require(numberOf(workspace, "SELECT count(*) FROM sqlite_master WHERE name = 'project_output_approvals' AND type = 'view';") == 1,
            "...and survives");
    }
    // R1: a table with the right COLUMN NAMES but not the right definition is refused, with its rows and its version untouched.
    {
        const std::string columns = "id TEXT, project_id TEXT, workspace_root TEXT, workspace_key TEXT, relative_path TEXT, path_key TEXT, "
                                    "inspection_complete INTEGER, nested_sources_acknowledged INTEGER, approved_by TEXT, approved_at_ms INTEGER";
        const std::string row = "INSERT INTO project_output_approvals VALUES ('r1','p1','C:/ws','c:/ws','Out','out',1,0,'',1);";
        const std::string fullChecks =
            "id TEXT PRIMARY KEY NOT NULL CHECK(length(CAST(id AS BLOB)) BETWEEN 1 AND 64), "
            "project_id TEXT NOT NULL %FK%, workspace_root TEXT NOT NULL CHECK(length(CAST(workspace_root AS BLOB)) BETWEEN 1 AND 2048), "
            "workspace_key TEXT NOT NULL CHECK(length(CAST(workspace_key AS BLOB)) BETWEEN 1 AND 2048), "
            "relative_path TEXT NOT NULL CHECK(length(CAST(relative_path AS BLOB)) BETWEEN 1 AND 512), "
            "path_key TEXT NOT NULL CHECK(length(CAST(path_key AS BLOB)) BETWEEN 1 AND 512), "
            "inspection_complete INTEGER NOT NULL CHECK(inspection_complete IN (0,1)), "
            "nested_sources_acknowledged INTEGER NOT NULL CHECK(nested_sources_acknowledged IN (0,1)), "
            "approved_by TEXT NOT NULL DEFAULT '' CHECK(length(CAST(approved_by AS BLOB)) <= 256), approved_at_ms INTEGER NOT NULL";
        const auto withForeignKey = [&](bool foreignKey, bool unique) {
            std::string text = fullChecks;
            text.replace(text.find("%FK%"), 4, foreignKey ? "REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE" : "");
            if (unique) text += ", UNIQUE(project_id, workspace_key, path_key)";
            return "CREATE TABLE project_output_approvals (" + text + ");";
        };
        struct Variant { const char* name; std::string setup; };
        const std::vector<Variant> variants = {
            {"the same column names without any constraint", "CREATE TABLE project_output_approvals (" + columns + ");"},
            {"no foreign key", withForeignKey(false, true)},
            {"no UNIQUE", withForeignKey(true, false)},
        };
        for (const auto& variant : variants) {
            Workspace workspace;
            { auto database = workspace.open(); }
            workspace.sql(("INSERT INTO projects(id,name,description,root_path,status,created_at_ms,updated_at_ms) "
                           "VALUES('p1','n','d','" + workspace.root.generic_string() + "/p1','active',1,1);"
                           "DROP TABLE project_output_approvals; " + variant.setup + row + "PRAGMA user_version = 70;").c_str());
            const auto refused = SqliteDatabase::open(workspace.databasePath);
            require(!refused.success() && refused.error()->code == "migration.v71_failed",
                (std::string("Step 71 refuses a table with ") + variant.name).c_str());
            require(numberOf(workspace, "PRAGMA user_version;") == 70, "...and the version stays 70");
            require(numberOf(workspace, "SELECT count(*) FROM project_output_approvals;") == 1, "...and its row is untouched");
            require(numberOf(workspace, "SELECT count(*) FROM pragma_foreign_key_list('project_output_approvals');")
                    == (std::string(variant.name) == "no UNIQUE" ? 1 : 0),
                "...and nothing was added to the table");
        }
        {   // an index of the step's name that is defined otherwise
            Workspace workspace;
            { auto database = workspace.open(); }
            workspace.sql("DROP INDEX project_output_approvals_pair_idx; "
                          "CREATE INDEX project_output_approvals_pair_idx ON project_output_approvals(path_key); PRAGMA user_version = 70;");
            const auto refused = SqliteDatabase::open(workspace.databasePath);
            require(!refused.success() && refused.error()->code == "migration.v71_failed", "An index of the step's name with another definition refuses it");
            require(numberOf(workspace, "SELECT count(*) FROM pragma_index_info('project_output_approvals_pair_idx');") == 1
                    && numberOf(workspace, "PRAGMA user_version;") == 70,
                "...and it stays as it was");
        }
    }
    // A database newer than the build is still refused.
    {
        Workspace workspace;
        { auto database = workspace.open(); }
        workspace.sql("PRAGMA user_version = 72;");
        const auto newer = SqliteDatabase::open(workspace.databasePath);
        require(!newer.success() && newer.error()->code == "migration.unsupported_version", "Version 72 is refused");
    }
}

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
        testWorkspaceActiveProject();
        testReadinessEveryStateRoundTripsAndProjectsAreIsolated();
        testReadinessRefusedWritesKeepTheStoredRecord();
        testReadinessCorruptRowsAreHandledAsPinned();
        testOutputApprovalContractOnSqlite();
        testOutputApprovalTwoConnectionsToOneFile();
        testOutputApprovalStorageGuards();
        testSchemaStep71IsRepeatableAndNeverDestroysData();
        std::cout << "SQLite Projects contracts: 16 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Projects contracts failed: " << error.what() << '\n';
        return 1;
    }
}
