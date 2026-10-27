#include "ai/application/proposals/ChangeProposalService.hpp"

#include "ai/application/proposals/ChangeProposalValidator.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/proposals/ProposedChangeSetParser.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

common::Error makeError(std::string code, std::string msg)
{
    return {std::move(code), std::move(msg)};
}

// Read the full content of a file, returning an error if it is missing or
// unreadable. Used to snapshot originals before publishing.
common::Result<std::string> readFileContent(
    const fs::path& path,
    std::size_t maximumBytes)
{
    std::ifstream in(detail::longPath(path), std::ios::binary);
    if (!in) {
        return common::Result<std::string>::failure(
            makeError("proposal.file_not_found",
                      "Could not open file: " + detail::genericUtf8(path.filename())));
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return common::Result<std::string>::failure(
            makeError("proposal.file_read_error",
                      "Could not determine original file size"));
    }
    if (static_cast<std::uintmax_t>(size) > maximumBytes) {
        return common::Result<std::string>::failure(
            makeError("proposal.original_too_large",
                      "Original file exceeds the proposal snapshot byte limit"));
    }
    in.seekg(0, std::ios::beg);

    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        in.read(content.data(), size);
    }
    if (!in) {
        return common::Result<std::string>::failure(
            makeError("proposal.file_read_error",
                      "Could not read file: " + detail::genericUtf8(path.filename())));
    }
    return common::Result<std::string>::ok(std::move(content));
}

std::optional<fs::path> hashSubjectPath(const ProposedFileChange& change)
{
    switch (change.operation) {
    case FileChangeOperation::Modify:
    case FileChangeOperation::Delete:
        return change.relativePath;
    case FileChangeOperation::Rename:
        return change.sourcePath;
    case FileChangeOperation::Create:
        return std::nullopt;
    }
    return std::nullopt;
}

