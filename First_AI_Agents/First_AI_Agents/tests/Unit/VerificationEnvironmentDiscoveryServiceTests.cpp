// Unit tests for AG-18.1's VerificationEnvironmentDiscoveryService: proves
// discovery-content outcomes (missing files, malformed JSON, an unusable
// cmake probe, workspace drift since the baseline was captured) never
// surface as a hard failure - only a genuine repository-persistence failure
// does - and that a real, cmake-confirmed profile is recorded with correct
// linked configure/build identity, honest provenance, and a candidate-relative
// best-effort binaryDir resolution. Real temp-directory files, a scripted
// command runner, no real cmake invocation.
#include "ai/application/InMemoryVerificationEnvironmentRepository.hpp"
#include "ai/application/verification/VerificationEnvironmentDiscoveryService.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDir {
    fs::path path {fs::temp_directory_path()
        / ("ai-orchestrator-verification-environment-discovery-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    TempDir() { fs::create_directories(path); }
    ~TempDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};

class FakeVerificationCommandRunner final : public IManifestVerificationCommandRunner {
public:
    std::vector<ManifestVerificationCommandOutcome> outcomes;
    std::vector<ManifestVerificationCommandRequest> requests;
    std::function<void(std::size_t)> afterRun;

    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        requests.push_back(request);
        ManifestVerificationCommandOutcome result;
        if (requests.size() <= outcomes.size()) result = outcomes[requests.size() - 1];
        else {
            result.started = true;
            result.exitCode = 0;
        }
        if (afterRun) afterRun(requests.size());
        return result;
    }
};

class RejectingVerificationEnvironmentRepository final
    : public IVerificationEnvironmentRepository {
public:
    common::Result<void> recordSnapshot(VerificationEnvironmentSnapshot) override
    {
        return common::Result<void>::failure({"storage.rejected", "Simulated persistence failure"});
    }
    common::Result<std::optional<VerificationEnvironmentSnapshot>> findForRun(
        const RunId&) const override
    {
        return common::Result<std::optional<VerificationEnvironmentSnapshot>>::ok(std::nullopt);
    }
};

void writeFile(const fs::path& path, std::string_view content)
{
    std::ofstream stream(path, std::ios::binary);
    stream << content;
}

ManifestVerificationCommandOutcome listOutcome(std::string output)
{
    ManifestVerificationCommandOutcome outcome;
    outcome.started = true;
    outcome.exitCode = 0;
    outcome.boundedOutput = std::move(output);
    return outcome;
}

// A genuine full WorkspaceBaselineSnapshot::capture() over workspacePath -
// discovery's own baseline-consistency check now re-runs this exact same
// capture and requires it to compare as "unchanged", so the fixture must be
// a real capture, not a hand-picked subset of files. Call this AFTER
// writing the fixture's files, exactly as the real ManifestWorkflowRunner
// captures the baseline before discovery reads anything.
WorkspaceBaselineSnapshot makeConsistentBaseline(const fs::path& workspacePath)
{
    const auto captured = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(workspacePath));
    require(captured.success(), "Could not capture a baseline for the test fixture");
    return captured.value();
}

void testNoPresetFilesYieldsSetupRequiredWithoutCallingCmake()
{
    TempDir workspace;
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Discovery must succeed even with no preset files");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "No preset file must classify as SetupRequired");
    require(result.value().profiles.empty() && result.value().unlinkedBuildPresets.empty(),
        "No profiles should be recorded when no preset file exists");
    require(!result.value().diagnostic.empty(), "SetupRequired must carry a diagnostic");
    require(commandRunner.requests.empty(),
        "cmake must never be invoked when no preset file exists at all");
    require(result.value().runId == "run-1" && result.value().projectId == "project-1"
            && result.value().baselineFingerprint == baseline.fingerprint,
        "Snapshot identity/provenance fields must round-trip");
}

