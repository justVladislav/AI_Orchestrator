#include "../../test_support/QtTestSupport.hpp"

#include <ai/application/conversations/ChatTypes.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/PromptWorkshopController.hpp>
#include <ai/ui/RunListModel.hpp>

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// PC-3C: the Qt boundary of the Prompt Workshop and the per-message plan state, on a real BackendBridge /
// BackendRuntime over a disposable SQLite database. Chat messages are seeded through a second connection (a model
// reply cannot be produced without a provider); nothing here reaches a provider or starts a Run.

namespace {

namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("aio-pwctl-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";

    TempWorkspace() { fs::create_directories(projectPath / "src"); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    [[nodiscard]] QString projectPathQt() const { return QString::fromStdWString(projectPath.wstring()); }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }
};

const std::string kPlan = R"json({
  "schemaVersion": "next-run-plan.v1",
  "title": "History page",
  "sharedContracts": [
    {"id": "S1", "kind": "interface_contract", "title": "Loss entry",
     "text": "property string metricId\nproperty real share", "appliesToPaths": ["qml/models"]}
  ],
  "drafts": [
    {"key": "model", "title": "Model task", "task": "Create the model.", "sharedContractIds": ["S1"]},
    {"key": "view", "title": "View task", "task": "Create the view.",
     "dependsOn": ["model"], "sharedContractIds": ["S1"],
     "contracts": [{"id": "V1", "kind": "constraint", "title": "Size",
                    "text": "No file above 300 lines.", "appliesToPaths": []}]}
  ]
})json";

std::string envelope(const std::string& json = kPlan)
{
    return "Here is the plan.\n\n```next-run-plan.v1\n" + json + "\n```\n\nTell me what to change.";
}

struct Setup {
    TempWorkspace workspace;
    std::unique_ptr<BackendBridge> bridge;
    std::shared_ptr<ai::infrastructure::SqliteDatabase> database;
    std::unique_ptr<ai::infrastructure::SqliteChatRepository> chat;
    QString projectId;
    QString providerId;
    QString modelId;
    QString conversationId;
    int messageCounter {0};

    Setup()
    {
        // The composition root is large; keep it off the test thread's stack like the other adapter tests.
        bridge = std::make_unique<BackendBridge>(workspace.databasePathQt());
        require(bridge->storageAvailable(), "Storage must be available");
        projectId = bridge->projectController().createProject(
            QStringLiteral("Workshop project"), QStringLiteral("d"), workspace.projectPathQt());
        require(!projectId.isEmpty(), "Project was not created");
        auto& catalog = bridge->providerCatalogController();
        providerId = catalog.createProvider(QStringLiteral("Local test provider"), QStringLiteral("local"),
            QStringLiteral("http://127.0.0.1:11434/v1"));
        require(!providerId.isEmpty(), "Provider was not created");
        modelId = catalog.createModel(providerId, QStringLiteral("local-test-model"), QStringLiteral("Local test model"),
            32768, 4096, 0, 0);
        require(!modelId.isEmpty(), "Model was not created");
        conversationId = bridge->chatController().createConversation(providerId, modelId);
        require(!conversationId.isEmpty(), "Conversation was not created");
        require(bridge->chatController().setProject(conversationId, projectId), "The conversation must be bound to the project");

        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "The second connection must open");
        database = opened.value();
        chat = std::make_unique<ai::infrastructure::SqliteChatRepository>(database);
    }

    QString addMessage(ai::application::ChatRole role, ai::application::ChatMessageStatus status,
        const std::string& content, const QString& conversation = {})
    {
        ai::application::ChatMessage message;
        message.id = "message-" + std::to_string(++messageCounter);
        message.conversationId = (conversation.isEmpty() ? conversationId : conversation).toStdString();
        message.role = role;
        message.status = status;
        message.content = content;
        message.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'000 + messageCounter));
        require(chat->insertMessage(message).success(), "The message must be stored");
        return QString::fromStdString(message.id);
    }

    QString addPlanMessage(const std::string& content = envelope())
    {
        return addMessage(ai::application::ChatRole::Assistant, ai::application::ChatMessageStatus::Sent, content);
    }

    [[nodiscard]] PromptWorkshopController& workshop() { return bridge->promptWorkshopController(); }
    [[nodiscard]] int draftRows() { return bridge->runDraftController().drafts()->rowCount(); }

    QVariantMap row(const QString& stageType, const QString& role, bool lead, bool workspaceAccess, int position) const
    {
        return QVariantMap {
            {QStringLiteral("stageType"), stageType},
            {QStringLiteral("providerId"), providerId},
            {QStringLiteral("modelId"), modelId},
            {QStringLiteral("agentName"), role},
            {QStringLiteral("role"), role},
            {QStringLiteral("instructions"), QStringLiteral("Own %1 work.").arg(role)},
            {QStringLiteral("leadArchitect"), lead},
            {QStringLiteral("workspaceAccess"), workspaceAccess},
            {QStringLiteral("position"), position},
            {QStringLiteral("dependencies"), lead ? QVariantList {} : QVariantList {QStringLiteral("architect")}},
            {QStringLiteral("timeoutMs"), 90000},
            {QStringLiteral("maxAttempts"), 1}};
    }

    QString createTemplateDraft()
    {
        QVariantList roster;
        roster.push_back(row(QStringLiteral("architect"), QStringLiteral("Lead"), true, false, 0));
        roster.push_back(row(QStringLiteral("qml-implementer"), QStringLiteral("QML implementer"), false, true, 1));
        roster.push_back(row(QStringLiteral("cpp-implementer"), QStringLiteral("C++ implementer"), false, true, 2));
        const QString id = bridge->runDraftController().createDraft(QStringLiteral("Roster template"), projectId,
            QStringLiteral("Template title"), QStringLiteral("Template task"), QStringLiteral("custom_roster"),
            QString {}, roster, 3, QString {}, false, QStringLiteral("execution-manifest-v1"));
        require(!id.isEmpty(), "The template draft must be created");
        return id;
    }
};

