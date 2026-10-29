#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/ProjectSetupReadiness.hpp"
#include "ai/application/verification/VerificationEnvironmentPresetSetupService.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace ai::application;

void require(bool condition, const std::string& message)
{
    // Active in Release builds, unlike assert().
    if (!condition) throw std::runtime_error(message);
}

struct TestDirectory {
    fs::path path;

    explicit TestDirectory(const std::string& tag)
    {
        static int counter = 0;
        path = fs::temp_directory_path()
            / ("aio-psr-" + tag + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)
                  + "-" + std::to_string(++counter));
        std::error_code error;
        fs::create_directories(path, error);
        require(!error, "could not create the test directory");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        fs::remove_all(detail::longPath(path), ignored);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
};

void writeFile(const fs::path& path, const std::string& content)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary) << content;
}

bool isUtf8(const std::string& text)
{
    for (std::size_t at = 0; at < text.size();) {
        const auto byte = static_cast<unsigned char>(text[at]);
        std::size_t length = 1;
        if (byte >= 0x80) {
            if (byte >= 0xC2 && byte <= 0xDF) length = 2;
            else if (byte >= 0xE0 && byte <= 0xEF) length = 3;
            else if (byte >= 0xF0 && byte <= 0xF4) length = 4;
            else return false;
            if (at + length > text.size()) return false;
            for (std::size_t i = 1; i < length; ++i) {
                if ((static_cast<unsigned char>(text[at + i]) & 0xC0) != 0x80) return false;
            }
        }
        at += length;
    }
    return true;
}

// ---- a scripted cmake ----

struct FakeCmake final : IManifestVerificationCommandRunner {
    std::vector<std::vector<std::string>> calls;
    std::vector<fs::path> workingDirectories;
    // Preset listings, by kind.
    std::string configureListing {"Available configure presets:\n\n  \"local\" - Local\n"};
    std::string buildListing {"Available build presets:\n\n  \"local-build\" - Local build\n"};
    // What `cmake --preset <name>` does.
    int configureExit {0};
    std::string configureOutput {"-- Configuring done\n"};
    bool configureCancelled {false};
    std::string cacheCompiler {"C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51/bin/Hostx64/x64/cl.exe"};
    std::string cacheGenerator {"Ninja"};
    std::string binaryDir {"build/local"};
    // What `cmake --build --preset <name> --target help` does.
    int targetsExit {0};
    std::string targetsOutput {"The following are some of the valid targets for this Makefile:\n... all (the default if no target "
                               "is provided)\n... app\n... app_qmllint\n"};

    fs::path root;

    ManifestVerificationCommandOutcome run(const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        calls.push_back(request.arguments);
        workingDirectories.push_back(request.workingDirectory);
        ManifestVerificationCommandOutcome outcome;
        outcome.started = true;
        outcome.exitCode = 0;
        const auto& args = request.arguments;
        if (!args.empty() && args[0] == "--list-presets=configure") {
            outcome.boundedOutput = configureListing;
        } else if (!args.empty() && args[0] == "--list-presets=build") {
            outcome.boundedOutput = buildListing;
        } else if (args.size() == 2 && args[0] == "--preset") {
            if (configureCancelled) {
                outcome.cancelled = true;
                return outcome;
            }
            outcome.exitCode = configureExit;
            outcome.boundedOutput = configureOutput;
            if (configureExit == 0) {
                writeFile(root / binaryDir / "CMakeCache.txt",
                    "CMAKE_BUILD_TYPE:STRING=Debug\nCMAKE_CXX_COMPILER:FILEPATH=" + cacheCompiler + "\n"
                        + (cacheGenerator.empty() ? std::string() : "CMAKE_GENERATOR:INTERNAL=" + cacheGenerator + "\n"));
            }
        } else if (!args.empty() && args[0] == "--build") {
            outcome.exitCode = targetsExit;
            outcome.boundedOutput = targetsOutput;
        } else {
            outcome.exitCode = 99;
        }
        return outcome;
    }

    [[nodiscard]] std::size_t callsStartingWith(const std::string& first) const
    {
        std::size_t count = 0;
        for (const auto& call : calls) {
            if (!call.empty() && call[0] == first) ++count;
        }
        return count;
    }
};

const char* kSinglePreset = R"json({
  "version": 3,
  "configurePresets": [
    {"name": "local", "generator": "Ninja", "binaryDir": "${sourceDir}/build/local",
     "cacheVariables": {"CMAKE_PREFIX_PATH": "C:/Qt/6.11.1/msvc2022_64"}}
  ],
  "buildPresets": [{"name": "local-build", "configurePreset": "local"}]
})json";

ProjectSetupReadiness::ToolchainProvider readyToolchain()
{
    return [] {
        VerificationToolchainEnvironment environment;
        environment.prepared = true;
        environment.identity = "msvc=test;sdk=test;vs=test";
        return environment;
    };
}

ProjectSetupCheck runCheck(FakeCmake& cmake, const fs::path& root, const std::optional<ProjectSetupSelection>& saved = {},
    const ProjectSetupReadiness::ToolchainProvider& toolchain = readyToolchain())
{
    cmake.root = root;
    return ProjectSetupReadiness::check(root, saved, cmake, {}, toolchain);
}