// Once an audited tool session requires hash evidence, the backend-observed
// digest becomes the proposal's actual immutable precondition. Validation
// alone is not enough: ChangeSetApplyService later reads the persisted
// expectedOriginalHash, so leaving a model-typed typo there would create a
// ReadyForReview proposal that can never be applied even when the file has
// not changed.
void bindObservedHashes(
    ProposedChangeSet& changeSet,
    bool requireHashEvidence,
    const std::unordered_map<std::string, std::string>& observedFileHashes)
{
    if (!requireHashEvidence) return;

    for (auto& change : changeSet.changes) {
        const auto subjectPath = hashSubjectPath(change);
        if (!subjectPath.has_value()) continue;

        const auto evidence = observedFileHashes.find(
            detail::normalizedPathKey(*subjectPath));
        if (evidence == observedFileHashes.end()) continue;

        std::string trustedHash = evidence->second;
        std::transform(trustedHash.begin(), trustedHash.end(), trustedHash.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        change.expectedOriginalHash = std::move(trustedHash);
    }
}

// Manifest verification reliability hardening (lineage-aware repair
// materialization): a change's identity for matching a repair amendment
// against a predecessor change - NOT the same thing as hashSubjectPath
// above (which is only about which operations carry a hash precondition).
// Documented matching rule, one identity per operation:
//   Create/Modify/Delete -> the change's own relativePath (what it operates
//     on today).
//   Rename -> the change's own sourcePath (what it takes away from) - a
//     Rename's defining precondition is that its SOURCE currently exists;
//     its relativePath (the destination) does not exist yet, the same as a
//     Create's target, so relativePath cannot be Rename's identity without
//     colliding with an unrelated Create of that same destination name.
// Two changes with the same footprint are, by definition, two different
// proposals' operations on the exact same file - which is exactly the
// relationship a repair amendment has to the predecessor change it revises.
std::string changeFootprint(const ProposedFileChange& change)
{
    const auto& identityPath = change.operation == FileChangeOperation::Rename
        && change.sourcePath.has_value()
        ? *change.sourcePath
        : change.relativePath;
    return detail::normalizedPathKey(identityPath);
}

// Every normalized path identity a change occupies - its footprint (see
// above) AND, for Rename, its own relativePath too (the destination it is
// about to create). Used only for the final whole-set collision scan below;
// deliberately broader than changeFootprint's single-identity matching rule,
// since a Rename's destination must not silently collide with an unrelated
// change's target either.
std::vector<std::string> changeOccupiedKeys(const ProposedFileChange& change)
{
    std::vector<std::string> keys {detail::normalizedPathKey(change.relativePath)};
    if (change.operation == FileChangeOperation::Rename && change.sourcePath.has_value()) {
        keys.push_back(detail::normalizedPathKey(*change.sourcePath));
    }
    return keys;
}

// materializeChangeList's result: the merged change list, plus the exact
// set of PREDECESSOR changeIds that were carried forward unchanged (never
// touched by the repair). changeId is the right identity for this, not
// array position or content equality: a carried-forward entry is always the
// literal predecessor ProposedFileChange copy (same changeId the
// predecessor itself was validated and durably persisted with), and every
// predecessor changeId was already proven unique when the predecessor was
// created - so this set is exactly "which successor entries still need the
// predecessor's own hash evidence injected", independent of whatever
// changeId the repair submission's OWN new/replacement entries happen to
// use (which could coincidentally collide with an unrelated predecessor
// changeId string and must never be confused with one).
struct MaterializedChangeList {
    std::vector<ProposedFileChange> changes;
    std::unordered_set<std::string> carriedForwardPredecessorChangeIds;
};

// Manifest verification reliability hardening (lineage-aware repair
// materialization): the trusted Backend-owned merge - see
// ChangeProposalService::materializeRepairSuccessor's doc comment for the
// full contract this implements.
//   complete successor =
//       untouched validated predecessor changes
//       + replacements for predecessor paths amended by repair
//       + valid new non-conflicting repair changes
// Deterministic ordering: predecessor changes first (in their own original
// order, each either carried forward or replaced in place), then genuinely
// new repair changes (in the repair submission's own order) - so a restart
// or a second materialization of the same inputs always produces
// byte-identical ordering.
// A REPAIR-internal duplicate footprint (two repair changes both amending
// the same predecessor path) or any final whole-set path/identity collision
// (see changeOccupiedKeys) fails closed with
// verification.repair_ambiguous_overlap - this is a materialization-level
// pre-check; the ordinary ChangeProposalValidator::validate() re-run
// afterward remains the authoritative safety net for anything subtler.
common::Result<MaterializedChangeList> materializeChangeList(
    const std::vector<ProposedFileChange>& predecessorChanges,
    const std::vector<ProposedFileChange>& repairChanges)
{
    std::unordered_map<std::string, std::size_t> repairFootprintIndex;
    for (std::size_t index = 0; index < repairChanges.size(); ++index) {
        const auto footprint = changeFootprint(repairChanges[index]);
        if (!repairFootprintIndex.emplace(footprint, index).second) {
            return common::Result<MaterializedChangeList>::failure(
                {"verification.repair_ambiguous_overlap",
                 "The repair submission contains more than one change for the same "
                 "predecessor file/footprint"});
        }
    }

    MaterializedChangeList result;
    result.changes.reserve(predecessorChanges.size() + repairChanges.size());
    std::unordered_set<std::size_t> consumedRepairIndices;
    for (const auto& predecessorChange : predecessorChanges) {
        const auto footprint = changeFootprint(predecessorChange);
        const auto found = repairFootprintIndex.find(footprint);
        if (found != repairFootprintIndex.end()) {
            result.changes.push_back(repairChanges[found->second]);
            consumedRepairIndices.insert(found->second);
        } else {
            result.changes.push_back(predecessorChange);
            result.carriedForwardPredecessorChangeIds.insert(predecessorChange.changeId);
        }
    }
    for (std::size_t index = 0; index < repairChanges.size(); ++index) {
        if (!consumedRepairIndices.contains(index)) {
            result.changes.push_back(repairChanges[index]);
        }
    }

    // Defensive invariant: carrying forward every non-amended predecessor
    // change plus a 1-for-1 replacement for each amended one plus genuinely
    // new changes can never produce fewer entries than the predecessor
    // alone had. A violation here means a bug in the logic above, not a
    // legitimate outcome - fail closed rather than silently persist a
    // successor that dropped predecessor content.
    if (result.changes.size() < predecessorChanges.size()) {
        return common::Result<MaterializedChangeList>::failure(
            {"verification.repair_incomplete_replacement",
             "Materialized successor unexpectedly carries fewer changes than its "
             "predecessor"});
    }

    std::unordered_map<std::string, std::size_t> occupancy;
    for (const auto& change : result.changes) {
        for (const auto& key : changeOccupiedKeys(change)) {
            if (++occupancy[key] > 1) {
                return common::Result<MaterializedChangeList>::failure(
                    {"verification.repair_ambiguous_overlap",
                     "The materialized successor would contain more than one change "
                     "for the same target or rename-source path"});
            }
        }
    }

    return common::Result<MaterializedChangeList>::ok(std::move(result));
}

} // namespace

