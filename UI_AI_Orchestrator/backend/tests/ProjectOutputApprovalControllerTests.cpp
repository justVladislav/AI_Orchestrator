// P3d: the asynchronous Qt controller of the output-folder approvals, over the REAL ProjectOutputApprovalService with an inspector and a
// repository the tests can hold at a gate. The checks throw, so they stay active under NDEBUG. Every folder and every database here is
// a temporary one; no provider, no CMake and no user data is touched.
#include <ai/application/inmemory/InMemoryProjectOutputApprovalRepository.hpp>
#include <ai/application/inmemory/InMemoryRepositories.hpp>
#include <ai/application/workspace/ProjectOutputApprovalService.hpp>
#include <ai/application/workspace/ToolRegistry.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/ProjectContextController.hpp>
#include <ai/ui/ProjectOutputApprovalController.hpp>
#include "../../test_support/QtTestSupport.hpp"

#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ai::application;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string utf8Of(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

bool waitFor(const char* label, const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 10s)
{
    return ai::ui::test_support::waitForCondition(label, predicate, [] { return std::string(); }, timeout);
}

// ---- temporary folders ----

class TempFolder final {
public:
    TempFolder()
    {
        static std::atomic<int> counter {0};
        path_ = fs::temp_directory_path()
            / ("aio-p3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
                + std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_);
        path_ = fs::canonical(path_);
    }
    ~TempFolder()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

    void touch(const fs::path& relative, const std::string& content = "x") const
    {
        const fs::path absolute = path_ / relative;
        fs::create_directories(absolute.parent_path());
        std::ofstream out(absolute, std::ios::binary | std::ios::trunc);
        out << content;
    }

private:
    fs::path path_;
};

// A directory link: a junction on Windows, a symlink elsewhere. False when none can be made.
bool makeDirectoryLink(const fs::path& link, const fs::path& target)
{
#ifdef _WIN32
    const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    return std::system(command.c_str()) == 0 && fs::exists(link);
#else
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    return !error;
#endif
}

// ---- the doubles the real service runs on ----

// The real inspector, optionally held at a gate that opens on release OR on the stop request, optionally deaf to a stop request
// (a slow OS call that cannot be interrupted), optionally throwing.
class GatedInspector final : public IOutputDirectoryInspector {
public:
    std::atomic<bool> gated {false};
    std::atomic<bool> deaf {false};
    std::atomic<bool> throws {false};
    std::atomic<int> force {0};   // 1: the real walk is reported Incomplete, 2: it is reported Failed
    std::atomic<int> entered {0};

    void release()
    {
        std::lock_guard lock(mutex_);
        open_ = true;
        condition_.notify_all();
    }
    void rearm()
    {
        std::lock_guard lock(mutex_);
        open_ = false;
    }

    [[nodiscard]] OutputDirectoryInspection inspect(const fs::path& directory, const OutputDirectoryInspectionLimits& limits,
        std::stop_token stopToken) const override
    {
        ++const_cast<GatedInspector*>(this)->entered;
        if (throws) throw std::runtime_error("the inspector blew up");
        if (gated) {
            std::stop_callback wake(stopToken, [this] {
                std::lock_guard lock(mutex_);
                condition_.notify_all();
            });
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return open_ || (!deaf && stopToken.stop_requested()); });
            if (!open_ && stopToken.stop_requested()) {
                OutputDirectoryInspection cancelled;
                cancelled.status = OutputInspectionStatus::Cancelled;
                return cancelled;
            }
        }
        OutputDirectoryInspection result = real_.inspect(directory, limits, deaf ? std::stop_token {} : stopToken);   // a deaf inspector never sees the request
        if (force == 1) {
            result.status = OutputInspectionStatus::Incomplete;
            result.hitEntryLimit = true;
        } else if (force == 2) {
            result.status = OutputInspectionStatus::Failed;
            result.failure = "forced by the test";
        }
        return result;
    }

private:
    FilesystemOutputDirectoryInspector real_;
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    bool open_ {false};
};

// The in-memory repository, observed: which threads called it, and a gate inside commit() (the boundary a cancel must not cross).
class ObservedApprovals final : public IProjectOutputApprovalRepository {
public:
    InMemoryProjectOutputApprovalRepository inner;
    std::atomic<bool> commitGated {false};
    std::atomic<int> commitEntered {0};

    void releaseCommit()
    {
        std::lock_guard lock(mutex_);
        commitOpen_ = true;
        condition_.notify_all();
    }
    [[nodiscard]] std::vector<std::thread::id> threads() const
    {
        std::lock_guard lock(mutex_);
        return threads_;
    }

    ai::common::Result<ProjectOutputApprovalCommitResult> commit(
        const ProjectOutputApproval& candidate, const ProjectOutputApprovalSetValidator& validator) override
    {
        note();
        ++commitEntered;
        if (commitGated) {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return commitOpen_; });
        }
        return inner.commit(candidate, validator);
    }
    ai::common::Result<std::vector<ProjectOutputApproval>> listForWorkspace(
        const ProjectId& projectId, const std::string& workspaceKey) const override
    {
        note();
        return inner.listForWorkspace(projectId, workspaceKey);
    }
    ai::common::Result<std::vector<ProjectOutputApproval>> listForProject(const ProjectId& projectId) const override
    {
        note();
        return inner.listForProject(projectId);
    }
    ai::common::Result<bool> revoke(const ProjectId& projectId, const std::string& approvalId) override
    {
        note();
        return inner.revoke(projectId, approvalId);
    }

private:
    void note() const
    {
        std::lock_guard lock(mutex_);
        threads_.push_back(std::this_thread::get_id());
    }
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::vector<std::thread::id> threads_;
    bool commitOpen_ {false};
};

// The in-memory project repository, with a gate on ONE chosen read: the n-th findById after arm() waits until released. The service
// reads the project twice in an approve - before the walk and again just before the commit - and the second read is the one a
// cancel must be able to reach.
class GatedProjects final : public IProjectRepository {
public:
    InMemoryProjectRepository inner;
    std::atomic<int> entered {0};

    void arm(int readNumber)
    {
        std::lock_guard lock(mutex_);
        reads_ = 0;
        gateAt_ = readNumber;
        open_ = false;
        entered = 0;
    }
    void release()
    {
        std::lock_guard lock(mutex_);
        open_ = true;
        condition_.notify_all();
    }