struct Signals {
    int created {0};
    int createdDrafts {0};
    QString lastGroup;
    int failed {0};
    QString lastCode;
};

void listen(PromptWorkshopController& controller, Signals& seen)
{
    QObject::connect(&controller, &PromptWorkshopController::draftsCreated,
        [&seen](const QString& groupId, int count) {
            ++seen.created;
            seen.createdDrafts = count;
            seen.lastGroup = groupId;
        });
    QObject::connect(&controller, &PromptWorkshopController::operationFailed,
        [&seen](const QString& code, const QString&) {
            ++seen.failed;
            seen.lastCode = code;
        });
}

QString envelopeStateOf(Setup& setup, const QString& messageId)
{
    for (const QVariant& entry : setup.bridge->chatController().messages(setup.conversationId)) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("messageId")).toString() == messageId) {
            require(row.contains(QStringLiteral("planEnvelope")), "Every message row carries planEnvelope");
            return row.value(QStringLiteral("planEnvelope")).toString();
        }
    }
    throw std::runtime_error("The message is not in the list");
}

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

QString completedMockRun(Setup& setup, const QString& projectId)
{
    const QString runId = setup.bridge->runController().startRun(projectId, QStringLiteral("Mock"), QStringLiteral("Mock task"));
    require(!runId.isEmpty(), "A Mock Run must start");
    require(ai::ui::test_support::waitForCondition("Run completed", [&] {
                return statusForRun(setup.bridge->runController().model(), runId) == QStringLiteral("completed");
            }, std::chrono::seconds(20)),
        "The Mock Run did not complete");
    return runId;
}

// One role of one row of the Drafts list model, by role NAME (the names QML uses).
QVariant draftRole(Setup& setup, const QString& draftId, const char* roleName)
{
    QAbstractItemModel* model = setup.bridge->runDraftController().drafts();
    const int role = model->roleNames().key(QByteArray(roleName), -1);
    require(role >= 0, "The Drafts list model must publish this role");
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, model->roleNames().key("draftId")).toString() == draftId) return model->data(index, role);
    }
    throw std::runtime_error("The draft is not in the Drafts list");
}

QStringList sortedKeys(const QVariantMap& map)
{
    QStringList keys = map.keys();
    keys.sort();
    return keys;
}

// ---------------------------------------------------------------------------------------------------------

void testTheMessageListCarriesTheBackendDecidedPlanEnvelopeState()
{
    using ai::application::ChatMessageStatus;
    using ai::application::ChatRole;
    Setup setup;
    const QString one = setup.addPlanMessage();
    const QString two = setup.addPlanMessage(envelope() + "\n" + envelope());
    const QString huge = setup.addPlanMessage("```next-run-plan.v1\n" + std::string(200 * 1024, 'x') + "\n```");
    const QString plain = setup.addMessage(ChatRole::Assistant, ChatMessageStatus::Sent, "An ordinary reply.");
    const QString prose = setup.addMessage(ChatRole::Assistant, ChatMessageStatus::Sent, "I will use next-run-plan.v1 later.");
    const QString fromUser = setup.addMessage(ChatRole::User, ChatMessageStatus::Sent, envelope());
    const QString failed = setup.addMessage(ChatRole::Assistant, ChatMessageStatus::Failed, envelope());
    const QString generating = setup.addMessage(ChatRole::Assistant, ChatMessageStatus::Generating, envelope());

    require(envelopeStateOf(setup, one) == QStringLiteral("one"), "One envelope on a completed Assistant message");
    require(envelopeStateOf(setup, two) == QStringLiteral("multiple"), "Two envelopes");
    require(envelopeStateOf(setup, huge) == QStringLiteral("too_large"), "A body over the limit");
    require(envelopeStateOf(setup, plain) == QStringLiteral("none"), "An ordinary reply");
    require(envelopeStateOf(setup, prose) == QStringLiteral("none"), "A mention of the tag in prose");
    require(envelopeStateOf(setup, fromUser) == QStringLiteral("none"), "The user's own message offers nothing");
    require(envelopeStateOf(setup, failed) == QStringLiteral("none"), "A failed message offers nothing");
    require(envelopeStateOf(setup, generating) == QStringLiteral("none"), "A message still being written offers nothing");
}

