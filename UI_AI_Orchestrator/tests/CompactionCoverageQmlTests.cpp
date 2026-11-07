#include "CompactionCoverageQmlTests.hpp"

#include "../test_support/ProjectMemoryOutcomeFixtures.hpp"
#include "../test_support/QtTestSupport.hpp"

#include <ProjectCostTestSupport.hpp>

#include <ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/CompactionIntentRecoveryController.hpp>
#include <ai/ui/ProjectCostController.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QMetaObject>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QtMath>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace app = ai::application;
namespace fs = std::filesystem;
namespace memory_outcome = ai::ui::test_support::memory_outcome;

// ---- checks (active under NDEBUG) and the visual-tree helpers ----------------------------------------------------------------------
void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "[compaction-coverage-qml] FAILED: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

void settle(int cycles = 30)
{
    for (int cycle = 0; cycle < cycles; ++cycle) ai::ui::test_support::pumpOneEventCycle();
}

QRectF sceneRectOf(QObject* object)
{
    auto* item = qobject_cast<QQuickItem*>(object);
    check(item != nullptr, "a visual item");
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

QObject* visualChildNamed(QObject* root, const QString& name)
{
    auto* item = qobject_cast<QQuickItem*>(root);
    if (item == nullptr) return nullptr;
    if (item->objectName() == name) return item;
    for (QQuickItem* child : item->childItems()) {
        if (QObject* found = visualChildNamed(child, name)) return found;
    }
    return nullptr;
}

void collectNamed(QObject* root, const QString& name, QList<QObject*>& out)
{
    auto* item = qobject_cast<QQuickItem*>(root);
    if (item == nullptr) return;
    if (item->objectName() == name) out.append(item);
    for (QQuickItem* child : item->childItems()) collectNamed(child, name, out);
}

bool flagOf(QObject* object, const char* name) { return object->property(name).toBool(); }

// An item that exists and is visible; a missing item is a failure, never "not visible".
bool visibleNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
    if (found == nullptr) std::fprintf(stderr, "[compaction-coverage-qml] missing item '%s'\n", name);
    check(found != nullptr, "the named item exists");
    return flagOf(found, "visible");
}

QString textNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
    if (found == nullptr) std::fprintf(stderr, "[compaction-coverage-qml] missing item '%s'\n", name);
    check(found != nullptr, "the named text exists");
    return found->property("text").toString();
}

int visibleCount(QObject* root, const char* name)
{
    QList<QObject*> found;
    collectNamed(root, QString::fromLatin1(name), found);
    int count = 0;
    for (QObject* item : found) count += flagOf(item, "visible") ? 1 : 0;
    return count;
}

bool overlaps(QObject* first, QObject* second)
{
    return sceneRectOf(first).adjusted(0.5, 0.5, -0.5, -0.5).intersects(sceneRectOf(second).adjusted(0.5, 0.5, -0.5, -0.5));
}

bool inside(QObject* inner, const QRectF& outer)
{
    const QRectF rect = sceneRectOf(inner);
    return rect.left() >= outer.left() - 0.5 && rect.right() <= outer.right() + 0.5 && rect.top() >= outer.top() - 0.5
        && rect.bottom() <= outer.bottom() + 0.5;
}

void checkTextFits(QObject* text, const char* what)
{
    const double width = text->property("width").toDouble();
    const double height = text->property("height").toDouble();
    const bool fits = !flagOf(text, "truncated") && text->property("contentWidth").toDouble() <= width + 0.5
        && text->property("contentHeight").toDouble() <= height + 0.5;
    if (!fits) {
        std::fprintf(stderr, "[layout] %s does not fit: width=%.1f contentWidth=%.1f height=%.1f contentHeight=%.1f truncated=%d text='%s'\n", what,
            width, text->property("contentWidth").toDouble(), height, text->property("contentHeight").toDouble(), flagOf(text, "truncated") ? 1 : 0,
            text->property("text").toString().toUtf8().constData());
    }
    check(fits, what);
}

// Every VISIBLE item of the given names: whole (not cut), inside `outer`, and clear of every other one of them (they are all in one stack).
void checkStack(QObject* root, const QRectF& outer, const std::vector<const char*>& names, const char* what)
{
    std::vector<QObject*> shown;
    for (const char* name : names) {
        QList<QObject*> found;
        collectNamed(root, QString::fromLatin1(name), found);
        for (QObject* item : found) {
            if (!flagOf(item, "visible")) continue;
            checkTextFits(item, what);
            if (!inside(item, outer)) {
                const QRectF r = sceneRectOf(item);
                std::fprintf(stderr, "[layout] %s: '%s' is outside: item=(%.1f,%.1f,%.1f,%.1f) outer=(%.1f,%.1f,%.1f,%.1f)\n", what, name, r.left(), r.top(),
                    r.right(), r.bottom(), outer.left(), outer.top(), outer.right(), outer.bottom());
            }
            check(inside(item, outer), what);
            for (QObject* other : shown) check(!overlaps(item, other), what);
            shown.push_back(item);
        }
    }
    check(!shown.empty(), "the stack shows something");
}

void saveShot(QObject* root, const QString& name)
{
    const QByteArray directory = qgetenv("AIO_SHOT_DIR");
    auto* item = qobject_cast<QQuickItem*>(root);
    if (directory.isEmpty() || item == nullptr) return;
    settle(40);
    const QSharedPointer<QQuickItemGrabResult> result = item->grabToImage(QSize(qCeil(item->width()), qCeil(item->height())));
    check(!result.isNull(), "the harness item can be grabbed");
    QEventLoop loop;
    QObject::connect(result.data(), &QQuickItemGrabResult::ready, &loop, &QEventLoop::quit);
    loop.exec();
    check(result->saveToFile(QString::fromLocal8Bit(directory) + QLatin1Char('/') + name + QStringLiteral(".png")), "the shot is saved");
}

// A real tap (press + release at the centre) delivered to the window: the TapHandler of a toggle must react to it.
void tap(QQuickWindow& window, QObject* item)
{
    const QPointF point = sceneRectOf(item).center();
    QMouseEvent press(QEvent::MouseButtonPress, point, window.mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, window.mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &release);
    settle();
}

