// Unit tests for ManifestWorkLedgerService (AG-14): the pure assembly/gating
// logic, entirely in-memory - no SQLite, no provider sessions. Repository
// round-trip coverage lives in ManifestWorkLedgerRepositoryTests.cpp;
// production wiring is exercised end to end through ManifestWorkflowRunner,
// ChangeSetApplyService, ProposalReviewService, RunManager, and
// OrchestrationService's own test suites.
#include "ai/application/ManifestWorkLedgerService.hpp"
#include "RecordingOutcomeBriefTrigger.hpp"
#include "ai/application/IWorkflowResolver.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/application/RunManager.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/ExecutionManifestPlanner.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryManifestTaskExecutionRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryRunWorkspaceScopeRepository.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/verification/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace ai::application;
namespace common = ai::common;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

// Minimal in-memory IChangeProposalRepository fake that behaves like the SQLite adapter: findChangeSet and listForRun
// return a HEADER (no file changes, which is why the Work Ledger once recorded no paths in production while this fake
// still returned them), and the items come only from listChanges / listChangesForSets (without the runtime-only
// content, which SQLite does not store either). The batched read is counted and can be made to fail.
class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
    mutable int batchReads {0};
    mutable std::vector<ChangeSetId> lastBatchIds;
    bool failBatchRead {false};
    bool reverseListOrder {false};

    static ProposedChangeSet headerOf(ProposedChangeSet changeSet)
    {
        changeSet.changes.clear();
        return changeSet;
    }
    static std::vector<ProposedFileChange> itemsOf(const ProposedChangeSet& changeSet)
    {
        std::vector<ProposedFileChange> items = changeSet.changes;
        for (auto& item : items) {
            item.proposedContent.reset();
            item.capturedOriginalContent.reset();
        }
        return items;
    }

    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        std::lock_guard lock(mu_);
        byId_[cs.changeSetId] = cs;
        return common::Result<void>::ok();
    }
    common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<ProposedChangeSet>::failure({"proposal.not_found", "not found"});
        it->second.state = newState;
        return common::Result<ProposedChangeSet>::ok(it->second);
    }
    common::Result<void> markMissingEvidence(const ChangeSetId&, std::string, std::string) override
    {
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(headerOf(it->second));
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> result;
        for (const auto& [id, cs] : byId_) {
            if (cs.runId == runId) result.push_back(headerOf(cs));
        }
        if (reverseListOrder) std::reverse(result.begin(), result.end());
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(itemsOf(it->second));
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId&) const override
    {
        return common::Result<std::vector<ValidationFinding>>::ok({});
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& ids) const override
    {
        using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>;
        std::lock_guard lock(mu_);
        ++batchReads;
        lastBatchIds = ids;
        if (failBatchRead) return ResultType::failure({"proposal.batch_read_failed", "Simulated batched read failure"});
        std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> byChangeSet;
        for (const auto& id : ids) {
            const auto it = byId_.find(id);
            if (it != byId_.end() && !it->second.changes.empty()) byChangeSet[id] = itemsOf(it->second);
        }
        return ResultType::ok(std::move(byChangeSet));
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>&) const override
    {
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>::ok({});
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
};

// AG-14.2: minimal in-memory IChangeApprovalRepository/IProposalReviewRepository
// fakes, only for reconcileMissingTransitions() coverage - findApproval/
// findRejection/recordApproval/recordRejection are all that is exercised.
class FakeChangeApprovalRepository final : public IChangeApprovalRepository {
public:
    common::Result<void> recordApproval(const ApprovalRecord& record) override
    {
        byId_[record.changeSetId] = record;
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ApprovalRecord>> findApproval(
        const ChangeSetId& changeSetId) const override
    {
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end()) return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
        return common::Result<std::optional<ApprovalRecord>>::ok(it->second);
    }
    common::Result<void> recordApplyOutcome(
        const ChangeSetId& changeSetId, const std::string& result,
        const std::optional<std::string>& errorCode,
        const std::vector<ApplyHashEntry>& hashLog,
        const std::optional<std::string>& postApplyWorkspaceFingerprint) override
    {
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end()) return common::Result<void>::failure({"proposal.not_found", "not found"});
        it->second.appliedAt = std::chrono::system_clock::now();
        it->second.applyResult = result;
        it->second.applyErrorCode = errorCode;
        it->second.applyHashLog = hashLog;
        it->second.postApplyWorkspaceFingerprint = postApplyWorkspaceFingerprint;
        return common::Result<void>::ok();
    }

private:
    std::map<ChangeSetId, ApprovalRecord> byId_;
};

class FakeProposalReviewRepository final : public IProposalReviewRepository {
public:
    common::Result<void> recordRejection(const ProposalReviewRejection& record) override
    {
        byId_[record.changeSetId] = record;
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ProposalReviewRejection>> findRejection(
        const ChangeSetId& changeSetId) const override
    {
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end())
            return common::Result<std::optional<ProposalReviewRejection>>::ok(std::nullopt);
        return common::Result<std::optional<ProposalReviewRejection>>::ok(it->second);
    }

private:
    std::map<ChangeSetId, ProposalReviewRejection> byId_;
};

class FailingManifestWorkLedgerRepository final : public IManifestWorkLedgerRepository {
public:
    common::Result<void> insertEntry(ManifestWorkLedgerEntry) override
    {
        return common::Result<void>::failure({"ledger.write_failed", "simulated"});
    }
    common::Result<std::optional<ManifestWorkLedgerEntry>> findEntry(
        const RunId&) const override
    {
        return common::Result<std::optional<ManifestWorkLedgerEntry>>::failure(
            {"ledger.read_failed", "simulated"});
    }
    common::Result<std::vector<ManifestWorkLedgerEntry>> listRecentForProject(
        const ProjectId&, std::size_t) const override
    {
        return common::Result<std::vector<ManifestWorkLedgerEntry>>::failure(
            {"ledger.read_failed", "simulated"});
    }
    common::Result<void> appendTransition(ManifestWorkLedgerReviewTransition) override
    {
        return common::Result<void>::failure({"ledger.write_failed", "simulated"});
    }
    common::Result<std::vector<ManifestWorkLedgerReviewTransition>>
    listTransitionsForRun(const RunId&) const override
    {
        return common::Result<std::vector<ManifestWorkLedgerReviewTransition>>::failure(
            {"ledger.read_failed", "simulated"});
    }
};

struct Fixture {
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    FakeChangeProposalRepository proposals;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts {artifactRepository, artifactStore};
    FakeChangeApprovalRepository approvals;
    FakeProposalReviewRepository reviews;

    ManifestWorkLedgerService makeService()
    {
        return ManifestWorkLedgerService(
            ledger, runs, manifests, taskExecutions, proposals,
            nullptr, nullptr, nullptr, nullptr);
    }

    // AG-14.2: wires a real ArtifactService (backed by in-memory storage,
    // same pair ArtifactServiceTests.cpp itself uses) so tests can seed a
    // genuine typed identity artifact and prove recordTerminalRun's
    // fail-closed reads/invariant checks against real round-tripped data,
    // not just the artifacts_ == nullptr path.
    ManifestWorkLedgerService makeServiceWithArtifacts()
    {
        return ManifestWorkLedgerService(ledger, runs, manifests, taskExecutions, proposals, &artifacts);
    }

    ManifestWorkLedgerService makeServiceWithArtifactsAndWorkspaceScope()
    {
        return ManifestWorkLedgerService(
            ledger, runs, manifests, taskExecutions, proposals,
            &artifacts, nullptr, nullptr, &workspaceScopes);
    }

    // AG-14.2: wires the approval/review fakes so reconcileMissingTransitions
    // has something to recover from.
    ManifestWorkLedgerService makeServiceWithApprovalsAndReviews()
    {
        return ManifestWorkLedgerService(
            ledger, runs, manifests, taskExecutions, proposals, nullptr, &approvals, &reviews);
    }
};

// AG-14.1: a IExecutionManifestRepository fake whose findForRun can be told
// to fail on demand, proving recordTerminalRun propagates that failure as
// its own rather than silently treating it as "no Manifest exists" (the
// exact fabrication bug the AG-14.1 review flagged).
class FailOnDemandExecutionManifestRepository final : public IExecutionManifestRepository {
public:
    bool shouldFail {false};

    common::Result<ExecutionManifest> insert(ExecutionManifest manifest) override
    {
        stored_[manifest.runId] = manifest;
        return common::Result<ExecutionManifest>::ok(std::move(manifest));
    }
    common::Result<std::optional<ExecutionManifest>> findForRun(const RunId& runId) const override
    {
        if (shouldFail) {
            return common::Result<std::optional<ExecutionManifest>>::failure(
                {"manifest.transient_read_failure", "Simulated transient read failure"});
        }
        const auto it = stored_.find(runId);
        if (it == stored_.end())
            return common::Result<std::optional<ExecutionManifest>>::ok(std::nullopt);
        return common::Result<std::optional<ExecutionManifest>>::ok(it->second);
    }

private:
    std::map<RunId, ExecutionManifest> stored_;
};

Run makeRun(const RunId& id, const ProjectId& projectId, const std::string& workflowId, RunStatus status)
{
    Run run;
    run.id = id;
    run.projectId = projectId;
    run.workflowId = workflowId;
    run.status = status;
    run.title = "Test run";
    run.task = "Test task";
    run.createdAt = std::chrono::system_clock::now();
    return run;
}