void testAPreviewWritesNothingAndCreateWritesDraftsOnly()
{
    Setup setup;
    const QString messageId = setup.addPlanMessage();
    auto& workshop = setup.workshop();
    require(setup.bridge->promptWorkshop() == &workshop, "BackendBridge must expose the controller to QML");
    Signals seen;
    listen(workshop, seen);

    const QVariantMap preview = workshop.previewFromMessage(setup.conversationId, messageId);
    require(preview.value(QStringLiteral("ok")).toBool(), "A valid plan previews");
    require(sortedKeys(preview) == QStringList({QStringLiteral("conversationId"), QStringLiteral("drafts"),
                QStringLiteral("existingGroupId"), QStringLiteral("hasStaffing"), QStringLiteral("messageId"), QStringLiteral("ok"),
                QStringLiteral("origin"), QStringLiteral("planFingerprint"), QStringLiteral("planTitle"),
                QStringLiteral("previewToken"), QStringLiteral("projectId"), QStringLiteral("sharedContractCount"),
                QStringLiteral("shortFingerprint"), QStringLiteral("staffingMaxWorkers"), QStringLiteral("warnings")}),
        "The preview map has exactly the documented fields");
    require(preview.value(QStringLiteral("projectId")).toString() == setup.projectId
            && preview.value(QStringLiteral("origin")).toString() == QStringLiteral("chat_message")
            && preview.value(QStringLiteral("messageId")).toString() == messageId
            && !preview.value(QStringLiteral("previewToken")).toString().isEmpty()
            && preview.value(QStringLiteral("existingGroupId")).toString().isEmpty()
            && preview.value(QStringLiteral("planTitle")).toString() == QStringLiteral("History page")
            && preview.value(QStringLiteral("planFingerprint")).toString().size() == 64
            && preview.value(QStringLiteral("shortFingerprint")).toString().size() == 12
            && preview.value(QStringLiteral("sharedContractCount")).toInt() == 1
            && preview.value(QStringLiteral("warnings")).toList().isEmpty(),
        "The preview names the project from the conversation, the message and the plan");
    const QVariantList previewDrafts = preview.value(QStringLiteral("drafts")).toList();
    require(previewDrafts.size() == 2, "Two drafts");
    const QVariantMap second = previewDrafts.at(1).toMap();
    require(sortedKeys(second) == QStringList({QStringLiteral("contracts"), QStringLiteral("dependsOn"), QStringLiteral("key"),
                QStringLiteral("sharedContractCount"), QStringLiteral("sharedContractIds"), QStringLiteral("staffing"),
                QStringLiteral("task"), QStringLiteral("title")}),
        "A previewed draft has exactly the documented fields");
    require(second.value(QStringLiteral("key")).toString() == QStringLiteral("view")
            && second.value(QStringLiteral("task")).toString() == QStringLiteral("Create the view.")
            && second.value(QStringLiteral("dependsOn")).toStringList() == QStringList({QStringLiteral("model")})
            && second.value(QStringLiteral("sharedContractIds")).toStringList() == QStringList({QStringLiteral("S1")})
            && second.value(QStringLiteral("sharedContractCount")).toInt() == 1
            && second.value(QStringLiteral("contracts")).toList().size() == 2,
        "...with the merged contracts (the shared one, then its own)");
    require(setup.draftRows() == 0 && seen.created == 0 && seen.failed == 0, "A preview writes nothing and announces nothing");

    const QString messageToken = preview.value(QStringLiteral("previewToken")).toString();
    const QVariantMap created = workshop.createFromPreview(messageToken);
    require(created.value(QStringLiteral("ok")).toBool() && !created.value(QStringLiteral("alreadyExisted")).toBool(),
        "Confirming the preview writes");
    require(sortedKeys(created) == QStringList({QStringLiteral("alreadyExisted"), QStringLiteral("drafts"),
                QStringLiteral("groupId"), QStringLiteral("ok"), QStringLiteral("origin"), QStringLiteral("planFingerprint"),
                QStringLiteral("planTitle"), QStringLiteral("projectId"), QStringLiteral("sourceRunIds")}),
        "The creation map has exactly the documented fields");
    const QVariantList createdDrafts = created.value(QStringLiteral("drafts")).toList();
    require(createdDrafts.size() == 2 && seen.created == 1 && seen.createdDrafts == 2
            && seen.lastGroup == created.value(QStringLiteral("groupId")).toString(),
        "draftsCreated announces the group once");
    const QVariantMap viewRow = createdDrafts.at(1).toMap();
    require(viewRow.value(QStringLiteral("planKey")).toString() == QStringLiteral("view")
            && viewRow.value(QStringLiteral("ordinal")).toInt() == 1
            && viewRow.value(QStringLiteral("dependsOn")).toStringList() == QStringList({QStringLiteral("model")})
            && viewRow.value(QStringLiteral("contractCount")).toInt() == 2
            && viewRow.value(QStringLiteral("rosterSize")).toInt() == 0
            && viewRow.value(QStringLiteral("title")).toString() == QStringLiteral("View task"),
        "A created draft row");

    // The Drafts list refreshed on its own, and a draft opens exactly as the New Run dialog will read it.
    require(setup.draftRows() == 2, "The Drafts list shows the new drafts without a restart");
    const QVariantMap opened = setup.bridge->runDraftController().openDraft(viewRow.value(QStringLiteral("draftId")).toString());
    require(opened.value(QStringLiteral("projectId")).toString() == setup.projectId
            && opened.value(QStringLiteral("workflowId")).toString() == QStringLiteral("execution-manifest-v1")
            && opened.value(QStringLiteral("launchMode")).toString() == QStringLiteral("custom_roster")
            && opened.value(QStringLiteral("title")).toString() == QStringLiteral("View task")
            && opened.value(QStringLiteral("task")).toString() == QStringLiteral("Create the view.")
            && opened.value(QStringLiteral("roster")).toList().isEmpty()
            && opened.value(QStringLiteral("interfaceContracts")).toList().size() == 2,
        "A created draft reopens as a custom Manifest roster draft with its contracts");
    const QVariantMap sharedContract = opened.value(QStringLiteral("interfaceContracts")).toList().at(0).toMap();
    require(sharedContract.value(QStringLiteral("id")).toString() == QStringLiteral("S1")
            && sharedContract.value(QStringLiteral("text")).toString() == QStringLiteral("property string metricId\nproperty real share"),
        "The shared contract text arrived verbatim");

    // A repeated confirm is the same group: no write, no announcement - the same token (a double click) or a second
    // preview of the unchanged message.
    const QVariantMap doubleClick = workshop.createFromPreview(messageToken);
    require(doubleClick.value(QStringLiteral("ok")).toBool() && doubleClick.value(QStringLiteral("alreadyExisted")).toBool()
            && doubleClick.value(QStringLiteral("groupId")) == created.value(QStringLiteral("groupId")),
        "Confirming the same preview twice returns the existing group");
    const QVariantMap previewAgain = workshop.previewFromMessage(setup.conversationId, messageId);
    require(previewAgain.value(QStringLiteral("existingGroupId")) == created.value(QStringLiteral("groupId")),
        "The preview now names the existing group");
    const QVariantMap again = workshop.createFromPreview(previewAgain.value(QStringLiteral("previewToken")).toString());
    require(again.value(QStringLiteral("ok")).toBool() && again.value(QStringLiteral("alreadyExisted")).toBool()
            && again.value(QStringLiteral("groupId")) == created.value(QStringLiteral("groupId")),
        "A second preview of the unchanged message confirms to the same group");
    require(seen.created == 1 && setup.draftRows() == 2, "...announces nothing and writes nothing");

    // Drafts are not Runs: nothing was started or queued.
    require(setup.bridge->runController().model()->rowCount() == 0, "Creating Drafts must not create a Run");
}

