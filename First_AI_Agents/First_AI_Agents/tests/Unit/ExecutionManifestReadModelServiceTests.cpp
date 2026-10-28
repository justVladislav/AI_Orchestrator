// Unit tests for ExecutionManifestReadModelService's AG-15.3 workspace-scope
// projection (ManifestTaskView::scopeEntries/inspectedPaths/
// proposedCreatedPaths/proposedModifiedPaths, ExecutionManifestView::
// scopeDependencyEdges/workspaceScopeAvailable). Entirely in-memory - no
// SQLite, no provider/turn-loop machinery. Proves the read model is a pure,
// deterministic, no-duplicate projection over already-durable Backend state
// (frozen RunWorkspaceScope + expansions + ManifestTaskState +
// change-proposal data), never something a UI layer could compute itself.
#include "ai/application/execution/ExecutionManifestReadModelService.hpp"

#include "ai/application/execution/ExecutionManifestService.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryManifestTaskExecutionRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryRunWorkspaceScopeRepository.hpp"
#include "ai/application/InMemoryVerificationBootstrapRepository.hpp"
#include "ai/application/InMemoryVerificationEnvironmentRepository.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/RunWorkspaceScopeService.hpp"

#include <chrono>
#include <map>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

namespace common = ai::common;
using namespace ai::application;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

// Minimal IChangeProposalRepository fake: only listChanges() is exercised by
// the read model, and this test seeds it directly rather than through the
// full parse/validate pipeline (already covered elsewhere).
class SimpleChangeProposalRepository final : public IChangeProposalRepository {
public:
    void seedChanges(const ChangeSetId& id, std::vector<ProposedFileChange> changes)
    {
        std::lock_guard lock(mutex_);
        changesBySet_[id] = std::move(changes);
    }

    common::Result<void> insertChangeSet(const ProposedChangeSet&) override
    {
        return common::Result<void>::ok();
    }
    common::Result<ProposedChangeSet> updateChangeSetState(const ChangeSetId&, ChangeSetState) override
    {
        return common::Result<ProposedChangeSet>::failure({"test.unsupported", "unused in this fake"});
    }
    common::Result<void> markMissingEvidence(const ChangeSetId&, std::string, std::string) override
    {
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId&) const override
    {
        return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId& id) const override
    {
        std::lock_guard lock(mutex_);
        const auto it = changesBySet_.find(id);
        if (it == changesBySet_.end()) return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second);
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId&) const override
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
    mutable std::mutex mutex_;
    std::map<ChangeSetId, std::vector<ProposedFileChange>> changesBySet_;
};

WorkspaceBaselineSnapshot makeBaseline(std::vector<std::pair<std::string, std::string>> filesAndHashes)
{
    WorkspaceBaselineSnapshot baseline;
    for (auto& [path, hash] : filesAndHashes) {
        WorkspaceBaselineEntry entry;
        entry.relativePath = path;
        entry.byteSize = 10;
        entry.sha256 = hash;
        baseline.files.push_back(std::move(entry));
    }
    return baseline;
}

ExecutionManifestTask makeTask(const std::string& id)
{
    ExecutionManifestTask task;
    task.id = id;
    task.objective = "Objective for " + id;
    task.role = "worker";
    task.requestedCapability = WorkspaceCapability::Propose;
    return task;
}

struct Fixture {
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    InMemoryRunWorkspaceScopeRepository scopeRepository;
    SimpleChangeProposalRepository changeProposals;
    // AG-18.4A
    InMemoryVerificationEnvironmentRepository verificationEnvironmentRepository;
    InMemoryVerificationBootstrapRepository verificationBootstrapRepository;

    ExecutionManifestService manifests {manifestRepository, runs};
    RunWorkspaceScopeService scopeService {scopeRepository};
    ExecutionManifestReadModelService readModel {
        manifests, taskExecutions, &scopeRepository, &changeProposals,
        &verificationEnvironmentRepository, &verificationBootstrapRepository};

    void seedRun(const RunId& id)
    {
        Run run;
        run.id = id;
        run.projectId = "project-1";
        run.title = "Read model test run";
        run.task = "Exercise the AG-15.3 projection";
        run.workflowId = "execution-manifest-v1";
        const auto inserted = runs.insertRun(run);
        req(inserted.success(), "Could not seed Run for read-model test");
    }
};

