#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ai::application {

// AG-18.4B-R1: the MSVC / Windows SDK environment that the verification's child processes (cmake -> Ninja -> cl / link) need.
//
// A Windows application that was started from the Start menu has no LIB / INCLUDE / PATH entries for the compiler, so a CMake
// preset that finds cl.exe on its own still fails to link ("LNK1104: cannot open file 'kernel32.lib'"). The environment is
// therefore prepared for the CHILD PROCESSES ONLY: Visual Studio's own vcvars64.bat is run once in a throw-away cmd.exe and the
// resulting variables are handed to CreateProcess as an explicit environment block. Nothing global is changed - not this
// process's environment, not the user's or the machine's variables - and the application does not have to be started from a
// developer prompt. A process that already has a complete environment (it was started from one) is used as it is.
struct VerificationToolchainEnvironment {
    // A complete environment exists: either `environmentBlock` (prepared here) or the process's own (`inherited`).
    bool prepared {false};
    bool inherited {false};
    // UTF-16 "NAME=VALUE\0...\0\0", sorted by name; empty when `inherited` or not `prepared`.
    std::wstring environmentBlock;
    // Machine-local identity of the toolchain ("msvc=14.51.36231;sdk=10.0.26100.0;vs=3f9a..."); empty when not prepared.
    // Readiness evidence is only valid for the toolchain it was gathered with.
    std::string identity;
    // toolchain.* when not prepared, and an instruction that ends with what to do about it.
    std::string errorCode;
    std::string message;
};

inline constexpr const char* kToolchainUnsupportedPlatform = "toolchain.unsupported_platform";
inline constexpr const char* kToolchainVisualStudioNotFound = "toolchain.visual_studio_not_found";
inline constexpr const char* kToolchainVcvarsMissing = "toolchain.vcvars_missing";
inline constexpr const char* kToolchainVcvarsFailed = "toolchain.vcvars_failed";
inline constexpr const char* kToolchainWindowsSdkMissing = "toolchain.windows_sdk_missing";

class VerificationToolchain final {
public:
    VerificationToolchain() = delete;

    // The environment for the verification's child processes. A success is cached for the life of the process; a failure is
    // remembered only briefly, so installing the missing component and pressing "check again" works without a restart while a
    // burst of commands does not repeat a failing, slow preparation.
    [[nodiscard]] static VerificationToolchainEnvironment current();

    // Forgets the cached result (tests, and an explicit "check again").
    static void forgetCachedResult();

    // True when current() has already prepared (or failed to prepare) the environment in this process - lets a test prove
    // that a command which needs no toolchain did not wait for one.
    [[nodiscard]] static bool hasCachedResult();

    // ---- pure helpers (testable without Visual Studio) ----

    // "NAME=VALUE" lines of the output of `set` (CRLF or LF). A line without '=' or with an empty name, and the
    // drive-current-directory entries that start with '=', are dropped.
    [[nodiscard]] static std::vector<std::pair<std::wstring, std::wstring>> parseSetOutput(const std::wstring& output);

    // True when some entry of the ';'-separated list names a directory that contains `fileName`.
    [[nodiscard]] static bool libraryPathContains(const std::wstring& libraryList, const std::wstring& fileName);

    // The sorted, case-insensitively de-duplicated "A=B\0C=D\0\0" block of CreateProcess.
    [[nodiscard]] static std::wstring composeEnvironmentBlock(
        const std::vector<std::pair<std::wstring, std::wstring>>& variables);

    // Turns the variables of a prepared environment into a result: checks that a Windows SDK import library is reachable
    // (the exact failure that was observed), and derives the identity. `installationPath` is only hashed into the identity.
    [[nodiscard]] static VerificationToolchainEnvironment fromPreparedVariables(
        const std::vector<std::pair<std::wstring, std::wstring>>& variables, const std::wstring& installationPath);
};

} // namespace ai::application
