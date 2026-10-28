// Unit tests for AG-18.3's ManifestBootstrapValidator (the manifest-level
// same-Run bootstrap admission predicate) and
// ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements (the
// candidate-overlay resolver for requirements deferred pending that
// bootstrap). Pure function tests over already-constructed ExecutionManifest/
// VerificationEnvironmentSnapshot values - no HTTP, no SQLite, no filesystem.
#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/verification/ManifestBootstrapValidator.hpp"
#include "ai/application/verification/ManifestVerificationProfileBinder.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

ExecutionManifestTask bootstrapOwningTask(bool existingFile)
{
    ExecutionManifestTask task;
    task.id = "bootstrap";
    task.objective = "Create the CMake user preset";
    task.role = "worker";
    task.requestedCapability = WorkspaceCapability::Propose;
    if (existingFile) task.ownedPaths = {"CMakeUserPresets.json"};
    else task.plannedCreatePaths = {"CMakeUserPresets.json"};
    return task;
}

ExecutionAcceptanceRequirement deferredCMakeBuildRequirement()
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "build";
    requirement.description = "The bootstrapped project builds";
    requirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    requirement.required = true;
    requirement.verificationBootstrapPending = true;
    return requirement;
}

ExecutionManifestTask dependentTask(bool withDependencyEdge)
{
    ExecutionManifestTask task;
    task.id = "build-task";
    task.objective = "Build the project";
    task.role = "worker";
    task.requestedCapability = WorkspaceCapability::Propose;
    task.ownedPaths = {"src/app.cpp"};
    if (withDependencyEdge) task.dependencies = {"bootstrap"};
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(deferredCMakeBuildRequirement());
    task.acceptanceContract = std::move(contract);
    return task;
}

ExecutionManifest validBootstrapManifest()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = {bootstrapOwningTask(/*existingFile=*/false), dependentTask(/*withDependencyEdge=*/true)};
    ManifestBootstrapDeclaration declaration;
    declaration.bootstrapTaskId = "bootstrap";
    declaration.setupPaths = {"CMakeUserPresets.json"};
    manifest.bootstrapDeclaration = std::move(declaration);
    return manifest;
}

// ---------------------------------------------------------------------
// ManifestBootstrapValidator - admission predicate
// ---------------------------------------------------------------------

void testValidBootstrapDeclarationIsAdmitted()
{
    const auto manifest = validBootstrapManifest();
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(findings.empty(), "A safe, correctly-declared bootstrap was rejected");
}

void testDeferredFlagWithoutDeclarationIsRejected()
{
    auto manifest = validBootstrapManifest();
    manifest.bootstrapDeclaration.reset();
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A deferred requirement without a bootstrapDeclaration was not rejected - absence of "
        "declaration must never be inferred as consent");
}

void testDeclarationWithoutAnyDeferredRequirementIsRejected()
{
    auto manifest = validBootstrapManifest();
    manifest.tasks[1].acceptanceContract->requirements.front().verificationBootstrapPending = false;
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrapDeclaration with no deferred requirement referencing it was accepted");
}

void testUnsupportedSetupFilenameIsRejected()
{
    auto manifest = validBootstrapManifest();
    manifest.bootstrapDeclaration->setupPaths = {"CMakeLists.txt"};
    manifest.tasks[0].plannedCreatePaths = {"CMakeLists.txt"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "An unsupported bootstrap setup filename was accepted");
}

void testSubdirectorySetupPathIsRejected()
{
    auto manifest = validBootstrapManifest();
    manifest.bootstrapDeclaration->setupPaths = {"cmake/CMakeUserPresets.json"};
    manifest.tasks[0].plannedCreatePaths = {"cmake/CMakeUserPresets.json"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A subdirectory bootstrap setup path was accepted - only repository-root files are "
        "supported");
}

void testExistingFileRequiresOwnedPathsNotPlannedCreate()
{
    auto manifest = validBootstrapManifest();
    // The bootstrap task declares the file as a NEW file (plannedCreatePaths)
    // but never actually places it in either role for this variant - the
    // predicate must reject it as neither owned nor planned-created.
    manifest.tasks[0].plannedCreatePaths.clear();
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup file declared in neither ownedPaths nor plannedCreatePaths was "
        "accepted");
}

