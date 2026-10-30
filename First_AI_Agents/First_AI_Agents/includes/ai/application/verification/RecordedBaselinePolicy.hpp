#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace ai::application {

inline constexpr const char* kRecordedBaselineAmbiguousCode =
    "workspace_baseline.recorded_baseline_ambiguous";
inline constexpr const char* kRecordedBaselineIdentityMismatchCode =
    "workspace_baseline.recorded_baseline_identity_mismatch";
inline constexpr const char* kRecordedBaselineMissingCode =
    "workspace_baseline.recorded_baseline_missing";

// The read policy under which a LIVE workspace must be captured to be
// compared with the baseline a Run recorded: the current static defaults plus
// the dynamic `path:` exclusions that Run's own workspace-baseline.v1.json
// artifact froze (see RecordedWorkspaceExclusions for the grammar).
//
// Owner and identity. `ownerRunId` is the Run that owns the baseline
// artifact whose fingerprint the caller compares against - the Run whose Work
// Ledger row supplied that fingerprint (a source Run, a continuation source,
// a handoff anchor, the Run that produced an applied change set). It is never
// resolved through parentRunId: a derived Run without an artifact of its own
// (a manual-handoff attempt) is not an owner of any P1 call site. When
// `expectedFingerprint` is given the artifact must hash to exactly it - a
// valid artifact of another generation proves nothing; a normalised or pinned
// fingerprint is never passed here, only the one the artifact itself carries.
// The exclusions are read from the artifact through ArtifactService, which
// verifies the stored size and SHA, because the baseline's file fingerprint
// does not cover them.
//
// A non-empty `expectedFingerprint` is a DURABLE baseline identity (the
// Work Ledger recorded it from this very artifact), so the owner is a
// mandatory one: its artifact must exist. A baseline that recorded
// `path:Out2` over an empty or absent Out2 has the same file fingerprint
// with or without that exclusion, so the lost artifact cannot be detected
// afterwards by drift - it has to be refused here. Without an expected
// fingerprint (`std::nullopt`, or an empty one) the caller has no durable
// identity and the artifact is optional: the post-apply capture of a Run
// that never recorded one keeps the plain defaults, exactly as before.
//
// Outcomes are distinct on purpose:
//   * mandatory owner, no artifact            -> kRecordedBaselineMissingCode.
//   * optional owner, no artifact             -> success with the plain
//     secureDefaults(workspaceRoot): nothing is recorded to restore.
//   * a failed listing                        -> that failure, never the same
//     as an empty listing; neither permits losing a mandatory artifact.
//   * artifact present but reading/parsing failed, more than one artifact, a
//     fingerprint other than the expected one, or an invalid recorded
//     exclusion -> failure; never the same as "no dynamic rules".
class RecordedBaselinePolicy final {
public:
    RecordedBaselinePolicy() = delete;

    [[nodiscard]] static common::Result<WorkspaceReadPolicy> forRun(
        const ArtifactService& artifacts,
        const RunId& ownerRunId,
        const std::optional<std::string>& expectedFingerprint,
        std::filesystem::path workspaceRoot);

    // Stores `snapshot` (with its exclusions) as `runId`'s own
    // workspace-baseline.v1.json. Used by a Run that verifies against a
    // baseline captured by someone else's policy (a manual-handoff attempt):
    // it makes the frozen area, the original fingerprint and the later
    // post-apply provenance travel with the Run that owns the successor.
    [[nodiscard]] static common::Result<void> persist(
        ArtifactService& artifacts,
        const RunId& runId,
        const std::optional<StageId>& stageId,
        const WorkspaceBaselineSnapshot& snapshot);
};

} // namespace ai::application
