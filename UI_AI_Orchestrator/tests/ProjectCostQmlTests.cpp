#include "ProjectCostQmlTests.hpp"

#include "../test_support/QtTestSupport.hpp"

#include <ProjectCostTestSupport.hpp>

#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/ProjectCostController.hpp>

#include <QDateTime>
#include <QEventLoop>
#include <QJSValue>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QtMath>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace project_cost_test;
namespace app = ai::application;
using S = app::ProjectMemoryCompactionStatus;

// ---- checks: they stay active under NDEBUG and say what failed ----------------------------------------------------------------------
void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "[project-cost-qml] FAILED: %s\n", what);
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

bool flagOf(QObject* object, const char* name) { return object->property(name).toBool(); }

// An item that exists and is visible. A missing item is a failure, never "not visible".
bool visibleNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
    if (found == nullptr) std::fprintf(stderr, "[project-cost-qml] missing item '%s'\n", name);
    check(found != nullptr, "the named item exists");
    return flagOf(found, "visible");
}

QString textNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
    if (found == nullptr) std::fprintf(stderr, "[project-cost-qml] missing item '%s'\n", name);
    check(found != nullptr, "the named text exists");
    return found->property("text").toString();
}

bool overlaps(QObject* first, QObject* second)
{
    return sceneRectOf(first).adjusted(0.5, 0.5, -0.5, -0.5).intersects(sceneRectOf(second).adjusted(0.5, 0.5, -0.5, -0.5));
}

bool inside(QObject* inner, const QRectF& outer)
{
    const QRectF rect = sceneRectOf(inner);
    return rect.left() >= outer.left() - 0.5 && rect.right() <= outer.right() + 0.5
        && rect.top() >= outer.top() - 0.5 && rect.bottom() <= outer.bottom() + 0.5;
}

// The text fits its own box: nothing elided, nothing wider/taller than the box that was given to it.
void checkTextFits(QObject* text, const char* what)
{
    const double width = text->property("width").toDouble();
    const double height = text->property("height").toDouble();
    const bool fits = !flagOf(text, "truncated") && text->property("contentWidth").toDouble() <= width + 0.5
        && text->property("contentHeight").toDouble() <= height + 0.5;
    if (!fits) {
        std::fprintf(stderr, "[layout] %s does not fit: width=%.1f contentWidth=%.1f height=%.1f contentHeight=%.1f truncated=%d text='%s'\n",
            what, width, text->property("contentWidth").toDouble(), height, text->property("contentHeight").toDouble(),
            flagOf(text, "truncated") ? 1 : 0, text->property("text").toString().toUtf8().constData());
    }
    check(fits, what);
}

// The ITEM is grabbed, not the window: it keeps its own size whatever the screen's scaling does to a top-level window.
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

// A real tap (press + release at the centre of the item) delivered to the window: the TapHandler of the toggle must react to it.
void tap(QQuickWindow& window, QObject* item)
{
    const QPointF point = sceneRectOf(item).center();
    QMouseEvent press(QEvent::MouseButtonPress, point, window.mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, window.mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &release);
    settle();
}

QString money(const char* text) { return QString::fromUtf8(text); }

// ---- the rig: the REAL controller over the real services and scripted sources -------------------------------------------------------
struct Rig {
    Fixture fixture;
    app::InMemoryModelRepository names;
    app::ProviderCatalogService catalogService {fixture.providers, names};
    ProjectCostController controller {fixture.service, catalogService};

    Rig()
    {
        app::Model model;
        model.id = "m";
        model.providerId = "prov";
        model.apiModelId = "m";
        model.displayName = "m";
        model.contextWindowTokens = 100'000;
        model.maxOutputTokens = 4096;
        model.inputPriceMicrosPerMillionTokens = 2'000'000;
        model.outputPriceMicrosPerMillionTokens = 10'000'000;
        check(fixture.catalog.insert(model).success(), "seed the catalog");
    }
    void ledger(const char* id, std::uint64_t cost, bool priceKnown, int seconds = 1)
    {
        check(fixture.ledger.insertExecution(ledgerRow(id, "p", "prov", "m", 10, 10, cost, priceKnown, seconds)).success(), "seed the ledger");
    }
    void compaction(const char* id, std::optional<bool> known, std::uint64_t cost, std::uint64_t in, std::uint64_t out, S status,
        int seconds = 1)
    {
        fixture.compactions.records.push_back(compactionRecord(id, "p", "prov", "m", known, cost, in, out, status, seconds));
    }
    void refresh()
    {
        controller.refreshProject(QStringLiteral("p"));
        settle();
    }
};

const char* const kCardTexts[] = {"projectCostTitle", "projectCostValue", "projectCostStatus", "projectCostBreakdown",
    "projectCostNoPriceRecords", "projectCostErrorCode", "projectCostCoverage", "projectCostDetailsToggle",
    "projectCostDetailsCoverage", "projectCostDetailsPricing"};

// Every VISIBLE text of the card: whole (not cut), inside the card, and clear of every other visible text.
void checkCardGeometry(QObject* card, const char* what)
{
    const QRectF cardRect = sceneRectOf(card);
    check(cardRect.height() >= card->property("implicitHeight").toDouble() - 0.5, what);
    std::vector<QObject*> shown;
    for (const char* name : kCardTexts) {
        QObject* found = visualChildNamed(card, QString::fromLatin1(name));
        if (found == nullptr || !flagOf(found, "visible")) continue;
        checkTextFits(found, what);
        check(inside(found, cardRect), what);
        for (QObject* other : shown) check(!overlaps(found, other), what);
        shown.push_back(found);
    }
    check(!shown.empty(), "the card shows something");
}

