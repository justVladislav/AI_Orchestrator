#pragma once

#include "ai/common/Result.hpp"

#include <filesystem>
#include <memory>

namespace ai::infrastructure {

// The one live owner of a database file (PC-4C, review 2).
//
// Everything the application keeps to guard its own data lives in memory of ONE process: the path claims of the Runs, the import lock of
// the manual-handoff service, the single Outcome Brief reconciler whose verdicts are validated against each other. All of them assume that
// exactly one process works on a database file at a time - and nothing used to enforce it. This lock does: a runtime takes it before
// SQLite opens the file and releases it after the connection is closed, and anybody else who asks for the same file in the meantime - a
// second application instance, or a second runtime of this process - is refused instead of being let in to race the first.
//
// It is an operating-system lock on a companion file, `<database>.owner` (a Windows file opened with no sharing, `flock` elsewhere), so an
// owner that crashes never leaves a stale lock behind: the system lets go of it together with the process. The companion file is never
// deleted (a deleted and re-created file would let two owners hold two different files of the same name) and holds nothing. Tools that
// only READ the database (the read-only diagnosis procedure) do not take the lock and are not affected by it.
class DatabaseOwnerLock {
public:
    // Error codes: `persistence.owned_by_another_instance` when somebody else holds the lock, `persistence.empty_path` for an empty path,
    // `persistence.directory_failed` / `persistence.owner_lock_failed` when the lock file cannot be created or locked. The messages carry
    // no path. The in-memory database (":memory:") has no other process to share it with and needs no lock: the result is an empty lock.
    [[nodiscard]] static common::Result<std::unique_ptr<DatabaseOwnerLock>> acquire(const std::filesystem::path& databasePath);

    ~DatabaseOwnerLock();
    DatabaseOwnerLock(const DatabaseOwnerLock&) = delete;
    DatabaseOwnerLock& operator=(const DatabaseOwnerLock&) = delete;

    // The companion file this lock holds; empty for the in-memory database.
    [[nodiscard]] const std::filesystem::path& lockFilePath() const noexcept;

private:
    struct Impl;
    explicit DatabaseOwnerLock(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ai::infrastructure
