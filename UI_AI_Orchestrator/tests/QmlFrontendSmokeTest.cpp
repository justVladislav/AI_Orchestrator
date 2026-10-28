// Headless end-to-end smoke test for the ProjectsPage/RunsPage <-> Qt Adapter
// wiring. It loads the real Main.qml with a real BackendBridge (no fakes) and
// drives the same QML functions the "New project"/"Orchestration"/"New run"/
// "Cancel" UI controls call, then asserts the resulting state through the Qt
// Adapter's own read models - proving the front end and the already-tested
// adapter agree end to end.
//
// Also covers OverviewPage's live data binding: after project creation and run
// completion the Overview page must reflect real backend state, not demo data.
#include <ai/application/proposals/ChangeProposalService.hpp>
#include <ai/application/proposals/ChangeProposalTypes.hpp>
#include <ai/application/execution/ExecutionAcceptanceContract.hpp>
#include <ai/application/execution/IManifestWorkLedgerRepository.hpp>
#include <ai/application/IRunRepository.hpp>
#include <ai/application/IWorkflowRunner.hpp>
#include <ai/application/MockWorkflowRunner.hpp>
#include <ai/application/verification/ManifestProposalContentHash.hpp>
#include <ai/application/verification/ManifestVerificationContinuationPreparationService.hpp>
#include <ai/application/verification/ManifestVerificationContinuationTypes.hpp>
#include <ai/application/execution/ManifestWorkLedgerTypes.hpp>
#include <ai/application/Sha256.hpp>
#include <ai/application/workspace/WorkspaceBaselineProvenance.hpp>
#include <ai/application/workspace/WorkspaceReadPolicy.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/ComparisonResultListModel.hpp>
#include <ai/ui/ComparisonSessionListModel.hpp>
#include <ai/ui/ProviderCatalogController.hpp>
#include <ai/ui/ProjectListModel.hpp>
#include <ai/ui/RunListModel.hpp>
#include <ai/ui/RunStageModel.hpp>
#include "../test_support/QtTestSupport.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJSValue>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QVariant>
#include <QStringList>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string>

namespace {
void checkpoint(const char* label)
{
    std::fprintf(stderr, "[checkpoint] %s\n", label);
    std::fflush(stderr);
}
} // namespace

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// A fresh project directory plus a fresh "state/orchestrator.sqlite" path.
// The database path is passed to BackendBridge's explicit-path constructor
// so this test never reads or writes the real per-user database that the
// default constructor would resolve via QStandardPaths::AppDataLocation.
struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("ai-orchestrator-qml-smoke-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";

    TempWorkspace() { fs::create_directories(projectPath); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }

    // PC-4C: a BackendBridge is the ONE live owner of the database file. The second, short-lived runtime that a fixture seeds the file
    // through while the bridge is alive is therefore a GUEST: it takes no ownership lock and writes no Outcome Brief.
    [[nodiscard]] ai::backend::BackendRuntimeConfig guestRuntimeConfig() const
    {
        ai::backend::BackendRuntimeConfig config {databasePath};
        config.exclusiveDatabaseOwnership = false;
        return config;
    }
};

// Test infrastructure for the Run-cancellation check: a barrier the Mock workflow stops at, instead of a race against its clock.
// arm() makes the NEXT Run that reaches a step the held one - the test does not have to know its id beforehand - and that Run then
// stays at the barrier, certainly Running and unable to finish, until the test releases it or the Run is cancelled (the wait ends on
// the stop request, which is how a real cancellation reaches the workflow). Every other Run passes through untouched.
class RunBarrier final : public ai::application::IMockWorkflowGate {
public:
    void arm()
    {
        std::lock_guard lock(mutex_);
        armed_ = true;
        held_ = false;
        released_ = false;
        heldRunId_.clear();
    }

    void beforeStep(const ai::application::RunId& runId, std::size_t, std::size_t, std::stop_token stopToken) override
    {
        std::unique_lock lock(mutex_);
        if (armed_) {
            armed_ = false;
            held_ = true;
            heldRunId_ = runId;
            changed_.notify_all();
        }
        if (!held_ || released_ || heldRunId_ != runId) return;
        changed_.wait(lock, stopToken, [this] { return released_; });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool held() const
    {
        std::lock_guard lock(mutex_);
        return held_;
    }

    [[nodiscard]] std::string heldRunId() const
    {
        std::lock_guard lock(mutex_);
        return heldRunId_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    bool armed_ {false};
    bool held_ {false};
    bool released_ {false};
    std::string heldRunId_;
};

QString statusForRun(QAbstractItemModel* model, const QString& runId)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, RunListModel::RunIdRole).toString() == runId) {
            return model->data(index, RunListModel::StatusRole).toString();
        }
    }
    return {};
}

QString waitForStatus(
    QAbstractItemModel* model,
    const QString& runId,
    const QStringList& expected,
    std::chrono::milliseconds timeout = 5s)
{
    ai::ui::test_support::waitForCondition("QML Run status", [&] {
        return expected.contains(statusForRun(model, runId));
    }, [&] {
        return ("runId=" + runId + ", status=" + statusForRun(model, runId)).toStdString();
    }, timeout);
    return statusForRun(model, runId);
}

bool waitForRowCount(
    QAbstractItemModel* model,
    int expected,
    std::chrono::milliseconds timeout = 5s)
{
    return ai::ui::test_support::waitForCondition("QML model row count", [&] {
        return model->rowCount() == expected;
    }, [&] {
        return "expected=" + std::to_string(expected)
            + ", actual=" + std::to_string(model->rowCount());
    }, timeout);
}

QString statusForComparisonSession(QAbstractItemModel* model, const QString& sessionId)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, ComparisonSessionListModel::SessionIdRole).toString()
            == sessionId) {
            return model->data(index, ComparisonSessionListModel::StatusRole).toString();
        }
    }
    return {};
}

QString waitForComparisonStatus(
    QAbstractItemModel* model,
    const QString& sessionId,
    const QStringList& expected,
    std::chrono::milliseconds timeout = 5s)
{
    ai::ui::test_support::waitForCondition("QML comparison status", [&] {
        return expected.contains(statusForComparisonSession(model, sessionId));
    }, [&] {
        return ("sessionId=" + sessionId + ", status="
            + statusForComparisonSession(model, sessionId)).toStdString();
    }, timeout);
    return statusForComparisonSession(model, sessionId);
}

QObject* waitForChild(QObject* root, const char* name, std::chrono::milliseconds timeout = 5s)
{
    QObject* found = nullptr;
    ai::ui::test_support::waitForCondition("QML child", [&] {
        found = root->findChild<QObject*>(QString::fromLatin1(name));
        return found != nullptr;
    }, [name] { return std::string("objectName=") + name; }, timeout);
    return found;
}

// Spin the event loop until pred(obj->property(name)) is true or timeout expires.
bool waitForProperty(
    QObject* obj,
    const char* name,
    const std::function<bool(const QVariant&)>& pred,
    std::chrono::milliseconds timeout = 5s)
{
    return ai::ui::test_support::waitForCondition("QML property", [&] {
        return pred(obj->property(name));
    }, [&] {
        return std::string("property=") + name + ", value="
            + obj->property(name).toString().toStdString();
    }, timeout);
}