void testAManualPasteIsConfirmedByTokenOnly()
{
    Setup setup;
    auto& workshop = setup.workshop();
    Signals seen;
    listen(workshop, seen);

    const QVariantMap preview = workshop.previewFromPaste(setup.conversationId, QString::fromStdString(kPlan));
    require(preview.value(QStringLiteral("ok")).toBool()
            && preview.value(QStringLiteral("origin")).toString() == QStringLiteral("manual_import")
            && !preview.value(QStringLiteral("previewToken")).toString().isEmpty()
            && preview.value(QStringLiteral("messageId")).toString().isEmpty(),
        "A manual paste previews with a token");
    require(setup.draftRows() == 0, "A preview writes nothing");

    const QString token = preview.value(QStringLiteral("previewToken")).toString();
    const QVariantMap created = workshop.createFromPreview(token);
    require(created.value(QStringLiteral("ok")).toBool() && !created.value(QStringLiteral("alreadyExisted")).toBool()
            && created.value(QStringLiteral("origin")).toString() == QStringLiteral("manual_import")
            && created.value(QStringLiteral("drafts")).toList().size() == 2,
        "Confirming by token creates");
    require(seen.created == 1 && setup.draftRows() == 2, "...once");

    const QVariantMap again = workshop.createFromPreview(token);
    require(again.value(QStringLiteral("alreadyExisted")).toBool() && seen.created == 1 && setup.draftRows() == 2,
        "Confirming the same preview twice changes nothing");

    const QVariantMap secondPreview = workshop.previewFromPaste(setup.conversationId, QString::fromStdString(envelope()));
    require(secondPreview.value(QStringLiteral("ok")).toBool() && secondPreview.value(QStringLiteral("previewToken")).toString() != token,
        "A fenced paste previews too, with another token");
    require(workshop.createFromPreview(secondPreview.value(QStringLiteral("previewToken")).toString())
                .value(QStringLiteral("groupId")).toString()
            != created.value(QStringLiteral("groupId")).toString()
            && setup.draftRows() == 4,
        "A new preview of the same plan is a deliberate separate copy");
    require(setup.bridge->runController().model()->rowCount() == 0, "No Run was created");
}

// 6a (a): a plan's staffing hint reaches the dialog as the Backend's own map (worded there), a plan without one says so, and an invalid
// hint is dropped with a warning instead of failing the plan.
void testAStaffingHintIsProjectedForTheDialog()
{
    Setup setup;
    auto& workshop = setup.workshop();
    const auto planWith = [](const std::string& staffing) {
        return std::string(R"json({"schemaVersion": "next-run-plan.v1", "title": "Staffed", "drafts": [
          {"key": "a", "title": "First", "task": "Edit A.qml.", "staffing": )json") + staffing
            + R"json(},
          {"key": "b", "title": "Second", "task": "Edit B.qml."}]})json";
    };
    const QVariantMap preview = workshop.previewFromPaste(setup.conversationId, QString::fromStdString(planWith(
        R"json({"workers": [{"role": "implementation", "focus": "A.qml"}, {"role": "documentation", "focus": "notes"}],
                "parallelism": 2, "reason": "Two independent parts."})json")));
    require(preview.value(QStringLiteral("ok")).toBool() && preview.value(QStringLiteral("hasStaffing")).toBool()
            && preview.value(QStringLiteral("staffingMaxWorkers")).toInt() == 2
            && preview.value(QStringLiteral("warnings")).toList().isEmpty(),
        "A plan with a hint says so, with the largest suggested worker count");
    const QVariantList drafts = preview.value(QStringLiteral("drafts")).toList();
    const QVariantMap first = drafts.at(0).toMap().value(QStringLiteral("staffing")).toMap();
    require(sortedKeys(first) == QStringList({QStringLiteral("parallelism"), QStringLiteral("present"), QStringLiteral("reason"),
                QStringLiteral("summary"), QStringLiteral("workerCount"), QStringLiteral("workers")})
            && first.value(QStringLiteral("present")).toBool() && first.value(QStringLiteral("workerCount")).toInt() == 2
            && first.value(QStringLiteral("parallelism")).toInt() == 2
            && first.value(QStringLiteral("summary")).toString()
                == QStringLiteral("2 workers (1 implementation, 1 documentation), parallelism 2")
            && first.value(QStringLiteral("workers")).toList().size() == 2,
        "The Backend words the hint; QML only renders it");
    const QVariantMap second = drafts.at(1).toMap().value(QStringLiteral("staffing")).toMap();
    require(!second.value(QStringLiteral("present")).toBool() && second.value(QStringLiteral("summary")).toString().isEmpty(),
        "A draft without a hint says so");

    const QVariantMap dropped = workshop.previewFromPaste(setup.conversationId, QString::fromStdString(planWith(
        R"json({"workers": [{"role": "implementation", "focus": "A.qml"}], "parallelism": 5, "reason": "x"})json")));
    const QVariantList warnings = dropped.value(QStringLiteral("warnings")).toList();
    require(dropped.value(QStringLiteral("ok")).toBool() && !dropped.value(QStringLiteral("hasStaffing")).toBool()
            && warnings.size() == 1
            && warnings.at(0).toMap().value(QStringLiteral("code")).toString() == QStringLiteral("next_run_plan.staffing_ignored"),
        "An invalid hint is dropped with a warning and the plan stays valid");
}