void testExistingConfigurePresetIsRecordedAsLinkedProfileWithEnrichment()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"${sourceDir}/build/debug"}],)"
        R"("buildPresets":[{"name":"debug","configurePreset":"debug","targets":["all"]}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\"   - Debug build\n"));
    commandRunner.outcomes.push_back(
        listOutcome("Available build presets:\n\n  \"debug\"   - Debug build\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Discovery must succeed for a real preset");
    require(result.value().state == VerificationEnvironmentState::ExistingProfile,
        "A cmake-confirmed preset must classify as ExistingProfile");
    require(result.value().diagnostic.empty(), "A fully successful discovery must carry no diagnostic");
    require(result.value().profiles.size() == 1, "Expected exactly one linked profile");
    const auto& profile = result.value().profiles.front();
    require(profile.profileId == "debug" && profile.configurePresetName == "debug",
        "Preset name must come from the cmake probe");
    require(profile.configureDisplayName == "Debug build",
        "displayName must be parsed from the cmake probe's own output");
    require(profile.configureSource == VerificationPresetFileSource::ProjectPresets,
        "Enrichment must be attributed to CMakePresets.json");
    require(profile.declaredBinaryDir == "${sourceDir}/build/debug",
        "declaredBinaryDir must be the raw, unexpanded string from the JSON file");
    require(profile.resolvedBinaryDir == "build/debug",
        "resolvedBinaryDir must be candidate-relative (${sourceDir} stripped), never an absolute host path");
    require(profile.buildPresetsConfirmed,
        "A successful build-preset probe must leave buildPresetsConfirmed true");
    require(profile.buildPresets.size() == 1
            && profile.buildPresets.front().declaredConfigurePreset == "debug"
            && profile.buildPresets.front().declaredTargets == std::vector<std::string>{"all"}
            && profile.buildPresets.front().allowedTargetsConfirmed
            && profile.buildPresets.front().allowedTargets == std::vector<std::string>{"all"},
        "Build preset enrichment (configurePreset/targets) must round-trip, linked under its profile");
    require(result.value().unlinkedBuildPresets.empty(),
        "A correctly-linked build preset must not also appear as unlinked");
}

void testAbsoluteDeclaredBinaryDirIsNeverResolved()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"C:/some/real/machine/path"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\"   - Debug build\n"));
    commandRunner.outcomes.push_back(listOutcome("Available build presets:\n\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Discovery must succeed");
    require(result.value().profiles.size() == 1
            && result.value().profiles.front().declaredBinaryDir == "C:/some/real/machine/path"
            && result.value().profiles.front().resolvedBinaryDir.empty(),
        "An already-absolute declaredBinaryDir must never be surfaced as resolvedBinaryDir - "
        "it is not a candidate-relative path a later package could safely reuse");
}

void testUnsafeSourceDirRemaindersAreNeverResolved()
{
    const std::vector<std::string> unsafe {
        "${sourceDir}/../outside",
        "${sourceDir}/C:/outside",
        "${sourceDir}//double-separator",
        "${sourceDir}/./dot-segment",
        "${sourceDir}/build/$env{PRIVATE_DIR}"
    };
    for (std::size_t index = 0; index < unsafe.size(); ++index) {
        TempDir workspace;
        writeFile(workspace.path / "CMakePresets.json",
            std::string("{\"version\":3,\"configurePresets\":[{\"name\":\"debug\",\"binaryDir\":\"")
                + unsafe[index] + "\"}]}");
        const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
        const auto baseline = makeConsistentBaseline(workspace.path);
        InMemoryVerificationEnvironmentRepository repository;
        VerificationEnvironmentDiscoveryService service(repository);
        FakeVerificationCommandRunner commandRunner;
        commandRunner.outcomes.push_back(
            listOutcome("Available configure presets:\n\n  \"debug\" - Debug\n"));
        commandRunner.outcomes.push_back(listOutcome("Available build presets:\n\n"));

        const auto result = service.discoverAndRecord(
            "run-" + std::to_string(index), "project-1", workspace.path,
            baseline, policy, commandRunner, std::stop_token());
        require(result.success() && result.value().profiles.size() == 1,
            "Unsafe binaryDir fixture did not complete discovery");
        require(result.value().profiles.front().resolvedBinaryDir.empty(),
            "An unsafe sourceDir-relative remainder escaped candidate path validation");
    }
}

