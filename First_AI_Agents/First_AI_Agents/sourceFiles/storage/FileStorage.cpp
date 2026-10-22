#include "ai/storage/FileStorage.hpp"

#include <fstream>
#include <sstream>

namespace ai::storage {

bool FileStorage::writeText(const std::filesystem::path& path, const std::string& content)
{
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }

    output << content;
    return static_cast<bool>(output);
}

std::optional<std::string> FileStorage::readText(const std::filesystem::path& path) const
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool FileStorage::exists(const std::filesystem::path& path) const
{
    return std::filesystem::exists(path);
}

} // namespace ai::storage
