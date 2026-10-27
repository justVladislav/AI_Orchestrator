// Unit tests for ManifestVerificationManualHandoffReadModelService (QML-P4R.5
// package 1): the single authoritative Backend projection a Qt/QML adapter
// reads manual-handoff state through. Entirely in-memory - no SQLite, no
// provider sessions. Proves the read model is a pure, deterministic
// projection over already-durable Backend state (handoff/attempt/typed-result
// rows, the Work Ledger, and a real ManifestApplyVerificationGate (AG-16)
// evaluation) - it never invents eligibility, never re-derives candidate
// health, and never selects the authoritative attempt independently of
// QML-P4R.4's own established rule (the highest-ordinal VerifiedSuccess
// attempt).
#include "ai/application/ManifestVerificationManualHandoffReadModelService.hpp"

#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/InMemoryManifestVerificationContinuationRepository.hpp"
#include "ai/application/InMemoryManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryRunWorkspaceScopeRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestApplyVerificationGate.hpp"
#include "ai/application/ManifestProposalContentHash.hpp"
#include "ai/application/ManifestProposalOverlay.hpp"
#include "ai/application/ManifestVerificationAttributionService.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationContinuationService.hpp"
#include "ai/application/ManifestVerificationLegacyBaselineNormalizationService.hpp"
#include "ai/application/ManifestVerificationManualHandoffService.hpp"
#include "ai/application/proposals/ProposalReviewService.hpp"
#include "ai/application/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
using namespace ai::infrastructure;
namespace common = ai::common;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

struct TempDir {
    fs::path path;

    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-manual-handoff-rm-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

// Minimal real IChangeProposalRepository fake - same shape as the one every
// other manual-handoff test file in this codebase duplicates locally (per
// this codebase's own convention; see e.g. ManifestVerificationContinuationServiceTests.cpp).
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
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "not found"});
        it->second.missingEvidenceCode = std::move(code);
        it->second.missingEvidenceMessage = std::move(message);
        return common::Result<void>::ok();
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
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second.changes);
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
};

// Post-P4R.6 human review/Apply hardening: same minimal shape as
// ManifestWorkLedgerServiceTests.cpp's own local FakeProposalReviewRepository
// - this codebase's own convention is per-file fixture duplication (see this
// file's own FakeChangeProposalRepository doc comment above).
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

class UnusedVerificationRunner final : public IManifestVerificationCommandRunner {
public:
    std::size_t calls {0};
    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest&, std::stop_token) override
    { ++calls; return {}; }
};

// Same shape as ManifestVerificationContinuationServiceTests.cpp's own
// ScriptedVerificationCommandRunner - pops queuedOutcomes in call order,
// falling back to a clean outcome once exhausted.
class ScriptedVerificationCommandRunner final : public IManifestVerificationCommandRunner {
public:
    std::deque<ManifestVerificationCommandOutcome> queuedOutcomes;
    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest&, std::stop_token) override
    {
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

// Full fixture: project, source Run, a real ReadyForReview component
// proposal, its frozen Manifest, and the durable source Work Ledger entry
// materialize() itself requires for admission - the exact same boilerplate
// ManifestVerificationContinuationServiceTests.cpp's own materialize()-based
// tests use, kept local since this codebase's own convention is per-file
// fixture duplication (see e.g. ManifestApplyVerificationGateTests.cpp).
struct Fixture {
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
    FakeProposalReviewRepository proposalReviewRepository;
    std::shared_ptr<IChangeProposalStore> store;
    std::unique_ptr<ChangeProposalService> proposals;
    std::unique_ptr<ProposalReviewService> reviewService;
    std::unique_ptr<ManifestVerificationContinuationService> admission;
    InMemoryRunWorkspaceScopeRepository workspaceScopes;
    InMemoryManifestVerificationBaselineNormalizationRepository baselineNormalizations;
    std::unique_ptr<ManifestVerificationAttributionService> attribution;
    ScriptedVerificationCommandRunner commandRunner;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    std::unique_ptr<ArtifactService> artifacts;
    std::unique_ptr<ManifestVerificationLegacyBaselineNormalizationService> baselineNormalization;
    std::unique_ptr<ManifestVerificationManualHandoffService> manualHandoffService;
    std::unique_ptr<ManifestApplyVerificationGate> applyGate;
    std::unique_ptr<ManifestVerificationManualHandoffReadModelService> readModel;

    Project project;
    Run source;
    ChangeSetId componentProposalId;

    Fixture()
    {
        auto storeResult = FileChangeProposalStore::open(proposalStore.path);
        req(storeResult.success(), "Could not open the real proposal store");
        store = std::move(storeResult.value());
        proposals = std::make_unique<ChangeProposalService>(proposalRepository, *store);
        reviewService = std::make_unique<ProposalReviewService>(
            proposalRepository, proposalReviewRepository);
        admission = std::make_unique<ManifestVerificationContinuationService>(
            runs, ledger, proposalRepository, continuations);
        attribution = std::make_unique<ManifestVerificationAttributionService>(
            manifests, ledger, continuations, workspaceScopes, baselineNormalizations);
        artifacts = std::make_unique<ArtifactService>(artifactRepository, artifactStore);
        baselineNormalization = std::make_unique<ManifestVerificationLegacyBaselineNormalizationService>(
            baselineNormalizations, *artifacts, *proposals);
        manualHandoffService = std::make_unique<ManifestVerificationManualHandoffService>(
            runs, projects, manifests, ledger, manualHandoffs, continuations, *admission,
            *attribution, *proposals, commandRunner, *baselineNormalization,
            manualHandoffRoot.path, scratch.path);
        applyGate = std::make_unique<ManifestApplyVerificationGate>(
            *proposals, ledger, runs,
            /*verificationBootstrapRepository=*/nullptr, &manifests, &continuations,
            &manualHandoffs);
        readModel = std::make_unique<ManifestVerificationManualHandoffReadModelService>(
            runs, ledger, manualHandoffs, *manualHandoffService, *applyGate,
            *proposals, *reviewService);

        const auto now = std::chrono::system_clock::now();
        project.id = "project-1";
        project.name = "Manual handoff read model";
        project.rootPath = workspace.path;
        project.status = ProjectStatus::Active;
        project.createdAt = now;
        project.updatedAt = now;
        req(projects.insert(project).success(), "Could not seed project");

        source.id = "source-run";
        source.projectId = project.id;
        source.title = "Stage 3 failure";
        source.task = "Repair qml lint";
        source.workflowId = kExecutionManifestWorkflowId;
        source.status = RunStatus::Failed;
        source.createdAt = now;
        req(runs.insertRun(source).success(), "Could not seed source Run");

        const auto componentProposal = proposals->createFromModelOutput(makeCreateFileRequest(
            workspace.path, source.id, project.id, "chg-component", "qml/App.qml", "Item {}"));
        req(componentProposal.success()
                && componentProposal.value().state == ChangeSetState::ReadyForReview,
            "Could not create a real, ReadyForReview component proposal");
        componentProposalId = componentProposal.value().changeSetId;

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
        overlayRequest.orderedProposalIds = {componentProposalId};
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
            {"<candidate>/qml/App.qml", 4, 1, "unqualified", "Unqualified access"});
        durable.diagnostics.push_back(std::move(diagnostic));

        ManifestWorkLedgerEntry entry;
        entry.runId = source.id;
        entry.projectId = project.id;
        entry.terminalStatus = RunStatus::Failed;
        entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
        entry.manifestSchemaVersion = "execution-manifest.v1";
        entry.verification = durable;
        entry.taskDispositions = {{"component", "succeeded", componentProposalId, std::nullopt}};
        entry.createdAt = now;
        req(ledger.insertEntry(entry).success(), "Could not seed the source Work Ledger entry");
    }
};

void testViewForRunReturnsNulloptForMissingRun()
{
    Fixture fx;
    const auto view = fx.readModel->viewForRun("does-not-exist");
    req(view.success() && !view.value().has_value(),
        "A missing Run must project to nullopt, never an error or a stale view");
}

void testViewForRunReturnsNulloptForUnrelatedWorkflow()
{
    Fixture fx;
    Run mockRun;
    mockRun.id = "mock-run";
    mockRun.projectId = fx.project.id;
    mockRun.title = "Mock";
    mockRun.task = "task";
    mockRun.workflowId = "mock";
    mockRun.status = RunStatus::Completed;
    mockRun.createdAt = std::chrono::system_clock::now();
    req(fx.runs.insertRun(mockRun).success(), "Could not seed the mock Run");

    const auto view = fx.readModel->viewForRun(mockRun.id);
    req(view.success() && !view.value().has_value(),
        "A Run from an unrelated workflow must project to nullopt - a manual handoff can never "
        "mean anything for it");
}

void testViewForRunShowsNotYetCreatedWhenNoHandoff()
{
    Fixture fx;
    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(),
        "A real, eligible anchor Run with no handoff yet must still project a view");
    const auto& v = *view.value();
    req(!v.hasHandoff, "hasHandoff must be false before materialize() is ever called");
    req(v.verificationAnchorRunId == fx.source.id, "The anchor identity must be projected");
    req(v.verificationAnchorStatus == toString(RunStatus::Failed),
        "The anchor's own current status must be projected even with no handoff yet");
    req(v.handoffId.empty() && v.attempts.empty() && v.allowedRelativePaths.empty()
            && !v.hasFinalVerification && !v.canApply,
        "Every handoff-dependent field must stay at its default before materialize()");
    req(v.canCreate, "A genuinely eligible source Run must offer Create");
    req(v.createReasonCode.empty() && v.createReasonMessage.empty(),
        "A true canCreate must carry no denial reason");
}

