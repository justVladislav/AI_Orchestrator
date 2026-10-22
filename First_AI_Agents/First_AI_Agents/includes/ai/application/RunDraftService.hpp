#pragma once

#include "ai/application/IRunDraftRepository.hpp"

namespace ai::application {

/**
 * Coordinates RunDraft commands. Unlike ExecutionProfileService, a draft's
 * roster is deliberately not validated against the live Provider/Model
 * catalog at write time: a draft is explicitly a not-yet-executable, freely
 * editable in-progress state (it is only ever rehydrated back into
 * NewRunDialog for further editing, never executed directly), so a
 * temporarily stale or incomplete roster entry is expected and must not
 * block saving.
 */
class RunDraftService {
public:
    explicit RunDraftService(IRunDraftRepository& drafts);

    [[nodiscard]] common::Result<std::vector<RunDraft>> listDrafts() const;
    [[nodiscard]] common::Result<std::optional<RunDraft>> findDraft(const RunDraftId& id) const;
    // PC-3D: the group a plan draft belongs to (read-only; the Drafts list shows its title and origin) and its
    // drafts in ordinal order (to name the drafts a draft depends on).
    [[nodiscard]] common::Result<std::optional<RunDraftGroup>> findGroup(const RunDraftGroupId& id) const;
    [[nodiscard]] common::Result<std::vector<RunDraft>> listGroupDrafts(const RunDraftGroupId& id) const;
    common::Result<RunDraftId> createDraft(CreateRunDraftRequest request);
    common::Result<void> updateDraft(UpdateRunDraftRequest request);
    common::Result<void> removeDraft(const RunDraftId& id);

private:
    IRunDraftRepository& drafts_;
};

} // namespace ai::application