ProposedChangeSet makeProposal(
    const ChangeSetId& id, const RunId& runId, const std::string& relativePath)
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = id;
    cs.runId = runId;
    cs.projectId = "proj-1";
    cs.summary = "test";
    ProposedFileChange change;
    change.changeId = "chg-1";
    change.operation = FileChangeOperation::Create;
    change.relativePath = relativePath;
    change.proposedContent = "content";
    cs.changes = {change};
    return cs;
}

// A change set with several file changes (the items a header never carries), in the given order.
ProposedChangeSet makeProposalWithPaths(
    const ChangeSetId& id, const RunId& runId, const ProjectId& projectId,
    const std::vector<std::string>& paths, ChangeSetState state = ChangeSetState::ReadyForReview)
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = id;
    cs.runId = runId;
    cs.projectId = projectId;
    cs.summary = "test";
    cs.state = state;
    cs.createdAt = std::chrono::system_clock::now();
    int ordinal = 0;
    for (const auto& path : paths) {
        ProposedFileChange change;
        change.changeId = "chg-" + std::to_string(++ordinal);
        change.operation = FileChangeOperation::Modify;
        change.relativePath = path;
        change.proposedContent = "content";
        cs.changes.push_back(std::move(change));
    }
    return cs;
}

// AG-14.2: proves InMemoryManifestWorkLedgerRepository now matches
// SqliteManifestWorkLedgerRepository's actual immutable/idempotent
// contract (the interface doc comment previously claimed "replaces the
// earlier entry," which was already false for the Sqlite implementation
// since AG-14.1 - this test pins the corrected, shared contract for both).
void testInMemoryLedgerRejectsDuplicateEntryAndTransition()
{
    Fixture fx;
    ManifestWorkLedgerEntry entry;
    entry.runId = "run-1";
    entry.projectId = "proj-1";
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::NotProduced;
    entry.createdAt = std::chrono::system_clock::now();
    req(fx.ledger.insertEntry(entry).success(), "First insertEntry failed");

    auto duplicate = entry;
    duplicate.terminalReason = "should never be visible";
    const auto rejected = fx.ledger.insertEntry(duplicate);
    req(!rejected.success(), "A duplicate runId insert must fail, not replace");
    req(rejected.error()->code == "manifest_work_ledger.duplicate_entry",
        "Expected the duplicate_entry failure code");
    const auto found = fx.ledger.findEntry("run-1");
    req(found.success() && found.value().has_value() && found.value()->terminalReason.empty(),
        "The original entry must remain untouched by a rejected duplicate insert");

    ManifestWorkLedgerReviewTransition transition;
    transition.id = "cs-1:approved";
    transition.runId = "run-1";
    transition.changeSetId = "cs-1";
    transition.kind = ManifestWorkLedgerTransitionKind::Approved;
    transition.occurredAt = std::chrono::system_clock::now();
    req(fx.ledger.appendTransition(transition).success(), "First appendTransition failed");

    auto duplicateTransition = transition;
    duplicateTransition.reason = "should never be visible";
    const auto rejectedTransition = fx.ledger.appendTransition(duplicateTransition);
    req(!rejectedTransition.success(), "A duplicate transition id must fail, not duplicate");
    req(rejectedTransition.error()->code == "manifest_work_ledger.duplicate_transition",
        "Expected the duplicate_transition failure code");
    const auto transitions = fx.ledger.listTransitionsForRun("run-1");
    req(transitions.success() && transitions.value().size() == 1,
        "A rejected duplicate transition must never be appended");
}

void testRecordTerminalRunSkipsNonManifestWorkflow()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionProfileWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    auto service = fx.makeService();

    const auto result = service.recordTerminalRun("run-1");
    req(result.success(), "recordTerminalRun should succeed as a no-op");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && !entry.value().has_value(),
        "A non-Manifest-workflow Run must never get a ledger entry");
}

void testRecordTerminalRunSkipsNonTerminalStatus()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Running);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    auto service = fx.makeService();

    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun should succeed as a no-op");
    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && !entry.value().has_value(),
        "A still-Running Manifest Run must never get a ledger entry");
}

void testRecordTerminalRunAssemblesManifestReadyEntry()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    ExecutionManifestTask task;
    task.id = "task-a";
    task.objective = "Create a.cpp";
    manifest.tasks = {task};
    req(fx.manifests.insert(manifest).success(), "insert manifest failed");

    ManifestTaskState state;
    state.status = ManifestTaskStatus::Completed;
    state.changeSetId = "cs-1";
    state.inspectedRelativePaths = {"src/a.cpp"};
    req(fx.taskExecutions.upsert("run-1", "task-a", state).success(), "upsert task state failed");

    req(fx.proposals.insertChangeSet(makeProposal("cs-1", "run-1", "src/a.cpp")).success(),
        "insert proposal failed");

    RunWorkspaceScope scope;
    scope.runId = "run-1";
    scope.manifestSchemaVersion = manifest.schemaVersion;
    scope.entries.push_back({"src/a.cpp", WorkspaceScopeRole::PlannedCreate,
        "Declared as planned_create by task 'task-a'", "task-a", std::nullopt,
        WorkspaceScopeOrigin::FrozenInitial});
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "freeze workspace scope failed");

    // AG-14.2: recordTerminalRun's own defensive invariant now requires a
    // real manifest-verification-identity.v1.json artifact for a Completed
    // ManifestReady Run (see ManifestWorkflowRunner's own write-side gate) -
    // seed one exactly as that code would, and assert it round-trips
    // faithfully rather than being fabricated as "passed" or silently
    // dropped.
    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = "workspace-1";
    verification.baselineFingerprint = "baseline-fingerprint-1";
    verification.overlayFingerprint = "overlay-fingerprint-1";
    verification.orderedProposalIds = {"cs-1"};
    verification.orderedProposalContentHashes = {"hash-1"};
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;
    req(fx.artifacts.createArtifact(
            "run-1", std::nullopt, ArtifactKind::Report,
            "manifest-verification-identity.v1.json", "application/json",
            renderManifestProposalSetVerificationJson(verification))
            .success(),
        "Could not seed verification identity artifact");

    auto service = fx.makeServiceWithArtifactsAndWorkspaceScope();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && entry.value().has_value(), "Expected a ledger entry for the terminal Run");
    const auto& value = *entry.value();
    req(value.projectId == "proj-1", "projectId mismatch");
    req(value.terminalStatus == RunStatus::Completed, "terminalStatus mismatch");
    req(value.disposition == ManifestWorkLedgerDisposition::ManifestReady,
        "A Run with a frozen Manifest must be ManifestReady");
    req(value.taskDispositions.size() == 1 && value.taskDispositions.front().taskId == "task-a"
            && value.taskDispositions.front().changeSetId.has_value()
            && *value.taskDispositions.front().changeSetId == "cs-1",
        "task disposition not assembled correctly");
    req(value.proposalIds == std::vector<ChangeSetId> {"cs-1"}, "proposalIds not assembled correctly");
    req(value.affectedRelativePaths == std::vector<std::string> {"src/a.cpp"},
        "affectedRelativePaths not assembled correctly");
    req(value.verification.has_value() && value.verification->workspaceIdentity == "workspace-1"
            && value.verification->acceptanceStatus == ExecutionVerificationStatus::Passed,
        "The seeded verification identity did not round-trip into the ledger entry");
    req(value.verificationSummary.find("safety: passed") != std::string::npos,
        "verificationSummary must reflect the real verification identity, never a guess");
    req(value.workspaceSnapshot.has_value()
            && value.workspaceSnapshot->entries.size() == 1
            && value.workspaceSnapshot->taskActivities.size() == 1
            && value.workspaceSnapshot->taskActivities.front().inspectedRelativePaths
                == std::vector<std::string> {"src/a.cpp"}
            && value.workspaceSnapshot->taskActivities.front().proposedCreatedPaths
                == std::vector<std::string> {"src/a.cpp"},
        "Terminal ledger entry lost frozen scope or actual task activity");
}

void testRecordTerminalRunFailsClosedWhenManifestReadyCompletedButVerificationIdentityMissing()
{
    // AG-14.2: artifacts_ is wired (unlike testRecordTerminalRunFailsClosed
    // WhenManifestReadFails above) but no manifest-verification-identity.v1
    // .json artifact was ever seeded - this Run should be unreachable in
    // production (ManifestWorkflowRunner now gates Completed on persisting
    // it first), but recordTerminalRun must still refuse to silently write
    // an incomplete "Completed, unverified" row if it somehow is reached.
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    req(fx.manifests.insert(manifest).success(), "insert manifest failed");

    auto service = fx.makeServiceWithArtifacts();
    const auto result = service.recordTerminalRun("run-1");
    req(!result.success(), "A missing required verification identity must fail recordTerminalRun");
    req(result.error()->code == "manifest_work_ledger.missing_required_identity",
        "Expected the missing_required_identity failure code");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && !entry.value().has_value(),
        "No incomplete ledger entry must be written when the invariant is violated");
}

void testRecordTerminalRunNoChangesNeededWhenManifestAbsentButCompleted()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");

    PlannerNoChangesNeeded noChanges;
    noChanges.summary = "Task already satisfied";
    noChanges.evidence.push_back(
        {"Required file already exists", "src/a.cpp", "Inspected implementation is complete"});
    noChanges.recommendedNextStep = "Offer Stage 2 to the user";
    req(fx.artifacts.createArtifact(
            "run-1", std::nullopt, ArtifactKind::Report,
            "manifest-planning-no-changes-identity.v1.json", "application/json",
            renderPlannerNoChangesNeededJson(noChanges))
            .success(),
        "Could not seed no-changes identity artifact");

    auto service = fx.makeServiceWithArtifacts();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && entry.value().has_value(), "Expected a ledger entry");
    req(entry.value()->disposition == ManifestWorkLedgerDisposition::NoChangesNeeded,
        "A Completed Run with no frozen Manifest must be NoChangesNeeded");
    req(entry.value()->remainingWorkNote == "Offer Stage 2 to the user",
        "The seeded recommendedNextStep did not round-trip into remainingWorkNote");
}