void invokeNoArgs(QObject* object, const char* method) { check(QMetaObject::invokeMethod(object, method), method); }

// ---- the harness: a QML Item created from source in a shown window ----------------------------------------------------------------------
struct Host {
    QQuickWindow window;
    std::unique_ptr<QObject> owner;
    QQuickItem* root {nullptr};
};

std::unique_ptr<Host> makeHost(QQmlApplicationEngine& engine, const char* source, const QUrl& url, const QVariantMap& initial, const QSize& size)
{
    QQmlComponent component(&engine);
    component.setData(source, url);
    if (component.isError()) {
        for (const auto& error : component.errors()) std::fprintf(stderr, "[compaction-coverage-qml] %s\n", error.toString().toUtf8().constData());
    }
    check(component.isReady(), "the harness compiles");
    auto host = std::make_unique<Host>();
    host->window.resize(size);
    host->owner.reset(component.createWithInitialProperties(initial));
    check(host->owner != nullptr, "the harness is created");
    host->root = qobject_cast<QQuickItem*>(host->owner.get());
    check(host->root != nullptr, "the harness is an item");
    host->root->setParentItem(host->window.contentItem());
    host->window.show();
    check(ai::ui::test_support::waitForCondition("the harness is exposed", [&] { return host->window.isExposed(); }, 5s), "exposed");
    settle();
    return host;
}

const char* const kCardHarness = R"QML(
import QtQuick
import QtQuick.Layouts
import UI_AI_Orchestrator
Item {
    id: root
    property var hostRef
    property var colorsRef
    property var costRef
    property real cardWidth: 420
    width: 760
    height: 1400
    ProjectCostCard {
        id: cardItem
        objectName: "card"
        x: 10; y: 10
        width: root.cardWidth
        height: implicitHeight
        hostWindow: root.hostRef; colors: root.colorsRef
        cost: root.costRef
    }
    Rectangle { objectName: "nextBlock"; x: 10; y: cardItem.y + cardItem.height + 8; width: root.cardWidth; height: 30; color: "#334455" }
}
)QML";

const char* const kPanelHarness = R"QML(
import QtQuick
import QtQuick.Layouts
import UI_AI_Orchestrator
Item {
    id: root
    property var hostRef
    property var colorsRef
    property var backendRef
    property string projectIdValue: "cp1"
    property real panelWidth: 560
    width: 800
    height: 4200
    Column {
        x: 10; y: 10
        width: root.panelWidth
        spacing: 12
        ProjectMemoryPanel {
            id: panelItem
            objectName: "projectMemoryPanel"
            width: root.panelWidth
            height: implicitHeight
            hostWindow: root.hostRef; colors: root.colorsRef
            backend: root.backendRef
            projectId: root.projectIdValue
        }
        Rectangle { objectName: "nextBlock"; width: root.panelWidth; height: 40; color: "#334455" }
    }
}
)QML";

// ---- the cost-card side: the REAL controller over scripted sources ------------------------------------------------------------------------
struct CostRig {
    project_cost_test::Fixture fixture;
    app::InMemoryModelRepository names;
    app::ProviderCatalogService catalogService {fixture.providers, names};
    ProjectCostController controller {fixture.service, catalogService};

    CostRig()
    {
        app::Model model;
        model.id = "model";
        model.providerId = "prov";
        model.apiModelId = "model";
        model.displayName = "model";
        model.contextWindowTokens = 100'000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 2'000'000;
        model.outputPriceMicrosPerMillionTokens = 10'000'000;
        check(fixture.catalog.insert(model).success(), "seed the catalog");
    }
    void ledger(std::uint64_t cost)
    {
        check(fixture.ledger.insertExecution(project_cost_test::ledgerRow("l1", "p1", "prov", "model", 10, 10, cost, true)).success(), "seed the ledger");
    }
    void refresh()
    {
        controller.refreshProject(QStringLiteral("p1"));
        settle();
    }
};

app::ProjectMemoryCompactionIntent intentRow(const std::string& id, const std::string& project, app::CompactionIntentState state, std::int64_t created)
{
    app::ProjectMemoryCompactionIntent value;
    value.id = id;
    value.projectId = project;
    value.providerId = "prov";
    value.modelId = "model-" + id;
    value.sourceSegmentIds = {"s1", "s2", "s3"};
    value.sourceContentHash = std::string(64, 'a');
    value.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(created));
    value.state = state;
    if (state == app::CompactionIntentState::Unresolved) value.unresolvedMarkedAt = value.createdAt + std::chrono::milliseconds(10);
    return value;
}

const std::vector<const char*> kCardStack = {"projectCostTitle", "projectCostValue", "projectCostStatus", "projectCostBreakdown", "projectCostNoPriceRecords",
    "projectCostErrorCode", "compactionCoverageTitle", "compactionCoverageScope", "compactionCoverageCounts", "compactionCoverageNone",
    "compactionCoverageWarning", "compactionCoverageUnavailable", "compactionCoverageErrorMessage", "compactionCoverageErrorCode", "projectCostCoverage",
    "projectCostDetailsToggle", "projectCostDetailsCoverage", "projectCostDetailsTokens", "projectCostDetailsPricing"};

void checkCardLayout(QObject* root, const char* what)
{
    QObject* card = visualChildNamed(root, QStringLiteral("card"));
    QObject* next = visualChildNamed(root, QStringLiteral("nextBlock"));
    check(card != nullptr && next != nullptr, "the card and the next block exist");
    const QRectF cardRect = sceneRectOf(card);
    check(cardRect.height() >= card->property("implicitHeight").toDouble() - 0.5, what);
    checkStack(card, cardRect, kCardStack, what);
    check(!overlaps(card, next) && sceneRectOf(next).top() >= cardRect.bottom() - 0.5, what);
}

