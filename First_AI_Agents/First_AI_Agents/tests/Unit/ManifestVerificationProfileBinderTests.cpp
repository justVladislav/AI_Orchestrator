// Unit tests for AG-18.2's ManifestVerificationProfileBinder: the single
// Backend-owned resolver between a Planner's draft profile/build/target
// selections and the exact, immutable verificationArguments the existing
// verification pipeline already consumes unchanged. No provider HTTP calls,
// no SQLite - pure function tests over already-constructed
// VerificationEnvironmentSnapshot/ExecutionManifest values.
#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/verification/ManifestVerificationProfileBinder.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// The exact real-world incident AG-18 exists to close: a Backend-discovered
// configure/build profile named "local-qt-debug", a candidate-relative
// build directory matching the real QtCreator-generated layout, and a
// confirmed "all_qmllint" target - the same shape
// VerificationEnvironmentDiscoveryService actually produces.
VerificationEnvironmentSnapshot regressionIncidentSnapshot(
    const RunId& runId = "run-1", const ProjectId& projectId = "project-1",
    const std::string& baselineFingerprint = "baseline-fp-1")
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = projectId;
    snapshot.baselineFingerprint = baselineFingerprint;
    snapshot.state = VerificationEnvironmentState::ExistingProfile;

    VerificationProfile profile;
    profile.profileId = "local-qt-debug";
    profile.configurePresetName = "local-qt-debug";
    profile.configureDisplayName = "Desktop Qt 6.11.1 Debug";
    profile.configureSource = VerificationPresetFileSource::UserPresets;
    profile.declaredBinaryDir = "${sourceDir}/build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug";
    profile.resolvedBinaryDir = "build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug";
    profile.origin = VerificationProfileOrigin::Discovered;
    profile.buildPresetsConfirmed = true;

    VerificationBuildPresetDescriptor buildPreset;
    buildPreset.name = "local-qt-debug";
    buildPreset.displayName = "Desktop Qt 6.11.1 Debug";
    buildPreset.source = VerificationPresetFileSource::UserPresets;
    buildPreset.declaredConfigurePreset = "local-qt-debug";
    buildPreset.allowedTargetsConfirmed = true;
    buildPreset.allowedTargets = {"all_qmllint", "all"};
    profile.buildPresets.push_back(buildPreset);

    snapshot.profiles.push_back(profile);
    snapshot.discoveredAt = std::chrono::system_clock::now();
    return snapshot;
}

ExecutionAcceptanceRequirement cmakeBuildRequirement(
    const std::string& profileId, const std::string& buildProfileId)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "build";
    requirement.description = "The project builds with the selected profile";
    requirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    requirement.verificationProfileId = profileId;
    requirement.verificationBuildProfileId = buildProfileId;
    requirement.required = true;
    return requirement;
}

ExecutionAcceptanceRequirement qmlLintRequirement(
    const std::string& profileId, const std::string& buildProfileId, const std::string& targetId)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "lint";
    requirement.description = "QML lint is warning-free";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationProfileId = profileId;
    requirement.verificationBuildProfileId = buildProfileId;
    requirement.verificationTargetId = targetId;
    requirement.required = true;
    return requirement;
}

ExecutionManifestTask taskWithRequirement(ExecutionAcceptanceRequirement requirement)
{
    ExecutionManifestTask task;
    task.id = "task-1";
    task.objective = "Exercise verification binding";
    task.role = "worker";
    task.requestedCapability = WorkspaceCapability::Propose;
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(std::move(requirement));
    task.acceptanceContract = std::move(contract);
    return task;
}

// ---------------------------------------------------------------------
// Section 1: snapshot identity validation (required tests 1-6)
// ---------------------------------------------------------------------

void testValidSnapshotIdentityPasses()
{
    const auto snapshot = regressionIncidentSnapshot();
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(result.success(), "A genuinely matching snapshot identity was rejected");
}

void testRunIdMismatchFails()
{
    const auto snapshot = regressionIncidentSnapshot("run-1", "project-1", "baseline-fp-1");
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-2", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.identity_mismatch",
        "A Run id mismatch was not rejected");
}

