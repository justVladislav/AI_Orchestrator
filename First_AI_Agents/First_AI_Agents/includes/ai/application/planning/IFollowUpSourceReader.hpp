#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/planning/FollowUpComposerContext.hpp"
#include "ai/application/planning/OutcomeBrief.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// PC-5C: everything the follow-up composer needs to KNOW about one source Run, read at one moment and read-only. The quote, the
// authorisation and the claim each read it again and assemble the provider bytes from it themselves; nothing here is a stored
// copy and nothing is trusted from an earlier step.
struct FollowUpSourceMaterial {
    // The project row (its `status` is the "still Active" check) and the project the RUN ROW names (the authority: a request or a
    // brief never says which project a Run belongs to).
    Project project;
    ProjectId runProjectId;
    // The CURRENT revision of the Run's Outcome Brief (the repository has already applied its read authority: project and
    // terminal status agree with the Run now).
    OutcomeBrief brief;
    // The PC-1 snapshot built with `selectedRunId` = the source Run.
    ProjectContextSnapshot snapshot;
    // EVERY candidate file of the source Run, bytewise by relative path, selected = false. Content (and its sha256) is present only
    // for an Eligible candidate; the loader has already run the path guard, the symlink check, the binary / UTF-8 / size checks and
    // the secret scan (the pure assembler repeats the policy and the scan over what it would send).
    std::vector<FollowUpFileCandidate> candidates;
    // Set INSTEAD of the candidates (which stay empty, and so does the snapshot) when the Run has more candidate files than the dialog
    // can show: the list is never cut short, it is refused whole (`follow_up.too_many_candidate_files`, a free refusal). The caller
    // raises it where the files are judged: after the project, source, Outcome and provider checks, so the order of causes holds.
    std::optional<common::Error> candidateRefusal;
};

// Read-only and synchronous. It may hash a workspace and read files, so a UI caller invokes it off the UI thread.
//
// Failure codes a caller relies on: follow_up.project_not_found, follow_up.run_not_found, follow_up.outcome_missing (the Run has no
// brief yet); anything else is a persistence or filesystem fault and is passed on unchanged (never turned into a `stale`).
class IFollowUpSourceReader {
public:
    virtual ~IFollowUpSourceReader() = default;

    [[nodiscard]] virtual common::Result<FollowUpSourceMaterial> read(
        const ProjectId& projectId, const RunId& sourceRunId) const = 0;
};

} // namespace ai::application
