#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// AG-16.1 hardening: one change's already-resolved identity/content-hash
// inputs to the canonical proposal content-hash algorithm below. Gathering
// these - which may require reading a proposal's stored content, and for
// ManifestProposalOverlay::build() also writing that content into a
// candidate overlay - is entirely the caller's job; this module performs no
// I/O of its own and has no filesystem or ChangeProposalService dependency.
struct ManifestProposalContentHashChangeInput {
    FileChangeOperation operation;
    std::filesystem::path relativePath;
    std::optional<std::filesystem::path> sourcePath;
    // Present only for Create/Modify (SHA-256 of the change's own proposed
    // content); absent for Delete/Rename, which carry no content of their
    // own in this identity hash.
    std::optional<std::string> contentSha256;
};

// The single canonical algorithm for a proposal's durable content-identity
// hash. Shared by ManifestProposalOverlay::build() (which stores this exact
// value into ManifestProposalSetVerification::orderedProposalContentHashes
// at verification time) and ManifestApplyVerificationGate (which
// recomputes it at apply-gate time to prove a proposal's durable content
// has not changed since it was verified). Never duplicated - any caller
// needing this hash must go through this function, so there is exactly one
// place the algorithm itself could ever need to change.
[[nodiscard]] std::string computeManifestProposalContentHash(
    const ChangeSetId& proposalId,
    const std::vector<ManifestProposalContentHashChangeInput>& changes);

} // namespace ai::application