// Two tasks: "define-shared" owns src/shared.hpp (existing baseline file);
// "implement" requires reading it and plans to create src/new_file.cpp.
// Proves: scope entries are grouped per task with the correct role strings,
// dependency edges name both sides, inspected/proposed paths come through
// from durable state (never duplicated across the create/modify buckets),
// and an approved expansion shows up tagged isExpansion=true.
void testViewProjectsScopeInspectedAndProposedPathsWithoutDuplicates()
{
    Fixture fx;
    const RunId runId = "run-1";
    fx.seedRun(runId);

    ExecutionManifest manifest;
    manifest.runId = runId;
    auto define = makeTask("define-shared");
    define.ownedPaths = {std::filesystem::path("src/shared.hpp")};
    auto implement = makeTask("implement");
    implement.dependencies = {"define-shared"};
    implement.requiredReadPaths = {std::filesystem::path("src/shared.hpp")};
    implement.plannedCreatePaths = {std::filesystem::path("src/new_file.cpp")};
    manifest.tasks = {std::move(define), std::move(implement)};

    const auto frozen = fx.manifests.freeze(manifest, ExecutionManifestPolicy {});
    req(frozen.success(), "Could not freeze the test manifest");

    const auto baseline = makeBaseline({
        {"src/shared.hpp", "hash-shared"},
        {"docs/notes.md", "hash-notes"},
    });
    const auto frozenScope = fx.scopeService.freezeForManifest(runId, frozen.value(), baseline);
    req(frozenScope.success(), "Could not freeze the test workspace scope");

    // "implement" discovers a genuine extra dependency mid-attempt.
    const auto expansion = fx.scopeService.requestExpansion(
        runId, "implement", "docs/notes.md", WorkspaceScopeRole::ReferenceOnly,
        "Useful background context", WorkspaceReadPolicy::secureDefaults("C:/workspace"),
        baseline, /*maxExpansionsForTask=*/5);
    req(expansion.success(), "Could not record the test scope expansion");

    ManifestTaskState defineState;
    defineState.status = ManifestTaskStatus::Completed;
    defineState.inspectedRelativePaths = {"src/shared.hpp"};
    req(fx.taskExecutions.upsert(runId, "define-shared", defineState).success(),
        "Could not seed define-shared task state");

    ManifestTaskState implementState;
    implementState.status = ManifestTaskStatus::Completed;
    implementState.inspectedRelativePaths = {"src/shared.hpp", "docs/notes.md"};
    implementState.changeSetId = ChangeSetId {"cs-1"};
    req(fx.taskExecutions.upsert(runId, "implement", implementState).success(),
        "Could not seed implement task state");

    ProposedFileChange createNew;
    createNew.changeId = "c1";
    createNew.operation = FileChangeOperation::Create;
    createNew.relativePath = std::filesystem::path("src/new_file.cpp");
    ProposedFileChange modifyShared;
    modifyShared.changeId = "c2";
    modifyShared.operation = FileChangeOperation::Modify;
    modifyShared.relativePath = std::filesystem::path("src/shared.hpp");
    fx.changeProposals.seedChanges(ChangeSetId {"cs-1"}, {createNew, modifyShared});

    const auto view = fx.readModel.viewForRun(runId);
    req(view.success(), "viewForRun failed");
    req(view.value().has_value(), "Expected a view for a frozen manifest");
    const auto& manifestView = *view.value();

    req(manifestView.workspaceScopeAvailable, "workspaceScopeAvailable was not set");
    req(manifestView.scopeDependencyEdges.size() == 1
            && manifestView.scopeDependencyEdges.front().dependentTaskId == "implement"
            && manifestView.scopeDependencyEdges.front().providerTaskId == "define-shared"
            && manifestView.scopeDependencyEdges.front().relativePath == "src/shared.hpp",
        "Dependency edge was not projected correctly");
    req(manifestView.tasks.size() == 2, "Expected exactly 2 tasks in the view");

    const ManifestTaskView* implementView = nullptr;
    const ManifestTaskView* defineView = nullptr;
    for (const auto& task : manifestView.tasks) {
        if (task.id == "implement") implementView = &task;
        if (task.id == "define-shared") defineView = &task;
    }
    req(implementView != nullptr && defineView != nullptr, "Expected both tasks present");

    req(defineView->scopeEntries.size() == 1
            && defineView->scopeEntries.front().relativePath == "src/shared.hpp"
            && defineView->scopeEntries.front().role == "owned_write"
            && !defineView->scopeEntries.front().isExpansion,
        "define-shared's scope projection was wrong");
    req(defineView->inspectedPaths.size() == 1 && defineView->inspectedPaths.front() == "src/shared.hpp",
        "define-shared's inspected paths were not projected");

    // implement's scope: required_read (src/shared.hpp), planned_create
    // (src/new_file.cpp), plus the approved reference_only expansion
    // (docs/notes.md) - exactly 3 entries, no path double-counted.
    req(implementView->scopeEntries.size() == 3,
        "implement's scope projection did not include the frozen entries plus the expansion");
    bool sawRequiredRead = false;
    bool sawPlannedCreate = false;
    bool sawExpansion = false;
    for (const auto& entry : implementView->scopeEntries) {
        if (entry.relativePath == "src/shared.hpp" && entry.role == "required_read" && !entry.isExpansion)
            sawRequiredRead = true;
        if (entry.relativePath == "src/new_file.cpp" && entry.role == "planned_create" && !entry.isExpansion)
            sawPlannedCreate = true;
        if (entry.relativePath == "docs/notes.md" && entry.role == "reference_only" && entry.isExpansion)
            sawExpansion = true;
    }
    req(sawRequiredRead && sawPlannedCreate && sawExpansion,
        "implement's scope entries were missing an expected role/expansion combination");

    req(implementView->inspectedPaths.size() == 2, "implement's inspected paths were not projected");
    req(implementView->proposedCreatedPaths.size() == 1
            && implementView->proposedCreatedPaths.front() == "src/new_file.cpp",
        "implement's proposed created paths were wrong");
    req(implementView->proposedModifiedPaths.size() == 1
            && implementView->proposedModifiedPaths.front() == "src/shared.hpp",
        "implement's proposed modified paths were wrong");
    // No path appears in both buckets.
    req(implementView->proposedCreatedPaths.front() != implementView->proposedModifiedPaths.front(),
        "A single path was double-counted across created and modified");
}

// A read model constructed without a workspace-scope repository (the
// pre-AG-15.3 constructor shape, still valid via the trailing-optional
// parameters) must keep behaving exactly as before: no scope fields
// populated, no failure, workspaceScopeAvailable stays false.
void testViewWithoutWorkspaceScopeRepositoryStaysBackwardCompatible()
{
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    ExecutionManifestService manifests(manifestRepository, runs);
    ExecutionManifestReadModelService readModel(manifests, taskExecutions);

    const RunId runId = "run-2";
    Run run;
    run.id = runId;
    run.projectId = "project-1";
    run.title = "Backward-compat test run";
    run.task = "No workspace scope wired";
    run.workflowId = "execution-manifest-v1";
    req(runs.insertRun(run).success(), "Could not seed Run");

    ExecutionManifest manifest;
    manifest.runId = runId;
    manifest.tasks = {makeTask("solo")};
    req(manifests.freeze(manifest, ExecutionManifestPolicy {}).success(),
        "Could not freeze the backward-compat test manifest");

    const auto view = readModel.viewForRun(runId);
    req(view.success() && view.value().has_value(), "viewForRun failed");
    req(!view.value()->workspaceScopeAvailable, "workspaceScopeAvailable should stay false");
    req(view.value()->scopeDependencyEdges.empty(), "scopeDependencyEdges should stay empty");
    req(view.value()->tasks.size() == 1 && view.value()->tasks.front().scopeEntries.empty(),
        "Task-level scopeEntries should stay empty without a wired repository");
}

// ---------------------------------------------------------------------
// AG-18.4A: verificationEnvironmentViewForRun
// ---------------------------------------------------------------------

VerificationEnvironmentSnapshot makeSnapshot(
    const RunId& runId, VerificationEnvironmentState state, std::string diagnostic = {})
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = "project-1";
    snapshot.baselineFingerprint = "baseline-fp";
    snapshot.state = state;
    snapshot.diagnostic = std::move(diagnostic);
    snapshot.discoveredAt = std::chrono::system_clock::now();
    return snapshot;
}

VerificationBootstrapDecision makeBootstrapDecision(
    const RunId& runId, VerificationBootstrapDisposition disposition)
{
    VerificationBootstrapDecision decision;
    decision.runId = runId;
    decision.projectId = "project-1";
    decision.baselineFingerprint = "baseline-fp";
    decision.verificationSnapshotSchemaVersion =
        std::string(kVerificationEnvironmentSnapshotSchemaVersion);
    decision.disposition = disposition;
    decision.reasonCode = disposition == VerificationBootstrapDisposition::ActionRequired
        ? "verification_environment.action_required"
        : "verification_environment.bootstrap_pending";
    decision.message = "Test decision";
    decision.decidedAt = std::chrono::system_clock::now();
    return decision;
}

void testVerificationEnvironmentViewIsAbsentWithoutSnapshot()
{
    Fixture fx;
    fx.seedRun("run-3");
    const auto view = fx.readModel.verificationEnvironmentViewForRun("run-3");
    req(view.success() && !view.value().has_value(),
        "A Run with no recorded VerificationEnvironmentSnapshot must project no view");
}