    ai::common::Result<Project> insert(Project project) override { return inner.insert(std::move(project)); }
    ai::common::Result<Project> update(Project project) override { return inner.update(std::move(project)); }
    [[nodiscard]] ai::common::Result<std::optional<Project>> findById(const ProjectId& id) const override
    {
        {
            std::unique_lock lock(mutex_);
            if (gateAt_ != 0 && ++reads_ == gateAt_) {
                ++const_cast<GatedProjects*>(this)->entered;
                condition_.wait(lock, [&] { return open_; });
            }
        }
        return inner.findById(id);
    }
    [[nodiscard]] ai::common::Result<std::optional<Project>> findByRootPath(const fs::path& rootPath) const override
    {
        return inner.findByRootPath(rootPath);
    }
    [[nodiscard]] ai::common::Result<std::vector<Project>> listAll() const override { return inner.listAll(); }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable int reads_ {0};
    int gateAt_ {0};
    bool open_ {false};
};

struct Rig {
    TempFolder root;
    GatedProjects projects;
    ObservedApprovals approvals;
    GatedInspector inspector;
    std::vector<ProtectedStoragePath> extraProtected;
    FunctionProtectedStoragePathSource protectedPaths {[this](const fs::path&) {
        return ai::common::Result<std::vector<ProtectedStoragePath>>::ok(extraProtected);
    }};
    ProjectOutputApprovalService service;

    explicit Rig(OutputDirectoryInspectionLimits limits = {})
        : service(projects, approvals, protectedPaths, inspector, limits)
    {
        addProject("p1", root.path());
    }

    void addProject(const std::string& id, const fs::path& rootPath, ProjectStatus status = ProjectStatus::Active)
    {
        Project project;
        project.id = id;
        project.name = "Project " + id;
        project.description = "P3d fixture";
        project.rootPath = rootPath;
        project.status = status;
        require(projects.insert(project).success(), "setup: project " + id);
    }
    void setProject(const std::string& id, const fs::path& rootPath, ProjectStatus status)
    {
        auto project = projects.findById(id).value().value();
        project.rootPath = rootPath;
        project.status = status;
        require(projects.update(project).success(), "setup: update project " + id);
    }
};

// What the controller announced.
struct Terminal {
    QString operation;
    quint64 id {0};
    QString project;
    QString subject;
    QVariantMap map;
};

class Seen final {
public:
    explicit Seen(ProjectOutputApprovalController& controller)
    {
        using C = ProjectOutputApprovalController;
        const auto sub = [this](const char* op) {
            return [this, op](quint64 id, const QString& project, const QString& subject, const QVariantMap& map) {
                terminals.push_back({QString::fromLatin1(op), id, project, subject, map});
            };
        };
        QObject::connect(&controller, &C::previewFinished, &controller, sub("preview"));
        QObject::connect(&controller, &C::approveFinished, &controller, sub("approve"));
        QObject::connect(&controller, &C::recheckFinished, &controller, sub("recheck"));
        QObject::connect(&controller, &C::revokeFinished, &controller, sub("revoke"));
        QObject::connect(&controller, &C::listFinished, &controller,
            [this](quint64 id, const QString& project, const QVariantMap& map) {
                terminals.push_back({QStringLiteral("list"), id, project, QString(), map});
            });
        QObject::connect(&controller, &C::approvalsChanged, &controller, [this](const QString& project) { changed.push_back(project); });
        QObject::connect(&controller, &C::busyChanged, &controller, [this] { ++busyChanges; });
    }
    std::vector<Terminal> terminals;
    std::vector<QString> changed;
    int busyChanges {0};
};

bool ok(const QVariantMap& map) { return map.value(QStringLiteral("success")).toBool(); }
QString code(const QVariantMap& map) { return map.value(QStringLiteral("errorCode")).toString(); }
bool flag(const QVariantMap& map, const char* key) { return map.value(QString::fromLatin1(key)).toBool(); }

const Terminal& awaitTerminal(Seen& seen, std::size_t count, const char* label)
{
    require(waitFor(label, [&] { return seen.terminals.size() >= count; }), std::string("timeout: ") + label);
    return seen.terminals[count - 1];
}

QString osUserName()
{
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) user = qEnvironmentVariable("USER");
    return user;
}

// A preview of `path`, awaited: returns its request id.
quint64 previewOf(ProjectOutputApprovalController& controller, Seen& seen, const QString& project, const QString& path)
{
    const std::size_t before = seen.terminals.size();
    const quint64 id = controller.previewApproval(project, path);
    require(id != 0, "the preview was accepted: " + utf8Of(controller.lastErrorCode()));
    const Terminal& done = awaitTerminal(seen, before + 1, "a preview finishes");
    require(done.operation == QLatin1String("preview") && done.id == id, "...and is the one asked for");
    return id;
}

// ---- tests ----

// All five operations run on a worker; ids come back at once; one operation at a time; a second request is refused (0) without
// harming the first, and a request for another project asks the first inspection to stop but starts nothing.
void testOperationsRunOnWorkersAndTheStateStaysConsistent()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    require(controller.available() && !controller.busy() && !controller.inspecting() && controller.activeRequestId() == 0,
        "idle at the start");

    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(2);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
    heartbeat.start();

    rig.inspector.gated = true;
    const quint64 first = controller.previewApproval(QStringLiteral("p1"), QStringLiteral("Out2"));
    require(first != 0, "an accepted call returns an id at once");
    require(controller.busy() && controller.inspecting() && controller.activeRequestId() == first
            && controller.activeProjectId() == QLatin1String("p1") && controller.activeOperation() == QLatin1String("preview"),
        "busy / inspecting / the active identity are consistent while the worker runs");
    require(waitFor("the worker reaches the gate", [&] { return rig.inspector.entered.load() == 1; }),
        "the worker is inside the inspector");
    const int before = heartbeats;
    require(waitFor("the GUI keeps running", [&] { return heartbeats >= before + 5; }), "the GUI thread is not blocked by the worker");
    require(seen.terminals.empty(), "...and the operation has not finished");

    // A repeat while busy is refused with a reason and changes nothing of the first request.
    require(controller.previewApproval(QStringLiteral("p1"), QStringLiteral("Other")) == 0
            && controller.lastErrorCode() == QLatin1String("output_approval.busy"),
        "a second preview is refused (busy)");
    require(controller.listApprovals(QStringLiteral("p1")) == 0 && controller.revokeApproval(QStringLiteral("p1"), QStringLiteral("x")) == 0
            && controller.recheckApproval(QStringLiteral("p1"), QStringLiteral("x")) == 0,
        "...and so is every other operation");
    require(controller.activeRequestId() == first && controller.activeProjectId() == QLatin1String("p1") && seen.terminals.empty()
            && rig.inspector.entered.load() == 1,
        "the first request is untouched and no second worker started");

    // Another project's request asks the running inspection to stop - and still starts nothing.
    require(controller.previewApproval(QStringLiteral("p2"), QStringLiteral("Out2")) == 0, "a request for another project is refused");
    const Terminal& cancelled = awaitTerminal(seen, 1, "the first inspection ends after the request for another project");
    require(cancelled.operation == QLatin1String("preview") && cancelled.id == first && cancelled.project == QLatin1String("p1")
            && cancelled.subject == QLatin1String("Out2") && !ok(cancelled.map) && flag(cancelled.map, "cancelled")
            && flag(cancelled.map, "stale"),
        "...which ends cancelled and stale, with its own id, project and path");
    require(!controller.busy() && !controller.inspecting() && controller.activeRequestId() == 0 && rig.inspector.entered.load() == 1,
        "busy is cleared only when the worker has ended; still just one worker ever ran");
    require(seen.terminals.size() == 1, "exactly one terminal signal for the accepted id");
    heartbeat.stop();
}

