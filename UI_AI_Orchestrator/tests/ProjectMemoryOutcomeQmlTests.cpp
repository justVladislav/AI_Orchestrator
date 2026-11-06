#include "ProjectMemoryOutcomeQmlTests.hpp"

#include <ai/application/conversations/ProjectMemoryCompactionAccounting.hpp>
#include <ai/ui/ProjectMemoryOutcomeProjection.hpp>
#include "../test_support/ProjectMemoryOutcomeFixtures.hpp"

#include <QEventLoop>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QtMath>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace ai::ui::test_support::memory_outcome;

// ---- checks: they stay active under NDEBUG and say what failed -----------------------------------------------------------------------

void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "[project-memory-outcome-qml] FAILED: %s\n", what);
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

void visualChildrenNamed(QObject* root, const QString& name, QList<QObject*>& out)
{
    auto* item = qobject_cast<QQuickItem*>(root);
    if (item == nullptr) return;
    if (item->objectName() == name) out.append(item);
    for (QQuickItem* child : item->childItems()) visualChildrenNamed(child, name, out);
}

bool flagOf(QObject* object, const char* name) { return object->property(name).toBool(); }

// An item that exists and is visible. A missing item is a failure, never "not visible".
bool visibleNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
    if (found == nullptr) std::fprintf(stderr, "[project-memory-outcome-qml] missing item '%s'\n", name);
    check(found != nullptr, "the named item exists");
    return flagOf(found, "visible");
}

QString textNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
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

void invokeNoArgs(QObject* object, const char* method)
{
    check(QMetaObject::invokeMethod(object, method), method);
    settle();
}

void clickNamed(QObject* root, const char* name)
{
    QObject* found = visualChildNamed(root, QString::fromLatin1(name));
    check(found != nullptr && flagOf(found, "visible") && flagOf(found, "enabled"), "a visible, enabled control is clicked");
    check(QMetaObject::invokeMethod(found, "click"), "click");
    settle();
}

// ---- the outcomes under test (built through the real projection) -------------------------------------------------------------------------

ai::application::ProjectMemoryCompactionRecord recordOf(ai::application::ProjectMemoryCompactionStatus status,
    std::optional<bool> priceKnown, std::uint64_t micros, std::uint64_t in, std::uint64_t out)
{
    ai::application::ProjectMemoryCompactionRecord value;
    value.id = "rec";
    value.projectId = "p";
    value.providerId = "prov";
    value.modelId = "model";
    value.sourceSegmentIds = {"a", "b"};
    value.sourceContentHash = "hash";
    value.inputTokens = in;
    value.outputTokens = out;
    value.costMicros = micros;
    value.costPriceKnown = priceKnown;
    value.status = status;
    if (status != ai::application::ProjectMemoryCompactionStatus::Completed) {
        value.errorCode = "project_memory.provider_failed";
        value.errorMessage = "The provider failed";
    }
    value.createdAt = std::chrono::system_clock::now();
    return value;
}

std::string utf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString repeated(const QString& unit, int times)
{
    QString result;
    for (int index = 0; index < times; ++index) result += unit;
    return result;
}

