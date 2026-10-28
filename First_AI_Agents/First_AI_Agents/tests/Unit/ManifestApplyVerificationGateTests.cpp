// Tests for AG-16.1's ManifestApplyVerificationGate: the centralized
// Backend decision for whether an ordinary Approve/Apply may proceed for a
// change set, plus its wiring into ChangeSetApplyService::approve()/apply().
// Real FileChangeProposalStore + temp directories, in-memory repositories,
// no real provider HTTP calls.
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/proposals/ChangeSetApplyService.hpp"
#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/proposals/IChangeApprovalRepository.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryVerificationBootstrapRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/verification/ManifestApplyVerificationGate.hpp"
#include "ai/application/ManifestWorkLedgerTypes.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;
using namespace ai::infrastructure;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orch-apply-gate-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Same shape as ChangeSetApplyServiceTests.cpp's own fake - each test file
// in this codebase is self-contained rather than sharing test-only fakes
// across translation units.
class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(cs.changeSetId))
            return common::Result<void>::failure({"proposal.duplicate_id", "Duplicate"});
        byId_[cs.changeSetId] = cs;
        changeLists_[cs.changeSetId] = cs.changes;
        byId_[cs.changeSetId].changes.clear();
        return common::Result<void>::ok();
    }
    common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<ProposedChangeSet>::failure({"proposal.not_found", "Not found"});
        it->second.state = newState;
        return common::Result<ProposedChangeSet>::ok(it->second);
    }
    common::Result<void> markMissingEvidence(
        const ChangeSetId&, std::string, std::string) override
    {
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(it->second);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> r;
        for (const auto& [id, cs] : byId_) if (cs.runId == runId) r.push_back(cs);
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(r));
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = changeLists_.find(id);
        if (it == changeLists_.end())
            return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second);
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId&) const override
    {
        return common::Result<std::vector<ValidationFinding>>::ok({});
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& ids) const override
    {
        std::lock_guard lock(mu_);
        std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> result;
        for (const auto& id : ids) {
            const auto it = changeLists_.find(id);
            if (it != changeLists_.end()) result[id] = it->second;
        }
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>::ok(
            std::move(result));
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>&) const override
    {
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>::ok({});
    }

    // Test helper: directly insert a change set with a given state, bypassing
    // the validation pipeline - used to simulate a diverged/foreign row.
    void insertRaw(ProposedChangeSet cs, std::vector<ProposedFileChange> changes)
    {
        std::lock_guard lock(mu_);
        const auto id = cs.changeSetId;
        changeLists_[id] = std::move(changes);
        cs.changes.clear();
        byId_[id] = std::move(cs);
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
    std::map<ChangeSetId, std::vector<ProposedFileChange>> changeLists_;
};

class FakeChangeApprovalRepository final : public IChangeApprovalRepository {
public:
    common::Result<void> recordApproval(const ApprovalRecord& record) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(record.changeSetId))
            return common::Result<void>::failure({"proposal.duplicate_id", "Duplicate"});
        byId_[record.changeSetId] = record;
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ApprovalRecord>> findApproval(
        const ChangeSetId& changeSetId) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end())
            return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
        return common::Result<std::optional<ApprovalRecord>>::ok(it->second);
    }
    common::Result<void> recordApplyOutcome(
        const ChangeSetId& changeSetId, const std::string& result,
        const std::optional<std::string>& errorCode,
        const std::vector<ApplyHashEntry>& hashLog,
        const std::optional<std::string>& postApplyWorkspaceFingerprint) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "No approval record"});
        it->second.appliedAt = std::chrono::system_clock::now();
        it->second.applyResult = result;
        it->second.applyErrorCode = errorCode;
        it->second.applyHashLog = hashLog;
        it->second.postApplyWorkspaceFingerprint = postApplyWorkspaceFingerprint;
        return common::Result<void>::ok();
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ApprovalRecord> byId_;
};

// Minimal IRunRepository that can be told to fail findRun() on demand - used
// only to prove a repository read failure while resolving the owning Run
// fails closed and is never reported as gate_not_applicable.
class FailingRunRepository final : public IRunRepository {
public:
    common::Result<Run> insertRun(Run run) override
    {
        std::lock_guard lock(mu_);
        byId_[run.id] = run;
        return common::Result<Run>::ok(run);
    }
    common::Result<Run> insertRunWithStages(Run run, std::vector<RunStage>) override
    {
        return insertRun(std::move(run));
    }
    common::Result<Run> updateRun(Run run) override { return insertRun(std::move(run)); }
    common::Result<std::optional<Run>> findRun(const RunId& id) const override
    {
        std::lock_guard lock(mu_);
        if (failFindRun) {
            return common::Result<std::optional<Run>>::failure(
                {"storage.unavailable", "Simulated repository read failure"});
        }
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::optional<Run>>::ok(std::nullopt);
        return common::Result<std::optional<Run>>::ok(it->second);
    }
    common::Result<std::vector<Run>> listRuns(const std::optional<ProjectId>&) const override
    {
        return common::Result<std::vector<Run>>::ok({});
    }
    common::Result<std::vector<Run>> listUnfinishedRuns() const override
    {
        return common::Result<std::vector<Run>>::ok({});
    }
    common::Result<void> replaceStages(const RunId&, std::vector<RunStage>) override
    {
        return common::Result<void>::ok();
    }
    common::Result<RunStage> updateStage(RunStage stage) override
    {
        return common::Result<RunStage>::ok(stage);
    }
    common::Result<std::vector<RunStage>> listStages(const RunId&) const override
    {
        return common::Result<std::vector<RunStage>>::ok({});
    }

    bool failFindRun {false};

private:
    mutable std::mutex mu_;
    std::map<RunId, Run> byId_;
};

