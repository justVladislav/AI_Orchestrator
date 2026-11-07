// Entry point of AIOrchestrator_CompactionIntentCrash_tests: `--child <phase> <nonce> <db> <marker> <counter>` is the child that kills
// itself; anything else is the parent that runs the five scenarios. The same executable is both, so there is never a second binary to
// get out of date.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CrashCommon.hpp"

#include <windows.h>

#include <cwchar>
#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2 && std::wcscmp(argv[1], L"--child") == 0) {
        return crash::runChild(argc - 2, argv + 2);
    }
    // Keep a failed child's exit code from raising a Debug CRT dialog in the parent: errors are reported on stderr and by the exit code.
    return crash::runParentScenarios();
}