// An ExistingProfile snapshot projects its discovered profile/build-preset
// values verbatim and is never eligible for "Retry after setup" (a usable
// profile already exists).
void testVerificationEnvironmentViewProjectsExistingProfile()
{
    Fixture fx;
    const RunId runId = "run-4";
    fx.seedRun(runId);

    auto snapshot = makeSnapshot(runId, VerificationEnvironmentState::ExistingProfile);
    VerificationProfile profile;
    profile.profileId = "local-qt-debug";
    profile.configurePresetName = "local-qt-debug";
    profile.configureSource = VerificationPresetFileSource::UserPresets;
    profile.resolvedBinaryDir = "build/local-qt-debug";
    profile.buildPresetsConfirmed = true;
    VerificationBuildPresetDescriptor buildPreset;
    buildPreset.name = "local-qt-debug";
    buildPreset.allowedTargetsConfirmed = true;
    buildPreset.allowedTargets = {"all_qmllint", "all"};
    profile.buildPresets.push_back(buildPreset);
    snapshot.profiles.push_back(profile);
    req(fx.verificationEnvironmentRepository.recordSnapshot(snapshot).success(),
        "Could not seed the test snapshot");

    const auto view = fx.readModel.verificationEnvironmentViewForRun(runId);
    req(view.success() && view.value().has_value(), "verificationEnvironmentViewForRun failed");
    const auto& ve = *view.value();
    req(ve.state == "existing_profile", "Expected existing_profile state");
    req(ve.issue == "none", "Expected no issue for a fully-confirmed profile");
    req(ve.profiles.size() == 1, "Expected exactly one projected profile");
    req(ve.profiles.front().profileId == "local-qt-debug"
            && ve.profiles.front().configureSource == "user_presets"
            && ve.profiles.front().resolvedBinaryDir == "build/local-qt-debug"
            && ve.profiles.front().buildPresetsConfirmed
            && ve.profiles.front().buildPresets.size() == 1
            && ve.profiles.front().buildPresets.front().allowedTargets
                == std::vector<std::string> {"all_qmllint", "all"},
        "Profile/build-preset values did not project verbatim");
    req(!ve.canRetryAfterSetup && !ve.canRetryAfterSetupReason.empty(),
        "An ExistingProfile Run must never be eligible for Retry after setup, and must explain why");
    req(!ve.canConfigureBackendProfile
            && ve.canConfigureBackendProfileReasonCode
                == "verification_environment.profile_configuration_unavailable"
            && ve.canConfigureBackendProfileReason.find("existing Project") != std::string::npos
            && ve.canConfigureBackendProfileReason.find("Run Details")
                != std::string::npos,
        "Run Details must keep existing-Project configuration disabled without denying the "
        "creation-time setup capability");
}

// A stored SetupRequired snapshot is reclassified to proposed_bootstrap at
// read time when a durable BootstrapPending decision exists - the
// persisted snapshot's own state is never mutated (only the InMemory
// repository's returned copy is inspected here, proving the reclassification
// happens in the read model, not the repository).
void testVerificationEnvironmentViewReclassifiesProposedBootstrap()
{
    Fixture fx;
    const RunId runId = "run-5";
    fx.seedRun(runId);
    req(fx.verificationEnvironmentRepository.recordSnapshot(
            makeSnapshot(runId, VerificationEnvironmentState::SetupRequired,
                "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline"))
            .success(),
        "Could not seed the test snapshot");
    req(fx.verificationBootstrapRepository.recordDecision(
            makeBootstrapDecision(runId, VerificationBootstrapDisposition::BootstrapPending))
            .success(),
        "Could not seed the test bootstrap decision");

    const auto view = fx.readModel.verificationEnvironmentViewForRun(runId);
    req(view.success() && view.value().has_value(), "verificationEnvironmentViewForRun failed");
    const auto& ve = *view.value();
    req(ve.state == "proposed_bootstrap",
        "A SetupRequired snapshot with a BootstrapPending decision must reclassify to "
        "proposed_bootstrap");
    req(ve.hasBootstrapDecision && ve.bootstrapDisposition == "bootstrap_pending",
        "Bootstrap decision was not projected");
    req(!ve.canRetryAfterSetup,
        "A BootstrapPending Run (not ActionRequired) must never be eligible for Retry after setup");

    const auto stored = fx.verificationEnvironmentRepository.findForRun(runId);
    req(stored.success() && stored.value().has_value()
            && stored.value()->state == VerificationEnvironmentState::SetupRequired,
        "The persisted snapshot's own state must never be mutated by the read-model "
        "reclassification");
}

// The one scenario Retry after setup is actually eligible for: a durable
// ActionRequired decision.
void testVerificationEnvironmentViewSetupRequiredAllowsRetryAfterSetup()
{
    Fixture fx;
    const RunId runId = "run-6";
    fx.seedRun(runId);
    req(fx.verificationEnvironmentRepository.recordSnapshot(
            makeSnapshot(runId, VerificationEnvironmentState::SetupRequired,
                "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline"))
            .success(),
        "Could not seed the test snapshot");
    req(fx.verificationBootstrapRepository.recordDecision(
            makeBootstrapDecision(runId, VerificationBootstrapDisposition::ActionRequired))
            .success(),
        "Could not seed the test bootstrap decision");

    const auto view = fx.readModel.verificationEnvironmentViewForRun(runId);
    req(view.success() && view.value().has_value(), "verificationEnvironmentViewForRun failed");
    const auto& ve = *view.value();
    req(ve.state == "setup_required", "An ActionRequired decision must not reclassify to "
        "proposed_bootstrap");
    req(ve.issue == "none", "A genuinely-missing-preset diagnostic must not classify as an issue");
    req(ve.canRetryAfterSetup && ve.canRetryAfterSetupReason.empty(),
        "A durable ActionRequired decision must make Retry after setup eligible with no reason "
        "text");
}

// The Backend's own diagnostic-drift literals classify as "drifted" -
// matched only against the closed set VerificationEnvironmentDiscoveryService
// itself produces (see ManifestVerificationEnvironmentIssue's doc comment).
void testVerificationEnvironmentViewClassifiesDrift()
{
    Fixture fx;
    const RunId runId = "run-7";
    fx.seedRun(runId);
    req(fx.verificationEnvironmentRepository.recordSnapshot(
            makeSnapshot(runId, VerificationEnvironmentState::SetupRequired,
                "The workspace changed since the baseline was captured"))
            .success(),
        "Could not seed the test snapshot");

    const auto view = fx.readModel.verificationEnvironmentViewForRun(runId);
    req(view.success() && view.value().has_value(), "verificationEnvironmentViewForRun failed");
    req(view.value()->issue == "drifted", "Expected the drift diagnostic to classify as drifted");
}

