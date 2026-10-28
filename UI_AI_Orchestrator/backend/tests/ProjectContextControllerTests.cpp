#include "../../test_support/QtTestSupport.hpp"

#include <ai/application/projects/ProjectService.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/ChatController.hpp>
#include <ai/ui/ProjectContextController.hpp>
#include <ai/ui/RunListModel.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// One isolated temp directory per test: the project root and the database. Tests never touch the real
// per-user database.
struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("aio-pcctl-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";

    TempWorkspace() { fs::create_directories(projectPath / "src"); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void write(const fs::path& relative, const std::string& content) const
    {
        const auto target = projectPath / relative;
        fs::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << content;
    }
    [[nodiscard]] QString projectPathQt() const { return QString::fromStdWString(projectPath.wstring()); }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }
};

struct FinishedEvents {
    int count {0};
    quint64 lastId {0};
    bool lastSuccess {false};
};

void connectFinished(ProjectContextController& controller, FinishedEvents& events)
{
    QObject::connect(&controller, &ProjectContextController::previewFinished,
        [&events](quint64 id, bool success) {
            ++events.count;
            events.lastId = id;
            events.lastSuccess = success;
        });
}

void waitNotBusy(ProjectContextController& controller)
{
    require(ai::ui::test_support::waitForCondition("preview finished", [&] { return !controller.busy(); }, 30s),
        "A preview did not finish in time");
}

std::unique_ptr<BackendBridge> openBridge(const TempWorkspace& workspace)
{
    // The composition root is large; keep it off the test thread's stack like the other adapter tests.
    auto bridge = std::make_unique<BackendBridge>(workspace.databasePathQt());
    require(bridge->storageAvailable(), "Storage must be available");
    return bridge;
}

QString projectRootOf(const TempWorkspace& workspace) { return workspace.projectPathQt(); }

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

// ---------------------------------------------------------------------------------------------------------