// ---- the scenarios -------------------------------------------------------------------------------------------------------------------------
void scenarios(QObject* root, QQuickWindow& window, Rig& rig)
{
    auto* card = visualChildNamed(root, QStringLiteral("card"));
    check(card != nullptr, "the card exists");
    const auto set = [&](const char* name, const QVariant& value) { check(root->setProperty(name, value), name); settle(); };

    // idle: no project
    check(textNamed(card, "projectCostTitle") == QStringLiteral("Project cost estimate"), "the title is 'Project cost estimate'");
    check(textNamed(card, "projectCostValue") == QStringLiteral("—") && textNamed(card, "projectCostStatus") == QStringLiteral("No project selected"),
        "no project: a dash and 'No project selected'");
    check(!visibleNamed(card, "projectCostCoverage") && !visibleNamed(card, "projectCostBreakdown") && !visibleNamed(card, "projectCostDetailsToggle"),
        "an idle card claims nothing about coverage");

    // an empty project is EMPTY (and only that is)
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("—")
            && textNamed(card, "projectCostStatus") == QStringLiteral("No recorded model executions or memory compactions yet"),
        "an empty project: a dash and the empty phrase");
    check(!visibleNamed(card, "projectCostBreakdown") && visibleNamed(card, "projectCostCoverage"), "...no breakdown, the coverage caption is there");
    check(textNamed(card, "projectCostCoverage") == QStringLiteral("Based on recorded executions and memory compactions"), "the coverage caption");
    checkCardGeometry(card, "empty card");

    // a project made ONLY of records without price data is not empty
    rig.compaction("n1", std::nullopt, 0, 0, 0, S::Cancelled);
    rig.compaction("n2", std::nullopt, 0, 0, 0, S::Failed, 2);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("No cost data recorded"), "none-only: 'No cost data recorded'");
    check(textNamed(card, "projectCostStatus") == QStringLiteral("2 memory compaction records have no price data"), "none-only: the count of such records");
    check(!textNamed(card, "projectCostStatus").contains(QStringLiteral("No recorded model executions")) && !textNamed(card, "projectCostValue").contains(QStringLiteral("$")),
        "none-only is never called empty and never shows a cost");
    check(textNamed(card, "projectCostBreakdown") == QStringLiteral("0 model executions · 2 memory compactions"), "...the breakdown shows the records");
    check(!visibleNamed(card, "projectCostNoPriceRecords"), "...the count is not repeated on a second line");
    checkCardGeometry(card, "none-only card");

    // compaction-only, known: the amount of the stored record, the breakdown, no ledger "calls"
    rig.fixture.compactions.records.clear();
    rig.compaction("c1", true, 321, 10, 5, S::Completed);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == money("$0.0003") && !visibleNamed(card, "projectCostStatus"), "compaction-only known: $0.0003, no status line");
    check(textNamed(card, "projectCostBreakdown") == QStringLiteral("0 model executions · 1 memory compactions"), "the breakdown");

    // a known zero is visible as $0.0000
    rig.fixture.compactions.records.clear();
    rig.compaction("z", true, 0, 10, 5, S::Completed);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == money("$0.0000"), "a known zero is $0.0000");

    // positive amount with zero tokens is visible
    rig.fixture.compactions.records.clear();
    rig.compaction("pz", true, 700, 0, 0, S::Failed);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == money("$0.0007"), "a positive stored amount at zero tokens is shown");

    // unknown: no amount, no catalog advice
    rig.fixture.compactions.records.clear();
    rig.compaction("u1", false, 0, 0, 0, S::Failed);
    rig.compaction("u2", std::nullopt, 0, 7, 0, S::Completed, 2);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("Cost unknown"), "unknown: 'Cost unknown'");
    check(textNamed(card, "projectCostStatus") == QStringLiteral("2 items have an unknown price"), "unknown: the count");
    check(!textNamed(card, "projectCostStatus").contains(QStringLiteral("pricing"), Qt::CaseInsensitive), "unknown never advises changing prices");
    checkCardGeometry(card, "unknown card");

    // partial: the amount that could be priced, the total unknown, the denominator is the pricing evidence
    rig.fixture.compactions.records.clear();
    rig.compaction("k", true, 300, 1, 1, S::Completed);
    rig.compaction("p1", false, 200, 1, 1, S::Failed, 2);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("Incomplete estimate: $0.0005; total unknown"), "partial: the amount and 'total unknown'");
    check(textNamed(card, "projectCostStatus") == QStringLiteral("1 of 2 pricing entries have an incomplete price"), "partial: 1 of 2 entries");
    checkCardGeometry(card, "partial card");
    saveShot(root, QStringLiteral("a1min-cost-card-partial"));

    // the explanation is one tap away (no hover)
    check(!visibleNamed(card, "projectCostDetailsCoverage") && !visibleNamed(card, "projectCostDetailsPricing"), "the details start closed");
    check(textNamed(card, "projectCostDetailsToggle") == QStringLiteral("Details"), "the toggle is labelled");
    tap(window, visualChildNamed(card, QStringLiteral("projectCostDetailsToggle")));
    check(visibleNamed(card, "projectCostDetailsCoverage") && visibleNamed(card, "projectCostDetailsPricing"), "a tap opens the details");
    check(textNamed(card, "projectCostDetailsCoverage") == QStringLiteral("Operations that were interrupted or never recorded may be missing."),
        "...what may be missing");
    check(textNamed(card, "projectCostDetailsPricing").contains(QStringLiteral("priced from the current model catalog"))
            && textNamed(card, "projectCostDetailsPricing").contains(QStringLiteral("memory compaction costs are shown as recorded")),
        "...the legacy pricing explanation");
    check(textNamed(card, "projectCostDetailsToggle") == QStringLiteral("Hide details"), "the toggle now hides");
    checkCardGeometry(card, "card with details");
    saveShot(root, QStringLiteral("a1min-cost-card-details"));
    tap(window, visualChildNamed(card, QStringLiteral("projectCostDetailsToggle")));
    check(!visibleNamed(card, "projectCostDetailsCoverage"), "a second tap closes them");

    // an unreadable estimate after a success: no old sum, the original code, then recovery
    rig.refresh();
    check(textNamed(card, "projectCostValue").contains(QStringLiteral("$")), "a sum is shown before the failure");
    rig.fixture.compactions.listFailure = scripted("test.compaction_down");
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("Unavailable"), "unreadable: 'Unavailable'");
    check(textNamed(card, "projectCostStatus") == QStringLiteral("The project cost could not be read: scripted: test.compaction_down"),
        "unreadable: the original message");
    check(visibleNamed(card, "projectCostErrorCode") && textNamed(card, "projectCostErrorCode") == QStringLiteral("Error code: test.compaction_down"),
        "unreadable: the original code");
    check(!visibleNamed(card, "projectCostBreakdown") && !visibleNamed(card, "projectCostCoverage") && !visibleNamed(card, "projectCostDetailsToggle")
            && !textNamed(card, "projectCostValue").contains(QStringLiteral("$")),
        "unreadable: no old sum, no breakdown, no coverage claim");
    checkCardGeometry(card, "unavailable card");
    saveShot(root, QStringLiteral("a1min-cost-card-unavailable"));
    rig.fixture.compactions.listFailure.reset();
    rig.refresh();
    check(textNamed(card, "projectCostValue") == QStringLiteral("Incomplete estimate: $0.0005; total unknown") && !visibleNamed(card, "projectCostErrorCode"),
        "recovery: the estimate is back and the error line is gone");

    // mixed, plus a none record apart
    rig.fixture.compactions.records.clear();
    rig.ledger("e1", 200, true);
    rig.compaction("c1", true, 300, 5, 5, S::Completed);
    rig.compaction("n1", std::nullopt, 0, 0, 0, S::Cancelled, 2);
    rig.refresh();
    check(textNamed(card, "projectCostValue") == money("$0.0005"), "mixed: the sum of both sources");
    check(textNamed(card, "projectCostBreakdown") == QStringLiteral("1 model executions · 2 memory compactions"), "mixed: both counts, apart");
    check(visibleNamed(card, "projectCostNoPriceRecords")
            && textNamed(card, "projectCostNoPriceRecords") == QStringLiteral("1 memory compaction records have no price data"),
        "mixed: the record without price data is counted on its own line");
    checkCardGeometry(card, "mixed card");

    // geometry at the three logical widths, with a long amount and a long, unbroken error text
    rig.fixture.compactions.records.clear();
    rig.compaction("big", true, 123'456'789'012ULL, 1, 1, S::Completed);
    for (const double width : {620.0, 420.0, 300.0}) {
        set("cardWidth", width);
        rig.refresh();
        check(textNamed(card, "projectCostValue") == money("$123456.79"), "a long known amount");
        checkCardGeometry(card, "long amount");
    }
    std::string longMessage(180, 'x');
    rig.fixture.compactions.listFailure = common::Error {"persistence." + std::string(60, 'c'), longMessage + " — データベースエラー"};
    for (const double width : {620.0, 420.0, 300.0}) {
        set("cardWidth", width);
        rig.refresh();
        check(textNamed(card, "projectCostValue") == QStringLiteral("Unavailable"), "a long error: still unavailable");
        checkCardGeometry(card, "long error text");
    }
    rig.fixture.compactions.listFailure.reset();
    set("cardWidth", 420.0);
}