void testRecordTerminalRunFailsClosedWhenNoChangesNeededButPlannerIdentityMissing()
{
    // AG-14.2: same invariant as the ManifestReady case above, for the
    // Planner no-op path.
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");

    auto service = fx.makeServiceWithArtifacts();
    const auto result = service.recordTerminalRun("run-1");
    req(!result.success(), "A missing required no-changes identity must fail recordTerminalRun");
    req(result.error()->code == "manifest_work_ledger.missing_required_identity",
        "Expected the missing_required_identity failure code");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && !entry.value().has_value(),
        "No incomplete ledger entry must be written when the invariant is violated");
}

// A Manifest workflow that fails at once: the Run ends Failed with no frozen Manifest, which the ledger records as NotProduced.
class FailingManifestWorkflow final : public IWorkflowRunner {
public:
    [[nodiscard]] std::vector<RunStage> createStages(const RunId& runId, const ResolvedExecutionPlan& /*plan*/) const override
    {
        RunStage stage;
        stage.id = runId + ":planning";
        stage.runId = runId;
        stage.type = "planning";
        stage.title = "Planning";
        return {std::move(stage)};
    }
    WorkflowResult run(const WorkflowRunContext& /*context*/, std::stop_token /*stopToken*/, IRunEventSink& /*eventSink*/) override
    {
        return {false, RunStatus::Failed, "Manifest planning failed", {}};
    }
};

// PC-4C: the Outcome Brief is derived from the ledger entry, so RunManager::finishRun nudges it only AFTER the entry has been written. A
// nudge that came first would make the reconciler see a terminal Manifest Run without an entry (source_ledger_missing) until the next
// nudge. The recorder looks at the ledger at the moment it is nudged.
void testRunManagerNudgesTheOutcomeBriefOnlyAfterTheLedgerEntry()
{
    Fixture fx;
    InMemoryProjectRepository projects;
    const auto root = std::filesystem::temp_directory_path()
        / ("ai-orch-ledger-order-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup {root};
    Project project;
    project.id = "proj-1";
    project.name = "Ledger order";
    project.rootPath = root;
    project.status = ProjectStatus::Active;
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    req(projects.insert(project).success(), "insert project failed");

    FailingManifestWorkflow workflow;
    SingleRunnerResolver resolver(workflow, kExecutionManifestWorkflowId);
    auto ledgerService = fx.makeService();
    outcomebrief_hooks::RecordingTrigger trigger;   // outlives the manager and its worker thread
    std::atomic<int> ledgerEntryVisibleAtNudge {-1};
    trigger.onRun = [&](const std::string& runId) {
        const auto entry = fx.ledger.findEntry(runId);
        ledgerEntryVisibleAtNudge = entry.success() && entry.value().has_value() ? 1 : 0;
    };
    RunManager manager(projects, fx.runs, resolver, nullptr, nullptr, nullptr, &ledgerService);
    manager.setOutcomeBriefTrigger(&trigger);
    OrchestrationService orchestration(projects, fx.runs, manager);
    const auto started = orchestration.startRun({project.id, "Ledger order", "Fail at once", kExecutionManifestWorkflowId});
    req(started.success(), "startRun failed");
    req(trigger.waitFor([&] { return trigger.runCalls(started.value()) >= 1; }), "The finished Run must nudge the Outcome Brief");
    req(trigger.runs().size() == 1, "...exactly once");
    req(ledgerEntryVisibleAtNudge.load() == 1, "...and only after its Work Ledger entry was written");
    const auto run = fx.runs.findRun(started.value());
    req(run.success() && run.value().has_value() && run.value()->status == RunStatus::Failed, "the Run ended Failed");
}

void testRecordTerminalRunNotProducedWhenManifestAbsentAndFailed()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    run.errorMessage = "Manifest planning failed";
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    auto service = fx.makeService();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && entry.value().has_value(), "Expected a ledger entry");
    req(entry.value()->disposition == ManifestWorkLedgerDisposition::NotProduced,
        "A Failed Run with no frozen Manifest must be NotProduced");
    req(entry.value()->terminalReason == "Manifest planning failed",
        "terminalReason must reflect the Run's own errorMessage");
    req(entry.value()->verificationSummary == "Manifest planning failed",
        "A non-Completed Run's verification summary must reuse terminalReason");
}

void testRecordReviewTransitionSkipsNonManifestWorkflow()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionProfileWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    req(fx.proposals.insertChangeSet(makeProposal("cs-1", "run-1", "src/a.cpp")).success(),
        "insert proposal failed");
    auto service = fx.makeService();

    const auto result = service.recordReviewTransition(
        "cs-1", ManifestWorkLedgerTransitionKind::Approved, "operator", std::nullopt, std::nullopt);
    req(result.success(), "recordReviewTransition should succeed as a no-op");

    const auto transitions = fx.ledger.listTransitionsForRun("run-1");
    req(transitions.success() && transitions.value().empty(),
        "A non-Manifest-workflow Run's proposal must never get a ledger transition");
}

void testRecordReviewTransitionAppendsForManifestWorkflow()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    req(fx.proposals.insertChangeSet(makeProposal("cs-1", "run-1", "src/a.cpp")).success(),
        "insert proposal failed");
    auto service = fx.makeService();

    req(service.recordReviewTransition(
            "cs-1", ManifestWorkLedgerTransitionKind::Applied, std::nullopt, std::nullopt, "fingerprint-1")
            .success(),
        "recordReviewTransition failed");

    const auto transitions = fx.ledger.listTransitionsForRun("run-1");
    req(transitions.success() && transitions.value().size() == 1, "Expected exactly one transition");
    const auto& transition = transitions.value().front();
    req(transition.kind == ManifestWorkLedgerTransitionKind::Applied, "transition kind mismatch");
    req(transition.postActionWorkspaceFingerprint.has_value()
            && *transition.postActionWorkspaceFingerprint == "fingerprint-1",
        "postActionWorkspaceFingerprint not persisted");
}

void testRecordReviewTransitionIsIdempotentOnRetry()
{
    // AG-14.2: a retry (or a reconciliation pass racing the original write)
    // for the exact same (changeSetId, kind) must never create a second row.
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    req(fx.proposals.insertChangeSet(makeProposal("cs-1", "run-1", "src/a.cpp")).success(),
        "insert proposal failed");
    auto service = fx.makeService();

    req(service.recordReviewTransition(
            "cs-1", ManifestWorkLedgerTransitionKind::Approved, "operator", std::nullopt, std::nullopt)
            .success(),
        "First recordReviewTransition call failed");
    req(service.recordReviewTransition(
            "cs-1", ManifestWorkLedgerTransitionKind::Approved, "operator", std::nullopt, std::nullopt)
            .success(),
        "A retried recordReviewTransition call must be a benign idempotent no-op, not fail");

    const auto transitions = fx.ledger.listTransitionsForRun("run-1");
    req(transitions.success() && transitions.value().size() == 1,
        "A retried transition must never create a duplicate row");
}

void testReconcileMissingTransitionsRecoversApprovalRejectionAndApply()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    req(fx.proposals.insertChangeSet(makeProposal("cs-approved-applied", "run-1", "src/a.cpp"))
            .success(),
        "insert approved+applied proposal failed");
    req(fx.proposals.insertChangeSet(makeProposal("cs-rejected", "run-1", "src/b.cpp")).success(),
        "insert rejected proposal failed");

    // Seed durable Approval/Rejection records directly, exactly as
    // ChangeSetApplyService/ProposalReviewService would - but never call
    // recordReviewTransition, simulating a best-effort write that was
    // discarded.
    ApprovalRecord approval;
    approval.changeSetId = "cs-approved-applied";
    approval.snapshotHash = "hash-1";
    approval.approvedAt = std::chrono::system_clock::now();
    approval.approvedBy = "operator";
    approval.appliedAt = approval.approvedAt + std::chrono::seconds(5);
    approval.applyResult = "applied";
    approval.postApplyWorkspaceFingerprint = "post-apply-fingerprint";
    req(fx.approvals.recordApproval(approval).success(), "Could not seed approval record");

    ProposalReviewRejection rejection;
    rejection.changeSetId = "cs-rejected";
    rejection.rejectedAt = std::chrono::system_clock::now();
    rejection.rejectedBy = "operator";
    rejection.reason = "duplicated work";
    req(fx.reviews.recordRejection(rejection).success(), "Could not seed rejection record");

    auto service = fx.makeServiceWithApprovalsAndReviews();
    const auto result = service.reconcileMissingTransitions();
    req(result.success(), "Transition reconciliation must succeed");
    req(result.value().recorded == 3, "Expected 3 recovered transitions (Approved, Applied, Rejected)");
    req(result.value().stillMissing.empty(), "No Run should remain missing after a clean reconciliation pass");

    const auto transitions = fx.ledger.listTransitionsForRun("run-1");
    req(transitions.success() && transitions.value().size() == 3,
        "Expected all 3 transitions to now exist");
    const auto find = [&](const ChangeSetId& changeSetId, ManifestWorkLedgerTransitionKind kind) {
        return std::find_if(transitions.value().begin(), transitions.value().end(),
            [&](const ManifestWorkLedgerReviewTransition& t) {
                return t.changeSetId == changeSetId && t.kind == kind;
            });
    };
    const auto approved = find("cs-approved-applied", ManifestWorkLedgerTransitionKind::Approved);
    req(approved != transitions.value().end() && approved->actor.has_value()
            && *approved->actor == "operator",
        "Recovered Approved transition lost its actor");
    const auto applied = find("cs-approved-applied", ManifestWorkLedgerTransitionKind::Applied);
    req(applied != transitions.value().end()
            && applied->postActionWorkspaceFingerprint == approval.postApplyWorkspaceFingerprint,
        "A reconciliation-recovered Applied transition must retain the authoritative fingerprint");
    const auto rejected = find("cs-rejected", ManifestWorkLedgerTransitionKind::Rejected);
    req(rejected != transitions.value().end() && rejected->reason.has_value()
            && *rejected->reason == "duplicated work",
        "Recovered Rejected transition lost its reason");

    // A second pass must be a clean no-op - nothing left to recover.
    const auto secondPass = service.reconcileMissingTransitions();
    req(secondPass.success() && secondPass.value().recorded == 0,
        "A second reconciliation pass must find nothing left to recover");
}

