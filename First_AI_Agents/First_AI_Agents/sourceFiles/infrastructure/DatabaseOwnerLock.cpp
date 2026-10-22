#include "ai/infrastructure/DatabaseOwnerLock.hpp"

#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ai::infrastructure {

namespace {

common::Error lockError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

struct DatabaseOwnerLock::Impl {
    std::filesystem::path lockFile;
#ifdef _WIN32
    HANDLE handle {INVALID_HANDLE_VALUE};
#else
    int descriptor {-1};
#endif

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl()
    {
#ifdef _WIN32
        // Closing the handle is what releases the lock; the file itself stays.
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (descriptor >= 0) ::close(descriptor);   // closing the descriptor drops its flock
#endif
    }
};

DatabaseOwnerLock::DatabaseOwnerLock(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

DatabaseOwnerLock::~DatabaseOwnerLock() = default;

const std::filesystem::path& DatabaseOwnerLock::lockFilePath() const noexcept
{
    return impl_->lockFile;
}

common::Result<std::unique_ptr<DatabaseOwnerLock>> DatabaseOwnerLock::acquire(const std::filesystem::path& databasePath)
{
    using Result = common::Result<std::unique_ptr<DatabaseOwnerLock>>;
    if (databasePath.empty()) {
        return Result::failure(lockError("persistence.empty_path", "SQLite database path cannot be empty"));
    }
    auto impl = std::make_unique<Impl>();
    if (databasePath == std::filesystem::path(":memory:")) {
        return Result::ok(std::unique_ptr<DatabaseOwnerLock>(new DatabaseOwnerLock(std::move(impl))));
    }

    // The directory is the database's own (SQLite would create it a moment later): the companion file needs it first.
    std::error_code filesystemError;
    const auto parent = databasePath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) {
            return Result::failure(lockError("persistence.directory_failed",
                "Could not create SQLite database directory: " + filesystemError.message()));
        }
    }
    impl->lockFile = databasePath;
    impl->lockFile += ".owner";

    const auto inUse = [] {
        return Result::failure(lockError("persistence.owned_by_another_instance",
            "The database is already in use by another running instance of the application. Close it and start again."));
    };
#ifdef _WIN32
    // No sharing at all: while this handle is open nobody else can open the file, whatever the process.
    impl->handle = CreateFileW(impl->lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl->handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) return inUse();
        return Result::failure(lockError("persistence.owner_lock_failed",
            "Could not take the ownership lock of the database (system error " + std::to_string(code) + ")"));
    }
#else
    impl->descriptor = ::open(impl->lockFile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl->descriptor < 0) {
        return Result::failure(lockError("persistence.owner_lock_failed",
            "Could not take the ownership lock of the database (system error " + std::to_string(errno) + ")"));
    }
    if (::flock(impl->descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        if (code == EWOULDBLOCK || code == EAGAIN) return inUse();
        return Result::failure(lockError("persistence.owner_lock_failed",
            "Could not take the ownership lock of the database (system error " + std::to_string(code) + ")"));
    }
#endif
    return Result::ok(std::unique_ptr<DatabaseOwnerLock>(new DatabaseOwnerLock(std::move(impl))));
}

} // namespace ai::infrastructure
