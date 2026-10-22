#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ai::storage {

class IStorage {
public:
    virtual ~IStorage() = default;

    virtual bool writeText(const std::filesystem::path& path, const std::string& content) = 0;
    [[nodiscard]] virtual std::optional<std::string> readText(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual bool exists(const std::filesystem::path& path) const = 0;
};

} // namespace ai::storage
