// The PARENT of the crash tests: for each of the five phases it starts the child, PROVES that the child reached the intended phase and was
// killed there (wait result, exit code, durable marker with the right counters, durable call counter), and only then opens the database:
// first with a raw read-only connection (what the dead process left), then with the real BackendRuntime as the new owner (which also
// proves that the operating system released the dead child's owner lock), then a second startup. A child that returned 0, died for
// another reason, did not reach its hook or left no marker turns the scenario red BEFORE any database analysis.
//
// This is a PROCESS-crash guarantee (WAL + synchronous=NORMAL): it is not a power-loss test and says nothing about billing or about
// whether a request was delivered. No network, no credential, no live database.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CrashCommon.hpp"
#include "CrashProcess.hpp"

#include "ai/application/conversations/CompactionIntentRecoveryReport.hpp"
#include "ai/backend/BackendRuntime.hpp"

#include <windows.h>

#include <sqlite3.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace crash {

namespace {

namespace fs = std::filesystem;
using ai::application::CompactionIntentRecoveryStatus;
using ai::backend::BackendRuntime;

constexpr unsigned long kChildTimeoutMs = 30'000;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

struct ReadOnly {
    sqlite3* handle {nullptr};
    explicit ReadOnly(const fs::path& file)
    {
        const int rc = sqlite3_open_v2(utf8(file).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
        require(rc == SQLITE_OK, "read-only open failed (" + std::to_string(rc) + ")");
        sqlite3_busy_timeout(handle, 5000);
    }
    ~ReadOnly() { sqlite3_close_v2(handle); }
    ReadOnly(const ReadOnly&) = delete;
    ReadOnly& operator=(const ReadOnly&) = delete;

    long long scalar(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare failed: " + sql);
        long long value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }
    std::string text(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare failed: " + sql);
        std::string value = "<none>";
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* raw = sqlite3_column_text(statement, 0);
            value = raw != nullptr ? reinterpret_cast<const char*>(raw) : "<null>";
        }
        sqlite3_finalize(statement);
        return value;
    }
    std::string dump(const std::string& table) const
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, ("SELECT * FROM " + table + " ORDER BY rowid;").c_str(), -1, &statement, nullptr) == SQLITE_OK,
            "dump prepare failed: " + table);
        std::string out = table + "\n";
        while (sqlite3_step(statement) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                const unsigned char* raw = sqlite3_column_text(statement, i);
                out += raw != nullptr ? reinterpret_cast<const char*>(raw) : "<NULL>";
                out += '\x1f';
            }
            out += '\n';
        }
        sqlite3_finalize(statement);
        return out;
    }
};

// Everything money- or summary-related: the final records, the ledger, the segments.
std::string moneySnapshot(const fs::path& file)
{
    ReadOnly db(file);
    return db.dump("project_memory_compaction_records") + db.dump("model_executions") + db.dump("project_memory_segments");
}

struct Scenario {
    const char* phase;
    int clientCalls;
    int responseConsumed;
    int hookFlag;
    bool finalCommitted;   // P4 only: the final transaction IS in the database
};

