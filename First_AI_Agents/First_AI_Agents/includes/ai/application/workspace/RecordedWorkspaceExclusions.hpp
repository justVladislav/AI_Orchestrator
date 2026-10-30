#pragma once

#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ai::application {

inline constexpr const char* kRecordedExclusionInvalidCode =
    "workspace_baseline.recorded_exclusion_invalid";

// Restores the dynamic part of a workspace read policy from the `exclusions`
// a baseline recorded (WorkspaceBaselineSnapshot::exclusions).
//
// Only `path:<directory>` entries are dynamic: they are the Run's own
// deniedPaths. Every other recorded kind (directory-name, directory-sequence,
// file-name, extension) is a static rule that the current
// WorkspaceReadPolicy::secureDefaults() regenerates; this class does not
// promise an exact replay of an older static list - it freezes only the
// dynamic prefixes.
//
// The parser is strict and never repairs: a recorded string is either a
// canonical relative directory prefix exactly as WorkspaceBaselineProvenance
// writes it, or the whole restoration fails with
// kRecordedExclusionInvalidCode. Nothing is normalised into a different
// area. Accepted grammar for a `path:` value:
//   * UTF-8, 1..512 bytes, no control characters, only '/' as a separator;
//   * no empty, "." or ".." component, no leading or trailing '/';
//   * no ':' (drive-relative paths, alternate data streams), no
//     \ * ? " < > | characters;
//   * no component that ends in '.' or ' ' or names a Windows device
//     (CON, PRN, AUX, NUL, CLOCK$, COM1-9, LPT1-9, with or without extension);
//   * no duplicates and no nesting between entries (comparison per
//     component, ASCII case-insensitive on Windows, like the policy itself).
// An unrecognised entry kind is an error too: a rule this reader does not
// understand must not be dropped silently. There is no count limit beyond
// the baseline format's own limit of 128 recorded exclusions.
class RecordedWorkspaceExclusions final {
public:
    RecordedWorkspaceExclusions() = delete;

    // The validated `path:` prefixes, in recorded order.
    [[nodiscard]] static common::Result<std::vector<std::filesystem::path>> parsePaths(
        const std::vector<std::string>& exclusions);

    // secureDefaults(workspaceRoot) with deniedPaths set to the recorded
    // prefixes.
    [[nodiscard]] static common::Result<WorkspaceReadPolicy> policyFor(
        std::filesystem::path workspaceRoot,
        const std::vector<std::string>& exclusions);
};

} // namespace ai::application