QVariantList chartRows(bool withCompactions, const char* basis)
{
    QVariantMap row {{"name", QStringLiteral("Provider A")}, {"executionCount", 3}, {"tokensLabel", QStringLiteral("4.2M")},
        {"costLabel", withCompactions ? QStringLiteral("No price data") : QStringLiteral("$0.2507")}, {"priceIncomplete", false},
        {"share", 0.0}, {"shareBasis", QString::fromLatin1(basis)}, {"models", QVariantList {}}};
    if (withCompactions) row.insert(QStringLiteral("compactionCount"), 2);
    return QVariantList {row};
}

void chartScenarios(QObject* root)
{
    auto* chart = visualChildNamed(root, QStringLiteral("chart"));
    check(chart != nullptr, "the chart exists");
    const auto set = [&](const char* name, const QVariant& value) { check(root->setProperty(name, value), name); settle(); };

    // executions and compactions are labelled apart; the old label said 'calls' for ledger rows only
    set("providers", chartRows(true, "unavailable"));
    set("sharesNote", QStringLiteral("Provider shares are unavailable because some rows have no price data"));
    check(textNamed(chart, "providerCallsLabel") == QStringLiteral("3 executions"), "ledger rows are 'executions', not 'calls'");
    check(visibleNamed(chart, "providerCompactionsLabel") && textNamed(chart, "providerCompactionsLabel") == QStringLiteral("2 memory compactions"),
        "memory compactions are counted next to them");
    check(visibleNamed(chart, "providerSharesNote")
            && textNamed(chart, "providerSharesNote") == QStringLiteral("Provider shares are unavailable because some rows have no price data"),
        "the neutral explanation of the missing shares");
    check(!visibleNamed(chart, "incompletePricingNote"), "...and it is not worded as an incomplete price");
    check(textNamed(chart, "providerCostLabel") == QStringLiteral("No price data"), "a row without price data says so");

    // rows without compactions (the older shape) keep their layout: no compactions label, no note
    set("sharesNote", QString());
    set("providers", chartRows(false, "cost"));
    check(!visibleNamed(chart, "providerCompactionsLabel") && !visibleNamed(chart, "providerSharesNote"), "no compactions, no note");

    // geometry of the row with every element, at the three widths
    set("providers", chartRows(true, "unavailable"));
    set("sharesNote", QStringLiteral("Provider shares are unavailable because some rows have no price data"));
    for (const double width : {620.0, 420.0, 300.0}) {
        set("cardWidth", width);
        const QRectF chartRect = sceneRectOf(chart);
        QObject* name = visualChildNamed(chart, QStringLiteral("providerNameLabel"));
        QObject* calls = visualChildNamed(chart, QStringLiteral("providerCallsLabel"));
        QObject* compactions = visualChildNamed(chart, QStringLiteral("providerCompactionsLabel"));
        QObject* tokens = visualChildNamed(chart, QStringLiteral("providerTokensLabel"));
        QObject* cost = visualChildNamed(chart, QStringLiteral("providerCostLabel"));
        QObject* note = visualChildNamed(chart, QStringLiteral("providerSharesNote"));
        check(name && calls && compactions && tokens && cost && note, "the row elements exist");
        for (QObject* part : {calls, compactions, tokens, cost, note}) {
            checkTextFits(part, "chart element");
            check(inside(part, chartRect), "chart element inside the panel");
        }
        check(!overlaps(calls, compactions) && !overlaps(compactions, tokens) && !overlaps(compactions, cost) && !overlaps(calls, tokens)
                && !overlaps(cost, tokens) && !overlaps(name, calls) && !overlaps(compactions, note),
            "the row elements do not overlap");
    }
    saveShot(root, QStringLiteral("a1min-cost-chart-unavailable-shares"));
}

} // namespace