void testMissingDependencyEdgeIsRejected()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = {bootstrapOwningTask(false), dependentTask(/*withDependencyEdge=*/false)};
    ManifestBootstrapDeclaration declaration;
    declaration.bootstrapTaskId = "bootstrap";
    declaration.setupPaths = {"CMakeUserPresets.json"};
    manifest.bootstrapDeclaration = std::move(declaration);
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_dependency_invalid",
        "A dependent task missing an explicit dependency edge on the bootstrap task was accepted");
}

void testUnknownBootstrapTaskIdIsRejected()
{
    auto manifest = validBootstrapManifest();
    manifest.bootstrapDeclaration->bootstrapTaskId = "does-not-exist";
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "An unknown bootstrapTaskId was accepted");
}

// ---------------------------------------------------------------------
// AG-18.3 hardening (blocker 5): a bootstrap setup path must appear
// EXACTLY ONCE across the WHOLE Manifest - never in any scope-role list of
// any OTHER task, checked across all six role lists, not just the
// bootstrap-owning task's own. Each test below plants the exact same
// setup path ("CMakeUserPresets.json") in one forbidden role of a THIRD,
// unrelated task and confirms rejection.
// ---------------------------------------------------------------------

ExecutionManifest bootstrapManifestWithMasqueradingThirdTask()
{
    auto manifest = validBootstrapManifest();
    ExecutionManifestTask thirdTask;
    thirdTask.id = "unrelated-task";
    thirdTask.objective = "Do something unrelated";
    thirdTask.role = "worker";
    thirdTask.requestedCapability = WorkspaceCapability::Inspect;
    manifest.tasks.push_back(std::move(thirdTask));
    return manifest;
}

void testMasqueradeViaOtherTaskOwnedPathsIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().ownedPaths = {"CMakeUserPresets.json"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup path also claimed via another task's ownedPaths was accepted");
}

void testMasqueradeViaOtherTaskPlannedCreatePathsIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().plannedCreatePaths = {"CMakeUserPresets.json"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup path also claimed via another task's plannedCreatePaths was accepted");
}

void testMasqueradeViaOtherTaskRequiredReadPathsIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().requiredReadPaths = {"CMakeUserPresets.json"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup path also claimed via another task's requiredReadPaths was accepted");
}

void testMasqueradeViaOtherTaskReferencePathsIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().referencePaths = {"CMakeUserPresets.json"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup path also claimed via another task's referencePaths was accepted");
}

void testMasqueradeViaOtherTaskExcludedPathsIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().excludedPaths = {{"CMakeUserPresets.json", "Not part of this task"}};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup path also claimed via another task's excludedPaths was accepted");
}

void testMasqueradeViaOtherTaskDeferredPathsIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().deferredPaths = {{"CMakeUserPresets.json", "Deferred to a later stage"}};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A bootstrap setup path also claimed via another task's deferredPaths was accepted");
}

// A differently-formatted but logically identical path (mixed separators,
// different case) must be caught by the same normalized comparison, not
// only a byte-identical string.
void testMasqueradeViaNormalizedEquivalentPathIsRejected()
{
    auto manifest = bootstrapManifestWithMasqueradingThirdTask();
    manifest.tasks.back().ownedPaths = {"CMAKEUSERPRESETS.JSON"};
    const auto findings = ManifestBootstrapValidator::validate(manifest);
    require(!findings.empty()
            && findings.front().code == "verification_environment.bootstrap_scope_invalid",
        "A case-different but normalized-equivalent masquerading path was accepted");
}

// ---------------------------------------------------------------------
// ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements
// ---------------------------------------------------------------------

VerificationEnvironmentSnapshot singleProfileCandidateSnapshot()
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.state = VerificationEnvironmentState::ExistingProfile;
    VerificationProfile profile;
    profile.profileId = "local-qt-debug";
    profile.configurePresetName = "local-qt-debug";
    profile.resolvedBinaryDir = "build/local-qt-debug";
    profile.buildPresetsConfirmed = true;
    VerificationBuildPresetDescriptor buildPreset;
    buildPreset.name = "local-qt-debug";
    buildPreset.declaredConfigurePreset = "local-qt-debug";
    // AG-18.3 hardening (blocker 3): allowedTargets is now the REAL
    // confirmed catalog (potentially large); declaredTargets is the
    // build preset's own requested/hint set the binder intersects
    // against it to find the single QML-lint-eligible target.
    buildPreset.declaredTargets = {"all_qmllint"};
    buildPreset.allowedTargetsConfirmed = true;
    buildPreset.allowedTargets = {"all_qmllint", "all", "app"};
    profile.buildPresets.push_back(buildPreset);
    snapshot.profiles.push_back(profile);
    return snapshot;
}