void testFailuresAreTypedAndLeaveNothingBehind()
{
    using ai::application::ChatMessageStatus;
    using ai::application::ChatRole;
    Setup setup;
    auto& workshop = setup.workshop();
    Signals seen;
    listen(workshop, seen);
    const auto expectFailure = [&](const QVariantMap& result, const char* code, const char* what) {
        require(!result.value(QStringLiteral("ok")).toBool() && result.value(QStringLiteral("errorCode")).toString() == QLatin1String(code)
                && !result.value(QStringLiteral("errorMessage")).toString().isEmpty(),
            what);
        require(seen.lastCode == QLatin1String(code) && !workshop.lastError().isEmpty(), "...and is reported through the signal");
        require(sortedKeys(result) == QStringList({QStringLiteral("errorCode"), QStringLiteral("errorMessage"), QStringLiteral("ok")}),
            "...with the failure map only");
        require(setup.draftRows() == 0, "...and nothing was written");
    };

    expectFailure(workshop.previewFromMessage(setup.conversationId, QStringLiteral("nope")),
        "run_draft_plan.message_not_found", "An unknown message");
    const QString user = setup.addMessage(ChatRole::User, ChatMessageStatus::Sent, envelope());
    expectFailure(workshop.previewFromMessage(setup.conversationId, user), "run_draft_plan.message_not_eligible",
        "The user's own message is not eligible");
    expectFailure(workshop.createFromPreview(user), "run_draft_plan.preview_not_found",
        "A message id is not a confirmation token");
    const QString plain = setup.addMessage(ChatRole::Assistant, ChatMessageStatus::Sent, "No plan here.");
    expectFailure(workshop.previewFromMessage(setup.conversationId, plain), "next_run_plan.envelope_missing", "No envelope");
    expectFailure(workshop.previewFromPaste(setup.conversationId, QStringLiteral("{")), "next_run_plan.invalid_json", "Broken JSON");
    expectFailure(workshop.createFromPreview(QStringLiteral("not-a-token")), "run_draft_plan.preview_not_found", "An unknown token");
    expectFailure(workshop.previewFromPaste(QStringLiteral("no-such-conversation"), QString::fromStdString(kPlan)),
        "run_draft_plan.conversation_not_found", "An unknown conversation");

    // A conversation with no project cannot take drafts.
    ai::application::ChatConversation loose;
    loose.id = "conversation-loose";
    loose.providerId = setup.providerId.toStdString();
    loose.modelId = setup.modelId.toStdString();
    loose.title = "Loose";
    require(setup.chat->insertConversation(loose).success(), "setup: a conversation without a project");
    expectFailure(workshop.previewFromPaste(QStringLiteral("conversation-loose"), QString::fromStdString(kPlan)),
        "run_draft_plan.conversation_without_project", "A conversation without a project");
    const int failedBefore = seen.failed;
    const QVariantMap loosePicker = workshop.rosterTemplates(QStringLiteral("conversation-loose"));
    require(!loosePicker.value(QStringLiteral("ok")).toBool()
            && loosePicker.value(QStringLiteral("errorCode")).toString() == QStringLiteral("run_draft_plan.conversation_without_project")
            && !loosePicker.value(QStringLiteral("errorMessage")).toString().isEmpty()
            && !loosePicker.contains(QStringLiteral("templates"))
            && seen.failed == failedBefore + 1 && !workshop.lastError().isEmpty(),
        "...also when listing templates: a typed failure, not an empty list");

    // A later success clears the error.
    const QVariantMap good = workshop.previewFromPaste(setup.conversationId, QString::fromStdString(kPlan));
    require(good.value(QStringLiteral("ok")).toBool() && workshop.lastError().isEmpty(), "A success clears lastError");
    workshop.clearError();
    require(workshop.lastError().isEmpty(), "clearError is harmless when there is no error");
}

