#pragma once

// A bounded, managed child process for the crash tests (Win32 only): CreateProcessW without a shell or a window, handles not inherited,
// the child placed in a Job Object that kills it if the parent dies, a hard time limit after which exactly that child is terminated and
// waited for. Nothing is detached and nothing outlives the call.
#include <string>
#include <vector>

namespace crash {

struct ChildResult {
    bool started {false};
    bool timedOut {false};
    unsigned long exitCode {0};
    std::string failure;   // why it did not start, when it did not
};

// `arguments` are plain arguments (no program name); they are quoted for the Windows command line.
[[nodiscard]] ChildResult runBoundedChild(const std::wstring& executable, const std::vector<std::wstring>& arguments, unsigned long timeoutMs);

// The path of the running executable.
[[nodiscard]] std::wstring currentExecutablePath();

} // namespace crash
