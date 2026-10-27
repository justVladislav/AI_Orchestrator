#pragma once

#include "ai/application/catalog/ProviderCatalogTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

/** Persistence port for model metadata under a Provider. */
class IModelRepository {
public:
    virtual ~IModelRepository() = default;

    virtual common::Result<Model> insert(Model model) = 0;
    virtual common::Result<Model> update(Model model) = 0;
    [[nodiscard]] virtual common::Result<std::optional<Model>> findById(const ModelId& id) const = 0;
    // Deterministic order (createdAt, then id). providerId filters to one
    // provider's models when set; std::nullopt lists every model.
    [[nodiscard]] virtual common::Result<std::vector<Model>> listAll(
        const std::optional<ProviderId>& providerId = std::nullopt) const = 0;

    // Atomically unsets any previous default model for providerId (if one
    // exists) and sets modelId as the new default, or leaves both untouched
    // on failure (e.g. modelId does not belong to providerId). Implementations
    // must never leave two models under the same provider marked default,
    // and must never leave zero committed changes plus a partially-applied
    // unset.
    virtual common::Result<void> setDefaultModel(
        const ProviderId& providerId,
        const ModelId& modelId) = 0;

    // Fails with "model.in_use" (not a generic write failure) when the model
    // is still referenced by any run/comparison/chat execution history,
    // execution profile assignment, run stage assignment, or run draft
    // roster entry - every such table's model_id column is
    // ON DELETE RESTRICT by design, so this can never silently orphan a
    // historical record.
    virtual common::Result<void> remove(const ModelId& id) = 0;
};

} // namespace ai::application
