#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/ManifestProposalSetVerifier.hpp"
#include "ai/application/verification/VerificationToolchain.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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
            / ("aio-tc-" + tag + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)
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

using Variables = std::vector<std::pair<std::wstring, std::wstring>>;

void testSetOutputIsParsedLikeCmdWritesIt()
{
    const std::wstring output =
        L"=C:=C:\\work\r\n"            // a drive-current-directory entry: never a variable
        L"Path=C:\\a;C:\\b\r\n"
        L"EMPTYVALUE=\r\n"             // legal: an empty value
        L"NOEQUALS\r\n"                // not a variable
        L"=\r\n"
        L"WITH=EQUALS=INSIDE\n"        // only the first '=' separates; a bare LF also ends a line
        L"LIB=D:\\sdk\\um\\x64;D:\\msvc\\lib";
    const auto variables = VerificationToolchain::parseSetOutput(output);
    require(variables.size() == 4, "Four lines are variables, got " + std::to_string(variables.size()));
    require(variables[0].first == L"Path" && variables[0].second == L"C:\\a;C:\\b", "Path is kept whole");
    require(variables[1].first == L"EMPTYVALUE" && variables[1].second.empty(), "An empty value is kept");
    require(variables[2].first == L"WITH" && variables[2].second == L"EQUALS=INSIDE", "Only the first '=' splits");
    require(variables[3].first == L"LIB" && variables[3].second == L"D:\\sdk\\um\\x64;D:\\msvc\\lib",
        "The last line needs no line break");
}

void testEnvironmentBlockIsSortedDeduplicatedAndDoublyTerminated()
{
    const Variables variables {{L"zeta", L"1"}, {L"Alpha", L"2"}, {L"PATH", L"x"}, {L"Path", L"y"}};
    const auto block = VerificationToolchain::composeEnvironmentBlock(variables);
    // "Path" replaces "PATH" (names are case-insensitive on Windows; the later entry wins).
    const std::wstring expected = std::wstring(L"Alpha=2") + L'\0' + L"Path=y" + L'\0' + L"zeta=1" + L'\0' + L'\0';
    require(block == expected, "The block is sorted by name, de-duplicated case-insensitively and ends with two NULs");
}

void testLibraryPathSearchFindsOnlyARealFile()
{
    TestDirectory directory("lib");
    const fs::path present = directory.path / "present";
    const fs::path empty = directory.path / "empty";
    fs::create_directories(present);
    fs::create_directories(empty);
    std::ofstream(present / "kernel32.lib") << "x";
    const std::wstring both = empty.wstring() + L";" + (directory.path / "missing").wstring() + L"; " + present.wstring();
    require(VerificationToolchain::libraryPathContains(both, L"kernel32.lib"), "A later entry with the file counts");
    require(!VerificationToolchain::libraryPathContains(empty.wstring() + L";", L"kernel32.lib"),
        "A directory without the file does not count");
    require(!VerificationToolchain::libraryPathContains(L"", L"kernel32.lib"), "An empty list has nothing");
    fs::create_directories(present / "kernel32.lib.d");
    require(!VerificationToolchain::libraryPathContains((present / "kernel32.lib.d").wstring(), L"kernel32.lib"),
        "Only a regular file counts");
}

void testAPreparedEnvironmentWithoutTheSdkIsNamedNotBlamedOnTheCompiler()
{
    TestDirectory directory("sdk");
    const fs::path lib = directory.path / "lib";
    fs::create_directories(lib);

    // The failure that was observed on a fresh Windows account: no import library is reachable.
    const Variables withoutSdk {{L"LIB", lib.wstring()}, {L"VCToolsVersion", L"14.51.36231"}};
    const auto missing = VerificationToolchain::fromPreparedVariables(withoutSdk, L"C:\\VS");
    require(!missing.prepared && missing.errorCode == kToolchainWindowsSdkMissing,
        "A LIB without kernel32.lib is toolchain.windows_sdk_missing");
    require(missing.message.find("Install") != std::string::npos, "The message ends with what to do");
    require(missing.identity.empty() && missing.environmentBlock.empty(), "Nothing is offered for a failed preparation");

    const Variables noLib {{L"VCToolsVersion", L"14.51.36231"}};
    require(VerificationToolchain::fromPreparedVariables(noLib, L"C:\\VS").errorCode == kToolchainWindowsSdkMissing,
        "No LIB at all is the same refusal");

    std::ofstream(lib / "kernel32.lib") << "x";
    const Variables complete {{L"LIB", lib.wstring()}, {L"VCToolsVersion", L"14.51.36231"},
        {L"WindowsSDKVersion", L"10.0.26100.0\\"}};
    const auto ready = VerificationToolchain::fromPreparedVariables(complete, L"C:\\VS");
    require(ready.prepared && !ready.inherited && ready.errorCode.empty(), "A complete environment is prepared");
    require(!ready.environmentBlock.empty() && ready.environmentBlock.back() == L'\0', "The block is handed over");
    require(ready.identity.rfind("msvc=14.51.36231;sdk=10.0.26100.0;vs=", 0) == 0,
        "The identity names the compiler, the SDK (without its trailing backslash) and the installation: " + ready.identity);

    const auto otherInstallation = VerificationToolchain::fromPreparedVariables(complete, L"D:\\OtherVS");
    require(otherInstallation.identity != ready.identity, "Another Visual Studio installation is another identity");
    const Variables newerSdk {{L"LIB", lib.wstring()}, {L"VCToolsVersion", L"14.51.36231"},
        {L"WindowsSDKVersion", L"10.0.26200.0\\"}};
    require(VerificationToolchain::fromPreparedVariables(newerSdk, L"C:\\VS").identity != ready.identity,
        "A changed SDK is another identity (readiness evidence must not survive it)");
}

