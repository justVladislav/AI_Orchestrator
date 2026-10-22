#pragma once

#include <ai/application/ChatService.hpp>
#include <ai/application/ProviderCatalogService.hpp>

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <stop_token>
#include <thread>

class ProjectContextController;

class ChatController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(QString generatingConversationId READ generatingConversationId NOTIFY generatingChanged)

public:
    ChatController(ai::application::ChatService&, ai::application::ProviderCatalogService&,
        ProjectContextController&, QObject* parent = nullptr);
    ~ChatController() override;

    [[nodiscard]] bool generating() const noexcept;
    [[nodiscard]] QString generatingConversationId() const;

    Q_INVOKABLE QVariantList conversations();
    Q_INVOKABLE QVariantList messages(const QString& conversationId);
    Q_INVOKABLE QVariantList providerChoices();
    Q_INVOKABLE QVariantList projectChoices();
    Q_INVOKABLE QVariantList modelMetrics(const QString& conversationId);
    Q_INVOKABLE QString createConversation(const QString& providerId, const QString& modelId);
    Q_INVOKABLE bool removeConversation(const QString& conversationId);
    Q_INVOKABLE bool setSelection(const QString& conversationId, const QString& providerId, const QString& modelId);
    Q_INVOKABLE bool setProject(const QString& conversationId, const QString& projectId);
    Q_INVOKABLE bool sendMessage(const QString& conversationId, const QString& content);
    // Quote is provider-free. It exposes metadata only (and the Backend's single-use `quoteToken`); the snapshot text
    // never crosses into QML. The send below needs the live token of a quote made for this conversation, snapshot and
    // model: without it the Backend refuses (chat_context.quote_required / quote_expired / stale_quote) and nothing is
    // written or sent.
    Q_INVOKABLE QVariantMap projectContextQuote(
        const QString& conversationId, const QString& snapshotFingerprint);
    Q_INVOKABLE bool sendMessageWithProjectContext(const QString& conversationId, const QString& content,
        const QString& snapshotFingerprint, const QString& quoteToken);
    Q_INVOKABLE bool retryMessage(const QString& messageId);
    Q_INVOKABLE bool editMessage(const QString& messageId, const QString& content);
    Q_INVOKABLE void stopGeneration();
    Q_INVOKABLE bool clearUsage(const QString& conversationId);
    void notifyChoicesChanged();

signals:
    void conversationsChanged();
    void messagesChanged(const QString& conversationId);
    void generatingChanged();
    void errorOccurred(const QString& code, const QString& message);
    void choicesChanged();

private:
    void startGeneration(QString conversationId, QString assistantMessageId, bool retry);
    void report(const ai::common::Error&);

    ai::application::ChatService& service_;
    ai::application::ProviderCatalogService& catalog_;
    ProjectContextController& projectContext_;
    std::optional<std::jthread> worker_;
    QString generatingConversationId_;
};
