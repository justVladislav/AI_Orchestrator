#include <ai/ui/ChatController.hpp>

#include <ai/ui/ProjectContextController.hpp>

#include <ai/application/planning/RunDraftPlanService.hpp>

#include <QDateTime>
#include <QMetaObject>

#include <algorithm>
#include <map>

namespace {

QString relativeTime(std::chrono::system_clock::time_point value)
{
    const auto then = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count());
    const qint64 seconds = then.secsTo(QDateTime::currentDateTime());
    if (seconds < 60) return QObject::tr("Just now");
    if (seconds < 3600) return QObject::tr("%1 min ago").arg(seconds / 60);
    if (seconds < 86400) return QObject::tr("%1 hours ago").arg(seconds / 3600);
    return QLocale().toString(then.date(), QLocale::ShortFormat);
}

QVariantMap conversationMap(const ai::application::ChatConversation& value)
{
    return {{"conversationId", QString::fromStdString(value.id)},
            {"title", QString::fromStdString(value.title)}, {"preview", QString::fromStdString(value.preview)},
            {"updatedAt", relativeTime(value.updatedAt)}, {"unreadCount", 0},
            {"pinned", value.pinned}, {"archived", value.archived},
            {"providerId", QString::fromStdString(value.providerId)}, {"modelId", QString::fromStdString(value.modelId)},
            {"projectId", value.projectId.has_value() ? QString::fromStdString(*value.projectId) : QString {}},
            {"inputTokens", QVariant::fromValue<qulonglong>(value.inputTokens)},
            {"outputTokens", QVariant::fromValue<qulonglong>(value.outputTokens)},
            {"estimatedCost", QStringLiteral("$%1").arg(value.estimatedCostMicros / 1'000'000.0, 0, 'f', 4)},
            {"lastResponseTime", value.lastLatencyMs > 0 ? QObject::tr("%1 ms").arg(value.lastLatencyMs) : QString {}}};
}

QVariantMap messageMap(const ai::application::ChatMessage& value)
{
    return {{"messageId", QString::fromStdString(value.id)},
            {"role", QString::fromLatin1(ai::application::toString(value.role).data(), static_cast<int>(ai::application::toString(value.role).size()))},
            {"content", QString::fromStdString(value.content)},
            {"timestamp", QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(value.createdAt.time_since_epoch()).count()).toString(QStringLiteral("hh:mm"))},
            {"status", QString::fromLatin1(ai::application::toString(value.status).data(), static_cast<int>(ai::application::toString(value.status).size()))},
            {"tokenCount", QVariant::fromValue<qulonglong>(value.outputTokens)},
            {"contextFingerprint", value.projectContext.has_value()
                ? QString::fromStdString(value.projectContext->snapshotFingerprint) : QString {}},
            {"contextTokens", QVariant::fromValue<qulonglong>(value.projectContext.has_value()
                ? value.projectContext->renderedEstimatedTokens : 0)},
            // PC-3C: "none" | "one" | "multiple" | "too_large", decided by the Backend from the stored text (QML never
            // scans a message): "none" unless this is a completed Assistant message holding a next-run-plan.v1 envelope.
            {"planEnvelope", QString::fromLatin1(ai::application::toString(ai::application::planEnvelopeStateOf(value)))}};
}

}

ChatController::ChatController(ai::application::ChatService& service,
    ai::application::ProviderCatalogService& catalog, ProjectContextController& projectContext, QObject* parent)
    : QObject(parent), service_(service), catalog_(catalog), projectContext_(projectContext)
{
}

ChatController::~ChatController()
{
    if (worker_) { worker_->request_stop(); worker_->join(); }
}

bool ChatController::generating() const noexcept { return worker_.has_value(); }
QString ChatController::generatingConversationId() const { return generatingConversationId_; }

void ChatController::report(const ai::common::Error& value)
{
    emit errorOccurred(QString::fromStdString(value.code), QString::fromStdString(value.message));
}

QVariantList ChatController::conversations()
{
    const auto result = service_.listConversations(); QVariantList rows;
    if (!result.success()) { report(*result.error()); return rows; }
    for (const auto& value : result.value()) rows.push_back(conversationMap(value));
    return rows;
}

QVariantList ChatController::messages(const QString& id)
{
    const auto result = service_.listMessages(id.toStdString()); QVariantList rows;
    if (!result.success()) { report(*result.error()); return rows; }
    for (const auto& value : result.value()) rows.push_back(messageMap(value));
    return rows;
}

