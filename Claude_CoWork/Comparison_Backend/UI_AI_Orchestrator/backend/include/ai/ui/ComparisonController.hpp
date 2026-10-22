#pragma once

#include <ai/application/ComparisonService.hpp>
#include <ai/application/ProjectService.hpp>
#include <ai/application/ProviderCatalogService.hpp>
#include <ai/ui/ComparisonListModel.hpp>
#include <ai/ui/ComparisonProjectsModel.hpp>
#include <ai/ui/ComparisonResultListModel.hpp>

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <stop_token>
#include <thread>

/**
 * Qt command/read-model adapter for ComparisonService.
 *
 * Owns the three Qt list models exposed to QML and drives parallel provider
 * execution on a background jthread when a comparison session has Pending
 * results. GUI-thread model updates are posted via QMetaObject::invokeMethod
 * (Qt::QueuedConnection) so the worker thread never touches Qt objects directly.
 */
class ComparisonController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* projectsModel    READ projectsModel    CONSTANT)
    Q_PROPERTY(QAbstractItemModel* comparisonListModel READ comparisonListModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* resultListModel  READ resultListModel  CONSTANT)
    Q_PROPERTY(QString selectedProjectId    READ selectedProjectId    NOTIFY selectedProjectIdChanged)
    Q_PROPERTY(QString selectedComparisonId READ selectedComparisonId NOTIFY selectedComparisonIdChanged)
    Q_PROPERTY(QString selectedComparisonUpdatedAt READ selectedComparisonUpdatedAt NOTIFY selectedComparisonIdChanged)
    Q_PROPERTY(bool isLoading              READ isLoading              NOTIFY isLoadingChanged)
    Q_PROPERTY(QString lastError           READ lastError             NOTIFY lastErrorChanged)

public:
    explicit ComparisonController(
        ai::application::ComparisonService& service,
        ai::application::ProjectService& projectService,
        ai::application::ProviderCatalogService& catalogService,
        QObject* parent = nullptr);
    ~ComparisonController() override;

    [[nodiscard]] QAbstractItemModel* projectsModel() noexcept;
    [[nodiscard]] QAbstractItemModel* comparisonListModel() noexcept;
    [[nodiscard]] QAbstractItemModel* resultListModel() noexcept;
    [[nodiscard]] QString selectedProjectId() const;
    [[nodiscard]] QString selectedComparisonId() const;
    [[nodiscard]] QString selectedComparisonUpdatedAt() const;
    [[nodiscard]] bool isLoading() const;
    [[nodiscard]] QString lastError() const;

    // ── Selection ──────────────────────────────────────────────────────────
    Q_INVOKABLE void selectProject(const QString& projectId);
    Q_INVOKABLE void selectComparison(const QString& comparisonId);

    // ── Result mutations (called from QML via delegate actions) ───────────
    Q_INVOKABLE void markAsBest(const QString& comparisonId, const QString& resultId);
    Q_INVOKABLE void toggleSynthesisSelection(const QString& resultId);

    // Resets the result to Pending and enqueues it for re-execution.
    Q_INVOKABLE void retryResult(const QString& resultId);

    // Triggers a refresh of the result list for the current comparison.
    Q_INVOKABLE void refresh();

    // Returns all results of the current comparison as a QVariantList of
    // QVariantMap objects. Used by QML to compute derived aggregations
    // (summaryData, statusText) without having to iterate a C++ model.
    Q_INVOKABLE QVariantList resultsSnapshot() const;

signals:
    void selectedProjectIdChanged();
    void selectedComparisonIdChanged();
    void isLoadingChanged();
    void lastErrorChanged();
    void resultsUpdated(const QString& comparisonId);
    void operationFailed(const QString& code, const QString& message);

private:
    void loadProjects();
    void loadComparisons(const QString& projectId);
    void loadResults(const QString& comparisonId);
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::ComparisonService& service_;
    ai::application::ProjectService& projectService_;
    ai::application::ProviderCatalogService& catalogService_;

    ComparisonProjectsModel projectsModel_;
    ComparisonListModel comparisonListModel_;
    ComparisonResultListModel resultListModel_;

    QString selectedProjectId_;
    QString selectedComparisonId_;
    bool isLoading_ { false };
    QString lastError_;
};