void runCardScenarios(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    CostRig rig;
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    initial.insert(QStringLiteral("costRef"), QVariant::fromValue<QObject*>(&rig.controller));
    auto host = makeHost(engine, kCardHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/CardHarness.qml")), initial, QSize(780, 1420));
    QObject* root = host->root;
    QObject* card = visualChildNamed(root, QStringLiteral("card"));

    // Idle: nothing measured, nothing drawn of the coverage.
    check(!visibleNamed(card, "compactionCoverageBlock"), "idle: no coverage block");

    // A known amount, no unfinished protocols: a real zero is worded as such - never as "everything is recorded".
    rig.ledger(900);
    rig.refresh();
    const QString amount = textNamed(card, "projectCostValue");
    const QString status = textNamed(card, "projectCostStatus");
    check(amount == QStringLiteral("$0.0009") && visibleNamed(card, "compactionCoverageBlock"), "the amount and the coverage block");
    check(visibleNamed(card, "compactionCoverageNone")
            && textNamed(card, "compactionCoverageNone") == QStringLiteral("No open or unresolved compaction operations in this scope")
            && textNamed(card, "compactionCoverageScope") == QStringLiteral("All time") && !visibleNamed(card, "compactionCoverageWarning")
            && !visibleNamed(card, "compactionCoverageCounts") && !visibleNamed(card, "compactionCoverageUnavailable"),
        "a real zero: the exact sentence, the scope, no warning");
    check(!textNamed(card, "compactionCoverageNone").contains(QStringLiteral("recorded")) && !textNamed(card, "compactionCoverageNone").contains(QStringLiteral("known")),
        "...and nothing like 'all recorded' or 'costs known'");
    checkCardLayout(root, "zero coverage");

    // Unfinished protocols: the counts and one warning next to the SAME amount and status.
    rig.fixture.intents.listRows = {intentRow("a", "p1", app::CompactionIntentState::Open, 1000), intentRow("b", "p1", app::CompactionIntentState::Unresolved, 2000),
        intentRow("c", "p1", app::CompactionIntentState::Unresolved, 3000)};
    rig.refresh();
    check(textNamed(card, "projectCostValue") == amount && textNamed(card, "projectCostStatus") == status, "the amount and its status did not change");
    check(textNamed(card, "compactionCoverageCounts") == QStringLiteral("Open protocols: 1 · Unresolved compaction operations: 2")
            && visibleNamed(card, "compactionCoverageWarning")
            && textNamed(card, "compactionCoverageWarning")
                == QStringLiteral("These operations have no confirmed final recording; delivery, token usage and costs are unconfirmed. A request may not have started.")
            && !visibleNamed(card, "compactionCoverageNone"),
        "the counts and the exact warning");
    const QString warning = textNamed(card, "compactionCoverageWarning").toLower();
    check(!warning.contains(QStringLiteral("running")) && !warning.contains(QStringLiteral("failed")) && !warning.contains(QStringLiteral("paid")),
        "an open protocol is not 'running' and an unresolved one is not a proven paid failure");
    checkCardLayout(root, "counts and warning");

    // The explanation about tokens is one real tap away.
    check(!visibleNamed(card, "projectCostDetailsTokens"), "the token explanation starts folded");
    tap(host->window, visualChildNamed(card, QStringLiteral("projectCostDetailsToggle")));
    check(visibleNamed(card, "projectCostDetailsTokens") && textNamed(card, "projectCostDetailsTokens").contains(QStringLiteral("not counted")), "...and opens by a tap");
    checkCardLayout(root, "details open");
    tap(host->window, visualChildNamed(card, QStringLiteral("projectCostDetailsToggle")));

    // The intent read fails (a LONG original message): the amount stays, the coverage is unavailable with the original code and message.
    std::string longMessage;
    for (int i = 0; i < 12; ++i) longMessage += "the intent table could not be read because of a very long storage message " + std::to_string(i) + "; ";
    rig.fixture.intents.listFailure = ai::common::Error {"persistence.read_failed", longMessage};
    rig.refresh();
    check(textNamed(card, "projectCostValue") == amount && !visibleNamed(card, "projectCostErrorCode"), "the money survives the intent failure");
    check(visibleNamed(card, "compactionCoverageUnavailable")
            && textNamed(card, "compactionCoverageUnavailable") == QStringLiteral("Compaction recording coverage unavailable")
            && textNamed(card, "compactionCoverageErrorMessage") == QString::fromStdString(longMessage)
            && textNamed(card, "compactionCoverageErrorCode") == QStringLiteral("Error code: persistence.read_failed")
            && !visibleNamed(card, "compactionCoverageCounts") && !visibleNamed(card, "compactionCoverageNone") && !visibleNamed(card, "compactionCoverageWarning"),
        "unavailable: the sentence, the original message and code; no counts, no zero");
    checkCardLayout(root, "coverage unavailable, long message");
    saveShot(root, QStringLiteral("b3iii-card-coverage-unavailable-420"));

    // A money failure hides the coverage together with the sum.
    rig.fixture.intents.listFailure.reset();
    rig.fixture.ledger.listFailure = project_cost_test::scripted("test.ledger_down");
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("Unavailable") && !visibleNamed(card, "compactionCoverageBlock"),
        "an unreadable estimate shows no coverage next to it");
    rig.fixture.ledger.listFailure.reset();

    // A period: the scope says so.
    rig.fixture.intents.listRows = {intentRow("a", "p1", app::CompactionIntentState::Open, 1'000'000'000'500)};
    rig.controller.refreshProjectRange(QStringLiteral("p1"), QDateTime::fromMSecsSinceEpoch(1'000'000'000'000), QDateTime::fromMSecsSinceEpoch(1'000'000'001'000));
    settle();
    check(textNamed(card, "compactionCoverageScope") == QStringLiteral("In the selected period")
            && textNamed(card, "compactionCoverageCounts") == QStringLiteral("Open protocols: 1 · Unresolved compaction operations: 0"),
        "a windowed read is labelled as the selected period");

    // A project with only unfinished protocols: not "empty", and no invented money.
    {
        CostRig only;
        only.fixture.intents.listRows = {intentRow("a", "p1", app::CompactionIntentState::Unresolved, 1000)};
        QVariantMap props;
        props.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
        props.insert(QStringLiteral("colorsRef"), colors);
        props.insert(QStringLiteral("costRef"), QVariant::fromValue<QObject*>(&only.controller));
        auto second = makeHost(engine, kCardHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/CardHarnessOnly.qml")), props, QSize(780, 1420));
        only.refresh();
        QObject* onlyCard = visualChildNamed(second->root, QStringLiteral("card"));
        check(textNamed(onlyCard, "projectCostStatus") == QStringLiteral("No stored cost entries") && textNamed(onlyCard, "projectCostValue") == QStringLiteral("—")
                && !textNamed(onlyCard, "projectCostValue").contains(QStringLiteral("$")),
            "only an unfinished protocol: 'No stored cost entries', a dash, no money");
        check(visibleNamed(onlyCard, "compactionCoverageCounts") && textNamed(onlyCard, "compactionCoverageCounts") == QStringLiteral("Open protocols: 0 · Unresolved compaction operations: 1"),
            "...and its coverage is visible");
        checkCardLayout(second->root, "intent-only project");
    }

    // The geometry at the three widths: counts+warning, and the long unavailable error.
    for (const double width : {620.0, 420.0, 300.0}) {
        check(root->setProperty("cardWidth", width), "the card width");
        rig.fixture.intents.listRows = {intentRow("a", "p1", app::CompactionIntentState::Open, 1000), intentRow("b", "p1", app::CompactionIntentState::Unresolved, 2000)};
        rig.fixture.intents.listFailure.reset();
        rig.refresh();
        settle(40);
        checkCardLayout(root, "counts and warning at a width");
        saveShot(root, QStringLiteral("b3iii-card-counts-%1").arg(static_cast<int>(width)));
        rig.fixture.intents.listFailure = ai::common::Error {"persistence.read_failed", longMessage};
        rig.refresh();
        settle(40);
        checkCardLayout(root, "unavailable and a long message at a width");
        saveShot(root, QStringLiteral("b3iii-card-unavailable-%1").arg(static_cast<int>(width)));
        tap(host->window, visualChildNamed(card, QStringLiteral("projectCostDetailsToggle")));
        checkCardLayout(root, "details open at a width");
        tap(host->window, visualChildNamed(card, QStringLiteral("projectCostDetailsToggle")));
    }
    std::fprintf(stderr, "[compaction-coverage-qml] cost card scenarios passed\n");
}

// ---- the memory-panel side: the REAL controller over the scripted ports of the outcome fixtures ----------------------------------------------
app::CompactionIntentRecoveryReport partialReport()
{
    app::CompactionIntentRecoveryReport report;
    report.status = app::CompactionIntentRecoveryStatus::Partial;
    report.inspected = 12;
    report.markedUnresolved = 3;
    report.alreadyUnresolved = 4;
    report.closedConfirmed = 1;
    report.conflicts = 2;
    report.unconfirmed = 2;
    std::string longMessage;
    for (int i = 0; i < 10; ++i) longMessage += "the operation could not be read because of a long storage message " + std::to_string(i) + "; ";
    report.firstError = ai::common::Error {"persistence.connection_unusable", longMessage};
    report.firstNative = app::CompactionNativeDetail {5, 261, "database table is locked"};
    for (int i = 0; i < 8; ++i) {
        report.diagnostics.push_back({app::CompactionDiagnosticStage::IntentRecovery,
            ai::common::Error {"project_memory.intent_record_conflict", "diagnostic number " + std::to_string(i) + " with a longer explanation that has to wrap on a narrow screen"}});
        report.writeCauses.push_back({ai::common::Error {"persistence.commit_unknown", "write cause " + std::to_string(i) + " may or may not be stored"},
            app::CompactionNativeDetail {10, 522 + i, "disk I/O error " + std::to_string(i)}});
    }
    report.diagnosticsIncomplete = true;
    return report;
}

app::CompactionIntentRecoveryReport completeReport(std::uint64_t inspected)
{
    app::CompactionIntentRecoveryReport report;
    report.status = app::CompactionIntentRecoveryStatus::Complete;
    report.inspected = inspected;
    report.markedUnresolved = inspected;
    return report;
}

QVariantMap backendFor(ProjectMemoryController* memory, CompactionIntentRecoveryController* recovery)
{
    QVariantMap backend;
    backend.insert(QStringLiteral("projectMemory"), QVariant::fromValue<QObject*>(memory));
    if (recovery != nullptr) backend.insert(QStringLiteral("compactionRecovery"), QVariant::fromValue<QObject*>(recovery));
    return backend;
}

const std::vector<const char*> kPanelStack = {"startupRecoveryScope", "startupRecoveryStatus", "startupRecoverySummary", "startupRecoveryFirstError",
    "startupRecoveryFirstErrorCode", "startupRecoveryFirstNative", "startupRecoveryDetailsToggle", "startupRecoveryCounts", "startupRecoveryScopeNote",
    "startupRecoveryDiagnostic", "startupRecoveryWriteCause", "startupRecoveryIncomplete", "unfinishedProtocolsTitle", "unfinishedProtocolsScope",
    "unfinishedProtocolsUnavailable", "unfinishedProtocolsError", "unfinishedProtocolsErrorCode", "unfinishedProtocolsNone", "unfinishedProtocolState",
    "unfinishedProtocolText", "unfinishedProtocolMeta"};

void checkPanelLayout(QObject* root, const char* what)
{
    QObject* panel = visualChildNamed(root, QStringLiteral("projectMemoryPanel"));
    QObject* next = visualChildNamed(root, QStringLiteral("nextBlock"));
    check(panel != nullptr && next != nullptr, "the panel and the next block exist");
    const QRectF panelRect = sceneRectOf(panel);
    if (panelRect.height() < panel->property("implicitHeight").toDouble() - 0.5) {
        std::fprintf(stderr, "[layout] %s: the panel is %.1f high but wants %.1f\n", what, panelRect.height(), panel->property("implicitHeight").toDouble());
    }
    check(panelRect.height() >= panel->property("implicitHeight").toDouble() - 0.5, what);
    // The two new sections are blocks of their own: each inside the panel, clear of each other and of the next block; their texts whole.
    QObject* startup = visualChildNamed(panel, QStringLiteral("projectMemoryStartupRecovery"));
    QObject* unfinished = visualChildNamed(panel, QStringLiteral("projectMemoryUnfinishedProtocols"));
    check(startup != nullptr && unfinished != nullptr, "the two new sections exist");
    const auto describe = [](const char* label, QObject* item) {
        const QRectF r = sceneRectOf(item);
        std::fprintf(stderr, "[layout] %s=(%.1f,%.1f,%.1f,%.1f)\n", label, r.left(), r.top(), r.right(), r.bottom());
    };
    if (flagOf(startup, "visible")) {
        if (!inside(startup, panelRect)) { describe("panel", panel); describe("startup", startup); }
        check(inside(startup, panelRect), what);
        if (flagOf(unfinished, "visible")) {
            if (overlaps(startup, unfinished)) { describe("startup", startup); describe("unfinished", unfinished); }
            check(!overlaps(startup, unfinished), what);
        }
    }
    if (flagOf(unfinished, "visible")) {
        if (!inside(unfinished, panelRect)) { describe("panel", panel); describe("unfinished", unfinished); }
        check(inside(unfinished, panelRect), what);
    }
    for (const char* name : kPanelStack) {
        QList<QObject*> found;
        collectNamed(panel, QString::fromLatin1(name), found);
        for (QObject* item : found) {
            if (!flagOf(item, "visible")) continue;
            checkTextFits(item, what);
            check(inside(item, panelRect), what);
        }
    }
    // Within one block the stacked texts never overlap each other.
    for (QObject* block : {startup, unfinished}) {
        if (!flagOf(block, "visible")) continue;
        std::vector<QObject*> shown;
        for (const char* name : kPanelStack) {
            QList<QObject*> found;
            collectNamed(block, QString::fromLatin1(name), found);
            for (QObject* item : found) {
                if (!flagOf(item, "visible")) continue;
                for (QObject* other : shown) {
                    if (overlaps(item, other)) { describe(name, item); describe("other", other); }
                    check(!overlaps(item, other), what);
                }
                shown.push_back(item);
            }
        }
    }
    if (flagOf(next, "visible") && (overlaps(panel, next) || sceneRectOf(next).top() < panelRect.bottom() - 0.5)) { describe("panel", panel); describe("next", next); }
    if (flagOf(next, "visible")) check(!overlaps(panel, next) && sceneRectOf(next).top() >= panelRect.bottom() - 0.5, what);
}

void runPanelScenarios(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    using namespace memory_outcome;
    // ---- unfinished protocols, wording and ownership of the errors -----------------------------------------------------------------------
    {
        Rig rig;
        QQmlEngine::setObjectOwnership(rig.controller.get(), QQmlEngine::CppOwnership);
        rig.intents.listRows = {intentRow("op-a", "cp1", app::CompactionIntentState::Open, 1000),
            intentRow("op-b", "cp1", app::CompactionIntentState::Unresolved, 2000), intentRow("op-c", "cp1", app::CompactionIntentState::Unresolved, 3000)};
        CompactionIntentRecoveryController recovery(partialReport(), partialReport().firstError);
        QVariantMap initial;
        initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
        initial.insert(QStringLiteral("colorsRef"), colors);
        initial.insert(QStringLiteral("backendRef"), backendFor(rig.controller.get(), &recovery));
        auto host = makeHost(engine, kPanelHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/PanelHarness.qml")), initial, QSize(820, 4220));
        QObject* root = host->root;
        QObject* panel = visualChildNamed(root, QStringLiteral("projectMemoryPanel"));

        // The unfinished section: three rows from ONE read, apart from the history, with no money.
        check(visibleNamed(panel, "projectMemoryUnfinishedProtocols") && visibleCount(panel, "unfinishedProtocolRow") == 3, "three unfinished protocols are listed");
        check(textNamed(panel, "unfinishedProtocolsTitle") == QStringLiteral("Unfinished compaction protocols")
                && textNamed(panel, "unfinishedProtocolsScope") == QStringLiteral("All time"),
            "the section and its scope");
        QList<QObject*> stateLabels, texts;
        collectNamed(panel, QStringLiteral("unfinishedProtocolState"), stateLabels);
        collectNamed(panel, QStringLiteral("unfinishedProtocolText"), texts);
        check(stateLabels.size() == 3 && stateLabels[0]->property("text").toString() == QStringLiteral("Open")
                && stateLabels[1]->property("text").toString() == QStringLiteral("Unresolved"),
            "the states, in the order of the read");
        check(texts[1]->property("text").toString()
                == QStringLiteral("An earlier compaction did not complete its recording protocol. Delivery and cost are unconfirmed; the request may not have started."),
            "an unresolved protocol: the exact sentence");
        const QString openText = texts[0]->property("text").toString().toLower();
        check(!openText.contains(QStringLiteral("earlier")) && !openText.contains(QStringLiteral("crash")) && !openText.contains(QStringLiteral("sent"))
                && !openText.contains(QStringLiteral("running")),
            "an open protocol is neutral: not 'earlier', not a crash, not 'sent', not 'running'");
        QList<QObject*> rows;
        collectNamed(panel, QStringLiteral("unfinishedProtocolRow"), rows);
        for (QObject* row : rows) {
            QList<QObject*> every;
            for (const char* name : {"unfinishedProtocolState", "unfinishedProtocolText", "unfinishedProtocolMeta"}) collectNamed(row, QString::fromLatin1(name), every);
            for (QObject* text : every) check(!text->property("text").toString().contains(QLatin1Char('$')), "an unfinished protocol shows no amount");
        }
        {
            const int before = rig.intents.listCalls;
            invokeNoArgs(panel, "refreshHistory");
            check(rig.intents.listCalls == before + 1, "ONE intent read per history refresh (the counts and the rows share it)");
        }
        checkPanelLayout(root, "panel with rows and a partial report");

        // The database-scope startup report: Partial with the ORIGINAL first error and native detail; Details by a real tap.
        check(visibleNamed(panel, "projectMemoryStartupRecovery")
                && textNamed(panel, "startupRecoveryScope") == QStringLiteral("Database startup recovery — all projects")
                && textNamed(panel, "startupRecoveryStatus") == QStringLiteral("Partial") && visibleNamed(panel, "startupRecoveryFirstError")
                && textNamed(panel, "startupRecoveryFirstError") == QString::fromStdString(partialReport().firstError->message)
                && textNamed(panel, "startupRecoveryFirstErrorCode") == QStringLiteral("Error code: persistence.connection_unusable")
                && visibleNamed(panel, "startupRecoveryFirstNative") && textNamed(panel, "startupRecoveryFirstNative").contains(QStringLiteral("261"))
                && textNamed(panel, "startupRecoveryFirstNative").contains(QStringLiteral("database table is locked")),
            "the startup report: scope, Partial, the original first error and its native detail");
        check(!visibleNamed(panel, "startupRecoveryDetails") && visibleCount(panel, "startupRecoveryDiagnostic") == 0, "Details start folded");
        tap(host->window, visualChildNamed(panel, QStringLiteral("startupRecoveryDetailsToggle")));
        check(visibleNamed(panel, "startupRecoveryDetails") && visibleCount(panel, "startupRecoveryDiagnostic") == 8 && visibleCount(panel, "startupRecoveryWriteCause") == 8
                && visibleNamed(panel, "startupRecoveryIncomplete"),
            "a real tap opens all 8 diagnostics, all 8 write causes and the incomplete note");
        QList<QObject*> causes;
        collectNamed(panel, QStringLiteral("startupRecoveryWriteCause"), causes);
        check(causes.front()->property("text").toString().contains(QStringLiteral("persistence.commit_unknown"))
                && causes.front()->property("text").toString().contains(QStringLiteral("522")) && causes.front()->property("text").toString().contains(QStringLiteral("disk I/O error 0")),
            "each write cause keeps ITS native detail");
        check(textNamed(panel, "startupRecoveryCounts").contains(QStringLiteral("Inspected 12")) && textNamed(panel, "startupRecoveryCounts").contains(QStringLiteral("conflicts 2")),
            "the counters");
        checkPanelLayout(root, "details open");
        saveShot(root, QStringLiteral("b3iii-panel-partial-details-560"));

        // The record, segment and operation errors of the SAME text never overwrite the coverage error; the report does not move.
        const auto& same = sharedReadFailure();
        rig.intents.listFailure = same;
        rig.repository.listFails = true;
        rig.memory.listFails = true;
        invokeNoArgs(panel, "refreshSegments");
        invokeNoArgs(panel, "refreshHistory");
        check(panel->property("segmentsErrorText").toString() == QString::fromStdString(same.message)
                && panel->property("historyErrorText").toString() == QString::fromStdString(same.message) && panel->property("errorText").toString().isEmpty(),
            "the segments and records errors have their own slots; the generic slot stays empty");
        check(visibleNamed(panel, "unfinishedProtocolsUnavailable") && textNamed(panel, "unfinishedProtocolsError") == QString::fromStdString(same.message)
                && textNamed(panel, "unfinishedProtocolsErrorCode") == QStringLiteral("Error code: persistence.read_failed") && visibleCount(panel, "unfinishedProtocolRow") == 0,
            "the coverage error is shown in the section itself, with no rows and no zero");
        check(visibleNamed(panel, "projectMemorySegmentsError") && visibleNamed(panel, "projectMemoryHistoryError") && visibleNamed(panel, "projectMemoryHistoryUnavailable"),
            "all three errors are visible at the same time");
        check(visibleNamed(panel, "projectMemoryStartupRecovery") && textNamed(panel, "startupRecoveryStatus") == QStringLiteral("Partial"), "the database-scope report did not move");
        settle(40);   // the layout of the new texts is settled before it is measured
        checkPanelLayout(root, "three read errors");
        saveShot(root, QStringLiteral("b3iii-panel-three-errors-560"));

        // Recovery of ONE read leaves the others alone.
        rig.repository.listFails = false;
        invokeNoArgs(panel, "refreshHistory");
        check(panel->property("historyErrorText").toString().isEmpty() && !visibleNamed(panel, "projectMemoryHistoryUnavailable")
                && visibleNamed(panel, "unfinishedProtocolsUnavailable") && !panel->property("segmentsErrorText").toString().isEmpty(),
            "the records recovered; the coverage error and the segments error stayed");
        rig.intents.listFailure.reset();
        invokeNoArgs(panel, "refreshHistory");
        check(!visibleNamed(panel, "unfinishedProtocolsUnavailable") && visibleCount(panel, "unfinishedProtocolRow") == 3 && !panel->property("segmentsErrorText").toString().isEmpty(),
            "the coverage recovered; the segments error is still its own");
        rig.memory.listFails = false;
        invokeNoArgs(panel, "refreshSegments");

        // Another project: the project rows are dropped BEFORE the new reads; the database report stays.
        check(root->setProperty("projectIdValue", QStringLiteral("cp2")), "another project");
        settle();
        check(visibleCount(panel, "unfinishedProtocolRow") == 0 && visibleNamed(panel, "unfinishedProtocolsNone")
                && textNamed(panel, "unfinishedProtocolsNone") == QStringLiteral("No open or unresolved compaction operations in this scope"),
            "another project: its own real zero, none of the previous rows");
        check(visibleNamed(panel, "projectMemoryStartupRecovery") && textNamed(panel, "startupRecoveryStatus") == QStringLiteral("Partial"),
            "the startup report is the database's, not the project's: it stays");
        check(root->setProperty("projectIdValue", QString()), "no project");
        settle();
        check(!visibleNamed(panel, "projectMemoryUnfinishedProtocols") && visibleNamed(panel, "projectMemoryStartupRecovery"),
            "no project: no unfinished section, the database report stays");
    }

    // ---- the report states: complete with activity, complete and empty, absent ----------------------------------------------------------------
    for (const int variant : {0, 1, 2}) {
        Rig rig;
        QQmlEngine::setObjectOwnership(rig.controller.get(), QQmlEngine::CppOwnership);
        std::unique_ptr<CompactionIntentRecoveryController> recovery;
        if (variant == 0) recovery = std::make_unique<CompactionIntentRecoveryController>(completeReport(2), std::nullopt);
        if (variant == 1) recovery = std::make_unique<CompactionIntentRecoveryController>(completeReport(0), std::nullopt);
        if (variant == 2) recovery = std::make_unique<CompactionIntentRecoveryController>(std::nullopt, std::nullopt);
        QVariantMap initial;
        initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
        initial.insert(QStringLiteral("colorsRef"), colors);
        initial.insert(QStringLiteral("backendRef"), backendFor(rig.controller.get(), recovery.get()));
        auto host = makeHost(engine, kPanelHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/PanelHarnessReport.qml")), initial, QSize(820, 2200));
        QObject* panel = visualChildNamed(host->root, QStringLiteral("projectMemoryPanel"));
        const bool shown = visibleNamed(panel, "projectMemoryStartupRecovery");
        if (variant == 0) {
            check(shown && textNamed(panel, "startupRecoveryStatus") == QStringLiteral("Complete") && !visibleNamed(panel, "startupRecoveryFirstError")
                    && !visibleNamed(panel, "startupRecoveryFirstNative"),
                "Complete with inspected rows: a neutral summary, no error, no native");
        } else {
            check(!shown, variant == 1 ? "Complete without rows or notes is hidden" : "an absent report (the pass did not take part) is hidden, not shown as complete or as zeros");
        }
    }
    std::fprintf(stderr, "[compaction-coverage-qml] memory panel scenarios passed\n");
}

// The geometry of the panel at the three widths, with the heaviest content.
void runPanelGeometry(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    using namespace memory_outcome;
    Rig rig;
    QQmlEngine::setObjectOwnership(rig.controller.get(), QQmlEngine::CppOwnership);
    for (int i = 0; i < 6; ++i) {
        rig.intents.listRows.push_back(intentRow("op-" + std::to_string(i), "cp1", i % 2 == 0 ? app::CompactionIntentState::Open : app::CompactionIntentState::Unresolved, 1000 + i));
    }
    CompactionIntentRecoveryController recovery(partialReport(), partialReport().firstError);
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    initial.insert(QStringLiteral("backendRef"), backendFor(rig.controller.get(), &recovery));
    auto host = makeHost(engine, kPanelHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/PanelGeometry.qml")), initial, QSize(820, 4220));
    QObject* panel = visualChildNamed(host->root, QStringLiteral("projectMemoryPanel"));
    for (const double width : {620.0, 420.0, 300.0}) {
        check(host->root->setProperty("panelWidth", width), "the panel width");
        settle(40);
        // folded, then open
        if (flagOf(visualChildNamed(panel, QStringLiteral("startupRecoveryDetails")), "visible"))
            tap(host->window, visualChildNamed(panel, QStringLiteral("startupRecoveryDetailsToggle")));
        checkPanelLayout(host->root, "geometry, folded");
        saveShot(host->root, QStringLiteral("b3iii-panel-folded-%1").arg(static_cast<int>(width)));
        tap(host->window, visualChildNamed(panel, QStringLiteral("startupRecoveryDetailsToggle")));
        check(visibleCount(panel, "startupRecoveryDiagnostic") == 8 && visibleCount(panel, "startupRecoveryWriteCause") == 8 && visibleCount(panel, "unfinishedProtocolRow") == 6,
            "8 causes, 8 write causes and 6 rows are on screen");
        checkPanelLayout(host->root, "geometry, details open");
        saveShot(host->root, QStringLiteral("b3iii-panel-open-%1").arg(static_cast<int>(width)));
        tap(host->window, visualChildNamed(panel, QStringLiteral("startupRecoveryDetailsToggle")));
    }
    std::fprintf(stderr, "[compaction-coverage-qml] panel geometry passed\n");
}

// ---- a REAL bridge over a disposable database that was seeded BEFORE its construction ----------------------------------------------------
struct Workspace {
    fs::path root = fs::temp_directory_path() / ("aio-b3iii-qml-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path project = root / "project";
    fs::path database = root / "state" / "orchestrator.sqlite";
    Workspace() { fs::create_directories(project); }
    ~Workspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    [[nodiscard]] QString databaseQt() const { return QString::fromStdWString(database.wstring()); }
    [[nodiscard]] QString projectQt() const { return QString::fromStdWString(project.wstring()); }
};

void seedIntent(const fs::path& databasePath, const std::string& id, const std::string& projectId)
{
    const auto opened = ai::infrastructure::SqliteDatabase::open(databasePath);
    check(opened.success(), "the second connection to the disposable database opens");
    ai::infrastructure::SqliteProjectMemoryCompactionIntentRepository intents(opened.value());
    app::ProjectMemoryCompactionIntent intent = intentRow(id, projectId, app::CompactionIntentState::Open, 1'700'000'000'000);
    intent.providerId = "pcost-prov";
    intent.modelId = "pcost-model";
    check(app::mayDispatch(intents.recordIntentObserved(intent)), "the intent is admitted");
}

void runRealBridgeScenario(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    Workspace workspace;
    std::string projectId;
    {
        BackendBridge first(workspace.databaseQt());
        check(first.storageAvailable(), "durable storage");
        const QString created = first.projectController().createProject(QStringLiteral("Coverage"), QStringLiteral("B3-III"), workspace.projectQt());
        check(!created.isEmpty(), "a project");
        projectId = created.toStdString();
    }
    seedIntent(workspace.database, "bridge-op", projectId);   // a previous process left it open

    BackendBridge bridge(workspace.databaseQt());
    check(bridge.storageAvailable(), "the owner opens the database");
    QVariantMap report = bridge.compactionRecovery()->property("report").toMap();
    check(report.value(QStringLiteral("status")).toString() == QStringLiteral("complete") && report.value(QStringLiteral("markedUnresolved")).toULongLong() == 1,
        "the owner's startup marked the intent unresolved");

    // The cost card over the REAL bridge controller.
    {
        QVariantMap initial;
        initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
        initial.insert(QStringLiteral("colorsRef"), colors);
        initial.insert(QStringLiteral("costRef"), QVariant::fromValue<QObject*>(bridge.projectCost()));
        auto host = makeHost(engine, kCardHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/BridgeCard.qml")), initial, QSize(780, 1420));
        check(bridge.projectCostController().refreshProject(QString::fromStdString(projectId)), "the real estimate refreshes");
        settle();
        QObject* card = visualChildNamed(host->root, QStringLiteral("card"));
        check(textNamed(card, "projectCostStatus") == QStringLiteral("No stored cost entries") && textNamed(card, "projectCostValue") == QStringLiteral("—"),
            "the real runtime: no money, and not an 'empty' project");
        check(textNamed(card, "compactionCoverageCounts") == QStringLiteral("Open protocols: 0 · Unresolved compaction operations: 1")
                && visibleNamed(card, "compactionCoverageWarning") && textNamed(card, "compactionCoverageScope") == QStringLiteral("All time"),
            "the real runtime: the Unresolved intent in the coverage");
        checkCardLayout(host->root, "real bridge card");
        saveShot(host->root, QStringLiteral("b3iii-bridge-card"));
    }

    // The panel over the REAL bridge: the Unresolved row and the database report.
    {
        QVariantMap initial;
        initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
        initial.insert(QStringLiteral("colorsRef"), colors);
        initial.insert(QStringLiteral("backendRef"), QVariant::fromValue<QObject*>(&bridge));
        initial.insert(QStringLiteral("projectIdValue"), QString::fromStdString(projectId));
        auto host = makeHost(engine, kPanelHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/BridgePanel.qml")), initial, QSize(820, 2400));
        QObject* panel = visualChildNamed(host->root, QStringLiteral("projectMemoryPanel"));
        check(ai::ui::test_support::waitForCondition("the real panel lists the protocol",
                  [&] { return visibleCount(panel, "unfinishedProtocolRow") == 1; }, 5s),
            "the real panel lists the Unresolved protocol");
        check(textNamed(panel, "unfinishedProtocolState") == QStringLiteral("Unresolved") && textNamed(panel, "unfinishedProtocolMeta").contains(QStringLiteral("marked unresolved")),
            "the real row: Unresolved with its mark");
        check(visibleNamed(panel, "projectMemoryStartupRecovery") && textNamed(panel, "startupRecoveryStatus") == QStringLiteral("Complete"),
            "the real database report: Complete with the inspected row");
        tap(host->window, visualChildNamed(panel, QStringLiteral("startupRecoveryDetailsToggle")));
        check(textNamed(panel, "startupRecoveryCounts").contains(QStringLiteral("Inspected 1")) && textNamed(panel, "startupRecoveryCounts").contains(QStringLiteral("marked unresolved 1")),
            "...and its Details name the counters");
        checkPanelLayout(host->root, "real bridge panel");
        saveShot(host->root, QStringLiteral("b3iii-bridge-panel"));
    }
    std::fprintf(stderr, "[compaction-coverage-qml] real bridge scenario passed\n");
}

} // namespace

void runCompactionCoverageQmlTests(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    runCardScenarios(engine, hostWindow, colors);
    runPanelScenarios(engine, hostWindow, colors);
    runPanelGeometry(engine, hostWindow, colors);
    runRealBridgeScenario(engine, hostWindow, colors);
}

void checkCompactionCoverageOverviewPage(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors, QObject* overviewPage,
    const std::filesystem::path& databasePath, const std::string& projectId)
{
    auto* pageItem = qobject_cast<QQuickItem*>(overviewPage);
    check(pageItem != nullptr && pageItem->window() != nullptr, "the real Overview page is an item in a window");
    QQuickWindow* const appWindow = pageItem->window();
    const QVariant previousSection = appWindow->property("currentSection");
    check(appWindow->setProperty("currentSection", QStringLiteral("overview")), "the Overview section is shown");
    settle(40);

    check(ai::ui::test_support::waitForCondition("the Overview cost card shows its coverage block",
              [&] { return visibleNamed(overviewPage, "compactionCoverageBlock"); }, 5s),
        "the real page shows the coverage block of a readable estimate");
    const QString valueBefore = textNamed(overviewPage, "projectCostValue");
    const QString statusBefore = textNamed(overviewPage, "projectCostStatus");
    check(visibleNamed(overviewPage, "compactionCoverageNone"), "the real page: a real zero before the intent exists");

    seedIntent(databasePath, "overview-open-1", projectId);   // AFTER the live startup: an Open protocol
    check(QMetaObject::invokeMethod(overviewPage, "refreshAnalytics"), "the real page refreshes");
    check(ai::ui::test_support::waitForCondition("the real page shows the Open protocol",
              [&] { return visibleNamed(overviewPage, "compactionCoverageCounts"); }, 5s),
        "the real page shows the counts");
    check(textNamed(overviewPage, "compactionCoverageCounts") == QStringLiteral("Open protocols: 1 · Unresolved compaction operations: 0")
            && visibleNamed(overviewPage, "compactionCoverageWarning") && textNamed(overviewPage, "compactionCoverageScope") == QStringLiteral("All time"),
        "the real page: the Open protocol, the warning and the scope");
    check(textNamed(overviewPage, "projectCostValue") == valueBefore && textNamed(overviewPage, "projectCostStatus") == statusBefore,
        "the real page: the amount and its status are exactly what they were");

    // A real ProjectMemoryPanel over the very same bridge lists the protocol (Open: neutral wording, no amount).
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    initial.insert(QStringLiteral("backendRef"), overviewPage->property("backend"));
    initial.insert(QStringLiteral("projectIdValue"), QString::fromStdString(projectId));
    auto host = makeHost(engine, kPanelHarness, QUrl(QStringLiteral("qrc:/b3iii-coverage/OverviewBridgePanel.qml")), initial, QSize(820, 2400));
    QObject* panel = visualChildNamed(host->root, QStringLiteral("projectMemoryPanel"));
    check(ai::ui::test_support::waitForCondition("the panel over the live bridge lists the protocol",
              [&] { return visibleCount(panel, "unfinishedProtocolRow") == 1; }, 5s),
        "the panel lists the Open protocol");
    check(textNamed(panel, "unfinishedProtocolState") == QStringLiteral("Open") && !textNamed(panel, "unfinishedProtocolText").contains(QStringLiteral("earlier")),
        "an Open protocol, worded neutrally");
    check(!visibleNamed(panel, "projectMemoryStartupRecovery"), "the live startup report of an empty database has nothing to show");
    checkPanelLayout(host->root, "live bridge panel");

    appWindow->setProperty("currentSection", previousSection);
    std::fprintf(stderr, "[compaction-coverage-qml] overview page scenario passed\n");
}