// ---- the tests ----

void testASingleUsablePairIsConfiguredAndReportsItsTargets()
{
    TestDirectory project("ok");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake cmake;
    const auto result = runCheck(cmake, project.path);
    require(result.ready, "A single pair that configures and lists targets is ready: " + result.failureMessage);
    require(result.selection == ProjectSetupSelection {"local", "local-build"}, "The only pair is chosen");
    require(result.targetsComplete && result.targets.size() >= 2, "Targets are confirmed");
    require(std::find(result.targets.begin(), result.targets.end(), "app_qmllint") != result.targets.end(),
        "The QML-lint target is part of the evidence");
    require(result.toolchainIdentity == "msvc=test;sdk=test;vs=test", "The toolchain the evidence belongs to is recorded");
    require(cmake.callsStartingWith("--preset") == 1, "Exactly one real configure ran");
    bool configuredInTheProject = false;
    for (std::size_t i = 0; i < cmake.calls.size(); ++i) {
        if (cmake.calls[i].size() == 2 && cmake.calls[i][0] == "--preset") {
            configuredInTheProject = cmake.calls[i][1] == "local" && cmake.workingDirectories[i] == project.path;
        }
    }
    require(configuredInTheProject, "The configure ran the selected preset in the project root");
}

void testAFailedConfigureIsNotReadyAndShowsTheLinkerLine()
{
    TestDirectory project("cfg");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake cmake;
    cmake.configureExit = 1;
    // Real text of the observed failure, with one line in a console code page (not UTF-8).
    cmake.configureOutput = "CMake Error: the C++ compiler is not able to compile a simple test program.\r\n"
                            "LINK : fatal error LNK1104: \x8d\xa5 \xe3\xa4\xa0\xa5\xe2\xe1\xef \xae\xe2\xaa\xe0\xeb\xe2\xec \xe4\xa0\xa9\xab "
                            "\"kernel32.lib\"\r\nninja: build stopped: subcommand failed.\r\n";
    const auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupConfigureFailed, "A failed configure is not ready: " + result.failureCode);
    require(result.failureMessage.find("LNK1104") != std::string::npos, "The linker line is shown");
    require(result.failureMessage.find("Windows SDK") != std::string::npos, "A missing SDK library gets its own instruction");
    require(result.failureMessage.find("check again") != std::string::npos, "The message ends with what to do");
    require(isUtf8(result.failureMessage), "A console-code-page line is converted, never stored as invalid UTF-8");
    require(result.targets.empty() && cmake.callsStartingWith("--build") == 0, "No target is claimed after a failed configure");
}

void testDisplayableTailKeepsTheEndAndNeverBreaksACharacter()
{
    const std::string utf8Line = "\xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0"; // "папка"
    const std::string tail = ProjectSetupReadiness::displayableTail("first\r\n" + utf8Line + "\r\n\x01second\r\n\r\n", 1000);
    require(tail == "first\n" + utf8Line + "\nsecond", "Lines are cleaned, CR and control characters dropped: " + tail);
    const std::string limited = ProjectSetupReadiness::displayableTail("aaaa\nbbbb\ncccc\n", 9);
    require(limited == "bbbb\ncccc", "Only the last lines that fit are kept: " + limited);
    const std::string oneLong = ProjectSetupReadiness::displayableTail(std::string(50, 'x') + utf8Line, 7);
    require(isUtf8(oneLong) && oneLong.size() <= 7, "A long line is cut from the front on a character boundary");
    require(ProjectSetupReadiness::displayableTail("", 100).empty(), "Nothing in, nothing out");
    require(isUtf8(ProjectSetupReadiness::displayableTail("bad \xFF\xFE byte\n", 100)), "Invalid bytes never survive");
}

void testNoToolchainStopsBeforeAnyConfigureAndNamesTheReason()
{
    TestDirectory project("tc");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake cmake;
    const auto result = runCheck(cmake, project.path, {}, [] {
        VerificationToolchainEnvironment environment;
        environment.errorCode = kToolchainWindowsSdkMissing;
        environment.message = "Install a Windows SDK and check again.";
        return environment;
    });
    require(!result.ready && result.failureCode == kProjectSetupToolchainUnavailable, "No toolchain: " + result.failureCode);
    require(result.failureMessage.find(kToolchainWindowsSdkMissing) != std::string::npos, "The toolchain's own code is shown");
    require(cmake.callsStartingWith("--preset") == 0 && cmake.callsStartingWith("--build") == 0,
        "Nothing is configured or built without a toolchain");
}

