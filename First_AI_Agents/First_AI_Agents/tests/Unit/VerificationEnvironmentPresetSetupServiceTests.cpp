// AG-18.4B (reopened 2026-09-22): unit/integration tests for
// VerificationEnvironmentPresetSetupService, the stateless, provider-free
// typed contract that creates a minimal local CMakeUserPresets.json only
// after explicit user confirmation. Zero provider/Planner dependency is
// proven by construction here, not by a dedicated runtime test: the service
// is two static methods over a plain typed request/result pair, and neither
// this file nor the service itself references any provider/session type -
// matching this codebase's own established "by construction" convention
// (see e.g. ARCHITECTURE_GAPS.md's QML-P4R.6 canary entries).
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/InMemoryVerificationEnvironmentRepository.hpp"
#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/VerificationEnvironmentDiscoveryService.hpp"
#include "ai/application/VerificationEnvironmentPresetSetupService.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
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
        / ("ai-orchestrator-verif-preset-setup-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    TempDir() { fs::create_directories(path); }
    ~TempDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};

std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Could not open a real file for reading in the test");
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void writeFile(const fs::path& path, std::string_view content)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "Could not open a real file for writing in the test");
    stream << content;
}

// A Qt prefix the Backend accepts: it must exist and hold lib/cmake/Qt6/Qt6Config.cmake. The fixture is built here so that the tests
// do not depend on a Qt of one exact version being installed at one exact path on the machine that runs them (a hosted CI runner has none).
const std::string& fakeQtPrefix()
{
    struct Fixture {
        TempDir dir;
        std::string prefix;
        Fixture()
        {
            const fs::path root = dir.path / "Qt" / "6.11.1" / "msvc2022_64";
            fs::create_directories(root / "lib" / "cmake" / "Qt6");
            std::ofstream(root / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake") << "# test fixture\n";
            const auto utf8 = root.generic_u8string();
            prefix.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        }
    };
    static const Fixture fixture;
    return fixture.prefix;
}

CreateLocalVerificationPresetRequest sampleRequest(const fs::path& projectRoot)
{
    CreateLocalVerificationPresetRequest request;
    request.projectRoot = projectRoot;
    request.generator = "Ninja";
    request.configuration = "Debug";
    request.qtPrefixPath = fakeQtPrefix();
    request.configurePresetName = "local-qt-debug";
    request.buildPresetName = "local-qt-debug";
    request.binaryDirRelative = "build/local-qt-debug";
    return request;
}

class FakeVerificationCommandRunner final : public IManifestVerificationCommandRunner {
public:
    std::vector<ManifestVerificationCommandOutcome> outcomes;
    std::vector<ManifestVerificationCommandRequest> requests;

    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        requests.push_back(request);
        if (requests.size() <= outcomes.size()) return outcomes[requests.size() - 1];
        ManifestVerificationCommandOutcome fallback;
        fallback.started = true;
        fallback.exitCode = 0;
        return fallback;
    }
};

std::size_t countTempLeftovers(const fs::path& directory)
{
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos) ++count;
    }
    return count;
}

// Requirement 1: a valid typed configuration creates the expected JSON
// document, and createLocalPreset() writes exactly what
// composePresetDocument() previewed - never a second, divergent
// serialization.
void testValidConfigurationProducesExpectedDocumentAndFile()
{
    TempDir project;
    const auto request = sampleRequest(project.path);

    const auto preview = VerificationEnvironmentPresetSetupService::composePresetDocument(request);
    require(preview.success(), "Composing the preview for a valid request must succeed");
    const auto previewJson = nlohmann::json::parse(preview.value());
    require(previewJson["version"] == 6, "Document version must be 6");
    require(previewJson["configurePresets"][0]["name"] == "local-qt-debug", "Wrong configure preset name");
    require(previewJson["configurePresets"][0]["generator"] == "Ninja", "Wrong generator");
    require(previewJson["configurePresets"][0]["binaryDir"] == "${sourceDir}/build/local-qt-debug",
        "binaryDir must always carry the ${sourceDir} macro this service itself prepends");
    require(previewJson["configurePresets"][0]["cacheVariables"]["CMAKE_BUILD_TYPE"] == "Debug",
        "CMAKE_BUILD_TYPE must reflect the chosen configuration");
    require(previewJson["configurePresets"][0]["cacheVariables"]["CMAKE_PREFIX_PATH"]
            == fakeQtPrefix(),
        "CMAKE_PREFIX_PATH must be the user-selected Qt prefix verbatim");
    require(previewJson["buildPresets"][0]["name"] == "local-qt-debug", "Wrong build preset name");
    require(previewJson["buildPresets"][0]["configurePreset"] == "local-qt-debug",
        "Build preset must link back to its own configure preset");

    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(request);
    require(created.success(), "Creating a preset for a valid request must succeed");
    require(created.value().writtenPath == project.path / "CMakeUserPresets.json",
        "Must write to <projectRoot>/CMakeUserPresets.json, never CMakePresets.json");
    require(created.value().writtenJson == preview.value(),
        "The written bytes must be exactly the previewed bytes - never a second serialization");
    require(readFile(created.value().writtenPath) == preview.value(),
        "The bytes actually on disk must match the previewed/returned document exactly");
}

