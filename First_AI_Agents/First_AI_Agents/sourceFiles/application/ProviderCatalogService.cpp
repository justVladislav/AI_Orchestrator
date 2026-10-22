#include "ai/application/ProviderCatalogService.hpp"

#include "ai/application/IdGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

bool hasDuplicateApiModelId(
    const std::vector<Model>& providerModels,
    const std::string& apiModelId,
    const ModelId& excludeId)
{
    return std::any_of(providerModels.begin(), providerModels.end(), [&](const Model& model) {
        return model.id != excludeId && model.apiModelId == apiModelId;
    });
}

// The rules of a long-context price tier (see checkLongContextPricing, shared with the
// SQLite layer), checked against the model as it would be SAVED - the context window
// included - so neither a create nor an update that only lowers the window can leave an
// unreachable or unpriced tier behind.
common::Result<void> validateLongContextPricing(
    std::uint64_t contextWindowTokens,
    const LongContextPricing& pricing)
{
    const auto problem = checkLongContextPricing(contextWindowTokens, pricing);
    if (problem == LongContextPricingProblem::None) {
        return common::Result<void>::ok();
    }
    return common::Result<void>::failure(error(
        longContextPricingProblemCode(problem), longContextPricingProblemMessage(problem)));
}

} // namespace

ProviderCatalogService::ProviderCatalogService(IProviderRepository& providers, IModelRepository& models)
    : providers_(providers)
    , models_(models)
{
}

common::Result<std::vector<Provider>> ProviderCatalogService::listProviders() const
{
    return providers_.listAll();
}

common::Result<std::optional<Provider>> ProviderCatalogService::findProvider(const ProviderId& id) const
{
    return providers_.findById(id);
}

common::Result<ProviderId> ProviderCatalogService::createProvider(CreateProviderRequest request)
{
    if (request.name.empty()) {
        return common::Result<ProviderId>::failure(
            error("provider.empty_name", "Provider name cannot be empty"));
    }
    if (!isKnownProviderKind(request.kind)) {
        return common::Result<ProviderId>::failure(
            error("provider.unknown_kind", "Provider kind must be one of: openai, anthropic, openai_compatible, local"));
    }

    Provider provider;
    provider.id = generateUuid();
    provider.name = std::move(request.name);
    provider.kind = std::move(request.kind);
    provider.baseUrl = std::move(request.baseUrl);
    provider.enabled = request.enabled;
    provider.credentialReference = std::move(request.credentialReference);
    provider.createdAt = std::chrono::system_clock::now();
    provider.updatedAt = provider.createdAt;

    const auto inserted = providers_.insert(std::move(provider));
    if (!inserted.success()) {
        return common::Result<ProviderId>::failure(*inserted.error());
    }
    return common::Result<ProviderId>::ok(inserted.value().id);
}