// Mirrors WorkspaceBaselineProvenance's own fingerprint formula exactly
// (sha256 of "path\0byteSize\0sha256\n" per file, in path order) - a
// WorkspaceBaselineSnapshot round-tripped through renderJson/parseJson is
// rejected with workspace_baseline.identity_mismatch unless its declared
// fingerprint actually matches its own file entries.
std::string computeBaselineFingerprint(const std::vector<WorkspaceBaselineEntry>& files)
{
    std::ostringstream material;
    for (const auto& entry : files) {
        material << entry.relativePath << '\0' << entry.byteSize << '\0' << entry.sha256 << '\n';
    }
    return sha256Hex(material.str());
}

WorkspaceBaselineSnapshot makeBaselineSnapshot(const std::string& fingerprint)
{
    WorkspaceBaselineSnapshot snapshot;
    snapshot.fingerprint = fingerprint;
    snapshot.files = {{"src/a.cpp", 100, "hash-a"}};
    snapshot.totalBytes = 100;
    return snapshot;
}

void testBuildPlannerContextIncludesCurrentBaselineEvenWithNoHistory()
{
    // AG-14.2: a project's very first Manifest Run must still see the
    // current workspace baseline - previously buildPlannerContext returned
    // an entirely empty context whenever there was no ledger history yet.
    Fixture fx;
    auto service = fx.makeService();
    const auto context = service.buildPlannerContext(
        "proj-never-run", makeBaselineSnapshot("current-fingerprint"));
    req(context.success(), "Planner context rendering must succeed");
    req(!context.value().renderedText.empty(),
        "A project with no ledger history must still render the current baseline");
    req(context.value().renderedText.find("current-fingerprint") != std::string::npos,
        "Rendered text must mention the current baseline fingerprint");
    req(context.value().includedRunIds.empty(),
        "A project with no ledger history must include no run ids");
}