void testSeveralPairsAreNeverGuessed()
{
    TestDirectory project("many");
    writeFile(project.path / "CMakePresets.json", R"json({
      "version": 3,
      "configurePresets": [
        {"name": "debug", "generator": "Ninja", "binaryDir": "${sourceDir}/build/debug"},
        {"name": "release", "generator": "Ninja", "binaryDir": "${sourceDir}/build/release"}],
      "buildPresets": [
        {"name": "debug-build", "configurePreset": "debug"},
        {"name": "release-build", "configurePreset": "release"}]
    })json");
    FakeCmake cmake;
    cmake.configureListing = "Available configure presets:\n\n  \"debug\"\n  \"release\"\n";
    cmake.buildListing = "Available build presets:\n\n  \"debug-build\"\n  \"release-build\"\n";
    auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupSelectionRequired, "Two pairs need the user: " + result.failureCode);
    require(result.candidates.size() == 2, "Both pairs are offered");
    require(result.failureMessage.find("debug / debug-build") != std::string::npos
            && result.failureMessage.find("release / release-build") != std::string::npos,
        "The message lists them");
    require(cmake.callsStartingWith("--preset") == 0, "Nothing is configured while the choice is open");

    FakeCmake chosen;
    chosen.configureListing = cmake.configureListing;
    chosen.buildListing = cmake.buildListing;
    chosen.binaryDir = "build/release";
    result = runCheck(chosen, project.path, ProjectSetupSelection {"release", "release-build"});
    require(result.ready && result.selection.configurePreset == "release", "The user's choice is used: " + result.failureMessage);
}

void testOnlyTheCompatiblePairCountsForTheAutomaticChoice()
{
    TestDirectory project("mixed");
    writeFile(project.path / "CMakePresets.json", R"json({
      "version": 3,
      "configurePresets": [
        {"name": "msvc", "generator": "Ninja", "binaryDir": "${sourceDir}/build/local",
         "cacheVariables": {"CMAKE_PREFIX_PATH": "C:/Qt/6.11.1/msvc2022_64"}},
        {"name": "mingw", "generator": "Ninja", "binaryDir": "${sourceDir}/build/mingw",
         "cacheVariables": {"CMAKE_PREFIX_PATH": "C:/Qt/6.11.1/mingw_64"}}],
      "buildPresets": [
        {"name": "msvc-build", "configurePreset": "msvc"},
        {"name": "mingw-build", "configurePreset": "mingw"}]
    })json");
    FakeCmake cmake;
    cmake.configureListing = "Available configure presets:\n\n  \"msvc\"\n  \"mingw\"\n";
    cmake.buildListing = "Available build presets:\n\n  \"msvc-build\"\n  \"mingw-build\"\n";
    const auto result = runCheck(cmake, project.path);
    require(result.ready && result.selection == ProjectSetupSelection {"msvc", "msvc-build"},
        "The single MSVC pair is chosen automatically: " + result.failureMessage);
}

void testAMinGwOrArmSetupIsRefusedWithTheLimitationBeforeAnyConfigure()
{
    for (const char* variant : {"mingw", "arm64", "arch", "generator"}) {
        TestDirectory project(variant);
        std::string preset;
        const std::string name = variant;
        if (name == "mingw") {
            preset = R"({"name":"local","generator":"Ninja","binaryDir":"${sourceDir}/build/local","cacheVariables":{"CMAKE_PREFIX_PATH":"C:/Qt/6.11.1/mingw_64"}})";
        } else if (name == "arm64") {
            preset = R"({"name":"local","generator":"Ninja","binaryDir":"${sourceDir}/build/local","cacheVariables":{"CMAKE_PREFIX_PATH":{"type":"PATH","value":"C:/Qt/6.11.1/msvc2022_arm64"}}})";
        } else if (name == "arch") {
            preset = R"({"name":"local","generator":"Ninja","architecture":{"value":"arm64","strategy":"external"},"binaryDir":"${sourceDir}/build/local"})";
        } else {
            preset = R"({"name":"local","generator":"MinGW Makefiles","binaryDir":"${sourceDir}/build/local"})";
        }
        writeFile(project.path / "CMakePresets.json",
            "{\"version\":3,\"configurePresets\":[" + preset + "],\"buildPresets\":[{\"name\":\"local-build\",\"configurePreset\":\"local\"}]}");
        FakeCmake cmake;
        const auto result = runCheck(cmake, project.path);
        require(!result.ready && result.failureCode == kProjectSetupToolchainUnsupported,
            std::string(variant) + " is refused as unsupported: " + result.failureCode);
        require(result.failureMessage.find("MSVC x64") != std::string::npos, "The limitation is stated plainly");
        require(cmake.callsStartingWith("--preset") == 0, std::string(variant) + ": nothing is configured");
    }
}

void testUnsupportedGeneratorsStopBeforeToolchainConfigureAndCachedTargetProbes()
{
    for (const std::string generator : {"Visual Studio 18 2026", "Ninja Multi-Config", "Unix Makefiles"}) {
        TestDirectory project("generator");
        writeFile(project.path / "CMakePresets.json",
            "{\"version\":3,\"configurePresets\":[{\"name\":\"local\",\"generator\":\"" + generator
                + "\",\"binaryDir\":\"${sourceDir}/build/local\"}],"
                  "\"buildPresets\":[{\"name\":\"local-build\",\"configurePreset\":\"local\"}]}");
        // A previous configure must not cause catalog discovery to probe this unsupported generator's targets.
        writeFile(project.path / "build/local/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=" + generator + "\n");
        for (const bool useSaved : {false, true}) {
            FakeCmake cmake;
            int toolchainCalls = 0;
            const auto saved = useSaved ? std::optional<ProjectSetupSelection> {{"local", "local-build"}} : std::nullopt;
            const auto result = runCheck(cmake, project.path, saved, [&] {
                ++toolchainCalls;
                return readyToolchain()();
            });
            require(!result.ready && result.failureCode == "project_setup.generator_unsupported",
                generator + " is refused with its own code: " + result.failureCode);
            require(result.failureMessage.find(generator) != std::string::npos
                    && result.failureMessage.find("Ninja") != std::string::npos
                    && result.failureMessage.find("MSVC x64") != std::string::npos,
                "The refusal names the generator and the supported alternative");
            require(toolchainCalls == 0 && cmake.callsStartingWith("--preset") == 0
                    && cmake.callsStartingWith("--build") == 0 && result.targets.empty(),
                "An unsupported generator prepares no toolchain, configures nothing and probes no cached targets");
        }
    }
}

