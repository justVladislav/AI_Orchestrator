// The durable marker of the crash tests: written with the Win32 file API only (never a buffered stream), flushed to the file system
// before the process is killed. The parent reads it back; a phase is "reached" only if this file says so.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CrashCommon.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace crash {

namespace {

HANDLE g_marker = INVALID_HANDLE_VALUE;
char g_nonce[64] = {0};

[[noreturn]] void dieWith(unsigned code)
{
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) {}   // TerminateProcess does not return; this keeps the compiler honest
}

} // namespace

bool prepareMarker(const std::wstring& path, const std::string& nonce)
{
    if (nonce.size() >= sizeof(g_nonce)) return false;
    std::memcpy(g_nonce, nonce.c_str(), nonce.size() + 1);
    g_marker = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_marker != INVALID_HANDLE_VALUE;
}

void dieAtPhase(const char* phase, int clientCalls, int responseConsumed, int hookFlag)
{
    if (g_marker == INVALID_HANDLE_VALUE) dieWith(kMarkerFailed);
    char line[256];
    const int length = std::snprintf(line, sizeof(line), "phase=%s nonce=%s clientCalls=%d responseConsumed=%d hookFlag=%d\n", phase, g_nonce,
        clientCalls, responseConsumed, hookFlag);
    if (length <= 0 || length >= static_cast<int>(sizeof(line))) dieWith(kMarkerFailed);
    DWORD written = 0;
    if (!WriteFile(g_marker, line, static_cast<DWORD>(length), &written, nullptr) || written != static_cast<DWORD>(length)) dieWith(kMarkerFailed);
    if (!FlushFileBuffers(g_marker)) dieWith(kMarkerFailed);
    dieWith(kExpectedDeath);
}

std::optional<MarkerData> readMarker(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    char buffer[512] = {0};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return std::nullopt;

    MarkerData data;
    std::istringstream stream{std::string(buffer, read)};
    std::string token;
    int fields = 0;
    while (stream >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) return std::nullopt;
        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);
        try {
            if (key == "phase") { data.phase = value; ++fields; }
            else if (key == "nonce") { data.nonce = value; ++fields; }
            else if (key == "clientCalls") { data.clientCalls = std::stoi(value); ++fields; }
            else if (key == "responseConsumed") { data.responseConsumed = std::stoi(value); ++fields; }
            else if (key == "hookFlag") { data.hookFlag = std::stoi(value); ++fields; }
            else return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }
    if (fields != 5) return std::nullopt;
    return data;
}

} // namespace crash