// A profile whose build-preset probe genuinely failed (buildPresetsConfirmed
// == false - the structural signal, not a diagnostic string) classifies as
// tool_unavailable even while the profile itself is still ExistingProfile.
void testVerificationEnvironmentViewClassifiesToolUnavailableFromUnconfirmedBuildPresets()
{
    Fixture fx;
    const RunId runId = "run-8";
    fx.seedRun(runId);
    auto snapshot = makeSnapshot(runId, VerificationEnvironmentState::ExistingProfile);
    VerificationProfile profile;
    profile.profileId = "local-qt-debug";
    profile.configurePresetName = "local-qt-debug";
    profile.buildPresetsConfirmed = false;
    snapshot.profiles.push_back(profile);
    req(fx.verificationEnvironmentRepository.recordSnapshot(snapshot).success(),
        "Could not seed the test snapshot");

    const auto view = fx.readModel.verificationEnvironmentViewForRun(runId);
    req(view.success() && view.value().has_value(), "verificationEnvironmentViewForRun failed");
    const auto& ve = *view.value();
    req(ve.state == "existing_profile", "An unconfirmed build preset must not change state");
    req(ve.issue == "tool_unavailable",
        "An unconfirmed build-preset profile must classify as tool_unavailable");
}

// A durable decision whose own identity does not match its Run's snapshot
// (baselineFingerprint here) must fail closed - never silently projected
// as if it belonged to this Run.
void testVerificationEnvironmentViewFailsClosedOnIdentityMismatch()
{
    Fixture fx;
    const RunId runId = "run-9";
    fx.seedRun(runId);
    req(fx.verificationEnvironmentRepository.recordSnapshot(
            makeSnapshot(runId, VerificationEnvironmentState::SetupRequired,
                "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline"))
            .success(),
        "Could not seed the test snapshot");
    auto mismatched = makeBootstrapDecision(runId, VerificationBootstrapDisposition::ActionRequired);
    mismatched.baselineFingerprint = "a-completely-different-baseline";
    req(fx.verificationBootstrapRepository.recordDecision(mismatched).success(),
        "Could not seed the mismatched test decision");

    const auto view = fx.readModel.verificationEnvironmentViewForRun(runId);
    req(!view.success(), "An identity-mismatched decision was silently accepted");
    req(view.error()->code == "verification_environment.corrupt_bootstrap_record",
        "Expected corrupt_bootstrap_record for a decision/snapshot identity mismatch");
}

// AG-18.4A hardening test-only fake: InMemoryVerificationBootstrapRepository's
// own appendTransition() already refuses a second resolving transition for
// the same Run (a real, already-tested invariant - see
// VerificationBootstrapRepositoryTests.cpp), so a corrupted table with two
// resolving transitions can never be produced through the real repository's
// own public API. This fake exists only to simulate that otherwise-
// unreachable corrupted state, proving the READ MODEL's own independent
// defensive check (never silently pick the first transition from a corrupt
// set) actually fires rather than relying solely on the repository.
class CorruptTransitionsRepository final : public IVerificationBootstrapRepository {
public:
    explicit CorruptTransitionsRepository(InMemoryVerificationBootstrapRepository& decisions)
        : decisions_(decisions)
    {
    }
    common::Result<void> recordDecision(VerificationBootstrapDecision decision) override
    {
        return decisions_.recordDecision(std::move(decision));
    }
    common::Result<std::optional<VerificationBootstrapDecision>> findDecisionForRun(
        const RunId& runId) const override
    {
        return decisions_.findDecisionForRun(runId);
    }
    common::Result<void> appendTransition(VerificationBootstrapTransition) override
    {
        return common::Result<void>::failure({"test.unsupported", "unused in this fake"});
    }
    common::Result<std::vector<VerificationBootstrapTransition>> listTransitionsForRun(
        const RunId&) const override
    {
        return common::Result<std::vector<VerificationBootstrapTransition>>::ok(corruptTransitions);
    }
    common::Result<void> recordRetryLink(VerificationBootstrapRetryLink) override
    {
        return common::Result<void>::failure({"test.unsupported", "unused in this fake"});
    }
    common::Result<std::optional<VerificationBootstrapRetryLink>> findRetryLinkForPredecessor(
        const RunId&) const override
    {
        return common::Result<std::optional<VerificationBootstrapRetryLink>>::ok(std::nullopt);
    }

    std::vector<VerificationBootstrapTransition> corruptTransitions;

private:
    InMemoryVerificationBootstrapRepository& decisions_;
};

void testVerificationEnvironmentViewFailsClosedOnMultipleResolvingTransitions()
{
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    InMemoryVerificationEnvironmentRepository veRepository;
    InMemoryVerificationBootstrapRepository decisionsOnly;
    CorruptTransitionsRepository corruptRepository(decisionsOnly);
    ExecutionManifestService manifests(manifestRepository, runs);
    ExecutionManifestReadModelService readModel(
        manifests, taskExecutions, nullptr, nullptr, &veRepository, &corruptRepository);

    const RunId runId = "run-10";
    Run run;
    run.id = runId;
    run.projectId = "project-1";
    run.title = "Corrupt transitions test run";
    run.task = "Exercise the multi-resolving-transition guard";
    run.workflowId = "execution-manifest-v1";
    req(runs.insertRun(run).success(), "Could not seed Run");
    req(veRepository
            .recordSnapshot(makeSnapshot(runId, VerificationEnvironmentState::SetupRequired,
                "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline"))
            .success(),
        "Could not seed the test snapshot");
    req(corruptRepository
            .recordDecision(makeBootstrapDecision(runId, VerificationBootstrapDisposition::BootstrapPending))
            .success(),
        "Could not seed the test decision");

    VerificationBootstrapTransition verified;
    verified.id = runId + ":bootstrap_verified";
    verified.runId = runId;
    verified.toDisposition = VerificationBootstrapDisposition::BootstrapVerified;
    verified.reasonCode = "verification_environment.bootstrap_verified";
    VerificationBootstrapCandidateIdentity identity;
    identity.overlayFingerprint = "overlay-fp";
    identity.candidateProfileId = "local-qt-debug";
    identity.candidateBuildProfileId = "local-qt-debug";
    verified.candidateIdentity = identity;
    verified.transitionedAt = std::chrono::system_clock::now();

    VerificationBootstrapTransition failed = verified;
    failed.id = runId + ":bootstrap_failed";
    failed.toDisposition = VerificationBootstrapDisposition::BootstrapFailed;
    failed.reasonCode = "verification_environment.bootstrap_verification_failed";
    failed.candidateIdentity.reset();

    corruptRepository.corruptTransitions = {verified, failed};

    const auto view = readModel.verificationEnvironmentViewForRun(runId);
    req(!view.success(), "Two resolving transitions for the same Run were silently accepted");
    req(view.error()->code == "verification_environment.corrupt_bootstrap_record",
        "Expected corrupt_bootstrap_record for more than one resolving transition");
}

