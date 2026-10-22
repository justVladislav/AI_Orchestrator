// AG-18.3 hardening (blocker 3) integration test: a REAL, temporary,
// minimal CMake project - no scripted/fake command runner - proving
// VerificationEnvironmentDiscoveryService::discoverForCandidateRoot
// confirms targets from an actual configured CMake build, never merely
// from a provider-proposed preset's declared `targets` array. No provider
// HTTP calls; only a real local `cmake` invocation, exactly like the
// existing ManifestProposalVerification_tests real-cmake regression tests.
#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/VerificationEnvironmentDiscoveryService.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace ai::application;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDir {
    fs::path path {fs::temp_directory_path()
        / ("ai-orchestrator-candidate-discovery-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    TempDir() { fs::create_directories(path); }
    ~TempDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};

void write(const fs::path& path, const std::string& content)
{
    std::ofstream stream(path, std::ios::binary);
    stream << content;
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

#ifdef _WIN32
fs::path resolvedNinjaPath()
{
#ifndef AI_TEST_CMAKE_MAKE_PROGRAM
#error "Candidate-discovery test requires the configured CMake make program"
#endif
    const fs::path path(AI_TEST_CMAKE_MAKE_PROGRAM);
    require(!path.empty() && fs::is_regular_file(path),
        "The configured CMake make program is unavailable to the real fixture");
    return path;
}
#endif

void testCandidateDiscoveryConfirmsRealConfiguredTargetsAndRejectsInventedOnes()
{
#ifdef _WIN32
    TempDir candidate;
    // A minimal, compiler-free project (NONE language) so this test never
    // needs a real C/C++ toolchain to complete quickly - only CMake's own
    // custom-target registration, which is exactly what the target-help
    // catalog probe reads.
    write(candidate.path / "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(candidate_fixture NONE)\n"
        "add_custom_target(all_qmllint)\n"
        "add_custom_target(unrelated_target)\n"
        "add_custom_target(second_unrelated_target)\n");
    const auto ninjaPath = resolvedNinjaPath().generic_string();
    write(candidate.path / "CMakeUserPresets.json", std::string(R"JSON({
  "version": 6,
  "configurePresets": [{
    "name": "candidate-debug",
    "generator": "Ninja",
    "binaryDir": "${sourceDir}/build/candidate-debug",
    "cacheVariables": {"CMAKE_MAKE_PROGRAM": ")JSON") + ninjaPath + R"JSON("}
  }],
  "buildPresets": [{
    "name": "candidate-debug",
    "configurePreset": "candidate-debug",
    "targets": ["all_qmllint", "invented_target_never_real"]
  }]
})JSON");

    // Starts without a configured build tree - the whole point of this
    // fixture is that discovery must configure it fresh itself.
    require(!fs::exists(candidate.path / "build"),
        "Fixture must start without a pre-existing configured build tree");

    SystemManifestVerificationCommandRunner runner;
    const auto discovered = VerificationEnvironmentDiscoveryService::discoverForCandidateRoot(
        candidate.path, runner, {});
    require(discovered.success(),
        "Candidate discovery failed against a real minimal CMake project");
    const auto& snapshot = discovered.value();
    require(snapshot.state == VerificationEnvironmentState::ExistingProfile,
        "A real, confirmed configure preset must classify as ExistingProfile");
    require(snapshot.profiles.size() == 1, "Expected exactly one discovered profile");
    const auto& profile = snapshot.profiles.front();
    require(profile.buildPresetsConfirmed && profile.buildPresets.size() == 1,
        "Expected exactly one confirmed build preset");
    const auto& buildPreset = profile.buildPresets.front();

    require(buildPreset.allowedTargetsConfirmed,
        "Real candidate-side target confirmation did not run at all");
    // Proves target discovery used the actual configured CMake output, not
    // just the declared JSON: both the requested target and targets the
    // JSON never even mentioned are present in the real catalog.
    require(contains(buildPreset.allowedTargets, "all_qmllint"),
        "The real configured target catalog lost the requested target");
    require(contains(buildPreset.allowedTargets, "unrelated_target")
            && contains(buildPreset.allowedTargets, "second_unrelated_target"),
        "Unrelated real configured targets did not appear in the real catalog - target "
        "discovery is not reading actual CMake output");
    // Proves an invented declared target is rejected: it must never appear
    // in the real confirmed catalog merely because the preset JSON claimed
    // it.
    require(!contains(buildPreset.allowedTargets, "invented_target_never_real"),
        "An invented declared target was accepted as confirmed merely because the preset "
        "JSON claimed it");

    // Proves the source/candidate workspace's own declared inputs remain
    // exactly as authored - discovery only ever configures/probes, never
    // rewrites the project's own files.
    std::ifstream presetsAfter(candidate.path / "CMakeUserPresets.json");
    std::string presetsContent(
        (std::istreambuf_iterator<char>(presetsAfter)), std::istreambuf_iterator<char>());
    require(presetsContent.find("invented_target_never_real") != std::string::npos,
        "CMakeUserPresets.json content must remain exactly as authored on disk");
#endif
}

} // namespace

int main()
{
    try {
        testCandidateDiscoveryConfirmsRealConfiguredTargetsAndRejectsInventedOnes();
        std::cout << "VerificationEnvironmentCandidateDiscovery tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "VerificationEnvironmentCandidateDiscovery tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
