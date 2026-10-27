#pragma once

// Filesystem-level path guard for read-only workspace tool execution.
//
// WorkspaceReadPolicyValidator (WorkspaceReadPolicy.hpp) provides a pure
// lexical guard. WorkspacePathGuard adds the OS-level layer on top:
//
//   1. Delegates to WorkspaceReadPolicyValidator::validateRelativePath
//      (covers .., absolute, UNC, ADS/colon, Windows device names, deny lists).
//   2. Canonicalises the workspace root via weakly_canonical (resolves any
//      OS-level symlink or junction in the trusted root itself).
//   3. Builds the candidate absolute path and calls weakly_canonical on it —
//      this follows symlinks in the relative portion and catches escape attempts.
//   4. Canonical containment check: verified component-by-component, case-
//      insensitive on Windows.
//   5. (Windows only) Explicit FILE_ATTRIBUTE_REPARSE_POINT scan from the
//      first child below the canonical root — belt-and-suspenders guard
//      against junctions / symlinks the OS resolves non-canonically (e.g.
//      relative symlinks on network paths).
//
// Contract:
//   - Call validate() immediately before every filesystem operation.
//     Never cache the returned path across suspension points.
//   - policy.workspaceRoot MUST originate from the trusted Run/Project
//     configuration — never from model output.
//   - Error messages MUST NOT contain absolute paths, the workspace root,
//     or any model-supplied input strings.

#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <string>

namespace ai::application {

class WorkspacePathGuard {
public:
    // Returns the canonical absolute path of modelPath inside the workspace.
    //
    // modelPath  — raw untrusted string received from the model (not validated)
    // policy     — trusted backend configuration; workspaceRoot must be absolute
    //
    // On success: canonical absolute path guaranteed to be inside workspaceRoot.
    //
    // Result::failure error codes (provider-neutral, no secrets or paths):
    //   workspace_policy.invalid_root          — workspaceRoot not absolute / empty
    //   workspace_policy.invalid_limit         — zero limit in policy
    //   workspace_policy.invalid_allowed_root  — allowed root is absolute or ..
    //   workspace_policy.invalid_denied_path   — denied path is absolute or ..
    //   workspace_path.unsafe                  — traversal / absolute / UNC / ADS /
    //                                            Windows device name / NUL byte
    //   workspace_path.denied                  — blocked by policy deny lists
    //   workspace_path.outside_allowed_roots   — not inside any allowed root
    //   workspace_path.outside_workspace       — canonical path escapes root
    //   workspace_path.reparse_point           — symlink/junction (Windows)
    //   workspace_path.root_inaccessible       — workspace root cannot be resolved
    [[nodiscard]] static common::Result<std::filesystem::path> validate(
        const std::string& modelPath,
        const WorkspaceReadPolicy& policy);

    // The same six steps for a path the CALLER has already built with the right text encoding. validate(string) constructs its path
    // with the narrow std::filesystem::path constructor, which on Windows misreads UTF-8 bytes through the system codepage; a caller
    // that holds UTF-8 text (a candidate file of a Run, a stored relative path) builds the path itself (detail::pathFromUtf8) and
    // calls this, so the path that is vetted is the path that is read.
    [[nodiscard]] static common::Result<std::filesystem::path> validatePath(
        const std::filesystem::path& relativePath,
        const WorkspaceReadPolicy& policy);
};

} // namespace ai::application