// Requirement 2: a repeat call for the same project never overwrites the
// file this service itself already wrote.
void testRepeatCreateNeverOverwritesOwnPreviouslyWrittenFile()
{
    TempDir project;
    const auto first = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(first.success(), "First creation must succeed");
    const std::string originalContent = readFile(first.value().writtenPath);

    auto secondRequest = sampleRequest(project.path);
    secondRequest.configuration = "Release";
    secondRequest.configurePresetName = "local-qt-release";
    secondRequest.buildPresetName = "local-qt-release";
    const auto second = VerificationEnvironmentPresetSetupService::createLocalPreset(secondRequest);
    require(!second.success() && second.error()->code == "verification_setup.preset_already_exists",
        "A second create for the same project must fail closed with the stable already-exists code");
    require(readFile(first.value().writtenPath) == originalContent,
        "The first, already-written preset must remain completely unchanged");
}

// Requirement 3: a pre-existing CMakeUserPresets.json this service never
// wrote - not one of its own - is never touched, merged, or read either.
void testExistingForeignPresetFileRemainsByteIdentical()
{
    TempDir project;
    const std::string foreignContent = "{\n  \"version\": 6,\n  \"foreign\": true\n}\n";
    writeFile(project.path / "CMakeUserPresets.json", foreignContent);

    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(!created.success() && created.error()->code == "verification_setup.preset_already_exists",
        "A pre-existing foreign file must fail closed with the same already-exists code");
    require(readFile(project.path / "CMakeUserPresets.json") == foreignContent,
        "A foreign, never-written-by-this-service file must remain byte-identical");
}

// Requirement 4: a symlink/reparse point sitting exactly at the target
// location is refused, never followed or overwritten. Skips gracefully (the
// same established convention as ManifestVerificationContinuationServiceTests.cpp's
// own symlink tests) when this machine has no privilege to create a real
// symlink.
void testSymlinkAtTargetLocationIsRejected()
{
    TempDir project;
    TempDir outside;
    writeFile(outside.path / "real.json", "{}\n");

    std::error_code linkError;
    fs::create_symlink(outside.path / "real.json",
        project.path / "CMakeUserPresets.json", linkError);
    if (linkError) {
        // No privilege to create a real symlink on this machine (Windows
        // without Developer Mode/admin) - this scenario cannot be exercised
        // here; never fail the suite over an environment gap.
        return;
    }

    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(!created.success(),
        "A symlink/reparse point at the target location must never be treated as writable");
    // WorkspacePathGuard rejects this one of two ways depending on exactly
    // how far symlink resolution gets: canonical() following the existing
    // leaf symlink all the way to its real, outside-the-root target trips
    // the containment check first (outside_workspace); a link that does not
    // fully resolve trips the explicit reparse-point scan instead
    // (reparse_point). Either is a genuine WorkspacePathGuard rejection -
    // the invariant this test actually cares about is that SOME such
    // rejection fires, never a silent follow-and-overwrite.
    require(created.error()->code == "workspace_path.outside_workspace"
            || created.error()->code == "workspace_path.reparse_point",
        "Must fail via one of WorkspacePathGuard's own escape-rejection codes, proving the shared "
        "guard ran rather than following the symlink");
    require(readFile(outside.path / "real.json") == "{}\n",
        "The symlink's real target content must never be read, written, or replaced");
}

