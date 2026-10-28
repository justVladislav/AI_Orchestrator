#pragma once

#include "ai/application/planning/RunDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

/**
 * Persistence port for saved Run drafts and their complete ordered roster.
 * insert/update must commit the draft row and every roster entry atomically
 * (all-or-nothing), the same aggregate guarantee IRunRepository and
 * IExecutionProfileRepository already provide for their own child rows.
 *
 * PC-3B adds draft GROUPS: drafts created together from one plan. The contract, identical in every
 * implementation:
 *  - insertGroup writes the group and ALL its drafts (with their roster rows) or nothing;
 *  - a draft's provenance is set only there and never changes: update() refuses a different provenance
 *    (`run_draft.provenance_immutable`) and a project change of a grouped draft
 *    (`run_draft.group_project_immutable`), insert() refuses a draft that carries provenance
 *    (`run_draft.group_insert_required`);
 *  - remove() of the LAST draft of a group removes the group in the same transaction;
 *  - a group therefore always has at least one draft.
 */
class IRunDraftRepository {
public:
    virtual ~IRunDraftRepository() = default;

    virtual common::Result<RunDraft> insert(RunDraft draft) = 0;
    virtual common::Result<RunDraft> update(RunDraft draft) = 0;
    [[nodiscard]] virtual common::Result<std::optional<RunDraft>> findById(
        const RunDraftId& id) const = 0;
    // Deterministic order (updatedAt descending, then id) - most recently
    // touched draft first, matching how a user expects a "recent drafts"
    // list to read.
    [[nodiscard]] virtual common::Result<std::vector<RunDraft>> listAll() const = 0;
    virtual common::Result<void> remove(const RunDraftId& id) = 0;

    // Refuses a group of the follow-up origin (checkRunDraftGroupPublicInsert: it is written only by the atomic Confirm of
    // its request, never through this public port). Validates with validateRunDraftGroupInsertion (failures
    // `run_draft_group.*`, nothing written), then writes the
    // group row and every draft in one transaction. Failure codes on top of the validation ones:
    // `run_draft_group.duplicate_id`, `run_draft_group.duplicate_idempotency_key` (the caller looks the key up
    // first and resolves a race by looking it up again) and `run_draft.duplicate_id`. The drafts come back in
    // ordinal order.
    virtual common::Result<std::vector<RunDraft>> insertGroup(RunDraftGroup group, std::vector<RunDraft> drafts) = 0;
    [[nodiscard]] virtual common::Result<std::optional<RunDraftGroup>> findGroupById(
        const RunDraftGroupId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<RunDraftGroup>> findGroupByIdempotencyKey(
        const std::string& idempotencyKey) const = 0;
    // The drafts of one group (roster included), ordered by ordinal; empty for an unknown group.
    [[nodiscard]] virtual common::Result<std::vector<RunDraft>> listGroupDrafts(const RunDraftGroupId& id) const = 0;
};

} // namespace ai::application
