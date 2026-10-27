#pragma once

#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

struct ManifestProposalOverlayLimits {
    std::size_t maxFiles {20'000};
    std::size_t maxTotalBytes {512ULL * 1024 * 1024};
    std::size_t maxProposals {64};
    std::size_t maxChanges {4'096};
};

struct BuildManifestProposalOverlayRequest {
    std::filesystem::path workspaceRoot;
    std::filesystem::path trustedScratchRoot;
    std::string workspaceIdentity;
    WorkspaceReadPolicy baselinePolicy;
    // The overlay is materialized from this exact policy-visible source
    // snapshot. It must never enumerate/copy the workspace independently,
    // otherwise denied generated trees (build/out/install) and secrets can
    // silently enter verification under a different baseline identity.
    WorkspaceBaselineSnapshot baselineSnapshot;
    std::vector<ChangeSetId> orderedProposalIds;
    ManifestProposalOverlayLimits limits;
};

struct ManifestProposalOverlayDescriptor {
    std::filesystem::path root;
    std::filesystem::path trustedScratchRoot;
    std::string workspaceIdentity;
    std::string baselineFingerprint;
    std::vector<ChangeSetId> orderedProposalIds;
    std::vector<std::string> orderedProposalContentHashes;
    std::string overlayFingerprint;
    std::size_t fileCount {0};
    std::size_t totalBytes {0};
};

class ManifestProposalOverlay final {
public:
    [[nodiscard]] static common::Result<ManifestProposalOverlayDescriptor> build(
        const BuildManifestProposalOverlayRequest& request,
        ChangeProposalService& proposals);

    // Removes only a previously returned overlay whose canonical parent is
    // the descriptor's canonical trusted scratch root. Fails closed on any
    // mismatch instead of recursively deleting a computed broad path.
    [[nodiscard]] static common::Result<void> cleanup(
        const ManifestProposalOverlayDescriptor& descriptor);
};

} // namespace ai::application
