// PC-5D: real-QML coverage of the follow-up planner. It loads the real Main.qml with a real BackendBridge over a disposable SQLite
// database and drives the real FollowUpPlanPanel, its quote dialog and the plan review dialog through their own functions, exactly the
// ones the buttons call:
//   1. a Failed Run shows `Plan recovery...` (not `Plan next Run...`), enabled by the Backend's gate;
//   2. the quote dialog lists the model the Backend offers, keeps `Get quote` disabled until a goal is typed, and a quote shows what is
//      sent, to whom and the worst-case cost - NOTHING is sent and no Run starts (this test never presses Authorize: the paid path is
//      proven by the controller tests over a scripted provider, and no provider is configured here);
//   3. the quote is re-opened from the status line and discarded for free;
//   4. a stored plan (a paid answer already in the request) is reviewed in the PC-3D dialog with its source Run locked, confirmed once,
//      and the status line says the Drafts were saved - nothing else happened.
// Any QML warning fails the test.
#include "FollowUpRequestTestSupport.hpp"

#include <ai/application/execution/IWorkflowRunner.hpp>
#include <ai/application/execution/ManifestWorkLedgerTypes.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/FollowUpPlanController.hpp>
#include <ai/ui/RunController.hpp>
#include "../test_support/QtTestSupport.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace app = ai::application;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("ai-orchestrator-followup-qml-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";
    TempWorkspace() { fs::create_directories(projectPath); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }
};

QObject* waitForChild(QObject* root, const char* name, std::chrono::milliseconds timeout = 10s)
{
    QObject* found = nullptr;
    ai::ui::test_support::waitForCondition("QML child", [&] {
        found = root->findChild<QObject*>(QString::fromLatin1(name));
        return found != nullptr;
    }, [name] { return std::string("objectName=") + name; }, timeout);
    return found;
}

bool waitFor(const char* label, const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 15s)
{
    return ai::ui::test_support::waitForCondition(label, predicate, timeout);
}

void call(QObject* object, const char* method)
{
    require(QMetaObject::invokeMethod(object, method), std::string("invoke ") + method);
}
void call(QObject* object, const char* method, const QVariant& argument)
{
    require(QMetaObject::invokeMethod(object, method, Q_ARG(QVariant, argument)), std::string("invoke ") + method);
}

// A Repeater's delegate items are not QObject children of any ancestor: walk the VISUAL tree to reach them.
void collectNamed(QQuickItem* item, const QString& name, QList<QQuickItem*>& found)
{
    if (item == nullptr) return;
    if (item->objectName() == name) found.push_back(item);
    for (QQuickItem* child : item->childItems()) collectNamed(child, name, found);
}

// Optional visual review: with AIO_SHOT_DIR set, the window is saved as a PNG at the named step (never part of a normal run).
void shot(QObject* window, const char* name, bool scrollDetailsDown = false)
{
    const QByteArray directory = qgetenv("AIO_SHOT_DIR");
    if (directory.isEmpty()) return;
    if (scrollDetailsDown) {
        if (QObject* flick = window->findChild<QObject*>(QStringLiteral("runDetailsFlickable"))) {
            // Bring the follow-up block to the top of Run Details.
            auto* panelItem = qobject_cast<QQuickItem*>(window->findChild<QObject*>(QStringLiteral("followUpPlanPanel")));
            auto* contentItem = qobject_cast<QQuickItem*>(flick->property("contentItem").value<QObject*>());
            if (panelItem != nullptr && contentItem != nullptr) {
                flick->setProperty("contentY", qMax(0.0, panelItem->mapToItem(contentItem, QPointF(0, 0)).y() - 24.0));
            }
        }
    }
    for (int cycle = 0; cycle < 40; ++cycle) ai::ui::test_support::pumpOneEventCycle();
    if (auto* quick = qobject_cast<QQuickWindow*>(window)) {
        quick->grabWindow().save(QString::fromLocal8Bit(directory) + QLatin1Char('/') + QString::fromLatin1(name) + QStringLiteral(".png"));
    }
}

bool flag(QObject* object, const char* name) { return object->property(name).toBool(); }
QString text(QObject* object, const char* name) { return object->property(name).toString(); }

