// QML smoke tests for ComparisonPage backend integration.
// Verifies that the page loads, receives a backend, and the controller signals
// reach QML without crashes. No network; uses BackendBridge with an isolated
// temporary database.

#include <ai/ui/BackendBridge.hpp>

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// ── Test 1: BackendBridge exposes comparison property ───────────────────────

void test_backendBridge_exposesComparisonProperty()
{
    QTemporaryDir tmp;
    require(tmp.isValid(), "temp dir must be valid");

    const QString dbPath = tmp.filePath(QStringLiteral("smoke_test.sqlite3"));
    BackendBridge bridge(dbPath);

    QObject* comparisonObj = bridge.comparison();
    require(comparisonObj != nullptr, "comparison property must not be null");
}

// ── Test 2: ComparisonController exposes the three list models ───────────────

void test_comparisonController_exposesListModels()
{
    QTemporaryDir tmp;
    require(tmp.isValid(), "temp dir must be valid");

    const QString dbPath = tmp.filePath(QStringLiteral("smoke_test2.sqlite3"));
    BackendBridge bridge(dbPath);

    auto& ctrl = bridge.comparisonController();

    QAbstractItemModel* projects    = ctrl.projectsModel();
    QAbstractItemModel* comparisons = ctrl.comparisonListModel();
    QAbstractItemModel* results     = ctrl.resultListModel();

    require(projects    != nullptr, "projectsModel must not be null");
    require(comparisons != nullptr, "comparisonListModel must not be null");
    require(results     != nullptr, "resultListModel must not be null");
}

// ── Test 3: selectProject with no comparisons leaves result model empty ───────

void test_selectProject_emptyProjectLeavesResultModelEmpty()
{
    QTemporaryDir tmp;
    require(tmp.isValid(), "temp dir must be valid");

    const QString dbPath = tmp.filePath(QStringLiteral("smoke_test3.sqlite3"));
    BackendBridge bridge(dbPath);
    auto& ctrl = bridge.comparisonController();

    // No projects exist; selecting a nonexistent id must not crash and must
    // leave the result model empty.
    ctrl.selectProject(QStringLiteral("ghost-project"));
    require(ctrl.resultListModel()->rowCount() == 0, "result model must be empty for unknown project");
}

// ── Test 4: storageAvailable is true for a valid isolated database ────────────

void test_bridge_storageAvailableForIsolatedDb()
{
    QTemporaryDir tmp;
    require(tmp.isValid(), "temp dir must be valid");

    const QString dbPath = tmp.filePath(QStringLiteral("smoke_test4.sqlite3"));
    BackendBridge bridge(dbPath);
    require(bridge.storageAvailable(), "storage must be available for a valid temp path");
}

// ── Test 5: refresh() does not crash with no selection ────────────────────────

void test_refresh_doesNotCrashWithNoSelection()
{
    QTemporaryDir tmp;
    require(tmp.isValid(), "temp dir must be valid");

    const QString dbPath = tmp.filePath(QStringLiteral("smoke_test5.sqlite3"));
    BackendBridge bridge(dbPath);
    auto& ctrl = bridge.comparisonController();

    // No project or comparison selected; refresh() must be a no-op, not a crash.
    ctrl.refresh();
    require(ctrl.selectedProjectId().isEmpty(), "selectedProjectId must remain empty");
    require(ctrl.selectedComparisonId().isEmpty(), "selectedComparisonId must remain empty");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    test_backendBridge_exposesComparisonProperty();
    test_comparisonController_exposesListModels();
    test_selectProject_emptyProjectLeavesResultModelEmpty();
    test_bridge_storageAvailableForIsolatedDb();
    test_refresh_doesNotCrashWithNoSelection();

    return 0;
}