void testBuildPlannerContextRespectsMaxEntriesAndReturnsIncludedIds()
{
    Fixture fx;
    for (int i = 0; i < 3; ++i) {
        const RunId runId = "run-" + std::to_string(i);
        // AG-14.2: Failed (not Completed) so disposition resolves to
        // NotProduced - this test is about buildPlannerContext's own
        // maxEntries/includedIds behavior, not about the Completed+
        // NoChangesNeeded typed-identity invariant exercised elsewhere.
        Run run = makeRun(runId, "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
        req(fx.runs.insertRun(run).success(), "insertRun failed");
        auto service = fx.makeService();
        req(service.recordTerminalRun(runId).success(), "recordTerminalRun failed");
    }
    auto service = fx.makeService();
    const auto context = service.buildPlannerContext(
        "proj-1", makeBaselineSnapshot("current-fingerprint"), /*maxEntries=*/2);
    req(context.success(), "Planner context rendering must succeed");
    req(context.value().includedRunIds.size() <= 2, "buildPlannerContext must respect maxEntries");
    req(!context.value().renderedText.empty(), "A project with history must render non-empty context");
    for (const auto& includedRunId : context.value().includedRunIds) {
        req(context.value().renderedText.find(includedRunId) != std::string::npos,
            "Rendered text must mention every included run id");
    }
}

void testBuildPlannerContextRendersRealDiffAgainstMostRecentBaseline()
{
    // AG-14.2: proves a genuine added/modified/deleted diff is computed and
    // rendered - never just two bare fingerprints left for the Planner to
    // compare by eye.
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");

    WorkspaceBaselineSnapshot previousSnapshot;
    previousSnapshot.files = {
        {"src/a.cpp", 100, sha256Hex("hash-a")}, {"src/b.cpp", 50, sha256Hex("hash-b")}};
    previousSnapshot.totalBytes = 150;
    previousSnapshot.fingerprint = computeBaselineFingerprint(previousSnapshot.files);
    const auto previousJson = WorkspaceBaselineProvenance::renderJson(previousSnapshot);
    req(previousJson.success(), "Could not render the previous baseline snapshot");
    req(fx.artifacts.createArtifact(
            "run-1", std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", previousJson.value())
            .success(),
        "Could not seed the previous baseline artifact");

    auto service = fx.makeServiceWithArtifacts();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");

    WorkspaceBaselineSnapshot currentSnapshot;
    currentSnapshot.fingerprint = sha256Hex("current-fingerprint");
    // src/a.cpp unchanged, src/b.cpp deleted, src/c.cpp added.
    currentSnapshot.files = {
        {"src/a.cpp", 100, sha256Hex("hash-a")}, {"src/c.cpp", 25, sha256Hex("hash-c")}};
    currentSnapshot.totalBytes = 125;

    const auto context = service.buildPlannerContext("proj-1", currentSnapshot);
    req(context.success(), "Planner context rendering must succeed");
    req(context.value().renderedText.find("src/c.cpp") != std::string::npos,
        "Rendered diff must name the added path");
    req(context.value().renderedText.find("src/b.cpp") != std::string::npos,
        "Rendered diff must name the deleted path");
    req(context.value().renderedText.find("Added:\n- src/a.cpp") == std::string::npos,
        "The unchanged path must not be listed as added");
}

void testManualHandoffReviewTransitionsAreRecordedAndRecovered()
{
    // A manual-handoff successor is a real Manifest proposal whose review and
    // Apply history must remain just as durable as the origin/paid-continuation
    // members. The workflow filter used to silently skip this third Manifest
    // workflow both on the live write and during startup reconciliation.
    Fixture fx;
    Run run = makeRun(
        "manual-attempt", "proj-1", std::string(kManifestVerificationManualHandoffWorkflowId),
        RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insert manual-handoff Run failed");
    req(fx.proposals.insertChangeSet(
            makeProposal("cs-live", run.id, "src/live.cpp")).success(),
        "insert live-transition proposal failed");
    req(fx.proposals.insertChangeSet(
            makeProposal("cs-recover", run.id, "src/recover.cpp")).success(),
        "insert reconciliation proposal failed");

    auto service = fx.makeServiceWithApprovalsAndReviews();
    req(service.recordReviewTransition(
            "cs-live", ManifestWorkLedgerTransitionKind::Approved,
            "operator", std::nullopt, std::nullopt).success(),
        "A live manual-handoff review transition must be accepted");

    ApprovalRecord approval;
    approval.changeSetId = "cs-recover";
    approval.snapshotHash = "hash-1";
    approval.approvedAt = std::chrono::system_clock::now();
    approval.approvedBy = "operator";
    approval.appliedAt = approval.approvedAt + std::chrono::seconds(1);
    approval.applyResult = "applied";
    approval.postApplyWorkspaceFingerprint = "manual-post-apply";
    req(fx.approvals.recordApproval(approval).success(),
        "Could not seed the manual-handoff approval record");

    const auto reconciled = service.reconcileMissingTransitions();
    req(reconciled.success() && reconciled.value().recorded == 2
            && reconciled.value().stillMissing.empty(),
        "Startup reconciliation must recover Approved+Applied for manual-handoff proposals");

    const auto transitions = fx.ledger.listTransitionsForRun(run.id);
    req(transitions.success() && transitions.value().size() == 3,
        "Manual-handoff review history must contain the live transition and both recoveries");
    const auto repeated = service.reconcileMissingTransitions();
    req(repeated.success() && repeated.value().recorded == 0
            && repeated.value().stillMissing.empty(),
        "A repeated manual-handoff transition reconciliation must be an idempotent no-op");
}

void testBuildPlannerContextResolvesDerivedVerificationBaselineThroughManifestParent()
{
    // Production regression: a manual-handoff attempt is a legitimate Work
    // Ledger authority but deliberately owns no duplicate live-workspace
    // baseline artifact. When it is newest, Planner context must resolve its
    // exact baseline through manual attempt -> continuation anchor -> origin
    // Manifest Run, never fail merely because the derived Run has no copy.
    Fixture fx;
    const auto now = std::chrono::system_clock::now();
    Run origin = makeRun(
        "origin-run", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    origin.createdAt = now;
    Run continuation = makeRun(
        "continuation-run", "proj-1",
        std::string(kManifestVerificationContinuationWorkflowId),
        RunStatus::Failed);
    continuation.parentRunId = origin.id;
    continuation.createdAt = now + std::chrono::milliseconds(1);
    Run manualAttempt = makeRun(
        "manual-attempt", "proj-1",
        std::string(kManifestVerificationManualHandoffWorkflowId), RunStatus::Failed);
    manualAttempt.parentRunId = continuation.id;
    manualAttempt.createdAt = now + std::chrono::milliseconds(2);
    req(fx.runs.insertRun(origin).success(), "Could not seed origin Manifest Run");
    req(fx.runs.insertRun(continuation).success(), "Could not seed continuation Run");
    req(fx.runs.insertRun(manualAttempt).success(), "Could not seed manual-handoff attempt Run");

    WorkspaceBaselineSnapshot previous;
    previous.files = {{"src/a.cpp", 100, sha256Hex("old-a")}};
    previous.totalBytes = 100;
    previous.fingerprint = computeBaselineFingerprint(previous.files);
    const auto previousJson = WorkspaceBaselineProvenance::renderJson(previous);
    req(previousJson.success(), "Could not render origin baseline");
    req(fx.artifacts.createArtifact(
            origin.id, std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", previousJson.value()).success(),
        "Could not seed the origin Manifest baseline artifact");

    ManifestWorkLedgerEntry newest;
    newest.runId = manualAttempt.id;
    newest.projectId = manualAttempt.projectId;
    newest.terminalStatus = RunStatus::Failed;
    newest.baselineFingerprint = previous.fingerprint;
    newest.disposition = ManifestWorkLedgerDisposition::NotProduced;
    newest.createdAt = now + std::chrono::milliseconds(3);
    req(fx.ledger.insertEntry(newest).success(), "Could not seed derived Work Ledger entry");

    WorkspaceBaselineSnapshot current;
    current.files = {{"src/a.cpp", 100, sha256Hex("new-a")}};
    current.totalBytes = 100;
    current.fingerprint = computeBaselineFingerprint(current.files);
    auto service = fx.makeServiceWithArtifacts();
    const auto context = service.buildPlannerContext("proj-1", current);
    req(context.success(),
        "A derived verification Run must reuse its Manifest parent's baseline artifact");
    req(context.value().renderedText.find(manualAttempt.id) != std::string::npos,
        "The rendered diff must remain attributed to the newest ledger Run");
    req(context.value().renderedText.find("src/a.cpp") != std::string::npos,
        "The parent baseline must still produce a real modified-path diff");
    req(context.value().includedRunIds.size() == 1
            && context.value().includedRunIds.front() == manualAttempt.id,
        "The newest derived ledger Run remains the rendered history identity");
}

void testBuildPlannerContextStillFailsClosedOnMissingOrdinaryManifestBaseline()
{
    // The derived-lineage support above must never degrade the original
    // fail-closed contract: an ordinary Manifest Run owns its own artifact,
    // and losing that artifact remains a hard planning-context failure.
    Fixture fx;
    Run run = makeRun(
        "ordinary-run", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    req(fx.runs.insertRun(run).success(), "Could not seed ordinary Manifest Run");
    ManifestWorkLedgerEntry entry;
    entry.runId = run.id;
    entry.projectId = run.projectId;
    entry.terminalStatus = run.status;
    entry.baselineFingerprint = std::string(64, 'a');
    entry.disposition = ManifestWorkLedgerDisposition::NotProduced;
    entry.createdAt = std::chrono::system_clock::now();
    req(fx.ledger.insertEntry(entry).success(), "Could not seed ordinary ledger entry");

    auto service = fx.makeServiceWithArtifacts();
    const auto context = service.buildPlannerContext(
        "proj-1", makeBaselineSnapshot("current-fingerprint"));
    req(!context.success()
            && context.error()->code == "manifest_work_ledger.context_baseline_missing",
        "A missing ordinary Manifest baseline artifact must remain fail-closed");
}

void testBuildPlannerContextFailsClosedOnBrokenDerivedBaselineLineage()
{
    Fixture fx;
    Run attempt = makeRun(
        "orphan-attempt", "proj-1",
        std::string(kManifestVerificationManualHandoffWorkflowId), RunStatus::Failed);
    req(fx.runs.insertRun(attempt).success(), "Could not seed orphan manual-handoff Run");
    ManifestWorkLedgerEntry entry;
    entry.runId = attempt.id;
    entry.projectId = attempt.projectId;
    entry.terminalStatus = attempt.status;
    entry.baselineFingerprint = std::string(64, 'a');
    entry.disposition = ManifestWorkLedgerDisposition::NotProduced;
    entry.createdAt = std::chrono::system_clock::now();
    req(fx.ledger.insertEntry(entry).success(), "Could not seed orphan ledger entry");

    auto service = fx.makeServiceWithArtifacts();
    const auto context = service.buildPlannerContext(
        "proj-1", makeBaselineSnapshot("current-fingerprint"));
    req(!context.success()
            && context.error()->code
                == "manifest_work_ledger.context_baseline_lineage_invalid",
        "A derived verification Run without its parent must fail closed");
}

void testRecordTerminalRunFailsClosedWhenManifestReadFails()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");

    FailOnDemandExecutionManifestRepository failingManifests;
    failingManifests.shouldFail = true;
    ManifestWorkLedgerService service(
        fx.ledger, fx.runs, failingManifests, fx.taskExecutions, fx.proposals, &fx.artifacts);

    // AG-14.1: a transient repository read failure must propagate as this
    // call's own failure, never be silently reinterpreted as "no Manifest
    // exists" - which would wrongly record a Completed Run as NoChangesNeeded.
    const auto result = service.recordTerminalRun("run-1");
    req(!result.success(), "A failing manifest repository read must fail recordTerminalRun");

    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && !entry.value().has_value(),
        "No ledger entry must be written when a dependent read fails");
    const auto artifacts = fx.artifacts.listArtifacts("run-1");
    req(artifacts.success() && std::any_of(
            artifacts.value().begin(), artifacts.value().end(), [](const Artifact& artifact) {
                return artifact.name == "manifest-work-ledger-write-failed.md";
            }),
        "A dependent read failure must leave a visible failure artifact");
}

constexpr const char* kWriteFailureReportName = "manifest-work-ledger-write-failed.md";

std::size_t writeFailureReportsOf(ArtifactService& artifacts, const RunId& runId)
{
    const auto listed = artifacts.listArtifacts(runId);
    req(listed.success(), "listArtifacts failed");
    return static_cast<std::size_t>(std::count_if(
        listed.value().begin(), listed.value().end(),
        [](const Artifact& artifact) { return artifact.name == kWriteFailureReportName; }));
}

// The legacy case: a Completed no-changes-needed Run without its planner identity can never get a ledger entry (this
// service must not invent the identity), and the startup reconciliation re-tries it on EVERY start. The report of a
// failure that repeats unchanged is written once; a different failure, another Run and another name are not affected.
void testARepeatedIdenticalWriteFailureIsReportedOnce()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed)).success(),
        "insertRun (run-1) failed");
    auto service = fx.makeServiceWithArtifacts();

    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto result = service.recordTerminalRun("run-1");
        req(!result.success() && result.error()->code == "manifest_work_ledger.missing_required_identity",
            "The legacy Run must keep failing with missing_required_identity");
    }
    req(writeFailureReportsOf(fx.artifacts, "run-1") == 1,
        "The same failure, repeated, must leave exactly ONE report");

    // The real path: every startup reconciliation pass.
    for (int pass = 0; pass < 3; ++pass) {
        const auto reconciled = service.reconcileMissingEntries({});
        req(reconciled.success() && reconciled.value().recorded == 0
                && reconciled.value().stillMissing.size() == 1 && reconciled.value().stillMissing.front() == "run-1",
            "A Run that cannot be recorded must still be reported as missing on every pass");
    }
    req(writeFailureReportsOf(fx.artifacts, "run-1") == 1,
        "Repeated reconciliation passes must not append more identical reports");
    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && !entry.value().has_value(), "Nothing may be invented for the legacy Run");

    // Another Run with the very same failure keeps its own report (the check is per Run).
    req(fx.runs.insertRun(makeRun("run-2", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed)).success(),
        "insertRun (run-2) failed");
    req(!service.recordTerminalRun("run-2").success(), "run-2 must fail the same way");
    req(!service.recordTerminalRun("run-2").success(), "run-2 must fail the same way again");
    req(writeFailureReportsOf(fx.artifacts, "run-2") == 1 && writeFailureReportsOf(fx.artifacts, "run-1") == 1,
        "Each Run holds exactly one report of its own");

    // The same bytes under ANOTHER name are not that report: the name is part of the identity.
    const auto first = fx.artifacts.listArtifacts("run-1");
    req(first.success(), "listArtifacts failed");
    std::string reportBytes;
    for (const auto& artifact : first.value()) {
        if (artifact.name == kWriteFailureReportName) {
            const auto content = fx.artifacts.getArtifactContent(artifact.id);
            req(content.success(), "Could not read the report");
            reportBytes = content.value();
        }
    }
    req(!reportBytes.empty(), "The report text must not be empty");
    req(fx.runs.insertRun(makeRun("run-3", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed)).success(),
        "insertRun (run-3) failed");
    req(fx.artifacts.createArtifact("run-3", std::nullopt, ArtifactKind::Report, "other-name.md", "text/markdown",
            reportBytes).success(),
        "Could not seed the same bytes under another name");
    req(!service.recordTerminalRun("run-3").success(), "run-3 must fail the same way");
    req(writeFailureReportsOf(fx.artifacts, "run-3") == 1,
        "An artifact with the same bytes but another name must not suppress the report");
}