void testViewForRunProjectsMaterializedHandoffCandidateHealthAndAllowedPaths()
{
    Fixture fx;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    fx.commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = fx.manualHandoffService->materialize(fx.source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "A materialized handoff must project a view");
    const auto& v = *view.value();
    req(v.hasHandoff, "hasHandoff must be true once materialize() has succeeded");
    req(v.handoffId == materialized.value().handoff.id, "The real handoff id must be projected");
    req(v.originManifestRunId == fx.source.id, "The simple (non-continuation) case's own origin "
        "equals its anchor");
    req(v.originManifestStatus == toString(RunStatus::Failed),
        "The origin Run's own current status must be projected");
    req(v.candidateHealth == "unchanged",
        "A freshly materialized, never-edited candidate must read back Unchanged");
    req(v.allowedRelativePaths.size() == 1 && v.allowedRelativePaths.front() == "qml/App.qml",
        "The handoff's own frozen allowed path must be projected verbatim");
    req(v.attempts.empty() && !v.hasFinalVerification && !v.canApply,
        "No reverify has happened yet - attempt history and Apply eligibility stay empty/false");
    req(v.initialScopeDiagnostics.size() == 1
            && v.initialScopeDiagnostics.front().find("qml/App.qml:4:1") != std::string::npos
            && v.initialScopeDiagnostics.front().find("Unqualified access") != std::string::npos,
        "The anchor Run's own in-scope qml-lint finding must be visible before any Reverify attempt");
}

// Full end-to-end: materialize -> real user edit -> successful provider-free
// reverify -> the read model's own attempt history/authoritative flag/final
// verification, AND a REAL ManifestApplyVerificationGate (AG-16) evaluation -
// never a hand-constructed "fake eligible" state, since AG-16's own strict
// cross-checks (exact set, hash, lineage) are exactly what this test must
// prove the read model defers to rather than recomputing.
void testViewForRunProjectsAuthoritativeAttemptAndRealAg16ApplyEligibility()
{
    Fixture fx;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    // materialize()'s own baseline verify (clean), candidate verify (the
    // real warning), then importAndReverify()'s one final combined verify -
    // scripted clean, standing in for "the user's fix actually worked."
    fx.commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, cleanOutcome};

    const auto materialized = fx.manualHandoffService->materialize(fx.source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.fill: parent }";
    }

    const auto imported = fx.manualHandoffService->importAndReverify(handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Completed,
        "A legitimate single-file edit must reverify to a genuine Completed authoritative result");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "A handoff with a real attempt must project");
    const auto& v = *view.value();
    req(v.candidateHealth == "edited",
        "The candidate must read back Edited once the user's real edit is on disk");
    req(v.attempts.size() == 1, "Exactly one real attempt must be projected");
    const auto& attemptView = v.attempts.front();
    req(attemptView.attemptRunId == imported.value().attemptRunId,
        "The projected attempt must be the real one importAndReverify() just created");
    req(attemptView.resultKind == "verified_success",
        "A genuinely passed reverify must project VerifiedSuccess, never a guessed kind");
    req(attemptView.isAuthoritative,
        "The sole VerifiedSuccess attempt must be flagged as the handoff's own authority");
    req(v.hasFinalVerification, "A VerifiedSuccess attempt must carry a final verification summary");
    req(v.finalProposalSafetyStatus == toString(ExecutionVerificationStatus::Passed)
            && v.finalBuildLintTestStatus == toString(ExecutionVerificationStatus::Passed)
            && v.finalAcceptanceStatus == toString(ExecutionVerificationStatus::Passed),
        "The final verification's own three top-level statuses must be projected verbatim");

    // The real AG-16 authority, never recomputed by the read model itself.
    // requiredAcceptancePassed's own Manifest-workflow scope means this
    // Run's proposal set is genuinely, independently eligible once verified.
    req(v.canApply, "A real, exact-match, fully-passed verified set must be AG-16-eligible");
    req(v.applyReasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The Apply reason code must be AG-16's own stable eligible code, not a read-model guess");

    // Post-P4R.6 human review/Apply hardening: reviewMembers must project
    // the exact same authoritative ledger proposal set, with a real,
    // freshly-evaluated AG-16 decision per member - never a second,
    // independently-maintained copy of canApply's own logic. This scenario's
    // authoritative member is genuinely a SUCCESSOR of fx.componentProposalId
    // (the user's real edit above went through importAndReverify()'s own
    // materializeRepairSuccessor path, which amends the original proposal
    // into a brand-new proposal id owned by the attempt Run) - so this
    // assertion deliberately checks the real successor's own shape, never
    // the original id/Run, which the exact-set multi-member test below
    // already covers for the unchanged case.
    req(v.reviewMembers.size() == 1, "Exactly one authoritative member must be projected");
    const auto& reviewMember = v.reviewMembers.front();
    req(!reviewMember.proposalId.empty()
            && reviewMember.proposalId != fx.componentProposalId,
        "The real edit must have produced a genuine successor proposal id, distinct from the "
        "original component proposal");
    req(!reviewMember.owningRunId.empty() && reviewMember.owningRunId != fx.source.id,
        "The successor's real owning Run must be the attempt Run that amended it, never the "
        "original source Run");
    req(reviewMember.state == toString(ChangeSetState::ReadyForReview),
        "An unapplied, unblocked member's real state must be projected verbatim");
    req(!reviewMember.alreadyApplied, "A ReadyForReview member is not yet applied");
    req(!reviewMember.reviewRejected, "No reviewer rejection exists yet");
    req(reviewMember.applyAllowed && reviewMember.applyBlockCode
            == ManifestApplyVerificationGate::kEligibleCode,
        "The per-member Apply decision must be the real, fresh AG-16 evaluation");
    req(reviewMember.changes.size() == 1
            && reviewMember.changes.front().relativePath == "qml/App.qml",
        "The member's own file changes must be projected for human review");
}

// A genuine verified content failure has no Apply authority, but its Work
// Ledger still contains the exact reason the user needs for the next edit.
// The read model must project that evidence in the handoff attempt history;
// it must not collapse it to the synthetic attempt Run's generic error.
void testViewForRunProjectsFailedAttemptVerificationDiagnostics()
{
    Fixture fx;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    fx.commandRunner.queuedOutcomes = {
        cleanOutcome, warningOutcome, // legacy materialize baseline/candidate
        warningOutcome, cleanOutcome  // current reverify candidate/baseline delta
    };

    const auto materialized = fx.manualHandoffService->materialize(fx.source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not edit the candidate file");
        appQml << "Item { property string stillUnqualified: missingValue }";
    }

    const auto imported = fx.manualHandoffService->importAndReverify(
        materialized.value().handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Failed,
        "The scripted remaining lint warning must produce a verified content failure");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "The failed attempt must still project");
    const auto& attempt = view.value()->attempts.front();
    req(attempt.resultKind == "verified_content_failure" && !attempt.isAuthoritative,
        "A failed verification must remain non-authoritative durable history");
    req(attempt.hasVerification
            && attempt.proposalSafetyStatus == toString(ExecutionVerificationStatus::Passed)
            && attempt.buildLintTestStatus == toString(ExecutionVerificationStatus::Failed)
            && attempt.acceptanceStatus == toString(ExecutionVerificationStatus::Failed),
        "The failed attempt's own three verification statuses must be projected");
    req(attempt.verificationDiagnostics.size() == 1
            && attempt.verificationDiagnostics.front().find("verification.qml_warning")
                != std::string::npos
            && attempt.verificationDiagnostics.front().find("qml-lint") != std::string::npos,
        "The bounded Work Ledger diagnostic summary must reach the attempt history");
    req(attempt.actionableDiagnostics.size() == 1
            && attempt.actionableDiagnostics.front().find("Failed to import module")
                != std::string::npos,
        "The structured finding detail must reach the actionable diagnostic bucket");
    req(!view.value()->hasFinalVerification && !view.value()->canApply,
        "Displaying failed evidence must never promote it to final Apply authority");
    // Post-P4R.6 human review/Apply hardening: a failed/non-authoritative
    // attempt provides no review/apply authority at all - reviewMembers
    // must stay empty, never projecting the failed attempt's own candidate
    // set as if it were reviewable.
    req(view.value()->reviewMembers.empty(),
        "A failed, non-authoritative attempt must never expose a review/apply surface");
}

// A deterministic pre-verification content rejection (QML-P4R.3.4's own
// RejectedBeforeVerification kind) must project its stable rejection code/
// message and must never be treated as authoritative - Apply eligibility
// stays false with the read model's own explicit "no authoritative attempt"
// reason, never AG-16's kEligibleCode.
void testViewForRunProjectsRejectionWithoutAuthority()
{
    Fixture fx;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    fx.commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = fx.manualHandoffService->materialize(fx.source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    const auto& handoff = materialized.value().handoff;

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "AKIAABCDEFGHIJKLMNOP";
    }

    const auto imported = fx.manualHandoffService->importAndReverify(handoff.id);
    req(!imported.success() && imported.error()->code == "proposal.secret_detected",
        "A genuine secret-scan safety rejection must surface as a failure");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "A handoff with a rejected attempt must "
        "still project a view");
    const auto& v = *view.value();
    req(v.attempts.size() == 1, "The rejected reservation must remain durable audit history");
    const auto& attemptView = v.attempts.front();
    req(attemptView.resultKind == "rejected_before_verification",
        "A deterministic content/policy rejection must project its real typed kind");
    req(attemptView.rejectionCode == "proposal.secret_detected",
        "The real rejection code must be projected verbatim");
    req(!attemptView.rejectionMessage.empty(), "The real rejection message must be projected too");
    req(!attemptView.isAuthoritative, "A RejectedBeforeVerification attempt never carries "
        "Apply-authority");
    req(!v.hasFinalVerification && !v.canApply,
        "No authoritative attempt exists - final verification and Apply eligibility stay empty/false");
    req(v.applyReasonCode == "manual_handoff.no_authoritative_attempt",
        "The read model's own explicit reason must be used, never AG-16's eligible code");
}