QVariantList ChatController::providerChoices()
{
    const auto providers = catalog_.listProviders(); const auto models = catalog_.listModels(); QVariantList rows;
    if (!providers.success()) { report(*providers.error()); return rows; }
    if (!models.success()) { report(*models.error()); return rows; }
    for (const auto& provider : providers.value()) {
        for (const auto& model : models.value()) {
            if (model.providerId != provider.id) continue;
            const bool available = provider.enabled && model.enabled;
            const QString status = !available ? tr("Disabled")
                : provider.credentialReference.has_value() ? tr("Ready") : tr("Credential required");
            rows.push_back(QVariantMap {{"providerId", QString::fromStdString(provider.id)}, {"providerName", QString::fromStdString(provider.name)},
                {"modelId", QString::fromStdString(model.id)}, {"modelName", QString::fromStdString(model.displayName)},
                {"available", available}, {"statusText", status}});
        }
    }
    return rows;
}

QVariantList ChatController::projectChoices()
{
    QVariantList rows;
    rows.push_back(QVariantMap {{"projectId", QString {}}, {"projectName", tr("No project context")}});
    const auto result = service_.listAvailableProjects();
    if (!result.success()) { report(*result.error()); return rows; }
    for (const auto& project : result.value())
        rows.push_back(QVariantMap {{"projectId", QString::fromStdString(project.id)}, {"projectName", QString::fromStdString(project.name)}});
    return rows;
}

QVariantList ChatController::modelMetrics(const QString& conversationId)
{
    struct Totals { qulonglong input=0, output=0, cost=0, latency=0, responses=0; std::string providerId; };
    const auto messagesResult = service_.listMessages(conversationId.toStdString());
    if (!messagesResult.success()) { report(*messagesResult.error()); return {}; }
    std::map<std::string, Totals> totals;
    for (const auto& message : messagesResult.value()) {
        if (message.modelId.empty() || message.role != ai::application::ChatRole::Assistant) continue;
        auto& value = totals[message.modelId]; value.providerId = message.providerId;
        value.input += message.inputTokens; value.output += message.outputTokens;
        value.cost += message.costMicros; value.latency += message.latencyMs; ++value.responses;
    }
    QVariantList rows;
    for (const auto& [modelId, value] : totals) {
        const auto model = catalog_.findModel(modelId); const auto provider = catalog_.findProvider(value.providerId);
        rows.push_back(QVariantMap {
            {"providerId", QString::fromStdString(value.providerId)}, {"modelId", QString::fromStdString(modelId)},
            {"providerName", provider.success() && provider.value() ? QString::fromStdString(provider.value()->name) : tr("Unknown provider")},
            {"modelName", model.success() && model.value() ? QString::fromStdString(model.value()->displayName) : tr("Unknown model")},
            {"inputTokens", value.input}, {"outputTokens", value.output}, {"totalTokens", value.input + value.output},
            {"estimatedCost", QStringLiteral("$%1").arg(value.cost / 1'000'000.0, 0, 'f', 4)},
            {"averageLatency", value.responses ? tr("%1 ms").arg(value.latency / value.responses) : QString {}},
            {"responseCount", value.responses}});
    }
    return rows;
}

QString ChatController::createConversation(const QString& providerId, const QString& modelId)
{
    auto result = service_.createConversation({std::nullopt, providerId.toStdString(), modelId.toStdString(), "New chat"});
    if (!result.success()) { report(*result.error()); return {}; }
    emit conversationsChanged(); return QString::fromStdString(result.value().id);
}

bool ChatController::removeConversation(const QString& id)
{
    auto result = service_.removeConversation(id.toStdString()); if (!result.success()) { report(*result.error()); return false; }
    emit conversationsChanged(); return true;
}

bool ChatController::setSelection(const QString& id, const QString& provider, const QString& model)
{
    auto result = service_.setSelection(id.toStdString(), provider.toStdString(), model.toStdString());
    if (!result.success()) { report(*result.error()); return false; } emit conversationsChanged(); return true;
}

bool ChatController::setProject(const QString& id, const QString& projectId)
{
    const std::optional<ai::application::ProjectId> value = projectId.isEmpty()
        ? std::nullopt : std::optional<ai::application::ProjectId> {projectId.toStdString()};
    auto result = service_.setProject(id.toStdString(), value);
    if (!result.success()) { report(*result.error()); return false; }
    emit conversationsChanged(); return true;
}

bool ChatController::sendMessage(const QString& id, const QString& content)
{
    if (worker_) { emit errorOccurred(QStringLiteral("chat.busy"), tr("Wait for the current response or stop it first.")); return false; }
    auto prepared = service_.prepareTurn(id.toStdString(), content.toStdString());
    if (!prepared.success()) { report(*prepared.error()); return false; }
    emit messagesChanged(id); emit conversationsChanged();
    startGeneration(id, QString::fromStdString(prepared.value().assistantMessage.id), false); return true;
}