void testADifferentWriteFailureOfTheSameRunIsStillReported()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed)).success(),
        "insertRun failed");
    FailOnDemandExecutionManifestRepository manifests;
    ManifestWorkLedgerService service(fx.ledger, fx.runs, manifests, fx.taskExecutions, fx.proposals, &fx.artifacts);

    manifests.shouldFail = true;   // failure A: a transient read failure
    req(!service.recordTerminalRun("run-1").success(), "A read failure must fail recordTerminalRun");
    req(writeFailureReportsOf(fx.artifacts, "run-1") == 1, "Failure A is reported");

    manifests.shouldFail = false;  // failure B: no Manifest and no planner identity (the legacy failure)
    const auto second = service.recordTerminalRun("run-1");
    req(!second.success() && second.error()->code == "manifest_work_ledger.missing_required_identity",
        "Failure B must be the missing identity");
    req(writeFailureReportsOf(fx.artifacts, "run-1") == 2,
        "A DIFFERENT failure of the same Run is reported too, whatever came before");

    req(!service.recordTerminalRun("run-1").success(), "B repeats");
    manifests.shouldFail = true;
    req(!service.recordTerminalRun("run-1").success(), "A repeats");
    req(writeFailureReportsOf(fx.artifacts, "run-1") == 2,
        "Neither failure is written again once its report exists, in any order");
}

// An IArtifactRepository whose listing can be made to fail; everything else is the in-memory repository.
class ListFailingArtifactRepository final : public IArtifactRepository {
public:
    explicit ListFailingArtifactRepository(IArtifactRepository& inner) : inner_(inner) {}
    bool failListing {false};

    common::Result<Artifact> insertArtifact(Artifact artifact) override { return inner_.insertArtifact(std::move(artifact)); }
    [[nodiscard]] common::Result<std::optional<Artifact>> findArtifact(const ArtifactId& id) const override
    {
        return inner_.findArtifact(id);
    }
    [[nodiscard]] common::Result<std::vector<Artifact>> listArtifacts(const RunId& runId) const override
    {
        if (failListing) {
            return common::Result<std::vector<Artifact>>::failure({"artifact.list_failed", "Simulated listing failure"});
        }
        return inner_.listArtifacts(runId);
    }

private:
    IArtifactRepository& inner_;
};

void testAFailureIsStillReportedWhenTheRunsArtifactsCannotBeListed()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Completed)).success(),
        "insertRun failed");
    InMemoryArtifactRepository inner;
    InMemoryArtifactStore store;
    ListFailingArtifactRepository repository(inner);
    ArtifactService artifacts(repository, store);
    ManifestWorkLedgerService service(fx.ledger, fx.runs, fx.manifests, fx.taskExecutions, fx.proposals, &artifacts);

    repository.failListing = true;
    req(!service.recordTerminalRun("run-1").success(), "The Run still cannot be recorded");
    const auto stored = inner.listArtifacts("run-1");
    req(stored.success() && std::count_if(stored.value().begin(), stored.value().end(), [](const Artifact& artifact) {
            return artifact.name == kWriteFailureReportName;
        }) == 1,
        "When the artifacts cannot be listed the failure report is written anyway: a duplicate beats a hidden failure");
}

// ---- the affected paths come from the file-change ITEMS, read in one batch (the port returns headers) -------------------

void testAffectedPathsAreBuiltFromTheBatchedItemsOfEveryProposal()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed)).success(),
        "insertRun failed");
    // Repeated paths inside one set and across sets, an unsorted insertion order and a set without any item.
    req(fx.proposals.insertChangeSet(makeProposalWithPaths("cs-b", "run-1", "proj-1",
            {"src/b.cpp", "src/a.cpp", "src/a.cpp"})).success(), "insert cs-b failed");
    req(fx.proposals.insertChangeSet(makeProposalWithPaths("cs-a", "run-1", "proj-1",
            {"docs/z.md", "src/a.cpp"})).success(), "insert cs-a failed");
    req(fx.proposals.insertChangeSet(makeProposalWithPaths("cs-0", "run-1", "proj-1", {},
            ChangeSetState::NoChangesNeeded)).success(), "insert cs-c failed");

    // Like production: the headers carry no file changes at all.
    const auto header = fx.proposals.findChangeSet("cs-b");
    req(header.success() && header.value().has_value() && header.value()->changes.empty(),
        "The fake must return headers only, like the SQLite adapter");

    auto service = fx.makeService();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");
    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && entry.value().has_value(), "Expected a ledger entry");
    req(entry.value()->affectedRelativePaths == std::vector<std::string> {"docs/z.md", "src/a.cpp", "src/b.cpp"},
        "The affected paths must be the unique, sorted paths of the items of ALL proposals");
    req(entry.value()->proposalIds.size() == 3, "Every proposal is still listed");
    req(fx.proposals.batchReads == 1, "ONE batched read for the whole Run, not one per proposal");
    auto ids = fx.proposals.lastBatchIds;
    std::sort(ids.begin(), ids.end());
    req(ids == std::vector<ChangeSetId> {"cs-0", "cs-a", "cs-b"}, "The batch must ask for every proposal of the Run");
}

void testAffectedPathsDoNotDependOnTheOrderOfTheProposalsAndAreBoundedInSortedOrder()
{
    Fixture fx;
    for (const char* id : {"run-1", "run-2", "run-3"}) {
        req(fx.runs.insertRun(makeRun(id, "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed)).success(),
            "insertRun failed");
    }
    for (const char* runId : {"run-1", "run-2"}) {
        const std::string suffix = runId;
        req(fx.proposals.insertChangeSet(makeProposalWithPaths("one-" + suffix, runId, "proj-1", {"src/z.cpp", "src/m.cpp"}))
                .success(), "insert failed");
        req(fx.proposals.insertChangeSet(makeProposalWithPaths("two-" + suffix, runId, "proj-1", {"src/a.cpp"}))
                .success(), "insert failed");
    }
    auto service = fx.makeService();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun (run-1) failed");
    fx.proposals.reverseListOrder = true;
    req(service.recordTerminalRun("run-2").success(), "recordTerminalRun (run-2) failed");
    fx.proposals.reverseListOrder = false;
    const auto first = fx.ledger.findEntry("run-1");
    const auto second = fx.ledger.findEntry("run-2");
    req(first.success() && second.success() && first.value().has_value() && second.value().has_value(), "Expected two entries");
    req(first.value()->affectedRelativePaths == std::vector<std::string> {"src/a.cpp", "src/m.cpp", "src/z.cpp"},
        "Sorted paths expected");
    req(first.value()->affectedRelativePaths == second.value()->affectedRelativePaths,
        "The order in which the repository lists the proposals must not change the recorded paths");

    // The bound keeps the FIRST paths of the canonical (sorted) order, whatever came first from the repository.
    std::vector<std::string> many;
    for (int i = 299; i >= 0; --i) {
        std::string number = std::to_string(i);
        number.insert(0, 3 - number.size(), '0');
        many.push_back("src/f" + number + ".cpp");
    }
    req(fx.proposals.insertChangeSet(makeProposalWithPaths("cs-many", "run-3", "proj-1", many)).success(), "insert failed");
    req(service.recordTerminalRun("run-3").success(), "recordTerminalRun (run-3) failed");
    const auto bounded = fx.ledger.findEntry("run-3");
    req(bounded.success() && bounded.value().has_value(), "Expected an entry for run-3");
    const auto& paths = bounded.value()->affectedRelativePaths;
    req(paths.size() == kManifestWorkLedgerMaxAffectedPaths && paths.front() == "src/f000.cpp" && paths.back() == "src/f255.cpp"
            && std::is_sorted(paths.begin(), paths.end()),
        "The bound must keep the first 256 paths in sorted order");
}

void testARunWithoutProposalsNeedsNoBatchedRead()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed)).success(),
        "insertRun failed");
    auto service = fx.makeService();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");
    req(fx.proposals.batchReads == 0, "No proposal means no batched read");
    const auto entry = fx.ledger.findEntry("run-1");
    req(entry.success() && entry.value().has_value() && entry.value()->affectedRelativePaths.empty()
            && entry.value()->proposalIds.empty(),
        "A Run without proposals records no paths");
}

void testAFailedBatchedReadFailsClosedAndLeavesTheNormalFailureReport()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed)).success(),
        "insertRun failed");
    req(fx.proposals.insertChangeSet(makeProposalWithPaths("cs-1", "run-1", "proj-1", {"src/a.cpp"})).success(),
        "insert failed");
    auto service = fx.makeServiceWithArtifacts();

    fx.proposals.failBatchRead = true;
    const auto failed = service.recordTerminalRun("run-1");
    req(!failed.success() && failed.error()->code == "proposal.batch_read_failed",
        "The read error must propagate unchanged, never be masked");
    const auto absent = fx.ledger.findEntry("run-1");
    req(absent.success() && !absent.value().has_value(),
        "No ledger entry - above all no entry with empty paths - may be written when the items cannot be read");
    req(writeFailureReportsOf(fx.artifacts, "run-1") == 1, "The ordinary failure report must be left behind");
    req(!service.recordTerminalRun("run-1").success() && writeFailureReportsOf(fx.artifacts, "run-1") == 1,
        "The same failure again is still reported once");

    fx.proposals.failBatchRead = false;
    req(service.recordTerminalRun("run-1").success(), "The next call must succeed once the read works");
    const auto recorded = fx.ledger.findEntry("run-1");
    req(recorded.success() && recorded.value().has_value()
            && recorded.value()->affectedRelativePaths == std::vector<std::string> {"src/a.cpp"},
        "The entry written after the failure carries the real paths");
}