// Requirement 5: unsafe inputs are rejected and nothing is ever written for
// any of them.
void testUnsafeInputsAreRejectedAndNothingIsWritten()
{
    struct Case {
        const char* label;
        std::function<void(CreateLocalVerificationPresetRequest&)> corrupt;
    };
    const std::vector<Case> cases = {
        {"unknown generator", [](auto& r) { r.generator = "Make"; }},
        {"unknown configuration", [](auto& r) { r.configuration = "RelWithDebInfo"; }},
        {"empty Qt prefix", [](auto& r) { r.qtPrefixPath = ""; }},
        {"relative Qt prefix", [](auto& r) { r.qtPrefixPath = "Qt/6.11.1/msvc2022_64"; }},
        {"nonexistent Qt prefix", [](auto& r) {
             r.qtPrefixPath = "C:/definitely-not-a-real-qt-install-xyz";
         }},
        {"Qt prefix with no Qt6 CMake package", [](auto& r) {
             // A real, existing directory - just not a Qt install. Must be
             // rejected on the Qt6Config.cmake check, not merely "not found".
             r.qtPrefixPath = fs::temp_directory_path().generic_string();
         }},
        {"empty preset name", [](auto& r) { r.configurePresetName = ""; }},
        {"preset name with path separator", [](auto& r) { r.configurePresetName = "a/b"; }},
        {"preset name with space", [](auto& r) { r.configurePresetName = "local qt"; }},
        {"absolute binaryDir", [](auto& r) { r.binaryDirRelative = "/build/local-qt-debug"; }},
        {"binaryDir with backslash", [](auto& r) { r.binaryDirRelative = "build\\local-qt-debug"; }},
        {"binaryDir traversal", [](auto& r) { r.binaryDirRelative = "../escape"; }},
        {"binaryDir with drive letter", [](auto& r) { r.binaryDirRelative = "C:/build"; }},
    };

    for (const auto& testCase : cases) {
        TempDir project;
        auto request = sampleRequest(project.path);
        testCase.corrupt(request);

        const auto preview = VerificationEnvironmentPresetSetupService::composePresetDocument(request);
        require(!preview.success(), (std::string("Preview must reject: ") + testCase.label).c_str());

        const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(request);
        require(!created.success(), (std::string("Create must reject: ") + testCase.label).c_str());
        require(!fs::exists(project.path / "CMakeUserPresets.json"),
            (std::string("Nothing may be written for: ") + testCase.label).c_str());
    }
}

// Requirement 6: a rejected attempt never leaves a false "configured" state
// - no target file, and no leftover temp sibling either.
void testFailedAttemptsLeaveNoFalseConfiguredStateOrLeftoverTempFiles()
{
    TempDir project;

    auto invalid = sampleRequest(project.path);
    invalid.configurePresetName = "";
    const auto rejected = VerificationEnvironmentPresetSetupService::createLocalPreset(invalid);
    require(!rejected.success(), "An invalid request must be rejected");
    require(!fs::exists(project.path / "CMakeUserPresets.json"),
        "A rejected request must leave no target file");
    require(countTempLeftovers(project.path) == 0,
        "A rejected request (which never reaches the write step) must leave no temp sibling");

    const auto discoveredAfterRejection = VerificationEnvironmentDiscoveryService::discoverForCandidateRoot(
        project.path, *std::make_unique<FakeVerificationCommandRunner>(), {});
    require(discoveredAfterRejection.success()
            && discoveredAfterRejection.value().state == VerificationEnvironmentState::SetupRequired,
        "No usable profile must be discoverable after a rejected, never-written attempt");

    // Now succeed once, then fail a second time via the already-exists path
    // - that failure must also leave no temp leftover.
    const auto first = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(first.success(), "First creation must succeed");
    const auto second = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(!second.success(), "Second creation must fail (already exists)");
    require(countTempLeftovers(project.path) == 0,
        "An already-exists rejection must leave no temp sibling either");
}

// Requirement 7: a preset this service generated is genuinely discoverable
// by the real, unmodified VerificationEnvironmentDiscoveryService as
// ExistingProfile - a real `cmake --list-presets` call, never a fake/scripted
// one, proving the generated JSON is actually valid CMake preset syntax, not
// merely valid per this service's own parser.
void testGeneratedPresetIsDiscoveredAsExistingProfileByRealCmake()
{
    TempDir project;
    writeFile(project.path / "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(preset_setup_fixture NONE)\n");

    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(created.success(), "Creating the real preset file must succeed");

    // `cmake --list-presets` only parses and lists JSON-declared presets -
    // it never invokes the generator - so this classification is meaningful
    // regardless of whether a real Ninja/Qt install exists on this machine.
    SystemManifestVerificationCommandRunner runner;
    const auto discovered = VerificationEnvironmentDiscoveryService::discoverForCandidateRoot(
        project.path, runner, {});
    require(discovered.success(), "Discovery against a real generated preset must not fail");
    require(discovered.value().state == VerificationEnvironmentState::ExistingProfile,
        "A real, cmake-confirmed generated preset must classify as ExistingProfile");
    require(discovered.value().profiles.size() == 1, "Expected exactly the one generated profile");
    require(discovered.value().profiles.front().configurePresetName == "local-qt-debug",
        "The discovered profile must be the exact preset this service generated");
}