void testConfiguredTargetCatalogIsDiscoveredWhenPresetDeclaresNoTargets()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"${sourceDir}/build/debug"}],)"
        R"("buildPresets":[{"name":"debug","configurePreset":"debug"}]})");
    fs::create_directories(workspace.path / "build" / "debug");
    writeFile(workspace.path / "build" / "debug" / "CMakeCache.txt", "configured");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.outcomes.push_back(
        listOutcome("Available build presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.outcomes.push_back(
        listOutcome("The following are some of the valid targets:\n... all\n... all_qmllint\nclean: phony\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success() && result.value().profiles.size() == 1
            && result.value().profiles.front().buildPresets.size() == 1,
        "Configured-target discovery did not produce the linked build preset");
    const auto& build = result.value().profiles.front().buildPresets.front();
    require(build.allowedTargetsConfirmed,
        "A successful configured-target probe must confirm the allow-list");
    require(build.allowedTargets == std::vector<std::string>({"all", "all_qmllint", "clean"}),
        "Configured-target output was not parsed into a deterministic bounded allow-list");
    require(commandRunner.requests.size() == 3
            && commandRunner.requests.back().arguments
                == std::vector<std::string>({"--build", "--preset", "debug", "--target", "help"}),
        "Target discovery must use the fixed Backend-owned argument vector");
}

void testUserPresetsTakePrecedenceOverProjectPresetsForSameName()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"project/build"}]})");
    writeFile(workspace.path / "CMakeUserPresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"user/build"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\"   - Debug build\n"));
    commandRunner.outcomes.push_back(listOutcome("Available build presets:\n\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Discovery must succeed");
    require(result.value().profiles.size() == 1, "Expected exactly one linked profile");
    require(result.value().profiles.front().declaredBinaryDir == "user/build",
        "CMakeUserPresets.json enrichment must win over CMakePresets.json for the same name");
    require(result.value().profiles.front().configureSource == VerificationPresetFileSource::UserPresets,
        "The winning enrichment's source must be reported as UserPresets");
}

void testMalformedPresetFileDoesNotBreakDiscoveryButSourceStaysUnknown()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json", "{not valid json");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\"   - Debug build\n"));
    commandRunner.outcomes.push_back(listOutcome("Available build presets:\n\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "A malformed preset file must never fail discovery");
    require(result.value().state == VerificationEnvironmentState::ExistingProfile,
        "The authoritative cmake probe must still classify the preset as existing");
    require(result.value().profiles.size() == 1 && result.value().profiles.front().configurePresetName == "debug"
            && result.value().profiles.front().declaredBinaryDir.empty(),
        "Enrichment must simply be absent when the JSON file could not be parsed");
    require(result.value().profiles.front().configureSource == VerificationPresetFileSource::Unknown,
        "Provenance must never be fabricated as ProjectPresets when enrichment never actually matched it");
}

void testCmakeUnavailableDegradesGracefullyToSetupRequired()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json", R"({"version":3,"configurePresets":[]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    ManifestVerificationCommandOutcome notStarted;
    notStarted.started = false;
    commandRunner.outcomes.push_back(notStarted);

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(),
        "cmake being unavailable must degrade to SetupRequired, never fail discovery");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "An unusable cmake probe must classify as SetupRequired");
    require(!result.value().diagnostic.empty(), "SetupRequired must carry a diagnostic");
    require(commandRunner.requests.size() == 1,
        "A failed configure probe must never lead to a build probe being invoked at all");
}