ExecutionManifest manifestWithDeferredQmlLint()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = {bootstrapOwningTask(false)};
    ExecutionManifestTask task;
    task.id = "lint-task";
    task.objective = "Lint QML";
    task.role = "worker";
    task.requestedCapability = WorkspaceCapability::Propose;
    task.dependencies = {"bootstrap"};
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "lint";
    requirement.description = "QML lint is clean";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.required = true;
    requirement.verificationBootstrapPending = true;
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(requirement);
    task.acceptanceContract = std::move(contract);
    manifest.tasks.push_back(std::move(task));
    ManifestBootstrapDeclaration declaration;
    declaration.bootstrapTaskId = "bootstrap";
    declaration.setupPaths = {"CMakeUserPresets.json"};
    manifest.bootstrapDeclaration = std::move(declaration);
    return manifest;
}

void testCandidateBindingResolvesQmlLintFromSingleProfile()
{
    auto manifest = manifestWithDeferredQmlLint();
    const auto snapshot = singleProfileCandidateSnapshot();
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(bound.success() && findings.empty(),
        "A single-profile/single-build-preset/single-target candidate was rejected");
    require(bound.value().candidateProfileId == "local-qt-debug"
            && bound.value().candidateBuildProfileId == "local-qt-debug"
            && bound.value().candidateTargetId == "all_qmllint",
        "Candidate identity did not record the exact resolved profile/build/target");
    const auto& requirement = manifest.tasks[1].acceptanceContract->requirements.front();
    require(!requirement.verificationBootstrapPending,
        "verificationBootstrapPending was not cleared after successful candidate binding");
    require(requirement.verificationArguments.size() == 2
            && requirement.verificationArguments[0].name == "buildDirectory"
            && requirement.verificationArguments[0].value == "build/local-qt-debug"
            && requirement.verificationArguments[1].name == "target"
            && requirement.verificationArguments[1].value == "all_qmllint",
        "qml_lint did not resolve to the exact candidate-discovered buildDirectory/target");
}

void testCandidateBindingRejectsAmbiguousMultiProfileCandidate()
{
    auto manifest = manifestWithDeferredQmlLint();
    auto snapshot = singleProfileCandidateSnapshot();
    auto secondProfile = snapshot.profiles.front();
    secondProfile.profileId = "second-profile";
    secondProfile.configurePresetName = "second-profile";
    snapshot.profiles.push_back(secondProfile);
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(!bound.success()
            && bound.error()->code == "verification_environment.bootstrap_candidate_invalid",
        "A candidate overlay rediscovering more than one profile was silently disambiguated "
        "instead of rejected");
}

void testCandidateBindingRejectsSetupRequiredCandidate()
{
    auto manifest = manifestWithDeferredQmlLint();
    VerificationEnvironmentSnapshot empty;
    empty.state = VerificationEnvironmentState::SetupRequired;
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, empty, findings);
    require(!bound.success()
            && bound.error()->code == "verification_environment.bootstrap_candidate_invalid",
        "A candidate overlay that failed to rediscover any profile (the bootstrap proposal did "
        "not actually create a usable preset) was accepted");
}

void testCandidateBindingRejectsAmbiguousTarget()
{
    auto manifest = manifestWithDeferredQmlLint();
    auto snapshot = singleProfileCandidateSnapshot();
    // Two DECLARED (requested) targets both present in the real confirmed
    // catalog - genuinely ambiguous, since neither is more "the" QML-lint
    // target than the other.
    auto& buildPreset = snapshot.profiles.front().buildPresets.front();
    buildPreset.declaredTargets.push_back("second_target");
    buildPreset.allowedTargets.push_back("second_target");
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(!bound.success()
            && bound.error()->code == "verification_environment.bootstrap_target_ambiguous",
        "A candidate build preset with two eligible declared/confirmed targets was silently "
        "disambiguated for qml_lint instead of rejected");
}