// An approve needs a preview the controller itself kept and that is still current; the root and the path of the request come from the
// backend, the author is the operating-system user, and a spent preview is spent.
void testApproveIsBoundToTheBackendPreview()
{
    Rig rig;
    const fs::path nested = fs::path(u"Сборка") / fs::path(u"выход");
    rig.root.touch(nested / "gen.txt");
    const QString path = QString::fromUtf16(u"Сборка/выход");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    const QString p1 = QStringLiteral("p1");
    const auto refusedStale = [&](quint64 id, const char* what) {
        require(id == 0 && controller.lastErrorCode() == QLatin1String("output_approval.preview_stale"), std::string("refused as stale: ") + what);
    };

    refusedStale(controller.approve(p1, path, 0, false, false), "no preview at all");
    refusedStale(controller.approve(p1, path, 777, false, false), "an unknown preview id");

    const quint64 first = previewOf(controller, seen, p1, path);
    const QVariantMap& view = seen.terminals.back().map;
    require(ok(view) && flag(view, "exists") && view.value(QStringLiteral("relativePath")).toString() == path
            && view.value(QStringLiteral("files")).toULongLong() == 1 && !flag(view, "requiresNestedConfirmation")
            && !flag(view, "requiresIncompleteConfirmation") && !flag(view, "alreadyApproved")
            && view.value(QStringLiteral("inspectionStatus")).toString() == QLatin1String("complete") && !flag(view, "countsAreLowerBounds")
            && view.value(QStringLiteral("workspaceRoot")).toString() == QString::fromUtf8(rig.root.path().generic_u8string().c_str()),
        "the preview carries the backend's facts, Unicode intact");
    refusedStale(controller.approve(p1, QStringLiteral("Other"), first, false, false), "another path");
    refusedStale(controller.approve(QStringLiteral("p2"), path, first, false, false), "another project");

    controller.invalidatePreview();
    refusedStale(controller.approve(p1, path, first, false, false), "an invalidated preview");
    const quint64 second = previewOf(controller, seen, p1, path);
    controller.setProjectContext(QStringLiteral("other"));
    refusedStale(controller.approve(p1, path, second, false, false), "a changed project context");
    controller.setProjectContext(p1);
    const quint64 third = previewOf(controller, seen, p1, path);
    refusedStale(controller.approve(p1, path, second, false, false), "an older preview after a newer one");
    require(!controller.cancelInspection(), "nothing runs, so there is nothing to cancel");

    const std::size_t terminalsBefore = seen.terminals.size();
    const quint64 id = controller.approve(p1, path, third, false, false);
    require(id != 0, "the approve bound to the current preview is accepted");
    const Terminal& done = awaitTerminal(seen, terminalsBefore + 1, "the approval finishes");
    require(done.operation == QLatin1String("approve") && done.id == id && done.project == p1 && done.subject == path && ok(done.map)
            && flag(done.map, "changed") && !flag(done.map, "alreadyApproved")
            && done.map.value(QStringLiteral("outcome")).toString() == QLatin1String("inserted")
            && done.map.value(QStringLiteral("approvedBy")).toString() == osUserName()
            && done.map.value(QStringLiteral("relativePath")).toString() == path && !flag(done.map, "stale"),
        "inserted, with the operating-system user as the author");
    require(seen.changed.size() == 1 && seen.changed.front() == p1, "approvalsChanged names the project, once");
    refusedStale(controller.approve(p1, path, third, false, false), "a spent preview");
    const qlonglong approvedAt = done.map.value(QStringLiteral("approvedAtMs")).toLongLong();

    // The same folder again: the stored record comes back unchanged and nothing "changed".
    const quint64 fourth = previewOf(controller, seen, p1, path);
    require(flag(seen.terminals.back().map, "alreadyApproved"), "the preview says it is already approved");
    const std::size_t afterPreview = seen.terminals.size();
    const quint64 again = controller.approve(p1, path, fourth, false, false);
    require(again != 0, "accepted");
    const Terminal& repeat = awaitTerminal(seen, afterPreview + 1, "the repeated approval finishes");
    require(ok(repeat.map) && flag(repeat.map, "alreadyApproved") && !flag(repeat.map, "changed")
            && repeat.map.value(QStringLiteral("approvedAtMs")).toLongLong() == approvedAt && seen.changed.size() == 1,
        "AlreadyApproved: the same record, no time change, no approvalsChanged");
}