void testPreviewPublishesMetadataOnlyAndNeverWrites()
{
    TempWorkspace workspace;
    workspace.write("src/a.qml", "Item {}\n");
    workspace.write("docs/notes.md", "Secret-looking text that must never reach QML\n");
    auto bridge = openBridge(workspace);
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("Preview project"), QStringLiteral("Curated intent"), workspace.projectPathQt());
    require(!projectId.isEmpty(), "Project was not created");

    ProjectContextController& controller = bridge->projectContextController();
    require(bridge->projectContext() == &controller, "BackendBridge must expose the controller to QML");
    require(!controller.busy() && !controller.hasSnapshot() && controller.sections().isEmpty()
            && controller.errorCode().isEmpty(),
        "A fresh controller holds nothing");
    FinishedEvents events;
    connectFinished(controller, events);

    require(controller.preview(QString(), QString(), false) == 0, "An empty project id must be refused");
    require(!controller.busy() && events.count == 0, "A refused call changes nothing");

    const quint64 id = controller.preview(projectId, QString(), false);
    require(id != 0 && controller.busy(), "An accepted preview is running");
    require(controller.preview(projectId, QString(), false) == 0, "Only one build runs at a time");
    waitNotBusy(controller);
    require(events.count == 1 && events.lastId == id && events.lastSuccess, "previewFinished announces the result once");

    require(controller.hasSnapshot() && controller.errorCode().isEmpty() && controller.projectId() == projectId
            && !controller.includesProjectMemory(),
        "A successful preview publishes a snapshot");
    require(controller.policyId() == QStringLiteral("project_context_budget_v1") && controller.totalTokens() == 24000
            && controller.usedTokens() > 0 && controller.usedTokens() <= controller.totalTokens()
            && controller.fingerprint().size() == 64 && controller.shortFingerprint().size() == 12
            && controller.fingerprint().startsWith(controller.shortFingerprint())
            && controller.workspaceFingerprint().size() == 12 && controller.sourceCount() >= 3,
        "The budget and the identity of the snapshot are published");

    QStringList categories;
    for (const QVariant& row : controller.sections()) {
        const QVariantMap map = row.toMap();
        categories.append(map.value(QStringLiteral("category")).toString());
        QStringList keys = map.keys();
        keys.sort();
        require(keys == QStringList({QStringLiteral("authority"), QStringLiteral("category"), QStringLiteral("included"),
                    QStringLiteral("label"), QStringLiteral("limitTokens"), QStringLiteral("omitted"),
                    QStringLiteral("sources"), QStringLiteral("tokens"), QStringLiteral("truncated")}),
            "A section row carries metadata only: no rendered text");
    }
    require(categories == QStringList({QStringLiteral("project_metadata"), QStringLiteral("workspace_catalog")}),
        "A project-only snapshot without memory has the project and the catalog sections");
    int summedTokens = 0;
    for (const QVariant& row : controller.sections()) {
        const QVariantMap map = row.toMap();
        require(map.value(QStringLiteral("tokens")).toInt() > 0
                && map.value(QStringLiteral("tokens")).toInt() <= map.value(QStringLiteral("limitTokens")).toInt(),
            "A section stays inside its own limit");
        summedTokens += map.value(QStringLiteral("tokens")).toInt();
    }
    require(summedTokens == controller.usedTokens() && !controller.truncated(),
        "The section tokens add up to the used budget and a small workspace is not truncated");
    const auto exactSnapshot = controller.snapshotForAttachment(
        projectId.toStdString(), controller.fingerprint().toStdString());
    require(exactSnapshot.success() && exactSnapshot.value().fingerprint == controller.fingerprint().toStdString(),
        "The C++ Chat boundary can retrieve only the exact reviewed snapshot");
    require(!controller.snapshotForAttachment(projectId.toStdString(), std::string(64, '0')).success()
            && !controller.snapshotForAttachment("another-project", controller.fingerprint().toStdString()).success(),
        "A stale fingerprint or another project cannot retrieve the snapshot");
    const QVariantMap catalog = controller.sections().at(1).toMap();
    require(catalog.value(QStringLiteral("authority")).toString() == QStringLiteral("authoritative")
            && catalog.value(QStringLiteral("included")).toInt() >= 3,
        "The catalog is authoritative and lists the workspace files");

    // Nothing the preview publishes may carry a path or any workspace text.
    QStringList everything = {controller.errorText(), controller.fingerprint(), controller.policyId(),
        controller.workspaceFingerprint()};
    everything.append(QString::fromUtf8(QJsonDocument::fromVariant(controller.sections()).toJson()));
    const QString dump = everything.join(QLatin1Char('\n'));
    require(!dump.contains(projectRootOf(workspace), Qt::CaseInsensitive)
            && !dump.contains(QStringLiteral("a.qml")) && !dump.contains(QStringLiteral("Secret-looking"))
            && !dump.contains(QStringLiteral("notes.md")),
        "QML must receive no path, file name or workspace text");

    const QString first = controller.fingerprint();
    require(controller.preview(projectId, QString(), false) != 0, "A second preview is accepted once idle");
    waitNotBusy(controller);
    require(controller.fingerprint() == first, "The same inputs give the same fingerprint");

    require(bridge->runController().model()->rowCount() == 0, "A preview must not create a Run");
}

