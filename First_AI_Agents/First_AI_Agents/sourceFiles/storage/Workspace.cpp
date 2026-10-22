#include "ai/storage/Workspace.hpp"

#include <utility>

namespace ai::storage {

Workspace::Workspace(std::filesystem::path root)
    : root_(std::move(root))
{
}

const std::filesystem::path& Workspace::root() const noexcept
{
    return root_;
}

std::filesystem::path Workspace::inputDir() const
{
    return root_ / "input";
}

std::filesystem::path Workspace::runsDir() const
{
    return root_ / "runs";
}

std::filesystem::path Workspace::outputDir() const
{
    return root_ / "output";
}

bool Workspace::ensure() const
{
    // These folders are the stable contract for inputs, intermediate runs, and outputs.
    std::filesystem::create_directories(inputDir());
    std::filesystem::create_directories(runsDir());
    std::filesystem::create_directories(outputDir());
    return std::filesystem::exists(inputDir())
        && std::filesystem::exists(runsDir())
        && std::filesystem::exists(outputDir());
}

bool Workspace::writeOutput(const std::string& filename, const std::string& content)
{
    return storage_.writeText(outputDir() / filename, content);
}

std::optional<std::string> Workspace::readInputTask(const std::string& filename) const
{
    return storage_.readText(inputDir() / filename);
}

} // namespace ai::storage
