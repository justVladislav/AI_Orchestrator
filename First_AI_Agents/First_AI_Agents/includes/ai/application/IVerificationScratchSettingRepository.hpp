#pragma once

#include "ai/common/Result.hpp"

#include <filesystem>
#include <mutex>
#include <optional>

namespace ai::application {

/** Persistence port for the ONE per-installation setting "folder for the temporary verification copies" (no value = the default). */
class IVerificationScratchSettingRepository {
public:
    virtual ~IVerificationScratchSettingRepository() = default;

    [[nodiscard]] virtual common::Result<std::optional<std::filesystem::path>> load() const = 0;
    // nullopt clears the setting (back to the default).
    virtual common::Result<void> store(const std::optional<std::filesystem::path>& root) = 0;
};

/** Thread-safe development/test adapter. */
class InMemoryVerificationScratchSettingRepository final : public IVerificationScratchSettingRepository {
public:
    [[nodiscard]] common::Result<std::optional<std::filesystem::path>> load() const override
    {
        std::lock_guard lock(mutex_);
        return common::Result<std::optional<std::filesystem::path>>::ok(root_);
    }
    common::Result<void> store(const std::optional<std::filesystem::path>& root) override
    {
        std::lock_guard lock(mutex_);
        root_ = root;
        return common::Result<void>::ok();
    }

private:
    mutable std::mutex mutex_;
    std::optional<std::filesystem::path> root_;
};

} // namespace ai::application
