#pragma once

#include <ai/application/providers/CredentialManagementService.hpp>
#include <ai/ui/ProviderCatalogController.hpp>

#include <QObject>
#include <QString>

/**
 * Write-only Qt credential boundary. Secrets enter setCredential() and are
 * never retained, returned to QML, placed in a model role, or logged.
 */
class CredentialController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    CredentialController(
        ai::application::CredentialManagementService& service,
        ProviderCatalogController& catalog,
        QObject* parent = nullptr);

    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE bool setCredential(const QString& providerId, const QString& secret);
    Q_INVOKABLE bool removeCredential(const QString& providerId);
    Q_INVOKABLE bool isConfigured(const QString& providerId);

signals:
    void credentialChanged(const QString& providerId, bool configured);
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::CredentialManagementService& service_;
    ProviderCatalogController& catalog_;
    QString lastError_;
};