void testAnExistingLedgerEntryIsNeverRewrittenByTheFix()
{
    Fixture fx;
    req(fx.runs.insertRun(makeRun("run-old", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed)).success(),
        "insertRun failed");
    req(fx.proposals.insertChangeSet(makeProposalWithPaths("cs-old", "run-old", "proj-1", {"src/old.cpp"})).success(),
        "insert failed");
    // An entry written before the fix: the proposal is listed, the paths are empty.
    ManifestWorkLedgerEntry oldEntry;
    oldEntry.runId = "run-old";
    oldEntry.projectId = "proj-1";
    oldEntry.terminalStatus = RunStatus::Failed;
    oldEntry.terminalReason = "written before the fix";
    oldEntry.baselineFingerprint = std::string(64, 'a');
    oldEntry.disposition = ManifestWorkLedgerDisposition::NotProduced;
    oldEntry.proposalIds = {"cs-old"};
    oldEntry.createdAt = std::chrono::system_clock::now();
    req(fx.ledger.insertEntry(oldEntry).success(), "Could not seed the old entry");

    auto service = fx.makeService();
    req(service.recordTerminalRun("run-old").success(), "A repeat for a recorded Run is a benign no-op");
    const auto reconciled = service.reconcileMissingEntries({});
    req(reconciled.success() && reconciled.value().recorded == 0, "Reconciliation must not re-record a recorded Run");

    const auto after = fx.ledger.findEntry("run-old");
    req(after.success() && after.value().has_value(), "The old entry must still exist");
    req(after.value()->affectedRelativePaths.empty() && after.value()->proposalIds == oldEntry.proposalIds
            && after.value()->terminalReason == "written before the fix",
        "An entry written before the fix stays exactly as it was: immutable, never back-filled");
}