void testARosterTemplateIsCopiedAndOnlyForTheSameProject()
{
    Setup setup;
    auto& workshop = setup.workshop();
    Signals seen;
    listen(workshop, seen);
    const QString messageId = setup.addPlanMessage();

    const QVariantMap noTemplates = workshop.rosterTemplates(setup.conversationId);
    require(noTemplates.value(QStringLiteral("ok")).toBool() && noTemplates.value(QStringLiteral("templates")).toList().isEmpty(),
        "No roster draft yet: a successful answer with nothing to offer");
    const QString templateId = setup.createTemplateDraft();
    const QVariantList choices = workshop.rosterTemplates(setup.conversationId).value(QStringLiteral("templates")).toList();
    require(choices.size() == 1, "The saved roster draft is offered");
    const QVariantMap choice = choices.at(0).toMap();
    require(sortedKeys(choice) == QStringList({QStringLiteral("draftId"), QStringLiteral("maxParallelism"),
                QStringLiteral("name"), QStringLiteral("rosterSize"), QStringLiteral("title"), QStringLiteral("workerCount")})
            && choice.value(QStringLiteral("workerCount")).toInt() == 2
            && choice.value(QStringLiteral("draftId")).toString() == templateId
            && choice.value(QStringLiteral("rosterSize")).toInt() == 3
            && choice.value(QStringLiteral("maxParallelism")).toInt() == 3
            && choice.value(QStringLiteral("name")).toString() == QStringLiteral("Roster template"),
        "A template choice");

    // An unknown template is refused and writes nothing.
    const QVariantMap templatePreview = workshop.previewFromMessage(setup.conversationId, messageId);
    require(templatePreview.value(QStringLiteral("ok")).toBool(), "setup: a preview");
    const QString templateToken = templatePreview.value(QStringLiteral("previewToken")).toString();
    const QVariantMap refused = workshop.createFromPreview(templateToken, QStringLiteral("no-such-draft"));
    require(!refused.value(QStringLiteral("ok")).toBool()
            && refused.value(QStringLiteral("errorCode")).toString() == QStringLiteral("run_draft_plan.template_not_found")
            && setup.draftRows() == 1 && seen.created == 0,
        "An unknown template is refused");

    const QVariantMap created = workshop.createFromPreview(templateToken, templateId);
    require(created.value(QStringLiteral("ok")).toBool() && created.value(QStringLiteral("drafts")).toList().size() == 2
            && setup.draftRows() == 3,
        "Create with a template");
    for (const QVariant& entry : created.value(QStringLiteral("drafts")).toList()) {
        const QVariantMap row = entry.toMap();
        require(row.value(QStringLiteral("rosterSize")).toInt() == 3, "Each draft got the roster");
        const QVariantMap opened = setup.bridge->runDraftController().openDraft(row.value(QStringLiteral("draftId")).toString());
        require(opened.value(QStringLiteral("maxParallelism")).toInt() == 3
                && !opened.value(QStringLiteral("automaticVerificationRepair")).toBool()
                && opened.value(QStringLiteral("roster")).toList().size() == 3
                && opened.value(QStringLiteral("launchMode")).toString() == QStringLiteral("custom_roster")
                && opened.value(QStringLiteral("workflowId")).toString() == QStringLiteral("execution-manifest-v1")
                && opened.value(QStringLiteral("title")).toString() != QStringLiteral("Template title")
                && opened.value(QStringLiteral("task")).toString() != QStringLiteral("Template task"),
            "The roster, parallelism and repair policy are copied; the objective is the plan's");
    }
    require(workshop.rosterTemplates(setup.conversationId).value(QStringLiteral("templates")).toList().size() == 3,
        "Drafts with a roster qualify as templates themselves");
    require(setup.bridge->runController().model()->rowCount() == 0, "No Run was created");
}


void testAMessageChangedAfterItsPreviewIsRefusedAsStale()
{
    Setup setup;
    auto& workshop = setup.workshop();
    Signals seen;
    listen(workshop, seen);
    const QString messageId = setup.addPlanMessage();
    const QVariantMap preview = workshop.previewFromMessage(setup.conversationId, messageId);
    require(preview.value(QStringLiteral("ok")).toBool(), "setup: a preview");
    const QString token = preview.value(QStringLiteral("previewToken")).toString();

    // The reply is edited after the user looked at the preview.
    auto stored = setup.chat->findMessage(messageId.toStdString()).value().value();
    stored.content += "\nEdited after the preview.";
    require(setup.chat->updateMessage(stored).success(), "setup: the message changes");
    const QVariantMap stale = workshop.createFromPreview(token);
    require(!stale.value(QStringLiteral("ok")).toBool()
            && stale.value(QStringLiteral("errorCode")).toString() == QStringLiteral("run_draft_plan.stale_preview")
            && seen.lastCode == QStringLiteral("run_draft_plan.stale_preview") && !workshop.lastError().isEmpty(),
        "A message changed since its preview is refused as stale");
    require(setup.draftRows() == 0 && seen.created == 0, "...and nothing was written or announced");

    const QVariantMap fresh = workshop.previewFromMessage(setup.conversationId, messageId);
    const QVariantMap created = workshop.createFromPreview(fresh.value(QStringLiteral("previewToken")).toString());
    require(created.value(QStringLiteral("ok")).toBool() && setup.draftRows() == 2 && workshop.lastError().isEmpty(),
        "A new preview of the changed message confirms, and clears the error");
}

