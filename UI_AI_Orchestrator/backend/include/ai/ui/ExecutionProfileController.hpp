#pragma once

#include <ai/application/catalog/ExecutionProfileService.hpp>
#include <ai/ui/ExecutionProfileListModel.hpp>

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

/** Qt command/read-model adapter for persisted execution profiles. */
class ExecutionProfileController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* profiles READ profiles CONSTANT)
    Q_PROPERTY(QAbstractItemModel* runnableProfiles READ runnableProfiles CONSTANT)
    Q_PROPERTY(QString defaultProfileId READ defaultProfileId NOTIFY defaultProfileIdChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ExecutionProfileController(
        ai::application::ExecutionProfileService& service,
        QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* profiles() noexcept;
    [[nodiscard]] QAbstractItemModel* runnableProfiles() noexcept;
    [[nodiscard]] QString defaultProfileId() const;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE QString createProfile(
        const QString& name,
        const QString& workflowId,
        const QVariantList& assignments,
        const QVariantMap& providerBudget = {});
    Q_INVOKABLE bool updateProfile(
        const QString& profileId,
        const QString& name,
        const QString& workflowId,
        const QVariantList& assignments,
        const QVariantMap& providerBudget = {});
    Q_INVOKABLE bool setDefaultProfile(const QString& profileId);
    Q_INVOKABLE void refresh();

signals:
    void profilesChanged();
    void defaultProfileIdChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    [[nodiscard]] static std::vector<ai::application::StageModelAssignment>
    parseAssignments(const QVariantList& assignments);
    [[nodiscard]] static ai::application::ProviderExecutionBudgetOverrides
    parseProviderBudget(const QVariantMap& providerBudget);
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::ExecutionProfileService& service_;
    ExecutionProfileListModel profiles_;
    ExecutionProfileListModel runnableProfiles_;
    QString defaultProfileId_;
    QString lastError_;
};