// ---------------------------------------------------------------------
// QML-lint advisory-severity policy (2026-09-28): ExecutionManifestView::
// verificationAdvisory, sourced from ManifestWorkLedgerEntry::verification -
// the one Backend-owned projection, never a second persistence model or a
// QML-side reclassification.
// ---------------------------------------------------------------------

ManifestVerificationDiagnostic makeAdvisoryDiagnostic(std::string fileKey)
{
    ManifestVerificationDiagnosticFinding finding;
    finding.fileKey = std::move(fileKey);
    finding.line = 2;
    finding.column = 1;
    finding.tag = "unqualified";
    finding.text = "Unqualified access";
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = "verification.qml_warning_advisory";
    diagnostic.message =
        "QML lint emitted one or more advisory-severity warnings that do not block this check";
    diagnostic.requirementId = "lint";
    diagnostic.structuredFindings = {finding};
    return diagnostic;
}

ManifestProposalSetVerification makePassedVerificationWithAdvisory(
    ManifestVerificationDiagnostic advisory)
{
    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = "project-1";
    verification.baselineFingerprint = "baseline-1";
    verification.overlayFingerprint = "overlay-1";
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;
    verification.diagnostics = {std::move(advisory)};
    verification.criteria = {{"lint", true, ExecutionVerificationStatus::Passed, {}}};
    return verification;
}

void testVerificationAdvisoryProjectsSafeBoundedFindingsWhenPassed()
{
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    InMemoryManifestWorkLedgerRepository workLedger;
    ExecutionManifestService manifests(manifestRepository, runs);
    ExecutionManifestReadModelService readModel(
        manifests, taskExecutions, nullptr, nullptr, nullptr, nullptr, &workLedger);

    const RunId runId = "run-advisory";
    Run run;
    run.id = runId;
    run.projectId = "project-1";
    run.title = "Advisory read-model test run";
    run.task = "Exercise the advisory projection";
    run.workflowId = "execution-manifest-v1";
    req(runs.insertRun(run).success(), "Could not seed Run");

    ExecutionManifest manifest;
    manifest.runId = runId;
    manifest.tasks = {makeTask("component")};
    req(manifests.freeze(manifest, ExecutionManifestPolicy {}).success(),
        "Could not freeze the test manifest");

    const auto verification = makePassedVerificationWithAdvisory(
        makeAdvisoryDiagnostic("<candidate>/qml/AlertsView.qml"));
    ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = "project-1";
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.verification = verification;
    req(workLedger.insertEntry(entry).success(), "Could not seed the work ledger entry");

    const auto view = readModel.viewForRun(runId);
    req(view.success(), "viewForRun failed");
    req(view.value().has_value(), "Expected a view for a frozen manifest");
    const auto& advisoryView = view.value()->verificationAdvisory;
    req(advisoryView.verificationAvailable,
        "verificationAvailable must be true once a verification identity exists");
    req(advisoryView.verificationPassed,
        "verificationPassed must mirror the authoritative requiredAcceptancePassed() result");
    req(advisoryView.hasAdvisories && advisoryView.advisoryCount == 1,
        "Exactly one advisory finding must be projected");
    req(advisoryView.advisories.size() == 1
            && advisoryView.advisories.front().relativePath == "qml/AlertsView.qml"
            && advisoryView.advisories.front().line == 2
            && advisoryView.advisories.front().column == 1
            && advisoryView.advisories.front().tag == "unqualified"
            && advisoryView.advisories.front().text == "Unqualified access"
            && advisoryView.advisories.front().requirementId.has_value()
            && *advisoryView.advisories.front().requirementId == "lint",
        "The advisory finding must be projected with a safe relative path and its full detail, "
        "never an absolute path");
}

void testVerificationAdvisoryFailsClosedOnUnsafeFindingPath()
{
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    InMemoryManifestTaskExecutionRepository taskExecutions;
    InMemoryManifestWorkLedgerRepository workLedger;
    ExecutionManifestService manifests(manifestRepository, runs);
    ExecutionManifestReadModelService readModel(
        manifests, taskExecutions, nullptr, nullptr, nullptr, nullptr, &workLedger);

    const RunId runId = "run-advisory-unsafe";
    Run run;
    run.id = runId;
    run.projectId = "project-1";
    run.title = "Advisory unsafe-path test run";
    run.task = "Exercise the fail-closed unsafe-path guard";
    run.workflowId = "execution-manifest-v1";
    req(runs.insertRun(run).success(), "Could not seed Run");

    ExecutionManifest manifest;
    manifest.runId = runId;
    manifest.tasks = {makeTask("component")};
    req(manifests.freeze(manifest, ExecutionManifestPolicy {}).success(),
        "Could not freeze the test manifest");

    // No safe "<candidate>/" prefix at all - displayQmlLintFindingPath()
    // cannot resolve a candidate-relative path from this fileKey.
    const auto verification = makePassedVerificationWithAdvisory(
        makeAdvisoryDiagnostic("diagnostic source unavailable"));
    ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = "project-1";
    entry.terminalStatus = RunStatus::Completed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.verification = verification;
    req(workLedger.insertEntry(entry).success(), "Could not seed the work ledger entry");

    const auto view = readModel.viewForRun(runId);
    req(!view.success(),
        "An advisory finding with no safe candidate-relative path must fail the whole read "
        "closed, never silently report zero advisories");
    req(view.error()->code == "execution_manifest_read_model.verification_corrupt",
        "Expected the stable verification_corrupt code, not a generic failure");
}

void testVerificationAdvisoryAbsentWithoutWorkLedgerRepository()
{
    // Every other test in this file constructs Fixture::readModel without a
    // 7th (IManifestWorkLedgerRepository) argument - this proves that
    // backward-compatible default explicitly, rather than only implicitly.
    Fixture fx;
    const RunId runId = "run-no-ledger";
    fx.seedRun(runId);
    ExecutionManifest manifest;
    manifest.runId = runId;
    manifest.tasks = {makeTask("component")};
    req(fx.manifests.freeze(manifest, ExecutionManifestPolicy {}).success(),
        "Could not freeze the test manifest");

    const auto view = fx.readModel.viewForRun(runId);
    req(view.success() && view.value().has_value(), "viewForRun failed");
    req(!view.value()->verificationAdvisory.verificationAvailable,
        "Without a wired IManifestWorkLedgerRepository, verificationAdvisory must stay "
        "unavailable - never fabricated");
}

// ---------------------------------------------------------------------------
// AG-17.6: routing projection (task -> assignee -> provider/model; roster members
// with their participation). Everything the UI shows comes from here.
// ---------------------------------------------------------------------------

StageModelAssignment rosterMember(const char* id, const char* role, bool lead, bool workspace,
    const char* providerId, const char* modelId, std::uint32_t position)
{
    StageModelAssignment value;
    value.stageType = id;
    value.role = role;
    value.leadArchitect = lead;
    value.workspaceAccess = workspace;
    value.providerId = providerId;
    value.modelId = modelId;
    value.position = position;
    value.timeout = std::chrono::milliseconds(60000);
    return value;
}

