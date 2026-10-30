#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace ai::application {

// P3a: what the user is told - and must confirm - about a folder before it is declared generated output. The walk reads METADATA only
// (names, types, sizes); it never opens a file, never follows a link and never creates anything.

struct OutputDirectoryInspectionLimits {
    // Entries (files and folders) looked at before the walk stops and reports Incomplete.
    std::size_t maxEntries {20'000};
    // Depth below the inspected folder (its children are depth 1) that is entered; deeper folders are reported, not entered.
    std::size_t maxDepth {12};
    // Relative paths of nested CMakeLists.txt kept as samples for the preview.
    std::size_t maxSamples {10};
};

enum class OutputInspectionStatus {
    Complete,   // every entry was seen: the counts are exact
    Incomplete, // a limit was reached or a link was not entered: every count is "at least"
    Failed,     // an entry could not be listed or its type read (access denied, I/O): nothing may be concluded
    Cancelled,  // the stop token was requested: the partial result is not a result
};

struct OutputDirectoryInspection {
    OutputInspectionStatus status {OutputInspectionStatus::Complete};
    std::size_t entries {0};
    std::size_t files {0};
    std::uint64_t bytes {0};
    // CMakeLists.txt files BELOW the inspected folder (the one directly in it is a separate, harder refusal checked by the caller).
    std::size_t nestedSourceCount {0};
    std::vector<std::string> nestedSourceSamples; // relative to the inspected folder, UTF-8, generic separators
    bool hitEntryLimit {false};
    bool hitDepthLimit {false};
    std::size_t skippedLinks {0}; // symlinks / junctions / reparse points that were neither entered nor counted
    std::string failure;          // for Failed: a short reason without file content
};

class IOutputDirectoryInspector {
public:
    virtual ~IOutputDirectoryInspector() = default;

    // Synchronous and cancellable; the caller (a future asynchronous controller) owns the thread. `directory` is an existing,
    // already-vetted folder.
    [[nodiscard]] virtual OutputDirectoryInspection inspect(const std::filesystem::path& directory,
        const OutputDirectoryInspectionLimits& limits, std::stop_token stopToken) const = 0;
};

class FilesystemOutputDirectoryInspector final : public IOutputDirectoryInspector {
public:
    [[nodiscard]] OutputDirectoryInspection inspect(const std::filesystem::path& directory,
        const OutputDirectoryInspectionLimits& limits, std::stop_token stopToken) const override;
};

namespace detail {

// True for a symbolic link and, on Windows, for ANY reparse point (junction, mount point, cloud placeholder), read from the file
// attribute itself - not left to the standard library's iterator. An entry whose attributes cannot be read sets `error`.
[[nodiscard]] bool isSymlinkOrReparsePoint(const std::filesystem::path& path, std::error_code& error);

} // namespace detail

} // namespace ai::application