// Requirement 8: a project whose CMake toolchain is unavailable still
// degrades to a typed, actionable SetupRequired diagnostic - proven here
// against a preset this service itself generated (the genuinely new
// end-to-end path this package introduces), not just against a hand-written
// fixture as VerificationEnvironmentDiscoveryServiceTests.cpp already does
// for the underlying discovery logic in isolation.
void testGeneratedPresetWithUnavailableCmakeDegradesToSetupRequired()
{
    TempDir project;
    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(created.success(), "Creating the real preset file must succeed");

    FakeVerificationCommandRunner commandRunner;
    ManifestVerificationCommandOutcome notStarted;
    notStarted.started = false;
    commandRunner.outcomes.push_back(notStarted);

    const auto discovered = VerificationEnvironmentDiscoveryService::discoverForCandidateRoot(
        project.path, commandRunner, {});
    require(discovered.success(),
        "cmake being unavailable must degrade to SetupRequired, never fail discovery outright");
    require(discovered.value().state == VerificationEnvironmentState::SetupRequired,
        "An unusable cmake probe must classify a generated-but-unconfirmed preset as SetupRequired");
    require(!discovered.value().diagnostic.empty(),
        "SetupRequired must carry a bounded, actionable diagnostic");
}

// Hardening: a genuine concurrent race between two create attempts for the
// same project must never corrupt the file or let both "win" - exactly one
// candidate's document is durably written, the other observes
// preset_already_exists, and the file on disk is exactly one of the two
// candidates, never a mix.
void testConcurrentDuplicateCreateExactlyOneWinsCleanly()
{
    TempDir project;
    auto requestA = sampleRequest(project.path);
    requestA.configurePresetName = "local-qt-debug";
    requestA.buildPresetName = "local-qt-debug";
    auto requestB = sampleRequest(project.path);
    requestB.configuration = "Release";
    requestB.configurePresetName = "local-qt-release";
    requestB.buildPresetName = "local-qt-release";

    common::Result<CreateLocalVerificationPresetResult> resultA {
        common::Result<CreateLocalVerificationPresetResult>::failure({"unset", "unset"})};
    common::Result<CreateLocalVerificationPresetResult> resultB {
        common::Result<CreateLocalVerificationPresetResult>::failure({"unset", "unset"})};

    std::thread threadA([&] {
        resultA = VerificationEnvironmentPresetSetupService::createLocalPreset(requestA);
    });
    std::thread threadB([&] {
        resultB = VerificationEnvironmentPresetSetupService::createLocalPreset(requestB);
    });
    threadA.join();
    threadB.join();

    const bool aWon = resultA.success();
    const bool bWon = resultB.success();
    require(aWon != bWon, "Exactly one of two racing creates must win - never both, never neither");

    const auto& winner = aWon ? resultA : resultB;
    const auto& loser = aWon ? resultB : resultA;
    require(loser.error()->code == "verification_setup.preset_already_exists",
        "The losing racer must observe the stable already-exists code, never a corrupt write error");
    require(readFile(project.path / "CMakeUserPresets.json") == winner.value().writtenJson,
        "The file on disk must be exactly the winner's own document - never a mix of both");
}

// Hardening: a project root containing non-ASCII characters must round-trip exactly - this codebase's own established UTF-8
// path convention (see FilesystemPathUtils.hpp's own pathFromUtf8 doc comment). Two names: Cyrillic and CJK, so that no single
// Windows ANSI code page can hold both (the first hosted CI run, an English runner, failed on the Cyrillic one with "No mapping
// for the Unicode character exists in the target multi-byte code page": the workspace checks converted path components with
// generic_string()). The names are spelled with \u escapes: the source file's encoding and the compiling machine's code page play no part.
void requireUnicodeProjectRootRoundTrips(const fs::path& folderName)
{
    TempDir parent;
    const fs::path unicodeRoot = parent.path / folderName;
    std::error_code mkdirError;
    fs::create_directories(unicodeRoot, mkdirError);
    require(!mkdirError, "Could not create a real Unicode-named directory for this test");

    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(unicodeRoot));
    require(created.success(), "Creating a preset under a Unicode project root must succeed");
    require(fs::exists(unicodeRoot / "CMakeUserPresets.json"),
        "The file must genuinely exist on disk under the real Unicode path");
    require(readFile(unicodeRoot / "CMakeUserPresets.json") == created.value().writtenJson,
        "Content must round-trip exactly under a Unicode project root");
}

