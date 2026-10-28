#include "ai/application/planning/FilesystemFollowUpFileLoader.hpp"

#include "ai/application/proposals/ChangeProposalValidator.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/workspace/WorkspacePathGuard.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <limits.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace ai::application {
namespace {

namespace fs = std::filesystem;

// The existing proposal secret scanner over exactly the bytes that would be sent, plus the path. A scan that cannot complete (a
// path conversion, a regex limit) is not a clean scan.
bool containsSecret(const std::string& relativePath, const std::string& content)
{
    try {
        ProposedFileChange change {};
        change.relativePath = detail::pathFromUtf8(relativePath);
        change.proposedContent = content;
        ProposedChangeSet changeSet;
        changeSet.changes.push_back(std::move(change));
        return !ChangeProposalValidator::scanForSecrets(changeSet).empty();
    } catch (const std::exception&) {
        return true;
    }
}

std::string comparable(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    std::string out(reinterpret_cast<const char*>(text.data()), text.size());
#ifdef _WIN32
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
#endif
    return out;
}

FollowUpFileCandidate verdict(const std::string& relativePath, FollowUpFileSafety safety)
{
    FollowUpFileCandidate candidate;
    candidate.relativePath = relativePath;
    candidate.safety = safety;
    return candidate;
}

// What reading one file through ONE handle came to. Anything but Read carries no bytes.
enum class HandleRead { Read, Link, ReadFailed, TooLarge };

#ifdef _WIN32

// The handle's own path without the Win32 namespace prefix, as a path.
fs::path pathWithoutNamespacePrefix(const std::wstring& finalPath)
{
    constexpr std::wstring_view dosPrefix = L"\\\\?\\";
    constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
    if (finalPath.starts_with(uncPrefix)) return fs::path(L"\\\\" + finalPath.substr(uncPrefix.size()));
    if (finalPath.starts_with(dosPrefix)) return fs::path(finalPath.substr(dosPrefix.size()));
    return fs::path(finalPath);
}

struct HandleCloser {
    HANDLE handle;
    ~HandleCloser() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }
};

// ONE handle, opened without following a link and shared for reading ONLY: while it is open nobody can write, rename or delete the
// file (or rename a directory above it), and every question below is put to the handle, never to the path again.
HandleRead readThroughOneHandle(
    const fs::path& actual, const fs::path& expected, const FollowUpFileLoaderHooks& hooks, std::string& content)
{
    if (hooks.beforeOpen) hooks.beforeOpen(actual);
    const HANDLE handle = ::CreateFileW(actual.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return HandleRead::ReadFailed;
    const HandleCloser closer {handle};
    if (hooks.afterOpen) hooks.afterOpen(actual);

    BY_HANDLE_FILE_INFORMATION info {};
    if (!::GetFileInformationByHandle(handle, &info)) return HandleRead::ReadFailed;
    // A symbolic link or junction opened as itself - and any other reparse point, whose bytes this loader cannot vouch for.
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return HandleRead::Link;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return HandleRead::ReadFailed;
    if (::GetFileType(handle) != FILE_TYPE_DISK) return HandleRead::ReadFailed;

    // The path this handle REALLY is: a junction swapped in above the file makes it differ from the expected one.
    std::wstring finalPath(512, L'\0');
    DWORD length = ::GetFinalPathNameByHandleW(handle, finalPath.data(), static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= finalPath.size()) {
        finalPath.assign(static_cast<std::size_t>(length) + 1, L'\0');
        length = ::GetFinalPathNameByHandleW(handle, finalPath.data(), static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (length == 0 || length >= finalPath.size()) return HandleRead::ReadFailed;
    finalPath.resize(length);
    if (comparable(pathWithoutNamespacePrefix(finalPath)) != comparable(expected)) return HandleRead::Link;

    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > kFollowUpComposerMaximumCandidateBytes) return HandleRead::TooLarge;

    content.assign(static_cast<std::size_t>(size), '\0');
    std::size_t have = 0;
    while (have < content.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(content.size() - have, 1u << 20));
        if (!::ReadFile(handle, content.data() + have, want, &got, nullptr) || got == 0) return HandleRead::ReadFailed;
        have += got;
    }
    char extra = 0;
    DWORD more = 0;
    if (!::ReadFile(handle, &extra, 1, &more, nullptr) || more != 0) return HandleRead::ReadFailed;   // it grew while it was read
    return HandleRead::Read;
}

#else

struct FdCloser {
    int fd;
    ~FdCloser() { if (fd >= 0) ::close(fd); }
};

// The same contract on POSIX: ONE descriptor, opened with O_NOFOLLOW (a link as the last component is refused by the kernel), asked
// for its type, size and real path (a link swapped into a parent directory makes the real path differ from the expected one).
// Where the real path of a descriptor cannot be asked for, the file is not read.
HandleRead readThroughOneHandle(
    const fs::path& actual, const fs::path& expected, const FollowUpFileLoaderHooks& hooks, std::string& content)
{
    if (hooks.beforeOpen) hooks.beforeOpen(actual);
    const int fd = ::open(actual.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ELOOP ? HandleRead::Link : HandleRead::ReadFailed;
    const FdCloser closer {fd};
    if (hooks.afterOpen) hooks.afterOpen(actual);

    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) return HandleRead::ReadFailed;

    std::string real;
#if defined(__linux__)
    real.resize(PATH_MAX + 1);
    const ssize_t written = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), real.data(), real.size() - 1);
    if (written <= 0) return HandleRead::ReadFailed;
    real.resize(static_cast<std::size_t>(written));
