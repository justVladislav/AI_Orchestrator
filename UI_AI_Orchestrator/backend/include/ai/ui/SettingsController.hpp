#pragma once

#include <ai/application/ApplicationSettingsService.hpp>
#include <ai/application/ExecutionProfileService.hpp>
#include <ai/application/ProviderCatalogService.hpp>
#include <ai/application/VerificationScratchService.hpp>

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtTypes>

/**
 * Qt projection and command adapter for the single durable application
 * settings record. Validation and persistence remain in the Application
 * Service; QML receives only typed values and success/failure signals.
 */
class SettingsController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString themeMode READ themeMode NOTIFY settingsChanged)
    Q_PROPERTY(int interfaceScalePercent READ interfaceScalePercent NOTIFY settingsChanged)
    Q_PROPERTY(bool compactMode READ compactMode NOTIFY settingsChanged)
    Q_PROPERTY(bool reduceAnimations READ reduceAnimations NOTIFY settingsChanged)
    Q_PROPERTY(QString runContextPolicy READ runContextPolicy NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultExecutionProfileId READ defaultExecutionProfileId NOTIFY settingsChanged)
    Q_PROPERTY(qulonglong defaultTimeoutMs READ defaultTimeoutMs NOTIFY settingsChanged)
    Q_PROPERTY(int defaultMaxParallelAgents READ defaultMaxParallelAgents NOTIFY settingsChanged)
    Q_PROPERTY(bool confirmManyModels READ confirmManyModels NOTIFY settingsChanged)
    Q_PROPERTY(QString chatContextPolicy READ chatContextPolicy NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultChatProviderId READ defaultChatProviderId NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultChatModelId READ defaultChatModelId NOTIFY settingsChanged)
    Q_PROPERTY(int chatHistoryLimit READ chatHistoryLimit NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList executionProfileChoices READ executionProfileChoices NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList chatModelChoices READ chatModelChoices NOTIFY choicesChanged)
    // The folder for the TEMPORARY verification copies (see VerificationScratchService). The path the user set (empty = the default), the
    // folder a verification would use now, whether it is usable and, if not, why - all decided by the Backend.
    Q_PROPERTY(QString verificationScratchRoot READ verificationScratchRoot NOTIFY verificationScratchChanged)
    Q_PROPERTY(QString verificationScratchEffective READ verificationScratchEffective NOTIFY verificationScratchChanged)
    Q_PROPERTY(QString verificationScratchSource READ verificationScratchSource NOTIFY verificationScratchChanged)
    Q_PROPERTY(bool verificationScratchUsable READ verificationScratchUsable NOTIFY verificationScratchChanged)
    Q_PROPERTY(QString verificationScratchProblem READ verificationScratchProblem NOTIFY verificationScratchChanged)
    Q_PROPERTY(int verificationScratchMaxLength READ verificationScratchMaxLength CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    SettingsController(
        ai::application::ApplicationSettingsService& settings,
        ai::application::ExecutionProfileService& profiles,
        ai::application::ProviderCatalogService& catalog,
        ai::application::VerificationScratchService& verificationScratch,
        QObject* parent = nullptr);

    [[nodiscard]] QString themeMode() const;
    [[nodiscard]] int interfaceScalePercent() const noexcept;
    [[nodiscard]] bool compactMode() const noexcept;
    [[nodiscard]] bool reduceAnimations() const noexcept;
    [[nodiscard]] QString runContextPolicy() const;
    [[nodiscard]] QString defaultExecutionProfileId() const;
    [[nodiscard]] qulonglong defaultTimeoutMs() const noexcept;
    [[nodiscard]] int defaultMaxParallelAgents() const noexcept;
    [[nodiscard]] bool confirmManyModels() const noexcept;
    [[nodiscard]] QString chatContextPolicy() const;
    [[nodiscard]] QString defaultChatProviderId() const;
    [[nodiscard]] QString defaultChatModelId() const;
    [[nodiscard]] int chatHistoryLimit() const noexcept;
    [[nodiscard]] QVariantList executionProfileChoices() const;
    [[nodiscard]] QVariantList chatModelChoices() const;
    [[nodiscard]] QString verificationScratchRoot() const;
    [[nodiscard]] QString verificationScratchEffective() const;
    [[nodiscard]] QString verificationScratchSource() const;
    [[nodiscard]] bool verificationScratchUsable() const noexcept;
    [[nodiscard]] QString verificationScratchProblem() const;
    [[nodiscard]] int verificationScratchMaxLength() const noexcept;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE void refresh();
    // Catalog changes affect choices, not the stored settings or verification folder.
    void refreshChoices();
    Q_INVOKABLE bool setThemeMode(const QString& value);
    Q_INVOKABLE bool setInterfaceScalePercent(int value);
    Q_INVOKABLE bool setCompactMode(bool value);
    Q_INVOKABLE bool setReduceAnimations(bool value);
    Q_INVOKABLE bool setRunContextPolicy(const QString& value);
    Q_INVOKABLE bool setDefaultExecutionProfileId(const QString& value);
    Q_INVOKABLE bool setDefaultTimeoutMs(qulonglong value);
    Q_INVOKABLE bool setDefaultMaxParallelAgents(int value);
    Q_INVOKABLE bool setConfirmManyModels(bool value);
    Q_INVOKABLE bool setChatContextPolicy(const QString& value);
    Q_INVOKABLE bool setDefaultChatModel(const QString& providerId, const QString& modelId);
    Q_INVOKABLE bool setChatHistoryLimit(int value);
    // Validates the folder in the Backend (a missing folder is created private) and stores it; an empty string goes back to the default.
    // A refused folder leaves the previous setting and reports the Backend's own message.
    Q_INVOKABLE bool setVerificationScratchRoot(const QString& path);

signals:
    void settingsChanged();
    void choicesChanged();
    void verificationScratchChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    [[nodiscard]] ai::application::UpdateApplicationSettingsRequest requestFromCache() const;
    bool save(ai::application::UpdateApplicationSettingsRequest request);
    void refreshVerificationScratch();
    void report(const ai::common::Error& error);
    void clearError();

    ai::application::ApplicationSettingsService& service_;
    ai::application::ExecutionProfileService& profiles_;
    ai::application::ProviderCatalogService& catalog_;
    ai::application::VerificationScratchService& verificationScratch_;
    ai::application::VerificationScratchService::Status scratchStatus_;
    ai::application::ApplicationSettings value_;
    QVariantList executionProfileChoices_;
    QVariantList chatModelChoices_;
    QString lastError_;
};