// QML-P4R.5 hardening: the real content-identity algorithm
// ManifestApplyVerificationGate itself recomputes (ManifestProposalContentHash.hpp),
// used here to hand-build a durable Work Ledger verification whose recorded
// content hashes genuinely match a real, already-created proposal - so the
// synthetic authority fixtures below exercise the SAME real resolver/gate
// code every production handoff goes through, never a hand-waved stand-in.
std::string computeRealProposalContentHash(Fixture& fx, const ChangeSetId& proposalId)
{
    const auto changes = fx.proposals->listChanges(proposalId);
    req(changes.success(), "Could not list changes for content hash computation");
    std::vector<ManifestProposalContentHashChangeInput> inputs;
    for (const auto& change : changes.value()) {
        ManifestProposalContentHashChangeInput input;
        input.operation = change.operation;
        input.relativePath = change.relativePath;
        input.sourcePath = change.sourcePath;
        if (change.operation == FileChangeOperation::Create
                || change.operation == FileChangeOperation::Modify) {
            const auto content = fx.proposals->readProposedContent(proposalId, change.changeId);
            req(content.success(), "Could not read real proposed content for hashing");
            input.contentSha256 = sha256Hex(content.value());
        }
        inputs.push_back(std::move(input));
    }
    return computeManifestProposalContentHash(proposalId, inputs);
}

// A frozen, single-real-member handoff row only - deliberately built by
// direct repository insertion (never materialize()) so the tests below can
// exercise ONLY the shared authority resolver/read-model projection, without
// also depending on materialize()'s own attribution/scope machinery. The
// candidate directory is deliberately left nonexistent - probeCandidateHealth
// itself never errors on that (see its own doc comment), it just reads back
// candidateHealth == "missing", which none of these tests care about.
ManifestVerificationManualHandoff seedSingleMemberHandoffOnly(
    Fixture& fx, const std::string& handoffId)
{
    const auto now = std::chrono::system_clock::now();
    ManifestVerificationManualHandoff handoff;
    handoff.id = handoffId;
    handoff.originManifestRunId = fx.source.id;
    handoff.verificationAnchorRunId = fx.source.id;
    handoff.sourceProjectId = fx.project.id;
    handoff.storageKey = "no-candidate-" + handoffId;
    handoff.workspaceIdentity = fx.project.id;
    handoff.baselineFingerprint = sha256Hex("baseline-" + handoffId);
    handoff.sourceOverlayFingerprint = sha256Hex("overlay-" + handoffId);
    handoff.frozenMembers = {{fx.componentProposalId, fx.source.id,
        computeRealProposalContentHash(fx, fx.componentProposalId), fx.componentProposalId}};
    handoff.allowedPaths = {{"qml/App.qml", fx.componentProposalId, "component"}};
    handoff.originalSnapshot.schemaVersion = std::string(kWorkspaceBaselineSchemaVersion);
    handoff.originalSnapshot.fingerprint = sha256Hex("snapshot-" + handoffId);
    handoff.createdAt = now;
    req(fx.manualHandoffs.insertHandoff(handoff).success(), "Could not seed the synthetic handoff");
    return handoff;
}

// Seeds one genuine Completed-attempt Run + Work Ledger entry (both real
// repository rows, never fabricated in-memory-only structs the resolver
// never actually reads) backing handoff's single frozen member. Returns the
// new attempt Run's id.
RunId seedCompletedSingleMemberAttempt(
    Fixture& fx, const ManifestVerificationManualHandoff& handoff, const std::string& suffix,
    std::uint64_t ordinal)
{
    const auto now = std::chrono::system_clock::now();
    Run attemptRun;
    attemptRun.id = "attempt-run-" + suffix;
    attemptRun.projectId = fx.project.id;
    attemptRun.title = "Manual handoff attempt";
    attemptRun.task = "verify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = fx.source.id;
    attemptRun.status = RunStatus::Completed;
    attemptRun.createdAt = now;
    req(fx.runs.insertRun(attemptRun).success(), "Could not seed an attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "attempt-" + suffix;
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = fx.source.id;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = ordinal;
    attempt.editedSnapshotFingerprint = sha256Hex("edited-" + suffix);
    attempt.createdAt = now;
    req(fx.manualHandoffs.insertAttempt(attempt).success(), "Could not seed an attempt");

    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = fx.project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {fx.componentProposalId};
    verification.orderedProposalContentHashes =
        {computeRealProposalContentHash(fx, fx.componentProposalId)};
    verification.overlayFingerprint = sha256Hex("overlay-verify-" + suffix);
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRun.id;
    entry.projectId = fx.project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = verification;
    entry.proposalIds = {fx.componentProposalId};
    entry.baselineFingerprint = handoff.baselineFingerprint;
    entry.createdAt = now;
    req(fx.ledger.insertEntry(entry).success(), "Could not seed a Work Ledger entry");
    return attemptRun.id;
}

// QML-P4R.5 hardening (blocker 1): a Work Ledger entry that reached
// Completed is, on its own, already sufficient to make an attempt
// authoritative - the crash window between that commit and the typed
// VerifiedSuccess projection's own commit must never leave the read model
// unable to resolve authority (and never independently invent a fallback
// rule of its own - it must be resolveManualHandoffApplyAuthority itself,
// the exact same function AG-16 uses, that resolves it).
void testViewForRunResolvesAuthorityFromLedgerAloneWithoutTypedProjection()
{
    Fixture fx;
    const auto handoff = seedSingleMemberHandoffOnly(fx, "handoff-ledger-only");
    const auto attemptRunId = seedCompletedSingleMemberAttempt(fx, handoff, "ledger-only", 1);

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(),
        "A Completed Work Ledger entry alone (no typed projection yet) must still resolve "
        "authority, never hard-fail the whole view");
    const auto& v = *view.value();
    req(v.attempts.size() == 1, "Exactly one attempt must be projected");
    req(v.attempts.front().attemptRunId == attemptRunId, "The real attempt must be projected");
    req(v.attempts.front().resultKind.empty(),
        "With no typed result row at all, resultKind must stay empty - never guessed");
    req(v.attempts.front().isAuthoritative,
        "The Work Ledger's own Completed entry, not the (absent) typed projection, is what "
        "makes this attempt authoritative");
    req(v.hasFinalVerification
            && v.finalProposalSafetyStatus == toString(ExecutionVerificationStatus::Passed)
            && v.finalBuildLintTestStatus == toString(ExecutionVerificationStatus::Passed)
            && v.finalAcceptanceStatus == toString(ExecutionVerificationStatus::Passed),
        "The final verification summary must be read from the Work Ledger entry directly");
}

// QML-P4R.5 hardening (blocker 1): a typed VerifiedSuccess row with NO
// backing Work Ledger entry is a genuine durable-storage integrity conflict
// (see resolveManualHandoffApplyAuthority's own doc comment) - the read
// model must fail closed, exactly like AG-16 itself would, never silently
// trust the typed projection alone.
void testViewForRunFailsClosedOnTypedResultConflictingWithMissingLedger()
{
    Fixture fx;
    const auto handoff = seedSingleMemberHandoffOnly(fx, "handoff-conflict");
    const auto now = std::chrono::system_clock::now();

    Run attemptRun;
    attemptRun.id = "attempt-run-conflict";
    attemptRun.projectId = fx.project.id;
    attemptRun.title = "Manual handoff attempt";
    attemptRun.task = "verify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = fx.source.id;
    attemptRun.status = RunStatus::Completed;
    attemptRun.createdAt = now;
    req(fx.runs.insertRun(attemptRun).success(), "Could not seed the attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "attempt-conflict-1";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = fx.source.id;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = sha256Hex("edited-conflict");
    attempt.createdAt = now;
    req(fx.manualHandoffs.insertAttempt(attempt).success(), "Could not seed the attempt");

    ManifestVerificationManualHandoffAttemptResult typedResult;
    typedResult.id = "attempt-result-conflict";
    typedResult.attemptId = attempt.id;
    typedResult.attemptRunId = attemptRun.id;
    typedResult.kind = ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess;
    typedResult.recordedAt = now;
    req(fx.manualHandoffs.insertAttemptResult(typedResult).success(),
        "Could not seed the conflicting typed result");
    // Deliberately no Work Ledger entry for attemptRun.id.

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(!view.success(),
        "A typed VerifiedSuccess result with no backing Work Ledger entry must fail closed, "
        "never silently trusted");
    req(view.error()->code == "manifest_apply.manual_handoff_authority_corrupt",
        "The real shared resolver's own stable corruption code must surface, not a read-model "
        "guess");
}

// QML-P4R.4/P4R.5: two genuinely Completed attempts must never both carry
// authority - only the higher-ordinal one does, and the resolver never even
// consults the older one once the newer match is found (see
// resolveManualHandoffApplyAuthority's own doc comment).
void testViewForRunNewerCompletedAttemptSupersedesOlderAsAuthority()
{
    Fixture fx;
    const auto handoff = seedSingleMemberHandoffOnly(fx, "handoff-supersede");
    const auto olderAttemptRunId = seedCompletedSingleMemberAttempt(fx, handoff, "supersede-older", 1);
    const auto newerAttemptRunId = seedCompletedSingleMemberAttempt(fx, handoff, "supersede-newer", 2);

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(),
        "Two genuine Completed attempts must still project");
    const auto& v = *view.value();
    req(v.attempts.size() == 2, "Both attempts must remain durable audit history");
    for (const auto& attemptView : v.attempts) {
        const bool isNewer = attemptView.attemptRunId == newerAttemptRunId;
        req(attemptView.isAuthoritative == isNewer,
            "Only the higher-ordinal Completed attempt may carry Apply-authority, never an "
            "older one even though it is also genuinely Completed");
    }
    (void)olderAttemptRunId;
}