void testEmptyPresetListYieldsSetupRequired()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json", R"({"version":3,"configurePresets":[]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(listOutcome("Available configure presets:\n\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "An empty cmake preset list must not fail discovery");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "Zero selectable presets must classify as SetupRequired");
}

void testDuplicateDiscoveryForSameRunIsIdempotent()
{
    TempDir workspace;
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;

    const auto first = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(first.success(), "First discovery call must succeed");
    const auto second = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(second.success(),
        "A second discovery call for the same Run must be a benign idempotent success");
    require(second.value().runId == "run-1", "The re-read snapshot must still be for the same Run");
}

void testGenuinePersistenceFailurePropagates()
{
    TempDir workspace;
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    RejectingVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(!result.success(),
        "A genuine repository persistence failure must propagate, unlike discovery-content outcomes");
    require(result.error()->code == "storage.rejected", "Expected the repository's own error code");
}

// AG-18.1 hardening: the baseline is captured moments before discovery
// reads the live filesystem independently - a file changed in that window
// must never be silently trusted just because a fingerprint string still
// matches the Run.
void testWorkspaceDriftSinceBaselineCaptureIsDetected()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    // The workspace changes after the baseline was captured - exactly the
    // race this check exists to catch.
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"},{"name":"release"}]})");
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Baseline drift must degrade to SetupRequired, never fail discovery");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "Content that no longer matches the captured baseline must never be trusted as ExistingProfile");
    require(!result.value().diagnostic.empty(), "Drift must carry a diagnostic");
    require(commandRunner.requests.empty(),
        "cmake must never be invoked against content the baseline never actually saw");
}

// AG-18.1 hardening: the consistency check must catch drift ANYWHERE in the
// policy-visible workspace, not just the two named preset files - a preset
// can pull additional definitions in via CMake's own `include` mechanism,
// so a change to an unrelated/included file must be caught too.
void testDriftInAnUnrelatedFileIsAlsoDetected()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"}]})");
    writeFile(workspace.path / "included-presets-fragment.json", "{}");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    // CMakePresets.json itself never changes - only the file it would have
    // `include`d does. A check narrowly scoped to the two well-known
    // filenames would miss this entirely.
    writeFile(workspace.path / "included-presets-fragment.json", R"({"changed": true})");
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Drift in an unrelated file must degrade to SetupRequired, never fail discovery");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "A change to any policy-visible file since the baseline must be caught, not just the preset files");
    require(commandRunner.requests.empty(),
        "cmake must never be invoked while any part of the workspace has drifted from the baseline");
}

void testDriftDuringDiscoveryDiscardsTheCompletedCatalog()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"${sourceDir}/build/debug"}],)"
        R"("buildPresets":[{"name":"debug","configurePreset":"debug","targets":["all"]}]})");
    writeFile(workspace.path / "source.txt", "baseline");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.outcomes.push_back(
        listOutcome("Available build presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.afterRun = [&](std::size_t call) {
        if (call == 2) writeFile(workspace.path / "source.txt", "changed during discovery");
    };

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Concurrent drift must produce a durable typed snapshot");
    require(result.value().state == VerificationEnvironmentState::SetupRequired
            && result.value().profiles.empty()
            && result.value().unlinkedBuildPresets.empty(),
        "A catalog derived while the workspace drifted must be discarded before persistence");
    require(!result.value().diagnostic.empty(),
        "Concurrent drift must retain a bounded Backend diagnostic");
}

// A preset file that exists now but was absent from the baseline (e.g. the
// baseline capture's own policy somehow missed it) must be treated exactly
// like content drift, not silently trusted.
void testPresetFileMissingFromBaselineIsDetected()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    WorkspaceBaselineSnapshot emptyBaseline;
    emptyBaseline.fingerprint = "baseline-fp";
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, emptyBaseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "A baseline-invisible preset file must degrade to SetupRequired");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "A file the baseline never recorded must never be trusted as ExistingProfile");
    require(commandRunner.requests.empty(), "cmake must never be invoked in this case either");
}