void testCandidateBindingRejectsUnconfirmedDeclaredTarget()
{
    auto manifest = manifestWithDeferredQmlLint();
    auto snapshot = singleProfileCandidateSnapshot();
    // A declared target that the real configured-target probe never
    // actually confirmed must never be trusted merely because the
    // provider-proposed preset JSON claimed it.
    auto& buildPreset = snapshot.profiles.front().buildPresets.front();
    buildPreset.declaredTargets = {"invented_target"};
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(!bound.success()
            && bound.error()->code == "verification_environment.bootstrap_target_ambiguous",
        "A declared target absent from the real confirmed catalog was accepted");
}

void testCandidateBindingIgnoresUnrelatedRealTargets()
{
    // The base fixture already carries "all"/"app" as real-but-unrequested
    // targets alongside the single declared+confirmed "all_qmllint" -
    // proves an unrelated real target never makes an otherwise
    // unambiguous declared target invalid.
    auto manifest = manifestWithDeferredQmlLint();
    const auto snapshot = singleProfileCandidateSnapshot();
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(bound.success() && bound.value().candidateTargetId == "all_qmllint",
        "Unrelated real configured targets incorrectly made an unambiguous declared target "
        "invalid");
}

void testCandidateBindingRejectsMissingBuildDirectory()
{
    auto manifest = manifestWithDeferredQmlLint();
    auto snapshot = singleProfileCandidateSnapshot();
    snapshot.profiles.front().resolvedBinaryDir.clear();
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(!bound.success()
            && bound.error()->code == "verification_environment.bootstrap_candidate_invalid",
        "qml_lint bound against a candidate with no candidate-relative build directory");
}

void testCandidateBindingResolvesCMakeBuildWithoutTargetRequirement()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = {bootstrapOwningTask(false), dependentTask(true)};
    const auto snapshot = singleProfileCandidateSnapshot();
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
        manifest, snapshot, findings);
    require(bound.success() && findings.empty(),
        "cmake_build (which needs no target) was rejected against a valid single-profile "
        "candidate");
    const auto& requirement = manifest.tasks[1].acceptanceContract->requirements.front();
    // Manifest verification reliability hardening: singleProfileCandidateSnapshot()'s
    // profile carries a non-empty resolvedBinaryDir, so the binder now also
    // appends a third best-effort buildDirectory argument.
    require(requirement.verificationArguments.size() == 3
            && requirement.verificationArguments[0].value == "local-qt-debug"
            && requirement.verificationArguments[1].value == "local-qt-debug"
            && requirement.verificationArguments[2].name == "buildDirectory"
            && requirement.verificationArguments[2].value == "build/local-qt-debug",
        "cmake_build did not resolve to the exact candidate preset/buildPreset/buildDirectory");
}

} // namespace

int runManifestBootstrapValidatorTests()
{
    try {
        testValidBootstrapDeclarationIsAdmitted();
        testDeferredFlagWithoutDeclarationIsRejected();
        testDeclarationWithoutAnyDeferredRequirementIsRejected();
        testUnsupportedSetupFilenameIsRejected();
        testSubdirectorySetupPathIsRejected();
        testExistingFileRequiresOwnedPathsNotPlannedCreate();
        testMissingDependencyEdgeIsRejected();
        testUnknownBootstrapTaskIdIsRejected();
        testMasqueradeViaOtherTaskOwnedPathsIsRejected();
        testMasqueradeViaOtherTaskPlannedCreatePathsIsRejected();
        testMasqueradeViaOtherTaskRequiredReadPathsIsRejected();
        testMasqueradeViaOtherTaskReferencePathsIsRejected();
        testMasqueradeViaOtherTaskExcludedPathsIsRejected();
        testMasqueradeViaOtherTaskDeferredPathsIsRejected();
        testMasqueradeViaNormalizedEquivalentPathIsRejected();
        testCandidateBindingResolvesQmlLintFromSingleProfile();
        testCandidateBindingRejectsAmbiguousMultiProfileCandidate();
        testCandidateBindingRejectsSetupRequiredCandidate();
        testCandidateBindingRejectsAmbiguousTarget();
        testCandidateBindingRejectsUnconfirmedDeclaredTarget();
        testCandidateBindingIgnoresUnrelatedRealTargets();
        testCandidateBindingRejectsMissingBuildDirectory();
        testCandidateBindingResolvesCMakeBuildWithoutTargetRequirement();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ManifestBootstrapValidator tests failed: " << exception.what() << '\n';
        return 1;
    }
}