// QML-P4R.5 hardening (blocker 2): a handoff's verified set can legitimately
// contain a member that was already Applied (via a normal one-at-a-time
// apply sequence) alongside one still ReadyForReview. canApply must reflect
// the SECOND, still-eligible member - never just proposalIds.front(), which
// would wrongly read this whole handoff as no-longer-Apply-eligible.
void testViewForRunCanApplyIteratesAllMembersNotJustFirst()
{
    Fixture fx;
    const auto now = std::chrono::system_clock::now();

    const auto proposalAResult = fx.proposals->createFromModelOutput(makeCreateFileRequest(
        fx.workspace.path, fx.source.id, fx.project.id, "chg-a", "qml/A.qml", "Item {}"));
    req(proposalAResult.success()
            && proposalAResult.value().state == ChangeSetState::ReadyForReview,
        "Could not create real proposal A");
    const auto proposalBResult = fx.proposals->createFromModelOutput(makeCreateFileRequest(
        fx.workspace.path, fx.source.id, fx.project.id, "chg-b", "qml/B.qml", "Item {}"));
    req(proposalBResult.success()
            && proposalBResult.value().state == ChangeSetState::ReadyForReview,
        "Could not create real proposal B");
    const ChangeSetId proposalA = proposalAResult.value().changeSetId;
    const ChangeSetId proposalB = proposalBResult.value().changeSetId;
    const std::string hashA = computeRealProposalContentHash(fx, proposalA);
    const std::string hashB = computeRealProposalContentHash(fx, proposalB);

    ManifestVerificationManualHandoff handoff;
    handoff.id = "handoff-multi";
    handoff.originManifestRunId = fx.source.id;
    handoff.verificationAnchorRunId = fx.source.id;
    handoff.sourceProjectId = fx.project.id;
    handoff.storageKey = "no-candidate-multi";
    handoff.workspaceIdentity = fx.project.id;
    handoff.baselineFingerprint = sha256Hex("baseline-multi");
    handoff.sourceOverlayFingerprint = sha256Hex("overlay-multi");
    handoff.frozenMembers = {
        {proposalA, fx.source.id, hashA, proposalA},
        {proposalB, fx.source.id, hashB, proposalB},
    };
    handoff.allowedPaths = {
        {"qml/A.qml", proposalA, "task-a"},
        {"qml/B.qml", proposalB, "task-b"},
    };
    handoff.originalSnapshot.schemaVersion = std::string(kWorkspaceBaselineSchemaVersion);
    handoff.originalSnapshot.fingerprint = sha256Hex("snapshot-multi");
    handoff.createdAt = now;
    req(fx.manualHandoffs.insertHandoff(handoff).success(), "Could not seed the synthetic handoff");

    Run attemptRun;
    attemptRun.id = "attempt-run-multi";
    attemptRun.projectId = fx.project.id;
    attemptRun.title = "Manual handoff attempt";
    attemptRun.task = "verify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = fx.source.id;
    attemptRun.status = RunStatus::Completed;
    attemptRun.createdAt = now;
    req(fx.runs.insertRun(attemptRun).success(), "Could not seed the synthetic attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "attempt-multi-1";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = fx.source.id;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = sha256Hex("edited-multi");
    attempt.createdAt = now;
    req(fx.manualHandoffs.insertAttempt(attempt).success(), "Could not seed the synthetic attempt");

    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = fx.project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {proposalA, proposalB};
    verification.orderedProposalContentHashes = {hashA, hashB};
    verification.overlayFingerprint = sha256Hex("overlay-verify-multi");
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRun.id;
    entry.projectId = fx.project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = verification;
    entry.proposalIds = {proposalA, proposalB};
    entry.baselineFingerprint = handoff.baselineFingerprint;
    entry.taskDispositions = {
        {"task-a", "succeeded", proposalA, std::nullopt},
        {"task-b", "succeeded", proposalB, std::nullopt},
    };
    entry.createdAt = now;
    req(fx.ledger.insertEntry(entry).success(), "Could not seed the synthetic Work Ledger entry");

    req(fx.proposalRepository.updateChangeSetState(proposalA, ChangeSetState::Applied).success(),
        "Could not mark the first member Applied");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "A synthetic multi-member handoff must project");
    const auto& v = *view.value();
    req(v.attempts.size() == 1 && v.attempts.front().isAuthoritative,
        "The synthetic attempt must be the handoff's sole authority");
    req(v.hasFinalVerification, "A Completed ledger entry must carry a final verification summary");
    req(v.canApply,
        "canApply must be true via the SECOND, still-ReadyForReview member even though the "
        "FIRST proposalIds entry is already Applied - proving the fix iterates every member "
        "instead of only proposalIds.front()");
    req(v.applyReasonCode == ManifestApplyVerificationGate::kEligibleCode,
        "The surfaced reason must be AG-16's own real eligible code for the eligible member");

    // Post-P4R.6 human review/Apply hardening: an already-Applied member
    // must remain VISIBLE (never disappear from the review surface) while a
    // still-ReadyForReview sibling is correctly offered for Apply - proving
    // "already Applied members remain visible, but repeat Apply is not
    // offered" and "the second member is correctly recognized as eligible"
    // together, on the exact same handoff.
    req(v.reviewMembers.size() == 2, "Both members of the exact verified set must be projected");
    const auto& memberA = v.reviewMembers[0];
    const auto& memberB = v.reviewMembers[1];
    req(memberA.proposalId == proposalA && memberB.proposalId == proposalB,
        "Review members must preserve the ledger's own ordered proposal set");
    req(memberA.alreadyApplied && memberA.state == toString(ChangeSetState::Applied),
        "The first member's real Applied state must be projected, not hidden");
    req(!memberA.applyAllowed,
        "An already-Applied member must never be re-offered for Apply (ReadyForReview only)");
    req(!memberB.alreadyApplied && memberB.state == toString(ChangeSetState::ReadyForReview),
        "The second member's real ReadyForReview state must be projected");
    req(memberB.applyAllowed && memberB.applyBlockCode == ManifestApplyVerificationGate::kEligibleCode,
        "The second, still-eligible member must be independently offered for Apply");
}