// The two confirmations are independent and come from the caller's action; the hard refusals are not bypassed by anything.
void testConfirmationsAreIndependentAndHardRefusalsStay()
{
    {
        Rig rig;
        rig.root.touch("gen/sub/CMakeLists.txt", "project(x)");
        rig.root.touch("gen/out.o");
        ProjectOutputApprovalController controller(&rig.service);
        Seen seen(controller);
        const QString p1 = QStringLiteral("p1");
        const quint64 preview = previewOf(controller, seen, p1, QStringLiteral("gen"));
        const QVariantMap view = seen.terminals.back().map;
        require(ok(view) && flag(view, "requiresNestedConfirmation") && !flag(view, "requiresIncompleteConfirmation")
                && view.value(QStringLiteral("nestedSourceCount")).toULongLong() == 1
                && view.value(QStringLiteral("nestedSourceSamples")).toStringList() == QStringList {QStringLiteral("sub/CMakeLists.txt")},
            "nested sources are shown with their count and samples");
        std::size_t seenTerminals = seen.terminals.size();
        require(controller.approve(p1, QStringLiteral("gen"), preview, false, false) != 0, "accepted");
        const Terminal& refused = awaitTerminal(seen, seenTerminals + 1, "the unconfirmed approve ends");
        require(!ok(refused.map) && code(refused.map) == QLatin1String("output_dir.nested_sources_unconfirmed"), "refused without the confirmation");
        require(seen.changed.empty(), "nothing was written");
        // The preview survives a refusal: the user confirms and tries again without a second inspection.
        seenTerminals = seen.terminals.size();
        require(controller.approve(p1, QStringLiteral("gen"), preview, /*nested=*/true, false) != 0, "the same preview is still usable");
        const Terminal& written = awaitTerminal(seen, seenTerminals + 1, "the confirmed approve ends");
        require(ok(written.map) && flag(written.map, "nestedSourcesAcknowledged") && flag(written.map, "inspectionComplete"),
            "written with the nested confirmation recorded");
    }
    {
        OutputDirectoryInspectionLimits limits;
        limits.maxEntries = 3;
        Rig rig(limits);
        for (int i = 0; i < 10; ++i) rig.root.touch("big/f" + std::to_string(i) + ".o");
        rig.root.touch("big/sub/CMakeLists.txt", "project(x)");
        ProjectOutputApprovalController controller(&rig.service);
        Seen seen(controller);
        const QString p1 = QStringLiteral("p1");
        const quint64 preview = previewOf(controller, seen, p1, QStringLiteral("big"));
        const QVariantMap view = seen.terminals.back().map;
        require(flag(view, "requiresIncompleteConfirmation") && flag(view, "countsAreLowerBounds") && flag(view, "hitEntryLimit")
                && view.value(QStringLiteral("inspectionStatus")).toString() == QLatin1String("incomplete")
                && !flag(view, "inspectionComplete"),
            "an incomplete walk is reported as lower bounds");
        std::size_t n = seen.terminals.size();
        require(controller.approve(p1, QStringLiteral("big"), preview, true, false) != 0, "accepted");
        require(code(awaitTerminal(seen, n + 1, "needs the other confirmation").map) == QLatin1String("output_dir.inspection_incomplete"),
            "the nested confirmation does not stand in for the incomplete one");
        n = seen.terminals.size();
        const bool nestedNeeded = flag(view, "requiresNestedConfirmation");
        require(controller.approve(p1, QStringLiteral("big"), preview, nestedNeeded, true) != 0, "accepted");
        const Terminal& written = awaitTerminal(seen, n + 1, "the confirmed approve ends");
        require(ok(written.map) && !flag(written.map, "inspectionComplete"), ("written, with the incompleteness recorded: " + utf8Of(code(written.map))));
    }
    {
        Rig rig;
        rig.root.touch("src2/CMakeLists.txt", "project(x)");
        rig.root.touch("state/db.sqlite");
        rig.root.touch("real/x.o");
        rig.extraProtected.push_back({rig.root.path() / "state", true});
        ProjectOutputApprovalController controller(&rig.service);
        Seen seen(controller);
        const QString p1 = QStringLiteral("p1");
        previewOf(controller, seen, p1, QStringLiteral("src2"));
        require(code(seen.terminals.back().map) == QLatin1String("output_dir.contains_cmake_sources"), "a root CMakeLists.txt is refused");
        previewOf(controller, seen, p1, QStringLiteral("state"));
        require(code(seen.terminals.back().map) == QLatin1String("output_approval.protected_storage"), "the application's storage is refused");
        previewOf(controller, seen, p1, QStringLiteral("../escape"));
        require(!ok(seen.terminals.back().map), "a path leaving the root is refused");
        const quint64 refusedPreview = seen.terminals[seen.terminals.size() - 3].id;
        require(controller.approve(p1, QStringLiteral("src2"), refusedPreview, true, true) == 0, "a refused preview is no authority to approve");
        // A link is never followed.
        const fs::path elsewhere = rig.root.path() / "elsewhere";
        fs::create_directories(elsewhere);
        if (makeDirectoryLink(rig.root.path() / "linked", elsewhere)) {
            previewOf(controller, seen, p1, QStringLiteral("linked"));
            require(code(seen.terminals.back().map) == QLatin1String("output_approval.reparse_point"), "a link is refused");
        } else {
            std::cout << "NOTE: no directory link could be made here; the link refusal was not exercised" << std::endl;
        }
    }
}

// Cancel before the commit writes nothing; a late read-only result never becomes the preview an approve may use; the destructor
// stops a running worker and waits for it.
void testCancelWritesNothingAndLateResultsNeverBecomeThePreview()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    const QString p1 = QStringLiteral("p1");

    // Cancel during the inspector of a preview.
    rig.inspector.gated = true;
    const quint64 id = controller.previewApproval(p1, QStringLiteral("Out2"));
    require(id != 0 && waitFor("the worker is at the gate", [&] { return rig.inspector.entered.load() == 1; }), "a preview at the gate");
    require(controller.cancelInspection() && !controller.cancelInspection(), "cancel is accepted once");
    require(controller.busy(), "busy stays until the worker has really ended or is told so");
    const Terminal& cancelled = awaitTerminal(seen, 1, "the cancelled preview ends");
    require(flag(cancelled.map, "cancelled") && !ok(cancelled.map) && flag(cancelled.map, "cancelRequested") && cancelled.id == id,
        "cancelled, with its own id");
    require(controller.approve(p1, QStringLiteral("Out2"), id, false, false) == 0, "a cancelled preview is no authority");

    // A late SUCCESSFUL read-only result (an inspector that cannot be interrupted) is announced as stale and is not kept.
    rig.inspector.rearm();
    rig.inspector.deaf = true;
    const quint64 late = controller.previewApproval(p1, QStringLiteral("Out2"));
    require(late != 0 && waitFor("the deaf worker is at the gate", [&] { return rig.inspector.entered.load() == 2; }), "a deaf preview at the gate");
    controller.setProjectContext(QStringLiteral("other-project"));
    rig.inspector.release();
    const Terminal& lateDone = awaitTerminal(seen, 2, "the late preview ends");
    require(ok(lateDone.map) && flag(lateDone.map, "stale") && lateDone.id == late && lateDone.project == p1,
        "the late result keeps its identity and says it is stale");
    require(controller.approve(p1, QStringLiteral("Out2"), late, false, false) == 0 && controller.lastErrorCode() == QLatin1String("output_approval.preview_stale"),
        "...and it never became the preview an approve may use (asked before anything else could reset it)");
    controller.setProjectContext(p1);

    // Cancel during the inspector of an approve: nothing is written.
    rig.inspector.gated = false;
    rig.inspector.deaf = false;
    const quint64 good = previewOf(controller, seen, p1, QStringLiteral("Out2"));
    rig.inspector.rearm();
    rig.inspector.gated = true;
    const int enteredBefore = rig.inspector.entered.load();
    const quint64 approving = controller.approve(p1, QStringLiteral("Out2"), good, false, false);
    require(approving != 0 && waitFor("the approve is at the gate", [&] { return rig.inspector.entered.load() == enteredBefore + 1; }),
        "an approve at the gate");
    require(controller.cancelInspection(), "cancel the approve");
    const Terminal& approveCancelled = awaitTerminal(seen, 4, "the cancelled approve ends");
    require(approveCancelled.operation == QLatin1String("approve") && flag(approveCancelled.map, "cancelled") && seen.changed.empty(),
        "cancelled, and no approvalsChanged");
    require(rig.approvals.inner.listForProject("p1").value().empty(), "no record was written");
    require(controller.approve(p1, QStringLiteral("Out2"), good, false, false) == 0, "a cancelled approve spent its preview");
}