// The same defect over the REAL SQLite repositories: the headers stay headers and the new entry carries the paths.
void testRealSqliteRepositoriesGiveTheLedgerItsPathsAndKeepHeadersHeaderOnly()
{
    namespace fs = std::filesystem;
    struct TempDir {
        fs::path root {fs::temp_directory_path()
            / ("ai-orchestrator-ledger-paths-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
        ~TempDir() { std::error_code ignored; fs::remove_all(root, ignored); }
    } temp;
    fs::create_directories(temp.root);
    const auto opened = ai::infrastructure::SqliteDatabase::open(temp.root / "orchestrator.sqlite3");
    req(opened.success(), "Could not open the SQLite database");
    const auto database = opened.value();

    const auto now = std::chrono::system_clock::now();
    ai::infrastructure::SqliteProjectRepository projects(database);
    Project project {"proj-1", "Ledger paths test project", "Affected paths over real repositories",
        temp.root / "proj-1", ProjectStatus::Active, now, now};
    req(projects.insert(project).success(), "Could not seed the project");
    ai::infrastructure::SqliteRunRepository runs(database);
    for (const char* id : {"run-1", "run-old"}) {
        Run run = makeRun(id, "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
        run.createdAt = now;
        req(runs.insertRun(run).success(), "Could not seed the Run");
    }
    ai::infrastructure::SqliteExecutionManifestRepository manifests(database);
    ai::infrastructure::SqliteManifestTaskExecutionRepository taskExecutions(database);
    ai::infrastructure::SqliteChangeProposalRepository proposals(database);
    ai::infrastructure::SqliteManifestWorkLedgerRepository ledger(database);

    req(proposals.insertChangeSet(makeProposalWithPaths("cs-1", "run-1", "proj-1", {"src/b.cpp", "src/a.cpp"})).success(),
        "Could not store cs-1");
    req(proposals.insertChangeSet(makeProposalWithPaths("cs-2", "run-1", "proj-1", {"src/a.cpp", "docs/z.md", "src/c.cpp"})).success(),
        "Could not store cs-2");

    // What the port really returns: headers. The items are stored and are read separately.
    const auto forRun = proposals.listForRun("run-1");
    req(forRun.success() && forRun.value().size() == 2, "listForRun must list both proposals");
    for (const auto& changeSet : forRun.value()) req(changeSet.changes.empty(), "listForRun must stay header-only");
    const auto found = proposals.findChangeSet("cs-1");
    req(found.success() && found.value().has_value() && found.value()->changes.empty(), "findChangeSet must stay header-only");
    const auto forProject = proposals.listForProject("proj-1");
    req(forProject.success() && forProject.value().size() == 2, "listForProject must list both proposals");
    for (const auto& changeSet : forProject.value()) req(changeSet.changes.empty(), "listForProject must stay header-only");
    const auto items = proposals.listChangesForSets({"cs-1", "cs-2"});
    req(items.success() && items.value().at("cs-1").size() == 2 && items.value().at("cs-2").size() == 3,
        "The items are read through listChangesForSets");

    ManifestWorkLedgerService service(ledger, runs, manifests, taskExecutions, proposals);
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun over SQLite failed");
    const auto entry = ledger.findEntry("run-1");
    req(entry.success() && entry.value().has_value(), "Expected a ledger entry in SQLite");
    req(entry.value()->affectedRelativePaths == std::vector<std::string> {"docs/z.md", "src/a.cpp", "src/b.cpp", "src/c.cpp"},
        "The stored file changes must appear, unique and sorted, in the new ledger entry");
    req(entry.value()->proposalIds.size() == 2, "Both proposals are listed");

    // An entry written before the fix is never rewritten.
    ManifestWorkLedgerEntry oldEntry;
    oldEntry.runId = "run-old";
    oldEntry.projectId = "proj-1";
    oldEntry.terminalStatus = RunStatus::Failed;
    oldEntry.terminalReason = "written before the fix";
    oldEntry.baselineFingerprint = std::string(64, 'a');
    oldEntry.disposition = ManifestWorkLedgerDisposition::NotProduced;
    oldEntry.proposalIds = {"cs-old"};
    oldEntry.createdAt = now;
    req(proposals.insertChangeSet(makeProposalWithPaths("cs-old", "run-old", "proj-1", {"src/old.cpp"})).success(),
        "Could not store cs-old");
    req(ledger.insertEntry(oldEntry).success(), "Could not seed the old entry");
    req(service.recordTerminalRun("run-old").success(), "A repeat for a recorded Run is a benign no-op");
    const auto reread = ledger.findEntry("run-old");
    req(reread.success() && reread.value().has_value() && reread.value()->affectedRelativePaths.empty()
            && reread.value()->terminalReason == "written before the fix",
        "The old entry in SQLite stays exactly as it was");
}

void testReconcileMissingEntriesRecordsOnlyTrulyMissingRuns()
{
    Fixture fx;
    // AG-14.2: Failed (not Completed) - this test is about reconciliation's
    // own dedup/pickup behavior, not the Completed+NoChangesNeeded typed-
    // identity invariant exercised elsewhere.
    Run alreadyRecorded = makeRun(
        "run-recorded", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    Run missing = makeRun("run-missing", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    missing.errorMessage = "planning failed";
    Run stillRunning = makeRun("run-running", "proj-1", kExecutionManifestWorkflowId, RunStatus::Running);
    req(fx.runs.insertRun(alreadyRecorded).success(), "insertRun (recorded) failed");
    req(fx.runs.insertRun(missing).success(), "insertRun (missing) failed");
    req(fx.runs.insertRun(stillRunning).success(), "insertRun (running) failed");

    auto service = fx.makeService();
    req(service.recordTerminalRun("run-recorded").success(),
        "Could not pre-record run-recorded's own entry");
    const auto preExisting = fx.ledger.findEntry("run-recorded");
    req(preExisting.success() && preExisting.value().has_value(), "Precondition: entry must exist");
    const std::string originalReason = preExisting.value()->terminalReason;

    const auto result = service.reconcileMissingEntries();
    req(result.success(), "Entry reconciliation must succeed");
    req(result.value().recorded == 1, "Exactly one truly-missing terminal Run must be recorded");
    req(result.value().stillMissing.empty(), "No Run should remain missing after a clean reconciliation pass");

    const auto recordedEntry = fx.ledger.findEntry("run-recorded");
    req(recordedEntry.success() && recordedEntry.value().has_value()
            && recordedEntry.value()->terminalReason == originalReason,
        "reconcileMissingEntries must never re-touch an already-recorded entry");
    const auto missingEntry = fx.ledger.findEntry("run-missing");
    req(missingEntry.success() && missingEntry.value().has_value(),
        "reconcileMissingEntries must record the truly-missing terminal Run");
    const auto runningEntry = fx.ledger.findEntry("run-running");
    req(runningEntry.success() && !runningEntry.value().has_value(),
        "reconcileMissingEntries must never record a still-Running Run");
}

void testBuildPlannerContextWithTinyMaxBytesStaysEmptyRatherThanTruncatingTheHeaderOnly()
{
    Fixture fx;
    Run run = makeRun("run-1", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");
    auto service = fx.makeService();
    req(service.recordTerminalRun("run-1").success(), "recordTerminalRun failed");

    // AG-14.1/AG-14.2: a budget too small to fit even the header (which now
    // always includes the current baseline section) must produce an
    // entirely empty context, never a header-only string that itself
    // exceeds maxBytes.
    const auto context = service.buildPlannerContext(
        "proj-1", makeBaselineSnapshot("current-fingerprint"), /*maxEntries=*/5, /*maxBytes=*/8);
    req(context.success(), "A tiny budget is not a repository failure");
    req(context.value().renderedText.empty(),
        "A maxBytes budget too small for the header must render nothing, not a truncated header");
    req(context.value().includedRunIds.empty(),
        "A maxBytes budget too small for any entry must include no run ids");
}

void testBuildPlannerContextFailsClosedOnLedgerReadFailure()
{
    Fixture fx;
    FailingManifestWorkLedgerRepository failingLedger;
    ManifestWorkLedgerService service(
        failingLedger, fx.runs, fx.manifests, fx.taskExecutions, fx.proposals, &fx.artifacts);
    const auto context = service.buildPlannerContext(
        "proj-1", makeBaselineSnapshot("current-fingerprint"));
    req(!context.success() && context.error()->code == "ledger.read_failed",
        "A ledger read failure must stop context construction");
}

void testBuildPlannerContextPreservesAnchorAndTruncatesLargeDiff()
{
    Fixture fx;
    Run run = makeRun("run-large-diff", "proj-1", kExecutionManifestWorkflowId, RunStatus::Failed);
    req(fx.runs.insertRun(run).success(), "insertRun failed");

    WorkspaceBaselineSnapshot previous;
    previous.files = {{"src/stable.cpp", 10, sha256Hex("stable")}};
    previous.totalBytes = 10;
    previous.fingerprint = computeBaselineFingerprint(previous.files);
    const auto previousJson = WorkspaceBaselineProvenance::renderJson(previous);
    req(previousJson.success(), "Could not render prior baseline");
    req(fx.artifacts.createArtifact(
            run.id, std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", previousJson.value()).success(),
        "Could not persist prior baseline");
    auto service = fx.makeServiceWithArtifacts();
    req(service.recordTerminalRun(run.id).success(), "Could not record prior Run");

    WorkspaceBaselineSnapshot current = previous;
    for (int i = 0; i < 100; ++i) {
        current.files.push_back({
            "generated/very-long-component-name-" + std::to_string(i) + ".cpp",
            20, sha256Hex("content-" + std::to_string(i))});
        current.totalBytes += 20;
    }
    current.fingerprint = computeBaselineFingerprint(current.files);
    const auto context = service.buildPlannerContext(
        "proj-1", current, /*maxEntries=*/5, /*maxBytes=*/700);
    req(context.success(), "A large diff must be bounded, not rejected");
    req(!context.value().renderedText.empty() && context.value().renderedText.size() <= 700,
        "A bounded context must retain its anchor and respect maxBytes");
    req(context.value().renderedText.find(current.fingerprint) != std::string::npos,
        "The current baseline anchor must survive diff truncation");
    req(context.value().renderedText.find("diff paths truncated") != std::string::npos,
        "A truncated diff must say that it was truncated");
    req(context.value().includedRunIds.size() == 1
            && context.value().includedRunIds.front() == run.id,
        "includedRunIds must exactly name the prior Run used by the rendered diff");
}

void testPlannerNoChangesIdentityParserIsStrict()
{
    PlannerNoChangesNeeded valid;
    valid.summary = "No work remains";
    valid.evidence.push_back({"criterion", "src/a.cpp", "already implemented"});
    valid.recommendedNextStep = "Continue with the next stage";
    req(parsePlannerNoChangesNeededJson(renderPlannerNoChangesNeededJson(valid)).success(),
        "The renderer's canonical identity must parse");
    req(!parsePlannerNoChangesNeededJson(
            R"({"schemaVersion":"execution-manifest-no-changes.v1","summary":"x","evidence":[],"recommendedNextStep":null})")
            .success(),
        "An empty evidence list must be rejected");
    req(!parsePlannerNoChangesNeededJson(
            R"({"schemaVersion":"execution-manifest-no-changes.v1","summary":"x","evidence":[{"criterion":"c","path":"p","observation":"o","extra":true}],"recommendedNextStep":null})")
            .success(),
        "Unexpected nested fields must be rejected");
    req(!parsePlannerNoChangesNeededJson(
            R"({"schemaVersion":"wrong","summary":"x","evidence":[{"criterion":"c","path":"p","observation":"o"}],"recommendedNextStep":null})")
            .success(),
        "An unsupported identity schema must be rejected");
}

void testPaidRetryGuardBlocksOnlyUnconfirmedUnchangedFailure()
{
    Fixture fx;
    ManifestWorkLedgerEntry failed;
    failed.runId = "failed-run";
    failed.projectId = "proj-1";
    failed.terminalStatus = RunStatus::Failed;
    failed.disposition = ManifestWorkLedgerDisposition::NotProduced;
    failed.planningInputFingerprint = std::string(64, 'a');
    failed.planningFailureSignature = std::string(64, 'b');
    failed.createdAt = std::chrono::system_clock::now();
    req(fx.ledger.insertEntry(failed).success(), "Could not seed planning failure");
    auto service = fx.makeService();

    const auto blocked = service.authorizePlanningAttempt(
        "proj-1", failed.planningInputFingerprint, false);
    req(!blocked.success() && blocked.error()->code == "planner.unchanged_failed_input",
        "An unchanged unconfirmed paid retry must be blocked");
    req(service.authorizePlanningAttempt(
            "proj-1", failed.planningInputFingerprint, true).success(),
        "An explicit linked retry must be allowed");
    req(service.authorizePlanningAttempt(
            "proj-1", std::string(64, 'c'), false).success(),
        "A changed planning input must be allowed");
    const auto malformed = service.authorizePlanningAttempt("proj-1", "not-a-hash", false);
    req(!malformed.success()
            && malformed.error()->code == "planner.invalid_planning_input_fingerprint",
        "A malformed Backend fingerprint must fail closed");
}

void testPlannerContextCollapsesConsecutiveRejectedPlanningHistory()
{
    Fixture fx;
    for (int index = 0; index < 2; ++index) {
        ManifestWorkLedgerEntry failed;
        failed.runId = "failed-run-" + std::to_string(index);
        failed.projectId = "proj-1";
        failed.terminalStatus = RunStatus::Failed;
        failed.disposition = ManifestWorkLedgerDisposition::NotProduced;
        failed.planningInputFingerprint = std::string(64, 'a');
        failed.planningFailureSignature = std::string(64, 'b');
        failed.createdAt = std::chrono::system_clock::now()
            + std::chrono::milliseconds(index);
        req(fx.ledger.insertEntry(failed).success(), "Could not seed repeated failure");
    }
    auto service = fx.makeService();
    const auto context = service.buildPlannerContext(
        "proj-1", makeBaselineSnapshot("current-fingerprint"));
    req(context.success(), "Could not render collapsed planning history");
    req(context.value().includedRunIds.size() == 1,
        "Consecutive identical planning failures must collapse to one entry");
    req(context.value().renderedText.find("non-authoritative allocation history")
            != std::string::npos,
        "Rejected planning history must be explicitly non-authoritative");
}

} // namespace

int runManifestWorkLedgerServiceTests()
{
    testInMemoryLedgerRejectsDuplicateEntryAndTransition();
    testRecordTerminalRunSkipsNonManifestWorkflow();
    testRecordTerminalRunSkipsNonTerminalStatus();
    testRecordTerminalRunAssemblesManifestReadyEntry();
    testRecordTerminalRunFailsClosedWhenManifestReadyCompletedButVerificationIdentityMissing();
    testRecordTerminalRunNoChangesNeededWhenManifestAbsentButCompleted();
    testRecordTerminalRunFailsClosedWhenNoChangesNeededButPlannerIdentityMissing();
    testRecordTerminalRunNotProducedWhenManifestAbsentAndFailed();
    testRunManagerNudgesTheOutcomeBriefOnlyAfterTheLedgerEntry();
    testRecordTerminalRunFailsClosedWhenManifestReadFails();
    testARepeatedIdenticalWriteFailureIsReportedOnce();
    testADifferentWriteFailureOfTheSameRunIsStillReported();
    testAFailureIsStillReportedWhenTheRunsArtifactsCannotBeListed();
    testAffectedPathsAreBuiltFromTheBatchedItemsOfEveryProposal();
    testAffectedPathsDoNotDependOnTheOrderOfTheProposalsAndAreBoundedInSortedOrder();
    testARunWithoutProposalsNeedsNoBatchedRead();
    testAFailedBatchedReadFailsClosedAndLeavesTheNormalFailureReport();
    testAnExistingLedgerEntryIsNeverRewrittenByTheFix();
    testRealSqliteRepositoriesGiveTheLedgerItsPathsAndKeepHeadersHeaderOnly();
    testReconcileMissingEntriesRecordsOnlyTrulyMissingRuns();
    testRecordReviewTransitionSkipsNonManifestWorkflow();
    testRecordReviewTransitionAppendsForManifestWorkflow();
    testRecordReviewTransitionIsIdempotentOnRetry();
    testReconcileMissingTransitionsRecoversApprovalRejectionAndApply();
    testManualHandoffReviewTransitionsAreRecordedAndRecovered();
    testBuildPlannerContextIncludesCurrentBaselineEvenWithNoHistory();
    testBuildPlannerContextRespectsMaxEntriesAndReturnsIncludedIds();
    testBuildPlannerContextRendersRealDiffAgainstMostRecentBaseline();
    testBuildPlannerContextResolvesDerivedVerificationBaselineThroughManifestParent();
    testBuildPlannerContextStillFailsClosedOnMissingOrdinaryManifestBaseline();
    testBuildPlannerContextFailsClosedOnBrokenDerivedBaselineLineage();
    testBuildPlannerContextWithTinyMaxBytesStaysEmptyRatherThanTruncatingTheHeaderOnly();
    testBuildPlannerContextFailsClosedOnLedgerReadFailure();
    testBuildPlannerContextPreservesAnchorAndTruncatesLargeDiff();
    testPlannerNoChangesIdentityParserIsStrict();
    testPaidRetryGuardBlocksOnlyUnconfirmedUnchangedFailure();
    testPlannerContextCollapsesConsecutiveRejectedPlanningHistory();
    return 0;
}
