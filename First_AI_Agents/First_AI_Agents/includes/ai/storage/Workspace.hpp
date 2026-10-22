#pragma once

#include "ai/storage/FileStorage.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace ai::storage {

// Knows the project workspace layout and keeps file paths out of main/orchestration code.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::filesystem::path inputDir() const;
    [[nodiscard]] std::filesystem::path runsDir() const;
    [[nodiscard]] std::filesystem::path outputDir() const;

    bool ensure() const;
    bool writeOutput(const std::string& filename, const std::string& content);
    [[nodiscard]] std::optional<std::string> readInputTask(const std::string& filename = "task.md") const;

private:
    std::filesystem::path root_;
    FileStorage storage_;
};

} // namespace ai::storage