void testTheDestructorStopsAndWaitsForTheWorker()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    rig.inspector.gated = true;
    auto controller = std::make_unique<ProjectOutputApprovalController>(&rig.service);
    require(controller->previewApproval(QStringLiteral("p1"), QStringLiteral("Out2")) != 0, "a preview at the gate");
    require(waitFor("the worker is at the gate", [&] { return rig.inspector.entered.load() == 1; }), "inside the inspector");
    controller.reset();   // request_stop, then wait: the gate opens on the stop, with no GUI timer involved
    require(rig.inspector.entered.load() == 1, "the worker ended and nothing else ran");
}

// The commit boundary: once the atomic commit has begun, a cancel or another selection changes nothing - the record that was written is
// reported as it is, for the project it was started for.
void testALateCancelNeverHidesARecordThatWasWritten()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    const QString p1 = QStringLiteral("p1");
    const quint64 preview = previewOf(controller, seen, p1, QStringLiteral("Out2"));
    rig.approvals.commitGated = true;
    const quint64 id = controller.approve(p1, QStringLiteral("Out2"), preview, false, false);
    require(id != 0 && waitFor("the commit has begun", [&] { return rig.approvals.commitEntered.load() == 1; }), "inside the commit");
    controller.cancelInspection();
    controller.setProjectContext(QStringLiteral("p2"));
    rig.approvals.releaseCommit();
    const Terminal& done = awaitTerminal(seen, 2, "the approve ends");
    require(done.id == id && done.project == p1 && ok(done.map) && flag(done.map, "changed")
            && done.map.value(QStringLiteral("outcome")).toString() == QLatin1String("inserted"),
        "the written record is reported, not 'cancelled'");
    require(flag(done.map, "stale") && flag(done.map, "cancelRequested"), "...marked stale so a future panel can filter it");
    require(seen.changed.size() == 1 && seen.changed.front() == p1, "approvalsChanged names the ORIGINAL project");
    require(rig.approvals.inner.listForProject("p1").value().size() == 1, "and the record is there");
}

// A cancel that arrives while the service reads the project again - after its last earlier check, before the commit - writes nothing.
void testACancelDuringTheSecondProjectReadWritesNothing()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    const QString p1 = QStringLiteral("p1");
    const quint64 preview = previewOf(controller, seen, p1, QStringLiteral("Out2"));
    rig.projects.arm(2);   // the first read opens the approve, the second one is just before the commit
    const quint64 id = controller.approve(p1, QStringLiteral("Out2"), preview, false, false);
    require(id != 0 && waitFor("the approve waits at the second project read", [&] { return rig.projects.entered.load() == 1; }),
        "an approve held at the second read of the project");
    require(rig.approvals.commitEntered.load() == 0, "the commit has not begun");
    require(controller.cancelInspection(), "the GUI cancels");
    rig.projects.release();
    const Terminal& done = awaitTerminal(seen, 2, "the cancelled approve ends");
    require(done.operation == QLatin1String("approve") && done.id == id && !ok(done.map) && flag(done.map, "cancelled")
            && code(done.map) == QLatin1String("output_approval.cancelled"),
        "it ends cancelled, with its own id");
    require(rig.approvals.commitEntered.load() == 0 && rig.approvals.inner.listForProject("p1").value().empty() && seen.changed.empty(),
        "the commit was never entered, no row exists and approvalsChanged was not emitted");
    require(controller.approve(p1, QStringLiteral("Out2"), preview, false, false) == 0, "the cancelled approve spent its preview");
}

// recheck: the top level is TODAY's inspection, the stored record is its own thing and is never rewritten.
void testRecheckKeepsTheStoredRecordApartFromTodaysInspection()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    rig.root.touch("Out3/b.txt");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    const QString p1 = QStringLiteral("p1");
    const auto approveFolder = [&](const QString& folder, bool acknowledgeIncomplete) {
        const quint64 preview = previewOf(controller, seen, p1, folder);
        const std::size_t n = seen.terminals.size();
        require(controller.approve(p1, folder, preview, false, acknowledgeIncomplete) != 0, "approve accepted");
        const Terminal& done = awaitTerminal(seen, n + 1, "approve ends");
        require(ok(done.map), "approved: " + utf8Of(code(done.map)));
        return done.map.value(QStringLiteral("approvalId")).toString();
    };
    const auto recheck = [&](const QString& approvalId) {
        const std::size_t n = seen.terminals.size();
        require(controller.recheckApproval(p1, approvalId) != 0, "recheck accepted");
        return awaitTerminal(seen, n + 1, "recheck ends").map;
    };
    const auto storedRows = [&] {
        const std::size_t n = seen.terminals.size();
        controller.listApprovals(p1);
        return awaitTerminal(seen, n + 1, "list ends").map.value(QStringLiteral("approvals")).toList();
    };

    // A COMPLETE approval, a walk that is INCOMPLETE today.
    const QString complete = approveFolder(QStringLiteral("Out2"), false);
    const QVariantList before = storedRows();
    require(before.size() == 1 && flag(before.front().toMap(), "inspectionComplete"), "the stored approval is complete");
    rig.inspector.force = 1;
    const QVariantMap nowIncomplete = recheck(complete);
    require(ok(nowIncomplete) && nowIncomplete.value(QStringLiteral("inspectionStatus")).toString() == QLatin1String("incomplete")
            && !flag(nowIncomplete, "inspectionComplete") && flag(nowIncomplete, "requiresIncompleteConfirmation")
            && flag(nowIncomplete, "countsAreLowerBounds") && flag(nowIncomplete, "hitEntryLimit") && flag(nowIncomplete, "alreadyApproved"),
        "the current facts are the incomplete walk's, as a group");
    const QVariantMap storedOne = nowIncomplete.value(QStringLiteral("approval")).toMap();
    require(storedOne.value(QStringLiteral("approvalId")).toString() == complete
            && storedOne.value(QStringLiteral("approvedAtMs")).toLongLong()
                == before.front().toMap().value(QStringLiteral("approvedAtMs")).toLongLong()
            && flag(nowIncomplete, "storedInspectionComplete") && flag(storedOne, "inspectionComplete")
            && nowIncomplete.value(QStringLiteral("approvedAtMs")).toLongLong() == storedOne.value(QStringLiteral("approvedAtMs")).toLongLong(),
        "the stored record, its approval time and its completeness are reported as they were");
    require(storedRows() == before, "nothing was written to the stored record");

    // The inspection FAILS today: no current fact is invented, and the stored completeness is not passed off as one.
    rig.inspector.force = 2;
    const QVariantMap failed = recheck(complete);
    require(!ok(failed) && code(failed) == QLatin1String("output_dir.inspection_failed") && !failed.contains(QStringLiteral("inspectionComplete"))
            && !failed.contains(QStringLiteral("inspectionStatus")) && flag(failed, "storedInspectionComplete")
            && failed.value(QStringLiteral("approval")).toMap().value(QStringLiteral("approvalId")).toString() == complete,
        "a refused inspection reports only the stored record, labelled as stored");

    // An INCOMPLETE approval, a walk that is COMPLETE today.
    rig.inspector.force = 1;
    const QString incomplete = approveFolder(QStringLiteral("Out3"), true);
    rig.inspector.force = 0;
    const QVariantMap nowComplete = recheck(incomplete);
    require(ok(nowComplete) && nowComplete.value(QStringLiteral("inspectionStatus")).toString() == QLatin1String("complete")
            && flag(nowComplete, "inspectionComplete") && !flag(nowComplete, "requiresIncompleteConfirmation")
            && !flag(nowComplete, "countsAreLowerBounds") && !flag(nowComplete, "hitEntryLimit"),
        "the current facts are the complete walk's, as a group");
    require(!flag(nowComplete, "storedInspectionComplete")
            && !flag(nowComplete.value(QStringLiteral("approval")).toMap(), "inspectionComplete")
            && nowComplete.value(QStringLiteral("approval")).toMap().value(QStringLiteral("approvalId")).toString() == incomplete,
        "the stored record stays incomplete");
    require(storedRows().size() == 2 && seen.changed.size() == 2, "no row was rewritten or added by any recheck");
}