void testDerivedMemoryAppearsOnlyWhenRequested()
{
    TempWorkspace workspace;
    workspace.write("src/a.qml", "Item {}\n");
    auto bridge = openBridge(workspace);
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("Memory project"), QStringLiteral("d"), workspace.projectPathQt());
    require(!projectId.isEmpty(), "Project was not created");
    ProjectContextController& controller = bridge->projectContextController();

    // A completed Mock Run makes the Backend record a real Project Memory segment.
    const QString runId = bridge->runController().startRun(projectId, QStringLiteral("Mock"), QStringLiteral("Mock task"));
    require(!runId.isEmpty(), "Run was not started");
    require(ai::ui::test_support::waitForCondition("Run completed", [&] {
                return statusForRun(bridge->runController().model(), runId) == QStringLiteral("completed");
            }, 20s),
        "The Mock Run did not complete");

    const auto memoryRow = [&controller]() {
        for (const QVariant& row : controller.sections()) {
            if (row.toMap().value(QStringLiteral("category")).toString() == QStringLiteral("project_memory")) {
                return row.toMap();
            }
        }
        return QVariantMap();
    };

    require(controller.preview(projectId, QString(), false) != 0, "Preview without memory is accepted");
    waitNotBusy(controller);
    require(controller.hasSnapshot() && memoryRow().isEmpty() && !controller.includesProjectMemory(),
        "Project Memory is not part of the snapshot unless it was requested");

    // The memory write after a completed Run is best-effort and asynchronous: poll until the segment shows.
    bool seen = false;
    for (int attempt = 0; attempt < 40 && !seen; ++attempt) {
        require(controller.preview(projectId, QString(), true) != 0, "Preview with memory is accepted");
        waitNotBusy(controller);
        require(controller.hasSnapshot() && controller.includesProjectMemory(), "The memory snapshot is published");
        seen = memoryRow().value(QStringLiteral("included")).toInt() >= 2;  // the status item plus the segment
        if (!seen) ai::ui::test_support::pumpOneEventCycle();
    }
    require(seen, "A requested Project Memory must list the recorded segment");
    const QVariantMap row = memoryRow();
    require(row.value(QStringLiteral("authority")).toString() == QStringLiteral("derived")
            && row.value(QStringLiteral("label")).toString().startsWith(QStringLiteral("Project Memory")),
        "Project Memory is always labelled derived");
}

void testAFailureLeavesNoSnapshot()
{
    TempWorkspace workspace;
    workspace.write("src/a.qml", "Item {}\n");
    auto bridge = openBridge(workspace);
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("Failure project"), QStringLiteral("d"), workspace.projectPathQt());
    ProjectContextController& controller = bridge->projectContextController();
    FinishedEvents events;
    connectFinished(controller, events);

    require(controller.preview(projectId, QString(), false) != 0, "Preview is accepted");
    waitNotBusy(controller);
    require(controller.hasSnapshot(), "A snapshot first");

    const quint64 id = controller.preview(QStringLiteral("no-such-project"), QString(), false);
    require(id != 0, "An unknown project id is still an accepted request");
    require(!controller.hasSnapshot() && controller.fingerprint().isEmpty() && controller.sections().isEmpty(),
        "The old snapshot is dropped the moment a new preview is accepted");
    waitNotBusy(controller);
    require(events.count == 2 && events.lastId == id && !events.lastSuccess, "A failure is announced");
    require(!controller.hasSnapshot() && controller.fingerprint().isEmpty() && controller.sections().isEmpty()
            && controller.usedTokens() == 0,
        "A failure leaves no snapshot behind");
    require(controller.errorCode() == QStringLiteral("project_context.project_not_found")
            && !controller.errorText().isEmpty() && controller.projectId() == QStringLiteral("no-such-project"),
        "A failure publishes its code and text");

    // The selected Run travels to the Backend: an unknown one is reported by the Backend, not ignored.
    require(controller.preview(projectId, QStringLiteral("no-such-run"), false) != 0, "Preview with a Run is accepted");
    waitNotBusy(controller);
    require(!controller.hasSnapshot() && controller.errorCode() == QStringLiteral("project_context.selected_run_not_found")
            && controller.selectedRunId() == QStringLiteral("no-such-run"),
        "The selected Run id reaches the Backend");

    // An unreadable workspace is reported with the Backend's own code, never as a partial context.
    std::error_code ignored;
    fs::remove_all(workspace.projectPath, ignored);
    require(controller.preview(projectId, QString(), false) != 0, "Preview is accepted");
    waitNotBusy(controller);
    require(!controller.hasSnapshot() && controller.errorCode() == QStringLiteral("workspace_baseline.invalid_root"),
        "A missing workspace root fails with the capture error and no snapshot");
}

