// Unit tests for ManifestVerificationContinuationService (MW-P5.1): the
// pure fail-closed eligibility/admission logic, entirely in-memory - no
// SQLite, no provider sessions. Repository round-trip coverage lives in
// ManifestVerificationContinuationRepositoryTests.cpp.
#include "ai/application/ManifestVerificationContinuationService.hpp"
#include "ai/application/ManifestVerificationAttributionService.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationLegacyBaselineNormalizationService.hpp"
#include "ai/application/InMemoryManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/ManifestVerificationContinuationReadModelService.hpp"
#include "ai/application/ManifestVerificationContinuationRepairService.hpp"
#include "ai/application/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/ManifestVerificationManualHandoffService.hpp"
#include "ai/application/InMemoryManifestVerificationManualHandoffRepository.hpp"
#include "RecordingOutcomeBriefTrigger.hpp"

#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/infrastructure/FileArtifactStore.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryManifestTaskExecutionRepository.hpp"
#include "ai/application/InMemoryManifestVerificationContinuationRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryRunWorkspaceScopeRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestApplyVerificationGate.hpp"
#include "ai/application/ManifestProposalOverlay.hpp"
#include "ai/application/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/ManifestWorkLedgerService.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stop_token>
#include <stdexcept>
#include <string>

namespace {

using namespace ai::application;
using namespace ai::infrastructure;
namespace common = ai::common;
namespace fs = std::filesystem;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

struct TempDir {
    fs::path path;

    TempDir()
    {
        // Kept short (see "Short worktree names" lesson): the real test
        // below nests two Run ids' worth of UUIDs under
        // .proposals/runs/<id>/proposals/<id>/proposed/<sha256> - a long
        // prefix here pushes that path past Windows' MAX_PATH, making
        // fs::file_size/fs::exists silently fail on a file
        // recursive_directory_iterator still lists.
        path = fs::temp_directory_path()
            / ("aio-vc-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Minimal in-memory IChangeProposalRepository fake, same shape as the one
// ManifestWorkLedgerServiceTests.cpp already uses for this exact class of
// service test - only insertChangeSet/findChangeSet/updateChangeSetState
// are exercised here.
class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
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
    common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message) override
    {
        std::lock_guard lock(mu_);
        // Test-injectable failure point (QML-P4R.3 hardening): lets a
        // reconciliation test prove a repository-level markMissingEvidence
        // failure is surfaced, never silently swallowed.
        if (failMarkMissingEvidenceFor_.contains(id)) {
            return common::Result<void>::failure(
                {"proposal.mark_missing_evidence_failed", "Injected test failure"});
        }
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "not found"});
        it->second.missingEvidenceCode = std::move(code);
        it->second.missingEvidenceMessage = std::move(message);
        return common::Result<void>::ok();
    }
    void failMarkMissingEvidenceFor(const ChangeSetId& id)
    {
        std::lock_guard lock(mu_);
        failMarkMissingEvidenceFor_.insert(id);
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(it->second);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> result;
        for (const auto& [id, cs] : byId_) {
            if (cs.runId == runId) result.push_back(cs);
        }
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId& id) const override
    {
        // Must actually reflect what was stored - ChangeProposalService::
        // materializeRepairSuccessor reads a predecessor's changes through
        // this exact repository method (never ProposedChangeSet::changes
        // directly), so a permanent empty stub here would silently break
        // every real repair-materialization test in this file.
        std::lock_guard lock(mu_);
        // Test-injectable failure point (QML-P4R.3.4): lets a test force a
        // genuine transient/tool/infrastructure failure inside
        // ManifestVerificationManualHandoffService::importAndReverify -
        // this repository call is on its critical path both while grouping
        // edited paths by owning proposal and inside
        // ManifestProposalSetVerifier::verify's own combined safety check -
        // without needing a real filesystem/process failure to reproduce it.
        if (failListChangesFor_.contains(id)) {
            return common::Result<std::vector<ProposedFileChange>>::failure(
                {"proposal.list_changes_failed", "Injected test failure"});
        }
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second.changes);
    }
    void failListChangesFor(const ChangeSetId& id)
    {
        std::lock_guard lock(mu_);
        failListChangesFor_.insert(id);
    }
    void clearListChangesFailures()
    {
        std::lock_guard lock(mu_);
        failListChangesFor_.clear();
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId&) const override
    {
        return common::Result<std::vector<ValidationFinding>>::ok({});
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>&) const override
    {
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>::ok({});
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>&) const override
    {
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>::ok({});
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
    std::set<ChangeSetId> failMarkMissingEvidenceFor_;
    std::set<ChangeSetId> failListChangesFor_;
};

class UnusedProposalStore final : public IChangeProposalStore {
public:
    common::Result<PublishedProposalLocation> publishChangeSet(
        const ProposedChangeSet&, const ProjectWritePolicy&) override
    { return common::Result<PublishedProposalLocation>::failure({"test.unused", "unused"}); }
    common::Result<std::string> readOriginalContent(const std::string&, const ChangeId&) const override
    { return common::Result<std::string>::failure({"test.unused", "unused"}); }
    common::Result<std::string> readProposedContent(const std::string&, const ChangeId&) const override
    { return common::Result<std::string>::failure({"test.unused", "unused"}); }
};

class UnusedVerificationRunner final : public IManifestVerificationCommandRunner {
public:
    std::size_t calls {0};
    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest&, std::stop_token) override
    { ++calls; return {}; }
};

// A scripted runner for prepare()'s real baseline+candidate verification
// calls: same shape as ExecutionManifestExecutorTests.cpp's own
// FakeVerificationCommandRunner - pops queuedOutcomes in call order, falling
// back to a clean "started, exit 0, no output" outcome once exhausted so an
// unanticipated extra call never crashes the test, only ever surfaces as a
// wrong assertion.
class ScriptedVerificationCommandRunner final : public IManifestVerificationCommandRunner {
public:
    std::deque<ManifestVerificationCommandOutcome> queuedOutcomes;
    std::size_t calls {0};
    // Where every command was asked to run (the verification copy it was started in).
    std::vector<fs::path> workingDirectories;
    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        ++calls;
        workingDirectories.push_back(request.workingDirectory);
        if (!queuedOutcomes.empty()) {
            auto next = queuedOutcomes.front();
            queuedOutcomes.pop_front();
            return next;
        }
        ManifestVerificationCommandOutcome clean;
        clean.started = true;
        clean.exitCode = 0;
        clean.boundedOutput = "Lint passed with no warnings.\n";
        return clean;
    }
};

// The validated verification folder as a service sees it: it either REFUSES (the Backend found the root unusable) or hands out a root,
// and remembers for which workspace it was asked.
class ScratchProviderStub final : public IVerificationScratchRootProvider {
public:
    std::optional<common::Error> refusal;
    fs::path root;
    mutable std::vector<std::optional<fs::path>> asked;

    common::Result<VerificationScratchRoot> resolve(const std::optional<fs::path>& workspaceRoot) const override
    {
        asked.push_back(workspaceRoot);
        if (refusal.has_value()) return common::Result<VerificationScratchRoot>::failure(*refusal);
        return common::Result<VerificationScratchRoot>::ok({root, VerificationScratchSource::Setting});
    }
};

bool isUnder(const fs::path& candidate, const fs::path& root)
{
    return candidate.native().rfind(root.native(), 0) == 0;
}

// Real change-set creation through ChangeProposalService's ordinary
// validated pipeline (schema, safety scan, publish to a real
// FileChangeProposalStore) - never a hand-built ProposedChangeSet - so the
// resulting content hashes/overlay ManifestProposalOverlay::build computes
// are the genuine article prepare() will independently reconstruct.
CreateChangeProposalRequest makeCreateFileRequest(
    const fs::path& projectRoot, const RunId& runId, const ProjectId& projectId,
    const std::string& changeId, const std::string& relativePath, const std::string& content)
{
    std::ostringstream raw;
    raw << R"({"schemaVersion":"proposal-submission.v2","summary":"Fixture proposal",)"
        << R"("changes":[{"change_id":")" << changeId << R"(","operation":"create",)"
        << R"("relative_path":")" << relativePath << R"(","source_path":null,)"
        << R"("proposed_content":")" << content << R"("}]})";
    CreateChangeProposalRequest request;
    request.runId = runId;
    request.projectId = projectId;
    request.providerId = "openai";
    request.modelId = "gpt-4";
    request.agentRole = "qml_component_author";
    request.rawModelOutput = raw.str();
    request.policy.projectRoot = projectRoot;
    request.baseRevision = "main";
    return request;
}

struct Fixture {
    InMemoryRunRepository runs;
    InMemoryManifestWorkLedgerRepository workLedger;
    FakeChangeProposalRepository changeProposals;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationContinuationService service {runs, workLedger, changeProposals, continuations};

    Run makeRun(const RunId& id, RunStatus status, std::string workflowId = kExecutionManifestWorkflowId)
    {
        Run run;
        run.id = id;
        run.projectId = "project-1";
        run.title = "Continuation eligibility fixture";
        run.task = "Exercise MW-P5.1 admission";
        run.workflowId = std::move(workflowId);
        run.status = status;
        run.createdAt = std::chrono::system_clock::now();
        req(runs.insertRun(run).success(), "Could not seed Run");
        return run;
    }

    void seedProposal(const RunId& runId, const ChangeSetId& changeSetId, ChangeSetState state)
    {
        ProposedChangeSet cs;
        cs.schemaVersion = "proposal.v1";
        cs.changeSetId = changeSetId;
        cs.runId = runId;
        cs.projectId = "project-1";
        cs.agentRole = "qml_component_author";
        cs.summary = "Fixture proposal";
        cs.createdAt = std::chrono::system_clock::now();
        cs.state = state;
        req(changeProposals.insertChangeSet(cs).success(), "Could not seed proposal");
    }

    // A verification identity shaped like a real combined-verification
    // failure: safety passed, one required criterion Failed, one
    // content-repairable diagnostic naming that criterion's requirement.
    ManifestProposalSetVerification makeFailedVerification(
        const std::vector<ChangeSetId>& proposalIds,
        std::string diagnosticCode = "verification.qml_warning")
    {
        ManifestProposalSetVerification verification;
        verification.schemaVersion = "manifest-proposal-set-verification.v1";
        verification.workspaceIdentity = "workspace-1";
        verification.baselineFingerprint = "baseline-1";
        verification.orderedProposalIds = proposalIds;
        for (const auto& id : proposalIds) verification.orderedProposalContentHashes.push_back("hash-" + id);
        verification.overlayFingerprint = "overlay-1";
        verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
        verification.buildLintTestStatus = ExecutionVerificationStatus::Failed;
        verification.acceptanceStatus = ExecutionVerificationStatus::Failed;

        ManifestAcceptanceCriterionResult criterion;
        criterion.requirementId = "qml-lint";
        criterion.required = true;
        criterion.status = ExecutionVerificationStatus::Failed;
        verification.criteria.push_back(criterion);

        ManifestVerificationDiagnostic diagnostic;
        diagnostic.code = std::move(diagnosticCode);
        diagnostic.message = "Fixture diagnostic";
        diagnostic.requirementId = "qml-lint";
        verification.diagnostics.push_back(diagnostic);
        return verification;
    }

    void seedLedgerEntry(
        const RunId& runId,
        ManifestWorkLedgerDisposition disposition,
        std::optional<ManifestProposalSetVerification> verification)
    {
        ManifestWorkLedgerEntry entry;
        entry.runId = runId;
        entry.projectId = "project-1";
        entry.terminalStatus = RunStatus::Failed;
        entry.disposition = disposition;
        entry.manifestSchemaVersion = disposition == ManifestWorkLedgerDisposition::ManifestReady
            ? "execution-manifest.v1" : "";
        entry.verification = std::move(verification);
        entry.createdAt = std::chrono::system_clock::now();
        req(workLedger.insertEntry(entry).success(), "Could not seed Work Ledger entry");
    }
};

void testAdmitsAFullyEligibleFailedRun()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));

    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success(), "A fully eligible Run must admit successfully");
    req(result.value().status == ManifestVerificationContinuationStatus::Admitted,
        "A fully eligible Run must be Admitted");
    req(result.value().denialCode.empty(), "An Admitted checkpoint must carry no denial code");
    req(result.value().sourceRunId == "run-1", "sourceRunId must match the evaluated Run");
    req(result.value().sourceProjectId == "project-1", "sourceProjectId must match the Run's project");
}

void testIdempotentOnRepeatCall()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));

    const auto first = fx.service.admitOrEvaluate("run-1");
    const auto second = fx.service.admitOrEvaluate("run-1");
    req(first.success() && second.success(), "Both calls must succeed");
    req(first.value().id == second.value().id,
        "A repeat call for the same source Run must return the same checkpoint, not a new one");
    req(first.value().createdAt == second.value().createdAt,
        "A repeat call must not re-evaluate or overwrite the original decision");
}

void testDeniesNonManifestWorkflow()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed, "mock-sequential-v1");
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success(), "Denial is a normal Result, not a failure");
    req(result.value().status == ManifestVerificationContinuationStatus::Denied,
        "A non-Manifest-workflow Run must be Denied");
    req(result.value().denialCode == "verification_continuation.not_manifest_workflow",
        "Expected the not_manifest_workflow denial code");
}

void testDeniesNonFailedRun()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Completed);
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success() && result.value().denialCode == "verification_continuation.source_run_not_failed",
        "A non-Failed Run must be denied with source_run_not_failed");
}

void testDeniesMissingLedgerEntry()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success()
            && result.value().denialCode == "verification_continuation.missing_work_ledger_entry",
        "A Run with no Work Ledger entry must be denied with missing_work_ledger_entry");
}

void testDeniesPreFreezeFailure()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::NotProduced, std::nullopt);
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success() && result.value().denialCode == "verification_continuation.pre_freeze_failure",
        "A Run whose Manifest was never frozen must be denied with pre_freeze_failure");
}

void testDeniesMissingVerificationIdentity()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady, std::nullopt);
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success()
            && result.value().denialCode == "verification_continuation.missing_verification_identity",
        "A frozen Manifest that never reached combined verification must be denied "
        "with missing_verification_identity");
}

void testDeniesFailedProposalSafety()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    auto verification = fx.makeFailedVerification({"cs-1"});
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Failed;
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady, std::move(verification));
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success()
            && result.value().denialCode == "verification_continuation.proposal_safety_not_passed",
        "Failed proposal safety must be denied with proposal_safety_not_passed");
}

void testDeniesNoFailedCriteria()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    auto verification = fx.makeFailedVerification({"cs-1"});
    verification.criteria.front().status = ExecutionVerificationStatus::Passed;
    verification.diagnostics.clear();
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady, std::move(verification));
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success() && result.value().denialCode == "verification_continuation.no_failed_criteria",
        "A verification identity with no failed required criterion must be denied "
        "with no_failed_criteria");
}

void testDeniesNonRepairableDiagnostic()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}, "verification.timeout"));
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success()
            && result.value().denialCode == "verification_continuation.non_repairable_failure",
        "An infrastructure-only diagnostic code must be denied with non_repairable_failure");
}

void testOutsideScopePassedDiagnosticDoesNotBlockContentRepairAdmission()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    auto verification = fx.makeFailedVerification({"cs-1"});
    verification.diagnostics.push_back({
        "verification.qml_warning_outside_task_scope",
        "QML lint warnings were outside another task's frozen scope",
        "qml-lint"});
    fx.seedLedgerEntry(
        "run-1", ManifestWorkLedgerDisposition::ManifestReady, std::move(verification));

    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success()
            && result.value().status == ManifestVerificationContinuationStatus::Admitted,
        "A Passed criterion's outside-scope informational diagnostic must not veto an "
        "otherwise content-repairable failure");
}

void testDeniesChangedProposalState()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::Applied);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));
    const auto result = fx.service.admitOrEvaluate("run-1");
    req(result.success()
            && result.value().denialCode == "verification_continuation.source_proposal_state_changed",
        "A source proposal no longer ReadyForReview must be denied with "
        "source_proposal_state_changed");
}

void testDeniesMissingSourceRun()
{
    Fixture fx;
    const auto result = fx.service.admitOrEvaluate("no-such-run");
    req(!result.success(), "A missing source Run must fail, not return a Denied checkpoint");
    req(result.error()->code == "verification_continuation.source_run_not_found",
        "Expected the source_run_not_found failure code");
}

struct AttributionFixture {
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository workLedger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService service {
        manifests, workLedger, continuations, workspaceScopes, baselineNormalizations};

    ManifestProposalSetVerification candidate(std::string path = "qml/App.qml") const
    {
        ManifestProposalSetVerification value;
        value.workspaceIdentity = "workspace-1";
        value.baselineFingerprint = "baseline-1";
        value.orderedProposalIds = {"cs-component", "cs-integration"};
        value.orderedProposalContentHashes = {"hash-component", "hash-integration"};
        value.overlayFingerprint = "candidate-overlay";
        value.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
        value.buildLintTestStatus = ExecutionVerificationStatus::Failed;
        value.acceptanceStatus = ExecutionVerificationStatus::Failed;
        value.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
        ManifestVerificationDiagnostic diagnostic;
        diagnostic.code = "verification.qml_warning";
        diagnostic.message = "QML lint reported a warning";
        diagnostic.requirementId = "qml-lint";
        diagnostic.structuredFindings.push_back({"<candidate>/" + path,
            12, 4, "unqualified", "Unqualified access"});
        value.diagnostics.push_back(std::move(diagnostic));
        return value;
    }

    ManifestProposalSetVerification baseline() const
    {
        auto value = candidate();
        value.orderedProposalIds.clear();
        value.orderedProposalContentHashes.clear();
        value.overlayFingerprint = "baseline-overlay";
        value.diagnostics.clear();
        value.buildLintTestStatus = ExecutionVerificationStatus::Passed;
        value.acceptanceStatus = ExecutionVerificationStatus::Passed;
        value.criteria.front().status = ExecutionVerificationStatus::Passed;
        return value;
    }

    void seed(const ManifestProposalSetVerification& durable,
        std::vector<ManifestWorkLedgerTaskDisposition> taskDispositions = {},
        std::vector<ChangeSetId> proposalIds = {},
        std::vector<ManifestWorkLedgerProposalLineage> proposalLineage = {})
    {
        ExecutionAcceptanceRequirement requirement;
        requirement.id = "qml-lint";
        requirement.description = "QML lint is clean";
        requirement.verificationKind = ExecutionVerificationKind::QmlLint;
        requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
        ExecutionAcceptanceContract contract;
        contract.requirements.push_back(requirement);

        ExecutionManifestTask component;
        component.id = "component";
        component.ownedPaths = {"qml/App.qml"};
        component.requestedCapability = WorkspaceCapability::Propose;
        ExecutionManifestTask integration;
        integration.id = "integration";
        integration.ownedPaths = {"qml/CMakeLists.txt"};
        integration.acceptanceContract = contract;
        integration.requestedCapability = WorkspaceCapability::Propose;
        ExecutionManifest manifest;
        manifest.runId = "source-run";
        manifest.tasks = {component, integration};
        req(manifests.insert(manifest).success(), "Could not seed source Manifest");

        ManifestWorkLedgerEntry entry;
        entry.runId = "source-run";
        entry.projectId = "project-1";
        entry.terminalStatus = RunStatus::Failed;
        entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
        entry.verification = durable;
        entry.taskDispositions = taskDispositions.empty()
            ? std::vector<ManifestWorkLedgerTaskDisposition> {
                  {"component", "succeeded", ChangeSetId("cs-component"), std::nullopt},
                  {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}}
            : std::move(taskDispositions);
        entry.proposalIds = std::move(proposalIds);
        entry.proposalLineage = std::move(proposalLineage);
        entry.createdAt = std::chrono::system_clock::now();
        req(workLedger.insertEntry(entry).success(), "Could not seed source ledger");

        ManifestVerificationContinuationCheckpoint admission;
        admission.id = "verification-continuation:source-run";
        admission.sourceRunId = "source-run";
        admission.sourceProjectId = "project-1";
        admission.status = ManifestVerificationContinuationStatus::Admitted;
        admission.createdAt = std::chrono::system_clock::now();
        req(continuations.insert(admission).success(), "Could not seed admission");
    }
};

RunWorkspaceScopeEntry frozenScopeEntry(std::string path, WorkspaceScopeRole role,
    std::string taskId)
{
    RunWorkspaceScopeEntry entry;
    entry.relativePath = std::move(path);
    entry.role = role;
    entry.reason = "Backend-frozen test scope entry";
    entry.taskId = std::move(taskId);
    if (role != WorkspaceScopeRole::PlannedCreate) entry.baselineHash = std::string(64, 'a');
    entry.origin = WorkspaceScopeOrigin::FrozenInitial;
    return entry;
}

void testAttributesCandidateFindingToActualProposalOwner()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate);
    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Attributed,
        "A structured candidate finding must be attributed");
    req(result.value().findings.size() == 1
            && result.value().findings.front().kind
                == ManifestVerificationFindingAttributionKind::SourceProposalOwner
            && result.value().findings.front().ownerTaskId == "component"
            && result.value().findings.front().ownerProposalId == "cs-component",
        "The finding was not routed to its real proposal/task owner");
}

void testAttributesRequirementOwnerAsIntegrationOwner()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate("qml/CMakeLists.txt");
    fx.seed(candidate);
    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().findings.front().kind
            == ManifestVerificationFindingAttributionKind::IntegrationOwner,
        "A requirement-owner path must be classified as integration-owned");
}

void testAttributionMapsPartialRepairSuccessorsToExactVerifiedAncestors()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate,
        {{"component", "succeeded", ChangeSetId("cs-component-repair"), std::nullopt},
         {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}},
        {"cs-component", "cs-integration", "cs-component-repair"},
        {{"cs-component", "ready_for_review", std::nullopt},
         {"cs-integration", "ready_for_review", std::nullopt},
         {"cs-component-repair", "ready_for_review", ChangeSetId("cs-component")}});

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success()
            && result.value().status == ManifestVerificationAttributionStatus::Attributed
            && result.value().findings.size() == 1
            && result.value().findings.front().ownerTaskId == "component"
            && result.value().findings.front().ownerProposalId == "cs-component",
        "A terminal successor task disposition must map backwards to the exact proposal member "
        "that the last authoritative combined verification actually checked");
}

void testAttributionFailsClosedOnMissingOrCyclicPartialRepairLineage()
{
    const auto runCase = [](std::vector<ChangeSetId> proposalIds,
                             std::vector<ManifestWorkLedgerProposalLineage> lineage,
                             const std::string& label) {
        AttributionFixture fx;
        const auto candidate = fx.candidate();
        fx.seed(candidate,
            {{"component", "succeeded", ChangeSetId("cs-component-repair"), std::nullopt},
             {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}},
            std::move(proposalIds), std::move(lineage));
        const auto result = fx.service.attributeOrEvaluate(
            "source-run", fx.baseline(), candidate);
        req(result.success()
                && result.value().status == ManifestVerificationAttributionStatus::Blocked
                && result.value().blockCode
                    == "verification_continuation.attribution_manifest_ambiguous",
            "Partial-repair ownership must fail closed on incomplete or cyclic lineage");
        static_cast<void>(label);
    };

    runCase({"cs-component", "cs-integration", "cs-component-repair"},
        {{"cs-integration", "ready_for_review", std::nullopt},
         {"cs-component-repair", "ready_for_review", ChangeSetId("cs-component")}},
        "missing its verified predecessor node");
    runCase({"cs-component", "cs-integration", "cs-component-repair", "cs-cycle"},
        {{"cs-component", "ready_for_review", std::nullopt},
         {"cs-integration", "ready_for_review", std::nullopt},
         {"cs-component-repair", "ready_for_review", ChangeSetId("cs-cycle")},
         {"cs-cycle", "ready_for_review", ChangeSetId("cs-component-repair")}},
        "cyclic");
}

void testAttributionFailsClosedWhenTwoTasksResolveToOneVerifiedLineageRoot()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate,
        {{"component", "succeeded", ChangeSetId("cs-component-repair-a"), std::nullopt},
         {"integration", "succeeded", ChangeSetId("cs-component-repair-b"), std::nullopt}},
        {"cs-component", "cs-integration", "cs-component-repair-a", "cs-component-repair-b"},
        {{"cs-component", "ready_for_review", std::nullopt},
         {"cs-integration", "ready_for_review", std::nullopt},
         {"cs-component-repair-a", "ready_for_review", ChangeSetId("cs-component")},
         {"cs-component-repair-b", "ready_for_review", ChangeSetId("cs-component")}});

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success()
            && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode
                == "verification_continuation.attribution_manifest_ambiguous",
        "Two task dispositions must never claim the same exact verified lineage member");
}

void testLegacyCachedOwnershipMismatchReevaluatesWithoutRewritingAuditRow()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate,
        {{"component", "succeeded", ChangeSetId("cs-component-repair"), std::nullopt},
         {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}},
        {"cs-component", "cs-integration", "cs-component-repair"},
        {{"cs-component", "ready_for_review", std::nullopt},
         {"cs-integration", "ready_for_review", std::nullopt},
         {"cs-component-repair", "ready_for_review", ChangeSetId("cs-component")}});

    ManifestVerificationAttributionCheckpoint legacy;
    legacy.id = "verification-attribution:source-run";
    legacy.sourceRunId = "source-run";
    legacy.sourceProjectId = "project-1";
    legacy.status = ManifestVerificationAttributionStatus::Blocked;
    legacy.blockCode = "verification_continuation.attribution_source_changed";
    legacy.blockMessage =
        "The source ledger task ownership does not match the exact verified proposal set";
    legacy.baselineFingerprint = candidate.baselineFingerprint;
    legacy.sourceOverlayFingerprint = candidate.overlayFingerprint;
    legacy.createdAt = std::chrono::system_clock::now();
    req(fx.continuations.insertAttribution(legacy).success(),
        "Could not seed the immutable legacy false-denial checkpoint");

    const auto recovered = fx.service.attributeOrEvaluate(
        "source-run", fx.baseline(), candidate);
    req(recovered.success()
            && recovered.value().status == ManifestVerificationAttributionStatus::Attributed
            && recovered.value().findings.size() == 1
            && recovered.value().findings.front().ownerProposalId == "cs-component",
        "The exact legacy ownership false-denial must be safely recoverable from immutable "
        "lineage evidence");

    const auto durable = fx.continuations.findAttributionBySourceRun("source-run");
    req(durable.success() && durable.value().has_value()
            && durable.value()->status == ManifestVerificationAttributionStatus::Blocked
            && durable.value()->blockCode == legacy.blockCode
            && durable.value()->blockMessage == legacy.blockMessage,
        "Recovering a legacy ownership false-denial must never rewrite its immutable audit row");
}

void testLegacyCachedOwnershipMismatchSurfacesCurrentBlockWithoutRewritingAuditRow()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate,
        {{"component", "succeeded", ChangeSetId("cs-component-repair"), std::nullopt},
         {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}},
        {"cs-component", "cs-integration", "cs-component-repair"},
        {// Deliberately omit the verified predecessor node. The old direct-id
         // mismatch is no longer authoritative, but the current evidence must
         // still fail closed with its own precise lineage error.
         {"cs-integration", "ready_for_review", std::nullopt},
         {"cs-component-repair", "ready_for_review", ChangeSetId("cs-component")}});

    ManifestVerificationAttributionCheckpoint legacy;
    legacy.id = "verification-attribution:source-run";
    legacy.sourceRunId = "source-run";
    legacy.sourceProjectId = "project-1";
    legacy.status = ManifestVerificationAttributionStatus::Blocked;
    legacy.blockCode = "verification_continuation.attribution_source_changed";
    legacy.blockMessage =
        "The source ledger task ownership does not match the exact verified proposal set";
    legacy.baselineFingerprint = candidate.baselineFingerprint;
    legacy.sourceOverlayFingerprint = candidate.overlayFingerprint;
    legacy.createdAt = std::chrono::system_clock::now();
    req(fx.continuations.insertAttribution(legacy).success(),
        "Could not seed the immutable legacy false-denial checkpoint");

    const auto reevaluated = fx.service.attributeOrEvaluate(
        "source-run", fx.baseline(), candidate);
    req(reevaluated.success()
            && reevaluated.value().status == ManifestVerificationAttributionStatus::Blocked
            && reevaluated.value().blockCode
                == "verification_continuation.attribution_manifest_ambiguous",
        "Legacy recovery must surface the current precise fail-closed result instead of "
        "masking it with the historical ownership mismatch");

    const auto durable = fx.continuations.findAttributionBySourceRun("source-run");
    req(durable.success() && durable.value().has_value()
            && durable.value()->status == ManifestVerificationAttributionStatus::Blocked
            && durable.value()->blockCode == legacy.blockCode
            && durable.value()->blockMessage == legacy.blockMessage,
        "Surfacing the current blocker must never rewrite the immutable legacy audit row");
}

void testMixedAnchorLegacyOwnershipMismatchSurfacesCurrentBlockWithoutRewritingAuditRow()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate);

    ManifestWorkLedgerEntry anchorLedger;
    anchorLedger.runId = "anchor-run";
    anchorLedger.projectId = "project-1";
    anchorLedger.terminalStatus = RunStatus::Failed;
    anchorLedger.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    anchorLedger.verification = candidate;
    anchorLedger.taskDispositions = {
        {"component", "succeeded", ChangeSetId("cs-component-repair"), std::nullopt}};
    anchorLedger.proposalIds = {"cs-component", "cs-integration", "cs-component-repair"};
    anchorLedger.proposalLineage = {
        {"cs-integration", "ready_for_review", std::nullopt},
        {"cs-component-repair", "ready_for_review", ChangeSetId("cs-component")}};
    anchorLedger.createdAt = std::chrono::system_clock::now();
    req(fx.workLedger.insertEntry(anchorLedger).success(),
        "Could not seed the mixed verification anchor ledger");

    ManifestVerificationAttributionCheckpoint legacy;
    legacy.id = "verification-attribution:anchor-run";
    legacy.sourceRunId = "anchor-run";
    legacy.sourceProjectId = "project-1";
    legacy.status = ManifestVerificationAttributionStatus::Blocked;
    legacy.blockCode = "verification_continuation.attribution_source_changed";
    legacy.blockMessage =
        "The source ledger task ownership does not match the exact verified proposal set";
    legacy.baselineFingerprint = candidate.baselineFingerprint;
    legacy.sourceOverlayFingerprint = candidate.overlayFingerprint;
    legacy.createdAt = std::chrono::system_clock::now();
    req(fx.continuations.insertAttribution(legacy).success(),
        "Could not seed the mixed anchor's immutable legacy false-denial checkpoint");

    const auto reevaluated = fx.service.attributeOrEvaluateForAnchor(
        "anchor-run", "source-run", fx.baseline(), candidate);
    req(reevaluated.success()
            && reevaluated.value().status == ManifestVerificationAttributionStatus::Blocked
            && reevaluated.value().blockCode
                == "verification_continuation.attribution_manifest_ambiguous",
        "Mixed-anchor legacy recovery must surface the current precise fail-closed result");

    const auto durable = fx.continuations.findAttributionBySourceRun("anchor-run");
    req(durable.success() && durable.value().has_value()
            && durable.value()->blockCode == legacy.blockCode
            && durable.value()->blockMessage == legacy.blockMessage,
        "Mixed-anchor current-block reporting must preserve its immutable legacy audit row");
}

// MW-P5.6C: the Manifest silently has no owner for the finding path (the
// exact shape a Manifest persisted before dc0391a's fix would read back as -
// see ManifestVerificationAttributionService.hpp's class comment) but the
// frozen AG-15 workspace scope names exactly one owner. Legacy fallback must
// attribute the finding to that owner, never leave it unattributed.
void testAttributionFallsBackToFrozenScopeWhenManifestOwnerMissing()
{
    AttributionFixture fx;

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = "source-run";
    manifest.tasks = {component, integration};
    req(fx.manifests.insert(manifest).success(), "Could not seed owner-missing Manifest");

    RunWorkspaceScope scope;
    scope.runId = "source-run";
    scope.manifestSchemaVersion = manifest.schemaVersion;
    scope.entries = {
        frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::OwnedWrite, "component"),
        frozenScopeEntry("qml/cmakelists.txt", WorkspaceScopeRole::OwnedWrite, "integration")};
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze fallback scope");

    const auto candidate = fx.candidate();
    ManifestWorkLedgerEntry ledgerEntry;
    ledgerEntry.runId = "source-run";
    ledgerEntry.projectId = "project-1";
    ledgerEntry.terminalStatus = RunStatus::Failed;
    ledgerEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    ledgerEntry.verification = candidate;
    ledgerEntry.taskDispositions = {
        {"component", "succeeded", ChangeSetId("cs-component"), std::nullopt},
        {"integration", "succeeded", ChangeSetId("cs-integration"), std::nullopt}};
    ledgerEntry.createdAt = std::chrono::system_clock::now();
    req(fx.workLedger.insertEntry(ledgerEntry).success(), "Could not seed owner-missing ledger");

    ManifestVerificationContinuationCheckpoint admission;
    admission.id = "verification-continuation:source-run";
    admission.sourceRunId = "source-run";
    admission.sourceProjectId = "project-1";
    admission.status = ManifestVerificationContinuationStatus::Admitted;
    admission.createdAt = std::chrono::system_clock::now();
    req(fx.continuations.insert(admission).success(), "Could not seed owner-missing admission");

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Attributed
            && result.value().findings.size() == 1
            && result.value().findings.front().ownerTaskId == "component"
            && result.value().findings.front().ownerProposalId == "cs-component",
        "A Manifest with no owner for the path must still attribute via the frozen scope "
        "fallback, never fail with attribution_outside_manifest");
}

// MW-P5.6C: the Manifest and a structurally valid frozen scope both name an
// owner for the same path, and they disagree. Since the scope is derived
// directly from the in-memory Manifest at freeze time, any such disagreement
// means one of the two was corrupted afterward - never silently prefer
// either side.
void testAttributionFailsClosedOnManifestScopeConflict()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate);

    RunWorkspaceScope scope;
    scope.runId = "source-run";
    scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
    scope.entries = {
        frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::OwnedWrite, "integration")};
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze conflicting scope");

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode
                == "verification_continuation.attribution_manifest_scope_conflict",
        "A Manifest/scope ownership disagreement must fail closed with a distinct conflict code, "
        "never silently pick one side");
}

void testAttributionRequiresScopeToCorroborateAManifestOwner()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate);

    RunWorkspaceScope scope;
    scope.runId = "source-run";
    scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
    scope.entries = {
        frozenScopeEntry("qml/cmakelists.txt", WorkspaceScopeRole::OwnedWrite, "integration")};
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze incomplete ownership scope");

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode
                == "verification_continuation.attribution_manifest_scope_conflict",
        "Once a frozen scope exists, omission of a Manifest-owned target path must be treated "
        "as a conflict rather than trusting only one immutable source");
}

// MW-P5.6C: a frozen scope row exists for this Run but fails its own
// identity check (here: manifestSchemaVersion does not match the Manifest it
// is supposed to have been frozen alongside) - must fail closed rather than
// being silently ignored or silently trusted.
void testAttributionFailsClosedOnStructurallyInvalidScope()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate);

    RunWorkspaceScope scope;
    scope.runId = "source-run";
    scope.manifestSchemaVersion = "some-other-manifest-generation";
    scope.entries = {
        frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::OwnedWrite, "component")};
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze identity-mismatched scope");

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode == "verification_continuation.attribution_scope_invalid",
        "A structurally invalid frozen scope must fail closed with a distinct code, never be "
        "silently ignored");
}

void testAttributionValidatesTheEntireFrozenScopeBeforePathLookup()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate();
    fx.seed(candidate);

    RunWorkspaceScope scope;
    scope.runId = "source-run";
    scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
    scope.entries = {
        frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::OwnedWrite, "component"),
        frozenScopeEntry("qml/unrelated.qml", WorkspaceScopeRole::OwnedWrite, "component"),
        frozenScopeEntry("qml/unrelated.qml", WorkspaceScopeRole::PlannedCreate, "integration")};
    scope.entries.back().baselineHash.reset();
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze globally ambiguous scope");

    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode == "verification_continuation.attribution_scope_invalid",
        "An ownership collision elsewhere in the immutable scope must invalidate the whole scope, "
        "not only a lookup for the colliding path");
}

void testAttributionRejectsMalformedFrozenEntriesOfEveryRole()
{
    const auto runCase = [](RunWorkspaceScopeEntry malformed, const std::string& label) {
        AttributionFixture fx;
        const auto candidate = fx.candidate();
        fx.seed(candidate);
        RunWorkspaceScope scope;
        scope.runId = "source-run";
        scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
        scope.entries = {
            frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::OwnedWrite, "component"),
            std::move(malformed)};
        scope.frozenAt = std::chrono::system_clock::now();
        req(fx.workspaceScopes.freeze(scope).success(),
            "Could not freeze malformed scope fixture");
        const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
        req(result.success()
                && result.value().blockCode
                    == "verification_continuation.attribution_scope_invalid",
            "A malformed entry of any role must invalidate the complete frozen scope");
        static_cast<void>(label);
    };

    auto unknownTask = frozenScopeEntry(
        "qml/reference.qml", WorkspaceScopeRole::ReferenceOnly, "unknown-task");
    runCase(std::move(unknownTask), "non-write entry with an unknown task");

    auto nonCanonical = frozenScopeEntry(
        "qml/../secret.qml", WorkspaceScopeRole::ReferenceOnly, "component");
    runCase(std::move(nonCanonical), "unsafe path");

    auto expansion = frozenScopeEntry(
        "qml/reference.qml", WorkspaceScopeRole::ReferenceOnly, "component");
    expansion.origin = WorkspaceScopeOrigin::ApprovedExpansion;
    runCase(std::move(expansion), "non-frozen origin");
}

void testAttributionRejectsInvalidDependencyEdgesAndScopeBounds()
{
    {
        AttributionFixture fx;
        const auto candidate = fx.candidate();
        fx.seed(candidate);
        RunWorkspaceScope scope;
        scope.runId = "source-run";
        scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
        scope.entries = {
            frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::OwnedWrite, "component"),
            frozenScopeEntry("qml/app.qml", WorkspaceScopeRole::RequiredRead, "integration")};
        scope.dependencyEdges.push_back(
            {"integration", "integration", "qml/app.qml"});
        scope.frozenAt = std::chrono::system_clock::now();
        req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze invalid dependency edge");
        const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
        req(result.success()
                && result.value().blockCode
                    == "verification_continuation.attribution_scope_invalid",
            "A dependency edge inconsistent with frozen ownership must fail closed");
    }
    {
        AttributionFixture fx;
        const auto candidate = fx.candidate();
        fx.seed(candidate);
        RunWorkspaceScope scope;
        scope.runId = "source-run";
        scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
        scope.entries.reserve(kRunWorkspaceScopeMaxEntries + 1);
        for (std::size_t i = 0; i <= kRunWorkspaceScopeMaxEntries; ++i) {
            scope.entries.push_back(frozenScopeEntry("qml/reference-" + std::to_string(i) + ".qml",
                WorkspaceScopeRole::ReferenceOnly, "component"));
        }
        scope.frozenAt = std::chrono::system_clock::now();
        req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze oversized scope");
        const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
        req(result.success()
                && result.value().blockCode
                    == "verification_continuation.attribution_scope_invalid",
            "A frozen scope above the Backend bound must fail closed");
    }
}

void testInvalidScopeBlocksEvenWhenEveryFindingIsPreExistingDebt()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate("qml/Legacy.qml");
    auto baseline = fx.baseline();
    baseline.diagnostics = candidate.diagnostics;
    baseline.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    baseline.acceptanceStatus = ExecutionVerificationStatus::Failed;
    baseline.criteria.front().status = ExecutionVerificationStatus::Failed;
    fx.seed(candidate);

    RunWorkspaceScope scope;
    scope.runId = "source-run";
    scope.manifestSchemaVersion = std::string(kExecutionManifestSchemaVersion);
    scope.entries = {
        frozenScopeEntry("qml/reference.qml", WorkspaceScopeRole::ReferenceOnly, "unknown-task")};
    scope.frozenAt = std::chrono::system_clock::now();
    req(fx.workspaceScopes.freeze(scope).success(), "Could not freeze invalid debt-only scope");

    const auto result = fx.service.attributeOrEvaluate("source-run", baseline, candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode == "verification_continuation.attribution_scope_invalid",
        "An invalid immutable scope must block before finding classification, including a Run "
        "whose findings are all pre-existing debt");
}

void testProvesPreExistingDebtByExactBaselineFinding()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate("qml/Legacy.qml");
    auto baseline = fx.baseline();
    baseline.diagnostics = candidate.diagnostics;
    baseline.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    baseline.acceptanceStatus = ExecutionVerificationStatus::Failed;
    baseline.criteria.front().status = ExecutionVerificationStatus::Failed;
    fx.seed(candidate);
    const auto result = fx.service.attributeOrEvaluate("source-run", baseline, candidate);
    req(result.success() && result.value().findings.front().kind
            == ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt
            && !result.value().findings.front().ownerTaskId.has_value(),
        "An exact baseline finding must become explicit pre-existing debt");
}

void testCandidateIntroducedOutsideManifestFailsClosed()
{
    AttributionFixture fx;
    const auto candidate = fx.candidate("qml/Outside.qml");
    fx.seed(candidate);
    const auto result = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(result.success() && result.value().status == ManifestVerificationAttributionStatus::Blocked
            && result.value().blockCode
                == "verification_continuation.attribution_outside_manifest",
        "Candidate-introduced out-of-scope diagnostics must block continuation");
}

void testOutsideScopeBaselineWarningRemainsPreExistingWhenCandidateHasMixedWarnings()
{
    AttributionFixture fx;
    auto baseline = fx.baseline();
    ManifestVerificationDiagnostic baselineOutside;
    baselineOutside.code = "verification.qml_warning_outside_task_scope";
    baselineOutside.message = "QML lint warnings were outside the task scope";
    baselineOutside.requirementId = "qml-lint";
    baselineOutside.structuredFindings.push_back({"<candidate>/qml/Outside.qml",
        3, 1, "unqualified", "Unqualified access"});
    baseline.diagnostics.push_back(std::move(baselineOutside));

    auto candidate = fx.candidate();
    candidate.diagnostics.front().structuredFindings.insert(
        candidate.diagnostics.front().structuredFindings.begin(),
        {"<candidate>/qml/Outside.qml", 3, 1, "unqualified", "Unqualified access"});
    fx.seed(candidate);

    const auto result = fx.service.attributeOrEvaluate("source-run", baseline, candidate);
    req(result.success()
            && result.value().status == ManifestVerificationAttributionStatus::Attributed
            && result.value().findings.size() == 2
            && result.value().findings.front().kind
                == ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt
            && result.value().findings.back().kind
                == ManifestVerificationFindingAttributionKind::SourceProposalOwner
            && result.value().findings.back().relativePath == "qml/app.qml",
        "An unchanged outside-scope baseline warning must remain pre-existing debt when the "
        "candidate's combined qmllint diagnostic also contains a real in-scope warning");
}

void testOutsideScopePassedDiagnosticDoesNotBlockAttribution()
{
    AttributionFixture fx;
    auto candidate = fx.candidate();
    candidate.diagnostics.push_back({
        "verification.qml_warning_outside_task_scope",
        "QML lint warnings were outside another task's frozen scope",
        "qml-lint"});
    fx.seed(candidate);

    const auto result = fx.service.attributeOrEvaluate(
        "source-run", fx.baseline(), candidate);
    req(result.success()
            && result.value().status == ManifestVerificationAttributionStatus::Attributed
            && result.value().findings.size() == 2
            && result.value().findings.front().kind
                == ManifestVerificationFindingAttributionKind::SourceProposalOwner
            && result.value().findings.back().kind
                == ManifestVerificationFindingAttributionKind::IgnorableCascade,
        "A Passed criterion's outside-scope informational diagnostic must remain visible but "
        "must not block attribution of a separate repairable finding");
}

void testUnstructuredAndIdentityMismatchedResultsFailClosedAndRemainImmutable()
{
    AttributionFixture fx;
    auto candidate = fx.candidate();
    fx.seed(candidate);
    candidate.diagnostics.front().structuredFindings.clear();
    const auto first = fx.service.attributeOrEvaluate("source-run", fx.baseline(), candidate);
    req(first.success() && first.value().blockCode
            == "verification_continuation.attribution_diagnostic_unmapped",
        "An unstructured diagnostic must block attribution");
    auto corrected = fx.candidate();
    const auto second = fx.service.attributeOrEvaluate("source-run", fx.baseline(), corrected);
    req(second.success() && second.value().id == first.value().id
            && second.value().status == ManifestVerificationAttributionStatus::Blocked,
        "A persisted blocked attribution must be immutable/idempotent");
}

void testAttributionSourceMismatchNamesTheChangedIdentityField()
{
    AttributionFixture fx;
    const auto durable = fx.candidate();
    fx.seed(durable);
    auto reconstructed = durable;
    reconstructed.criteria.front().status = ExecutionVerificationStatus::NotVerifiable;

    const auto result = fx.service.attributeOrEvaluate(
        "source-run", fx.baseline(), reconstructed);
    req(result.success()
            && result.value().blockCode
                == "verification_continuation.attribution_source_changed"
            && result.value().blockMessage.find("criterion_status:qml-lint")
                != std::string::npos,
        "A source-identity refusal must name the changed bounded field, not hide it behind "
        "a generic mismatch message");
}

// End-to-end MW-P5.3 happy path: a real workspace, real proposals through
// ChangeProposalService's ordinary validated pipeline, a real baseline
// capture, a real ManifestProposalOverlay reconstruction, and real
// ManifestProposalSetVerifier calls (scripted only at the command-runner
// boundary) - exercising prepare()'s actual logic end to end for the first
// time, not just its reconcileMissingChildren() crash-recovery path.
void runPrepareScenario(const std::string& ephemeralScratchPadding, ScratchProviderStub* provider = nullptr)
{
    TempDir workspace;
    TempDir scratch;
    // The scratch root the verification overlays are built under: the short TempDir itself, or (padding) one made long on purpose.
    fs::path ephemeralScratch = scratch.path;
    if (!ephemeralScratchPadding.empty()) {
        ephemeralScratch /= ephemeralScratchPadding;
        fs::create_directories(ephemeralScratch);
    }
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparation(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, ephemeralScratch);
    if (provider != nullptr) preparation.setVerificationScratchProvider(provider);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Prepare happy path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requiredReadPaths = {"qml/App.qml"};
    integration.dependencies = {"component"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    // Learn the real overlay/content-hash identity a durable verification
    // record must carry for prepare()'s own source_candidate_mismatch check
    // to accept it - never invented strings, the genuine article.
    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    // prepare()'s own two ManifestProposalSetVerifier::verify() calls: one
    // clean for the empty baseline overlay, one reproducing the same
    // baseline-only outside-scope warning followed by the same warning plus
    // a real in-scope candidate warning. This is the production Stage 3
    // shape: admission still sees the required failed criterion, while
    // attribution must preserve the former as pre-existing debt and route
    // only qml/App.qml to its SourceProposalOwner.
    ManifestVerificationCommandOutcome baselineOutsideOutcome;
    baselineOutsideOutcome.started = true;
    baselineOutsideOutcome.exitCode = 0;
    baselineOutsideOutcome.boundedOutput =
        "Warning: <candidate>/qml/Outside.qml:3:1: Unqualified access [unqualified]\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/Outside.qml:3:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {baselineOutsideOutcome, warningOutcome};

    const auto prepared = preparation.prepare(source.id);
    if (provider != nullptr && provider->refusal.has_value()) {
        // The Backend refused the verification folder: prepare() says so with the Backend's own words, BEFORE anything was copied or run.
        req(!prepared.success() && prepared.error()->code == provider->refusal->code
                && prepared.error()->message == provider->refusal->message,
            "prepare() must return the refusal of the verification folder as it is");
        req(commandRunner.calls == 0, "a refused verification folder must stop prepare() before any command runs");
        req(fs::is_empty(scratch.path), "a refused verification folder must stop prepare() before anything is copied");
        req(provider->asked.size() == 1 && provider->asked.front() == std::optional<fs::path>(workspace.path),
            "the folder is asked for once, for the project's own workspace");
        return;
    }
    if (!ephemeralScratchPadding.empty()) {
        // A scratch root too long for the build tools' path budget: prepare() REFUSES, naming the path budget - never the
        // misleading "attribution_diagnostic_unmapped", and nothing is recorded as an attribution of the code under test.
        req(!prepared.success(), "a verification copy that cannot fit the path budget must not prepare successfully");
        req(prepared.error()->code == "verification_continuation.verification_path_too_long",
            ("the refusal must name the path budget, not '" + prepared.error()->code + "'").c_str());
        req(prepared.error()->message.find("TEMP") != std::string::npos
                && prepared.error()->message.find(ephemeralScratch.string()) == std::string::npos,
            "the message must point at the temporary-directory location without leaking a filesystem path");
        return;
    }
    req(prepared.success(), "A fully eligible source Run must prepare successfully");
    req(prepared.value().attribution.status == ManifestVerificationAttributionStatus::Attributed,
        "The real candidate finding must attribute cleanly");
    req(prepared.value().attribution.findings.size() == 2
            && prepared.value().attribution.findings.front().kind
                == ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt
            && prepared.value().attribution.findings.back().kind
                == ManifestVerificationFindingAttributionKind::SourceProposalOwner
            && prepared.value().attribution.findings.back().ownerTaskId == "component",
        "The real qml/App.qml finding must route to the component task, not integration");
    req(prepared.value().input.sourceRunId == source.id
            && prepared.value().input.orderedProposalIds == overlayRequest.orderedProposalIds
            && prepared.value().input.orderedProposalContentHashes == contentHashes,
        "The frozen input must carry the exact real source proposal identity");
    req(!prepared.value().input.childRunId.empty(), "A child Run id must be generated");
    if (provider != nullptr) {
        // Every temporary copy was built under the root the Backend validated - not under the constructor's root.
        req(!commandRunner.workingDirectories.empty(), "commands must have run in verification copies");
        for (const auto& directory : commandRunner.workingDirectories) {
            req(isUnder(directory, provider->root) && !isUnder(directory, scratch.path),
                "a verification copy must be built under the validated root");
        }
    }

    const auto child = runs.findRun(prepared.value().input.childRunId);
    req(child.success() && child.value().has_value(), "The Prepared child Run was not persisted");
    req(child.value()->status == RunStatus::Prepared
            && child.value()->parentRunId == std::optional<RunId>(source.id)
            && child.value()->workflowId == kManifestVerificationContinuationWorkflowId,
        "The Prepared child must carry the exact continuation identity");
    req(child.value()->id == prepared.value().childRun.id,
        "The returned childRun must be the same row just persisted");
}

// MW-P5.6D end-to-end: the source Run's durable baseline was captured with
// .qtcreator/ visible (the real pre-75afa2b shape); the live workspace today
// is captured with the current, real secureDefaults() policy that excludes
// it. prepare() must prove this drift is limited to .qtcreator/, persist a
// normalization proof, and still successfully prepare a continuation - with
// its frozen input pinned to the CURRENT baseline/overlay identity, not the
// source Run's stale one.
void testPrepareCreatesInputAndPreparedChildForAFullyEligibleRun()
{
    runPrepareScenario("");
}

void testPrepareNamesThePathBudgetWhenTheVerificationCopyCannotFit()
{
    runPrepareScenario(std::string(120, 's'));
}

void testPrepareIsRefusedBeforeAnyCopyWhenTheVerificationFolderIsNotUsable()
{
    ScratchProviderStub provider;
    provider.refusal = common::Error {"verification_scratch.too_long", "The verification folder is too long. Choose a short folder."};
    runPrepareScenario("", &provider);
}

void testPrepareBuildsItsCopiesUnderTheRootTheBackendValidated()
{
    TempDir providerRoot;
    ScratchProviderStub provider;
    provider.root = providerRoot.path;
    runPrepareScenario("", &provider);
}
void testPrepareNormalizesLegacyQtCreatorBaselineDriftAndPinsFreshIdentity()
{
    TempDir workspace;
    TempDir scratch;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparation(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Legacy baseline normalization";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    std::error_code createError;
    std::filesystem::create_directories(workspace.path / ".qtcreator", createError);
    req(!createError, "Could not create the .qtcreator fixture directory");
    {
        std::ofstream qtcreator(workspace.path / ".qtcreator" / "CMakeLists.txt.user",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(qtcreator), "Could not write the .qtcreator fixture file");
        qtcreator << "<original-session/>";
    }

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    // The DURABLE baseline, exactly as originally captured before 75afa2b's
    // WorkspaceReadPolicy fix - .qtcreator was visible then.
    auto legacyPolicy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    legacyPolicy.deniedDirectoryNames.erase(
        std::remove(legacyPolicy.deniedDirectoryNames.begin(),
            legacyPolicy.deniedDirectoryNames.end(), std::string(".qtcreator")),
        legacyPolicy.deniedDirectoryNames.end());
    const auto legacyBaselineSnapshot = WorkspaceBaselineProvenance::capture(legacyPolicy);
    // Not-yet-applied proposals never appear on the real workspace filesystem
    // (they live only in the separate .proposals/ store, itself captured by
    // this permissive policy exactly like any other real file) - only assert
    // the one fact this test actually depends on: the legacy capture saw the
    // real .qtcreator housekeeping file.
    req(legacyBaselineSnapshot.success()
            && std::any_of(legacyBaselineSnapshot.value().files.begin(),
                legacyBaselineSnapshot.value().files.end(), [](const auto& entry) {
                    return entry.relativePath.starts_with(".qtcreator/");
                }),
        "Could not capture the legacy (.qtcreator-visible) workspace baseline");
    const auto legacyBaselineJson =
        WorkspaceBaselineProvenance::renderJson(legacyBaselineSnapshot.value());
    req(legacyBaselineJson.success(), "Could not render the legacy baseline artifact");
    req(artifacts.createArtifact(source.id, std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", legacyBaselineJson.value())
            .success(),
        "Could not persist the durable legacy baseline artifact");

    // The reference overlay/content-hash identity the source Run's durable
    // verification carries - built against the LEGACY (.qtcreator-visible)
    // baseline, exactly as it would have been at the time.
    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = legacyPolicy;
    overlayRequest.baselineSnapshot = legacyBaselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the legacy reference candidate overlay");
    const std::string legacyOverlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the legacy reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = legacyBaselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = legacyOverlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    // The current, REAL secureDefaults() policy excludes .qtcreator (post-
    // 75afa2b) - prepare()'s own fresh capture will therefore never see the
    // .qtcreator entry the durable baseline had, reproducing exactly the
    // real bb56e001-shaped drift this package normalizes.
    const auto currentBaseline = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(workspace.path));
    req(currentBaseline.success()
            && std::none_of(currentBaseline.value().files.begin(),
                currentBaseline.value().files.end(), [](const auto& entry) {
                    return entry.relativePath.starts_with(".qtcreator/");
                }),
        "Could not capture the current, policy-excluded workspace baseline");
    req(currentBaseline.value().fingerprint != durable.baselineFingerprint,
        "Test fixture did not actually produce baseline drift");

    const auto prepared = preparation.prepare(source.id);
    req(prepared.success(), "A Run whose drift is limited to .qtcreator/ must still prepare");
    req(prepared.value().attribution.status == ManifestVerificationAttributionStatus::Attributed
            && prepared.value().attribution.findings.size() == 1
            && prepared.value().attribution.findings.front().ownerTaskId == "component",
        "The real qml/App.qml finding must still attribute cleanly after normalization");
    req(prepared.value().input.baselineFingerprint == currentBaseline.value().fingerprint
            && prepared.value().input.baselineFingerprint != durable.baselineFingerprint,
        "The frozen continuation input must pin the CURRENT (post-normalization) baseline "
        "identity, not the source Run's stale pre-fix one");
    req(prepared.value().input.sourceOverlayFingerprint != legacyOverlayFingerprint,
        "The frozen continuation input must pin a freshly rebuilt overlay identity, not the "
        "legacy .qtcreator-including one");
    req(prepared.value().input.orderedProposalContentHashes == contentHashes,
        "Proposal content identity (baseline-independent) must still match exactly");

    const auto normalization = baselineNormalizations.findBySourceRun(source.id);
    req(normalization.success() && normalization.value().has_value(),
        "A durable normalization proof must be recorded");
    req(normalization.value()->excludedPrefixes == std::vector<std::string> {".qtcreator/"}
            && normalization.value()->normalizedPaths.size() == 1
            && normalization.value()->normalizedPaths.front() == ".qtcreator/CMakeLists.txt.user",
        "The normalization proof must name the real excluded prefix and forgiven path");

    // Regression for the retained bb56e001 incident: the attribution keeps
    // the historical identity while the frozen input intentionally pins the
    // normalized current identity. The read model must accept that exact,
    // immutable proof instead of hiding an otherwise valid Prepared child.
    ManifestVerificationContinuationReadModelService readModel(
        runs, ledger, continuations, baselineNormalizations, admission);
    const auto childView = readModel.viewForRun(prepared.value().childRun.id);
    req(childView.success() && childView.value().has_value()
            && childView.value()->selectedRole == "child"
            && childView.value()->childStatus == "prepared"
            && childView.value()->hasAttribution
            && childView.value()->canStartRepair,
        "The read model rejected a Prepared child backed by the exact legacy-normalization proof");
}

// MW-P5.6D negative case: a source proposal targeting a path under the
// excluded prefix itself must make the whole drift ineligible for
// normalization - never silently forgiven just because it also happens to
// fall in the approved delta.
void testLegacyBaselineNormalizationRefusesWhenAProposalTouchesTheExcludedPrefix()
{
    TempDir workspace;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    InMemoryManifestVerificationBaselineNormalizationRepository normalizations;
    ManifestVerificationLegacyBaselineNormalizationService service(
        normalizations, artifacts, proposals);

    std::error_code createError;
    std::filesystem::create_directories(workspace.path / ".qtcreator", createError);
    req(!createError, "Could not create the .qtcreator fixture directory");
    {
        std::ofstream qtcreator(workspace.path / ".qtcreator" / "CMakeLists.txt.user",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(qtcreator), "Could not write the .qtcreator fixture file");
        qtcreator << "<original-session/>";
    }

    auto legacyPolicy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    legacyPolicy.deniedDirectoryNames.erase(
        std::remove(legacyPolicy.deniedDirectoryNames.begin(),
            legacyPolicy.deniedDirectoryNames.end(), std::string(".qtcreator")),
        legacyPolicy.deniedDirectoryNames.end());
    const auto legacyBaseline = WorkspaceBaselineProvenance::capture(legacyPolicy);
    req(legacyBaseline.success(), "Could not capture the legacy baseline");
    const auto legacyBaselineJson =
        WorkspaceBaselineProvenance::renderJson(legacyBaseline.value());
    req(legacyBaselineJson.success(), "Could not render the legacy baseline artifact");
    req(artifacts.createArtifact("source-run", std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", legacyBaselineJson.value())
            .success(),
        "Could not persist the durable legacy baseline artifact");

    const auto currentBaseline = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(workspace.path));
    req(currentBaseline.success()
            && currentBaseline.value().fingerprint != legacyBaseline.value().fingerprint,
        "Test fixture did not produce baseline drift");

    const auto safe = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, "source-run", "project-1", "chg-safe",
        "qml/App.qml", "Item {}"));
    req(safe.success() && safe.value().state == ChangeSetState::ReadyForReview,
        "Could not create the safe source proposal");

    const auto mismatchedArtifact = service.normalizeOrEvaluate(
        "source-run", std::string(64, 'c'), currentBaseline.value(),
        {safe.value().changeSetId});
    req(mismatchedArtifact.success() && !mismatchedArtifact.value().has_value(),
        "A baseline artifact not matching the Work Ledger fingerprint must never mint a proof");

    const auto proved = service.normalizeOrEvaluate(
        "source-run", legacyBaseline.value().fingerprint, currentBaseline.value(),
        {safe.value().changeSetId});
    req(proved.success() && proved.value().has_value(),
        "The safe exact source proposal set must permit the narrow proof");

    // A proposal that (unusually, but not impossibly) targets a path under
    // the excluded prefix itself.
    const auto touching = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, "source-run", "project-1", "chg-touches-qtcreator",
        ".qtcreator/newfile.txt", "should never be forgiven"));
    req(touching.success() && touching.value().state == ChangeSetState::ReadyForReview,
        "Could not create the excluded-path-touching proposal");

    const auto result = service.normalizeOrEvaluate(
        "source-run", legacyBaseline.value().fingerprint,
        currentBaseline.value(), {touching.value().changeSetId});
    req(result.success() && !result.value().has_value(),
        "An existing proof must be rechecked against the exact proposal set; a proposal "
        "targeting the excluded prefix must remain ineligible on the idempotent path");
}

void testPrepareIsIdempotentAndNeverRepeatsVerificationOnRepeatCall()
{
    TempDir workspace;
    TempDir scratch;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparation(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Prepare idempotency";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success(), "Could not create component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success(), "Could not create integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto first = preparation.prepare(source.id);
    req(first.success(), "First prepare() call must succeed");
    const auto callsAfterFirst = commandRunner.calls;
    req(callsAfterFirst > 0, "The first call must have actually run verification");

    const auto second = preparation.prepare(source.id);
    req(second.success(), "A repeat prepare() call must succeed");
    req(second.value().input.childRunId == first.value().input.childRunId
            && second.value().input.inputFingerprint == first.value().input.inputFingerprint,
        "A repeat call must return the exact same frozen input, not a new one");
    req(commandRunner.calls == callsAfterFirst,
        "A repeat prepare() call must never re-run verification or provider work");
}

void testPreparedChildRecoveryIsIdempotentAndNeverQueuesProviderWork()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(
        runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparation(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Continuation recovery";
    project.rootPath = std::filesystem::temp_directory_path();
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed recovery project");
    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Failed source";
    source.task = "Repair verification";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed recovery source Run");

    auto attributed = ManifestVerificationAttributionCheckpoint{};
    attributed.id = "verification-attribution:source-run";
    attributed.sourceRunId = source.id;
    attributed.sourceProjectId = project.id;
    attributed.status = ManifestVerificationAttributionStatus::Attributed;
    attributed.baselineFingerprint = std::string(64, 'a');
    attributed.sourceOverlayFingerprint = std::string(64, 'b');
    attributed.createdAt = now;
    ManifestVerificationFindingAttribution finding;
    finding.diagnosticCode = "verification.qml_warning";
    finding.requirementId = "qml-lint";
    finding.relativePath = "qml/App.qml";
    finding.kind = ManifestVerificationFindingAttributionKind::SourceProposalOwner;
    finding.ownerTaskId = "task-1";
    finding.ownerProposalId = "proposal-1";
    attributed.findings.push_back(finding);
    req(continuations.insertAttribution(attributed).success(),
        "Could not seed recovery attribution");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = source.id;
    input.childRunId = "reserved-child";
    input.sourceProjectId = project.id;
    input.attributionId = attributed.id;
    input.baselineFingerprint = attributed.baselineFingerprint;
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = attributed.sourceOverlayFingerprint;
    input.orderedProposalIds = {"proposal-1"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = now;
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(continuations.insertInput(input).success(), "Could not seed recovery input");

    const auto first = preparation.reconcileMissingChildren();
    req(first.success() && first.value().recreatedChildRunIds
            == std::vector<RunId>{"reserved-child"},
        "Recovery did not recreate the exact reserved child id");
    const auto child = runs.findRun("reserved-child");
    req(child.success() && child.value().has_value()
            && child.value()->status == RunStatus::Prepared
            && child.value()->parentRunId == std::optional<RunId>(source.id)
            && child.value()->workflowId == kManifestVerificationContinuationWorkflowId,
        "Recovered child does not carry the prepared continuation identity");
    const auto unfinished = runs.listUnfinishedRuns();
    req(unfinished.success() && unfinished.value().empty(),
        "Prepared continuation child must not enter startup interruption recovery");
    const auto second = preparation.reconcileMissingChildren();
    req(second.success() && second.value().recreatedChildRunIds.empty()
            && commandRunner.calls == 0,
        "Repeated recovery was not idempotent or invoked verification/provider work");
}

void testTerminalContinuationChildSurvivesReconciliationWithoutConflictOrMutation()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(
        runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparation(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Terminal continuation recovery";
    project.rootPath = std::filesystem::temp_directory_path();
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed recovery project");
    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Failed source";
    source.task = "Repair verification";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed recovery source Run");

    auto attributed = ManifestVerificationAttributionCheckpoint{};
    attributed.id = "verification-attribution:source-run";
    attributed.sourceRunId = source.id;
    attributed.sourceProjectId = project.id;
    attributed.status = ManifestVerificationAttributionStatus::Attributed;
    attributed.baselineFingerprint = std::string(64, 'a');
    attributed.sourceOverlayFingerprint = std::string(64, 'b');
    attributed.createdAt = now;
    ManifestVerificationFindingAttribution finding;
    finding.diagnosticCode = "verification.qml_warning";
    finding.requirementId = "qml-lint";
    finding.relativePath = "qml/App.qml";
    finding.kind = ManifestVerificationFindingAttributionKind::SourceProposalOwner;
    finding.ownerTaskId = "task-1";
    finding.ownerProposalId = "proposal-1";
    attributed.findings.push_back(finding);
    req(continuations.insertAttribution(attributed).success(),
        "Could not seed recovery attribution");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = source.id;
    input.childRunId = "reserved-child";
    input.sourceProjectId = project.id;
    input.attributionId = attributed.id;
    input.baselineFingerprint = attributed.baselineFingerprint;
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = attributed.sourceOverlayFingerprint;
    input.orderedProposalIds = {"proposal-1"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = now;
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(continuations.insertInput(input).success(), "Could not seed recovery input");

    // Simulate the normal end state of a real repair continuation: the
    // reserved child already ran to completion (Failed here; Completed is
    // symmetric) well before this restart, not merely reserved-but-untouched.
    Run child = source;
    child.id = input.childRunId;
    child.parentRunId = source.id;
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.title = source.title + " - verification continuation";
    child.status = RunStatus::Failed;
    child.progress = 1.0;
    child.errorMessage = "req-cmake-build failed";
    child.createdAt = now;
    child.startedAt = now;
    child.finishedAt = now;
    req(runs.insertRun(child).success(), "Could not seed the terminal continuation child");

    const auto reconciled = preparation.reconcileMissingChildren();
    req(reconciled.success(), "Reconciliation must not fail for a terminal continuation child");
    req(reconciled.value().recreatedChildRunIds.empty(),
        "A terminal continuation child must not be treated as missing/recreated");
    req(commandRunner.calls == 0,
        "Reconciling a terminal continuation child must never invoke verification/provider work");

    const auto reread = runs.findRun(input.childRunId);
    req(reread.success() && reread.value().has_value()
            && reread.value()->status == RunStatus::Failed
            && reread.value()->errorMessage == "req-cmake-build failed"
            && reread.value()->finishedAt == std::optional(now),
        "Reconciliation must not mutate a terminal continuation child's own state");
}

void testReadModelEligibilityIsReadOnlyAndBackendOwned()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));
    ManifestVerificationContinuationReadModelService readModel(
        fx.runs, fx.workLedger, fx.continuations, fx.baselineNormalizations, fx.service);

    const auto view = readModel.viewForRun("run-1");
    req(view.success() && view.value().has_value() && view.value()->canContinue
            && !view.value()->hasDurableCheckpoint
            && view.value()->selectedRole == "source",
        "Read-only Backend eligibility was not projected for an eligible failed Run");
    const auto checkpoint = fx.continuations.findBySourceRun("run-1");
    req(checkpoint.success() && !checkpoint.value().has_value(),
        "Opening the continuation read model must not create a durable checkpoint");
}

void testReadModelProjectsLineageEvidenceAndBlocksPreExistingDebt()
{
    Fixture fx;
    const auto now = std::chrono::system_clock::now();
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));
    req(fx.service.admitOrEvaluate("run-1").success(), "Could not admit source fixture");

    ManifestVerificationAttributionCheckpoint attribution;
    attribution.id = "verification-attribution:run-1";
    attribution.sourceRunId = "run-1";
    attribution.sourceProjectId = "project-1";
    attribution.status = ManifestVerificationAttributionStatus::Attributed;
    attribution.baselineFingerprint = std::string(64, 'a');
    attribution.sourceOverlayFingerprint = std::string(64, 'b');
    attribution.createdAt = now;
    attribution.findings.push_back({"verification.qml_warning", "qml-lint",
        "qml/Legacy.qml", 4, 2, "unused-imports",
        ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt,
        std::nullopt, std::nullopt});
    req(fx.continuations.insertAttribution(attribution).success(),
        "Could not seed debt attribution");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = "run-1";
    input.childRunId = "child-1";
    input.sourceProjectId = "project-1";
    input.attributionId = attribution.id;
    input.baselineFingerprint = attribution.baselineFingerprint;
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = attribution.sourceOverlayFingerprint;
    input.orderedProposalIds = {"cs-1"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = now;
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(fx.continuations.insertInput(input).success(), "Could not seed continuation input");
    Run child = *fx.runs.findRun("run-1").value();
    child.id = "child-1";
    child.parentRunId = "run-1";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.status = RunStatus::Prepared;
    req(fx.runs.insertRun(child).success(), "Could not seed continuation child");

    ManifestVerificationContinuationReadModelService readModel(
        fx.runs, fx.workLedger, fx.continuations, fx.baselineNormalizations, fx.service);
    const auto sourceView = readModel.viewForRun("run-1");
    req(sourceView.success() && sourceView.value().has_value()
            && !sourceView.value()->canContinue
            && !sourceView.value()->canStartRepair
            && sourceView.value()->plannerSkipped
            && sourceView.value()->childRunId == "child-1"
            && sourceView.value()->blockCode
                == "verification_continuation.pre_existing_debt_requires_prerequisite"
            && sourceView.value()->criteria.size() == 1
            && sourceView.value()->findings.size() == 1,
        "Source projection lost lineage, criterion, attribution, or debt blocking");
    const auto childView = readModel.viewForRun("child-1");
    req(childView.success() && childView.value().has_value()
            && childView.value()->selectedRole == "child"
            && childView.value()->sourceRunId == "run-1"
            && !childView.value()->canContinue,
        "Child projection did not preserve its immutable source lineage");
}

// QML-P0: a terminal Failed continuation child whose own final combined
// verification is safety-clean, build-clean, and only lint-failed with an
// Advisory-only finding must project contentOutcome="needs_manual_action"
// plus the exact remaining finding - the real shape of the 2026-09-17 paid
// canary child bdbe0f7d-5c4c-4cf9-9dd5-1eb554abb4d4.
void testReadModelProjectsNeedsManualActionForBoundedAdvisoryChildFailure()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));
    req(fx.service.admitOrEvaluate("run-1").success(), "Could not admit source fixture");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = "run-1";
    input.childRunId = "child-1";
    input.sourceProjectId = "project-1";
    input.attributionId = "verification-attribution:run-1";
    input.baselineFingerprint = std::string(64, 'a');
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = std::string(64, 'b');
    input.orderedProposalIds = {"cs-1"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = std::chrono::system_clock::now();
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(fx.continuations.insertInput(input).success(), "Could not seed continuation input");

    Run child = *fx.runs.findRun("run-1").value();
    child.id = "child-1";
    child.parentRunId = "run-1";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.status = RunStatus::Failed;
    req(fx.runs.insertRun(child).success(), "Could not seed terminal continuation child");

    ManifestProposalSetVerification finalVerification;
    finalVerification.workspaceIdentity = "workspace-1";
    finalVerification.baselineFingerprint = "baseline-1";
    finalVerification.overlayFingerprint = "overlay-1";
    finalVerification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    finalVerification.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    finalVerification.acceptanceStatus = ExecutionVerificationStatus::Failed;
    ManifestVerificationCheckResult buildCheck;
    buildCheck.profileId = "build";
    buildCheck.kind = ExecutionVerificationKind::CMakeBuild;
    buildCheck.status = ExecutionVerificationStatus::Passed;
    ManifestVerificationCheckResult lintCheck;
    lintCheck.profileId = "qml-lint";
    lintCheck.kind = ExecutionVerificationKind::QmlLint;
    lintCheck.status = ExecutionVerificationStatus::Failed;
    finalVerification.checks = {buildCheck, lintCheck};
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access"});
    finalVerification.diagnostics = {diagnostic};
    fx.seedLedgerEntry("child-1", ManifestWorkLedgerDisposition::ManifestReady, finalVerification);

    ManifestVerificationContinuationReadModelService readModel(
        fx.runs, fx.workLedger, fx.continuations, fx.baselineNormalizations, fx.service);
    const auto childView = readModel.viewForRun("child-1");
    req(childView.success() && childView.value().has_value(),
        "Could not project the terminal continuation child");
    req(childView.value()->contentOutcome == "needs_manual_action",
        "A safety-clean, build-clean, Advisory-only lint failure must project "
        "needs_manual_action");
    req(childView.value()->remainingFindings.size() == 1
            && childView.value()->remainingFindings.front().tag == "unqualified"
            && childView.value()->remainingFindings.front().severity == "advisory"
            && childView.value()->remainingFindings.front().relativePath
                == "<candidate>/qml/AppSidebar.qml"
            && childView.value()->remainingFindings.front().line == 4
            && childView.value()->remainingFindings.front().column == 1
            && childView.value()->remainingFindings.front().message == "Unqualified access",
        "The remaining Advisory finding must be projected with its real tag/severity/path/"
        "line/column/message - enough evidence for a human to actually fix it");
}

// QML-P0 negative case: a Hard finding anywhere in the same failed check
// must fail closed to the plain, undifferentiated projection - never
// NeedsManualAction. An independent source/child pair (a source Run only
// ever has one linked child) rather than reusing the positive test's.
void testReadModelKeepsUndifferentiatedProjectionWhenAHardFindingRemains()
{
    Fixture fx;
    fx.makeRun("run-2", RunStatus::Failed);
    fx.seedProposal("run-2", "cs-2", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-2", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-2"}));
    req(fx.service.admitOrEvaluate("run-2").success(), "Could not admit source fixture");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = "run-2";
    input.childRunId = "child-2";
    input.sourceProjectId = "project-1";
    input.attributionId = "verification-attribution:run-2";
    input.baselineFingerprint = std::string(64, 'a');
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = std::string(64, 'b');
    input.orderedProposalIds = {"cs-2"};
    input.orderedProposalContentHashes = {std::string(64, 'c')};
    input.createdAt = std::chrono::system_clock::now();
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(fx.continuations.insertInput(input).success(), "Could not seed continuation input");

    Run child = *fx.runs.findRun("run-2").value();
    child.id = "child-2";
    child.parentRunId = "run-2";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.status = RunStatus::Failed;
    req(fx.runs.insertRun(child).success(), "Could not seed terminal continuation child");

    ManifestProposalSetVerification finalVerification;
    finalVerification.workspaceIdentity = "workspace-1";
    finalVerification.baselineFingerprint = "baseline-1";
    finalVerification.overlayFingerprint = "overlay-1";
    finalVerification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    finalVerification.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    finalVerification.acceptanceStatus = ExecutionVerificationStatus::Failed;
    ManifestVerificationCheckResult buildCheck;
    buildCheck.profileId = "build";
    buildCheck.kind = ExecutionVerificationKind::CMakeBuild;
    buildCheck.status = ExecutionVerificationStatus::Passed;
    ManifestVerificationCheckResult lintCheck;
    lintCheck.profileId = "qml-lint";
    lintCheck.kind = ExecutionVerificationKind::QmlLint;
    lintCheck.status = ExecutionVerificationStatus::Failed;
    finalVerification.checks = {buildCheck, lintCheck};
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings = {
        {"<candidate>/qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access"},
        {"<candidate>/qml/PageHost.qml", 1, 1, "import", "Failed to import module"}};
    finalVerification.diagnostics = {diagnostic};
    fx.seedLedgerEntry("child-2", ManifestWorkLedgerDisposition::ManifestReady, finalVerification);

    ManifestVerificationContinuationReadModelService readModel(
        fx.runs, fx.workLedger, fx.continuations, fx.baselineNormalizations, fx.service);
    const auto childView = readModel.viewForRun("child-2");
    req(childView.success() && childView.value().has_value()
            && childView.value()->contentOutcome == "undifferentiated"
            && childView.value()->remainingFindings.empty(),
        "A Hard finding must keep the projection undifferentiated, never NeedsManualAction");
}

void testReadModelFailsClosedForOrphanedContinuationChild()
{
    Fixture fx;
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));
    Run child = fx.makeRun(
        "child-1", RunStatus::Prepared,
        std::string(kManifestVerificationContinuationWorkflowId));
    child.parentRunId = "run-1";
    const auto persistedChild = fx.runs.findRun("child-1");
    req(persistedChild.success() && persistedChild.value().has_value(),
        "Could not seed orphaned continuation child");
    child.revision = persistedChild.value()->revision;
    req(fx.runs.updateRun(child).success(), "Could not link orphaned continuation child");

    ManifestVerificationContinuationReadModelService readModel(
        fx.runs, fx.workLedger, fx.continuations, fx.baselineNormalizations, fx.service);
    const auto view = readModel.viewForRun("child-1");
    req(!view.success()
            && view.error()->code == "verification_continuation.read_input_corrupt",
        "A continuation child without its immutable input must fail closed");
}

void seedReadModelNormalizationMismatch(Fixture& fx, bool insertWrongProof)
{
    const auto now = std::chrono::system_clock::now();
    fx.makeRun("run-1", RunStatus::Failed);
    fx.seedProposal("run-1", "cs-1", ChangeSetState::ReadyForReview);
    fx.seedLedgerEntry("run-1", ManifestWorkLedgerDisposition::ManifestReady,
        fx.makeFailedVerification({"cs-1"}));
    req(fx.service.admitOrEvaluate("run-1").success(), "Could not admit source fixture");

    ManifestVerificationAttributionCheckpoint attribution;
    attribution.id = "verification-attribution:run-1";
    attribution.sourceRunId = "run-1";
    attribution.sourceProjectId = "project-1";
    attribution.status = ManifestVerificationAttributionStatus::Attributed;
    attribution.baselineFingerprint = std::string(64, 'a');
    attribution.sourceOverlayFingerprint = std::string(64, 'b');
    attribution.createdAt = now;
    attribution.findings.push_back({"verification.qml_warning", "qml-lint",
        "qml/App.qml", 4, 1, "unqualified",
        ManifestVerificationFindingAttributionKind::SourceProposalOwner,
        std::optional<std::string>("task-1"), std::optional<std::string>("cs-1")});
    req(fx.continuations.insertAttribution(attribution).success(),
        "Could not seed normalized attribution fixture");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = "run-1";
    input.childRunId = "child-1";
    input.sourceProjectId = "project-1";
    input.attributionId = attribution.id;
    input.baselineFingerprint = std::string(64, 'c');
    input.sourceWorkspaceIdentity = "workspace-1";
    input.sourceOverlayFingerprint = std::string(64, 'd');
    input.orderedProposalIds = {"cs-1"};
    input.orderedProposalContentHashes = {std::string(64, 'e')};
    input.createdAt = now;
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(fx.continuations.insertInput(input).success(),
        "Could not seed normalized continuation input fixture");

    Run child = *fx.runs.findRun("run-1").value();
    child.id = "child-1";
    child.parentRunId = "run-1";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.status = RunStatus::Prepared;
    req(fx.runs.insertRun(child).success(), "Could not seed normalized continuation child");

    if (insertWrongProof) {
        ManifestVerificationBaselineNormalization proof;
        proof.id = "verification-baseline-normalization:run-1";
        proof.sourceRunId = "run-1";
        proof.policyId = std::string(kLegacyBaselineNormalizationPolicyId);
        proof.durableBaselineFingerprint = std::string(64, 'f');
        proof.normalizedBaselineFingerprint = input.baselineFingerprint;
        proof.excludedPrefixes = {".qtcreator/"};
        proof.normalizedPaths = {".qtcreator/CMakeLists.txt.user"};
        proof.createdAt = now;
        req(fx.baselineNormalizations.insert(proof).success(),
            "Could not seed wrong-pair normalization proof");
    }
}

void testReadModelRejectsNormalizedIdentityWithoutExactProof()
{
    for (const bool insertWrongProof : {false, true}) {
        Fixture fx;
        seedReadModelNormalizationMismatch(fx, insertWrongProof);
        ManifestVerificationContinuationReadModelService readModel(
            fx.runs, fx.workLedger, fx.continuations, fx.baselineNormalizations, fx.service);
        const auto view = readModel.viewForRun("child-1");
        req(!view.success()
                && view.error()->code == "verification_continuation.read_attribution_corrupt",
            "A normalized continuation identity without its exact proof did not fail closed");
    }
}

// ---------------------------------------------------------------------
// MW-P5.4 (ManifestVerificationContinuationRepairService): a real, scripted
// end-to-end repair - no network, no CURL, no real provider adapter. Shape
// mirrors ExecutionManifestExecutorTests.cpp's own ScriptedProviderAgentSession
// fixture (kept local since every test file in this codebase is
// self-contained).
// ---------------------------------------------------------------------

class NoOpToolExecutor final : public IWorkspaceToolExecutor {
public:
    common::Result<AgentToolResult> execute(
        const AgentToolCall& call, const WorkspaceReadPolicy&) override
    {
        AgentToolResult r;
        r.callId = call.callId;
        r.toolName = call.toolName;
        r.success = false;
        r.errorCode = "test.unexpected_tool_call";
        return common::Result<AgentToolResult>::ok(std::move(r));
    }
};

class ScriptedProviderAgentSession final : public ai::providers::IProviderAgentSession {
public:
    std::deque<common::Result<ai::providers::ProviderAgentTurnResponse>> responses;
    // AG-17.2: shared (the session itself is destroyed by the turn loop) record of the
    // exact provider session request the Executor built for this worker.
    std::shared_ptr<std::vector<ai::providers::ProviderAgentSessionRequest>> capturedRequests;

    common::Result<ai::providers::ProviderAgentTurnResponse> beginSession(
        const ai::providers::ProviderAgentSessionRequest& request, std::stop_token) override
    {
        if (capturedRequests) capturedRequests->push_back(request);
        return nextResponse();
    }
    common::Result<ai::providers::ProviderAgentTurnResponse> submitToolResults(
        std::vector<AgentToolResult>, std::stop_token,
        std::optional<ai::providers::NextTurnToolPolicy> = std::nullopt) override
    {
        return nextResponse();
    }

private:
    common::Result<ai::providers::ProviderAgentTurnResponse> nextResponse()
    {
        if (responses.empty()) {
            return common::Result<ai::providers::ProviderAgentTurnResponse>::failure(
                {"test.no_more_responses", "ScriptedProviderAgentSession ran out of responses"});
        }
        auto next = std::move(responses.front());
        responses.pop_front();
        return next;
    }
};

class ScriptedProviderAgentSessionFactory final : public ai::providers::IProviderAgentSessionFactory {
public:
    std::deque<std::unique_ptr<ScriptedProviderAgentSession>> preparedSessions;
    // AG-17.2: the (providerId, providerKind) every session was created for.
    std::vector<std::pair<ProviderId, std::string>> createdFor;

    common::Result<std::unique_ptr<ai::providers::IProviderAgentSession>> createSession(
        const ProviderId& providerId, const std::string& providerKind) override
    {
        createdFor.emplace_back(providerId, providerKind);
        if (preparedSessions.empty()) {
            return common::Result<std::unique_ptr<ai::providers::IProviderAgentSession>>::failure(
                {"test.no_more_sessions", "ScriptedProviderAgentSessionFactory ran out of prepared sessions"});
        }
        auto session = std::move(preparedSessions.front());
        preparedSessions.pop_front();
        return common::Result<std::unique_ptr<ai::providers::IProviderAgentSession>>::ok(
            std::move(session));
    }
};

ai::providers::ProviderAgentTurnResponse repairStopResponse(std::string text)
{
    ai::providers::ProviderAgentTurnResponse resp;
    resp.finishReason = ai::providers::ProviderAgentFinishReason::Stop;
    resp.assistantText = std::move(text);
    resp.usage.inputTokens = 10;
    resp.usage.outputTokens = 5;
    return resp;
}

// A single-file create proposal - the shape a real repair worker submits to
// amend exactly one owned/planned-create file of its predecessor. The
// predecessor's own change (qml/App.qml) is itself a Create - the real
// baseline never had this file - so the genuine amendment operation a repair
// worker would submit for it is also Create, never Modify (which would
// require a live baseline file to hash).
std::string repairCreateProposalJson(const std::string& path, const std::string& content)
{
    return "```json\n"
        "{\"schemaVersion\":\"proposal-submission.v2\","
        "\"summary\":\"Fix the unqualified access.\",\"changes\":[{"
        "\"change_id\":\"create-1\",\"operation\":\"create\","
        "\"relative_path\":\"" + path + "\",\"source_path\":null,"
        "\"proposed_content\":\"" + content + "\"}]}\n"
        "```";
}

// AG-17.0: `anchorInsideRepairSet` chooses where the source Manifest's one
// qml_lint requirement (and the mandatory-verification decision that names it)
// lives. true: on the repaired `component` task, as a Backend-injected
// (origin=backend_mandatory) requirement - the repair Manifest must keep it,
// with its provenance, and carry the record field for field. false: on the
// non-repaired `integration` task as a Planner-declared requirement the policy
// merely recognised - the repair subset does not contain it, so the record must
// be omitted (never left dangling) while the child still freezes.
void runRepairMaterializationScenario(bool anchorInsideRepairSet, bool rosterRouting = false)
{
    TempDir workspace;
    TempDir scratch;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    ExecutionManifestService manifestService(manifestRepository, runs);
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    FakeChangeProposalRepository proposalRepository;
    // Deliberately OUTSIDE workspace.path - matching BackendRuntime's real
    // production layout (databasePath.parent_path() / "proposals", never
    // inside the project workspace). repair()'s own final combined
    // verification re-captures the real workspace baseline and fails closed
    // on any drift; publishing the repair's successor proposal under
    // workspace.path itself would make that capture legitimately (and
    // correctly) detect its own proposal store as workspace drift.
    auto storeResult = FileChangeProposalStore::open(scratch.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestWorkLedgerService workLedgerService(
        ledger, runs, manifestRepository, taskExecutions, proposalRepository, &artifacts);

    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifestRepository, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparation(
        runs, projects, manifestRepository, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);

    NoOpToolExecutor toolExecutor;
    InMemoryAgentToolSessionRepository toolSessions;
    InMemoryAgentToolAuditRepository toolAudits;
    auto registry = ToolRegistry::createDefaultReadOnly();
    req(registry.success(), "Could not create tool registry");
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), toolExecutor, toolSessions, toolAudits, true);
    ScriptedProviderAgentSessionFactory sessionFactory;

    ManifestVerificationContinuationRepairService repairService(
        runs, projects, manifestService, ledger, continuations, taskExecutions, providers, models,
        proposals, workLedgerService, artifacts, sessionFactory, toolExecution, commandRunner,
        nullptr, scratch.path);
    outcomebrief_hooks::RecordingTrigger outcomeBriefTrigger;   // PC-4C: records the nudges to the Outcome Brief
    preparation.setOutcomeBriefTrigger(&outcomeBriefTrigger);
    repairService.setOutcomeBriefTrigger(&outcomeBriefTrigger);

    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, nullptr, &continuations);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Repair happy path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Provider provider;
    provider.id = "openai";
    provider.name = "OpenAI";
    provider.kind = "openai";
    provider.createdAt = now;
    provider.updatedAt = now;
    req(providers.insert(provider).success(), "Could not seed provider");
    Model model;
    model.id = "gpt-4-test";
    model.providerId = provider.id;
    model.apiModelId = "gpt-4-test";
    model.displayName = "GPT-4 test";
    model.maxOutputTokens = 4096;
    req(models.insert(model).success(), "Could not seed model");

    Provider leadProvider;
    Model leadModel;
    Provider integrationProvider;
    Model integrationModel;
    if (rosterRouting) {
        leadProvider.id = "lead-provider";
        leadProvider.name = "Lead Provider";
        leadProvider.kind = "anthropic";
        leadProvider.createdAt = now;
        leadProvider.updatedAt = now;
        req(providers.insert(leadProvider).success(), "Could not seed lead provider");
        leadModel.id = "lead-model";
        leadModel.providerId = leadProvider.id;
        leadModel.apiModelId = "lead-model";
        leadModel.displayName = "Lead model";
        leadModel.maxOutputTokens = 8192;
        req(models.insert(leadModel).success(), "Could not seed lead model");

        integrationProvider.id = "integration-provider";
        integrationProvider.name = "Integration Provider";
        integrationProvider.kind = "openai_compatible";
        integrationProvider.createdAt = now;
        integrationProvider.updatedAt = now;
        req(providers.insert(integrationProvider).success(),
            "Could not seed integration provider");
        integrationModel.id = "integration-model";
        integrationModel.providerId = integrationProvider.id;
        integrationModel.apiModelId = "integration-model";
        integrationModel.displayName = "Integration model";
        integrationModel.maxOutputTokens = 8192;
        req(models.insert(integrationModel).success(), "Could not seed integration model");
    }

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    StageModelAssignment assignment;
    assignment.stageType = "implementation";
    assignment.providerId = provider.id;
    assignment.modelId = model.id;
    assignment.role = "qml_component_author";
    assignment.timeout = std::chrono::milliseconds {5000};
    assignment.workspaceAccess = true;
    assignment.position = rosterRouting ? 1 : 0;
    if (rosterRouting) {
        StageModelAssignment lead;
        lead.stageType = "architect";
        lead.providerId = leadProvider.id;
        lead.modelId = leadModel.id;
        lead.role = "lead";
        lead.instructions = "Plan only.";
        lead.timeout = std::chrono::milliseconds {5000};
        lead.leadArchitect = true;
        lead.position = 0;
        StageModelAssignment integrationAssignment;
        integrationAssignment.stageType = "documentation";
        integrationAssignment.providerId = integrationProvider.id;
        integrationAssignment.modelId = integrationModel.id;
        integrationAssignment.role = "qml_integration";
        integrationAssignment.instructions = "Own integration work.";
        integrationAssignment.timeout = std::chrono::milliseconds {5000};
        integrationAssignment.workspaceAccess = true;
        integrationAssignment.position = 2;
        source.assignmentSnapshot = {lead, assignment, integrationAssignment};
    } else {
        source.assignmentSnapshot = {assignment};
    }
    // PC-2: the source Run froze interface contracts; the continuation child copies the whole Run, so the
    // repair worker must receive the ones that cover its file (and not the one scoped elsewhere).
    {
        RunInterfaceContract covering;
        covering.id = "RC1";
        covering.title = "App shape";
        covering.text = "REPAIR-CONTRACT-BODY keep the root id";
        covering.appliesToPaths = {"qml"};
        RunInterfaceContract elsewhere;
        elsewhere.id = "RC2";
        elsewhere.title = "Docs rule";
        elsewhere.text = "REPAIR-OFFSCOPE-BODY";
        elsewhere.appliesToPaths = {"docs"};
        source.interfaceContracts = {covering, elsewhere};
    }
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    requirement.qmlLintDiagnosticPolicyId =
        std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
    // The frozen selection identity a real bound requirement carries (the
    // repair Manifest's own freeze validates it when the requirement is copied).
    requirement.verificationProfileId = "debug";
    requirement.verificationBuildProfileId = "debug";
    requirement.verificationTargetId = "all_qmllint";
    // AG-17.0 provenance: Backend-injected when it lives on the repaired task.
    requirement.origin = anchorInsideRepairSet ? ExecutionRequirementOrigin::BackendMandatory
                                               : ExecutionRequirementOrigin::PlannerDeclared;
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.objective = "Author the App root component";
    component.role = "qml_component_author";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    if (rosterRouting) component.assigneeMemberId = "implementation";
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.objective = "Wire the CMake build";
    integration.role = "qml_integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requestedCapability = WorkspaceCapability::Propose;
    if (rosterRouting) integration.assigneeMemberId = "documentation";
    // Exactly one task carries the one qml_lint requirement (the verifier runs
    // every requirement of every task, so it must not be doubled).
    (anchorInsideRepairSet ? component : integration).acceptanceContract = contract;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    // AG-17.0: the source Manifest's frozen mandatory-verification record.
    {
        ManifestMandatoryVerificationDecision decision;
        decision.kind = ExecutionVerificationKind::QmlLint;
        decision.status = anchorInsideRepairSet
            ? MandatoryVerificationDecisionStatus::Injected
            : MandatoryVerificationDecisionStatus::SatisfiedByExisting;
        decision.reasonCode = "qml_paths_written";
        decision.requirementId = "qml-lint";
        decision.taskId = anchorInsideRepairSet ? "component" : "integration";
        decision.profileId = "debug";
        decision.buildProfileId = "debug";
        decision.targetId = "all_qmllint";
        ManifestMandatoryVerificationRecord record;
        record.decisions = {decision};
        manifest.mandatoryVerification = std::move(record);
    }
    req(!ManifestMandatoryVerificationPolicy::recordIntegrityProblem(manifest).has_value(),
        "The seeded source record must itself be consistent with the source Manifest");
    // AG-17.1: the source Manifest's frozen routing record. Production builds it from
    // the SAME assignment snapshot the Run carries (and the continuation child Run is a
    // copy of the source Run), so the fixture must too - AG-17.2 resolves the repair
    // workers' route from the child's snapshot and rejects any identity drift.
    manifest.routing = ManifestRouting::fromAssignments(source.assignmentSnapshot);
    req(manifestRepository.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // prepare()'s own two verify() calls (baseline-only, then exact source
    // candidate) - clean then the reproduced warning, exactly like the P5.3
    // happy-path test above.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto prepared = preparation.prepare(source.id);
    req(prepared.success(), "A fully eligible source Run must prepare successfully");
    const RunId childRunId = prepared.value().input.childRunId;
    // PC-4C: the Failed source now has a typed continuation child, so its brief is nudged - once, about the SOURCE.
    req(outcomeBriefTrigger.runs() == std::vector<std::string> {source.id},
        "Preparing the continuation must nudge the Outcome Brief of the source Run, once");

    // The repair worker's one-shot response: a full-replacement modify
    // amending exactly the flagged file.
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    const auto repairRequests =
        std::make_shared<std::vector<ai::providers::ProviderAgentSessionRequest>>();
    repairSession->capturedRequests = repairRequests;
    repairSession->responses.push_back(common::Result<ai::providers::ProviderAgentTurnResponse>::ok(
        repairStopResponse(repairCreateProposalJson("qml/App.qml", "Item { id: root }"))));
    sessionFactory.preparedSessions.push_back(std::move(repairSession));
    // The repair() call's own final combined verification, against the
    // source Manifest's full task list: one new in-scope advisory against a
    // clean source baseline. The frozen policy must complete the child
    // without another repair/manual route.
    ManifestVerificationCommandOutcome advisoryOutcome;
    advisoryOutcome.started = true;
    advisoryOutcome.exitCode = 0;
    advisoryOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Unqualified access [unqualified]\n";
    commandRunner.queuedOutcomes.push_back(advisoryOutcome);
    commandRunner.queuedOutcomes.push_back(cleanOutcome);

    const auto repaired = repairService.repair(childRunId);
    req(repaired.success(), "A fully prepared child Run must repair successfully");
    // PC-4C: the paid child bypasses RunManager::finishRun, so finishing it nudges the brief of the CHILD, once.
    req(outcomeBriefTrigger.runCalls(childRunId) == 1 && outcomeBriefTrigger.runs().size() == 2,
        "Finishing the continuation child must nudge its own Outcome Brief exactly once");
    req(repaired.value().finalStatus == RunStatus::Completed,
        "An advisory-only final verification must complete the continuation child Run");
    req(repaired.value().repairedTaskIds == std::vector<std::string> {"component"},
        "Only the component task (the real qml/App.qml owner) must have been repaired");
    req(repaired.value().finalVerification.has_value()
            && repaired.value().finalVerification->orderedProposalIds.size() == 2,
        "The final verified set must contain exactly the two source members");
    req(std::any_of(repaired.value().finalVerification->diagnostics.begin(),
                repaired.value().finalVerification->diagnostics.end(),
                [](const ManifestVerificationDiagnostic& item) {
                    return item.code == "verification.qml_warning_advisory";
                }),
        "The paid continuation's final authority must retain its advisory diagnostic");

    const auto childRun = runs.findRun(childRunId);
    req(childRun.success() && childRun.value().has_value()
            && childRun.value()->status == RunStatus::Completed,
        "The continuation child Run must be durably Completed");
    if (rosterRouting) {
        req(childRun.value()->assignmentSnapshot.size() == 3
                && childRun.value()->assignmentSnapshot[0].stageType == "architect"
                && childRun.value()->assignmentSnapshot[1].stageType == "implementation"
                && childRun.value()->assignmentSnapshot[2].stageType == "documentation",
            "The continuation child must inherit the source roster snapshot verbatim");
    }
    // AG-17.0 provenance invariant. Two distinct guarantees: (a) the repair
    // Manifest never DERIVES or DUPLICATES a Backend requirement; (b) a frozen
    // requirement keeps its identity and origin, and the decision record never
    // dangles.
    const auto sourceFrozen = manifestRepository.findForRun(source.id);
    const auto childManifest = manifestRepository.findForRun(childRunId);
    req(sourceFrozen.success() && sourceFrozen.value().has_value()
            && childManifest.success() && childManifest.value().has_value(),
        "Both the source and the repair Manifest must be frozen");
    const auto requirementOf = [](const ExecutionManifest& frozen, const std::string& taskId,
                                  const std::string& requirementId)
        -> const ExecutionAcceptanceRequirement* {
        for (const auto& task : frozen.tasks) {
            if (task.id != taskId || !task.acceptanceContract.has_value()) continue;
            for (const auto& item : task.acceptanceContract->requirements) {
                if (item.id == requirementId) return &item;
            }
        }
        return nullptr;
    };
    const auto countRequirements = [](const ExecutionManifest& frozen, const std::string& id,
                                      std::optional<ExecutionRequirementOrigin> origin) {
        std::size_t count = 0;
        for (const auto& task : frozen.tasks) {
            if (!task.acceptanceContract.has_value()) continue;
            for (const auto& item : task.acceptanceContract->requirements) {
                if ((id.empty() || item.id == id) && (!origin.has_value() || item.origin == *origin)) {
                    ++count;
                }
            }
        }
        return count;
    };
    const auto& sourceM = *sourceFrozen.value();
    const auto& childM = *childManifest.value();

    // AG-17.1: the repair Manifest inherits the SOURCE routing record verbatim -
    // nothing is re-planned or re-resolved, and the record stays self-consistent.
    req(sourceM.routing.has_value() && childM.routing.has_value(),
        "Both the source and the repair Manifest must carry the routing record");
    req(childM.routing->fingerprint == sourceM.routing->fingerprint
            && childM.routing->mode == sourceM.routing->mode
            && childM.routing->members.size() == sourceM.routing->members.size()
            && childM.routing->members.front().memberId == sourceM.routing->members.front().memberId
            && childM.routing->members.front().providerId == sourceM.routing->members.front().providerId
            && childM.routing->members.front().modelId == sourceM.routing->members.front().modelId
            && childM.routing->members.front().instructionsHash
                == sourceM.routing->members.front().instructionsHash,
        "The repair Manifest must carry the source routing record verbatim");
    req(!ManifestRouting::recordProblem(*childM.routing).has_value(),
        "The inherited routing record must be self-consistent");
    if (rosterRouting) {
        req(sourceM.routing->mode == ManifestRoutingMode::Roster
                && childM.tasks.size() == 1
                && childM.tasks.front().assigneeMemberId
                    == std::optional<std::string>("implementation"),
            "The repair subset must preserve its source roster assignee");
    }

    // AG-17.2: the repair worker ran on the route that was frozen for the source Run
    // (resolved from the child's inherited snapshot, nothing re-chosen) ...
    req(sessionFactory.createdFor.size() == 1
            && sessionFactory.createdFor.front().first == "openai"
            && sessionFactory.createdFor.front().second == "openai",
        "The repair worker's session must be created for the frozen route's provider and kind");
    req(repairRequests->size() == 1
            && repairRequests->front().providerId == "openai"
            && repairRequests->front().modelId == "gpt-4-test"
            && repairRequests->front().apiModelId == "gpt-4-test",
        "The repair worker must run on the frozen route's provider and model");
    req(repairRequests->front().userContent.find("[contract RC1 begin,") != std::string::npos
            && repairRequests->front().userContent.find("REPAIR-CONTRACT-BODY keep the root id")
                != std::string::npos
            && repairRequests->front().userContent.find("REPAIR-OFFSCOPE-BODY") == std::string::npos,
        "The repair worker must receive the Run's interface contracts that cover its file, and only those");
    // ... and with the worker output-ceiling floor the primary workflow has used since
    // the max_output_tokens incident (the fixture model's own ceiling is 4096): the
    // continuation path used to pass the raw model value - a gap AG-17.2 closes.
    req(repairRequests->front().maxOutputTokens == kWorkerProposeMinOutputTokens,
        "A continuation repair worker must get the floor-applied output ceiling");

    if (anchorInsideRepairSet) {
        // 1. The source Manifest really holds the Backend-injected requirement.
        const auto* sourceRequirement = requirementOf(sourceM, "component", "qml-lint");
        req(sourceRequirement != nullptr
                && sourceRequirement->origin == ExecutionRequirementOrigin::BackendMandatory,
            "The source Manifest must hold the injected backend_mandatory requirement");
        // 2. The child holds the same requirement id with equivalent semantics.
        const auto* childRequirement = requirementOf(childM, "component", "qml-lint");
        req(childRequirement != nullptr, "The repair Manifest lost the injected requirement");
        req(childRequirement->verificationKind == sourceRequirement->verificationKind
                && childRequirement->required == sourceRequirement->required
                && childRequirement->description == sourceRequirement->description
                && childRequirement->requiredOutputs == sourceRequirement->requiredOutputs
                && childRequirement->verificationProfileId == sourceRequirement->verificationProfileId
                && childRequirement->verificationBuildProfileId
                    == sourceRequirement->verificationBuildProfileId
                && childRequirement->verificationTargetId == sourceRequirement->verificationTargetId
                && childRequirement->verificationBootstrapPending
                    == sourceRequirement->verificationBootstrapPending
                && childRequirement->qmlLintDiagnosticPolicyId
                    == sourceRequirement->qmlLintDiagnosticPolicyId
                && childRequirement->verificationArguments.size()
                    == sourceRequirement->verificationArguments.size()
                && std::equal(childRequirement->verificationArguments.begin(),
                       childRequirement->verificationArguments.end(),
                       sourceRequirement->verificationArguments.begin(),
                       [](const ExecutionVerificationArgument& a, const ExecutionVerificationArgument& b) {
                           return a.name == b.name && a.value == b.value;
                       }),
            "The repair Manifest's requirement must be semantically identical to the source's");
        // 3. Its provenance is preserved - it is NOT relabelled planner_declared.
        req(childRequirement->origin == ExecutionRequirementOrigin::BackendMandatory,
            "A carried Backend requirement must keep origin=backend_mandatory");
        // 4. The record is carried field for field.
        req(childM.mandatoryVerification.has_value() && sourceM.mandatoryVerification.has_value(),
            "The repair Manifest must carry the mandatory verification record");
        const auto& sourceRecord = *sourceM.mandatoryVerification;
        const auto& childRecord = *childM.mandatoryVerification;
        req(childRecord.policyId == sourceRecord.policyId && childRecord.mode == sourceRecord.mode
                && childRecord.decisions.size() == sourceRecord.decisions.size()
                && std::equal(childRecord.decisions.begin(), childRecord.decisions.end(),
                       sourceRecord.decisions.begin(),
                       [](const ManifestMandatoryVerificationDecision& a,
                          const ManifestMandatoryVerificationDecision& b) {
                           return a.kind == b.kind && a.status == b.status
                               && a.reasonCode == b.reasonCode && a.requirementId == b.requirementId
                               && a.taskId == b.taskId && a.profileId == b.profileId
                               && a.buildProfileId == b.buildProfileId && a.targetId == b.targetId;
                       }),
            "The mandatory verification record must be carried field for field");
        // 5. Every requirement a decision names exists in the child Manifest,
        //    on the named task, and the record is consistent with it.
        for (const auto& decision : childRecord.decisions) {
            if (decision.requirementId.empty()) continue;
            req(requirementOf(childM, decision.taskId, decision.requirementId) != nullptr,
                "A decision names a requirement the repair Manifest does not hold");
        }
        req(!ManifestMandatoryVerificationPolicy::recordIntegrityProblem(childM).has_value(),
            "The carried record must be consistent with the repair Manifest's own tasks");
        // 6. Nothing was derived again and nothing was duplicated.
        req(countRequirements(childM, "qml-lint", std::nullopt) == 1,
            "The repair Manifest duplicated the injected requirement");
        req(countRequirements(childM, {}, ExecutionRequirementOrigin::BackendMandatory)
                == countRequirements(sourceM, "qml-lint", ExecutionRequirementOrigin::BackendMandatory),
            "The repair Manifest must hold exactly the source's Backend requirements, no more");
        req(countRequirements(childM, "backend-mandatory-qml-lint", std::nullopt) == 0
                && countRequirements(childM, "backend-mandatory-cmake-build", std::nullopt) == 0,
            "The policy must not have been evaluated for the repair Manifest");
    } else {
        // The source's one requirement lives on the non-repaired task, so the
        // repair subset does not contain it and the record cannot be carried
        // without dangling: it is omitted, and the child still froze.
        req(sourceM.mandatoryVerification.has_value(),
            "The source Manifest keeps its own record untouched");
        req(childM.tasks.size() == 1 && childM.tasks.front().id == "component"
                && !childM.tasks.front().acceptanceContract.has_value(),
            "The repair subset must hold only the repaired task");
        req(!childM.mandatoryVerification.has_value(),
            "A record that would dangle in the repair subset must be omitted, not carried");
        req(countRequirements(childM, {}, ExecutionRequirementOrigin::BackendMandatory) == 0,
            "No Backend requirement may appear in a repair Manifest that was not copied");
    }
    const auto childStages = runs.listStages(childRunId);
    req(childStages.success() && childStages.value().size() == 1
            && childStages.value().front().status == StageStatus::Completed
            && childStages.value().front().progress == 1.0
            && childStages.value().front().startedAt.has_value()
            && childStages.value().front().finishedAt.has_value(),
        "The continuation stage must cross the same durable Running-to-Completed boundary as "
        "its child Run");

    const auto& finalIds = repaired.value().finalVerification->orderedProposalIds;
    const ChangeSetId successorId = finalIds.front() == integrationProposal.value().changeSetId
        ? finalIds.back() : finalIds.front();
    req(successorId != componentProposal.value().changeSetId,
        "The final set must reference the NEW successor, never the original repaired predecessor");
    const auto successorChanges = proposals.listChanges(successorId);
    req(successorChanges.success() && successorChanges.value().size() == 1,
        "The successor must carry exactly one materialized change");
    const auto successorContent = proposals.readProposedContent(
        successorId, successorChanges.value().front().changeId);
    req(successorContent.success() && successorContent.value() == "Item { id: root }",
        "The successor must carry the repair worker's fixed content");

    // AG-16 (MW-P5.4 extension): both the cross-Run genuinely-unchanged
    // reference (integration, still living on the source Run) and the
    // child-run-native successor must be independently Apply-eligible.
    const auto integrationEligibility = applyGate.evaluate(integrationProposal.value().changeSetId);
    req(integrationEligibility.allowed
            && integrationEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "An unchanged cross-Run reference member must be Apply-eligible through the typed "
        "continuation checkpoint chain");
    const auto successorEligibility = applyGate.evaluate(successorId);
    req(successorEligibility.allowed
            && successorEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The child-run-native successor must be Apply-eligible");

    // The stale, superseded source predecessor must never be independently
    // Apply-eligible again through this gate - it is no longer a member of
    // the child Run's own verified set.
    const auto staleEligibility = applyGate.evaluate(componentProposal.value().changeSetId);
    req(!staleEligibility.allowed,
        "A superseded source predecessor must never remain Apply-eligible after repair");
}

// QML-P4 package 2 end-to-end happy path: a real workspace, real proposals,
// a real baseline capture, and real ManifestProposalOverlay/
// ManifestProposalSetVerifier calls - the same shape as
// testPrepareCreatesInputAndPreparedChildForAFullyEligibleRun above, but
// exercising materialize()'s OWN divergence: a durable candidate directory
// (never cleaned up), and allowedPaths restricted to the one owner finding's
// Create target (qml/App.qml) - never the untouched integration proposal's
// own qml/CMakeLists.txt target, which carries no finding at all.
void testMaterializeProducesDurableHandoffRestrictedToOwnerFindingTargets()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Materialize happy path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requiredReadPaths = {"qml/App.qml"};
    integration.dependencies = {"component"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome baselineOutsideOutcome;
    baselineOutsideOutcome.started = true;
    baselineOutsideOutcome.exitCode = 0;
    baselineOutsideOutcome.boundedOutput =
        "Warning: <candidate>/qml/Outside.qml:3:1: Unqualified access [unqualified]\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/Outside.qml:3:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {baselineOutsideOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;
    req(handoff.originManifestRunId == source.id
            && handoff.verificationAnchorRunId == source.id
            && handoff.sourceProjectId == project.id,
        "The handoff must carry the exact source identity");
    req(handoff.frozenMembers.size() == overlayRequest.orderedProposalIds.size(),
        "The handoff must freeze exactly the real source proposal set");
    for (std::size_t i = 0; i < handoff.frozenMembers.size(); ++i) {
        req(handoff.frozenMembers[i].proposalId == overlayRequest.orderedProposalIds[i]
                && handoff.frozenMembers[i].owningRunId == source.id
                && handoff.frozenMembers[i].contentHash == contentHashes[i]
                && handoff.frozenMembers[i].lineageRootProposalId
                    == overlayRequest.orderedProposalIds[i],
            "Each frozen member must carry the exact real source proposal identity, owning Run, "
            "and (never-yet-superseded) lineage root");
    }
    req(handoff.allowedPaths.size() == 1
            && handoff.allowedPaths.front().relativePath == "qml/App.qml"
            && handoff.allowedPaths.front().ownerProposalId
                == componentProposal.value().changeSetId
            && handoff.allowedPaths.front().ownerTaskId == "component",
        "Only the real owner finding's Create target must be editable - never the untouched "
        "integration proposal's own target, which carries no finding");

    req(std::filesystem::exists(materialized.value().candidateRoot / "qml" / "App.qml"),
        "The materialized candidate must contain the real component file");
    req(std::filesystem::exists(materialized.value().candidateRoot / "qml" / "CMakeLists.txt"),
        "The materialized candidate must contain the real integration file too - the whole "
        "workspace baseline plus proposals, not only the editable subset");
    {
        std::ifstream appQml(materialized.value().candidateRoot / "qml" / "App.qml");
        std::ostringstream content;
        content << appQml.rdbuf();
        req(content.str() == "Item {}", "The materialized candidate file must carry real content");
    }

    // Idempotent: a second call for the same source Run must return the
    // SAME durable handoff, never re-materialize or duplicate it.
    const auto second = manualHandoffService.materialize(source.id);
    req(second.success() && second.value().handoff.id == handoff.id
            && second.value().handoff.storageKey == handoff.storageKey
            && second.value().candidateRoot == materialized.value().candidateRoot,
        "A repeat materialize() call must return the same durable handoff");

    const auto resolved = manualHandoffService.resolveCandidateRoot(handoff);
    req(resolved.success() && resolved.value() == materialized.value().candidateRoot,
        "resolveCandidateRoot must re-derive the exact same real directory");
}

// QML-P4R.6: the exact realistic Qt-generated build-directory name observed
// in the retained-Run canary investigation - kept as a named constant so
// every test below that reproduces the incident's shape uses the identical
// string.
constexpr const char* kRealisticQtBuildDirectory =
    "build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug";

// QML-P4R.6: pads a TempDir's own (short) path with one extra long path
// segment so the padded root, combined with kRealisticQtBuildDirectory,
// reliably exceeds ManifestProposalSetVerifier.cpp's own
// kReservedForBuildArtifacts budget - reproducing the real incident's shape
// (a long per-user AppData install path) - while staying safely under
// Windows' raw 260-character MAX_PATH, so creating the directory itself
// never needs long-path support. Only the (never-attempted, once this
// package's fix is in place) build-artifact descendants would have needed
// that.
fs::path longRealisticRoot(const TempDir& base)
{
    return base.path / std::string(120, 'p');
}

// QML-P4R.6 regression: materialize()'s candidate overlay used to be built
// AND verified directly under manualHandoffRoot_ - a durable, database-
// adjacent directory that in production is long enough (the real per-user
// AppData install path) that, combined with a project's own generated
// build-directory name, it tripped ManifestProposalSetVerifier.cpp's own
// overlayPathBudgetCheck before any command ever ran, even though the exact
// same content verifies cleanly under a short root. Confirmed live via the
// retained-Run canary investigation. This test reproduces that exact shape
// (a manualHandoffRoot_ long enough to trip the budget on its own) and
// proves materialize() now succeeds, because verification runs against a
// short-root execution copy instead of the durable directory itself.
void runMaterializeLongRootScenario(const std::string& ephemeralScratchPadding, ScratchProviderStub* provider = nullptr)
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRootBase;
    // The scratch root the verification copy is built under: the short TempDir itself, or (padding) one made long on purpose.
    fs::path ephemeralScratch = scratch.path;
    if (!ephemeralScratchPadding.empty()) {
        ephemeralScratch /= ephemeralScratchPadding;
        fs::create_directories(ephemeralScratch);
    }
    const auto manualHandoffRoot = longRealisticRoot(manualHandoffRootBase);
    // Deliberately OUTSIDE workspace.path (matches
    // testImportAndReverifyProducesSuccessorAttemptAndIsIdempotent's own
    // established real-production placement, see its doc comment) - nesting
    // it under workspace.path would make the workspace baseline capture see
    // the proposal store's own ".proposals/runs/<id>/proposals/<id>" depth
    // too, which combined with this test's deliberately padded root is
    // exactly the kind of incidental extra length this package's fix must
    // not depend on to stay under Windows' path budget.
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot, ephemeralScratch);
    if (provider != nullptr) manualHandoffService.setVerificationScratchProvider(provider);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Long manual-handoff root";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {
        {"buildDirectory", kRealisticQtBuildDirectory}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    // The durable record's own repairable failure (a real qml-lint warning,
    // matching every other materialize() fixture in this file - admission
    // requires at least one failed required criterion to repair) must be
    // reproduced identically by the candidate's fresh re-verification below
    // under the fixed code (short-root execution copy), or attribution would
    // reject it as attribution_source_changed. Under the OLD code the
    // candidate's own criterion never got the chance to reproduce anything -
    // overlayPathBudgetCheck failed it closed before any command ran, which
    // disagreed with this durable record in the exact same way.
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    if (provider != nullptr && provider->refusal.has_value()) {
        // The Backend refused the verification folder: materialize() says so with the Backend's own words, BEFORE the workspace was
        // baselined, copied (also into the durable candidate) or any command was run.
        req(!materialized.success() && materialized.error()->code == provider->refusal->code
                && materialized.error()->message == provider->refusal->message,
            "materialize() must return the refusal of the verification folder as it is");
        req(commandRunner.calls == 0, "a refused verification folder must stop materialize() before any command runs");
        req(fs::is_empty(scratch.path), "a refused verification folder must stop materialize() before anything is copied");
        req(!fs::exists(manualHandoffRoot) || fs::is_empty(manualHandoffRoot),
            "no durable candidate may be created when the verification folder is refused");
        req(provider->asked.size() == 1 && provider->asked.front() == std::optional<fs::path>(workspace.path),
            "the folder is asked for once, for the project's own workspace");
        return;
    }
    if (!ephemeralScratchPadding.empty()) {
        // A scratch root too long for the build tools' path budget: the verification copy is REFUSED, and the user is told exactly that
        // - not "attribution_diagnostic_unmapped" (no file provenance), which blamed the code under test for a path problem.
        req(!materialized.success(), "a verification copy that cannot fit the path budget must not be reported as a success");
        req(materialized.error()->code == "manual_handoff.verification_path_too_long",
            ("the refusal must name the path budget, not '" + materialized.error()->code + "'").c_str());
        req(materialized.error()->message.find("TEMP") != std::string::npos
                && materialized.error()->message.find(ephemeralScratch.string()) == std::string::npos,
            "the message must point at the temporary-directory location without leaking a filesystem path");
        return;
    }
    req(materialized.success(),
        "materialize() must succeed despite a realistically long manualHandoffRoot_, because "
        "verification now runs against a short-root execution copy, never the durable directory");
    req(commandRunner.calls == 2,
        "Both the baseline and candidate qml-lint checks must have actually run a command - "
        "overlayPathBudgetCheck must not have fired for the short execution root");
    if (provider != nullptr) {
        // The temporary verification copies (never the durable candidate) were built under the root the Backend validated.
        for (const auto& directory : commandRunner.workingDirectories) {
            req(isUnder(directory, provider->root) && !isUnder(directory, scratch.path)
                    && !isUnder(directory, manualHandoffRoot),
                "a verification copy must be built under the validated root");
        }
    }
    req(std::filesystem::exists(materialized.value().candidateRoot / "qml" / "App.qml"),
        "The durable candidate must still contain the real component file");
    req(materialized.value().candidateRoot.native().find(manualHandoffRootBase.path.native())
            == 0,
        "The durable candidate must still live under the long manualHandoffRoot_, never redirected "
        "to the short scratch root");

    // Restart simulation: a second Service instance over the SAME durable
    // repository row and the SAME manualHandoffRoot_ on disk must see the
    // identical candidate - proving materialize()'s fix did not make the
    // durable handoff itself dependent on any in-process state.
    ManifestVerificationManualHandoffService restartedService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot, ephemeralScratch);
    const auto afterRestart = restartedService.resolveCandidateRootForAnchor(source.id);
    req(afterRestart.success() && afterRestart.value() == materialized.value().candidateRoot,
        "A fresh Service instance (restart) must resolve the exact same durable candidate root");
    const auto probeAfterRestart = restartedService.probeCandidateHealth(materialized.value().handoff);
    req(probeAfterRestart.success()
            && probeAfterRestart.value().health
                == ManifestVerificationManualHandoffCandidateHealth::Unchanged,
        "A fresh Service instance (restart) must see the same, unmodified durable candidate");
}

// QML-P4R.6 regression: importAndReverify() already builds its own
// verification overlay under ephemeralScratchRoot_ (never manualHandoffRoot_
// - see its own doc comment), so a long manualHandoffRoot_ was never this
// call's own defect. Locks that in as tested, intentional behavior: the same
// realistically long root as the materialize() regression above must not
// break a real edit-and-reverify cycle either.
void testMaterializeSucceedsWithRealisticallyLongManualHandoffRootViaShortScratchVerification()
{
    runMaterializeLongRootScenario("");
}

void testMaterializeIsRefusedBeforeAnyCopyWhenTheVerificationFolderIsNotUsable()
{
    ScratchProviderStub provider;
    provider.refusal = common::Error {"verification_scratch.not_private", "The verification folder is not private. Choose another."};
    runMaterializeLongRootScenario("", &provider);
}

void testMaterializeBuildsItsVerificationCopiesUnderTheRootTheBackendValidated()
{
    TempDir providerRoot;
    ScratchProviderStub provider;
    provider.root = providerRoot.path;
    runMaterializeLongRootScenario("", &provider);
}

void testMaterializeNamesThePathBudgetWhenTheVerificationCopyCannotFit()
{
    // 90 characters of padding under an already non-trivial TempDir: overlay root + the realistic build directory + the reserved
    // build-artifact headroom exceed 260.
    runMaterializeLongRootScenario(std::string(90, 's'));
}
void testImportAndReverifyVerifiesInShortScratchDespiteLongManualHandoffRoot()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRootBase;
    const auto manualHandoffRoot = longRealisticRoot(manualHandoffRootBase);
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Long manual-handoff root reverify";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {
        {"buildDirectory", kRealisticQtBuildDirectory}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // Consumed in order: materialize()'s baseline overlay verify (clean),
    // materialize()'s candidate overlay verify (the real warning, matching
    // durable's own failed criterion), then importAndReverify()'s ONE final
    // combined verify - scripted clean, standing in for "the user's fix
    // actually worked."
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, cleanOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(),
        "materialize() must succeed despite the realistically long manualHandoffRoot_");

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto imported = manualHandoffService.importAndReverify(materialized.value().handoff.id);
    req(imported.success(),
        "reverify() must succeed against the long manualHandoffRoot_ - it already verifies in the "
        "short ephemeralScratchRoot_, never the durable directory");
    req(imported.value().finalStatus == RunStatus::Completed,
        "A scripted clean re-lint must complete the attempt");
}

// QML-P4R.6 regression: overlayPathBudgetCheck must not be weakened by this
// package's fix - it must still fail closed when the EXECUTION root itself
// (ephemeralScratchRoot_, now the real verification target) is the one that
// is too long, even though manualHandoffRoot_ is short this time.
void testMaterializeFailsClosedWhenExecutionScratchRootItselfExceedsPathBudget()
{
    TempDir workspace;
    TempDir scratchBase;
    const auto scratch = longRealisticRoot(scratchBase);
    TempDir manualHandoffRoot;
    // Deliberately OUTSIDE workspace.path - see
    // testMaterializeSucceedsWithRealisticallyLongManualHandoffRootViaShortScratchVerification's
    // own doc comment for why this matters whenever a root in this test is
    // deliberately padded.
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Long execution scratch root";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {
        {"buildDirectory", kRealisticQtBuildDirectory}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    // scratch is now the deliberately-long root, so the reference overlay
    // used to compute the durable verification identity must be built under
    // it too, matching what materialize()'s own baselineRequest will do.
    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    // Admission requires at least one failed required criterion to admit a
    // Run for repair - matching every other materialize() fixture in this
    // file. The scripted outcome queue below is never actually consumed
    // (commandRunner.calls must stay 0), so its exact content does not
    // matter - only that a repairable failure exists for admission to admit.
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {warningOutcome, warningOutcome};

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    const auto manualHandoffRootEntriesBefore = std::distance(
        fs::directory_iterator(manualHandoffRoot.path), fs::directory_iterator());
    req(manualHandoffRootEntriesBefore == 0, "The durable root must start empty");

    const auto materialized = manualHandoffService.materialize(source.id);
    req(!materialized.success(),
        "materialize() must still fail closed when the EXECUTION root itself is too long - "
        "overlayPathBudgetCheck must never be weakened by this package's fix");
    req(commandRunner.calls == 0,
        "No command may run when the execution overlay's own root already exceeds the path budget "
        "- proves the preflight fired for the execution root, not the (now-short) durable one");

    // The durable manualHandoffRoot_ must never be left holding a partial/
    // orphaned candidate directory after a failed materialize() call.
    const auto manualHandoffRootEntriesAfter = std::distance(
        fs::directory_iterator(manualHandoffRoot.path), fs::directory_iterator());
    req(manualHandoffRootEntriesAfter == 0,
        "A failed materialize() call must not leave a partial durable candidate directory behind");
}

// QML-P4R.6 regression: a concurrent writer that mutates the durable
// candidate directory WHILE the short-root execution copy is being verified
// must be caught, not silently ignored - the durable directory is the one
// the user will open next, and materialize() must never hand back a handoff
// whose originalSnapshot does not match what is actually on disk.
void testMaterializeFailsClosedOnDurableCandidateDriftDuringVerification()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);

    // Mutates a known file inside the durable candidate directory (the sole
    // child of manualHandoffRoot.path by the time any command runs - both
    // the baseline and candidate/execution overlays are already built
    // before ManifestProposalSetVerifier::verify() ever calls this runner)
    // on its first invocation, then delegates to a real
    // ScriptedVerificationCommandRunner for the actual scripted outcome.
    struct DurableMutatingCommandRunner final : IManifestVerificationCommandRunner {
        fs::path manualHandoffRoot;
        ScriptedVerificationCommandRunner delegate;
        bool mutated {false};
        ManifestVerificationCommandOutcome run(
            const ManifestVerificationCommandRequest& request, std::stop_token stopToken) override
        {
            if (!mutated) {
                mutated = true;
                for (const auto& entry : fs::directory_iterator(manualHandoffRoot)) {
                    std::ofstream appQml(entry.path() / "qml" / "App.qml",
                        std::ios::binary | std::ios::app);
                    if (appQml) appQml << "// mutated mid-verification\n";
                }
            }
            return delegate.run(request, stopToken);
        }
    };
    DurableMutatingCommandRunner commandRunner;
    commandRunner.manualHandoffRoot = manualHandoffRoot.path;

    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Durable candidate drift";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    // Admission requires at least one failed required criterion; the
    // candidate's fresh re-verification must reproduce it identically (the
    // same warning) or attribution would reject it before this test's own
    // injected mutation is ever reached.
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.delegate.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(!materialized.success() && materialized.error()->code == "manual_handoff.workspace_unstable",
        "A durable candidate mutated mid-verification must fail closed as workspace_unstable, "
        "never be handed back as a healthy handoff");
    req(commandRunner.mutated, "The injected mutation must actually have run for this test to prove "
        "anything");
}

// QML-P4R.6 regression: builds a real, multi-file Modify predecessor
// proposal (three files owned by one task) where only ONE file carries a
// remaining owner finding - the exact shape the retained-Run canary
// investigation found in production (a real Run's frozen proposal touched
// three qml files; only one still had a lint warning). Shared by every test
// below that exercises this fixture shape.
struct MultiFilePredecessorFixture {
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    std::unique_ptr<FileChangeProposalStore> store;
    std::unique_ptr<ChangeProposalService> proposals;
    std::unique_ptr<ManifestVerificationContinuationService> admission;
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    std::unique_ptr<ManifestVerificationAttributionService> attribution;
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts {artifactRepository, artifactStore};
    std::unique_ptr<ManifestVerificationLegacyBaselineNormalizationService> baselineNormalization;
    std::unique_ptr<ManifestVerificationManualHandoffService> service;

    Project project;
    Run source;
    ChangeSetId predecessorId;
    ManifestVerificationManualHandoffMaterializeResult materialized;

    MultiFilePredecessorFixture()
    {
        auto storeResult = FileChangeProposalStore::open(proposalStore.path);
        req(storeResult.success(), "Could not open the real proposal store");
        store = std::move(storeResult.value());
        proposals = std::make_unique<ChangeProposalService>(proposalRepository, *store);
        admission = std::make_unique<ManifestVerificationContinuationService>(
            runs, ledger, proposalRepository, continuations);
        attribution = std::make_unique<ManifestVerificationAttributionService>(
            manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
        baselineNormalization = std::make_unique<ManifestVerificationLegacyBaselineNormalizationService>(
            baselineNormalizations, artifacts, *proposals);
        service = std::make_unique<ManifestVerificationManualHandoffService>(
            runs, projects, manifests, ledger, manualHandoffs, continuations, *admission, *attribution,
            *proposals, commandRunner, *baselineNormalization, manualHandoffRoot.path, scratch.path);

        const auto now = std::chrono::system_clock::now();
        project.id = "project-1";
        project.name = "Multi-file predecessor";
        project.rootPath = workspace.path;
        project.status = ProjectStatus::Active;
        project.createdAt = now;
        project.updatedAt = now;
        req(projects.insert(project).success(), "Could not seed project");

        std::filesystem::create_directories(workspace.path / "qml");
        writeReal("qml/A.qml", "original-a");
        writeReal("qml/B.qml", "original-b");
        writeReal("qml/C.qml", "original-c");

        source.id = "source-run";
        source.projectId = project.id;
        source.title = "Stage 3 failure";
        source.task = "Repair qml lint";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = now;
        req(runs.insertRun(source).success(), "Could not seed source Run");

        CreateChangeProposalRequest predecessorRequest;
        predecessorRequest.runId = source.id;
        predecessorRequest.projectId = project.id;
        predecessorRequest.providerId = "openai";
        predecessorRequest.modelId = "gpt-4";
        predecessorRequest.agentRole = "qml_component_author";
        predecessorRequest.baseRevision = "main";
        predecessorRequest.policy.projectRoot = workspace.path;
        predecessorRequest.requireHashEvidence = true;
        predecessorRequest.observedFileHashes = {
            {"qml/a.qml", sha256Hex("original-a")},
            {"qml/b.qml", sha256Hex("original-b")},
            {"qml/c.qml", sha256Hex("original-c")}};
        predecessorRequest.rawModelOutput = R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Fix qml lint","changes":[)"
            R"({"change_id":"fix-a","operation":"modify","relative_path":"qml/A.qml",)"
            R"("source_path":null,"proposed_content":"fixed-a"},)"
            R"({"change_id":"fix-b","operation":"modify","relative_path":"qml/B.qml",)"
            R"("source_path":null,"proposed_content":"fixed-b"},)"
            R"({"change_id":"fix-c","operation":"modify","relative_path":"qml/C.qml",)"
            R"("source_path":null,"proposed_content":"fixed-c"}]})";
        const auto predecessor = proposals->createFromModelOutput(predecessorRequest);
        req(predecessor.success() && predecessor.value().state == ChangeSetState::ReadyForReview,
            "Could not create the real multi-file predecessor proposal");
        predecessorId = predecessor.value().changeSetId;

        ExecutionAcceptanceRequirement requirement;
        requirement.id = "qml-lint";
        requirement.description = "QML lint is clean";
        requirement.verificationKind = ExecutionVerificationKind::QmlLint;
        requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
        ExecutionAcceptanceContract contract;
        contract.requirements.push_back(requirement);

        ExecutionManifestTask component;
        component.id = "component";
        component.ownedPaths = {"qml/A.qml", "qml/B.qml", "qml/C.qml"};
        component.acceptanceContract = contract;
        component.requestedCapability = WorkspaceCapability::Propose;
        ExecutionManifest manifest;
        manifest.runId = source.id;
        manifest.tasks = {component};
        req(manifests.insert(manifest).success(), "Could not seed source Manifest");

        const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
        const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
        req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

        BuildManifestProposalOverlayRequest overlayRequest;
        overlayRequest.workspaceRoot = workspace.path;
        overlayRequest.trustedScratchRoot = scratch.path;
        overlayRequest.workspaceIdentity = project.id;
        overlayRequest.baselinePolicy = policy;
        overlayRequest.baselineSnapshot = baselineSnapshot.value();
        overlayRequest.orderedProposalIds = {predecessorId};
        auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, *proposals);
        req(referenceOverlay.success(), "Could not build the reference candidate overlay");
        const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
        const std::vector<std::string> contentHashes =
            referenceOverlay.value().orderedProposalContentHashes;
        req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
            "Could not clean up the reference overlay");

        ManifestProposalSetVerification durable;
        durable.schemaVersion = "manifest-proposal-set-verification.v1";
        durable.workspaceIdentity = project.id;
        durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
        durable.orderedProposalIds = overlayRequest.orderedProposalIds;
        durable.orderedProposalContentHashes = contentHashes;
        durable.overlayFingerprint = overlayFingerprint;
        durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
        durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
        durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
        durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
        ManifestVerificationDiagnostic diagnostic;
        diagnostic.code = "verification.qml_warning";
        diagnostic.message = "QML lint reported a warning";
        diagnostic.requirementId = "qml-lint";
        diagnostic.structuredFindings.push_back(
            {"<candidate>/qml/A.qml", 1, 1, "unqualified", "Unqualified access"});
        durable.diagnostics.push_back(std::move(diagnostic));

        ManifestWorkLedgerEntry entry;
        entry.runId = source.id;
        entry.projectId = project.id;
        entry.terminalStatus = RunStatus::Failed;
        entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
        entry.manifestSchemaVersion = "execution-manifest.v1";
        entry.verification = durable;
        entry.taskDispositions = {{"component", "succeeded", predecessorId, std::nullopt}};
        entry.createdAt = now;
        req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

        ManifestVerificationCommandOutcome cleanOutcome;
        cleanOutcome.started = true;
        cleanOutcome.exitCode = 0;
        cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
        ManifestVerificationCommandOutcome warningOutcome;
        warningOutcome.started = true;
        warningOutcome.exitCode = 0;
        warningOutcome.boundedOutput =
            "Warning: <candidate>/qml/A.qml:1:1: Unqualified access [unqualified]\n";
        commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

        const auto result = service->materialize(source.id);
        req(result.success(), "A fully eligible multi-file source Run must materialize successfully");
        req(result.value().handoff.allowedPaths.size() == 1
                && result.value().handoff.allowedPaths.front().relativePath == "qml/A.qml",
            "Only A.qml (the file with a remaining owner finding) must be editable");
        materialized = result.value();
    }

    void writeReal(const std::string& relativePath, const std::string& content)
    {
        std::ofstream f(workspace.path / relativePath, std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(f), "Could not seed a real workspace file");
        f << content;
    }
};

// Points 1-4 and 8 of the QML-P4R.6 multi-file carried-forward contract:
// editing the one authorized file must succeed, the successor must preserve
// the other two predecessor changes byte-identically, and AG-16 must accept
// only the resulting verified successor.
void testImportAndReverifyPreservesCarriedForwardChangesInMultiFilePredecessor()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedA(fx.materialized.candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not open the candidate file for editing");
        editedA << "fixed-a-by-user";
    }

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    fx.commandRunner.queuedOutcomes = {cleanOutcome};

    const auto imported = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(imported.success(),
        "Editing only the authorized file in a multi-file predecessor must reverify successfully");
    req(imported.value().finalStatus == RunStatus::Completed,
        "A scripted clean re-lint must complete the attempt");
    req(imported.value().finalVerification.has_value()
            && imported.value().finalVerification->orderedProposalIds.size() == 1,
        "The final verified set must still have exactly one member - the amended successor");
    const ChangeSetId successorId = imported.value().finalVerification->orderedProposalIds.front();
    req(successorId != fx.predecessorId, "The verified member must be the NEW successor");

    const auto successorChanges = fx.proposals->listChanges(successorId);
    req(successorChanges.success() && successorChanges.value().size() == 3,
        "The successor must carry forward all three predecessor changes, not just the edited one");
    for (const auto& change : successorChanges.value()) {
        const auto path = change.relativePath.generic_string();
        if (path == "qml/A.qml") {
            const auto content = fx.proposals->readProposedContent(successorId, change.changeId);
            req(content.success() && content.value() == "fixed-a-by-user",
                "The edited file's successor content must be the user's real edit");
            continue;
        }
        req(path == "qml/B.qml" || path == "qml/C.qml",
            "The successor must not contain any path beyond the original three");
        req(change.changeId == (path == "qml/B.qml" ? "fix-b" : "fix-c"),
            "A carried-forward change must keep the predecessor's own changeId");
        const auto content = fx.proposals->readProposedContent(successorId, change.changeId);
        const std::string expected = path == "qml/B.qml" ? "fixed-b" : "fixed-c";
        req(content.success() && content.value() == expected,
            "A carried-forward change's content must be byte-identical to the predecessor's own");
    }

    // AG-16 (point 8): only the verified successor may authorize apply; the
    // superseded predecessor must not.
    ManifestApplyVerificationGate gate(
        *fx.proposals, fx.ledger, fx.runs, nullptr, &fx.manifests, &fx.continuations,
        &fx.manualHandoffs);
    const auto successorEligibility = gate.evaluate(successorId);
    req(successorEligibility.allowed, "The verified successor must be AG-16 eligible");
    const auto predecessorEligibility = gate.evaluate(fx.predecessorId);
    req(!predecessorEligibility.allowed,
        "The superseded predecessor must not itself be AG-16 eligible after a verified successor "
        "exists");
}

// Point 5 of the QML-P4R.6 multi-file carried-forward contract: editing one
// of the two NON-editable predecessor files (B.qml, which carries no
// remaining owner finding) must still fail closed - this pre-dates and is
// unrelated to this package's own allowedRoots widening, which only ever
// applies once materializeRepairSuccessor() is reached; this diff-detection
// check runs well before that.
void testImportAndReverifyRejectsEditToNonAllowedFileInMultiFilePredecessor()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }

    const auto imported = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.forbidden_path_changed",
        "Editing a predecessor path outside handoff.allowedPaths must fail closed, even though "
        "that same path is now authorized for the SUCCESSOR's own allowedRoots widening");
    req(imported.error()->message.find("qml/B.qml") != std::string::npos,
        "The forbidden-path rejection must name the safe candidate-relative path so the user "
        "can restore the exact file without guessing");
}

// Point 6 of the QML-P4R.6 multi-file carried-forward contract: a candidate
// directory containing an entirely new, never-frozen path must fail closed
// - the successor's allowedRoots widening is derived strictly from the
// frozen predecessor's own paths plus handoff.allowedPaths, so a fourth
// path can never legitimately appear there regardless.
void testImportAndReverifyRejectsNewPathInMultiFilePredecessor()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream newFile(fx.materialized.candidateRoot / "qml" / "D.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(newFile), "Could not create a new file in the candidate directory");
        newFile << "unauthorized-new-file";
    }

    const auto imported = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.new_path_not_allowed",
        "A path never part of the frozen handoff must fail closed, never silently accepted into "
        "the successor's own widened allowedRoots");
    req(imported.error()->message.find("qml/D.qml") != std::string::npos,
        "The new-path rejection must name the safe candidate-relative path so the user can "
        "remove the exact file without guessing");
}

// QML-P4R.6 follow-up hardening: a corrupt Modify entry that is CARRIED
// FORWARD (B.qml, never part of this round's own edits - only A.qml is
// authorized) must still fail closed with the call's own stable
// predecessor_hash_evidence_missing code. Before this follow-up, the
// hash-evidence check lived only inside the `paths` loop matched against
// this round's own edited Create/Modify entries, so a corrupt carried-
// forward entry would have silently reached materializeRepairSuccessor()
// instead. The corrupt row is injected directly through the repository -
// ChangeProposalValidator::checkOperationInvariants makes this
// unconstructible through the ordinary public creation API alone (see
// that check's own doc comment) - exactly the "corrupted durable row from
// any other source" defense-in-depth scenario it targets.
void testImportAndReverifyFailsClosedOnCorruptCarriedForwardModifyHashEvidence()
{
    MultiFilePredecessorFixture fx;

    {
        const auto predecessor = fx.proposalRepository.findChangeSet(fx.predecessorId);
        req(predecessor.success() && predecessor.value().has_value(),
            "Could not read back the real predecessor to corrupt it");
        auto corrupted = *predecessor.value();
        bool found = false;
        for (auto& change : corrupted.changes) {
            if (change.relativePath.generic_string() == "qml/B.qml") {
                change.expectedOriginalHash.reset();
                found = true;
            }
        }
        req(found, "Test setup: B.qml's own change must exist in the predecessor to corrupt");
        req(fx.proposalRepository.insertChangeSet(corrupted).success(),
            "Could not overwrite the predecessor with a corrupted carried-forward entry");
    }

    {
        std::ofstream editedA(fx.materialized.candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not open the candidate file for editing");
        editedA << "fixed-a-by-user";
    }

    const auto imported = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(!imported.success()
            && imported.error()->code == "manual_handoff.predecessor_hash_evidence_missing",
        "A carried-forward Modify entry with no expectedOriginalHash must fail closed with this "
        "call's own stable code, even though the edited path itself (A.qml) is perfectly valid");
    // No new/overwritten Work Ledger entry took the original's place - this
    // attempt was rejected before ever reaching a real verification pass.
    const auto sourceEntry = fx.ledger.findEntry(fx.source.id);
    req(sourceEntry.success() && sourceEntry.value().has_value()
            && sourceEntry.value()->verification.has_value()
            && sourceEntry.value()->verification->buildLintTestStatus
                == ExecutionVerificationStatus::Failed,
        "The source Run's own original Work Ledger entry must be unchanged - still the seeded "
        "Failed verification, never overwritten by a fresh one this rejected attempt never earned");

    // A stable RejectedBeforeVerification result: never a fresh verification
    // ordinal/ledger write, and a bare retry replays the identical
    // rejection rather than a different or transient outcome.
    const auto retried = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(!retried.success()
            && retried.error()->code == "manual_handoff.predecessor_hash_evidence_missing",
        "The same corrupt predecessor must replay the identical rejection on a bare retry");
}

// QML-P4R.6 follow-up hardening: the same defense, but for a Delete entry -
// which, unlike Modify, could never even be MATCHED by the old per-edited-
// path check (that check's own std::find_if was restricted to Create/
// Modify only), so a corrupt Delete entry was entirely invisible to it
// regardless of whether it happened to be edited this round.
void testImportAndReverifyFailsClosedOnCorruptCarriedForwardDeleteHashEvidence()
{
    MultiFilePredecessorFixture fx;

    {
        const auto predecessor = fx.proposalRepository.findChangeSet(fx.predecessorId);
        req(predecessor.success() && predecessor.value().has_value(),
            "Could not read back the real predecessor to corrupt it");
        auto corrupted = *predecessor.value();
        ProposedFileChange deleteChange;
        deleteChange.changeId = "del-1";
        deleteChange.operation = FileChangeOperation::Delete;
        deleteChange.relativePath = "qml/ToDelete.qml";
        // No expectedOriginalHash - Delete structurally requires one (see
        // ChangeProposalValidator::checkOperationInvariants), so this is a
        // corrupt durable row, unconstructible through the public creation
        // API and injected here directly through the repository instead.
        corrupted.changes.push_back(deleteChange);
        req(fx.proposalRepository.insertChangeSet(corrupted).success(),
            "Could not overwrite the predecessor with a corrupted carried-forward Delete entry");
    }

    {
        std::ofstream editedA(fx.materialized.candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not open the candidate file for editing");
        editedA << "fixed-a-by-user";
    }

    const auto imported = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(!imported.success()
            && imported.error()->code == "manual_handoff.predecessor_hash_evidence_missing",
        "A carried-forward Delete entry with no expectedOriginalHash must fail closed with this "
        "call's own stable code - never silently matched (Delete can never be an edited path) and "
        "never allowed through to materializeRepairSuccessor()");
}

// QML-P4 package 3 end-to-end happy path: materialize a real handoff (same
// fixture shape as the test above), edit the one real editable file on disk
// exactly like a human would in their own editor, then reverify for free.
// Proves the real successor lineage, the real sibling attempt Run/Work
// Ledger shape, and free-retry idempotency - never a hand-built
// ManifestProposalSetVerification standing in for the real thing.
void testImportAndReverifyProducesSuccessorAttemptAndIsIdempotent()
{
    outcomebrief_hooks::RecordingTrigger outcomeBriefTrigger;   // PC-4C: records the nudges to the Outcome Brief
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    // Deliberately OUTSIDE workspace.path - matches
    // BackendRuntime::openProposalStorage's real production placement
    // (beside the app database, never inside the project tree). Nesting it
    // under workspace.path here would make the workspace baseline capture
    // below see the proposal store's own growth as it publishes the
    // successor this test creates.
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);
    manualHandoffService.setOutcomeBriefTrigger(&outcomeBriefTrigger);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Import and reverify happy path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    requirement.qmlLintDiagnosticPolicyId =
        std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    ManifestVerificationCommandOutcome advisoryOutcome;
    advisoryOutcome.started = true;
    advisoryOutcome.exitCode = 0;
    advisoryOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Unqualified access [unqualified]\n";
    // Consumed in order: materialize()'s baseline overlay verify (clean),
    // materialize()'s candidate overlay verify (the real warning), then
    // importAndReverify()'s final candidate+baseline comparison - a real,
    // new in-scope Advisory under the frozen policy against a clean source
    // baseline. It must be authoritative success, never a second paid
    // repair/manual cycle.
    commandRunner.queuedOutcomes = {
        cleanOutcome, warningOutcome, advisoryOutcome, cleanOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;
    // PC-4C: a new handoff changes the brief of its ANCHOR Run (it now waits for the manual resolution).
    req(outcomeBriefTrigger.runs() == std::vector<std::string> {source.id}, "Materializing a handoff must nudge its anchor once");

    // Edit the one real editable file exactly like a human would in their
    // own editor - never through any Backend API, a plain file write.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(imported.success(), "A legitimate single-file edit must reverify successfully");
    req(outcomeBriefTrigger.runs() == (std::vector<std::string> {source.id, source.id}), "Reverifying must nudge the anchor once more");
    req(!imported.value().idempotentReplay, "The first call must do real work, not a replay");
    req(imported.value().finalStatus == RunStatus::Completed,
        "An advisory-only re-lint under the frozen policy must complete the attempt");
    req(imported.value().finalVerification.has_value()
            && requiredAcceptancePassed(*imported.value().finalVerification),
        "The attempt's own recorded verification must reflect the passed-with-advisories re-lint");
    req(std::any_of(imported.value().finalVerification->diagnostics.begin(),
                imported.value().finalVerification->diagnostics.end(),
                [](const ManifestVerificationDiagnostic& item) {
                    return item.code == "verification.qml_warning_advisory";
                }),
        "Manual Reverify must durably retain its advisory diagnostic");
    req(!imported.value().attemptRunId.empty()
            && imported.value().attemptRunId != handoff.verificationAnchorRunId,
        "The attempt must be a genuinely new, distinct sibling Run");

    const auto attemptRun = runs.findRun(imported.value().attemptRunId);
    req(attemptRun.success() && attemptRun.value().has_value()
            && attemptRun.value()->status == RunStatus::Completed
            && attemptRun.value()->parentRunId == std::optional<RunId>(source.id)
            && attemptRun.value()->workflowId
                == std::string(kManifestVerificationManualHandoffWorkflowId),
        "The attempt Run must carry the exact manual-handoff sibling identity");

    const auto attemptLedger = ledger.findEntry(imported.value().attemptRunId);
    req(attemptLedger.success() && attemptLedger.value().has_value()
            && attemptLedger.value()->disposition == ManifestWorkLedgerDisposition::ManifestReady
            && attemptLedger.value()->verification.has_value(),
        "The attempt's own Work Ledger entry must record a passed ManifestReady verification");
    req(attemptLedger.value()->verification->orderedProposalIds.size() == 1,
        "The final verified set must still have exactly one member - the amended successor");
    const ChangeSetId successorId = attemptLedger.value()->verification->orderedProposalIds.front();
    req(successorId != componentProposal.value().changeSetId,
        "The verified member must be the NEW successor, not the original predecessor");

    const auto successor = proposals.findChangeSet(successorId);
    req(successor.success()
            && successor.value().supersedesChangeSetId
                == std::optional<ChangeSetId>(componentProposal.value().changeSetId)
            && successor.value().providerId == "manual-handoff"
            && successor.value().agentRole == "user",
        "The successor must carry honest, non-fictitious manual-handoff provenance and correct "
        "lineage back to the real predecessor");
    const auto successorChanges = proposals.listChanges(successorId);
    req(successorChanges.success() && successorChanges.value().size() == 1
            && successorChanges.value().front().operation == FileChangeOperation::Create,
        "The successor must preserve the predecessor's own Create operation shape, not silently "
        "switch to Modify");
    const auto successorContent = proposals.readProposedContent(
        successorId, successorChanges.value().front().changeId);
    req(successorContent.success() && successorContent.value() == "Item { anchors.fill: parent }",
        "The successor must carry the user's real edited content");

    // Free re-verification is idempotent: calling again against the exact
    // same edited state must return the SAME attempt, never a duplicate.
    const auto replay = manualHandoffService.importAndReverify(handoff.id);
    req(replay.success() && replay.value().idempotentReplay
            && replay.value().attemptRunId == imported.value().attemptRunId,
        "A repeat call against unchanged edited state must be an idempotent replay");
    req(outcomeBriefTrigger.runCalls(source.id) == 3, "Even a replay nudges (the nudge is idempotent)");
}

// QML-P4R.3.4: a genuine CONTENT failure - ManifestProposalSetVerifier::
// verify() itself succeeds structurally, but the required acceptance does
// not pass (a real qml-lint warning finding) - is still an authoritative
// result, backed by its own Work Ledger entry exactly like a passed one. A
// repeat call against the SAME edited bytes must replay it (same Run, same
// Failed status, same typed VerifiedContentFailure classification) without
// running the verifier again, and must never create a second, competing
// attempt/ledger entry for the same fingerprint.
void testImportAndReverifyContentFailureReplaysWithoutReverifying()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Content failure replay";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    // QML-lint advisory-severity policy (2026-09-28): deliberately a Hard
    // tag ("import"), not "unqualified" - this test proves genuine content
    // failures still replay as Failed, which an Advisory-severity tag would
    // no longer demonstrate now that a new in-scope Advisory finding passes
    // (see ManifestProposalVerificationTests.cpp's own advisory-only tests).
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // materialize()'s baseline overlay verify (clean), materialize()'s
    // candidate overlay verify (the real warning), then
    // importAndReverify()'s ONE final combined verify - ALSO scripted with
    // the warning, standing in for "the user's edit did not actually fix
    // the lint issue" - a genuine, authoritative content failure.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(imported.success(), "A structurally successful but content-failing verify is not itself "
        "an error");
    req(!imported.value().idempotentReplay, "The first call must do real work, not a replay");
    req(imported.value().finalStatus == RunStatus::Failed,
        "The scripted persisted warning must fail the required acceptance");
    req(imported.value().finalVerification.has_value(),
        "A content failure still carries its own authoritative verification record");
    const std::size_t callsAfterFirst = commandRunner.calls;

    const auto attempt = manualHandoffs.findAttemptByRunId(imported.value().attemptRunId);
    req(attempt.success() && attempt.value().has_value(), "The attempt row must be durable");
    const auto typedResult = manualHandoffs.findAttemptResultByAttemptId(attempt.value()->id);
    req(typedResult.success() && typedResult.value().has_value(),
        "A content failure must record its own durable typed result");
    req(typedResult.value()->kind
            == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure,
        "A structurally successful but not-passed verify must classify as VerifiedContentFailure, "
        "never Incomplete");

    // Repeated calls against the exact same edited bytes must replay the
    // SAME authoritative result without ever re-running the verifier, and
    // must never allocate a second attempt/ledger entry for the same
    // fingerprint (no competing Apply-authority).
    for (int i = 0; i < 3; ++i) {
        const auto replay = manualHandoffService.importAndReverify(handoff.id);
        req(replay.success() && replay.value().idempotentReplay
                && replay.value().attemptRunId == imported.value().attemptRunId
                && replay.value().finalStatus == RunStatus::Failed,
            "A repeat call against an unchanged content failure must replay the SAME attempt");
        req(commandRunner.calls == callsAfterFirst,
            "A replay of a content failure must never re-run the verifier");
    }
    const auto attemptsForHandoff = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsForHandoff.success() && attemptsForHandoff.value().size() == 1,
        "Repeated replay calls must never create a second, competing attempt for the same "
        "fingerprint");
}

// QML-P4R.3.4: a transient/tool/infrastructure failure (here: the
// repository call ManifestProposalSetVerifier::verify() itself depends on
// fails, standing in for any tool/I-O boundary error) never produces a Work
// Ledger entry, so it is classified Incomplete - never a content verdict -
// and a later call against the EXACT SAME edited bytes must allocate a
// fresh ordinal retry rather than replaying anything or refusing.
void testImportAndReverifySameBytesAfterInfrastructureFailureAllocatesNewOrdinal()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Infrastructure failure retry";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, cleanOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    // Force a genuine transient/tool/infrastructure failure: the component
    // proposal's own listChanges() call - on the critical path grouping
    // edited paths by owning proposal, well after the attempt row and its
    // Run already exist - fails structurally. Never a content verdict.
    proposalRepository.failListChangesFor(componentProposal.value().changeSetId);
    const auto firstAttempt = manualHandoffService.importAndReverify(handoff.id);
    req(!firstAttempt.success(), "An injected repository failure must surface, not be swallowed");
    req(firstAttempt.error()->code != "manual_handoff.attempt_result_missing"
            && firstAttempt.error()->code != "manual_handoff.attempt_result_corrupt",
        "The very first attempt at a fingerprint must fail with the injected repository error, "
        "not a typed-result lookup error");

    const auto attemptsAfterFirst = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsAfterFirst.success() && attemptsAfterFirst.value().size() == 1,
        "The failed reservation must remain durable audit history, not be discarded");
    const auto firstAttemptRow = attemptsAfterFirst.value().front();
    req(firstAttemptRow.attemptOrdinal == 1, "The first reservation must be ordinal 1");
    const auto firstTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(firstAttemptRow.id);
    req(firstTypedResult.success() && firstTypedResult.value().has_value()
            && firstTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::Incomplete,
        "A transient/tool/infrastructure failure must classify as Incomplete, never a content "
        "verdict");
    const auto firstRun = runs.findRun(firstAttemptRow.attemptRunId);
    req(firstRun.success() && firstRun.value().has_value()
            && firstRun.value()->status == RunStatus::Failed,
        "The failed reservation's own Run must reach a terminal Failed status");

    // The SAME edited bytes are still on disk - clearing the injected
    // failure and calling again must allocate a fresh ordinal retry, never
    // refuse and never replay the (nonexistent) prior result.
    proposalRepository.clearListChangesFailures();
    const auto secondAttempt = manualHandoffService.importAndReverify(handoff.id);
    req(secondAttempt.success(), "The same edited bytes must be retryable once the transient "
        "failure clears");
    req(!secondAttempt.value().idempotentReplay,
        "A retry after an Incomplete classification must do real work, never replay");
    req(secondAttempt.value().attemptRunId != firstAttemptRow.attemptRunId,
        "A retry must be a genuinely new sibling attempt Run, never reopen the failed one");
    req(secondAttempt.value().finalStatus == RunStatus::Completed,
        "The scripted clean re-lint on the retry must complete the attempt");

    const auto attemptsAfterSecond = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsAfterSecond.success() && attemptsAfterSecond.value().size() == 2,
        "The retry must be recorded as a second, sibling attempt - never overwriting the first");
    req(attemptsAfterSecond.value().back().attemptOrdinal == 2,
        "The retry must allocate the next ordinal, not reuse ordinal 1");
    const auto secondTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(attemptsAfterSecond.value().back().id);
    req(secondTypedResult.success() && secondTypedResult.value().has_value()
            && secondTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess,
        "The successful retry must record its own VerifiedSuccess typed result");
}

// QML-P4R.3.4 fail-closed guard: an attempt whose Run reached a terminal
// status but whose typed classification was never durably recorded (no Work
// Ledger entry AND no ManifestVerificationManualHandoffAttemptResult row -
// the exact shape a bug in the write path this same remediation package
// just hardened could produce) must refuse to guess whether a retry is
// safe, rather than defaulting to either "replay" (nothing to replay) or
// "retry" (which could mask a genuine unrecorded content failure).
void testImportAndReverifyMissingTypedResultFailsClosed()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Missing typed result fails closed";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    // Compute the REAL live fingerprint of the edited candidate exactly the
    // way importAndReverify() itself does, so a directly-seeded attempt row
    // below is the SAME one importAndReverify()'s own lookup will find -
    // without ever calling importAndReverify() first (which would durably
    // write its own typed result, defeating the point of this test).
    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(materialized.value().candidateRoot);
    const auto liveCapture = WorkspaceBaselineProvenance::capture(candidatePolicy);
    req(liveCapture.success(), "Could not capture the edited candidate's own fingerprint");

    const RunId attemptRunId = "attempt-run-corrupt";
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:" + attemptRunId;
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRunId;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = liveCapture.value().fingerprint;
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(), "Could not seed the corrupt attempt row");

    Run attemptRun = source;
    attemptRun.id = attemptRunId;
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.status = RunStatus::Failed;
    attemptRun.finishedAt = now;
    req(runs.insertRun(attemptRun).success(), "Could not seed the corrupt attempt's own Run");
    // Deliberately NO Work Ledger entry and NO
    // ManifestVerificationManualHandoffAttemptResult row - the exact
    // "neither durable signal exists" shape this guard exists to catch.

    const auto blocked = manualHandoffService.importAndReverify(handoff.id);
    req(!blocked.success(), "A terminal attempt with neither a ledger entry nor a typed result "
        "must never silently succeed");
    req(blocked.error()->code == "manual_handoff.attempt_result_missing",
        "The refusal must be the specific fail-closed code, not a generic or guessed one");

    // The corrupt row must remain untouched audit history - this guard
    // must never itself attempt to "fix" the gap by writing a guessed
    // classification.
    const auto stillMissing = manualHandoffs.findAttemptResultByAttemptId(attempt.id);
    req(stillMissing.success() && !stillMissing.value().has_value(),
        "The fail-closed guard must never itself write a guessed typed result");
}

// QML-P4R.3.4: a still-active reservation for the EXACT same edited
// fingerprint (Run genuinely Queued/Running, neither a ledger entry nor a
// typed result recorded yet) must block a concurrent retry outright -
// never replay (there is nothing authoritative yet) and never allocate a
// competing new ordinal while the first is still in flight.
void testImportAndReverifyActiveAttemptBlocksConcurrentRetry()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Active attempt blocks retry";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(materialized.value().candidateRoot);
    const auto liveCapture = WorkspaceBaselineProvenance::capture(candidatePolicy);
    req(liveCapture.success(), "Could not capture the edited candidate's own fingerprint");

    const RunId attemptRunId = "attempt-run-active";
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:" + attemptRunId;
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRunId;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = liveCapture.value().fingerprint;
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(), "Could not seed the in-flight attempt row");

    Run attemptRun = source;
    attemptRun.id = attemptRunId;
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.status = RunStatus::Running;
    req(runs.insertRun(attemptRun).success(), "Could not seed the in-flight attempt's own Run");

    const auto blocked = manualHandoffService.importAndReverify(handoff.id);
    req(!blocked.success(), "A genuinely active reservation must block a concurrent retry");
    req(blocked.error()->code == "manual_handoff.attempt_in_progress",
        "The refusal must be the specific in-progress code");

    const auto attemptsForHandoff = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsForHandoff.success() && attemptsForHandoff.value().size() == 1,
        "A blocked retry must never allocate a competing new ordinal");
    req(!manualHandoffs.findAttemptResultByAttemptId(attempt.id).value().has_value(),
        "A blocked retry must never write any typed result for the still-active attempt");
}

// QML-P4R.3.4 hardening: an existing typed result whose OWN attemptRunId
// field disagrees with the attempt it is filed under - even when its kind
// otherwise matches what the ledger implies - is exactly as much a
// durable-storage integrity conflict as a kind mismatch, and must be
// caught BEFORE the Run/stage projection is rewritten to match the ledger.
void testImportAndReverifyResultAttemptRunIdMismatchFailsClosed()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Result attemptRunId mismatch fails closed";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification sourceDurable;
    sourceDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    sourceDurable.workspaceIdentity = project.id;
    sourceDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    sourceDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    sourceDurable.orderedProposalContentHashes = contentHashes;
    sourceDurable.overlayFingerprint = overlayFingerprint;
    sourceDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    sourceDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    sourceDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    sourceDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic sourceDiagnostic;
    sourceDiagnostic.code = "verification.qml_warning";
    sourceDiagnostic.message = "QML lint reported a warning";
    sourceDiagnostic.requirementId = "qml-lint";
    sourceDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    sourceDurable.diagnostics.push_back(std::move(sourceDiagnostic));

    ManifestWorkLedgerEntry sourceEntry;
    sourceEntry.runId = source.id;
    sourceEntry.projectId = project.id;
    sourceEntry.terminalStatus = RunStatus::Failed;
    sourceEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    sourceEntry.manifestSchemaVersion = "execution-manifest.v1";
    sourceEntry.verification = sourceDurable;
    sourceEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    sourceEntry.createdAt = now;
    req(ledger.insertEntry(sourceEntry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(materialized.value().candidateRoot);
    const auto liveCapture = WorkspaceBaselineProvenance::capture(candidatePolicy);
    req(liveCapture.success(), "Could not capture the edited candidate's own fingerprint");

    const RunId attemptRunId = "attempt-run-runid-mismatch";
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:" + attemptRunId;
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRunId;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = liveCapture.value().fingerprint;
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(), "Could not seed the attempt row");

    // Seeded Interrupted (not yet Completed) so a wrongful repair to match
    // the ledger below - which must never happen - is actually observable.
    Run attemptRun = source;
    attemptRun.id = attemptRunId;
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.finishedAt = now;
    req(runs.insertRun(attemptRun).success(), "Could not seed the attempt's own Run");

    ManifestProposalSetVerification verification;
    verification.schemaVersion = "manifest-proposal-set-verification.v1";
    verification.workspaceIdentity = project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {componentProposal.value().changeSetId};
    verification.orderedProposalContentHashes = {std::string(64, 'c')};
    verification.overlayFingerprint = std::string(64, 'f');
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRunId;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = verification;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the authoritative Work Ledger entry");

    // The conflict: kind correctly matches what the ledger implies
    // (VerifiedSuccess for Completed), but the row's own attemptRunId does
    // not match this attempt's real attemptRunId - isolating the
    // attemptRunId check from the kind check.
    ManifestVerificationManualHandoffAttemptResult mismatchedResult;
    mismatchedResult.id = "manual-handoff-attempt-result:" + attempt.id;
    mismatchedResult.attemptId = attempt.id;
    mismatchedResult.attemptRunId = "some-other-attempt-run-id";
    mismatchedResult.kind = ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess;
    mismatchedResult.recordedAt = now;
    req(manualHandoffs.insertAttemptResult(mismatchedResult).success(),
        "Could not seed the attemptRunId-mismatched typed result");

    const auto blocked = manualHandoffService.importAndReverify(handoff.id);
    req(!blocked.success(), "A typed result recorded against a different attemptRunId must fail "
        "closed, never be silently trusted");
    req(blocked.error()->code == "manual_handoff.attempt_result_conflict",
        "The refusal must be the specific conflict code");

    const auto runAfter = runs.findRun(attemptRunId);
    req(runAfter.success() && runAfter.value().has_value()
            && runAfter.value()->status == RunStatus::Interrupted,
        "The Run must not be repaired to match the ledger before the conflict was detected");
}

// QML-P4R.3.4 hardening: a deterministic pre-verification content/policy
// rejection of the edited bytes themselves (here: materializeRepairSuccessor's
// own secret-scan safety check rejecting the edit) is a stable verdict, not
// a transient one - a repeat call against the exact SAME rejected bytes
// must replay the SAME rejection verbatim, never allocate a new ordinal
// (which would otherwise churn forever, since the same bytes always fail
// the same way).
void testImportAndReverifySameBytesAfterProposalSafetyRejectionReplayWithoutNewOrdinal()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Proposal safety rejection replay";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification sourceDurable;
    sourceDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    sourceDurable.workspaceIdentity = project.id;
    sourceDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    sourceDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    sourceDurable.orderedProposalContentHashes = contentHashes;
    sourceDurable.overlayFingerprint = overlayFingerprint;
    sourceDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    sourceDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    sourceDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    sourceDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic sourceDiagnostic;
    sourceDiagnostic.code = "verification.qml_warning";
    sourceDiagnostic.message = "QML lint reported a warning";
    sourceDiagnostic.requirementId = "qml-lint";
    sourceDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    sourceDurable.diagnostics.push_back(std::move(sourceDiagnostic));

    ManifestWorkLedgerEntry sourceEntry;
    sourceEntry.runId = source.id;
    sourceEntry.projectId = project.id;
    sourceEntry.terminalStatus = RunStatus::Failed;
    sourceEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    sourceEntry.manifestSchemaVersion = "execution-manifest.v1";
    sourceEntry.verification = sourceDurable;
    sourceEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    sourceEntry.createdAt = now;
    req(ledger.insertEntry(sourceEntry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // materialize()'s own two verify calls (baseline clean, candidate
    // overlay reproducing the SAME warning sourceDurable's Failed status
    // above records - materialize()'s own drift check rejects a
    // reconstructed candidate whose re-verification disagrees with the
    // durable source identity). The final combined verify() is never
    // reached - the secret scan rejects the edit before that point.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    // A real AWS-access-key-shaped secret, deterministically rejected by
    // ChangeProposalValidator::scanForSecrets before any file I/O beyond
    // reading this edited file - never a transient condition.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "AKIAABCDEFGHIJKLMNOP";
    }

    const auto firstAttempt = manualHandoffService.importAndReverify(handoff.id);
    req(!firstAttempt.success(), "A genuine secret-scan safety rejection must surface as a "
        "failure");
    req(firstAttempt.error()->code == "proposal.secret_detected",
        "The rejection must be the real ChangeProposalValidator code, not a generic one");

    const auto attemptsAfterFirst = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsAfterFirst.success() && attemptsAfterFirst.value().size() == 1,
        "The rejected reservation must remain durable audit history");
    const auto firstAttemptRow = attemptsAfterFirst.value().front();
    const auto firstTypedResult = manualHandoffs.findAttemptResultByAttemptId(firstAttemptRow.id);
    req(firstTypedResult.success() && firstTypedResult.value().has_value()
            && firstTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification,
        "A deterministic content/policy rejection must classify as RejectedBeforeVerification, "
        "never Incomplete");
    req(firstTypedResult.value()->rejectionCode == std::optional<std::string>("proposal.secret_detected"),
        "The typed result must durably carry the SAME rejection code for replay");

    // The exact same rejected bytes are still on disk - a repeat call must
    // replay the SAME rejection verbatim, never allocate a new ordinal.
    for (int i = 0; i < 3; ++i) {
        const auto replay = manualHandoffService.importAndReverify(handoff.id);
        req(!replay.success(), "A replay of a stable content rejection must still fail");
        req(replay.error()->code == "proposal.secret_detected",
            "The replayed rejection must carry the SAME code every time");
    }
    const auto attemptsAfterReplay = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsAfterReplay.success() && attemptsAfterReplay.value().size() == 1,
        "Repeated replay calls must never allocate a new ordinal for a deterministic rejection "
        "of unchanged bytes");
}

// Editing a real, materialized file that is NOT in the handoff's editable
// allowlist (here: the untouched integration proposal's own
// qml/CMakeLists.txt, present in the candidate only as read-only baseline
// context) must fail closed rather than silently accepting or ignoring it.
void testImportAndReverifyRejectsAnEditOutsideTheAllowedPath()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Import and reverify forbidden path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    // Edit the read-only integration file - never a path this handoff
    // actually authorized.
    {
        std::ofstream cmake(materialized.value().candidateRoot / "qml" / "CMakeLists.txt",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(cmake), "Could not open the forbidden candidate file for editing");
        cmake << "# tampered";
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.forbidden_path_changed",
        "An edit outside the allowlist must fail closed with forbidden_path_changed, never "
        "silently accepted or ignored");

    const auto attempts = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attempts.success() && attempts.value().empty(),
        "A rejected import must leave no durable attempt behind");
}

// QML-P4 package 4: both the attempt-Run-native successor and the
// untouched, never-copied cross-Run reference (still living on the source
// Run) must be independently Apply-eligible through AG-16's new
// manual-handoff eligibility path - the same "own-Run member via task
// dispositions, cross-Run member via the frozen immutable set" shape
// ManifestApplyVerificationGate already proves for the paid continuation
// path in testRepairMaterializesSuccessorAndPassesFinalCombinedVerification
// above. Also proves a manual-handoff-workflow proposal fails closed
// (never kGateNotApplicableCode) when the gate is composed without this
// repository wired.
void testManualHandoffAttemptAndUnmodifiedReferenceAreBothApplyEligible()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);
    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, nullptr, nullptr, &manualHandoffs);
    ManifestApplyVerificationGate applyGateWithoutManualHandoff(
        proposals, ledger, runs, nullptr, nullptr, nullptr, nullptr);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Manual handoff AG-16 eligibility";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, cleanOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Completed,
        "A legitimate single-file edit must reverify successfully and complete");
    req(imported.value().finalVerification.has_value()
            && imported.value().finalVerification->orderedProposalIds.size() == 2,
        "The final verified set must contain exactly the two members");
    const auto& finalIds = imported.value().finalVerification->orderedProposalIds;
    const ChangeSetId successorId = finalIds.front() == integrationProposal.value().changeSetId
        ? finalIds.back() : finalIds.front();
    req(successorId != componentProposal.value().changeSetId,
        "The verified member must be the NEW successor, not the original predecessor");

    const auto integrationEligibility = applyGate.evaluate(integrationProposal.value().changeSetId);
    req(integrationEligibility.allowed
            && integrationEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The unchanged cross-Run reference member (still living on the source Run) must be "
        "Apply-eligible through the typed manual-handoff checkpoint chain");
    const auto successorEligibility = applyGate.evaluate(successorId);
    req(successorEligibility.allowed
            && successorEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The attempt-Run-native successor must be Apply-eligible");

    // The stale, superseded source predecessor must never remain
    // independently Apply-eligible after a legitimate manual-handoff edit.
    const auto staleEligibility = applyGate.evaluate(componentProposal.value().changeSetId);
    req(!staleEligibility.allowed,
        "A superseded source predecessor must never remain Apply-eligible after reverify");

    // A second green sibling attempt against different edited bytes becomes
    // the ONE authority for the whole handoff. The first successor remains
    // immutable audit history but must immediately lose Apply eligibility;
    // the unchanged member is resolved through this same second attempt,
    // never independently through whichever older attempt happened to
    // contain it.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not reopen the candidate for a second edit");
        appQml << "Item { width: 42; height: 42 }";
    }
    commandRunner.queuedOutcomes.push_back(cleanOutcome);
    const auto secondImported = manualHandoffService.importAndReverify(handoff.id);
    const auto secondAttempt = secondImported.success()
        ? manualHandoffs.findAttemptByRunId(secondImported.value().attemptRunId)
        : common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>::ok(std::nullopt);
    req(secondImported.success() && secondImported.value().finalStatus == RunStatus::Completed
            && secondAttempt.success() && secondAttempt.value().has_value()
            && secondAttempt.value()->attemptOrdinal == 2,
        "A second distinct green edit must create the next successful sibling attempt");
    const auto& secondIds = secondImported.value().finalVerification->orderedProposalIds;
    const ChangeSetId secondSuccessorId = secondIds.front() == integrationProposal.value().changeSetId
        ? secondIds.back() : secondIds.front();
    req(secondSuccessorId != componentProposal.value().changeSetId
            && secondSuccessorId != successorId,
        "The second attempt must materialize its own direct successor of the frozen member");

    const auto retiredFirst = applyGate.evaluate(successorId);
    req(!retiredFirst.allowed
            && retiredFirst.reasonCode == "manifest_apply.manual_handoff_attempt_superseded",
        "An older green attempt's successor must lose Apply authority once a newer green "
        "attempt exists");
    const auto secondSuccessorEligibility = applyGate.evaluate(secondSuccessorId);
    req(secondSuccessorEligibility.allowed,
        "The newest successful attempt's genuine successor must be Apply-eligible");
    const auto integrationThroughSecond = applyGate.evaluate(
        integrationProposal.value().changeSetId);
    req(integrationThroughSecond.allowed,
        "An unchanged member must be authorized through the same newest successful attempt");

    // Without the manual-handoff repository wired, a manual-handoff-
    // workflow proposal must fail closed, never silently fall through to
    // kGateNotApplicableCode (which would mean "unguarded, always allowed").
    const auto unavailable = applyGateWithoutManualHandoff.evaluate(successorId);
    req(!unavailable.allowed
            && unavailable.reasonCode == "manifest_apply.manual_handoff_gate_unavailable",
        "A manual-handoff-workflow proposal must fail closed when this Backend composition "
        "cannot evaluate manual-handoff eligibility, never silently pass through");
}

// A source Run whose only diagnostic finding is provably pre-existing (the
// same warning already present in the source-only baseline overlay, before
// any candidate proposal is applied) has no SourceProposalOwner/
// IntegrationOwner finding at all - materialize() must fail closed rather
// than hand the user an empty or unscoped editable set.
void testMaterializeFailsClosedWhenNoOwnerFindingIsEditable()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Materialize pre-existing debt";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    // Same warning scripted for BOTH the baseline-only and candidate
    // overlays: the finding already existed before any proposal was
    // applied, so attribution must classify it as PreExistingVerificationDebt
    // (not SourceProposalOwner) - see
    // testProvesPreExistingDebtByExactBaselineFinding above.
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {warningOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(!materialized.success()
            && materialized.error()->code == "manual_handoff.no_editable_findings",
        "A Run with no editable owner finding must fail closed with no_editable_findings, "
        "never hand the user an unscoped or empty candidate");

    const auto stored = manualHandoffs.findHandoffByAnchorRun(source.id);
    req(stored.success() && !stored.value().has_value(),
        "A failed materialize() attempt must leave no durable handoff behind");
}

// QML-P4R.1 hardening: resolveLineageRoot() must fail CLOSED, never silently
// treat a broken chain as if it were the root, when a frozen member's own
// supersedesChangeSetId names a predecessor that cannot be read. Simulated
// via direct repository corruption (the real-world equivalent of a pruned or
// inconsistent durable store) - never producible through the legitimate
// repair API, which always requires a real, readable predecessor.
void testMaterializeFailsClosedOnMissingLineagePredecessor()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Lineage missing predecessor";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    auto corrupted = componentProposal.value();
    corrupted.supersedesChangeSetId = "missing-predecessor-id";
    req(proposalRepository.insertChangeSet(corrupted).success(),
        "Could not overwrite the component proposal with a corrupt lineage reference");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(!materialized.success()
            && materialized.error()->code == "manual_handoff.lineage_predecessor_missing",
        "A frozen member whose lineage chain names a missing predecessor must fail closed, "
        "never silently treat the corrupt id as if it were the lineage root");
}

// QML-P4R.1 hardening: the cyclic counterpart of the test above - a frozen
// member whose own supersedesChangeSetId names ITSELF must fail closed with
// a distinct lineage_cycle_detected code, never loop forever or silently
// accept the cycle as a root.
void testMaterializeFailsClosedOnCyclicLineage()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Lineage cycle";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    auto corrupted = componentProposal.value();
    corrupted.supersedesChangeSetId = corrupted.changeSetId;
    req(proposalRepository.insertChangeSet(corrupted).success(),
        "Could not overwrite the component proposal with a self-cyclic lineage reference");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(!materialized.success()
            && materialized.error()->code == "manual_handoff.lineage_cycle_detected",
        "A self-referencing supersedesChangeSetId lineage must fail closed with a distinct "
        "cycle-detected code");
}

// QML-P4R.1 hardening: a frozen member that is ITSELF already a repair
// successor (its own supersedesChangeSetId names a prior generation) must
// have its lineageRootProposalId resolve all the way back to the true,
// never-superseded ORIGINAL proposal - never merely itself - proving
// resolveLineageRoot's multi-hop walk, not only its trivial self-lineage
// case (already covered by the package-2 happy-path test).
void testMaterializeResolvesMultiHopLineageRootThroughPriorRepairGeneration()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Multi-hop lineage";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    // Generation 1: the true original, never-superseded proposal.
    const auto componentOriginal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component-gen1", "qml/App.qml", "Item {}"));
    req(componentOriginal.success()
            && componentOriginal.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real generation-1 component proposal");

    // Generation 2: a genuine repair successor (a prior, already-closed
    // manual-handoff or continuation repair cycle) - materialize() below
    // must walk PAST this generation to find generation 1, never freeze
    // this one's own id as the lineage root.
    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = source.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "manual-handoff";
    repairRequest.modelId = "manual-handoff";
    repairRequest.agentRole = "user";
    repairRequest.predecessorChangeSetId = componentOriginal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Prior repair generation",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    const auto componentProposal = proposals.materializeRepairSuccessor(repairRequest);
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real generation-2 repair successor");
    req(componentProposal.value().supersedesChangeSetId.has_value()
            && *componentProposal.value().supersedesChangeSetId
                == componentOriginal.value().changeSetId,
        "The generation-2 successor must genuinely supersede generation 1");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;
    req(handoff.frozenMembers.size() == 1
            && handoff.frozenMembers.front().proposalId == componentProposal.value().changeSetId
            && handoff.frozenMembers.front().owningRunId == source.id
            && handoff.frozenMembers.front().lineageRootProposalId
                == componentOriginal.value().changeSetId,
        "The frozen member's lineage root must resolve THROUGH the prior repair generation to "
        "the true original proposal, never merely to itself");
}

// QML-P4R.1 hardening: proves the anchor-based identity/persistence
// foundation and AG-16's new proposal-id-indexed reverse lookup already
// correctly support the real retained mixed-run scenario end to end - a
// frozen set whose members are owned by TWO DIFFERENT Runs (an origin
// Manifest Run and a paid continuation child). Written when materialize()
// itself still could not build such a handoff (QML-P4R.2's own job) - the
// handoff here is deliberately constructed directly through the repository
// rather than via materialize(). QML-P4R.2 has since taught materialize()
// to actually produce one for a real paid-continuation-child anchor - see
// testMaterializeResolvesMixedRunOriginAndProducesEligibleHandoff below,
// which exercises the real end-to-end path this test's own handoff was
// only ever a stand-in for. Both are kept: this one isolates AG-16's own
// reverse-lookup/routing logic from materialize()'s own machinery.
//
// P4R.4 now additionally makes this a real exact-identity proof: both frozen
// members carry their actual content hashes, and AG-16 re-binds each member's
// proposal id, owning Run, content hash, and lineage root before allowing it.
void testMixedRunFrozenSetAuthorizesBothOriginAndContinuationChildMembers()
{
    TempDir workspace;
    TempDir scratch;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, nullptr, nullptr, &manualHandoffs);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-mixed";
    project.name = "Mixed-run frozen set";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run origin;
    origin.id = "origin-run";
    origin.projectId = project.id;
    origin.title = "Origin Manifest Run";
    origin.task = "Origin task";
    origin.workflowId = kExecutionManifestWorkflowId;
    origin.status = RunStatus::Failed;
    origin.createdAt = now;
    req(runs.insertRun(origin).success(), "Could not seed origin Run");

    Run child;
    child.id = "continuation-child-run";
    child.projectId = project.id;
    child.title = "Paid continuation child";
    child.task = "Continuation task";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.parentRunId = origin.id;
    child.status = RunStatus::Failed;
    child.createdAt = now;
    req(runs.insertRun(child).success(), "Could not seed continuation child Run");

    const auto originProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, origin.id, project.id, "chg-origin", "qml/Origin.qml", "origin-content"));
    req(originProposal.success()
            && originProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real origin-owned proposal");
    const auto childProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, child.id, project.id, "chg-child", "qml/Child.qml", "child-content"));
    req(childProposal.success()
            && childProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real continuation-child-owned proposal");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = "workspace-mixed";
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        originProposal.value().changeSetId, childProposal.value().changeSetId};
    auto overlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(overlay.success(), "Could not build the mixed-run reference overlay");
    const std::string overlayFingerprint = overlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes = overlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(overlay.value()).success(),
        "Could not clean up the reference overlay");

    // QML-P4R.1: constructed directly - the shape QML-P4R.2's origin
    // resolution will eventually have materialize() itself produce for a
    // paid-continuation-child anchor.
    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:mixed";
    handoff.originManifestRunId = origin.id;
    handoff.verificationAnchorRunId = child.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "mixed-handoff-key";
    handoff.workspaceIdentity = "workspace-mixed";
    handoff.baselineFingerprint = baselineSnapshot.value().fingerprint;
    handoff.sourceOverlayFingerprint = std::string(64, 'f');
    handoff.frozenMembers = {
        {originProposal.value().changeSetId, origin.id, contentHashes.at(0),
            originProposal.value().changeSetId},
        {childProposal.value().changeSetId, child.id, contentHashes.at(1),
            childProposal.value().changeSetId}};
    handoff.allowedPaths = {
        {"qml/Origin.qml", originProposal.value().changeSetId, "task-origin"}};
    handoff.originalSnapshot = baselineSnapshot.value();
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(),
        "Could not persist the hand-constructed mixed-run handoff");

    Run attemptRun;
    attemptRun.id = "attempt-run-mixed";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Completed;
    attemptRun.createdAt = now;
    req(runs.insertRun(attemptRun).success(), "Could not seed the manual-handoff attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:mixed";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'c');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not persist the manual-handoff attempt record");

    ManifestProposalSetVerification verification;
    verification.schemaVersion = "manifest-proposal-set-verification.v1";
    verification.workspaceIdentity = "workspace-mixed";
    verification.baselineFingerprint = baselineSnapshot.value().fingerprint;
    verification.orderedProposalIds = overlayRequest.orderedProposalIds;
    verification.orderedProposalContentHashes = contentHashes;
    verification.overlayFingerprint = overlayFingerprint;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRun.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.baselineFingerprint = baselineSnapshot.value().fingerprint;
    entry.proposalIds = overlayRequest.orderedProposalIds;
    entry.verification = verification;
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(),
        "Could not seed the manual-handoff attempt's Work Ledger entry");

    // The origin Run's own unmodified proposal must be Apply-eligible
    // through the NEW proposal-id-indexed reverse lookup.
    const auto originEligibility = applyGate.evaluate(originProposal.value().changeSetId);
    req(originEligibility.allowed
            && originEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "An unmodified origin-Run-owned frozen member must be Apply-eligible through the "
        "mixed-run manual-handoff checkpoint chain");

    // The continuation child's own unmodified proposal must ALSO be
    // Apply-eligible - this is exactly the case the OLD Run-id-keyed
    // reverse lookup (gated to kExecutionManifestWorkflowId only) could
    // never reach, since the child's own workflow branch would already
    // have returned via its own direct forward path first.
    const auto childEligibility = applyGate.evaluate(childProposal.value().changeSetId);
    req(childEligibility.allowed
            && childEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "An unmodified continuation-child-owned frozen member must be Apply-eligible through "
        "the mixed-run manual-handoff checkpoint chain, not swallowed by the continuation "
        "workflow's own direct forward path");

    // Exact frozen order is part of the identity, not merely set membership.
    // A newer green sibling that reverses the two valid members becomes the
    // handoff-wide authority but is rejected as a substituted positional
    // representation; the older correct attempt is never used as fallback.
    Run reorderedRun = attemptRun;
    reorderedRun.id = "attempt-run-mixed-reordered";
    reorderedRun.createdAt = now + std::chrono::seconds(1);
    req(runs.insertRun(reorderedRun).success(), "Could not seed the reordered sibling Run");
    ManifestVerificationManualHandoffAttempt reorderedAttempt = attempt;
    reorderedAttempt.id = "manual-handoff-attempt:mixed-reordered";
    reorderedAttempt.attemptRunId = reorderedRun.id;
    reorderedAttempt.attemptOrdinal = 2;
    reorderedAttempt.previousAttemptId = attempt.id;
    reorderedAttempt.editedSnapshotFingerprint = std::string(64, 'd');
    reorderedAttempt.createdAt = reorderedRun.createdAt;
    req(manualHandoffs.insertAttempt(reorderedAttempt).success(),
        "Could not persist the reordered sibling attempt");
    ManifestProposalSetVerification reorderedVerification = verification;
    std::reverse(reorderedVerification.orderedProposalIds.begin(),
        reorderedVerification.orderedProposalIds.end());
    std::reverse(reorderedVerification.orderedProposalContentHashes.begin(),
        reorderedVerification.orderedProposalContentHashes.end());
    ManifestWorkLedgerEntry reorderedEntry = entry;
    reorderedEntry.runId = reorderedRun.id;
    reorderedEntry.proposalIds = reorderedVerification.orderedProposalIds;
    reorderedEntry.verification = reorderedVerification;
    reorderedEntry.createdAt = reorderedRun.createdAt;
    req(ledger.insertEntry(reorderedEntry).success(),
        "Could not seed the reordered sibling ledger");
    const auto originAfterReorder = applyGate.evaluate(originProposal.value().changeSetId);
    const auto childAfterReorder = applyGate.evaluate(childProposal.value().changeSetId);
    req(!originAfterReorder.allowed && !childAfterReorder.allowed,
        "A reordered newer attempt must block the whole handoff rather than falling back to "
        "an older green attempt");

    // A still-newer nominally-green sibling is likewise the handoff-wide
    // authority, not a per-proposal fallback hint. If its set is only a
    // subset, ALL members fail closed; AG-16 must not retrieve the missing
    // child member from either older green sibling.
    Run subsetRun = attemptRun;
    subsetRun.id = "attempt-run-mixed-subset";
    subsetRun.createdAt = now + std::chrono::seconds(2);
    req(runs.insertRun(subsetRun).success(), "Could not seed the subset sibling Run");
    ManifestVerificationManualHandoffAttempt subsetAttempt = attempt;
    subsetAttempt.id = "manual-handoff-attempt:mixed-subset";
    subsetAttempt.attemptRunId = subsetRun.id;
    subsetAttempt.attemptOrdinal = 3;
    subsetAttempt.previousAttemptId = reorderedAttempt.id;
    subsetAttempt.editedSnapshotFingerprint = std::string(64, 'e');
    subsetAttempt.createdAt = subsetRun.createdAt;
    req(manualHandoffs.insertAttempt(subsetAttempt).success(),
        "Could not persist the subset sibling attempt");
    ManifestProposalSetVerification subsetVerification = verification;
    subsetVerification.orderedProposalIds = {originProposal.value().changeSetId};
    subsetVerification.orderedProposalContentHashes = {contentHashes.at(0)};
    ManifestWorkLedgerEntry subsetEntry = entry;
    subsetEntry.runId = subsetRun.id;
    subsetEntry.proposalIds = subsetVerification.orderedProposalIds;
    subsetEntry.verification = subsetVerification;
    subsetEntry.createdAt = subsetRun.createdAt;
    req(ledger.insertEntry(subsetEntry).success(), "Could not seed the subset sibling ledger");

    const auto originAfterSubset = applyGate.evaluate(originProposal.value().changeSetId);
    const auto childAfterSubset = applyGate.evaluate(childProposal.value().changeSetId);
    req(!originAfterSubset.allowed && !childAfterSubset.allowed
            && originAfterSubset.reasonCode == "manifest_apply.verified_set_mismatch"
            && childAfterSubset.reasonCode == "manifest_apply.verified_set_mismatch",
        "A newer subset attempt must block the whole handoff, never mix authority with an "
        "older green sibling");

    // Re-freeze the same members only after a physical discard released the
    // immutable membership claims, this time with a syntactically valid but
    // false frozen content hash. The full green ledger still cannot launder
    // that mismatch: AG-16 independently re-hashes each frozen identity.
    req(manualHandoffs.discardHandoff(handoff.id).success(),
        "Could not discard the first hand-constructed handoff fixture");
    ManifestVerificationManualHandoff hashDivergedHandoff = handoff;
    hashDivergedHandoff.frozenMembers.front().contentHash = std::string(64, '0');
    req(manualHandoffs.insertHandoff(hashDivergedHandoff).success(),
        "Could not persist the hash-diverged handoff fixture");
    Run hashRun = attemptRun;
    hashRun.id = "attempt-run-mixed-hash-diverged";
    hashRun.createdAt = now + std::chrono::seconds(3);
    req(runs.insertRun(hashRun).success(), "Could not seed the hash-diverged attempt Run");
    ManifestVerificationManualHandoffAttempt hashAttempt = attempt;
    hashAttempt.id = "manual-handoff-attempt:mixed-hash-diverged";
    hashAttempt.attemptRunId = hashRun.id;
    hashAttempt.editedSnapshotFingerprint = std::string(64, 'f');
    hashAttempt.createdAt = hashRun.createdAt;
    req(manualHandoffs.insertAttempt(hashAttempt).success(),
        "Could not persist the hash-diverged attempt");
    ManifestWorkLedgerEntry hashEntry = entry;
    hashEntry.runId = hashRun.id;
    hashEntry.createdAt = hashRun.createdAt;
    req(ledger.insertEntry(hashEntry).success(), "Could not seed the hash-diverged ledger");

    const auto hashDiverged = applyGate.evaluate(originProposal.value().changeSetId);
    req(!hashDiverged.allowed
            && hashDiverged.reasonCode == "manifest_apply.proposal_set_diverged",
        "A frozen-member content-hash mismatch must fail closed despite a green full-set ledger");
}

// QML-P4R.2: the real retained scenario end to end, through materialize()
// itself (never hand-constructed) - origin Manifest Run -> real paid
// continuation child (via the actual ManifestVerificationContinuationPreparationService::
// prepare(), never a fabricated ManifestVerificationContinuationInput) ->
// the child's own genuine cross-Run repair successor -> materialize()
// resolving the origin through the durable continuation input, admitting
// the anchor through its own content-repairable-failure check (never the
// no-chaining admission_ checkpoint), and attributing findings through the
// merged origin+anchor task-disposition view. Zero provider/Planner calls
// anywhere in this test - the "repair" is materializeRepairSuccessor
// called directly, exactly like ManifestVerificationContinuationRepairService
// itself would, never a real provider session.
void testMaterializeResolvesMixedRunOriginAndProducesEligibleHandoff()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparationService(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);
    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, nullptr, &continuations, &manualHandoffs);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Mixed-run materialize";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification originDurable;
    originDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    originDurable.workspaceIdentity = project.id;
    originDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    originDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    originDurable.orderedProposalContentHashes = contentHashes;
    originDurable.overlayFingerprint = overlayFingerprint;
    originDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    originDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    originDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    originDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic originDiagnostic;
    originDiagnostic.code = "verification.qml_warning";
    originDiagnostic.message = "QML lint reported a warning";
    originDiagnostic.requirementId = "qml-lint";
    originDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    originDurable.diagnostics.push_back(std::move(originDiagnostic));

    ManifestWorkLedgerEntry originEntry;
    originEntry.runId = source.id;
    originEntry.projectId = project.id;
    originEntry.terminalStatus = RunStatus::Failed;
    originEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    originEntry.manifestSchemaVersion = "execution-manifest.v1";
    originEntry.baselineFingerprint = originDurable.baselineFingerprint;
    originEntry.verification = originDurable;
    originEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    originEntry.createdAt = now;
    req(ledger.insertEntry(originEntry).success(), "Could not seed the origin Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // prepare()'s own baseline(clean) + candidate(warning) verify.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto prepared = preparationService.prepare(source.id);
    req(prepared.success(), "Could not prepare a real paid continuation child");
    const Run childRun = prepared.value().childRun;
    req(childRun.workflowId == std::string(kManifestVerificationContinuationWorkflowId)
            && childRun.parentRunId == std::optional<RunId>(source.id),
        "The prepared child must carry the real continuation identity");

    // Simulate the child's own paid repair - never a real provider/Planner
    // call in this test - by materializing a genuine cross-Run successor
    // exactly like ManifestVerificationContinuationRepairService itself
    // would.
    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = childRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "test-provider";
    repairRequest.modelId = "test-model";
    repairRequest.agentRole = "code-agent";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Paid repair attempt",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real cross-Run repair successor");

    // The child's own final combined verification, mixing its own successor
    // with the origin's untouched integration proposal - the real retained
    // scenario's own shape.
    BuildManifestProposalOverlayRequest childOverlayRequest = overlayRequest;
    childOverlayRequest.orderedProposalIds = {
        successor.value().changeSetId, integrationProposal.value().changeSetId};
    auto childOverlay = ManifestProposalOverlay::build(childOverlayRequest, proposals);
    req(childOverlay.success(), "Could not build the child's own reference overlay");
    const std::string childOverlayFingerprint = childOverlay.value().overlayFingerprint;
    const std::vector<std::string> childContentHashes =
        childOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(childOverlay.value()).success(),
        "Could not clean up the child's own reference overlay");

    ManifestProposalSetVerification childDurable;
    childDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    childDurable.workspaceIdentity = project.id;
    childDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    childDurable.orderedProposalIds = childOverlayRequest.orderedProposalIds;
    childDurable.orderedProposalContentHashes = childContentHashes;
    childDurable.overlayFingerprint = childOverlayFingerprint;
    childDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    childDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    childDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    childDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic childDiagnostic;
    childDiagnostic.code = "verification.qml_warning";
    childDiagnostic.message = "QML lint still reports a warning after one repair attempt";
    childDiagnostic.requirementId = "qml-lint";
    childDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    childDurable.diagnostics.push_back(std::move(childDiagnostic));

    const auto childRunFetched = runs.findRun(childRun.id);
    req(childRunFetched.success() && childRunFetched.value().has_value(),
        "Could not refetch the prepared child Run");
    Run finishedChild = *childRunFetched.value();
    finishedChild.status = RunStatus::Failed;
    finishedChild.progress = 0.0;
    finishedChild.currentStageId.reset();
    finishedChild.finishedAt = now;
    req(runs.updateRun(finishedChild).success(), "Could not mark the child Run terminal Failed");

    ManifestWorkLedgerEntry childEntry;
    childEntry.runId = childRun.id;
    childEntry.projectId = project.id;
    childEntry.terminalStatus = RunStatus::Failed;
    childEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    childEntry.manifestSchemaVersion = "execution-manifest.v1";
    childEntry.baselineFingerprint = childDurable.baselineFingerprint;
    childEntry.verification = childDurable;
    // Only "component" - the ONE task the child itself repaired.
    // "integration" is deliberately absent: this is exactly what
    // QML-P4R.2's attribution merge (originLedgerForMixedAnchor) must fold
    // in from the ORIGIN's own ledger instead.
    childEntry.taskDispositions = {
        {"component", "succeeded", successor.value().changeSetId, std::nullopt}};
    childEntry.createdAt = now;
    req(ledger.insertEntry(childEntry).success(), "Could not seed the child's own Work Ledger entry");

    // materialize()'s own baseline(clean) + candidate(still warning) verify.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(childRun.id);
    req(materialized.success(),
        "A mixed-run anchor (real paid continuation child) must now materialize successfully");
    const auto& handoff = materialized.value().handoff;
    req(handoff.originManifestRunId == source.id && handoff.verificationAnchorRunId == childRun.id,
        "The handoff must resolve the real origin Manifest Run through the durable "
        "continuation input, distinct from the anchor");
    req(handoff.frozenMembers.size() == 2, "The frozen set must contain exactly both mixed members");
    bool sawSuccessorMember = false;
    bool sawOriginMember = false;
    for (const auto& member : handoff.frozenMembers) {
        if (member.proposalId == successor.value().changeSetId) {
            sawSuccessorMember = true;
            req(member.owningRunId == childRun.id,
                "The child's own successor member must carry the child as its owning Run");
            req(member.lineageRootProposalId == componentProposal.value().changeSetId,
                "The successor's lineage root must resolve back to the true original proposal");
        } else if (member.proposalId == integrationProposal.value().changeSetId) {
            sawOriginMember = true;
            req(member.owningRunId == source.id,
                "The untouched integration member must carry the origin as its owning Run");
        }
    }
    req(sawSuccessorMember && sawOriginMember,
        "The frozen set must contain both the child's own successor and the origin's unchanged "
        "member");
    req(!handoff.allowedPaths.empty() && handoff.allowedPaths.front().relativePath == "qml/App.qml",
        "The remaining owner finding must still be correctly attributed and editable");

    req(std::filesystem::exists(materialized.value().candidateRoot / "qml" / "App.qml")
            && std::filesystem::exists(materialized.value().candidateRoot / "qml" / "CMakeLists.txt"),
        "The materialized mixed-run candidate must contain both real files");

    const auto callsAfterFirstMaterialization = commandRunner.calls;
    const auto replayed = manualHandoffService.materialize(childRun.id);
    req(replayed.success() && replayed.value().handoff.id == handoff.id
            && commandRunner.calls == callsAfterFirstMaterialization,
        "An existing valid mixed-run handoff must be revalidated and returned idempotently "
        "without rerunning verification commands or rewriting immutable state");

    const auto failedPaidSet = applyGate.evaluate(successor.value().changeSetId);
    const auto failedOriginSet = applyGate.evaluate(integrationProposal.value().changeSetId);
    req(!failedPaidSet.allowed && !failedOriginSet.allowed
            && failedPaidSet.reasonCode
                == "manifest_apply.manual_handoff_verification_missing"
            && failedOriginSet.reasonCode
                == "manifest_apply.manual_handoff_verification_missing",
        "Neither the failed paid-anchor member nor the failed origin member may become "
        "Apply-eligible merely because a mixed handoff was materialized");

    // P4R.4: drive the REAL materialized mixed handoff through a user edit,
    // provider-free re-verification, and independent AG-16 evaluation. The
    // resulting exact set is [manual successor of the paid-child member,
    // unchanged origin member] in the frozen order.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not edit the real mixed-run candidate");
        appQml << "Item { anchors.centerIn: parent }";
    }
    commandRunner.queuedOutcomes = {cleanOutcome};
    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Completed
            && imported.value().finalVerification.has_value(),
        "The real mixed-run handoff must complete provider-free re-verification");
    const auto& finalIds = imported.value().finalVerification->orderedProposalIds;
    req(finalIds.size() == 2
            && finalIds.at(1) == integrationProposal.value().changeSetId
            && finalIds.at(0) != successor.value().changeSetId,
        "The final mixed set must preserve order and replace only the edited paid-child member");
    const auto manualSuccessor = proposals.findChangeSet(finalIds.at(0));
    req(manualSuccessor.success()
            && manualSuccessor.value().runId == imported.value().attemptRunId
            && manualSuccessor.value().supersedesChangeSetId
                == std::optional<ChangeSetId>(successor.value().changeSetId),
        "The edited member must be a genuine direct successor owned by the attempt Run");

    const auto manualSuccessorEligibility = applyGate.evaluate(finalIds.at(0));
    const auto originMemberEligibility = applyGate.evaluate(
        integrationProposal.value().changeSetId);
    req(manualSuccessorEligibility.allowed && originMemberEligibility.allowed,
        "AG-16 must independently authorize both representations from the same real mixed set");
    const auto replacedPaidMember = applyGate.evaluate(successor.value().changeSetId);
    req(!replacedPaidMember.allowed,
        "The replaced paid-child frozen member must not remain independently Apply-eligible");
}

// QML-P4R.2 hardening (P4R.1-acceptance review, item 3): the exact retained
// bb56e001-shaped scenario, driven all the way through a mixed-Run handoff -
// legacy .qtcreator drift at the ORIGIN -> a real normalization proof at
// prepare()-time -> a real paid continuation child -> the child's own
// genuine mixed repair (one amended proposal, one untouched origin member)
// -> a terminal Failed child -> materialize() itself. Proves two things at
// once: a mixed anchor whose baseline is already normalized (pinned by
// prepare() itself) never triggers a second, redundant normalization
// attempt keyed by the anchor's own id, and the resulting handoff is still
// produced correctly end to end - never an assumption taken on faith.
void testMaterializeResolvesMixedRunAfterLegacyBaselineNormalization()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparationService(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Mixed-run legacy normalization";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    std::error_code createError;
    std::filesystem::create_directories(workspace.path / ".qtcreator", createError);
    req(!createError, "Could not create the .qtcreator fixture directory");
    {
        std::ofstream qtcreator(workspace.path / ".qtcreator" / "CMakeLists.txt.user",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(qtcreator), "Could not write the .qtcreator fixture file");
        qtcreator << "<original-session/>";
    }

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    auto legacyPolicy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    legacyPolicy.deniedDirectoryNames.erase(
        std::remove(legacyPolicy.deniedDirectoryNames.begin(),
            legacyPolicy.deniedDirectoryNames.end(), std::string(".qtcreator")),
        legacyPolicy.deniedDirectoryNames.end());
    const auto legacyBaselineSnapshot = WorkspaceBaselineProvenance::capture(legacyPolicy);
    req(legacyBaselineSnapshot.success()
            && std::any_of(legacyBaselineSnapshot.value().files.begin(),
                legacyBaselineSnapshot.value().files.end(), [](const auto& entry) {
                    return entry.relativePath.starts_with(".qtcreator/");
                }),
        "Could not capture the legacy (.qtcreator-visible) workspace baseline");
    const auto legacyBaselineJson =
        WorkspaceBaselineProvenance::renderJson(legacyBaselineSnapshot.value());
    req(legacyBaselineJson.success(), "Could not render the legacy baseline artifact");
    req(artifacts.createArtifact(source.id, std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", legacyBaselineJson.value())
            .success(),
        "Could not persist the durable legacy baseline artifact");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = legacyPolicy;
    overlayRequest.baselineSnapshot = legacyBaselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the legacy reference candidate overlay");
    const std::string legacyOverlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the legacy reference overlay");

    ManifestProposalSetVerification originDurable;
    originDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    originDurable.workspaceIdentity = project.id;
    originDurable.baselineFingerprint = legacyBaselineSnapshot.value().fingerprint;
    originDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    originDurable.orderedProposalContentHashes = contentHashes;
    originDurable.overlayFingerprint = legacyOverlayFingerprint;
    originDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    originDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    originDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    originDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic originDiagnostic;
    originDiagnostic.code = "verification.qml_warning";
    originDiagnostic.message = "QML lint reported a warning";
    originDiagnostic.requirementId = "qml-lint";
    originDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    originDurable.diagnostics.push_back(std::move(originDiagnostic));

    ManifestWorkLedgerEntry originEntry;
    originEntry.runId = source.id;
    originEntry.projectId = project.id;
    originEntry.terminalStatus = RunStatus::Failed;
    originEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    originEntry.manifestSchemaVersion = "execution-manifest.v1";
    originEntry.baselineFingerprint = originDurable.baselineFingerprint;
    originEntry.verification = originDurable;
    originEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    originEntry.createdAt = now;
    req(ledger.insertEntry(originEntry).success(), "Could not seed the origin Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto currentBaseline = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(workspace.path));
    req(currentBaseline.success()
            && std::none_of(currentBaseline.value().files.begin(),
                currentBaseline.value().files.end(), [](const auto& entry) {
                    return entry.relativePath.starts_with(".qtcreator/");
                }),
        "Could not capture the current, policy-excluded workspace baseline");
    req(currentBaseline.value().fingerprint != originDurable.baselineFingerprint,
        "Test fixture did not actually produce baseline drift");

    const auto prepared = preparationService.prepare(source.id);
    req(prepared.success(), "A Run whose drift is limited to .qtcreator/ must still prepare");
    const Run childRun = prepared.value().childRun;
    req(childRun.workflowId == std::string(kManifestVerificationContinuationWorkflowId)
            && childRun.parentRunId == std::optional<RunId>(source.id),
        "The prepared child must carry the real continuation identity");
    req(prepared.value().input.baselineFingerprint == currentBaseline.value().fingerprint,
        "The frozen continuation input must pin the normalized current baseline identity");

    const auto originNormalization = baselineNormalizations.findBySourceRun(source.id);
    req(originNormalization.success() && originNormalization.value().has_value(),
        "A durable normalization proof must be recorded for the ORIGIN");

    // Simulate the child's own paid repair - never a real provider/Planner
    // call in this test - by materializing a genuine cross-Run successor
    // exactly like ManifestVerificationContinuationRepairService itself
    // would.
    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = childRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "test-provider";
    repairRequest.modelId = "test-model";
    repairRequest.agentRole = "code-agent";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Paid repair attempt",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real cross-Run repair successor");

    // The child's own final combined verification, built against the SAME
    // already-normalized current baseline prepare() itself pinned - a
    // genuinely already-normalized child never drifts again on its own.
    const auto childPolicy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    BuildManifestProposalOverlayRequest childOverlayRequest;
    childOverlayRequest.workspaceRoot = workspace.path;
    childOverlayRequest.trustedScratchRoot = scratch.path;
    childOverlayRequest.workspaceIdentity = project.id;
    childOverlayRequest.baselinePolicy = childPolicy;
    childOverlayRequest.baselineSnapshot = currentBaseline.value();
    childOverlayRequest.orderedProposalIds = {
        successor.value().changeSetId, integrationProposal.value().changeSetId};
    auto childOverlay = ManifestProposalOverlay::build(childOverlayRequest, proposals);
    req(childOverlay.success(), "Could not build the child's own reference overlay");
    const std::string childOverlayFingerprint = childOverlay.value().overlayFingerprint;
    const std::vector<std::string> childContentHashes =
        childOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(childOverlay.value()).success(),
        "Could not clean up the child's own reference overlay");

    ManifestProposalSetVerification childDurable;
    childDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    childDurable.workspaceIdentity = project.id;
    childDurable.baselineFingerprint = currentBaseline.value().fingerprint;
    childDurable.orderedProposalIds = childOverlayRequest.orderedProposalIds;
    childDurable.orderedProposalContentHashes = childContentHashes;
    childDurable.overlayFingerprint = childOverlayFingerprint;
    childDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    childDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    childDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    childDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic childDiagnostic;
    childDiagnostic.code = "verification.qml_warning";
    childDiagnostic.message = "QML lint still reports a warning after one repair attempt";
    childDiagnostic.requirementId = "qml-lint";
    childDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    childDurable.diagnostics.push_back(std::move(childDiagnostic));

    const auto childRunFetched = runs.findRun(childRun.id);
    req(childRunFetched.success() && childRunFetched.value().has_value(),
        "Could not refetch the prepared child Run");
    Run finishedChild = *childRunFetched.value();
    finishedChild.status = RunStatus::Failed;
    finishedChild.progress = 0.0;
    finishedChild.currentStageId.reset();
    finishedChild.finishedAt = now;
    req(runs.updateRun(finishedChild).success(), "Could not mark the child Run terminal Failed");

    ManifestWorkLedgerEntry childEntry;
    childEntry.runId = childRun.id;
    childEntry.projectId = project.id;
    childEntry.terminalStatus = RunStatus::Failed;
    childEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    childEntry.manifestSchemaVersion = "execution-manifest.v1";
    childEntry.baselineFingerprint = childDurable.baselineFingerprint;
    childEntry.verification = childDurable;
    // Only "component" - the ONE task the child itself repaired.
    childEntry.taskDispositions = {
        {"component", "succeeded", successor.value().changeSetId, std::nullopt}};
    childEntry.createdAt = now;
    req(ledger.insertEntry(childEntry).success(), "Could not seed the child's own Work Ledger entry");

    // materialize()'s own baseline(clean) + candidate(still warning) verify.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(childRun.id);
    req(materialized.success(),
        "A mixed-run anchor whose baseline was already normalized at the origin must "
        "materialize successfully without attempting to renormalize under its own id");
    const auto& handoff = materialized.value().handoff;
    req(handoff.originManifestRunId == source.id && handoff.verificationAnchorRunId == childRun.id,
        "The handoff must resolve the real origin Manifest Run through the durable continuation "
        "input");
    req(handoff.frozenMembers.size() == 2, "The frozen set must contain exactly both mixed members");

    // The decisive proof of the explicit guard: normalizeOrEvaluate must
    // NEVER have been invoked keyed by the anchor's own id - no second,
    // redundant proof was minted for a mixed anchor.
    const auto anchorNormalization = baselineNormalizations.findBySourceRun(childRun.id);
    req(anchorNormalization.success() && !anchorNormalization.value().has_value(),
        "materialize() must never attempt (or mint a proof for) renormalization keyed by a "
        "mixed anchor's own id");

    req(std::filesystem::exists(materialized.value().candidateRoot / "qml" / "App.qml")
            && std::filesystem::exists(materialized.value().candidateRoot / "qml" / "CMakeLists.txt"),
        "The materialized mixed-run candidate must contain both real files");
}

// QML-P4R.2 hardening (P4R.1-acceptance review, item 3, second half): once a
// mixed anchor's baseline has been established, ANY further real workspace
// drift since the anchor's own verification must fail honestly with
// manual_handoff.baseline_drift - never be silently re-normalized, since
// legacy-baseline normalization is scoped to a single sourceRun's own
// proposal set and a mixed anchor's frozen set never satisfies that
// contract by construction.
void testMaterializeFailsClosedOnGenuineDriftForMixedRunAnchor()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparationService(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Mixed-run genuine drift";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification originDurable;
    originDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    originDurable.workspaceIdentity = project.id;
    originDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    originDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    originDurable.orderedProposalContentHashes = contentHashes;
    originDurable.overlayFingerprint = overlayFingerprint;
    originDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    originDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    originDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    originDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic originDiagnostic;
    originDiagnostic.code = "verification.qml_warning";
    originDiagnostic.message = "QML lint reported a warning";
    originDiagnostic.requirementId = "qml-lint";
    originDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    originDurable.diagnostics.push_back(std::move(originDiagnostic));

    ManifestWorkLedgerEntry originEntry;
    originEntry.runId = source.id;
    originEntry.projectId = project.id;
    originEntry.terminalStatus = RunStatus::Failed;
    originEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    originEntry.manifestSchemaVersion = "execution-manifest.v1";
    originEntry.baselineFingerprint = originDurable.baselineFingerprint;
    originEntry.verification = originDurable;
    originEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    originEntry.createdAt = now;
    req(ledger.insertEntry(originEntry).success(), "Could not seed the origin Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto prepared = preparationService.prepare(source.id);
    req(prepared.success(), "Could not prepare a real paid continuation child");
    const Run childRun = prepared.value().childRun;

    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = childRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "test-provider";
    repairRequest.modelId = "test-model";
    repairRequest.agentRole = "code-agent";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Paid repair attempt",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real cross-Run repair successor");

    BuildManifestProposalOverlayRequest childOverlayRequest = overlayRequest;
    childOverlayRequest.orderedProposalIds = {
        successor.value().changeSetId, integrationProposal.value().changeSetId};
    auto childOverlay = ManifestProposalOverlay::build(childOverlayRequest, proposals);
    req(childOverlay.success(), "Could not build the child's own reference overlay");
    const std::string childOverlayFingerprint = childOverlay.value().overlayFingerprint;
    const std::vector<std::string> childContentHashes =
        childOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(childOverlay.value()).success(),
        "Could not clean up the child's own reference overlay");

    ManifestProposalSetVerification childDurable;
    childDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    childDurable.workspaceIdentity = project.id;
    childDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    childDurable.orderedProposalIds = childOverlayRequest.orderedProposalIds;
    childDurable.orderedProposalContentHashes = childContentHashes;
    childDurable.overlayFingerprint = childOverlayFingerprint;
    childDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    childDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    childDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    childDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic childDiagnostic;
    childDiagnostic.code = "verification.qml_warning";
    childDiagnostic.message = "QML lint still reports a warning after one repair attempt";
    childDiagnostic.requirementId = "qml-lint";
    childDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    childDurable.diagnostics.push_back(std::move(childDiagnostic));

    const auto childRunFetched = runs.findRun(childRun.id);
    req(childRunFetched.success() && childRunFetched.value().has_value(),
        "Could not refetch the prepared child Run");
    Run finishedChild = *childRunFetched.value();
    finishedChild.status = RunStatus::Failed;
    finishedChild.progress = 0.0;
    finishedChild.currentStageId.reset();
    finishedChild.finishedAt = now;
    req(runs.updateRun(finishedChild).success(), "Could not mark the child Run terminal Failed");

    ManifestWorkLedgerEntry childEntry;
    childEntry.runId = childRun.id;
    childEntry.projectId = project.id;
    childEntry.terminalStatus = RunStatus::Failed;
    childEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    childEntry.manifestSchemaVersion = "execution-manifest.v1";
    childEntry.baselineFingerprint = childDurable.baselineFingerprint;
    childEntry.verification = childDurable;
    childEntry.taskDispositions = {
        {"component", "succeeded", successor.value().changeSetId, std::nullopt}};
    childEntry.createdAt = now;
    req(ledger.insertEntry(childEntry).success(), "Could not seed the child's own Work Ledger entry");

    // Genuine, real drift on disk since the child's own final verification -
    // never approved by any legacy-normalization prefix, and never covered
    // by the origin's own already-consumed normalization proof.
    {
        std::ofstream drift(workspace.path / "unexpected-drift.txt",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(drift), "Could not write the genuine drift fixture file");
        drift << "unapproved change";
    }

    const auto materialized = manualHandoffService.materialize(childRun.id);
    req(!materialized.success() && materialized.error()->code == "manual_handoff.baseline_drift",
        "Genuine workspace drift since a mixed anchor's own verification must fail closed as "
        "baseline_drift, never be silently re-normalized");

    const auto anchorNormalization = baselineNormalizations.findBySourceRun(childRun.id);
    req(anchorNormalization.success() && !anchorNormalization.value().has_value(),
        "A mixed anchor's genuine drift must never even attempt (or mint a proof for) "
        "renormalization keyed by its own id");
}

// QML-P4R.2 post-review hardening: set equality is insufficient. A legacy
// immutable handoff whose roots are all genuine but reordered relative to
// the continuation input must be revalidated (never returned by the old
// idempotent fast path) and rejected without rewriting that row.
void testMaterializeRejectsReorderedLineageInExistingMixedRunHandoff()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparationService(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Mixed-run lineage set mismatch";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification originDurable;
    originDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    originDurable.workspaceIdentity = project.id;
    originDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    originDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    originDurable.orderedProposalContentHashes = contentHashes;
    originDurable.overlayFingerprint = overlayFingerprint;
    originDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    originDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    originDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    originDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic originDiagnostic;
    originDiagnostic.code = "verification.qml_warning";
    originDiagnostic.message = "QML lint reported a warning";
    originDiagnostic.requirementId = "qml-lint";
    originDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    originDurable.diagnostics.push_back(std::move(originDiagnostic));

    ManifestWorkLedgerEntry originEntry;
    originEntry.runId = source.id;
    originEntry.projectId = project.id;
    originEntry.terminalStatus = RunStatus::Failed;
    originEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    originEntry.manifestSchemaVersion = "execution-manifest.v1";
    originEntry.baselineFingerprint = originDurable.baselineFingerprint;
    originEntry.verification = originDurable;
    originEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    originEntry.createdAt = now;
    req(ledger.insertEntry(originEntry).success(), "Could not seed the origin Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto prepared = preparationService.prepare(source.id);
    req(prepared.success(), "Could not prepare a real paid continuation child");
    const Run childRun = prepared.value().childRun;

    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = childRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "test-provider";
    repairRequest.modelId = "test-model";
    repairRequest.agentRole = "code-agent";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Paid repair attempt",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real cross-Run repair successor");

    BuildManifestProposalOverlayRequest childOverlayRequest = overlayRequest;
    childOverlayRequest.orderedProposalIds = {
        integrationProposal.value().changeSetId, successor.value().changeSetId};
    auto childOverlay = ManifestProposalOverlay::build(childOverlayRequest, proposals);
    req(childOverlay.success(), "Could not build the child's own reference overlay");
    const std::string childOverlayFingerprint = childOverlay.value().overlayFingerprint;
    const std::vector<std::string> childContentHashes =
        childOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(childOverlay.value()).success(),
        "Could not clean up the child's own reference overlay");

    ManifestProposalSetVerification childDurable;
    childDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    childDurable.workspaceIdentity = project.id;
    childDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    childDurable.orderedProposalIds = childOverlayRequest.orderedProposalIds;
    childDurable.orderedProposalContentHashes = childContentHashes;
    childDurable.overlayFingerprint = childOverlayFingerprint;
    childDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    childDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    childDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    childDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic childDiagnostic;
    childDiagnostic.code = "verification.qml_warning";
    childDiagnostic.message = "QML lint still reports a warning after one repair attempt";
    childDiagnostic.requirementId = "qml-lint";
    childDiagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    childDurable.diagnostics.push_back(std::move(childDiagnostic));

    const auto childRunFetched = runs.findRun(childRun.id);
    req(childRunFetched.success() && childRunFetched.value().has_value(),
        "Could not refetch the prepared child Run");
    Run finishedChild = *childRunFetched.value();
    finishedChild.status = RunStatus::Failed;
    finishedChild.progress = 0.0;
    finishedChild.currentStageId.reset();
    finishedChild.finishedAt = now;
    req(runs.updateRun(finishedChild).success(), "Could not mark the child Run terminal Failed");

    ManifestWorkLedgerEntry childEntry;
    childEntry.runId = childRun.id;
    childEntry.projectId = project.id;
    childEntry.terminalStatus = RunStatus::Failed;
    childEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    childEntry.manifestSchemaVersion = "execution-manifest.v1";
    childEntry.baselineFingerprint = childDurable.baselineFingerprint;
    childEntry.verification = childDurable;
    childEntry.taskDispositions = {
        {"component", "succeeded", successor.value().changeSetId, std::nullopt}};
    childEntry.createdAt = now;
    req(ledger.insertEntry(childEntry).success(), "Could not seed the child's own Work Ledger entry");

    ManifestVerificationManualHandoff legacyHandoff;
    legacyHandoff.id = "manual-handoff:" + childRun.id;
    legacyHandoff.originManifestRunId = source.id;
    legacyHandoff.verificationAnchorRunId = childRun.id;
    legacyHandoff.sourceProjectId = project.id;
    legacyHandoff.storageKey = "pre-hardening-reordered-handoff";
    legacyHandoff.workspaceIdentity = childDurable.workspaceIdentity;
    legacyHandoff.baselineFingerprint = childDurable.baselineFingerprint;
    legacyHandoff.sourceOverlayFingerprint = childDurable.overlayFingerprint;
    legacyHandoff.frozenMembers = {
        {integrationProposal.value().changeSetId, source.id, childContentHashes.at(0),
            integrationProposal.value().changeSetId},
        {successor.value().changeSetId, childRun.id, childContentHashes.at(1),
            componentProposal.value().changeSetId}};
    legacyHandoff.allowedPaths = {
        {"qml/App.qml", successor.value().changeSetId, "component"}};
    legacyHandoff.originalSnapshot = baselineSnapshot.value();
    legacyHandoff.createdAt = now;
    req(manualHandoffs.insertHandoff(legacyHandoff).success(),
        "Could not seed the immutable pre-hardening handoff");

    const auto materialized = manualHandoffService.materialize(childRun.id);
    req(!materialized.success()
            && materialized.error()->code == "manual_handoff.lineage_set_mismatch",
        "A pre-hardening mixed handoff with the right root set in the wrong order must be "
        "revalidated and rejected, never returned or rewritten");
}

// QML-P4R.2 hardening (P4R.1-acceptance review, item 2): a hand-corrupted
// durable continuation input whose own sourceProjectId no longer agrees
// with the real anchor Run's projectId must fail closed - findInputByChildRun
// only proves internal validity and childRunId uniqueness, never that the
// row still agrees with the real Run rows it references.
void testMaterializeFailsClosedOnCorruptedContinuationInputProjectIdentity()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Corrupted continuation input identity";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run origin;
    origin.id = "origin-run";
    origin.projectId = project.id;
    origin.title = "Origin Manifest Run";
    origin.task = "Origin task";
    origin.workflowId = kExecutionManifestWorkflowId;
    origin.status = RunStatus::Failed;
    origin.createdAt = now;
    req(runs.insertRun(origin).success(), "Could not seed origin Run");

    Run child;
    child.id = "continuation-child-run";
    child.projectId = project.id;
    child.title = "Paid continuation child";
    child.task = "Continuation task";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.parentRunId = origin.id;
    child.status = RunStatus::Failed;
    child.createdAt = now;
    req(runs.insertRun(child).success(), "Could not seed continuation child Run");

    // A hand-corrupted continuation input: internally valid (schema, hex
    // content hashes, self-consistent fingerprint) and correctly names this
    // child/origin pair by id, but claims a DIFFERENT sourceProjectId than
    // the real anchor Run actually carries - exactly the class of row a
    // corrupted/hand-edited database could produce.
    ManifestVerificationContinuationInput input;
    input.sourceRunId = origin.id;
    input.childRunId = child.id;
    input.sourceProjectId = "project-mismatch";
    input.attributionId = "attribution-x";
    input.baselineFingerprint = "baseline-x";
    input.sourceWorkspaceIdentity = "workspace-x";
    input.sourceOverlayFingerprint = "overlay-x";
    input.orderedProposalIds = {"chg-placeholder"};
    input.orderedProposalContentHashes = {std::string(64, 'a')};
    input.createdAt = now;
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(continuations.insertInput(input).success(),
        "Could not seed the hand-corrupted continuation input");

    const auto materialized = manualHandoffService.materialize(child.id);
    req(!materialized.success()
            && materialized.error()->code == "manual_handoff.origin_identity_conflict",
        "A continuation input whose own sourceProjectId disagrees with the real anchor Run's "
        "project must fail closed, never be trusted on the strength of matching ids alone");
}

// QML-P4R.2 hardening (P4R.1-acceptance review, item 2, second half): even
// when the continuation input's own recorded fields agree with the anchor
// Run, the origin Run it names must itself be proven to still own a
// Manifest for the anchor's own project - a stale/repurposed origin Run id
// (deleted, reassigned to a different workflow, or moved to another
// project) must fail closed rather than silently be trusted.
void testMaterializeFailsClosedWhenOriginRunNoLongerOwnsAManifest()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Stale origin Run";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    // The "origin" Run exists but does NOT own a Manifest - its workflow is
    // some other, non-Manifest workflow (e.g. it was later repurposed, or
    // the id was reused by an unrelated Run).
    Run origin;
    origin.id = "origin-run";
    origin.projectId = project.id;
    origin.title = "Not actually a Manifest Run";
    origin.task = "Unrelated task";
    origin.workflowId = "some-other-workflow";
    origin.status = RunStatus::Failed;
    origin.createdAt = now;
    req(runs.insertRun(origin).success(), "Could not seed the stale origin Run");

    Run child;
    child.id = "continuation-child-run";
    child.projectId = project.id;
    child.title = "Paid continuation child";
    child.task = "Continuation task";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.parentRunId = origin.id;
    child.status = RunStatus::Failed;
    child.createdAt = now;
    req(runs.insertRun(child).success(), "Could not seed continuation child Run");

    ManifestVerificationContinuationInput input;
    input.sourceRunId = origin.id;
    input.childRunId = child.id;
    input.sourceProjectId = project.id;
    input.attributionId = "attribution-x";
    input.baselineFingerprint = "baseline-x";
    input.sourceWorkspaceIdentity = "workspace-x";
    input.sourceOverlayFingerprint = "overlay-x";
    input.orderedProposalIds = {"chg-placeholder"};
    input.orderedProposalContentHashes = {std::string(64, 'a')};
    input.createdAt = now;
    input.inputFingerprint = computeManifestVerificationContinuationInputFingerprint(input);
    req(continuations.insertInput(input).success(),
        "Could not seed the continuation input naming a stale/repurposed origin Run");

    const auto materialized = manualHandoffService.materialize(child.id);
    req(!materialized.success()
            && materialized.error()->code == "manual_handoff.origin_identity_conflict",
        "An origin Run that no longer owns a Manifest workflow must fail closed, never be "
        "trusted merely because the continuation input names its id");
}

// QML-P4R.3 package 1: materialize()'s idempotent existing-handoff fast path
// must validate the candidate directory's real on-disk state on EVERY load,
// never merely trust the immutable handoff row. A live user edit (a changed
// fingerprint) must NOT block the idempotent return - that is the entire
// point of a manual handoff - but a candidate directory that has actually
// disappeared since materialization must fail closed rather than silently
// handing back a path to nothing.
void testMaterializeRevalidatesExistingCandidateHealthOnEveryLoad()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Candidate health on load";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    // A live user edit changes the candidate's own fingerprint - this must
    // NOT block the idempotent fast path on the next call.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not simulate a user edit on the real candidate file");
        appQml << "Item { anchors.fill: parent }";
    }
    const auto afterEdit = manualHandoffService.materialize(source.id);
    req(afterEdit.success() && afterEdit.value().handoff.id == handoff.id
            && afterEdit.value().candidateRoot == materialized.value().candidateRoot,
        "A live edit to the candidate must never block the idempotent existing-handoff return - "
        "editing is the entire point of a manual handoff");

    // The directory genuinely disappearing (external deletion, disk cleanup,
    // a corrupted mount) is a real structural problem and must fail closed
    // rather than silently handing back a path to nothing.
    std::error_code removeError;
    const auto removedCount =
        std::filesystem::remove_all(materialized.value().candidateRoot, removeError);
    req(!removeError && removedCount > 0, "Could not delete the real candidate directory");
    const auto afterDelete = manualHandoffService.materialize(source.id);
    req(!afterDelete.success()
            && afterDelete.error()->code == "manual_handoff.candidate_unavailable",
        "A missing candidate directory must fail closed as candidate_unavailable, never be "
        "silently returned as if it still existed");
}

// QML-P4R.3 package 2: discardHandoff() must be a genuine physical delete
// that releases the frozen members' own GLOBAL UNIQUE(proposal_id) claim -
// the exact risk the P4R.1-acceptance review named. Proves the full
// contract: the row and every frozen-member claim are gone, the candidate
// directory is removed from disk, a fresh materialize() call for the same
// anchor Run is free to refreeze the same proposals into a brand-new
// handoff, and discarding an unknown id fails closed.
// (defined with the retention tests below)
ManifestVerificationManualHandoff makeMinimalRetentionHandoff(
    const RunId& anchorRunId, const ProjectId& projectId, const std::string& storageKey,
    const ChangeSetId& proposalId, std::chrono::system_clock::time_point createdAt);

void testDiscardHandoffReleasesProposalClaimAndRemovesCandidate()
{
    outcomebrief_hooks::RecordingTrigger outcomeBriefTrigger;   // PC-4C: records the nudges to the Outcome Brief
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);
    manualHandoffService.setOutcomeBriefTrigger(&outcomeBriefTrigger);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Discard releases proposal claim";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    // PC-4C: the pool is exactly at capacity (every handoff old enough to be evicted), so the materialize below has to evict the oldest
    // of them - and the brief of THAT handoff's anchor changes as well as the one of the new handoff.
    for (std::size_t i = 0; i < kManifestVerificationManualHandoffMaxRetainedHandoffs; ++i) {
        const auto createdAt = now - std::chrono::hours(100 + static_cast<int>(kManifestVerificationManualHandoffMaxRetainedHandoffs - i));
        auto pooled = makeMinimalRetentionHandoff("pool-anchor-" + std::to_string(i), project.id, "pool-storage-" + std::to_string(i),
            "chg-pool-" + std::to_string(i), createdAt);
        req(manualHandoffs.insertHandoff(pooled).success(), "Could not seed a pooled handoff");
    }

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;
    const auto candidateRoot = materialized.value().candidateRoot;
    req(!manualHandoffs.findHandoffById("manual-handoff:pool-anchor-0").value().has_value()
            && manualHandoffs.listAllHandoffs().value().size() == kManifestVerificationManualHandoffMaxRetainedHandoffs,
        "The materialize evicted exactly the oldest pooled handoff to make room");
    req(outcomeBriefTrigger.runCalls("pool-anchor-0") == 1, "...and nudged the Outcome Brief of the anchor it evicted");
    for (std::size_t i = 1; i < kManifestVerificationManualHandoffMaxRetainedHandoffs; ++i) {
        req(outcomeBriefTrigger.runCalls("pool-anchor-" + std::to_string(i)) == 0, "...and of no handoff that stayed");
    }
    req(std::filesystem::exists(candidateRoot), "The materialized candidate must exist on disk");

    const auto beforeDiscard = manualHandoffs.findHandoffByMemberProposalId(
        componentProposal.value().changeSetId);
    req(beforeDiscard.success() && beforeDiscard.value().has_value()
            && beforeDiscard.value()->id == handoff.id,
        "The component proposal must be claimed by the handoff before discard");

    const auto nudgesBeforeDiscard = outcomeBriefTrigger.runCalls(source.id);
    const auto discardResult = manualHandoffService.discardHandoff(handoff.id);
    req(discardResult.success(), "Discarding an existing, inactive handoff must succeed");
    // PC-4C: the anchor is read BEFORE the row is deleted, and its brief is nudged once the handoff is gone.
    req(outcomeBriefTrigger.runCalls(source.id) == nudgesBeforeDiscard + 1, "Discarding a handoff must nudge its anchor");

    const auto afterDiscard = manualHandoffs.findHandoffById(handoff.id);
    req(afterDiscard.success() && !afterDiscard.value().has_value(),
        "The handoff row must be genuinely gone after discard - never merely hidden");
    const auto claimAfterDiscard = manualHandoffs.findHandoffByMemberProposalId(
        componentProposal.value().changeSetId);
    req(claimAfterDiscard.success() && !claimAfterDiscard.value().has_value(),
        "Discard must release the frozen member's own GLOBAL UNIQUE(proposal_id) claim - the "
        "exact risk the P4R.1-acceptance review named");
    req(!std::filesystem::exists(candidateRoot),
        "Discard must remove the candidate directory from disk after the database delete commits");

    // The proposal is now free to be refrozen into a brand-new handoff -
    // the whole point of releasing the claim. A second materialize() call
    // for the SAME anchor Run must succeed and produce a genuinely fresh
    // materialization. The handoff id itself is deterministic
    // ("manual-handoff:" + verificationAnchorRunId - see materialize()'s own
    // construction) and is therefore EXPECTED to be identical to the
    // discarded one's, since both share the same anchor Run; storageKey (a
    // freshly random overlay directory name each materialize() call) is
    // what actually distinguishes a genuinely new materialization from a
    // stale reference.
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};
    const auto rematerialized = manualHandoffService.materialize(source.id);
    req(rematerialized.success(),
        "A discarded handoff's proposals must be freely refreezable after discard");
    req(rematerialized.value().handoff.storageKey != handoff.storageKey,
        "The refrozen handoff must materialize into a genuinely fresh candidate directory, never "
        "reuse the discarded one's storage key");
    req(std::filesystem::exists(rematerialized.value().candidateRoot),
        "The refrozen handoff's candidate directory must actually exist on disk");

    const auto unknownDiscard = manualHandoffService.discardHandoff("no-such-handoff");
    req(!unknownDiscard.success() && unknownDiscard.error()->code == "manual_handoff.not_found",
        "Discarding an unknown handoff id must fail closed, never silently succeed");
    req(outcomeBriefTrigger.runCalls(source.id) == nudgesBeforeDiscard + 1 + 1 && outcomeBriefTrigger.runs().size() == 4,
        "...and an unknown handoff has no anchor, so it nudges nothing (only the re-materialize before it did, and no pooled handoff "
        "was evicted again)");
}

// QML-P4R.3 package 2: discardHandoff() must refuse while any of the
// handoff's own reverify attempts still has a Queued/Running/Cancelling Run - deleting
// the row out from under an in-progress importAndReverify() would corrupt
// that call's own view of the world. The handoff must remain completely
// untouched after the refusal.
void testDiscardHandoffRefusesWhileAttemptActive()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Discard refuses active attempt";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    // A hand-constructed handoff - materialize()'s own real path is already
    // proven by the sibling test above; this test only needs a real,
    // persisted handoff row plus a real, active attempt Run against it.
    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:active-attempt";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "active-attempt-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {{"chg-component", source.id, std::string(64, 'c'), "chg-component"}};
    handoff.allowedPaths = {{"qml/App.qml", "chg-component", "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    // QML-P4R.5 package-2 hardening: Discard is cooperatively cancellable
    // up to (but never after) its irreversible DB-first commit boundary. A
    // token already stopped before dispatch must leave both the immutable
    // handoff and all of its claims untouched.
    std::stop_source cancelledDiscard;
    req(cancelledDiscard.request_stop(), "Could not request the discard cancellation fixture");
    const auto preCancelledDiscard = manualHandoffService.discardHandoff(
        handoff.id, cancelledDiscard.get_token());
    req(!preCancelledDiscard.success()
            && preCancelledDiscard.error()->code == "manual_handoff.discard_cancelled",
        "A pre-commit discard cancellation must surface its stable cancellation code");
    const auto stillThereAfterCancellation = manualHandoffs.findHandoffById(handoff.id);
    req(stillThereAfterCancellation.success()
            && stillThereAfterCancellation.value().has_value(),
        "A cancelled discard must not delete or rewrite the handoff");

    Run attemptRun;
    attemptRun.id = "attempt-run-active";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Running;
    attemptRun.createdAt = now;
    req(runs.insertRun(attemptRun).success(), "Could not seed the active attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:active";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the real active attempt record");

    const auto discardResult = manualHandoffService.discardHandoff(handoff.id);
    req(!discardResult.success()
            && discardResult.error()->code == "manual_handoff.discard_attempt_active",
        "Discard must refuse while a reverify attempt against this handoff is still "
        "Queued/Running");

    const auto stillThere = manualHandoffs.findHandoffById(handoff.id);
    req(stillThere.success() && stillThere.value().has_value(),
        "A refused discard must leave the handoff completely untouched");

    attemptRun.status = RunStatus::Cancelling;
    req(runs.updateRun(attemptRun).success(), "Could not move the active attempt to Cancelling");
    const auto cancellingDiscard = manualHandoffService.discardHandoff(handoff.id);
    req(!cancellingDiscard.success()
            && cancellingDiscard.error()->code == "manual_handoff.discard_attempt_active",
        "Discard must also refuse during the Cancelling window, while attempt cleanup is still "
        "in progress");
    req(manualHandoffs.findHandoffById(handoff.id).value().has_value(),
        "A Cancelling-attempt refusal must leave the handoff completely untouched");
}

// QML-P4R.3 package 3, crash window 1: insertAttempt() committed but the
// process died before insertRunWithStages() ever ran for it - a real
// attempt row with NO corresponding Run at all. reconcileInterruptedAttempts()
// must leave this row completely alone (nothing downstream could exist yet
// to repair) and must never let it block a fresh reservation for the same
// edited fingerprint.
void testReconcileLeavesRunlessAttemptHarmlessAndFingerprintUnblocked()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile runless attempt";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:runless";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "runless-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {{"chg-component", source.id, std::string(64, 'c'), "chg-component"}};
    handoff.allowedPaths = {{"qml/App.qml", "chg-component", "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    // Reserved via insertAttempt() alone - no matching Run row was ever
    // created for "attempt-run-orphan", simulating a crash between the two
    // calls importAndReverify() itself makes in that exact order.
    ManifestVerificationManualHandoffAttempt orphan;
    orphan.id = "manual-handoff-attempt:orphan";
    orphan.handoffId = handoff.id;
    orphan.verificationAnchorRunId = handoff.verificationAnchorRunId;
    orphan.attemptRunId = "attempt-run-orphan";
    orphan.attemptOrdinal = 1;
    orphan.editedSnapshotFingerprint = std::string(64, 'e');
    orphan.createdAt = now;
    req(manualHandoffs.insertAttempt(orphan).success(),
        "Could not seed the real Run-less attempt record");

    const auto reconciled = manualHandoffService.reconcileInterruptedAttempts();
    req(reconciled.success(), "Reconciliation must succeed even with a Run-less attempt present");
    req(std::find(reconciled.value().incompleteAttemptRunIds.begin(),
            reconciled.value().incompleteAttemptRunIds.end(), orphan.attemptRunId)
            == reconciled.value().incompleteAttemptRunIds.end()
        && std::find(reconciled.value().repairedFromLedgerRunIds.begin(),
            reconciled.value().repairedFromLedgerRunIds.end(), orphan.attemptRunId)
            == reconciled.value().repairedFromLedgerRunIds.end(),
        "A Run-less attempt has nothing to repair and must not appear in either result bucket");

    // The row itself is preserved exactly as immutable audit history.
    const auto stillThere = manualHandoffs.findAttemptByFingerprint(
        handoff.id, orphan.editedSnapshotFingerprint);
    req(stillThere.success() && stillThere.value().has_value()
            && stillThere.value()->id == orphan.id,
        "The Run-less attempt must remain as untouched audit history");
    const auto noRun = runs.findRun(orphan.attemptRunId);
    req(noRun.success() && !noRun.value().has_value(),
        "Reconciliation must never fabricate a Run for a reservation that never got one");

    // The same edited fingerprint must still be free to reserve under a
    // fresh ordinal - the orphan never blocks it, matching
    // findAttemptByFingerprint's own "no Run" branch that importAndReverify()
    // already relies on.
    ManifestVerificationManualHandoffAttempt retry;
    retry.id = "manual-handoff-attempt:retry";
    retry.handoffId = handoff.id;
    retry.verificationAnchorRunId = handoff.verificationAnchorRunId;
    retry.attemptRunId = "attempt-run-retry";
    retry.attemptOrdinal = 2;
    retry.previousAttemptId = orphan.id;
    retry.editedSnapshotFingerprint = orphan.editedSnapshotFingerprint;
    retry.createdAt = now;
    req(manualHandoffs.insertAttempt(retry).success(),
        "A fresh ordinal retry of the same edited fingerprint must never be blocked by a "
        "Run-less prior attempt");
}

// QML-P4R.3 package 3, crash windows 2/3/5: the attempt Run was already
// recovered generically as Interrupted (recoverInterruptedRuns() only ever
// touches the Run row itself, never stages/claims/successors), a real
// successor proposal was materialized mid-attempt, and no authoritative
// Work Ledger entry was ever written. reconcileInterruptedAttempts() must
// mark that successor missing-evidence, release its path claim, and bring
// the stage/Run to a terminal Failed status - the exact same three effects
// importAndReverify()'s own in-process ScopeExit produces for an in-process
// crash, now produced for one that outlived the process.
void testReconcileMarksIncompleteAttemptSuccessorMissingEvidenceAndReleasesClaim()
{
    TempDir workspace;
    TempDir scratch;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    RunPathClaimRegistry pathClaims;
    ChangeProposalService proposals(proposalRepository, *storeResult.value(), &pathClaims);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    TempDir manualHandoffRoot;
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile incomplete attempt";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:incomplete";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "incomplete-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {
        {componentProposal.value().changeSetId, source.id, std::string(64, 'c'),
            componentProposal.value().changeSetId}};
    handoff.allowedPaths = {{"qml/App.qml", componentProposal.value().changeSetId, "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    // The attempt Run, already generically recovered as Interrupted by
    // recoverInterruptedRuns() (which never touches stages) - the exact
    // state a real restart leaves behind.
    Run attemptRun;
    attemptRun.id = "attempt-run-incomplete";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.errorMessage = "Recovered after an interrupted application session";
    attemptRun.createdAt = now;
    attemptRun.finishedAt = now;
    RunStage stage;
    stage.id = attemptRun.id + ":manual-handoff-reverify";
    stage.runId = attemptRun.id;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.3;
    stage.startedAt = now;
    req(runs.insertRunWithStages(attemptRun, {stage}).success(),
        "Could not seed the real interrupted attempt Run and stage");

    // A real successor, materialized mid-attempt before the crash - this is
    // exactly what importAndReverify() itself does via
    // materializeRepairSuccessor(), including the real path claim it takes.
    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = attemptRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "manual-handoff";
    repairRequest.modelId = "manual-handoff";
    repairRequest.agentRole = "user";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Manual handoff edit",
        "changes": [
            {"change_id":"manual-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real mid-attempt successor proposal");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:incomplete";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the real incomplete attempt record");

    // No Work Ledger entry for attemptRun.id - the crash happened before
    // that authoritative write.

    const auto reconciled = manualHandoffService.reconcileInterruptedAttempts();
    req(reconciled.success(), "Reconciliation of an incomplete attempt must succeed");
    req(std::find(reconciled.value().incompleteAttemptRunIds.begin(),
            reconciled.value().incompleteAttemptRunIds.end(), attemptRun.id)
            != reconciled.value().incompleteAttemptRunIds.end(),
        "The incomplete attempt must be reported in the incomplete bucket");

    const auto markedSuccessor = proposals.findChangeSet(successor.value().changeSetId);
    req(markedSuccessor.success()
            && markedSuccessor.value().missingEvidenceCode.has_value()
            && *markedSuccessor.value().missingEvidenceCode == "manual_handoff.attempt_incomplete",
        "The mid-attempt successor must be marked missing-evidence, exactly like the in-process "
        "ScopeExit cleanup would have done");

    const auto stages = runs.listStages(attemptRun.id);
    req(stages.success() && stages.value().size() == 1
            && stages.value().front().status == StageStatus::Failed
            && stages.value().front().progress == 0.0,
        "The stage still open at crash time must be brought to a terminal Failed status");

    const auto repairedRun = runs.findRun(attemptRun.id);
    req(repairedRun.success() && repairedRun.value().has_value()
            && repairedRun.value()->status == RunStatus::Failed,
        "The attempt Run must end at a terminal Failed status, never left at the generic "
        "Interrupted recovery status forever");

    // The successor's own path claim, taken by materializeRepairSuccessor
    // above through the real RunPathClaimRegistry, must be released - a
    // DIFFERENT changeSetId can now claim the same path under the same Run
    // without conflict.
    const auto reclaim = pathClaims.tryClaim(
        attemptRun.id, "some-other-changeset", {{"qml/App.qml", "some-other-change"}});
    req(reclaim.claimed,
        "The successor's path claim must be released by reconciliation - a fresh claim on the "
        "same path under the same Run must not conflict");
}

// QML-P4R.3.4 hardening: an attempt that already carries a VerifiedSuccess
// typed result (from a genuinely completed, prior call) but has NO Work
// Ledger entry visible to this reconciliation pass is a durable-storage
// integrity conflict - the two authorities disagree. reconcileInterruptedAttempts()
// must fail closed, and - critically - must never have ALREADY marked the
// real successor missing-evidence or overwritten the Run/stage projection
// before discovering the conflict (the whole point of checking BEFORE any
// mutation, not merely reporting the conflict after the damage is done).
void testReconcileRefusesToTreatExistingVerifiedResultAsIncomplete()
{
    TempDir workspace;
    TempDir scratch;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    RunPathClaimRegistry pathClaims;
    ChangeProposalService proposals(proposalRepository, *storeResult.value(), &pathClaims);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    TempDir manualHandoffRoot;
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile verified-result conflict";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:verified-conflict";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "verified-conflict-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {
        {componentProposal.value().changeSetId, source.id, std::string(64, 'c'),
            componentProposal.value().changeSetId}};
    handoff.allowedPaths = {{"qml/App.qml", componentProposal.value().changeSetId, "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    Run attemptRun;
    attemptRun.id = "attempt-run-verified-conflict";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.errorMessage = "Recovered after an interrupted application session";
    attemptRun.createdAt = now;
    attemptRun.finishedAt = now;
    RunStage stage;
    stage.id = attemptRun.id + ":manual-handoff-reverify";
    stage.runId = attemptRun.id;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.3;
    stage.startedAt = now;
    req(runs.insertRunWithStages(attemptRun, {stage}).success(),
        "Could not seed the real interrupted attempt Run and stage");

    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = attemptRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "manual-handoff";
    repairRequest.modelId = "manual-handoff";
    repairRequest.agentRole = "user";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Manual handoff edit",
        "changes": [
            {"change_id":"manual-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real mid-attempt successor proposal");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:verified-conflict";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the real attempt record");

    // The integrity conflict itself: this attempt already carries a
    // VerifiedSuccess typed result (as if a prior call genuinely completed
    // it), yet NO Work Ledger entry exists for attemptRun.id - the two
    // authorities disagree.
    ManifestVerificationManualHandoffAttemptResult conflictingResult;
    conflictingResult.id = "manual-handoff-attempt-result:" + attempt.id;
    conflictingResult.attemptId = attempt.id;
    conflictingResult.attemptRunId = attemptRun.id;
    conflictingResult.kind = ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess;
    conflictingResult.recordedAt = now;
    req(manualHandoffs.insertAttemptResult(conflictingResult).success(),
        "Could not seed the conflicting pre-existing typed result");

    const auto reconciled = manualHandoffService.reconcileInterruptedAttempts();
    req(!reconciled.success(), "A VerifiedSuccess typed result with no ledger entry must fail "
        "closed, never be silently treated as Incomplete");
    req(reconciled.error()->code == "manual_handoff.attempt_result_conflict",
        "The refusal must be the specific conflict code");

    // Nothing destructive may have happened before the conflict was
    // detected: the real successor must remain untouched, and the Run/
    // stage must remain exactly as seeded, never repaired/failed.
    const auto successorAfter = proposals.findChangeSet(successor.value().changeSetId);
    req(successorAfter.success() && !successorAfter.value().missingEvidenceCode.has_value(),
        "The real successor must never be marked missing-evidence once a result conflict is "
        "detected - it might genuinely be verified evidence");
    const auto runAfter = runs.findRun(attemptRun.id);
    req(runAfter.success() && runAfter.value().has_value()
            && runAfter.value()->status == RunStatus::Interrupted,
        "The attempt Run must be left exactly as seeded - never repaired to Failed before the "
        "conflict was detected");
    const auto stagesAfter = runs.listStages(attemptRun.id);
    req(stagesAfter.success() && stagesAfter.value().size() == 1
            && stagesAfter.value().front().status == StageStatus::Running,
        "The stage must be left exactly as seeded too");
}

// QML-P4R.3.4 hardening: the same conflict-detection discipline applies to
// the ledger-PRESENT self-heal branch - an existing typed result that
// disagrees with what the ledger's own terminalStatus implies must be
// caught BEFORE the Run/stage projection is rewritten to match the ledger,
// never discovered only afterward.
void testReconcileRefusesLedgerResultKindConflictBeforeRepairingRun()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile ledger-result conflict";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:ledger-result-conflict";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "ledger-result-conflict-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {{"chg-component", source.id, std::string(64, 'c'), "chg-component"}};
    handoff.allowedPaths = {{"qml/App.qml", "chg-component", "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    Run attemptRun;
    attemptRun.id = "attempt-run-ledger-result-conflict";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.errorMessage = "Recovered after an interrupted application session";
    attemptRun.createdAt = now;
    attemptRun.finishedAt = now;
    RunStage stage;
    stage.id = attemptRun.id + ":manual-handoff-reverify";
    stage.runId = attemptRun.id;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.8;
    stage.startedAt = now;
    req(runs.insertRunWithStages(attemptRun, {stage}).success(),
        "Could not seed the real Run/stage");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:ledger-result-conflict";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(), "Could not seed the real attempt record");

    ManifestProposalSetVerification verification;
    verification.schemaVersion = "manifest-proposal-set-verification.v1";
    verification.workspaceIdentity = project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {"chg-component"};
    verification.orderedProposalContentHashes = {std::string(64, 'c')};
    verification.overlayFingerprint = std::string(64, 'f');
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRun.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = verification;
    entry.taskDispositions = {{"component", "succeeded", "chg-component", std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the authoritative Work Ledger entry");

    // The conflict: the ledger says Completed (=> VerifiedSuccess expected),
    // but a pre-existing typed result already claims Incomplete.
    ManifestVerificationManualHandoffAttemptResult conflictingResult;
    conflictingResult.id = "manual-handoff-attempt-result:" + attempt.id;
    conflictingResult.attemptId = attempt.id;
    conflictingResult.attemptRunId = attemptRun.id;
    conflictingResult.kind = ManifestVerificationManualHandoffAttemptResultKind::Incomplete;
    conflictingResult.recordedAt = now;
    req(manualHandoffs.insertAttemptResult(conflictingResult).success(),
        "Could not seed the conflicting pre-existing typed result");

    const auto reconciled = manualHandoffService.reconcileInterruptedAttempts();
    req(!reconciled.success(), "A ledger-implied kind that disagrees with an existing typed "
        "result must fail closed");
    req(reconciled.error()->code == "manual_handoff.attempt_result_conflict",
        "The refusal must be the specific conflict code");

    // The Run/stage projection must remain exactly as seeded - never
    // rewritten to match the ledger before the conflict was detected.
    const auto runAfter = runs.findRun(attemptRun.id);
    req(runAfter.success() && runAfter.value().has_value()
            && runAfter.value()->status == RunStatus::Interrupted,
        "The Run must not be repaired to Completed once a result conflict is detected");
    const auto stagesAfter = runs.listStages(attemptRun.id);
    req(stagesAfter.success() && stagesAfter.value().size() == 1
            && stagesAfter.value().front().status == StageStatus::Running,
        "The stage must not be repaired either");
}

// QML-P4R.3 hardening (P4R.3.3 review): markMissingEvidence()'s own result
// must never be silently discarded inside reconcileInterruptedAttempts() -
// a repository-level failure to annotate an orphan successor is a real
// recovery gap a caller (BackendRuntime, through its own recoveryError()
// channel) must be told about, never masked behind an overall "success".
void testReconcileSurfacesMarkMissingEvidenceFailureRatherThanSilentlyDiscardingIt()
{
    TempDir workspace;
    TempDir scratch;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    RunPathClaimRegistry pathClaims;
    ChangeProposalService proposals(proposalRepository, *storeResult.value(), &pathClaims);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    TempDir manualHandoffRoot;
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile surfaces mark failure";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:mark-failure";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "mark-failure-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {
        {componentProposal.value().changeSetId, source.id, std::string(64, 'c'),
            componentProposal.value().changeSetId}};
    handoff.allowedPaths = {{"qml/App.qml", componentProposal.value().changeSetId, "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    Run attemptRun;
    attemptRun.id = "attempt-run-mark-failure";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.errorMessage = "Recovered after an interrupted application session";
    attemptRun.createdAt = now;
    attemptRun.finishedAt = now;
    RunStage stage;
    stage.id = attemptRun.id + ":manual-handoff-reverify";
    stage.runId = attemptRun.id;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.3;
    stage.startedAt = now;
    req(runs.insertRunWithStages(attemptRun, {stage}).success(),
        "Could not seed the real interrupted attempt Run and stage");

    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = attemptRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "manual-handoff";
    repairRequest.modelId = "manual-handoff";
    repairRequest.agentRole = "user";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Manual handoff edit",
        "changes": [
            {"change_id":"manual-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real mid-attempt successor proposal");

    // Inject a repository-level failure for THIS exact successor's
    // markMissingEvidence call - simulates a real write failure (disk full,
    // lock contention) reconciliation must not paper over.
    proposalRepository.failMarkMissingEvidenceFor(successor.value().changeSetId);

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:mark-failure";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the real incomplete attempt record");

    const auto reconciled = manualHandoffService.reconcileInterruptedAttempts();
    req(reconciled.success(),
        "A markMissingEvidence failure must not fail the whole reconciliation Result - it is "
        "reported through the result's own dedicated field instead, exactly like the Work "
        "Ledger's own stillMissing pattern");
    req(reconciled.value().unmarkedMissingEvidenceProposalIds.size() == 1
            && reconciled.value().unmarkedMissingEvidenceProposalIds.front()
                == successor.value().changeSetId,
        "The failed-to-mark successor must be reported in its own dedicated field - never "
        "silently discarded behind an overall successful Result");

    // Everything else this branch is responsible for must still have
    // happened despite the one failed annotation - a single repository
    // write failure must not abort the rest of this attempt's own cleanup.
    proposalRepository.failMarkMissingEvidenceFor("chg-does-not-block-other-cleanup");
    const auto stages = runs.listStages(attemptRun.id);
    req(stages.success() && stages.value().size() == 1
            && stages.value().front().status == StageStatus::Failed,
        "The stage must still reach its terminal Failed status despite the mark failure");
    const auto repairedRun = runs.findRun(attemptRun.id);
    req(repairedRun.success() && repairedRun.value().has_value()
            && repairedRun.value()->status == RunStatus::Failed,
        "The Run must still reach its terminal Failed status despite the mark failure");
}

// QML-P4R.3 package 3, crash window 4: the authoritative Work Ledger entry
// already committed, but the process died before the Run/stage terminal
// projection caught up to it - exactly the gap importAndReverify()'s own
// lazy self-heal branch already repairs on a caller's next call.
// reconcileInterruptedAttempts() must apply the identical repair proactively
// at startup, and must NEVER touch any proposal - the ledger is already the
// one authoritative record of what became of them.
void testReconcileRepairsRunAndStageFromLedgerWithoutTouchingProposals()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile from ledger";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:ledger-lag";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "ledger-lag-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {{"chg-component", source.id, std::string(64, 'c'), "chg-component"}};
    handoff.allowedPaths = {{"qml/App.qml", "chg-component", "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    // A decoy proposal that a buggy ledger-repair path might wrongly touch -
    // proves this branch never reaches for it at all.
    const auto decoyProposal = proposalRepository.insertChangeSet(
        [&] {
            ProposedChangeSet cs;
            cs.changeSetId = "chg-decoy";
            cs.runId = "attempt-run-ledger-lag";
            cs.projectId = project.id;
            cs.state = ChangeSetState::ReadyForReview;
            cs.createdAt = now;
            return cs;
        }());
    req(decoyProposal.success(), "Could not seed the decoy proposal");

    Run attemptRun;
    attemptRun.id = "attempt-run-ledger-lag";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.errorMessage = "Recovered after an interrupted application session";
    attemptRun.createdAt = now;
    attemptRun.finishedAt = now;
    RunStage stage;
    stage.id = attemptRun.id + ":manual-handoff-reverify";
    stage.runId = attemptRun.id;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.8;
    stage.startedAt = now;
    req(runs.insertRunWithStages(attemptRun, {stage}).success(),
        "Could not seed the real Run/stage lagging behind its own committed ledger entry");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:ledger-lag";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the real attempt record whose Run/stage lags behind its own ledger");

    ManifestProposalSetVerification verification;
    verification.schemaVersion = "manifest-proposal-set-verification.v1";
    verification.workspaceIdentity = project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {"chg-component"};
    verification.orderedProposalContentHashes = {std::string(64, 'c')};
    verification.overlayFingerprint = std::string(64, 'f');
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRun.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = verification;
    entry.taskDispositions = {{"component", "succeeded", "chg-component", std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(),
        "Could not seed the already-committed, authoritative Work Ledger entry");

    const auto reconciled = manualHandoffService.reconcileInterruptedAttempts();
    req(reconciled.success(), "Reconciliation of a ledger-authoritative attempt must succeed");
    req(std::find(reconciled.value().repairedFromLedgerRunIds.begin(),
            reconciled.value().repairedFromLedgerRunIds.end(), attemptRun.id)
            != reconciled.value().repairedFromLedgerRunIds.end(),
        "An attempt with an authoritative ledger entry must be reported in the "
        "repaired-from-ledger bucket");

    const auto repairedRun = runs.findRun(attemptRun.id);
    req(repairedRun.success() && repairedRun.value().has_value()
            && repairedRun.value()->status == RunStatus::Completed
            && repairedRun.value()->progress == 1.0
            && !repairedRun.value()->currentStageId.has_value(),
        "The Run must be repaired to match its own authoritative ledger's terminal status");
    const auto stages = runs.listStages(attemptRun.id);
    req(stages.success() && stages.value().size() == 1
            && stages.value().front().status == StageStatus::Completed,
        "The stage must be repaired to match the ledger's terminal status too");

    const auto decoyAfter = proposals.findChangeSet("chg-decoy");
    req(decoyAfter.success() && decoyAfter.value().state == ChangeSetState::ReadyForReview
            && !decoyAfter.value().missingEvidenceCode.has_value(),
        "The ledger-repair branch must never touch any proposal - the ledger is already the "
        "authoritative record of what became of them");
}

// QML-P4R.3 package 3, crash window 6: a repeated startup reconciliation
// pass must be fully idempotent - no duplicate mutation, no regression of
// an already-terminal Run/stage, no re-thrown error, and a stable result
// shape, exercised against the same incomplete-attempt scenario the
// dedicated test above proves in detail.
void testReconcileInterruptedAttemptsIsIdempotentOnRepeatedCall()
{
    TempDir workspace;
    TempDir scratch;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    RunPathClaimRegistry pathClaims;
    ChangeProposalService proposals(proposalRepository, *storeResult.value(), &pathClaims);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    TempDir manualHandoffRoot;
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Reconcile idempotent repeat";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:repeat";
    handoff.originManifestRunId = source.id;
    handoff.verificationAnchorRunId = source.id;
    handoff.sourceProjectId = project.id;
    handoff.storageKey = "repeat-key";
    handoff.workspaceIdentity = project.id;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {
        {componentProposal.value().changeSetId, source.id, std::string(64, 'c'),
            componentProposal.value().changeSetId}};
    handoff.allowedPaths = {{"qml/App.qml", componentProposal.value().changeSetId, "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    emptySnapshot.fingerprint = std::string(64, 'd');
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = now;
    req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed the real handoff row");

    Run attemptRun;
    attemptRun.id = "attempt-run-repeat";
    attemptRun.projectId = project.id;
    attemptRun.title = "Manual handoff reverify";
    attemptRun.task = "Manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.status = RunStatus::Interrupted;
    attemptRun.errorMessage = "Recovered after an interrupted application session";
    attemptRun.createdAt = now;
    attemptRun.finishedAt = now;
    RunStage stage;
    stage.id = attemptRun.id + ":manual-handoff-reverify";
    stage.runId = attemptRun.id;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.3;
    stage.startedAt = now;
    req(runs.insertRunWithStages(attemptRun, {stage}).success(),
        "Could not seed the real interrupted attempt Run and stage");

    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = attemptRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "manual-handoff";
    repairRequest.modelId = "manual-handoff";
    repairRequest.agentRole = "user";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Manual handoff edit",
        "changes": [
            {"change_id":"manual-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real mid-attempt successor proposal");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:repeat";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = std::string(64, 'e');
    attempt.createdAt = now;
    req(manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the real incomplete attempt record");

    const auto first = manualHandoffService.reconcileInterruptedAttempts();
    req(first.success() && first.value().incompleteAttemptRunIds.size() == 1
            && first.value().incompleteAttemptRunIds.front() == attemptRun.id,
        "The first reconciliation pass must find and repair exactly the one incomplete attempt");

    const auto runAfterFirst = runs.findRun(attemptRun.id);
    req(runAfterFirst.success() && runAfterFirst.value().has_value(), "Run must still exist");
    const auto stagesAfterFirst = runs.listStages(attemptRun.id);
    req(stagesAfterFirst.success() && stagesAfterFirst.value().size() == 1, "Stage must still exist");

    const auto second = manualHandoffService.reconcileInterruptedAttempts();
    req(second.success(), "A repeated reconciliation pass must never fail");
    req(second.value().incompleteAttemptRunIds.size() == 1
            && second.value().incompleteAttemptRunIds.front() == attemptRun.id,
        "A repeated pass must report the exact same stable bucket contents, never duplicate or "
        "drop the same attempt");

    const auto runAfterSecond = runs.findRun(attemptRun.id);
    req(runAfterSecond.success() && runAfterSecond.value().has_value()
            && runAfterSecond.value()->status == runAfterFirst.value()->status
            && runAfterSecond.value()->finishedAt == runAfterFirst.value()->finishedAt,
        "A repeated pass must never regress or re-timestamp an already-terminal Run");
    const auto stagesAfterSecond = runs.listStages(attemptRun.id);
    req(stagesAfterSecond.success() && stagesAfterSecond.value().size() == 1
            && stagesAfterSecond.value().front().status == stagesAfterFirst.value().front().status
            && stagesAfterSecond.value().front().finishedAt
                == stagesAfterFirst.value().front().finishedAt,
        "A repeated pass must never regress or re-timestamp an already-terminal stage");

    const auto successorAfter = proposals.findChangeSet(successor.value().changeSetId);
    req(successorAfter.success()
            && successorAfter.value().missingEvidenceCode.has_value()
            && *successorAfter.value().missingEvidenceCode == "manual_handoff.attempt_incomplete",
        "The successor must remain marked missing-evidence after the repeated pass");
}

// QML-P4R.3.5: a minimal, otherwise-eligible handoff for retention tests -
// no attempts, so isHandoffRetentionProtectedLocked's own active/
// Apply-authority checks both trivially pass (nothing to protect it) and
// only its createdAt decides age-eligibility.
ManifestVerificationManualHandoff makeMinimalRetentionHandoff(
    const RunId& anchorRunId, const ProjectId& projectId, const std::string& storageKey,
    const ChangeSetId& proposalId, std::chrono::system_clock::time_point createdAt)
{
    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:" + anchorRunId;
    handoff.originManifestRunId = anchorRunId;
    handoff.verificationAnchorRunId = anchorRunId;
    handoff.sourceProjectId = projectId;
    handoff.storageKey = storageKey;
    handoff.workspaceIdentity = projectId;
    handoff.baselineFingerprint = std::string(64, 'a');
    handoff.sourceOverlayFingerprint = std::string(64, 'b');
    handoff.frozenMembers = {{proposalId, anchorRunId, std::string(64, 'c'), proposalId}};
    handoff.allowedPaths = {{"qml/App.qml", proposalId, "component"}};
    WorkspaceBaselineSnapshot emptySnapshot;
    // Genuinely self-consistent (files empty -> fingerprint is sha256 of
    // empty material), not an arbitrary placeholder -
    // WorkspaceBaselineProvenance::parseJson recomputes and rejects any
    // mismatch on every SQLite read (see computeBaselineFingerprint), even
    // though the InMemory repository's own direct-struct storage never
    // exercises that check.
    emptySnapshot.fingerprint = sha256Hex("");
    handoff.originalSnapshot = emptySnapshot;
    handoff.createdAt = createdAt;
    return handoff;
}

// QML-P4R.3.5: the oldest eligible handoff, once capacity is exceeded, is
// evicted via the exact same discardHandoff() mechanics as an explicit
// discard - including releasing its frozen members' GLOBAL
// UNIQUE(proposal_id) claim, so a different anchor could later refreeze
// the same proposal.
void testEnforceRetentionEvictsOldestEligibleAndReleasesProposalClaim()
{
    outcomebrief_hooks::RecordingTrigger outcomeBriefTrigger;   // PC-4C: records the nudges to the Outcome Brief
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);
    manualHandoffService.setOutcomeBriefTrigger(&outcomeBriefTrigger);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Retention eviction";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    // One more than capacity (10) - all old enough, none active/verified -
    // so exactly one eviction is needed to get back under capacity.
    std::string oldestHandoffId;
    std::string oldestProposalId;
    for (int i = 0; i < 11; ++i) {
        Run source;
        source.id = "source-run-" + std::to_string(i);
        source.projectId = project.id;
        source.title = "Source";
        source.task = "Repair";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = now;
        req(runs.insertRun(source).success(), "Could not seed source Run");

        const std::string proposalId = "chg-retention-" + std::to_string(i);
        // Oldest first (index 0), each subsequent handoff one hour newer -
        // all still well past the 24h minimum retention age.
        const auto createdAt = now - std::chrono::hours(48 + (10 - i));
        auto handoff = makeMinimalRetentionHandoff(
            source.id, project.id, "storage-" + std::to_string(i), proposalId, createdAt);
        req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed handoff");
        if (i == 0) {
            oldestHandoffId = handoff.id;
            oldestProposalId = proposalId;
        }
    }

    const auto before = manualHandoffs.listAllHandoffs();
    req(before.success() && before.value().size() == 11, "Fixture must start with 11 handoffs");

    const auto retained = manualHandoffService.enforceRetention();
    req(retained.success(), "enforceRetention() must succeed against an all-eligible pool");
    req(retained.value().evictedHandoffIds.size() == 1
            && retained.value().evictedHandoffIds.front() == oldestHandoffId,
        "Exactly the single oldest handoff must be evicted to get back under capacity");
    // PC-4C: the anchor of the evicted handoff is not "waiting for the manual resolution" any more, so ITS brief is nudged - and only
    // its: the ten handoffs that stay are untouched.
    req(outcomeBriefTrigger.runs() == std::vector<std::string> {"source-run-0"},
        "enforceRetention must nudge the Outcome Brief of exactly the anchor of the evicted handoff");

    const auto after = manualHandoffs.listAllHandoffs();
    req(after.success() && after.value().size() == 10, "Exactly one handoff must have been evicted");
    req(!manualHandoffs.findHandoffById(oldestHandoffId).value().has_value(),
        "The evicted handoff's own row must be gone");
    req(!manualHandoffs.findHandoffByMemberProposalId(oldestProposalId).value().has_value(),
        "The evicted handoff's frozen member proposal-id claim must be released");

    // An explicit discard nudges its own anchor, once - and leaves nothing behind for the NEXT command to nudge again.
    req(manualHandoffService.discardHandoff("manual-handoff:source-run-3").success(), "an explicit discard must succeed");
    req(outcomeBriefTrigger.runs() == (std::vector<std::string> {"source-run-0", "source-run-3"}), "the discard nudges the anchor it removed");
    req(manualHandoffService.enforceRetention().success(), "a second enforceRetention() has nothing to evict");
    req(outcomeBriefTrigger.runs() == (std::vector<std::string> {"source-run-0", "source-run-3"}),
        "...and does not nudge the anchor of the earlier discard again");
}

// QML-P4R.3.5: an active attempt, an attempt already carrying Apply-
// authority (VerifiedSuccess), and a too-recently-created handoff must
// never be auto-evicted - even when each is chronologically OLDER than
// every genuinely eligible handoff retention actually does evict.
void testEnforceRetentionProtectsActiveSuccessfulAndFreshHandoffs()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Retention protection";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    auto seedSourceRun = [&](const std::string& id) {
        Run source;
        source.id = id;
        source.projectId = project.id;
        source.title = "Source";
        source.task = "Repair";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = now;
        req(runs.insertRun(source).success(), ("Could not seed source Run " + id).c_str());
    };

    // The oldest of all: an active attempt in progress.
    seedSourceRun("source-active");
    auto activeHandoff = makeMinimalRetentionHandoff(
        "source-active", project.id, "storage-active", "chg-active",
        now - std::chrono::hours(200));
    req(manualHandoffs.insertHandoff(activeHandoff).success(), "Could not seed active handoff");
    Run activeAttemptRun;
    activeAttemptRun.id = "attempt-active";
    activeAttemptRun.projectId = project.id;
    activeAttemptRun.title = "Manual handoff reverify";
    activeAttemptRun.task = "Manual handoff reverify";
    activeAttemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    activeAttemptRun.parentRunId = activeHandoff.verificationAnchorRunId;
    // Cancelling remains active: the attempt may still be unwinding and
    // releasing claims. This specifically covers the status omitted by the
    // original package-5 protection check.
    activeAttemptRun.status = RunStatus::Cancelling;
    activeAttemptRun.createdAt = now;
    req(runs.insertRun(activeAttemptRun).success(), "Could not seed active attempt Run");
    ManifestVerificationManualHandoffAttempt activeAttempt;
    activeAttempt.id = "manual-handoff-attempt:active";
    activeAttempt.handoffId = activeHandoff.id;
    activeAttempt.verificationAnchorRunId = activeHandoff.verificationAnchorRunId;
    activeAttempt.attemptRunId = activeAttemptRun.id;
    activeAttempt.attemptOrdinal = 1;
    activeAttempt.editedSnapshotFingerprint = std::string(64, 'e');
    activeAttempt.createdAt = now;
    req(manualHandoffs.insertAttempt(activeAttempt).success(), "Could not seed active attempt");

    // Second-oldest: an attempt already carrying Apply-authority.
    seedSourceRun("source-verified");
    auto verifiedHandoff = makeMinimalRetentionHandoff(
        "source-verified", project.id, "storage-verified", "chg-verified",
        now - std::chrono::hours(199));
    req(manualHandoffs.insertHandoff(verifiedHandoff).success(), "Could not seed verified handoff");
    Run verifiedAttemptRun;
    verifiedAttemptRun.id = "attempt-verified";
    verifiedAttemptRun.projectId = project.id;
    verifiedAttemptRun.title = "Manual handoff reverify";
    verifiedAttemptRun.task = "Manual handoff reverify";
    verifiedAttemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    verifiedAttemptRun.parentRunId = verifiedHandoff.verificationAnchorRunId;
    verifiedAttemptRun.status = RunStatus::Completed;
    verifiedAttemptRun.createdAt = now;
    req(runs.insertRun(verifiedAttemptRun).success(), "Could not seed verified attempt Run");
    ManifestVerificationManualHandoffAttempt verifiedAttempt;
    verifiedAttempt.id = "manual-handoff-attempt:verified";
    verifiedAttempt.handoffId = verifiedHandoff.id;
    verifiedAttempt.verificationAnchorRunId = verifiedHandoff.verificationAnchorRunId;
    verifiedAttempt.attemptRunId = verifiedAttemptRun.id;
    verifiedAttempt.attemptOrdinal = 1;
    verifiedAttempt.editedSnapshotFingerprint = std::string(64, 'f');
    verifiedAttempt.createdAt = now;
    req(manualHandoffs.insertAttempt(verifiedAttempt).success(), "Could not seed verified attempt");
    // Work Ledger, not the typed-result projection, is terminal authority.
    // Deliberately omit the VerifiedSuccess row to reproduce the crash window
    // after the successful ledger commit and before that projection commits.
    ManifestWorkLedgerEntry verifiedLedger;
    verifiedLedger.runId = verifiedAttemptRun.id;
    verifiedLedger.projectId = project.id;
    verifiedLedger.terminalStatus = RunStatus::Completed;
    verifiedLedger.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    verifiedLedger.createdAt = now;
    req(ledger.insertEntry(verifiedLedger).success(),
        "Could not seed authoritative successful attempt ledger");

    // Third-oldest by wall clock but too RECENT to be eligible - created
    // "now", well inside the 24h minimum age.
    seedSourceRun("source-fresh");
    auto freshHandoff = makeMinimalRetentionHandoff(
        "source-fresh", project.id, "storage-fresh", "chg-fresh", now);
    req(manualHandoffs.insertHandoff(freshHandoff).success(), "Could not seed fresh handoff");

    // Ten genuinely eligible handoffs, all older than the minimum age but
    // all younger than the three protected ones above.
    std::vector<std::string> eligibleIds;
    for (int i = 0; i < 10; ++i) {
        seedSourceRun("source-eligible-" + std::to_string(i));
        auto handoff = makeMinimalRetentionHandoff(
            "source-eligible-" + std::to_string(i), project.id,
            "storage-eligible-" + std::to_string(i), "chg-eligible-" + std::to_string(i),
            now - std::chrono::hours(100 - i));
        req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed eligible handoff");
        eligibleIds.push_back(handoff.id);
    }

    const auto before = manualHandoffs.listAllHandoffs();
    req(before.success() && before.value().size() == 13, "Fixture must start with 13 handoffs");

    const auto retained = manualHandoffService.enforceRetention();
    req(retained.success(), "enforceRetention() must succeed with a mix of protected/eligible");
    req(retained.value().evictedHandoffIds.size() == 3,
        "Exactly three of the ten eligible handoffs must be evicted to reach capacity 10");
    for (const auto& evictedId : retained.value().evictedHandoffIds) {
        req(evictedId != activeHandoff.id && evictedId != verifiedHandoff.id
                && evictedId != freshHandoff.id,
            "Retention must never evict a protected handoff even when it is chronologically "
            "older than every evicted one");
    }

    req(manualHandoffs.findHandoffById(activeHandoff.id).value().has_value(),
        "The active-attempt handoff must survive");
    req(manualHandoffs.findHandoffById(verifiedHandoff.id).value().has_value(),
        "The Apply-authority handoff must survive");
    req(manualHandoffs.findHandoffById(freshHandoff.id).value().has_value(),
        "The too-fresh handoff must survive");

    const auto after = manualHandoffs.listAllHandoffs();
    req(after.success() && after.value().size() == 10, "Retention must land exactly at capacity");
}

// QML-P4R.3.5: a repeated enforceRetention() call once already at/under
// capacity with nothing left to sweep must be a true no-op - no further
// eviction, no error, a stable empty result.
void testEnforceRetentionRepeatedCallIsIdempotent()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Retention idempotency";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    for (int i = 0; i < 11; ++i) {
        Run source;
        source.id = "source-run-" + std::to_string(i);
        source.projectId = project.id;
        source.title = "Source";
        source.task = "Repair";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = now;
        req(runs.insertRun(source).success(), "Could not seed source Run");
        auto handoff = makeMinimalRetentionHandoff(
            source.id, project.id, "storage-" + std::to_string(i),
            "chg-retention-idem-" + std::to_string(i), now - std::chrono::hours(48 + (10 - i)));
        req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed handoff");
    }

    const auto first = manualHandoffService.enforceRetention();
    req(first.success() && first.value().evictedHandoffIds.size() == 1,
        "The first call must evict exactly one handoff to reach capacity");
    const auto afterFirst = manualHandoffs.listAllHandoffs();
    req(afterFirst.success() && afterFirst.value().size() == 10, "Must be exactly at capacity");

    for (int i = 0; i < 3; ++i) {
        const auto repeated = manualHandoffService.enforceRetention();
        req(repeated.success(), "A repeated call at capacity must still succeed");
        req(repeated.value().evictedHandoffIds.empty(),
            "A repeated call already at capacity must evict nothing further");
        req(repeated.value().sweptOrphanDirectoryNames.empty(),
            "A repeated call with nothing orphaned must sweep nothing");
        const auto stillAt = manualHandoffs.listAllHandoffs();
        req(stillAt.success() && stillAt.value().size() == 10,
            "The handoff count must remain stable across repeated calls");
    }
}

// QML-P4R.3.5 hardening: when the retained count is already above capacity
// but too few rows are eligible to make room for one more, materialize() must
// preflight the complete eviction batch and fail without a hidden partial
// eviction. Evicting exactly one and inserting immediately would leave the
// store above its declared bound.
void testMaterializeFailsClosedWhenRetentionCapacityExhaustedByProtectedHandoffs()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Retention capacity exhausted";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    // Fill capacity (10) with handoffs that are all too FRESH to ever be
    // evicted - protected purely by age, the simplest protection to seed.
    for (int i = 0; i < 10; ++i) {
        Run source;
        source.id = "source-protected-" + std::to_string(i);
        source.projectId = project.id;
        source.title = "Source";
        source.task = "Repair";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = now;
        req(runs.insertRun(source).success(), "Could not seed protected source Run");
        auto handoff = makeMinimalRetentionHandoff(
            source.id, project.id, "storage-protected-" + std::to_string(i),
            "chg-protected-" + std::to_string(i), now);
        req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed protected handoff");
    }

    // Two additional old, eligible rows make the starting count 12. A new
    // insertion would require THREE evictions (12 -> 9, then insert -> 10),
    // but only these two are eligible. The refusal must therefore delete
    // neither of them.
    std::vector<std::string> insufficientEligibleIds;
    for (int i = 0; i < 2; ++i) {
        Run oldSource;
        oldSource.id = "source-eligible-insufficient-" + std::to_string(i);
        oldSource.projectId = project.id;
        oldSource.title = "Source";
        oldSource.task = "Repair";
        oldSource.workflowId = kExecutionManifestWorkflowId;
        oldSource.status = RunStatus::Failed;
        oldSource.createdAt = now;
        req(runs.insertRun(oldSource).success(), "Could not seed eligible source Run");
        auto handoff = makeMinimalRetentionHandoff(
            oldSource.id, project.id, "storage-eligible-insufficient-" + std::to_string(i),
            "chg-eligible-insufficient-" + std::to_string(i), now - std::chrono::hours(72 + i));
        req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed eligible handoff");
        insufficientEligibleIds.push_back(handoff.id);
    }

    // A genuinely new, otherwise fully eligible source Run/handoff.
    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto blocked = manualHandoffService.materialize(source.id);
    req(!blocked.success(), "materialize() must fail closed when too few retained handoffs are "
        "eligible to make room without exceeding capacity");
    req(blocked.error()->code == "manual_handoff.retention_capacity_exhausted",
        "The refusal must be the specific capacity-exhausted code");

    const auto afterBlocked = manualHandoffs.listAllHandoffs();
    req(afterBlocked.success() && afterBlocked.value().size() == 12,
        "A refused capacity preflight must never partially evict rows or insert a new row");
    for (const auto& eligibleId : insufficientEligibleIds) {
        req(manualHandoffs.findHandoffById(eligibleId).value().has_value(),
            "A mutation-free capacity refusal must retain every insufficient eligible candidate");
    }
    req(!manualHandoffs.findHandoffByAnchorRun(source.id).value().has_value(),
        "No new handoff row must have been created for the refused anchor");
}

// QML-P4R.3.5: the SQLite counterpart of the eviction test above, proving
// the real ON DELETE CASCADE chain - handoff -> attempt -> attempt-result
// (schema v56's own table) - actually fires, not merely the InMemory
// fake's own explicit mirroring of it.
void testSqliteEnforceRetentionCascadeIncludesAttemptResultsTable()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    const fs::path dbPath = workspace.path / "state" / "orchestrator.sqlite3";
    fs::create_directories(dbPath.parent_path());
    const auto opened = SqliteDatabase::open(dbPath);
    req(opened.success(), "Could not open a real SQLite database");
    const auto database = opened.value();

    SqliteProjectRepository projects(database);
    SqliteRunRepository runs(database);
    SqliteExecutionManifestRepository manifests(database);
    SqliteManifestWorkLedgerRepository ledger(database);
    SqliteManifestVerificationManualHandoffRepository manualHandoffs(database);
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Sqlite retention cascade";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run targetSource;
    targetSource.id = "source-target";
    targetSource.projectId = project.id;
    targetSource.title = "Source";
    targetSource.task = "Repair";
    targetSource.workflowId = kExecutionManifestWorkflowId;
    targetSource.status = RunStatus::Failed;
    targetSource.createdAt = now;
    req(runs.insertRun(targetSource).success(), "Could not seed target source Run");
    auto targetHandoff = makeMinimalRetentionHandoff(
        targetSource.id, project.id, "storage-target", "chg-target",
        now - std::chrono::hours(200));
    req(manualHandoffs.insertHandoff(targetHandoff).success(), "Could not seed target handoff");

    Run targetAttemptRun = targetSource;
    targetAttemptRun.id = "attempt-target";
    targetAttemptRun.parentRunId = targetHandoff.verificationAnchorRunId;
    targetAttemptRun.workflowId = kManifestVerificationManualHandoffWorkflowId;
    targetAttemptRun.status = RunStatus::Failed;
    req(runs.insertRun(targetAttemptRun).success(), "Could not seed target attempt Run");
    ManifestVerificationManualHandoffAttempt targetAttempt;
    targetAttempt.id = "manual-handoff-attempt:target";
    targetAttempt.handoffId = targetHandoff.id;
    targetAttempt.verificationAnchorRunId = targetHandoff.verificationAnchorRunId;
    targetAttempt.attemptRunId = targetAttemptRun.id;
    targetAttempt.attemptOrdinal = 1;
    targetAttempt.editedSnapshotFingerprint = std::string(64, 'e');
    targetAttempt.createdAt = now;
    req(manualHandoffs.insertAttempt(targetAttempt).success(), "Could not seed target attempt");
    ManifestVerificationManualHandoffAttemptResult targetResult;
    targetResult.id = "manual-handoff-attempt-result:target";
    targetResult.attemptId = targetAttempt.id;
    targetResult.attemptRunId = targetAttemptRun.id;
    targetResult.kind = ManifestVerificationManualHandoffAttemptResultKind::Incomplete;
    targetResult.recordedAt = now;
    req(manualHandoffs.insertAttemptResult(targetResult).success(),
        "Could not seed target typed result");

    for (int i = 0; i < 10; ++i) {
        Run filler;
        filler.id = "source-filler-" + std::to_string(i);
        filler.projectId = project.id;
        filler.title = "Source";
        filler.task = "Repair";
        filler.workflowId = kExecutionManifestWorkflowId;
        filler.status = RunStatus::Failed;
        filler.createdAt = now;
        req(runs.insertRun(filler).success(), ("Could not seed filler source Run " + filler.id).c_str());
        auto handoff = makeMinimalRetentionHandoff(
            filler.id, project.id, "storage-filler-" + std::to_string(i),
            "chg-filler-" + std::to_string(i), now - std::chrono::hours(100 - i));
        req(manualHandoffs.insertHandoff(handoff).success(), "Could not seed filler handoff");
    }

    const auto retained = manualHandoffService.enforceRetention();
    req(retained.success(), "enforceRetention() must succeed against a real SQLite database");
    req(retained.value().evictedHandoffIds.size() == 1
            && retained.value().evictedHandoffIds.front() == targetHandoff.id,
        "The oldest (target) handoff must be the one evicted");

    sqlite3* verifyHandle = nullptr;
    const std::u8string dbPathUtf8 = dbPath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(dbPathUtf8.data()), dbPathUtf8.size());
    req(sqlite3_open_v2(utf8Path.c_str(), &verifyHandle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open a raw verification connection");
    sqlite3_stmt* countStatement = nullptr;
    req(sqlite3_prepare_v2(verifyHandle,
        "SELECT "
        "(SELECT COUNT(*) FROM manifest_verification_manual_handoffs WHERE id=?1),"
        "(SELECT COUNT(*) FROM manifest_verification_manual_handoff_attempts WHERE id=?2),"
        "(SELECT COUNT(*) FROM manifest_verification_manual_handoff_attempt_results WHERE id=?3);",
        -1, &countStatement, nullptr) == SQLITE_OK, "Could not prepare the verification query");
    sqlite3_bind_text(countStatement, 1, targetHandoff.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(countStatement, 2, targetAttempt.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(countStatement, 3, targetResult.id.c_str(), -1, SQLITE_TRANSIENT);
    req(sqlite3_step(countStatement) == SQLITE_ROW, "Could not read the verification query");
    req(sqlite3_column_int(countStatement, 0) == 0, "The evicted handoff row must be gone");
    req(sqlite3_column_int(countStatement, 1) == 0,
        "The evicted attempt row must be gone via cascade");
    req(sqlite3_column_int(countStatement, 2) == 0,
        "The evicted attempt-result row (schema v56) must be gone too via cascade");
    sqlite3_finalize(countStatement);
    sqlite3_close_v2(verifyHandle);
}

// QML-P4R.3.5: a candidate directory left behind by a past discard whose
// filesystem removal step failed or never ran (a crash between the DB
// commit and that best-effort step) must be safely swept on a later
// enforceRetention() pass - exactly the "after restart" scenario startup
// calls this for.
void testSqliteEnforceRetentionSweepsOrphanDirectoryAfterRestart()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    const fs::path dbPath = workspace.path / "state" / "orchestrator.sqlite3";
    fs::create_directories(dbPath.parent_path());
    const auto opened = SqliteDatabase::open(dbPath);
    req(opened.success(), "Could not open a real SQLite database");
    const auto database = opened.value();

    SqliteProjectRepository projects(database);
    SqliteRunRepository runs(database);
    SqliteExecutionManifestRepository manifests(database);
    SqliteManifestWorkLedgerRepository ledger(database);
    SqliteManifestVerificationManualHandoffRepository manualHandoffs(database);
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto orphanPath = manualHandoffRoot.path / "orphan-storage-key";
    std::error_code createError;
    fs::create_directories(orphanPath, createError);
    req(!createError, "Could not create the orphan directory fixture");
    {
        std::ofstream canary(orphanPath / "leftover.txt", std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(canary), "Could not write the orphan canary file");
        canary << "orphaned content";
    }
    req(fs::exists(orphanPath), "Fixture setup: the orphan directory must exist before the sweep");

    const auto retained = manualHandoffService.enforceRetention();
    req(retained.success(), "enforceRetention() must succeed with an orphan directory present");
    req(retained.value().sweptOrphanDirectoryNames.size() == 1
            && retained.value().sweptOrphanDirectoryNames.front() == "orphan-storage-key",
        "The orphan directory must be reported as swept");
    req(!fs::exists(orphanPath), "The orphan directory must actually be removed from disk");
}

// QML-P4R.3.5: a symlink/reparse-point entry directly under
// manualHandoffRoot_ - however it got there - must never be followed into
// or removed by the orphan sweep, even though its name matches no live
// handoff's storageKey. Skips (rather than false-fails) if this test
// environment cannot create a real symlink (e.g. Windows without
// Developer Mode/admin), matching this codebase's own established
// precedent (WorkspaceBaselineProvenanceTests.cpp).
void testSqliteEnforceRetentionNeverFollowsOrRemovesSymlinkEscape()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir outside;
    const fs::path dbPath = workspace.path / "state" / "orchestrator.sqlite3";
    fs::create_directories(dbPath.parent_path());
    const auto opened = SqliteDatabase::open(dbPath);
    req(opened.success(), "Could not open a real SQLite database");
    const auto database = opened.value();

    SqliteProjectRepository projects(database);
    SqliteRunRepository runs(database);
    SqliteExecutionManifestRepository manifests(database);
    SqliteManifestWorkLedgerRepository ledger(database);
    SqliteManifestVerificationManualHandoffRepository manualHandoffs(database);
    InMemoryManifestVerificationContinuationRepository continuations;
    FakeChangeProposalRepository proposalRepository;
    UnusedProposalStore proposalStore;
    ChangeProposalService proposals(proposalRepository, proposalStore);
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    UnusedVerificationRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    {
        std::ofstream canary(outside.path / "canary.txt", std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(canary), "Could not write the outside canary file");
        canary << "must never be touched";
    }

    const auto linkPath = manualHandoffRoot.path / "escape-link";
    std::error_code linkError;
    fs::create_directory_symlink(outside.path, linkPath, linkError);
    if (linkError) {
        return;
    }

    const auto retained = manualHandoffService.enforceRetention();
    req(retained.success(), "enforceRetention() must succeed with a symlinked entry present");
    req(retained.value().sweptOrphanDirectoryNames.empty(),
        "A symlink/reparse-point entry must never be reported as swept");
    std::error_code statusError;
    req(fs::symlink_status(linkPath, statusError).type() == fs::file_type::symlink,
        "The symlink entry itself must remain completely untouched - never removed");
    req(fs::exists(outside.path / "canary.txt"),
        "The symlink's real target must never be followed into or modified");

    // A normal-looking direct orphan that contains a nested reparse point is
    // equally unsafe for recursive deletion. Leave the whole tree untouched.
    const auto nestedOrphan = manualHandoffRoot.path / "nested-reparse-orphan";
    fs::create_directories(nestedOrphan);
    const auto nestedLink = nestedOrphan / "escape-link";
    std::error_code nestedLinkError;
    fs::create_directory_symlink(outside.path, nestedLink, nestedLinkError);
    if (!nestedLinkError) {
        const auto nestedRetained = manualHandoffService.enforceRetention();
        req(nestedRetained.success(),
            "Retention must safely tolerate an orphan containing a nested reparse point");
        req(fs::exists(nestedOrphan) && fs::exists(nestedLink),
            "An orphan tree containing a nested reparse point must remain untouched");
        req(fs::exists(outside.path / "canary.txt"),
            "A nested reparse target must never be followed into or modified");
    }

    // The durable root itself may be replaced by a symlink/junction between
    // processes. Refuse the sweep rather than enumerating and deleting direct
    // children of the outside target.
    const auto outsideOrphan = outside.path / "must-remain";
    fs::create_directories(outsideOrphan);
    ManifestVerificationManualHandoffService reparseRootService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, linkPath, scratch.path);
    const auto reparseRootRetention = reparseRootService.enforceRetention();
    req(!reparseRootRetention.success()
            && reparseRootRetention.error()->code == "manual_handoff.retention_sweep_failed",
        "A reparse-point durable root must fail closed before orphan enumeration");
    req(fs::exists(outsideOrphan),
        "A reparse-point durable root must never delete a child of its outside target");
}

// QML-P4R.6: a brand-new path appearing in the candidate directory - never
// part of the frozen originalSnapshot - must fail closed, distinct from
// testImportAndReverifyRejectsAnEditOutsideTheAllowedPath (which edits an
// EXISTING frozen path outside the allowlist).
void testImportAndReverifyRejectsANewPathNotInFrozenSet()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Import and reverify new path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    // A genuinely new file - never part of the frozen originalSnapshot -
    // appears in the candidate directory.
    {
        std::ofstream stray(materialized.value().candidateRoot / "qml" / "Stray.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(stray), "Could not create the new candidate file");
        stray << "Item {}";
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.new_path_not_allowed",
        "A brand-new path in the candidate directory must fail closed with new_path_not_allowed, "
        "never silently accepted as an edit");

    const auto attempts = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attempts.success() && attempts.value().empty(),
        "A rejected import must leave no durable attempt behind");
}

// QML-P4R.6: deleting a frozen candidate path must fail closed - deletion is
// not a supported manual-handoff edit at all, regardless of whether the
// deleted path was itself inside the allowlist.
void testImportAndReverifyRejectsADeletedFrozenPath()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Import and reverify deleted path";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::error_code removeError;
        const bool removed = fs::remove(
            materialized.value().candidateRoot / "qml" / "CMakeLists.txt", removeError);
        req(removed && !removeError, "Could not delete the frozen candidate file");
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.path_deleted_not_allowed",
        "Deleting a frozen candidate path must fail closed with path_deleted_not_allowed, never "
        "silently treated as an edit");
    req(imported.error()->message.find("qml/CMakeLists.txt") != std::string::npos,
        "The deleted-path rejection must name the safe candidate-relative path so the user can "
        "restore the exact file without guessing");

    const auto attempts = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attempts.success() && attempts.value().empty(),
        "A rejected import must leave no durable attempt behind");
}

// QML-P4R.6: the candidate directory is entirely missing at reverify time
// (never checked by a caller who skips probeCandidateHealth()'s own
// materialize()-reload-path validation) - the same generic
// workspace_unstable code every unsafe capture condition surfaces, never a
// crash or a silent no-op.
void testImportAndReverifyFailsClosedOnMissingCandidateStorage()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Import and reverify missing storage";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::error_code removeError;
        fs::remove_all(materialized.value().candidateRoot, removeError);
        req(!removeError, "Could not remove the whole candidate directory");
    }

    const auto missingHealth = manualHandoffService.probeCandidateHealth(handoff);
    req(missingHealth.success()
            && missingHealth.value().health
                == ManifestVerificationManualHandoffCandidateHealth::Missing,
        "A safely missing direct-child candidate must project the typed Missing health state");
    auto escapingHandoff = handoff;
    escapingHandoff.storageKey = "../escape";
    const auto escapingRoot = manualHandoffService.resolveCandidateRoot(escapingHandoff);
    req(!escapingRoot.success()
            && escapingRoot.error()->code == "manual_handoff.storage_containment_failed",
        "Allowing a missing direct child must not allow a traversal-shaped storage key");

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.workspace_unstable",
        "A candidate directory missing entirely at reverify time must fail closed with "
        "workspace_unstable rather than crash or silently skip verification");

    const auto attempts = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attempts.success() && attempts.value().empty(),
        "A rejected import must leave no durable attempt behind");
}

// QML-P4R.6: a symlink/reparse point introduced INTO the candidate
// directory (as opposed to the durable manualHandoffRoot's own orphan-sweep
// symlink defence, already covered elsewhere) must fail closed at reverify
// time via WorkspaceBaselineProvenance::capture()'s own reparse-point
// refusal - never followed, never silently accepted as a content change.
// Gracefully skipped (never failed) on a machine where creating a real
// symlink requires a privilege this process does not have, matching this
// file's own established precedent for symlink-dependent tests.
void testImportAndReverifyFailsClosedOnSymlinkIntroducedIntoCandidate()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir linkTarget;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(workspace.path / ".proposals");
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Import and reverify symlink escape";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    const auto linkPath = materialized.value().candidateRoot / "qml" / "escape-link";
    std::error_code linkError;
    fs::create_directory_symlink(linkTarget.path, linkPath, linkError);
    if (linkError) {
        // No privilege to create a real symlink/junction on this machine
        // (Windows without Developer Mode/admin) - this scenario cannot be
        // exercised here; never fail the suite over an environment gap.
        return;
    }

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(!imported.success() && imported.error()->code == "manual_handoff.workspace_unstable",
        "A symlink/reparse point introduced into the candidate directory must fail closed with "
        "workspace_unstable (WorkspaceBaselineProvenance::capture's own reparse-point refusal), "
        "never followed or silently accepted as a content change");

    const auto attempts = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attempts.success() && attempts.value().empty(),
        "A rejected import must leave no durable attempt behind");
}

// QML-P4R.6: the real end-to-end sequence a genuine user session produces -
// a first edit that reverifies but does not pass (a real, authoritative
// VerifiedContentFailure, never Incomplete), followed by a second,
// genuinely different edit that does pass. Distinct from
// testImportAndReverifyContentFailureReplaysWithoutReverifying (which only
// replays the SAME failing bytes) and from
// testManualHandoffAttemptAndUnmodifiedReferenceAreBothApplyEligible (whose
// own two-attempt sequence has both attempts pass) - this is the
// fail-then-pass transition neither of those covers.
void testFirstAttemptContentFailureSecondDifferentEditSucceeds()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);
    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, nullptr, nullptr, &manualHandoffs);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Fail then pass sequence";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.acceptanceContract = contract;
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {componentProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // materialize()'s baseline(clean) + candidate(warning) verify, then
    // importAndReverify()'s first combined verify - still the warning, a
    // genuine content failure (the user's first edit did not actually fix
    // the lint issue).
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for the first edit");
        appQml << "Item { anchors.fill: parent }";
    }
    const auto firstImported = manualHandoffService.importAndReverify(handoff.id);
    req(firstImported.success() && firstImported.value().finalStatus == RunStatus::Failed,
        "The first, still-failing edit must reverify to a genuine content failure, not an error");
    const auto firstAttempt = manualHandoffs.findAttemptByRunId(firstImported.value().attemptRunId);
    req(firstAttempt.success() && firstAttempt.value().has_value()
            && firstAttempt.value()->attemptOrdinal == 1,
        "The first attempt must be ordinal 1");
    const auto firstTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(firstAttempt.value()->id);
    req(firstTypedResult.success() && firstTypedResult.value().has_value()
            && firstTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure,
        "The first attempt's typed result must be VerifiedContentFailure, never Incomplete");

    // A genuinely different second edit - this time actually fixing the
    // lint issue - must allocate a fresh ordinal and run real verification
    // again, never replay the first attempt's own failing result.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not reopen the candidate for the second edit");
        appQml << "Item { anchors.fill: parent; visible: true }";
    }
    commandRunner.queuedOutcomes.push_back(cleanOutcome);
    const auto secondImported = manualHandoffService.importAndReverify(handoff.id);
    req(secondImported.success() && secondImported.value().finalStatus == RunStatus::Completed,
        "The second, genuinely different and fixed edit must reverify to a real Completed success");
    req(!secondImported.value().idempotentReplay,
        "The second attempt must do real work, never a replay of the first attempt's own result");
    const auto secondAttempt = manualHandoffs.findAttemptByRunId(secondImported.value().attemptRunId);
    req(secondAttempt.success() && secondAttempt.value().has_value()
            && secondAttempt.value()->attemptOrdinal == 2
            && secondAttempt.value()->attemptRunId != firstAttempt.value()->attemptRunId,
        "The second attempt must be a genuinely new sibling Run at ordinal 2");
    const auto secondTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(secondAttempt.value()->id);
    req(secondTypedResult.success() && secondTypedResult.value().has_value()
            && secondTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess,
        "The second attempt's typed result must be VerifiedSuccess");

    const auto attemptsForHandoff = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsForHandoff.success() && attemptsForHandoff.value().size() == 2,
        "Both the failed first attempt and the successful second attempt must remain durable "
        "audit history - neither is ever deleted or overwritten");

    // AG-16 must authorize Apply through the second (and only successful)
    // attempt - the first attempt's own content failure never carried
    // authority to begin with.
    req(secondImported.value().finalVerification.has_value()
            && secondImported.value().finalVerification->orderedProposalIds.size() == 1,
        "The final verified set must contain exactly the one frozen member");
    const auto secondSuccessorId =
        secondImported.value().finalVerification->orderedProposalIds.front();
    req(secondSuccessorId != componentProposal.value().changeSetId,
        "The verified member must be the new successor, not the original predecessor");
    const auto eligibility = applyGate.evaluate(secondSuccessorId);
    req(eligibility.allowed && eligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The second attempt's own successor must be genuinely Apply-eligible through AG-16, "
        "independent of the first attempt's own recorded content failure");
}

// QML-P4R.6 (hardened): a mixed origin/paid-child handoff whose DURABLE,
// persisted Work Ledger verification diagnostics carry NO structuredFindings
// - exactly the shape a Run retained from before typed findings persistence
// existed (the real retained bdbe0f7d... continuation predates it).
//
// ManifestVerificationAttributionService::evaluate() actually DOES read
// structuredFindings - baseline.diagnostics[].structuredFindings classify
// PreExistingVerificationDebt (ManifestVerificationAttributionService.cpp:431-436),
// and a NON-EMPTY candidate structuredFindings is REQUIRED for every
// content-repairable diagnostic (:455-456, failing closed with
// attribution_diagnostic_unmapped otherwise). It is never true that
// attribution ignores this field.
//
// What actually protects a legacy Run here is that materialize() never
// hands attribution the durable, persisted diagnostic this test seeds into
// originEntry.verification/childEntry.verification at all: it always
// reconstructs fresh "baseline" and "candidate" ManifestProposalSetVerification
// objects by genuinely re-running the provider-free verifier (prepare()
// consumes its own baseline+candidate ScriptedVerificationCommandRunner
// outcomes, and materialize() consumes two more below) - attribution
// operates on THOSE freshly re-verified diagnostics, whose structuredFindings
// are parsed fresh from the scripted command output text, never on the
// legacy row. This test's real, accurately-described proof is: a legacy
// Run's own durably persisted (possibly structuredFindings-less)
// verification record is never itself the thing attribution/materialize()
// depends on - the pipeline always re-derives typed findings fresh, so a
// legacy row can never corrupt or silently degrade a live materialize()
// call. Proven below by asserting the exact real verifier call count,
// never merely assumed.
//
// Extended (2026-09-19 hardening) to cover, on this SAME mixed
// origin/paid-child handoff, the fail-then-pass attempt sequence and its
// own AG-16 authority proof together in one fixture, exactly as QML-P4R.6's
// own contract requires: a first real edit that reverifies to a genuine
// VerifiedContentFailure, a second, genuinely different edit that reverifies
// to VerifiedSuccess, both attempts retained as durable audit history, AG-16
// authorizing Apply for the second attempt's own successor AND the
// unchanged origin member through it, and denying both the first (failed,
// non-authoritative) attempt's own successor and the paid repair's own
// original (now twice-superseded) successor. No provider or Planner call is
// possible anywhere in this path by construction - neither
// ManifestVerificationManualHandoffService,
// ManifestVerificationContinuationPreparationService, nor
// ManifestVerificationAttributionService takes a provider-client dependency
// at all; the only "verification" work in this whole file is the scripted,
// entirely local ScriptedVerificationCommandRunner exercised below.
void testMixedRunLegacyMissingFindingsWithFailThenPassAttemptSequence()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationContinuationPreparationService preparationService(
        runs, projects, manifests, ledger, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, scratch.path);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Legacy missing findings mixed-run";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification originDurable;
    originDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    originDurable.workspaceIdentity = project.id;
    originDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    originDurable.orderedProposalIds = overlayRequest.orderedProposalIds;
    originDurable.orderedProposalContentHashes = contentHashes;
    originDurable.overlayFingerprint = overlayFingerprint;
    originDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    originDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    originDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    originDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic originDiagnostic;
    originDiagnostic.code = "verification.qml_warning";
    originDiagnostic.message = "QML lint reported a warning";
    originDiagnostic.requirementId = "qml-lint";
    // Deliberately EMPTY structuredFindings - the legacy shape a Run
    // retained from before typed findings persistence existed.
    originDurable.diagnostics.push_back(std::move(originDiagnostic));

    ManifestWorkLedgerEntry originEntry;
    originEntry.runId = source.id;
    originEntry.projectId = project.id;
    originEntry.terminalStatus = RunStatus::Failed;
    originEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    originEntry.manifestSchemaVersion = "execution-manifest.v1";
    originEntry.baselineFingerprint = originDurable.baselineFingerprint;
    originEntry.verification = originDurable;
    originEntry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    originEntry.createdAt = now;
    req(ledger.insertEntry(originEntry).success(), "Could not seed the origin Work Ledger entry");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto prepared = preparationService.prepare(source.id);
    req(prepared.success(), "Could not prepare a real paid continuation child");
    const Run childRun = prepared.value().childRun;

    MaterializeRepairSuccessorRequest repairRequest;
    repairRequest.runId = childRun.id;
    repairRequest.projectId = project.id;
    repairRequest.providerId = "test-provider";
    repairRequest.modelId = "test-model";
    repairRequest.agentRole = "code-agent";
    repairRequest.predecessorChangeSetId = componentProposal.value().changeSetId;
    repairRequest.repairRawModelOutput = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Paid repair attempt",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/App.qml",
             "source_path":null,"proposed_content":"Item { anchors.fill: parent }"}
        ]
    })";
    repairRequest.policy.projectRoot = workspace.path;
    repairRequest.policy.allowedRoots = {"qml/App.qml"};
    repairRequest.policy.allowedRootsAreExactPaths = true;
    repairRequest.expectedPredecessorRunId = source.id;
    const auto successor = proposals.materializeRepairSuccessor(repairRequest);
    req(successor.success() && successor.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real cross-Run repair successor");

    BuildManifestProposalOverlayRequest childOverlayRequest = overlayRequest;
    childOverlayRequest.orderedProposalIds = {
        successor.value().changeSetId, integrationProposal.value().changeSetId};
    auto childOverlay = ManifestProposalOverlay::build(childOverlayRequest, proposals);
    req(childOverlay.success(), "Could not build the child's own reference overlay");
    const std::string childOverlayFingerprint = childOverlay.value().overlayFingerprint;
    const std::vector<std::string> childContentHashes =
        childOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(childOverlay.value()).success(),
        "Could not clean up the child's own reference overlay");

    ManifestProposalSetVerification childDurable;
    childDurable.schemaVersion = "manifest-proposal-set-verification.v1";
    childDurable.workspaceIdentity = project.id;
    childDurable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    childDurable.orderedProposalIds = childOverlayRequest.orderedProposalIds;
    childDurable.orderedProposalContentHashes = childContentHashes;
    childDurable.overlayFingerprint = childOverlayFingerprint;
    childDurable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    childDurable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    childDurable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    childDurable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic childDiagnostic;
    childDiagnostic.code = "verification.qml_warning";
    childDiagnostic.message = "QML lint still reports a warning after one repair attempt";
    childDiagnostic.requirementId = "qml-lint";
    // Deliberately EMPTY structuredFindings here too.
    childDurable.diagnostics.push_back(std::move(childDiagnostic));

    const auto childRunFetched = runs.findRun(childRun.id);
    req(childRunFetched.success() && childRunFetched.value().has_value(),
        "Could not refetch the prepared child Run");
    Run finishedChild = *childRunFetched.value();
    finishedChild.status = RunStatus::Failed;
    finishedChild.progress = 0.0;
    finishedChild.currentStageId.reset();
    finishedChild.finishedAt = now;
    req(runs.updateRun(finishedChild).success(), "Could not mark the child Run terminal Failed");

    ManifestWorkLedgerEntry childEntry;
    childEntry.runId = childRun.id;
    childEntry.projectId = project.id;
    childEntry.terminalStatus = RunStatus::Failed;
    childEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    childEntry.manifestSchemaVersion = "execution-manifest.v1";
    childEntry.baselineFingerprint = childDurable.baselineFingerprint;
    childEntry.verification = childDurable;
    childEntry.taskDispositions = {
        {"component", "succeeded", successor.value().changeSetId, std::nullopt}};
    childEntry.createdAt = now;
    req(ledger.insertEntry(childEntry).success(), "Could not seed the child's own Work Ledger entry");

    commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = manualHandoffService.materialize(childRun.id);
    req(materialized.success(),
        "A mixed-run anchor whose diagnostics carry no legacy structuredFindings must still "
        "materialize successfully because provider-free re-verification reconstructs fresh "
        "typed findings before attribution evaluates them");
    const auto& handoff = materialized.value().handoff;
    req(handoff.frozenMembers.size() == 2,
        "The frozen set must still contain exactly both mixed members");
    req(!handoff.allowedPaths.empty() && handoff.allowedPaths.front().relativePath == "qml/App.qml",
        "The remaining owner finding must still be correctly attributed and editable even "
        "without legacy structuredFindings on its diagnostic");
    req(std::filesystem::exists(materialized.value().candidateRoot / "qml" / "App.qml")
            && std::filesystem::exists(materialized.value().candidateRoot / "qml" / "CMakeLists.txt"),
        "The materialized mixed-run candidate must contain both real files");

    const std::size_t callsAfterMaterialize = commandRunner.calls;
    req(callsAfterMaterialize == 4,
        "prepare() and materialize() must each run exactly two real, local verify calls "
        "(baseline + candidate) - never trusting the legacy persisted diagnostic for either");

    // First real edit: still fails. A genuine, authoritative content
    // failure - never Incomplete, never a replay of the legacy persisted
    // diagnostic this test deliberately seeded with no structuredFindings.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the mixed-run candidate for the first edit");
        appQml << "Item { anchors.centerIn: parent }";
    }
    // Post-Stage-3 QML-lint hardening: the "integration" task's own qml-lint
    // requirement has no owned QML path at all (it owns only
    // qml/CMakeLists.txt) and no Backend-derived dependency edge to
    // qml/App.qml either - the exact project-wide/empty-scope trigger. A
    // genuine reverify final verification now also runs one baseline-only
    // probe to prove this edit's own warning is a real candidate-introduced
    // finding, never assumed pre-existing debt (queued clean here, since the
    // baseline never had this warning at all).
    commandRunner.queuedOutcomes = {warningOutcome, cleanOutcome};
    const auto firstImported = manualHandoffService.importAndReverify(handoff.id);
    req(firstImported.success() && firstImported.value().finalStatus == RunStatus::Failed,
        "The first, still-failing mixed-run edit must reverify to a genuine content failure");
    req(commandRunner.calls == callsAfterMaterialize + 2,
        "The first reverify must run exactly two real verify calls (candidate + baseline) - the "
        "integration task's own qml-lint requirement has no static QML scope");
    const auto firstAttempt = manualHandoffs.findAttemptByRunId(firstImported.value().attemptRunId);
    req(firstAttempt.success() && firstAttempt.value().has_value()
            && firstAttempt.value()->attemptOrdinal == 1,
        "The first attempt must be ordinal 1");
    const auto firstTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(firstAttempt.value()->id);
    req(firstTypedResult.success() && firstTypedResult.value().has_value()
            && firstTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure,
        "The first attempt's typed result must be VerifiedContentFailure");
    req(firstImported.value().finalVerification.has_value()
            && firstImported.value().finalVerification->orderedProposalIds.size() == 2,
        "Even a failed attempt's own final verification must carry the full mixed set");
    const auto& firstIds = firstImported.value().finalVerification->orderedProposalIds;
    const ChangeSetId firstSuccessorId = firstIds.front() == integrationProposal.value().changeSetId
        ? firstIds.back() : firstIds.front();
    req(firstSuccessorId != successor.value().changeSetId
            && firstSuccessorId != componentProposal.value().changeSetId,
        "The first attempt's own edit must produce its own genuine direct successor");

    // Second, genuinely different edit: actually fixes the lint issue.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not reopen the mixed-run candidate for the second edit");
        appQml << "Item { anchors.centerIn: parent; visible: true }";
    }
    // A genuinely clean candidate lint needs no baseline probe at all -
    // qmlLintBaselineDeltaCheck() only reclassifies a real warning-bearing
    // exit; see its own doc comment.
    commandRunner.queuedOutcomes = {cleanOutcome};
    const auto secondImported = manualHandoffService.importAndReverify(handoff.id);
    req(secondImported.success() && secondImported.value().finalStatus == RunStatus::Completed,
        "The second, genuinely different and fixed mixed-run edit must reverify to a real "
        "Completed success");
    req(!secondImported.value().idempotentReplay,
        "The second attempt must do real work, never replay the first attempt's own failure");
    req(commandRunner.calls == callsAfterMaterialize + 3,
        "The second reverify must run exactly one more real verify call - a clean candidate needs "
        "no baseline probe");
    const auto secondAttempt = manualHandoffs.findAttemptByRunId(secondImported.value().attemptRunId);
    req(secondAttempt.success() && secondAttempt.value().has_value()
            && secondAttempt.value()->attemptOrdinal == 2
            && secondAttempt.value()->attemptRunId != firstAttempt.value()->attemptRunId,
        "The second attempt must be a genuinely new sibling Run at ordinal 2");
    const auto secondTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(secondAttempt.value()->id);
    req(secondTypedResult.success() && secondTypedResult.value().has_value()
            && secondTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess,
        "The second attempt's typed result must be VerifiedSuccess");

    const auto attemptsForHandoff = manualHandoffs.listAttemptsForHandoff(handoff.id);
    req(attemptsForHandoff.success() && attemptsForHandoff.value().size() == 2,
        "Both the failed first attempt and the successful second attempt must remain durable "
        "audit history on this same mixed-run handoff");

    req(secondImported.value().finalVerification.has_value()
            && secondImported.value().finalVerification->orderedProposalIds.size() == 2,
        "The final verified mixed set must contain exactly both members");
    const auto& secondIds = secondImported.value().finalVerification->orderedProposalIds;
    const ChangeSetId secondSuccessorId = secondIds.front() == integrationProposal.value().changeSetId
        ? secondIds.back() : secondIds.front();
    req(secondSuccessorId != successor.value().changeSetId
            && secondSuccessorId != firstSuccessorId
            && secondSuccessorId != componentProposal.value().changeSetId,
        "The second attempt must materialize its own direct successor, distinct from both the "
        "paid repair's own successor and the first (failed) attempt's own successor");

    // AG-16: the second (newest successful) attempt is the sole authority
    // for the whole mixed handoff - its own successor and the unchanged
    // origin member are both Apply-eligible through it; the first attempt's
    // own successor is never independently eligible (it never carried
    // authority to begin with - its own attempt is a recorded content
    // failure, never Completed), and neither is the paid repair's own
    // original successor, now superseded twice over.
    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, nullptr, &continuations, &manualHandoffs);
    const auto secondSuccessorEligibility = applyGate.evaluate(secondSuccessorId);
    req(secondSuccessorEligibility.allowed
            && secondSuccessorEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The second attempt's own successor must be genuinely Apply-eligible through AG-16");
    const auto originMemberEligibility = applyGate.evaluate(integrationProposal.value().changeSetId);
    req(originMemberEligibility.allowed
            && originMemberEligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The unchanged origin member must be authorized through the SAME second attempt");
    const auto firstSuccessorEligibility = applyGate.evaluate(firstSuccessorId);
    req(!firstSuccessorEligibility.allowed
            && firstSuccessorEligibility.reasonCode
                == "manifest_apply.manual_handoff_attempt_superseded",
        "The first (failed, non-authoritative) attempt's own successor must never be "
        "independently Apply-eligible");
    const auto pairSuccessorEligibility = applyGate.evaluate(successor.value().changeSetId);
    req(!pairSuccessorEligibility.allowed
            && pairSuccessorEligibility.reasonCode
                == "manifest_apply.proposal_not_in_verified_set",
        "The paid repair's own original successor - now superseded twice over by the manual "
        "handoff's own two attempts - must fail with the exact authoritative-set denial code");
}

// Provider-free Manual Handoff hardening: inspectCandidateEdits() must
// classify a candidate that has all four possible drift shapes at once -
// an allowed edit, a forbidden edit, a brand-new path, and a deleted frozen
// path - into exactly the two disjoint buckets the reset command consumes.
void testInspectCandidateEditsClassifiesEditableAndResetRequiredPaths()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedA(fx.materialized.candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not open the candidate file for editing");
        editedA << "fixed-a-by-user";
    }
    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }
    {
        std::ofstream stray(fx.materialized.candidateRoot / "qml" / "Stray.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(stray), "Could not create the new candidate file");
        stray << "Item {}";
    }
    {
        std::error_code removeError;
        const bool removed = fs::remove(
            fx.materialized.candidateRoot / "qml" / "C.qml", removeError);
        req(removed && !removeError, "Could not delete the frozen candidate file");
    }

    const auto inspection = fx.service->inspectCandidateEdits(fx.materialized.handoff);
    req(inspection.success(), "inspectCandidateEdits() must classify a genuinely edited candidate");
    req(inspection.value().editableChangedPaths.size() == 1
            && inspection.value().editableChangedPaths.front() == "qml/A.qml",
        "Only the allowed edit must appear in editableChangedPaths");
    req(inspection.value().resetRequiredPaths.size() == 3,
        "The forbidden edit, the new path, and the deleted frozen path must all require reset");
    const auto& resetPaths = inspection.value().resetRequiredPaths;
    req(std::find(resetPaths.begin(), resetPaths.end(), "qml/B.qml") != resetPaths.end(),
        "The forbidden edit to B.qml must require reset");
    req(std::find(resetPaths.begin(), resetPaths.end(), "qml/C.qml") != resetPaths.end(),
        "The deleted frozen path C.qml must require reset, not be treated as an allowed delete");
    req(std::find(resetPaths.begin(), resetPaths.end(), "qml/Stray.qml") != resetPaths.end(),
        "The brand-new path must require reset");
    req(std::is_sorted(resetPaths.begin(), resetPaths.end()),
        "resetRequiredPaths must be deterministically sorted");
}

// resetNonEditableChanges() must restore the forbidden edit and the deleted
// frozen path, remove the stray new file, and leave the one authorized edit
// byte-for-byte untouched.
void testResetNonEditableChangesRestoresForbiddenNewAndDeletedPreservesEditable()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedA(fx.materialized.candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not open the candidate file for editing");
        editedA << "fixed-a-by-user";
    }
    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }
    {
        std::ofstream stray(fx.materialized.candidateRoot / "qml" / "Stray.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(stray), "Could not create the new candidate file");
        stray << "Item {}";
    }
    {
        std::error_code removeError;
        const bool removed = fs::remove(
            fx.materialized.candidateRoot / "qml" / "C.qml", removeError);
        req(removed && !removeError, "Could not delete the frozen candidate file");
    }

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(reset.success(), "resetNonEditableChanges() must succeed against a genuinely mixed diff");
    req(reset.value().resetPaths.size() == 3, "Exactly the three non-editable paths must be reset");
    req(reset.value().editablePreservedPaths.size() == 1
            && reset.value().editablePreservedPaths.front() == "qml/A.qml",
        "The one authorized edit must be reported as preserved");

    const auto readBack = [&](const fs::path& relative) {
        std::ifstream stream(fx.materialized.candidateRoot / relative, std::ios::binary);
        req(static_cast<bool>(stream), "Could not read back a candidate file after reset");
        std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return content;
    };
    req(readBack("qml/A.qml") == "fixed-a-by-user",
        "The authorized edit must survive reset byte-for-byte");
    req(readBack("qml/B.qml") == "fixed-b",
        "The forbidden edit must be restored to the frozen candidate's own byte-for-byte content");
    req(readBack("qml/C.qml") == "fixed-c",
        "The deleted frozen path must be restored to the frozen candidate's own byte-for-byte "
        "content");
    req(!fs::exists(fx.materialized.candidateRoot / "qml" / "Stray.qml"),
        "The stray new file must be physically removed by reset");

    const auto afterInspection = fx.service->inspectCandidateEdits(fx.materialized.handoff);
    req(afterInspection.success(), "inspectCandidateEdits() must still succeed after reset");
    req(afterInspection.value().resetRequiredPaths.empty(),
        "No path should require reset any more");
    req(afterInspection.value().editableChangedPaths.size() == 1
            && afterInspection.value().editableChangedPaths.front() == "qml/A.qml",
        "The authorized edit must still be visible as an editable change after reset");
}

// A real end-to-end proof that reset genuinely un-blocks importAndReverify():
// before reset the exact same candidate must be rejected for each of the
// three fail-closed reasons reset is meant to clear, and after reset the
// same candidate must reverify successfully with the sole authorized edit.
void testImportAndReverifySucceedsAfterResetNonEditableChanges()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedA(fx.materialized.candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not open the candidate file for editing");
        editedA << "fixed-a-by-user";
    }
    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }

    const auto blocked = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(!blocked.success() && blocked.error()->code == "manual_handoff.forbidden_path_changed",
        "The unreset candidate must still fail closed exactly as before reset exists");

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(reset.success(), "resetNonEditableChanges() must succeed");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    fx.commandRunner.queuedOutcomes = {cleanOutcome};

    const auto imported = fx.service->importAndReverify(fx.materialized.handoff.id);
    req(imported.success(),
        "After reset, the candidate must reverify successfully - never forbidden_path_changed, "
        "new_path_not_allowed, or path_deleted_not_allowed");
    req(imported.value().finalStatus == RunStatus::Completed,
        "A scripted clean re-lint must complete the attempt after reset");
    req(imported.value().finalVerification.has_value()
            && imported.value().finalVerification->orderedProposalIds.size() == 1,
        "The final verified set must contain exactly one member - the amended successor");
}

// manual_handoff.no_non_editable_changes: reset must refuse a no-op call
// rather than silently succeeding with an empty result, and must never
// mutate the candidate while refusing.
void testResetNonEditableChangesFailsWhenNothingToReset()
{
    MultiFilePredecessorFixture fx;

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(!reset.success() && reset.error()->code == "manual_handoff.no_non_editable_changes",
        "Resetting an untouched candidate must fail closed with no_non_editable_changes");

    std::ifstream stream(fx.materialized.candidateRoot / "qml" / "B.qml", std::ios::binary);
    req(static_cast<bool>(stream), "Could not read back B.qml after the refused reset");
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    req(content == "fixed-b", "A refused reset must never touch the candidate");
}

// manual_handoff.reset_attempt_active: a genuinely in-flight reverify
// attempt (mirroring testImportAndReverifyActiveAttemptBlocksConcurrentRetry's
// own seeding) must block a concurrent reset, exactly as it already blocks a
// concurrent reverify - both share the same importMutex_/active-Run guard.
void testResetNonEditableChangesRefusesWhileAttemptActive()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }

    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(fx.materialized.candidateRoot);
    const auto liveCapture = WorkspaceBaselineProvenance::capture(candidatePolicy);
    req(liveCapture.success(), "Could not capture the edited candidate's own fingerprint");

    const RunId attemptRunId = "attempt-run-active";
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:" + attemptRunId;
    attempt.handoffId = fx.materialized.handoff.id;
    attempt.verificationAnchorRunId = fx.materialized.handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRunId;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = liveCapture.value().fingerprint;
    attempt.createdAt = std::chrono::system_clock::now();
    req(fx.manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the in-flight attempt row");

    Run attemptRun = fx.source;
    attemptRun.id = attemptRunId;
    attemptRun.parentRunId = fx.materialized.handoff.verificationAnchorRunId;
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.status = RunStatus::Running;
    req(fx.runs.insertRun(attemptRun).success(), "Could not seed the in-flight attempt's own Run");

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(!reset.success() && reset.error()->code == "manual_handoff.reset_attempt_active",
        "A genuinely active reverify attempt must block a concurrent reset");

    std::ifstream stream(fx.materialized.candidateRoot / "qml" / "B.qml", std::ios::binary);
    req(static_cast<bool>(stream), "Could not read back B.qml after the refused reset");
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    req(content == "tampered-b", "A refused reset must never touch the candidate");
}

// manual_handoff.baseline_drift: reset must never silently rebase against a
// live workspace that has moved since materialize() - the same discipline
// importAndReverify() already enforces - and must not touch the candidate
// while refusing.
void testResetNonEditableChangesFailsClosedOnBaselineDriftWithoutMutatingCandidate()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }

    {
        std::ofstream driftedLiveFile(fx.workspace.path / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(driftedLiveFile), "Could not drift the live workspace");
        driftedLiveFile << "drifted-after-materialize";
    }

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(!reset.success() && reset.error()->code == "manual_handoff.baseline_drift",
        "A live workspace that moved since materialize() must refuse reset as an implicit rebase");

    std::ifstream stream(fx.materialized.candidateRoot / "qml" / "B.qml", std::ios::binary);
    req(static_cast<bool>(stream), "Could not read back B.qml after the refused reset");
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    req(content == "tampered-b", "A refused reset must never touch the candidate");
}

// A symlink/reparse point introduced into the candidate directory must fail
// closed exactly like importAndReverify()'s own equivalent scenario - never
// followed, read, or silently accepted as an ordinary content change.
void testResetNonEditableChangesFailsClosedOnSymlinkIntroducedIntoCandidate()
{
    MultiFilePredecessorFixture fx;
    TempDir linkTarget;

    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }

    const auto linkPath = fx.materialized.candidateRoot / "qml" / "escape-link";
    std::error_code linkError;
    fs::create_directory_symlink(linkTarget.path, linkPath, linkError);
    if (linkError) {
        // No privilege to create a real symlink/junction on this machine
        // (Windows without Developer Mode/admin) - this scenario cannot be
        // exercised here; never fail the suite over an environment gap.
        return;
    }

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(!reset.success() && reset.error()->code == "manual_handoff.workspace_unstable",
        "A symlink/reparse point introduced into the candidate directory must fail closed with "
        "workspace_unstable, never followed or silently accepted");
}

// Reset must be exactly as inert against every piece of durable state as
// the doc comment on resetNonEditableChanges() promises: the immutable
// handoff row, its frozen members/allowedPaths, and the attempt/proposal
// history must all be byte-for-byte unchanged, and no attempt Run, typed
// result, Work Ledger entry, or proposal may be created.
void testResetNonEditableChangesNeverMutatesImmutableHandoffOrDurableState()
{
    MultiFilePredecessorFixture fx;

    {
        std::ofstream editedB(fx.materialized.candidateRoot / "qml" / "B.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedB), "Could not open the candidate file for editing");
        editedB << "tampered-b";
    }

    const auto beforeAttempts = fx.manualHandoffs.listAttemptsForHandoff(fx.materialized.handoff.id);
    req(beforeAttempts.success(), "Could not read attempts before reset");
    const auto beforeProposal = fx.proposals->findChangeSet(fx.predecessorId);
    req(beforeProposal.success(), "Could not read the predecessor proposal before reset");

    const auto reset = fx.service->resetNonEditableChanges(fx.materialized.handoff.id);
    req(reset.success(), "resetNonEditableChanges() must succeed against a genuine forbidden edit");

    const auto afterHandoff = fx.manualHandoffs.findHandoffById(fx.materialized.handoff.id);
    req(afterHandoff.success() && afterHandoff.value().has_value(),
        "The immutable handoff row must still exist after reset");
    req(afterHandoff.value()->id == fx.materialized.handoff.id
            && afterHandoff.value()->storageKey == fx.materialized.handoff.storageKey
            && afterHandoff.value()->baselineFingerprint == fx.materialized.handoff.baselineFingerprint
            && afterHandoff.value()->sourceOverlayFingerprint
                == fx.materialized.handoff.sourceOverlayFingerprint
            && afterHandoff.value()->originalSnapshot.fingerprint
                == fx.materialized.handoff.originalSnapshot.fingerprint
            && afterHandoff.value()->frozenMembers.size()
                == fx.materialized.handoff.frozenMembers.size()
            && afterHandoff.value()->allowedPaths.size() == fx.materialized.handoff.allowedPaths.size(),
        "Reset must never rewrite any field of the immutable handoff row");

    const auto afterAttempts = fx.manualHandoffs.listAttemptsForHandoff(fx.materialized.handoff.id);
    req(afterAttempts.success() && afterAttempts.value().size() == beforeAttempts.value().size(),
        "Reset must never create an attempt Run or typed attempt result");

    const auto afterProposal = fx.proposals->findChangeSet(fx.predecessorId);
    req(afterProposal.success()
            && afterProposal.value().state == beforeProposal.value().state
            && afterProposal.value().changes.size() == beforeProposal.value().changes.size(),
        "Reset must never mutate the existing predecessor proposal or create a new one");
}

// Post-Stage-3 QML-lint hardening, real manual-handoff integration: the
// "integration" task's own qml-lint requirement has no owned QML path at all
// (the exact project-wide/empty-scope trigger) and the baseline already
// carries one unrelated, never-changing warning (qml/Other.qml). Proves both
// halves of the target contract's manual-handoff scenarios end to end
// through the real importAndReverify() pipeline: fixing the owner's own
// warning while a genuinely new warning appears on a path no task owns must
// still fail closed (never silently widen Manifest scope), and once that new
// warning is also gone, the unchanged baseline debt alone must never block a
// real VerifiedSuccess.
void testImportAndReverifyOutsideManifestWarningBlocksThenBaselineDebtAlonePasses()
{
    TempDir workspace;
    TempDir scratch;
    TempDir manualHandoffRoot;
    TempDir proposalStore;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestWorkLedgerRepository ledger;
    InMemoryManifestVerificationContinuationRepository continuations;
    InMemoryManifestVerificationManualHandoffRepository manualHandoffs;
    FakeChangeProposalRepository proposalRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ManifestVerificationContinuationService admission(runs, ledger, proposalRepository, continuations);
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    ManifestVerificationAttributionService attribution(
        manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifacts(artifactRepository, artifactStore);
    ManifestVerificationLegacyBaselineNormalizationService baselineNormalization(
        baselineNormalizations, artifacts, proposals);
    ManifestVerificationManualHandoffService manualHandoffService(
        runs, projects, manifests, ledger, manualHandoffs, continuations, admission, attribution,
        proposals, commandRunner, baselineNormalization, manualHandoffRoot.path, scratch.path);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Outside-manifest then baseline-debt-only sequence";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    Run source;
    source.id = "source-run";
    source.projectId = project.id;
    source.title = "Stage 3 failure";
    source.task = "Repair qml lint";
    source.workflowId = kExecutionManifestWorkflowId;
    source.status = RunStatus::Failed;
    source.createdAt = now;
    req(runs.insertRun(source).success(), "Could not seed source Run");

    const auto componentProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
    req(componentProposal.success()
            && componentProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview component proposal");
    const auto integrationProposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace.path, source.id, project.id, "chg-integration", "qml/CMakeLists.txt", "# cmake"));
    req(integrationProposal.success()
            && integrationProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create a real, ReadyForReview integration proposal");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);

    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/App.qml"};
    component.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifestTask integration;
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.acceptanceContract = contract;
    integration.requestedCapability = WorkspaceCapability::Propose;
    ExecutionManifest manifest;
    manifest.runId = source.id;
    manifest.tasks = {component, integration};
    req(manifests.insert(manifest).success(), "Could not seed source Manifest");

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baselineSnapshot = WorkspaceBaselineProvenance::capture(policy);
    req(baselineSnapshot.success(), "Could not capture the real workspace baseline");

    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace.path;
    overlayRequest.trustedScratchRoot = scratch.path;
    overlayRequest.workspaceIdentity = project.id;
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baselineSnapshot.value();
    overlayRequest.orderedProposalIds = {
        componentProposal.value().changeSetId, integrationProposal.value().changeSetId};
    auto referenceOverlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    req(referenceOverlay.success(), "Could not build the reference candidate overlay");
    const std::string overlayFingerprint = referenceOverlay.value().overlayFingerprint;
    const std::vector<std::string> contentHashes =
        referenceOverlay.value().orderedProposalContentHashes;
    req(ManifestProposalOverlay::cleanup(referenceOverlay.value()).success(),
        "Could not clean up the reference overlay");

    ManifestProposalSetVerification durable;
    durable.schemaVersion = "manifest-proposal-set-verification.v1";
    durable.workspaceIdentity = project.id;
    durable.baselineFingerprint = baselineSnapshot.value().fingerprint;
    durable.orderedProposalIds = overlayRequest.orderedProposalIds;
    durable.orderedProposalContentHashes = contentHashes;
    durable.overlayFingerprint = overlayFingerprint;
    durable.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    durable.buildLintTestStatus = ExecutionVerificationStatus::Failed;
    durable.acceptanceStatus = ExecutionVerificationStatus::Failed;
    durable.criteria.push_back({"qml-lint", true, ExecutionVerificationStatus::Failed, {}});
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning";
    diagnostic.message = "QML lint reported a warning";
    diagnostic.requirementId = "qml-lint";
    diagnostic.structuredFindings.push_back(
        {"<candidate>/qml/App.qml", 1, 1, "unqualified", "Unqualified access"});
    durable.diagnostics.push_back(std::move(diagnostic));

    ManifestWorkLedgerEntry entry;
    entry.runId = source.id;
    entry.projectId = project.id;
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = durable;
    entry.taskDispositions = {
        {"component", "succeeded", componentProposal.value().changeSetId, std::nullopt},
        {"integration", "succeeded", integrationProposal.value().changeSetId, std::nullopt}};
    entry.createdAt = now;
    req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");

    // materialize() itself runs one baseline + one candidate probe. The
    // baseline already carries the unrelated qml/Other.qml debt; the
    // candidate additionally shows the real qml/App.qml owner finding.
    ManifestVerificationCommandOutcome materializeBaseline;
    materializeBaseline.started = true;
    materializeBaseline.exitCode = 0;
    materializeBaseline.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n";
    ManifestVerificationCommandOutcome materializeCandidate;
    materializeCandidate.started = true;
    materializeCandidate.exitCode = 0;
    materializeCandidate.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/App.qml:1:1: Unqualified access [unqualified]\n";
    commandRunner.queuedOutcomes = {materializeBaseline, materializeCandidate};

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;
    req(handoff.allowedPaths.size() == 1 && handoff.allowedPaths.front().relativePath == "qml/App.qml",
        "Only the real owner finding's own path must be editable");

    // Attempt 1: the user genuinely fixes qml/App.qml, but the scripted
    // candidate lint also reports a brand-new warning on a path neither task
    // owns. The unrelated qml/Other.qml debt is unchanged in both probes.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for the first edit");
        appQml << "Item { anchors.centerIn: parent }";
    }
    ManifestVerificationCommandOutcome firstCandidate;
    firstCandidate.started = true;
    firstCandidate.exitCode = 0;
    firstCandidate.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/Stray.qml:2:1: Unqualified access [unqualified]\n";
    ManifestVerificationCommandOutcome firstBaseline;
    firstBaseline.started = true;
    firstBaseline.exitCode = 0;
    firstBaseline.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n";
    commandRunner.queuedOutcomes = {firstCandidate, firstBaseline};

    const auto firstImported = manualHandoffService.importAndReverify(handoff.id);
    req(firstImported.success() && firstImported.value().finalStatus == RunStatus::Failed,
        "A genuinely new warning outside every task's ownership must fail closed even though the "
        "real owner finding was fixed");
    const auto firstAttempt = manualHandoffs.findAttemptByRunId(firstImported.value().attemptRunId);
    req(firstAttempt.success() && firstAttempt.value().has_value(),
        "The first attempt must be a real, durable attempt row");
    const auto firstTypedResult = manualHandoffs.findAttemptResultByAttemptId(firstAttempt.value()->id);
    req(firstTypedResult.success() && firstTypedResult.value().has_value()
            && firstTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure,
        "The new outside-Manifest warning must produce a genuine, authoritative content failure - "
        "never Incomplete, never guessed debt");
    req(handoff.allowedPaths.size() == 1,
        "A candidate-introduced outside-Manifest finding must never widen allowedPaths");

    // Attempt 2: the user edits the one allowed file again (a real byte
    // change is required so this attempt actually re-verifies rather than
    // replaying the first attempt's cached content failure - see
    // testImportAndReverifyContentFailureReplaysWithoutReverifying). The
    // stray warning is gone this time. Only the unchanged, unrelated
    // qml/Other.qml debt remains, in both the candidate and the baseline
    // probe - a real VerifiedSuccess despite the module-wide lint output
    // never being warning-free.
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for the second edit");
        appQml << "Item { anchors.centerIn: parent; opacity: 1.0 }";
    }
    ManifestVerificationCommandOutcome secondCandidate;
    secondCandidate.started = true;
    secondCandidate.exitCode = 0;
    secondCandidate.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n";
    ManifestVerificationCommandOutcome secondBaseline;
    secondBaseline.started = true;
    secondBaseline.exitCode = 0;
    secondBaseline.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n";
    commandRunner.queuedOutcomes = {secondCandidate, secondBaseline};

    const auto secondImported = manualHandoffService.importAndReverify(handoff.id);
    req(secondImported.success() && secondImported.value().finalStatus == RunStatus::Completed,
        "Unchanged, unrelated baseline debt alone must never block a real VerifiedSuccess");
    req(!secondImported.value().idempotentReplay,
        "The second attempt must do real work, never replay the first attempt's own failure");
    const auto secondAttempt = manualHandoffs.findAttemptByRunId(secondImported.value().attemptRunId);
    req(secondAttempt.success() && secondAttempt.value().has_value(),
        "The second attempt must be a real, durable attempt row");
    const auto secondTypedResult =
        manualHandoffs.findAttemptResultByAttemptId(secondAttempt.value()->id);
    req(secondTypedResult.success() && secondTypedResult.value().has_value()
            && secondTypedResult.value()->kind
                == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess,
        "A genuinely resolved outside-Manifest finding plus untouched baseline debt must be a real "
        "VerifiedSuccess, not merely a non-failure");

    // AG-16: only the exact authoritative second attempt's own verified
    // proposal set may ever become Apply-eligible.
    ManifestApplyVerificationGate applyGate(
        proposals, ledger, runs, nullptr, &manifests, &continuations, &manualHandoffs);
    req(secondImported.value().finalVerification.has_value()
            && secondImported.value().finalVerification->orderedProposalIds.size() == 2,
        "The authoritative final verification must still carry the full frozen member set");
    for (const auto& proposalId : secondImported.value().finalVerification->orderedProposalIds) {
        const auto eligibility = applyGate.evaluate(proposalId);
        req(eligibility.allowed
                && eligibility.reasonCode == ManifestApplyVerificationGate::kEligibleCode,
            "Every member of the exact authoritative second attempt must be genuinely Apply-eligible");
    }
    const auto firstAttemptEligibility = applyGate.evaluate(componentProposal.value().changeSetId);
    req(!firstAttemptEligibility.allowed,
        "The first, failed attempt's own superseded successor must never be independently "
        "Apply-eligible");
}

// AG-17.0: the repaired task holds the Backend-injected requirement - the repair
// Manifest keeps it with its origin, carries the record field for field, and
// derives/duplicates nothing.
void testRepairMaterializesSuccessorAndPassesFinalCombinedVerification()
{
    runRepairMaterializationScenario(/*anchorInsideRepairSet=*/true);
}

// AG-17.0: the requirement the record names lives on a task that is NOT in the
// repair subset - the record must be omitted (never dangling) and the repair
// Manifest must still freeze and complete the same scripted repair.
void testRepairWithRecordAnchorOutsideTheRepairSetOmitsTheRecordAndStillCompletes()
{
    runRepairMaterializationScenario(/*anchorInsideRepairSet=*/false);
}

// AG-17.5: continuation never re-plans or collapses a roster. The child Run
// inherits all members, the repair subset keeps the source task's assignee,
// and the worker is dispatched through that member's provider/model rather
// than through the lead Architect or another sibling.
void testRosterContinuationInheritsSnapshotAndRepairsOnSourceTaskRoute()
{
    runRepairMaterializationScenario(
        /*anchorInsideRepairSet=*/true, /*rosterRouting=*/true);
}

// ---------------------------------------------------------------------------
// End to end with names that no single ANSI code page holds (Cyrillic and CJK) and with paths longer than MAX_PATH:
// real workspace -> baseline capture -> a real proposal through ChangeProposalService -> overlay reconstruction -> baseline and
// verification-report artifacts persisted by the real FileArtifactStore and read back through a freshly opened one.
// No provider and no paid call is involved. Names are spelled with \u escapes: the source encoding and the code page of the
// machine that compiles or runs this play no part. (The first hosted CI run found both defects on an English runner.)
// ---------------------------------------------------------------------------

template <typename ResultType>
void reqSuccess(const ResultType& result, const std::string& step)
{
    if (!result.success()) {
        throw std::runtime_error(step + " failed: " + result.error()->code + " / " + result.error()->message);
    }
}

void seedWorkspaceFile(const fs::path& root, const fs::path& relative, const std::string& content)
{
    const fs::path target = root / relative;
    std::error_code error;
    fs::create_directories(detail::longPath(target.parent_path()), error);
    req(!error, "could not create a workspace directory");
    std::ofstream out(detail::longPath(target), std::ios::binary | std::ios::trunc);
    req(static_cast<bool>(out), "could not create a workspace file");
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    req(static_cast<bool>(out), "could not write a workspace file");
}

std::string readViaLongPath(const fs::path& path)
{
    std::ifstream in(detail::longPath(path), std::ios::binary);
    req(static_cast<bool>(in), "could not open a file through its long path");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void runUnicodeLongPathScenario(std::size_t minimumWorkspaceRootLength)
{
    const fs::path base = fs::temp_directory_path()
        / ("aio-uni-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path base;
        ~Cleanup() { std::error_code ec; fs::remove_all(detail::longPath(base), ec); }
    } cleanup {base};

    fs::path workspace = base;
    int segment = 0;
    while (workspace.native().size() + 4 < minimumWorkspaceRootLength) workspace /= "seg" + std::to_string(segment++) + std::string(36, 'x');
    workspace /= "ws";

    const fs::path cyrillicDirectory = fs::path(u8"\u041F\u0440\u043E\u0435\u043A\u0442");
    const fs::path cjkFile = fs::path(u8"\u6587\u4EF6.txt");
    const fs::path cyrillicFile = fs::path(u8"\u0422\u0435\u0441\u0442.md");
    fs::path deepDirectory = "deep";
    for (int level = 0; level < 6; ++level) deepDirectory /= "level" + std::to_string(level) + std::string(34, 'd');
    const std::string cjkText = detail::genericUtf8(fs::path(u8"\u6587\u4EF6 content: \u9879\u76EE"));

    seedWorkspaceFile(workspace, fs::path("src") / "main.cpp", "int main() { return 0; }\n");
    seedWorkspaceFile(workspace, cyrillicDirectory / cjkFile, cjkText);
    seedWorkspaceFile(workspace, cyrillicDirectory / cyrillicFile, "# Cyrillic file name\n");
    seedWorkspaceFile(workspace, deepDirectory / "leaf.txt", "deep leaf\n");
    // A repository: the baseline reads .git/HEAD and the referenced ref through plain workspace paths (not through the scan iterator).
    const std::string revision = "0123456789abcdef0123456789abcdef01234567";
    seedWorkspaceFile(workspace, fs::path(".git") / "HEAD", "ref: refs/heads/main\n");
    seedWorkspaceFile(workspace, fs::path(".git") / "refs" / "heads" / "main", revision + "\n");
    req((workspace / deepDirectory / "leaf.txt").native().size() > 260, "the deep file must exceed MAX_PATH to prove anything");

    const auto step = [](const char* name) { std::cerr << "[unicode e2e] " << name << "\n"; };
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace);
    const auto baseline = WorkspaceBaselineProvenance::capture(policy);
    reqSuccess(baseline, "baseline capture");
    const std::vector<std::string> expectedPaths = {
        detail::genericUtf8(cyrillicDirectory / cjkFile),
        detail::genericUtf8(cyrillicDirectory / cyrillicFile),
        detail::genericUtf8(deepDirectory / "leaf.txt"),
        "src/main.cpp"};
    std::vector<std::string> capturedPaths;
    for (const auto& entry : baseline.value().files) capturedPaths.push_back(entry.relativePath);
    req(capturedPaths.size() == expectedPaths.size(), "the baseline must list every file, long and non-ANSI names included (.git is excluded by policy)");
    req(baseline.value().repositoryRevisionAvailable && baseline.value().repositoryRevision == revision,
        "the repository revision must be read from .git even when the workspace root is long");
    for (const auto& expected : expectedPaths) {
        req(std::find(capturedPaths.begin(), capturedPaths.end(), expected) != capturedPaths.end(),
            ("the baseline lacks the path (as UTF-8): " + expected).c_str());
    }
    const auto cjkEntry = std::find_if(baseline.value().files.begin(), baseline.value().files.end(),
        [&](const WorkspaceBaselineEntry& entry) { return entry.relativePath == expectedPaths[0]; });
    req(cjkEntry != baseline.value().files.end() && cjkEntry->sha256 == sha256Hex(cjkText) && cjkEntry->byteSize == cjkText.size(),
        "the non-ANSI file must be hashed from its real bytes");
    const auto second = WorkspaceBaselineProvenance::capture(policy);
    reqSuccess(second, "second baseline capture");
    req(second.value().fingerprint == baseline.value().fingerprint, "the baseline fingerprint must be deterministic");
    // A link inside a workspace whose path is longer than MAX_PATH must still be seen as one: the plain GetFileAttributesW answers
    // "invalid" for such a path, which a naive check reads as "not a reparse point" (fail open).
    {
        const fs::path outside = base / "outside-of-the-workspace";
        seedWorkspaceFile(outside.parent_path(), fs::path("outside-of-the-workspace") / "x.txt", "x\n");
        const fs::path link = workspace / "linked-directory";
        std::error_code linkError;
        fs::create_directory_symlink(detail::longPath(outside), detail::longPath(link), linkError);
        if (linkError) {
            std::cerr << "[unicode e2e] no privilege to create a symbolic link here: the reparse-point check is skipped\n";
        } else {
            const auto withLink = WorkspaceBaselineProvenance::capture(policy);
            req(!withLink.success() && withLink.error()->code == "workspace_baseline.reparse_point",
                "a link inside a long workspace must make the baseline refuse (reparse point)");
            std::error_code removeError;
            fs::remove(detail::longPath(link), removeError);
            req(!removeError, "could not remove the test link");
            const auto afterRemoval = WorkspaceBaselineProvenance::capture(policy);
            reqSuccess(afterRemoval, "baseline capture after the link was removed");
            req(afterRemoval.value().fingerprint == baseline.value().fingerprint, "removing the link restores the same baseline");
        }
    }

    step("2 proposal");
    FakeChangeProposalRepository proposalRepository;
    auto store = FileChangeProposalStore::open(base / "proposals");
    reqSuccess(store, "proposal store");
    RunPathClaimRegistry pathClaims;
    ChangeProposalService proposals(proposalRepository, *store.value(), &pathClaims);
    const auto proposal = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace, "unicode-run", "unicode-project", "chg-cjk", "src/\\u9879\\u76EE.qml", "Item {}"));
    reqSuccess(proposal, "proposal creation");
    if (proposal.value().state != ChangeSetState::ReadyForReview) {
        std::string findings;
        for (const auto& finding : proposal.value().validationFindings) findings += " [" + finding.code + ": " + finding.message + "]";
        throw std::runtime_error("the proposal must be ReadyForReview, validation findings:" + findings);
    }

    // The cross-agent path claim is keyed by the path text: a second proposal for the same CJK-named file must be refused as a
    // conflict, not crash on the name and not slip through.
    const auto conflicting = proposals.createFromModelOutput(makeCreateFileRequest(
        workspace, "unicode-run", "unicode-project", "chg-cjk-2", "src/\\u9879\\u76EE.qml", "Item { x: 1 }"));
    reqSuccess(conflicting, "conflicting proposal creation");
    req(conflicting.value().state == ChangeSetState::Rejected, "a second proposal for the same non-ANSI path must be rejected");
    bool conflictReported = false;
    for (const auto& finding : conflicting.value().validationFindings) {
        if (finding.code == "proposal.cross_agent_conflict") conflictReported = true;
    }
    req(conflictReported, "the rejection must name the cross-agent conflict");
    step("3 overlay");
    BuildManifestProposalOverlayRequest overlayRequest;
    overlayRequest.workspaceRoot = workspace;
    overlayRequest.trustedScratchRoot = base / "scratch";
    overlayRequest.workspaceIdentity = "unicode-project";
    overlayRequest.baselinePolicy = policy;
    overlayRequest.baselineSnapshot = baseline.value();
    overlayRequest.orderedProposalIds = {proposal.value().changeSetId};
    auto overlay = ManifestProposalOverlay::build(overlayRequest, proposals);
    reqSuccess(overlay, "overlay build");
    req(overlay.value().fileCount == expectedPaths.size() + 1, "the overlay holds the baseline files plus the proposed one");
    for (const auto& relative : expectedPaths) {
        const fs::path inOverlay = overlay.value().root / detail::pathFromUtf8(relative);
        req(fs::exists(detail::longPath(inOverlay)), ("the overlay lacks " + relative).c_str());
    }
    req(readViaLongPath(overlay.value().root / detail::pathFromUtf8(expectedPaths[0])) == cjkText, "the overlay copy of the non-ANSI file must be byte-identical");
    req(readViaLongPath(overlay.value().root / detail::pathFromUtf8(detail::genericUtf8(fs::path("src") / fs::path(u8"\u9879\u76EE.qml")))) == "Item {}",
        "the proposed file with a CJK name must be materialised");
    const auto overlayRoot = overlay.value().root;
    reqSuccess(ManifestProposalOverlay::cleanup(overlay.value()), "overlay cleanup");
    req(!fs::exists(detail::longPath(overlayRoot)), "the overlay must be gone after cleanup");

    step("4 artifacts");
    InMemoryArtifactRepository artifactRepository;
    const std::string baselineJson = WorkspaceBaselineProvenance::renderJson(baseline.value()).value();
    const std::string report = "verified " + expectedPaths[0] + "\n";
    ArtifactId baselineId;
    ArtifactId reportId;
    {
        auto artifactStore = FileArtifactStore::open(base / "artifacts");
        reqSuccess(artifactStore, "artifact store");
        ArtifactService artifacts(artifactRepository, *artifactStore.value());
        const auto baselineArtifact = artifacts.createArtifact("unicode-run", std::nullopt, ArtifactKind::Report,
            "workspace-baseline.v1.json", "application/json", baselineJson);
        reqSuccess(baselineArtifact, "baseline artifact");
        baselineId = baselineArtifact.value().id;
        const auto reportArtifact = artifacts.createArtifact("unicode-run", std::nullopt, ArtifactKind::Report,
            "manifest-verification.md", "text/markdown", report);
        reqSuccess(reportArtifact, "report artifact");
        reportId = reportArtifact.value().id;
    }
    {
        auto reopened = FileArtifactStore::open(base / "artifacts");
        reqSuccess(reopened, "artifact store reopen");
        ArtifactService artifacts(artifactRepository, *reopened.value());
        const auto baselineBack = artifacts.getArtifactContent(baselineId);
        reqSuccess(baselineBack, "baseline artifact read after reopen");
        const auto parsed = WorkspaceBaselineProvenance::parseJson(baselineBack.value());
        reqSuccess(parsed, "baseline artifact parse");
        req(parsed.value().fingerprint == baseline.value().fingerprint && parsed.value().files.size() == expectedPaths.size(),
            "the persisted baseline must describe the same files after a restart");
        const auto reportBack = artifacts.getArtifactContent(reportId);
        reqSuccess(reportBack, "report artifact read after reopen");
        req(reportBack.value() == report, "the persisted report must be byte-identical after a restart");
    }
}

void testUnicodeNamesAndLongPathsSurviveBaselineProposalOverlayAndArtifacts()
{
    runUnicodeLongPathScenario(60);    // a short root: the deep file alone passes MAX_PATH
    runUnicodeLongPathScenario(180);   // a long root as well
    runUnicodeLongPathScenario(300);   // the workspace root alone is longer than MAX_PATH
}
} // namespace

int runManifestVerificationContinuationServiceTests()
{
    testAdmitsAFullyEligibleFailedRun();
    testIdempotentOnRepeatCall();
    testDeniesNonManifestWorkflow();
    testDeniesNonFailedRun();
    testDeniesMissingLedgerEntry();
    testDeniesPreFreezeFailure();
    testDeniesMissingVerificationIdentity();
    testDeniesFailedProposalSafety();
    testDeniesNoFailedCriteria();
    testDeniesNonRepairableDiagnostic();
    testOutsideScopePassedDiagnosticDoesNotBlockContentRepairAdmission();
    testDeniesChangedProposalState();
    testDeniesMissingSourceRun();
    testAttributesCandidateFindingToActualProposalOwner();
    testAttributesRequirementOwnerAsIntegrationOwner();
    testAttributionMapsPartialRepairSuccessorsToExactVerifiedAncestors();
    testAttributionFailsClosedOnMissingOrCyclicPartialRepairLineage();
    testAttributionFailsClosedWhenTwoTasksResolveToOneVerifiedLineageRoot();
    testLegacyCachedOwnershipMismatchReevaluatesWithoutRewritingAuditRow();
    testLegacyCachedOwnershipMismatchSurfacesCurrentBlockWithoutRewritingAuditRow();
    testMixedAnchorLegacyOwnershipMismatchSurfacesCurrentBlockWithoutRewritingAuditRow();
    testAttributionFallsBackToFrozenScopeWhenManifestOwnerMissing();
    testAttributionFailsClosedOnManifestScopeConflict();
    testAttributionRequiresScopeToCorroborateAManifestOwner();
    testAttributionFailsClosedOnStructurallyInvalidScope();
    testAttributionValidatesTheEntireFrozenScopeBeforePathLookup();
    testAttributionRejectsMalformedFrozenEntriesOfEveryRole();
    testAttributionRejectsInvalidDependencyEdgesAndScopeBounds();
    testInvalidScopeBlocksEvenWhenEveryFindingIsPreExistingDebt();
    testProvesPreExistingDebtByExactBaselineFinding();
    testCandidateIntroducedOutsideManifestFailsClosed();
    testOutsideScopeBaselineWarningRemainsPreExistingWhenCandidateHasMixedWarnings();
    testOutsideScopePassedDiagnosticDoesNotBlockAttribution();
    testUnstructuredAndIdentityMismatchedResultsFailClosedAndRemainImmutable();
    testAttributionSourceMismatchNamesTheChangedIdentityField();
    testPrepareCreatesInputAndPreparedChildForAFullyEligibleRun();
    testPrepareNamesThePathBudgetWhenTheVerificationCopyCannotFit();
    testPrepareIsRefusedBeforeAnyCopyWhenTheVerificationFolderIsNotUsable();
    testPrepareBuildsItsCopiesUnderTheRootTheBackendValidated();
    testPrepareNormalizesLegacyQtCreatorBaselineDriftAndPinsFreshIdentity();
    testLegacyBaselineNormalizationRefusesWhenAProposalTouchesTheExcludedPrefix();
    testPrepareIsIdempotentAndNeverRepeatsVerificationOnRepeatCall();
    testPreparedChildRecoveryIsIdempotentAndNeverQueuesProviderWork();
    testTerminalContinuationChildSurvivesReconciliationWithoutConflictOrMutation();
    testReadModelEligibilityIsReadOnlyAndBackendOwned();
    testReadModelProjectsLineageEvidenceAndBlocksPreExistingDebt();
    testReadModelProjectsNeedsManualActionForBoundedAdvisoryChildFailure();
    testReadModelKeepsUndifferentiatedProjectionWhenAHardFindingRemains();
    testReadModelFailsClosedForOrphanedContinuationChild();
    testReadModelRejectsNormalizedIdentityWithoutExactProof();
    testRepairMaterializesSuccessorAndPassesFinalCombinedVerification();
    testRepairWithRecordAnchorOutsideTheRepairSetOmitsTheRecordAndStillCompletes();
    testRosterContinuationInheritsSnapshotAndRepairsOnSourceTaskRoute();
    testMaterializeProducesDurableHandoffRestrictedToOwnerFindingTargets();
    testMaterializeSucceedsWithRealisticallyLongManualHandoffRootViaShortScratchVerification();
    testMaterializeNamesThePathBudgetWhenTheVerificationCopyCannotFit();
    testMaterializeIsRefusedBeforeAnyCopyWhenTheVerificationFolderIsNotUsable();
    testMaterializeBuildsItsVerificationCopiesUnderTheRootTheBackendValidated();
    testUnicodeNamesAndLongPathsSurviveBaselineProposalOverlayAndArtifacts();
    testImportAndReverifyVerifiesInShortScratchDespiteLongManualHandoffRoot();
    testMaterializeFailsClosedWhenExecutionScratchRootItselfExceedsPathBudget();
    testMaterializeFailsClosedOnDurableCandidateDriftDuringVerification();
    testImportAndReverifyPreservesCarriedForwardChangesInMultiFilePredecessor();
    testImportAndReverifyRejectsEditToNonAllowedFileInMultiFilePredecessor();
    testImportAndReverifyRejectsNewPathInMultiFilePredecessor();
    testImportAndReverifyFailsClosedOnCorruptCarriedForwardModifyHashEvidence();
    testImportAndReverifyFailsClosedOnCorruptCarriedForwardDeleteHashEvidence();
    testImportAndReverifyProducesSuccessorAttemptAndIsIdempotent();
    testImportAndReverifyContentFailureReplaysWithoutReverifying();
    testImportAndReverifySameBytesAfterInfrastructureFailureAllocatesNewOrdinal();
    testImportAndReverifyActiveAttemptBlocksConcurrentRetry();
    testImportAndReverifyMissingTypedResultFailsClosed();
    testImportAndReverifyResultAttemptRunIdMismatchFailsClosed();
    testImportAndReverifySameBytesAfterProposalSafetyRejectionReplayWithoutNewOrdinal();
    testImportAndReverifyRejectsAnEditOutsideTheAllowedPath();
    testManualHandoffAttemptAndUnmodifiedReferenceAreBothApplyEligible();
    testMaterializeFailsClosedWhenNoOwnerFindingIsEditable();
    testMaterializeFailsClosedOnMissingLineagePredecessor();
    testMaterializeFailsClosedOnCyclicLineage();
    testMaterializeResolvesMultiHopLineageRootThroughPriorRepairGeneration();
    testMixedRunFrozenSetAuthorizesBothOriginAndContinuationChildMembers();
    testMaterializeResolvesMixedRunOriginAndProducesEligibleHandoff();
    testMaterializeResolvesMixedRunAfterLegacyBaselineNormalization();
    testMaterializeFailsClosedOnGenuineDriftForMixedRunAnchor();
    testMaterializeRejectsReorderedLineageInExistingMixedRunHandoff();
    testMaterializeFailsClosedOnCorruptedContinuationInputProjectIdentity();
    testMaterializeFailsClosedWhenOriginRunNoLongerOwnsAManifest();
    testMaterializeRevalidatesExistingCandidateHealthOnEveryLoad();
    testDiscardHandoffReleasesProposalClaimAndRemovesCandidate();
    testDiscardHandoffRefusesWhileAttemptActive();
    testReconcileLeavesRunlessAttemptHarmlessAndFingerprintUnblocked();
    testReconcileMarksIncompleteAttemptSuccessorMissingEvidenceAndReleasesClaim();
    testReconcileSurfacesMarkMissingEvidenceFailureRatherThanSilentlyDiscardingIt();
    testReconcileRefusesToTreatExistingVerifiedResultAsIncomplete();
    testReconcileRefusesLedgerResultKindConflictBeforeRepairingRun();
    testReconcileRepairsRunAndStageFromLedgerWithoutTouchingProposals();
    testReconcileInterruptedAttemptsIsIdempotentOnRepeatedCall();
    testEnforceRetentionEvictsOldestEligibleAndReleasesProposalClaim();
    testEnforceRetentionProtectsActiveSuccessfulAndFreshHandoffs();
    testEnforceRetentionRepeatedCallIsIdempotent();
    testMaterializeFailsClosedWhenRetentionCapacityExhaustedByProtectedHandoffs();
    testSqliteEnforceRetentionCascadeIncludesAttemptResultsTable();
    testSqliteEnforceRetentionSweepsOrphanDirectoryAfterRestart();
    testSqliteEnforceRetentionNeverFollowsOrRemovesSymlinkEscape();
    testImportAndReverifyRejectsANewPathNotInFrozenSet();
    testImportAndReverifyRejectsADeletedFrozenPath();
    testImportAndReverifyFailsClosedOnMissingCandidateStorage();
    testImportAndReverifyFailsClosedOnSymlinkIntroducedIntoCandidate();
    testFirstAttemptContentFailureSecondDifferentEditSucceeds();
    testMixedRunLegacyMissingFindingsWithFailThenPassAttemptSequence();
    testInspectCandidateEditsClassifiesEditableAndResetRequiredPaths();
    testResetNonEditableChangesRestoresForbiddenNewAndDeletedPreservesEditable();
    testImportAndReverifySucceedsAfterResetNonEditableChanges();
    testResetNonEditableChangesFailsWhenNothingToReset();
    testResetNonEditableChangesRefusesWhileAttemptActive();
    testResetNonEditableChangesFailsClosedOnBaselineDriftWithoutMutatingCandidate();
    testResetNonEditableChangesFailsClosedOnSymlinkIntroducedIntoCandidate();
    testResetNonEditableChangesNeverMutatesImmutableHandoffOrDurableState();
    testImportAndReverifyOutsideManifestWarningBlocksThenBaselineDebtAlonePasses();
    return 0;
}