void testProjectIdMismatchFails()
{
    const auto snapshot = regressionIncidentSnapshot("run-1", "project-1", "baseline-fp-1");
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-2", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.identity_mismatch",
        "A project id mismatch was not rejected");
}

void testBaselineFingerprintMismatchFails()
{
    const auto snapshot = regressionIncidentSnapshot("run-1", "project-1", "baseline-fp-1");
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-DIFFERENT");
    require(!result.success() && result.error()->code == "verification_environment.baseline_mismatch",
        "A baseline fingerprint mismatch was not rejected");
}

void testUnsupportedSchemaVersionFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.schemaVersion = "verification-environment-snapshot.v0";
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "An unsupported schema version was not rejected");
}

void testStateContentMismatchFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.state = VerificationEnvironmentState::SetupRequired; // profiles still non-empty
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A state/content mismatch (SetupRequired with non-empty profiles) was not rejected");
}

void testDuplicateProfileIdFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.push_back(snapshot.profiles.front());
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A duplicate profile id was not rejected");
}

void testDuplicateTargetIdWithinProfileFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().buildPresets.front().allowedTargets.push_back("all_qmllint");
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A duplicate target id within one build profile was not rejected");
}

void testUnsafeBuildPathFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().resolvedBinaryDir = "../escape/outside/candidate";
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A path-traversing resolvedBinaryDir was not rejected");
}

// AG-18.2 hardening follow-up: declaredBinaryDir is the raw, unexpanded
// string as authored - it is legitimate for a real CMakePresets.json to
// declare an absolute binaryDir (AG-18.1 then leaves resolvedBinaryDir
// empty rather than guessing). Rejecting the snapshot outright for this
// would incorrectly block every such legitimate preset before the Planner
// ever runs, even though resolvedBinaryDir itself stays empty/safe and
// this binder never reads declaredBinaryDir for anything.
void testAbsoluteDeclaredBinaryDirWithSafeResolvedDoesNotFailIdentity()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().declaredBinaryDir = "C:/Users/someone/build/local-qt-debug";
    snapshot.profiles.front().resolvedBinaryDir.clear();
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(result.success(),
        "An absolute (unresolved) declaredBinaryDir must not fail identity validation - only "
        "resolvedBinaryDir needs to be a safe candidate-relative path");
}

void testOversizedDeclaredBinaryDirFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().declaredBinaryDir =
        std::string(kVerificationEnvironmentMaxRelativePathBytes + 1, 'a');
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "An unbounded declaredBinaryDir was not rejected");
}

void testProfileIdNotMatchingConfigurePresetNameFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().profileId = "some-other-id";
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A discovered profile whose id disagrees with its own configure preset name was accepted");
}

void testBuildPresetLinkedToWrongConfigurePresetFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().buildPresets.front().declaredConfigurePreset = "some-other-preset";
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A build preset linked to a different configure preset than its own profile was accepted");
}

void testUnconfirmedAllowListWithNonEmptyTargetsFails()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().buildPresets.front().allowedTargetsConfirmed = false;
    // allowedTargets is deliberately left non-empty ("all_qmllint", "all")
    // - an unconfirmed allow-list must never carry entries, since AG-18.1's
    // own contract treats an unconfirmed empty list as "unknown", not as
    // "these specific targets happen to be confirmed after all".
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "An unconfirmed target allow-list carrying target entries was accepted");
}

void testTooManyProfilesFails()
{
    auto snapshot = regressionIncidentSnapshot();
    const auto templateProfile = snapshot.profiles.front();
    snapshot.profiles.clear();
    for (std::size_t i = 0; i <= kVerificationEnvironmentMaxProfiles; ++i) {
        auto profile = templateProfile;
        profile.profileId = "profile-" + std::to_string(i);
        profile.configurePresetName = profile.profileId;
        profile.buildPresets.clear();
        snapshot.profiles.push_back(std::move(profile));
    }
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A snapshot exceeding the trusted profile ceiling was accepted");
}

