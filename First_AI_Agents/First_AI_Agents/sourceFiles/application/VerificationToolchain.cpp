#include "ai/application/VerificationToolchain.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <chrono>
#endif

namespace ai::application {

namespace fs = std::filesystem;

namespace {

std::wstring lowered(std::wstring text)
{
    for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

std::wstring trimmed(std::wstring text)
{
    const auto isSpace = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
    while (!text.empty() && isSpace(text.back())) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && isSpace(text[start])) ++start;
    return text.substr(start);
}

const std::wstring* findVariable(
    const std::vector<std::pair<std::wstring, std::wstring>>& variables, const std::wstring& lowerName)
{
    for (const auto& [name, value] : variables) {
        if (lowered(name) == lowerName) return &value;
    }
    return nullptr;
}

std::wstring stripTrailingSeparators(std::wstring text)
{
    while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return text;
}

std::string utf8(const std::wstring& text)
{
    return detail::genericUtf8(fs::path(text));
}

VerificationToolchainEnvironment failure(const char* code, std::string message)
{
    VerificationToolchainEnvironment result;
    result.errorCode = code;
    result.message = std::move(message);
    return result;
}

constexpr const char* kInstallVisualStudio =
    "Install Visual Studio (or the Build Tools) with the \"Desktop development with C++\" workload - it includes the MSVC x64 "
    "tools and a Windows SDK - and check again.";

#ifdef _WIN32

std::wstring environmentVariable(const wchar_t* name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(written);
    return value;
}

struct CapturedProcess {
    bool started {false};
    bool timedOut {false};
    DWORD exitCode {0};
    std::string bytes;
};

// Runs a command line (no window, no console input) and collects everything it writes to stdout / stderr. Only used for two
// fixed, Backend-built command lines (vswhere and cmd.exe + the vcvars64.bat vswhere named) - never a model- or user-authored
// string.
CapturedProcess capture(std::wstring commandLine, std::chrono::milliseconds timeout)
{
    CapturedProcess result;
    SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0) || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        return result;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nullInput != INVALID_HANDLE_VALUE ? nullInput : nullptr;
    PROCESS_INFORMATION process {};
    commandLine.push_back(L'\0');
    const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (!created) {
        CloseHandle(readPipe);
        return result;
    }
    result.started = true;
    const auto startedAt = std::chrono::steady_clock::now();
    const auto drain = [&] {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            char buffer[8192];
            DWORD read = 0;
            if (!ReadFile(readPipe, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &read, nullptr)
                || read == 0) {
                break;
            }
            // vcvars64 + `set` is well under a megabyte; the cap only stops a runaway child.
            if (result.bytes.size() < 4 * 1024 * 1024) result.bytes.append(buffer, read);
        }
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(process.hProcess, 25) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() - startedAt > timeout) {
            result.timedOut = true;
            TerminateProcess(process.hProcess, 3);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    drain();
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return result;
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring utf16LeToWide(const std::string& bytes)
{
    std::wstring result(bytes.size() / 2, L'\0');
    if (!result.empty()) std::memcpy(result.data(), bytes.data(), result.size() * sizeof(wchar_t));
    if (!result.empty() && result.front() == 0xFEFF) result.erase(0, 1);
    return result;
}

bool processHasCompleteEnvironment()
{
    return !environmentVariable(L"VCINSTALLDIR").empty()
        && VerificationToolchain::libraryPathContains(environmentVariable(L"LIB"), L"kernel32.lib");
}

std::vector<std::pair<std::wstring, std::wstring>> processVariables()
{
    std::vector<std::pair<std::wstring, std::wstring>> variables;
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr) return variables;
    for (const wchar_t* cursor = block; *cursor != L'\0';) {
        const std::wstring entry(cursor);
        cursor += entry.size() + 1;
        const auto equals = entry.find(L'=', 1);
        if (equals == std::wstring::npos || entry.front() == L'=') continue;
        variables.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
    }
    FreeEnvironmentStringsW(block);
    return variables;
}

VerificationToolchainEnvironment prepareOnWindows()
{
    if (processHasCompleteEnvironment()) {
        auto inherited = VerificationToolchain::fromPreparedVariables(processVariables(), environmentVariable(L"VSINSTALLDIR"));
        inherited.inherited = true;
        inherited.environmentBlock.clear(); // the child simply inherits this process's own environment
        return inherited;
    }

    std::wstring programFilesX86 = environmentVariable(L"ProgramFiles(x86)");
    if (programFilesX86.empty()) programFilesX86 = L"C:\\Program Files (x86)";
    const fs::path vswhere = fs::path(programFilesX86) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
    std::error_code error;
    if (!fs::is_regular_file(vswhere, error)) {
        return failure(kToolchainVisualStudioNotFound,
            std::string("Visual Studio was not found on this computer (no vswhere.exe). ") + kInstallVisualStudio);
    }
    const auto located = capture(L"\"" + vswhere.wstring()
            + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
              L"-property installationPath -utf8",
        std::chrono::seconds(30));
    const std::wstring installationPath = trimmed(utf8ToWide(located.bytes));
    if (!located.started || located.timedOut || located.exitCode != 0 || installationPath.empty()
        || installationPath.find(L'\n') != std::wstring::npos) {
        return failure(kToolchainVisualStudioNotFound,
            std::string("No Visual Studio installation with the MSVC x64 tools was found. ") + kInstallVisualStudio);
    }
    const fs::path vcvars = fs::path(installationPath) / L"VC" / L"Auxiliary" / L"Build" / L"vcvars64.bat";
    if (!fs::is_regular_file(vcvars, error)) {
        return failure(kToolchainVcvarsMissing,
            std::string("The Visual Studio installation at ") + utf8(installationPath)
                + " has no vcvars64.bat (the MSVC tools are not installed). " + kInstallVisualStudio);
    }
    // /u makes cmd's built-in `set` write UTF-16, so a user name or path with non-ANSI characters survives.
    const auto prepared = capture(L"cmd.exe /u /d /s /c \"\"" + vcvars.wstring() + L"\" >nul 2>&1 && set\"",
        std::chrono::seconds(90));
    const auto variables = VerificationToolchain::parseSetOutput(utf16LeToWide(prepared.bytes));
    if (!prepared.started || prepared.timedOut || variables.empty() || findVariable(variables, L"lib") == nullptr) {
        return failure(kToolchainVcvarsFailed,
            std::string("Visual Studio's vcvars64.bat did not produce a build environment (") + utf8(vcvars.wstring())
                + "). Repair the Visual Studio installation and check again.");
    }
    return VerificationToolchain::fromPreparedVariables(variables, installationPath);
}

#endif // _WIN32

std::mutex gCacheMutex;
std::optional<VerificationToolchainEnvironment> gCached;
#ifdef _WIN32
std::chrono::steady_clock::time_point gCachedAt;
#endif

} // namespace

