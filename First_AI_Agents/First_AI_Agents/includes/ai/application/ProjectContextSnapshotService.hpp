#pragma once

#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IProjectMemoryRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/ManifestVerificationManualHandoffResolution.hpp"
#include "ai/application/ProjectContextSnapshot.hpp"

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

    [[nodiscard]] common::Result<ProjectContextSnapshot> build(
        const ProjectContextSnapshotRequest& request) const;

private:
    IProjectRepository& projects_;
    IRunRepository& runs_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationManualHandoffResolutionReader& manualResolutions_;
    IProjectMemoryRepository& projectMemory_;
};

} // namespace ai::application