ChangeProposalService::ChangeProposalService(
    IChangeProposalRepository& repository,
    IChangeProposalStore& store,
    RunPathClaimRegistry* pathClaimRegistry)
    : repository_(repository)
    , store_(store)
    , pathClaimRegistry_(pathClaimRegistry)
{
}

common::Result<ProposedChangeSet> ChangeProposalService::createFromModelOutput(
    CreateChangeProposalRequest request)
{
    // --- Step 1: Parse raw model output -------------------------------------
    TrustedChangeSetMetadata meta;
    meta.runId            = request.runId;
    meta.projectId        = request.projectId;
    meta.providerId       = request.providerId;
    meta.modelId          = request.modelId;
    meta.modelExecutionId = request.modelExecutionId;
    meta.agentRole        = request.agentRole;
    meta.baseRevision     = request.baseRevision;
    meta.supersedesChangeSetId = request.supersedesChangeSetId;

    auto parseResult = ProposedChangeSetParser::parse(request.rawModelOutput, meta);
    if (!parseResult.success()) {
        return common::Result<ProposedChangeSet>::failure(*parseResult.error());
    }
    return validateAndPersist(std::move(parseResult.value()), request.policy,
        request.requireHashEvidence, request.observedFileHashes);
}

// Steps 2-8 of the class-level pipeline doc - see ChangeProposalService.hpp's
// validateAndPersist doc comment for the claim-rollback contract.
common::Result<ProposedChangeSet> ChangeProposalService::validateAndPersist(
    ProposedChangeSet cs,
    const ProjectWritePolicy& policy,
    bool requireHashEvidence,
    const std::unordered_map<std::string, std::string>& observedFileHashes)
{
    // Bind trusted evidence before secret scanning, validation, snapshotting,
    // publishing, and repository persistence. Missing evidence is deliberately
    // left missing here so ChangeProposalValidator can reject it fail-closed
    // with proposal.missing_hash_evidence.
    bindObservedHashes(cs, requireHashEvidence, observedFileHashes);

    // --- Step 2: Secret scan — fail fast, save nothing ----------------------
    const auto secretFindings = ChangeProposalValidator::scanForSecrets(cs);
    if (!secretFindings.empty()) {
        // Return the first finding's code/message; never echo the secret value.
        return common::Result<ProposedChangeSet>::failure(
            makeError(secretFindings.front().code, secretFindings.front().message));
    }

    // --- Step 3: Validate paths before any source-tree read -----------------
    const fs::path projectRoot = policy.projectRoot;
    std::vector<ValidationFinding> allFindings;
    std::error_code rootError;
    if (projectRoot.empty() || !projectRoot.is_absolute()
        || !fs::is_directory(detail::longPath(projectRoot), rootError) || rootError) {
        allFindings.push_back({
            "proposal.invalid_project_root",
            "Project root must be an existing absolute directory",
            {}});
    } else {
        allFindings =
            ChangeProposalValidator::validate(cs, policy, projectRoot,
                requireHashEvidence, observedFileHashes);
    }

    // Set (only) inside the block below when this changeset actually claims
    // paths in pathClaimRegistry_; read again after Step 6/7 to release (or
    // roll back) the claim if the changeset is abandoned before it can
    // durably persist.
    bool claimedPaths = false;
    // Manifest verification reliability hardening follow-up: the exact
    // snapshot a successful transferClaim reported for the superseded
    // predecessor (empty when there is no predecessor, or the plain
    // tryClaim branch ran instead, or the predecessor held nothing) - see
    // releaseClaimOnFailure below for why this must be reused verbatim
    // rather than reconstructed.
    std::vector<RunPathClaimRegistry::PathClaim> supersededClaimSnapshot;

    // No model-controlled path may reach filesystem APIs before the pure
    // structural/path/policy validation above has accepted the whole set.
    if (allFindings.empty()) {
    constexpr std::size_t defaultSnapshotLimit = 10ULL * 1024 * 1024;
    const std::size_t snapshotLimit = policy.maxTotalBytes == 0
        ? defaultSnapshotLimit
        : policy.maxTotalBytes;
    std::size_t capturedBytes = 0;

    for (auto& ch : cs.changes) {
        switch (ch.operation) {
        case FileChangeOperation::Modify:
        case FileChangeOperation::Delete: {
            const fs::path target = projectRoot / ch.relativePath;
            std::error_code ec;
            if (!fs::exists(detail::longPath(target), ec) || ec) {
                // Missing file — validator will add finding; skip snapshot.
                break;
            }
            const std::size_t remaining =
                capturedBytes < snapshotLimit ? snapshotLimit - capturedBytes : 0;
            auto contentResult = readFileContent(target, remaining);
            if (contentResult.success()) {
                capturedBytes += contentResult.value().size();
                ch.capturedOriginalContent = std::move(contentResult.value());
            } else {
                allFindings.push_back(
                    {contentResult.error()->code,
                     contentResult.error()->message,
                     ch.changeId});
            }
            break;
        }
        case FileChangeOperation::Rename: {
            if (!ch.sourcePath.has_value()) break;
            const fs::path src = projectRoot / *ch.sourcePath;
            std::error_code ec;
            if (!fs::exists(detail::longPath(src), ec) || ec) break;
            const std::size_t remaining =
                capturedBytes < snapshotLimit ? snapshotLimit - capturedBytes : 0;
            auto contentResult = readFileContent(src, remaining);
            if (contentResult.success()) {
                capturedBytes += contentResult.value().size();
                ch.capturedOriginalContent = std::move(contentResult.value());
            } else {
                allFindings.push_back(
                    {contentResult.error()->code,
                     contentResult.error()->message,
                     ch.changeId});
            }
            break;
        }
        case FileChangeOperation::Create:
            // No original to snapshot.
            break;
        }
    }

    const auto snapshotSecretFindings =
        ChangeProposalValidator::scanForSecrets(cs);
    if (!snapshotSecretFindings.empty()) {
        return common::Result<ProposedChangeSet>::failure(
            makeError(
                snapshotSecretFindings.front().code,
                snapshotSecretFindings.front().message));
    }

    // --- Step 4: Structural + policy + precondition validation ---------------
    const auto postSnapshotFindings =
        ChangeProposalValidator::validate(cs, policy, projectRoot,
            requireHashEvidence, observedFileHashes);
    allFindings.insert(
        allFindings.end(),
        postSnapshotFindings.begin(),
        postSnapshotFindings.end());

    // File-existence preconditions (the validator checks path safety but not
    // filesystem state — we do that here to keep the validator pure).
    for (const auto& ch : cs.changes) {
        std::error_code ec;
        switch (ch.operation) {
        case FileChangeOperation::Create: {
            const fs::path target = projectRoot / ch.relativePath;
            if (fs::exists(detail::longPath(target), ec) && !ec) {
                allFindings.push_back(
                    {"proposal.create_target_exists",
                     "Create target already exists on disk", ch.changeId});
            }
            break;
        }
        case FileChangeOperation::Modify:
        case FileChangeOperation::Delete: {
            const fs::path target = projectRoot / ch.relativePath;
            if (!fs::exists(detail::longPath(target), ec) || ec) {
                allFindings.push_back(
                    {"proposal.file_not_found",
                     "Target file does not exist", ch.changeId});
            }
            break;
        }
        case FileChangeOperation::Rename: {
            std::optional<fs::path> src;
            if (ch.sourcePath.has_value()) {
                src = projectRoot / *ch.sourcePath;
                if (!fs::exists(detail::longPath(*src), ec) || ec) {
                    allFindings.push_back(
                        {"proposal.file_not_found",
                         "Rename source file does not exist", ch.changeId});
                }
            }
            const fs::path tgt = projectRoot / ch.relativePath;
            if (fs::exists(detail::longPath(tgt), ec) && !ec) {
                // On a case-insensitive/case-preserving filesystem (the
                // default for NTFS on Windows and APFS/HFS+ on macOS), a
                // case-only rename's target path (e.g. "Main.qml") resolves
                // fs::exists() to the very file being renamed ("main.qml")
                // - that is the rename's own source, not a real collision.
                std::error_code eqEc;
                const bool targetIsRenameSource = src.has_value()
                    && fs::equivalent(detail::longPath(*src), detail::longPath(tgt), eqEc) && !eqEc;
                if (!targetIsRenameSource) {
                    allFindings.push_back(
                        {"proposal.rename_target_exists",
                         "Rename target path already exists", ch.changeId});
                }
            }
            break;
        }
        }
    }

    // --- Step 4.5: Cross-agent path-claim check (multi-agent parallel runs) -
    // Only reached when every finding above is still empty: a proposal
    // already rejected for its own reasons must not claim/block paths that
    // belong to it, since it can never legitimately race another agent for
    // ownership it isn't going to be granted anyway. This is deliberately a
    // separate mechanism from ChangeProposalValidator's intra-changeset
    // proposal.conflicting_path check: that one catches two changes in the
    // SAME proposal targeting one path; this one catches two DIFFERENT
    // proposals (different changeSetId, produced by two agents racing in the
    // same run) doing so - something a single, pure validate() call over one
    // ProposedChangeSet can never see.
    if (allFindings.empty() && pathClaimRegistry_ != nullptr) {
        std::vector<RunPathClaimRegistry::PathClaim> claims;
        claims.reserve(cs.changes.size() * 2);
        for (const auto& ch : cs.changes) {
            claims.push_back({detail::genericUtf8(ch.relativePath), ch.changeId});
            if (ch.sourcePath.has_value()) {
                claims.push_back({detail::genericUtf8(*ch.sourcePath), ch.changeId});
            }
        }
        // Manifest verification reliability hardening: a proposal that
        // durably supersedes an earlier, already-claimed one (see
        // ProposedChangeSet::supersedesChangeSetId) is replacing that exact
        // task's own prior claim, not racing another agent for it -
        // transferClaim atomically releases the superseded change set's
        // claim and acquires this one under a single lock acquisition, so a
        // conflict restores the superseded claim exactly instead of losing
        // it (see RunPathClaimRegistry::transferClaim's own doc comment).
        // Safe no-op transfer-source when the superseded id never actually
        // held a claim (e.g. it was Rejected before ever reaching this
        // step). The superseded row itself is never mutated or deleted by
        // this - only the transient in-memory claim moves to its successor.
        const auto claimResult = cs.supersedesChangeSetId.has_value()
            ? pathClaimRegistry_->transferClaim(
                  cs.runId, *cs.supersedesChangeSetId, cs.changeSetId, claims)
            : pathClaimRegistry_->tryClaim(cs.runId, cs.changeSetId, claims);
        if (claimResult.claimed) {
            claimedPaths = true;
            // Follow-up hardening: capture the EXACT snapshot transferClaim
            // itself reports (never a separately reconstructed
            // approximation - see ClaimOutcome::previousClaims's own doc
            // comment) for releaseClaimOnFailure below. Always empty for
            // the plain tryClaim branch (no supersedesChangeSetId, so there
            // is nothing to roll back to).
            supersededClaimSnapshot = claimResult.previousClaims;
        } else {
            allFindings.push_back(
                {"proposal.cross_agent_conflict",
                 "Path is already claimed by change '" + claimResult.conflictingChangeId
                     + "' in change set '" + claimResult.conflictingChangeSetId
                     + "' from another agent in this run",
                 claimResult.conflictingChangeId});
        }
    }
    }

    // Manifest verification reliability hardening follow-up: undoes a
    // successful Step 4.5 claim acquisition when a LATER step still fails
    // (publish or repository insert). Reverses via transferClaim ONLY when
    // supersededClaimSnapshot is non-empty - i.e. the superseded predecessor
    // actually held a claim at the moment of the forward transfer, per that
    // exact call's own returned snapshot - so a predecessor that never held
    // one (e.g. it was created through a caller with no path-claim registry
    // wired) can never end this call owning a claim it never actually had.
    // Falls back to a plain release of this changeset's own transient claim
    // in every other case.
    auto releaseClaimOnFailure = [&]() {
        if (!claimedPaths) return;
        if (cs.supersedesChangeSetId.has_value() && !supersededClaimSnapshot.empty()) {
            static_cast<void>(pathClaimRegistry_->transferClaim(
                cs.runId, cs.changeSetId, *cs.supersedesChangeSetId,
                supersededClaimSnapshot));
        } else {
            pathClaimRegistry_->release(cs.runId, cs.changeSetId);
        }
    };

    // Persist the terminal state directly so the immutable manifest and the
    // SQLite row cannot diverge across a crash between two state updates.
    // An otherwise-clean but empty change set is NoChangesNeeded, not
    // ReadyForReview (there is nothing to review or apply) and not Rejected
    // (nothing about it was actually wrong).
    cs.state = !allFindings.empty() ? ChangeSetState::Rejected
             : cs.changes.empty()   ? ChangeSetState::NoChangesNeeded
                                    : ChangeSetState::ReadyForReview;
    cs.validationFindings = allFindings;

    // --- Step 6: Atomically publish to IChangeProposalStore -----------------
    auto publishResult = store_.publishChangeSet(cs, policy);
    if (!publishResult.success()) {
        releaseClaimOnFailure();
        return common::Result<ProposedChangeSet>::failure(*publishResult.error());
    }
    cs.artifactRelativeRoot = publishResult.value().artifactRelativeRoot;
    cs.policySnapshotHash   = publishResult.value().policySnapshotHash;

    // --- Step 7: Persist metadata to IChangeProposalRepository ---------------
    auto insertResult = repository_.insertChangeSet(cs);
    if (!insertResult.success()) {
        // Best-effort: leave the published directory (it is immutable and not
        // harmful); the metadata row simply won't exist so it stays invisible.
        releaseClaimOnFailure();
        return common::Result<ProposedChangeSet>::failure(*insertResult.error());
    }

    // --- Step 8: Set terminal state ----------------------------------------
    const ChangeSetState finalState = !allFindings.empty() ? ChangeSetState::Rejected
                                     : cs.changes.empty()   ? ChangeSetState::NoChangesNeeded
                                                             : ChangeSetState::ReadyForReview;
    auto stateResult = repository_.updateChangeSetState(cs.changeSetId, finalState);
    if (stateResult.success()) {
        // Only adopt the new state; keep validationFindings and other fields
        // set above — the repository header row does not carry findings.
        cs.state = finalState;
    } else {
        // Non-fatal: the proposal is stored but state could not be advanced.
        // Leave cs in Validated state; caller can retry via a future management API.
        cs.state = finalState;
    }

    // Reachable via NoChangesNeeded: an empty changes[] trivially "claims"
    // zero paths (claimedPaths becomes true with nothing actually held), but
    // finalState is NoChangesNeeded rather than ReadyForReview. Release keeps
    // this a no-op in that case and guards any future refactor of the gate
    // above from leaving a real claim held by a changeset that isn't
    // durably ReadyForReview. (A materialized successor's changes[] can
    // never be empty - see materializeChangeList's size invariant - so this
    // path is unreachable for materializeRepairSuccessor specifically; a
    // plain release is correct here regardless.)
    if (claimedPaths && finalState != ChangeSetState::ReadyForReview) {
        pathClaimRegistry_->release(cs.runId, cs.changeSetId);
    }

    return common::Result<ProposedChangeSet>::ok(std::move(cs));
}

