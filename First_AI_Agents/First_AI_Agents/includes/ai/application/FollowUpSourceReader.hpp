#pragma once

#include "ai/application/IChangeProposalRepository.hpp"
#include "ai/application/IFollowUpFileLoader.hpp"
#include "ai/application/IFollowUpSourceReader.hpp"
#include "ai/application/IOutcomeBriefRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/context/ProjectContextSnapshotService.hpp"

#include <string>
#include <vector>

namespace ai::application {

// PC-5C-2a: the production IFollowUpSourceReader. Read-only by construction: every collaborator is a const view, and nothing here
// writes a Run, a proposal, an Apply, the Work Ledger, an Outcome Brief or a file.
//
//   - the project row and the Run row (the Run row names the project it belongs to);
//   - the CURRENT Outcome head, through the brief repository (which applies its own read authority: a head that is stale against the
//     Run, or a brief of another project, is `persistence.invalid_data` - a fault the caller leaves alone, never a brief it can use);
//   - the PC-1 snapshot of the Run (`selectedRunId` = the source Run);
//   - the candidate files: the RELATIVE paths of the file changes of the source Run's proposals (all of them, in every state, read
//     through listForRun / listChangesForSets - not through the Work Ledger), loaded by IFollowUpFileLoader from the trusted root: the
//     Run's own frozen workspace, else the project root (the same rule as the snapshot's workspace).
//
// A Run with more than kFollowUpMaximumCandidates unique candidate paths is not an error of the read: the material comes back with
// `candidateRefusal` set and no snapshot and no candidates, and the caller raises it in its own order.
// When the project is not Active, or the Run belongs to another project, the snapshot and the candidates are left EMPTY: the caller
// refuses on exactly those two facts before it uses anything else, and the reader never hashes or reads a workspace it was not entitled
// to read. Failure codes: follow_up.project_not_found, follow_up.run_not_found, follow_up.outcome_missing (no head), the snapshot
// service's `project_context.*` codes and the loader's follow_up.workspace_unavailable; anything else is a persistence fault, unchanged.
class FollowUpSourceReader final : public IFollowUpSourceReader {
public:
    FollowUpSourceReader(
        const IProjectRepository& projects,
        const IRunRepository& runs,
        const IOutcomeBriefRepository& briefs,
        const ProjectContextSnapshotService& snapshots,
        const IChangeProposalRepository& proposals,
        const IFollowUpFileLoader& files);

    [[nodiscard]] common::Result<FollowUpSourceMaterial> read(
        const ProjectId& projectId, const RunId& sourceRunId) const override;

private:
    const IProjectRepository& projects_;
    const IRunRepository& runs_;
    const IOutcomeBriefRepository& briefs_;
    const ProjectContextSnapshotService& snapshots_;
    const IChangeProposalRepository& proposals_;
    const IFollowUpFileLoader& files_;
};

inline constexpr std::size_t kFollowUpMaximumCandidates = 256;

// The candidate paths of a Run's file changes: the relative path of every change (any operation, any change set), spelled '/'-separated
// UTF-8, kept only when it is canonical for the assembler (followUpCandidatePathIsCanonical - a path the proposal validator would never
// have admitted is not a file, so it is left out instead of refusing the whole request), unique under the workspace's case rule (the
// bytewise smallest spelling wins), bytewise ordered. ALL of them: this function never cuts the list. FollowUpSourceReader::read counts
// them and, over kFollowUpMaximumCandidates, refuses the whole list (candidateRefusal, follow_up.too_many_candidate_files) before the
// workspace is hashed or a file opened: the dialog shows every candidate or none, and says why.
[[nodiscard]] std::vector<std::string> followUpCandidatePathsOf(const std::vector<ProposedFileChange>& changes);

} // namespace ai::application