// list / recheck / revoke: on a worker, strictly per project, dormant roots are never "rechecked" against today's folder.
void testListRecheckAndRevokeAreWorkerSideAndPerProject()
{
    Rig rig;
    TempFolder second;
    rig.root.touch("Out2/a.txt");
    second.touch("Gen/b.txt");
    rig.addProject("p2", second.path());
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    const QString p1 = QStringLiteral("p1");
    const QString p2 = QStringLiteral("p2");
    const auto approveFolder = [&](const QString& project, const QString& folder) {
        const quint64 preview = previewOf(controller, seen, project, folder);
        const std::size_t n = seen.terminals.size();
        require(controller.approve(project, folder, preview, false, false) != 0, "approve accepted");
        const Terminal& done = awaitTerminal(seen, n + 1, "approve ends");
        require(ok(done.map), "approved: " + utf8Of(code(done.map)));
        return done.map.value(QStringLiteral("approvalId")).toString();
    };
    const QString id1 = approveFolder(p1, QStringLiteral("Out2"));
    const QString id2 = approveFolder(p2, QStringLiteral("Gen"));

    std::size_t n = seen.terminals.size();
    const quint64 listId = controller.listApprovals(p1);
    require(listId != 0 && controller.busy() && !controller.inspecting() && controller.activeOperation() == QLatin1String("list"),
        "list is a worker operation, not an inspection");
    const Terminal& listed = awaitTerminal(seen, n + 1, "list ends");
    const QVariantList rows = listed.map.value(QStringLiteral("approvals")).toList();
    require(listed.operation == QLatin1String("list") && listed.id == listId && ok(listed.map) && rows.size() == 1,
        "one record, only this project's");
    const QVariantMap row = rows.front().toMap();
    require(row.value(QStringLiteral("approvalId")).toString() == id1 && row.value(QStringLiteral("relativePath")).toString() == QLatin1String("Out2")
            && flag(row, "appliesToCurrentRoot") && flag(row, "inspectionComplete") && !flag(row, "nestedSourcesAcknowledged")
            && row.value(QStringLiteral("approvedBy")).toString() == osUserName(),
        "the stored facts are shown");
    for (const auto& thread : rig.approvals.threads()) {
        require(thread != std::this_thread::get_id(), "the repository was never called from the GUI thread");
    }

    // Recheck: a read-only inspection that changes nothing.
    n = seen.terminals.size();
    require(controller.recheckApproval(p1, id1) != 0, "recheck accepted");
    const Terminal& rechecked = awaitTerminal(seen, n + 1, "recheck ends");
    require(rechecked.operation == QLatin1String("recheck") && rechecked.subject == id1 && ok(rechecked.map) && flag(rechecked.map, "exists")
            && rechecked.map.value(QStringLiteral("approvedAtMs")).toLongLong() == row.value(QStringLiteral("approvedAtMs")).toLongLong(),
        "recheck shows the current facts and the original approval time");
    n = seen.terminals.size();
    controller.listApprovals(p1);
    require(awaitTerminal(seen, n + 1, "list again").map.value(QStringLiteral("approvals")).toList().front().toMap()
                == row,
        "the stored record is exactly as it was");
    n = seen.terminals.size();
    require(controller.recheckApproval(p1, QStringLiteral("no-such-id")) != 0, "accepted");
    require(code(awaitTerminal(seen, n + 1, "unknown").map) == QLatin1String("output_approval.not_found"), "an unknown id is a typed refusal");
    n = seen.terminals.size();
    require(controller.recheckApproval(p1, id2) != 0, "accepted");
    require(code(awaitTerminal(seen, n + 1, "foreign").map) == QLatin1String("output_approval.not_found"), "another project's id is not found here");
    n = seen.terminals.size();
    require(controller.revokeApproval(p1, id2) != 0, "accepted");
    const Terminal& foreignRevoke = awaitTerminal(seen, n + 1, "foreign revoke");
    require(ok(foreignRevoke.map) && !flag(foreignRevoke.map, "removed") && seen.changed.size() == 2,
        "revoking another project's record removes nothing and announces nothing new");
    require(rig.approvals.inner.listForProject("p2").value().size() == 1, "...and that record is still there");

    // The project's folder changed: the approval is dormant, visible, revocable - and never checked against the new folder.
    TempFolder moved;
    moved.touch("Out2/other.txt");
    rig.setProject("p1", moved.path(), ProjectStatus::Active);
    n = seen.terminals.size();
    controller.listApprovals(p1);
    const QVariantMap dormant = awaitTerminal(seen, n + 1, "list after a root change").map.value(QStringLiteral("approvals")).toList().front().toMap();
    require(!flag(dormant, "appliesToCurrentRoot"), "the approval is dormant");
    n = seen.terminals.size();
    controller.recheckApproval(p1, id1);
    const Terminal& dormantCheck = awaitTerminal(seen, n + 1, "recheck of a dormant approval");
    require(!ok(dormantCheck.map) && code(dormantCheck.map) == QLatin1String("output_approval.root_changed")
            && dormantCheck.map.value(QStringLiteral("relativePath")).toString() == QLatin1String("Out2"),
        "root_changed - the other folder's Out2 is not inspected in its place");

    // An archived project can still list and revoke, but not preview / approve / recheck.
    rig.setProject("p1", moved.path(), ProjectStatus::Archived);
    n = seen.terminals.size();
    controller.recheckApproval(p1, id1);
    require(code(awaitTerminal(seen, n + 1, "recheck, archived").map) != QString(), "recheck is refused for an archived project");
    n = seen.terminals.size();
    controller.previewApproval(p1, QStringLiteral("Out2"));
    require(code(awaitTerminal(seen, n + 1, "preview, archived").map) == QLatin1String("output_approval.project_not_active"),
        "preview is refused for an archived project");
    n = seen.terminals.size();
    controller.revokeApproval(p1, id1);
    const Terminal& revoked = awaitTerminal(seen, n + 1, "revoke, archived");
    require(ok(revoked.map) && flag(revoked.map, "removed") && flag(revoked.map, "changed") && seen.changed.size() == 3
            && seen.changed.back() == p1,
        "a dormant record of an archived project can be revoked, and that is announced");
    require(rig.approvals.inner.listForProject("p1").value().empty(), "it is gone");
}