void runProjectCostQmlTests(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import QtQuick.Layouts
import UI_AI_Orchestrator
Item {
    id: root
    property var hostRef
    property var colorsRef
    property var costRef
    property real cardWidth: 420
    property var providers: []
    property string sharesNote: ""
    width: 720
    height: 1100
    ProjectCostCard {
        objectName: "card"
        x: 10; y: 10
        width: root.cardWidth
        height: implicitHeight
        hostWindow: root.hostRef; colors: root.colorsRef
        cost: root.costRef
    }
    OverviewProviderChart {
        id: chart
        objectName: "chart"
        x: 10
        y: 760
        width: root.cardWidth
        height: implicitHeight
        hostWindow: root.hostRef; colors: root.colorsRef
        providerModel: root.providers
        sharesNote: root.sharesNote
    }
}
)QML", QUrl(QStringLiteral("qrc:/a1min-cost/Harness.qml")));
    if (component.isError()) {
        for (const auto& error : component.errors()) std::fprintf(stderr, "[project-cost-qml] %s\n", error.toString().toUtf8().constData());
    }
    check(component.isReady(), "the project cost harness compiles");

    Rig rig;
    QQuickWindow window;
    window.resize(740, 1120);
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    initial.insert(QStringLiteral("costRef"), QVariant::fromValue<QObject*>(&rig.controller));
    std::unique_ptr<QObject> owner(component.createWithInitialProperties(initial));
    check(owner != nullptr, "the harness is created");
    auto* root = qobject_cast<QQuickItem*>(owner.get());
    check(root != nullptr, "the harness is an item");
    root->setParentItem(window.contentItem());
    window.show();
    check(ai::ui::test_support::waitForCondition("the cost harness is exposed", [&] { return window.isExposed(); }, 5s), "exposed");
    settle();

    scenarios(root, window, rig);
    std::fprintf(stderr, "[project-cost-qml] card scenarios passed\n");
    chartScenarios(root);
    std::fprintf(stderr, "[project-cost-qml] chart scenarios passed\n");
}