common::Result<void> ProviderCatalogService::updateProvider(UpdateProviderRequest request)
{
    if (request.name.empty()) {
        return common::Result<void>::failure(
            error("provider.empty_name", "Provider name cannot be empty"));
    }
    if (!isKnownProviderKind(request.kind)) {
        return common::Result<void>::failure(
            error("provider.unknown_kind", "Provider kind must be one of: openai, anthropic, openai_compatible, local"));
    }

    const auto found = providers_.findById(request.id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("provider.not_found", "Provider not found"));
    }

    Provider provider = *found.value();
    provider.name = std::move(request.name);
    provider.kind = std::move(request.kind);
    provider.baseUrl = std::move(request.baseUrl);
    provider.credentialReference = std::move(request.credentialReference);
    provider.updatedAt = std::chrono::system_clock::now();

    const auto updated = providers_.update(std::move(provider));
    if (!updated.success()) {
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> ProviderCatalogService::setProviderEnabled(const ProviderId& id, bool enabled)
{
    const auto found = providers_.findById(id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("provider.not_found", "Provider not found"));
    }

    // Disabling a provider never touches its models: they remain exactly as
    // they were and simply become unusable for new work until re-enabled.
    Provider provider = *found.value();
    provider.enabled = enabled;
    provider.updatedAt = std::chrono::system_clock::now();

    const auto updated = providers_.update(std::move(provider));
    if (!updated.success()) {
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<Model>> ProviderCatalogService::listModels(
    std::optional<ProviderId> providerId) const
{
    return models_.listAll(providerId);
}

common::Result<std::optional<Model>> ProviderCatalogService::findModel(const ModelId& id) const
{
    return models_.findById(id);
}

common::Result<ModelId> ProviderCatalogService::createModel(CreateModelRequest request)
{
    if (request.apiModelId.empty()) {
        return common::Result<ModelId>::failure(
            error("model.empty_api_model_id", "Model apiModelId cannot be empty"));
    }

    const auto provider = providers_.findById(request.providerId);
    if (!provider.success()) {
        return common::Result<ModelId>::failure(*provider.error());
    }
    if (!provider.value().has_value()) {
        return common::Result<ModelId>::failure(error("provider.not_found", "Provider not found"));
    }

    const auto longContext =
        validateLongContextPricing(request.contextWindowTokens, request.longContextPricing);
    if (!longContext.success()) {
        return common::Result<ModelId>::failure(*longContext.error());
    }

    const auto existingModels = models_.listAll(request.providerId);
    if (!existingModels.success()) {
        return common::Result<ModelId>::failure(*existingModels.error());
    }
    if (hasDuplicateApiModelId(existingModels.value(), request.apiModelId, /*excludeId=*/{})) {
        return common::Result<ModelId>::failure(error(
            "model.duplicate_api_model_id",
            "A model with this apiModelId already exists for this provider"));
    }

    Model model;
    model.id = generateUuid();
    model.providerId = std::move(request.providerId);
    model.apiModelId = std::move(request.apiModelId);
    model.displayName = std::move(request.displayName);
    model.contextWindowTokens = request.contextWindowTokens;
    model.maxOutputTokens = request.maxOutputTokens;
    model.inputPriceMicrosPerMillionTokens = request.inputPriceMicrosPerMillionTokens;
    model.outputPriceMicrosPerMillionTokens = request.outputPriceMicrosPerMillionTokens;
    model.cacheWritePriceMicrosPerMillionTokens = request.cacheWritePriceMicrosPerMillionTokens;
    model.cacheReadPriceMicrosPerMillionTokens = request.cacheReadPriceMicrosPerMillionTokens;
    model.longContextPricing = request.longContextPricing;
    model.cacheWriteOneHourPriceMicrosPerMillionTokens =
        request.cacheWriteOneHourPriceMicrosPerMillionTokens;
    model.enabled = request.enabled;
    model.isDefault = false; // Only setDefaultModel() may set this.
    model.createdAt = std::chrono::system_clock::now();
    model.updatedAt = model.createdAt;

    const auto inserted = models_.insert(std::move(model));
    if (!inserted.success()) {
        return common::Result<ModelId>::failure(*inserted.error());
    }
    return common::Result<ModelId>::ok(inserted.value().id);
}

common::Result<void> ProviderCatalogService::updateModel(UpdateModelRequest request)
{
    if (request.apiModelId.empty()) {
        return common::Result<void>::failure(
            error("model.empty_api_model_id", "Model apiModelId cannot be empty"));
    }

    const auto found = models_.findById(request.id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }

    Model model = *found.value();
    const auto siblingModels = models_.listAll(model.providerId);
    if (!siblingModels.success()) {
        return common::Result<void>::failure(*siblingModels.error());
    }
    if (hasDuplicateApiModelId(siblingModels.value(), request.apiModelId, model.id)) {
        return common::Result<void>::failure(error(
            "model.duplicate_api_model_id",
            "A model with this apiModelId already exists for this provider"));
    }

    model.apiModelId = std::move(request.apiModelId);
    model.displayName = std::move(request.displayName);
    model.contextWindowTokens = request.contextWindowTokens;
    model.maxOutputTokens = request.maxOutputTokens;
    model.inputPriceMicrosPerMillionTokens = request.inputPriceMicrosPerMillionTokens;
    model.outputPriceMicrosPerMillionTokens = request.outputPriceMicrosPerMillionTokens;
    model.cacheWritePriceMicrosPerMillionTokens = request.cacheWritePriceMicrosPerMillionTokens;
    model.cacheReadPriceMicrosPerMillionTokens = request.cacheReadPriceMicrosPerMillionTokens;
    if (request.cacheWriteOneHourPriceMicrosPerMillionTokens.has_value()) {
        model.cacheWriteOneHourPriceMicrosPerMillionTokens =
            *request.cacheWriteOneHourPriceMicrosPerMillionTokens;
    }
    // std::nullopt leaves the stored tier untouched; a value replaces it.
    if (request.longContextPricing.has_value()) {
        model.longContextPricing = *request.longContextPricing;
    }
    // Validated on the model as it will be saved, so lowering the context window
    // below an existing tier threshold is refused as well.
    const auto longContext =
        validateLongContextPricing(model.contextWindowTokens, model.longContextPricing);
    if (!longContext.success()) {
        return common::Result<void>::failure(*longContext.error());
    }
    model.updatedAt = std::chrono::system_clock::now();

    const auto updated = models_.update(std::move(model));
    if (!updated.success()) {
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> ProviderCatalogService::setModelEnabled(const ModelId& id, bool enabled)
{
    const auto found = models_.findById(id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }

    Model model = *found.value();
    if (!enabled && model.isDefault) {
        return common::Result<void>::failure(error(
            "model.default_must_stay_enabled",
            "Select a different default model before disabling the current default"));
    }
    model.enabled = enabled;
    model.updatedAt = std::chrono::system_clock::now();

    const auto updated = models_.update(std::move(model));
    if (!updated.success()) {
        return common::Result<void>::failure(*updated.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> ProviderCatalogService::deleteModel(const ModelId& id)
{
    const auto found = models_.findById(id);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }
    if (found.value()->isDefault) {
        return common::Result<void>::failure(error(
            "model.default_cannot_be_deleted",
            "Select a different default model before deleting the current default"));
    }
    return models_.remove(id);
}

common::Result<void> ProviderCatalogService::setDefaultModel(
    const ProviderId& providerId,
    const ModelId& modelId)
{
    const auto found = models_.findById(modelId);
    if (!found.success()) {
        return common::Result<void>::failure(*found.error());
    }
    if (!found.value().has_value()) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }
    if (found.value()->providerId != providerId) {
        return common::Result<void>::failure(error(
            "model.provider_mismatch", "Model does not belong to the given provider"));
    }
    if (!found.value()->enabled) {
        return common::Result<void>::failure(error(
            "model.disabled_cannot_be_default", "A disabled model cannot become the provider default"));
    }

    return models_.setDefaultModel(providerId, modelId);
}

} // namespace ai::application