void testWithoutAServiceEveryOperationRefusesTypedAndNothingStarts()
{
    ProjectOutputApprovalController controller(nullptr);
    Seen seen(controller);
    require(!controller.available() && !controller.busy(), "unavailable, idle");
    const QString p1 = QStringLiteral("p1");
    require(controller.previewApproval(p1, QStringLiteral("Out2")) == 0 && controller.lastErrorCode() == QLatin1String("output_approval.unavailable"),
        "preview refuses");
    require(controller.approve(p1, QStringLiteral("Out2"), 1, false, false) == 0 && controller.listApprovals(p1) == 0
            && controller.recheckApproval(p1, QStringLiteral("x")) == 0 && controller.revokeApproval(p1, QStringLiteral("x")) == 0
            && controller.lastErrorCode() == QLatin1String("output_approval.unavailable"),
        "so does every other operation");
    QCoreApplication::processEvents();
    require(seen.terminals.empty() && seen.busyChanges == 0 && !controller.cancelInspection(), "nothing started and nothing was announced");
    controller.setProjectContext(p1);
    controller.invalidatePreview();
}

void testAWorkerExceptionBecomesATypedFailureWithTheOriginalIdentity()
{
    Rig rig;
    rig.root.touch("Out2/a.txt");
    ProjectOutputApprovalController controller(&rig.service);
    Seen seen(controller);
    rig.inspector.throws = true;
    const quint64 id = controller.previewApproval(QStringLiteral("p1"), QStringLiteral("Out2"));
    require(id != 0, "accepted");
    const Terminal& done = awaitTerminal(seen, 1, "the failing preview ends");
    require(done.id == id && done.project == QLatin1String("p1") && done.subject == QLatin1String("Out2") && !ok(done.map)
            && code(done.map) == QLatin1String("output_approval.internal_error"),
        "a typed failure that keeps the identity of the call");
    require(!controller.busy(), "the controller is usable again");
}

// The model's tool surface does not contain the approval operations: they are a local user's action, not a tool.
void testNoModelToolCanApproveOrRevoke()
{
    auto registry = ToolRegistry::createDefaultReadOnly();
    require(registry.success(), "the tool registry");
    for (const auto& definition : registry.value().definitions()) {
        require(definition.readOnly, "every tool is read-only: " + definition.name);
        require(definition.name.find("approv") == std::string::npos && definition.name.find("revok") == std::string::npos
                && definition.name.find("output") == std::string::npos,
            "no approval tool: " + definition.name);
    }
}

// ---- through the bridge, on a temporary SQLite database ----

struct BridgeFolders {
    TempFolder root;
    fs::path project;
    fs::path database;

    BridgeFolders()
    {
        project = root.path() / "project";
        database = root.path() / "state" / "orchestrator.sqlite";
        fs::create_directories(project);
    }
    [[nodiscard]] QString projectPathQt() const { return QString::fromStdWString(project.wstring()); }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(database.wstring()); }
};

// A context snapshot the existing project-context controller builds, awaited. True when the snapshot exists; the error code otherwise.
std::pair<bool, QString> buildProjectContext(BackendBridge& bridge, const QString& projectId)
{
    ProjectContextController& context = bridge.projectContextController();
    quint64 finished = 0;
    const auto connection = QObject::connect(&context, &ProjectContextController::previewFinished, &context,
        [&finished](quint64 id, bool) { finished = id; });
    const quint64 id = context.preview(projectId, QString(), false);
    require(id != 0, "the context preview was accepted");
    require(waitFor("the context preview ends", [&] { return finished == id; }), "the context preview ended");
    QObject::disconnect(connection);
    return {context.hasSnapshot(), context.errorCode()};
}

// The approval is created, listed, rechecked and revoked through the bridge's controller on a temporary SQLite database; it survives a
// restart of the whole composition; and it is what the P3c context reads - an output file the defaults refuse stops the context until
// the folder is approved, and stops it again after the revocation.
void testTheBridgeCreatesListsRechecksAndRevokesOnARealDatabase()
{
    BridgeFolders folders;
    folders.root.touch("project/src/a.cpp", "int a;");
    folders.root.touch("project/Out2/big.obj", std::string(300 * 1024, 'x')); // over the read limit: the defaults refuse a read on it
    QString projectId;
    QString approvalId;
    {
        auto bridge = std::make_unique<BackendBridge>(folders.databasePathQt());
        require(bridge->storageAvailable(), "the temporary database opens");
        projectId = bridge->projectController().createProject(
            QStringLiteral("P3d bridge"), QStringLiteral("approval controller"), folders.projectPathQt());
        require(!projectId.isEmpty(), "a project");

        QObject* exported = bridge->projectOutputApprovals();
        require(exported != nullptr && exported == &bridge->projectOutputApprovalController()
                && exported->property("available").toBool()
                && bridge->property("projectOutputApprovals").value<QObject*>() == exported,
            "the controller is exported as a CONSTANT bridge property and is available");
        ProjectOutputApprovalController& controller = bridge->projectOutputApprovalController();
        Seen seen(controller);

        const auto before = buildProjectContext(*bridge, projectId);
        require(!before.first && before.second == QLatin1String("workspace_baseline.file_limit"),
            "before the approval the context is refused on the output file");

        controller.setProjectContext(projectId);
        const quint64 preview = previewOf(controller, seen, projectId, QStringLiteral("Out2"));
        require(ok(seen.terminals.back().map) && flag(seen.terminals.back().map, "exists"), "the real inspector previews the folder");
        const std::size_t n = seen.terminals.size();
        require(controller.approve(projectId, QStringLiteral("Out2"), preview, false, false) != 0, "approve accepted");
        const Terminal& approved = awaitTerminal(seen, n + 1, "the approval ends");
        require(ok(approved.map) && flag(approved.map, "changed"), "approved through the real service and SQLite: " + utf8Of(code(approved.map)));
        approvalId = approved.map.value(QStringLiteral("approvalId")).toString();
        require(seen.changed.size() == 1 && seen.changed.front() == projectId, "approvalsChanged names the project");

        const auto after = buildProjectContext(*bridge, projectId);
        require(after.first, "after the approval the P3c context reads the project without the output folder: " + utf8Of(after.second));

        controller.listApprovals(projectId);
        const Terminal& listed = awaitTerminal(seen, n + 2, "list");
        const QVariantList rows = listed.map.value(QStringLiteral("approvals")).toList();
        require(rows.size() == 1 && rows.front().toMap().value(QStringLiteral("approvalId")).toString() == approvalId
                && flag(rows.front().toMap(), "appliesToCurrentRoot"),
            "the record is listed");
        controller.recheckApproval(projectId, approvalId);
        const Terminal& rechecked = awaitTerminal(seen, n + 3, "recheck");
        require(ok(rechecked.map) && flag(rechecked.map, "alreadyApproved") && seen.changed.size() == 1,
            "a recheck inspects again and approves nothing new");
    }
    {   // a restart of the whole composition
        auto bridge = std::make_unique<BackendBridge>(folders.databasePathQt());
        require(bridge->storageAvailable(), "the database reopens");
        ProjectOutputApprovalController& controller = bridge->projectOutputApprovalController();
        Seen seen(controller);
        controller.listApprovals(projectId);
        const Terminal& listed = awaitTerminal(seen, 1, "list after the restart");
        const QVariantList rows = listed.map.value(QStringLiteral("approvals")).toList();
        require(rows.size() == 1 && rows.front().toMap().value(QStringLiteral("approvalId")).toString() == approvalId,
            "the approval survived the restart");
        require(buildProjectContext(*bridge, projectId).first, "...and the context still reads under it");

        controller.revokeApproval(projectId, approvalId);
        const Terminal& revoked = awaitTerminal(seen, 2, "revoke");
        require(ok(revoked.map) && flag(revoked.map, "removed") && seen.changed.size() == 1 && seen.changed.front() == projectId,
            "revoked, and announced");
        const auto later = buildProjectContext(*bridge, projectId);
        require(!later.first && later.second == QLatin1String("workspace_baseline.file_limit"), "after the revocation the output file refuses the context again");
        controller.listApprovals(projectId);
        require(awaitTerminal(seen, 3, "list after the revoke").map.value(QStringLiteral("approvals")).toList().isEmpty(), "nothing is left");
    }
}

