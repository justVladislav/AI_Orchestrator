#include "ai/application/workspace/OutputDirectoryInspector.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ai::application {

namespace fs = std::filesystem;

namespace detail {

bool isSymlinkOrReparsePoint(const fs::path& path, std::error_code& error)
{
    error.clear();
    const fs::file_status status = fs::symlink_status(longPath(path), error);
    if (error) return false;
    if (status.type() == fs::file_type::symlink) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(longPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

} // namespace detail

namespace {

bool isCMakeListsName(const std::string& name)
{
#ifdef _WIN32
    return detail::asciiLower(name) == "cmakelists.txt";
#else
    return name == "CMakeLists.txt";
#endif
}

struct PendingDirectory {
    fs::path path;
    std::string relative; // generic UTF-8, "" for the inspected folder
    std::size_t depth {0};
};

} // namespace

OutputDirectoryInspection FilesystemOutputDirectoryInspector::inspect(
    const fs::path& directory, const OutputDirectoryInspectionLimits& limits, std::stop_token stopToken) const
{
    OutputDirectoryInspection result;
    const auto fail = [&result](std::string reason) {
        result.status = OutputInspectionStatus::Failed;
        result.failure = std::move(reason);
        return result;
    };
    const auto cancelled = [&result] {
        result.status = OutputInspectionStatus::Cancelled;
        return result;
    };
    bool incomplete = false;

    std::vector<PendingDirectory> stack;
    stack.push_back({directory, std::string(), 0});
    while (!stack.empty()) {
        if (stopToken.stop_requested()) return cancelled();
        PendingDirectory current = std::move(stack.back());
        stack.pop_back();

        std::error_code listError;
        fs::directory_iterator iterator(detail::longPath(current.path), fs::directory_options::none, listError);
        if (listError) return fail("a folder could not be listed");
        for (const fs::directory_iterator end; iterator != end; iterator.increment(listError)) {
            if (listError) return fail("a folder could not be listed completely");
            if (stopToken.stop_requested()) return cancelled();
            if (result.entries >= limits.maxEntries) {
                result.hitEntryLimit = true;
                incomplete = true;
                break;
            }
            ++result.entries;

            const fs::path entryPath = iterator->path();
            std::error_code typeError;
            const bool link = detail::isSymlinkOrReparsePoint(entryPath, typeError);
            if (typeError) return fail("the type of an entry could not be read");
            if (link) {
                ++result.skippedLinks;
                incomplete = true;
                continue;
            }
            const fs::file_status status = fs::symlink_status(detail::longPath(entryPath), typeError);
            if (typeError) return fail("the type of an entry could not be read");
            const std::string name = detail::genericUtf8(entryPath.filename());
            const std::string relative = current.relative.empty() ? name : current.relative + "/" + name;

            if (status.type() == fs::file_type::directory) {
                if (current.depth + 1 >= limits.maxDepth) {
                    // The folder is at the deepest level that is looked at: its content is not seen.
                    result.hitDepthLimit = true;
                    incomplete = true;
                } else {
                    stack.push_back({entryPath, relative, current.depth + 1});
                }
                continue;
            }
            if (status.type() == fs::file_type::regular) {
                ++result.files;
                std::error_code sizeError;
                const auto size = fs::file_size(detail::longPath(entryPath), sizeError);
                if (sizeError) return fail("the size of a file could not be read");
                result.bytes += size;
                // A CMakeLists.txt directly in the inspected folder is the caller's separate, harder refusal.
                if (current.depth >= 1 && isCMakeListsName(name)) {
                    ++result.nestedSourceCount;
                    if (result.nestedSourceSamples.size() < limits.maxSamples) result.nestedSourceSamples.push_back(relative);
                }
            }
        }
        // An increment that failed ends the loop like the end of the folder does: it must not pass for a complete listing.
        if (listError) return fail("a folder could not be listed completely");
        if (result.hitEntryLimit) break;
    }
    result.status = incomplete ? OutputInspectionStatus::Incomplete : OutputInspectionStatus::Complete;
    return result;
}

} // namespace ai::application