std::vector<StageModelAssignment> projectionRoster()
{
    return {
        rosterMember("architect", "Lead Architect", true, false, "p-openai", "m-lead", 0),
        rosterMember("qml-worker", "QML", false, true, "p-anthropic", "m-claude", 1),
        rosterMember("cpp-worker", "C++", false, true, "p-openai", "m-gpt", 2),
        rosterMember("doc-writer", "Docs", false, false, "p-openai", "m-gpt", 3)};
}

void seedRoutingCatalog(InMemoryProviderRepository& providers, InMemoryModelRepository& models)
{
    Provider openai;
    openai.id = "p-openai";
    openai.name = "OpenAI";
    openai.kind = "openai";
    Provider anthropic;
    anthropic.id = "p-anthropic";
    anthropic.name = "Anthropic";
    anthropic.kind = "anthropic";
    req(providers.insert(openai).success() && providers.insert(anthropic).success(),
        "test fixture: providers");
    Model lead;
    lead.id = "m-lead";
    lead.providerId = "p-openai";
    lead.apiModelId = "lead-api";
    lead.displayName = "Lead model";
    Model gpt;
    gpt.id = "m-gpt";
    gpt.providerId = "p-openai";
    gpt.apiModelId = "gpt-api";
    gpt.displayName = ""; // falls back to the API id
    Model claude;
    claude.id = "m-claude";
    claude.providerId = "p-anthropic";
    claude.apiModelId = "claude-api";
    claude.displayName = "Claude X";
    req(models.insert(lead).success() && models.insert(gpt).success()
            && models.insert(claude).success(),
        "test fixture: models");
}

ExecutionManifestTask routedTask(const std::string& id, const char* assignee,
    WorkspaceCapability capability = WorkspaceCapability::Propose)
{
    auto task = makeTask(id);
    task.requestedCapability = capability;
    if (assignee != nullptr) task.assigneeMemberId = assignee;
    return task;
}

void freezeWithRouting(ExecutionManifestService& manifests, const RunId& runId,
    std::optional<ManifestRoutingRecord> routing, std::vector<ExecutionManifestTask> tasks)
{
    ExecutionManifest manifest;
    manifest.runId = runId;
    manifest.routing = std::move(routing);
    manifest.tasks = std::move(tasks);
    const auto frozen = manifests.freeze(manifest, ExecutionManifestPolicy {});
    req(frozen.success(), "Could not freeze the routing projection manifest");
}

const ManifestRouteMemberView* findMemberView(
    const ExecutionManifestView& view, const std::string& memberId)
{
    for (const auto& member : view.routingMembers) {
        if (member.memberId == memberId) return &member;
    }
    return nullptr;
}

const ManifestTaskView* findTaskView(const ExecutionManifestView& view, const std::string& taskId)
{
    for (const auto& task : view.tasks) {
        if (task.id == taskId) return &task;
    }
    return nullptr;
}

void testRosterRoutingProjectionClassifiesEveryMembersParticipation()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    seedRoutingCatalog(providers, models);
    Fixture fx;
    ExecutionManifestReadModelService readModel(
        fx.manifests, fx.taskExecutions, nullptr, nullptr, nullptr, nullptr, nullptr,
        &providers, &models);
    const RunId runId = "run-routing";
    fx.seedRun(runId);
    // qml-worker owns two tasks; cpp-worker owns none (available but unused);
    // doc-writer has no workspace access (never assignable); one coordination task.
    freezeWithRouting(fx.manifests, runId, ManifestRouting::fromAssignments(projectionRoster()),
        {routedTask("t-ui", "qml-worker"), routedTask("t-ui2", "qml-worker"),
         routedTask("t-gate", nullptr, WorkspaceCapability::None)});

    const auto result = readModel.viewForRun(runId);
    req(result.success() && result.value().has_value(), "The roster Run must project a view");
    const auto& view = *result.value();
    req(view.routingMode == "roster", "A roster Manifest must project the roster mode");
    req(view.routingMembers.size() == 4, "Every routing member must be projected, in record order");
    req(view.routingMembers[0].memberId == "architect" && view.routingMembers[3].memberId == "doc-writer",
        "Members must keep the frozen record order");

    const auto* lead = findMemberView(view, "architect");
    const auto* qml = findMemberView(view, "qml-worker");
    const auto* cpp = findMemberView(view, "cpp-worker");
    const auto* docs = findMemberView(view, "doc-writer");
    req(lead && qml && cpp && docs, "A routing member is missing from the projection");
    req(lead->participation == "lead" && lead->lead && lead->assignedTaskCount == 0,
        "The lead plans and is never an assignee");
    req(qml->participation == "assigned" && qml->assignedTaskCount == 2,
        "A worker with tasks is assigned and counts them");
    req(cpp->participation == "unused" && cpp->assignedTaskCount == 0 && cpp->workspaceAccess,
        "An available worker with no task is Unused - not an error");
    req(docs->participation == "not_assignable" && !docs->workspaceAccess,
        "A member without workspace access is not assignable, which is different from unused");

    req(qml->label == "Anthropic / Claude X" && qml->providerName == "Anthropic"
            && qml->modelName == "Claude X" && qml->providerId == "p-anthropic"
            && qml->modelId == "m-claude" && qml->role == "QML",
        "The projection must carry the provider/model names and the frozen role");
    req(cpp->label == "OpenAI / gpt-api",
        "A model without a display name must be labelled by its API id");

    const auto* ui = findTaskView(view, "t-ui");
    const auto* gate = findTaskView(view, "t-gate");
    req(ui && gate, "A task is missing from the projection");
    req(ui->assigneeMemberId == "qml-worker" && ui->assigneeRole == "QML"
            && ui->assigneeLabel == "Anthropic / Claude X",
        "A task must project its frozen assignee, role and provider/model label");
    req(gate->assigneeMemberId.empty() && gate->assigneeLabel.empty(),
        "A coordination task executes nothing and has no assignee");
}

void testRoutingProjectionFallsBackToIdsWithoutTheCatalog()
{
    Fixture fx;
    const RunId runId = "run-routing-ids";
    fx.seedRun(runId);
    freezeWithRouting(fx.manifests, runId, ManifestRouting::fromAssignments(projectionRoster()),
        {routedTask("t-ui", "qml-worker")});
    const auto result = fx.readModel.viewForRun(runId);
    req(result.success() && result.value().has_value(), "The view must still project");
    const auto* qml = findMemberView(*result.value(), "qml-worker");
    req(qml != nullptr && qml->label == "p-anthropic / m-claude"
            && qml->providerName == "p-anthropic" && qml->modelName == "m-claude",
        "Without the catalog the projection must fall back to the frozen ids, never fail");
    req(findTaskView(*result.value(), "t-ui")->assigneeLabel == "p-anthropic / m-claude",
        "The task label must use the same fallback");
}