// Without durable storage the property exists and the controller is simply unavailable; nothing stands in for it.
void testTheBridgeWithoutStorageExportsAnUnavailableController()
{
    BridgeFolders folders;
    auto owner = std::make_unique<BackendBridge>(folders.databasePathQt());
    require(owner->storageAvailable() && owner->projectOutputApprovalController().available(), "the owner has the service");
    auto intruder = std::make_unique<BackendBridge>(folders.databasePathQt());   // the database file is owned by another runtime
    require(!intruder->storageAvailable(), "the second bridge gets unavailable storage");
    require(intruder->projectOutputApprovals() != nullptr, "...but the property is still there");
    ProjectOutputApprovalController& controller = intruder->projectOutputApprovalController();
    require(!controller.available(), "the controller is unavailable");
    require(controller.previewApproval(QStringLiteral("p1"), QStringLiteral("Out2")) == 0
            && controller.lastErrorCode() == QLatin1String("output_approval.unavailable")
            && controller.listApprovals(QStringLiteral("p1")) == 0 && !controller.busy(),
        "every operation refuses with a typed reason");
}

// Tearing the whole composition down while an operation is running neither crashes nor hangs: the controller stops its worker and
// waits for it before the runtime (and so the service) goes away.
void testTheBridgeCanBeDestroyedWhileAnOperationRuns()
{
    BridgeFolders folders;
    for (int i = 0; i < 400; ++i) folders.root.touch("project/Out2/d" + std::to_string(i % 20) + "/f" + std::to_string(i) + ".o");
    auto bridge = std::make_unique<BackendBridge>(folders.databasePathQt());
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("P3d teardown"), QStringLiteral("approval controller"), folders.projectPathQt());
    require(!projectId.isEmpty(), "a project");
    ProjectOutputApprovalController& controller = bridge->projectOutputApprovalController();
    require(controller.previewApproval(projectId, QStringLiteral("Out2")) != 0, "a preview is running (or has just finished)");
    bridge.reset();
    require(true, "the bridge was destroyed");
}

} // namespace

int runProjectOutputApprovalControllerTests()
{
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"testOperationsRunOnWorkersAndTheStateStaysConsistent", testOperationsRunOnWorkersAndTheStateStaysConsistent},
        {"testApproveIsBoundToTheBackendPreview", testApproveIsBoundToTheBackendPreview},
        {"testConfirmationsAreIndependentAndHardRefusalsStay", testConfirmationsAreIndependentAndHardRefusalsStay},
        {"testCancelWritesNothingAndLateResultsNeverBecomeThePreview", testCancelWritesNothingAndLateResultsNeverBecomeThePreview},
        {"testTheDestructorStopsAndWaitsForTheWorker", testTheDestructorStopsAndWaitsForTheWorker},
        {"testALateCancelNeverHidesARecordThatWasWritten", testALateCancelNeverHidesARecordThatWasWritten},
        {"testACancelDuringTheSecondProjectReadWritesNothing", testACancelDuringTheSecondProjectReadWritesNothing},
        {"testRecheckKeepsTheStoredRecordApartFromTodaysInspection", testRecheckKeepsTheStoredRecordApartFromTodaysInspection},
        {"testListRecheckAndRevokeAreWorkerSideAndPerProject", testListRecheckAndRevokeAreWorkerSideAndPerProject},
        {"testWithoutAServiceEveryOperationRefusesTypedAndNothingStarts", testWithoutAServiceEveryOperationRefusesTypedAndNothingStarts},
        {"testAWorkerExceptionBecomesATypedFailureWithTheOriginalIdentity", testAWorkerExceptionBecomesATypedFailureWithTheOriginalIdentity},
        {"testNoModelToolCanApproveOrRevoke", testNoModelToolCanApproveOrRevoke},
        {"testTheBridgeCreatesListsRechecksAndRevokesOnARealDatabase", testTheBridgeCreatesListsRechecksAndRevokesOnARealDatabase},
        {"testTheBridgeWithoutStorageExportsAnUnavailableController", testTheBridgeWithoutStorageExportsAnUnavailableController},
        {"testTheBridgeCanBeDestroyedWhileAnOperationRuns", testTheBridgeCanBeDestroyedWhileAnOperationRuns},
    };
    int failures = 0;
    for (const auto& item : cases) {
        try {
            item.run();
        } catch (const std::exception& exception) {
            std::cerr << "FAILED " << item.name << ": " << exception.what() << std::endl;
            ++failures;
        }
    }
    if (failures == 0) std::cout << "ProjectOutputApprovalController tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