void testTruncationIsPublishedAsMetadata()
{
    TempWorkspace workspace;
    for (int index = 0; index < 1300; ++index) {
        workspace.write("src/dir" + std::to_string(index / 100) + "/file-" + std::to_string(index) + ".txt", "x");
    }
    auto bridge = openBridge(workspace);
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("Large project"), QStringLiteral("d"), workspace.projectPathQt());
    ProjectContextController& controller = bridge->projectContextController();
    require(controller.preview(projectId, QString(), false) != 0, "Preview is accepted");
    waitNotBusy(controller);
    require(controller.hasSnapshot() && controller.truncated() && controller.usedTokens() <= controller.totalTokens(),
        "A large workspace is trimmed to the budget and the controller says so");
    const QVariantMap catalog = controller.sections().at(1).toMap();
    require(catalog.value(QStringLiteral("truncated")).toBool() && catalog.value(QStringLiteral("omitted")).toInt() > 0
            && catalog.value(QStringLiteral("tokens")).toInt() <= catalog.value(QStringLiteral("limitTokens")).toInt(),
        "The catalog row reports its omitted items");
}

void testClearInvalidatesARunningBuild()
{
    TempWorkspace workspace;
    for (int index = 0; index < 400; ++index) {
        workspace.write("src/file-" + std::to_string(index) + ".txt", "content " + std::to_string(index));
    }
    auto bridge = openBridge(workspace);
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("Clear project"), QStringLiteral("d"), workspace.projectPathQt());
    ProjectContextController& controller = bridge->projectContextController();
    FinishedEvents events;
    connectFinished(controller, events);
    int changes = 0;
    QObject::connect(&controller, &ProjectContextController::snapshotChanged, [&changes] { ++changes; });

    require(controller.preview(projectId, QString(), false) != 0 && controller.busy(), "A build is running");
    controller.clear();
    require(controller.busy(), "clear() does not interrupt the running build");
    require(controller.preview(projectId, QString(), false) == 0, "A new preview waits for the running build to end");
    waitNotBusy(controller);
    require(events.count == 0, "The invalidated result is never announced");
    require(!controller.hasSnapshot() && controller.projectId().isEmpty() && controller.fingerprint().isEmpty(),
        "The invalidated result is never shown");

    // After a visible snapshot, clear() empties it and says so.
    require(controller.preview(projectId, QString(), false) != 0, "A fresh preview is accepted");
    waitNotBusy(controller);
    require(controller.hasSnapshot() && events.count == 1, "The fresh preview is published");
    const int before = changes;
    controller.clear();
    require(!controller.hasSnapshot() && controller.sections().isEmpty() && changes == before + 1,
        "clear() empties the state and announces it once");
    controller.clear();
    require(changes == before + 1, "clear() on an empty controller announces nothing");
}

void testDestroyingTheControllerWaitsForARunningBuild()
{
    TempWorkspace workspace;
    for (int index = 0; index < 400; ++index) {
        workspace.write("src/file-" + std::to_string(index) + ".txt", "content " + std::to_string(index));
    }
    // A runtime of its own: the controller is built over the runtime's service exactly like BackendBridge does,
    // and destroyed while the build is still running.
    auto runtime = std::make_unique<ai::backend::BackendRuntime>(
        ai::backend::BackendRuntimeConfig {.databasePath = workspace.databasePath});
    require(runtime->storageAvailable(), "Storage must be available");
    ai::application::CreateProjectRequest request;
    request.name = "Lifetime project";
    request.description = "d";
    request.rootPath = workspace.projectPath;
    const auto created = runtime->projectService().createProject(request);
    require(created.success(), "Project was not created");

    auto controller = std::make_unique<ProjectContextController>(runtime->projectContextSnapshotService());
    require(controller->preview(QString::fromStdString(created.value().id), QString(), false) != 0 && controller->busy(),
        "A build is running");
    controller.reset();  // waits for the build; the service and its repositories are still alive
    runtime.reset();
}