// A terminal Manifest Run with its Work Ledger entry; the Outcome Brief is written by the application's own history pass.
void seedRun(const std::shared_ptr<ai::infrastructure::SqliteDatabase>& database, const std::string& runId, const std::string& projectId, const std::string& title,
    app::RunStatus status = app::RunStatus::Failed)
{
    ai::infrastructure::SqliteRunRepository runs(database);
    ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database);
    app::Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = title;
    run.task = "A finished Manifest Run";
    run.workflowId = app::kExecutionManifestWorkflowId;
    run.status = status;
    run.createdAt = std::chrono::system_clock::now();
    require(runs.insertRun(run).success(), "seed: the Run");
    app::ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = projectId;
    entry.terminalStatus = status;
    entry.disposition = status == app::RunStatus::Completed ? app::ManifestWorkLedgerDisposition::NoChangesNeeded
                                                            : app::ManifestWorkLedgerDisposition::NotProduced;
    entry.baselineFingerprint = std::string(64, 'b');
    entry.terminalReason = "The Run ended";
    require(ledger.insertEntry(entry).success(), "seed: the ledger entry");
}

} // namespace

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setDefaultAlphaBuffer(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIOrchestrator"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("ai-orchestrator.local"));
    QCoreApplication::setApplicationName(QStringLiteral("AIOrchestratorFollowUpQmlTests"));
    QGuiApplication application(argc, argv);

    try {
        TempWorkspace workspace;
        using namespace followup_test;
        const std::string runA = "run-recovery";   // quoted and discarded in the dialog
        const std::string runB = "run-ready";      // already holds a paid, stored plan
        const std::string runC = "run-done";       // a Completed Run: offered the next-Run plan, not recovery
        std::string projectId;
        {
            ai::backend::BackendRuntime first({workspace.databasePath});
            require(first.storageAvailable(), "storage is available");
            const auto created = first.projectService().createProject({"Follow-up QML", "d", workspace.projectPath});
            require(created.success(), "the project is created");
            projectId = created.value().id;
        }
        const Catalog catalog;
        {
            const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
            require(opened.success(), "a raw connection opens");
            auto database = opened.value();
            seedRun(database, runA, projectId, "Recovery run");
            seedRun(database, runB, projectId, "Ready run");
            seedRun(database, runC, projectId, "Done run", app::RunStatus::Completed);
            // Push runB outside the ordinary newest-50 picker window. The follow-up review must still include that immutable source
            // Run as its one selected, disabled row.
            for (int index = 0; index < 50; ++index) {
                seedRun(database, "newer-run-" + std::to_string(100 + index), projectId,
                    "Newer Run " + std::to_string(index), app::RunStatus::Completed);
            }
            ai::infrastructure::SqliteProviderRepository providers(database);
            ai::infrastructure::SqliteModelRepository models(database);
            ai::infrastructure::SqliteFollowUpPlanRepository requests(database);
            require(providers.insert(providerOf(catalog)).success(), "seed: the provider");
            require(models.insert(modelOf(catalog)).success(), "seed: the model");
            // Run B already holds a plan_ready request: authorised, claimed and answered (the paid call happened in an earlier session).
            const auto now = followUpFloorToMilliseconds(std::chrono::system_clock::now());
            auto request = quotedRequest(catalog, "req-ready", projectId, runB, now);
            require(requests.insertQuoted(request).success(), "seed: a quote");
            require(requests.transition("req-ready", authorizeStep(request), now + 1ms).success(), "seed: authorised");
            require(requests.transition("req-ready", plainStep(FollowUpPlanEvent::Claim, FollowUpPlanRequestState::Authorized), now + 2ms).success(), "seed: claimed");
            // The stored plan carries a staffing hint on its first draft (6a (a)): the review shows it and Confirm can copy its parallelism.
            std::string stored = planJsonOf('c', 2);
            const std::string firstTask = "\"task\":\"Task for k0 (c)\"";
            const auto taskAt = stored.find(firstTask);
            require(taskAt != std::string::npos, "seed: the fixture plan has the first task");
            stored.insert(taskAt + firstTask.size(),
                ",\"staffing\":{\"workers\":[{\"role\":\"implementation\",\"focus\":\"the first part\"},"
                "{\"role\":\"documentation\",\"focus\":\"the notes\"}],\"parallelism\":2,\"reason\":\"Two independent parts.\"}");
            const auto storedPlan = app::parseNextRunPlan(stored);
            require(storedPlan.success() && storedPlan.value().drafts[0].staffing.has_value(), "seed: the staffed plan parses");
            require(requests.transition("req-ready",
                        planStep(app::FollowUpPlanAnswer {replyOfPlanJson(stored), storedPlan.value().fingerprint, "[]"}), now + 3ms).success(),
                "seed: the paid answer is stored");
        }

        auto bridge = std::make_unique<BackendBridge>(workspace.databasePathQt());
        require(bridge->storageAvailable(), "the bridge opens the storage");
        QQmlApplicationEngine engine;
        QStringList qmlWarnings;
        QVariantMap initialProperties;
        initialProperties.insert(QStringLiteral("backend"), QVariant::fromValue(bridge.get()));
        engine.setInitialProperties(initialProperties);
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine, [&qmlWarnings](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) {
                qmlWarnings.push_back(warning.toString());
                std::fprintf(stderr, "[qml-warning] %s\n", warning.toString().toLocal8Bit().constData());
            }
            std::fflush(stderr);
        });
        engine.loadFromModule("UI_AI_Orchestrator", "Main");
        require(waitFor("the QML engine root object", [&] { return !engine.rootObjects().isEmpty(); }, 10s), "Main.qml loaded");
        QObject* window = engine.rootObjects().constFirst();

        // The Runs page must be the visible one: the panel's `visible` is the EFFECTIVE visibility.
        require(window->setProperty("currentSection", QStringLiteral("runs")), "navigate to Runs");
        bridge->runController().refresh(QString::fromStdString(projectId));
        constexpr int kSeededRunCount = 53;
        require(waitFor("the Runs list loads every seeded Run", [&] {
            bridge->runController().refresh(QString::fromStdString(projectId));
            return bridge->runController().model()->rowCount() == kSeededRunCount;
        }), "the Runs list holds every seeded Run: " + std::to_string(bridge->runController().model()->rowCount()));
        QObject* panel = waitForChild(window, "followUpPlanPanel");
        require(panel != nullptr, "the follow-up panel is in the loaded tree");
        FollowUpPlanController& controller = bridge->followUpPlanController();
        QObject* runDetails = waitForChild(window, "runDetailsPanel");
        require(runDetails != nullptr, "Run Details is in the loaded tree");

        const auto selectAndEvaluate = [&](const std::string& runId) {
            // The Runs page is calendar-first: showing a Run's details means leaving the calendar for that Run's day (as a click does).
            QObject* runsPage = waitForChild(window, "runsPage");
            require(runsPage != nullptr, "the Runs page exists");
            call(runsPage, "focusRunToday", QString::fromStdString(runId));
            require(waitFor("the panel evaluates the selected Run", [&] {
                bridge->outcomeBriefController().refresh();   // the history pass writes the brief on a worker
                controller.refresh();
                return text(panel, "availability") == QLatin1String("evaluated") && panel->property("status").toMap().value(QStringLiteral("runId")).toString()
                    == QString::fromStdString(runId);
            }), "the follow-up block evaluates " + runId);
            // ...and Run Details shows that Run (its panel is a live binding to the Runs page's selection).
            require(waitFor("Run Details shows the selected Run", [&] {
                return runDetails->property("runData").toMap().value(QStringLiteral("runId")).toString() == QString::fromStdString(runId) && flag(panel, "visible");
            }), "Run Details and the follow-up block are visible for " + runId + " (runData=" + runDetails->property("runData").toMap().value(QStringLiteral("runId")).toString().toStdString()
                + ", modelRows=" + std::to_string(bridge->runController().model()->rowCount())
                + ", snapshot=" + std::to_string(waitForChild(window, "runsPage")->property("runsSnapshot").toList().size())
                + ", selectedRunId=" + bridge->runController().selectedRunId().toStdString()
                + ", pageSelected=" + waitForChild(window, "runsPage")->property("selectedRunId").toString().toStdString() + ")");
        };

        // ---- 1. a Failed Run is offered recovery planning, with the Backend's gate ----------------------------------------------------
        selectAndEvaluate(runA);
        QObject* planRecovery = waitForChild(window, "planRecoveryButton");
        QObject* planNextRun = waitForChild(window, "planNextRunButton");
        require(planRecovery && planNextRun, "both command buttons exist in the tree");
        require(flag(planRecovery, "visible") && flag(planRecovery, "enabled"),
            std::string("`Plan recovery...` is shown and enabled for a Failed Run: visible=") + (flag(planRecovery, "visible") ? "1" : "0")
                + " enabled=" + (flag(planRecovery, "enabled") ? "1" : "0") + " panelVisible=" + (flag(panel, "visible") ? "1" : "0")
                + " section=" + text(window, "currentSection").toStdString()
                + " status=" + QString::fromStdString("").toStdString() + controller.status().value(QStringLiteral("availability")).toString().toStdString()
                + " recovery.allowed=" + (controller.status().value(QStringLiteral("recovery")).toMap().value(QStringLiteral("allowed")).toBool() ? "1" : "0")
                + " busy=" + (controller.busy() ? "1" : "0"));
        require(!flag(planNextRun, "visible"), "`Plan next Run...` is not offered for a Failed Run");
        QObject* explanationBefore = waitForChild(window, "followUpPlanExplanation");
        require(explanationBefore && !flag(explanationBefore, "visible"), "no request yet: no status text");

        shot(window, "01_panel_failed_run", true);
        // A Completed Run is offered the next-Run plan instead (the same gate, the other kind).
        selectAndEvaluate(runC);
        require(flag(planNextRun, "visible") && flag(planNextRun, "enabled"), "`Plan next Run...` is shown and enabled for a Completed Run");
        require(!flag(planRecovery, "visible"), "`Plan recovery...` is not offered for a Completed Run");
        selectAndEvaluate(runA);
        require(flag(planRecovery, "visible") && !flag(planNextRun, "visible"), "back on the Failed Run only recovery is offered (nothing of the previous Run leaks across)");

        // ---- 2. the quote dialog -----------------------------------------------------------------------------------------------------
        call(panel, "openRequestDialog", QStringLiteral("recovery"));
        QObject* dialog = waitForChild(window, "followUpPlanDialog");
        require(dialog && waitFor("the dialog is open", [&] { return flag(dialog, "visible"); }), "the quote dialog opened");
        require(waitFor("the candidate files are read", [&] { return !flag(dialog, "filesLoading"); }), "the files finished loading");
        require(text(dialog, "filesError").isEmpty(), "no file error: " + text(dialog, "filesError").toStdString());
        require(dialog->property("models").toList().size() == 1 && dialog->property("modelOptions").toList().size() == 1,
            "the model the Backend offers is the picker's one entry");
        QObject* getQuote = waitForChild(dialog, "followUpGetQuoteButton");
        QObject* goalField = waitForChild(dialog, "followUpGoalField");
        QObject* notice = waitForChild(dialog, "followUpPlanNotice");
        require(getQuote && goalField && notice, "the form controls exist");
        require(text(notice, "text").contains(QStringLiteral("Nothing is sent until you press Authorize")), "the standing promise is on the dialog");
        require(!flag(getQuote, "enabled"), "`Get quote` stays disabled until a goal is typed");
        goalField->setProperty("text", QStringLiteral("Recover the failed work in one focused step."));
        require(waitFor("Get quote enables", [&] { return flag(getQuote, "enabled"); }), "...and enables once there is a goal");

        shot(window, "02_request_form");
        call(dialog, "requestQuote");
        require(waitFor("the quote arrives", [&] { return flag(dialog, "hasQuote"); }), "the quote stage is shown: " + text(dialog, "errorText").toStdString());
        QObject* warning = waitForChild(dialog, "followUpSendWarning");
        QObject* recipient = waitForChild(dialog, "followUpQuoteRecipient");
        QObject* authorize = waitForChild(dialog, "followUpAuthorizeButton");
        require(warning && recipient && authorize, "the quote stage controls exist");
        require(text(warning, "text").contains(QStringLiteral("CONTENT")) && text(warning, "text").contains(QStringLiteral("Nothing starts")),
            "the warning says that content is sent and that nothing starts");
        require(text(warning, "text").contains(QString::fromStdString(catalog.providerKind)), "...and to which provider");
        require(text(recipient, "text").contains(QStringLiteral("Valid until")), "the quote's deadline is shown");
        require(text(authorize, "text").startsWith(QStringLiteral("Authorize and send (up to $")), "the paid action states its worst-case cost: " + text(authorize, "text").toStdString());
        require(controller.status().value(QStringLiteral("request")).toMap().value(QStringLiteral("state")).toString() == QLatin1String("quoted"),
            "the Backend holds ONE quoted request");

        shot(window, "03_quote_stage");
        // The quote is NOT authorised: nothing is sent. Close the dialog; the status line keeps the quote.
        call(dialog, "close");
        require(waitFor("the dialog closed", [&] { return !flag(dialog, "visible"); }), "the dialog closed");
        QObject* stateLine = waitForChild(window, "followUpPlanState");
        require(waitFor("the status line shows the quote", [&] { return text(stateLine, "text").contains(QStringLiteral("Quote ready")); }), "the status line shows the quote");
        QObject* openQuote = waitForChild(window, "followUpOpenQuoteButton");
        QObject* explanation = waitForChild(window, "followUpPlanExplanation");
        require(flag(openQuote, "visible") && text(explanation, "text").contains(QStringLiteral("Nothing has been sent and nothing is charged")),
            "...honestly: nothing sent, nothing charged");
        require(!flag(planRecovery, "enabled") && !flag(waitForChild(window, "followUpCancelButton"), "visible"),
            "the open quote holds the Run's slot (no second plan) and has nothing to cancel yet");

        shot(window, "04_status_quoted", true);
        // ---- 3. the quote is re-opened from the status line and discarded for free -------------------------------------------------
        call(panel, "openStoredQuote");
        require(waitFor("the stored quote opens", [&] { return flag(dialog, "visible") && flag(dialog, "hasQuote"); }), "the stored quote re-opens at the quote stage");
        call(dialog, "changeRequest");
        require(waitFor("the stored quote becomes an editable request", [&] {
            return !flag(dialog, "hasQuote") && !flag(dialog, "filesLoading") && flag(getQuote, "enabled");
        }), "Change the request restores a usable form: " + text(dialog, "filesError").toStdString());
        require(text(goalField, "text") == QLatin1String("Recover the failed work in one focused step."),
            "the stored goal is restored instead of an empty form");
        require(dialog->property("modelOptions").toList().size() == 1 && dialog->property("chosenModel").isValid(),
            "the stored recipient is restored from the current model choices");
        call(dialog, "close");
        require(waitFor("the dialog closed again", [&] { return !flag(dialog, "visible"); }), "closed again");
        call(panel, "discardRequest");
        require(waitFor("the quote is discarded", [&] { return !controller.status().value(QStringLiteral("requestPresent")).toBool(); }),
            "a discarded quote leaves no status line: " + text(panel, "actionError").toStdString());
        require(waitFor("the slot is free", [&] { return flag(planRecovery, "enabled"); }), "...and the Run can be planned again");

        // ---- 4. a stored paid plan is reviewed in the PC-3D dialog and confirmed once -----------------------------------------------
        selectAndEvaluate(runB);
        require(waitFor("the status line shows a plan ready", [&] { return text(stateLine, "text").contains(QStringLiteral("Plan ready")); }), "the status line shows `Plan ready`");
        QObject* review = waitForChild(window, "followUpReviewPlanButton");
        require(flag(review, "visible") && flag(review, "enabled"), "`Review plan` is offered");
        require(text(explanation, "text").contains(QStringLiteral("No Drafts exist until you review the plan and confirm it")), "the status line says that no Drafts exist yet");
        require(text(explanation, "text").contains(QStringLiteral("Paid:")), "...and what the paid answer cost");

        call(panel, "openReview");
        QObject* reviewDialog = waitForChild(window, "followUpReviewDialog");
        require(reviewDialog && waitFor("the review dialog opens", [&] { return flag(reviewDialog, "visible") && flag(reviewDialog, "hasPreview"); }),
            "the review dialog shows the stored plan: " + text(reviewDialog, "errorText").toStdString());
        require(text(reviewDialog, "title") == QLatin1String("Review the follow-up plan") && text(reviewDialog, "lockedRunId") == QString::fromStdString(runB),
            "it is the follow-up review of THIS Run's request");
        require(reviewDialog->property("planDrafts").toList().size() == 2, "both drafts of the stored plan are listed");
        QList<QQuickItem*> checks;
        require(waitFor("the source Run choices are listed", [&] {
            checks.clear();
            collectNamed(qobject_cast<QQuickItem*>(reviewDialog->property("contentItem").value<QObject*>()), QStringLiteral("nextRunPlanSourceRunCheck"), checks);
            return checks.size() == static_cast<qsizetype>(app::FollowUpPlanReadModel::kMaxSourceRunChoices);
        }), "the picker lists the project's Runs");
        int locked = 0;
        for (QQuickItem* check : checks) {
            if (flag(check, "checked") && !flag(check, "enabled")) ++locked;
        }
        require(checks.size() == static_cast<qsizetype>(app::FollowUpPlanReadModel::kMaxSourceRunChoices) && locked == 1,
            "the old source Run remains in the bounded picker, shown ticked and immutable: "
                + std::to_string(locked) + " locked of " + std::to_string(checks.size()));

        // 6a (a): the staffing hint is shown on its draft only, with the Backend's wording, and the tick that copies its parallelism is offered.
        require(flag(reviewDialog, "hasStaffing") && reviewDialog->property("staffingMaxWorkers").toInt() == 2,
            "the dialog knows the plan carries a staffing hint of up to two workers");
        QList<QQuickItem*> staffingBlocks;
        collectNamed(qobject_cast<QQuickItem*>(reviewDialog->property("contentItem").value<QObject*>()), QStringLiteral("nextRunPlanStaffing"), staffingBlocks);
        int visibleStaffing = 0;
        for (QQuickItem* block : staffingBlocks) {
            if (block->isVisible()) ++visibleStaffing;
        }
        require(staffingBlocks.size() == 2 && visibleStaffing == 1, "only the draft that carries a hint shows it: "
            + std::to_string(visibleStaffing) + " of " + std::to_string(staffingBlocks.size()));
        // The Prompt Workshop's dialog in Chats has the same named items: look inside THIS dialog only.
        const auto inReview = [&](const char* name) -> QQuickItem* {
            QList<QQuickItem*> found;
            collectNamed(qobject_cast<QQuickItem*>(reviewDialog->property("contentItem").value<QObject*>()), QString::fromLatin1(name), found);
            return found.size() == 1 ? found.front() : nullptr;
        };
        QQuickItem* applyParallelism = inReview("nextRunPlanApplyParallelismCheck");
        require(applyParallelism && flag(applyParallelism, "visible") && flag(applyParallelism, "enabled") && !flag(applyParallelism, "checked"),
            "the follow-up review offers the unticked parallelism tick");
        QQuickItem* comparison = inReview("nextRunPlanStaffingComparison");
        require(comparison && flag(comparison, "visible") && text(comparison, "text").contains(QStringLiteral("up to 2")),
            "the plan's advice is set next to the roster choice");
        applyParallelism->setProperty("checked", true);

        shot(window, "05_review_plan");
        call(reviewDialog, "confirm");
        require(waitFor("the plan is confirmed", [&] { return flag(reviewDialog, "created"); }), "the explicit Confirm saved the group: " + text(reviewDialog, "errorText").toStdString());
        require(waitFor("the status line follows", [&] { return text(stateLine, "text").contains(QStringLiteral("Drafts saved")); }), "the status line says `Drafts saved`");
        require(flag(waitForChild(window, "followUpOpenDraftsButton"), "visible"), "`Open Drafts` is offered");
        require(controller.status().value(QStringLiteral("request")).toMap().value(QStringLiteral("draftGroupId")).toString().size() > 0, "the request names its saved group");
        {
            // The tick copied the suggested parallelism into the Draft that carries the hint, and into no other.
            const QVariantList savedDrafts = reviewDialog->property("creation").toMap().value(QStringLiteral("drafts")).toList();
            require(savedDrafts.size() == 2, "two Drafts were saved");
            const QVariantMap firstDraft = bridge->runDraftController().openDraft(savedDrafts.at(0).toMap().value(QStringLiteral("draftId")).toString());
            const QVariantMap secondDraft = bridge->runDraftController().openDraft(savedDrafts.at(1).toMap().value(QStringLiteral("draftId")).toString());
            require(firstDraft.value(QStringLiteral("maxParallelism")).toInt() == 2 && secondDraft.value(QStringLiteral("maxParallelism")).toInt() == 4,
                "the ticked suggestion reached the first Draft (" + std::to_string(firstDraft.value(QStringLiteral("maxParallelism")).toInt())
                    + ") and left the second at the default (" + std::to_string(secondDraft.value(QStringLiteral("maxParallelism")).toInt()) + ")");
        }
        shot(window, "06_review_confirmed");
        call(reviewDialog, "close");
        shot(window, "07_status_drafts_saved", true);

        require(qmlWarnings.isEmpty(), "the real QML raised no warning: " + (qmlWarnings.isEmpty() ? std::string() : qmlWarnings.front().toStdString()));
        std::cout << "AIOrchestrator_FollowUpQml_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
