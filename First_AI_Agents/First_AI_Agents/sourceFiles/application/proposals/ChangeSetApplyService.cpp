#include "ai/application/proposals/ChangeSetApplyService.hpp"

#include "ai/application/planning/IOutcomeBriefTrigger.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/proposals/IChangeApprovalRepository.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/proposals/IChangeProposalStore.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/execution/IRunRepository.hpp"
#include "ai/application/verification/ManifestApplyVerificationGate.hpp"
#include "ai/application/execution/ManifestWorkLedgerService.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

// Returns true when absPath is strictly inside canonRoot (both lexically
// normalised; absPath must not equal canonRoot itself).
bool isInsideRoot(const fs::path& absPath, const fs::path& canonRoot)
{
    const fs::path np = absPath.lexically_normal();
    const fs::path nr = canonRoot.lexically_normal();
    auto it = np.begin();
    for (const auto& comp : nr) {
        if (it == np.end() || *it != comp) return false;
        ++it;
    }
    // it must still have at least one component left (the file is inside, not at root)
    return it != np.end();
}

std::optional<std::string> readFileContent(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool writeFileContent(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// std::filesystem::rename() can silently no-op a case-only rename on a
// case-insensitive/case-preserving filesystem (NTFS on Windows, APFS/HFS+ on
// macOS): the OS resolves "main.qml" and "Main.qml" to the same directory
// entry, so a direct rename between them may leave the on-disk casing
// unchanged. Route those renames through a temporary name in the same
// directory so the case change actually lands; every other rename goes
// through directly.
void renameFileOnDisk(const fs::path& from, const fs::path& to, std::error_code& ec)
{
    std::error_code eqEc;
    const bool caseOnly = fs::equivalent(from, to, eqEc) && !eqEc;
    if (!caseOnly) {
        fs::rename(from, to, ec);
        return;
    }
    const fs::path tempPath = from.parent_path() / (".rename-temp-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::rename(from, tempPath, ec);
    if (ec) return;
    fs::rename(tempPath, to, ec);
    if (ec) {
        std::error_code restoreEc;
        fs::rename(tempPath, from, restoreEc); // best-effort restore of the original name
    }
}

// Rollback info for one applied change.
struct AppliedEntry {
    FileChangeOperation op;
    fs::path absPath;                         // Rename: the file's location *after* the rename (the target)
    std::optional<std::string> savedContent;  // Modify: content before overwrite; Delete: content before deletion
    std::optional<fs::path> renameSourcePath; // Rename only: original location, for rollback
};

// Best-effort rollback in reverse; returns true if every undo succeeded.
bool rollbackApplied(const std::vector<AppliedEntry>& applied)
{
    bool ok = true;
    std::error_code ec;
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        switch (it->op) {
        case FileChangeOperation::Create:
            fs::remove(it->absPath, ec);
            if (ec) ok = false;
            break;
        case FileChangeOperation::Modify:
            if (it->savedContent && !writeFileContent(it->absPath, *it->savedContent))
                ok = false;
            break;
        case FileChangeOperation::Delete:
            if (!it->savedContent || !writeFileContent(it->absPath, *it->savedContent))
                ok = false;
            break;
        case FileChangeOperation::Rename:
            if (!it->renameSourcePath) {
                ok = false;
                break;
            }
            renameFileOnDisk(it->absPath, *it->renameSourcePath, ec);
            if (ec) ok = false;
            break;
        }
    }
    return ok;
}

template<typename T>
common::Result<T> failWith(std::string code, std::string msg)
{
    return common::Result<T>::failure({std::move(code), std::move(msg)});
}

// Deterministic manifest hash: an approval binds to this exact set of
// changes (id/operation/path/expected-hash), not their proposed file
// content. A genuinely-edited proposal always gets a new changeSetId (see
// ChangeProposalService), so this only needs to catch this row's own
// changes somehow differing from what was approved - it does not need to
// re-hash file content.
std::string computeSnapshotHash(const std::vector<ProposedFileChange>& changes)
{
    std::string manifest;
    for (const auto& change : changes) {
        manifest += change.changeId;
        manifest += '|';
        manifest += toString(change.operation);
        manifest += '|';
        manifest += detail::genericUtf8(change.relativePath);
        manifest += '|';
        manifest += change.expectedOriginalHash.value_or(std::string {});
        manifest += '\n';
    }
    return sha256Hex(manifest);
}

// AG-14.1: errorCode is set on any failure (see the individual return sites
// below); fingerprint carries the real WorkspaceBaselineSnapshot::fingerprint
// of the post-apply workspace (WorkspaceBaselineProvenance.hpp) on success -
// the same value this function already computes for its own report, now
// also surfaced to the caller instead of discarded. Never a homegrown hash
// of the apply's own hash log.
struct PostApplyProvenanceResult {
    std::optional<std::string> errorCode;
    std::string fingerprint;
};

PostApplyProvenanceResult createPostApplyProvenance(
    ArtifactService& artifacts,
    IChangeProposalRepository& proposals,
    IChangeApprovalRepository& approvals,
    const ProposedChangeSet& appliedChangeSet,
    const WorkspaceBaselineSnapshot& current)
{
    const auto artifactRows = artifacts.listArtifacts(appliedChangeSet.runId);
    if (!artifactRows.success()) return {artifactRows.error()->code, {}};

    const Artifact* baselineArtifact = nullptr;
    for (const auto& artifact : artifactRows.value()) {
        if (artifact.name == "workspace-baseline.v1.json") {
            baselineArtifact = &artifact;
            break;
        }
    }
    if (baselineArtifact == nullptr)
        return {std::string("workspace_provenance.baseline_missing"), {}};

    const auto baselineContent = artifacts.getArtifactContent(baselineArtifact->id);
    if (!baselineContent.success()) return {baselineContent.error()->code, {}};
    const auto baseline = WorkspaceBaselineProvenance::parseJson(baselineContent.value());
    if (!baseline.success()) return {baseline.error()->code, {}};

    std::map<std::string, std::string> expected;
    for (const auto& file : baseline.value().files)
        expected.emplace(file.relativePath, file.sha256);

    const auto runProposals = proposals.listForRun(appliedChangeSet.runId);
    if (!runProposals.success()) return {runProposals.error()->code, {}};

    std::vector<std::string> appliedProposalIds;
    for (const auto& proposal : runProposals.value()) {
        if (proposal.state != ChangeSetState::Applied) continue;
        const auto changes = proposals.listChanges(proposal.changeSetId);
        if (!changes.success()) return {changes.error()->code, {}};
        const auto approval = approvals.findApproval(proposal.changeSetId);
        if (!approval.success()) return {approval.error()->code, {}};
        if (!approval.value() || approval.value()->applyResult != "applied")
            return {std::string("workspace_provenance.apply_audit_missing"), {}};

        std::map<ChangeId, ApplyHashEntry> hashes;
        for (const auto& entry : approval.value()->applyHashLog)
            hashes.emplace(entry.changeId, entry);
        for (const auto& change : changes.value()) {
            const auto hashIt = hashes.find(change.changeId);
            if (hashIt == hashes.end())
                return {std::string("workspace_provenance.apply_hash_missing"), {}};
            const std::string target = change.relativePath.generic_string();
            if (change.operation == FileChangeOperation::Delete) {
                expected.erase(target);
            } else if (change.operation == FileChangeOperation::Rename) {
                if (!change.sourcePath || !hashIt->second.newHash)
                    return {std::string("workspace_provenance.apply_hash_missing"), {}};
                expected.erase(change.sourcePath->generic_string());
                expected[target] = *hashIt->second.newHash;
            } else {
                if (!hashIt->second.newHash)
                    return {std::string("workspace_provenance.apply_hash_missing"), {}};
                expected[target] = *hashIt->second.newHash;
            }
        }
        appliedProposalIds.push_back(proposal.changeSetId);
    }

    std::map<std::string, std::string> actual;
    for (const auto& file : current.files)
        actual.emplace(file.relativePath, file.sha256);

    std::vector<std::string> added;
    std::vector<std::string> modified;
    std::vector<std::string> deleted;
    for (const auto& [path, hash] : actual) {
        const auto it = expected.find(path);
        if (it == expected.end()) added.push_back(path);
        else if (it->second != hash) modified.push_back(path);
    }
    for (const auto& [path, hash] : expected) {
        static_cast<void>(hash);
        if (!actual.contains(path)) deleted.push_back(path);
    }

    const bool matchesAppliedSet = added.empty() && modified.empty() && deleted.empty();
    std::ostringstream report;
    report << "# Workspace apply provenance\n\n"
           << "Schema: workspace-apply-provenance.v1\n\n"
           << "Baseline fingerprint: " << baseline.value().fingerprint << "\n\n"
           << "Current fingerprint: " << current.fingerprint << "\n\n"
           << "Classification: "
           << (matchesAppliedSet ? "applied_proposal_set"
                                 : "applied_with_manual_or_external_drift")
           << "\n\nApplied proposal IDs:\n";
    for (const auto& id : appliedProposalIds) report << "- " << id << '\n';
    auto appendPaths = [&](std::string_view title, const std::vector<std::string>& paths) {
        report << "\n" << title << ":\n";
        if (paths.empty()) report << "- none\n";
        else for (const auto& path : paths) report << "- " << path << '\n';
    };
    appendPaths("Unexpected added paths", added);
    appendPaths("Unexpected modified paths", modified);
    appendPaths("Unexpected deleted paths", deleted);

    const auto persisted = artifacts.createArtifact(
        appliedChangeSet.runId, std::nullopt, ArtifactKind::Report,
        "manifest-workspace-apply-provenance.md", "text/markdown", report.str());
    if (!persisted.success()) return {persisted.error()->code, {}};
    return {std::nullopt, current.fingerprint};
}

} // namespace

ChangeSetApplyService::ChangeSetApplyService(
    IChangeProposalRepository& repository,
    IChangeProposalStore& store,
    IRunRepository& runs,
    IProjectRepository& projects,
    IChangeApprovalRepository& approvals,
    ArtifactService* artifacts,
    ManifestWorkLedgerService* manifestWorkLedgerService,
    ManifestApplyVerificationGate* verificationGate)
    : repository_(repository)
    , store_(store)
    , runs_(runs)
    , projects_(projects)
    , approvals_(approvals)
    , artifacts_(artifacts)
    , manifestWorkLedgerService_(manifestWorkLedgerService)
    , verificationGate_(verificationGate)
{
}

common::Result<ApprovalRecord> ChangeSetApplyService::approve(
    const ChangeSetId& changeSetId, std::optional<std::string> approvedBy)
{
    const auto found = repository_.findChangeSet(changeSetId);
    if (!found.success())
        return common::Result<ApprovalRecord>::failure(*found.error());
    if (!found.value().has_value())
        return failWith<ApprovalRecord>("proposal.not_found",
            "Change set not found: " + changeSetId);
    if (found.value()->state != ChangeSetState::ReadyForReview)
        return failWith<ApprovalRecord>("proposal.not_ready",
            "Change set is not in ReadyForReview state");

    // AG-16.1: fail-closed here for early, honest feedback if this proposal
    // is already known-ineligible for apply due to the verification gate -
    // apply() itself re-runs the exact same check regardless, immediately
    // before mutating the workspace, so this is never the only enforcement
    // point (see ChangeSetApplyService::apply()'s own gate call below).
    if (verificationGate_ != nullptr) {
        const auto eligibility = verificationGate_->evaluate(changeSetId);
        if (!eligibility.allowed) {
            return failWith<ApprovalRecord>(eligibility.reasonCode, eligibility.reasonMessage);
        }
    }

    const auto changesResult = repository_.listChanges(changeSetId);
    if (!changesResult.success())
        return common::Result<ApprovalRecord>::failure(*changesResult.error());
    const std::string snapshotHash = computeSnapshotHash(changesResult.value());

    // Idempotent: a double-click or a re-opened confirmation dialog must not
    // error just because this exact snapshot was already approved.
    const auto existing = approvals_.findApproval(changeSetId);
    if (!existing.success())
        return common::Result<ApprovalRecord>::failure(*existing.error());
    if (existing.value().has_value()) {
        if (existing.value()->snapshotHash != snapshotHash) {
            // Unreachable in practice - change sets are immutable once
            // stored - but fail closed rather than silently reuse a stale
            // approval for different content.
            return failWith<ApprovalRecord>("proposal.approval_conflict",
                "An existing approval record does not match this change set's current content");
        }
        return common::Result<ApprovalRecord>::ok(*existing.value());
    }

    ApprovalRecord record;
    record.changeSetId = changeSetId;
    record.snapshotHash = snapshotHash;
    record.approvedAt = std::chrono::system_clock::now();
    record.approvedBy = std::move(approvedBy);

    const auto recorded = approvals_.recordApproval(record);
    if (!recorded.success())
        return common::Result<ApprovalRecord>::failure(*recorded.error());
    // AG-14: only the first, genuine approval - never the idempotent replay
    // at the existing.value() branch above - becomes a Work Ledger
    // transition. Best-effort: failure here must never undo the just-recorded
    // approval.
    if (manifestWorkLedgerService_ != nullptr) {
        static_cast<void>(manifestWorkLedgerService_->recordReviewTransition(
            changeSetId, ManifestWorkLedgerTransitionKind::Approved,
            record.approvedBy, std::nullopt, std::nullopt));
    }
    // PC-4C: only the first, genuine approval nudges the Outcome Brief (best effort).
    if (outcomeBriefTrigger_ != nullptr) {
        outcomeBriefTrigger_->proposalResolved(changeSetId);
    }
    return common::Result<ApprovalRecord>::ok(std::move(record));
}

common::Result<ApplyChangeSetResult> ChangeSetApplyService::apply(
    const ChangeSetId& changeSetId)
{
    auto result = applyUnnotified(changeSetId);
    // PC-4C: whatever happened - applied, ApplyFailed or refused - the outcome of the Run may have changed; the nudge is best
    // effort and idempotent (an unchanged outcome is just "unchanged").
    if (outcomeBriefTrigger_ != nullptr) {
        outcomeBriefTrigger_->proposalResolved(changeSetId);
    }
    return result;
}

common::Result<ApplyChangeSetResult> ChangeSetApplyService::applyUnnotified(
    const ChangeSetId& changeSetId)
{
    // 1. Load and gate on ReadyForReview state.
    const auto found = repository_.findChangeSet(changeSetId);
    if (!found.success())
        return common::Result<ApplyChangeSetResult>::failure(*found.error());
    if (!found.value().has_value())
        return failWith<ApplyChangeSetResult>("proposal.not_found",
            "Change set not found: " + changeSetId);

    const auto& cs = *found.value();
    if (cs.state != ChangeSetState::ReadyForReview)
        return failWith<ApplyChangeSetResult>("proposal.not_ready",
            "Change set is not in ReadyForReview state");

    // 2. Load changes early - needed both for the approval-hash comparison
    // right below and the pre-flight/apply loops further down.
    const auto changesResult = repository_.listChanges(changeSetId);
    if (!changesResult.success())
        return common::Result<ApplyChangeSetResult>::failure(*changesResult.error());
    const auto& changes = changesResult.value();
    std::vector<ApplyHashEntry> hashLog;

    // Every failure from here on is against an already-identified,
    // already-approved-or-rejected change set, so it's worth auditing -
    // wrap it once instead of repeating the recordApplyOutcome call at
    // every return site below.
    auto applyFailure = [&](std::string code, std::string message) {
        static_cast<void>(approvals_.recordApplyOutcome(
            changeSetId, "apply_failed", std::optional<std::string>(code), hashLog,
            std::nullopt));
        return failWith<ApplyChangeSetResult>(std::move(code), std::move(message));
    };
    auto applyFailureFromError = [&](const common::Error& err) {
        static_cast<void>(approvals_.recordApplyOutcome(
            changeSetId, "apply_failed", std::optional<std::string>(err.code), hashLog,
            std::nullopt));
        return common::Result<ApplyChangeSetResult>::failure(err);
    };

    // 3. A human must have already approved this exact, immutable snapshot
    // through the UI - see approve(). No file is touched without one.
    const auto approval = approvals_.findApproval(changeSetId);
    if (!approval.success())
        return common::Result<ApplyChangeSetResult>::failure(*approval.error());
    if (!approval.value().has_value())
        return applyFailure("proposal.no_approval",
            "This change set has not been approved for apply");
    if (approval.value()->snapshotHash != computeSnapshotHash(changes))
        return applyFailure("proposal.approval_mismatch",
            "The approved snapshot no longer matches this change set");

    // 4. Resolve the trusted workspace exclusively from persisted Backend
    // state. The identity check prevents a malformed/corrupted change-set
    // row from borrowing another Run's workspace.
    const auto runResult = runs_.findRun(cs.runId);
    if (!runResult.success())
        return applyFailureFromError(*runResult.error());
    if (!runResult.value().has_value())
        return applyFailure("proposal.run_not_found",
            "The Run that produced this change set no longer exists");
    const Run& run = *runResult.value();
    if (run.projectId != cs.projectId)
        return applyFailure("proposal.project_mismatch",
            "Change set and Run belong to different projects");

    fs::path projectRoot;
    if (run.workspacePath.has_value()) {
        projectRoot = *run.workspacePath;
    } else {
        // Compatibility for in-memory/legacy rows that predate schema v24.
        const auto projectResult = projects_.findById(run.projectId);
        if (!projectResult.success())
            return applyFailureFromError(*projectResult.error());
        if (!projectResult.value().has_value())
            return applyFailure("proposal.project_not_found",
                "The Project that owns this change set no longer exists");
        projectRoot = projectResult.value()->rootPath;
    }

    std::error_code rootError;
    const fs::path canonRoot = fs::weakly_canonical(projectRoot, rootError);
    if (rootError || canonRoot.empty() || !fs::is_directory(canonRoot, rootError) || rootError)
        return applyFailure("proposal.workspace_unavailable",
            "The Run workspace is unavailable");

    // 5. Pre-flight: path containment, policy, hash checks — no writes yet.
    for (const auto& change : changes) {
        const fs::path absPath = (canonRoot / change.relativePath).lexically_normal();

        if (!isInsideRoot(absPath, canonRoot))
            return applyFailure("proposal.path_traversal",
                "Resolved path escapes project root");

        std::error_code ec;
        if (change.operation == FileChangeOperation::Modify) {
            if (!fs::exists(absPath, ec))
                return applyFailure("proposal.target_missing",
                    "Modify target does not exist on disk");
            if (change.expectedOriginalHash) {
                const auto current = readFileContent(absPath);
                if (!current)
                    return applyFailure("proposal.target_unreadable",
                        "Cannot read current content of modify target");
                if (sha256Hex(*current) != *change.expectedOriginalHash)
                    return applyFailure("proposal.hash_mismatch",
                        "File has been modified since the proposal was created");
            }
        }

        if (change.operation == FileChangeOperation::Create) {
            if (fs::exists(absPath, ec))
                return applyFailure("proposal.create_conflict",
                    "Create target already exists on disk");
        }

        if (change.operation == FileChangeOperation::Delete) {
            if (!fs::is_regular_file(absPath, ec))
                return applyFailure("proposal.target_missing",
                    "Delete target does not exist on disk");
            if (change.expectedOriginalHash) {
                const auto current = readFileContent(absPath);
                if (!current)
                    return applyFailure("proposal.target_unreadable",
                        "Cannot read current content of delete target");
                if (sha256Hex(*current) != *change.expectedOriginalHash)
                    return applyFailure("proposal.hash_mismatch",
                        "File has been modified since the proposal was created");
            }
        }

        if (change.operation == FileChangeOperation::Rename) {
            if (!change.sourcePath)
                return applyFailure("proposal.rename_source_missing",
                    "Rename change is missing its source path");
            const fs::path absSourcePath =
                (canonRoot / *change.sourcePath).lexically_normal();
            if (!isInsideRoot(absSourcePath, canonRoot))
                return applyFailure("proposal.path_traversal",
                    "Resolved source path escapes project root");
            if (!fs::is_regular_file(absSourcePath, ec))
                return applyFailure("proposal.rename_source_missing",
                    "Rename source does not exist on disk");
            if (fs::exists(absPath, ec)) {
                // See renameFileOnDisk(): on a case-insensitive filesystem
                // the target of a case-only rename resolves to the same
                // file as the source - not a real conflict.
                std::error_code eqEc;
                const bool targetIsSource =
                    fs::equivalent(absSourcePath, absPath, eqEc) && !eqEc;
                if (!targetIsSource)
                    return applyFailure("proposal.rename_target_conflict",
                        "Rename target already exists on disk");
            }
            if (change.expectedOriginalHash) {
                const auto current = readFileContent(absSourcePath);
                if (!current)
                    return applyFailure("proposal.target_unreadable",
                        "Cannot read current content of rename source");
                if (sha256Hex(*current) != *change.expectedOriginalHash)
                    return applyFailure("proposal.hash_mismatch",
                        "File has been modified since the proposal was created");
            }
        }
    }

    // 6. AG-16.1: re-run the exact same authoritative verification gate
    // approve() already checked - never trust that earlier decision.
    // Verification state, the durable proposal set, or a sibling proposal's
    // content could all have changed between approve() and this apply()
    // call (a different proposal in the same combined set superseded,
    // rejected, or otherwise diverged). This is the gate's final word,
    // immediately before any workspace write below - closing exactly the
    // TOCTOU window AG-16 exists to close, and running after every existing
    // stale-workspace/hash/path-safety preflight check above rather than
    // instead of them.
    if (verificationGate_ != nullptr) {
        const auto eligibility = verificationGate_->evaluate(changeSetId);
        if (!eligibility.allowed) {
            return applyFailure(eligibility.reasonCode, eligibility.reasonMessage);
        }
    }

    // 7. Apply with rollback tracking.
    std::vector<AppliedEntry> applied;
    applied.reserve(changes.size());

    for (const auto& change : changes) {
        const fs::path absPath = (canonRoot / change.relativePath).lexically_normal();

        if (change.operation == FileChangeOperation::Create) {
            const auto proposed = store_.readProposedContent(
                cs.artifactRelativeRoot, change.changeId);
            if (!proposed.success()) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailureFromError(*proposed.error());
            }
            if (!writeFileContent(absPath, proposed.value())) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailure("proposal.write_failed",
                    "Failed to write new file to workspace");
            }
            applied.push_back({change.operation, absPath, std::nullopt, std::nullopt});
            hashLog.push_back({change.changeId, change.relativePath,
                std::nullopt, sha256Hex(proposed.value())});
        }
        else if (change.operation == FileChangeOperation::Modify) {
            auto savedContent = readFileContent(absPath);
            const auto proposed = store_.readProposedContent(
                cs.artifactRelativeRoot, change.changeId);
            if (!proposed.success()) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailureFromError(*proposed.error());
            }
            if (!writeFileContent(absPath, proposed.value())) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailure("proposal.write_failed",
                    "Failed to overwrite file in workspace");
            }
            const std::optional<std::string> originalHash = savedContent.has_value()
                ? std::optional<std::string>(sha256Hex(*savedContent)) : std::nullopt;
            hashLog.push_back({change.changeId, change.relativePath,
                originalHash, sha256Hex(proposed.value())});
            applied.push_back({change.operation, absPath, std::move(savedContent), std::nullopt});
        }
        else if (change.operation == FileChangeOperation::Delete) {
            auto savedContent = readFileContent(absPath);
            if (!savedContent) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailure("proposal.target_unreadable",
                    "Cannot read current content of delete target");
            }
            const std::string originalHash = sha256Hex(*savedContent);
            std::error_code ec;
            fs::remove(absPath, ec);
            if (ec) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailure("proposal.delete_failed",
                    "Failed to delete file from workspace");
            }
            hashLog.push_back({change.changeId, change.relativePath,
                originalHash, std::nullopt});
            applied.push_back({change.operation, absPath, std::move(savedContent), std::nullopt});
        }
        else if (change.operation == FileChangeOperation::Rename) {
            const fs::path absSourcePath =
                (canonRoot / *change.sourcePath).lexically_normal();
            const auto sourceContent = readFileContent(absSourcePath);
            if (!sourceContent) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailure("proposal.target_unreadable",
                    "Cannot read current content of rename source");
            }
            const std::string contentHash = sha256Hex(*sourceContent);
            std::error_code ec;
            renameFileOnDisk(absSourcePath, absPath, ec);
            if (ec) {
                const bool rbOk = rollbackApplied(applied);
                if (!rbOk)
                    static_cast<void>(repository_.updateChangeSetState(
                        changeSetId, ChangeSetState::ApplyFailed));
                return applyFailure("proposal.rename_failed",
                    "Failed to rename file in workspace");
            }
            hashLog.push_back({change.changeId, change.relativePath,
                contentHash, contentHash});
            applied.push_back({change.operation, absPath, std::nullopt, absSourcePath});
        }
    }

    // 8. All operations succeeded; transition to Applied and record the
    // apply audit outcome.
    const auto updateResult = repository_.updateChangeSetState(
        changeSetId, ChangeSetState::Applied);
    if (!updateResult.success())
        return applyFailureFromError(*updateResult.error());

    std::optional<std::string> provenanceErrorCode;
    std::optional<std::string> postApplyFingerprint;
    const auto postApplySnapshot = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(canonRoot));
    if (postApplySnapshot.success()) {
        postApplyFingerprint = postApplySnapshot.value().fingerprint;
    } else {
        provenanceErrorCode = postApplySnapshot.error()->code;
    }

    const auto auditRecorded = approvals_.recordApplyOutcome(
        changeSetId, "applied", std::nullopt, hashLog, postApplyFingerprint);
    if (!auditRecorded.success()) {
        provenanceErrorCode = auditRecorded.error()->code;
    } else if (artifacts_ != nullptr && postApplySnapshot.success()) {
        auto provenance = createPostApplyProvenance(
            *artifacts_, repository_, approvals_, cs, postApplySnapshot.value());
        provenanceErrorCode = std::move(provenance.errorCode);
        if (!provenanceErrorCode.has_value()) {
            postApplyFingerprint = std::move(provenance.fingerprint);
        }
    }
    // AG-14.1/AG-14.2/AG-14.3: postApplyFingerprint is the real
    // WorkspaceBaselineSnapshot::fingerprint of the post-apply workspace -
    // one snapshot shared by the approval audit, provenance report, and live
    // ledger transition, never a homegrown hash of the apply's own hash log.
    // The transition itself is still best-effort and must never undo writes
    // that already succeeded; startup reconciliation can now recover it from
    // the persisted ApprovalRecord, including this fingerprint.
    if (manifestWorkLedgerService_ != nullptr) {
        static_cast<void>(manifestWorkLedgerService_->recordReviewTransition(
            changeSetId, ManifestWorkLedgerTransitionKind::Applied,
            std::nullopt, std::nullopt, postApplyFingerprint));
    }
    return common::Result<ApplyChangeSetResult>::ok(
        ApplyChangeSetResult{applied.size(), std::move(provenanceErrorCode)});
}

} // namespace ai::application
