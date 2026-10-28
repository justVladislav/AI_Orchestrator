#include "ai/application/providers/CredentialManagementService.hpp"

#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

UpdateProviderRequest requestFromProvider(const Provider& provider, std::optional<std::string> credentialReference)
{
    UpdateProviderRequest request;
    request.id = provider.id;
    request.name = provider.name;
    request.kind = provider.kind;
    request.baseUrl = provider.baseUrl;
    request.credentialReference = std::move(credentialReference);
    return request;
}

} // namespace

CredentialManagementService::CredentialManagementService(ICredentialStore& store, ProviderCatalogService& catalog)
    : store_(store)
    , catalog_(catalog)
{
}

std::string CredentialManagementService::referenceFor(const ProviderId& providerId)
{
    // Deterministic and derived only from the (non-secret) provider id, so
    // the reference itself never needs to be persisted separately from
    // Provider::credentialReference and never depends on the secret value.
    return "provider:" + providerId;
}

common::Result<void> CredentialManagementService::setCredential(
    const ProviderId& providerId, const std::string& secret)
{
    const auto found = catalog_.findProvider(providerId);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("provider.not_found", "Provider not found"));
    }

    const std::string reference = referenceFor(providerId);
    const auto stored = store_.store(reference, secret);
    if (!stored.success()) {
        return common::Result<void>::failure(*stored.error());
    }

    const auto updated = catalog_.updateProvider(requestFromProvider(*found.value(), reference));
    if (!updated.success()) {
        // Roll back the just-written secret so status() cannot report
        // Configured for metadata that never actually committed.
        static_cast<void>(store_.remove(reference));
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> CredentialManagementService::removeCredential(const ProviderId& providerId)
{
    const auto found = catalog_.findProvider(providerId);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("provider.not_found", "Provider not found"));
    }
    if (!found.value()->credentialReference.has_value()) {
        // Idempotent: nothing configured, nothing to remove.
        return common::Result<void>::ok();
    }

    const auto removed = store_.remove(*found.value()->credentialReference);
    if (!removed.success()) {
        // Leave metadata untouched: a secret still exists in the store, so
        // "still Configured" remains the truthful answer.
        return common::Result<void>::failure(*removed.error());
    }

    const auto updated = catalog_.updateProvider(requestFromProvider(*found.value(), std::nullopt));
    if (!updated.success()) {
        return common::Result<void>::failure(error(
            "credential.metadata_clear_failed",
            "Secret was removed but clearing Provider.credentialReference failed: " + updated.error()->message));
    }
    return common::Result<void>::ok();
}

common::Result<CredentialStatus> CredentialManagementService::credentialStatus(const ProviderId& providerId) const
{
    const auto found = catalog_.findProvider(providerId);
    if (!found.success()) {
        return common::Result<CredentialStatus>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<CredentialStatus>::failure(error("provider.not_found", "Provider not found"));
    }
    return common::Result<CredentialStatus>::ok(
        found.value()->credentialReference.has_value() ? CredentialStatus::Configured : CredentialStatus::NotConfigured);
}

} // namespace ai::application