common::Result<ProposedChangeSet> ChangeProposalService::materializeRepairSuccessor(
    MaterializeRepairSuccessorRequest request)
{
    auto fail = [](std::string code, std::string message) {
        return common::Result<ProposedChangeSet>::failure(
            {std::move(code), std::move(message)});
    };

    // --- Step A: load and validate predecessor identity/state --------------
    auto predecessorLookup = repository_.findChangeSet(request.predecessorChangeSetId);
    if (!predecessorLookup.success()) {
        return fail("verification.repair_predecessor_corrupt",
            "The predecessor proposal could not be read from durable storage");
    }
    if (!predecessorLookup.value().has_value()) {
        return fail("verification.repair_predecessor_missing",
            "The predecessor proposal this repair was expected to amend no longer exists");
    }
    const ProposedChangeSet predecessor = std::move(*predecessorLookup.value());
    // MW-P5.4: expectedPredecessorRunId narrows this check to the one
    // trusted cross-Run case (see its own doc comment) without loosening it
    // for any other caller - every existing (non-continuation) call site
    // leaves it unset, so predecessor.runId must still equal request.runId
    // exactly as before.
    const RunId& expectedPredecessorRunId =
        request.expectedPredecessorRunId.has_value()
            ? *request.expectedPredecessorRunId
            : request.runId;
    if (predecessor.runId != expectedPredecessorRunId
            || predecessor.projectId != request.projectId) {
        return fail("verification.repair_predecessor_mismatch",
            "The predecessor proposal does not belong to this Run/project");
    }
    if (predecessor.state != ChangeSetState::ReadyForReview) {
        return fail("verification.repair_predecessor_mismatch",
            "The predecessor proposal is not in a replaceable state");
    }

    // --- Step B: load predecessor's structural changes + durable content ---
    auto predecessorChangesLookup = repository_.listChanges(request.predecessorChangeSetId);
    if (!predecessorChangesLookup.success()) {
        return fail("verification.repair_predecessor_corrupt",
            "The predecessor proposal's changes could not be read from durable storage");
    }
    std::vector<ProposedFileChange> predecessorChanges =
        std::move(predecessorChangesLookup.value());
    if (predecessorChanges.empty()) {
        // A ReadyForReview change set with zero changes cannot exist (an
        // empty proposal is always NoChangesNeeded) - a predecessor that
        // somehow has none is a corrupt/ambiguous record, not a valid
        // replacement target.
        return fail("verification.repair_predecessor_corrupt",
            "The predecessor proposal has no recorded changes");
    }
    for (auto& predecessorChange : predecessorChanges) {
        if (predecessorChange.operation == FileChangeOperation::Create
            || predecessorChange.operation == FileChangeOperation::Modify) {
            auto content = store_.readProposedContent(
                predecessor.artifactRelativeRoot, predecessorChange.changeId);
            if (!content.success()) {
                return fail("verification.repair_predecessor_corrupt",
                    "A predecessor proposal's durable content could not be read");
            }
            predecessorChange.proposedContent = std::move(content.value());
        }
    }

    // --- Step C: parse the repair's raw output as an amendment --------------
    // Reuses the exact same proposal-submission.v2 parser/contract every
    // ordinary submission goes through - a materialization-mode repair
    // submission is held to the identical shape rules (see
    // ProposedChangeSetParser::parse's own error codes, which
    // isModelFormatError still recognizes for the existing one-shot
    // same-session repair retry). supersedesChangeSetId is set here from
    // the TRUSTED predecessor id, never anything the model could influence.
    TrustedChangeSetMetadata meta;
    meta.runId = request.runId;
    meta.projectId = request.projectId;
    meta.providerId = request.providerId;
    meta.modelId = request.modelId;
    meta.modelExecutionId = request.modelExecutionId;
    meta.agentRole = request.agentRole;
    meta.baseRevision = request.baseRevision;
    meta.supersedesChangeSetId = request.predecessorChangeSetId;

    auto parseResult = ProposedChangeSetParser::parse(request.repairRawModelOutput, meta);
    if (!parseResult.success()) {
        return common::Result<ProposedChangeSet>::failure(*parseResult.error());
    }
    ProposedChangeSet repairAmendment = std::move(parseResult.value());

    // --- Step D: materialize the complete successor's change list -----------
    auto materialized = materializeChangeList(predecessorChanges, repairAmendment.changes);
    if (!materialized.success()) {
        return common::Result<ProposedChangeSet>::failure(*materialized.error());
    }

    // --- Step E: assemble the complete successor + merged hash evidence -----
    ProposedChangeSet successor;
    successor.schemaVersion = "proposal.v1";
    successor.changeSetId = generateUuid();
    successor.runId = request.runId;
    successor.projectId = request.projectId;
    successor.providerId = request.providerId;
    successor.modelId = request.modelId;
    successor.modelExecutionId = request.modelExecutionId;
    successor.agentRole = request.agentRole;
    successor.baseRevision = request.baseRevision;
    successor.supersedesChangeSetId = request.predecessorChangeSetId;
    successor.summary = repairAmendment.summary;
    successor.changes = materialized.value().changes;
    successor.createdAt = std::chrono::system_clock::now();
    successor.state = ChangeSetState::Received;

    // For every CARRIED-FORWARD (never touched by the repair) Modify/
    // Delete/Rename change - identified by materializeChangeList's own
    // authoritative carriedForwardPredecessorChangeIds set, not by
    // reasoning about changeId collisions here - inject the predecessor's
    // own already-durable Backend-observed hash evidence, never a model-
    // authored value. bindObservedHashes (inside validateAndPersist)
    // requires a matching EVIDENCE MAP entry regardless of what
    // expectedOriginalHash already contains, so this must populate the map,
    // not just carry the field forward (already true by construction, since
    // a carried-forward entry is the literal predecessor ProposedFileChange
    // copy).
    std::unordered_map<std::string, std::string> mergedHashes = request.observedFileHashes;
    for (const auto& predecessorChange : predecessorChanges) {
        if (!materialized.value().carriedForwardPredecessorChangeIds.contains(
                predecessorChange.changeId)) {
            continue;
        }
        const auto subjectPath = hashSubjectPath(predecessorChange);
        if (!subjectPath.has_value() || !predecessorChange.expectedOriginalHash.has_value()) {
            continue;
        }
        const auto key = detail::normalizedPathKey(*subjectPath);
        mergedHashes.emplace(key, *predecessorChange.expectedOriginalHash);
    }

    // --- Step F: run the complete successor through the ordinary boundary --
    // validateAndPersist's own Step 4.5 acquires the successor's claim via
    // transferClaim(..., *successor.supersedesChangeSetId, ...) and keeps
    // the EXACT snapshot that call reports for the predecessor - never a
    // separately reconstructed path list here - so a later failure in this
    // same call (publish/insert) restores precisely what the predecessor
    // actually held, nothing more.
    return validateAndPersist(std::move(successor), request.policy,
        /*requireHashEvidence=*/true, mergedHashes);
}