// The stored compaction history of a project, seeded through a SECOND connection with the real repository (no production seam). The
// provider and the model are not in the catalog: the page then names them by their ids.
void seedPositiveCompaction(const std::filesystem::path& databasePath, const std::string& projectId)
{
    const auto opened = ai::infrastructure::SqliteDatabase::open(databasePath);
    check(opened.success(), "the second connection to the disposable database opens");
    ai::infrastructure::SqliteProjectMemoryCompactionRepository compactions(opened.value());
    app::ProjectMemoryCompactionRecord record;
    record.id = "overview-positive-1";
    record.projectId = projectId;
    record.providerId = "pcost-prov";
    record.modelId = "pcost-model";
    record.sourceSegmentIds = {"s1"};
    record.sourceContentHash = "hash-overview-positive-1";
    record.inputTokens = 10;
    record.outputTokens = 5;
    record.costMicros = 321;
    record.costPriceKnown = true;
    record.status = S::Failed;
    record.errorCode = "provider.failed";
    record.errorMessage = "scripted";
    record.createdAt = std::chrono::system_clock::time_point {std::chrono::milliseconds {1'700'000'000'000}};
    check(compactions.insertCompactionRecord(record).success(), "the positive compaction record is stored");
}

// The page scrolls: bring an item into the viewport of the nearest Flickable above it, so a real tap lands on it.
void scrollIntoView(QObject* target)
{
    auto* item = qobject_cast<QQuickItem*>(target);
    check(item != nullptr, "a visual item to scroll to");
    QQuickItem* flick = item->parentItem();
    while (flick != nullptr && !flick->inherits("QQuickFlickable")) flick = flick->parentItem();
    check(flick != nullptr, "the item sits in a Flickable");
    auto* content = flick->property("contentItem").value<QQuickItem*>();
    check(content != nullptr, "the Flickable has a content item");
    const double top = item->mapToItem(content, QPointF(0, 0)).y();
    const double maximumY = qMax(0.0, flick->property("contentHeight").toDouble() - flick->height());
    check(flick->setProperty("contentY", qBound(0.0, top - 40.0, maximumY)), "the page is scrolled");
    settle(40);
    const QRectF rect = sceneRectOf(item);
    check(rect.top() >= 0 && rect.bottom() <= flick->window()->height(), "the item is inside the window after scrolling");
}

// A QML text is whole: not elided and not wider/taller than its box.
void checkNamedTextFits(QObject* root, const char* name)
{
    QObject* text = visualChildNamed(root, QString::fromLatin1(name));
    check(text != nullptr, name);
    checkTextFits(text, name);
}

void collectNamed(QObject* root, const QString& name, QList<QObject*>& out)
{
    auto* item = qobject_cast<QQuickItem*>(root);
    if (item == nullptr) return;
    if (item->objectName() == name) out.append(item);
    for (QQuickItem* child : item->childItems()) collectNamed(child, name, out);
}

// The Overview is hidden while another section is current: show it for a check and give the previous section back afterwards.
struct ShownOverview {
    QQuickWindow* window = nullptr;
    QVariant previousSection;
    explicit ShownOverview(QObject* overviewPage)
    {
        check(overviewPage != nullptr, "the real Overview page exists");
        auto* pageItem = qobject_cast<QQuickItem*>(overviewPage);
        check(pageItem != nullptr && pageItem->window() != nullptr, "the real Overview page is an item in a window");
        window = pageItem->window();
        previousSection = window->property("currentSection");
        check(window->setProperty("currentSection", QStringLiteral("overview")), "the Overview section is shown");
    }
    ~ShownOverview() { window->setProperty("currentSection", previousSection); }
};

// The first look at the REAL page: its cost card is the new one and an empty project is an honest empty estimate.
void checkProjectCostOverviewPage(QObject* overviewPage)
{
    ShownOverview shown(overviewPage);
    check(ai::ui::test_support::waitForCondition("the Overview cost card shows a readable estimate",
              [&] {
                  QObject* coverage = visualChildNamed(overviewPage, QStringLiteral("projectCostCoverage"));
                  return coverage != nullptr && flagOf(coverage, "visible");
              },
              5s),
        "the real page shows the new card with a readable (available) estimate for the selected project");
    check(textNamed(overviewPage, "projectCostTitle") == QStringLiteral("Project cost estimate"), "the real page: the card title");
    check(textNamed(overviewPage, "projectCostValue") == QStringLiteral("—")
            && textNamed(overviewPage, "projectCostStatus") == QStringLiteral("No recorded model executions or memory compactions yet"),
        "the real page: an empty project is a dash and the empty phrase");
    check(!visibleNamed(overviewPage, "projectCostBreakdown") && visibleNamed(overviewPage, "projectCostDetailsToggle"),
        "the real page: no breakdown for nothing, but the explanation is reachable");
    check(!visibleNamed(overviewPage, "projectCostErrorCode"), "the real page: no error");
}

QString providerDataName(QObject* popup)
{
    const QVariant data = popup->property("providerData");
    if (data.metaType() == QMetaType::fromType<QJSValue>()) return data.value<QJSValue>().property(QStringLiteral("name")).toString();
    return data.toMap().value(QStringLiteral("name")).toString();
}

int visibleTextsContaining(QQuickItem* root, const QString& needle)
{
    int count = 0;
    if (root->isVisible() && root->inherits("QQuickText") && root->property("text").toString().contains(needle)) ++count;
    for (QQuickItem* child : root->childItems()) count += visibleTextsContaining(child, needle);
    return count;
}