// PC-3E: the Chat controller's glue around the reviewed snapshot, over the real Backend. The provider is a local address
// nothing listens on, so a send fails at once on this machine: no network, no cost - and that failure is the very
// situation (a failed generation on a turn that carries a context) the conversation must survive.
void testChatSendsTheReviewedSnapshotOnlyThroughALiveQuote()
{
    TempWorkspace workspace;
    workspace.write("src/a.qml", "Item {}\n");
    auto bridge = openBridge(workspace);
    const QString projectId = bridge->projectController().createProject(
        QStringLiteral("Chat context project"), QStringLiteral("Intent"), workspace.projectPathQt());
    require(!projectId.isEmpty(), "Project was not created");
    auto& catalog = bridge->providerCatalogController();
    const QString providerId = catalog.createProvider(
        QStringLiteral("Unreachable"), QStringLiteral("local"), QStringLiteral("http://127.0.0.1:9/v1"));
    require(!providerId.isEmpty(), "Provider was not created");
    const QString modelId = catalog.createModel(providerId, QStringLiteral("unreachable-model"),
        QStringLiteral("Unreachable"), 32768, 4096, 0, 0);
    require(!modelId.isEmpty(), "Model was not created");
    ChatController& chat = bridge->chatController();
    const QString conversationId = chat.createConversation(providerId, modelId);
    require(!conversationId.isEmpty() && chat.setProject(conversationId, projectId), "The conversation was not created");
    ProjectContextController& context = bridge->projectContextController();

    QStringList codes;
    QObject::connect(&chat, &ChatController::errorOccurred, [&codes](const QString& code, const QString&) { codes.push_back(code); });
    const auto idle = [&] {
        require(ai::ui::test_support::waitForCondition("generation finished", [&] { return !chat.generating(); }, 30s),
            "The generation did not finish in time");
    };

    // Project Memory is requested although this project has none: the snapshot has no memory section and that is fine.
    require(context.preview(projectId, QString(), true) != 0, "The preview was refused");
    waitNotBusy(context);
    require(context.hasSnapshot() && context.includesProjectMemory(), "The preview with memory requested must succeed");
    const QString fingerprint = context.fingerprint();

    const QVariantMap quote = chat.projectContextQuote(conversationId, fingerprint);
    require(quote.value(QStringLiteral("ok")).toBool() && !quote.value(QStringLiteral("quoteToken")).toString().isEmpty()
            && quote.value(QStringLiteral("estimatedInputTokens")).toULongLong() > 0
            && !quote.value(QStringLiteral("priceKnown")).toBool(),
        "An empty requested memory must still be quoted (no price is configured for this model)");
    const QString token = quote.value(QStringLiteral("quoteToken")).toString();

    // The direct calls that skip or fake the quote write nothing and reach no provider.
    require(!chat.sendMessageWithProjectContext(conversationId, QStringLiteral("Hello"), fingerprint, QString())
            && codes.last() == QStringLiteral("chat_context.quote_required"),
        "A send without a quote must be refused");
    require(!chat.sendMessageWithProjectContext(conversationId, QStringLiteral("Hello"), fingerprint, QStringLiteral("not-a-token"))
            && codes.last() == QStringLiteral("chat_context.quote_required"),
        "A send with an unknown token must be refused");
    require(!chat.sendMessageWithProjectContext(conversationId, QStringLiteral("Hello"), QString(64, QLatin1Char('0')), token)
            && codes.last() == QStringLiteral("chat_context.stale_preview"),
        "A send naming another snapshot than the reviewed one must be refused");
    require(chat.messages(conversationId).isEmpty() && !chat.generating(), "Refused sends must write nothing");

    // The live quote lets exactly one send through; the provider is unreachable, so the answer is an error row.
    require(chat.sendMessageWithProjectContext(conversationId, QStringLiteral("Hello"), fingerprint, token),
        "The live quote must allow its send");
    idle();
    QVariantList rows = chat.messages(conversationId);
    require(rows.size() == 2, "The conversation must stay readable after the failed generation");
    const QVariantMap answer = rows.at(1).toMap();
    require(answer.value(QStringLiteral("role")).toString() == QStringLiteral("error")
            && answer.value(QStringLiteral("contextFingerprint")).toString() == fingerprint
            && answer.value(QStringLiteral("contextTokens")).toULongLong() > 0,
        "The failed turn must still say which context it carried");
    const QString answerId = answer.value(QStringLiteral("messageId")).toString();

    // A quote confirms ONE message.
    require(!chat.sendMessageWithProjectContext(conversationId, QStringLiteral("Again"), fingerprint, token)
            && codes.last() == QStringLiteral("chat_context.quote_required"),
        "A used quote must not confirm a second message");

    // Retry of the failed message works and the conversation is still readable afterwards.
    require(chat.retryMessage(answerId), "Retry of the failed message must start");
    idle();
    rows = chat.messages(conversationId);
    require(rows.size() == 2 && rows.at(1).toMap().value(QStringLiteral("contextFingerprint")).toString() == fingerprint,
        "The conversation and the sealed context must survive a Retry");

    // A fresh quote allows the next context message; a plain message needs none.
    const QVariantMap again = chat.projectContextQuote(conversationId, fingerprint);
    require(again.value(QStringLiteral("ok")).toBool()
            && again.value(QStringLiteral("quoteToken")).toString() != token
            && chat.sendMessageWithProjectContext(conversationId, QStringLiteral("Second"), fingerprint,
                   again.value(QStringLiteral("quoteToken")).toString()),
        "A new quote must allow the next message with context");
    idle();
    require(chat.sendMessage(conversationId, QStringLiteral("A plain message")), "A plain message needs no quote");
    idle();
    require(chat.messages(conversationId).size() == 6, "Every turn must be stored and readable");
}