void testSourceRunsAreCheckedThroughTheRealRunRepository()
{
    Setup setup;
    auto& workshop = setup.workshop();
    Signals seen;
    listen(workshop, seen);
    const QString messageId = setup.addPlanMessage();

    fs::create_directories(setup.workspace.root / "project2");
    const QString otherProject = setup.bridge->projectController().createProject(QStringLiteral("Other project"),
        QStringLiteral("d"), QString::fromStdWString((setup.workspace.root / "project2").wstring()));
    require(!otherProject.isEmpty(), "The second project was not created");
    const QString ownRun = completedMockRun(setup, setup.projectId);
    const QString foreignRun = completedMockRun(setup, otherProject);
    const int runRows = setup.bridge->runController().model()->rowCount();

    // The picker offers the project's own Runs and never the other project's.
    const QVariantMap offeredResult = workshop.sourceRunChoices(setup.conversationId);
    require(offeredResult.value(QStringLiteral("ok")).toBool(), "The picker loads");
    const QVariantList offered = offeredResult.value(QStringLiteral("runs")).toList();
    require(offered.size() == 1 && offered.at(0).toMap().value(QStringLiteral("runId")).toString() == ownRun
            && offered.at(0).toMap().value(QStringLiteral("status")).toString() == QStringLiteral("completed")
            && !offered.at(0).toMap().value(QStringLiteral("title")).toString().isEmpty()
            && offered.at(0).toMap().value(QStringLiteral("createdAt")).toDateTime().isValid(),
        "The source-Run picker offers the conversation's project's Run only");
    const QVariantMap brokenPicker = workshop.sourceRunChoices(QStringLiteral("no-such-conversation"));
    require(!brokenPicker.value(QStringLiteral("ok")).toBool()
            && brokenPicker.value(QStringLiteral("errorCode")).toString() == QStringLiteral("run_draft_plan.conversation_not_found")
            && !brokenPicker.contains(QStringLiteral("runs")) && !workshop.lastError().isEmpty(),
        "...and fails for an unknown conversation with a typed result");
    // A failure is not wiped by the next successful call: the first answer carries its own error.
    require(brokenPicker.value(QStringLiteral("errorMessage")).toString() == workshop.lastError(),
        "The failed answer and lastError agree until the next success");
    require(workshop.rosterTemplates(setup.conversationId).value(QStringLiteral("ok")).toBool() && workshop.lastError().isEmpty(),
        "A later success clears lastError - which is why the answers, not lastError, carry the failure");

    const QVariantMap preview = workshop.previewFromMessage(setup.conversationId, messageId);
    const QString token = preview.value(QStringLiteral("previewToken")).toString();
    const auto expectRefused = [&](const QStringList& ids, const char* code, const char* what) {
        const QVariantMap result = workshop.createFromPreview(token, QString {}, ids);
        require(!result.value(QStringLiteral("ok")).toBool() && result.value(QStringLiteral("errorCode")).toString() == QLatin1String(code),
            what);
        require(setup.draftRows() == 0 && seen.created == 0, "...and nothing was written");
    };
    expectRefused({ownRun, foreignRun}, "run_draft_plan.source_run_wrong_project", "A Run of another project is refused");
    expectRefused({QStringLiteral("no-such-run")}, "run_draft_plan.source_run_not_found", "An unknown Run is refused");
    expectRefused({ownRun, ownRun}, "run_draft_plan.duplicate_source_run", "A Run named twice is refused");

    const QVariantMap created = workshop.createFromPreview(token, QString {}, {ownRun});
    require(created.value(QStringLiteral("ok")).toBool()
            && created.value(QStringLiteral("sourceRunIds")).toStringList() == QStringList({ownRun}) && seen.created == 1,
        "The project's own Run is recorded as the source");
    require(setup.draftRows() == 2, "Two drafts");
    require(setup.bridge->runController().model()->rowCount() == runRows, "Confirming the plan created no Run");
}