common::Result<void> ChangeProposalService::markMissingEvidence(
    const ChangeSetId& id, std::string code, std::string message)
{
    return repository_.markMissingEvidence(id, std::move(code), std::move(message));
}

void ChangeProposalService::releaseRunClaims(const RunId& runId)
{
    if (pathClaimRegistry_ != nullptr) pathClaimRegistry_->releaseRun(runId);
}

common::Result<std::vector<ProposedChangeSet>> ChangeProposalService::listForRun(
    const RunId& runId) const
{
    return repository_.listForRun(runId);
}

common::Result<std::vector<ProposedChangeSet>> ChangeProposalService::listForProject(
    const ProjectId& projectId) const
{
    return repository_.listForProject(projectId);
}

common::Result<ProposedChangeSet> ChangeProposalService::findChangeSet(
    const ChangeSetId& id) const
{
    auto result = repository_.findChangeSet(id);
    if (!result.success()) {
        return common::Result<ProposedChangeSet>::failure(*result.error());
    }
    if (!result.value().has_value()) {
        return common::Result<ProposedChangeSet>::failure(
            makeError("proposal.not_found", "Change set not found: " + id));
    }
    return common::Result<ProposedChangeSet>::ok(std::move(*result.value()));
}

common::Result<std::vector<ProposedFileChange>> ChangeProposalService::listChanges(
    const ChangeSetId& changeSetId) const
{
    return repository_.listChanges(changeSetId);
}