QVariantMap ChatController::projectContextQuote(const QString& id, const QString& fingerprint)
{
    const auto conversations = service_.listConversations();
    if (!conversations.success()) return {{"ok", false}, {"errorCode", QString::fromStdString(conversations.error()->code)},
        {"errorMessage", QString::fromStdString(conversations.error()->message)}};
    const auto found = std::find_if(conversations.value().begin(), conversations.value().end(),
        [&id](const auto& value) { return value.id == id.toStdString(); });
    if (found == conversations.value().end() || !found->projectId.has_value()) {
        return {{"ok", false}, {"errorCode", QStringLiteral("chat_context.project_mismatch")},
            {"errorMessage", tr("This conversation has no matching project context.")}};
    }
    const auto snapshot = projectContext_.snapshotForAttachment(*found->projectId, fingerprint.toStdString());
    if (!snapshot.success()) return {{"ok", false}, {"errorCode", QString::fromStdString(snapshot.error()->code)},
        {"errorMessage", QString::fromStdString(snapshot.error()->message)}};
    const auto quote = service_.quoteProjectContext(id.toStdString(), snapshot.value());
    if (!quote.success()) return {{"ok", false}, {"errorCode", QString::fromStdString(quote.error()->code)},
        {"errorMessage", QString::fromStdString(quote.error()->message)}};
    return {{"ok", true}, {"projectId", QString::fromStdString(quote.value().projectId)},
        {"fingerprint", QString::fromStdString(quote.value().snapshotFingerprint)},
        {"quoteToken", QString::fromStdString(quote.value().quoteToken)},
        {"estimatedInputTokens", QVariant::fromValue<qulonglong>(quote.value().estimatedInputTokens)},
        {"estimatedCostMicros", QVariant::fromValue<qulonglong>(quote.value().estimatedCostMicros)},
        {"priceKnown", quote.value().priceKnown},
        {"estimatedCost", quote.value().priceKnown
            ? QStringLiteral("$%1").arg(quote.value().estimatedCostMicros / 1'000'000.0, 0, 'f', 6)
            : tr("price unavailable")}};
}

bool ChatController::sendMessageWithProjectContext(
    const QString& id, const QString& content, const QString& fingerprint, const QString& quoteToken)
{
    if (worker_) { emit errorOccurred(QStringLiteral("chat.busy"), tr("Wait for the current response or stop it first.")); return false; }
    const auto conversations = service_.listConversations();
    if (!conversations.success()) { report(*conversations.error()); return false; }
    const auto found = std::find_if(conversations.value().begin(), conversations.value().end(),
        [&id](const auto& value) { return value.id == id.toStdString(); });
    if (found == conversations.value().end() || !found->projectId.has_value()) {
        report({"chat_context.project_mismatch", "This conversation has no matching project context"});
        return false;
    }
    const auto snapshot = projectContext_.snapshotForAttachment(*found->projectId, fingerprint.toStdString());
    if (!snapshot.success()) { report(*snapshot.error()); return false; }
    auto prepared = service_.prepareTurnWithProjectContext(
        id.toStdString(), content.toStdString(), snapshot.value(), quoteToken.toStdString());
    if (!prepared.success()) { report(*prepared.error()); return false; }
    emit messagesChanged(id); emit conversationsChanged();
    startGeneration(id, QString::fromStdString(prepared.value().assistantMessage.id), false);
    return true;
}

bool ChatController::retryMessage(const QString& messageId)
{
    if (worker_) return false;
    const auto conversationsResult = service_.listConversations();
    if (!conversationsResult.success()) { report(*conversationsResult.error()); return false; }
    for (const auto& c : conversationsResult.value()) {
        const auto ms = service_.listMessages(c.id); if (!ms.success()) continue;
        for (const auto& m : ms.value()) if (m.id == messageId.toStdString()) { startGeneration(QString::fromStdString(c.id), messageId, true); return true; }
    }
    emit errorOccurred(QStringLiteral("chat.message_not_found"), tr("Message not found")); return false;
}

bool ChatController::editMessage(const QString& id, const QString& content)
{
    auto result = service_.editUserMessage(id.toStdString(), content.toStdString()); if (!result.success()) { report(*result.error()); return false; }
    emit messagesChanged(QString::fromStdString(result.value().conversationId)); return true;
}

void ChatController::startGeneration(QString conversationId, QString assistantId, bool retry)
{
    generatingConversationId_ = conversationId; emit generatingChanged();
    worker_.emplace([this, conversationId, assistantId, retry](std::stop_token token) {
        auto result = retry ? service_.retry(assistantId.toStdString(), token) : service_.generate(assistantId.toStdString(), token);
        QMetaObject::invokeMethod(this, [this, conversationId, result = std::move(result)]() mutable {
            if (!result.success()) report(*result.error());
            worker_.reset(); generatingConversationId_.clear(); emit messagesChanged(conversationId); emit conversationsChanged(); emit generatingChanged();
        }, Qt::QueuedConnection);
    });
}

void ChatController::stopGeneration()
{
    if (worker_) worker_->request_stop();
}

bool ChatController::clearUsage(const QString& id)
{
    auto result = service_.clearUsage(id.toStdString()); if (!result.success()) { report(*result.error()); return false; }
    emit conversationsChanged(); return true;
}

void ChatController::notifyChoicesChanged() { emit choicesChanged(); }