// The worst case: long Unicode with no breakable gap, a code with no gap at all, an incomplete amount that is NOT saved, an isolated
// connection, both native codes, eight diagnostics of the maximal text and the incomplete-diagnostics mark.
QVariantMap longOutcome(const QString& projectId)
{
    using namespace ai::application;
    const QString russian = repeated(QStringLiteral("Запись не удалась "), 12);
    const QString japanese = repeated(QStringLiteral("データベースエラー"), 30);
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::failure(
        {"persistence." + std::string(150, 'x'), utf8(russian + japanese)});
    outcome.responseObserved = true;
    outcome.accounting = CompactionAccounting::NotSaved;
    outcome.observedRecord = recordOf(ProjectMemoryCompactionStatus::Completed, false, 1'234'567'890, 123'456, 7'890);
    outcome.connection = CompactionConnectionState::Quarantined;
    outcome.primaryNative = CompactionNativeDetail {19, 787, std::string(300, 'N')};
    outcome.fallbackNative = CompactionNativeDetail {19, 1555, utf8(japanese)};
    const Stage stages[] = {Stage::PrimaryWrite, Stage::Rollback, Stage::ProofRead, Stage::FallbackWrite, Stage::FallbackRollback,
        Stage::FallbackProofRead, Stage::IdentityCheck, Stage::IdentityCheck};
    for (const Stage stage : stages) {
        outcome.diagnostics.push_back(CompactionDiagnostic {stage, ai::common::Error {"persistence.cause_" + std::string(80, 'c'),
            utf8(russian + japanese + russian)}});
    }
    outcome.diagnosticsIncomplete = true;
    return projectMemoryOutcomeMap(projectId, outcome);
}

QVariantMap savedSuccessOutcome(const QString& projectId)
{
    using namespace ai::application;
    ProjectMemoryCompactionOutcome outcome;
    const auto stored = recordOf(ProjectMemoryCompactionStatus::Completed, true, 250'700, 1'000, 200);
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::ok(stored);
    outcome.responseObserved = true;
    outcome.accounting = CompactionAccounting::Saved;
    outcome.observedRecord = stored;
    outcome.connection = CompactionConnectionState::Healthy;
    return projectMemoryOutcomeMap(projectId, outcome);
}

QVariantMap cancelledOutcome(const QString& projectId)
{
    using namespace ai::application;
    ProjectMemoryCompactionOutcome outcome;
    const auto stored = recordOf(ProjectMemoryCompactionStatus::Cancelled, true, 4'000, 40, 15);
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::ok(stored);
    outcome.responseObserved = true;
    outcome.accounting = CompactionAccounting::Saved;
    outcome.observedRecord = stored;
    outcome.connection = CompactionConnectionState::Healthy;
    return projectMemoryOutcomeMap(projectId, outcome);
}

QVariantMap unsavedFailureOutcome(const QString& projectId, ai::application::CompactionAccounting accounting)
{
    using namespace ai::application;
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::failure({"persistence.transaction_failed", "the write failed"});
    outcome.responseObserved = true;
    outcome.accounting = accounting;
    outcome.observedRecord = recordOf(ProjectMemoryCompactionStatus::Completed, true, 120'000, 40, 15);
    outcome.connection = CompactionConnectionState::Healthy;
    return projectMemoryOutcomeMap(projectId, outcome);
}

// The accounting-fallback row the application REALLY builds for a Completed attempt whose commit failed (its fixed words claim the usage
// is kept), as the CANDIDATE of an outcome whose accounting does not confirm it: NotSaved or Unknown after the fallback write.
// marks: 0 = the fallback write cause survives; 1 = eight other causes pushed it out (diagnosticsIncomplete); 2 = no cause at all (diagnosticsIncomplete).
QVariantMap fallbackCandidateOutcome(const QString& projectId, ai::application::CompactionAccounting verdict, int marks = 0)
{
    using namespace ai::application;
    const ai::common::Error primary {"persistence.transaction_failed", "the commit failed"};
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::failure(primary);
    outcome.responseObserved = true;
    outcome.accounting = verdict;
    outcome.observedRecord = compaction_accounting::makeFailedAccountingRecord(
        recordOf(ProjectMemoryCompactionStatus::Completed, true, 120'000, 40, 15), primary);
    outcome.connection = CompactionConnectionState::Healthy;
    if (marks == 0) outcome.diagnostics.push_back(CompactionDiagnostic {Stage::FallbackWrite, ai::common::Error {"persistence.write_failed", "the fallback insert failed"}});
    if (marks == 1) for (int index = 0; index < 8; ++index) outcome.diagnostics.push_back(CompactionDiagnostic {Stage::Rollback, ai::common::Error {"persistence.rollback_failed", "an early cause"}});
    outcome.diagnosticsIncomplete = marks != 0;
    if (verdict == CompactionAccounting::Saved) outcome.diagnostics.clear();
    return projectMemoryOutcomeMap(projectId, outcome);
}

// A terminal (Cancelled) record whose own insert failed: the cause is the attempt's, not a durable row.
QVariantMap failedTerminalInsertOutcome(const QString& projectId)
{
    using namespace ai::application;
    auto cancelled = recordOf(ProjectMemoryCompactionStatus::Cancelled, std::nullopt, 0, 0, 0);
    cancelled.errorCode = "project_memory.cancelled";
    cancelled.errorMessage = "The compaction was cancelled";
    ProjectMemoryCompactionOutcome outcome;
    outcome.result = ai::common::Result<ProjectMemoryCompactionRecord>::failure({"persistence.transaction_failed", "the write failed"});
    outcome.accounting = CompactionAccounting::NotApplicable;
    outcome.observedRecord = cancelled;
    outcome.connection = CompactionConnectionState::Healthy;
    return projectMemoryOutcomeMap(projectId, outcome);
}

// ---- 1. the block alone: geometry ---------------------------------------------------------------------------------------------------------

void runBlockGeometry(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
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
    property real blockWidth: 620
    property var outcomeValue: null
    width: 700
    height: 3000
    ProjectMemoryOperationOutcome {
        id: block
        x: 10; y: 10
        width: root.blockWidth
        height: implicitHeight
        hostWindow: root.hostRef; colors: root.colorsRef
        outcome: root.outcomeValue
    }
    Rectangle {
        objectName: "nextBlock"
        x: 10
        y: block.y + block.height + 12
        width: root.blockWidth
        height: 40
        color: "#334455"
    }
}
)QML", QUrl(QStringLiteral("qrc:/s2b3-outcome/BlockHarness.qml")));
    if (component.isError()) {
        for (const auto& error : component.errors()) std::fprintf(stderr, "[s2b3-block] %s\n", error.toString().toUtf8().constData());
    }
    check(component.isReady(), "the block harness compiles");

    QQuickWindow window;
    window.resize(720, 3020);
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    std::unique_ptr<QObject> owner(component.createWithInitialProperties(initial));
    check(owner != nullptr, "the block harness is created");
    auto* root = qobject_cast<QQuickItem*>(owner.get());
    root->setParentItem(window.contentItem());
    window.show();
    check(ai::ui::test_support::waitForCondition("the block harness is exposed", [&] { return window.isExposed(); }, 5s), "exposed");

    QObject* block = visualChildNamed(root, QStringLiteral("projectMemoryOperationOutcome"));
    QObject* nextBlock = visualChildNamed(root, QStringLiteral("nextBlock"));
    check(block != nullptr && nextBlock != nullptr, "the block and its neighbour exist");

    // Nothing to show: the block takes no room and is hidden.
    check(!flagOf(block, "visible") && block->property("height").toDouble() < 0.5, "no outcome, no block");

    const char* mainTexts[] = {"projectMemoryOutcomeHeadline", "projectMemoryOutcomePrimary", "projectMemoryOutcomeAccounting",
        "projectMemoryOutcomeCost", "projectMemoryOutcomeTokens", "projectMemoryOutcomeConnection",
        "projectMemoryOutcomeDiagnosticsIncomplete"};

    for (const double width : {620.0, 420.0, 300.0}) {
        check(root->setProperty("blockWidth", width), "width");
        const QVariantMap outcome = longOutcome(QStringLiteral("p"));
        check(root->setProperty("outcomeValue", outcome), "the long outcome");
        settle();
        check(flagOf(block, "visible"), "the long outcome is shown");

        // MAIN texts: whole, inside the block, apart from each other, and the block clear of what follows it.
        const QRectF blockRect = sceneRectOf(block);
        QList<QObject*> shown;
        for (const char* name : mainTexts) {
            QObject* text = visualChildNamed(block, QString::fromLatin1(name));
            check(text != nullptr, name);
            check(flagOf(text, "visible"), name);
            checkTextFits(text, name);
            check(inside(text, blockRect), name);
            shown.append(text);
        }
        for (int a = 0; a < shown.size(); ++a) {
            for (int b = a + 1; b < shown.size(); ++b) check(!overlaps(shown[a], shown[b]), "main texts do not overlap");
        }
        check(blockRect.bottom() <= sceneRectOf(nextBlock).top() - 0.5, "the block is clear of the next block");
        check(!flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeDetails")), "visible"), "details start collapsed");
        // The primary cause, the amount, its completeness and the save state are all in the visible text, not cut.
        check(textNamed(block, "projectMemoryOutcomeCost").contains(QStringLiteral("Incomplete estimate"))
                && textNamed(block, "projectMemoryOutcomeCost").contains(QStringLiteral("$1234.5679"))
                && textNamed(block, "projectMemoryOutcomeCost").contains(QStringLiteral("total unknown"))
                && textNamed(block, "projectMemoryOutcomeCost").endsWith(QStringLiteral("not saved")),
            "the cost line keeps the amount, the incompleteness and 'not saved'");
        check(textNamed(block, "projectMemoryOutcomePrimary") == outcome.value(QStringLiteral("primaryMessage")).toString(),
            "the primary cause is shown whole");
        check(textNamed(block, "projectMemoryOutcomeDiagnosticsIncomplete").contains(QStringLiteral("incomplete")),
            "the incomplete diagnostics are on the surface");
        const double collapsedHeight = blockRect.height();
        saveShot(root, QStringLiteral("s2b3-outcome-block-%1-collapsed").arg(static_cast<int>(width)));

        // DETAILS: one click, eight bounded causes, native codes, codes - none of them pushes the main text out or runs outside.
        clickNamed(block, "projectMemoryOutcomeDetailsToggle");
        check(flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeDetails")), "visible"), "details expand");
        const QRectF expandedRect = sceneRectOf(block);
        check(expandedRect.height() > collapsedHeight + 20, "the block grows with its details");
        check(expandedRect.bottom() <= sceneRectOf(nextBlock).top() - 0.5, "the expanded block is clear of the next block");
        QList<QObject*> diagnostics;
        visualChildrenNamed(block, QStringLiteral("projectMemoryOutcomeDiagnostic"), diagnostics);
        check(diagnostics.size() == 8, "all eight causes are listed");
        for (QObject* line : diagnostics) {
            checkTextFits(line, "diagnostic line");
            check(inside(line, expandedRect), "diagnostic line inside the block");
        }
        for (int a = 0; a < diagnostics.size(); ++a) {
            for (int b = a + 1; b < diagnostics.size(); ++b) check(!overlaps(diagnostics[a], diagnostics[b]), "causes do not overlap");
        }
        for (const char* name : {"projectMemoryOutcomePrimaryCode", "projectMemoryOutcomeAccountingCode", "projectMemoryOutcomePrimaryNative",
                 "projectMemoryOutcomeFallbackNative"}) {
            QObject* text = visualChildNamed(block, QString::fromLatin1(name));
            check(text != nullptr && flagOf(text, "visible"), name);
            checkTextFits(text, name);
            check(inside(text, expandedRect), name);
        }
        for (const char* name : mainTexts) {
            QObject* text = visualChildNamed(block, QString::fromLatin1(name));
            checkTextFits(text, name);   // the main texts did not change under the details
            for (QObject* line : diagnostics) check(!overlaps(text, line), "the main text is clear of the details");
        }
        saveShot(root, QStringLiteral("s2b3-outcome-block-%1-expanded").arg(static_cast<int>(width)));
        clickNamed(block, "projectMemoryOutcomeDetailsToggle");
        check(!flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeDetails")), "visible"), "details collapse");
    }

    // A plain success: no attention texts, the cost line is the saved estimate; a cancellation is not renamed.
    check(root->setProperty("blockWidth", 420.0), "width");
    check(root->setProperty("outcomeValue", savedSuccessOutcome(QStringLiteral("p"))), "a success");
    settle();
    check(textNamed(block, "projectMemoryOutcomeHeadline") == QStringLiteral("Memory compacted."), "success headline");
    check(textNamed(block, "projectMemoryOutcomeCost") == QStringLiteral("Estimated cost $0.2507 — saved"), "saved estimate");
    check(!flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeConnection")), "visible")
            && !flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeDiagnosticsIncomplete")), "visible")
            && !flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomePrimary")), "visible"),
        "nothing to warn about on a clean success");
    saveShot(root, QStringLiteral("s2b3-outcome-block-success"));
    check(root->setProperty("outcomeValue", cancelledOutcome(QStringLiteral("p"))), "a cancellation");
    settle();
    check(textNamed(block, "projectMemoryOutcomeHeadline") == QStringLiteral("Compaction cancelled."), "cancelled stays cancelled");
    // R1: the provenance of a reason, on the REAL component and the real production fallback row. At the narrow width too.
    const auto mainTextsAreFreeOfRecordClaims = [&](const char* what) {
        for (const char* name : {"projectMemoryOutcomeHeadline", "projectMemoryOutcomePrimary", "projectMemoryOutcomeTerminal",
                 "projectMemoryOutcomeAccounting", "projectMemoryOutcomeCost", "projectMemoryOutcomeTokens"}) {
            QObject* text = visualChildNamed(block, QString::fromLatin1(name));
            check(text != nullptr, name);
            const QString shown = flagOf(text, "visible") ? text->property("text").toString().toLower() : QString();
            if (shown.contains(QStringLiteral("kept")) || shown.contains(QStringLiteral("failed attempt")) || shown.contains(QStringLiteral("recorded reason")))
                std::fprintf(stderr, "[s2b3f] %s: '%s' claims a record: %s\n", what, name, shown.toUtf8().constData());
            check(!shown.contains(QStringLiteral("kept")) && !shown.contains(QStringLiteral("failed attempt"))
                    && !shown.contains(QStringLiteral("recorded reason")), "the main screen claims no unconfirmed record");
        }
    };
    for (const double width : {420.0, 300.0}) {
        check(root->setProperty("blockWidth", width), "width");
        const int shotWidth = static_cast<int>(width);
        // every combination of verdict x surviving marks: the main screen must not depend on which diagnostic marks were lost
        for (const int marks : {0, 1, 2})
        for (const auto verdict : {ai::application::CompactionAccounting::NotSaved, ai::application::CompactionAccounting::Unknown}) {
            const bool notSaved = verdict == ai::application::CompactionAccounting::NotSaved;
            check(root->setProperty("outcomeValue", fallbackCandidateOutcome(QStringLiteral("p"), verdict, marks)), "an unconfirmed fallback candidate");
            settle();
            const char* what = notSaved ? "fallback NotSaved" : "fallback Unknown after the fallback write";
            mainTextsAreFreeOfRecordClaims(what);
            check(flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeTerminal")), "visible")
                    && textNamed(block, "projectMemoryOutcomeTerminal") == QStringLiteral("The compaction attempt failed; its record is not confirmed saved."),
                "the unconfirmed fallback is a neutral attempt status on the main screen, whatever marks survived");
            if (marks == 1) check(textNamed(block, "projectMemoryOutcomeDiagnosticsIncomplete").contains(QStringLiteral("incomplete")), "the incomplete diagnostics are shown");
            check(textNamed(block, "projectMemoryOutcomePrimary") == QStringLiteral("the commit failed"), "the real primary error is on the main screen");
            check(textNamed(block, "projectMemoryOutcomeCost").endsWith(QString::fromLatin1(notSaved ? "not saved" : "saving not confirmed"))
                    && textNamed(block, "projectMemoryOutcomeCost").contains(QStringLiteral("$0.1200")), "the amount stays, qualified");
            saveShot(root, QStringLiteral("s2b3g-outcome-block-fallback-%1-marks%2-%3-collapsed").arg(notSaved ? "not-saved" : "unknown").arg(marks).arg(shotWidth));
            clickNamed(block, "projectMemoryOutcomeDetailsToggle");
            QObject* detail = visualChildNamed(block, QStringLiteral("projectMemoryOutcomeTerminalCode"));
            check(detail != nullptr && flagOf(detail, "visible"), "the candidate is shown in the details");
            check(textNamed(block, "projectMemoryOutcomeTerminalCode").contains(QStringLiteral("not confirmed saved"))
                    && textNamed(block, "projectMemoryOutcomeTerminalCode").contains(QStringLiteral("kept as a failed attempt")),
                "...labelled as unconfirmed metadata, never as recorded");
            checkTextFits(detail, "candidate detail");
            check(inside(detail, sceneRectOf(block)) && sceneRectOf(block).bottom() <= sceneRectOf(nextBlock).top() - 0.5, "inside the block, clear of the next block");
            QList<QObject*> causes;
            visualChildrenNamed(block, QStringLiteral("projectMemoryOutcomeDiagnostic"), causes);
            check(causes.size() == (marks == 0 ? 1 : marks == 1 ? 8 : 0), "the causes are kept exactly as the application gave them");
            for (QObject* cause : causes) { checkTextFits(cause, "cause"); check(inside(cause, sceneRectOf(block)), "cause inside the block"); }
            saveShot(root, QStringLiteral("s2b3g-outcome-block-fallback-%1-marks%2-%3-details").arg(notSaved ? "not-saved" : "unknown").arg(marks).arg(shotWidth));
            clickNamed(block, "projectMemoryOutcomeDetailsToggle");
        }
        // a failed terminal insert: the cause is kept on the main screen as the ATTEMPT's cause
        check(root->setProperty("outcomeValue", failedTerminalInsertOutcome(QStringLiteral("p"))), "a failed terminal insert");
        settle();
        QObject* reason = visualChildNamed(block, QStringLiteral("projectMemoryOutcomeTerminal"));
        check(reason != nullptr && flagOf(reason, "visible"), "the cause of the original record is not lost");
        check(textNamed(block, "projectMemoryOutcomeTerminal") == QStringLiteral("The compaction attempt was cancelled; its record is not confirmed saved."),
            "the main screen says what the attempt did, not a recorded row");
        checkTextFits(reason, "attempt reason");
        clickNamed(block, "projectMemoryOutcomeDetailsToggle");
        check(flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeTerminalCode")), "visible")
                && textNamed(block, "projectMemoryOutcomeTerminalCode").contains(QStringLiteral("The compaction was cancelled"))
                && textNamed(block, "projectMemoryOutcomeTerminalCode").contains(QStringLiteral("project_memory.cancelled"))
                && textNamed(block, "projectMemoryOutcomeTerminalCode").contains(QStringLiteral("not confirmed saved")),
            "the original cause and code are whole in the details");
        clickNamed(block, "projectMemoryOutcomeDetailsToggle");
        saveShot(root, QStringLiteral("s2b3f-outcome-block-terminal-insert-failed-%1").arg(shotWidth));
    }
    // controls: a SAVED fallback and returned records keep their exact meaning ("Recorded reason")
    check(root->setProperty("blockWidth", 420.0), "width");
    check(root->setProperty("outcomeValue", fallbackCandidateOutcome(QStringLiteral("p"), ai::application::CompactionAccounting::Saved)), "a saved fallback");
    settle();
    check(flagOf(visualChildNamed(block, QStringLiteral("projectMemoryOutcomeTerminal")), "visible")
            && textNamed(block, "projectMemoryOutcomeTerminal").startsWith(QStringLiteral("Recorded reason:"))
            && textNamed(block, "projectMemoryOutcomeTerminal").contains(QStringLiteral("kept as a failed attempt")), "a saved fallback is a recorded reason");
    check(textNamed(block, "projectMemoryOutcomeCost").endsWith(QStringLiteral("saved")) && !textNamed(block, "projectMemoryOutcomeCost").endsWith(QStringLiteral("not saved")),
        "...and its amount is saved");
    saveShot(root, QStringLiteral("s2b3f-outcome-block-fallback-saved"));
    check(root->setProperty("outcomeValue", cancelledOutcome(QStringLiteral("p"))), "a returned cancellation");
    settle();
    check(textNamed(block, "projectMemoryOutcomeHeadline") == QStringLiteral("Compaction cancelled."), "returned Cancelled keeps its state");
    check(root->setProperty("outcomeValue", QVariant()), "cleared");
    settle();
    check(!flagOf(block, "visible"), "an empty value hides the block again");
}

// ---- 2. the panel over a fake transport: ownership and reset --------------------------------------------------------------------------------

void runPanelOwnership(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
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
    property real panelWidth: 560
    property string projectIdValue: "project-a"
    property var backendObject: ({ projectMemory: fakeMemory })
    property bool compactReturns: true
    property int compactCalls: 0
    property bool panelActive: true
    width: 700
    height: 2600
    function emitOutcome(projectId, outcome) { fakeMemory.compactionOutcomeReported(projectId, outcome) }
    function emitError(code, message) { fakeMemory.errorOccurred(code, message) }
    QtObject {
        id: fakeMemory
        property bool compacting: false
        property string compactingProjectId: ""
        signal segmentsChanged(string projectId)
        signal compactionHistoryChanged(string projectId)
        signal compactionOutcomeReported(string projectId, var outcome)
        signal errorOccurred(string code, string message)
        function segments(projectId) { return [] }
        function providerChoices() {
            return [{ providerId: "p", providerName: "P", modelId: "m", modelName: "Model", available: true, statusText: "Ready" }]
        }
        function stopCompaction() {}
        function compact(projectId, providerId, modelId) { root.compactCalls++; return root.compactReturns }
        function compactionHistory(projectId) { return [] }
        function compactionHistorySnapshot(projectId) { return { state: "available", rows: [], errorCode: "", errorMessage: "" } }
    }
    Column {
        x: 10; y: 10
        width: root.panelWidth
        spacing: 12
        Repeater {
            model: root.panelActive ? 1 : 0
            delegate: ProjectMemoryPanel {
                objectName: "projectMemoryPanel"
                width: root.panelWidth
                height: implicitHeight
                hostWindow: root.hostRef; colors: root.colorsRef
                backend: root.backendObject
                projectId: root.projectIdValue
            }
        }
        Rectangle {
            objectName: "nextBlock"
            width: root.panelWidth
            height: 40
            color: "#334455"
        }
    }
}
)QML", QUrl(QStringLiteral("qrc:/s2b3-outcome/PanelHarness.qml")));
    if (component.isError()) {
        for (const auto& error : component.errors()) std::fprintf(stderr, "[s2b3-panel] %s\n", error.toString().toUtf8().constData());
    }
    check(component.isReady(), "the panel harness compiles");

    QQuickWindow window;
    window.resize(720, 2620);
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    std::unique_ptr<QObject> owner(component.createWithInitialProperties(initial));
    check(owner != nullptr, "the panel harness is created");
    auto* root = qobject_cast<QQuickItem*>(owner.get());
    root->setParentItem(window.contentItem());
    window.show();
    check(ai::ui::test_support::waitForCondition("the panel harness is exposed", [&] { return window.isExposed(); }, 5s), "exposed");
    settle();

    const auto panel = [&]() -> QObject* { return visualChildNamed(root, QStringLiteral("projectMemoryPanel")); };
    const auto emitOutcome = [&](const QString& projectId, const QVariantMap& outcome) {
        check(QMetaObject::invokeMethod(root, "emitOutcome", Q_ARG(QVariant, QVariant(projectId)), Q_ARG(QVariant, QVariant(outcome))), "emit");
        settle();
    };
    const auto shown = [&] { return panel() != nullptr && visibleNamed(panel(), "projectMemoryOperationOutcome"); };
    const auto setRoot = [&](const char* name, const QVariant& value) { check(root->setProperty(name, value), name); settle(); };
    const auto requestCompact = [&] {
        check(panel()->setProperty("selectedChoiceIndex", 0), "choice");
        invokeNoArgs(panel(), "requestCompact");
    };

    check(panel() != nullptr && !shown(), "a fresh panel shows no result");

    // A result for the selected project is shown, with the words the projection gave.
    emitOutcome(QStringLiteral("project-a"), unsavedFailureOutcome(QStringLiteral("project-a"), ai::application::CompactionAccounting::NotSaved));
    check(shown(), "the result of the selected project is shown");
    check(textNamed(panel(), "projectMemoryOutcomePrimary") == QStringLiteral("the write failed"), "the primary cause");
    check(textNamed(panel(), "projectMemoryOutcomeCost") == QStringLiteral("Estimated cost $0.1200 — not saved"), "the unsaved estimate stays visible");

    // A DELAYED result of another project is ignored: it neither replaces nor clears what is shown.
    emitOutcome(QStringLiteral("project-b"), savedSuccessOutcome(QStringLiteral("project-b")));
    check(textNamed(panel(), "projectMemoryOutcomeHeadline") == QStringLiteral("Compaction failed.")
            && textNamed(panel(), "projectMemoryOutcomeCost").endsWith(QStringLiteral("not saved")),
        "another project's result is not shown here");

    // A refusal (the call was not accepted) does NOT erase an unsaved estimate; Stop does not clear it in advance either.
    setRoot("compactReturns", false);
    requestCompact();
    check(root->property("compactCalls").toInt() == 1, "the call was made");
    check(shown() && textNamed(panel(), "projectMemoryOutcomeCost").endsWith(QStringLiteral("not saved")), "a refused call keeps the result");

    // An ACCEPTED call resets its own previous result.
    setRoot("compactReturns", true);
    requestCompact();
    check(root->property("compactCalls").toInt() == 2, "the call was made again");
    check(!shown(), "an accepted operation drops the previous result");

    // Accepted cancellation, and a late real save, are shown as they are - not renamed.
    emitOutcome(QStringLiteral("project-a"), cancelledOutcome(QStringLiteral("project-a")));
    check(textNamed(panel(), "projectMemoryOutcomeHeadline") == QStringLiteral("Compaction cancelled."), "cancelled stays cancelled");
    emitOutcome(QStringLiteral("project-a"), savedSuccessOutcome(QStringLiteral("project-a")));
    check(textNamed(panel(), "projectMemoryOutcomeHeadline") == QStringLiteral("Memory compacted.")
            && textNamed(panel(), "projectMemoryOutcomeAccounting").contains(QStringLiteral("saved")),
        "a real save is shown as saved");

    // The Unknown verdict is worded as unknown, never as 'not saved'.
    emitOutcome(QStringLiteral("project-a"), unsavedFailureOutcome(QStringLiteral("project-a"), ai::application::CompactionAccounting::Unknown));
    check(textNamed(panel(), "projectMemoryOutcomeCost").endsWith(QStringLiteral("saving not confirmed"))
            && !textNamed(panel(), "projectMemoryOutcomeAccounting").contains(QStringLiteral("were not saved")),
        "unknown is not rendered as not saved");

    // A project change drops the result AND every error slot BEFORE the reads; an empty project clears the view.
    check(QMetaObject::invokeMethod(root, "emitError", Q_ARG(QVariant, QVariant(QStringLiteral("project_memory.busy"))),
              Q_ARG(QVariant, QVariant(QStringLiteral("Wait for the current compaction")))), "an unrelated error");
    settle();
    check(panel()->property("errorText").toString() == QStringLiteral("Wait for the current compaction"), "the unrelated error is shown");
    setRoot("projectIdValue", QStringLiteral("project-b"));
    check(!shown() && panel()->property("errorText").toString().isEmpty(), "another project inherits neither the result nor the error");
    emitOutcome(QStringLiteral("project-b"), savedSuccessOutcome(QStringLiteral("project-b")));
    check(shown(), "the result of the newly selected project is shown");
    setRoot("projectIdValue", QStringLiteral(""));
    check(!shown(), "an empty project clears the view");
    emitOutcome(QStringLiteral(""), savedSuccessOutcome(QStringLiteral("")));
    check(!shown(), "a result without a project is never shown");
    setRoot("projectIdValue", QStringLiteral("project-a"));
    check(!shown(), "coming back to a project does not resurrect an old result");

    // The panel can disappear and appear: a signal that arrives while it does not exist is harmless (no dangling connection).
    emitOutcome(QStringLiteral("project-a"), savedSuccessOutcome(QStringLiteral("project-a")));
    check(shown(), "shown before the panel disappears");
    setRoot("panelActive", false);
    check(panel() == nullptr, "the panel is gone");
    emitOutcome(QStringLiteral("project-a"), savedSuccessOutcome(QStringLiteral("project-a")));
    check(QMetaObject::invokeMethod(root, "emitError", Q_ARG(QVariant, QVariant(QStringLiteral("x"))), Q_ARG(QVariant, QVariant(QStringLiteral("y")))), "a late error");
    settle();
    setRoot("panelActive", true);
    check(panel() != nullptr && !shown(), "a new panel starts empty and does not inherit the old result");
    emitOutcome(QStringLiteral("project-a"), savedSuccessOutcome(QStringLiteral("project-a")));
    check(shown(), "and it is connected again");
}

// ---- 3. the panel: geometry inside the page ----------------------------------------------------------------------------------------------------

void runPanelGeometry(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
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
    property real panelWidth: 620
    property var backendObject: ({ projectMemory: fakeMemory })
    property string projectIdValue: "project-a"   // a binding, so that backend is in place before projectId triggers the first read
    width: 700
    height: 3000
    QtObject {
        id: fakeMemory
        property bool compacting: false
        property string compactingProjectId: ""
        signal segmentsChanged(string projectId)
        signal compactionHistoryChanged(string projectId)
        signal compactionOutcomeReported(string projectId, var outcome)
        signal errorOccurred(string code, string message)
        function segments(projectId) { return [] }
        function providerChoices() { return [] }
        function stopCompaction() {}
        function compact(projectId, providerId, modelId) { return false }
        function compactionHistory(projectId) { return [] }
        function compactionHistorySnapshot(projectId) {
            return { state: "available", rows: [
                { recordId: "r1", status: "failed", modelId: "model-1", createdAt: "2026-11-06 10:00", errorMessage: "A stored reason",
                  inputTokens: 40, outputTokens: 15, sourceSegmentIds: ["a", "b"], priceState: "known", costText: "$0.1200", priceIncomplete: false }
            ], errorCode: "", errorMessage: "" }
        }
    }
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
            backend: root.backendObject
            projectId: root.projectIdValue
        }
        Rectangle {
            objectName: "nextBlock"
            width: root.panelWidth
            height: 40
            color: "#334455"
        }
    }
}
)QML", QUrl(QStringLiteral("qrc:/s2b3-outcome/PanelGeometryHarness.qml")));
    if (component.isError()) {
        for (const auto& error : component.errors()) std::fprintf(stderr, "[s2b3-geometry] %s\n", error.toString().toUtf8().constData());
    }
    check(component.isReady(), "the geometry harness compiles");

    QQuickWindow window;
    window.resize(720, 3020);
    QVariantMap initial;
    initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
    initial.insert(QStringLiteral("colorsRef"), colors);
    std::unique_ptr<QObject> owner(component.createWithInitialProperties(initial));
    check(owner != nullptr, "the geometry harness is created");
    auto* root = qobject_cast<QQuickItem*>(owner.get());
    root->setParentItem(window.contentItem());
    window.show();
    check(ai::ui::test_support::waitForCondition("the geometry harness is exposed", [&] { return window.isExposed(); }, 5s), "exposed");
    settle();

    QObject* panel = visualChildNamed(root, QStringLiteral("projectMemoryPanel"));
    QObject* nextBlock = visualChildNamed(root, QStringLiteral("nextBlock"));
    check(panel != nullptr && nextBlock != nullptr, "panel and neighbour exist");

    for (const double width : {620.0, 420.0, 300.0}) {
        check(root->setProperty("panelWidth", width), "width");
        check(panel->setProperty("operationOutcome", longOutcome(QStringLiteral("project-a"))), "the long outcome");
        settle();
        QObject* block = visualChildNamed(panel, QStringLiteral("projectMemoryOperationOutcome"));
        check(block != nullptr && flagOf(block, "visible"), "the block is shown in the panel");
        for (const bool expanded : {false, true}) {
            if (expanded) clickNamed(block, "projectMemoryOutcomeDetailsToggle");
            const QRectF panelRect = sceneRectOf(panel);
            const QRectF blockRect = sceneRectOf(block);
            check(blockRect.left() >= panelRect.left() - 0.5 && blockRect.right() <= panelRect.right() + 0.5
                    && blockRect.bottom() <= panelRect.bottom() + 0.5, "the block is inside the panel");
            check(panelRect.bottom() <= sceneRectOf(nextBlock).top() - 0.5, "the panel is clear of the next block");
            for (const char* name : {"projectMemoryOutcomeHeadline", "projectMemoryOutcomePrimary", "projectMemoryOutcomeAccounting",
                     "projectMemoryOutcomeCost", "projectMemoryOutcomeConnection", "projectMemoryOutcomeDiagnosticsIncomplete"}) {
                QObject* text = visualChildNamed(block, QString::fromLatin1(name));
                checkTextFits(text, name);
                check(inside(text, blockRect), name);
            }
            // the history row below the block is still whole and does not run under it
            QList<QObject*> costLines;
            visualChildrenNamed(panel, QStringLiteral("compactionCostLine"), costLines);
            check(costLines.size() == 1, "the S1 history row is still shown");
            checkTextFits(costLines.front(), "history cost line");
            check(!overlaps(costLines.front(), block), "the history row is clear of the block");
            saveShot(root, QStringLiteral("s2b3-outcome-panel-%1-%2").arg(static_cast<int>(width)).arg(expanded ? "expanded" : "collapsed"));
        }
        clickNamed(block, "projectMemoryOutcomeDetailsToggle");
    }
    owner.reset();
}