void testTheDraftsListAndOpenDraftDescribeAPlanDraft()
{
    Setup setup;
    auto& workshop = setup.workshop();
    const QString manualId = setup.createTemplateDraft();   // a manual roster draft
    const QString messageId = setup.addPlanMessage();
    const QVariantMap preview = workshop.previewFromMessage(setup.conversationId, messageId);
    const QVariantMap created = workshop.createFromPreview(preview.value(QStringLiteral("previewToken")).toString());
    require(created.value(QStringLiteral("ok")).toBool(), "setup: the plan is confirmed");
    const QVariantList rows = created.value(QStringLiteral("drafts")).toList();
    const QString modelId = rows.at(0).toMap().value(QStringLiteral("draftId")).toString();
    const QString viewId = rows.at(1).toMap().value(QStringLiteral("draftId")).toString();
    const QString groupId = created.value(QStringLiteral("groupId")).toString();

    // The plan draft says where it came from.
    require(draftRole(setup, viewId, "groupId").toString() == groupId
            && draftRole(setup, viewId, "groupTitle").toString() == QStringLiteral("History page")
            && draftRole(setup, viewId, "groupOrigin").toString() == QStringLiteral("chat_message")
            && draftRole(setup, viewId, "groupOrdinal").toInt() == 1
            && draftRole(setup, viewId, "groupDraftCount").toInt() == 2
            && draftRole(setup, viewId, "planKey").toString() == QStringLiteral("view")
            && draftRole(setup, viewId, "dependsOn").toStringList() == QStringList({QStringLiteral("model")})
            && draftRole(setup, viewId, "dependsOnTitles").toStringList() == QStringList({QStringLiteral("Model task")})
            && draftRole(setup, viewId, "sourceRunCount").toInt() == 0
            && !draftRole(setup, viewId, "edited").toBool(),
        "A plan draft carries its group, order, advisory dependency (as a title) and no edit mark");
    require(draftRole(setup, modelId, "groupOrdinal").toInt() == 0
            && draftRole(setup, modelId, "dependsOn").toStringList().isEmpty()
            && draftRole(setup, modelId, "dependsOnTitles").toStringList().isEmpty(),
        "The first draft depends on nothing");
    // A manual draft shows nothing of it.
    require(draftRole(setup, manualId, "groupId").toString().isEmpty() && draftRole(setup, manualId, "groupOrdinal").toInt() == -1
            && draftRole(setup, manualId, "groupTitle").toString().isEmpty() && draftRole(setup, manualId, "groupDraftCount").toInt() == 0
            && draftRole(setup, manualId, "dependsOn").toStringList().isEmpty() && !draftRole(setup, manualId, "edited").toBool(),
        "A manual draft has no group");

    // openDraft: the same story for New Run, only for a plan draft.
    const QVariantMap opened = setup.bridge->runDraftController().openDraft(viewId);
    require(opened.contains(QStringLiteral("planGroup")), "An opened plan draft carries planGroup");
    const QVariantMap group = opened.value(QStringLiteral("planGroup")).toMap();
    require(sortedKeys(group) == QStringList({QStringLiteral("dependsOn"), QStringLiteral("dependsOnTitles"), QStringLiteral("edited"),
                QStringLiteral("groupId"), QStringLiteral("ordinal"), QStringLiteral("origin"), QStringLiteral("planKey"),
                QStringLiteral("planTitle"), QStringLiteral("sourceRunIds")})
            && group.value(QStringLiteral("planTitle")).toString() == QStringLiteral("History page")
            && group.value(QStringLiteral("origin")).toString() == QStringLiteral("chat_message")
            && group.value(QStringLiteral("dependsOnTitles")).toStringList() == QStringList({QStringLiteral("Model task")})
            && !group.value(QStringLiteral("edited")).toBool(),
        "The planGroup map has exactly the documented fields");
    require(!setup.bridge->runDraftController().openDraft(manualId).contains(QStringLiteral("planGroup")),
        "A manual draft has no planGroup key");

    // "Edited since creation" follows the title, task, contracts and dependencies only.
    const auto update = [&](const QVariantMap& source, const QString& task, int maxParallelism) {
        return setup.bridge->runDraftController().updateDraft(source.value(QStringLiteral("draftId")).toString(),
            source.value(QStringLiteral("title")).toString(), source.value(QStringLiteral("projectId")).toString(),
            source.value(QStringLiteral("title")).toString(), task, source.value(QStringLiteral("launchMode")).toString(),
            QString {}, source.value(QStringLiteral("roster")).toList(), maxParallelism, QString {}, true,
            source.value(QStringLiteral("workflowId")).toString(), QVariantMap {},
            source.value(QStringLiteral("interfaceContracts")).toList());
    };
    require(update(opened, opened.value(QStringLiteral("task")).toString(), 6), "setup: a roster-side edit");
    require(!draftRole(setup, viewId, "edited").toBool(), "Changing only parallelism is not an edit of the plan content");
    require(update(opened, QStringLiteral("A different task."), 6), "setup: a task edit");
    require(draftRole(setup, viewId, "edited").toBool(), "Changing the task marks the draft as edited");
    require(setup.bridge->runDraftController().openDraft(viewId).value(QStringLiteral("planGroup")).toMap()
                .value(QStringLiteral("edited")).toBool(),
        "...also in the opened draft");
    require(update(opened, opened.value(QStringLiteral("task")).toString(), 6) && !draftRole(setup, viewId, "edited").toBool(),
        "Restoring the task clears the mark");
    require(!draftRole(setup, modelId, "edited").toBool(), "The sibling is untouched");

    // Removing the first draft: the survivor keeps its own number, the count says how many are left, and a dependency
    // on the removed draft says so instead of looking like a live one.
    require(setup.bridge->runDraftController().removeDraft(modelId), "setup: remove the first draft");
    require(draftRole(setup, viewId, "groupDraftCount").toInt() == 1 && draftRole(setup, viewId, "groupOrdinal").toInt() == 1,
        "The survivor is still draft number 2 and one draft remains");
    require(draftRole(setup, viewId, "dependsOnTitles").toStringList() == QStringList({QStringLiteral("model (removed draft)")})
            && draftRole(setup, viewId, "dependsOn").toStringList() == QStringList({QStringLiteral("model")}),
        "A dependency whose draft is gone is stated as removed; the plan key itself is unchanged");
    require(setup.bridge->runDraftController().openDraft(viewId).value(QStringLiteral("planGroup")).toMap()
                .value(QStringLiteral("dependsOnTitles")).toStringList() == QStringList({QStringLiteral("model (removed draft)")}),
        "...also in the opened draft");
}

} // namespace

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();
    QCoreApplication application(argc, argv);
    try {
        testTheMessageListCarriesTheBackendDecidedPlanEnvelopeState();
        testAPreviewWritesNothingAndCreateWritesDraftsOnly();
        testAManualPasteIsConfirmedByTokenOnly();
        testAStaffingHintIsProjectedForTheDialog();
        testFailuresAreTypedAndLeaveNothingBehind();
        testARosterTemplateIsCopiedAndOnlyForTheSameProject();
        testAMessageChangedAfterItsPreviewIsRefusedAsStale();
        testSourceRunsAreCheckedThroughTheRealRunRepository();
        testTheDraftsListAndOpenDraftDescribeAPlanDraft();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
