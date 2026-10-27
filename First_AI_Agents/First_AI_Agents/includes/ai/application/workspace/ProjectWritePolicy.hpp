#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ai::application {

// Defines what a ProposedChangeSet is allowed to do.
// This struct is supplied exclusively by the trusted Backend/Application
// caller; it is NEVER accepted from model output.
//
// Defaults are intentionally restrictive: delete and rename are disabled,
// the allowed-extensions list is empty (any extension permitted when empty),
// and the byte budget is conservative.  The caller tightens or relaxes
// these based on the project's operational security posture.
struct ProjectWritePolicy {
    // Absolute canonical path to the project root.  Every relative path in a
    // proposal is resolved against this and must resolve inside it.
    std::filesystem::path projectRoot;

    // When non-empty, only paths equal to or nested below one of these roots
    // are permitted. Each entry may therefore identify one exact file or a
    // directory subtree and is relative to projectRoot.
    // An empty list means the whole projectRoot is accessible (subject to
    // deniedPaths).
    std::vector<std::filesystem::path> allowedRoots;

    // Manifest workspace scopes are file-exact: a task assigned a.qml must
    // not acquire the whole a.qml/... subtree, and a directory-like entry
    // must not silently authorize undeclared files. Legacy callers retain
    // prefix semantics unless they opt in.
    bool allowedRootsAreExactPaths {false};

    // Paths that are always rejected, relative to projectRoot.
    // Applied after allowedRoots; a denied path overrides an allowed root.
    std::vector<std::filesystem::path> deniedPaths;

    // Permitted file-name extensions (including the leading dot, e.g. ".cpp").
    // An empty list means all extensions are allowed.
    std::vector<std::string> allowedExtensions;

    // Per-proposal safety limits.  A zero value keeps the hardcoded default.
    std::size_t maxFilesPerChangeSet {50};
    std::size_t maxTotalBytes {10ULL * 1024 * 1024}; // 10 MiB

    // Irreversible operations disabled by default.  Enable only when the
    // enclosing pipeline has a verified rollback path (Stage 2+).
    bool allowDelete {false};
    bool allowRename {false};
};

} // namespace ai::application