std::vector<std::pair<std::wstring, std::wstring>> VerificationToolchain::parseSetOutput(const std::wstring& output)
{
    std::vector<std::pair<std::wstring, std::wstring>> variables;
    std::size_t start = 0;
    while (start <= output.size()) {
        auto end = output.find(L'\n', start);
        std::wstring line = output.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        const auto equals = line.find(L'=');
        if (equals != std::wstring::npos && equals > 0 && line.front() != L'=') {
            variables.emplace_back(line.substr(0, equals), line.substr(equals + 1));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return variables;
}

bool VerificationToolchain::libraryPathContains(const std::wstring& libraryList, const std::wstring& fileName)
{
    std::size_t start = 0;
    while (start <= libraryList.size()) {
        const auto end = libraryList.find(L';', start);
        const std::wstring entry = trimmed(libraryList.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!entry.empty()) {
            std::error_code error;
            if (fs::is_regular_file(fs::path(entry) / fileName, error)) return true;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

std::wstring VerificationToolchain::composeEnvironmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& variables)
{
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> byName; // lower-case name -> (name, value)
    for (const auto& [name, value] : variables) byName[lowered(name)] = {name, value};
    std::wstring block;
    for (const auto& [lowerName, entry] : byName) {
        block += entry.first + L"=" + entry.second;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

VerificationToolchainEnvironment VerificationToolchain::fromPreparedVariables(
    const std::vector<std::pair<std::wstring, std::wstring>>& variables, const std::wstring& installationPath)
{
    const std::wstring* lib = findVariable(variables, L"lib");
    if (lib == nullptr || !libraryPathContains(*lib, L"kernel32.lib")) {
        return failure(kToolchainWindowsSdkMissing,
            "The prepared MSVC environment cannot reach the Windows SDK libraries (kernel32.lib is not in its LIB). Install a "
            "Windows 10 or Windows 11 SDK with the Visual Studio Installer and check again.");
    }
    VerificationToolchainEnvironment result;
    result.prepared = true;
    result.environmentBlock = composeEnvironmentBlock(variables);
    const auto text = [&](const wchar_t* name) {
        const std::wstring* value = findVariable(variables, name);
        return value == nullptr ? std::string() : utf8(stripTrailingSeparators(*value));
    };
    result.identity = "msvc=" + text(L"vctoolsversion") + ";sdk=" + text(L"windowssdkversion")
        + ";vs=" + sha256Hex(utf8(installationPath)).substr(0, 12);
    return result;
}

VerificationToolchainEnvironment VerificationToolchain::current()
{
#ifdef _WIN32
    constexpr auto kFailureMemory = std::chrono::seconds(60);
    std::lock_guard lock(gCacheMutex);
    if (gCached.has_value()
        && (gCached->prepared || std::chrono::steady_clock::now() - gCachedAt < kFailureMemory)) {
        return *gCached;
    }
    gCached = prepareOnWindows();
    gCachedAt = std::chrono::steady_clock::now();
    return *gCached;
#else
    return failure(kToolchainUnsupportedPlatform, "Preparing an MSVC environment is only available on Windows.");
#endif
}

void VerificationToolchain::forgetCachedResult()
{
    std::lock_guard lock(gCacheMutex);
    gCached.reset();
}

bool VerificationToolchain::hasCachedResult()
{
    std::lock_guard lock(gCacheMutex);
    return gCached.has_value();
}

} // namespace ai::application
