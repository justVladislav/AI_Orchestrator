#pragma once

#include <ai/application/analytics/ExecutionAnalyticsService.hpp>
#include <ai/application/conversations/ChatService.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>

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
    // `analytics` is the Backend's execution analytics (the model-execution ledger projected per source). Chat cost is
    // read ONLY from it. When it is absent (a composition that did not provide it) the recorded cost is reported as
    // "unavailable" - never as the old conversation/message counters presented as exact.
    ChatController(ai::application::ChatService&, ai::application::ProviderCatalogService&,
        ProjectContextController&, ai::application::ExecutionAnalyticsService* analytics = nullptr,
        QObject* parent = nullptr);
    ~ChatController() override;

    [[nodiscard]] bool generating() const noexcept;
    [[nodiscard]] QString generatingConversationId() const;

    Q_INVOKABLE QVariantList conversations();
    Q_INVOKABLE QVariantList messages(const QString& conversationId);
    Q_INVOKABLE QVariantList providerChoices();
    Q_INVOKABLE QVariantList projectChoices();
    // Per provider/model usage of the conversation, built from the RECORDED executions (every attempt, including a retry
    // whose message row was overwritten). Each row: providerId, modelId, providerName, modelName, inputTokens,
    // outputTokens, totalTokens, executionCount (recorded attempts, not visible messages), costState
    // (none|known|partial|unknown), costText (the recorded amount, "" when no amount exists), unknownPriceExecutionCount,
    // averageLatency. Empty when the ledger cannot be read (the failure is reported through errorOccurred).
    Q_INVOKABLE QVariantList modelMetrics(const QString& conversationId);
    // The recorded cost of the whole conversation over the model-execution ledger: state (none|known|partial|unknown|
    // unavailable), costText ("" unless the state carries an amount), costMicros, executionCount,
    // unknownPriceExecutionCount, errorCode/errorMessage (unavailable only). It does NOT depend on the resettable
    // conversation counters, so clearing usage never removes it. An unreadable ledger is reported (errorOccurred).
    Q_INVOKABLE QVariantMap recordedCost(const QString& conversationId);
    // What a UI refresh calls: {cost: <recordedCost map>, models: <modelMetrics rows>} built from ONE ledger read, so the
    // header and the rows always agree and an unreadable ledger is reported exactly once. recordedCost() and modelMetrics()
    // are views of this method that each read (and report) on their own.
    Q_INVOKABLE QVariantMap recordedUsage(const QString& conversationId);
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
    ai::application::ExecutionAnalyticsService* analytics_ {nullptr};
    std::optional<std::jthread> worker_;
    QString generatingConversationId_;
};