// ---- 4. the decisive chain on the REAL controller -------------------------------------------------------------------------------------------

void runDecisiveChain(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    // variant 0: NotSaved (commit and fallback insert both fail); 1: Unknown right after the commit (no fallback); 2: Unknown AFTER a fallback attempt
    for (const int variant : {0, 1, 2}) {
        const bool unknown = variant != 0;
        Rig rig;
        QQmlEngine::setObjectOwnership(rig.controller.get(), QQmlEngine::CppOwnership);
        // After the service read its sources the storage stops answering reads: segments and history fail with the SAME code and message
        // as the operation's own failure - ownership has to be the operation, never the text.
        rig.client.whileInFlight = [&] {
            rig.memory.listFails = true;
            rig.repository.listFails = true;
        };
        const auto& same = sharedReadFailure();
        if (variant == 1) {
            rig.repository.commitScript.push_back(failedWrite(S::Unknown, C::Healthy, same.code.c_str(), same.message.c_str()));
        } else if (variant == 2) {
            rig.repository.commitScript.push_back(failedWrite(S::NotCommitted, C::Healthy, same.code.c_str(), same.message.c_str()));
            rig.repository.insertScript.push_back(failedWrite(S::Unknown, C::Healthy, same.code.c_str(), same.message.c_str()));
        } else {
            rig.repository.commitScript.push_back(failedWrite(S::NotCommitted, C::Healthy, same.code.c_str(), same.message.c_str()));
            rig.repository.insertScript.push_back(failedWrite(S::NotCommitted, C::Healthy, same.code.c_str(), same.message.c_str()));
        }

        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick
import QtQuick.Layouts
import UI_AI_Orchestrator
Item {
    id: root
    property var hostRef
    property var colorsRef
    property var controllerRef
    property string projectIdValue: "cp1"
    property var backendObject: ({ projectMemory: root.controllerRef })
    width: 700
    height: 2200
    Column {
        x: 10; y: 10
        width: 560
        spacing: 12
        ProjectMemoryPanel {
            id: panelItem
            objectName: "projectMemoryPanel"
            width: 560
            height: implicitHeight
            hostWindow: root.hostRef; colors: root.colorsRef
            backend: root.backendObject
            projectId: root.projectIdValue
        }
        Rectangle { objectName: "nextBlock"; width: 560; height: 40; color: "#334455" }
    }
}
)QML", QUrl(QStringLiteral("qrc:/s2b3-outcome/ChainHarness.qml")));
        check(component.isReady(), "the chain harness compiles");
        QQuickWindow window;
        window.resize(720, 2220);
        QVariantMap initial;
        initial.insert(QStringLiteral("hostRef"), QVariant::fromValue(hostWindow));
        initial.insert(QStringLiteral("colorsRef"), colors);
        initial.insert(QStringLiteral("controllerRef"), QVariant::fromValue(static_cast<QObject*>(rig.controller.get())));
        std::unique_ptr<QObject> owner(component.createWithInitialProperties(initial));
        check(owner != nullptr, "the chain harness is created");
        auto* root = qobject_cast<QQuickItem*>(owner.get());
        root->setParentItem(window.contentItem());
        window.show();
        check(ai::ui::test_support::waitForCondition("the chain harness is exposed", [&] { return window.isExposed(); }, 5s), "exposed");
        settle();

        QObject* panel = visualChildNamed(root, QStringLiteral("projectMemoryPanel"));
        check(panel != nullptr, "the panel exists");
        check(panel->property("segmentsErrorText").toString().isEmpty() && panel->property("historyErrorText").toString().isEmpty(),
            "the initial reads succeeded");
        check(!visibleNamed(panel, "projectMemoryOperationOutcome"), "no result yet");

        check(panel->setProperty("selectedChoiceIndex", 0), "the only choice is selected");
        invokeNoArgs(panel, "requestCompact");
        check(ai::ui::test_support::waitForCondition("the operation result reaches the panel",
                  [&] { return !rig.controller->compacting() && visibleNamed(panel, "projectMemoryOperationOutcome"); }, 10s),
            "the result is shown");
        settle();

        // The operation's own result: the primary cause, the verdict and the received amount are on screen.
        check(textNamed(panel, "projectMemoryOutcomePrimary") == QString::fromStdString(same.message), "the primary cause is the operation's");
        const QString verdict = textNamed(panel, "projectMemoryOutcomeAccounting");
        const QString cost = textNamed(panel, "projectMemoryOutcomeCost");
        check(cost.startsWith(QStringLiteral("Estimated cost $")), "the received estimate is shown");
        if (unknown) {
            check(verdict.contains(QStringLiteral("not confirmed")) && cost.endsWith(QStringLiteral("saving not confirmed")), "unknown, worded as unknown");
            check(rig.repository.inserts == (variant == 2 ? 1 : 0), variant == 2 ? "one fallback attempt, then unknown" : "no fallback after an unknown commit");
        } else {
            check(verdict.contains(QStringLiteral("not saved")) && cost.endsWith(QStringLiteral("not saved")), "not saved, worded as not saved");
        }
        // R1: nothing on the main screen may claim that an UNCONFIRMED record exists. The fallback row's fixed words (it says the usage is
        // kept) are shown, if at all, only as labelled metadata in the details.
        for (const char* name : {"projectMemoryOutcomeHeadline", "projectMemoryOutcomePrimary", "projectMemoryOutcomeTerminal",
                 "projectMemoryOutcomeAccounting", "projectMemoryOutcomeCost", "projectMemoryOutcomeTokens"}) {
            QObject* text = visualChildNamed(panel, QString::fromLatin1(name));
            check(text != nullptr, name);
            const QString shown = flagOf(text, "visible") ? text->property("text").toString().toLower() : QString();
            check(!shown.contains(QStringLiteral("kept")) && !shown.contains(QStringLiteral("failed attempt"))
                    && !shown.contains(QStringLiteral("recorded reason")), "no main text claims an unconfirmed record");
        }
        if (variant != 1) {
            check(visibleNamed(panel, "projectMemoryOutcomeTerminal")
                    && textNamed(panel, "projectMemoryOutcomeTerminal") == QStringLiteral("The compaction attempt failed; its record is not confirmed saved."),
                "the unconfirmed fallback is a neutral attempt status on the main screen");
            check(rig.repository.insertedRecords.size() == 1 && rig.repository.insertedRecords.front().errorMessage.find("kept as a failed attempt") != std::string::npos,
                "the candidate really was the production fallback row");
            clickNamed(panel, "projectMemoryOutcomeDetailsToggle");
            check(visibleNamed(panel, "projectMemoryOutcomeTerminalCode"), "the candidate is in the details");
            const QString detail = textNamed(panel, "projectMemoryOutcomeTerminalCode");
            check(detail.contains(QStringLiteral("not confirmed saved")) && detail.contains(QStringLiteral("kept as a failed attempt")),
                "...labelled as an unconfirmed attempt");
            saveShot(root, QStringLiteral("s2b3f-outcome-chain-%1-details").arg(variant == 0 ? "not-saved" : "unknown-after-fallback"));
            clickNamed(panel, "projectMemoryOutcomeDetailsToggle");
        }
        // BOTH reads failed, each with its own slot, though code and message equal the operation's; the generic slot is empty.
        check(panel->property("segmentsErrorText").toString() == QStringLiteral("storage boom"), "the segments read error");
        check(panel->property("historyErrorText").toString() == QStringLiteral("storage boom"), "the history read error");
        check(panel->property("errorText").toString().isEmpty(), "no read error took the generic slot");
        check(visibleNamed(panel, "projectMemorySegmentsError") && visibleNamed(panel, "projectMemoryHistoryError")
                && visibleNamed(panel, "projectMemoryHistoryUnavailable"), "the two read errors are visible apart from the result");
        check(visibleNamed(panel, "projectMemoryOperationOutcome"), "the result survives both read failures");
        saveShot(root, QStringLiteral("s2b3f-outcome-chain-%1").arg(variant == 0 ? "not-saved" : variant == 1 ? "unknown" : "unknown-after-fallback"));

        // Recovery of ONE read leaves the other error and the result alone.
        rig.memory.listFails = false;
        invokeNoArgs(panel, "refreshSegments");
        check(panel->property("segmentsErrorText").toString().isEmpty() && !visibleNamed(panel, "projectMemorySegmentsError"), "the segments error left");
        check(panel->property("historyErrorText").toString() == QStringLiteral("storage boom") && visibleNamed(panel, "projectMemoryHistoryError"),
            "the history error stayed");
        check(visibleNamed(panel, "projectMemoryOperationOutcome") && textNamed(panel, "projectMemoryOutcomeCost") == cost, "the result stayed");
        rig.repository.listFails = false;
        invokeNoArgs(panel, "refreshHistory");
        check(panel->property("historyErrorText").toString().isEmpty() && !visibleNamed(panel, "projectMemoryHistoryUnavailable"), "the history error left");
        check(visibleNamed(panel, "projectMemoryOperationOutcome") && textNamed(panel, "projectMemoryOutcomeCost") == cost,
            "the result is still there after both recoveries");

        check(rig.client.observedCalls == 1 && rig.client.sendCalls == 0, "exactly one provider request for the whole scenario");
        owner.reset();
    }
}

} // namespace

void runProjectMemoryOutcomeQmlTests(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors)
{
    runBlockGeometry(engine, hostWindow, colors);
    std::fprintf(stderr, "[checkpoint] S2b-3: the outcome block geometry verified\n");
    runPanelOwnership(engine, hostWindow, colors);
    std::fprintf(stderr, "[checkpoint] S2b-3: the panel ownership and reset verified\n");
    runPanelGeometry(engine, hostWindow, colors);
    std::fprintf(stderr, "[checkpoint] S2b-3: the panel geometry verified\n");
    runDecisiveChain(engine, hostWindow, colors);
    std::fprintf(stderr, "[checkpoint] S2b-3: the decisive chain on the real controller verified\n");
}
