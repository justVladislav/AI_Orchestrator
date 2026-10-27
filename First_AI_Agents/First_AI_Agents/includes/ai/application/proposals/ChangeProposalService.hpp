#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/proposals/IChangeProposalStore.hpp"
#include "ai/application/workspace/ProjectWritePolicy.hpp"
#include "ai/application/RunPathClaimRegistry.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::application {

// Command DTO for the public pipeline entry point.
// rawModelOutput is the raw text the provider returned.
// policy comes from the trusted Backend/Application caller; the model
// cannot influence it.
struct CreateChangeProposalRequest {
    RunId runId;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    std::optional<ModelExecutionId> modelExecutionId;
    std::string agentRole;
    std::string rawModelOutput;
    ProjectWritePolicy policy;
    std::string baseRevision; // supplied by trusted caller, may be empty

    // See ProposedChangeSet::supersedesChangeSetId - set only by a caller
    // issuing a repair retry, pointing back at the changeSetId of the
    // attempt this one replaces.
    std::optional<ChangeSetId> supersedesChangeSetId;

    // See ChangeProposalValidator::validate()'s doc comment for the exact
    // contract. Set by a caller that actually ran an audited agent tool
    // session (AgentToolTurnLoopRunner) so a Modify/Delete/Rename original-
    // hash precondition is populated from, validated against, and persisted as
    // real observed evidence. proposal-submission.v2 never accepts a model-
    // supplied hash. Consequently, a direct caller that wants a ReadyForReview
    // Modify/Delete/Rename proposal must collect trusted hashes and enable this
    // gate; the false/empty default remains useful for Create-only seeding and
    // for tests that intentionally exercise missing-precondition rejection.
    bool requireHashEvidence {false};
    std::unordered_map<std::string, std::string> observedFileHashes;
};

// Manifest verification reliability hardening (lineage-aware repair
// materialization): command DTO for the trusted Backend-owned repair-
// materialization entry point. Populated ONLY by the bounded combined-
// verification repair path in ExecutionManifestExecutor (via
// AgentToolTurnLoopRequest::repairMaterializationPredecessorId) - never
// inferred from provider/model data. predecessorChangeSetId,
// runId/projectId/policy, and every other trusted field here are exactly
// that: trusted. repairRawModelOutput is the ONLY untrusted input, and it is
// treated strictly as an AMENDMENT to the predecessor (see
// ChangeProposalService::materializeRepairSuccessor's doc comment), never as
// a complete replacement proposal on its own.
struct MaterializeRepairSuccessorRequest {
    RunId runId;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    std::optional<ModelExecutionId> modelExecutionId;
    std::string agentRole;
    ChangeSetId predecessorChangeSetId;
    std::string repairRawModelOutput;
    ProjectWritePolicy policy;
    std::string baseRevision;
    // MW-P5.4 (durable failed-verification continuation): when set, the
    // predecessor is expected to belong to THIS Run instead of `runId` - the
    // one narrow cross-Run case where a continuation child Run's repair
    // worker amends a proposal that still lives on its terminal source Run
    // (never copied there - see ManifestVerificationContinuationTypes.hpp's
    // "reference, never copy" principle). Only ever set by
    // ManifestVerificationContinuationRepairService after proving the link
    // through a durable MW-P5.1 admission + MW-P5.3 frozen input for this
    // exact (predecessorChangeSetId, runId) pair; never inferred from
    // provider/model data, never set by any ordinary (non-continuation)
    // caller, which keeps materializeRepairSuccessor's existing same-Run
    // guarantee exactly as strict as before for every other caller.
    std::optional<RunId> expectedPredecessorRunId;
    // The repair session's OWN audited hash evidence (from
    // AgentToolTurnLoopRunner::preflightTrustedHashes, exactly like
    // CreateChangeProposalRequest::observedFileHashes) - covers whatever the
    // repair worker itself actually re-read/re-hashed in this session.
    // materializeRepairSuccessor separately injects the predecessor's own
    // already-durable hash evidence for every CARRIED-forward Modify/Delete/
    // Rename change the repair never touched; this map is never required to
    // already contain those.
    std::unordered_map<std::string, std::string> observedFileHashes;
};

// Application service: Stage 1 safe change-proposal pipeline.
//
// PUBLIC PIPELINE (createFromModelOutput)
//   1. Parse raw model output → ProposedChangeSet (Received).
//      Parse failure → return error, save nothing.
//   2. Secret scan (content, summary, paths).
//      Secret detected → return proposal.secret_detected, save nothing.
//   3. Structural + path + policy validation before any filesystem read.
//   4. For path-safe proposals only, read bounded original snapshots and
//      validate hashes/filesystem preconditions.
//   5. Set terminal state = ReadyForReview or Rejected before publishing.
//   6. Atomically publish to IChangeProposalStore (staging → final).
//      Publish failure → return error, save nothing.
//   7. Persist metadata to IChangeProposalRepository.
//      Persist failure → remove published directory (compensation), return error.
//   8. Return the final ProposedChangeSet.
//
// No apply/backup/rollback/shell/git is performed here.
// UI must not receive filesystem paths; content is accessed only through
// typed changeSetId + changeId pairs.
class ChangeProposalService {
public:
    // pathClaimRegistry is optional: when null (the default), no cross-agent
    // path-conflict check is performed and this service behaves exactly as
    // it did before that check existed - the correct choice for single-agent
    // callers (e.g. read-only analysis) where no other agent can race it.
    ChangeProposalService(
        IChangeProposalRepository& repository,
        IChangeProposalStore& store,
        RunPathClaimRegistry* pathClaimRegistry = nullptr);