void testGeneratorInheritanceUsesIncludesParentOrderAndChildOverrides()
{
    const std::vector<std::pair<std::string, bool>> variants {
        {R"("inherits":["vs","ninja"])", false},
        {R"("inherits":["ninja","vs"])", true},
        {R"("inherits":"vs","generator":"Ninja")", true},
    };
    for (const auto& [fields, supported] : variants) {
        TestDirectory project("generator-inheritance");
        writeFile(project.path / "CMakePresets.json",
            "{\"version\":4,\"include\":[\"cmake/generators.json\"],\"configurePresets\":[{\"name\":\"local\","
                + fields + ",\"binaryDir\":\"${sourceDir}/build/local\"}],"
                           "\"buildPresets\":[{\"name\":\"local-build\",\"configurePreset\":\"local\"}]}");
        writeFile(project.path / "cmake/generators.json", R"json({
          "version":4,"configurePresets":[
            {"name":"vs-root","hidden":true,"generator":"Visual Studio 18 2026"},
            {"name":"vs","hidden":true,"inherits":"vs-root"},
            {"name":"ninja","hidden":true,"generator":"Ninja"}]
        })json");
        FakeCmake cmake;
        const auto result = runCheck(cmake, project.path);
        require(result.ready == supported, "Generator inheritance follows CMake precedence: " + result.failureMessage);
        if (!supported) {
            require(result.failureCode == "project_setup.generator_unsupported"
                    && cmake.callsStartingWith("--preset") == 0 && cmake.callsStartingWith("--build") == 0,
                "An inherited unsupported generator stops before configure");
        }
    }
}

void testOnlyNinjaPairsAreOfferedButASavedUnsupportedPairIsNeverReplaced()
{
    TestDirectory project("generator-choice");
    writeFile(project.path / "CMakePresets.json", R"json({
      "version":3,"configurePresets":[
        {"name":"local","generator":"Ninja","binaryDir":"${sourceDir}/build/local"},
        {"name":"vs","generator":"Visual Studio 18 2026","binaryDir":"${sourceDir}/build/vs"}],
      "buildPresets":[{"name":"local-build","configurePreset":"local"},{"name":"vs-build","configurePreset":"vs"}]
    })json");
    for (const bool useSaved : {false, true}) {
        FakeCmake cmake;
        cmake.configureListing = "Available configure presets:\n  \"local\"\n  \"vs\"\n";
        cmake.buildListing = "Available build presets:\n  \"local-build\"\n  \"vs-build\"\n";
        const auto saved = useSaved ? std::optional<ProjectSetupSelection> {{"vs", "vs-build"}} : std::nullopt;
        const auto result = runCheck(cmake, project.path, saved);
        if (useSaved) {
            require(!result.ready && result.failureCode == "project_setup.generator_unsupported"
                    && result.selection == *saved && cmake.callsStartingWith("--preset") == 0,
                "A saved Visual Studio pair is refused instead of silently switching to Ninja");
        } else {
            require(result.ready && result.selection == ProjectSetupSelection {"local", "local-build"},
                "The single supported generator pair is selected automatically: " + result.failureMessage);
        }
    }
}

void testAMissingGeneratorUsesCMakeWithoutChangingTheUserPreset()
{
    const std::string presets = R"json({
      "version":3,"configurePresets":[{"name":"local","binaryDir":"${sourceDir}/build/local"}],
      "buildPresets":[{"name":"local-build","configurePreset":"local"}]
    })json";
    for (const std::string actualGenerator : {"Ninja", "Visual Studio 18 2026", ""}) {
        for (const bool existingCache : {false, true}) {
            TestDirectory project("generator-missing");
            writeFile(project.path / "CMakePresets.json", presets);
            if (existingCache) {
                writeFile(project.path / "build/local/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n");
            }
            FakeCmake cmake;
            cmake.cacheGenerator = actualGenerator;
            const auto result = runCheck(cmake, project.path);
            require(cmake.callsStartingWith("--preset") == 1,
                "An omitted generator is resolved by CMake, not rejected or supplied by the application");
            if (actualGenerator == "Ninja") {
                require(result.ready && cmake.callsStartingWith("--build") == 1,
                    "CMake's confirmed default or cached Ninja generator is usable: " + result.failureMessage);
            } else {
                const std::string code = actualGenerator.empty()
                    ? "project_setup.generator_unconfirmed" : "project_setup.generator_unsupported";
                require(!result.ready && result.failureCode == code && cmake.callsStartingWith("--build") == 0,
                    "Only a confirmed supported generator may reach the target probe: " + result.failureCode);
                require(actualGenerator.empty() || result.failureMessage.find(actualGenerator) != std::string::npos,
                    "The refusal names CMake's actual unsupported generator");
            }
            for (const auto& call : cmake.calls) {
                if (!call.empty() && call.front() == "--preset") {
                    require(call == std::vector<std::string> {"--preset", "local"},
                        "The application invokes the user's preset without adding a generator override");
                }
            }
            std::ifstream saved(project.path / "CMakePresets.json", std::ios::binary);
            require(std::string(std::istreambuf_iterator<char>(saved), {}) == presets,
                "The user's preset file stays byte-identical");
        }
    }
}

