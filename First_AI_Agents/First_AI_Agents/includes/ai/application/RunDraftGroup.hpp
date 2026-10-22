#pragma once

#include "ai/application/RunDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-3B: the pure rules every IRunDraftRepository applies to a draft group. They read no repository and no clock.

[[nodiscard]] const char* toString(RunDraftGroupOrigin origin) noexcept;
[[nodiscard]] std::optional<RunDraftGroupOrigin> runDraftGroupOriginFromString(std::string_view value) noexcept;

// Data integrity of ONE group insertion (group row + all its drafts), checked before anything is written. It is not
// the plan validation (that is parseNextRunPlan); it protects what is stored. Failure codes (all
// `run_draft_group.*`, nothing written):
//  - invalid_group: id, project, idempotency key / plan fingerprint (64 lowercase hex), plan title (1-512 bytes, no
//    control character), a KNOWN origin (an enum value no enumerator names is refused, never read as another
//    origin), origin-specific source fields (a chat message group has conversation id, message id and a 64-hex
//    message fingerprint; a manual import has none of them; a follow-up group (PC-5) has the 1-64 character id of
//    its request, the sealed context fingerprint of that request and at least one source Run, and none of the
//    message fields), a context fingerprint (64 hex) only on a chat message or follow-up group - it names a snapshot
//    or sealed context attached to the source request, which a manual paste does not have - a request id only on a
//    follow-up group, and source runs (at most 16, non-empty, unique);
//  - invalid_drafts: 1-8 drafts, non-empty unique ids, every draft in the group's project, a per-Run Manifest
//    roster draft (CustomRoster + the Manifest workflow, the only combination that may carry contracts), contracts
//    already in the canonical PC-2 form;
//  - invalid_provenance: every draft carries provenance naming this group, ordinals are exactly 0..n-1, plan keys
//    are valid (`[A-Za-z0-9][A-Za-z0-9_.-]{0,63}`) and unique ignoring case, and the created-content fingerprint IS
//    nextRunPlanDraftContentFingerprint(title, task, contracts, dependency keys) of the draft being inserted (it is
//    immutable, so a wrong value would make the "edited since creation" indicator wrong forever);
//  - invalid_dependencies: every dependsOn entry is the exact plan key of ANOTHER draft of the group, listed once,
//    and the dependencies form no cycle.
[[nodiscard]] common::Result<void> validateRunDraftGroupInsertion(
    const RunDraftGroup& group, const std::vector<RunDraft>& drafts);

// The PUBLIC insertGroup of every IRunDraftRepository refuses a group of the follow-up origin: such a group claims that a
// paid, confirmed composer request produced it, so it is written only by the atomic Confirm port (IFollowUpPlanConfirmPort),
// which checks that claim against the stored request in the same transaction. Failure:
// `run_draft_group.follow_up_requires_confirm`, nothing written.
[[nodiscard]] common::Result<void> checkRunDraftGroupPublicInsert(const RunDraftGroup& group);

// The rule updateDraft shares in every repository: provenance and group never change after insertGroup. `stored*`
// are what the repository holds, `incoming` is the draft the caller wants written. A different provenance (including
// attaching one to a manual draft or dropping it from a grouped one) fails with `run_draft.provenance_immutable`; a
// grouped draft cannot move to another project (`run_draft.group_project_immutable`). Nothing is silently ignored.
[[nodiscard]] common::Result<void> checkRunDraftProvenanceUnchanged(
    const std::optional<RunDraftProvenance>& storedProvenance, const ProjectId& storedProjectId,
    const RunDraft& incoming);

} // namespace ai::application