common::Result<std::string> ChangeProposalService::readOriginalContent(
    const ChangeSetId& changeSetId, const ChangeId& changeId) const
{
    auto csResult = findChangeSet(changeSetId);
    if (!csResult.success()) {
        return common::Result<std::string>::failure(*csResult.error());
    }
    return store_.readOriginalContent(csResult.value().artifactRelativeRoot, changeId);
}

common::Result<std::string> ChangeProposalService::readProposedContent(
    const ChangeSetId& changeSetId, const ChangeId& changeId) const
{
    auto csResult = findChangeSet(changeSetId);
    if (!csResult.success()) {
        return common::Result<std::string>::failure(*csResult.error());
    }
    return store_.readProposedContent(csResult.value().artifactRelativeRoot, changeId);
}

common::Result<std::vector<ValidationFinding>> ChangeProposalService::listValidationFindings(
    const ChangeSetId& changeSetId) const
{
    return repository_.listValidationFindings(changeSetId);
}

common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
ChangeProposalService::listChangesForSets(const std::vector<ChangeSetId>& changeSetIds) const
{
    return repository_.listChangesForSets(changeSetIds);
}

common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
ChangeProposalService::listValidationFindingsForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    return repository_.listValidationFindingsForSets(changeSetIds);
}

} // namespace ai::application