    // Main pipeline entry point.  See class-level doc for the exact steps.
    common::Result<ProposedChangeSet> createFromModelOutput(
        CreateChangeProposalRequest request);

    // Manifest verification reliability hardening (lineage-aware repair
    // materialization): the trusted Backend-owned counterpart to
    // createFromModelOutput for exactly one case - a bounded combined-
    // verification repair amending an already-ReadyForReview predecessor
    // proposal. The model's raw output is parsed with the SAME
    // ProposedChangeSetParser/proposal-submission.v2 contract, but its
    // resulting changes[] is then treated strictly as an AMENDMENT, never a
    // complete replacement: for every predecessor change whose own
    // path/footprint (see the .cpp for the exact per-operation rule) is not
    // unambiguously amended by the repair, the predecessor's own durable
    // validated change (content and, for Modify/Delete/Rename, hash
    // evidence) is carried forward unchanged; an unambiguous same-footprint
    // repair change replaces it exactly once; a genuinely new, non-
    // conflicting repair change is appended. The fully materialized result
    // is a brand-new ProposedChangeSet that goes through the ordinary
    // validate/secret-scan/policy/hash-evidence/path-claim/publish/persist
    // pipeline again in full (see validateAndPersist) - a materialized
    // change is never trusted merely because it originated from a validated
    // predecessor. Fails closed with a verification.repair_predecessor_*/
    // verification.repair_ambiguous_overlap/verification.repair_materialization_failed
    // code (see the .cpp) before ever touching the predecessor's identity,
    // content, or path claim if the predecessor cannot be trusted as-is.
    // Never writes to the live workspace. The predecessor row itself is
    // never mutated or deleted; the returned successor carries
    // supersedesChangeSetId back to it.
    common::Result<ProposedChangeSet> materializeRepairSuccessor(
        MaterializeRepairSuccessorRequest request);

    // Pass-through to IChangeProposalRepository::markMissingEvidence() - see
    // its doc comment. Exposed on the service (not just the repository) so
    // AgentToolTurnLoopRunner, which only holds a ChangeProposalService&,
    // can call it without reaching past the service boundary.
    common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message);

    // Terminal-workflow cleanup for Backend-owned proposal producers that do
    // not execute through RunManager (for example QML-P4 manual-handoff
    // re-verification). Claims are only an in-process concurrency guard; once
    // the owning Run is terminal they must never remain pinned. No-op when
    // this service was composed without a RunPathClaimRegistry.
    void releaseRunClaims(const RunId& runId);

    // Queries — all return Result so storage failures surface cleanly.
    [[nodiscard]] common::Result<std::vector<ProposedChangeSet>> listForRun(
        const RunId& runId) const;
    [[nodiscard]] common::Result<std::vector<ProposedChangeSet>> listForProject(
        const ProjectId& projectId) const;
    [[nodiscard]] common::Result<ProposedChangeSet> findChangeSet(
        const ChangeSetId& id) const;
    [[nodiscard]] common::Result<std::vector<ProposedFileChange>> listChanges(
        const ChangeSetId& changeSetId) const;
    [[nodiscard]] common::Result<std::string> readOriginalContent(
        const ChangeSetId& changeSetId, const ChangeId& changeId) const;
    [[nodiscard]] common::Result<std::string> readProposedContent(
        const ChangeSetId& changeSetId, const ChangeId& changeId) const;
    [[nodiscard]] common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId& changeSetId) const;
    // Batched counterparts for a caller (AgentToolReadModelService::session())
    // that otherwise issues listChanges()+listValidationFindings() once per
    // proposal in a session - see IChangeProposalRepository for the batching
    // contract these forward to.
    [[nodiscard]] common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& changeSetIds) const;
    [[nodiscard]] common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>& changeSetIds) const;

private:
    // Steps 2-8 of the pipeline described on the class - secret scan,
    // structural/path/policy validation, snapshotting, hash-evidence
    // binding, path-claim (acquire, or atomically transfer from
    // cs.supersedesChangeSetId's owner when set), publish, and repository
    // persistence - shared between createFromModelOutput (after parsing raw
    // text) and materializeRepairSuccessor (after materializing a complete
    // successor). When cs.supersedesChangeSetId is set and the forward
    // transferClaim succeeds, the predecessor's exact prior-claim snapshot
    // (RunPathClaimRegistry::ClaimOutcome::previousClaims - never a
    // separately reconstructed approximation) is kept so a LATER failure in
    // this same call (publish/insert) can restore it via the reverse
    // transferClaim, rather than losing it or fabricating a claim the
    // predecessor never actually held.
    common::Result<ProposedChangeSet> validateAndPersist(
        ProposedChangeSet cs,
        const ProjectWritePolicy& policy,
        bool requireHashEvidence,
        const std::unordered_map<std::string, std::string>& observedFileHashes);

    IChangeProposalRepository& repository_;
    IChangeProposalStore& store_;
    RunPathClaimRegistry* pathClaimRegistry_ {nullptr};
};

} // namespace ai::application
