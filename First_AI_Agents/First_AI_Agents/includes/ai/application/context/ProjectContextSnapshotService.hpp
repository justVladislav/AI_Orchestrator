#pragma once

#include "ai/application/execution/IManifestWorkLedgerRepository.hpp"
#include "ai/application/conversations/IProjectMemoryRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/execution/IRunRepository.hpp"
#include "ai/application/verification/ManifestVerificationManualHandoffResolution.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"
#include "ai/application/workspace/OutputApprovalPolicySource.hpp"

#include <cstddef>
#include <optional>

namespace ai::application {

inline constexpr std::size_t kProjectContextMaximumLineageRuns = 64;
inline constexpr std::size_t kProjectContextRecentLedgerLimit = 32;
// PC-1C: a Project Memory segment (task up to ~1000 tokens plus final output up to ~2000, or a compacted
// summary of up to 256 KiB) is far larger than the 2 000 token memory section, so each segment is shown as a
// bounded, deterministic EXCERPT (never the whole text) and only the newest ones are listed.
inline constexpr std::size_t kProjectContextRecentMemoryLimit = 32;
inline constexpr std::size_t kProjectContextMemoryExcerptBytes = 1'536;

struct ProjectContextSnapshotRequest {
    ProjectId projectId;
    std::optional<RunId> selectedRunId;
    // Opt-in. When true the derived Project Memory section is always present (a status item says how many
    // segments exist, or that none were recorded) and a failing memory read fails the whole build: a
    // requested source never disappears silently. When false the memory repository is not touched.
    bool includeDerivedProjectMemory {false};
};

// PC-1B/1C: synchronous, provider-free, read-only loader (authoritative sources, plus the opt-in derived
// Project Memory). Workspace capture hashes the
// policy-visible tree, so UI callers must invoke build() off the UI thread and only after an explicit
// preview request. PC-1D owns that asynchronous Qt boundary.
class ProjectContextSnapshotService final {
public:
    ProjectContextSnapshotService(
        IProjectRepository& projects,
        IRunRepository& runs,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationManualHandoffResolutionReader& manualResolutions,
        IProjectMemoryRepository& projectMemory);

    // P3c: where the CURRENT read policy of the workspace comes from - today's secureDefaults plus the output folders the user approved
    // for (project, the root this snapshot reads). A snapshot is a read of the files as they are now, not the comparison of an older
    // Run's frozen baseline: that Run's own baseline artifact is neither changed nor replaced by it. Set once while the runtime is
    // composed. Null (the default, a standalone service) keeps secureDefaults.
    void setOutputApprovalPolicySource(const IOutputApprovalPolicySource* source) noexcept
    {
        outputApprovals_ = source;
    }

    // The policy is asked ONCE per build, after the cheap request checks and before the workspace is hashed. An error of the source is
    // the failure of the whole build, never a snapshot under the defaults.
    [[nodiscard]] common::Result<ProjectContextSnapshot> build(
        const ProjectContextSnapshotRequest& request) const;

    // For a caller that has already obtained the policy of ITS operation (the follow-up read, which also reads files under it): the
    // same policy is used and the source is not asked again. It must be the policy of the root this request reads (the selected Run's
    // workspace, else the project root) - a policy for any other root is refused as workspace_policy.root_mismatch, nothing is read.
    [[nodiscard]] common::Result<ProjectContextSnapshot> build(
        const ProjectContextSnapshotRequest& request, const WorkspaceReadPolicy& policy) const;

private:
    [[nodiscard]] common::Result<ProjectContextSnapshot> buildWith(
        const ProjectContextSnapshotRequest& request, const WorkspaceReadPolicy* provided) const;

    const IOutputApprovalPolicySource* outputApprovals_ {nullptr};
    IProjectRepository& projects_;
    IRunRepository& runs_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationManualHandoffResolutionReader& manualResolutions_;
    IProjectMemoryRepository& projectMemory_;
};

} // namespace ai::application
