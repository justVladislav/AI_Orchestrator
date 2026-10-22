#pragma once

#include "ai/storage/IStorage.hpp"

namespace ai::storage {

// Small filesystem adapter used so higher layers do not open fstreams directly.
class FileStorage final : public IStorage {
public:
    bool writeText(const std::filesystem::path& path, const std::string& content) override;
    [[nodiscard]] std::optional<std::string> readText(const std::filesystem::path& path) const override;
    [[nodiscard]] bool exists(const std::filesystem::path& path) const override;
};

} // namespace ai::storage
