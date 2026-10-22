#pragma once

#include "ai/application/ChangeProposalTypes.hpp"
#include "ai/application/ProjectWritePolicy.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::application {

// Validates a ProposedChangeSet and returns all findings.
// An empty finding list means the proposal passed every check.
//
// DESIGN PRINCIPLES
//   - Validation never modifies the proposal.
//   - Every check that can be expressed as a finding IS expressed as one,
//     so the caller receives a complete picture rather than stopping at the
//     first error (except for secret detection, which is done separately and
//     causes immediate rejection before any finding is stored).
//   - Finding messages are intentionally terse and secret-free; they describe
//     the structural problem, not the offending value.
//
// CHECK CATEGORIES
//   1. Structural / operation invariants per FileChangeOperation.
//   2. Path safety (absolute, UNC, drive-qualified, traversal, NUL, ADS,
//      .git, credential/secret/env files, build/system directories, symlinks).
//   3. Policy enforcement (allowedRoots, deniedPaths, allowedExtensions,
//      maxFilesPerChangeSet, maxTotalBytes, allowDelete, allowRename).
//   4. Original-file preconditions (hash match for Modify/Delete/Rename;
//      absence check for Create / Rename target).
//   5. Duplicate changeId / conflicting-path detection.
//   6. UTF-8 / binary content check (proposed content only).
//
// Secret scanning is deliberately SEPARATE (scanForSecrets) because it must
// run BEFORE any data is saved and must cause immediate rejection without
// emitting the offending value in an error message.
class ChangeProposalValidator {
public:
    // Run all structural, path-safety, policy, precondition, and content
    // checks.  projectRoot must be an absolute canonical path.
    // The returned findings are ordered: change-set-wide findings first, then
    // per-change findings in the order the changes appear in the proposal.
    //
    // requireHashEvidence/observedFileHashes: when requireHashEvidence is
    // true, a Modify/Delete/Rename change's original-hash precondition is
    // checked against observedFileHashes (relativePath.generic_string(),
    // lowercased on Windows -> sha256 hex). The map is a caller-collected
    // record of real workspace_hash_file results from the same submission
    // attempt, never a free-form string the model typed. A path with no
    // matching entry fails closed with proposal.missing_hash_evidence
    // regardless of what expectedOriginalHash contains. When false, this
    // generic validator can still validate an already-materialized trusted
    // expectedOriginalHash against disk, but proposal-submission.v2 itself
    // never parses such a hash from provider output.
    [[nodiscard]] static std::vector<ValidationFinding> validate(
        const ProposedChangeSet& changeSet,
        const ProjectWritePolicy& policy,
        const std::filesystem::path& projectRoot,
        bool requireHashEvidence = false,
        const std::unordered_map<std::string, std::string>& observedFileHashes = {});

    // Scan proposed content, summary, and provider-supplied paths for common
    // API-key, private-key, authorization-header, and .env patterns.
    // Returns findings with code "proposal.secret_detected" if anything is
    // found; the message names the field type but never echoes the value.
    // If the returned list is non-empty the caller MUST NOT save anything.
    [[nodiscard]] static std::vector<ValidationFinding> scanForSecrets(
        const ProposedChangeSet& changeSet);
};

} // namespace ai::application
