#pragma once

#include "ai/common/Result.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ai::application {

// Trusted Backend configuration for read-only model access. None of these
// fields may be populated from model arguments.
struct WorkspaceReadPolicy {
    std::filesystem::path workspaceRoot;
    std::vector<std::filesystem::path> allowedRoots;
    // When true, allowedRoots is an exact file/path allow-list rather than
    // a set of directory prefixes. Manifest workers enable this so an
    // unlisted workspace file stays inactive until an audited expansion is
    // approved. Defaults to the legacy prefix behaviour for other callers.
    bool allowedRootsAreExactPaths {false};
    std::vector<std::filesystem::path> deniedPaths;
    std::vector<std::string> deniedDirectoryNames;
    // Whole, consecutive directory components matched at any workspace depth.
    // For example, x64/Debug excludes conventional MSVC output without
    // excluding every directory named Debug or every architecture directory.
    std::vector<std::filesystem::path> deniedDirectorySequences;
    std::vector<std::string> deniedFileNames;
    std::vector<std::string> deniedExtensions;

    std::size_t maxFileBytes {256ULL * 1024};
    std::size_t maxDirectoryEntries {500};
    std::size_t maxSearchMatches {200};
    std::size_t maxResultBytes {256ULL * 1024};

    [[nodiscard]] static WorkspaceReadPolicy secureDefaults(
        std::filesystem::path trustedWorkspaceRoot);
};

// Pure lexical guard. Infrastructure must additionally perform canonical
// containment and reparse-point checks immediately before every filesystem
// operation.
class WorkspaceReadPolicyValidator {
public:
    [[nodiscard]] static bool matchesDeniedDirectorySequence(
        const std::filesystem::path& relativePath,
        const WorkspaceReadPolicy& policy);

    // True when the path is at or below one of policy.deniedPaths: the lexically normalised path is compared per component, ASCII
    // case-insensitively on Windows, so a neighbour sharing only a string prefix (Out2x for Out2) does not match. The very test
    // validateRelativePath applies; exposed so the write side shares it instead of keeping a second copy.
    [[nodiscard]] static bool matchesDeniedPath(
        const std::filesystem::path& relativePath,
        const WorkspaceReadPolicy& policy);

    [[nodiscard]] static common::Result<void> validateTrustedPolicy(
        const WorkspaceReadPolicy& policy);

    [[nodiscard]] static common::Result<std::filesystem::path> validateRelativePath(
        const std::filesystem::path& modelPath,
        const WorkspaceReadPolicy& policy);
};

} // namespace ai::application