std::string makeCreateProposal(const std::string& relativePath, const std::string& content)
{
    return R"(```json
{
  "schemaVersion": "proposal-submission.v2",
  "summary": "test create",
  "changes": [
    {
      "change_id": "c1",
      "operation": "create",
      "relative_path": ")" + relativePath + R"(",
      "source_path": null,
      "proposed_content": ")" + content + R"("
    }
  ]
}
```)";
}

// AG-16.1: mirrors ManifestApplyVerificationGate.cpp's own
// computeProposalContentHash exactly - a test oracle used only to construct
// correct verification records, never to re-test the algorithm itself
// (production behavior is what every test below actually exercises).
std::string testComputeContentHash(
    const ChangeSetId& proposalId,
    const std::vector<ProposedFileChange>& changes,
    ChangeProposalService& proposals)
{
    std::ostringstream material;
    material << proposalId << '\n';
    for (const auto& change : changes) {
        material << toString(change.operation) << '\0'
            << change.relativePath.generic_string() << '\0';
        if (change.sourcePath.has_value()) material << change.sourcePath->generic_string();
        material << '\0';
        if (change.operation == FileChangeOperation::Create
                || change.operation == FileChangeOperation::Modify) {
            const auto content = proposals.readProposedContent(proposalId, change.changeId);
            req(content.success(), "test oracle could not read proposed content");
            material << sha256Hex(content.value());
        }
        material << '\n';
    }
    return sha256Hex(material.str());
}

// Fixed identity constants every test's Fixture uses - "p1" is the only
// project, "baseline-1" the only baseline fingerprint - so
// makePassingVerification()'s defaults line up with Fixture::seedLedgerEntry()'s
// defaults without every call site having to repeat them.
constexpr const char* kFixtureProjectId = "p1";
constexpr const char* kFixtureBaselineFingerprint = "baseline-1";

ManifestProposalSetVerification makePassingVerification(
    std::vector<ChangeSetId> orderedIds, std::vector<std::string> orderedHashes)
{
    ManifestProposalSetVerification v;
    v.workspaceIdentity = kFixtureProjectId;
    v.baselineFingerprint = kFixtureBaselineFingerprint;
    v.overlayFingerprint = "overlay-1";
    v.orderedProposalIds = std::move(orderedIds);
    v.orderedProposalContentHashes = std::move(orderedHashes);
    v.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    v.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    v.acceptanceStatus = ExecutionVerificationStatus::Passed;
    return v;
}

// AG-16.1 hardening: builds the Work Ledger's own independent record of
// "which changeSetId did each task finally produce" - the second source of
// truth the exact-final-proposal-set check cross-references against
// verification.orderedProposalIds. One synthetic task per id, in order.
std::vector<ManifestWorkLedgerTaskDisposition> makeTaskDispositions(
    const std::vector<ChangeSetId>& changeSetIds)
{
    std::vector<ManifestWorkLedgerTaskDisposition> result;
    for (std::size_t i = 0; i < changeSetIds.size(); ++i) {
        ManifestWorkLedgerTaskDisposition disposition;
        disposition.taskId = "task-" + std::to_string(i);
        disposition.status = "completed";
        disposition.changeSetId = changeSetIds[i];
        result.push_back(std::move(disposition));
    }
    return result;
}

struct Fixture {
    TempDir projectDir;
    TempDir proposalDir;
    FakeChangeProposalRepository repo;
    FakeChangeApprovalRepository approvals;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryManifestWorkLedgerRepository ledger;
    std::unique_ptr<FileChangeProposalStore> store;

    Fixture()
    {
        auto storeResult = FileChangeProposalStore::open(proposalDir.path / "proposals");
        req(storeResult.success(), "Could not open change-proposal store");
        store = std::move(storeResult.value());

        Project project;
        project.id = kFixtureProjectId;
        project.name = "AG-16.1 gate fixture";
        project.rootPath = fs::weakly_canonical(projectDir.path);
        project.status = ProjectStatus::Active;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        req(projects.insert(project).success(), "Could not seed project");
    }

    ChangeProposalService makeProposalService() { return ChangeProposalService(repo, *store); }

    ManifestApplyVerificationGate makeGate(ChangeProposalService& proposals)
    {
        return ManifestApplyVerificationGate(proposals, ledger, runs);
    }

    ChangeSetApplyService makeApplyService(ManifestApplyVerificationGate* gate)
    {
        return ChangeSetApplyService(
            repo, *store, runs, projects, approvals,
            /*artifacts=*/nullptr, /*manifestWorkLedgerService=*/nullptr, gate);
    }

    ProjectWritePolicy defaultPolicy()
    {
        ProjectWritePolicy p;
        p.projectRoot = projectDir.path;
        return p;
    }

    Run seedRun(const RunId& id, const std::string& workflowId)
    {
        Run run;
        run.id = id;
        run.projectId = kFixtureProjectId;
        run.title = "AG-16.1 gate fixture run";
        run.task = "Exercise the verification gate";
        run.workflowId = workflowId;
        run.workspacePath = fs::weakly_canonical(projectDir.path);
        run.status = RunStatus::Completed;
        run.createdAt = std::chrono::system_clock::now();
        req(runs.insertRun(run).success(), "Could not seed Run");
        return run;
    }

    // taskDispositions defaults to one disposition per verification's own
    // orderedProposalIds (the consistent, non-diverged case every "happy
    // path" and per-member-loop test wants) - pass an explicit list only
    // when a test deliberately wants the two independent sources to
    // disagree (the exact-final-proposal-set hardening tests) or wants
    // extra/fewer synthetic tasks than proposals.
    void seedLedgerEntry(
        const RunId& runId,
        std::optional<ManifestProposalSetVerification> verification,
        std::optional<std::vector<ManifestWorkLedgerTaskDisposition>> taskDispositions = std::nullopt,
        RunStatus terminalStatus = RunStatus::Completed,
        std::optional<ManifestWorkLedgerDisposition> dispositionOverride = std::nullopt,
        std::optional<std::string> projectIdOverride = std::nullopt,
        std::optional<std::string> baselineFingerprintOverride = std::nullopt)
    {
        ManifestWorkLedgerEntry entry;
        entry.runId = runId;
        entry.projectId = projectIdOverride.value_or(kFixtureProjectId);
        entry.terminalStatus = terminalStatus;
        entry.baselineFingerprint = baselineFingerprintOverride.value_or(kFixtureBaselineFingerprint);
        entry.disposition = dispositionOverride.value_or(
            verification.has_value()
                ? ManifestWorkLedgerDisposition::ManifestReady
                : ManifestWorkLedgerDisposition::NotProduced);
        entry.taskDispositions = taskDispositions.has_value()
            ? std::move(*taskDispositions)
            : (verification.has_value()
                  ? makeTaskDispositions(verification->orderedProposalIds)
                  : std::vector<ManifestWorkLedgerTaskDisposition> {});
        entry.verification = std::move(verification);
        entry.createdAt = std::chrono::system_clock::now();
        req(ledger.insertEntry(entry).success(), "Could not seed Work Ledger entry");
    }

    // Creates a real ReadyForReview proposal (real stored content, via the
    // full ChangeProposalService pipeline) for runId, owning path
    // relativePath/content. Returns the changeSetId.
    ChangeSetId createReadyProposal(
        ChangeProposalService& proposals, const RunId& runId,
        const std::string& relativePath, const std::string& content)
    {
        CreateChangeProposalRequest request;
        request.runId = runId;
        request.projectId = kFixtureProjectId;
        request.providerId = "prov1";
        request.modelId = "m1";
        request.agentRole = "test";
        request.rawModelOutput = makeCreateProposal(relativePath, content);
        request.policy = defaultPolicy();
        const auto created = proposals.createFromModelOutput(std::move(request));
        req(created.success(), "createFromModelOutput should succeed");
        req(created.value().state == ChangeSetState::ReadyForReview,
            "test fixture proposal must reach ReadyForReview");
        return created.value().changeSetId;
    }
};

// ---------------------------------------------------------------------
// Evaluator tests (pure ManifestApplyVerificationGate::evaluate(), no
// ChangeSetApplyService involved).
// ---------------------------------------------------------------------

void testExactVerifiedSetAllPassed_Allows()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto changes = proposals.listChanges(csId);
    req(changes.success(), "listChanges failed");
    const auto hash = testComputeContentHash(csId, changes.value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(eligibility.allowed, "exact verified set with all statuses passed must be allowed");
    req(eligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "wrong allow reasonCode");
}

// QML-lint advisory-severity policy (2026-09-28): AG-16 is untouched code -
// it only ever reads the aggregate ExecutionVerificationStatus/
// orderedProposalIds fields, never a diagnostic code. This proves that
// directly: a Passed verification carrying a durable
// verification.qml_warning_advisory diagnostic is allowed exactly like any
// other Passed verification.
void testAdvisoryOnlyPassedVerification_ExactMemberAllowed()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "qml/AlertsView.qml", "Item {}");
    const auto changes = proposals.listChanges(csId);
    req(changes.success(), "listChanges failed");
    const auto hash = testComputeContentHash(csId, changes.value(), proposals);
    auto verification = makePassingVerification({csId}, {hash});
    ManifestVerificationDiagnostic advisory;
    advisory.code = "verification.qml_warning_advisory";
    advisory.message =
        "QML lint emitted one or more advisory-severity warnings that do not block this check";
    advisory.requirementId = "lint";
    advisory.structuredFindings.push_back(
        {"<candidate>/qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access"});
    verification.diagnostics = {advisory};
    fx.seedLedgerEntry("r1", verification);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(eligibility.allowed,
        "a Passed verification carrying only a durable advisory diagnostic must be allowed");
    req(eligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "wrong allow reasonCode");
}

// The advisory diagnostic's mere presence is never itself an authority
// signal - AG-16 still requires exact proposal-set membership regardless of
// what a Passed verification's diagnostics contain.
void testAdvisoryDiagnosticIsNotAuthority_ProposalOutsideVerifiedSetStillDenied()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "qml/AlertsView.qml", "Item {}");
    const auto changes = proposals.listChanges(csId);
    req(changes.success(), "listChanges failed");
    const auto hash = testComputeContentHash(csId, changes.value(), proposals);
    auto verification = makePassingVerification({csId}, {hash});
    ManifestVerificationDiagnostic advisory;
    advisory.code = "verification.qml_warning_advisory";
    advisory.message = "advisory";
    advisory.requirementId = "lint";
    advisory.structuredFindings.push_back(
        {"<candidate>/qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access"});
    verification.diagnostics = {advisory};
    fx.seedLedgerEntry("r1", verification);

    const auto foreignCsId = fx.createReadyProposal(proposals, "r1", "qml/Other.qml", "Item {}");
    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(foreignCsId);
    req(!eligibility.allowed,
        "a proposal outside the verified set must be denied regardless of advisory diagnostics "
        "on the verification that DID pass");
    req(eligibility.reasonCode == "manifest_apply.proposal_not_in_verified_set", "wrong reasonCode");
}

// The legacy blocking code must remain fully denied - advisory-severity
// hardening never reclassifies an existing verification.qml_warning failure.
void testLegacyQmlWarningFailedVerification_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "qml/App.qml", "Item {}");
    const auto changes = proposals.listChanges(csId);
    req(changes.success(), "listChanges failed");
    const auto hash = testComputeContentHash(csId, changes.value(), proposals);
    auto verification = makePassingVerification({csId}, {hash});
    verification.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Failed;
    ManifestVerificationDiagnostic legacy;
    legacy.code = "verification.qml_warning";
    legacy.message = "QML lint reported a warning";
    legacy.requirementId = "lint";
    legacy.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    verification.diagnostics = {legacy};
    fx.seedLedgerEntry("r1", verification);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed,
        "a legacy Failed verification.qml_warning result must remain denied unchanged");
    req(eligibility.reasonCode == "manifest_apply.verification_not_passed", "wrong reasonCode");
}

void testVerificationMissing_NoLedgerEntryAtAll_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    // No seedLedgerEntry call at all - the Run never got a Work Ledger entry.

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "missing ledger entry must deny");
    req(eligibility.reasonCode == "manifest_apply.verification_missing", "wrong reasonCode");
}

// AG-16.1 requirement 7: a legacy Manifest Run whose ledger entry exists but
// never recorded a typed verification (e.g. a Planner no-op Run, or a Run
// that predates AG-16) must be safely incompatible with ordinary Apply.
void testLegacyManifestRunWithLedgerEntryButNoVerification_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    fx.seedLedgerEntry("r1", std::nullopt);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a ledger entry without a verification must deny");
    req(eligibility.reasonCode == "manifest_apply.verification_missing", "wrong reasonCode");
}

// Structural corruption: unequal orderedProposalIds/orderedProposalContentHashes
// cardinality is what validateManifestProposalSetVerification itself
// rejects - the exact shape a corrupted or malformed durable record would
// have.
void testStructurallyInvalidVerification_FailsClosed()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    auto verification = makePassingVerification({csId}, {}); // hashes[] shorter than ids[]
    fx.seedLedgerEntry("r1", verification);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "structurally invalid verification must deny");
    req(eligibility.reasonCode == "manifest_apply.verification_corrupt", "wrong reasonCode");
}

void testRequiredStatusNotPassed_Denies()
{
    const ExecutionVerificationStatus badStatuses[] = {
        ExecutionVerificationStatus::Failed,
        ExecutionVerificationStatus::NotRun,
        ExecutionVerificationStatus::NotVerifiable,
    };
    for (const auto bad : badStatuses) {
        Fixture fx;
        auto proposals = fx.makeProposalService();
        fx.seedRun("r1", kExecutionManifestWorkflowId);
        const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
        const auto changes = proposals.listChanges(csId);
        const auto hash = testComputeContentHash(csId, changes.value(), proposals);
        auto verification = makePassingVerification({csId}, {hash});
        verification.acceptanceStatus = bad;
        fx.seedLedgerEntry("r1", verification);

        auto gate = fx.makeGate(proposals);
        const auto eligibility = gate.evaluate(csId);
        req(!eligibility.allowed, "a not-passed required status must deny");
        req(eligibility.reasonCode == "manifest_apply.verification_not_passed", "wrong reasonCode");
    }
}

void testProposalNotInVerifiedSet_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    // Verification (and the ledger's own matching task disposition) names a
    // different proposal id entirely - csId itself was never verified.
    fx.seedLedgerEntry("r1", makePassingVerification({"cs-other"}, {std::string(64, 'a')}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a proposal absent from the verified set must deny");
    req(eligibility.reasonCode == "manifest_apply.proposal_not_in_verified_set", "wrong reasonCode");
}

void testContentHashChangedAfterVerification_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    // A verification record whose stored hash does not match this proposal's
    // real durable content - the same observable shape as content changing
    // (or the record being tampered with) after verification ran.
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {std::string(64, 'f')}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a content hash mismatch on the target proposal must deny");
    req(eligibility.reasonCode == "manifest_apply.content_hash_mismatch", "wrong reasonCode");
}

void testProposalSetDiverged_SiblingMissing_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto changes = proposals.listChanges(csId);
    const auto hash = testComputeContentHash(csId, changes.value(), proposals);
    // The verified combined set (and the ledger's own task dispositions,
    // consistently) names a sibling proposal that never existed (or has
    // since been removed) - the target itself is otherwise exactly as
    // verified.
    fx.seedLedgerEntry("r1",
        makePassingVerification({csId, "cs-missing-sibling"}, {hash, std::string(64, 'b')}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a diverged (missing) sibling must deny the whole set");
    req(eligibility.reasonCode == "manifest_apply.proposal_set_diverged", "wrong reasonCode");
}

void testProposalSetDiverged_SiblingRejectedAfterVerification_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto siblingId = fx.createReadyProposal(proposals, "r1", "other.md", "world");
    const auto csChanges = proposals.listChanges(csId);
    const auto siblingChanges = proposals.listChanges(siblingId);
    const auto csHash = testComputeContentHash(csId, csChanges.value(), proposals);
    const auto siblingHash = testComputeContentHash(siblingId, siblingChanges.value(), proposals);
    fx.seedLedgerEntry("r1",
        makePassingVerification({csId, siblingId}, {csHash, siblingHash}));

    // The sibling was rejected (e.g. by a human reviewer) after the combined
    // verification ran - the combined set has diverged even though the
    // target proposal itself is untouched, and the ledger's own durably-
    // recorded task dispositions (fixed at seed time) still name both.
    req(fx.repo.updateChangeSetState(siblingId, ChangeSetState::Rejected).success(),
        "could not transition sibling to Rejected");

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a diverged (rejected) sibling must deny the whole set");
    req(eligibility.reasonCode == "manifest_apply.proposal_set_diverged", "wrong reasonCode");
}

// A sibling that has already been Applied (a normal one-at-a-time apply
// sequence over the same combined-verified set) must NOT count as diverged.
void testProposalSetWithAppliedSibling_StillAllowed()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto siblingId = fx.createReadyProposal(proposals, "r1", "other.md", "world");
    const auto csChanges = proposals.listChanges(csId);
    const auto siblingChanges = proposals.listChanges(siblingId);
    const auto csHash = testComputeContentHash(csId, csChanges.value(), proposals);
    const auto siblingHash = testComputeContentHash(siblingId, siblingChanges.value(), proposals);
    fx.seedLedgerEntry("r1",
        makePassingVerification({csId, siblingId}, {csHash, siblingHash}));
    req(fx.repo.updateChangeSetState(siblingId, ChangeSetState::Applied).success(),
        "could not transition sibling to Applied");

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(eligibility.allowed, "an already-Applied sibling from the same verified set must not block");
}

// The TARGET proposal itself, in contrast, must be strictly ReadyForReview -
// an already-Applied target cannot be "re-applied" through the gate.
void testAppliedTarget_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}));
    req(fx.repo.updateChangeSetState(csId, ChangeSetState::Applied).success(),
        "could not transition target to Applied");

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "an already-Applied target must never be re-authorized");
    req(eligibility.reasonCode == "proposal.not_ready", "wrong reasonCode");
}

void testForeignRunVerificationNeverUsed()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    fx.seedRun("r2", kExecutionManifestWorkflowId);
    const auto csA = fx.createReadyProposal(proposals, "r1", "a.md", "content-a");
    const auto csB = fx.createReadyProposal(proposals, "r2", "b.md", "content-b");
    const auto hashB = testComputeContentHash(csB, proposals.listChanges(csB).value(), proposals);
    // Run r1's own verification never mentions csA at all - only Run r2's
    // verification (for a different Run) does. If cross-Run lookups ever
    // leaked, csA would incorrectly appear eligible.
    fx.seedLedgerEntry("r1", makePassingVerification({"cs-unrelated"}, {std::string(64, 'c')}));
    fx.seedLedgerEntry("r2", makePassingVerification({csB}, {hashB}));

    auto gate = fx.makeGate(proposals);
    const auto resultA = gate.evaluate(csA);
    req(!resultA.allowed, "Run r1's proposal must never be authorized by Run r2's verification");
    req(resultA.reasonCode == "manifest_apply.proposal_not_in_verified_set", "wrong reasonCode");
    const auto resultB = gate.evaluate(csB);
    req(resultB.allowed, "Run r2's own correctly-verified proposal must remain allowed");
}

// A superseded/rejected proposal was never part of any verified combined
// set in the first place (ManifestProposalOverlay::build() only ever admits
// ReadyForReview proposals) - directly evaluating it must still deny, even
// though the specific reasonCode is the same membership check other tests
// exercise (there is no separate "lineage" code needed - see the
// evaluate() doc comment).
void testSupersededProposalCannotUseFinalVerification()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto finalId = fx.createReadyProposal(proposals, "r1", "notes.md", "final content");
    const auto finalHash = testComputeContentHash(finalId, proposals.listChanges(finalId).value(), proposals);

    ProposedChangeSet supersededRaw;
    supersededRaw.changeSetId = "cs-superseded";
    supersededRaw.runId = "r1";
    supersededRaw.projectId = kFixtureProjectId;
    supersededRaw.state = ChangeSetState::Rejected;
    fx.repo.insertRaw(supersededRaw, {});
    fx.seedLedgerEntry("r1", makePassingVerification({finalId}, {finalHash}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate("cs-superseded");
    req(!eligibility.allowed, "a superseded/rejected proposal must never gain apply eligibility");
    req(eligibility.reasonCode == "manifest_apply.proposal_not_in_verified_set", "wrong reasonCode");
}

// ---------------------------------------------------------------------
// Exact-final-proposal-set hardening tests: verification.orderedProposalIds
// must exactly match (cardinality + membership, no duplicates) the
// changeSetIds this Run's own Work Ledger task dispositions recorded.
// ---------------------------------------------------------------------

void testVerifiedSetHasExtraProposalNotInTaskDispositions_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    // Verification claims TWO proposals, but the ledger's own task
    // dispositions only ever recorded one - "extra-id" is not a real final
    // proposal of any task.
    fx.seedLedgerEntry("r1",
        makePassingVerification({csId, "extra-id"}, {hash, std::string(64, 'd')}),
        makeTaskDispositions({csId}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "an extra verified proposal absent from task dispositions must deny");
    req(eligibility.reasonCode == "manifest_apply.verified_set_mismatch", "wrong reasonCode");
}

void testTaskDispositionsHaveProposalMissingFromVerifiedSet_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    // The ledger's own task dispositions recorded TWO final proposals, but
    // verification only ever covered one - "missing-id" was never verified.
    fx.seedLedgerEntry("r1",
        makePassingVerification({csId}, {hash}),
        makeTaskDispositions({csId, "missing-id"}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a task-disposition proposal missing from the verified set must deny");
    req(eligibility.reasonCode == "manifest_apply.verified_set_mismatch", "wrong reasonCode");
}

void testVerifiedSetContainsDuplicateId_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1",
        makePassingVerification({csId, csId}, {hash, hash}),
        makeTaskDispositions({csId}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a duplicate id within the verified set must deny");
    req(eligibility.reasonCode == "manifest_apply.verified_set_invalid", "wrong reasonCode");
}

// ---------------------------------------------------------------------
// Authoritative-identity hardening tests: the verification must be bound to
// exactly this Run/project/baseline, and the Run/ledger entry itself must
// have actually completed a Manifest execution.
// ---------------------------------------------------------------------

void testProposalProjectIdentityMismatch_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    ProposedChangeSet foreignProjectProposal;
    foreignProjectProposal.changeSetId = "cs-foreign-project";
    foreignProjectProposal.runId = "r1";
    foreignProjectProposal.projectId = "some-other-project";
    foreignProjectProposal.state = ChangeSetState::ReadyForReview;
    fx.repo.insertRaw(foreignProjectProposal, {});
    fx.seedLedgerEntry("r1",
        makePassingVerification({"cs-foreign-project"}, {std::string(64, 'e')}));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate("cs-foreign-project");
    req(!eligibility.allowed, "a proposal whose projectId disagrees with its Run must deny");
    req(eligibility.reasonCode == "proposal.project_mismatch", "wrong reasonCode");
}

void testLedgerEntryProjectIdentityMismatch_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}),
        makeTaskDispositions({csId}), RunStatus::Completed,
        /*dispositionOverride=*/std::nullopt,
        /*projectIdOverride=*/std::string("a-different-project"));

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a ledger entry whose own projectId disagrees with the Run must deny");
    req(eligibility.reasonCode == "manifest_apply.verification_identity_mismatch", "wrong reasonCode");
}

void testVerificationWorkspaceIdentityMismatch_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    auto verification = makePassingVerification({csId}, {hash});
    verification.workspaceIdentity = "some-other-workspace";
    fx.seedLedgerEntry("r1", verification);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a verification whose workspaceIdentity disagrees with the Run must deny");
    req(eligibility.reasonCode == "manifest_apply.verification_identity_mismatch", "wrong reasonCode");
}

void testVerificationBaselineFingerprintMismatch_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    auto verification = makePassingVerification({csId}, {hash});
    verification.baselineFingerprint = "a-different-baseline";
    fx.seedLedgerEntry("r1", verification);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a verification whose baseline disagrees with the ledger entry must deny");
    req(eligibility.reasonCode == "manifest_apply.verification_identity_mismatch", "wrong reasonCode");
}

void testRunNotCompleted_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}),
        makeTaskDispositions({csId}), RunStatus::Failed);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a non-Completed Work Ledger entry must deny");
    req(eligibility.reasonCode == "manifest_apply.run_not_completed", "wrong reasonCode");
}

void testLedgerEntryNotManifestReadyDisposition_Denies()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    // An internally inconsistent row - a verification is present, but the
    // disposition itself claims this was not a Manifest-ready Run. Real
    // production code never writes this combination (see
    // ManifestWorkLedgerService::recordTerminalRun's own invariant); this
    // proves the gate does not simply infer ManifestReady from
    // verification.has_value() alone.
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}),
        makeTaskDispositions({csId}), RunStatus::Completed,
        /*dispositionOverride=*/ManifestWorkLedgerDisposition::NoChangesNeeded);

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a non-ManifestReady disposition must deny even with a verification present");
    req(eligibility.reasonCode == "manifest_apply.verification_missing", "wrong reasonCode");
}

// ---------------------------------------------------------------------
// Scope-decision tests (workflowId gating - explicitly confirmed with the
// user before implementation): the gate only enforces AG-16 for
// kExecutionManifestWorkflowId Runs.
// ---------------------------------------------------------------------

void testLegacyExecutionProfileRun_GateNotApplicableAndApplyUnaffected()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionProfileWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    // Deliberately no Work Ledger entry at all - this workflow never
    // produces one, and none should ever be required for it.

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(eligibility.allowed, "a legacy execution-profile Run must be unaffected by AG-16.1");
    req(eligibility.reasonCode == ManifestApplyVerificationGate::kGateNotApplicableCode,
        "wrong allow reasonCode for a non-Manifest Run");

    auto applyService = fx.makeApplyService(&gate);
    const auto approved = applyService.approve(csId);
    req(approved.success(), "approve() must behave exactly as before AG-16.1 for this workflow");
    const auto applied = applyService.apply(csId);
    req(applied.success(), "apply() must behave exactly as before AG-16.1 for this workflow");
    req(fs::exists(fx.projectDir.path / "notes.md"), "apply() must have written the file");
}

void testMockWorkflowRun_GateNotApplicableAndApplyUnaffected()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kMockWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate(csId);
    req(eligibility.allowed, "a Mock-workflow Run must be unaffected by AG-16.1");
    req(eligibility.reasonCode == ManifestApplyVerificationGate::kGateNotApplicableCode,
        "wrong allow reasonCode for a non-Manifest Run");
}

void testMissingOwningRun_DeniesNotGateNotApplicable()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    // No seedRun call at all for "missing-run".
    ProposedChangeSet rawCs;
    rawCs.changeSetId = "cs-orphan";
    rawCs.runId = "missing-run";
    rawCs.projectId = kFixtureProjectId;
    rawCs.state = ChangeSetState::ReadyForReview;
    fx.repo.insertRaw(rawCs, {});

    auto gate = fx.makeGate(proposals);
    const auto eligibility = gate.evaluate("cs-orphan");
    req(!eligibility.allowed, "a missing owning Run must fail closed");
    req(eligibility.reasonCode == "proposal.run_not_found",
        "a missing Run must never be reported as gate_not_applicable");
}

void testRunRepositoryReadFailure_DeniesNotGateNotApplicable()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");

    FailingRunRepository failingRuns;
    Run run;
    run.id = "r1";
    run.projectId = kFixtureProjectId;
    run.workflowId = kExecutionManifestWorkflowId;
    req(failingRuns.insertRun(run).success(), "could not seed failing-repo Run");
    failingRuns.failFindRun = true;

    ManifestApplyVerificationGate gate(proposals, fx.ledger, failingRuns);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed, "a Run repository read failure must fail closed");
    req(eligibility.reasonCode == "storage.unavailable",
        "a repository read failure must never be reported as gate_not_applicable");
}

// A Manifest-workflow Run must never receive kGateNotApplicableCode,
// regardless of why it is denied or allowed - that code is reserved
// exclusively for a genuinely different workflow.
void testManifestRunNeverGetsGateNotApplicable()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    auto gate = fx.makeGate(proposals);

    // Missing verification.
    req(gate.evaluate(csId).reasonCode != ManifestApplyVerificationGate::kGateNotApplicableCode,
        "a Manifest Run must never receive gate_not_applicable (missing verification case)");

    // Passed verification.
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}));
    const auto passed = gate.evaluate(csId);
    req(passed.allowed && passed.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "expected the passing-verification case to be allowed with kEligibleCode");
    req(passed.reasonCode != ManifestApplyVerificationGate::kGateNotApplicableCode,
        "a Manifest Run must never receive gate_not_applicable (passing case)");
}

// AG-18.3 final hardening: the production gate must derive whether the Run
// declared a bootstrap from the immutable Manifest, not from the mere
// presence of a lifecycle row. Losing that durable row must therefore fail
// closed instead of making a bootstrap Run look like an ordinary one.
void testBootstrapManifestWithoutDurableDecisionDeniesApply()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}));

    InMemoryExecutionManifestRepository manifests;
    ExecutionManifest manifest;
    manifest.runId = "r1";
    manifest.bootstrapDeclaration = ManifestBootstrapDeclaration {
        "bootstrap", {fs::path("CMakeUserPresets.json")}};
    req(manifests.insert(std::move(manifest)).success(), "Could not seed bootstrap Manifest");
    InMemoryVerificationBootstrapRepository bootstrap;

    ManifestApplyVerificationGate gate(
        proposals, fx.ledger, fx.runs, &bootstrap, &manifests);
    const auto eligibility = gate.evaluate(csId);
    req(!eligibility.allowed,
        "A bootstrap Manifest without its durable lifecycle decision must fail closed");
    req(eligibility.reasonCode == "manifest_apply.bootstrap_not_verified",
        "A missing bootstrap decision produced the wrong Apply denial code");

    VerificationBootstrapDecision decision;
    decision.runId = "r1";
    decision.projectId = kFixtureProjectId;
    decision.baselineFingerprint = kFixtureBaselineFingerprint;
    decision.disposition = VerificationBootstrapDisposition::BootstrapPending;
    decision.reasonCode = "verification_environment.bootstrap_pending";
    decision.bootstrapSetupPaths = {fs::path("CMakeUserPresets.json")};
    decision.owningTaskIds = {"bootstrap"};
    decision.decidedAt = std::chrono::system_clock::now();
    req(bootstrap.recordDecision(decision).success(),
        "Could not seed the durable BootstrapPending decision");

    VerificationBootstrapTransition transition;
    transition.id = "r1:bootstrap_verified";
    transition.runId = "r1";
    transition.toDisposition = VerificationBootstrapDisposition::BootstrapVerified;
    transition.reasonCode = "verification_environment.bootstrap_verified";
    transition.candidateIdentity = VerificationBootstrapCandidateIdentity {
        "overlay-1", "candidate-profile", "candidate-build", {}};
    transition.transitionedAt = std::chrono::system_clock::now();
    req(bootstrap.appendTransition(transition).success(),
        "Could not seed the durable BootstrapVerified transition");

    const auto verifiedEligibility = gate.evaluate(csId);
    req(verifiedEligibility.allowed
            && verifiedEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "A bootstrap Manifest with an exact durable BootstrapVerified lifecycle was denied");
}

// ---------------------------------------------------------------------
// ChangeSetApplyService integration: proves the gate is actually wired
// into approve()/apply(), re-checked on every call (no TOCTOU window), and
// that a denial never mutates the workspace or any durable state.
// ---------------------------------------------------------------------

void testApproveFailsClosedEarlyForIneligibleProposal()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    // No verification at all - ineligible from the start.

    auto gate = fx.makeGate(proposals);
    auto applyService = fx.makeApplyService(&gate);
    const auto approved = applyService.approve(csId);
    req(!approved.success(), "approve() must fail closed for an ineligible proposal");
    req(approved.error()->code == "manifest_apply.verification_missing", "wrong reasonCode");
    req(!fs::exists(fx.projectDir.path / "notes.md"), "a denied approve() must never touch the workspace");
}

void testApplyRevalidatesGateEvenAfterApprove_NoToctou()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto siblingId = fx.createReadyProposal(proposals, "r1", "other.md", "world");
    const auto csHash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    const auto siblingHash =
        testComputeContentHash(siblingId, proposals.listChanges(siblingId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId, siblingId}, {csHash, siblingHash}));

    auto gate = fx.makeGate(proposals);
    auto applyService = fx.makeApplyService(&gate);

    // approve() succeeds while the combined verified set is still intact.
    const auto approved = applyService.approve(csId);
    req(approved.success(), "approve() should succeed while the verified set is intact");

    // The combined set diverges AFTER approve() but BEFORE apply() - the
    // sibling proposal (part of the same verified combined set) is rejected.
    req(fx.repo.updateChangeSetState(siblingId, ChangeSetState::Rejected).success(),
        "could not diverge the sibling proposal");

    // apply() must re-derive eligibility itself rather than trust the
    // earlier approve() decision - this is the TOCTOU window AG-16 exists
    // to close.
    const auto applied = applyService.apply(csId);
    req(!applied.success(), "apply() must re-check the gate and deny after the set diverged");
    req(applied.error()->code == "manifest_apply.proposal_set_diverged", "wrong reasonCode");
    req(!fs::exists(fx.projectDir.path / "notes.md"),
        "a denied apply() must never touch the workspace, even after a successful approve()");

    // The proposal's own state must remain ReadyForReview - never silently
    // promoted to Applied.
    const auto stillReady = proposals.findChangeSet(csId);
    req(stillReady.success() && stillReady.value().state == ChangeSetState::ReadyForReview,
        "a denied apply() must never transition the proposal to Applied");
}

void testDeniedApplyRecordsNoApprovalSuccessButLeavesNoWorkspaceChange()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    // Verification (and matching task disposition) names a completely
    // different proposal - csId is not a member of the verified set at all.
    fx.seedLedgerEntry("r1", makePassingVerification({"cs-other"}, {std::string(64, 'a')}));

    auto gate = fx.makeGate(proposals);
    auto applyService = fx.makeApplyService(&gate);
    const auto approved = applyService.approve(csId);
    req(!approved.success(), "approve() must deny a proposal outside the verified set");
    req(approved.error()->code == "manifest_apply.proposal_not_in_verified_set", "wrong reasonCode");

    // approve() denial must not have persisted an approval - a subsequent
    // apply() must fail on the pre-existing "no approval" gate, never reach
    // (or need) the verification gate a second time to prove this.
    const auto applied = applyService.apply(csId);
    req(!applied.success(), "apply() without any approval record must fail");
    req(applied.error()->code == "proposal.no_approval", "wrong reasonCode");
    req(!fs::exists(fx.projectDir.path / "notes.md"), "no file must ever be written");
}

// Positive end-to-end control: a fully eligible proposal still approves and
// applies successfully with the gate wired in - the gate does not add
// friction for a genuinely passing case.
void testEligibleProposalStillAppliesSuccessfullyWithGateWired()
{
    Fixture fx;
    auto proposals = fx.makeProposalService();
    fx.seedRun("r1", kExecutionManifestWorkflowId);
    const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
    const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
    fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}));

    auto gate = fx.makeGate(proposals);
    auto applyService = fx.makeApplyService(&gate);
    req(applyService.approve(csId).success(), "approve() should succeed for an eligible proposal");
    const auto applied = applyService.apply(csId);
    req(applied.success(), "apply() should succeed for an eligible proposal");
    req(fs::exists(fx.projectDir.path / "notes.md"), "apply() must have written the file");

    const auto finalState = proposals.findChangeSet(csId);
    req(finalState.success() && finalState.value().state == ChangeSetState::Applied,
        "a successful apply() must transition the proposal to Applied");
}

// The read-only evaluator and ChangeSetApplyService's actual approve()/
// apply() decisions must never disagree - proves there is exactly one
// implementation of this logic, not two independently-maintained copies.
void testReadOnlyEvaluatorMatchesApproveApplyDecisions()
{
    {
        Fixture fx;
        auto proposals = fx.makeProposalService();
        fx.seedRun("r1", kExecutionManifestWorkflowId);
        const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
        const auto hash = testComputeContentHash(csId, proposals.listChanges(csId).value(), proposals);
        fx.seedLedgerEntry("r1", makePassingVerification({csId}, {hash}));
        auto gate = fx.makeGate(proposals);
        const auto eligibility = gate.evaluate(csId);
        auto applyService = fx.makeApplyService(&gate);
        const auto approved = applyService.approve(csId);
        req(eligibility.allowed == approved.success(),
            "evaluate() and approve() disagreed on the allowed case");
    }
    {
        Fixture fx;
        auto proposals = fx.makeProposalService();
        fx.seedRun("r1", kExecutionManifestWorkflowId);
        const auto csId = fx.createReadyProposal(proposals, "r1", "notes.md", "hello");
        // No verification - denied.
        auto gate = fx.makeGate(proposals);
        const auto eligibility = gate.evaluate(csId);
        auto applyService = fx.makeApplyService(&gate);
        const auto approved = applyService.approve(csId);
        req(!eligibility.allowed && !approved.success(),
            "evaluate() and approve() disagreed on the denied case");
        req(approved.error()->code == eligibility.reasonCode,
            "approve()'s error code must be exactly evaluate()'s reasonCode");
    }
}

} // namespace

int runManifestApplyVerificationGateTests()
{
    testExactVerifiedSetAllPassed_Allows();
    testAdvisoryOnlyPassedVerification_ExactMemberAllowed();
    testAdvisoryDiagnosticIsNotAuthority_ProposalOutsideVerifiedSetStillDenied();
    testLegacyQmlWarningFailedVerification_Denies();
    testVerificationMissing_NoLedgerEntryAtAll_Denies();
    testLegacyManifestRunWithLedgerEntryButNoVerification_Denies();
    testStructurallyInvalidVerification_FailsClosed();
    testRequiredStatusNotPassed_Denies();
    testProposalNotInVerifiedSet_Denies();
    testContentHashChangedAfterVerification_Denies();
    testProposalSetDiverged_SiblingMissing_Denies();
    testProposalSetDiverged_SiblingRejectedAfterVerification_Denies();
    testProposalSetWithAppliedSibling_StillAllowed();
    testAppliedTarget_Denies();
    testForeignRunVerificationNeverUsed();
    testSupersededProposalCannotUseFinalVerification();
    testVerifiedSetHasExtraProposalNotInTaskDispositions_Denies();
    testTaskDispositionsHaveProposalMissingFromVerifiedSet_Denies();
    testVerifiedSetContainsDuplicateId_Denies();
    testProposalProjectIdentityMismatch_Denies();
    testLedgerEntryProjectIdentityMismatch_Denies();
    testVerificationWorkspaceIdentityMismatch_Denies();
    testVerificationBaselineFingerprintMismatch_Denies();
    testRunNotCompleted_Denies();
    testLedgerEntryNotManifestReadyDisposition_Denies();
    testLegacyExecutionProfileRun_GateNotApplicableAndApplyUnaffected();
    testMockWorkflowRun_GateNotApplicableAndApplyUnaffected();
    testMissingOwningRun_DeniesNotGateNotApplicable();
    testRunRepositoryReadFailure_DeniesNotGateNotApplicable();
    testManifestRunNeverGetsGateNotApplicable();
    testBootstrapManifestWithoutDurableDecisionDeniesApply();
    testApproveFailsClosedEarlyForIneligibleProposal();
    testApplyRevalidatesGateEvenAfterApprove_NoToctou();
    testDeniedApplyRecordsNoApprovalSuccessButLeavesNoWorkspaceChange();
    testEligibleProposalStillAppliesSuccessfullyWithGateWired();
    testReadOnlyEvaluatorMatchesApproveApplyDecisions();
    return 0;
}