// Post-P4R.6 human review/Apply hardening: a durable-storage integrity gap
// (the Run's own recorded Work Ledger proposalIds names a proposal that
// genuinely does not exist) must fail the WHOLE view closed - never silently
// omit the missing member and show a partial, misleadingly-complete review
// list.
void testViewForRunReviewMembersFailClosedOnMissingProposal()
{
    Fixture fx;
    const auto now = std::chrono::system_clock::now();

    const auto proposalResult = fx.proposals->createFromModelOutput(makeCreateFileRequest(
        fx.workspace.path, fx.source.id, fx.project.id, "chg-present", "qml/Present.qml",
        "Item {}"));
    req(proposalResult.success()
            && proposalResult.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real, present proposal");
    const ChangeSetId presentProposal = proposalResult.value().changeSetId;
    const std::string presentHash = computeRealProposalContentHash(fx, presentProposal);
    // Deliberately never inserted into fx.proposalRepository - simulates a
    // durable-storage integrity gap (e.g. a proposal row lost or never
    // written) rather than a normal "not found" a UI could route around.
    const ChangeSetId missingProposal = "chg-does-not-exist";

    ManifestVerificationManualHandoff handoff;
    handoff.id = "handoff-missing-member";
    handoff.originManifestRunId = fx.source.id;
    handoff.verificationAnchorRunId = fx.source.id;
    handoff.sourceProjectId = fx.project.id;
    handoff.storageKey = "no-candidate-missing-member";
    handoff.workspaceIdentity = fx.project.id;
    handoff.baselineFingerprint = sha256Hex("baseline-missing-member");
    handoff.sourceOverlayFingerprint = sha256Hex("overlay-missing-member");
    handoff.frozenMembers = {
        {presentProposal, fx.source.id, presentHash, presentProposal},
        {missingProposal, fx.source.id, sha256Hex("missing"), missingProposal},
    };
    handoff.allowedPaths = {{"qml/Present.qml", presentProposal, "task-present"}};
    handoff.originalSnapshot.schemaVersion = std::string(kWorkspaceBaselineSchemaVersion);
    handoff.originalSnapshot.fingerprint = sha256Hex("snapshot-missing-member");
    handoff.createdAt = now;
    req(fx.manualHandoffs.insertHandoff(handoff).success(),
        "Could not seed the synthetic handoff");

    Run attemptRun;
    attemptRun.id = "attempt-run-missing-member";
    attemptRun.projectId = fx.project.id;
    attemptRun.title = "Manual handoff attempt";
    attemptRun.task = "verify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = fx.source.id;
    attemptRun.status = RunStatus::Completed;
    attemptRun.createdAt = now;
    req(fx.runs.insertRun(attemptRun).success(), "Could not seed the synthetic attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "attempt-missing-member-1";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = fx.source.id;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = sha256Hex("edited-missing-member");
    attempt.createdAt = now;
    req(fx.manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the synthetic attempt");

    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = fx.project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {presentProposal, missingProposal};
    verification.orderedProposalContentHashes = {presentHash, sha256Hex("missing")};
    verification.overlayFingerprint = sha256Hex("overlay-verify-missing-member");
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry entry;
    entry.runId = attemptRun.id;
    entry.projectId = fx.project.id;
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.verification = verification;
    entry.proposalIds = {presentProposal, missingProposal};
    entry.baselineFingerprint = handoff.baselineFingerprint;
    entry.taskDispositions = {
        {"task-present", "succeeded", presentProposal, std::nullopt},
    };
    entry.createdAt = now;
    req(fx.ledger.insertEntry(entry).success(), "Could not seed the synthetic Work Ledger entry");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(!view.success(),
        "A Work Ledger proposalIds entry naming a genuinely missing proposal must fail the "
        "whole view closed, never silently omit the missing member from reviewMembers");
}

// QML-P4R.5 hardening (blocker 3): a real Manifest-workflow Run that never
// reached a Failed terminal status can never admit a manual handoff -
// canCreate must reflect this, never left true by default.
void testViewForRunProjectsCanCreateFalseForIneligibleManifestAnchor()
{
    Fixture fx;
    Run completedRun;
    completedRun.id = "completed-manifest-run";
    completedRun.projectId = fx.project.id;
    completedRun.title = "Completed Manifest Run";
    completedRun.task = "Build feature";
    completedRun.workflowId = kExecutionManifestWorkflowId;
    completedRun.status = RunStatus::Completed;
    completedRun.createdAt = std::chrono::system_clock::now();
    req(fx.runs.insertRun(completedRun).success(), "Could not seed the completed Run");

    const auto view = fx.readModel->viewForRun(completedRun.id);
    req(view.success() && view.value().has_value(),
        "A real Manifest-workflow Run must still project a view even when ineligible");
    const auto& v = *view.value();
    req(!v.hasHandoff, "No handoff exists for this Run");
    req(!v.canCreate, "A Completed (never-failed) Manifest Run can never admit a manual handoff");
    req(!v.createReasonCode.empty() && !v.createReasonMessage.empty(),
        "A false canCreate must always carry a real, non-empty reason");
}

// QML-P4R.5 hardening (blocker 3): a continuation-child anchor with no
// durable continuation input naming it can never resolve its origin
// Manifest Run - canCreate must reflect this real, structural denial.
void testViewForRunProjectsCanCreateFalseForUnresolvableContinuationChildAnchor()
{
    Fixture fx;
    Run orphanChild;
    orphanChild.id = "orphan-continuation-child";
    orphanChild.projectId = fx.project.id;
    orphanChild.title = "Orphan continuation child";
    orphanChild.task = "Repair";
    orphanChild.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    orphanChild.parentRunId = fx.source.id;
    orphanChild.status = RunStatus::Prepared;
    orphanChild.createdAt = std::chrono::system_clock::now();
    req(fx.runs.insertRun(orphanChild).success(), "Could not seed the orphan continuation child Run");

    const auto view = fx.readModel->viewForRun(orphanChild.id);
    req(view.success() && view.value().has_value(),
        "A real continuation-workflow Run must still project a view even when unresolvable");
    const auto& v = *view.value();
    req(!v.hasHandoff, "No handoff exists for this Run");
    req(!v.canCreate, "A continuation child with no durable continuation input naming it can "
        "never resolve an origin Manifest Run");
    req(v.createReasonCode == "manual_handoff.origin_unresolvable",
        "The real origin-resolution denial code must be surfaced, never a read-model guess");
}

// QML-P4R.5: a handoff anchored on a paid continuation child must project
// the anchor and origin as the two distinct Run identities they really are -
// never conflated into one, even though they happen to share the same
// current status here.
void testViewForRunProjectsDistinctOriginAndAnchorForMixedContinuationChild()
{
    Fixture fx;
    const auto now = std::chrono::system_clock::now();

    Run child;
    child.id = "continuation-child-mixed";
    child.projectId = fx.project.id;
    child.title = "Paid continuation repair child";
    child.task = "Repair qml lint";
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.parentRunId = fx.source.id;
    child.status = RunStatus::Failed;
    child.createdAt = now;
    req(fx.runs.insertRun(child).success(), "Could not seed the continuation child Run");

    ManifestVerificationManualHandoff handoff;
    handoff.id = "handoff-mixed";
    handoff.originManifestRunId = fx.source.id;
    handoff.verificationAnchorRunId = child.id;
    handoff.sourceProjectId = fx.project.id;
    handoff.storageKey = "no-candidate-mixed";
    handoff.workspaceIdentity = fx.project.id;
    handoff.baselineFingerprint = sha256Hex("baseline-mixed");
    handoff.sourceOverlayFingerprint = sha256Hex("overlay-mixed");
    handoff.frozenMembers = {{fx.componentProposalId, fx.source.id,
        computeRealProposalContentHash(fx, fx.componentProposalId), fx.componentProposalId}};
    handoff.allowedPaths = {{"qml/App.qml", fx.componentProposalId, "component"}};
    handoff.originalSnapshot.schemaVersion = std::string(kWorkspaceBaselineSchemaVersion);
    handoff.originalSnapshot.fingerprint = sha256Hex("snapshot-mixed");
    handoff.createdAt = now;
    req(fx.manualHandoffs.insertHandoff(handoff).success(), "Could not seed the mixed-origin handoff");

    const auto view = fx.readModel->viewForRun(child.id);
    req(view.success() && view.value().has_value(),
        "A materialized handoff anchored on a paid continuation child must still project");
    const auto& v = *view.value();
    req(v.verificationAnchorRunId == child.id && v.originManifestRunId == fx.source.id,
        "The anchor and origin identities must be projected as the two distinct Runs they "
        "really are - never conflated");
    req(v.verificationAnchorStatus == toString(RunStatus::Failed),
        "The anchor's own status (the continuation child) must be projected");
    req(v.originManifestStatus == toString(RunStatus::Failed),
        "The origin's own status (the source Manifest Run) must be projected separately");
}

void testRunResolutionBatchTracksUnresolvedAwaitingPartialAndFullyApplied()
{
    Fixture fx;

    Run completed = fx.source;
    completed.id = "completed-nonfailed-run";
    completed.status = RunStatus::Completed;
    req(fx.runs.insertRun(completed).success(), "Could not seed the completed Run");

    auto resolutions = fx.readModel->resolutionsForRuns({fx.source, completed});
    req(resolutions.success() && resolutions.value().size() == 2,
        "The batch resolution projection must return one row per input Run");
    req(resolutions.value()[0].resolution
            == ManifestVerificationManualHandoffResolution::Unresolved,
        "A Failed Run with no authoritative manual handoff must be unresolved");
    req(resolutions.value()[1].resolution
            == ManifestVerificationManualHandoffResolution::NotApplicable,
        "A non-Failed Run must not acquire a manual-resolution state");

    const auto secondProposal = fx.proposals->createFromModelOutput(makeCreateFileRequest(
        fx.workspace.path, fx.source.id, fx.project.id, "chg-second", "qml/Second.qml",
        "Item {}"));
    req(secondProposal.success()
            && secondProposal.value().state == ChangeSetState::ReadyForReview,
        "Could not create the second authoritative member");
    const ChangeSetId secondProposalId = secondProposal.value().changeSetId;

    const auto now = std::chrono::system_clock::now();
    ManifestVerificationManualHandoff handoff;
    handoff.id = "handoff-resolution-batch";
    handoff.originManifestRunId = fx.source.id;
    handoff.verificationAnchorRunId = fx.source.id;
    handoff.sourceProjectId = fx.project.id;
    handoff.storageKey = "no-candidate-resolution-batch";
    handoff.workspaceIdentity = fx.project.id;
    handoff.baselineFingerprint = sha256Hex("baseline-resolution-batch");
    handoff.sourceOverlayFingerprint = sha256Hex("overlay-resolution-batch");
    handoff.frozenMembers = {
        {fx.componentProposalId, fx.source.id,
            computeRealProposalContentHash(fx, fx.componentProposalId),
            fx.componentProposalId},
        {secondProposalId, fx.source.id,
            computeRealProposalContentHash(fx, secondProposalId), secondProposalId},
    };
    handoff.allowedPaths = {
        {"qml/App.qml", fx.componentProposalId, "component"},
        {"qml/Second.qml", secondProposalId, "component"},
    };
    handoff.originalSnapshot.schemaVersion = std::string(kWorkspaceBaselineSchemaVersion);
    handoff.originalSnapshot.fingerprint = sha256Hex("snapshot-resolution-batch");
    handoff.createdAt = now;
    req(fx.manualHandoffs.insertHandoff(handoff).success(),
        "Could not seed the batch-resolution handoff");

    Run attemptRun;
    attemptRun.id = "attempt-run-resolution-batch";
    attemptRun.projectId = fx.project.id;
    attemptRun.title = "Manual handoff attempt";
    attemptRun.task = "verify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.parentRunId = fx.source.id;
    attemptRun.status = RunStatus::Completed;
    attemptRun.createdAt = now;
    req(fx.runs.insertRun(attemptRun).success(), "Could not seed the batch attempt Run");

    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "attempt-resolution-batch";
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = fx.source.id;
    attempt.attemptRunId = attemptRun.id;
    attempt.attemptOrdinal = 1;
    attempt.editedSnapshotFingerprint = sha256Hex("edited-resolution-batch");
    attempt.createdAt = now;
    req(fx.manualHandoffs.insertAttempt(attempt).success(),
        "Could not seed the batch attempt");

    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = fx.project.id;
    verification.baselineFingerprint = handoff.baselineFingerprint;
    verification.orderedProposalIds = {fx.componentProposalId, secondProposalId};
    verification.orderedProposalContentHashes = {
        computeRealProposalContentHash(fx, fx.componentProposalId),
        computeRealProposalContentHash(fx, secondProposalId),
    };
    verification.overlayFingerprint = sha256Hex("verified-overlay-resolution-batch");
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;

    ManifestWorkLedgerEntry ledgerEntry;
    ledgerEntry.runId = attemptRun.id;
    ledgerEntry.projectId = fx.project.id;
    ledgerEntry.terminalStatus = RunStatus::Completed;
    ledgerEntry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    ledgerEntry.manifestSchemaVersion = "execution-manifest.v1";
    ledgerEntry.verification = verification;
    ledgerEntry.proposalIds = {fx.componentProposalId, secondProposalId};
    ledgerEntry.baselineFingerprint = handoff.baselineFingerprint;
    ledgerEntry.createdAt = now;
    req(fx.ledger.insertEntry(ledgerEntry).success(),
        "Could not seed the batch authority ledger");

    resolutions = fx.readModel->resolutionsForRuns({fx.source});
    req(resolutions.success() && resolutions.value().front().resolution
            == ManifestVerificationManualHandoffResolution::VerifiedAwaitingApply,
        "An authoritative successful set with zero Applied members must await Apply");

    req(fx.proposalRepository.updateChangeSetState(
            fx.componentProposalId, ChangeSetState::Applied).success(),
        "Could not mark the first member Applied");
    resolutions = fx.readModel->resolutionsForRuns({fx.source});
    req(resolutions.success() && resolutions.value().front().resolution
            == ManifestVerificationManualHandoffResolution::PartiallyApplied,
        "A multi-member authoritative set with only one Applied member must be partial");

    req(fx.proposalRepository.updateChangeSetState(
            secondProposalId, ChangeSetState::Applied).success(),
        "Could not mark the second member Applied");
    resolutions = fx.readModel->resolutionsForRuns({fx.source});
    req(resolutions.success() && resolutions.value().front().resolution
            == ManifestVerificationManualHandoffResolution::ResolvedManuallyApplied,
        "Only the exact authoritative set being fully Applied may resolve the Failed Run");
}

// Provider-free Manual Handoff hardening: the view must directly forward
// inspectCandidateEdits()'s own classification - an allowed live edit
// projects as editableChangedRelativePaths, and an unsupported new path
// projects as resetRequiredRelativePaths - without QML ever deriving the
// diff itself.
void testViewForRunProjectsEditableAndResetRequiredCandidatePaths()
{
    Fixture fx;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    fx.commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};

    const auto materialized = fx.manualHandoffService->materialize(fx.source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not edit the candidate file");
        appQml << "Item { anchors.fill: parent }";
    }
    {
        std::ofstream stray(materialized.value().candidateRoot / "qml" / "Stray.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(stray), "Could not create the new candidate file");
        stray << "Item {}";
    }

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "A materialized handoff must still project");
    req(view.value()->editableChangedRelativePaths.size() == 1
            && view.value()->editableChangedRelativePaths.front() == "qml/App.qml",
        "The allowed live edit must project as an editable changed path");
    req(view.value()->resetRequiredRelativePaths.size() == 1
            && view.value()->resetRequiredRelativePaths.front() == "qml/Stray.qml",
        "The unsupported new path must project as a reset-required path, never silently ignored");
}

// A structured finding on the one editable allowlisted path must be
// projected as actionable, a structured finding on a path the SAME task
// owns but which carries no allowlisted finding of its own (mirroring
// ManifestVerificationContinuationServiceTests.cpp's own
// MultiFilePredecessorFixture shape) as outside-scope context only, and the
// two lists must never overlap or duplicate the same finding. This needs
// its own two-file fixture, distinct from the shared single-file Fixture
// above: a finding on a path the frozen Manifest does not own at all is a
// materialize()-time attribution_outside_manifest failure, not an
// outside-scope presentation case.
void testViewForRunSplitsStructuredFindingsIntoActionableAndOutsideScope()
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
    FakeProposalReviewRepository proposalReviewRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ProposalReviewService reviewService(proposalRepository, proposalReviewRepository);
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
        proposals, ledger, runs, nullptr, &manifests, &continuations, &manualHandoffs);
    ManifestVerificationManualHandoffReadModelService readModel(
        runs, ledger, manualHandoffs, manualHandoffService, applyGate, proposals, reviewService);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Split actionable/outside-scope";
    project.rootPath = workspace.path;
    project.status = ProjectStatus::Active;
    project.createdAt = now;
    project.updatedAt = now;
    req(projects.insert(project).success(), "Could not seed project");

    fs::create_directories(workspace.path / "qml");
    {
        std::ofstream a(workspace.path / "qml" / "A.qml", std::ios::binary | std::ios::trunc);
        a << "original-a";
    }
    {
        std::ofstream b(workspace.path / "qml" / "B.qml", std::ios::binary | std::ios::trunc);
        b << "original-b";
    }

    Run source;
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
        {"qml/a.qml", sha256Hex("original-a")}, {"qml/b.qml", sha256Hex("original-b")}};
    predecessorRequest.rawModelOutput = R"({"schemaVersion":"proposal-submission.v2",)"
        R"("summary":"Fix qml lint","changes":[)"
        R"({"change_id":"fix-a","operation":"modify","relative_path":"qml/A.qml",)"
        R"("source_path":null,"proposed_content":"fixed-a"},)"
        R"({"change_id":"fix-b","operation":"modify","relative_path":"qml/B.qml",)"
        R"("source_path":null,"proposed_content":"fixed-b"}]})";
    const auto predecessor = proposals.createFromModelOutput(predecessorRequest);
    req(predecessor.success() && predecessor.value().state == ChangeSetState::ReadyForReview,
        "Could not create the real two-file predecessor proposal");
    const ChangeSetId predecessorId = predecessor.value().changeSetId;

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    ExecutionManifestTask component;
    component.id = "component";
    component.ownedPaths = {"qml/A.qml", "qml/B.qml"};
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

    const auto materialized = manualHandoffService.materialize(source.id);
    req(materialized.success(), "A fully eligible two-file source Run must materialize successfully");
    req(materialized.value().handoff.allowedPaths.size() == 1
            && materialized.value().handoff.allowedPaths.front().relativePath == "qml/A.qml",
        "Only A.qml (the file with a remaining owner finding) must be editable");

    {
        std::ofstream editedA(materialized.value().candidateRoot / "qml" / "A.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(editedA), "Could not edit the candidate file");
        editedA << "still-unqualified-a";
    }

    ManifestVerificationCommandOutcome mixedOutcome;
    mixedOutcome.started = true;
    mixedOutcome.exitCode = 0;
    mixedOutcome.boundedOutput =
        "Warning: <candidate>/qml/A.qml:1:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/B.qml:2:1: Pre-existing warning [unqualified]\n"
        "Warning: module graph:1:1: Tool-level warning without a safe candidate path [import]\n";
    commandRunner.queuedOutcomes = {mixedOutcome};

    const auto imported = manualHandoffService.importAndReverify(materialized.value().handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Failed,
        "The scripted mixed lint warnings must produce a verified content failure");

    const auto view = readModel.viewForRun(source.id);
    req(view.success() && view.value().has_value(), "The failed attempt must still project");
    const auto& attempt = view.value()->attempts.front();
    req(view.value()->initialScopeDiagnostics.size() == 1
            && view.value()->initialScopeDiagnostics.front().find("qml/A.qml")
                != std::string::npos,
        "The initial (pre-Reverify) findings come from the anchor Run only, never from a later "
        "attempt's other-scope warnings");
    req(attempt.actionableDiagnostics.size() == 1
            && attempt.actionableDiagnostics.front().find("qml/A.qml") != std::string::npos,
        "The finding on the editable allowlisted path must be actionable, with its real on-disk "
        "case preserved for display");
    req(attempt.outsideScopeDiagnostics.size() == 1
            && attempt.outsideScopeDiagnostics.front().find("qml/B.qml") != std::string::npos,
        "The finding on the task-owned but non-allowlisted path must be outside-scope context only");
    req(attempt.generalDiagnostics.size() == 1
            && attempt.generalDiagnostics.front().find("Tool-level warning") != std::string::npos
            && attempt.generalDiagnostics.front().find("module graph") == std::string::npos,
        "A mixed diagnostic must preserve its unmappable finding exactly once without duplicating "
        "the whole parent diagnostic or guessing an unsafe path");
    req(attempt.hasVerification
            && attempt.buildLintTestStatus == toString(ExecutionVerificationStatus::Failed),
        "Splitting findings into presentation buckets must never change the raw verification status");
    req(!view.value()->canApply,
        "Splitting findings into presentation buckets must never change real AG-16 Apply authority");
}

