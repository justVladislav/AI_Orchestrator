#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace ai::ui::test_support {

inline void configureHeadlessFailureMode()
{
#if defined(_WIN32)
    // A failed assertion or unhandled exception under CTest must terminate
    // the process and return an exit code. Windows Error Reporting/CRT dialog
    // boxes otherwise turn an ordinary failure into an apparently hung test.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
        | SEM_NOOPENFILEERRORBOX);
#endif
#if defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

inline void pumpOneEventCycle()
{
    QEventLoop loop;
    QTimer::singleShot(1, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::AllEvents);
}

template<typename Predicate, typename Snapshot>
bool waitForCondition(
    const char* label,
    Predicate&& predicate,
    Snapshot&& snapshot,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::invoke(predicate)) return true;
        pumpOneEventCycle();
    }
    const bool satisfied = std::invoke(predicate);
    if (!satisfied) {
        const std::string current = std::invoke(snapshot);
        std::fprintf(stderr, "[timeout] %s; current=%s\n", label, current.c_str());
        std::fflush(stderr);
    }
    return satisfied;
}

template<typename Predicate>
bool waitForCondition(
    const char* label,
    Predicate&& predicate,
    std::chrono::milliseconds timeout)
{
    return waitForCondition(label, std::forward<Predicate>(predicate),
        [] { return std::string("unavailable"); }, timeout);
}

} // namespace ai::ui::test_support
