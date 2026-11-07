#pragma once

// AIOrchestrator_CompactionIntentCrash_tests (B3-II-B, Windows only): vocabulary shared by the parent and the child of the crash tests.
//
// The same executable is both: the parent runs the scenarios and starts itself as a CHILD (`--child <phase> <nonce> <db> <marker>
// <counter>`). The child builds the real compaction service over real SQLite with a fake provider, reaches ONE test-only point and ends
// its own process abruptly (TerminateProcess, no destructors, no RAII). The parent trusts nothing about the database until it has
// proved, in this order: the child was waited for and exited with kExpectedDeath, and the durable marker names exactly the intended
// phase with the counters that phase must show. Only then is the database opened.
#include <optional>
#include <string>

namespace crash {

// Exit codes of the child. kExpectedDeath is the ONLY one that means "the phase was reached and the process was killed on purpose".
inline constexpr unsigned kExpectedDeath = 77;
inline constexpr unsigned kMarkerFailed = 78;     // the marker could not be written and flushed: the phase is NOT proven
inline constexpr unsigned kSetupFailed = 79;      // the child could not build its fixture (lock, database, WAL/NORMAL check)
inline constexpr unsigned kPhaseNotReached = 80;  // the compaction returned: no hook ended the process
inline constexpr unsigned kBadArguments = 81;

// The five phases of §15c/§18. The names are the marker's `phase=` values.
inline constexpr const char* kP0 = "intent_committed_before_dispatch";
inline constexpr const char* kP1 = "inside_fake_send";
inline constexpr const char* kP2 = "response_ready_before_record_write";
inline constexpr const char* kP3 = "before_final_commit";
inline constexpr const char* kP4 = "after_final_commit";

// What the child wrote before it died. `clientCalls` is the child's own count of provider calls, which the fake also keeps durably in
// the counter file (one byte per call).
struct MarkerData {
    std::string phase;
    std::string nonce;
    int clientCalls {0};
    int responseConsumed {0};
    int hookFlag {0};
};

// Child side. Opens (CREATE_NEW) the marker file and keeps the handle for the death; false when it cannot.
[[nodiscard]] bool prepareMarker(const std::wstring& path, const std::string& nonce);
// Writes `phase nonce clientCalls responseConsumed hookFlag` with WriteFile + FlushFileBuffers and ends the process with kExpectedDeath;
// if the marker cannot be saved it ends with kMarkerFailed instead. Safe inside a SQLite hook: it runs no SQL and allocates nothing.
[[noreturn]] void dieAtPhase(const char* phase, int clientCalls, int responseConsumed, int hookFlag);

// Parent side. nullopt when the file is missing or malformed.
[[nodiscard]] std::optional<MarkerData> readMarker(const std::wstring& path);

// The child entry point (argv after `--child`). Returns an exit code only if the process was NOT killed on purpose.
int runChild(int argc, wchar_t** argv);

// The parent's scenarios. Returns 0 when all pass.
int runParentScenarios();

} // namespace crash
