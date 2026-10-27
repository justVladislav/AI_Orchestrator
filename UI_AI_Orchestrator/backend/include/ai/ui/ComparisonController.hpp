#pragma once

#include "ai/ui/ComparisonResultListModel.hpp"
#include "ai/ui/ComparisonSessionListModel.hpp"

#include <ai/application/comparison/ComparisonReadModelService.hpp>
#include <ai/application/comparison/ComparisonService.hpp>
#include <ai/application/comparison/ComparisonExecutionManager.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>

#include <QObject>
#include <QVariantMap>
#include <optional>

class ComparisonController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* sessionsModel READ sessionsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* resultsModel READ resultsModel CONSTANT)
    Q_PROPERTY(QString selectedProjectId READ selectedProjectId NOTIFY stateChanged)
    Q_PROPERTY(QString selectedSessionId READ selectedSessionId NOTIFY stateChanged)
    Q_PROPERTY(QString currentSessionStatus READ currentSessionStatus NOTIFY stateChanged)
    Q_PROPERTY(QString lastUpdatedText READ lastUpdatedText NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap summaryData READ summaryData NOTIFY stateChanged)
    Q_PROPERTY(QVariantList modelChoices READ modelChoices NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(bool canCreateComparison READ canCreateComparison NOTIFY stateChanged)
    Q_PROPERTY(bool canRefresh READ canRefresh NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)

public:
    ComparisonController(ai::application::ComparisonService& comparisons,
        ai::application::ComparisonReadModelService& readModel,
        ai::application::ProviderCatalogService& catalog,
        ai::application::ComparisonExecutionManager& executionManager,
        QObject* parent = nullptr);
    ~ComparisonController() override;

    [[nodiscard]] QAbstractItemModel* sessionsModel() noexcept { return &sessions_; }
    [[nodiscard]] QAbstractItemModel* resultsModel() noexcept { return &results_; }
    [[nodiscard]] QString selectedProjectId() const { return selectedProjectId_; }
    [[nodiscard]] QString selectedSessionId() const { return selectedSessionId_; }
    [[nodiscard]] QString currentSessionStatus() const;
    [[nodiscard]] QString lastUpdatedText() const;
    [[nodiscard]] QVariantMap summaryData() const;
    [[nodiscard]] QVariantList modelChoices() const;
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] QString errorText() const { return errorText_; }
    [[nodiscard]] bool canCreateComparison() const noexcept;
    [[nodiscard]] bool canRefresh() const noexcept;
    [[nodiscard]] bool canCancel() const noexcept;

    Q_INVOKABLE bool selectProject(const QString& projectId);
    Q_INVOKABLE bool selectSession(const QString& sessionId);
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE QString createComparison(const QVariantMap& request);
    Q_INVOKABLE bool cancelComparison();
    Q_INVOKABLE bool setPreferredResult(const QString& resultId);
    Q_INVOKABLE bool setResultSelected(const QString& resultId, bool selected);
    Q_INVOKABLE void notifyCatalogChanged() { emit stateChanged(); }

signals:
    void stateChanged();
    void errorOccurred(const QString& code, const QString& message);

private:
    bool loadSessions(bool preserveSelection);
    bool loadCurrent();
    void clearCurrent();
    bool fail(const ai::common::Error& error);
    void clearError();

    ai::application::ComparisonService& comparisons_;
    ai::application::ComparisonReadModelService& readModel_;
    ai::application::ProviderCatalogService& catalog_;
    ai::application::ComparisonExecutionManager& executionManager_;
    ComparisonSessionListModel sessions_;
    ComparisonResultListModel results_;
    QString selectedProjectId_;
    QString selectedSessionId_;
    QString errorText_;
    bool busy_ {false};
    std::optional<ai::application::ComparisonSessionReadModel> current_;
};