// The positive data of the seeded record is on the page and its provider popup is open. Then, on the REAL page and the REAL controller:
// a refused read (an invalid range - the production fail() path, no I/O fault needed) must show "unavailable" EVERYWHERE (card, tokens,
// chart, rows, popup) and the old amount nowhere; every new result, a cleared scope, an empty window and hiding the page must close and
// empty the popup; each state is left again by a plain refresh.
void checkRefusalAndDetailsLifecycle(QObject* overviewPage, QQuickWindow* appWindow, const std::string& projectId)
{
    QQuickItem* const windowContent = appWindow->contentItem();
    QObject* const popup = overviewPage->findChild<QObject*>(QStringLiteral("providerDetailsPopup"));
    check(popup != nullptr, "the page names its provider popup");
    QObject* const backend = overviewPage->property("backend").value<QObject*>();
    QObject* const controller = backend != nullptr ? backend->property("projectCost").value<QObject*>() : nullptr;
    check(controller != nullptr, "the page's real project cost controller");
    QObject* const tokens = visualChildNamed(overviewPage, QStringLiteral("tokensMetricCard"));
    check(tokens != nullptr, "the Tokens card");
    const QString project = QString::fromStdString(projectId);

    const auto mapped = [&] {
        for (const QVariant& row : overviewPage->property("providerAnalyticsSnapshot").toList()) {
            if (row.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("pcost-prov")) return true;
        }
        return false;
    };
    const auto nameLabel = [&]() -> QObject* {
        QList<QObject*> names;
        collectNamed(overviewPage, QStringLiteral("providerNameLabel"), names);
        for (QObject* candidate : names) {
            if (candidate->property("text").toString() == QStringLiteral("pcost-prov")) return candidate;
        }
        return nullptr;
    };
    const auto popupOpen = [&] { return popup->property("opened").toBool(); };
    const auto popupClosedAndEmpty = [&] {
        QObject* label = visualChildNamed(windowContent, QStringLiteral("modelNameLabel"));
        return !popupOpen() && providerDataName(popup).isEmpty() && (label == nullptr || !flagOf(label, "visible"));
    };
    const auto refreshPage = [&] {
        check(QMetaObject::invokeMethod(overviewPage, "refreshAnalytics"), "the page refreshes");
        check(ai::ui::test_support::waitForCondition("the seeded provider is mapped again", mapped, 5s), "the seeded provider is mapped again");
        settle(60);
    };
    const auto openDetails = [&] {
        QObject* label = nameLabel();
        check(label != nullptr, "a row of the seeded provider to tap");
        scrollIntoView(label);
        tap(*appWindow, label);
        check(ai::ui::test_support::waitForCondition("the provider details open",
                  [&] { QObject* n = visualChildNamed(windowContent, QStringLiteral("modelNameLabel")); return n != nullptr && flagOf(n, "visible"); }, 5s),
            "a tap on the row opens the details");
        check(providerDataName(popup) == QStringLiteral("pcost-prov") && textNamed(windowContent, "modelCostLabel") == money("$0.0003"),
            "the details show the row's amount");
    };
    const auto refreshRange = [&](const QDateTime& from, const QDateTime& to) {
        check(QMetaObject::invokeMethod(controller, "refreshProjectRange", Q_ARG(QString, project), Q_ARG(QDateTime, from), Q_ARG(QDateTime, to)),
            "the real controller is asked for a range");
        settle(60);
    };

    // precondition: the popup is open on positive data and the Tokens card shows the numbers of the cost result
    check(popupOpen() && providerDataName(popup) == QStringLiteral("pcost-prov"), "the details are open over positive data");
    check(textNamed(tokens, "metricValue") != QStringLiteral("—") && textNamed(tokens, "metricDetail").contains(QStringLiteral(" in · ")),
        "the Tokens card shows the tokens of the result");

    // ---- a refused read (invalid range) ----
    const QDateTime now = QDateTime::currentDateTime();
    refreshRange(now, now.addDays(-1));
    check(textNamed(overviewPage, "projectCostValue") == QStringLiteral("Unavailable"), "refused: the card says Unavailable");
    check(visibleNamed(overviewPage, "projectCostErrorCode")
            && textNamed(overviewPage, "projectCostErrorCode")
                == QStringLiteral("Error code: %1").arg(QString::fromLatin1(ai::application::kProjectCostInvalidDateRangeCode)),
        "refused: the original error code");
    check(textNamed(overviewPage, "projectCostStatus").contains(QStringLiteral("The project cost date range is invalid.")),
        "refused: the original message");
    check(textNamed(tokens, "metricValue") == QStringLiteral("—") && textNamed(tokens, "metricDetail") == QStringLiteral("Token usage could not be read"),
        "refused: Tokens is a dash and says so - not the Run statistics");
    check(visibleNamed(overviewPage, "providerChartEmptyTitle")
            && textNamed(overviewPage, "providerChartEmptyTitle") == QStringLiteral("Cost breakdown unavailable")
            && textNamed(overviewPage, "providerChartEmptyBody").contains(QStringLiteral("could not be read"))
            && !textNamed(overviewPage, "providerChartEmptyBody").contains(QStringLiteral("after the first recorded")),
        "refused: the chart says unavailable, not 'after the first recording'");
    check(overviewPage->property("providerAnalyticsSnapshot").toList().isEmpty() && nameLabel() == nullptr, "refused: no provider rows");
    check(popupClosedAndEmpty(), "refused: the details are closed and emptied");
    check(visibleTextsContaining(windowContent, QStringLiteral("$0.0003")) == 0, "refused: the old amount is nowhere visible");

    // recovery by a plain refresh; a fresh tap opens the new data
    refreshPage();
    check(textNamed(tokens, "metricValue") != QStringLiteral("—") && !visibleNamed(overviewPage, "projectCostErrorCode"),
        "recovered: tokens back, no error");
    openDetails();

    // ---- every new result closes and empties the popup ----
    refreshPage();
    check(popupClosedAndEmpty(), "a new result closes and empties the details");

    // ---- a cleared scope (idle) ----
    openDetails();
    check(QMetaObject::invokeMethod(controller, "clear"), "the controller is cleared");
    settle(60);
    check(popupClosedAndEmpty(), "a cleared scope closes and empties the details");
    check(textNamed(tokens, "metricValue") == QStringLiteral("—") && textNamed(tokens, "metricDetail") == QStringLiteral("No project selected"),
        "idle: Tokens is a dash without a project");
    check(textNamed(overviewPage, "providerChartEmptyTitle") == QStringLiteral("Cost breakdown not available yet")
            && textNamed(overviewPage, "providerChartEmptyBody") == QStringLiteral("Select a project to see its cost by provider."),
        "idle: the chart asks for a project");
    refreshPage();

    // ---- an empty window: available and EMPTY (not unavailable, not the Run statistics) ----
    openDetails();
    refreshRange(QDateTime(QDate(2000, 1, 1), QTime(0, 0)), QDateTime(QDate(2000, 1, 2), QTime(0, 0)));
    check(popupClosedAndEmpty(), "a new scope closes and empties the details");
    check(textNamed(tokens, "metricValue") == QStringLiteral("0") && textNamed(tokens, "metricDetail") == QStringLiteral("No token data yet"),
        "empty window: Tokens is 0, from the same empty result");
    check(textNamed(overviewPage, "providerChartEmptyTitle") == QStringLiteral("Cost breakdown not available yet")
            && textNamed(overviewPage, "providerChartEmptyBody").contains(QStringLiteral("after the first recorded")),
        "empty window: the chart is empty, not unavailable");
    check(!visibleNamed(overviewPage, "projectCostErrorCode") && visibleTextsContaining(windowContent, QStringLiteral("$0.0003")) == 0,
        "empty window: no error and no old amount");
    refreshPage();

    // ---- hiding the page while the details are open ----
    openDetails();
    check(appWindow->setProperty("currentSection", QStringLiteral("runs")), "another section is shown");
    settle(60);
    check(popupClosedAndEmpty(), "hiding the page closes and empties the details");
    check(appWindow->setProperty("currentSection", QStringLiteral("overview")), "the Overview is shown again");
    refreshPage();
}