void testTooManyTargetsPerBuildPresetFails()
{
    auto snapshot = regressionIncidentSnapshot();
    auto& targets = snapshot.profiles.front().buildPresets.front().allowedTargets;
    targets.clear();
    for (std::size_t i = 0; i <= kVerificationEnvironmentMaxTargetsPerPreset; ++i) {
        targets.push_back("target-" + std::to_string(i));
    }
    const auto result = ManifestVerificationProfileBinder::validateSnapshotIdentity(
        snapshot, "run-1", "project-1", "baseline-fp-1");
    require(!result.success() && result.error()->code == "verification_environment.snapshot_corrupt",
        "A snapshot exceeding the trusted per-preset target ceiling was accepted");
}

// ---------------------------------------------------------------------
// Section 2: successful binding, including the exact regression incident
// (required tests 7-12)
// ---------------------------------------------------------------------

void testCmakeBuildResolvesExactBackendValues()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        cmakeBuildRequirement("local-qt-debug", "local-qt-debug")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(bound.success() && findings.empty(), "A valid cmake_build selection was rejected");
    const auto& args = manifest.tasks.front().acceptanceContract->requirements.front()
        .verificationArguments;
    // Manifest verification reliability hardening: regressionIncidentSnapshot()'s
    // profile carries a non-empty resolvedBinaryDir (the exact diagnosed
    // "build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug" shape), so the binder
    // now also appends a third best-effort buildDirectory argument the
    // combined verifier's Windows path-budget preflight uses.
    require(args.size() == 3 && args[0].name == "preset" && args[0].value == "local-qt-debug"
            && args[1].name == "buildPreset" && args[1].value == "local-qt-debug"
            && args[2].name == "buildDirectory"
            && args[2].value == "build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug",
        "cmake_build did not resolve to the exact Backend-discovered preset/buildPreset/"
        "buildDirectory");
}

// The exact incident this whole gap exists to close: even when the
// provider's raw selection ids happen to be the same strings the real
// worker guessed (qt-debug/UI_Production_Dashboard_qml_lint would simply
// be REJECTED as unknown, never silently accepted) - here we prove the
// positive case: selecting the REAL advertised ids resolves to the REAL
// Backend values, never something a provider could have substituted.
void testQmlLintRegressionIncidentResolvesExactCandidateRelativeValues()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        qmlLintRequirement("local-qt-debug", "local-qt-debug", "all_qmllint")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(bound.success() && findings.empty(), "The regression-incident qml_lint selection was rejected");
    const auto& args = manifest.tasks.front().acceptanceContract->requirements.front()
        .verificationArguments;
    require(args.size() == 2 && args[0].name == "buildDirectory"
            && args[0].value == "build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug"
            && args[1].name == "target" && args[1].value == "all_qmllint",
        "qml_lint did not resolve to the exact candidate-relative build directory and target");
    require(manifest.tasks.front().acceptanceContract->requirements.front()
                .qmlLintDiagnosticPolicyId
            == std::optional<std::string>(
                std::string(kQmlLintDiagnosticPolicyAdvisoryV1)),
        "qml_lint binding did not freeze the Backend-owned diagnostic policy identity");
}

// A provider attempting to substitute the values from the real incident
// (qt-debug / build/qt-debug / UI_Production_Dashboard_qml_lint) never even
// reaches this point with those strings inside verificationArguments - it
// would have to select them as IDS, and the binder rejects every one of
// them as unknown to the catalog.
void testSubstitutedIncidentIdsAreRejectedNotSilentlyResolved()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        qmlLintRequirement("qt-debug", "qt-debug", "UI_Production_Dashboard_qml_lint")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_profile_unknown",
        "A provider-substituted, unadvertised profile id was not rejected");
}

