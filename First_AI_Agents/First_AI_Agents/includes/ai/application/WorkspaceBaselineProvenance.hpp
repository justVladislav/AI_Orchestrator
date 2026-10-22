#pragma once

#include "ai/application/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr const char* kWorkspaceBaselineSchemaVersion =
    "workspace-baseline.v1";
inline constexpr const char* kWorkspacePreflightSchemaVersion =
    "workspace-preflight.v1";

struct WorkspaceBaselineLimits {
    std::size_t maxFiles {20'000};
    std::uint64_t maxTotalBytes {512ULL * 1024 * 1024};
    std::size_t maxManifestBytes {4ULL * 1024 * 1024};
    std::size_t maxChangedPaths {512};
};

struct WorkspaceBaselineEntry {
    std::string relativePath;
    std::uint64_t byteSize {0};
    std::string sha256;
};

struct WorkspaceBaselineSnapshot {
    std::string schemaVersion {kWorkspaceBaselineSchemaVersion};
    std::string fingerprint;
    std::vector<WorkspaceBaselineEntry> files;
    std::uint64_t totalBytes {0};
    std::vector<std::string> exclusions;
    bool repositoryRevisionAvailable {false};
    std::optional<std::string> repositoryRevision;
};

struct WorkspaceBaselineDifference {
    std::string classification;
    std::string currentFingerprint;
    std::vector<std::string> addedPaths;
    std::vector<std::string> modifiedPaths;
    std::vector<std::string> deletedPaths;
};

struct WorkspaceCMakePreflightResult {
    bool configured {false};
    std::vector<std::string> passedCheckIds;
};

class WorkspaceBaselineProvenance final {
public:
    [[nodiscard]] static common::Result<WorkspaceBaselineSnapshot> capture(
        const WorkspaceReadPolicy& policy,
        const WorkspaceBaselineLimits& limits = {});

    [[nodiscard]] static common::Result<WorkspaceBaselineDifference> compare(
        const WorkspaceBaselineSnapshot& baseline,
        const WorkspaceBaselineSnapshot& current,
        const WorkspaceBaselineLimits& limits = {});

    // MW-P5.6D: removes every entry whose relativePath starts with one of
    // `excludedPrefixes` and recomputes totalBytes/fingerprint with the
    // exact same deterministic algorithm capture() itself uses - never an
    // approximation of it. Every prefix must be a safe, directory-shaped,
    // relative path ending in '/' (workspace_baseline.invalid_prefix
    // otherwise). Used only to PROVE an already-computed drift is limited to
    // a Backend-owned, pre-approved delta (see
    // ManifestVerificationLegacyBaselineNormalizationService) - this
    // function itself does not decide what is safe to exclude, it only
    // recomputes what the fingerprint would be once a caller-supplied set of
    // paths is set aside.
    [[nodiscard]] static common::Result<WorkspaceBaselineSnapshot> pruneExcludedPrefixes(
        const WorkspaceBaselineSnapshot& snapshot,
        const std::vector<std::string>& excludedPrefixes);

    // Optional project-owned preflight. When
    // .ai-orchestrator/preflight.json is absent this is a successful no-op.
    // When present, the strict versioned configuration is checked before any
    // provider session is created.
    [[nodiscard]] static common::Result<WorkspaceCMakePreflightResult>
    runConfiguredCMakePreflight(const WorkspaceReadPolicy& policy);

    [[nodiscard]] static common::Result<std::string> renderJson(
        const WorkspaceBaselineSnapshot& snapshot,
        const WorkspaceBaselineLimits& limits = {});

    [[nodiscard]] static common::Result<WorkspaceBaselineSnapshot> parseJson(
        std::string_view json,
        const WorkspaceBaselineLimits& limits = {});

    // AG-15: a compact, bounded, deterministic catalog of the already-
    // captured baseline - relative path, extension, byte size, and the real
    // Backend-computed sha256 - for the Planner to declare a frozen active
    // scope from, without ever loading every file's content into the
    // prompt. `snapshot.files` is already path-sorted (capture()'s own
    // invariant), so this rendering is deterministic by construction. Never
    // truncates mid-entry: once adding the next line would exceed maxBytes,
    // rendering stops and the (bounded) result is returned as-is.
    [[nodiscard]] static std::string renderDiscoveryCatalog(
        const WorkspaceBaselineSnapshot& snapshot,
        std::size_t maxEntries = 2000,
        std::size_t maxBytes = 32ULL * 1024);
};

} // namespace ai::application
