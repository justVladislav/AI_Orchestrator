#pragma once

#include "ai/ui/ProviderExecutionAnalyticsModel.hpp"

#include <ai/application/ExecutionAnalyticsService.hpp>
#include <ai/application/ProviderCatalogService.hpp>

#include <QDateTime>
#include <QObject>

class ExecutionAnalyticsController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* providers READ providers CONSTANT)
    Q_PROPERTY(qulonglong executionCount READ executionCount NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong completedCount READ completedCount NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong failedCount READ failedCount NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong inputTokens READ inputTokens NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong outputTokens READ outputTokens NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong costMicros READ costMicros NOTIFY summaryChanged)
    // How completely the total cost is known ("none" | "known" | "partial" | "unknown"), the
    // Backend-formatted amount (empty when unknown: never "$0"), how many executions have an
    // incomplete price, and whether the total is incomplete. QML words the state; it neither
    // reads costMicros nor formats a number.
    Q_PROPERTY(QString priceState READ priceState NOTIFY summaryChanged)
    Q_PROPERTY(QString costText READ costText NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong unknownPriceExecutionCount READ unknownPriceExecutionCount NOTIFY summaryChanged)
    Q_PROPERTY(bool priceIncomplete READ priceIncomplete NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong averageLatencyMs READ averageLatencyMs NOTIFY summaryChanged)
    Q_PROPERTY(QString scopeId READ scopeId NOTIFY summaryChanged)

public:
    ExecutionAnalyticsController(ai::application::ExecutionAnalyticsService& analytics,
                                 ai::application::ProviderCatalogService& catalog,
                                 QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* providers() noexcept { return &providers_; }
    [[nodiscard]] qulonglong executionCount() const noexcept;
    [[nodiscard]] qulonglong completedCount() const noexcept;
    [[nodiscard]] qulonglong failedCount() const noexcept;
    [[nodiscard]] qulonglong inputTokens() const noexcept;
    [[nodiscard]] qulonglong outputTokens() const noexcept;
    [[nodiscard]] qulonglong costMicros() const noexcept;
    [[nodiscard]] QString priceState() const;
    [[nodiscard]] QString costText() const;
    [[nodiscard]] qulonglong unknownPriceExecutionCount() const noexcept;
    [[nodiscard]] bool priceIncomplete() const noexcept;
    [[nodiscard]] qulonglong averageLatencyMs() const noexcept;
    [[nodiscard]] QString scopeId() const { return scopeId_; }

    Q_INVOKABLE bool refreshProject(const QString& projectId);
    Q_INVOKABLE bool refreshProjectRange(const QString& projectId,
                                         const QDateTime& fromInclusive,
                                         const QDateTime& toExclusive);
    Q_INVOKABLE bool refreshSource(const QString& source, const QString& sourceId);
    Q_INVOKABLE void clear();

signals:
    void summaryChanged();
    void errorOccurred(const QString& code, const QString& message);

private:
    bool apply(ai::common::Result<ai::application::ExecutionAnalyticsSnapshot> result,
               const QString& scopeId);

    ai::application::ExecutionAnalyticsService& analytics_;
    ai::application::ProviderCatalogService& catalog_;
    ProviderExecutionAnalyticsModel providers_;
    ai::application::ExecutionAggregate totals_;
    QString scopeId_;
};