void testCmakeTargetConsistencyResolvesFromUnionOfConfirmedTargets()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "registration";
    requirement.description = "Check target registration";
    requirement.verificationKind = ExecutionVerificationKind::CMakeTargetConsistency;
    requirement.verificationTargetId = "all_qmllint";
    requirement.verificationArguments = {
        {"definitionFile", "CMakeLists.txt"}, {"registrationFile", "qml/CMakeLists.txt"}};
    requirement.required = true;
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(std::move(requirement)));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(bound.success() && findings.empty(),
        "A cmake_target_consistency selection with a confirmed target was rejected");
    const auto& args = manifest.tasks.front().acceptanceContract->requirements.front()
        .verificationArguments;
    require(args.size() == 3, "cmake_target_consistency must end with exactly three arguments");
    const auto hasArg = [&](const char* name, const char* value) {
        for (const auto& arg : args) if (arg.name == name && arg.value == value) return true;
        return false;
    };
    require(hasArg("definitionFile", "CMakeLists.txt")
            && hasArg("registrationFile", "qml/CMakeLists.txt")
            && hasArg("target", "all_qmllint"),
        "cmake_target_consistency lost a task-owned or resolved argument");
}

void testInspectPathsNeverTouchesSnapshot()
{
    // A completely empty (SetupRequired) snapshot must never block
    // inspect_paths - it needs no profile at all.
    VerificationEnvironmentSnapshot empty;
    empty.runId = "run-1";
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "inspect";
    requirement.description = "Notes file exists";
    requirement.verificationKind = ExecutionVerificationKind::InspectPaths;
    requirement.requiredOutputs = {"README.md"};
    requirement.required = true;
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(std::move(requirement)));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, empty, findings);
    require(bound.success() && findings.empty(),
        "inspect_paths must never require a verification profile");
    require(manifest.tasks.front().acceptanceContract->requirements.front()
            .verificationArguments.empty(),
        "inspect_paths must never gain verificationArguments from binding");
}

// ---------------------------------------------------------------------
// Section 3: rejections (required tests 13-18)
// ---------------------------------------------------------------------

void testUnknownProfileIdIsRejected()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        cmakeBuildRequirement("invented-profile", "local-qt-debug")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_profile_unknown",
        "An invented profile id was not rejected");
    require(findings.size() == 1 && findings.front().taskId.has_value()
            && *findings.front().taskId == "task-1",
        "Binding rejection finding lost the owning task id");
}

void testUnknownBuildProfileIdIsRejected()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        cmakeBuildRequirement("local-qt-debug", "invented-build-profile")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_build_profile_unknown",
        "An invented build-profile id was not rejected");
}

void testUnknownTargetIsRejected()
{
    const auto snapshot = regressionIncidentSnapshot();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        qmlLintRequirement("local-qt-debug", "local-qt-debug", "invented-target")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_target_unknown",
        "An invented target id was not rejected");
}

void testUnconfirmedBuildCapabilityIsRejected()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().buildPresetsConfirmed = false;
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        cmakeBuildRequirement("local-qt-debug", "local-qt-debug")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_build_unavailable",
        "A selection against an unconfirmed (buildPresetsConfirmed=false) profile was accepted");
}

void testMissingCandidateRelativeBuildDirectoryIsRejectedForQmlLint()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().resolvedBinaryDir.clear();
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        qmlLintRequirement("local-qt-debug", "local-qt-debug", "all_qmllint")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success()
            && bound.error()->code == "manifest.verification_build_directory_unavailable",
        "qml_lint with no candidate-relative build directory was accepted");
}

void testUnconfirmedTargetAllowListIsRejectedForQmlLint()
{
    auto snapshot = regressionIncidentSnapshot();
    snapshot.profiles.front().buildPresets.front().allowedTargetsConfirmed = false;
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        qmlLintRequirement("local-qt-debug", "local-qt-debug", "all_qmllint")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_target_unknown",
        "qml_lint selecting a target from an unconfirmed allow-list was accepted");
}

void testSetupRequiredSnapshotRejectsEveryEnvironmentDependentKind()
{
    VerificationEnvironmentSnapshot setupRequired;
    setupRequired.runId = "run-1";
    setupRequired.state = VerificationEnvironmentState::SetupRequired;
    setupRequired.diagnostic = "No CMakePresets.json or CMakeUserPresets.json exists";
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks.push_back(taskWithRequirement(
        cmakeBuildRequirement("any-profile", "any-build-profile")));
    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(
        manifest, setupRequired, findings);
    require(!bound.success() && bound.error()->code == "manifest.verification_profile_unknown",
        "A SetupRequired snapshot (empty catalog) did not reject cmake_build - never invent a "
        "profile merely because the project contains CMake files");
}