void testUnicodeProjectRootRoundTrips()
{
    requireUnicodeProjectRootRoundTrips(fs::path(u8"Проект-Тест"));   // Cyrillic
    requireUnicodeProjectRootRoundTrips(fs::path(u8"项目-测试"));                                      // CJK
}

// Review follow-up, finding 1: a preset this service just wrote must be
// removable by its own compensating action when the caller's immediately-
// following Project-row creation then fails - and that action must be a
// no-op (still success) when there is nothing left to compensate, and must
// refuse to remove anything that is not a regular file.
void testDiscardGeneratedPresetCompensatesOrphanedWriteSafely()
{
    TempDir project;
    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(project.path));
    require(created.success(), "Creating the real preset file must succeed");
    require(fs::exists(created.value().writtenPath), "The preset must genuinely exist before discard");

    const auto discarded = VerificationEnvironmentPresetSetupService::discardGeneratedPreset(
        created.value());
    require(discarded.success(), "Discarding a preset this service just wrote must succeed");
    require(!fs::exists(created.value().writtenPath),
        "The preset file must genuinely be gone from disk after discard");

    const auto discardedAgain = VerificationEnvironmentPresetSetupService::discardGeneratedPreset(
        created.value());
    require(discardedAgain.success(),
        "Discarding an already-absent path must be a harmless no-op success, not a failure");

    // Refuses to remove a directory, even one that happens to sit at
    // exactly the path a preset could have occupied - never a
    // general-purpose delete.
    TempDir notAFile;
    CreateLocalVerificationPresetResult fabricated;
    fabricated.writtenPath = notAFile.path;
    fabricated.writtenJson = created.value().writtenJson;
    const auto refused = VerificationEnvironmentPresetSetupService::discardGeneratedPreset(
        fabricated);
    require(!refused.success() && refused.error()->code == "verification_setup.discard_refused",
        "discardGeneratedPreset() must refuse to remove anything that is not a regular file");
    require(fs::exists(notAFile.path), "The refused directory must remain completely untouched");

    TempDir changedProject;
    const auto changed = VerificationEnvironmentPresetSetupService::createLocalPreset(
        sampleRequest(changedProject.path));
    require(changed.success(), "Creating the second real preset file must succeed");
    writeFile(changed.value().writtenPath, "{\n  \"userEdited\": true\n}\n");
    const auto changedDiscard = VerificationEnvironmentPresetSetupService::discardGeneratedPreset(
        changed.value());
    require(!changedDiscard.success()
            && changedDiscard.error()->code == "verification_setup.discard_identity_conflict",
        "Compensation must fail closed when the generated file was edited or replaced");
    require(fs::exists(changed.value().writtenPath),
        "An edited or replaced preset must remain completely untouched");
}

} // namespace

int runVerificationEnvironmentPresetSetupServiceTests()
{
    const auto run = [](const char* name, auto test) {
        std::cerr << "[verification-preset] " << name << std::endl;
        test();
    };
    run("valid", testValidConfigurationProducesExpectedDocumentAndFile);
    run("repeat", testRepeatCreateNeverOverwritesOwnPreviouslyWrittenFile);
    run("foreign", testExistingForeignPresetFileRemainsByteIdentical);
    run("symlink", testSymlinkAtTargetLocationIsRejected);
    run("unsafe", testUnsafeInputsAreRejectedAndNothingIsWritten);
    run("failed-cleanup", testFailedAttemptsLeaveNoFalseConfiguredStateOrLeftoverTempFiles);
    run("real-cmake", testGeneratedPresetIsDiscoveredAsExistingProfileByRealCmake);
    run("unavailable-cmake", testGeneratedPresetWithUnavailableCmakeDegradesToSetupRequired);
    run("concurrent", testConcurrentDuplicateCreateExactlyOneWinsCleanly);
    run("unicode", testUnicodeProjectRootRoundTrips);
    run("compensation", testDiscardGeneratedPresetCompensatesOrphanedWriteSafely);

    std::cout << "VerificationEnvironmentPresetSetupServiceTests: all tests passed\n";
    return 0;
}