// AG-18.1 hardening: a genuinely-existing configure preset must never be
// reclassified away just because the (separate) build-preset probe failed -
// but that partial failure must be visible via both a diagnostic and a
// typed per-profile flag, not silently dropped.
void testBuildProbeFailureKeepsExistingProfileButRecordsDiagnosticAndFlag()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\"   - Debug build\n"));
    ManifestVerificationCommandOutcome buildFailure;
    buildFailure.started = true;
    buildFailure.exitCode = 1;
    commandRunner.outcomes.push_back(buildFailure);

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "A failed build probe must never fail discovery outright");
    require(result.value().state == VerificationEnvironmentState::ExistingProfile,
        "A confirmed configure preset must still classify as ExistingProfile");
    require(!result.value().diagnostic.empty(),
        "A partially-failed discovery must record a diagnostic, not silently look fully clean");
    require(result.value().profiles.size() == 1
            && result.value().profiles.front().buildPresets.empty(),
        "No build presets can be attributed when the build probe itself failed");
    require(!result.value().profiles.front().buildPresetsConfirmed,
        "buildPresetsConfirmed must be false whenever the build probe itself failed - a later package "
        "must not assume build/QML-lint verification is available for this profile");
}

// A build preset whose declared configurePreset does not match any
// discovered configure preset name must be preserved for provenance, never
// silently dropped or force-attached to the wrong profile.
void testUnlinkableBuildPresetIsRecordedAsUnlinked()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"}],)"
        R"("buildPresets":[{"name":"orphan-build","configurePreset":"nonexistent-configure"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\"   - Debug build\n"));
    commandRunner.outcomes.push_back(
        listOutcome("Available build presets:\n\n  \"orphan-build\"   - Orphan\n"));

    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, std::stop_token());
    require(result.success(), "Discovery must succeed");
    require(result.value().profiles.size() == 1
            && result.value().profiles.front().buildPresets.empty(),
        "An unlinkable build preset must never be force-attached to an unrelated profile");
    require(result.value().unlinkedBuildPresets.size() == 1
            && result.value().unlinkedBuildPresets.front().name == "orphan-build",
        "An unlinkable build preset must be preserved for provenance, not silently dropped");
}

// A cancelled configure probe must never still spawn a build probe on its
// way out.
void testCancellationSkipsBuildProbe()
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    ManifestVerificationCommandOutcome cancelled;
    cancelled.cancelled = true;
    commandRunner.outcomes.push_back(cancelled);

    std::stop_source source;
    source.request_stop();
    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy,
        commandRunner, source.get_token());
    require(result.success(), "A cancelled probe must degrade to SetupRequired, never fail discovery");
    require(result.value().state == VerificationEnvironmentState::SetupRequired,
        "A cancelled configure probe must classify as SetupRequired");
    require(commandRunner.requests.size() == 1,
        "A cancelled/already-stopping caller must never see a second cmake process spawned");
}

} // namespace

// ---- AG-17.0: target catalog completeness ----------------------------------
//
// Before AG-17.0 a target list cut off at kVerificationEnvironmentMaxTargetsPer-
// Preset was indistinguishable from a complete one (both read
// allowedTargetsConfirmed=true), so "no QML-lint target in the snapshot" could
// mean "the project has none" or "it fell past the cap".

std::string configuredTargetHelp(std::size_t distinctTargets, bool repeatFirst = false)
{
    std::string output = "The following are some of the valid targets:\n";
    for (std::size_t index = 0; index < distinctTargets; ++index) {
        output += "... t" + std::string(index < 10 ? "0" : "") + std::to_string(index) + "\n";
    }
    if (repeatFirst) output += "... t00\n";
    return output;
}

