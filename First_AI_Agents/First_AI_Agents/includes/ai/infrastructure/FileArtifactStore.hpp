#pragma once

#include "ai/application/IArtifactStore.hpp"

#include <memory>
#include <mutex>

namespace ai::infrastructure {

/**
 * Real filesystem IArtifactStore. Every write and every resolve re-validates
 * canonical containment inside the configured root, so neither a sanitized-
 * but-unlucky name nor a symlink planted inside the root can escape it.
 *
 * Construction is fallible (root creation/canonicalization can fail), so
 * instances are only produced through the static open() factory, matching
 * SqliteDatabase::open()'s pattern in this codebase.
 */
class FileArtifactStore final : public application::IArtifactStore {
public:
    [[nodiscard]] static common::Result<std::unique_ptr<FileArtifactStore>> open(
        const std::filesystem::path& root);

    common::Result<application::StoredArtifactFile> writeArtifact(
        const application::RunId& runId,
        std::string_view suggestedName,
        std::string_view content) override;
    common::Result<void> removeArtifact(
        const std::filesystem::path& relativePath) override;
    [[nodiscard]] common::Result<std::filesystem::path> resolveAbsolutePath(
        const std::filesystem::path& relativePath) const override;
    [[nodiscard]] common::Result<std::string> readArtifact(
        const std::filesystem::path& relativePath) const override;

private:
    explicit FileArtifactStore(std::filesystem::path canonicalRoot);

    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::uint64_t counter_ {0};
};

} // namespace ai::infrastructure