#ifdef _WIN32

void removeVariable(const wchar_t* name)
{
    _wputenv_s(name, L"");
}

std::string tail(const std::string& text)
{
    return text.size() <= 600 ? text : text.substr(text.size() - 600);
}

ManifestVerificationCommandOutcome runCmake(
    const std::vector<std::string>& arguments, const fs::path& workingDirectory)
{
    SystemManifestVerificationCommandRunner runner;
    ManifestVerificationCommandRequest request;
    request.commandIdentity = "toolchain.test";
    request.executable = "cmake";
    request.arguments = arguments;
    request.workingDirectory = workingDirectory;
    request.timeout = std::chrono::minutes(4);
    request.maxOutputBytes = 256 * 1024;
    return runner.run(request, {});
}

// The exact failure of a fresh Windows account: the application's own environment has no LIB / INCLUDE, so a configure that finds
// cl.exe still cannot link ("LNK1104: cannot open file 'kernel32.lib'"). Run from a process stripped of those variables, the
// runner's child must configure AND build a real executable.
void testAChildProcessBuildsFromAnApplicationWithoutAVisualStudioEnvironment()
{
    removeVariable(L"VCINSTALLDIR");
    removeVariable(L"LIB");
    removeVariable(L"INCLUDE");
    removeVariable(L"LIBPATH");
    VerificationToolchain::forgetCachedResult();

    // A command that needs no toolchain must not wait for one.
    const TestDirectory directory("build");
    const auto version = runCmake({"--version"}, directory.path);
    require(version.started && version.exitCode == 0, "cmake --version runs");
    require(!VerificationToolchain::hasCachedResult(), "--version did not prepare an MSVC environment");

    const auto environment = runCmake({"-E", "environment"}, directory.path);
    require(environment.started && environment.exitCode == 0, "cmake -E environment runs");
    require(environment.boundedOutput.find("\nLIB=") == std::string::npos && environment.boundedOutput.rfind("LIB=", 0) != 0,
        "Precondition: a command without a toolchain really has no LIB (the failing state of the fresh account)");

    const auto toolchain = VerificationToolchain::current();
    if (!toolchain.prepared) {
        // No Visual Studio here: the reason is a typed, actionable refusal, and there is nothing further to build with.
        require(!toolchain.errorCode.empty() && !toolchain.message.empty(), "A missing toolchain is named");
        std::cout << "  (no MSVC on this computer: " << toolchain.errorCode << " - build step skipped)" << std::endl;
        return;
    }
    require(!toolchain.inherited, "The process was stripped of its Visual Studio variables, so the environment is prepared");
    require(!toolchain.environmentBlock.empty() && !toolchain.identity.empty(), "A prepared environment has a block and an identity");
    require(VerificationToolchain::hasCachedResult(), "The prepared environment is cached");

    const fs::path source = directory.path / "src";
    fs::create_directories(source);
    std::ofstream(source / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.20)\nproject(aio_tc CXX)\n"
                                                "add_executable(aio_tc main.cpp)\n";
    std::ofstream(source / "main.cpp") << "int main() { return 0; }\n";
    const fs::path build = directory.path / "b";
    const auto configured = runCmake({"-S", source.string(), "-B", build.string(), "-G", "Ninja"}, source);
    require(configured.started && configured.exitCode == 0,
        "configure from an application without a Visual Studio environment: " + tail(configured.boundedOutput));
    const auto built = runCmake({"--build", build.string()}, source);
    require(built.started && built.exitCode == 0,
        "the link step finds the Windows SDK libraries: " + tail(built.boundedOutput));
    require(fs::exists(build / "aio_tc.exe"), "the executable exists");
}

#endif

} // namespace

int main()
{
    try {
        testSetOutputIsParsedLikeCmdWritesIt();
        testEnvironmentBlockIsSortedDeduplicatedAndDoublyTerminated();
        testLibraryPathSearchFindsOnlyARealFile();
        testAPreparedEnvironmentWithoutTheSdkIsNamedNotBlamedOnTheCompiler();
#ifdef _WIN32
        testAChildProcessBuildsFromAnApplicationWithoutAVisualStudioEnvironment();
#endif
    } catch (const std::exception& exception) {
        std::cerr << "VerificationToolchain tests failed: " << exception.what() << std::endl;
        return 1;
    }
    std::cout << "VerificationToolchain tests passed" << std::endl;
    return 0;
}