// Post-Stage-3 QML-lint hardening: an "integration" task with no .qml
// ownedPaths of its own (only the project-wide qml-lint requirement) takes
// the shared baseline-aware delta branch inside
// ManifestProposalSetVerifier::verify() - see qmlLintBaselineDeltaCheck().
// One reverify attempt whose candidate carries all three provenances at
// once (unchanged debt, a genuinely new in-scope finding, and a genuinely
// new unowned finding) must project into three DISTINCT, non-overlapping
// new-segmented buckets, never the single undifferentiated command-excerpt
// blob a raw baseline/candidate diff would otherwise show.
void testViewForRunProjectsBaselineAwareQmlLintDeltaIntoSegmentedBuckets()
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
    FakeProposalReviewRepository proposalReviewRepository;
    auto storeResult = FileChangeProposalStore::open(proposalStore.path);
    req(storeResult.success(), "Could not open the real proposal store");
    ChangeProposalService proposals(proposalRepository, *storeResult.value());
    ProposalReviewService reviewService(proposalRepository, proposalReviewRepository);
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
        proposals, ledger, runs, nullptr, &manifests, &continuations, &manualHandoffs);
    ManifestVerificationManualHandoffReadModelService readModel(
        runs, ledger, manualHandoffs, manualHandoffService, applyGate, proposals, reviewService);

    const auto now = std::chrono::system_clock::now();
    Project project;
    project.id = "project-1";
    project.name = "Baseline-aware delta segmented buckets";
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

    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not open the candidate file for editing");
        appQml << "Item { anchors.centerIn: parent }";
    }

    // The candidate probe carries all three provenances at once: unchanged
    // Other.qml debt, a genuinely new App.qml finding the component task
    // owns, and a genuinely new Stray.qml finding no frozen task owns. The
    // baseline probe proves Other.qml alone predates this candidate.
    ManifestVerificationCommandOutcome mixedCandidate;
    mixedCandidate.started = true;
    mixedCandidate.exitCode = 0;
    mixedCandidate.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/App.qml:2:1: Still unqualified [unqualified]\n"
        "Warning: <candidate>/qml/Stray.qml:2:1: Unqualified access [unqualified]\n";
    ManifestVerificationCommandOutcome mixedBaseline;
    mixedBaseline.started = true;
    mixedBaseline.exitCode = 0;
    mixedBaseline.boundedOutput =
        "Warning: <candidate>/qml/Other.qml:9:1: Unqualified access [unqualified]\n";
    commandRunner.queuedOutcomes = {mixedCandidate, mixedBaseline};

    const auto imported = manualHandoffService.importAndReverify(handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Failed,
        "A mixed set with a genuinely new finding must still fail closed");

    const auto view = readModel.viewForRun(source.id);
    req(view.success() && view.value().has_value(), "The failed attempt must still project");
    const auto& attempt = view.value()->attempts.front();

    req(attempt.newActionableDiagnostics.size() == 1
            && attempt.newActionableDiagnostics.front().find("qml/App.qml") != std::string::npos,
        "The genuinely new finding on the component-owned path must project as new-actionable");
    req(attempt.newOutsideManifestOrContextDiagnostics.size() == 1
            && attempt.newOutsideManifestOrContextDiagnostics.front().find("qml/Stray.qml")
                   != std::string::npos,
        "The genuinely new finding on no frozen task's path must project as new-outside-Manifest");
    req(attempt.unchangedBaselineDebtDiagnostics.size() == 1
            && attempt.unchangedBaselineDebtDiagnostics.front().find("qml/Other.qml")
                   != std::string::npos,
        "The unchanged, proven pre-existing finding must project as baseline debt, never as a new "
        "finding of either kind");

    // Every mapped finding lands in exactly one of the three new buckets -
    // never duplicated across them.
    req(attempt.newActionableDiagnostics.size() + attempt.newOutsideManifestOrContextDiagnostics.size()
                + attempt.unchangedBaselineDebtDiagnostics.size()
            == 3,
        "The three new segmented buckets together must account for every mapped finding exactly "
        "once, with no duplication and no finding left unclassified");
}