VerificationBuildPresetDescriptor discoverConfiguredProbe(const std::string& targetHelpOutput)
{
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"${sourceDir}/build/debug"}],)"
        R"("buildPresets":[{"name":"debug","configurePreset":"debug"}]})");
    fs::create_directories(workspace.path / "build" / "debug");
    writeFile(workspace.path / "build" / "debug" / "CMakeCache.txt", "configured");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.outcomes.push_back(
        listOutcome("Available build presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.outcomes.push_back(listOutcome(targetHelpOutput));
    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy, commandRunner, std::stop_token());
    require(result.success() && result.value().profiles.size() == 1
            && result.value().profiles.front().buildPresets.size() == 1,
        "Configured-target discovery did not produce the linked build preset");
    return result.value().profiles.front().buildPresets.front();
}

void testConfiguredTargetCatalogBelowTheCapIsComplete()
{
    const auto build = discoverConfiguredProbe(configuredTargetHelp(3));
    require(build.allowedTargetsConfirmed && build.allowedTargetsComplete
            && build.allowedTargets.size() == 3,
        "A target list under the cap must be confirmed and provably complete");
}

void testConfiguredTargetCatalogExactlyAtTheCapIsStillComplete()
{
    const auto build = discoverConfiguredProbe(
        configuredTargetHelp(kVerificationEnvironmentMaxTargetsPerPreset));
    require(build.allowedTargetsConfirmed && build.allowedTargetsComplete
            && build.allowedTargets.size() == kVerificationEnvironmentMaxTargetsPerPreset,
        "Exactly the cap's worth of targets, with nothing beyond it, is complete");
}

void testConfiguredTargetCatalogBeyondTheCapIsConfirmedButNotComplete()
{
    const auto build = discoverConfiguredProbe(
        configuredTargetHelp(kVerificationEnvironmentMaxTargetsPerPreset + 1));
    require(build.allowedTargetsConfirmed,
        "A cut-off list is still a confirmed allow-list of what it does contain");
    require(!build.allowedTargetsComplete,
        "A target list cut off at the cap must never be reported complete");
    require(build.allowedTargets.size() == kVerificationEnvironmentMaxTargetsPerPreset,
        "The kept slice must still honour the trusted target ceiling");
}

void testRepeatedTargetBeyondTheCapDoesNotMarkTruncation()
{
    const auto build = discoverConfiguredProbe(
        configuredTargetHelp(kVerificationEnvironmentMaxTargetsPerPreset, /*repeatFirst=*/true));
    require(build.allowedTargetsConfirmed && build.allowedTargetsComplete,
        "Re-listing an already-held target is not evidence of a cut-off catalog");
}

void testDeclaredTargetsBeyondTheCapAreNotComplete()
{
    for (const bool overCap : {false, true}) {
        TempDir workspace;
        const std::size_t count = kVerificationEnvironmentMaxTargetsPerPreset + (overCap ? 2 : 0);
        std::string targets;
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) targets += ',';
            targets += "\"d" + std::string(index < 10 ? "0" : "") + std::to_string(index) + "\"";
        }
        writeFile(workspace.path / "CMakePresets.json",
            R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"${sourceDir}/build/debug"}],)"
            R"("buildPresets":[{"name":"debug","configurePreset":"debug","targets":[)" + targets
                + "]}]}");
        const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
        const auto baseline = makeConsistentBaseline(workspace.path);
        InMemoryVerificationEnvironmentRepository repository;
        VerificationEnvironmentDiscoveryService service(repository);
        FakeVerificationCommandRunner commandRunner;
        commandRunner.outcomes.push_back(
            listOutcome("Available configure presets:\n\n  \"debug\" - Debug\n"));
        commandRunner.outcomes.push_back(
            listOutcome("Available build presets:\n\n  \"debug\" - Debug\n"));
        const auto result = service.discoverAndRecord(
            "run-1", "project-1", workspace.path, baseline, policy, commandRunner,
            std::stop_token());
        require(result.success() && result.value().profiles.size() == 1
                && result.value().profiles.front().buildPresets.size() == 1,
            "Declared-target discovery did not produce the linked build preset");
        const auto& build = result.value().profiles.front().buildPresets.front();
        // The declared list is cut at the cap BEFORE it is compared with its own
        // allow-list, so it still reads as "confirmed"; only the explicit
        // truncation flag tells the two cases apart.
        require(build.allowedTargetsConfirmed
                && build.allowedTargets.size() == kVerificationEnvironmentMaxTargetsPerPreset,
            "A declared target list must be confirmed (within the trusted ceiling)");
        require(build.allowedTargetsComplete == !overCap,
            overCap ? "A declared list longer than the cap must not be reported complete"
                    : "A declared list within the cap must be reported complete");
    }
}

