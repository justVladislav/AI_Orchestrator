#pragma once

#include "ai/application/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace ai::application {

// Persistence port for change-set metadata and per-change items.
// Implementations must be thread-safe.  The record of an inserted
// change set is immutable except for the state field and the
// missing-evidence annotation markMissingEvidence() writes.
class IChangeProposalRepository {
public:
    virtual ~IChangeProposalRepository() = default;

    // Insert a fully-constructed change set.
    // Fails with "proposal.duplicate_id" if changeSetId already exists.
    virtual common::Result<void> insertChangeSet(
        const ProposedChangeSet& changeSet) = 0;

    // Transition the state of an existing change set.
    // Returns the updated record on success.
    // Fails with "proposal.not_found" if id is unknown.
    virtual common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) = 0;

    // Records why a persisted NoChangesNeeded change set was not trusted
    // as-is (AgentToolTurnLoopRunner's no_changes_missing_evidence repair
    // case, called after this row is already durably persisted, before the
    // repair retry runs). Does not change state, which stays
    // NoChangesNeeded - see ProposedChangeSet::missingEvidenceCode's doc
    // comment. Fails with "proposal.not_found" if id is unknown.
    virtual common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const = 0;

    // Ordered by createdAt ascending.
    [[nodiscard]] virtual common::Result<std::vector<ProposedChangeSet>> listForRun(
        const RunId& runId) const = 0;

    // Ordered by createdAt ascending.
    [[nodiscard]] virtual common::Result<std::vector<ProposedChangeSet>> listForProject(
        const ProjectId& projectId) const = 0;

    // Returns the file-change items belonging to the change set, in
    // insertion order.
    [[nodiscard]] virtual common::Result<std::vector<ProposedFileChange>> listChanges(
        const ChangeSetId& changeSetId) const = 0;

    // Returns the validation findings for the change set (empty when valid).
    [[nodiscard]] virtual common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId& changeSetId) const = 0;

    // Batched counterparts of listChanges()/listValidationFindings() for a
    // caller (AgentToolReadModelService::session()) that otherwise issues one
    // query per change set for every proposal in a session - an N+1 that
    // runs synchronously on the Qt GUI thread on every session poll. A
    // missing changeSetId in the input is simply absent from the result map
    // rather than an error. An empty input returns an empty map without
    // touching the database.
    [[nodiscard]] virtual common::Result<
        std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& changeSetIds) const = 0;

    [[nodiscard]] virtual common::Result<
        std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>& changeSetIds) const = 0;
};

} // namespace ai::application