// ---------------------------------------------------------------------
// Section 4: trusted catalog rendering
// ---------------------------------------------------------------------

void testRenderedCatalogAdvertisesOnlySafeSelectableInformation()
{
    const auto snapshot = regressionIncidentSnapshot();
    const auto catalog = ManifestVerificationProfileBinder::renderTrustedCatalog(snapshot);
    require(catalog.find("local-qt-debug") != std::string::npos,
        "Rendered catalog omitted the advertised profile id");
    require(catalog.find("all_qmllint") != std::string::npos,
        "Rendered catalog omitted the advertised target id");
    require(catalog.find("build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug") != std::string::npos,
        "Rendered catalog omitted the candidate-relative build directory");
    require(catalog.find("buildPresetsConfirmed") != std::string::npos,
        "Rendered catalog omitted the availability flag the Planner must check");
    // Never a raw executable path, drive-letter absolute path, or shell
    // token - the catalog is read-only selection data, not a command.
    require(catalog.find(":\\") == std::string::npos && catalog.find(":/") == std::string::npos,
        "Rendered catalog leaked what looks like an absolute host path");
}

void testRenderedCatalogForSetupRequiredNeverImpliesAProfileExists()
{
    VerificationEnvironmentSnapshot setupRequired;
    setupRequired.runId = "run-1";
    setupRequired.state = VerificationEnvironmentState::SetupRequired;
    const auto catalog = ManifestVerificationProfileBinder::renderTrustedCatalog(setupRequired);
    require(catalog.find("No verification profile is currently available") != std::string::npos,
        "A SetupRequired catalog rendering did not clearly state no profile is available");
    require(catalog.find("profileId:") == std::string::npos,
        "A SetupRequired catalog rendering must never advertise a profile entry");
}

} // namespace

int runManifestVerificationProfileBinderTests()
{
    try {
        testValidSnapshotIdentityPasses();
        testRunIdMismatchFails();
        testProjectIdMismatchFails();
        testBaselineFingerprintMismatchFails();
        testUnsupportedSchemaVersionFails();
        testStateContentMismatchFails();
        testDuplicateProfileIdFails();
        testDuplicateTargetIdWithinProfileFails();
        testUnsafeBuildPathFails();
        testAbsoluteDeclaredBinaryDirWithSafeResolvedDoesNotFailIdentity();
        testOversizedDeclaredBinaryDirFails();
        testProfileIdNotMatchingConfigurePresetNameFails();
        testBuildPresetLinkedToWrongConfigurePresetFails();
        testUnconfirmedAllowListWithNonEmptyTargetsFails();
        testTooManyProfilesFails();
        testTooManyTargetsPerBuildPresetFails();
        testCmakeBuildResolvesExactBackendValues();
        testQmlLintRegressionIncidentResolvesExactCandidateRelativeValues();
        testSubstitutedIncidentIdsAreRejectedNotSilentlyResolved();
        testCmakeTargetConsistencyResolvesFromUnionOfConfirmedTargets();
        testInspectPathsNeverTouchesSnapshot();
        testUnknownProfileIdIsRejected();
        testUnknownBuildProfileIdIsRejected();
        testUnknownTargetIsRejected();
        testUnconfirmedBuildCapabilityIsRejected();
        testMissingCandidateRelativeBuildDirectoryIsRejectedForQmlLint();
        testUnconfirmedTargetAllowListIsRejectedForQmlLint();
        testSetupRequiredSnapshotRejectsEveryEnvironmentDependentKind();
        testRenderedCatalogAdvertisesOnlySafeSelectableInformation();
        testRenderedCatalogForSetupRequiredNeverImpliesAProfileExists();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ManifestVerificationProfileBinder tests failed: " << exception.what() << '\n';
        return 1;
    }
}
