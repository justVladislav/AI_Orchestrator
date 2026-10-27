#pragma once

#include "ai/application/settings/IApplicationSettingsRepository.hpp"
#include "ai/application/IVerificationScratchSettingRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace ai::infrastructure {

/** SQLite implementation of the verification-folder setting port (table verification_scratch_setting, schema step 69). */
class SqliteVerificationScratchSettingRepository final : public application::IVerificationScratchSettingRepository {
public:
    explicit SqliteVerificationScratchSettingRepository(std::shared_ptr<SqliteDatabase> database);

    [[nodiscard]] common::Result<std::optional<std::filesystem::path>> load() const override;
    common::Result<void> store(const std::optional<std::filesystem::path>& root) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteApplicationSettingsRepository final
    : public application::IApplicationSettingsRepository {
public:
    explicit SqliteApplicationSettingsRepository(
        std::shared_ptr<SqliteDatabase> database);
    [[nodiscard]] common::Result<application::ApplicationSettings> load() const override;
    common::Result<application::ApplicationSettings> save(
        application::ApplicationSettings value) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
