#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CrashProcess.hpp"

#include <windows.h>

#include <utility>

namespace crash {

namespace {

// Handle owner (RAII): the process, thread and job handles never leak, also when a check throws.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    void reset()
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = nullptr;
    }
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] bool valid() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE value_ {nullptr};
};

// The CommandLineToArgvW rules: a value with a space, a tab or a quote (or an empty one) is wrapped in quotes; backslashes before a
// quote are doubled and the quote is escaped; trailing backslashes are doubled before the closing quote.
std::wstring quoted(const std::wstring& value)
{
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(ch);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

} // namespace

std::wstring currentExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

ChildResult runBoundedChild(const std::wstring& executable, const std::vector<std::wstring>& arguments, unsigned long timeoutMs)
{
    ChildResult result;

    // A Job Object whose handle dies with this process: if the parent crashes, the child is killed with it.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        result.failure = "CreateJobObjectW failed";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        result.failure = "SetInformationJobObject failed";
        return result;
    }

    std::wstring commandLine = quoted(executable);
    for (const auto& argument : arguments) commandLine += L" " + quoted(argument);

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information {};
    // CREATE_SUSPENDED: the child is in the job before it executes a single instruction. bInheritHandles = FALSE: it inherits nothing of
    // ours (no lock, no database handle).
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &information)) {
        result.failure = "CreateProcessW failed (" + std::to_string(GetLastError()) + ")";
        return result;
    }
    Handle process(information.hProcess);
    Handle thread(information.hThread);
    result.started = true;

    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 10'000);
        result.started = false;
        result.failure = "AssignProcessToJobObject failed (" + std::to_string(GetLastError()) + ")";
        return result;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 10'000);
        result.started = false;
        result.failure = "ResumeThread failed";
        return result;
    }

    const DWORD waited = WaitForSingleObject(process.get(), timeoutMs);
    if (waited != WAIT_OBJECT_0) {
        // Exactly the child this call created, and only it, is ended and waited for.
        result.timedOut = true;
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 30'000);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    result.exitCode = code;
    return result;
}

} // namespace crash