void testUnconfirmedTargetCatalogIsNeverComplete()
{
    // No CMakeCache.txt: the configured-target probe is never attempted.
    TempDir workspace;
    writeFile(workspace.path / "CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"debug","binaryDir":"${sourceDir}/build/debug"}],)"
        R"("buildPresets":[{"name":"debug","configurePreset":"debug"}]})");
    const auto policy = WorkspaceReadPolicy::secureDefaults(workspace.path);
    const auto baseline = makeConsistentBaseline(workspace.path);
    InMemoryVerificationEnvironmentRepository repository;
    VerificationEnvironmentDiscoveryService service(repository);
    FakeVerificationCommandRunner commandRunner;
    commandRunner.outcomes.push_back(
        listOutcome("Available configure presets:\n\n  \"debug\" - Debug\n"));
    commandRunner.outcomes.push_back(
        listOutcome("Available build presets:\n\n  \"debug\" - Debug\n"));
    const auto result = service.discoverAndRecord(
        "run-1", "project-1", workspace.path, baseline, policy, commandRunner, std::stop_token());
    require(result.success(), "Discovery failed");
    const auto& build = result.value().profiles.front().buildPresets.front();
    require(!build.allowedTargetsConfirmed && !build.allowedTargetsComplete,
        "A catalog that was never probed must be neither confirmed nor complete");
}

int runVerificationEnvironmentDiscoveryServiceTests()
{
    testNoPresetFilesYieldsSetupRequiredWithoutCallingCmake();
    testExistingConfigurePresetIsRecordedAsLinkedProfileWithEnrichment();
    testAbsoluteDeclaredBinaryDirIsNeverResolved();
    testUnsafeSourceDirRemaindersAreNeverResolved();
    testConfiguredTargetCatalogIsDiscoveredWhenPresetDeclaresNoTargets();
    testUserPresetsTakePrecedenceOverProjectPresetsForSameName();
    testMalformedPresetFileDoesNotBreakDiscoveryButSourceStaysUnknown();
    testCmakeUnavailableDegradesGracefullyToSetupRequired();
    testEmptyPresetListYieldsSetupRequired();
    testDuplicateDiscoveryForSameRunIsIdempotent();
    testGenuinePersistenceFailurePropagates();
    testWorkspaceDriftSinceBaselineCaptureIsDetected();
    testDriftInAnUnrelatedFileIsAlsoDetected();
    testDriftDuringDiscoveryDiscardsTheCompletedCatalog();
    testPresetFileMissingFromBaselineIsDetected();
    testBuildProbeFailureKeepsExistingProfileButRecordsDiagnosticAndFlag();
    testUnlinkableBuildPresetIsRecordedAsUnlinked();
    testCancellationSkipsBuildProbe();
    testConfiguredTargetCatalogBelowTheCapIsComplete();
    testConfiguredTargetCatalogExactlyAtTheCapIsStillComplete();
    testConfiguredTargetCatalogBeyondTheCapIsConfirmedButNotComplete();
    testRepeatedTargetBeyondTheCapDoesNotMarkTruncation();
    testDeclaredTargetsBeyondTheCapAreNotComplete();
    testUnconfirmedTargetCatalogIsNeverComplete();
    return 0;
}
