#pragma once

#include "ai/application/IExecutionProfileRepository.hpp"
#include "ai/application/IModelRepository.hpp"
#include "ai/application/IProviderRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace ai::infrastructure {

/** SQLite implementation of the provider catalog's provider persistence port. */
class SqliteProviderRepository final : public application::IProviderRepository {
public:
    explicit SqliteProviderRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Provider> insert(application::Provider provider) override;
    common::Result<application::Provider> update(application::Provider provider) override;
    [[nodiscard]] common::Result<std::optional<application::Provider>> findById(
        const application::ProviderId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::Provider>> listAll() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the provider catalog's model persistence port. */
class SqliteModelRepository final : public application::IModelRepository {
public:
    explicit SqliteModelRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Model> insert(application::Model model) override;
    common::Result<application::Model> update(application::Model model) override;
    [[nodiscard]] common::Result<std::optional<application::Model>> findById(
        const application::ModelId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::Model>> listAll(
        const std::optional<application::ProviderId>& providerId = std::nullopt) const override;
    common::Result<void> setDefaultModel(
        const application::ProviderId& providerId,
        const application::ModelId& modelId) override;
    common::Result<void> remove(const application::ModelId& id) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of execution profiles and their assignment lists. */
class SqliteExecutionProfileRepository final : public application::IExecutionProfileRepository {
public:
    explicit SqliteExecutionProfileRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::ExecutionProfile> insert(application::ExecutionProfile profile) override;
    common::Result<application::ExecutionProfile> update(application::ExecutionProfile profile) override;
    [[nodiscard]] common::Result<std::optional<application::ExecutionProfile>> findById(
        const application::ExecutionProfileId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ExecutionProfile>> listAll() const override;
    common::Result<void> setDefaultProfile(const application::ExecutionProfileId& profileId) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