void testAMissingBuildToolKeepsCMakeFailureDistinctFromAnUnsupportedGenerator()
{
    TestDirectory project("generator-tool-missing");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake cmake;
    cmake.configureExit = 1;
    cmake.configureOutput = "CMake Error: CMAKE_MAKE_PROGRAM is not set. Ninja could not be found.\n";
    const auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupConfigureFailed
            && result.failureMessage.find("CMAKE_MAKE_PROGRAM") != std::string::npos,
        "A missing Ninja executable is reported as CMake's configure failure, not an unsupported generator");
    require(cmake.callsStartingWith("--build") == 0, "No target probe follows a missing build tool");
}

void testGeneratorRuleReadsUserInheritanceButNeverIncludesOutsideTheProject()
{
    TestDirectory project("generator-user");
    writeFile(project.path / "CMakePresets.json", R"json({
      "version":4,"configurePresets":[{"name":"base","hidden":true,"generator":"Ninja"}]
    })json");
    writeFile(project.path / "CMakeUserPresets.json", R"json({
      "version":4,"configurePresets":[{"name":"local","inherits":"base","binaryDir":"${sourceDir}/build/local"}],
      "buildPresets":[{"name":"local-build","configurePreset":"local"}]
    })json");
    FakeCmake cmake;
    require(runCheck(cmake, project.path).ready, "A user preset can inherit Ninja from the project preset file");

    TestDirectory outside("generator-outside");
    writeFile(outside.path / "presets.json", R"json({
      "version":4,"configurePresets":[{"name":"foreign","hidden":true,"generator":"Visual Studio 18 2026"}]
    })json");
    writeFile(project.path / "CMakeUserPresets.json",
        "{\"version\":4,\"include\":[\"../" + detail::genericUtf8(outside.path.filename())
            + "/presets.json\"],\"configurePresets\":[{\"name\":\"local\",\"inherits\":\"foreign\","
              "\"binaryDir\":\"${sourceDir}/build/local\"}]}");
    require(!ProjectSetupReadiness::unsupportedGeneratorReason(project.path, "local").has_value(),
        "The early reader does not inspect the out-of-project include or invent a generator verdict");
}

void testUnresolvedGeneratorInheritanceNeverFallsBackToTheNextParent()
{
    TestDirectory project("generator-depth");
    std::string presets = R"({"version":3,"configurePresets":[
      {"name":"local","inherits":["deep0","ninja"],"binaryDir":"${sourceDir}/build/local"},
      {"name":"ninja","hidden":true,"generator":"Ninja"})";
    for (int i = 0; i < 10; ++i) {
        presets += ",{\"name\":\"deep" + std::to_string(i) + "\",\"hidden\":true,";
        presets += i == 9 ? "\"generator\":\"Visual Studio 18 2026\"}"
                          : "\"inherits\":\"deep" + std::to_string(i + 1) + "\"}";
    }
    presets += R"(],"buildPresets":[{"name":"local-build","configurePreset":"local"}]})";
    writeFile(project.path / "CMakePresets.json", presets);
    FakeCmake cmake;
    cmake.cacheGenerator = "Visual Studio 18 2026";
    const auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupGeneratorUnsupported
            && cmake.callsStartingWith("--preset") == 1 && cmake.callsStartingWith("--build") == 0,
        "An unresolved first parent is left to CMake; its actual generator cannot be replaced by the next parent's Ninja");
}

void testACompilerThatIsNotMsvcIsRefusedAfterConfigure()
{
    TestDirectory project("cache");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake cmake;
    cmake.cacheCompiler = "C:/Qt/Tools/mingw1310_64/bin/g++.exe";
    const auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupToolchainUnsupported, "A g++ configure is not MSVC: " + result.failureCode);
    require(result.failureMessage.find("g++.exe") != std::string::npos, "The compiler CMake chose is named");
    require(cmake.callsStartingWith("--build") == 0, "No target evidence is taken from a foreign toolchain");
}

void testUnconfirmedTargetsAreNotReady()
{
    TestDirectory project("tg");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake cmake;
    cmake.targetsExit = 1;
    const auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupTargetsUnconfirmed, "No targets, not ready: " + result.failureCode);
    require(result.failureMessage.find("check again") != std::string::npos, "The message ends with what to do");
    require(result.targets.empty(), "No target is invented");
}

