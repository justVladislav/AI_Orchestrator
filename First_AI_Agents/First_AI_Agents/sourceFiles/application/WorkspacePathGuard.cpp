#include "ai/application/WorkspacePathGuard.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace ai::application {

namespace {

namespace fs = std::filesystem;

// Component-by-component containment check on two canonical absolute paths.
// Case-insensitive on Windows (NTFS is case-insensitive).
// Returns true when child is equal to root or is nested inside it.
bool isContainedIn(const fs::path& child, const fs::path& root)
{
    auto childIt = child.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++childIt) {
        if (childIt == child.end()) return false; // root longer than child

#ifdef _WIN32
        // UTF-8 text, never generic_string(): that one throws for a folder name the ANSI code page cannot hold.
        const std::string lhs = detail::asciiLower(detail::genericUtf8(*rootIt));
        const std::string rhs = detail::asciiLower(detail::genericUtf8(*childIt));
        if (lhs != rhs) return false;
#else
        if (*rootIt != *childIt) return false;
#endif
    }
    return true; // all root components matched
}

#ifdef _WIN32
// Returns true if any path component strictly below canonicalRoot in
// canonicalCandidate carries FILE_ATTRIBUTE_REPARSE_POINT.
//
// This catches junctions / symlinks that weakly_canonical cannot fully
// resolve — e.g. relative symlinks on network shares or broken links —
// where the canonical containment check (step 4) might still pass.
bool hasReparsePointBelowRoot(const fs::path& canonicalRoot,
                               const fs::path& canonicalCandidate)
{
    std::size_t rootDepth = 0;
    for (auto it = canonicalRoot.begin(); it != canonicalRoot.end(); ++it) ++rootDepth;

    fs::path current;
    std::size_t depth = 0;
    for (const auto& component : canonicalCandidate) {
        current /= component;
        ++depth;
        if (depth <= rootDepth) continue; // trusted root — skip

        const DWORD attrs = ::GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) break; // component does not exist
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    }
    return false;
}
#endif

} // namespace

common::Result<std::filesystem::path> WorkspacePathGuard::validate(
    const std::string& modelPath,
    const WorkspaceReadPolicy& policy)
{
    return validatePath(detail::pathFromUtf8(std::string(modelPath)), policy);
}

common::Result<std::filesystem::path> WorkspacePathGuard::validatePath(
    const std::filesystem::path& relativePath,
    const WorkspaceReadPolicy& policy)
{
    namespace fs = std::filesystem;

    // ── Step 1: Lexical validation ────────────────────────────────────────
    // Checks: trusted policy sanity; NUL bytes; drive-qualified / UNC /
    // absolute paths; colon (ADS on Windows); ".." components; empty
    // components; Windows reserved device names (CON, COM1, NUL, …);
    // denied directory names, file names, extensions; allowedRoots;
    // deniedPaths.
    // Never touches the filesystem.
    auto lexical = WorkspaceReadPolicyValidator::validateRelativePath(
        relativePath, policy);
    if (!lexical.success())
        return common::Result<fs::path>::failure(*lexical.error());

    const fs::path& safeRelative = lexical.value(); // lexically normalised

    // ── Step 2: Canonicalise the trusted workspace root ───────────────────
    // The root comes from the backend Run/Project — never from model output.
    // weakly_canonical resolves any symlink / junction in the root itself;
    // it does not error if non-existing path components are appended later.
    std::error_code ec;
    const fs::path canonicalRoot = fs::weakly_canonical(policy.workspaceRoot, ec);
    if (ec)
        return common::Result<fs::path>::failure({
            "workspace_path.root_inaccessible",
            "workspace root cannot be resolved"});

    // ── Step 3: Canonicalise the candidate ───────────────────────────────
    // Resolve symlinks in the existing prefix, then append a possibly-missing
    // suffix. This remains valid on Windows STL implementations where
    // weakly_canonical rejects that valid future-path case.
    const fs::path candidate = canonicalRoot / safeRelative;
    const fs::path canonicalCandidate =
        detail::canonicalizeWithMissingSuffix(candidate, ec);
    if (ec)
        return common::Result<fs::path>::failure({
            "workspace_path.access_error",
            "path is inaccessible"});

    // ── Step 4: Canonical containment ────────────────────────────────────
    // Validates component by component; case-insensitive on Windows.
    // This is the primary guard against symlink/junction escape.
    if (!isContainedIn(canonicalCandidate, canonicalRoot))
        return common::Result<fs::path>::failure({
            "workspace_path.outside_workspace",
            "path resolves outside the workspace root"});

    // ── Step 5: Windows reparse-point traversal check ────────────────────
    // Belt-and-suspenders: explicitly reject any component below the root
    // that carries FILE_ATTRIBUTE_REPARSE_POINT. Catches cases where the OS
    // does not fully resolve a relative symlink during weakly_canonical
    // (e.g. cross-volume relative links, broken junctions on network paths).
#ifdef _WIN32
    // Inspect the caller's path, not the resolved target. Inspecting the
    // canonical result would erase an in-workspace reparse point from the path.
    if (hasReparsePointBelowRoot(canonicalRoot, candidate))
        return common::Result<fs::path>::failure({
            "workspace_path.reparse_point",
            "path traverses a symbolic link or junction point"});
#endif

    return common::Result<fs::path>::ok(std::move(canonicalCandidate));
}

} // namespace ai::application