#elif defined(__APPLE__)
    real.resize(PATH_MAX + 1);
    if (::fcntl(fd, F_GETPATH, real.data()) != 0) return HandleRead::ReadFailed;
    real.resize(std::strlen(real.c_str()));
#else
    return HandleRead::ReadFailed;
#endif
    if (comparable(fs::path(real)) != comparable(expected)) return HandleRead::Link;

    if (static_cast<std::uintmax_t>(info.st_size) > kFollowUpComposerMaximumCandidateBytes) return HandleRead::TooLarge;
    content.assign(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t have = 0;
    while (have < content.size()) {
        const ssize_t got = ::read(fd, content.data() + have, content.size() - have);
        if (got <= 0) return HandleRead::ReadFailed;
        have += static_cast<std::size_t>(got);
    }
    char extra = 0;
    if (::read(fd, &extra, 1) != 0) return HandleRead::ReadFailed;   // it grew while it was read
    return HandleRead::Read;
}

#endif

// One candidate, start to finish. Never throws.
FollowUpFileCandidate loadOne(
    const WorkspaceReadPolicy& policy, const fs::path& canonicalRoot, const std::string& relativePath,
    const FollowUpFileLoaderHooks& hooks)
{
    try {
        // 1. policy + OS path guard, on the path built with the right encoding (the string overload would misread UTF-8). The policy
        // converts names through the narrow system codepage; a name that codepage cannot represent makes that conversion THROW. Such a
        // name cannot be vetted, so it is excluded as unsafe - never guessed at and never read through a mangled path.
        fs::path actual;
        try {
            const auto guarded = WorkspacePathGuard::validatePath(detail::pathFromUtf8(relativePath), policy);
            if (!guarded.success()) {
                const std::string& code = guarded.error()->code;
                if (code == "workspace_path.reparse_point") return verdict(relativePath, FollowUpFileSafety::Symlink);
                if (code == "workspace_path.access_error") return verdict(relativePath, FollowUpFileSafety::ReadFailed);
                return verdict(relativePath, FollowUpFileSafety::UnsafePath);
            }
            actual = guarded.value();
        } catch (const std::exception&) {
            return verdict(relativePath, FollowUpFileSafety::UnsafePath);
        }

        // The path the file MUST be: the canonical root and the relative path, nothing resolved. A first, early refusal when the path
        // already resolves elsewhere (a link or junction somewhere in it) - not what the bytes are trusted on.
        const fs::path expected = (canonicalRoot / detail::pathFromUtf8(relativePath)).lexically_normal();
        if (comparable(actual) != comparable(expected)) return verdict(relativePath, FollowUpFileSafety::Symlink);

        // 2. - 5. ONE handle: opened without following a link, then type, real path, size and the complete bytes are all taken from
        // it. Nothing below asks the path again, so a swap after this point cannot change what is read.
        std::string content;
        switch (readThroughOneHandle(actual, expected, hooks, content)) {
        case HandleRead::Read: break;
        case HandleRead::Link: return verdict(relativePath, FollowUpFileSafety::Symlink);
        case HandleRead::TooLarge: return verdict(relativePath, FollowUpFileSafety::TooLarge);
        case HandleRead::ReadFailed: return verdict(relativePath, FollowUpFileSafety::ReadFailed);
        }

        // 6. - 8. what the bytes are.
        if (content.find('\0') != std::string::npos) return verdict(relativePath, FollowUpFileSafety::Binary);
        if (!projectContextIsValidUtf8(content)) return verdict(relativePath, FollowUpFileSafety::InvalidUtf8);
        if (containsSecret(relativePath, content)) return verdict(relativePath, FollowUpFileSafety::SecretDetected);

        FollowUpFileCandidate candidate = verdict(relativePath, FollowUpFileSafety::Eligible);
        candidate.contentSha256 = sha256Hex(content);
        candidate.content = std::move(content);
        return candidate;
    } catch (const std::exception&) {
        return verdict(relativePath, FollowUpFileSafety::ReadFailed);
    }
}

} // namespace

FilesystemFollowUpFileLoader::FilesystemFollowUpFileLoader(FollowUpFileLoaderHooks hooks)
    : hooks_(std::move(hooks))
{
}

common::Result<std::vector<FollowUpFileCandidate>> FilesystemFollowUpFileLoader::load(
    const fs::path& workspaceRoot, const std::vector<std::string>& relativePaths) const
{
    using Result = common::Result<std::vector<FollowUpFileCandidate>>;
    const auto unavailable = [] {
        return Result::failure({"follow_up.workspace_unavailable", "The workspace of the source Run is not available"});
    };
    if (workspaceRoot.empty() || !workspaceRoot.is_absolute()) return unavailable();
    std::error_code error;
    if (!fs::is_directory(workspaceRoot, error) || error) return unavailable();
    const fs::path canonicalRoot = fs::weakly_canonical(workspaceRoot, error);
    if (error) return unavailable();

    const WorkspaceReadPolicy policy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto trusted = WorkspaceReadPolicyValidator::validateTrustedPolicy(policy);
    if (!trusted.success()) return unavailable();

    std::vector<FollowUpFileCandidate> candidates;
    candidates.reserve(relativePaths.size());
    for (const auto& path : relativePaths) candidates.push_back(loadOne(policy, canonicalRoot, path, hooks_));
    return Result::ok(std::move(candidates));
}

} // namespace ai::application