void testCancellationNeverLooksLikeReadiness()
{
    TestDirectory project("cx");
    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake early;
    early.root = project.path;
    std::stop_source stopped;
    stopped.request_stop();
    auto result = ProjectSetupReadiness::check(project.path, {}, early, stopped.get_token(), readyToolchain());
    require(!result.ready && result.failureCode == kProjectSetupCancelled && early.calls.empty(),
        "A cancelled check runs nothing and is not ready");

    FakeCmake midway;
    midway.configureCancelled = true;
    result = runCheck(midway, project.path);
    require(!result.ready && result.failureCode == kProjectSetupCancelled, "A configure cancelled midway is not ready");
    require(midway.callsStartingWith("--build") == 0, "No target probe follows a cancelled configure");
}

void testMissingPresetsAreNamedWithoutRunningCmake()
{
    TestDirectory project("none");
    FakeCmake cmake;
    auto result = runCheck(cmake, project.path);
    require(!result.ready && result.failureCode == kProjectSetupPresetNotFound, "No preset file: " + result.failureCode);
    require(result.failureMessage.find("preset") != std::string::npos && cmake.calls.empty(),
        "The reason is explained and cmake is not even started");

    writeFile(project.path / "CMakePresets.json", kSinglePreset);
    FakeCmake stale;
    result = runCheck(stale, project.path, ProjectSetupSelection {"gone", "gone-build"});
    require(!result.ready && result.failureCode == kProjectSetupPresetNotFound,
        "A saved pair that no longer exists is not silently replaced: " + result.failureCode);
    require(stale.callsStartingWith("--preset") == 0, "Nothing is configured for a vanished selection");
}

void testToolchainRuleFollowsInheritsAndIncludesButStaysInsideTheProject()
{
    TestDirectory project("inh");
    writeFile(project.path / "CMakePresets.json", R"json({
      "version": 4,
      "include": ["cmake/base.json"],
      "configurePresets": [
        {"name": "child", "inherits": ["hidden"], "binaryDir": "${sourceDir}/build/child"},
        {"name": "toolchain", "toolchainFile": "C:/tools/mingw-w64.cmake", "binaryDir": "${sourceDir}/build/t"},
        {"name": "clean", "generator": "Ninja", "binaryDir": "${sourceDir}/build/clean",
         "cacheVariables": {"CMAKE_PREFIX_PATH": "C:/Qt/6.11.1/msvc2022_64", "CMAKE_BUILD_TYPE": "Debug"}},
        {"name": "outside", "inherits": "from_outside"}]
    })json");
    writeFile(project.path / "cmake" / "base.json", R"json({
      "version": 4,
      "configurePresets": [{"name": "hidden", "hidden": true, "architecture": {"value": "ARM64"}}]
    })json");
    const fs::path outsideDirectory = project.path.parent_path() / (project.path.filename().string() + "-outside");
    writeFile(outsideDirectory / "evil.json", R"json({"configurePresets":[{"name":"from_outside","generator":"MinGW Makefiles"}]})json");
    // An include that climbs out of the project must not be read.
    writeFile(project.path / "CMakeUserPresets.json",
        "{\"version\":4,\"include\":[\"../" + outsideDirectory.filename().string() + "/evil.json\"],\"configurePresets\":[]}");

    require(ProjectSetupReadiness::unsupportedToolchainReason(project.path, "child").has_value(),
        "An architecture inherited from an included, hidden preset is found");
    require(ProjectSetupReadiness::unsupportedToolchainReason(project.path, "toolchain").has_value(),
        "A MinGW toolchain file is found");
    require(!ProjectSetupReadiness::unsupportedToolchainReason(project.path, "clean").has_value(),
        "A plain MSVC Qt preset is not refused");
    require(!ProjectSetupReadiness::unsupportedToolchainReason(project.path, "outside").has_value(),
        "A file outside the project is never read to decide anything");
    require(!ProjectSetupReadiness::unsupportedToolchainReason(project.path, "unknown").has_value(),
        "An unknown preset has no verdict here (the catalog decides existence)");
    std::error_code ignored;
    fs::remove_all(outsideDirectory, ignored);
}

void testCompilerRuleReadsTheCacheOfTheBuildTree()
{
    TestDirectory directory("cc");
    const auto cache = directory.path / "CMakeCache.txt";
    require(!ProjectSetupReadiness::unsupportedCompilerReason(cache).has_value(), "No cache, no verdict");
    writeFile(cache, "CMAKE_C_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/14.51/bin/Hostx64/x64/cl.exe\n"
                     "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/14.51/bin/Hostx64/x64/cl.exe\r\n");
    require(!ProjectSetupReadiness::unsupportedCompilerReason(cache).has_value(), "MSVC x64 is accepted");
    writeFile(cache, "CMAKE_CXX_COMPILER:FILEPATH=C:\\VS\\VC\\Tools\\MSVC\\14.51\\bin\\Hostx64\\arm64\\cl.exe\n");
    require(ProjectSetupReadiness::unsupportedCompilerReason(cache).has_value(), "The ARM64 cross compiler is refused");
    writeFile(cache, "CMAKE_CXX_COMPILER:FILEPATH=C:/mingw/bin/c++.exe\n");
    require(ProjectSetupReadiness::unsupportedCompilerReason(cache).has_value(), "A non-MSVC compiler is refused");
    writeFile(cache, "CMAKE_C_COMPILER:FILEPATH=C:/mingw/bin/gcc.exe\n");
    require(ProjectSetupReadiness::unsupportedCompilerReason(cache).has_value(), "The C compiler is used when no C++ one is listed");
}