QString makeLongMultilineText(const QString& heading, const QString& linePrefix)
{
    // Exceed the dialogs' 1000-character counter branch by a wide margin and
    // preserve many internal newlines through QML, Qt Adapter, and SQLite.
    QString text = heading + QLatin1Char('\n');
    for (int line = 0; line < 96; ++line) {
        text += QStringLiteral("%1 %2: context must remain complete and reviewable across the UI boundary.\n")
                    .arg(linePrefix)
                    .arg(line);
    }
    text += QStringLiteral("Final acceptance line without trailing whitespace.");
    return text;
}

} // namespace

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();

    // Uses whatever QPA platform is already deployed next to this executable
    // (the default "windows" platform, same as appUI_AI_Orchestrator). The
    // caller may still override QT_QPA_PLATFORM (e.g. to "offscreen") if an
    // offscreen plugin is deployed in that environment.
    // Match the desktop entry point: custom control backgrounds require
    // a non-native style before QGuiApplication is constructed.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setDefaultAlphaBuffer(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIOrchestrator"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("ai-orchestrator.local"));
    QCoreApplication::setApplicationName(QStringLiteral("AIOrchestratorTests"));

    QGuiApplication app(argc, argv);
    checkpoint("QGuiApplication constructed");
    TempWorkspace workspace;

    // Heap-owned, matching QtAdapterTests.cpp's own established fix for the
    // identical Windows-stack-layout-sensitive destructor crash a
    // stack-local BackendBridge instance can trigger (see
    // QML_PROJECT_CONTEXT.md's account of that earlier incident) -
    // BackendBridge is large enough, with enough controller members, that a
    // plain stack-local instance sits right at the edge of a safe stack
    // frame; AG-18.4B's new ProjectVerificationSetupController member
    // (itself already heap-owned inside BackendBridge for the same reason)
    // was enough additional growth to tip this exact fixture over that edge
    // during shutdown.
    // The Mock workflow of this bridge calls the barrier before every step (a test-only composition; it holds nothing until it is armed).
    auto runBarrier = std::make_shared<RunBarrier>();
    auto backendOwner = std::make_unique<BackendBridge>(workspace.databasePathQt(), runBarrier, nullptr);
    BackendBridge& backend = *backendOwner;
    checkpoint(backend.storageAvailable() ? "BackendBridge constructed (SQLite ready)" : "BackendBridge constructed (STORAGE UNAVAILABLE)");
    assert(backend.storageAvailable());
    QQmlApplicationEngine engine;
    QStringList qmlWarnings;
    QVariantMap initialProperties;
    initialProperties.insert(QStringLiteral("backend"), QVariant::fromValue(&backend));
    engine.setInitialProperties(initialProperties);
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
        [&qmlWarnings](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) {
                qmlWarnings.push_back(warning.toString());
                std::fprintf(stderr, "[qml-warning] %s\n", warning.toString().toLocal8Bit().constData());
            }
            std::fflush(stderr);
        });
    // What main() does from the generated header: the version is set before the QML loads (the bridge property is CONSTANT).
    QCoreApplication::setApplicationVersion(QStringLiteral("9.9.9-smoke.1"));
    engine.loadFromModule("UI_AI_Orchestrator", "Main");
    checkpoint("loadFromModule called");

    QObject* window = nullptr;
    const bool engineLoaded = ai::ui::test_support::waitForCondition(
        "QML engine root object", [&] { return !engine.rootObjects().isEmpty(); },
        5s);
    checkpoint("engine loaded");
    assert(engineLoaded && "Main.qml failed to load");
    window = engine.rootObjects().constFirst();

    // PageHost creates a page on its first selection and keeps the instance: at the start only Overview exists.
    // Visit every other page once (this is also how the user reaches them), check that the instances are kept
    // on a revisit, and return to Overview, where the rest of this test starts.
    assert(waitForChild(window, "overviewPage") && "Overview is created at start");
    for (const char* lazyPage : {"projectsPage", "chatsPage", "runsPage", "comparisonPage", "providersPage", "settingsPage"})
        assert(window->findChild<QObject*>(QString::fromLatin1(lazyPage)) == nullptr
            && "a page other than Overview must not exist before its first visit");
    for (const char* section : {"projects", "chats", "runs", "comparison", "providers", "settings"}) {
        assert(window->setProperty("currentSection", QString::fromLatin1(section)));
        const std::string pageName = std::string(section) + "Page";
        assert(waitForChild(window, pageName.c_str()) && "the page is created on its first visit");
    }
    {
        QObject* runsOnce = window->findChild<QObject*>(QStringLiteral("runsPage"));
        assert(window->setProperty("currentSection", QStringLiteral("projects")));
        assert(window->setProperty("currentSection", QStringLiteral("runs")));
        assert(window->findChild<QObject*>(QStringLiteral("runsPage")) == runsOnce
            && "a page keeps its instance when it is left and visited again");
    }
    assert(window->setProperty("currentSection", QStringLiteral("overview")));
    checkpoint("all pages visited once; instances kept");

    QObject* projectsPage = waitForChild(window, "projectsPage");
    checkpoint(projectsPage ? "found projectsPage" : "MISSING projectsPage");
    assert(projectsPage && "ProjectsPage was not found in the loaded QML tree");

    // Settings > Diagnostics shows the application version the process was given, not a placeholder.
    QObject* applicationVersionText = waitForChild(window, "applicationVersionText");
    assert(applicationVersionText && "the Settings version row was not found in the loaded QML tree");
    assert(applicationVersionText->property("text").toString() == QStringLiteral("9.9.9-smoke.1"));
    checkpoint("Settings shows the application version");

    QObject* runsPage = waitForChild(window, "runsPage");
    checkpoint(runsPage ? "found runsPage" : "MISSING runsPage");
    assert(runsPage && "RunsPage was not found in the loaded QML tree");

    QObject* overviewPage = waitForChild(window, "overviewPage");
    checkpoint(overviewPage ? "found overviewPage" : "MISSING overviewPage");
    assert(overviewPage && "OverviewPage was not found in the loaded QML tree");

    QObject* providersPage = waitForChild(window, "providersPage");
    assert(providersPage && "ProvidersPage was not found in the loaded QML tree");

    QObject* comparisonPage = waitForChild(window, "comparisonPage");
    checkpoint(comparisonPage ? "found comparisonPage" : "MISSING comparisonPage");
    assert(comparisonPage && "ComparisonPage was not found in the loaded QML tree");

    // Locate the actual dialog controls. The test submits through each
    // dialog's shared submit() function instead of bypassing the editor and
    // calling the owning page directly.
    QObject* newProjectDialog = waitForChild(window, "newProjectDialog");
    QObject* projectNameField = waitForChild(window, "projectNameField");
    QObject* projectPathField = waitForChild(window, "projectPathField");
    QObject* descriptionField = waitForChild(window, "descriptionField");
    QObject* descriptionCounter = waitForChild(window, "descriptionCounter");
    QObject* descriptionFlickable = waitForChild(window, "descriptionFlickable");
    // RunsPage loads NewRunDialog lazily to keep application startup stable.
    // Exercise the public page entry point before looking up the dialog tree.
    assert(QMetaObject::invokeMethod(
        runsPage, "openNewRunDialog", Q_ARG(QVariant, QVariant(QString {}))));
    QObject* newRunDialog = waitForChild(window, "newRunDialog");
    QObject* runProjectCombo = waitForChild(window, "runProjectCombo");
    QObject* runTitleField = waitForChild(window, "runTitleField");
    QObject* taskField = waitForChild(window, "taskField");
    QObject* taskCounter = waitForChild(window, "taskCounter");
    QObject* taskFlickable = waitForChild(window, "taskFlickable");
    QObject* realProviderCheck = waitForChild(window, "useExecutionProfileCheck");
    QObject* automaticVerificationRepairCheck =
        waitForChild(window, "automaticVerificationRepairCheck");
    assert(newProjectDialog && projectNameField && projectPathField
           && descriptionField && descriptionCounter && descriptionFlickable);
    assert(newRunDialog && runProjectCombo && runTitleField
           && taskField && taskCounter && taskFlickable && realProviderCheck
           && automaticVerificationRepairCheck);
    assert(automaticVerificationRepairCheck->property("checked").toBool()
           && "New Run must preserve the historical one-wave repair default");
    assert(waitForProperty(newRunDialog, "agentCount",
        [](const QVariant& value) { return value.toInt() == 1; })
        && "A fresh New Run dialog must start with exactly one agent");
    assert(waitForChild(window, "projectStatusLegend")
           && "Projects status legend was not loaded");
    QObject* projectStatusLegend = waitForChild(window, "projectStatusLegend");
    QObject* projectListView = waitForChild(window, "projectListView");
    assert(projectStatusLegend && projectListView);
    QObject* overviewFromDatePicker = waitForChild(window, "overviewFromDatePicker");
    QObject* overviewToDatePicker = waitForChild(window, "overviewToDatePicker");
    assert(overviewFromDatePicker && overviewToDatePicker);
    QObject* draftsDialogLoader = waitForChild(window, "draftsDialogLoader");
    assert(draftsDialogLoader && !draftsDialogLoader->property("active").toBool());
    assert(QMetaObject::invokeMethod(runsPage, "openDrafts"));
    assert(runsPage->property("notificationText").toString()
           == QStringLiteral("No drafts saved yet"));
    assert(!draftsDialogLoader->property("active").toBool()
           && "Empty Drafts action must not instantiate the dialog");
    QObject* runDetailsPanel = waitForChild(window, "runDetailsPanel");
    QObject* runDetailsFlickable = waitForChild(window, "runDetailsFlickable");
    QObject* runDetailsJumpToBottom = waitForChild(window, "runDetailsJumpToBottom");
    QObject* plannerOutcomeCard = waitForChild(window, "plannerOutcomeCard");
    QObject* viewPlannerReportButton = waitForChild(window, "viewPlannerReportButton");
    QObject* manifestVerificationCard = waitForChild(window, "manifestVerificationCard");
    QObject* manifestVerificationSummary = waitForChild(window, "manifestVerificationSummary");
    QObject* viewManifestVerificationButton =
        waitForChild(window, "viewManifestVerificationButton");
    QObject* workspaceProvenanceCard = waitForChild(window, "workspaceProvenanceCard");
    QObject* workspaceProvenanceSummary = waitForChild(window, "workspaceProvenanceSummary");
    QObject* viewWorkspaceBaselineButton =
        waitForChild(window, "viewWorkspaceBaselineButton");
    QObject* viewWorkspaceProvenanceButton =
        waitForChild(window, "viewWorkspaceProvenanceButton");
    assert(runDetailsPanel && runDetailsFlickable && runDetailsJumpToBottom
        && plannerOutcomeCard && viewPlannerReportButton
        && manifestVerificationCard && manifestVerificationSummary
        && viewManifestVerificationButton && workspaceProvenanceCard
        && workspaceProvenanceSummary && viewWorkspaceBaselineButton
        && viewWorkspaceProvenanceButton);
    assert(!runDetailsJumpToBottom->property("visible").toBool());

    // ── Overview: empty state before any project exists ───────────────────────
    // The liveProjectsModel must start empty; selectedProjectId must be "".
    const QVariant emptyProjects = overviewPage->property("liveProjectsModel");
    checkpoint(("overview liveProjectsModel initial count=" + std::to_string(emptyProjects.toList().size())).c_str());
    assert(emptyProjects.toList().isEmpty() && "OverviewPage must start with no live projects");
    assert(overviewPage->property("selectedProjectId").toString().isEmpty()
           && "OverviewPage selectedProjectId must start empty");

    // 1) Open and submit the real NewProjectDialog. This exercises clearing,
    // the >1000-character counter branch, scrolling, trimming, its unchanged
    // signal, the owning page, Qt Adapter, and SQLite in one path.
    const QString projectPath = QString::fromStdWString(workspace.projectPath.wstring());
    const QString longDescription = makeLongMultilineText(
        QStringLiteral("Project architecture context"),
        QStringLiteral("Description line"));
    assert(longDescription.size() > 1000);
    assert(QMetaObject::invokeMethod(newProjectDialog, "open"));
    assert(waitForProperty(newProjectDialog, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(projectNameField->setProperty("text", QStringLiteral("QML smoke test project")));
    assert(projectPathField->setProperty("text", projectPath));
    assert(descriptionField->setProperty("text", longDescription));
    assert(waitForProperty(descriptionCounter, "text", [&](const QVariant& value) {
        const QString text = value.toString();
        return text.contains(QString::number(longDescription.size()))
            && text.contains(QStringLiteral("KB"));
    }));
    assert(waitForProperty(descriptionFlickable, "contentHeight", [&](const QVariant& value) {
        return value.toDouble() > descriptionFlickable->property("height").toDouble();
    }));
    const QString descriptionCounterText = descriptionCounter->property("text").toString();
    assert(descriptionCounterText.contains(QString::number(longDescription.size())));
    assert(descriptionCounterText.contains(QStringLiteral("KB")));
    assert(descriptionFlickable->property("contentHeight").toDouble()
           > descriptionFlickable->property("height").toDouble());
    const bool createInvoked = QMetaObject::invokeMethod(newProjectDialog, "submit");
    checkpoint(createInvoked ? "NewProjectDialog submitted" : "NewProjectDialog submit FAILED");
    assert(createInvoked);

    checkpoint(("project rowCount=" + std::to_string(backend.projectController().model()->rowCount())).c_str());
    assert(backend.projectController().model()->rowCount() == 1);
    const QString projectId = backend.projectController().model()->data(
        backend.projectController().model()->index(0, 0),
        ProjectListModel::ProjectIdRole).toString();
    checkpoint(("projectId=" + projectId.toStdString()).c_str());
    assert(!projectId.isEmpty());
    assert(backend.projectController().model()->data(
               backend.projectController().model()->index(0, 0),
               ProjectListModel::DescriptionRole).toString() == longDescription);

    // The Projects page must select the created project by its stable ID, not by position.
    checkpoint(("projectsPage selectedProjectId=" + projectsPage->property("selectedProjectId").toString().toStdString()).c_str());
    assert(projectsPage->property("selectedProjectId").toString() == projectId);

    // AG-18.4B-R1: the selected project shows its verification setup (nothing was checked yet, and nothing starts by itself).
    QObject* setupHeadline = projectsPage->findChild<QObject*>(QStringLiteral("projectSetupHeadline"));
    assert(setupHeadline != nullptr && "ProjectsPage must show the project's verification setup");
    assert(waitForProperty(setupHeadline, "text",
        [](const QVariant& value) { return value.toString() == QStringLiteral("Not checked yet"); }));
    assert(!backend.projectVerificationSetupController().checkingProjectSetup());

    // Continue persists the application-wide project selection and navigates
    // to Runs. It must not start a run as a side effect.
    const bool continueInvoked = QMetaObject::invokeMethod(
        projectsPage, "continueProject",
        Q_ARG(QVariant, projectId));
    assert(continueInvoked);
    assert(backend.workspaceContextController().activeProjectId() == projectId);
    assert(backend.runController().model()->rowCount() == 0);
    assert(waitForProperty(window, "currentSection",
        [](const QVariant& value) { return value.toString() == QStringLiteral("runs"); }));
    assert(waitForProperty(runsPage, "projectFilter",
        [&](const QVariant& value) { return value.toString() == projectId; }));

    // ── Overview: auto-selects first project after creation ───────────────────
    // OverviewPage's projectsMirror Repeater fires rebuildProjectsSnapshot() when
    // the model gains a row, then selectedProjectId auto-picks the first entry.
    const bool overviewSelected = waitForProperty(overviewPage, "selectedProjectId",
        [&](const QVariant& v) { return v.toString() == projectId; });
    checkpoint(("overviewPage selectedProjectId=" + overviewPage->property("selectedProjectId").toString().toStdString()).c_str());
    assert(overviewSelected && "OverviewPage did not auto-select the created project");
    assert(overviewPage->property("liveProjectsModel").toList().size() == 1
           && "OverviewPage liveProjectsModel must have exactly one entry");

    // ProvidersPage no longer initializes demo rows. Create persisted catalog
    // metadata through its real command path and observe Backend projections.
    assert(providersPage->property("providersSnapshot").toList().isEmpty());
    assert(QMetaObject::invokeMethod(
        providersPage, "saveProvider",
        Q_ARG(QVariant, false),
        Q_ARG(QVariant, QString()),
        Q_ARG(QVariant, QStringLiteral("QML local provider")),
        Q_ARG(QVariant, QStringLiteral("Local runtime")),
        Q_ARG(QVariant, QStringLiteral("http://127.0.0.1:11434/v1")),
        Q_ARG(QVariant, QString())));
    assert(waitForProperty(providersPage, "providersSnapshot",
        [](const QVariant& value) { return value.toList().size() == 1; }));
    const QString providerId = providersPage->property("providersSnapshot")
                                   .toList().constFirst().toMap()
                                   .value(QStringLiteral("providerId")).toString();
    assert(!providerId.isEmpty());
    assert(QMetaObject::invokeMethod(
        providersPage, "addModel",
        Q_ARG(QVariant, providerId),
        Q_ARG(QVariant, QStringLiteral("qml-local-model")),
        Q_ARG(QVariant, QStringLiteral("QML local model")),
        Q_ARG(QVariant, QStringLiteral("32K")),
        Q_ARG(QVariant, QStringLiteral("0.00")),
        Q_ARG(QVariant, QStringLiteral("0.00"))));
    assert(waitForProperty(providersPage, "modelsSnapshot",
        [](const QVariant& value) { return value.toList().size() == 1; }));

    // Comparison needs at least two independently persisted model slots. Add
    // a second catalog model through the same ProvidersPage command path so
    // modelChoices is proven live rather than supplied by a test fixture.
    assert(QMetaObject::invokeMethod(
        providersPage, "addModel",
        Q_ARG(QVariant, providerId),
        Q_ARG(QVariant, QStringLiteral("qml-local-model-2")),
        Q_ARG(QVariant, QStringLiteral("QML local model 2")),
        Q_ARG(QVariant, QStringLiteral("32K")),
        Q_ARG(QVariant, QStringLiteral("0.00")),
        Q_ARG(QVariant, QStringLiteral("0.00"))));
    assert(waitForProperty(providersPage, "modelsSnapshot",
        [](const QVariant& value) { return value.toList().size() == 2; }));

    // Create the first-class Manifest launch profile through the real dialog.
    // Its UI must collapse the fixed implementation/documentation stages and
    // persist exactly one Architect assignment for the dedicated runner.
    QObject* executionProfileDialog = waitForChild(window, "executionProfileDialog");
    assert(executionProfileDialog);
    assert(QMetaObject::invokeMethod(executionProfileDialog, "openForCreate"));
    QObject* profileNameField = waitForChild(executionProfileDialog, "profileNameField");
    QObject* profileWorkflowCombo = waitForChild(executionProfileDialog, "profileWorkflowCombo");
    QObject* profileImplementationLabel =
        waitForChild(executionProfileDialog, "profileImplementationLabel");
    assert(profileNameField && profileWorkflowCombo && profileImplementationLabel);
    assert(profileNameField->setProperty("text", QStringLiteral("QML Manifest profile")));
    assert(profileWorkflowCombo->setProperty("currentIndex", 1));
    assert(waitForProperty(executionProfileDialog, "manifestMode",
        [](const QVariant& value) { return value.toBool(); }));
    assert(!profileImplementationLabel->property("visible").toBool());
    assert(QMetaObject::invokeMethod(executionProfileDialog, "submit"));
    assert(waitForProperty(providersPage, "profilesSnapshot",
        [](const QVariant& value) { return value.toList().size() == 1; }));
    const QVariantMap manifestProfile = providersPage->property("profilesSnapshot")
                                            .toList().constFirst().toMap();
    assert(manifestProfile.value(QStringLiteral("workflowId")).toString()
           == QStringLiteral("execution-manifest-v1"));
    const QVariantList manifestAssignments =
        manifestProfile.value(QStringLiteral("assignments")).toList();
    assert(manifestAssignments.size() == 1);
    assert(manifestAssignments.constFirst().toMap()
               .value(QStringLiteral("stageType")).toString()
           == QStringLiteral("architect"));

    // 2) Comparison: select the live project through the QML intent signal,
    // open the lazily loaded real dialog through ComparisonPage's public
    // entry point, select two live catalog models, and submit the form. The
    // real execution manager starts immediately. The deliberately unreachable
    // local providers must therefore produce durable failed results; no fake
    // content, token usage, quality score, or synthesis state may appear.
    QObject* comparisonProjectsObject =
        comparisonPage->property("projectsModel").value<QObject*>();
    auto* comparisonProjectsModel =
        qobject_cast<QAbstractItemModel*>(comparisonProjectsObject);
    assert(comparisonProjectsModel == backend.projectController().model());
    assert(comparisonProjectsModel->rowCount() == 1);
    assert(waitForProperty(comparisonPage, "modelChoices",
        [](const QVariant& value) { return value.toList().size() >= 2; }));

    assert(QMetaObject::invokeMethod(
        comparisonPage, "projectSelectionRequested",
        Q_ARG(QString, projectId)));
    assert(waitForProperty(comparisonPage, "selectedProjectId",
        [&](const QVariant& value) { return value.toString() == projectId; }));
    assert(comparisonPage->property("canCreateComparison").toBool());

    assert(window->setProperty("currentSection", QStringLiteral("comparison")));
    assert(waitForProperty(window, "currentSection",
        [](const QVariant& value) {
            return value.toString() == QStringLiteral("comparison");
        }));
    assert(QMetaObject::invokeMethod(comparisonPage, "openNewComparisonDialog"));
    QObject* newComparisonDialog = waitForChild(window, "newComparisonDialog");
    checkpoint(newComparisonDialog ? "found newComparisonDialog" : "MISSING newComparisonDialog");
    assert(newComparisonDialog);
    QObject* comparisonTitleField = waitForChild(newComparisonDialog, "comparisonTitleField");
    QObject* comparisonTaskField = waitForChild(newComparisonDialog, "comparisonTaskField");
    checkpoint(comparisonTitleField ? "found comparisonTitleField" : "MISSING comparisonTitleField");
    checkpoint(comparisonTaskField ? "found comparisonTaskField" : "MISSING comparisonTaskField");
    assert(comparisonTitleField && comparisonTaskField);

    assert(comparisonTitleField->setProperty(
        "text", QStringLiteral("Durable QML comparison")));
    assert(comparisonTaskField->setProperty(
        "text", QStringLiteral("Compare two independent candidate answers.")));
    assert(QMetaObject::invokeMethod(
        newComparisonDialog, "setModelSelected",
        Q_ARG(QVariant, 0),
        Q_ARG(QVariant, true)));
    assert(QMetaObject::invokeMethod(
        newComparisonDialog, "setModelSelected",
        Q_ARG(QVariant, 1),
        Q_ARG(QVariant, true)));
    assert(QMetaObject::invokeMethod(newComparisonDialog, "recalculateSelectedCount"));
    assert(newComparisonDialog->property("selectedModelCount").toInt() >= 2);

    QAbstractItemModel* comparisonSessions =
        backend.comparisonController().sessionsModel();
    QAbstractItemModel* comparisonResults =
        backend.comparisonController().resultsModel();
    assert(comparisonSessions->rowCount() == 0);
    assert(comparisonResults->rowCount() == 0);
    assert(QMetaObject::invokeMethod(newComparisonDialog, "submit"));
    assert(waitForProperty(comparisonPage, "currentSessionStatus",
        [](const QVariant& value) {
            return value.toString() == QStringLiteral("failed");
        }));

    assert(comparisonSessions->rowCount() == 1);
    assert(comparisonResults->rowCount() == 2);
    const QModelIndex comparisonSessionIndex = comparisonSessions->index(0, 0);
    const QString comparisonSessionId = comparisonSessions->data(
        comparisonSessionIndex,
        ComparisonSessionListModel::SessionIdRole).toString();
    assert(!comparisonSessionId.isEmpty());
    assert(comparisonSessions->data(
               comparisonSessionIndex,
               ComparisonSessionListModel::ProjectIdRole).toString() == projectId);
    assert(comparisonSessions->data(
               comparisonSessionIndex,
               ComparisonSessionListModel::TitleRole).toString()
           == QStringLiteral("Durable QML comparison"));
    assert(waitForComparisonStatus(
               comparisonSessions,
               comparisonSessionId,
               {QStringLiteral("failed")})
        == QStringLiteral("failed"));
    assert(comparisonSessions->data(
               comparisonSessionIndex,
               ComparisonSessionListModel::ResultCountRole).toInt() == 2);
    assert(comparisonPage->property("selectedSessionId").toString()
           == comparisonSessionId);

    QStringList comparisonResultIds;
    for (int row = 0; row < comparisonResults->rowCount(); ++row) {
        const QModelIndex resultIndex = comparisonResults->index(row, 0);
        const QString resultId = comparisonResults->data(
            resultIndex, ComparisonResultListModel::ResultIdRole).toString();
        assert(!resultId.isEmpty());
        comparisonResultIds.push_back(resultId);
        assert(comparisonResults->data(
                   resultIndex, ComparisonResultListModel::StatusRole).toString()
               == QStringLiteral("failed"));
        assert(comparisonResults->data(
                   resultIndex, ComparisonResultListModel::ContentRole).toString().isEmpty());
        assert(!comparisonResults->data(
                    resultIndex, ComparisonResultListModel::ErrorTextRole).toString().isEmpty());
        assert(!comparisonResults->data(
                    resultIndex, ComparisonResultListModel::QualityAvailableRole).toBool());
        assert(!comparisonResults->data(
                    resultIndex, ComparisonResultListModel::CanSelectForSynthesisRole).toBool());
        assert(comparisonResults->data(
                   resultIndex, ComparisonResultListModel::InputTokensRole).toULongLong() == 0);
        assert(comparisonResults->data(
                   resultIndex, ComparisonResultListModel::OutputTokensRole).toULongLong() == 0);
    }
    assert(comparisonResultIds.at(0) != comparisonResultIds.at(1));

    const QVariantMap comparisonSummary =
        comparisonPage->property("summaryData").toMap();
    assert(!comparisonSummary.value(QStringLiteral("synthesisEnabled")).toBool());
    assert(!comparisonSummary.value(QStringLiteral("selectionReady")).toBool());
    assert(comparisonSummary.value(QStringLiteral("selectedResultIds")).toList().isEmpty());

    // Explicit session selection and refresh force the controller to reload
    // from its repositories. The session and both planned executions must
    // survive; this is the durable-storage assertion rather than a QML list
    // that happened to retain locally fabricated rows.
    assert(QMetaObject::invokeMethod(
        comparisonPage, "sessionSelectionRequested",
        Q_ARG(QString, comparisonSessionId)));
    assert(QMetaObject::invokeMethod(
        comparisonPage, "refreshRequested",
        Q_ARG(QString, comparisonSessionId)));
    assert(ai::ui::test_support::waitForCondition("Comparison refresh projection", [&] {
        return comparisonSessions->rowCount() == 1
            && comparisonResults->rowCount() == 2
            && comparisonPage->property("selectedSessionId").toString()
                == comparisonSessionId;
    }, [&] {
        return "sessions=" + std::to_string(comparisonSessions->rowCount())
            + ", results=" + std::to_string(comparisonResults->rowCount())
            + ", selected="
            + comparisonPage->property("selectedSessionId").toString().toStdString();
    }, 5s));

    // 3) "New run" on the Projects page must establish the selected project
    // context and open the canonical Runs dialog. It must not silently start a
    // sub-second Mock workflow behind the user's back.
    const bool orchestrationInvoked = QMetaObject::invokeMethod(
        projectsPage, "startOrchestration",
        Q_ARG(QVariant, projectId));
    checkpoint(orchestrationInvoked ? "startOrchestration invoked" : "startOrchestration invoke FAILED");
    assert(orchestrationInvoked);

    QAbstractItemModel* runsModel = backend.runController().model();
    checkpoint(("run rowCount=" + std::to_string(runsModel->rowCount())).c_str());
    assert(runsModel->rowCount() == 0);
    assert(backend.workspaceContextController().activeProjectId() == projectId);
    assert(waitForProperty(window, "currentSection",
        [](const QVariant& value) { return value.toString() == QStringLiteral("runs"); }));
    assert(waitForProperty(newRunDialog, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(runProjectCombo->property("currentValue").toString() == projectId);

    // Submit that visible dialog in explicit local-test mode. This retains a
    // deterministic completed-run assertion without making Mock the product
    // action or issuing any network request from the test suite.
    assert(runTitleField->setProperty("text", QStringLiteral("Completed local test")));
    assert(taskField->setProperty("text", QStringLiteral("Verify the local orchestration pipeline")));
    assert(realProviderCheck->setProperty("checked", false));
    assert(automaticVerificationRepairCheck->setProperty("checked", false));
    assert(QMetaObject::invokeMethod(newRunDialog, "submit"));
    assert(waitForRowCount(runsModel, 1));
    const QString firstRunId = runsModel->data(
        runsModel->index(0, 0), RunListModel::RunIdRole).toString();
    assert(!firstRunId.isEmpty());

    // The unchecked QML cost-control crossed the real
    // signal/controller/SQLite boundary. Mock does not execute combined
    // verification, but it persists the exact same immutable Run input.
    {
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteRunRepository runs(database.value());
        const auto stored = runs.findRun(firstRunId.toStdString());
        assert(stored.success() && stored.value().has_value());
        assert(stored.value()->verificationRepairPolicy
            == ai::application::VerificationRepairPolicy::Disabled);
    }

    // RunsPage must reflect the same stable-ID selection the adapter already made.
    assert(runsPage->property("selectedRunId").toString() == firstRunId);

    const QString finalStatus = waitForStatus(runsModel, firstRunId, {QStringLiteral("completed")});
    checkpoint(("firstRun finalStatus=" + finalStatus.toStdString()).c_str());
    assert(finalStatus == QStringLiteral("completed"));

    // The completed Mock run must surface its Backend-owned review artifact
    // through RunController and the RunsPage mirror without exposing a path.
    const bool artifactVisible = waitForProperty(runsPage, "artifactsSnapshot",
        [](const QVariant& value) { return !value.toList().isEmpty(); });
    assert(artifactVisible && "RunsPage did not receive the completed run artifact");
    const QVariantMap artifact = runsPage->property("artifactsSnapshot")
                                     .toList().constFirst().toMap();
    assert(artifact.value(QStringLiteral("runId")).toString() == firstRunId);
    assert(!artifact.value(QStringLiteral("artifactId")).toString().isEmpty());
    assert(artifact.value(QStringLiteral("byteSize")).toULongLong() > 0);
    assert(!artifact.contains(QStringLiteral("relativePath")));

    // The stage timeline for the selected run must be fully populated and completed.
    QAbstractItemModel* stageModel = backend.runController().stageModel();
    assert(stageModel->rowCount() == 4);
    for (int row = 0; row < stageModel->rowCount(); ++row) {
        assert(stageModel->data(stageModel->index(row, 0), RunStageModel::StatusRole).toString()
            == QStringLiteral("completed"));
    }

    // ── Overview: runsSnapshot populated after run completion ─────────────────
    // OverviewPage's runsMirror Repeater fires rebuildRunsSnapshot() on each
    // status/progress change; by now at least one completed run must be present.
    const bool runsSnapshotPopulated = waitForProperty(overviewPage, "runsSnapshot",
        [](const QVariant& v) { return !v.toList().isEmpty(); });
    checkpoint(("overviewPage runsSnapshot size=" + std::to_string(overviewPage->property("runsSnapshot").toList().size())).c_str());
    assert(runsSnapshotPopulated && "OverviewPage runsSnapshot is empty after run completed");

    // liveProjectsModel must now show runCount >= 1 for the selected project.
    const QVariantList liveProjects = overviewPage->property("liveProjectsModel").toList();
    assert(!liveProjects.isEmpty());
    const QVariantMap projectEntry = liveProjects.constFirst().toMap();
    const int runCount = projectEntry.value(QStringLiteral("runCount"), 0).toInt();
    checkpoint(("overview project runCount=" + std::to_string(runCount)).c_str());
    assert(runCount >= 1 && "liveProjectsModel must count completed run for the project");

    const QDateTime futureDate = QDateTime::currentDateTime().addDays(1);
    assert(overviewFromDatePicker->setProperty("selectedDate", futureDate));
    assert(overviewToDatePicker->setProperty("selectedDate", futureDate));
    assert(QMetaObject::invokeMethod(overviewPage, "applyDateRange"));
    assert(waitForProperty(overviewPage, "liveProjectsModel",
        [](const QVariant& value) {
            return value.toList().isEmpty();
        }) && "Overview date range did not exclude projects created outside the selected day");
    assert(waitForProperty(overviewPage, "selectedProjectId",
        [](const QVariant& value) { return value.toString().isEmpty(); })
        && "Overview retained a project selection hidden by the creation-date range");
    assert(QMetaObject::invokeMethod(overviewPage, "clearDateRange"));
    assert(waitForProperty(overviewPage, "liveProjectsModel",
        [](const QVariant& value) {
            const QVariantList projects = value.toList();
            return !projects.isEmpty()
                && projects.constFirst().toMap().value(QStringLiteral("runCount")).toInt() >= 1;
        }) && "Clearing the Overview date range did not restore all-time data");
    assert(waitForProperty(overviewPage, "selectedProjectId",
        [&](const QVariant& value) { return value.toString() == projectId; }));

    // 4) Starting a run through RunsPage's own "New run" path and cancelling it
    // while it is RUNNING (RunManager::requestCancellation handles both the
    // queued and the already-running case, both already proven at the adapter
    // level in QtAdapterTests.cpp/ApplicationPipelineTests.cpp) must reach a
    // correctly displayed final Cancelled state. The Mock workflow would finish
    // in ~320ms and could race past "running" before this process observes it,
    // so the Run is HELD at a barrier (RunBarrier, armed below, released by the
    // cancellation itself): the test waits for the barrier to hold that very
    // Run and for the UI to show it as running, and only then cancels. Nothing
    // here depends on how long anything takes; the waits are watchdogs.
    // Submit through the real NewRunDialog and verify its large Task reaches
    // the TaskRole verbatim before cancellation.
    const QString longTask = makeLongMultilineText(
        QStringLiteral("Run objective and acceptance criteria"),
        QStringLiteral("Task line"));
    assert(longTask.size() > 1000);
    assert(QMetaObject::invokeMethod(newRunDialog, "open"));
    assert(waitForProperty(newRunDialog, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(waitForProperty(runProjectCombo, "currentValue",
        [&](const QVariant& value) { return value.toString() == projectId; })
        && "New Run did not preselect the active project");
    assert(runTitleField->setProperty("text", QStringLiteral("Cancel from RunsPage")));
    assert(taskField->setProperty("text", longTask));
    // The product defaults to real providers when configured. Automated UI
    // tests must explicitly stay on the deterministic, non-network Mock path.
    assert(realProviderCheck->setProperty("checked", false));
    assert(waitForProperty(taskCounter, "text", [&](const QVariant& value) {
        const QString text = value.toString();
        return text.contains(QString::number(longTask.size()))
            && text.contains(QStringLiteral("KB"));
    }));
    assert(waitForProperty(taskFlickable, "contentHeight", [&](const QVariant& value) {
        return value.toDouble() > taskFlickable->property("height").toDouble();
    }));
    const QString taskCounterText = taskCounter->property("text").toString();
    assert(taskCounterText.contains(QString::number(longTask.size())));
    assert(taskCounterText.contains(QStringLiteral("KB")));
    assert(taskFlickable->property("contentHeight").toDouble()
           > taskFlickable->property("height").toDouble());
    // The NEXT Run that reaches a step is the one the barrier holds; the first Run of this test has long finished.
    runBarrier->arm();
    const bool startInvoked = QMetaObject::invokeMethod(newRunDialog, "submit");
    assert(startInvoked);
    assert(waitForRowCount(runsModel, 2));
    QString secondRunId;
    for (int row = 0; row < runsModel->rowCount(); ++row) {
        const QModelIndex index = runsModel->index(row, 0);
        const QString candidateId = runsModel->data(index, RunListModel::RunIdRole).toString();
        if (!candidateId.isEmpty() && candidateId != firstRunId) {
            secondRunId = candidateId;
            break;
        }
    }
    assert(!secondRunId.isEmpty());
    assert(secondRunId != firstRunId);

    // The barrier holds exactly the Run the UI shows, and the UI shows it as RUNNING - a state that cannot change until the
    // cancellation below, whatever the machine is doing.
    assert(ai::ui::test_support::waitForCondition(
        "the barrier holds the new Run", [&] { return runBarrier->held(); },
        [&] { return std::string("held=") + (runBarrier->held() ? "true" : "false"); }, 30s));
    assert(runBarrier->heldRunId() == secondRunId.toStdString()
        && "the barrier held another Run than the one the UI shows");
    const QString runningStatus = waitForStatus(runsModel, secondRunId, {QStringLiteral("running")}, 30s);
    checkpoint(("secondRun runningStatus=" + runningStatus.toStdString()).c_str());
    assert(runningStatus == QStringLiteral("running"));

    // Cancel the running Run through the same QML function the Cancel button calls. The cancellation is what releases the barrier.
    const bool cancelInvoked = QMetaObject::invokeMethod(
        runsPage, "cancelRun",
        Q_ARG(QVariant, secondRunId));
    assert(cancelInvoked);

    const QString cancelledStatus = waitForStatus(runsModel, secondRunId, {QStringLiteral("cancelled")}, 30s);
    checkpoint(("secondRun cancelledStatus=" + cancelledStatus.toStdString()).c_str());
    assert(cancelledStatus == QStringLiteral("cancelled"));
    assert(waitForProperty(runsPage, "selectedRunId",
        [&](const QVariant& value) { return value.toString() == secondRunId; })
        && "RunsPage did not asynchronously select the newly created Run");

    bool taskRoundTripped = false;
    for (int row = 0; row < runsModel->rowCount(); ++row) {
        const QModelIndex index = runsModel->index(row, 0);
        if (runsModel->data(index, RunListModel::RunIdRole).toString() == secondRunId) {
            taskRoundTripped = runsModel->data(index, RunListModel::TaskRole).toString() == longTask;
            break;
        }
    }
    assert(taskRoundTripped && "NewRunDialog Task did not round-trip through the Qt model");

    assert(waitForProperty(runDetailsPanel, "runData",
        [&](const QVariant& value) {
            const QVariantMap runData = value.toMap();
            return runData.value(QStringLiteral("runId")).toString() == secondRunId
                && runData.value(QStringLiteral("task")).toString() == longTask;
        }) && "RunDetailsPanel did not receive the selected Run snapshot");
    assert(runDetailsPanel->setProperty("contextExpanded", true));
    assert(waitForProperty(runDetailsPanel, "contextExpanded",
        [](const QVariant& value) { return value.toBool(); })
        && "RunDetailsPanel reset contextExpanded after selecting the Run");
    // AG-16.2 grew this binary (more ai::application includes/seeding code
    // later in main()) enough to push process/QML-engine startup weight up,
    // which made this pre-existing layout-settle wait marginal against the
    // default 5s budget even though it settles instantly once running - a
    // wider timeout here changes no assertion semantics, just the budget.
    const bool detailsOverflow = waitForProperty(runDetailsFlickable, "contentHeight",
        [&](const QVariant& value) {
            return value.toDouble() > runDetailsFlickable->property("height").toDouble();
        }, 15s);
    checkpoint(("run details expanded="
        + std::to_string(runDetailsPanel->property("contextExpanded").toBool())
        + " contentHeight=" + std::to_string(runDetailsFlickable->property("contentHeight").toDouble())
        + " height=" + std::to_string(runDetailsFlickable->property("height").toDouble())
        + " maximumY=" + std::to_string(runDetailsPanel->property("detailMaximumY").toDouble())).c_str());
    assert(detailsOverflow && "Expanded Run context did not produce scrollable detail content");
    const double detailMaximumY =
        runDetailsPanel->property("detailMaximumY").toDouble();
    assert(detailMaximumY > 24.0);
    assert(runDetailsFlickable->setProperty("contentY", detailMaximumY / 2.0));
    assert(waitForProperty(runDetailsJumpToBottom, "visible",
        [](const QVariant& value) { return value.toBool(); })
        && "Jump to Bottom did not appear after scrolling expanded Run context");
    assert(QMetaObject::invokeMethod(runDetailsFlickable, "jumpToBottom"));
    assert(waitForProperty(runDetailsJumpToBottom, "visible",
        [](const QVariant& value) { return !value.toBool(); })
        && "Jump to Bottom remained visible after reaching the bottom");

    // ── Overview: both runs appear in runsSnapshot after cancel ───────────────
    const bool twoRunsVisible = waitForProperty(overviewPage, "runsSnapshot",
        [](const QVariant& v) { return v.toList().size() >= 2; });
    checkpoint(("overviewPage runsSnapshot after cancel size=" + std::to_string(overviewPage->property("runsSnapshot").toList().size())).c_str());
    assert(twoRunsVisible && "OverviewPage runsSnapshot must contain both runs after cancel");

    // Backend ordering is oldest-first. Overview must still identify the new
    // cancelled run as latest instead of falling back to the first completed
    // run's 100% progress.
    const bool latestRunVisible = waitForProperty(overviewPage, "liveProjectsModel",
        [](const QVariant& value) {
            const QVariantList projects = value.toList();
            if (projects.isEmpty()) {
                return false;
            }
            const QVariantMap project = projects.constFirst().toMap();
            return project.value(QStringLiteral("activeRunStatus")).toString()
                       == QStringLiteral("cancelled")
                && project.value(QStringLiteral("completedCount")).toInt() == 1
                && project.value(QStringLiteral("cancelledCount")).toInt() == 1;
        });
    assert(latestRunVisible && "Overview did not select the newest terminal run");

    // A planning-level no-op has no frozen manifest. Its durable report must
    // nevertheless be prominent in Run Details, rather than looking like
    // missing execution data in the generic artifact list.
    QVariantMap noChangesArtifact;
    noChangesArtifact.insert(QStringLiteral("artifactId"), QStringLiteral("planner-report-1"));
    noChangesArtifact.insert(
        QStringLiteral("name"), QStringLiteral("manifest-planning-no-changes.md"));
    noChangesArtifact.insert(QStringLiteral("type"), QStringLiteral("report"));
    QVariantMap noChangesRunData = runDetailsPanel->property("runData").toMap();
    noChangesRunData.insert(QStringLiteral("artifacts"), QVariantList {noChangesArtifact});
    assert(runDetailsPanel->setProperty("runData", noChangesRunData));
    assert(waitForProperty(plannerOutcomeCard, "visible",
        [](const QVariant& value) { return value.toBool(); })
        && "Run Details did not surface the planner no-changes report");
    assert(viewPlannerReportButton->property("visible").toBool());

    // Older manifest Runs may predate the durable verification record. They
    // must remain explicitly neutral/not-run instead of receiving a
    // retroactive green state. A current record exposes the viewer action,
    // but the compact card still does not infer a pass from presence alone.
    QVariantMap legacyManifestRunData = noChangesRunData;
    legacyManifestRunData.insert(
        QStringLiteral("workflowId"), QStringLiteral("execution-manifest-v1"));
    legacyManifestRunData.insert(QStringLiteral("artifacts"), QVariantList {});
    assert(runDetailsPanel->setProperty("runData", legacyManifestRunData));
    assert(waitForProperty(manifestVerificationCard, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(!viewManifestVerificationButton->property("visible").toBool());
    assert(manifestVerificationSummary->property("text").toString().contains(
        QStringLiteral("not available or not run"), Qt::CaseInsensitive));
    assert(manifestVerificationSummary->property("text").toString().contains(
        QStringLiteral("not treated as passed"), Qt::CaseInsensitive));
    assert(workspaceProvenanceCard->property("visible").toBool());
    assert(!viewWorkspaceBaselineButton->property("visible").toBool());
    assert(!viewWorkspaceProvenanceButton->property("visible").toBool());
    assert(workspaceProvenanceSummary->property("text").toString().contains(
        QStringLiteral("not available"), Qt::CaseInsensitive));

    QVariantMap verificationArtifact;
    verificationArtifact.insert(
        QStringLiteral("artifactId"), QStringLiteral("manifest-verification-1"));
    verificationArtifact.insert(
        QStringLiteral("name"), QStringLiteral("manifest-verification.md"));
    verificationArtifact.insert(QStringLiteral("type"), QStringLiteral("report"));
    QVariantMap verifiedManifestRunData = legacyManifestRunData;
    verifiedManifestRunData.insert(
        QStringLiteral("artifacts"), QVariantList {verificationArtifact});
    assert(runDetailsPanel->setProperty("runData", verifiedManifestRunData));
    assert(waitForProperty(viewManifestVerificationButton, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(manifestVerificationSummary->property("text").toString().contains(
        QStringLiteral("Recorded separately"), Qt::CaseInsensitive));

    QVariantMap baselineArtifact;
    baselineArtifact.insert(
        QStringLiteral("artifactId"), QStringLiteral("workspace-baseline-1"));
    baselineArtifact.insert(
        QStringLiteral("name"), QStringLiteral("workspace-baseline.v1.json"));
    baselineArtifact.insert(QStringLiteral("type"), QStringLiteral("report"));
    QVariantMap provenanceArtifact;
    provenanceArtifact.insert(
        QStringLiteral("artifactId"), QStringLiteral("workspace-provenance-1"));
    provenanceArtifact.insert(
        QStringLiteral("name"), QStringLiteral("manifest-workspace-provenance.md"));
    provenanceArtifact.insert(QStringLiteral("type"), QStringLiteral("report"));
    QVariantMap provenanceRunData = verifiedManifestRunData;
    provenanceRunData.insert(QStringLiteral("artifacts"), QVariantList {
        verificationArtifact, baselineArtifact, provenanceArtifact});
    assert(runDetailsPanel->setProperty("runData", provenanceRunData));
    assert(waitForProperty(viewWorkspaceBaselineButton, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(waitForProperty(viewWorkspaceProvenanceButton, "visible",
        [](const QVariant& value) { return value.toBool(); }));
    assert(workspaceProvenanceSummary->property("text").toString().contains(
        QStringLiteral("exact fingerprint"), Qt::CaseInsensitive));

    // Durable Run warnings (a diagnostic artifact that could not be stored): a Run without any has no section; with
    // warnings the section appears and shows every one of them.
    QObject* runWarningsSection = waitForChild(window, "runWarningsSection");
    assert(runWarningsSection && "Run Details has no warnings section");
    assert(!runWarningsSection->property("visible").toBool()
        && "A Run without warnings must not show a warnings section");
    QVariantMap warningOne;
    warningOne.insert(QStringLiteral("warningId"), QStringLiteral("warning-1"));
    warningOne.insert(QStringLiteral("message"),
        QStringLiteral("Diagnostic artifact 'manifest-run-report.md' was not saved (disk is full)"));
    QVariantMap warningTwo = warningOne;
    warningTwo.insert(QStringLiteral("warningId"), QStringLiteral("warning-2"));
    QVariantMap warningRunData = provenanceRunData;
    warningRunData.insert(QStringLiteral("warnings"), QVariantList {warningOne, warningTwo});
    assert(runDetailsPanel->setProperty("runData", warningRunData));
    assert(waitForProperty(runWarningsSection, "visible",
        [](const QVariant& value) { return value.toBool(); })
        && "Run Details did not show the warnings of the Run");
    assert(runDetailsPanel->property("runWarnings").toList().size() == 2);
    // Repeater delegates are visual children of the section (not QObject children): count them there.
    const auto warningRowCount = [runWarningsSection] {
        int rows = 0;
        const auto* section = qobject_cast<QQuickItem*>(runWarningsSection);
        for (QQuickItem* child : section->childItems()) {
            if (child->objectName() == QStringLiteral("runWarningRow")) ++rows;
        }
        return rows;
    };
    assert(ai::ui::test_support::waitForCondition("Run warning rows", [&] {
        return warningRowCount() == 2;
    }, [&] {
        return "rows=" + std::to_string(warningRowCount());
    }, 5s) && "Every warning must be listed");
    assert(runDetailsPanel->setProperty("runData", provenanceRunData));
    assert(waitForProperty(runWarningsSection, "visible",
        [](const QVariant& value) { return !value.toBool(); })
        && "The warnings section must disappear with the warnings");

    // ── Settings: the folder of the temporary verification copies. The page only sends what was typed; the Backend decides, and its
    // refusal is shown in its own words while the previous setting stays in force.
    {
        QObject* settingsPage = waitForChild(window, "settingsPage");
        QObject* verificationScratchField = waitForChild(window, "verificationScratchField");
        QObject* verificationScratchStatus = waitForChild(window, "verificationScratchStatus");
        QObject* verificationScratchDefaultButton = waitForChild(window, "verificationScratchDefaultButton");
        assert(settingsPage && verificationScratchField && verificationScratchStatus && verificationScratchDefaultButton
            && "The Settings page has no verification folder control");
        assert(verificationScratchField->property("text").toString().isEmpty()
            && "No folder is set at the start: the field is empty and the placeholder shows the default");
        assert(!verificationScratchField->property("placeholderText").toString().isEmpty());
        assert(verificationScratchStatus->property("text").toString().startsWith(QStringLiteral("In use:")));
        assert(!verificationScratchDefaultButton->property("actionEnabled").toBool()
            && "There is nothing to reset while the default is in use");

        // A relative path: refused by the Backend, shown, and nothing stored.
        assert(verificationScratchField->setProperty("text", QStringLiteral("relative/folder")));
        assert(QMetaObject::invokeMethod(settingsPage, "applyVerificationFolder"));
        assert(waitForProperty(verificationScratchStatus, "text",
                   [](const QVariant& value) { return value.toString().contains(QStringLiteral("absolute path")); })
            && "The Backend's refusal must be shown as it was worded");
        assert(!verificationScratchDefaultButton->property("actionEnabled").toBool()
            && "A refused folder must not have been stored");

        // A good folder: created by the Backend, stored, shown as in use.
        const fs::path chosen = fs::temp_directory_path()
            / ("aio-qml-vf-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
        assert(verificationScratchField->setProperty("text", QString::fromStdWString(chosen.wstring())));
        assert(QMetaObject::invokeMethod(settingsPage, "applyVerificationFolder"));
        assert(waitForProperty(verificationScratchStatus, "text",
                   [](const QVariant& value) { return value.toString().startsWith(QStringLiteral("In use:")); }));
        assert(fs::is_directory(chosen) && "The Backend creates a missing folder");
        assert(waitForProperty(verificationScratchDefaultButton, "actionEnabled",
            [](const QVariant& value) { return value.toBool(); }));

        // Back to the default.
        assert(QMetaObject::invokeMethod(verificationScratchDefaultButton, "clicked"));
        assert(waitForProperty(verificationScratchField, "text",
            [](const QVariant& value) { return value.toString().isEmpty(); }));
        assert(waitForProperty(verificationScratchDefaultButton, "actionEnabled",
            [](const QVariant& value) { return !value.toBool(); }));
        std::error_code ignored;
        fs::remove_all(chosen, ignored);
    }

    // ── AG-16.2: ManifestApplyVerificationGate projection through the real
    // AgentSessionPanel.qml. Seeds a Manifest-workflow Run + real
    // ReadyForReview proposal directly via a second short-lived
    // BackendRuntime on the same on-disk database (no real Planner/provider
    // execution - the same technique QtAdapterTests.cpp already
    // establishes), then reuses runDetailsPanel.runData's already-proven
    // "settable synthetic map" mechanism (see the manifest-verification-card
    // checks above) instead of driving RunsPage's own calendar/list
    // selection - this test is about the eligibility projection, not
    // Run-list navigation, which the rest of this file already covers.
    QObject* agentSessionPanel = waitForChild(window, "agentSessionPanel");
    assert(agentSessionPanel && "AgentSessionPanel was not found in the loaded QML tree");
    QObject* applyToWorkspaceButton = waitForChild(window, "applyToWorkspaceButton");
    QObject* applyVerificationStatusRow = waitForChild(window, "applyVerificationStatusRow");
    QObject* applyVerificationStatusText = waitForChild(window, "applyVerificationStatusText");
    QObject* agentSessionErrorText = waitForChild(window, "agentSessionErrorText");
    assert(applyToWorkspaceButton && applyVerificationStatusRow && applyVerificationStatusText
        && agentSessionErrorText);
    QObject* agentSessionControllerObject =
        agentSessionPanel->property("controller").value<QObject*>();
    assert(agentSessionControllerObject
        && "AgentSessionPanel.controller was not set - cannot poll session-load state");

    auto seedManifestProposal = [&](
            const std::string& runId, const std::string& relativePath, const std::string& content,
            ai::application::RunStatus runStatus = ai::application::RunStatus::Completed) {
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());

        ai::application::Run run;
        run.id = runId;
        run.projectId = projectId.toStdString();
        run.title = "AG-16.2 QML fixture run";
        run.task = "Exercise the apply-verification gate QML projection";
        run.workflowId = ai::application::kExecutionManifestWorkflowId;
        run.workspacePath = workspace.projectPath;
        run.status = runStatus;
        run.createdAt = std::chrono::system_clock::now();
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteRunRepository runs(database.value());
        assert(runs.insertRun(run).success());

        ai::application::AgentToolSession toolSession;
        toolSession.sessionId = "manifest-session-" + runId;
        toolSession.runId = runId;
        toolSession.providerId = "test-provider";
        toolSession.modelId = "test-model";
        toolSession.state = ai::application::AgentToolSessionState::Completed;
        toolSession.createdAt = run.createdAt;
        assert(seed.agentToolSessionRepository().insertSession(toolSession).success());

        ai::application::CreateChangeProposalRequest request;
        request.runId = runId;
        request.projectId = projectId.toStdString();
        request.providerId = "test-provider";
        request.modelId = "test-model";
        request.agentRole = "workspace-access";
        request.rawModelOutput = "```json\n{\"schemaVersion\":\"proposal-submission.v2\","
            "\"summary\":\"Add a file\",\"changes\":[{\"change_id\":\"c1\",\"operation\":\"create\","
            "\"relative_path\":\"" + relativePath + "\",\"source_path\":null,"
            "\"proposed_content\":\"" + content + "\"}]}\n```";
        request.policy.projectRoot = workspace.projectPath;
        const auto created = seed.changeProposalService().createFromModelOutput(std::move(request));
        assert(created.success());
        assert(created.value().state == ai::application::ChangeSetState::ReadyForReview);
        return created.value().changeSetId;
    };

    const std::string eligibleRunId = "ag16-2-qml-eligible-run";
    const std::string eligibleChangeSetId =
        seedManifestProposal(eligibleRunId, "qml-eligible.txt", "eligible content");
    {
        // A structurally-valid, fully-passed verification whose stored hash
        // is computed with the exact same canonical algorithm production
        // code uses (ManifestProposalContentHash.hpp) - never a hand-rolled
        // hash that would coincidentally happen to match.
        ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
        assert(seed.storageAvailable());
        const auto changes = seed.changeProposalService().listChanges(eligibleChangeSetId);
        assert(changes.success());
        std::vector<ai::application::ManifestProposalContentHashChangeInput> hashInputs;
        for (const auto& change : changes.value()) {
            ai::application::ManifestProposalContentHashChangeInput input;
            input.operation = change.operation;
            input.relativePath = change.relativePath;
            input.sourcePath = change.sourcePath;
            if (change.operation == ai::application::FileChangeOperation::Create
                    || change.operation == ai::application::FileChangeOperation::Modify) {
                const auto content = seed.changeProposalService().readProposedContent(
                    eligibleChangeSetId, change.changeId);
                assert(content.success());
                input.contentSha256 = ai::application::sha256Hex(content.value());
            }
            hashInputs.push_back(std::move(input));
        }
        constexpr const char* kBaselineFingerprint = "ai-orchestrator-qml-smoke-baseline";
        ai::application::ManifestProposalSetVerification verification;
        verification.workspaceIdentity = projectId.toStdString();
        verification.baselineFingerprint = kBaselineFingerprint;
        verification.overlayFingerprint = "ai-orchestrator-qml-smoke-overlay";
        verification.orderedProposalIds = {eligibleChangeSetId};
        verification.orderedProposalContentHashes = {
            ai::application::computeManifestProposalContentHash(eligibleChangeSetId, hashInputs)};
        verification.proposalSafetyStatus = ai::application::ExecutionVerificationStatus::Passed;
        verification.buildLintTestStatus = ai::application::ExecutionVerificationStatus::Passed;
        verification.acceptanceStatus = ai::application::ExecutionVerificationStatus::Passed;
        ai::application::ManifestWorkLedgerEntry entry;
        entry.runId = eligibleRunId;
        entry.projectId = projectId.toStdString();
        entry.terminalStatus = ai::application::RunStatus::Completed;
        entry.baselineFingerprint = kBaselineFingerprint;
        entry.disposition = ai::application::ManifestWorkLedgerDisposition::ManifestReady;
        ai::application::ManifestWorkLedgerTaskDisposition disposition;
        disposition.taskId = "task-1";
        disposition.status = "completed";
        disposition.changeSetId = eligibleChangeSetId;
        entry.taskDispositions = {disposition};
        entry.verification = verification;
        entry.createdAt = std::chrono::system_clock::now();
        const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(database.success());
        ai::infrastructure::SqliteExecutionManifestRepository manifests(database.value());
        ai::application::ExecutionManifest manifest;
        manifest.runId = eligibleRunId;
        assert(manifests.insert(std::move(manifest)).success());
        ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database.value());
        assert(ledger.insertEntry(entry).success());
    }

    // Blocked: a second Manifest Run/proposal with no Work Ledger
    // verification at all - deliberately the simplest possible denial
    // shape, since every specific reasonCode is already exhaustively
    // covered by QtAdapterTests.cpp; this smoke test is about the QML
    // projection/rendering, not re-testing the gate's own decision logic.
    const std::string blockedRunId = "ag16-2-qml-blocked-run";
    const std::string blockedChangeSetId =
        seedManifestProposal(blockedRunId, "qml-blocked.txt", "blocked content");

    // Real Runs navigation, not synthetic runData - RunDetailsPanel.runData
    // is a live binding to RunsPage.selectedRun (itself derived from
    // RunController's runsSnapshot/selectedRunId), which keeps reasserting
    // itself over any direct C++ override the moment the runs list
    // refreshes. The eligible/blocked runs seeded above are real durable
    // rows, so they are selected through the real RunController exactly
    // like every other Run in this file, instead of fighting that live
    // binding with synthetic data.
    auto waitForRunDataId = [&](const std::string& runId, const char* label) {
        return ai::ui::test_support::waitForCondition(label, [&] {
            return runDetailsPanel->property("runData").toMap()
                .value(QStringLiteral("runId")).toString() == QString::fromStdString(runId);
        }, [&] {
            return std::string("runDetailsPanel.runData.runId=")
                + runDetailsPanel->property("runData").toMap()
                      .value(QStringLiteral("runId")).toString().toStdString();
        }, 5s);
    };
    // selectRun() dispatches the session/proposal projection load on a
    // background thread (QtConcurrent) - selectProposalId() only reads
    // already-loaded in-memory proposals, so callers must wait for that
    // load to land (hasSession) rather than racing it, or it silently
    // finds nothing.
    auto waitForHasSession = [&](const char* label) {
        return ai::ui::test_support::waitForCondition(label, [&] {
            return agentSessionControllerObject->property("hasSession").toBool();
        }, [&] {
            return std::string("controller.hasSession still false; errorText=")
                + agentSessionControllerObject->property("errorText").toString().toStdString();
        }, 5s);
    };

    backend.runController().refresh(projectId);
    assert(ai::ui::test_support::waitForCondition("Eligible run visible in RunsPage", [&] {
        const QVariantList snapshot = runsPage->property("runsSnapshot").toList();
        for (const QVariant& entry : snapshot) {
            if (entry.toMap().value(QStringLiteral("runId")).toString()
                    == QString::fromStdString(eligibleRunId)) {
                return true;
            }
        }
        return false;
    }, [] { return std::string("eligible run not present in RunsPage.runsSnapshot"); }, 5s));

    // B.13: null safety before anything is selected - selecting a Run with
    // a real agent session but no proposal selected yet must not emit a
    // QML warning (cumulative qmlWarnings is asserted empty at the end of
    // main()).
    backend.runController().selectRun(QString::fromStdString(eligibleRunId));
    assert(waitForRunDataId(eligibleRunId, "Eligible runData bound"));
    assert(waitForHasSession("Eligible session loaded"));

    // B.8/B.9/B.10: select the eligible proposal - the action is enabled,
    // the reason line reads "Verified", and it is never styled as blocked.
    assert(QMetaObject::invokeMethod(
        agentSessionPanel, "selectProposalId",
        Q_ARG(QVariant, QString::fromStdString(eligibleChangeSetId))));
    assert(ai::ui::test_support::waitForCondition("Eligible status text", [&] {
        return applyVerificationStatusText->property("text").toString()
            == QStringLiteral("Verified");
    }, [&] {
        return applyVerificationStatusText->property("text").toString().toStdString();
    }, 5s));
    assert(applyVerificationStatusRow->property("visible").toBool());
    assert(applyToWorkspaceButton->property("enabled").toBool()
        && "Apply action must be enabled for an eligible, verified proposal");

    // B.12: context switching resets eligibility state - selecting a
    // different Run's proposal must never leak the previous proposal's
    // eligibility/status text.
    backend.runController().selectRun(QString::fromStdString(blockedRunId));
    assert(waitForRunDataId(blockedRunId, "Blocked runData bound"));
    assert(ai::ui::test_support::waitForCondition("Blocked session load", [&] {
        return agentSessionPanel->property("selectedChangeSetId").toString().isEmpty();
    }, [] { return std::string("selectedChangeSetId did not reset on Run switch"); }, 5s));
    assert(waitForHasSession("Blocked session loaded"));
    assert(QMetaObject::invokeMethod(
        agentSessionPanel, "selectProposalId",
        Q_ARG(QVariant, QString::fromStdString(blockedChangeSetId))));
    assert(ai::ui::test_support::waitForCondition("Blocked status text", [&] {
        return applyVerificationStatusText->property("text").toString()
            != QStringLiteral("Verified")
            && !applyVerificationStatusText->property("text").toString().isEmpty();
    }, [&] {
        return applyVerificationStatusText->property("text").toString().toStdString();
    }, 5s));
    // B.10: a blocked proposal must never carry the "Verified" success text,
    // and the action must be disabled - this is the "no success styling on
    // failure" guarantee, expressed the same way the rest of this file
    // asserts rendered content (text/enabled), not raw pixel colors.
    assert(applyVerificationStatusText->property("text").toString()
        != QStringLiteral("Verified"));
    assert(applyVerificationStatusText->property("text").toString().startsWith(
        QStringLiteral("manifest_apply.verification_missing:")));
    assert(!applyToWorkspaceButton->property("enabled").toBool()
        && "Apply action must be disabled for a proposal without a passed verification");

    // A direct invocation cannot bypass the disabled QML action: Backend
    // refuses it and the panel renders the exact bounded error code together
    // with its message.
    bool blockedApplyResult = true;
    assert(QMetaObject::invokeMethod(
        agentSessionControllerObject, "approveAndApply",
        Q_RETURN_ARG(bool, blockedApplyResult),
        Q_ARG(QString, QString::fromStdString(blockedChangeSetId))));
    assert(!blockedApplyResult);
    assert(ai::ui::test_support::waitForCondition("Backend rejection code rendered", [&] {
        return agentSessionErrorText->property("text").toString().startsWith(
            QStringLiteral("manifest_apply.verification_missing:"));
    }, [&] {
        return agentSessionErrorText->property("text").toString().toStdString();
    }, 5s));

    // B.11: a successful apply immediately renders Applied and disables the
    // action - switch back to the eligible Run/proposal, apply it, and
    // confirm the whole apply-button row disappears (state !== ready_for_review).
    backend.runController().selectRun(QString::fromStdString(eligibleRunId));
    assert(waitForRunDataId(eligibleRunId, "Re-select eligible runData bound"));
    assert(waitForHasSession("Re-selected eligible session loaded"));
    assert(QMetaObject::invokeMethod(
        agentSessionPanel, "selectProposalId",
        Q_ARG(QVariant, QString::fromStdString(eligibleChangeSetId))));
    assert(ai::ui::test_support::waitForCondition("Re-selected eligible status text", [&] {
        return applyVerificationStatusText->property("text").toString()
            == QStringLiteral("Verified");
    }, [] { return std::string("did not re-settle on Verified"); }, 5s));
    bool approveAndApplyResult = false;
    assert(QMetaObject::invokeMethod(
        agentSessionControllerObject, "approveAndApply",
        Q_RETURN_ARG(bool, approveAndApplyResult),
        Q_ARG(QString, QString::fromStdString(eligibleChangeSetId))));
    assert(approveAndApplyResult && "approveAndApply() did not succeed for the eligible proposal");
    assert(ai::ui::test_support::waitForCondition("Apply button hidden after Applied", [&] {
        return !applyToWorkspaceButton->property("visible").toBool();
    }, [&] {
        return std::string("applyToWorkspaceButton.visible=")
            + (applyToWorkspaceButton->property("visible").toBool() ? "true" : "false");
    }, 5s));
    assert(fs::exists(workspace.projectPath / "qml-eligible.txt"));

    // ---------------------------------------------------------------------
    // AG-18.4A hardening: ManifestPanel's verification-environment section
    // renders even for a Run that never froze a Manifest, and its
    // "Retry after setup"/"Configure verification profile" actions render
    // exactly the Backend-computed eligibility - QML never decides this
    // itself.
    // ---------------------------------------------------------------------
    {
        const std::string actionRequiredRunId = "ag18-4a-qml-action-required-run";
        {
            ai::application::Run run;
            run.id = actionRequiredRunId;
            run.projectId = projectId.toStdString();
            run.title = "AG-18.4A QML fixture run (ActionRequired)";
            run.task = "Exercise the verification-environment QML projection";
            run.workflowId = ai::application::kExecutionManifestWorkflowId;
            run.workspacePath = workspace.projectPath;
            run.status = ai::application::RunStatus::Failed;
            run.createdAt = std::chrono::system_clock::now();
            const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteRunRepository runs(database.value());
            assert(runs.insertRun(run).success());
            // Deliberately never insert an ExecutionManifest row - this Run
            // must project hasManifest=false, exactly like a real
            // ActionRequired Run that stopped before freeze.

            ai::application::VerificationEnvironmentSnapshot snapshot;
            snapshot.runId = actionRequiredRunId;
            snapshot.projectId = projectId.toStdString();
            snapshot.baselineFingerprint = "ai-orchestrator-qml-smoke-ve-baseline";
            snapshot.state = ai::application::VerificationEnvironmentState::SetupRequired;
            snapshot.diagnostic =
                "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline";
            snapshot.discoveredAt = std::chrono::system_clock::now();
            ai::infrastructure::SqliteVerificationEnvironmentRepository veRepo(database.value());
            assert(veRepo.recordSnapshot(snapshot).success());

            ai::application::VerificationBootstrapDecision decision;
            decision.runId = actionRequiredRunId;
            decision.projectId = projectId.toStdString();
            decision.baselineFingerprint = snapshot.baselineFingerprint;
            decision.verificationSnapshotSchemaVersion = snapshot.schemaVersion;
            decision.disposition = ai::application::VerificationBootstrapDisposition::ActionRequired;
            decision.reasonCode = "verification_environment.action_required";
            decision.message = "No usable profile and no safe bootstrap was declared";
            decision.decidedAt = std::chrono::system_clock::now();
            ai::infrastructure::SqliteVerificationBootstrapRepository bootstrapRepo(database.value());
            assert(bootstrapRepo.recordDecision(decision).success());
        }

        const std::string existingProfileRunId = "ag18-4a-qml-existing-profile-run";
        {
            ai::application::Run run;
            run.id = existingProfileRunId;
            run.projectId = projectId.toStdString();
            run.title = "AG-18.4A QML fixture run (ExistingProfile)";
            run.task = "Exercise the disabled Retry after setup state";
            run.workflowId = ai::application::kExecutionManifestWorkflowId;
            run.workspacePath = workspace.projectPath;
            run.status = ai::application::RunStatus::Failed;
            run.createdAt = std::chrono::system_clock::now();
            const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteRunRepository runs(database.value());
            assert(runs.insertRun(run).success());

            ai::application::VerificationEnvironmentSnapshot snapshot;
            snapshot.runId = existingProfileRunId;
            snapshot.projectId = projectId.toStdString();
            snapshot.baselineFingerprint = "ai-orchestrator-qml-smoke-ve-baseline-2";
            snapshot.state = ai::application::VerificationEnvironmentState::ExistingProfile;
            ai::application::VerificationProfile profile;
            profile.profileId = "local-qt-debug";
            profile.configurePresetName = "local-qt-debug";
            profile.buildPresetsConfirmed = true;
            snapshot.profiles.push_back(profile);
            snapshot.discoveredAt = std::chrono::system_clock::now();
            ai::infrastructure::SqliteVerificationEnvironmentRepository veRepo(database.value());
            assert(veRepo.recordSnapshot(snapshot).success());
            // Deliberately no bootstrap decision - an ExistingProfile Run
            // never has one, and canRetryAfterSetup must be false.
        }

        backend.runController().refresh(projectId);
        assert(ai::ui::test_support::waitForCondition(
            "ActionRequired/ExistingProfile fixture runs visible in RunsPage", [&] {
                const QVariantList snapshot = runsPage->property("runsSnapshot").toList();
                bool sawActionRequired = false;
                bool sawExistingProfile = false;
                for (const QVariant& entry : snapshot) {
                    const QString runId = entry.toMap().value(QStringLiteral("runId")).toString();
                    if (runId == QString::fromStdString(actionRequiredRunId)) sawActionRequired = true;
                    if (runId == QString::fromStdString(existingProfileRunId)) sawExistingProfile = true;
                }
                return sawActionRequired && sawExistingProfile;
            }, [] { return std::string("AG-18.4A fixture runs not present in RunsPage.runsSnapshot"); },
            5s));

        QObject* manifestPanel = waitForChild(window, "manifestPanel");
        assert(manifestPanel && "ManifestPanel was not found in the loaded QML tree");
        QObject* verificationSection = waitForChild(window, "verificationEnvironmentSection");
        QObject* retryButton = waitForChild(window, "retryAfterSetupButton");
        QObject* configureButton = waitForChild(window, "configureVerificationProfileButton");
        assert(verificationSection && retryButton && configureButton);

        backend.runController().selectRun(QString::fromStdString(actionRequiredRunId));
        assert(waitForRunDataId(actionRequiredRunId, "ActionRequired runData bound"));
        // The panel must be visible even though this Run never froze a
        // Manifest (hasManifest stays false) - only its verification-
        // environment projection makes it relevant.
        assert(ai::ui::test_support::waitForCondition(
            "ManifestPanel visible without a frozen Manifest (ActionRequired)", [&] {
                return manifestPanel->property("visible").toBool()
                    && verificationSection->property("visible").toBool();
            }, [&] {
                return std::string("manifestPanel.visible=")
                    + (manifestPanel->property("visible").toBool() ? "true" : "false")
                    + ", verificationEnvironmentSection.visible="
                    + (verificationSection->property("visible").toBool() ? "true" : "false");
            }, 5s));
        assert(ai::ui::test_support::waitForCondition(
            "Retry after setup enabled for a durable ActionRequired decision", [&] {
                return retryButton->property("enabled").toBool();
            }, [&] {
                return std::string("retryAfterSetupButton.enabled=")
                    + (retryButton->property("enabled").toBool() ? "true" : "false");
            }, 5s));
        assert(retryButton->property("text").toString() == QStringLiteral("Retry after setup"));
        // Configure verification profile stays disabled on Run Details:
        // AG-18.4B exists only in the New Project flow today.
        assert(!configureButton->property("enabled").toBool());
        assert(configureButton->property("text").toString()
            == QStringLiteral("Configure verification profile"));

        backend.runController().selectRun(QString::fromStdString(existingProfileRunId));
        assert(waitForRunDataId(existingProfileRunId, "ExistingProfile runData bound"));
        assert(ai::ui::test_support::waitForCondition(
            "ManifestPanel visible for an ExistingProfile projection too", [&] {
                return manifestPanel->property("visible").toBool()
                    && verificationSection->property("visible").toBool();
            }, [&] {
                return std::string("manifestPanel.visible=")
                    + (manifestPanel->property("visible").toBool() ? "true" : "false");
            }, 5s));
        assert(ai::ui::test_support::waitForCondition(
            "Retry after setup disabled without a durable ActionRequired decision", [&] {
                return !retryButton->property("enabled").toBool();
            }, [&] {
                return std::string("retryAfterSetupButton.enabled=")
                    + (retryButton->property("enabled").toBool() ? "true" : "false");
            }, 5s));
        assert(!configureButton->property("enabled").toBool());
    }

    // ---------------------------------------------------------------------
    // QML-P0: a terminal Failed continuation child's NeedsManualAction
    // banner must be driven ENTIRELY by the Backend-owned contentOutcome/
    // remainingFindings projection - visible for a bounded, Advisory-only
    // lint failure, absent for a Hard-finding failure, and correctly
    // re-derived (not stale) when the selection moves between the two.
    // ---------------------------------------------------------------------
    {
        const auto seedContinuationChild = [&](const std::string& suffix,
                const ai::application::ManifestVerificationDiagnosticFinding& finding) {
            const std::string sourceRunId = "qml-p0-source-" + suffix;
            const std::string childRunId = "qml-p0-child-" + suffix;
            const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteRunRepository runs(database.value());

            // The read model needs the source Run to EXIST (found by id) but
            // does not itself require a source Work Ledger entry or a real
            // proposal to project the CHILD's own contentOutcome - see
            // ManifestVerificationContinuationReadModelService::viewForRun's
            // own gating (only a genuine repository error is fatal, never a
            // missing/absent source ledger entry).
            ai::application::Run source;
            source.id = sourceRunId;
            source.projectId = projectId.toStdString();
            source.title = "QML-P0 continuation source";
            source.task = "Exercise the NeedsManualAction QML projection";
            source.workflowId = ai::application::kExecutionManifestWorkflowId;
            source.status = ai::application::RunStatus::Failed;
            source.createdAt = std::chrono::system_clock::now();
            assert(runs.insertRun(source).success());

            ai::application::ManifestVerificationContinuationInput input;
            input.sourceRunId = sourceRunId;
            input.childRunId = childRunId;
            input.sourceProjectId = projectId.toStdString();
            input.attributionId = "verification-attribution:" + sourceRunId;
            input.baselineFingerprint = std::string(64, 'b');
            input.sourceWorkspaceIdentity = projectId.toStdString();
            input.sourceOverlayFingerprint = std::string(64, 'c');
            input.orderedProposalIds = {"qml-p0-cs-" + suffix};
            input.orderedProposalContentHashes = {std::string(64, 'a')};
            input.createdAt = std::chrono::system_clock::now();
            input.inputFingerprint =
                ai::application::computeManifestVerificationContinuationInputFingerprint(input);
            ai::infrastructure::SqliteManifestVerificationContinuationRepository continuations(
                database.value());
            assert(continuations.insertInput(input).success());

            ai::application::Run child;
            child.id = childRunId;
            child.projectId = projectId.toStdString();
            child.title = "QML-P0 continuation child";
            child.task = "Exercise the NeedsManualAction QML projection";
            child.workflowId =
                std::string(ai::application::kManifestVerificationContinuationWorkflowId);
            child.parentRunId = sourceRunId;
            child.status = ai::application::RunStatus::Failed;
            child.createdAt = std::chrono::system_clock::now();
            assert(runs.insertRun(child).success());

            ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database.value());
            ai::application::ManifestProposalSetVerification finalVerification;
            finalVerification.workspaceIdentity = projectId.toStdString();
            finalVerification.baselineFingerprint = std::string(64, 'b');
            finalVerification.overlayFingerprint = std::string(64, 'c');
            finalVerification.proposalSafetyStatus =
                ai::application::ExecutionVerificationStatus::Passed;
            finalVerification.buildLintTestStatus =
                ai::application::ExecutionVerificationStatus::Failed;
            finalVerification.acceptanceStatus =
                ai::application::ExecutionVerificationStatus::Failed;
            ai::application::ManifestVerificationCheckResult lintCheck;
            lintCheck.profileId = "qml-lint";
            lintCheck.kind = ai::application::ExecutionVerificationKind::QmlLint;
            lintCheck.status = ai::application::ExecutionVerificationStatus::Failed;
            lintCheck.commandIdentity = "qmllint";
            finalVerification.checks = {lintCheck};
            finalVerification.criteria = {{
                "qml-lint", true, ai::application::ExecutionVerificationStatus::Failed, {}}};
            ai::application::ManifestVerificationDiagnostic diagnostic;
            diagnostic.code = "verification.qml_warning";
            diagnostic.message = "qmllint reported a finding";
            diagnostic.requirementId = "qml-lint";
            diagnostic.structuredFindings = {finding};
            finalVerification.diagnostics = {diagnostic};

            ai::application::ManifestWorkLedgerEntry childEntry;
            childEntry.runId = childRunId;
            childEntry.projectId = projectId.toStdString();
            childEntry.terminalStatus = ai::application::RunStatus::Failed;
            childEntry.disposition =
                ai::application::ManifestWorkLedgerDisposition::ManifestReady;
            childEntry.verification = finalVerification;
            childEntry.createdAt = std::chrono::system_clock::now();
            assert(ledger.insertEntry(childEntry).success());
            return childRunId;
        };

        const std::string advisoryChildId = seedContinuationChild("advisory",
            {"<candidate>/qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access"});
        const std::string hardChildId = seedContinuationChild("hard",
            {"<candidate>/qml/PageHost.qml", 1, 1, "import", "Failed to import module"});

        // Reproduce the production timing that used to leave Create stale:
        // selection observes the continuation child while it is still
        // Running, then the SAME selected Run becomes terminal Failed. Its
        // durable failed ledger is already present, but eligibility must not
        // change until RunController publishes runUpdated below.
        {
            const auto database = ai::infrastructure::SqliteDatabase::open(
                workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteRunRepository runs(database.value());
            const auto found = runs.findRun(hardChildId);
            assert(found.success() && found.value().has_value());
            auto runningChild = *found.value();
            runningChild.status = ai::application::RunStatus::Running;
            runningChild.finishedAt.reset();
            assert(runs.updateRun(std::move(runningChild)).success());
        }

        // QML-P4R.5 package 3: seed one immutable, structurally valid handoff
        // directly so the real QML module renders its hasHandoff state without
        // running local verification merely to arrange UI test data. Its
        // candidate directory and snapshot are real, so Backend health is
        // still derived by the production probe rather than a test fixture.
        {
            const auto database = ai::infrastructure::SqliteDatabase::open(
                workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteManifestVerificationManualHandoffRepository handoffs(
                database.value());
            ai::application::ManifestVerificationManualHandoff handoff;
            handoff.id = "qml-p4r5-handoff-advisory";
            handoff.originManifestRunId = "qml-p0-source-advisory";
            handoff.verificationAnchorRunId = advisoryChildId;
            handoff.sourceProjectId = projectId.toStdString();
            handoff.storageKey = "qml-p4r5-advisory-candidate";
            handoff.workspaceIdentity = projectId.toStdString();
            handoff.baselineFingerprint = std::string(64, 'b');
            handoff.sourceOverlayFingerprint = std::string(64, 'c');
            handoff.frozenMembers = {{"qml-p0-cs-advisory", advisoryChildId,
                std::string(64, 'a'), "qml-p0-cs-advisory"}};
            handoff.allowedPaths = {
                {"qml/AppSidebar.qml", "qml-p0-cs-advisory", "qml-task"}};
            const fs::path candidateRoot = workspace.databasePath.parent_path()
                / "manual-handoff" / handoff.storageKey;
            fs::create_directories(candidateRoot);
            const auto candidateSnapshot = ai::application::WorkspaceBaselineProvenance::capture(
                ai::application::WorkspaceReadPolicy::secureDefaults(candidateRoot));
            assert(candidateSnapshot.success());
            handoff.originalSnapshot = candidateSnapshot.value();
            handoff.createdAt = std::chrono::system_clock::now();
            assert(handoffs.insertHandoff(handoff).success());
        }

        backend.runController().refresh(projectId);
        QObject* needsManualActionBanner =
            waitForChild(window, "verificationContinuationNeedsManualAction");
        assert(needsManualActionBanner
            && "verificationContinuationNeedsManualAction was not found in the loaded QML tree");
        QObject* manualHandoffPanel = waitForChild(window, "manualHandoffPanel");
        QObject* manualHandoffEditableScopeNotice =
            waitForChild(window, "manualHandoffEditableScopeNotice");
        QObject* manualHandoffCreateButton =
            waitForChild(window, "manualHandoffCreateButton");
        QObject* manualHandoffReverifyButton =
            waitForChild(window, "manualHandoffReverifyButton");
        QObject* recordedFindingsBlock = waitForChild(window, "recordedFindingsBlock");
        QObject* recordedFindingsShowAllButton =
            waitForChild(window, "recordedFindingsShowAllButton");
        QObject* recordedFindingsDialog = waitForChild(window, "recordedFindingsDialog");
        QObject* prepareContinuationButton =
            waitForChild(window, "prepareVerificationContinuationButton");
        assert(manualHandoffPanel && manualHandoffEditableScopeNotice
            && manualHandoffCreateButton
            && manualHandoffReverifyButton && recordedFindingsBlock
            && recordedFindingsShowAllButton && recordedFindingsDialog
            && prepareContinuationButton
            && "The manual-handoff and continuation actions must exist in the real QML tree");

        backend.runController().selectRun(QString::fromStdString(advisoryChildId));
        assert(waitForRunDataId(advisoryChildId, "Advisory child runData bound"));
        assert(ai::ui::test_support::waitForCondition(
            "NeedsManualAction banner visible for a bounded Advisory-only lint failure", [&] {
                return needsManualActionBanner->property("visible").toBool();
            }, [&] {
                return std::string("needsManualActionBanner.visible=")
                    + (needsManualActionBanner->property("visible").toBool() ? "true" : "false");
            }, 5s));
        assert(ai::ui::test_support::waitForCondition(
            "Manual handoff shown for the terminal continuation child", [&] {
                return manualHandoffPanel->property("visible").toBool()
                    && manualHandoffEditableScopeNotice->property("visible").toBool()
                    && manualHandoffReverifyButton->property("visible").toBool()
                    && manualHandoffReverifyButton->property("enabled").toBool()
                    && !manualHandoffCreateButton->property("visible").toBool()
                    && !prepareContinuationButton->property("visible").toBool();
            }, [&] {
                return std::string("panel=")
                    + (manualHandoffPanel->property("visible").toBool() ? "true" : "false")
                    + ", reverify="
                    + (manualHandoffReverifyButton->property("visible").toBool()
                        ? "true" : "false")
                    + ", create="
                    + (manualHandoffCreateButton->property("visible").toBool()
                        ? "true" : "false")
                    + ", stalePrepare="
                    + (prepareContinuationButton->property("visible").toBool()
                        ? "true" : "false");
            }, 5s));
        assert(backend.manualHandoffController().hasHandoff());
        assert(backend.manualHandoffController().candidateHealth()
            == QStringLiteral("unchanged"));
        assert(recordedFindingsBlock->property("visible").toBool()
            && recordedFindingsShowAllButton->property("visible").toBool());
        {
            const QVariantList rows = backend.manualHandoffController().recordedFindings()
                .value(QStringLiteral("findings")).toList();
            assert(rows.size() == 1);
            const QVariantMap row = rows.front().toMap();
            assert(row.value(QStringLiteral("tag")).toString() == QStringLiteral("unqualified")
                && "The Qt projection must preserve the Backend finding tag");
        }

        // PC-4D: the read-only Outcome block is hosted in the real Run Details. The selected Run is a TERMINAL continuation child,
        // so the block exists and shows whatever the Backend says about it (a brief, or that none is recorded yet) - the panel
        // renders exactly the controller's map, never its own idea of it. Nothing in it starts, retries or applies anything.
        QObject* outcomeBriefPanel = waitForChild(window, "outcomeBriefPanel");
        assert(outcomeBriefPanel && "outcomeBriefPanel was not found in the loaded QML tree");
        assert(ai::ui::test_support::waitForCondition(
            "Outcome block shown for a terminal outcome Run, bound to the controller's availability", [&] {
                return backend.outcomeBriefController().applicable()
                    && outcomeBriefPanel->property("visible").toBool()
                    && outcomeBriefPanel->property("availability").toString()
                        == backend.outcomeBriefController().brief()
                               .value(QStringLiteral("availability")).toString();
            }, [&] {
                return std::string("applicable=")
                    + (backend.outcomeBriefController().applicable() ? "true" : "false")
                    + ", visible=" + (outcomeBriefPanel->property("visible").toBool() ? "true" : "false")
                    + ", panel.availability=" + outcomeBriefPanel->property("availability").toString().toStdString()
                    + ", controller.availability=" + backend.outcomeBriefController().brief()
                          .value(QStringLiteral("availability")).toString().toStdString();
            }, 5s));

        backend.runController().selectRun(QString::fromStdString(hardChildId));
        assert(waitForRunDataId(hardChildId, "Hard child runData bound"));
        assert(ai::ui::test_support::waitForCondition(
            "NeedsManualAction banner hidden for a Hard-finding failure - proves the previous "
            "Advisory child's projection did not leak across selection", [&] {
                return !needsManualActionBanner->property("visible").toBool();
            }, [&] {
                return std::string("needsManualActionBanner.visible=")
                    + (needsManualActionBanner->property("visible").toBool() ? "true" : "false");
            }, 5s));
        assert(ai::ui::test_support::waitForCondition(
            "Manual handoff Create stays disabled while the selected child is non-terminal", [&] {
                return manualHandoffPanel->property("visible").toBool()
                    && manualHandoffCreateButton->property("visible").toBool()
                    && !manualHandoffCreateButton->property("enabled").toBool()
                    && !prepareContinuationButton->property("visible").toBool();
            }, [&] {
                return std::string("panel=")
                    + (manualHandoffPanel->property("visible").toBool() ? "true" : "false")
                    + ", create="
                    + (manualHandoffCreateButton->property("visible").toBool()
                        ? "true" : "false")
                    + ", stalePrepare="
                    + (prepareContinuationButton->property("visible").toBool()
                        ? "true" : "false");
            }, 5s));
        assert(backend.manualHandoffController().createReasonCode()
            == QStringLiteral("verification_continuation.source_run_not_failed"));
        assert(ai::ui::test_support::waitForCondition(
            "Outcome block hidden for a non-terminal Run - nothing of the previous selection leaks across", [&] {
                return !backend.outcomeBriefController().applicable()
                    && !outcomeBriefPanel->property("visible").toBool();
            }, [&] {
                return std::string("applicable=")
                    + (backend.outcomeBriefController().applicable() ? "true" : "false")
                    + ", visible=" + (outcomeBriefPanel->property("visible").toBool() ? "true" : "false");
            }, 5s));

        // Complete the selected Run in durable storage, then publish the
        // exact dirty notification RunController emits after applying a real
        // terminal event. Selection is deliberately unchanged: the QML
        // connection itself must refresh the separate handoff read model.
        {
            const auto database = ai::infrastructure::SqliteDatabase::open(
                workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteRunRepository runs(database.value());
            const auto found = runs.findRun(hardChildId);
            assert(found.success() && found.value().has_value());
            auto failedChild = *found.value();
            failedChild.status = ai::application::RunStatus::Failed;
            failedChild.finishedAt = std::chrono::system_clock::now();
            assert(runs.updateRun(std::move(failedChild)).success());
        }
        const QString hardChildRunId = QString::fromStdString(hardChildId);
        assert(QMetaObject::invokeMethod(&backend.runController(), "runUpdated",
            Qt::DirectConnection, Q_ARG(QString, hardChildRunId)));
        assert(backend.runController().selectedRunId() == hardChildRunId
            && "The regression must not hide a reselection behind the eligibility refresh");
        assert(ai::ui::test_support::waitForCondition(
            "Run update refreshes eligible manual handoff Create without restart or reselection",
            [&] {
                return manualHandoffCreateButton->property("enabled").toBool()
                    && backend.manualHandoffController().canCreate()
                    && recordedFindingsBlock->property("visible").toBool()
                    && recordedFindingsShowAllButton->property("visible").toBool();
            }, [&] {
                return std::string("createEnabled=")
                    + (manualHandoffCreateButton->property("enabled").toBool()
                        ? "true" : "false")
                    + ", reason="
                    + backend.manualHandoffController().createReasonCode().toStdString();
            }, 5s));
        assert(!backend.manualHandoffController().hasHandoff()
            && "The recorded findings must be visible before any repair workspace exists");
        assert(QMetaObject::invokeMethod(recordedFindingsShowAllButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition(
            "Recorded findings open in the shared findings window before handoff creation", [&] {
                return recordedFindingsDialog->property("visible").toBool();
            }, [] { return std::string("recordedFindingsDialog.visible=false"); }, 5s));
        assert(QMetaObject::invokeMethod(recordedFindingsDialog, "close"));

        // Switch back to the Advisory child once more - the banner must
        // re-appear, proving the projection is freshly re-derived on every
        // selection rather than only ever clearing forward.
        backend.runController().selectRun(QString::fromStdString(advisoryChildId));
        assert(waitForRunDataId(advisoryChildId, "Advisory child runData bound again"));
        assert(ai::ui::test_support::waitForCondition(
            "NeedsManualAction banner visible again after switching back", [&] {
                return needsManualActionBanner->property("visible").toBool();
            }, [&] {
                return std::string("needsManualActionBanner.visible=")
                    + (needsManualActionBanner->property("visible").toBool() ? "true" : "false");
            }, 5s));

        // QML-P4R.5 package 4: every assertion above only proves the panel
        // correctly RENDERS a pre-seeded projection. Prove the full QML ->
        // async command -> QML re-render cycle for real: a genuine click on
        // the real Discard button, through its real two-click confirmation
        // flow, to the real Confirm-discard click that actually dispatches
        // controller.discard() - never the C++ controller called directly.
        // Discard's own onClicked only ever sets the panel's local
        // confirmingDiscard property (see ManualHandoffPanel.qml); the
        // separate "Confirm discard" button is the one that calls
        // discard(). Both are exercised here via their own clicked() signal
        // - the same signal Qt Quick emits for a real mouse click.
        // Discard is the one command safe to actually run here: unlike
        // Create/Reverify it never invokes a local qmllint/cmake subprocess,
        // only a DB delete plus best-effort filesystem cleanup of the
        // already-real, empty candidate directory seeded above - so this
        // stays deterministic and environment-independent, exactly like
        // every other assertion in this file.
        QObject* manualHandoffDiscardButton =
            waitForChild(window, "manualHandoffDiscardButton");
        QObject* manualHandoffConfirmDiscardButton =
            waitForChild(window, "manualHandoffConfirmDiscardButton");
        QObject* manualHandoffCancelDiscardButton =
            waitForChild(window, "manualHandoffCancelDiscardButton");
        assert(manualHandoffDiscardButton && manualHandoffConfirmDiscardButton
            && manualHandoffCancelDiscardButton
            && "Discard/confirm/keep buttons were not found in the loaded QML tree");
        assert(backend.manualHandoffController().selectedRunId()
            == QString::fromStdString(advisoryChildId));
        assert(backend.manualHandoffController().hasHandoff());
        assert(!backend.manualHandoffController().busy());

        // First real click: Discard must only reveal the confirmation step,
        // never dispatch the command itself.
        assert(QMetaObject::invokeMethod(manualHandoffDiscardButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition(
            "Discard confirmation appears after a real click", [&] {
                return manualHandoffConfirmDiscardButton->property("visible").toBool()
                    && manualHandoffCancelDiscardButton->property("visible").toBool();
            }, [&] {
                return std::string("confirmVisible=")
                    + (manualHandoffConfirmDiscardButton->property("visible").toBool()
                        ? "true" : "false");
            }, 5s));
        assert(!backend.manualHandoffController().busy()
            && "A real click on Discard must only reveal confirmation, never dispatch the command");
        assert(backend.manualHandoffController().hasHandoff()
            && "The handoff must remain untouched before an explicit real confirm click");

        // A real click on "Keep workspace" must cancel the confirmation
        // without touching the handoff at all.
        assert(QMetaObject::invokeMethod(manualHandoffCancelDiscardButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition(
            "Confirmation hides again after a real Keep workspace click", [&] {
                return !manualHandoffConfirmDiscardButton->property("visible").toBool();
            }, [&] {
                return std::string("confirmVisible=")
                    + (manualHandoffConfirmDiscardButton->property("visible").toBool()
                        ? "true" : "false");
            }, 5s));
        assert(backend.manualHandoffController().hasHandoff()
            && "A real Keep workspace click must never discard anything");

        // Real click on Discard again, then the real Confirm-discard click -
        // the only click in this flow that actually calls controller::discard().
        assert(QMetaObject::invokeMethod(manualHandoffDiscardButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition(
            "Discard confirmation appears again after a second real click", [&] {
                return manualHandoffConfirmDiscardButton->property("visible").toBool();
            }, [&] {
                return std::string("confirmVisible=")
                    + (manualHandoffConfirmDiscardButton->property("visible").toBool()
                        ? "true" : "false");
            }, 5s));
        assert(QMetaObject::invokeMethod(manualHandoffConfirmDiscardButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition(
            "A real, QML-click-dispatched Discard command completes and the live "
            "QML scene re-renders from it", [&] {
                return !backend.manualHandoffController().busy()
                    && !backend.manualHandoffController().hasHandoff()
                    && manualHandoffCreateButton->property("visible").toBool()
                    && !manualHandoffReverifyButton->property("visible").toBool()
                    && !manualHandoffDiscardButton->property("visible").toBool();
            }, [&] {
                return std::string("busy=")
                    + (backend.manualHandoffController().busy() ? "true" : "false")
                    + ", hasHandoff="
                    + (backend.manualHandoffController().hasHandoff() ? "true" : "false")
                    + ", createVisible="
                    + (manualHandoffCreateButton->property("visible").toBool() ? "true" : "false")
                    + ", reverifyVisible="
                    + (manualHandoffReverifyButton->property("visible").toBool() ? "true" : "false");
            }, 5s));
        assert(backend.manualHandoffController().errorText().isEmpty()
            && "A genuinely successful real Discard must never leave stale error text");
    }

    // AG-18.4B: a real click-through "Create local Qt/CMake preset" flow -
    // typed configuration genuinely reaches the Backend and writes a real
    // file, not just a direct C++ controller call. Reuses this same real
    // dialog/window (never a second isolated QML scene). Deliberately runs
    // last: it creates two additional Projects, which would break every
    // earlier single-project assumption (liveProjectsModel size, Run
    // wiring) elsewhere in this test if run any earlier.
    {
        assert(backend.projectController().model()->rowCount() == 1
               && "Exactly one project must exist before this block's own project-count math holds");
        const fs::path secondProjectPath = workspace.root / "second-project";
        std::error_code mkdirError;
        fs::create_directories(secondProjectPath, mkdirError);
        assert(!mkdirError);

        // The Backend accepts a Qt prefix only when it exists and holds lib/cmake/Qt6/Qt6Config.cmake. A fixture of its own keeps
        // this test independent of where (and whether) a Qt of one exact version is installed on the machine that runs it.
        const fs::path fakeQtPrefix = workspace.root / "fake-qt" / "6.11.1" / "msvc2022_64";
        fs::create_directories(fakeQtPrefix / "lib" / "cmake" / "Qt6", mkdirError);
        assert(!mkdirError);
        { std::ofstream(fakeQtPrefix / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake") << "# test fixture\n"; }
        assert(fs::exists(fakeQtPrefix / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake"));
        const QString qtPrefixText = QString::fromStdWString(fakeQtPrefix.generic_wstring());

        assert(QMetaObject::invokeMethod(newProjectDialog, "open"));
        assert(waitForProperty(newProjectDialog, "visible",
            [](const QVariant& value) { return value.toBool(); }));
        assert(projectNameField->setProperty("text", QStringLiteral("AG-18.4B create-mode project")));
        assert(projectPathField->setProperty("text",
            QString::fromStdWString(secondProjectPath.wstring())));
        assert(newProjectDialog->setProperty("verificationMode", QStringLiteral("create")));

        QObject* qtPrefixField = waitForChild(window, "verificationQtPrefixField");
        QObject* configurePresetField = waitForChild(window, "verificationConfigurePresetField");
        QObject* buildPresetField = waitForChild(window, "verificationBuildPresetField");
        QObject* binaryDirField = waitForChild(window, "verificationBinaryDirField");
        QObject* previewArea = waitForChild(window, "verificationPresetPreview");
        assert(qtPrefixField && configurePresetField && buildPresetField && binaryDirField
               && previewArea);
        assert(qtPrefixField->setProperty("text", qtPrefixText));

        // The live preview is Backend-composed (VerificationEnvironmentPresetSetupService::
        // composePresetDocument), not a QML-side guess - it must already show
        // the exact real preset name before any confirmation.
        assert(waitForProperty(previewArea, "text", [](const QVariant& value) {
            return value.toString().contains(QStringLiteral("local-qt-debug"));
        }));
        const QString previewText = previewArea->property("text").toString();
        checkpoint(("verification preview length=" + std::to_string(previewText.size())).c_str());
        assert(previewText.contains(QStringLiteral("CMAKE_PREFIX_PATH")));
        assert(previewText.contains(QStringLiteral("${sourceDir}/build/local-qt-debug")));

        const bool createModeSubmitted = QMetaObject::invokeMethod(newProjectDialog, "submit");
        checkpoint(createModeSubmitted ? "create-mode NewProjectDialog submitted"
                                        : "create-mode NewProjectDialog submit FAILED");
        assert(createModeSubmitted);
        assert(waitForProperty(newProjectDialog, "visible",
            [](const QVariant& value) { return !value.toBool(); })
            && "A successful create-mode submission must close the dialog");

        const fs::path expectedPresetPath = secondProjectPath / "CMakeUserPresets.json";
        assert(fs::exists(expectedPresetPath)
               && "createLocalPreset() must have genuinely written the real file on disk");
        std::ifstream presetStream(expectedPresetPath, std::ios::binary);
        std::ostringstream presetBuffer;
        presetBuffer << presetStream.rdbuf();
        const std::string presetContent = presetBuffer.str();
        assert(presetContent.find("\"CMAKE_PREFIX_PATH\": \"" + qtPrefixText.toStdString() + "\"")
               != std::string::npos);
        checkpoint("AG-18.4B create-mode preset written to real disk");

        assert(backend.projectController().model()->rowCount() == 2
               && "The second, create-mode project must also have been created");

        // The filesystem write and Project-row insert cannot share one
        // transaction. Exercise the real compensation path by targeting the
        // already-registered first Project root, which has no preset: the
        // preset write succeeds, Project creation is rejected as a duplicate,
        // and the exact just-written bytes must be removed via the controller's
        // opaque one-shot cleanup token.
        const fs::path compensatedPresetPath = workspace.projectPath / "CMakeUserPresets.json";
        assert(!fs::exists(compensatedPresetPath));
        assert(QMetaObject::invokeMethod(newProjectDialog, "open"));
        assert(waitForProperty(newProjectDialog, "visible",
            [](const QVariant& value) { return value.toBool(); }));
        assert(projectNameField->setProperty("text", QStringLiteral("AG-18.4B compensated duplicate")));
        assert(projectPathField->setProperty("text",
            QString::fromStdWString(workspace.projectPath.wstring())));
        assert(newProjectDialog->setProperty("verificationMode", QStringLiteral("create")));
        assert(qtPrefixField->setProperty("text", qtPrefixText));
        assert(QMetaObject::invokeMethod(newProjectDialog, "submit"));
        assert(waitForProperty(newProjectDialog, "errorText",
            [](const QVariant& value) { return !value.toString().isEmpty(); }));
        assert(newProjectDialog->property("visible").toBool());
        assert(!fs::exists(compensatedPresetPath)
               && "A rejected Project insert must remove only its own unchanged generated preset");
        assert(backend.projectController().model()->rowCount() == 2
               && "Compensation must not create or remove any Project row");
        checkpoint("AG-18.4B duplicate Project write compensated without orphan preset");

        // Backend rejection (a second attempt at the SAME project path, so
        // createLocalPreset() hits its own real preset_already_exists guard)
        // must stay visible in the dialog rather than being swallowed - and
        // the dialog must NOT close, and NO second Project row may appear,
        // since AG-18.4B writes the preset strictly before the Project row.
        assert(QMetaObject::invokeMethod(newProjectDialog, "open"));
        assert(waitForProperty(newProjectDialog, "visible",
            [](const QVariant& value) { return value.toBool(); }));
        assert(projectNameField->setProperty("text", QStringLiteral("AG-18.4B duplicate preset")));
        assert(projectPathField->setProperty("text",
            QString::fromStdWString(secondProjectPath.wstring())));
        assert(newProjectDialog->setProperty("verificationMode", QStringLiteral("create")));
        assert(qtPrefixField->setProperty("text", qtPrefixText));
        assert(QMetaObject::invokeMethod(newProjectDialog, "submit"));
        assert(waitForProperty(newProjectDialog, "errorText", [](const QVariant& value) {
            return value.toString().contains(QStringLiteral("preset_already_exists"));
        }));
        checkpoint(("rejection errorText=" + newProjectDialog->property("errorText")
                        .toString().toStdString()).c_str());
        assert(newProjectDialog->property("visible").toBool()
               && "A rejected create-mode submission must keep the dialog open");
        assert(backend.projectController().model()->rowCount() == 2
               && "A rejected preset write must never create a Project row");

        // "Use existing presets" must never write anything - retry with a
        // FRESH project path (a third project's own path can never collide
        // with the second project's already-registered rootPath) and prove
        // no CMakeUserPresets.json appears there either.
        const fs::path thirdProjectPath = workspace.root / "third-project";
        fs::create_directories(thirdProjectPath, mkdirError);
        assert(!mkdirError);
        const fs::path thirdPresetPath = thirdProjectPath / "CMakeUserPresets.json";
        assert(!fs::exists(thirdPresetPath));

        assert(newProjectDialog->setProperty("verificationMode", QStringLiteral("existing")));
        assert(projectNameField->setProperty("text", QStringLiteral("AG-18.4B existing-mode project")));
        assert(projectPathField->setProperty("text",
            QString::fromStdWString(thirdProjectPath.wstring())));
        assert(QMetaObject::invokeMethod(newProjectDialog, "submit"));

        // ── RunsPage project filter combo must never lie about the real filter ──
        // Regression: RunComboBox (Runs_Objects/RunComboBox.qml) only resynced
        // its displayed currentIndex from onSelectedValueChanged, never from a
        // later change to its own options. RunsPage applies a project-filter
        // switch synchronously (WorkspaceContextController::selectProject ->
        // activeProjectIdChanged -> RunsPage's own Connections handler ->
        // applyProjectFilter, all verified synchronous by reading the C++/QML),
        // while its own project list (page.projectOptions, built from the
        // async projectsMirror Repeater via a *deferred* Qt.callLater) only
        // catches up on a later event-loop turn. createProject() above just
        // inserted this third project's row synchronously (page.createProject
        // calls the synchronous Q_INVOKABLE backend.projects.createProject
        // directly) - so right here, before any waitForProperty/
        // waitForCondition call below runs the event loop, is the exact
        // window the real production bug lived in.
        assert(backend.projectController().model()->rowCount() == 3
               && "Third project must already be inserted synchronously by submit()");
        const QString thirdProjectId = backend.projectController().model()->data(
            backend.projectController().model()->index(2, 0),
            ProjectListModel::ProjectIdRole).toString();
        assert(!thirdProjectId.isEmpty());
        QObject* runsProjectFilterCombo = waitForChild(window, "runsProjectFilterCombo");
        assert(runsProjectFilterCombo && "Runs project filter combo not found");
        assert(QMetaObject::invokeMethod(
            projectsPage, "continueProject", Q_ARG(QVariant, thirdProjectId)));
        assert(backend.workspaceContextController().activeProjectId() == thirdProjectId);
        assert(runsPage->property("projectFilter").toString() == thirdProjectId
               && "RunsPage must apply a project switch to its filter synchronously");
        const int comboIndexBeforeOptionsCatchUp =
            runsProjectFilterCombo->property("currentIndex").toInt();
        checkpoint(("runsProjectFilterCombo currentIndex immediately after switch="
            + std::to_string(comboIndexBeforeOptionsCatchUp)).c_str());
        assert(comboIndexBeforeOptionsCatchUp == -1
               && "Project filter combo must not silently fall back to index 0 "
                  "(\"All projects\") merely because its options list has not "
                  "caught up with the real, already-applied filter yet - it "
                  "must show nothing (-1) rather than the wrong project");

        assert(waitForProperty(newProjectDialog, "visible",
            [](const QVariant& value) { return !value.toBool(); }));
        assert(waitForProperty(runsPage, "projectOptions", [](const QVariant& value) {
            return value.toList().size() == 4; // "all" + three real projects
        }) && "RunsPage.projectOptions must eventually include the third project");
        // Once the deferred snapshot rebuild has run (the wait above pumped
        // the event loop), the combo must resync itself and agree with the
        // real filter - the old code never re-ran syncIndex() after options
        // changed, so this would otherwise stay wrong (or stuck at -1)
        // forever, exactly matching the reported real-world symptom.
        assert(waitForProperty(runsProjectFilterCombo, "currentIndex", [&](const QVariant& value) {
            const QVariantList opts = runsProjectFilterCombo->property("options").toList();
            const int index = value.toInt();
            return index >= 0 && index < opts.size()
                && opts.at(index).toMap().value(QStringLiteral("value")).toString()
                       == thirdProjectId;
        }) && "Project filter combo must show the real project once its options catch up");

        assert(!fs::exists(thirdPresetPath)
               && "\"Use existing presets\" must never write a CMakeUserPresets.json");
        assert(backend.projectController().model()->rowCount() == 3
               && "A third, existing-mode project must have been created without any preset write");
        checkpoint("AG-18.4B existing-mode project created without writing any file");
    }

    // ── Post-P4R.6 human review/Apply hardening: real QML click-through ──
    // Every earlier manual-handoff scenario in this file only drives
    // Create/Reverify/Discard. This proves the NEW exact-member review/Apply
    // surface end to end through real button clicks (never a direct C++
    // controller call): select a real authoritative member -> inspect its
    // real diff (typed proposalId+changeId, no absolute path ever reaches
    // QML) -> open the Approve-and-apply confirmation (exact proposal id
    // implied by the selection, exact relative paths listed) -> a real
    // Cancel click mutates nothing -> reopening and a real Confirm click
    // genuinely writes the file and the live scene re-renders the member as
    // Applied. Zero provider/Planner calls anywhere in this scenario - the
    // proposal, handoff, attempt, and Work Ledger entry are all seeded
    // directly via real Application services/SQLite repositories, exactly
    // like every other synthetic-but-real fixture in this file.
    {
        // The AG-18.4B scenario just above switched the active project to a
        // third project; RunDetailsPanel.runData only binds to a Run that is
        // actually present in RunsPage's own current-project-filtered
        // runsSnapshot, so this scenario must first return selection to the
        // original project every earlier fixture (seedManifestProposal
        // included) seeded its own Runs/proposals under.
        assert(QMetaObject::invokeMethod(
            projectsPage, "continueProject", Q_ARG(QVariant, projectId)));
        assert(ai::ui::test_support::waitForCondition(
            "Active project restored before the review/Apply click-through scenario", [&] {
                return backend.workspaceContextController().activeProjectId() == projectId;
            }, [&] {
                return std::string("activeProjectId=")
                    + backend.workspaceContextController().activeProjectId().toStdString();
            }, 5s));

        const std::string reviewRunId = "review-apply-qml-click-run";
        const std::string reviewChangeSetId =
            seedManifestProposal(reviewRunId, "review-click.txt", "review click content",
                ai::application::RunStatus::Failed);
        const std::string reviewAttemptRunId = reviewRunId + "-attempt";

        std::string reviewRealHash;
        {
            ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
            assert(seed.storageAvailable());
            const auto changes = seed.changeProposalService().listChanges(reviewChangeSetId);
            assert(changes.success() && changes.value().size() == 1);
            ai::application::ManifestProposalContentHashChangeInput input;
            input.operation = changes.value().front().operation;
            input.relativePath = changes.value().front().relativePath;
            input.sourcePath = changes.value().front().sourcePath;
            const auto content = seed.changeProposalService().readProposedContent(
                reviewChangeSetId, changes.value().front().changeId);
            assert(content.success());
            input.contentSha256 = ai::application::sha256Hex(content.value());
            reviewRealHash = ai::application::computeManifestProposalContentHash(
                reviewChangeSetId, {input});
        }

        {
            const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteRunRepository runs(database.value());

            ai::application::Run attempt;
            attempt.id = reviewAttemptRunId;
            attempt.projectId = projectId.toStdString();
            attempt.title = "Manual handoff attempt";
            attempt.task = "Provider-free reverify";
            attempt.workflowId =
                std::string(ai::application::kManifestVerificationManualHandoffWorkflowId);
            attempt.parentRunId = reviewRunId;
            attempt.status = ai::application::RunStatus::Completed;
            attempt.createdAt = std::chrono::system_clock::now();
            assert(runs.insertRun(attempt).success());

            ai::infrastructure::SqliteManifestVerificationManualHandoffRepository handoffs(
                database.value());
            ai::application::ManifestVerificationManualHandoff handoff;
            handoff.id = "manual-handoff:" + reviewRunId;
            handoff.originManifestRunId = reviewRunId;
            handoff.verificationAnchorRunId = reviewRunId;
            handoff.sourceProjectId = projectId.toStdString();
            handoff.storageKey = "review-apply-qml-click-candidate";
            handoff.workspaceIdentity = projectId.toStdString();
            handoff.baselineFingerprint = std::string(64, 'a');
            handoff.sourceOverlayFingerprint = std::string(64, 'b');
            handoff.frozenMembers = {
                {reviewChangeSetId, reviewRunId, reviewRealHash, reviewChangeSetId}};
            handoff.allowedPaths = {{"review-click.txt", reviewChangeSetId, "task-1"}};
            handoff.originalSnapshot.fingerprint = ai::application::sha256Hex("");
            handoff.createdAt = std::chrono::system_clock::now();
            assert(handoffs.insertHandoff(handoff).success());

            ai::application::ManifestVerificationManualHandoffAttempt attemptRow;
            attemptRow.id = "attempt-1:" + handoff.id;
            attemptRow.handoffId = handoff.id;
            attemptRow.verificationAnchorRunId = reviewRunId;
            attemptRow.attemptRunId = reviewAttemptRunId;
            attemptRow.attemptOrdinal = 1;
            attemptRow.editedSnapshotFingerprint = ai::application::sha256Hex("edited-qml-click");
            attemptRow.createdAt = std::chrono::system_clock::now();
            assert(handoffs.insertAttempt(attemptRow).success());

            ai::infrastructure::SqliteExecutionManifestRepository manifests(database.value());
            ai::application::ExecutionManifest manifest;
            manifest.runId = reviewAttemptRunId;
            assert(manifests.insert(std::move(manifest)).success());

            ai::application::ManifestProposalSetVerification verification;
            verification.workspaceIdentity = projectId.toStdString();
            verification.baselineFingerprint = handoff.baselineFingerprint;
            verification.overlayFingerprint = "review-apply-qml-click-overlay";
            verification.orderedProposalIds = {reviewChangeSetId};
            verification.orderedProposalContentHashes = {reviewRealHash};
            verification.proposalSafetyStatus = ai::application::ExecutionVerificationStatus::Passed;
            verification.buildLintTestStatus = ai::application::ExecutionVerificationStatus::Passed;
            verification.acceptanceStatus = ai::application::ExecutionVerificationStatus::Passed;

            ai::application::ManifestWorkLedgerEntry entry;
            entry.runId = reviewAttemptRunId;
            entry.projectId = projectId.toStdString();
            entry.terminalStatus = ai::application::RunStatus::Completed;
            entry.baselineFingerprint = handoff.baselineFingerprint;
            entry.disposition = ai::application::ManifestWorkLedgerDisposition::ManifestReady;
            entry.proposalIds = {reviewChangeSetId};
            ai::application::ManifestWorkLedgerTaskDisposition taskDisposition;
            taskDisposition.taskId = "task-1";
            taskDisposition.status = "completed";
            taskDisposition.changeSetId = reviewChangeSetId;
            entry.taskDispositions = {taskDisposition};
            entry.verification = verification;
            entry.createdAt = std::chrono::system_clock::now();
            ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database.value());
            assert(ledger.insertEntry(entry).success());
        }

        backend.runController().refresh(projectId);
        assert(ai::ui::test_support::waitForCondition("Review run visible in RunsPage", [&] {
            const QVariantList snapshot = runsPage->property("runsSnapshot").toList();
            for (const QVariant& entry : snapshot) {
                if (entry.toMap().value(QStringLiteral("runId")).toString()
                        == QString::fromStdString(reviewRunId)) {
                    return true;
                }
            }
            return false;
        }, [] { return std::string("review run not present in RunsPage.runsSnapshot"); }, 5s));
        backend.runController().selectRun(QString::fromStdString(reviewRunId));
        assert(waitForRunDataId(reviewRunId, "Review/Apply click-through runData bound"));

        QObject* manualHandoffControllerObjectEarly = backend.manualHandoff();
        assert(ai::ui::test_support::waitForCondition(
            "reviewMembers populated for the seeded handoff", [&] {
                return manualHandoffControllerObjectEarly->property("reviewMembers")
                    .toList().size() > 0;
            }, [&] {
                return std::string("isApplicable=")
                    + manualHandoffControllerObjectEarly->property("isApplicable")
                          .toString().toStdString()
                    + ", hasHandoff="
                    + manualHandoffControllerObjectEarly->property("hasHandoff")
                          .toString().toStdString()
                    + ", hasFinalVerification="
                    + manualHandoffControllerObjectEarly->property("hasFinalVerification")
                          .toString().toStdString()
                    + ", canApply="
                    + manualHandoffControllerObjectEarly->property("canApply")
                          .toString().toStdString()
                    + ", errorText="
                    + manualHandoffControllerObjectEarly->property("errorText")
                          .toString().toStdString()
                    + ", reviewMembers.size()="
                    + std::to_string(manualHandoffControllerObjectEarly->property("reviewMembers")
                          .toList().size());
            }, 5s));

        // A Repeater's own delegate items are never reachable through
        // QObject::findChild() from an ancestor - Qt Quick's Repeater does
        // not reparent them into the ordinary QObject ownership tree the way
        // it does their QQuickItem visual parent, so the only reliable way
        // to reach one is the Repeater's own itemAt(index) API. Everything
        // ELSE this file finds via waitForChild (static, non-Repeater
        // objectNames like the Repeater itself or the confirm/cancel/apply
        // buttons) is unaffected by this.
        QObject* reviewMembersRepeater =
            waitForChild(window, "manualHandoffReviewMembersRepeater");
        assert(reviewMembersRepeater && "The review members Repeater was not found");
        QQuickItem* reviewMemberItem = nullptr;
        assert(ai::ui::test_support::waitForCondition(
            "Review member delegate item created", [&] {
                reviewMemberItem = nullptr;
                QMetaObject::invokeMethod(reviewMembersRepeater, "itemAt",
                    Q_RETURN_ARG(QQuickItem*, reviewMemberItem), Q_ARG(int, 0));
                return reviewMemberItem != nullptr
                    && reviewMemberItem->objectName()
                        == QString::fromStdString(
                               "manualHandoffReviewMemberItem_" + reviewChangeSetId);
            }, [&] {
                return std::string("itemAt(0)=")
                    + (reviewMemberItem ? "non-null" : "null")
                    + (reviewMemberItem
                           ? (", objectName=" + reviewMemberItem->objectName().toStdString())
                           : std::string());
            }, 5s));
        QObject* reviewMemberRowArea = reviewMemberItem->findChild<QObject*>(
            QString::fromStdString("manualHandoffReviewMemberRowArea_" + reviewChangeSetId));
        assert(reviewMemberRowArea
            && "The authoritative review member row's MouseArea was not found in the loaded QML tree");
        QObject* reviewApplyButton = waitForChild(window, "manualHandoffReviewApplyButton");
        QObject* reviewCancelApplyButton =
            waitForChild(window, "manualHandoffReviewCancelApplyButton");
        QObject* reviewConfirmApplyButton =
            waitForChild(window, "manualHandoffReviewConfirmApplyButton");
        assert(reviewApplyButton && reviewCancelApplyButton && reviewConfirmApplyButton
            && "Review Apply/Cancel/Confirm buttons were not found in the loaded QML tree");

        QObject* manualHandoffControllerObject =
            backend.manualHandoff();
        assert(manualHandoffControllerObject);

        // Real click 1: select the review member row via its own MouseArea.
        // MouseArea's built-in clicked(mouse) signal takes a MouseEvent
        // argument invokeMethod cannot easily construct, so onClicked also
        // emits a plain zero-arg "activated" signal purely for this
        // real-click-through test to invoke - the QML-side behavior
        // (panel.selectMemberProposal) still runs from the SAME onClicked
        // handler a genuine mouse click triggers.
        assert(QMetaObject::invokeMethod(reviewMemberRowArea, "activated"));
        assert(ai::ui::test_support::waitForCondition(
            "Review member selected after a real row click", [&] {
                return manualHandoffControllerObject->property("selectedMemberProposalId")
                    .toString() == QString::fromStdString(reviewChangeSetId);
            }, [&] {
                return std::string("selectedMemberProposalId=")
                    + manualHandoffControllerObject->property("selectedMemberProposalId")
                          .toString().toStdString();
            }, 5s));

        const QVariantMap selectedDetail =
            manualHandoffControllerObject->property("selectedMemberDetail").toMap();
        assert(selectedDetail.value(QStringLiteral("applyAllowed")).toBool());
        const QVariantList selectedChanges = selectedDetail.value(QStringLiteral("changes")).toList();
        assert(selectedChanges.size() == 1);
        const QString reviewChangeId =
            selectedChanges.first().toMap().value(QStringLiteral("changeId")).toString();
        assert(!reviewChangeId.isEmpty());

        // Real click 2: open the diff. Content is fetched through the
        // controller by typed (proposalId, changeId) - never an absolute path.
        // The diff toggle MouseArea lives inside another Repeater's delegate
        // (the selected member's own "changes" list) - same itemAt()
        // requirement as the member row above.
        QObject* reviewChangesRepeater =
            waitForChild(window, "manualHandoffReviewSelectedChangesRepeater");
        assert(reviewChangesRepeater && "The selected member's changes Repeater was not found");
        QQuickItem* reviewChangeItem = nullptr;
        assert(ai::ui::test_support::waitForCondition(
            "Selected member's change delegate item created", [&] {
                reviewChangeItem = nullptr;
                QMetaObject::invokeMethod(reviewChangesRepeater, "itemAt",
                    Q_RETURN_ARG(QQuickItem*, reviewChangeItem), Q_ARG(int, 0));
                return reviewChangeItem != nullptr;
            }, [&] { return std::string("itemAt(0) still null"); }, 5s));
        QObject* reviewDiffToggle = reviewChangeItem->findChild<QObject*>(
            QString::fromStdString("manualHandoffReviewDiffToggle_" + reviewChangeId.toStdString()));
        assert(reviewDiffToggle && "The diff toggle MouseArea was not found");
        assert(QMetaObject::invokeMethod(reviewDiffToggle, "activated"));
        assert(ai::ui::test_support::waitForCondition("Diff content loaded after a real click", [&] {
            return manualHandoffControllerObject->property("errorText").toString().isEmpty();
        }, [&] {
            return std::string("errorText=")
                + manualHandoffControllerObject->property("errorText").toString().toStdString();
        }, 5s));
        assert(manualHandoffControllerObject->property("errorText").toString().isEmpty());

        // Real click 3: open the Approve-and-apply confirmation. Cancel must
        // not mutate anything - the file must not exist yet.
        const std::filesystem::path reviewWrittenPath =
            workspace.projectPath / "review-click.txt";
        assert(!fs::exists(reviewWrittenPath));
        assert(QMetaObject::invokeMethod(reviewApplyButton, "clicked"));
        assert(QMetaObject::invokeMethod(reviewCancelApplyButton, "clicked"));
        assert(!fs::exists(reviewWrittenPath)
            && "A real Cancel click must never write anything to the workspace");
        assert(manualHandoffControllerObject->property("selectedMemberProposalId").toString()
            == QString::fromStdString(reviewChangeSetId)
            && "Cancel must leave the selection untouched, never blank the panel");

        // Real click 4: reopen and this time really confirm. The confirm
        // button dispatches approveAndApplyMember(), which re-checks AG-16
        // fresh via ChangeSetApplyService::approve()/apply() before writing.
        assert(QMetaObject::invokeMethod(reviewApplyButton, "clicked"));
        assert(QMetaObject::invokeMethod(reviewConfirmApplyButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition(
            "A real, QML-click-dispatched Apply completes and the scene re-renders Applied", [&] {
                const QVariantList members =
                    manualHandoffControllerObject->property("reviewMembers").toList();
                for (const QVariant& entry : members) {
                    const QVariantMap map = entry.toMap();
                    if (map.value(QStringLiteral("proposalId")).toString()
                            == QString::fromStdString(reviewChangeSetId)) {
                        return map.value(QStringLiteral("alreadyApplied")).toBool();
                    }
                }
                return false;
            }, [&] {
                return std::string("errorText=")
                    + manualHandoffControllerObject->property("errorText").toString().toStdString();
            }, 5s));
        assert(manualHandoffControllerObject->property("errorText").toString().isEmpty()
            && "A genuinely successful real Apply must never leave stale error text");
        QObject* applyResultText = waitForChild(window, "manualHandoffApplyResultText");
        assert(applyResultText);
        assert(applyResultText->property("visible").toBool());
        assert(applyResultText->property("text").toString().startsWith(
            QStringLiteral("Applied 1 file(s) to workspace"))
            && "The real QML surface must render the Backend-owned Apply result");
        assert(fs::exists(reviewWrittenPath)
            && "A real Confirm-apply click must genuinely write the file to the workspace");
        {
            std::ifstream written(reviewWrittenPath, std::ios::binary);
            std::ostringstream buffer;
            buffer << written.rdbuf();
            assert(buffer.str() == "review click content");
        }

        // The original Run remains Failed, while the independent Backend-
        // owned Runs projection becomes Resolved manually / Applied without
        // an app restart. This is refreshed from durable authority/member
        // state by the controller's dirty notification - QML never infers it
        // from the successful click result above.
        assert(ai::ui::test_support::waitForCondition(
            "RunsPage receives the durable manual-resolution projection", [&] {
                const QVariantList snapshot = runsPage->property("runsSnapshot").toList();
                for (const QVariant& entry : snapshot) {
                    const QVariantMap row = entry.toMap();
                    if (row.value(QStringLiteral("runId")).toString()
                            == QString::fromStdString(reviewRunId)) {
                        return row.value(QStringLiteral("status")).toString()
                                == QStringLiteral("failed")
                            && row.value(QStringLiteral("manualHandoffResolution")).toString()
                                == QStringLiteral("resolved_manually_applied")
                            && row.value(QStringLiteral("manualHandoffResolutionLabel")).toString()
                                == QStringLiteral("Resolved manually · Applied");
                    }
                }
                return false;
            }, [] { return std::string("resolved review Run not projected"); }, 5s));

        QObject* resolutionFilter =
            waitForChild(window, "runsManualResolutionFilterCombo");
        assert(resolutionFilter && resolutionFilter->property("options").toList().size() == 5
            && "RunsPage must expose the independent manual-resolution filter");
        assert(runsPage->setProperty(
            "manualResolutionFilter", QStringLiteral("resolved_manually_applied")));
        assert(waitForProperty(runsPage, "filteredRuns", [&](const QVariant& value) {
            const QVariantList filtered = value.toList();
            if (filtered.isEmpty()) return false;
            for (const QVariant& entry : filtered) {
                if (entry.toMap().value(QStringLiteral("manualHandoffResolution")).toString()
                        != QStringLiteral("resolved_manually_applied")) return false;
            }
            return true;
        }) && "The manual-resolution filter must use the Backend-owned enum verbatim");
        assert(runsPage->setProperty("manualResolutionFilter", QStringLiteral("all")));
        checkpoint("real QML review/Apply click-through completed");
    }

    // ── Provider-free reset hardening: real QML click-through ───────────
    // A genuine materialized candidate (real ManifestProposalOverlay over a
    // real proposal, its exact snapshot captured and copied into the real
    // candidate storage root, exactly like materialize() itself would) with
    // one authorized edit and one unsupported drift (a stray new file).
    // Proves: Reset reveals confirmation without dispatching anything -> a
    // real Cancel click leaves the candidate untouched -> Reset again -> the
    // real Confirm click genuinely calls resetNonEditableChanges() -> the
    // live scene re-renders resetRequiredRelativePaths empty while
    // editableChangedRelativePaths still shows the authorized edit, and the
    // stray file is genuinely removed from disk.
    {
        const std::string resetRunId = "reset-hardening-qml-click-run";
        const std::string resetChangeSetId =
            seedManifestProposal(resetRunId, "keep.qml", "kept content",
                ai::application::RunStatus::Failed);

        std::string resetRealHash;
        std::string resetOverlayFingerprint;
        ai::application::WorkspaceBaselineSnapshot resetOriginalSnapshot;
        const fs::path resetCandidateRoot = workspace.databasePath.parent_path()
            / "manual-handoff" / "reset-qml-click-candidate";
        {
            ai::backend::BackendRuntime seed(workspace.guestRuntimeConfig());
            assert(seed.storageAvailable());

            const auto changes = seed.changeProposalService().listChanges(resetChangeSetId);
            assert(changes.success() && changes.value().size() == 1);
            ai::application::ManifestProposalContentHashChangeInput input;
            input.operation = changes.value().front().operation;
            input.relativePath = changes.value().front().relativePath;
            input.sourcePath = changes.value().front().sourcePath;
            const auto content = seed.changeProposalService().readProposedContent(
                resetChangeSetId, changes.value().front().changeId);
            assert(content.success());
            input.contentSha256 = ai::application::sha256Hex(content.value());
            resetRealHash = ai::application::computeManifestProposalContentHash(
                resetChangeSetId, {input});

            const auto policy = ai::application::WorkspaceReadPolicy::secureDefaults(
                workspace.projectPath);
            const auto baselineSnapshot =
                ai::application::WorkspaceBaselineProvenance::capture(policy);
            assert(baselineSnapshot.success());

            ai::application::BuildManifestProposalOverlayRequest overlayRequest;
            overlayRequest.workspaceRoot = workspace.projectPath;
            overlayRequest.trustedScratchRoot = workspace.databasePath.parent_path() / "scratch";
            overlayRequest.workspaceIdentity = projectId.toStdString();
            overlayRequest.baselinePolicy = policy;
            overlayRequest.baselineSnapshot = baselineSnapshot.value();
            overlayRequest.orderedProposalIds = {resetChangeSetId};
            const auto built = ai::application::ManifestProposalOverlay::build(
                overlayRequest, seed.changeProposalService());
            assert(built.success());
            resetOverlayFingerprint = built.value().overlayFingerprint;

            const auto overlaySnapshot = ai::application::WorkspaceBaselineProvenance::capture(
                ai::application::WorkspaceReadPolicy::secureDefaults(built.value().root));
            assert(overlaySnapshot.success());
            resetOriginalSnapshot = overlaySnapshot.value();

            std::error_code copyError;
            fs::create_directories(resetCandidateRoot.parent_path(), copyError);
            assert(!copyError);
            fs::copy(built.value().root, resetCandidateRoot, fs::copy_options::recursive, copyError);
            assert(!copyError);
            assert(ai::application::ManifestProposalOverlay::cleanup(built.value()).success());
        }

        {
            const auto database = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
            assert(database.success());
            ai::infrastructure::SqliteManifestVerificationManualHandoffRepository handoffs(
                database.value());
            ai::application::ManifestVerificationManualHandoff handoff;
            handoff.id = "manual-handoff:" + resetRunId;
            handoff.originManifestRunId = resetRunId;
            handoff.verificationAnchorRunId = resetRunId;
            handoff.sourceProjectId = projectId.toStdString();
            handoff.storageKey = "reset-qml-click-candidate";
            handoff.workspaceIdentity = projectId.toStdString();
            const auto baselineAgain = ai::application::WorkspaceBaselineProvenance::capture(
                ai::application::WorkspaceReadPolicy::secureDefaults(workspace.projectPath));
            assert(baselineAgain.success());
            handoff.baselineFingerprint = baselineAgain.value().fingerprint;
            handoff.sourceOverlayFingerprint = resetOverlayFingerprint;
            handoff.frozenMembers = {{resetChangeSetId, resetRunId, resetRealHash, resetChangeSetId}};
            handoff.allowedPaths = {{"keep.qml", resetChangeSetId, "task-1"}};
            handoff.originalSnapshot = resetOriginalSnapshot;
            handoff.createdAt = std::chrono::system_clock::now();
            assert(handoffs.insertHandoff(handoff).success());
        }

        // The user edits the one authorized file and also drops a stray new
        // file directly into the candidate directory.
        {
            std::ofstream kept(resetCandidateRoot / "keep.qml", std::ios::binary | std::ios::trunc);
            assert(kept.is_open());
            kept << "kept content edited by user";
        }
        {
            std::ofstream stray(resetCandidateRoot / "stray.qml", std::ios::binary | std::ios::trunc);
            assert(stray.is_open());
            stray << "unexpected file";
        }

        backend.runController().refresh(projectId);
        assert(ai::ui::test_support::waitForCondition("Reset scenario Run visible in RunsPage", [&] {
            const QVariantList snapshot = runsPage->property("runsSnapshot").toList();
            for (const QVariant& entry : snapshot) {
                if (entry.toMap().value(QStringLiteral("runId")).toString()
                        == QString::fromStdString(resetRunId)) {
                    return true;
                }
            }
            return false;
        }, [] { return std::string("reset scenario run not present in RunsPage.runsSnapshot"); }, 5s));
        backend.runController().selectRun(QString::fromStdString(resetRunId));
        assert(waitForRunDataId(resetRunId, "Reset click-through runData bound"));

        QObject* manualHandoffControllerObject = backend.manualHandoff();
        assert(ai::ui::test_support::waitForCondition(
            "resetRequiredRelativePaths populated for the seeded reset handoff", [&] {
                return manualHandoffControllerObject->property("resetRequiredRelativePaths")
                    .toStringList().size() == 1;
            }, [&] {
                return std::string("resetRequired=")
                    + std::to_string(manualHandoffControllerObject
                        ->property("resetRequiredRelativePaths").toStringList().size());
            }, 5s));
        assert(manualHandoffControllerObject->property("resetRequiredRelativePaths")
            .toStringList() == QStringList {QStringLiteral("stray.qml")});
        assert(manualHandoffControllerObject->property("editableChangedRelativePaths")
            .toStringList() == QStringList {QStringLiteral("keep.qml")});

        QObject* resetSection = waitForChild(window, "manualHandoffResetRequiredSection");
        QObject* resetButton = waitForChild(window, "manualHandoffResetButton");
        QObject* confirmResetButton = waitForChild(window, "manualHandoffConfirmResetButton");
        QObject* cancelResetButton = waitForChild(window, "manualHandoffCancelResetButton");
        QObject* resetScenarioReverifyButton =
            waitForChild(window, "manualHandoffReverifyButton");
        assert(resetSection && resetButton && confirmResetButton && cancelResetButton
            && resetScenarioReverifyButton
            && "The reset section and its confirmation buttons must exist in the real QML tree");
        assert(resetSection->property("visible").toBool());
        assert(resetButton->property("visible").toBool()
            && resetButton->property("enabled").toBool());
        assert(!resetScenarioReverifyButton->property("enabled").toBool()
            && "Reverify must be visibly blocked while the Backend projects reset-required paths");

        // Reset reveals confirmation without dispatching anything.
        assert(QMetaObject::invokeMethod(resetButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition("Reset confirmation revealed", [&] {
            return confirmResetButton->property("visible").toBool()
                && cancelResetButton->property("visible").toBool();
        }, [&] {
            return std::string("confirmVisible=")
                + (confirmResetButton->property("visible").toBool() ? "true" : "false");
        }, 3s));
        assert(!manualHandoffControllerObject->property("busy").toBool()
            && "Revealing the confirmation must never itself start the command");

        // A real Cancel click leaves the candidate untouched.
        assert(QMetaObject::invokeMethod(cancelResetButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition("Reset confirmation dismissed by Cancel", [&] {
            return !confirmResetButton->property("visible").toBool();
        }, [&] {
            return std::string("confirmVisible=")
                + (confirmResetButton->property("visible").toBool() ? "true" : "false");
        }, 3s));
        assert(fs::exists(resetCandidateRoot / "stray.qml")
            && "Cancel must never remove the stray file");
        assert(manualHandoffControllerObject->property("resetRequiredRelativePaths")
            .toStringList() == QStringList {QStringLiteral("stray.qml")});

        // Reset again, then the real Confirm click genuinely dispatches
        // resetNonEditableChanges().
        assert(QMetaObject::invokeMethod(resetButton, "clicked"));
        assert(ai::ui::test_support::waitForCondition("Reset confirmation revealed again", [&] {
            return confirmResetButton->property("visible").toBool();
        }, [] { return std::string("confirmation not revealed again"); }, 3s));
        assert(QMetaObject::invokeMethod(confirmResetButton, "clicked"));

        assert(ai::ui::test_support::waitForCondition("Manual-handoff reset click-through completion",
            [&] {
                return !manualHandoffControllerObject->property("busy").toBool()
                    && manualHandoffControllerObject->property("resetRequiredRelativePaths")
                        .toStringList().isEmpty();
            }, [&] {
                return std::string("busy=")
                    + (manualHandoffControllerObject->property("busy").toBool() ? "true" : "false")
                    + ", error="
                    + manualHandoffControllerObject->property("errorText").toString().toStdString();
            }, 5s));

        assert(manualHandoffControllerObject->property("errorText").toString().isEmpty());
        assert(!manualHandoffControllerObject->property("resetResultText").toString().isEmpty());
        assert(manualHandoffControllerObject->property("editableChangedRelativePaths")
            .toStringList() == QStringList {QStringLiteral("keep.qml")}
            && "The authorized edit must remain visible after a real Reset click");
        assert(!fs::exists(resetCandidateRoot / "stray.qml")
            && "A real Confirm-reset click must genuinely remove the stray file");
        assert(resetScenarioReverifyButton->property("enabled").toBool()
            && "Reverify must become available again after the authoritative reset projection clears");
        {
            std::ifstream keptFile(resetCandidateRoot / "keep.qml", std::ios::binary);
            std::ostringstream buffer;
            buffer << keptFile.rdbuf();
            assert(buffer.str() == "kept content edited by user"
                && "The authorized edit's real bytes must survive a real Reset click");
        }

        QObject* resetResultText = waitForChild(window, "manualHandoffResetResultText");
        assert(resetResultText && resetResultText->property("visible").toBool()
            && !resetResultText->property("text").toString().isEmpty()
            && "The reset result summary must be visible in the live scene");

        checkpoint("real QML reset non-editable changes click-through completed");
    }

    // AG-17.6: the real New Run dialog in Manifest roster mode. The payload is the
    // Backend command input: row 0 plans (the lead), every other row is an available
    // worker with workspace access, one attempt and no fallback, identified by a
    // readable, unique, ASCII member id derived from its role.
    {
        ProviderCatalogController& rosterCatalog = backend.providerCatalogController();
        const QString rosterProviderId = rosterCatalog.createProvider(
            QStringLiteral("Roster provider"), QStringLiteral("local"),
            QStringLiteral("http://127.0.0.1:11434/v1"));
        assert(!rosterProviderId.isEmpty());
        assert(!rosterCatalog.createModel(
                    rosterProviderId, QStringLiteral("roster-model"),
                    QStringLiteral("Roster model"), 32768, 4096, 0, 0).isEmpty());
        backend.runController().refreshAgentChoices();
        assert(QMetaObject::invokeMethod(
            runsPage, "openNewRunDialog", Q_ARG(QVariant, QVariant(QString {}))));
        assert(waitForProperty(newRunDialog, "visible",
            [](const QVariant& value) { return value.toBool(); }));
        assert(waitForProperty(newRunDialog, "agentChoices",
            [](const QVariant& value) { return !value.toList().isEmpty(); })
            && "The New Run dialog never received the provider/model choice");

        QObject* rosterCustomCheck = waitForChild(window, "customRosterCheck");
        QObject* manifestRosterCheck = waitForChild(window, "manifestRosterCheck");
        assert(rosterCustomCheck && manifestRosterCheck);
        assert(realProviderCheck->setProperty("checked", true));
        assert(rosterCustomCheck->setProperty("checked", true));
        assert(!manifestRosterCheck->property("checked").toBool()
            && "The legacy fixed-stage roster must stay the default");
        assert(newRunDialog->property("manifestRoster").toBool() == false);

        // Selecting Manifest mode grows a one-agent roster to the lead plus one worker.
        assert(QMetaObject::invokeMethod(manifestRosterCheck, "click"));
        assert(waitForProperty(newRunDialog, "manifestRoster",
            [](const QVariant& value) { return value.toBool(); }));
        assert(waitForProperty(newRunDialog, "agentCount",
            [](const QVariant& value) { return value.toInt() == 2; })
            && "A Manifest roster needs a lead and at least one available worker");
        QVariant workflowId;
        assert(QMetaObject::invokeMethod(newRunDialog, "workflowIdForLaunch",
            Q_RETURN_ARG(QVariant, workflowId)));
        assert(workflowId.toString() == QStringLiteral("execution-manifest-v1"));

        // Provider limits of a per-Run roster. Off by default (an empty map = the Backend
        // defaults with an automatic turn limit); switched on, the explicit values are sent and
        // only "Fixed turn limit" adds maxTurns (pinning it); the inverse restores exactly the
        // map the Backend sent; and where no roster exists the section is gone and the payload
        // stays empty even with the switch left on.
        QObject* customLimitsCheck = waitForChild(window, "customProviderLimitsCheck");
        QObject* fixedTurnsCheck = waitForChild(window, "fixedTurnLimitCheck");
        QObject* limitTurns = waitForChild(window, "limitTurns");
        QObject* limitsSection = waitForChild(window, "providerLimitsSection");
        assert(customLimitsCheck && fixedTurnsCheck && limitTurns && limitsSection);
        assert(limitsSection->property("visible").toBool());
        const auto budgetPayload = [&] {
            QVariant value;
            assert(QMetaObject::invokeMethod(newRunDialog, "providerBudgetPayload",
                Q_RETURN_ARG(QVariant, value)));
            return value.toMap();
        };
        assert(!customLimitsCheck->property("checked").toBool() && budgetPayload().isEmpty());
        assert(customLimitsCheck->setProperty("checked", true));
        QVariantMap explicitBudget = budgetPayload();
        assert(explicitBudget.value("maxInputTokens").toULongLong() == 500000
               && explicitBudget.value("maxOutputTokens").toULongLong() == 64000
               && explicitBudget.value("maxToolCalls").toULongLong() == 128
               && explicitBudget.value("maxToolResultBytes").toULongLong() == 8388608
               && explicitBudget.value("maxWallTimeMs").toULongLong() == 900000
               && explicitBudget.value("maxRequestTimeMs").toULongLong() == 240000
               && !explicitBudget.contains("maxTurns")
               && "Custom limits without a fixed turn limit must leave the turn limit automatic");
        assert(fixedTurnsCheck->setProperty("checked", true));
        assert(limitTurns->setProperty("value", 40));
        explicitBudget = budgetPayload();
        assert(explicitBudget.value("maxTurns").toULongLong() == 40
               && "A fixed turn limit must be sent as maxTurns");
        assert(QMetaObject::invokeMethod(newRunDialog, "applyProviderBudget",
            Q_ARG(QVariant, QVariant(QVariantMap {{QStringLiteral("maxInputTokens"), 100000}}))));
        assert(customLimitsCheck->property("checked").toBool()
               && !fixedTurnsCheck->property("checked").toBool());
        explicitBudget = budgetPayload();
        assert(explicitBudget.value("maxInputTokens").toULongLong() == 100000
               && !explicitBudget.contains("maxTurns")
               && "Reopening a Run with only an input-token limit must not invent a fixed turn limit");
        assert(QMetaObject::invokeMethod(newRunDialog, "applyProviderBudget",
            Q_ARG(QVariant, QVariant(QVariantMap {}))));
        assert(!customLimitsCheck->property("checked").toBool() && budgetPayload().isEmpty()
               && "A Run on the Backend defaults must reopen with the section off");
        assert(customLimitsCheck->setProperty("checked", true));
        assert(rosterCustomCheck->setProperty("checked", false));
        assert(!limitsSection->property("visible").toBool() && budgetPayload().isEmpty()
               && "Without a per-Run roster no limit may be sent");
        assert(rosterCustomCheck->setProperty("checked", true));
        assert(QMetaObject::invokeMethod(newRunDialog, "applyProviderBudget",
            Q_ARG(QVariant, QVariant(QVariantMap {}))));

        QVariant payloadValue;
        assert(QMetaObject::invokeMethod(newRunDialog, "rosterPayload",
            Q_RETURN_ARG(QVariant, payloadValue)));
        QVariantList payload = payloadValue.toList();
        assert(payload.size() == 2);
        const QVariantMap lead = payload.at(0).toMap();
        const QVariantMap worker = payload.at(1).toMap();
        assert(lead.value("stageType").toString() == QStringLiteral("architect"));
        assert(lead.value("leadArchitect").toBool() && !lead.value("workspaceAccess").toBool());
        assert(lead.value("dependencies").toList().isEmpty());
        assert(!worker.value("leadArchitect").toBool() && worker.value("workspaceAccess").toBool());
        assert(worker.value("maxAttempts").toInt() == 1
               && worker.value("fallbackProviderId").toString().isEmpty()
               && worker.value("fallbackModelId").toString().isEmpty());
        assert(worker.value("stageType").toString() == QStringLiteral("implementation-specialist")
               && "A worker's member id must be a readable slug of its role");
        assert(worker.value("dependencies").toList() == QVariantList {QStringLiteral("architect")});
        assert(worker.value("position").toInt() == 1);

        // The second roster field is "Instructions". A Manifest lead has none (the Planner prompt
        // never carries a lead's instructions, so the field would silently discard its text) and a
        // value left in the lead's row must not reach the payload; a worker keeps the field because
        // the lead reads it when assigning and the worker receives it.
        QObject* rosterModel = waitForChild(window, "rosterModel");
        assert(rosterModel);
        const auto setRosterField = [&](int row, const char* field, const QString& value) {
            assert(QMetaObject::invokeMethod(rosterModel, "setProperty",
                Q_ARG(int, row), Q_ARG(QString, QString::fromLatin1(field)),
                Q_ARG(QVariant, QVariant(value))));
        };
        const auto instructionsApplicable = [&](int row) {
            QVariant value;
            assert(QMetaObject::invokeMethod(newRunDialog, "rosterInstructionsApplicable",
                Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, QVariant(row))));
            return value.toBool();
        };
        assert(!instructionsApplicable(0) && instructionsApplicable(1)
               && "A Manifest lead has no instructions field, a worker keeps it");
        setRosterField(0, "instructions", QStringLiteral("lead text"));
        setRosterField(1, "instructions", QStringLiteral("worker text"));
        assert(QMetaObject::invokeMethod(newRunDialog, "rosterPayload",
            Q_RETURN_ARG(QVariant, payloadValue)));
        payload = payloadValue.toList();
        assert(payload.at(0).toMap().value("instructions").toString().isEmpty()
               && "A Manifest lead's instructions never reach the payload");
        assert(payload.at(1).toMap().value("instructions").toString() == QStringLiteral("worker text")
               && "A worker's instructions reach the payload unchanged");
        assert(newRunDialog->property("duplicateRoleWarning").toString().isEmpty()
               && "A single worker cannot share a role");

        // Two workers with the same role still get distinct member ids, and a worker role
        // that would collide with the reserved lead id is renamed.
        assert(QMetaObject::invokeMethod(newRunDialog, "resizeRoster", Q_ARG(QVariant, 3)));
        assert(waitForProperty(newRunDialog, "agentCount",
            [](const QVariant& value) { return value.toInt() == 3; }));
        assert(QMetaObject::invokeMethod(newRunDialog, "rosterPayload",
            Q_RETURN_ARG(QVariant, payloadValue)));
        payload = payloadValue.toList();
        assert(payload.size() == 3);
        assert(payload.at(1).toMap().value("stageType").toString()
               == QStringLiteral("implementation-specialist"));
        assert(payload.at(2).toMap().value("stageType").toString()
               == QStringLiteral("implementation-specialist-2"));

        // Workers that keep the same (default) role are told apart by the lead only through their
        // model, which is what mis-assigned a Run: the dialog warns, comparing roles trimmed and
        // case-insensitively, and distinct roles clear the warning.
        const QString sharedRoleWarning = newRunDialog->property("duplicateRoleWarning").toString();
        assert(sharedRoleWarning.contains(QStringLiteral("Implementation specialist"))
               && "Two workers with the default role must trigger the shared-role warning");
        QObject* roleWarning = waitForChild(window, "manifestRoleWarning");
        assert(roleWarning && roleWarning->property("visible").toBool());
        setRosterField(2, "role", QStringLiteral("recent-alerts"));
        assert(newRunDialog->property("duplicateRoleWarning").toString().isEmpty()
               && !roleWarning->property("visible").toBool()
               && "Distinct roles clear the shared-role warning");
        setRosterField(2, "role", QStringLiteral("  implementation SPECIALIST "));
        assert(!newRunDialog->property("duplicateRoleWarning").toString().isEmpty()
               && "A role is compared trimmed and case-insensitively");

        // PC-2: the interface contracts editor. It collects text and nothing else (the Backend validates
        // and answers with its own error): here only that it exists for a Manifest roster, sends the
        // list of maps the Backend expects, adds with a free id, removes, and restores exactly what the
        // Backend froze for Edit & rerun and drafts.
        QObject* contractsSection = waitForChild(window, "interfaceContractsSection");
        QObject* contractsAdd = waitForChild(window, "interfaceContractsAddButton");
        assert(contractsSection && contractsAdd && contractsSection->property("visible").toBool());
        const auto contractsPayload = [&] {
            QVariant value;
            assert(QMetaObject::invokeMethod(newRunDialog, "interfaceContractsPayload",
                Q_RETURN_ARG(QVariant, value)));
            return value.toList();
        };
        assert(contractsPayload().isEmpty() && "A new dialog starts without contracts");
        assert(QMetaObject::invokeMethod(contractsAdd, "click"));
        assert(QMetaObject::invokeMethod(contractsAdd, "click"));
        assert(waitForProperty(contractsSection, "count",
            [](const QVariant& value) { return value.toInt() == 2; }));
        const QVariantList added = contractsPayload();
        assert(added.size() == 2 && added.at(0).toMap().value("id").toString() == QStringLiteral("C1")
               && added.at(1).toMap().value("id").toString() == QStringLiteral("C2")
               && added.at(0).toMap().value("kind").toString() == QStringLiteral("interface_contract")
               && added.at(0).toMap().value("text").toString().isEmpty()
               && "Added entries get distinct free ids and start empty");

        const QVariantList frozenContracts {
            QVariantMap {{QStringLiteral("id"), QStringLiteral("P1")},
                {QStringLiteral("kind"), QStringLiteral("interface_contract")},
                {QStringLiteral("title"), QStringLiteral("Loss entry")},
                {QStringLiteral("text"), QStringLiteral("property string metricId")},
                {QStringLiteral("appliesToPaths"),
                    QVariantList {QStringLiteral("qml/models"), QStringLiteral("qml/pages/History")}}},
            QVariantMap {{QStringLiteral("id"), QStringLiteral("K1")},
                {QStringLiteral("kind"), QStringLiteral("constraint")},
                {QStringLiteral("title"), QStringLiteral("Rule")},
                {QStringLiteral("text"), QStringLiteral("No file may exceed 300 lines.")},
                {QStringLiteral("appliesToPaths"), QVariantList {}}}};
        assert(QMetaObject::invokeMethod(newRunDialog, "applyInterfaceContracts",
            Q_ARG(QVariant, QVariant(frozenContracts))));
        assert(waitForProperty(contractsSection, "count",
            [](const QVariant& value) { return value.toInt() == 2; }));
        const QVariantList restored = contractsPayload();
        assert(restored.size() == 2
               && restored.at(0).toMap().value("id").toString() == QStringLiteral("P1")
               && restored.at(0).toMap().value("text").toString() == QStringLiteral("property string metricId")
               && restored.at(0).toMap().value("appliesToPaths").toString()
                   == QStringLiteral("qml/models, qml/pages/History")
               && restored.at(1).toMap().value("kind").toString() == QStringLiteral("constraint")
               && restored.at(1).toMap().value("appliesToPaths").toString().isEmpty()
               && "Reopening restores exactly the contracts the Backend froze");

        // Run Details shows what the selected Run froze, read-only.
        const QVariantMap selectedRunData = runDetailsPanel->property("runData").toMap();
        assert(!selectedRunData.isEmpty());
        QVariantMap contractsRunData = selectedRunData;
        contractsRunData.insert(QStringLiteral("interfaceContracts"), frozenContracts);
        contractsRunData.insert(QStringLiteral("interfaceContractsFingerprint"), QString(64, QChar('a')));
        assert(runDetailsPanel->setProperty("runData", contractsRunData));
        QObject* contractsPanel = waitForChild(window, "runContractsPanel");
        assert(contractsPanel && waitForProperty(contractsPanel, "visible",
            [](const QVariant& value) { return value.toBool(); }));
        QObject* contractsSummary = waitForChild(window, "runContractsSummary");
        // Repeater delegates are not QObject children of the window: ask the Repeater for its items.
        QObject* contractsRepeater = waitForChild(window, "runContractsRepeater");
        assert(contractsRepeater && waitForProperty(contractsRepeater, "count",
            [](const QVariant& value) { return value.toInt() == 2; }));
        QQuickItem* contractItem = nullptr;
        assert(QMetaObject::invokeMethod(contractsRepeater, "itemAt",
            Q_RETURN_ARG(QQuickItem*, contractItem), Q_ARG(int, 0)));
        QObject* contractLine = contractItem
            ? contractItem->findChild<QObject*>(QStringLiteral("runContractLine")) : nullptr;
        QObject* contractText = contractItem
            ? contractItem->findChild<QObject*>(QStringLiteral("runContractText")) : nullptr;
        assert(contractsSummary && contractLine && contractText
               && contractsSummary->property("text").toString().contains(QStringLiteral("2 frozen"))
               && contractsSummary->property("text").toString().contains(QStringLiteral("aaaaaaaaaaaa"))
               && contractLine->property("text").toString().contains(QStringLiteral("P1"))
               && contractLine->property("text").toString().contains(
                   QStringLiteral("qml/models, qml/pages/History")));
        // The exact text the Backend delivers is visible, read-only and selectable - not just id/title/scope.
        assert(contractText->property("text").toString() == QStringLiteral("property string metricId")
               && contractText->property("readOnly").toBool() && contractText->property("selectByMouse").toBool());
        QQuickItem* secondItem = nullptr;
        assert(QMetaObject::invokeMethod(contractsRepeater, "itemAt",
            Q_RETURN_ARG(QQuickItem*, secondItem), Q_ARG(int, 1)) && secondItem);
        QObject* secondText = secondItem->findChild<QObject*>(QStringLiteral("runContractText"));
        assert(secondText
               && secondText->property("text").toString() == QStringLiteral("No file may exceed 300 lines."));
        assert(runDetailsPanel->setProperty("runData", selectedRunData));
        assert(waitForProperty(contractsPanel, "visible",
            [](const QVariant& value) { return !value.toBool(); })
            && "A Run without contracts shows no contracts block");

        QObject* contractsEditorRepeater = waitForChild(window, "interfaceContractsRepeater");
        QQuickItem* firstContractRow = nullptr;
        assert(contractsEditorRepeater && QMetaObject::invokeMethod(contractsEditorRepeater, "itemAt",
            Q_RETURN_ARG(QQuickItem*, firstContractRow), Q_ARG(int, 0)) && firstContractRow);
        QObject* contractsRemove = firstContractRow->findChild<QObject*>(
            QStringLiteral("interfaceContractRemoveButton"));
        assert(contractsRemove && QMetaObject::invokeMethod(contractsRemove, "click"));
        assert(waitForProperty(contractsSection, "count",
            [](const QVariant& value) { return value.toInt() == 1; }));
        assert(contractsPayload().size() == 1
               && contractsPayload().at(0).toMap().value("id").toString() == QStringLiteral("K1"));

        // Back to the fixed-stage roster: the legacy payload (and no workflow id) returns.
        assert(QMetaObject::invokeMethod(manifestRosterCheck, "click"));
        assert(waitForProperty(newRunDialog, "manifestRoster",
            [](const QVariant& value) { return !value.toBool(); }));
        assert(newRunDialog->property("duplicateRoleWarning").toString().isEmpty()
               && !roleWarning->property("visible").toBool()
               && "The shared-role warning belongs to a Manifest roster only");
        assert(instructionsApplicable(0)
               && "The legacy roster keeps the lead's instructions field");
        assert(QMetaObject::invokeMethod(newRunDialog, "workflowIdForLaunch",
            Q_RETURN_ARG(QVariant, workflowId)));
        assert(workflowId.toString().isEmpty());
        assert(QMetaObject::invokeMethod(newRunDialog, "rosterPayload",
            Q_RETURN_ARG(QVariant, payloadValue)));
        assert(!contractsSection->property("visible").toBool() && contractsPayload().isEmpty()
               && "Interface contracts exist for a Manifest roster only: a hidden editor sends nothing");
        assert(QMetaObject::invokeMethod(newRunDialog, "applyInterfaceContracts",
            Q_ARG(QVariant, QVariant(QVariantList {}))));
        assert(!payloadValue.toList().at(1).toMap().value("workspaceAccess").toBool()
               && "The legacy roster keeps its own workspace toggle");
        assert(payloadValue.toList().at(1).toMap().value("maxAttempts").toInt() == 2);
        assert(payloadValue.toList().at(0).toMap().value("instructions").toString()
                   == QStringLiteral("lead text")
               && "A legacy lead's instructions are consumed and must stay in the payload");

        assert(QMetaObject::invokeMethod(newRunDialog, "close"));
        checkpoint("real QML Manifest roster mode payload verified");
    }

    // AG-17.7C-2: real legacy Runs, the real Edit & rerun routing and the real conversion dialog.
    // A lone workspace lead has NO legacy Edit & rerun (only the conversion dialog opens), a
    // proposal roster keeps its legacy dialog and offers the conversion as a separate choice, a
    // Retry that needs a decision opens the dialog instead of creating a Run, and pressing the one
    // explicit button creates exactly one audited Manifest child.
    {
        ProviderCatalogController& conversionCatalog = backend.providerCatalogController();
        const QString conversionProviderId = conversionCatalog.createProvider(
            QStringLiteral("Conversion provider"), QStringLiteral("local"),
            QStringLiteral("http://127.0.0.1:11434/v1"));
        assert(!conversionProviderId.isEmpty());
        const QString conversionModelId = conversionCatalog.createModel(
            conversionProviderId, QStringLiteral("conversion-model"),
            QStringLiteral("Conversion model"), 32768, 4096, 0, 0);
        assert(!conversionModelId.isEmpty());
        RunController& conversionRuns = backend.runController();
        conversionRuns.refreshAgentChoices();
        assert(ai::ui::test_support::waitForCondition("conversion catalog choice loaded", [&] {
            for (const QVariant& choice : conversionRuns.agentChoices()) {
                if (choice.toMap().value(QStringLiteral("providerId")).toString() == conversionProviderId)
                    return true;
            }
            return false;
        }, [] { return std::string("the new provider never reached the agent choices"); }, 5s));

        const QString secretInstructions = QStringLiteral("Analyse the workspace with great care.");
        const auto conversionRow = [&](const QString& stageType, const QString& role, bool lead,
                                       bool workspaceAccess, int position,
                                       const QString& instructions, int attempts) {
            return QVariantMap {
                {QStringLiteral("stageType"), stageType},
                {QStringLiteral("providerId"), conversionProviderId},
                {QStringLiteral("modelId"), conversionModelId},
                {QStringLiteral("agentName"), role},
                {QStringLiteral("role"), role},
                {QStringLiteral("instructions"), instructions},
                {QStringLiteral("leadArchitect"), lead},
                {QStringLiteral("workspaceAccess"), workspaceAccess},
                {QStringLiteral("position"), position},
                {QStringLiteral("dependencies"),
                    lead ? QVariantList {} : QVariantList {QStringLiteral("architect")}},
                {QStringLiteral("maxAttempts"), attempts}};
        };
        const QStringList terminalStatuses {
            QStringLiteral("failed"), QStringLiteral("cancelled"), QStringLiteral("completed")};
        const auto startLegacy = [&](const QString& title, const QVariantList& roster) {
            const QString id = conversionRuns.startRunWithRoster(
                projectId, title, QStringLiteral("Legacy task"), roster, 2,
                QString {}, QString {}, QString {}, false, false);
            assert(!id.isEmpty());
            static_cast<void>(waitForStatus(runsModel, id, terminalStatuses, 20s));
            return id;
        };
        const QString loneRunId = startLegacy(QStringLiteral("Lone lead with instructions"),
            {conversionRow(QStringLiteral("architect"), QStringLiteral("Analyst"), true, true, 0,
                secretInstructions, 1)});
        const QString proposalRunId = startLegacy(QStringLiteral("Proposal roster"),
            {conversionRow(QStringLiteral("architect"), QStringLiteral("Lead"), true, false, 0,
                secretInstructions, 2),
             conversionRow(QStringLiteral("qml-worker"), QStringLiteral("QML worker"), false, true, 1, {}, 1),
             conversionRow(QStringLiteral("cpp-worker"), QStringLiteral("C++ worker"), false, true, 2, {}, 1)});

        const auto waitForVisibleDialog = [&](const char* name) {
            QObject* found = nullptr;
            assert(ai::ui::test_support::waitForCondition("dialog is visible", [&] {
                found = window->findChild<QObject*>(QString::fromLatin1(name));
                return found != nullptr && found->property("visible").toBool();
            }, [name] { return std::string("objectName=") + name; }, 5s));
            return found;
        };
        const auto childCount = [&](const char* repeaterName) {
            QObject* repeater = waitForChild(window, repeaterName);
            assert(repeater);
            return repeater->property("count").toInt();
        };

        // 1. A lone workspace lead: ONLY the conversion dialog opens, never the legacy one.
        assert(QMetaObject::invokeMethod(runsPage, "openEditRunDialog",
            Q_ARG(QVariant, QVariant(loneRunId))));
        QObject* conversionDialog = waitForVisibleDialog("legacyConversionDialog");
        assert(!newRunDialog->property("visible").toBool()
            && "A lone workspace lead has no legacy Edit & rerun variant");
        assert(conversionDialog->property("convertible").toBool());
        assert(waitForProperty(waitForChild(window, "conversionRoster"), "count",
            [](const QVariant& value) { return value.toInt() == 1; })
            && "The locked target roster shows the single converted member");
        assert(waitForProperty(waitForChild(window, "conversionDropped"), "count",
            [](const QVariant& value) { return value.toInt() == 1; })
            && "Exactly the instructions are not carried over");
        QObject* instructionsWarning = waitForChild(window, "conversionInstructionsWarning");
        assert(instructionsWarning && instructionsWarning->property("visible").toBool()
            && "Dropped instructions must carry an explicit warning");
        QObject* titleField = waitForChild(window, "conversionTitleField");
        QObject* taskField = waitForChild(window, "conversionTaskField");
        assert(titleField && taskField);
        assert(titleField->property("text").toString() == QStringLiteral("Lone lead with instructions")
            && "The user's own fields start from the Backend's defaults");
        assert(taskField->property("text").toString() == QStringLiteral("Legacy task"));
        // Nothing is created before the explicit button is pressed.
        const int runsBeforeConversion = runsModel->rowCount();
        assert(titleField->setProperty("text", QStringLiteral("Converted lone lead")));
        QObject* convertButton = waitForChild(window, "conversionConvertButton");
        assert(convertButton && convertButton->property("enabled").toBool());
        assert(runsModel->rowCount() == runsBeforeConversion);
        assert(QMetaObject::invokeMethod(convertButton, "click"));
        assert(waitForRowCount(runsModel, runsBeforeConversion + 1));

        QString convertedRunId;
        for (int rowIndex = 0; rowIndex < runsModel->rowCount(); ++rowIndex) {
            const QModelIndex index = runsModel->index(rowIndex, 0);
            if (runsModel->data(index, RunListModel::ParentRunIdRole).toString() == loneRunId) {
                convertedRunId = runsModel->data(index, RunListModel::RunIdRole).toString();
                assert(runsModel->data(index, RunListModel::WorkflowIdRole).toString()
                       == QStringLiteral("execution-manifest-v1"));
                assert(runsModel->data(index, RunListModel::TitleRole).toString()
                       == QStringLiteral("Converted lone lead"));
            }
        }
        assert(!convertedRunId.isEmpty() && "The conversion must create a Manifest child of the source");
        static_cast<void>(waitForStatus(runsModel, convertedRunId, terminalStatuses, 20s));

        // AG-17.7C-3: Run Details shows the stored audit of the converted Run - and nothing for its
        // source - and the history rows carry the Backend's marker.
        assert(QMetaObject::invokeMethod(runsPage, "selectRun", Q_ARG(QVariant, QVariant(convertedRunId))));
        QObject* auditPanel = waitForChild(window, "legacyConversionRecordPanel");
        assert(auditPanel);
        assert(waitForProperty(auditPanel, "present", [](const QVariant& value) { return value.toBool(); })
            && "Run Details must read the conversion record of the selected converted Run");
        assert(waitForProperty(auditPanel, "visible", [](const QVariant& value) { return value.toBool(); }));
        QObject* auditConfirmation = waitForChild(window, "conversionRecordConfirmation");
        QObject* auditSource = waitForChild(window, "conversionRecordSource");
        QObject* auditAudit = waitForChild(window, "conversionRecordAudit");
        QObject* auditDropped = waitForChild(window, "conversionRecordDropped");
        QObject* auditInstructions = waitForChild(window, "conversionRecordInstructionsNote");
        assert(auditConfirmation && auditSource && auditAudit && auditDropped && auditInstructions);
        assert(auditConfirmation->property("text").toString().startsWith(
                   QStringLiteral("You confirmed this conversion on "))
            && "An explicit conversion shows when the user confirmed it");
        assert(auditSource->property("text").toString().contains(QStringLiteral("Lone lead with instructions"))
            && auditSource->property("text").toString().contains(loneRunId));
        assert(auditAudit->property("text").toString().contains(QStringLiteral("legacy-roster-adapter.v1")));
        assert(waitForProperty(auditDropped, "count", [](const QVariant& value) { return value.toInt() == 1; })
            && "The one dropped setting (the instructions) is listed");
        assert(auditInstructions->property("visible").toBool()
            && "The instructions note follows the Backend's flag");
        // The source Run is an ordinary legacy Run: Run Details shows no audit for it.
        assert(QMetaObject::invokeMethod(runsPage, "selectRun", Q_ARG(QVariant, QVariant(loneRunId))));
        assert(waitForProperty(auditPanel, "present", [](const QVariant& value) { return !value.toBool(); })
            && "An ordinary Run shows no conversion audit");
        // The history rows: the converted Run is marked, its source and every other Run is not.
        {
            bool sawConverted = false;
            bool sawSource = false;
            const QVariantList snapshot = runsPage->property("runsSnapshot").toList();
            for (const QVariant& entry : snapshot) {
                const QVariantMap run = entry.toMap();
                if (run.value(QStringLiteral("runId")).toString() == convertedRunId) {
                    sawConverted = run.value(QStringLiteral("legacyConversion")).toString()
                            == QStringLiteral("explicit_user")
                        && run.value(QStringLiteral("legacyConversionLabel")).toString()
                            == QStringLiteral("Converted");
                } else if (run.value(QStringLiteral("runId")).toString() == loneRunId) {
                    sawSource = run.value(QStringLiteral("legacyConversion")).toString().isEmpty();
                }
            }
            assert(sawConverted && "The history must mark the converted Run");
            assert(sawSource && "The source Run is not a converted Run and must carry no marker");
        }

        // 2. A proposal roster keeps its legacy dialog; the conversion is a separate, explicit choice.
        const int runsBeforeOffer = runsModel->rowCount();
        assert(QMetaObject::invokeMethod(runsPage, "openEditRunDialog",
            Q_ARG(QVariant, QVariant(proposalRunId))));
        assert(waitForProperty(newRunDialog, "visible", [](const QVariant& value) { return value.toBool(); }));
        assert(waitForProperty(newRunDialog, "conversionOffered",
            [](const QVariant& value) { return value.toBool(); })
            && "The Backend offers a conversion for a proposal roster");
        QObject* convertToManifestButton = waitForChild(window, "convertToManifestButton");
        assert(convertToManifestButton && convertToManifestButton->property("visible").toBool());
        assert(QMetaObject::invokeMethod(convertToManifestButton, "click"));
        assert(waitForProperty(newRunDialog, "visible", [](const QVariant& value) { return !value.toBool(); }));
        QObject* offeredDialog = waitForVisibleDialog("legacyConversionDialog");
        assert(offeredDialog->property("convertible").toBool());
        assert(waitForProperty(waitForChild(window, "conversionRoster"), "count",
            [](const QVariant& value) { return value.toInt() == 3; }));
        assert(waitForProperty(waitForChild(window, "conversionDropped"), "count",
            [](const QVariant& value) { return value.toInt() >= 2; })
            && "Attempts and instructions of the lead are listed as not carried over");
        // What the dialog shows follows the Backend's preview, in both directions: a preview that
        // says "not available" disables the one button and says why; one without dropped
        // instructions hides the instructions warning.
        const auto readPreview = [](QObject* dialogObject) {
            QVariant value = dialogObject->property("preview");
            if (value.canConvert<QJSValue>())
                value = value.value<QJSValue>().toVariant();
            return value.toMap();
        };
        const QVariantMap realPreview = readPreview(offeredDialog);
        assert(realPreview.value(QStringLiteral("available")).toBool()
            && realPreview.value(QStringLiteral("confirmationFingerprint")).toString().size() == 64);
        QObject* offeredConvertButton = waitForChild(window, "conversionConvertButton");
        QObject* offeredWarning = waitForChild(window, "conversionInstructionsWarning");
        QObject* notAvailableNote = waitForChild(window, "conversionNotAvailable");
        assert(offeredConvertButton && offeredWarning && notAvailableNote);
        assert(offeredWarning->property("visible").toBool() && !notAvailableNote->property("visible").toBool());
        QVariantMap unavailable = realPreview;
        unavailable.insert(QStringLiteral("available"), false);
        assert(offeredDialog->setProperty("preview", QVariant(unavailable)));
        assert(waitForProperty(notAvailableNote, "visible", [](const QVariant& value) { return value.toBool(); }));
        assert(!offeredConvertButton->property("enabled").toBool()
            && "A conversion the Backend says is not available cannot be started");
        QVariantMap noInstructions = realPreview;
        noInstructions.insert(QStringLiteral("hasDroppedInstructions"), false);
        assert(offeredDialog->setProperty("preview", QVariant(noInstructions)));
        assert(waitForProperty(offeredWarning, "visible", [](const QVariant& value) { return !value.toBool(); }));
        assert(waitForProperty(offeredConvertButton, "enabled", [](const QVariant& value) { return value.toBool(); }));

        // A stale confirmation: the Backend refuses it, the dialog stays open with what the user
        // typed, shows the Backend's CURRENT preview, and only a second press confirms THAT.
        QVariantMap stale = realPreview;
        stale.insert(QStringLiteral("confirmationFingerprint"), QString(64, QLatin1Char('0')));
        assert(offeredDialog->setProperty("preview", QVariant(stale)));
        QObject* offeredTitle = waitForChild(window, "conversionTitleField");
        assert(offeredTitle && offeredTitle->setProperty("text", QStringLiteral("Typed before the refusal")));
        assert(QMetaObject::invokeMethod(offeredConvertButton, "click"));
        assert(waitForProperty(offeredDialog, "errorText",
            [](const QVariant& value) { return !value.toString().isEmpty(); })
            && "A refused confirmation must be explained in the dialog");
        assert(runsModel->rowCount() == runsBeforeOffer && "A stale confirmation creates no Run");
        assert(offeredDialog->property("visible").toBool());
        assert(waitForProperty(offeredDialog, "preview", [&](const QVariant&) {
            return readPreview(offeredDialog).value(QStringLiteral("confirmationFingerprint")).toString()
                == realPreview.value(QStringLiteral("confirmationFingerprint")).toString();
        }) && "The dialog must show the Backend's current preview again");
        assert(offeredTitle->property("text").toString() == QStringLiteral("Typed before the refusal")
            && "Reloading the preview must keep what the user typed");
        assert(QMetaObject::invokeMethod(offeredConvertButton, "click"));
        assert(waitForRowCount(runsModel, runsBeforeOffer + 1));
        for (int rowIndex = 0; rowIndex < runsModel->rowCount(); ++rowIndex) {
            const QModelIndex index = runsModel->index(rowIndex, 0);
            if (runsModel->data(index, RunListModel::ParentRunIdRole).toString() == proposalRunId) {
                assert(runsModel->data(index, RunListModel::TitleRole).toString()
                       == QStringLiteral("Typed before the refusal"));
                static_cast<void>(waitForStatus(runsModel,
                    runsModel->data(index, RunListModel::RunIdRole).toString(), terminalStatuses, 20s));
            }
        }
        const int runsAfterOfferConversion = runsModel->rowCount();

        // 3. A Retry the Backend cannot do silently opens the dialog and creates no Run.
        assert(ai::ui::test_support::waitForCondition("the converted-from-offer dialog is gone", [&] {
            QObject* leftover = window->findChild<QObject*>(QStringLiteral("legacyConversionDialog"));
            return leftover == nullptr || !leftover->property("visible").toBool();
        }, [] { return std::string("the cancelled dialog stayed open"); }, 5s));
        assert(QMetaObject::invokeMethod(runsPage, "retryRun", Q_ARG(QVariant, QVariant(loneRunId))));
        QObject* retryDialog = waitForVisibleDialog("legacyConversionDialog");
        assert(retryDialog->property("convertible").toBool());
        assert(childCount("conversionRoster") == 1);
        assert(runsModel->rowCount() == runsAfterOfferConversion && "A routed Retry creates no Run");
        QObject* cancelRetry = waitForChild(window, "conversionCancelButton");
        assert(cancelRetry && QMetaObject::invokeMethod(cancelRetry, "click"));
        checkpoint("real QML legacy conversion dialog click-through completed");
    }

    // ── M2c: the pricing dialog, the long-context tier and the analytics wording ──────────────────
    {
        // A large-window model: a long-context threshold must be strictly below the context window.
        assert(QMetaObject::invokeMethod(
            providersPage, "addModel",
            Q_ARG(QVariant, providerId),
            Q_ARG(QVariant, QStringLiteral("qml-tier-model")),
            Q_ARG(QVariant, QStringLiteral("QML tier model")),
            Q_ARG(QVariant, QStringLiteral("1.05M")),
            Q_ARG(QVariant, QStringLiteral("2.00")),
            Q_ARG(QVariant, QStringLiteral("10.00"))));
        const auto snapshotOfTierModel = [&]() -> QVariantMap {
            for (const QVariant& entry : providersPage->property("modelsSnapshot").toList()) {
                if (entry.toMap().value(QStringLiteral("apiModelId")).toString()
                    == QStringLiteral("qml-tier-model")) {
                    return entry.toMap();
                }
            }
            return {};
        };
        assert(ai::ui::test_support::waitForCondition("the tier model is in the snapshot", [&] {
            return !snapshotOfTierModel().isEmpty();
        }, [] { return std::string("modelsSnapshot has no qml-tier-model"); }, 5s));
        const QString tierModelId = snapshotOfTierModel().value(QStringLiteral("modelId")).toString();
        assert(!tierModelId.isEmpty());
        assert(!snapshotOfTierModel().value(QStringLiteral("longContextEnabled")).toBool());

        QObject* pricingDialog = waitForChild(window, "pricingDialog");
        assert(pricingDialog);
        const auto field = [&](const char* name) {
            QObject* item = waitForChild(pricingDialog, name);
            assert(item && "a pricing dialog field is missing");
            return item;
        };
        const auto dialogText = [&](const char* name) { return field(name)->property("text").toString(); };
        const auto closeDialog = [&] {
            QMetaObject::invokeMethod(pricingDialog, "close");
            assert(ai::ui::test_support::waitForCondition("the pricing dialog closes", [&] {
                return !pricingDialog->property("visible").toBool();
            }, [] { return std::string("the pricing dialog stayed open"); }, 5s));
        };

        // 1. A model without a tier opens with EMPTY tier fields: nothing is guessed or pre-filled, and
        //    the flat / cache values are the stored ones.
        assert(QMetaObject::invokeMethod(pricingDialog, "openForModel",
            Q_ARG(QVariant, QVariant(snapshotOfTierModel()))));
        assert(!field("longContextCheck")->property("checked").toBool());
        assert(dialogText("longContextThresholdField").isEmpty());
        assert(dialogText("longContextInputPriceField").isEmpty());
        assert(dialogText("inputPriceField") == QStringLiteral("2"));
        assert(dialogText("outputPriceField") == QStringLiteral("10"));
        closeDialog();

        // 2. Save the whole form with a tier through the page command (ONE atomic Backend update).
        assert(QMetaObject::invokeMethod(providersPage, "saveModelPricing",
            Q_ARG(QVariant, tierModelId), Q_ARG(QVariant, QStringLiteral("2")),
            Q_ARG(QVariant, QStringLiteral("10")), Q_ARG(QVariant, QStringLiteral("2.5")),
            Q_ARG(QVariant, QStringLiteral("0.2")), Q_ARG(QVariant, true),
            Q_ARG(QVariant, QStringLiteral("272000")), Q_ARG(QVariant, QStringLiteral("4")),
            Q_ARG(QVariant, QStringLiteral("15")), Q_ARG(QVariant, QStringLiteral("5")),
            Q_ARG(QVariant, QStringLiteral("0.4"))));
        assert(ai::ui::test_support::waitForCondition("the tier is in the snapshot", [&] {
            return snapshotOfTierModel().value(QStringLiteral("longContextEnabled")).toBool();
        }, [] { return std::string("the saved tier never reached the snapshot"); }, 5s));
        {
            const QVariantMap saved = snapshotOfTierModel();
            assert(saved.value(QStringLiteral("longContextThresholdInputTokens")).toULongLong() == 272'000);
            assert(saved.value(QStringLiteral("longContextInputPriceMicros")).toULongLong() == 4'000'000);
            assert(saved.value(QStringLiteral("longContextOutputPriceMicros")).toULongLong() == 15'000'000);
            assert(saved.value(QStringLiteral("longContextCacheWritePriceMicros")).toULongLong() == 5'000'000);
            assert(saved.value(QStringLiteral("longContextCacheReadPriceMicros")).toULongLong() == 400'000);
            // The CACHE prices are in the snapshot too (the dialog prefill depends on them).
            assert(saved.value(QStringLiteral("cacheWritePriceMicros")).toULongLong() == 2'500'000);
            assert(saved.value(QStringLiteral("cacheReadPriceMicros")).toULongLong() == 200'000);
        }

        // 3. Reopening shows exactly what is stored - the cache prices and the tier included.
        assert(QMetaObject::invokeMethod(pricingDialog, "openForModel",
            Q_ARG(QVariant, QVariant(snapshotOfTierModel()))));
        assert(field("longContextCheck")->property("checked").toBool());
        assert(dialogText("cacheWritePriceField") == QStringLiteral("2.5"));
        assert(dialogText("cacheReadPriceField") == QStringLiteral("0.2"));
        assert(dialogText("longContextThresholdField") == QStringLiteral("272000"));
        assert(dialogText("longContextInputPriceField") == QStringLiteral("4"));
        assert(dialogText("longContextOutputPriceField") == QStringLiteral("15"));
        assert(dialogText("longContextCacheWritePriceField") == QStringLiteral("5"));
        assert(dialogText("longContextCacheReadPriceField") == QStringLiteral("0.4"));

        // 4. "Save pricing" without touching anything changes nothing (a dialog that lost a value on
        //    open would silently overwrite it with zero here).
        const QVariantMap beforeUnchangedSave = snapshotOfTierModel();
        assert(QMetaObject::invokeMethod(field("savePricingButton"), "click"));
        assert(ai::ui::test_support::waitForCondition("the unchanged save closes the dialog", [&] {
            return !pricingDialog->property("visible").toBool();
        }, [] { return std::string("the pricing dialog stayed open after an unchanged save"); }, 5s));
        assert(snapshotOfTierModel() == beforeUnchangedSave);

        // 5. An edit that does not name the tier keeps it (the model form's own command).
        assert(QMetaObject::invokeMethod(providersPage, "saveModelEdit",
            Q_ARG(QVariant, tierModelId), Q_ARG(QVariant, QStringLiteral("qml-tier-model")),
            Q_ARG(QVariant, QStringLiteral("QML tier model (renamed)")),
            Q_ARG(QVariant, QStringLiteral("1.05M")), Q_ARG(QVariant, 128000),
            Q_ARG(QVariant, QStringLiteral("3")), Q_ARG(QVariant, QStringLiteral("11"))));
        assert(ai::ui::test_support::waitForCondition("the rename reached the snapshot", [&] {
            return snapshotOfTierModel().value(QStringLiteral("name")).toString()
                == QStringLiteral("QML tier model (renamed)");
        }, [] { return std::string("the model edit never reached the snapshot"); }, 5s));
        {
            const QVariantMap edited = snapshotOfTierModel();
            assert(edited.value(QStringLiteral("longContextEnabled")).toBool());
            assert(edited.value(QStringLiteral("longContextThresholdInputTokens")).toULongLong() == 272'000);
            assert(edited.value(QStringLiteral("longContextInputPriceMicros")).toULongLong() == 4'000'000);
            assert(edited.value(QStringLiteral("cacheWritePriceMicros")).toULongLong() == 2'500'000);
        }

        // 5b. The older flat-only form (no long-context arguments at all) leaves the stored tier
        //     untouched - it can never clear it by omission.
        assert(QMetaObject::invokeMethod(providersPage, "saveModelPricing",
            Q_ARG(QVariant, tierModelId), Q_ARG(QVariant, QStringLiteral("4")),
            Q_ARG(QVariant, QStringLiteral("12")), Q_ARG(QVariant, QStringLiteral("2.5")),
            Q_ARG(QVariant, QStringLiteral("0.2")), Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
        assert(ai::ui::test_support::waitForCondition("the flat-only save reached the snapshot", [&] {
            return snapshotOfTierModel().value(QStringLiteral("inputPriceMicros")).toULongLong() == 4'000'000;
        }, [] { return std::string("the flat-only save never reached the snapshot"); }, 5s));
        {
            const QVariantMap flatOnly = snapshotOfTierModel();
            assert(flatOnly.value(QStringLiteral("outputPriceMicros")).toULongLong() == 12'000'000);
            assert(flatOnly.value(QStringLiteral("longContextEnabled")).toBool());
            assert(flatOnly.value(QStringLiteral("longContextThresholdInputTokens")).toULongLong() == 272'000);
            assert(flatOnly.value(QStringLiteral("longContextCacheReadPriceMicros")).toULongLong() == 400'000);
        }

        // 6. A refused save shows the BACKEND's own error text in the dialog and changes nothing:
        //    long-context prices without a threshold.
        ProviderCatalogController& catalogController = backend.providerCatalogController();
        const QVariantMap beforeRefusal = snapshotOfTierModel();
        assert(QMetaObject::invokeMethod(pricingDialog, "openForModel",
            Q_ARG(QVariant, QVariant(beforeRefusal))));
        assert(QMetaObject::invokeMethod(providersPage, "saveModelPricing",
            Q_ARG(QVariant, tierModelId), Q_ARG(QVariant, QStringLiteral("9")),
            Q_ARG(QVariant, QStringLiteral("9")), Q_ARG(QVariant, QStringLiteral("9")),
            Q_ARG(QVariant, QStringLiteral("9")), Q_ARG(QVariant, true),
            Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("4")),
            Q_ARG(QVariant, QString()), Q_ARG(QVariant, QString()), Q_ARG(QVariant, QString())));
        const QString backendText = QString::fromLatin1(ai::application::longContextPricingProblemMessage(
            ai::application::LongContextPricingProblem::PricesWithoutThreshold));
        assert(pricingDialog->property("errorText").toString() == backendText);
        assert(catalogController.lastError() == backendText);
        assert(pricingDialog->property("visible").toBool() && "a refused save must keep the dialog open");
        assert(snapshotOfTierModel() == beforeRefusal);
        // A threshold that is not a whole number is refused by the page before any Backend call.
        assert(QMetaObject::invokeMethod(providersPage, "saveModelPricing",
            Q_ARG(QVariant, tierModelId), Q_ARG(QVariant, QStringLiteral("2")),
            Q_ARG(QVariant, QStringLiteral("10")), Q_ARG(QVariant, QStringLiteral("0")),
            Q_ARG(QVariant, QStringLiteral("0")), Q_ARG(QVariant, true),
            Q_ARG(QVariant, QStringLiteral("27x2")), Q_ARG(QVariant, QString()),
            Q_ARG(QVariant, QString()), Q_ARG(QVariant, QString()), Q_ARG(QVariant, QString())));
        assert(pricingDialog->property("errorText").toString()
            == QStringLiteral("Enter a whole number of input tokens for the threshold."));
        assert(snapshotOfTierModel() == beforeRefusal);
        closeDialog();

        // 7. The tier is removed only explicitly: longContextEnabled == false sends zeros.
        assert(QMetaObject::invokeMethod(providersPage, "saveModelPricing",
            Q_ARG(QVariant, tierModelId), Q_ARG(QVariant, QStringLiteral("2")),
            Q_ARG(QVariant, QStringLiteral("10")), Q_ARG(QVariant, QStringLiteral("2.5")),
            Q_ARG(QVariant, QStringLiteral("0.2")), Q_ARG(QVariant, false),
            Q_ARG(QVariant, QStringLiteral("272000")), Q_ARG(QVariant, QStringLiteral("4")),
            Q_ARG(QVariant, QStringLiteral("15")), Q_ARG(QVariant, QStringLiteral("5")),
            Q_ARG(QVariant, QStringLiteral("0.4"))));
        assert(ai::ui::test_support::waitForCondition("the tier is removed", [&] {
            return !snapshotOfTierModel().value(QStringLiteral("longContextEnabled")).toBool();
        }, [] { return std::string("the explicit removal never reached the snapshot"); }, 5s));
        assert(snapshotOfTierModel().value(QStringLiteral("longContextThresholdInputTokens")).toULongLong() == 0);
        assert(snapshotOfTierModel().value(QStringLiteral("longContextInputPriceMicros")).toULongLong() == 0);
        assert(snapshotOfTierModel().value(QStringLiteral("cacheWritePriceMicros")).toULongLong() == 2'500'000);

        // 8. The Overview words the four Backend price states (QML only chooses the phrase).
        const auto wording = [&](const char* state, const char* amount) {
            QVariant result;
            assert(QMetaObject::invokeMethod(overviewPage, "costWording",
                Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, QString::fromLatin1(state)),
                Q_ARG(QVariant, QString::fromLatin1(amount))));
            return result.toString();
        };
        assert(wording("known", "$0.0007") == QStringLiteral("$0.0007"));
        assert(wording("partial", "$0.0007") == QStringLiteral("At least $0.0007"));
        assert(wording("partial", "$0.0000") == QStringLiteral("At least $0.0000"));
        assert(wording("unknown", "") == QStringLiteral("Cost unavailable"));
        assert(!wording("unknown", "$0.0000").contains(QLatin1Char('$')));
        assert(wording("none", "") == QString::fromUtf8("\xE2\x80\x94"));
        checkpoint("M2c pricing dialog, long-context tier and price-state wording verified in real QML");
    }

    {
        // PC-1D: the Chat panel's LOCAL Project Context preview, driven through the real QML and the real
        // Backend. Nothing is sent to a model; the page only asks backend.projectContext for a snapshot.
        QObject* chatsPage = waitForChild(window, "chatsPage");
        QObject* preview = waitForChild(window, "chatProjectContextPreview");
        QObject* previewButton = waitForChild(window, "chatProjectContextPreviewButton");
        QObject* memorySwitch = waitForChild(window, "chatProjectContextMemorySwitch");
        assert(chatsPage && preview && previewButton && memorySwitch);
        assert(!preview->property("hasProject").toBool()
               && !previewButton->property("enabled").toBool()
               && !memorySwitch->property("checked").toBool()
               && "Without a project the preview is inert and Project Memory is off");

        const fs::path previewRoot = workspace.root / "chat-preview-project";
        fs::create_directories(previewRoot / "src");
        std::ofstream(previewRoot / "src" / "main.qml") << "Item {}\n";
        const QString previewProjectId = backend.projectController().createProject(
            QStringLiteral("Chat preview project"), QStringLiteral("Preview intent"),
            QString::fromStdWString(previewRoot.wstring()));
        assert(!previewProjectId.isEmpty());

        // Selecting the conversation's project enables the action; nothing is built until the user presses it.
        assert(chatsPage->setProperty("selectedProjectId", previewProjectId));
        assert(waitForProperty(preview, "hasProject", [](const QVariant& v) { return v.toBool(); }));
        assert(waitForProperty(previewButton, "enabled", [](const QVariant& v) { return v.toBool(); }));
        assert(!backend.projectContextController().hasSnapshot() && !backend.projectContextController().busy());

        assert(QMetaObject::invokeMethod(previewButton, "clicked"));
        assert(waitForProperty(preview, "showSnapshot", [](const QVariant& v) { return v.toBool(); }, 20s));
        QObject* summary = waitForChild(window, "chatProjectContextSummary");
        assert(summary);
        const QString summaryText = summary->property("text").toString();
        assert(summaryText.startsWith(QStringLiteral("Fingerprint "))
               && summaryText.contains(QStringLiteral("of 24000 estimated tokens")));
        assert(!backend.projectContextController().includesProjectMemory());
        assert(!summaryText.contains(QStringLiteral("main.qml"))
               && "The preview must not show workspace file names");

        // The FULL fingerprint is in a read-only field that can be selected and copied; the Copy button selects it.
        // The session panel exists twice (the wide column and the narrow-layout popup), so every copy is checked.
        const QString fullFingerprint = backend.projectContextController().fingerprint();
        const QList<QObject*> fingerprintFields = window->findChildren<QObject*>(QStringLiteral("chatProjectContextFingerprint"));
        const QList<QObject*> copyButtons = window->findChildren<QObject*>(QStringLiteral("chatProjectContextCopyFingerprint"));
        const QList<QObject*> previewBlocks = window->findChildren<QObject*>(QStringLiteral("chatProjectContextPreview"));
        assert(!fingerprintFields.isEmpty() && fingerprintFields.size() == copyButtons.size()
               && fingerprintFields.size() == previewBlocks.size());
        assert(fullFingerprint.size() > 12);
        for (QObject* field : fingerprintFields) {
            assert(field->property("text").toString() == fullFingerprint
                   && field->property("readOnly").toBool()
                   && field->property("selectByMouse").toBool()
                   && field->property("selectedText").toString().isEmpty()
                   && "The preview shows the whole fingerprint in a selectable read-only field");
        }
        for (QObject* button : copyButtons)
            assert(QMetaObject::invokeMethod(button, "clicked"));
        for (QObject* field : fingerprintFields) {
            assert(field->property("selectedText").toString() == fullFingerprint
                   && "Copy selects the whole fingerprint");
        }
        for (QObject* block : previewBlocks)
            assert(block->property("copied").toBool() && "Copy says it copied");

        // The Project Memory toggle owns its own width: the label that follows it in the row must not be drawn
        // over the switch (the empty contentItem once left the Switch narrower than its indicator).
        {
            QObject* switchIndicator = memorySwitch->property("indicator").value<QObject*>();
            assert(switchIndicator
                   && memorySwitch->property("width").toDouble() >= switchIndicator->property("width").toDouble()
                   && "The Project Memory switch is at least as wide as its indicator");
        }

        // Project Memory is opt-in: switching it on and pressing Preview again adds the derived section.
        assert(memorySwitch->setProperty("checked", true));
        assert(QMetaObject::invokeMethod(previewButton, "clicked"));
        assert(waitForProperty(preview, "showSnapshot", [](const QVariant& v) { return v.toBool(); }, 20s));
        assert(backend.projectContextController().includesProjectMemory());
        bool memorySectionShown = false;
        for (const QVariant& row : backend.projectContextController().sections()) {
            if (row.toMap().value(QStringLiteral("category")).toString() == QStringLiteral("project_memory")
                && row.toMap().value(QStringLiteral("authority")).toString() == QStringLiteral("derived")) {
                memorySectionShown = true;
            }
        }
        assert(memorySectionShown && "A requested Project Memory is always shown, and as derived");

        // A failure is shown with its code, and never next to an old snapshot.
        assert(chatsPage->setProperty("selectedProjectId", QStringLiteral("no-such-project")));
        assert(waitForProperty(preview, "showSnapshot", [](const QVariant& v) { return !v.toBool(); }));
        assert(QMetaObject::invokeMethod(previewButton, "clicked"));
        assert(waitForProperty(preview, "showError", [](const QVariant& v) { return v.toBool(); }, 20s));
        QObject* errorText = waitForChild(window, "chatProjectContextError");
        assert(errorText
               && errorText->property("text").toString().contains(QStringLiteral("project_context.project_not_found")));

        // Leaving the project drops everything: the next project must never see this result.
        assert(chatsPage->setProperty("selectedProjectId", QString()));
        assert(waitForProperty(preview, "hasProject", [](const QVariant& v) { return !v.toBool(); }));
        assert(waitForProperty(preview, "showError", [](const QVariant& v) { return !v.toBool(); }));
        assert(backend.projectContextController().projectId().isEmpty()
               && !backend.projectContextController().hasSnapshot());
        checkpoint("PC-1D Chat project context preview verified in real QML");
    }

    {
        // PC-3D: the Prompt Workshop in the real QML over the real Backend. A model reply that holds one
        // next-run plan gets an explicit action; the preview shows what will be saved; confirming saves Run
        // Drafts (never a Run); the Drafts list says where a draft came from; and New Run opens an empty-roster
        // draft as a Manifest roster - never as a Mock Run - with its contracts, refusing to start it until the
        // models are chosen. Nothing here calls a provider.
        const std::string planJson = R"json({
          "schemaVersion": "next-run-plan.v1", "title": "History page",
          "sharedContracts": [{"id": "S1", "kind": "interface_contract", "title": "Loss entry",
                               "text": "property string metricId\nproperty real share", "appliesToPaths": ["qml/models"]}],
          "drafts": [
            {"key": "model", "title": "Model task", "task": "Create the model.", "sharedContractIds": ["S1"]},
            {"key": "view", "title": "View task", "task": "Create the view.", "dependsOn": ["model"],
             "sharedContractIds": ["S1"],
             "contracts": [{"id": "V1", "kind": "constraint", "title": "Size", "text": "No file above 300 lines.",
                            "appliesToPaths": []}]}]
        })json";
        const std::string fence(3, '`');
        const auto envelopeOf = [&](const std::string& json) {
            return "Here is the plan.\n\n" + fence + "next-run-plan.v1\n" + json + "\n" + fence + "\n\nTell me what to change.";
        };

        window->setProperty("currentSection", QStringLiteral("chats"));
        QObject* chatsPage = waitForChild(window, "chatsPage");
        assert(chatsPage);

        ProviderCatalogController& workshopCatalog = backend.providerCatalogController();
        const QString workshopProviderId = workshopCatalog.createProvider(
            QStringLiteral("Workshop provider"), QStringLiteral("local"), QStringLiteral("http://127.0.0.1:11434/v1"));
        assert(!workshopProviderId.isEmpty());
        const QString workshopModelId = workshopCatalog.createModel(
            workshopProviderId, QStringLiteral("workshop-model"), QStringLiteral("Workshop model"), 32768, 4096, 0, 0);
        assert(!workshopModelId.isEmpty());
        backend.runController().refreshAgentChoices();

        const fs::path workshopRoot = workspace.root / "workshop-project";
        fs::create_directories(workshopRoot / "src");
        const QString workshopProjectId = backend.projectController().createProject(
            QStringLiteral("Workshop project"), QStringLiteral("Workshop intent"),
            QString::fromStdWString(workshopRoot.wstring()));
        assert(!workshopProjectId.isEmpty());
        const QString conversationId = backend.chatController().createConversation(workshopProviderId, workshopModelId);
        assert(!conversationId.isEmpty() && backend.chatController().setProject(conversationId, workshopProjectId));

        // Chat messages are seeded through a second connection: no provider can write a model reply here.
        const auto workshopDatabase = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        assert(workshopDatabase.success());
        ai::infrastructure::SqliteChatRepository storedChats(workshopDatabase.value());
        int seeded = 0;
        const auto seed = [&](const std::string& id, ai::application::ChatRole role, const std::string& content) {
            ai::application::ChatMessage message;
            message.id = id;
            message.conversationId = conversationId.toStdString();
            message.role = role;
            message.status = ai::application::ChatMessageStatus::Sent;
            message.content = content;
            message.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'800'000'000'000 + ++seeded));
            assert(storedChats.insertMessage(message).success());
        };
        seed("pw-plain", ai::application::ChatRole::Assistant, "An ordinary reply without a plan.");
        seed("pw-user", ai::application::ChatRole::User, envelopeOf(planJson));
        seed("pw-double", ai::application::ChatRole::Assistant, envelopeOf(planJson) + "\n" + envelopeOf(planJson));
        seed("pw-plan", ai::application::ChatRole::Assistant, envelopeOf(planJson));

        assert(QMetaObject::invokeMethod(chatsPage, "refreshConversations"));
        assert(QMetaObject::invokeMethod(chatsPage, "selectConversation", Q_ARG(QVariant, QVariant(conversationId))));

        // PC-3E: previewing still sends nothing. The separate attach action first shows the Backend quote; closing
        // that dialog is the only action that arms the exact fingerprint for the NEXT message. This smoke test
        // deliberately does not press Send, so even the local provider is never called.
        assert(QMetaObject::invokeMethod(chatsPage, "requestProjectContextPreview", Q_ARG(QVariant, false)));
        assert(ai::ui::test_support::waitForCondition("the Workshop context preview completes", [&] {
            return backend.projectContextController().hasSnapshot()
                && backend.projectContextController().projectId() == workshopProjectId;
        }, 20s));
        // The attach action sits in the preview block itself (next to the numbers it quotes), not only behind the
        // composer's "+": pressing it opens the same quote dialog.
        QObject* attachFromPreview = waitForChild(window, "chatProjectContextAttachButton");
        assert(attachFromPreview && "The preview block has its own Attach button");
        assert(QMetaObject::invokeMethod(attachFromPreview, "clicked"));
        QObject* contextDialog = waitForChild(window, "chatProjectContextAttachmentDialog");
        QObject* contextCost = waitForChild(window, "chatProjectContextCostQuote");
        QObject* contextConfirm = waitForChild(window, "chatConfirmProjectContextAttachment");
        assert(contextDialog && contextCost && contextConfirm && contextDialog->property("visible").toBool());
        assert(!chatsPage->property("pendingContextQuote").isValid()
               || chatsPage->property("pendingContextQuote").isNull());
        assert(chatsPage->property("contextQuoteCandidate").toMap().value(QStringLiteral("estimatedInputTokens"))
                   .toULongLong() > 0);
        assert(contextCost->property("text").toString().contains(QStringLiteral("price is unavailable")));
        assert(QMetaObject::invokeMethod(contextConfirm, "clicked"));
        const QVariantMap armedContext = chatsPage->property("pendingContextQuote").toMap();
        assert(!contextDialog->property("visible").toBool()
               && armedContext.value(QStringLiteral("fingerprint")).toString()
                   == backend.projectContextController().fingerprint());
        assert(storedChats.listMessages(conversationId.toStdString()).value().size() == 4
               && "Quoting and confirming attachment must not create a message or call a provider");
        // Once armed, the preview block says so, for exactly the snapshot that was armed.
        QObject* attachedPreview = waitForChild(window, "chatProjectContextPreview");
        QObject* attachedNote = waitForChild(window, "chatProjectContextAttachedNote");
        assert(attachedPreview && attachedNote && attachedPreview->property("attached").toBool()
               && "The preview block shows that the snapshot is attached to the next message");
        assert(QMetaObject::invokeMethod(chatsPage, "clearPendingContextAttachment"));
        assert(!attachedPreview->property("attached").toBool()
               && "Removing the attachment clears the note");
        assert(chatsPage->property("pendingContextQuote").isNull());

        // The message list is a ListView: it builds only the rows near the viewport, so each row is brought into
        // view and looked at through the view itself (itemAtIndex), and its controls are found in the row's own
        // visual tree.
        QQuickItem* messageView = nullptr;
        assert(ai::ui::test_support::waitForCondition("the message list shows four messages", [&] {
            for (QQuickItem* item : qobject_cast<QQuickItem*>(chatsPage)->findChildren<QQuickItem*>()) {
                if (QString::fromLatin1(item->metaObject()->className()).contains(QStringLiteral("ListView"))
                        && item->property("count").toInt() == 4) {
                    messageView = item;
                    return true;
                }
            }
            return false;
        }, [] { return std::string("no ListView with the four seeded messages"); }, 10s));
        const auto messageRow = [&](int index) {
            QQuickItem* row = nullptr;
            assert(ai::ui::test_support::waitForCondition("a message row is built", [&] {
                assert(QMetaObject::invokeMethod(messageView, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 0)));
                row = nullptr;
                return QMetaObject::invokeMethod(messageView, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, index))
                       && row != nullptr && row->height() > 0;
            }, [index] { return "row " + std::to_string(index); }, 10s));
            return row;
        };
        // Items created by a Repeater or a ListView are not QObject children of the window, so they are found
        // in the VISUAL tree: every item of that name below `root`.
        const std::function<void(QQuickItem*, const QString&, QList<QQuickItem*>&)> collectItems =
            [&collectItems](QQuickItem* item, const QString& name, QList<QQuickItem*>& out) {
                if (item->objectName() == name) out.push_back(item);
                for (QQuickItem* child : item->childItems()) collectItems(child, name, out);
            };
        const auto itemsBelow = [&](QQuickItem* root, const char* name) {
            QList<QQuickItem*> found;
            if (root != nullptr) collectItems(root, QString::fromLatin1(name), found);
            return found;
        };
        // The content of a Dialog (a Popup is not an item itself).
        const auto contentOf = [](QObject* dialogObject) {
            return qobject_cast<QQuickItem*>(dialogObject->property("contentItem").value<QObject*>());
        };
        const auto rowControl = [&](int index, const char* name) -> QQuickItem* {
            const QList<QQuickItem*> found = itemsBelow(messageRow(index), name);
            return !found.isEmpty() && found.constFirst()->isVisible() ? found.constFirst() : nullptr;
        };
        // 0 plain reply, 1 the user's own message holding a block, 2 a reply with two blocks, 3 a reply with one.
        assert(rowControl(0, "chatMessagePlanActionButton") == nullptr && rowControl(0, "chatMessagePlanHint") == nullptr);
        assert(rowControl(1, "chatMessagePlanActionButton") == nullptr && rowControl(1, "chatMessagePlanHint") == nullptr
               && "The user's own message never offers the action");
        assert(rowControl(2, "chatMessagePlanActionButton") == nullptr && rowControl(2, "chatMessagePlanHint") != nullptr
               && "A reply with two plan blocks says why it has no action");
        QQuickItem* planAction = rowControl(3, "chatMessagePlanActionButton");
        assert(planAction != nullptr && rowControl(3, "chatMessagePlanHint") == nullptr
               && "The completed reply with exactly one plan offers the action");
        const auto visibleCount = [&](QQuickItem* root, const char* objectName) {
            int count = 0;
            for (QQuickItem* item : itemsBelow(root, objectName)) {
                if (item->isVisible()) ++count;
            }
            return count;
        };

        // ---- the preview of a reply ----------------------------------------------------------------------------------------
        const int runRowsBefore = backend.runController().model()->rowCount();
        assert(QMetaObject::invokeMethod(planAction, "clicked"));
        QObject* planDialog = waitForChild(window, "nextRunPlanDialog");
        assert(planDialog);
        assert(waitForProperty(planDialog, "hasPreview", [](const QVariant& v) { return v.toBool(); }));
        assert(waitForProperty(planDialog, "visible", [](const QVariant& v) { return v.toBool(); }));
        assert(planDialog->property("mode").toString() == QStringLiteral("message"));
        assert(itemsBelow(contentOf(planDialog), "nextRunPlanDraftCard").size() == 2
               && "One card per draft of the plan");
        const auto texts = [&](const char* objectName) {
            QStringList values;
            for (QQuickItem* item : itemsBelow(contentOf(planDialog), objectName)) {
                values.push_back(item->property("text").toString());
            }
            return values;
        };
        assert(texts("nextRunPlanTaskText").contains(QStringLiteral("Create the model."))
               && texts("nextRunPlanTaskText").contains(QStringLiteral("Create the view."))
               && "Every task is shown in full");
        assert(texts("nextRunPlanContractText").size() == 3
               && texts("nextRunPlanContractText").contains(QStringLiteral("property string metricId\nproperty real share"))
               && texts("nextRunPlanContractText").contains(QStringLiteral("No file above 300 lines."))
               && "Every merged contract is shown word for word, the shared one on both drafts");
        assert(waitForChild(window, "nextRunPlanSummary")->property("text").toString().contains(QStringLiteral("History page")));
        QObject* templateCombo = waitForChild(window, "nextRunPlanTemplateCombo");
        assert(templateCombo && templateCombo->property("count").toInt() == 1
               && "With no saved roster draft the only choice is the empty roster");
        assert(!waitForChild(window, "nextRunPlanSourceRunsSection")->property("visible").toBool()
               && "A project with no Run offers no source Runs");
        assert(backend.runDraftController().drafts()->rowCount() == 0 && "A preview saves nothing");

        // ---- the reply changes after the user looked: the old preview is refused ---------------------------------
        {
            auto storedPlan = storedChats.findMessage("pw-plan").value().value();
            const std::string::size_type at = storedPlan.content.find("Create the model.");
            assert(at != std::string::npos);
            storedPlan.content.replace(at, 17, "Create the model, version two.");
            assert(storedChats.updateMessage(storedPlan).success());
        }
        QObject* confirmButton = waitForChild(window, "nextRunPlanConfirmButton");
        assert(confirmButton && QMetaObject::invokeMethod(confirmButton, "clicked"));
        assert(waitForProperty(planDialog, "errorText",
            [](const QVariant& v) { return v.toString().contains(QStringLiteral("run_draft_plan.stale_preview")); })
            && "A reply changed since its preview is refused as stale");
        assert(backend.runDraftController().drafts()->rowCount() == 0 && "...and nothing was saved");
        QObject* previewAgain = waitForChild(window, "nextRunPlanPreviewAgainButton");
        assert(previewAgain && previewAgain->property("visible").toBool());
        assert(QMetaObject::invokeMethod(previewAgain, "clicked"));
        assert(waitForProperty(planDialog, "errorText", [](const QVariant& v) { return v.toString().isEmpty(); }));
        assert(texts("nextRunPlanTaskText").contains(QStringLiteral("Create the model, version two."))
               && "The new preview shows the new text");

        // ---- confirm: Run Drafts, never a Run ---------------------------------------------------------------------------------
        assert(QMetaObject::invokeMethod(confirmButton, "clicked"));
        assert(waitForProperty(planDialog, "created", [](const QVariant& v) { return v.toBool(); }));
        assert(waitForChild(window, "nextRunPlanResultSummary")->property("text").toString().contains(QStringLiteral("Saved 2 draft(s)")));
        QAbstractItemModel* draftsModel = backend.runDraftController().drafts();
        assert(draftsModel->rowCount() == 2 && "The Drafts list refreshed on its own");
        assert(backend.runController().model()->rowCount() == runRowsBefore && "Confirming a plan creates no Run");
        const auto draftRole = [&](int row, const char* roleName) {
            return draftsModel->data(draftsModel->index(row, 0), draftsModel->roleNames().key(QByteArray(roleName)));
        };
        QString viewDraftId;
        for (int row = 0; row < draftsModel->rowCount(); ++row) {
            if (draftRole(row, "title").toString() == QStringLiteral("View task")) viewDraftId = draftRole(row, "draftId").toString();
        }
        assert(!viewDraftId.isEmpty());

        // ---- Open Drafts: the Runs page, with where each draft came from ----------------------------------------------------
        QObject* openDrafts = waitForChild(window, "nextRunPlanOpenDraftsButton");
        assert(openDrafts && QMetaObject::invokeMethod(openDrafts, "clicked"));
        assert(waitForProperty(window, "currentSection", [](const QVariant& v) { return v.toString() == QStringLiteral("runs"); }));
        QObject* draftsLoader = waitForChild(window, "draftsDialogLoader");
        assert(draftsLoader);
        QObject* draftsDialogObject = nullptr;
        assert(ai::ui::test_support::waitForCondition("the Drafts dialog lists the plan drafts", [&] {
            draftsDialogObject = draftsLoader->property("item").value<QObject*>();
            return draftsDialogObject != nullptr && itemsBelow(contentOf(draftsDialogObject), "draftGroupBadge").size() == 2;
        }, [] { return std::string("the two plan badges of the Drafts dialog"); }, 10s));
        QQuickItem* draftsContent = contentOf(draftsDialogObject);
        QStringList groupBadges;
        for (QQuickItem* badge : itemsBelow(draftsContent, "draftGroupBadge")) {
            groupBadges.push_back(badge->property("label").toString());
        }
        assert(groupBadges.size() == 2);
        bool firstOfTwo = false;
        bool secondOfTwo = false;
        for (const QString& label : groupBadges) {
            assert(label.contains(QStringLiteral("History page")) && label.contains(QStringLiteral("2 drafts remaining")));
            firstOfTwo = firstOfTwo || label.contains(QStringLiteral("Draft #1"));
            secondOfTwo = secondOfTwo || label.contains(QStringLiteral("Draft #2"));
        }
        assert(firstOfTwo && secondOfTwo);
        assert(visibleCount(draftsContent, "draftOriginBadge") == 2 && visibleCount(draftsContent, "draftDependencyBadge") == 1
               && visibleCount(draftsContent, "draftEditedBadge") == 0
               && "Only the dependent draft shows a dependency; none is edited");
        for (QQuickItem* badge : itemsBelow(draftsContent, "draftDependencyBadge")) {
            if (badge->isVisible()) {
                assert(badge->property("label").toString().contains(QStringLiteral("Suggested after: Model task")));
            }
        }
        // The card keeps its actions on a row of their own, under the text and the settings line, inside the card.
        const QList<QQuickItem*> draftCards = itemsBelow(draftsContent, "draftCard");
        assert(draftCards.size() == 2);
        assert(ai::ui::test_support::waitForCondition("the draft cards are laid out", [&] {
            for (QQuickItem* card : draftCards) {
                const QList<QQuickItem*> openButtons = itemsBelow(card, "draftOpenButton");
                if (card->height() <= 0 || openButtons.size() != 1 || openButtons.constFirst()->height() <= 0) return false;
            }
            return true;
        }, [] { return std::string("laid-out draft cards"); }, 10s));
        for (QQuickItem* card : draftCards) {
            const QList<QQuickItem*> actionButtons = itemsBelow(card, "draftDetailsButton") + itemsBelow(card, "draftOpenButton")
                + itemsBelow(card, "draftDeleteButton");
            const QList<QQuickItem*> settingsLine = itemsBelow(card, "draftSettingsSummary");
            assert(actionButtons.size() == 3 && settingsLine.size() == 1 && card->height() > 0);
            const double textBottom = settingsLine.constFirst()->mapToItem(card, QPointF(0, settingsLine.constFirst()->height())).y();
            for (QQuickItem* button : actionButtons) {
                const QPointF corner = button->mapToItem(card, QPointF(0, 0));
                assert(corner.y() >= textBottom - 0.5 && "an action is never drawn over the text above it");
                assert(corner.x() >= 0 && corner.y() + button->height() <= card->height() + 0.5
                       && corner.x() + button->width() <= card->width() + 0.5 && "the actions stay inside their card");
            }
            assert(!card->property("expanded").toBool() && !itemsBelow(card, "draftDetails").constFirst()->isVisible()
                   && "details are closed until asked for");
        }

        // "Show details" reads the draft completely: the whole task, every setting, every contract word for word.
        {
            QQuickItem* viewCard = nullptr;
            for (QQuickItem* card : draftCards) {
                if (itemsBelow(card, "draftName").constFirst()->property("text").toString() == QStringLiteral("View task")) viewCard = card;
            }
            assert(viewCard);
            const QVariantMap viewDraft = backend.runDraftController().openDraft(viewDraftId);
            assert(QMetaObject::invokeMethod(itemsBelow(viewCard, "draftDetailsButton").constFirst(), "clicked"));
            assert(viewCard->property("expanded").toBool() && itemsBelow(viewCard, "draftDetails").constFirst()->isVisible());
            assert(itemsBelow(viewCard, "draftTaskText").constFirst()->property("text").toString()
                   == viewDraft.value(QStringLiteral("task")).toString());
            const bool repairOn = viewDraft.value(QStringLiteral("automaticVerificationRepair")).toBool();
            assert(itemsBelow(viewCard, "draftRepairRow").constFirst()->property("value").toString().startsWith(repairOn ? "On" : "Off"));
            assert(itemsBelow(viewCard, "draftLimitsRow").constFirst()->property("value").toString().contains(QStringLiteral("Not set")));
            assert(itemsBelow(viewCard, "draftRosterRow").constFirst()->property("value").toString().contains(QStringLiteral("Empty")));
            const QVariantList contractsOfDraft = viewDraft.value(QStringLiteral("interfaceContracts")).toList();
            const QList<QQuickItem*> contractTexts = itemsBelow(viewCard, "draftContractText");
            assert(contractsOfDraft.size() == 2 && contractTexts.size() == 2 && itemsBelow(viewCard, "draftContract").size() == 2);
            for (int i = 0; i < contractTexts.size(); ++i) {
                assert(contractTexts.at(i)->property("text").toString()
                           == contractsOfDraft.at(i).toMap().value(QStringLiteral("text")).toString()
                       && "a contract is shown word for word");
            }
            assert(QMetaObject::invokeMethod(itemsBelow(viewCard, "draftDetailsButton").constFirst(), "clicked"));
            assert(!viewCard->property("expanded").toBool() && !itemsBelow(viewCard, "draftDetails").constFirst()->isVisible());
        }
        assert(QMetaObject::invokeMethod(draftsDialogObject, "close"));

        // ---- New Run opens an empty-roster plan draft as a Manifest roster, not as a Mock Run --------------------
        assert(QMetaObject::invokeMethod(runsPage, "openDraftInDialog", Q_ARG(QVariant, QVariant(viewDraftId))));
        QObject* draftDialog = waitForChild(window, "newRunDialog");
        assert(draftDialog);
        assert(waitForProperty(draftDialog, "visible", [](const QVariant& v) { return v.toBool(); }));
        assert(waitForProperty(draftDialog, "manifestRoster", [](const QVariant& v) { return v.toBool(); })
               && "The draft opens as a Manifest roster, not as Mock");
        assert(waitForChild(window, "useExecutionProfileCheck")->property("checked").toBool()
               && waitForChild(window, "customRosterCheck")->property("checked").toBool()
               && waitForChild(window, "manifestRosterCheck")->property("checked").toBool());
        assert(waitForChild(window, "interfaceContractsSection")->property("visible").toBool()
               && waitForChild(window, "interfaceContractsSection")->property("count").toInt() == 2
               && "Its contracts stay visible: the shared one and its own");
        assert(waitForChild(window, "rosterModel")->property("count").toInt() == 2 && "A lead and one worker slot");
        assert(waitForProperty(draftDialog, "rosterModelsMissing", [](const QVariant& v) { return v.toBool(); }));
        assert(waitForChild(window, "newRunRosterSetupHint")->property("visible").toBool());
        const QString provenance = draftDialog->property("planProvenanceText").toString();
        assert(provenance.contains(QStringLiteral("History page")) && provenance.contains(QStringLiteral("draft 2"))
               && provenance.contains(QStringLiteral("Suggested to start after: Model task")));
        assert(waitForChild(window, "newRunPlanProvenance")->property("visible").toBool());
        assert(waitForChild(window, "updateDraftButton")->property("visible").toBool());

        // The Run cannot start until the models are chosen.
        assert(QMetaObject::invokeMethod(draftDialog, "submit"));
        assert(draftDialog->property("errorText").toString().contains(QStringLiteral("Choose a model"))
               && backend.runController().model()->rowCount() == runRowsBefore);
        QObject* rosterModel = waitForChild(window, "rosterModel");
        assert(waitForProperty(draftDialog, "agentChoices", [](const QVariant& v) { return !v.toList().isEmpty(); })
               && "The provider/model choice reached the dialog");
        for (int slot = 0; slot < 2; ++slot) {
            assert(QMetaObject::invokeMethod(rosterModel, "setProperty", Q_ARG(int, slot),
                Q_ARG(QString, QStringLiteral("modelChoiceIndex")), Q_ARG(QVariant, QVariant(0))));
        }
        assert(waitForProperty(draftDialog, "rosterModelsMissing", [](const QVariant& v) { return !v.toBool(); })
               && "Choosing the models lifts the block");
        assert(QMetaObject::invokeMethod(draftDialog, "reject"));
        assert(waitForProperty(draftDialog, "visible", [](const QVariant& v) { return !v.toBool(); }));

        // ---- a deleted draft: the survivor keeps its number and says what became of its dependency ---------------------------
        {
            QString modelDraftId;
            for (int row = 0; row < draftsModel->rowCount(); ++row) {
                if (draftRole(row, "title").toString() == QStringLiteral("Model task")) modelDraftId = draftRole(row, "draftId").toString();
            }
            assert(!modelDraftId.isEmpty() && backend.runDraftController().removeDraft(modelDraftId));
            assert(QMetaObject::invokeMethod(runsPage, "openDrafts"));
            QObject* reopenedLoader = waitForChild(window, "draftsDialogLoader");
            QObject* reopened = nullptr;
            assert(ai::ui::test_support::waitForCondition("the Drafts dialog lists the surviving plan draft", [&] {
                reopened = reopenedLoader->property("item").value<QObject*>();
                return reopened != nullptr && itemsBelow(contentOf(reopened), "draftGroupBadge").size() == 1;
            }, [] { return std::string("one plan badge after the deletion"); }, 10s));
            const QString survivorLabel = itemsBelow(contentOf(reopened), "draftGroupBadge").constFirst()->property("label").toString();
            assert(survivorLabel.contains(QStringLiteral("Draft #2")) && survivorLabel.contains(QStringLiteral("1 draft remaining"))
                   && !survivorLabel.contains(QStringLiteral("2 of 1")) && "The survivor keeps its own number");
            const QList<QQuickItem*> removedDependency = itemsBelow(contentOf(reopened), "draftDependencyBadge");
            assert(removedDependency.size() == 1
                   && removedDependency.constFirst()->property("label").toString().contains(QStringLiteral("model (removed draft)"))
                   && "A dependency on a deleted draft says so");
            assert(QMetaObject::invokeMethod(reopened, "close"));
        }

        // ---- Import next-run plan: a paste, a roster template and a source Run ---------------------------------------------------
        // A saved roster draft of this project, to copy the roster from.
        QVariantList templateRoster;
        for (int slot = 0; slot < 2; ++slot) {
            templateRoster.push_back(QVariantMap {
                {QStringLiteral("stageType"), slot == 0 ? QStringLiteral("architect") : QStringLiteral("worker-a")},
                {QStringLiteral("providerId"), workshopProviderId}, {QStringLiteral("modelId"), workshopModelId},
                {QStringLiteral("agentName"), slot == 0 ? QStringLiteral("Lead") : QStringLiteral("Worker")},
                {QStringLiteral("role"), slot == 0 ? QStringLiteral("lead") : QStringLiteral("worker")},
                {QStringLiteral("instructions"), QStringLiteral("Do the work.")},
                {QStringLiteral("leadArchitect"), slot == 0}, {QStringLiteral("workspaceAccess"), slot != 0},
                {QStringLiteral("position"), slot}, {QStringLiteral("timeoutMs"), 90000},
                {QStringLiteral("dependencies"), slot == 0 ? QVariantList {} : QVariantList {QStringLiteral("architect")}},
                {QStringLiteral("maxAttempts"), 1}});
        }
        const QString templateDraftId = backend.runDraftController().createDraft(QStringLiteral("Workshop roster"),
            workshopProjectId, QStringLiteral("Workshop roster"), QStringLiteral("Roster only"), QStringLiteral("custom_roster"),
            QString {}, templateRoster, 2, QString {}, true, QStringLiteral("execution-manifest-v1"));
        assert(!templateDraftId.isEmpty());
        const QString sourceRunId = backend.runController().startRun(
            workshopProjectId, QStringLiteral("Mock"), QStringLiteral("Mock task"));
        assert(!sourceRunId.isEmpty());
        assert(waitForStatus(backend.runController().model(), sourceRunId, {QStringLiteral("completed")}, 20s)
               == QStringLiteral("completed"));
        const int runRowsWithSource = backend.runController().model()->rowCount();

        window->setProperty("currentSection", QStringLiteral("chats"));
        QObject* importButton = waitForChild(window, "chatImportPlanButton");
        assert(importButton && importButton->property("enabled").toBool());
        assert(QMetaObject::invokeMethod(importButton, "clicked"));
        QObject* importDialog = waitForChild(window, "nextRunPlanDialog");
        assert(waitForProperty(importDialog, "visible", [](const QVariant& v) { return v.toBool(); }));
        assert(waitForProperty(importDialog, "mode", [](const QVariant& v) { return v.toString() == QStringLiteral("import"); }));
        QObject* pasteField = waitForChild(window, "nextRunPlanPasteField");
        QObject* previewButton = waitForChild(window, "nextRunPlanPreviewButton");
        assert(pasteField && previewButton);

        assert(pasteField->setProperty("text", QStringLiteral("hello")));
        assert(waitForProperty(previewButton, "enabled", [](const QVariant& v) { return v.toBool(); }));
        assert(QMetaObject::invokeMethod(previewButton, "clicked"));
        assert(waitForProperty(importDialog, "errorText",
            [](const QVariant& v) { return v.toString().contains(QStringLiteral("next_run_plan.envelope_missing")); })
            && "A paste that is not a plan is refused with the Backend's own code");
        assert(!importDialog->property("hasPreview").toBool());

        assert(pasteField->setProperty("text", QString::fromStdString(planJson)));
        assert(QMetaObject::invokeMethod(previewButton, "clicked"));
        assert(waitForProperty(importDialog, "hasPreview", [](const QVariant& v) { return v.toBool(); }));
        assert(importDialog->property("errorText").toString().isEmpty());
        QObject* importTemplates = waitForChild(window, "nextRunPlanTemplateCombo");
        assert(importTemplates->property("count").toInt() >= 2 && "The saved roster draft is offered as a template");

        // A picker that cannot be loaded is a visible failure with a way to retry - never "nothing to choose".
        assert(chatsPage->setProperty("selectedConversationId", QStringLiteral("no-such-conversation")));
        assert(QMetaObject::invokeMethod(chatsPage, "loadPlanChoices"));
        assert(waitForProperty(importDialog, "choiceErrors", [](const QVariant& v) { return v.toList().size() == 2; }));
        for (const QVariant& problem : importDialog->property("choiceErrors").toList()) {
            assert(problem.toString().contains(QStringLiteral("run_draft_plan.conversation_not_found")));
        }
        assert(itemsBelow(contentOf(importDialog), "nextRunPlanChoiceError").size() == 2
               && importTemplates->property("count").toInt() == 1);
        assert(chatsPage->setProperty("selectedConversationId", conversationId));
        const QList<QQuickItem*> reloadButtons = itemsBelow(contentOf(importDialog), "nextRunPlanReloadChoicesButton");
        assert(reloadButtons.size() == 1 && QMetaObject::invokeMethod(reloadButtons.constFirst(), "clicked"));
        assert(waitForProperty(importDialog, "choiceErrors", [](const QVariant& v) { return v.toList().isEmpty(); }));
        assert(importTemplates->property("count").toInt() >= 2 && "Reloading brings the choices back");
        int workshopTemplateIndex = -1;
        for (int index = 0; index < importTemplates->property("count").toInt(); ++index) {
            QString label;
            assert(QMetaObject::invokeMethod(importTemplates, "textAt", Q_RETURN_ARG(QString, label), Q_ARG(int, index)));
            if (label.startsWith(QStringLiteral("Workshop roster"))) workshopTemplateIndex = index;
        }
        assert(workshopTemplateIndex > 0);
        assert(importTemplates->setProperty("currentIndex", workshopTemplateIndex));
        assert(waitForChild(window, "nextRunPlanSourceRunsSection")->property("visible").toBool()
               && "The project's Run is offered as a source");
        const QList<QQuickItem*> sourceRunChecks = itemsBelow(contentOf(importDialog), "nextRunPlanSourceRunCheck");
        assert(sourceRunChecks.size() == 1 && QMetaObject::invokeMethod(sourceRunChecks.constFirst(), "click"));
        assert(waitForProperty(importDialog, "selectedRunIds", [&](const QVariant& v) { return v.toStringList() == QStringList({sourceRunId}); }));

        assert(QMetaObject::invokeMethod(waitForChild(window, "nextRunPlanConfirmButton"), "clicked"));
        assert(waitForProperty(importDialog, "created", [](const QVariant& v) { return v.toBool(); }));
        assert(backend.runController().model()->rowCount() == runRowsWithSource && "The import created no Run");
        draftsModel = backend.runDraftController().drafts();
        int importedWithRoster = 0;
        for (int row = 0; row < draftsModel->rowCount(); ++row) {
            if (draftRole(row, "groupOrigin").toString() == QStringLiteral("manual_import")) {
                assert(draftRole(row, "roster").toList().size() == 2 && draftRole(row, "sourceRunCount").toInt() == 1);
                ++importedWithRoster;
            }
        }
        assert(importedWithRoster == 2 && "The imported drafts carry the template's roster and the source Run");
        assert(QMetaObject::invokeMethod(waitForChild(window, "nextRunPlanCloseButton"), "clicked"));
        checkpoint("PC-3D Prompt Workshop verified in real QML");
    }

    assert(qmlWarnings.isEmpty() && "QML runtime warnings were emitted");

    checkpoint("all assertions passed");
    return 0;
}
