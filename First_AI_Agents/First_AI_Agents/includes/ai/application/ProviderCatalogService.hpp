#pragma once

#include "ai/application/IModelRepository.hpp"
#include "ai/application/IProviderRepository.hpp"

namespace ai::application {

/**
 * Validates and coordinates Provider/Model catalog commands. Never talks to
 * SQL directly - every operation goes through IProviderRepository/
 * IModelRepository. Never accepts, returns, or logs a credential value:
 * only Provider::credentialReference (an opaque pointer into a future
 * ICredentialStore) ever crosses this boundary.
 */
class ProviderCatalogService {
public:
    ProviderCatalogService(IProviderRepository& providers, IModelRepository& models);

    [[nodiscard]] common::Result<std::vector<Provider>> listProviders() const;
    [[nodiscard]] common::Result<std::optional<Provider>> findProvider(const ProviderId& id) const;
    common::Result<ProviderId> createProvider(CreateProviderRequest request);
    common::Result<void> updateProvider(UpdateProviderRequest request);
    common::Result<void> setProviderEnabled(const ProviderId& id, bool enabled);

    [[nodiscard]] common::Result<std::vector<Model>> listModels(
        std::optional<ProviderId> providerId = std::nullopt) const;
    [[nodiscard]] common::Result<std::optional<Model>> findModel(const ModelId& id) const;
    common::Result<ModelId> createModel(CreateModelRequest request);
    common::Result<void> updateModel(UpdateModelRequest request);
    // Rejects disabling a model that is currently its provider's default
    // ("model.default_must_stay_enabled"): callers must set a different
    // default (or clear it by picking another model) before disabling this
    // one, so "default Model must be enabled" is never violated even
    // momentarily.
    common::Result<void> setModelEnabled(const ModelId& id, bool enabled);
    // Requires modelId to belong to providerId and to be enabled before it
    // may become the provider's default.
    common::Result<void> setDefaultModel(const ProviderId& providerId, const ModelId& modelId);
    // Rejects deleting the provider's current default model
    // ("model.default_cannot_be_deleted") for the same reason
    // setModelEnabled() rejects disabling it - pick a different default
    // first. Fails with "model.in_use" when the model still has execution
    // history, saved profiles, drafts, or chats referencing it (enforced by
    // the repository's own foreign keys, not re-checked here).
    common::Result<void> deleteModel(const ModelId& id);

private:
    IProviderRepository& providers_;
    IModelRepository& models_;
};

} // namespace ai::application