#ifdef _WIN32

// The real thing, end to end, from a process that has no Visual Studio environment (a freshly opened application): the real
// runner, a real cmake and Ninja, a real project. Skipped, and said so, on a computer without Visual Studio.
void testARealProjectBecomesReadyFromAnUnpreparedProcessAndABrokenOneSaysWhy()
{
    for (const wchar_t* name : {L"VCINSTALLDIR", L"LIB", L"INCLUDE", L"LIBPATH"}) _wputenv_s(name, L"");
    VerificationToolchain::forgetCachedResult();
    const auto toolchain = VerificationToolchain::current();
    if (!toolchain.prepared) {
        std::cout << "  (no MSVC on this computer: " << toolchain.errorCode << " - real-project check skipped)" << std::endl;
        return;
    }

    TestDirectory project("real");
    writeFile(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(aio_ready CXX)\n"
                                              "add_executable(aio_ready main.cpp)\n");
    writeFile(project.path / "main.cpp", "int main() { return 0; }\n");
    writeFile(project.path / "CMakePresets.json", R"json({
      "version": 3,
      "configurePresets": [{"name": "local", "generator": "Ninja", "binaryDir": "${sourceDir}/build/local"}],
      "buildPresets": [{"name": "local-build", "configurePreset": "local"}]
    })json");
    SystemManifestVerificationCommandRunner runner;
    auto result = ProjectSetupReadiness::check(project.path, {}, runner, {});
    require(result.ready, "A real project is ready from an unprepared process: [" + result.failureCode + "] " + result.failureMessage);
    require(std::find(result.targets.begin(), result.targets.end(), "aio_ready") != result.targets.end(),
        "The real target catalog contains the project's own target");
    require(result.toolchainIdentity.rfind("msvc=", 0) == 0, "The identity names the toolchain: " + result.toolchainIdentity);
    require(fs::exists(project.path / "build" / "local" / "CMakeCache.txt"), "The preset's own build tree was configured");

    // A user's valid preset need not repeat the generator already selected for its existing build tree.
    const std::string implicitPreset = R"json({
      "version":3,"configurePresets":[{"name":"local","binaryDir":"${sourceDir}/build/local"}],
      "buildPresets":[{"name":"local-build","configurePreset":"local"}]
    })json";
    writeFile(project.path / "CMakePresets.json", implicitPreset);
    result = ProjectSetupReadiness::check(project.path, {}, runner, {});
    require(result.ready, "Real CMake reuses the user's cached generator when the preset omits it: " + result.failureMessage);
    std::ifstream unchangedPreset(project.path / "CMakePresets.json", std::ios::binary);
    require(std::string(std::istreambuf_iterator<char>(unchangedPreset), {}) == implicitPreset,
        "The real readiness check leaves the user's preset unchanged");

    TestDirectory broken("broken");
    writeFile(broken.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(aio_broken CXX)\n"
                                             "this_command_does_not_exist()\n");
    writeFile(broken.path / "CMakePresets.json", R"json({
      "version": 3,
      "configurePresets": [{"name": "local", "generator": "Ninja", "binaryDir": "${sourceDir}/build/local"}],
      "buildPresets": [{"name": "local-build", "configurePreset": "local"}]
    })json");
    result = ProjectSetupReadiness::check(broken.path, {}, runner, {});
    require(!result.ready && result.failureCode == kProjectSetupConfigureFailed, "A project that does not configure is not ready");
    require(result.failureMessage.find("this_command_does_not_exist") != std::string::npos,
        "cmake's own error is shown: " + result.failureMessage);
}

// The scenario that was reported from a fresh Windows account, with the application's OWN preset generator and a real Qt Quick
// project: opt-in (it needs a Qt kit and takes about a minute), enabled by AIO_TEST_QT_PREFIX=<an MSVC 64-bit Qt kit> and, to
// also prove the refusal, AIO_TEST_QT_MINGW_PREFIX=<a MinGW Qt kit>.
void writeQtQuickProject(const fs::path& root)
{
    writeFile(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(aio_qt_ready LANGUAGES CXX)\n"
                                      "find_package(Qt6 REQUIRED COMPONENTS Quick)\nqt_standard_project_setup()\n"
                                      "qt_add_executable(appaio_qt_ready main.cpp)\n"
                                      "qt_add_qml_module(appaio_qt_ready URI aio_qt_ready VERSION 1.0 QML_FILES Main.qml)\n"
                                      "target_link_libraries(appaio_qt_ready PRIVATE Qt6::Quick)\n");
    writeFile(root / "main.cpp", "#include <QGuiApplication>\n#include <QQmlApplicationEngine>\nint main(int argc, char* argv[])\n"
                                 "{\n    QGuiApplication app(argc, argv);\n    QQmlApplicationEngine engine;\n"
                                 "    engine.loadFromModule(\"aio_qt_ready\", \"Main\");\n    return app.exec();\n}\n");
    writeFile(root / "Main.qml", "import QtQuick\nWindow {\n    width: 320\n    height: 200\n    visible: true\n"
                                 "    title: qsTr(\"A title\")\n}\n");
}