// A diagnostic the Backend cannot safely map to any candidate-relative path
// (no structured finding matched the expected shape) must never be guessed
// into actionable or outsideScope - it belongs only in generalDiagnostics,
// distinct from the legacy flat verificationDiagnostics fallback.
void testViewForRunProjectsUnmappableDiagnosticAsGeneralOnly()
{
    Fixture fx;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    ManifestVerificationCommandOutcome unstructuredOutcome;
    unstructuredOutcome.started = true;
    unstructuredOutcome.exitCode = 0;
    unstructuredOutcome.boundedOutput = "Warning: qmllint could not resolve the module import graph\n";
    // materialize() itself needs a real structured, attributable finding to
    // admit App.qml as an editable owner finding at all; the unmappable
    // outcome under test is reserved for the later reverify pass below.
    fx.commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome, unstructuredOutcome};

    const auto materialized = fx.manualHandoffService->materialize(fx.source.id);
    req(materialized.success(), "A fully eligible source Run must materialize successfully");
    {
        std::ofstream appQml(materialized.value().candidateRoot / "qml" / "App.qml",
            std::ios::binary | std::ios::trunc);
        req(static_cast<bool>(appQml), "Could not edit the candidate file");
        appQml << "Item { property string stillUnqualified: missingValue }";
    }

    const auto imported = fx.manualHandoffService->importAndReverify(materialized.value().handoff.id);
    req(imported.success() && imported.value().finalStatus == RunStatus::Failed,
        "An unmappable diagnostic must still produce a verified content failure");

    const auto view = fx.readModel->viewForRun(fx.source.id);
    req(view.success() && view.value().has_value(), "The failed attempt must still project");
    const auto& attempt = view.value()->attempts.front();
    req(attempt.actionableDiagnostics.empty() && attempt.outsideScopeDiagnostics.empty(),
        "A diagnostic the Backend cannot safely map to a path must never be guessed into a bucket");
    req(attempt.generalDiagnostics.size() == 1,
        "The unmappable diagnostic must appear exactly once as a general line");
    req(attempt.verificationDiagnostics.size() == 1,
        "The legacy flat fallback list must still contain the diagnostic for backward compatibility");
}

// ---------------------------------------------------------------------------
// Recorded findings before the repair workspace (REMAIN_TO_DO 6a (b)+(c)).
// ---------------------------------------------------------------------------

using Op = FileChangeOperation;

void addProposal(Fixture& fx, const std::string& id,
    const std::vector<std::pair<Op, std::string>>& changes)
{
    ProposedChangeSet cs;
    cs.changeSetId = id;
    cs.runId = fx.source.id;
    cs.projectId = fx.project.id;
    cs.state = ChangeSetState::ReadyForReview;
    int n = 0;
    for (const auto& [op, path] : changes) {
        ProposedFileChange change;
        change.changeId = id + ":" + std::to_string(++n);
        change.operation = op;
        change.relativePath = path;
        if (op == Op::Rename) change.sourcePath = fs::path("old/" + path);
        cs.changes.push_back(std::move(change));
    }
    req(fx.proposalRepository.insertChangeSet(cs).success(), "Could not seed a proposal");
}

ManifestVerificationDiagnosticFinding finding(const std::string& file, int line = 3)
{
    return {"<candidate>/" + file, line, 1, "unqualified", "Unqualified access"};
}

ManifestVerificationDiagnostic diagnostic(const std::string& code,
    std::vector<ManifestVerificationDiagnosticFinding> findings, const std::string& message = "msg")
{
    ManifestVerificationDiagnostic d;
    d.code = code;
    d.message = message;
    d.requirementId = "qml-lint";
    d.structuredFindings = std::move(findings);
    return d;
}

std::optional<ManifestWorkLedgerEntry> ledgerWith(Fixture& fx,
    const std::vector<ChangeSetId>& proposalIds, std::vector<ManifestVerificationDiagnostic> diagnostics)
{
    auto entry = fx.ledger.findEntry(fx.source.id).value();
    req(entry.has_value() && entry->verification.has_value(), "The fixture ledger entry must exist");
    entry->verification->orderedProposalIds = proposalIds;
    entry->verification->diagnostics = std::move(diagnostics);
    return entry;
}

ManifestVerificationRecordedFindingsView recorded(Fixture& fx,
    const std::optional<ManifestWorkLedgerEntry>& entry, bool failed = true,
    const std::unordered_set<std::string>* allowed = nullptr)
{
    return buildManifestVerificationRecordedFindings(failed, entry, *fx.proposals, allowed);
}

const ManifestVerificationRecordedFindingView* rowFor(
    const ManifestVerificationRecordedFindingsView& v, const std::string& path)
{
    for (const auto& row : v.findings) {
        if (row.relativePath == path) return &row;
    }
    return nullptr;
}

constexpr const char* kBlocking = "verification.qml_warning";
constexpr const char* kOutsideManifest = "verification.qml_warning_outside_manifest";

void testRecordedFindingsProveOutsideOnlyWhenNoCreateOrModifyTargetMatches()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}, {Op::Modify, "qml/Changed.qml"},
                           {Op::Rename, "qml/Renamed.qml"}, {Op::Delete, "qml/Deleted.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, {
        finding("qml/Made.qml"), finding("qml/Changed.qml"), finding("qml/Renamed.qml"),
        finding("qml/Deleted.qml"), finding("qml/Elsewhere.qml")})}));
    req(v.classificationErrorCode.empty(), "All proposals were read: no classification error");
    req(rowFor(v, "qml/Made.qml")->authority == "unknown_until_created"
            && rowFor(v, "qml/Changed.qml")->authority == "unknown_until_created",
        "A Create or Modify target may become editable: unknown until the workspace exists");
    req(rowFor(v, "qml/Renamed.qml")->authority == "outside_all_create_modify_targets"
            && rowFor(v, "qml/Deleted.qml")->authority == "outside_all_create_modify_targets"
            && rowFor(v, "qml/Elsewhere.qml")->authority == "outside_all_create_modify_targets",
        "A Rename/Delete-only target and an untouched file can never enter allowedPaths");
    req(v.verdict == "blocked_outside_scope"
            && v.verdictCode == "manual_handoff.blocking_findings_outside_scope"
            && v.outsideScopePaths.size() == 3,
        "Three proven uneditable blocking findings, three distinct files");
}

void testRecordedFindingsMatchPathsCaseAndSeparatorInsensitively()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Views/Settings.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking,
        {finding("QML\\views\\settings.qml")})}));
    req(v.findings.size() == 1 && v.findings.front().authority == "unknown_until_created",
        "The same file written with another case/separator must still count as a target");
}

void testRecordedFindingsKeepFindingsAndSayUnknownWhenAProposalCannotBeRead()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1", "ghost"}, {diagnostic(kBlocking,
        {finding("qml/Made.qml"), finding("qml/Elsewhere.qml")})}));
    req(v.findings.size() == 2, "A classification failure must never hide the findings");
    req(v.findings[0].authority == "unknown" && v.findings[1].authority == "unknown",
        "No authority may be claimed when one proposal could not be read");
    req(v.classificationErrorCode == "manual_handoff.classification_unavailable"
            && !v.classificationErrorMessage.empty(),
        "The failure is reported with a Backend code and message, never silently empty");
    req(v.verdict == "unknown" && v.verdictCode == v.classificationErrorCode,
        "The verdict is unknown and names the classification error");
    req(v.outsideScopePaths.empty(), "Nothing is proven outside");
}

void testRecordedFindingsWithNoFrozenProposalsAreUnclassifiable()
{
    Fixture fx;
    const auto v = recorded(fx, ledgerWith(fx, {}, {diagnostic(kBlocking, {finding("qml/A.qml")})}));
    req(v.findings.size() == 1 && v.findings.front().authority == "unknown"
            && v.classificationErrorCode == "manual_handoff.classification_unavailable"
            && v.verdict == "unknown",
        "An empty proposal set would make every file look outside: it must be unknown instead");
}

void testRecordedFindingsKeepAProvenBlockerOverUnknownData()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {
        diagnostic("verification.command_failed", {}, "the build failed"),
        diagnostic(kBlocking, {finding("qml/Elsewhere.qml")})}));
    req(v.verdict == "blocked_outside_scope",
        "A proven outside-scope blocker stays proven even beside a non-QML failure");
    req(v.findings.size() == 2 && v.findings.front().relativePath.empty()
            && v.findings.front().authority == "unknown",
        "The diagnostic without structured findings is still its own row");
}

void testRecordedFindingsShowADiagnosticWithoutStructuredFindingsAndStayUnknown()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {
        diagnostic("verification.qml_findings_overflow", {}, "too many findings"),
        diagnostic(kBlocking, {finding("qml/Made.qml")})}));
    req(v.findings.size() == 2 && v.findings.front().severity == "blocking"
            && v.findings.front().text.find("too many findings") != std::string::npos,
        "A non-QML/overflow failure must be visible as a row");
    req(v.verdict == "unknown" && v.verdictCode == "manual_handoff.verdict_unmapped_blocking_finding",
        "A blocking failure with no file can never give no_known_obstacle");
}

void testRecordedFindingsTreatAnUnsafePathAsUnmapped()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    ManifestVerificationDiagnosticFinding absolute {"C:/secret/x.qml", 1, 1, "", "text"};
    ManifestVerificationDiagnosticFinding traversal {"<candidate>/../x.qml", 1, 1, "", "text"};
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, {absolute, traversal})}));
    req(v.findings.size() == 2 && v.findings[0].relativePath.empty() && v.findings[1].relativePath.empty()
            && v.findings[0].authority == "unknown" && v.findings[1].authority == "unknown",
        "A path that does not normalise safely is never classified");
    req(v.outsideScopePaths.empty() && v.verdict == "unknown"
            && v.verdictCode == "manual_handoff.verdict_unmapped_blocking_finding",
        "An unsafe path must not be claimed as outside, and it must count as a blocking failure with "
        "no comparable file");
}

