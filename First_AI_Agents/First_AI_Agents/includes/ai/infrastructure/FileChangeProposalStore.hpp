#pragma once

#include "ai/application/IChangeProposalStore.hpp"

#include <filesystem>
#include <memory>
#include <mutex>

namespace ai::infrastructure {

/**
 * Write-once filesystem IChangeProposalStore.
 *
 * Each proposal is published in two phases:
 *   1. Write all files to a staging (temp) directory.
 *   2. Atomically rename staging → final directory.
 * A partial staging write is never visible under the final path.
 *
 * Published directory layout (relative to root_):
 *   runs/<runId>/proposals/<changeSetId>/
 *     manifest.json
 *     summary.md
 *     policy.json        (SHA-256 recorded in policySnapshotHash)
 *     validation.json
 *     originals/<sha256(changeId)>   (Modify / Delete / Rename)
 *     proposed/<sha256(changeId)>    (Create / Modify)
 *
 * Model-supplied paths remain manifest data and are never used as artifact
 * paths. Hashed validated IDs also avoid collisions with reserved filenames.
 *
 * Construction is fallible; use FileChangeProposalStore::open().
 */
class FileChangeProposalStore final : public application::IChangeProposalStore {
public:
    [[nodiscard]] static common::Result<std::unique_ptr<FileChangeProposalStore>> open(
        const std::filesystem::path& root);

    common::Result<application::PublishedProposalLocation> publishChangeSet(
        const application::ProposedChangeSet& changeSet,
        const application::ProjectWritePolicy& policy) override;

    [[nodiscard]] common::Result<std::string> readOriginalContent(
        const std::string& artifactRelativeRoot,
        const application::ChangeId& changeId) const override;

    [[nodiscard]] common::Result<std::string> readProposedContent(
        const std::string& artifactRelativeRoot,
        const application::ChangeId& changeId) const override;

private:
    explicit FileChangeProposalStore(std::filesystem::path canonicalRoot);

    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

} // namespace ai::infrastructure