// POSITIVE data on the REAL page: a stored compaction record travels runtime -> controller -> the page's own mapping -> the chart row ->
// (after a real tap) the provider details. The page may already hold ledger rows of earlier Runs: only the seeded provider is asserted.
void checkProjectCostOverviewPagePositive(QObject* overviewPage, const std::filesystem::path& databasePath, const std::string& projectId)
{
    ShownOverview shown(overviewPage);
    QQuickWindow* const appWindow = shown.window;
    // The smoke test leaves the New Run dialog open after it submits (it opens the dialog again itself before the next Run): a modal
    // dialog swallows every tap, so this one is closed for the check. Any other modal dialog is a failure.
    QList<QPointer<QObject>> closedForTheCheck;
    for (QObject* candidate : appWindow->findChildren<QObject*>()) {
        if (!candidate->inherits("QQuickPopup") || !candidate->property("opened").toBool() || !candidate->property("modal").toBool()) continue;
        std::fprintf(stderr, "[project-cost-qml] closing the open modal dialog '%s' for the check\n", candidate->objectName().toUtf8().constData());
        check(QMetaObject::invokeMethod(candidate, "close"), "an open modal dialog is closed for the check");
        closedForTheCheck.append(candidate);
    }
    for (const QPointer<QObject>& closed : closedForTheCheck) {
        check(ai::ui::test_support::waitForCondition("the modal dialog closes", [&] { return !closed->property("opened").toBool(); }, 5s),
            "the modal dialog closes");
    }
    for (QObject* candidate : appWindow->findChildren<QObject*>()) {
        const bool blocks = candidate->inherits("QQuickPopup") && candidate->property("opened").toBool() && candidate->property("modal").toBool();
        if (blocks) {
            std::fprintf(stderr, "[project-cost-qml] a modal popup is open: class=%s objectName='%s'\n", candidate->metaObject()->className(),
                candidate->objectName().toUtf8().constData());
        }
        check(!blocks, "no modal dialog is open over the page (it would swallow the tap)");
    }

    seedPositiveCompaction(databasePath, projectId);
    check(QMetaObject::invokeMethod(overviewPage, "refreshAnalytics"), "the real page refreshes its estimate");
    const auto mappedProvider = [&]() -> QVariantMap {
        for (const QVariant& row : overviewPage->property("providerAnalyticsSnapshot").toList()) {
            if (row.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("pcost-prov")) return row.toMap();
        }
        return {};
    };
    check(ai::ui::test_support::waitForCondition("the page maps the seeded provider", [&] { return !mappedProvider().isEmpty(); }, 5s),
        "the page's own mapping holds the seeded provider");

    // the card
    check(!visibleNamed(overviewPage, "projectCostErrorCode") && visibleNamed(overviewPage, "projectCostCoverage"),
        "the real page: an available estimate with its coverage caption and no error");
    check(textNamed(overviewPage, "projectCostBreakdown").endsWith(QStringLiteral("· 1 memory compactions")),
        "the real page: the breakdown names the stored compaction");
    settle(60);
    checkNamedTextFits(overviewPage, "projectCostValue");
    checkNamedTextFits(overviewPage, "projectCostBreakdown");

    // the mapping of the page itself (rebuildProviderAnalyticsSnapshot)
    const QVariantMap provider = mappedProvider();
    check(provider.value(QStringLiteral("executionCount")).toInt() == 0 && provider.value(QStringLiteral("compactionCount")).toInt() == 1,
        "the mapped provider keeps executions and compactions apart");
    check(provider.value(QStringLiteral("costLabel")).toString() == money("$0.0003") && !provider.value(QStringLiteral("priceIncomplete")).toBool(),
        "the mapped provider amount is exact");
    const QVariantList models = provider.value(QStringLiteral("models")).toList();
    check(models.size() == 1 && models.constFirst().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("pcost-model")
            && models.constFirst().toMap().value(QStringLiteral("compactionCount")).toInt() == 1,
        "the mapped model row");

    // the chart row as drawn: found by its name, then its own labels
    QList<QObject*> names;
    collectNamed(overviewPage, QStringLiteral("providerNameLabel"), names);
    QObject* nameLabel = nullptr;
    for (QObject* candidate : names) {
        if (candidate->property("text").toString() == QStringLiteral("pcost-prov")) nameLabel = candidate;
    }
    check(nameLabel != nullptr, "the chart draws a row for the seeded provider");
    QQuickItem* row = qobject_cast<QQuickItem*>(nameLabel);
    while (row != nullptr) {
        bool hasTapHandler = false;
        for (QObject* extra : row->children()) hasTapHandler = hasTapHandler || extra->inherits("QQuickTapHandler");
        if (hasTapHandler) break;
        row = row->parentItem();
    }
    check(row != nullptr, "the chart row has a tap handler");
    check(textNamed(row, "providerCallsLabel") == QStringLiteral("0 executions"), "the chart row: ledger executions only");
    check(textNamed(row, "providerTokensLabel") == QStringLiteral("15 tokens"), "the chart row: the tokens of the compaction");
    check(visibleNamed(row, "providerCostLabel") && textNamed(row, "providerCostLabel") == money("$0.0003"),
        "the chart row: the exact amount is visible");
    check(visibleNamed(row, "providerCompactionsLabel") && textNamed(row, "providerCompactionsLabel") == QStringLiteral("1 memory compactions"),
        "the chart row: the compaction count on its own line");
    for (const char* name : {"providerNameLabel", "providerCallsLabel", "providerTokensLabel", "providerCostLabel", "providerCompactionsLabel"})
        checkNamedTextFits(row, name);

    // A REAL tap on the provider row opens the details; the popup lives in the window's overlay.
    QQuickItem* const windowContent = appWindow->contentItem();
    check(visualChildNamed(windowContent, QStringLiteral("modelNameLabel")) == nullptr, "the provider details start closed");
    scrollIntoView(nameLabel);
    tap(*appWindow, nameLabel);
    check(ai::ui::test_support::waitForCondition("the provider details open",
              [&] {
                  QObject* opened = visualChildNamed(windowContent, QStringLiteral("modelNameLabel"));
                  return opened != nullptr && flagOf(opened, "visible");
              },
              5s),
        "a tap on the provider row opens its details");
    check(textNamed(windowContent, "modelNameLabel") == QStringLiteral("pcost-model"), "the details name the model");
    check(textNamed(windowContent, "modelCostLabel") == money("$0.0003"), "the details show the exact amount");
    const QString stats = textNamed(windowContent, "modelStatsLabel");
    check(stats.contains(QStringLiteral("0 executions")) && stats.contains(QStringLiteral("1 compactions"))
            && stats.contains(QStringLiteral("10 in / 5 out")) && !stats.contains(QStringLiteral("ms avg")),
        "the details keep executions and compactions apart and show no latency without executions");
    settle(60);  // the popup has just opened: let its rows lay out before measuring them
    checkNamedTextFits(windowContent, "modelNameLabel");
    checkNamedTextFits(windowContent, "modelCostLabel");
    checkNamedTextFits(windowContent, "modelStatsLabel");
    // The provider popup is OPEN here, over positive data: the refusal, the new results and the lifecycle are checked on the page.
    checkRefusalAndDetailsLifecycle(overviewPage, appWindow, projectId);
    for (QObject* candidate : appWindow->findChildren<QObject*>()) {
        if (candidate->inherits("QQuickPopup") && candidate->objectName().isEmpty()) QMetaObject::invokeMethod(candidate, "close");
    }
    settle();

    // The dialogs the smoke test had left open are open again, as before the check.
    for (const QPointer<QObject>& closed : closedForTheCheck) {
        check(!closed.isNull() && QMetaObject::invokeMethod(closed, "open"), "a dialog closed for the check is opened again");
        check(ai::ui::test_support::waitForCondition("the modal dialog opens again", [&] { return closed->property("opened").toBool(); }, 5s),
            "the modal dialog opens again");
    }
}