void testRecordedFindingsCountEachOutsideFileOnceButEveryFinding()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, {
        finding("qml/Elsewhere.qml", 3), finding("qml/Elsewhere.qml", 9)})}));
    req(v.outsideScopePaths.size() == 1 && v.outsideScopePaths.front() == "qml/Elsewhere.qml",
        "The same file is listed once");
    req(v.verdictMessage.find("2 recorded blocking finding(s)") != std::string::npos,
        "Both findings in that file are still counted");
}

void testRecordedFindingsIgnorableCodesAreNeverBlocking()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {
        diagnostic("verification.prerequisite_failed", {finding("qml/Cascade.qml")}),
        diagnostic("verification.qml_warning_outside_task_scope", {finding("qml/Legacy.qml")}),
        diagnostic(kBlocking, {finding("qml/Made.qml")})}));
    req(rowFor(v, "qml/Cascade.qml")->severity == "advisory"
            && rowFor(v, "qml/Legacy.qml")->severity == "advisory",
        "A cascade or legacy-ignorable diagnostic is shown, but never as blocking");
    req(v.verdict == "no_known_obstacle" && v.outsideScopePaths.empty(),
        "An ignorable finding outside every target must not make the handoff blocked");
}

void testRecordedFindingsWithOnlyNonBlockingFindingsAreUnknown()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {
        diagnostic(std::string(kQmlAdvisoryDiagnosticCode), {finding("qml/Made.qml")})}));
    req(v.findings.size() == 1 && v.verdict == "unknown"
            && v.verdictCode == "manual_handoff.verdict_no_blocking_finding_recorded",
        "With no blocking finding recorded nothing is known about an obstacle: never no_known_obstacle");
}

void testRecordedFindingsSampleLimitMeansPossiblyTruncated()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto many = [](std::size_t count, bool withOutside) {
        std::vector<ManifestVerificationDiagnosticFinding> findings;
        for (std::size_t i = 0; i < count; ++i) {
            findings.push_back(finding(withOutside && i == 0 ? "qml/Elsewhere.qml" : "qml/Made.qml",
                static_cast<int>(i + 1)));
        }
        return findings;
    };
    const auto limit = kMaximumRecordedFindingsPerDiagnostic;
    const auto atLimit = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, many(limit, false))}));
    req(atLimit.verdict == "unknown"
            && atLimit.verdictCode == "manual_handoff.verdict_sample_may_be_truncated",
        "A blocking diagnostic holding exactly the sample limit may have been truncated");
    const auto belowLimit =
        recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, many(limit - 1, false))}));
    req(belowLimit.verdict == "no_known_obstacle", "Below the limit the sample is complete");
    const auto provenAtLimit =
        recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, many(limit, true))}));
    req(provenAtLimit.verdict == "blocked_outside_scope",
        "A proven blocker wins even when the sample may be truncated");
}

void testRecordedFindingsCountDebtAndNeverBlockOnAdvisory()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto v = recorded(fx, ledgerWith(fx, {"p1"}, {
        diagnostic(std::string(kQmlBaselineDebtDiagnosticCode),
            {finding("qml/Old1.qml"), finding("qml/Old2.qml")}),
        diagnostic(std::string(kQmlAdvisoryDiagnosticCode), {finding("qml/Soft.qml")}),
        diagnostic(kBlocking, {finding("qml/Made.qml")})}));
    req(v.debtCount == 2 && rowFor(v, "qml/Old1.qml") == nullptr,
        "Proven debt is counted, not listed");
    const auto* soft = rowFor(v, "qml/Soft.qml");
    req(soft != nullptr && soft->severity == "advisory",
        "An advisory finding is listed as advisory");
    req(v.verdict == "no_known_obstacle" && v.outsideScopePaths.empty(),
        "An advisory finding outside every target must never make the handoff blocked");
}

void testRecordedFindingsAreEmptyWithoutAFailedVerification()
{
    Fixture fx;
    addProposal(fx, "p1", {{Op::Create, "qml/Made.qml"}});
    const auto entry = ledgerWith(fx, {"p1"}, {diagnostic(kBlocking, {finding("qml/Made.qml")})});
    const auto notFailed = recorded(fx, entry, /*failed=*/false);
    req(notFailed.findings.empty() && notFailed.verdict == "unknown",
        "A non-failed anchor has no recorded findings to show");
    const auto noLedger = recorded(fx, std::nullopt);
    req(noLedger.findings.empty() && noLedger.verdict == "unknown",
        "An anchor without a ledger entry gives an empty, unknown projection");
    auto noVerification = entry;
    noVerification->verification.reset();
    req(recorded(fx, noVerification).findings.empty(),
        "An anchor without a verification gives an empty projection");
}

void testRecordedFindingsUseTheFrozenAllowedPathsOnceAHandoffExists()
{
    Fixture fx;
    const std::unordered_set<std::string> allowed {
        detail::normalizedPathKey(detail::pathFromUtf8("qml/App.qml"))};
    const auto onlyEditable = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking,
        {finding("qml/App.qml")})}), true, &allowed);
    req(onlyEditable.findings.front().authority == "editable"
            && onlyEditable.verdict == "no_known_obstacle",
        "A finding on a frozen allowed path is editable");
    const auto withOutside = recorded(fx, ledgerWith(fx, {"p1"}, {diagnostic(kBlocking,
        {finding("qml/App.qml"), finding("qml/SettingsSection.qml")})}), true, &allowed);
    req(rowFor(withOutside, "qml/SettingsSection.qml")->authority == "not_editable"
            && withOutside.verdict == "blocked_outside_scope",
        "After creation the frozen list alone decides; proposals are not read (p1 does not exist here)");
    req(withOutside.classificationErrorCode.empty(),
        "With a handoff no proposal read is needed, so there is no classification error");
}

void testViewForRunProjectsRecordedFindingsBeforeAndAfterCreation()
{
    Fixture fx;
    const auto before = fx.readModel->viewForRun(fx.source.id);
    req(before.success() && before.value().has_value(), "The pre-create view must exist");
    const auto& pre = before.value()->recordedFindings;
    req(pre.findings.size() == 1 && pre.findings.front().relativePath == "qml/App.qml"
            && pre.findings.front().authority == "unknown_until_created"
            && pre.verdict == "no_known_obstacle",
        "The recorded finding must be visible before any handoff exists");
    req(before.value()->initialScopeDiagnostics.empty(),
        "The legacy editable-scope list stays empty before a handoff exists");

    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "Warning: <candidate>/qml/App.qml:4:1: Failed to import module [import]\n";
    fx.commandRunner.queuedOutcomes = {cleanOutcome, warningOutcome};
    req(fx.manualHandoffService->materialize(fx.source.id).success(), "materialize must succeed");

    const auto after = fx.readModel->viewForRun(fx.source.id);
    req(after.success() && after.value().has_value(), "The post-create view must exist");
    const auto& post = after.value()->recordedFindings;
    req(post.findings.size() == 1 && post.findings.front().authority == "editable"
            && post.verdict == "no_known_obstacle",
        "After creation the same finding is editable");
    req(after.value()->initialScopeDiagnostics.size() == 1,
        "The legacy initialScopeDiagnostics contract is unchanged");
}

} // namespace

int runManifestVerificationManualHandoffReadModelServiceTests()
{
    testViewForRunReturnsNulloptForMissingRun();
    testViewForRunReturnsNulloptForUnrelatedWorkflow();
    testViewForRunShowsNotYetCreatedWhenNoHandoff();
    testViewForRunProjectsMaterializedHandoffCandidateHealthAndAllowedPaths();
    testViewForRunProjectsAuthoritativeAttemptAndRealAg16ApplyEligibility();
    testViewForRunProjectsFailedAttemptVerificationDiagnostics();
    testViewForRunProjectsRejectionWithoutAuthority();
    testViewForRunResolvesAuthorityFromLedgerAloneWithoutTypedProjection();
    testViewForRunFailsClosedOnTypedResultConflictingWithMissingLedger();
    testViewForRunNewerCompletedAttemptSupersedesOlderAsAuthority();
    testViewForRunCanApplyIteratesAllMembersNotJustFirst();
    testViewForRunReviewMembersFailClosedOnMissingProposal();
    testViewForRunProjectsCanCreateFalseForIneligibleManifestAnchor();
    testViewForRunProjectsCanCreateFalseForUnresolvableContinuationChildAnchor();
    testViewForRunProjectsDistinctOriginAndAnchorForMixedContinuationChild();
    testRunResolutionBatchTracksUnresolvedAwaitingPartialAndFullyApplied();
    testViewForRunProjectsEditableAndResetRequiredCandidatePaths();
    testViewForRunSplitsStructuredFindingsIntoActionableAndOutsideScope();
    testViewForRunProjectsBaselineAwareQmlLintDeltaIntoSegmentedBuckets();
    testViewForRunProjectsUnmappableDiagnosticAsGeneralOnly();
    testRecordedFindingsProveOutsideOnlyWhenNoCreateOrModifyTargetMatches();
    testRecordedFindingsMatchPathsCaseAndSeparatorInsensitively();
    testRecordedFindingsKeepFindingsAndSayUnknownWhenAProposalCannotBeRead();
    testRecordedFindingsWithNoFrozenProposalsAreUnclassifiable();
    testRecordedFindingsKeepAProvenBlockerOverUnknownData();
    testRecordedFindingsShowADiagnosticWithoutStructuredFindingsAndStayUnknown();
    testRecordedFindingsTreatAnUnsafePathAsUnmapped();
    testRecordedFindingsCountEachOutsideFileOnceButEveryFinding();
    testRecordedFindingsIgnorableCodesAreNeverBlocking();
    testRecordedFindingsWithOnlyNonBlockingFindingsAreUnknown();
    testRecordedFindingsSampleLimitMeansPossiblyTruncated();
    testRecordedFindingsCountDebtAndNeverBlockOnAdvisory();
    testRecordedFindingsAreEmptyWithoutAFailedVerification();
    testRecordedFindingsUseTheFrozenAllowedPathsOnceAHandoffExists();
    testViewForRunProjectsRecordedFindingsBeforeAndAfterCreation();
    return 0;
}
