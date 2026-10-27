#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ai::application {

// What a successful write reports back to ArtifactService: the artifact-
// root-relative path actually used (may differ from the caller's suggested
// name after sanitization/uniqueness handling), the exact byte size written,
// and a hex-encoded SHA-256 digest of the written content.
struct StoredArtifactFile {
    std::filesystem::path relativePath;
    std::uint64_t byteSize {0};
    std::string sha256;
};

/**
 * Filesystem boundary for artifact contents. Implementations own where the
 * artifact root lives and must never allow a caller-controlled name or a
 * stored relative path to resolve outside that root, including through
 * symlinks - every implementation re-validates canonical containment on both
 * write and resolve.
 */
class IArtifactStore {
public:
    virtual ~IArtifactStore() = default;

    // Writes content under a run-scoped location. suggestedName is only a
    // hint: implementations sanitize it and may rename to guarantee
    // uniqueness and containment. The write must be atomic with respect to a
    // concurrent reader (temp file + rename within the same directory).
    virtual common::Result<StoredArtifactFile> writeArtifact(
        const RunId& runId,
        std::string_view suggestedName,
        std::string_view content) = 0;

    // Compensation boundary used only when metadata persistence fails after
    // a successful file write. Implementations must revalidate containment
    // exactly like resolveAbsolutePath() before removing the one target file.
    virtual common::Result<void> removeArtifact(
        const std::filesystem::path& relativePath) = 0;

    // Re-validates that relativePath (as stored in an Artifact row) still
    // resolves inside the artifact root before returning an absolute path a
    // Qt adapter could open directly. This is the safe "open target" callers
    // must use instead of concatenating a root and a stored path themselves.
    [[nodiscard]] virtual common::Result<std::filesystem::path> resolveAbsolutePath(
        const std::filesystem::path& relativePath) const = 0;

    // Reads an artifact's full content back into the process, re-validating
    // containment exactly like resolveAbsolutePath(). For an in-app,
    // read-only viewer (e.g. a context-snapshot dialog) that must not shell
    // out to QDesktopServices::openUrl.
    [[nodiscard]] virtual common::Result<std::string> readArtifact(
        const std::filesystem::path& relativePath) const = 0;
};

} // namespace ai::application
