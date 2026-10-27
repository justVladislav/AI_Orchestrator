#pragma once

#include "ai/application/catalog/ProviderCatalogTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

/** Persistence port for provider connection metadata (never credentials). */
class IProviderRepository {
public:
    virtual ~IProviderRepository() = default;

    virtual common::Result<Provider> insert(Provider provider) = 0;
    virtual common::Result<Provider> update(Provider provider) = 0;
    [[nodiscard]] virtual common::Result<std::optional<Provider>> findById(
        const ProviderId& id) const = 0;
    // Deterministic order (createdAt, then id), matching every other list
    // query in this codebase.
    [[nodiscard]] virtual common::Result<std::vector<Provider>> listAll() const = 0;
};

} // namespace ai::application