void testSingleAssignmentProjectionNamesTheImplicitAssignee()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    seedRoutingCatalog(providers, models);
    Fixture fx;
    ExecutionManifestReadModelService readModel(
        fx.manifests, fx.taskExecutions, nullptr, nullptr, nullptr, nullptr, nullptr,
        &providers, &models);
    const RunId runId = "run-routing-single";
    fx.seedRun(runId);
    freezeWithRouting(fx.manifests, runId,
        ManifestRouting::fromAssignments(
            {rosterMember("architect", "Lead Architect", true, false, "p-openai", "m-lead", 0)}),
        {routedTask("t-a", nullptr), routedTask("t-b", nullptr)});
    const auto result = readModel.viewForRun(runId);
    req(result.success() && result.value().has_value(), "The single-assignment view must project");
    const auto& view = *result.value();
    req(view.routingMode == "single_assignment" && view.routingMembers.size() == 1,
        "A single-assignment Manifest projects one member");
    req(view.routingMembers[0].participation == "lead" && view.routingMembers[0].assignedTaskCount == 2,
        "The sole member is the implicit assignee of every provider-backed task");
    req(findTaskView(view, "t-a")->assigneeMemberId == "architect"
            && findTaskView(view, "t-a")->assigneeLabel == "OpenAI / Lead model",
        "A task without an explicit assignee resolves to the sole member");
}

void testManifestWithoutRoutingRecordProjectsNoRouting()
{
    Fixture fx;
    const RunId runId = "run-routing-none";
    fx.seedRun(runId);
    freezeWithRouting(fx.manifests, runId, std::nullopt, {routedTask("t-a", nullptr)});
    const auto result = fx.readModel.viewForRun(runId);
    req(result.success() && result.value().has_value(), "A pre-AG-17.1 Manifest must still project");
    req(result.value()->routingMode == "none" && result.value()->routingMembers.empty()
            && result.value()->tasks[0].assigneeMemberId.empty(),
        "A Manifest frozen before routing existed projects no routing at all");
}

// ---------------------------------------------------------------------------
// AG-17.7B: member-level usage/cost reconstruction in the view
// ---------------------------------------------------------------------------

ModelExecution usageRow(const char* id, std::uint32_t position, std::uint32_t attempt,
    const char* role, ModelExecutionStatus status, std::uint64_t input, std::uint64_t output,
    std::uint64_t cost, bool priceKnown = true)
{
    ModelExecution value;
    value.id = id;
    value.sourceId = "run-usage";
    value.position = position;
    value.attempt = attempt;
    value.role = role;
    value.status = status;
    value.inputTokens = input;
    value.outputTokens = output;
    value.costMicros = cost;
    value.costPriceKnown = priceKnown;
    return value;
}

void seedUsageRows(InMemoryModelExecutionRepository& repository, bool withStray)
{
    // projectionRoster(): architect@0, qml-worker@1, cpp-worker@2, doc-writer@3. The tasks
    // below: t-ui (qml) = 1, t-ui2 (qml, a repeat) = max member 3 + 1 + 0 = 4, so the last
    // position is 4 and the integration call is at 5.
    std::vector<ModelExecution> rows {
        usageRow("planner", 0, 1, "planner", ModelExecutionStatus::Completed, 1000, 500, 5000),
        usageRow("ui-a1", 1, 1, "QML", ModelExecutionStatus::Failed, 100, 0, 1000),
        usageRow("ui-a2", 1, 2, "QML", ModelExecutionStatus::Completed, 200, 50, 2000, false),
        usageRow("ui2", 4, 1, "QML", ModelExecutionStatus::Completed, 300, 60, 3000),
        usageRow("integration", 5, 1, "integration", ModelExecutionStatus::Completed, 400, 70, 4000)};
    if (withStray) {
        rows.push_back(usageRow("stray", 77, 1, "stray", ModelExecutionStatus::Failed, 5, 5, 50));
    }
    for (auto& value : rows) req(repository.insertExecution(std::move(value)).success(), "usage fixture");
}

void freezeUsageManifest(ExecutionManifestService& manifests)
{
    freezeWithRouting(manifests, "run-usage", ManifestRouting::fromAssignments(projectionRoster()),
        {routedTask("t-ui", "qml-worker"), routedTask("t-ui2", "qml-worker"),
         routedTask("t-gate", nullptr, WorkspaceCapability::None)});
}

void testRosterViewProjectsPerMemberUsageAndTheExactPartition()
{
    InMemoryModelExecutionRepository executions;
    seedUsageRows(executions, /*withStray=*/false);
    Fixture fx;
    ExecutionManifestReadModelService readModel(fx.manifests, fx.taskExecutions, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, &executions);
    fx.seedRun("run-usage");
    freezeUsageManifest(fx.manifests);

    const auto result = readModel.viewForRun("run-usage");
    req(result.success() && result.value().has_value(), "The roster Run must project a view");
    const auto& view = *result.value();
    req(view.usageAvailable && view.usagePartitionExact && view.usageDiagnostics.empty()
            && view.usageUnattributed.executions == 0 && view.usageUnattributedExecutions.empty(),
        "A healthy Run attributes every row with an exact partition and no diagnostics");
    req(view.usageRunTotal.executions == 5 && view.usageRunTotal.inputTokens == 2000,
        "The Run total counts every row");

    const auto* lead = findMemberView(view, "architect");
    const auto* qml = findMemberView(view, "qml-worker");
    const auto* cpp = findMemberView(view, "cpp-worker");
    req(lead && qml && cpp, "A member is missing");
    req(lead->usage.executions == 2 && lead->usage.inputTokens == 1400 && lead->usage.costMicros == 9000,
        "The lead's usage is its Planner and integration executions");
    req(qml->usage.executions == 3 && qml->usage.failed == 1 && qml->usage.completed == 2
            && qml->usage.inputTokens == 600 && qml->usage.outputTokens == 110
            && qml->usage.costMicros == 6000,
        "A worker's usage is every attempt of its tasks, including the failed one and the "
        "repeated task's slot");
    req(qml->usage.unknownPriceExecutions == 1
            && qml->usage.priceState() == ManifestUsagePriceState::Partial,
        "An unpriced attempt must make the worker's cost partial, not known");
    req(cpp->usage.executions == 0 && cpp->usage.priceState() == ManifestUsagePriceState::None
            && cpp->participation == "unused",
        "An unused worker has no executions - and no known price either");
}

