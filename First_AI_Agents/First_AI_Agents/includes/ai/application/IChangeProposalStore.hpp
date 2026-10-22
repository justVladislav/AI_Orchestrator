#pragma once

#include "ai/application/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <string>

namespace ai::application {

struct ProjectWritePolicy;

// What a successful publishChangeSet returns.
struct PublishedProposalLocation {
    // Path relative to the store root pointing to the top-level
    // change-set directory.  Never empty and never contains "..".
    std::string artifactRelativeRoot;

    // SHA-256 hex digest of policy.json written at publish time.
    // Stored on the change-set row so a future consumer can verify
    // the policy was not altered after the proposal was recorded.
    std::string policySnapshotHash;
};

// Write-once filesystem boundary for change-set proposal content.
//
// Implementations must:
//   - write everything to a staging (temp) directory first;
//   - atomically rename staging → final only after all files succeed;
//   - reject a duplicate changeSetId (no overwrite).
//
// Stage 1 concern only: no apply, no backup, no rollback here.
class IChangeProposalStore {
public:
    virtual ~IChangeProposalStore() = default;

    // Atomically publishes the complete proposal directory to storage.
    // changeSet.state must already be the terminal validation result
    // (ReadyForReview or Rejected), so the immutable manifest agrees with the
    // metadata row. On any partial failure nothing is left in the final path.
    virtual common::Result<PublishedProposalLocation> publishChangeSet(
        const ProposedChangeSet& changeSet,
        const ProjectWritePolicy& policy) = 0;

    // Reads the original-file snapshot recorded at publish time.
    // changeId must belong to the change set at the given root.
    virtual common::Result<std::string> readOriginalContent(
        const std::string& artifactRelativeRoot,
        const ChangeId& changeId) const = 0;

    // Reads the proposed file content recorded at publish time.
    virtual common::Result<std::string> readProposedContent(
        const std::string& artifactRelativeRoot,
        const ChangeId& changeId) const = 0;
};

} // namespace ai::application
