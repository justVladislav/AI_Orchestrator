#include <ai/ui/CredentialController.hpp>

#include <algorithm>

CredentialController::CredentialController(
    ai::application::CredentialManagementService& service,
    ProviderCatalogController& catalog,
    QObject* parent)
    : QObject(parent)
    , service_(service)
    , catalog_(catalog)
{
}

QString CredentialController::lastError() const { return lastError_; }

bool CredentialController::setCredential(const QString& providerId, const QString& secret)
{
    if (secret.isEmpty()) {
        reportError({"credential.empty_secret", "Credential cannot be empty"});
        return false;
    }
    std::string transientSecret = secret.toStdString();
    const auto result = service_.setCredential(providerId.toStdString(), transientSecret);
    // The QML TextField clears itself when the dialog closes. Independently
    // clear this mutable adapter-side copy immediately after the synchronous
    // Application command returns.
    std::fill(transientSecret.begin(), transientSecret.end(), '\0');
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    catalog_.refresh();
    emit credentialChanged(providerId, true);
    return true;
}

bool CredentialController::removeCredential(const QString& providerId)
{
    const auto result = service_.removeCredential(providerId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    catalog_.refresh();
    emit credentialChanged(providerId, false);
    return true;
}

bool CredentialController::isConfigured(const QString& providerId)
{
    const auto result = service_.credentialStatus(providerId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    return result.value() == ai::application::CredentialStatus::Configured;
}

void CredentialController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void CredentialController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