void testRosterViewReportsUnattributedRowsAsDebtWithoutLosingThem()
{
    InMemoryModelExecutionRepository executions;
    seedUsageRows(executions, /*withStray=*/true);
    Fixture fx;
    ExecutionManifestReadModelService readModel(fx.manifests, fx.taskExecutions, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, &executions);
    fx.seedRun("run-usage");
    freezeUsageManifest(fx.manifests);

    const auto result = readModel.viewForRun("run-usage");
    req(result.success() && result.value().has_value(), "The view must project");
    const auto& view = *result.value();
    req(view.usageAvailable && view.usagePartitionExact && view.usageRunTotal.executions == 6,
        "The partition stays exact with the stray row counted in the Run total");
    req(view.usageUnattributed.executions == 1 && view.usageUnattributed.failed == 1
            && view.usageUnattributedExecutions.size() == 1
            && view.usageUnattributedExecutions[0].id == "stray"
            && view.usageUnattributedExecutions[0].position == 77,
        "A row matching no bucket must be listed, never lost");
    req(!view.usageDiagnostics.empty()
            && view.usageDiagnostics[0].rfind("usage.unattributed: 1 execution(s)", 0) == 0,
        "Unattributed rows are surfaced as diagnostic debt");
    req(findMemberView(view, "qml-worker")->usage.executions == 3,
        "The stray row must not disturb the attribution of the real members");
}

void testUsageIsUnavailableWithoutTheRepositoryOrARoutingRecord()
{
    // No execution repository wired: every other projection is unchanged.
    {
        InMemoryProviderRepository providers;
        InMemoryModelRepository models;
        seedRoutingCatalog(providers, models);
        Fixture fx;
        ExecutionManifestReadModelService readModel(fx.manifests, fx.taskExecutions, nullptr, nullptr,
            nullptr, nullptr, nullptr, &providers, &models);
        fx.seedRun("run-usage");
        freezeUsageManifest(fx.manifests);
        const auto result = readModel.viewForRun("run-usage");
        req(result.success() && result.value().has_value() && !result.value()->usageAvailable
                && result.value()->routingMode == "roster"
                && findMemberView(*result.value(), "qml-worker")->usage.executions == 0,
            "Without the repository usage is unavailable but routing is unaffected");
    }
    // A Manifest frozen before routing records existed has nothing to attribute to.
    {
        InMemoryModelExecutionRepository executions;
        seedUsageRows(executions, false);
        Fixture fx;
        ExecutionManifestReadModelService readModel(fx.manifests, fx.taskExecutions, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, &executions);
        fx.seedRun("run-usage");
        freezeWithRouting(fx.manifests, "run-usage", std::nullopt, {routedTask("t-a", nullptr)});
        const auto result = readModel.viewForRun("run-usage");
        req(result.success() && result.value().has_value() && !result.value()->usageAvailable
                && result.value()->usageDiagnostics.empty(),
            "A Manifest without a routing record has no usage attribution");
    }
}

class FailingModelExecutionRepository final : public IModelExecutionRepository {
public:
    common::Result<ModelExecution> insertExecution(ModelExecution) override { return fail<ModelExecution>(); }
    common::Result<ModelExecution> updateExecution(ModelExecution) override { return fail<ModelExecution>(); }
    common::Result<std::optional<ModelExecution>> findExecution(const ModelExecutionId&) const override
    {
        return fail<std::optional<ModelExecution>>();
    }
    common::Result<std::vector<ModelExecution>> listExecutions(
        ModelExecutionSource, const std::string&) const override
    {
        return fail<std::vector<ModelExecution>>();
    }
    common::Result<std::vector<ModelExecution>> listProjectExecutions(const ProjectId&) const override
    {
        return fail<std::vector<ModelExecution>>();
    }
    common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const override
    {
        return fail<std::vector<ModelExecution>>();
    }
    common::Result<QualityEvaluation> insertEvaluation(QualityEvaluation) override
    {
        return fail<QualityEvaluation>();
    }
    common::Result<std::optional<QualityEvaluation>> findEvaluation(const QualityEvaluationId&) const override
    {
        return fail<std::optional<QualityEvaluation>>();
    }
    common::Result<std::vector<QualityEvaluation>> listEvaluations(const ModelExecutionId&) const override
    {
        return fail<std::vector<QualityEvaluation>>();
    }

private:
    template <typename T>
    static common::Result<T> fail()
    {
        return common::Result<T>::failure({"persistence.read_failed", "usage rows unreadable"});
    }
};

void testAUsageReadFailureDegradesOnlyTheUsageSection()
{
    FailingModelExecutionRepository executions;
    Fixture fx;
    ExecutionManifestReadModelService readModel(fx.manifests, fx.taskExecutions, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, &executions);
    fx.seedRun("run-usage");
    freezeUsageManifest(fx.manifests);
    const auto result = readModel.viewForRun("run-usage");
    req(result.success() && result.value().has_value(),
        "An unreadable ledger must not take the whole Manifest view down");
    const auto& view = *result.value();
    req(!view.usageAvailable && view.routingMode == "roster" && view.routingMembers.size() == 4
            && view.tasks.size() == 3,
        "Only the usage section degrades; routing and tasks still project");
    req(view.usageDiagnostics.size() == 1
            && view.usageDiagnostics[0] == "usage.unavailable: persistence.read_failed",
        "The degraded usage section must say why");
}

} // namespace

int runExecutionManifestReadModelServiceTests()
{
    try {
        testViewProjectsScopeInspectedAndProposedPathsWithoutDuplicates();
        testViewWithoutWorkspaceScopeRepositoryStaysBackwardCompatible();
        testVerificationEnvironmentViewIsAbsentWithoutSnapshot();
        testVerificationEnvironmentViewProjectsExistingProfile();
        testVerificationEnvironmentViewReclassifiesProposedBootstrap();
        testVerificationEnvironmentViewSetupRequiredAllowsRetryAfterSetup();
        testVerificationEnvironmentViewClassifiesDrift();
        testVerificationEnvironmentViewClassifiesToolUnavailableFromUnconfirmedBuildPresets();
        testVerificationEnvironmentViewFailsClosedOnIdentityMismatch();
        testVerificationEnvironmentViewFailsClosedOnMultipleResolvingTransitions();
        testVerificationAdvisoryProjectsSafeBoundedFindingsWhenPassed();
        testVerificationAdvisoryFailsClosedOnUnsafeFindingPath();
        testVerificationAdvisoryAbsentWithoutWorkLedgerRepository();
        testRosterRoutingProjectionClassifiesEveryMembersParticipation();
        testRoutingProjectionFallsBackToIdsWithoutTheCatalog();
        testSingleAssignmentProjectionNamesTheImplicitAssignee();
        testManifestWithoutRoutingRecordProjectsNoRouting();
        testRosterViewProjectsPerMemberUsageAndTheExactPartition();
        testRosterViewReportsUnattributedRowsAsDebtWithoutLosingThem();
        testUsageIsUnavailableWithoutTheRepositoryOrARoutingRecord();
        testAUsageReadFailureDegradesOnlyTheUsageSection();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ExecutionManifestReadModelService tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