// An environment variable of the test process, or an empty string.
std::string environmentText(const char* name)
{
    char* value = nullptr;
    std::size_t length = 0;
    std::string result;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) result = value;
    std::free(value);
    return result;
}

void testARealQtQuickProjectWithTheAppsOwnPresetIsReadyAndMinGwIsRefused()
{
    const std::string msvcPrefix = environmentText("AIO_TEST_QT_PREFIX");
    if (msvcPrefix.empty()) {
        std::cout << "  (set AIO_TEST_QT_PREFIX to an MSVC 64-bit Qt kit to run the real Qt project check)" << std::endl;
        return;
    }
    for (const wchar_t* name : {L"VCINSTALLDIR", L"LIB", L"INCLUDE", L"LIBPATH"}) _wputenv_s(name, L"");
    VerificationToolchain::forgetCachedResult();
    SystemManifestVerificationCommandRunner runner;

    TestDirectory project("qt");
    writeQtQuickProject(project.path);
    CreateLocalVerificationPresetRequest request;
    request.projectRoot = project.path;
    request.generator = "Ninja";
    request.configuration = "Debug";
    request.qtPrefixPath = msvcPrefix;
    request.configurePresetName = "local-qt-debug";
    request.buildPresetName = "local-qt-debug";
    request.binaryDirRelative = "build/local-qt-debug";
    const auto created = VerificationEnvironmentPresetSetupService::createLocalPreset(request);
    require(created.success(), "The application's own preset generator writes the preset: "
        + (created.success() ? std::string() : created.error()->message));

    const auto result = ProjectSetupReadiness::check(project.path, {}, runner, {});
    require(result.ready, "A real Qt Quick project is ready from an unprepared process: [" + result.failureCode + "] "
        + result.failureMessage);
    require(std::find(result.targets.begin(), result.targets.end(), "all_qmllint") != result.targets.end(),
        "The real target catalog contains all_qmllint (what the Run's mandatory QML lint selects)");
    require(result.binaryDirRelative == "build/local-qt-debug", "The build directory was resolved");
    std::cout << "  real Qt project: ready, " << result.targets.size() << " targets, " << result.toolchainIdentity << std::endl;

    if (const std::string mingwPrefix = environmentText("AIO_TEST_QT_MINGW_PREFIX"); !mingwPrefix.empty()) {
        TestDirectory mingw("qtmingw");
        writeQtQuickProject(mingw.path);
        request.projectRoot = mingw.path;
        request.qtPrefixPath = mingwPrefix;
        require(VerificationEnvironmentPresetSetupService::createLocalPreset(request).success(), "MinGW preset written");
        const auto refused = ProjectSetupReadiness::check(mingw.path, {}, runner, {});
        require(!refused.ready && refused.failureCode == kProjectSetupToolchainUnsupported,
            "A MinGW Qt kit is refused with the limitation: " + refused.failureCode);
    }
}

#endif

} // namespace

int main()
{
    try {
        testASingleUsablePairIsConfiguredAndReportsItsTargets();
        testAFailedConfigureIsNotReadyAndShowsTheLinkerLine();
        testDisplayableTailKeepsTheEndAndNeverBreaksACharacter();
        testNoToolchainStopsBeforeAnyConfigureAndNamesTheReason();
        testSeveralPairsAreNeverGuessed();
        testOnlyTheCompatiblePairCountsForTheAutomaticChoice();
        testAMinGwOrArmSetupIsRefusedWithTheLimitationBeforeAnyConfigure();
        testUnsupportedGeneratorsStopBeforeToolchainConfigureAndCachedTargetProbes();
        testGeneratorInheritanceUsesIncludesParentOrderAndChildOverrides();
        testOnlyNinjaPairsAreOfferedButASavedUnsupportedPairIsNeverReplaced();
        testAMissingGeneratorUsesCMakeWithoutChangingTheUserPreset();
        testAMissingBuildToolKeepsCMakeFailureDistinctFromAnUnsupportedGenerator();
        testGeneratorRuleReadsUserInheritanceButNeverIncludesOutsideTheProject();
        testUnresolvedGeneratorInheritanceNeverFallsBackToTheNextParent();
        testACompilerThatIsNotMsvcIsRefusedAfterConfigure();
        testUnconfirmedTargetsAreNotReady();
        testCancellationNeverLooksLikeReadiness();
        testMissingPresetsAreNamedWithoutRunningCmake();
        testToolchainRuleFollowsInheritsAndIncludesButStaysInsideTheProject();
        testCompilerRuleReadsTheCacheOfTheBuildTree();
#ifdef _WIN32
        testARealProjectBecomesReadyFromAnUnpreparedProcessAndABrokenOneSaysWhy();
        testARealQtQuickProjectWithTheAppsOwnPresetIsReadyAndMinGwIsRefused();
#endif
    } catch (const std::exception& exception) {
        std::cerr << "ProjectSetupReadiness tests failed: " << exception.what() << std::endl;
        return 1;
    }
    std::cout << "ProjectSetupReadiness tests passed" << std::endl;
    return 0;
}