void testChatTextTellsTheTruthAboutWhatIsSent()
{
    const QString directory = QStringLiteral(AI_ORCHESTRATOR_QML_SOURCE_DIR);
    QFile page(directory + QStringLiteral("/pages/Chats/ChatsPage.qml"));
    require(page.open(QIODevice::ReadOnly | QIODevice::Text), "ChatsPage.qml could not be read");
    const QString text = QString::fromUtf8(page.readAll());
    require(!text.contains(QStringLiteral("Active project context attached")),
        "The misleading 'Active project context attached' text must be gone");
    require(text.contains(QStringLiteral("Project Context Snapshot is opt-in per message"))
            && text.contains(QStringLiteral("sendMessageWithProjectContext"))
            && text.contains(QStringLiteral("pendingContextQuote.quoteToken"))
            && text.contains(QStringLiteral("Conservative additional input-cost estimate")),
        "Chat must describe and implement explicit per-message attachment after a cost quote");

    QFile component(directory + QStringLiteral("/pages/Chats/Chat_Objects/ChatProjectContextPreview.qml"));
    require(component.open(QIODevice::ReadOnly | QIODevice::Text), "ChatProjectContextPreview.qml could not be read");
    const QString componentText = QString::fromUtf8(component.readAll());
    require(componentText.contains(QStringLiteral("Previewing alone sends nothing"))
            && componentText.contains(QStringLiteral("confirmed separately with a token and cost estimate")),
        "The preview must distinguish previewing from separately confirmed attachment");
    require(componentText.contains(QStringLiteral("checked: false")),
        "Project Memory must be off by default");

    QFile panel(directory + QStringLiteral("/pages/Chats/Chat_Objects/ChatSessionPanel.qml"));
    require(panel.open(QIODevice::ReadOnly | QIODevice::Text), "ChatSessionPanel.qml could not be read");
    require(QString::fromUtf8(panel.readAll()).contains(QStringLiteral("ChatProjectContextPreview")),
        "The session panel must host the preview");
}

} // namespace

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();
    QCoreApplication application(argc, argv);
    try {
        testPreviewPublishesMetadataOnlyAndNeverWrites();
        testDerivedMemoryAppearsOnlyWhenRequested();
        testAFailureLeavesNoSnapshot();
        testTruncationIsPublishedAsMetadata();
        testClearInvalidatesARunningBuild();
        testDestroyingTheControllerWaitsForARunningBuild();
        testChatTextTellsTheTruthAboutWhatIsSent();
        testChatSendsTheReviewedSnapshotOnlyThroughALiveQuote();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
