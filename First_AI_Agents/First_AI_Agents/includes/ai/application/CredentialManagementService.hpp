#pragma once

#include "ai/application/ICredentialStore.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"

namespace ai::application {

enum class CredentialStatus { NotConfigured, Configured };

/**
 * The only Application-layer command surface a future Qt credential adapter
 * should use to set/replace/remove a provider's secret. It never reads a
 * secret back out - credentialStatus() reports only configured/not
 * configured, never the value, so a UI can never accidentally display or
 * log it.
 *
 * Coordinates two systems that cannot share one transaction (ICredentialStore
 * and the Provider catalog's own persistence), so it applies compensation in
 * the direction that avoids ever claiming "configured" falsely - see
 * setCredential()/removeCredential() for the exact ordering and rollback
 * behavior.
 */
class CredentialManagementService {
public:
    CredentialManagementService(ICredentialStore& store, ProviderCatalogService& catalog);

    // Writes secret first, then updates Provider::credentialReference to
    // point at it. If the metadata update fails after a successful write,
    // the just-written secret is removed again so credentialStatus() never
    // reports Configured for a provider whose metadata update never actually
    // committed.
    common::Result<void> setCredential(const ProviderId& providerId, const std::string& secret);

    // Removes the secret first, then clears Provider::credentialReference.
    // If the store removal itself fails, metadata is left untouched (still
    // Configured) - a truthful "still has a secret" beats a false
    // "not configured" while an actual secret still exists in the store.
    // If removal succeeds but the metadata clear fails, credentialStatus()
    // may keep reporting Configured even though the secret is gone; this is
    // the one non-atomic edge this design cannot fully close without
    // fabricating a placeholder secret, and it fails with a distinct code
    // ("credential.metadata_clear_failed") so a caller can safely retry just
    // the metadata clear (idempotent).
    common::Result<void> removeCredential(const ProviderId& providerId);

    // Never touches ICredentialStore::load() - status is derived only from
    // whether Provider::credentialReference is set.
    [[nodiscard]] common::Result<CredentialStatus> credentialStatus(const ProviderId& providerId) const;

private:
    [[nodiscard]] static std::string referenceFor(const ProviderId& providerId);

    ICredentialStore& store_;
    ProviderCatalogService& catalog_;
};

} // namespace ai::application