struct Workspace {
    fs::path root;
    fs::path database;
    fs::path marker;
    fs::path counter;
    explicit Workspace(const char* phase)
    {
        root = fs::temp_directory_path()
            / ("aio-b3iib-crash-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + phase);
        fs::create_directories(root / "state");
        database = root / "state" / "orchestrator.sqlite3";
        marker = root / "marker.txt";
        counter = root / "provider-calls.bin";
    }
    ~Workspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

long long counterBytes(const fs::path& counter)
{
    std::error_code error;
    const auto size = fs::file_size(counter, error);
    return error ? -1 : static_cast<long long>(size);
}

void runScenario(const Scenario& scenario, int index)
{
    const std::string name = scenario.phase;
    Workspace workspace(scenario.phase);
    const std::string nonce = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(index) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    // ---- 1. the child ----------------------------------------------------------------------------------------------------------------
    const std::wstring exe = currentExecutablePath();
    const ChildResult child = runBoundedChild(exe,
        {L"--child", std::wstring(name.begin(), name.end()), std::wstring(nonce.begin(), nonce.end()), workspace.database.wstring(),
            workspace.marker.wstring(), workspace.counter.wstring()},
        kChildTimeoutMs);
    // Before ANY database analysis: the child was started, was not timed out, and ended exactly where it was told to.
    require(child.started, name + ": the child did not start: " + child.failure);
    require(!child.timedOut, name + ": the child timed out and was terminated - the phase was NOT reached");
    require(child.exitCode == kExpectedDeath,
        name + ": the child did not die on purpose (exit code " + std::to_string(child.exitCode) + ", expected " + std::to_string(kExpectedDeath)
            + "); 78 = marker not saved, 79 = fixture failed, 80 = phase not reached, 0 = it simply finished");
    const auto marker = readMarker(workspace.marker.wstring());
    require(marker.has_value(), name + ": the child died but left no valid marker");
    require(marker->phase == name && marker->nonce == nonce, name + ": the marker names another phase or another run (" + marker->phase + ")");
    require(marker->clientCalls == scenario.clientCalls && marker->responseConsumed == scenario.responseConsumed && marker->hookFlag == scenario.hookFlag,
        name + ": the marker's counters are not those of this phase (clientCalls=" + std::to_string(marker->clientCalls) + " responseConsumed="
            + std::to_string(marker->responseConsumed) + " hookFlag=" + std::to_string(marker->hookFlag) + ")");
    const long long callsOnDisk = counterBytes(workspace.counter);
    require(callsOnDisk == scenario.clientCalls, name + ": the durable provider-call counter disagrees with the marker");
    std::cout << "  [" << name << "] child exit=" << child.exitCode << " marker: clientCalls=" << marker->clientCalls << " responseConsumed="
              << marker->responseConsumed << " hookFlag=" << marker->hookFlag << " durableCalls=" << callsOnDisk << '\n';

    // ---- 2. what the dead process left (raw, read-only) -----------------------------------------------------------------------------------
    std::string moneyBefore;
    long long openBefore = 0;
    {
        ReadOnly db(workspace.database);
        require(db.text("PRAGMA journal_mode;") == "wal", name + ": the database is in WAL mode");
        const long long intents = db.scalar("SELECT count(*) FROM project_memory_compaction_intents;");
        const long long records = db.scalar("SELECT count(*) FROM project_memory_compaction_records;");
        const long long sources = db.scalar("SELECT count(*) FROM project_memory_segments WHERE id IN ('s1','s2','s3');");
        const long long segments = db.scalar("SELECT count(*) FROM project_memory_segments;");
        const long long ledger = db.scalar("SELECT count(*) FROM model_executions;");
        require(ledger == 0, name + ": no ledger row exists");
        if (!scenario.finalCommitted) {
            require(intents == 1 && db.scalar("SELECT count(*) FROM project_memory_compaction_intents WHERE state='open';") == 1,
                name + ": exactly ONE committed open intent");
            require(records == 0 && sources == 3 && segments == 3, name + ": no final record, no result segment, the sources are in place");
        } else {
            require(intents == 0, name + ": the intent was closed by the final commit");
            require(records == 1 && db.scalar("SELECT count(*) FROM project_memory_compaction_records WHERE status='completed' AND input_tokens=40;") == 1,
                name + ": the Completed record with its usage is stored");
            require(sources == 0 && segments == 1, name + ": the sources were replaced by the result segment");
        }
        openBefore = intents;
        moneyBefore = db.dump("project_memory_compaction_records") + db.dump("model_executions") + db.dump("project_memory_segments");
    }

    // ---- 3. the new owner: this also proves the operating system released the dead child's lock -----------------------------------------
    long long markedAt = -1;
    {
        BackendRuntime runtime({workspace.database});
        require(runtime.storageAvailable(), name + ": the new owner could not open the database (is the dead child's lock still held? "
            + (runtime.storageError().has_value() ? runtime.storageError()->code : std::string("no error")) + ")");
        require(runtime.compactionIntentRecoveryReport().has_value(), name + ": the new owner has a recovery report");
        const auto& report = *runtime.compactionIntentRecoveryReport();
        require(report.status == CompactionIntentRecoveryStatus::Complete && report.conflicts == 0 && report.unconfirmed == 0
                    && !runtime.compactionIntentRecoveryError().has_value(),
            name + ": the recovery is Complete");
        require(!runtime.recoveryError().has_value(), name + ": no other startup channel is disturbed");
        ReadOnly db(workspace.database);
        if (!scenario.finalCommitted) {
            require(report.inspected == 1 && report.markedUnresolved == 1 && report.alreadyUnresolved == 0,
                name + ": the open intent was marked unresolved");
            require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents WHERE state='unresolved';") == 1
                        && db.scalar("SELECT count(*) FROM project_memory_compaction_intents;") == openBefore,
                name + ": TRUTH: unresolved, not deleted");
            markedAt = db.scalar("SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents;");
            require(markedAt > 0, name + ": markedAt is set");
        } else {
            require(report.inspected == 0 && report.markedUnresolved == 0 && report.closedConfirmed == 0,
                name + ": a finished operation leaves nothing to recover");
            require(db.scalar("SELECT count(*) FROM project_memory_compaction_intents;") == 0, name + ": still no intent");
        }
        require(moneySnapshot(workspace.database) == moneyBefore, name + ": records, ledger and segments are byte-for-byte unchanged by the recovery");
    }
    require(counterBytes(workspace.counter) == callsOnDisk, name + ": recovery made no provider call (the durable counter did not move)");

    // ---- 4. a second startup changes nothing ---------------------------------------------------------------------------------------------
    {
        BackendRuntime runtime({workspace.database});
        require(runtime.storageAvailable() && runtime.compactionIntentRecoveryReport().has_value(), name + ": the second owner started");
        const auto& report = *runtime.compactionIntentRecoveryReport();
        require(report.status == CompactionIntentRecoveryStatus::Complete && report.markedUnresolved == 0 && report.closedConfirmed == 0,
            name + ": the second startup changes nothing");
        ReadOnly db(workspace.database);
        if (!scenario.finalCommitted) {
            require(report.alreadyUnresolved == 1 && db.scalar("SELECT unresolved_marked_at_ms FROM project_memory_compaction_intents;") == markedAt,
                name + ": already unresolved, markedAt stable");
        }
        require(moneySnapshot(workspace.database) == moneyBefore, name + ": the money tables are still equal");
    }
    require(counterBytes(workspace.counter) == callsOnDisk, name + ": still no provider call");
}

} // namespace

int runParentScenarios()
{
    const Scenario scenarios[] = {
        {kP0, 0, 0, 0, false},
        {kP1, 1, 0, 0, false},
        {kP2, 1, 1, 0, false},
        {kP3, 1, 0, 1, false},
        {kP4, 1, 0, 1, true},
    };
    int failures = 0;
    int index = 0;
    for (const Scenario& scenario : scenarios) {
        try {
            runScenario(scenario, index++);
            std::cout << "CompactionIntentCrash [" << scenario.phase << "]: OK\n";
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "CompactionIntentCrash [" << scenario.phase << "] FAILED: " << exception.what() << '\n';
        }
    }
    if (failures == 0) std::cout << "CompactionIntentCrash tests: all five phases OK\n";
    return failures == 0 ? 0 : 1;
}

} // namespace crash
