#include "ai/application/verification/VerificationScratchPolicy.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ai::application {

namespace fs = std::filesystem;

namespace {

common::Result<fs::path> refuse(const char* code, std::string message)
{
    return common::Result<fs::path>::failure({code, std::move(message) + " " + kVerificationScratchChoosePrompt});
}

bool isReparsePoint(const fs::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(detail::longPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(path, error));
#endif
}

// Component-wise containment (case-insensitive on Windows): is `candidate` equal to or below `parent`?
bool isWithin(const fs::path& candidate, const fs::path& parent)
{
    auto candidateIt = candidate.begin();
    for (auto parentIt = parent.begin(); parentIt != parent.end(); ++parentIt, ++candidateIt) {
        if (candidateIt == candidate.end()) return false;
#ifdef _WIN32
        auto left = candidateIt->wstring();
        auto right = parentIt->wstring();
        std::transform(left.begin(), left.end(), left.begin(), ::towlower);
        std::transform(right.begin(), right.end(), right.begin(), ::towlower);
        if (left != right) return false;
#else
        if (*candidateIt != *parentIt) return false;
#endif
    }
    return true;
}

#ifdef _WIN32

struct LocalFreeDeleter {
    void operator()(void* pointer) const
    {
        if (pointer != nullptr) LocalFree(pointer);
    }
};
using LocalPointer = std::unique_ptr<void, LocalFreeDeleter>;

std::optional<std::wstring> sidText(PSID sid)
{
    if (sid == nullptr) return std::nullopt;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return std::nullopt;
    LocalPointer guard(text);
    return std::wstring(text);
}

std::optional<std::wstring> currentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::nullopt;
    struct TokenCloser {
        HANDLE handle;
        ~TokenCloser() { CloseHandle(handle); }
    } closer {token};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return std::nullopt;
    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return std::nullopt;
    return sidText(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}

// Wide SID text to the narrow text the decision works with (a SID is ASCII).
std::string narrow(const std::wstring& text)
{
    return std::string(text.begin(), text.end());
}

// Full control for SYSTEM, Administrators and the caller only; protected: nothing is inherited from the parent folder, and
// everything created inside (the overlay folders) inherits exactly this.
std::optional<std::wstring> privateSddl()
{
    const auto user = currentUserSid();
    if (!user.has_value()) return std::nullopt;
    return L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + *user + L")";
}

bool createPrivateDirectory(const fs::path& directory, std::error_code& error)
{
    const auto sddl = privateSddl();
    if (!sddl.has_value()) {
        error = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl->c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    LocalPointer guard(descriptor);
    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    if (!CreateDirectoryW(detail::longPath(directory).c_str(), &attributes)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return true;
}

// Replaces the DACL of an existing folder with the private one (only works for an owner / holder of WRITE_DAC).
bool tightenToPrivate(const fs::path& directory)
{
    const auto sddl = privateSddl();
    if (!sddl.has_value()) return false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl->c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    LocalPointer guard(descriptor);
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present || dacl == nullptr) return false;
    const std::wstring path = detail::longPath(directory).native();
    return SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr)
        == ERROR_SUCCESS;
}

#else

bool createPrivateDirectory(const fs::path& directory, std::error_code& error)
{
    if (!fs::create_directory(directory, error) || error) return false;
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
    return !error;
}

bool tightenToPrivate(const fs::path& directory)
{
    std::error_code error;
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
    return !error;
}

#endif

} // namespace

namespace {

// The accounts allowed to write without making the folder "shared": the caller, SYSTEM, Administrators, CREATOR OWNER (an
// inherit-only placeholder) and TrustedInstaller. Anyone else - Users, Authenticated Users, Everyone, another user - is "other".
bool isTrustedAccount(const std::string& sid, const std::string& currentUser)
{
    return (!currentUser.empty() && sid == currentUser) || sid == "S-1-5-18" || sid == "S-1-5-32-544" || sid == "S-1-3-0"
        || sid.rfind("S-1-5-80-956008885-", 0) == 0;
}

// Windows access-mask bits that let an account change what is in the folder (spelled out: FILE_WRITE_DATA = FILE_ADD_FILE, FILE_APPEND_DATA =
// FILE_ADD_SUBDIRECTORY).
constexpr std::uint32_t kAccessWriteMask = 0x00000002u   // FILE_WRITE_DATA
    | 0x00000004u                                         // FILE_APPEND_DATA
    | 0x00000010u                                         // FILE_WRITE_EA
    | 0x00000040u                                         // FILE_DELETE_CHILD
    | 0x00000100u                                         // FILE_WRITE_ATTRIBUTES
    | 0x00010000u                                         // DELETE
    | 0x00040000u                                         // WRITE_DAC
    | 0x00080000u                                         // WRITE_OWNER
    | 0x40000000u                                         // GENERIC_WRITE
    | 0x10000000u;                                        // GENERIC_ALL

} // namespace

bool VerificationScratchPolicy::evaluateWindowsAccess(const VerificationScratchAccessDescriptor& descriptor, std::string& reason)
{
    if (descriptor.currentUserSid.empty()) {
        reason = "the current account could not be identified";
        return false;
    }
    if (!descriptor.aclPresent) {
        reason = "it has no access list, so everyone can write to it";
        return false;
    }
    if (!isTrustedAccount(descriptor.ownerSid, descriptor.currentUserSid)) {
        reason = "it belongs to another account";
        return false;
    }
    for (const auto& entry : descriptor.allowedEntries) {
        if ((entry.mask & kAccessWriteMask) == 0) continue;
        if (isTrustedAccount(entry.sid, descriptor.currentUserSid)) continue;
        reason = "other accounts can write to it";
        return false;
    }
    return true;
}

bool VerificationScratchPolicy::isPrivateToCurrentUser(const fs::path& directory, std::string& reason)
{
#ifdef _WIN32
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring path = detail::longPath(directory).native();
    const DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
    LocalPointer guard(descriptor);
    if (status != ERROR_SUCCESS) {
        reason = "its permissions could not be read";
        return false;
    }
    const auto user = currentUserSid();
    if (!user.has_value()) {
        reason = "the current account could not be identified";
        return false;
    }
    VerificationScratchAccessDescriptor access;
    access.currentUserSid = narrow(*user);
    access.aclPresent = dacl != nullptr;
    const auto ownerText = sidText(owner);
    access.ownerSid = ownerText.has_value() ? narrow(*ownerText) : std::string {};
    if (dacl != nullptr) {
        ACL_SIZE_INFORMATION information {};
        if (!GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation)) {
            reason = "its access list could not be read";
            return false;
        }
        for (DWORD index = 0; index < information.AceCount; ++index) {
            LPVOID entry = nullptr;
            if (!GetAce(dacl, index, &entry)) {
                reason = "its access list could not be read";
                return false;
            }
            const auto* header = static_cast<const ACE_HEADER*>(entry);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE) continue;
            const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
            const auto sidString = sidText(const_cast<DWORD*>(&allowed->SidStart));
            // A SID that cannot be read is an account nobody can vouch for: it stays in the list as "other".
            access.allowedEntries.push_back({sidString.has_value() ? narrow(*sidString) : std::string("unreadable"), allowed->Mask});
        }
    }
    return evaluateWindowsAccess(access, reason);
#else
    struct stat information {};
    if (::stat(directory.c_str(), &information) != 0) {
        reason = "its permissions could not be read";
        return false;
    }
    if (information.st_uid != ::geteuid()) {
        reason = "it belongs to another account";
        return false;
    }
    if ((information.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        reason = "other accounts can access it";
        return false;
    }
    return true;
#endif
}

common::Result<fs::path> VerificationScratchPolicy::validate(
    const fs::path& candidate,
    const std::optional<fs::path>& workspaceRoot,
    bool createIfMissing,
    bool adoptExistingDefault,
    bool probeWritable)
{
    if (candidate.empty() || !candidate.is_absolute()
        || std::any_of(candidate.begin(), candidate.end(), [](const fs::path& part) { return part == ".."; })) {
        return refuse(kVerificationScratchInvalidPathCode,
            "The verification folder must be an absolute path without '..' parts.");
    }
#ifdef _WIN32
    // The spelling is checked first, so that a folder that would be refused for its length is never created.
    if (candidate.native().size() > kMaxVerificationScratchRootLength) {
        return refuse(kVerificationScratchTooLongCode,
            "The verification folder path is " + std::to_string(candidate.native().size())
                + " characters long; at most " + std::to_string(kMaxVerificationScratchRootLength)
                + " are allowed so that the build tools stay within the Windows path limit.");
    }
#endif
    // Overlap with the workspace is decided on the SPELLING first, so that a folder inside the project is never created at all.
    const auto overlapsWorkspace = [&workspaceRoot](const fs::path& folder) {
        if (!workspaceRoot.has_value() || workspaceRoot->empty()) return false;
        std::error_code workspaceError;
        const auto workspace = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(*workspaceRoot), workspaceError));
        return !workspaceError && (isWithin(folder, workspace) || isWithin(workspace, folder));
    };
    const char* const overlapMessage = "The verification folder must not contain, or lie inside, the project's own folder.";
    std::error_code error;
    {
        const auto spelled = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(candidate), error));
        if (!error && overlapsWorkspace(spelled)) return refuse(kVerificationScratchOverlapCode, overlapMessage);
        error.clear();
    }
    const bool exists = fs::exists(detail::longPath(candidate), error);
    if (error) {
        return refuse(kVerificationScratchUnavailableCode, "The verification folder could not be checked.");
    }
    if (exists) {
        if (isReparsePoint(candidate)) {
            return refuse(kVerificationScratchUnsafeCode,
                "The verification folder is a link or junction, which is not accepted.");
        }
        if (!fs::is_directory(detail::longPath(candidate), error) || error) {
            return refuse(kVerificationScratchUnavailableCode, "The verification path is not a folder.");
        }
    } else {
        if (!createIfMissing) {
            return refuse(kVerificationScratchUnavailableCode, "The verification folder does not exist.");
        }
        fs::create_directories(detail::longPath(candidate.parent_path()), error);
        if (error || !createPrivateDirectory(candidate, error)) {
            return refuse(kVerificationScratchUnavailableCode, "The verification folder could not be created.");
        }
    }

    const auto canonical = detail::withoutLongPrefix(fs::canonical(detail::longPath(candidate), error));
    if (error) {
        return refuse(kVerificationScratchUnavailableCode, "The verification folder could not be resolved.");
    }
#ifdef _WIN32
    if (canonical.native().size() > kMaxVerificationScratchRootLength) {
        return refuse(kVerificationScratchTooLongCode,
            "The verification folder path is " + std::to_string(canonical.native().size())
                + " characters long; at most " + std::to_string(kMaxVerificationScratchRootLength)
                + " are allowed so that the build tools stay within the Windows path limit.");
    }
#endif
    if (isReparsePoint(canonical)) {
        return refuse(kVerificationScratchUnsafeCode,
            "The verification folder is a link or junction, which is not accepted.");
    }
    // Again on the resolved path: a link inside the spelling can lead into the workspace.
    if (overlapsWorkspace(canonical)) return refuse(kVerificationScratchOverlapCode, overlapMessage);

    std::string reason;
    if (!isPrivateToCurrentUser(canonical, reason)) {
        // Only OUR default folder is adopted: a folder the user picked is never re-permissioned behind their back.
        if (!(adoptExistingDefault && tightenToPrivate(canonical) && isPrivateToCurrentUser(canonical, reason))) {
            return refuse(kVerificationScratchNotPrivateCode,
                "The verification folder is not private: " + reason
                    + ". The project's sources are copied there, so only you may be able to write to it. "
                      "A folder that does not exist yet is created for you with access for you only.");
        }
    }

    if (!probeWritable) return common::Result<fs::path>::ok(canonical);

    // A probe: the folder can really be written to and cleaned up.
    const auto probe = canonical / (".aio-probe-" + generateShortToken());
    {
        std::ofstream stream(detail::longPath(probe), std::ios::binary);
        if (!stream || !(stream << 'x').flush()) {
            return refuse(kVerificationScratchNoAccessCode, "The verification folder cannot be written to.");
        }
    }
    fs::remove(detail::longPath(probe), error);
    if (error) {
        return refuse(kVerificationScratchNoAccessCode, "The verification folder cannot be cleaned up.");
    }
    return common::Result<fs::path>::ok(canonical);
}

} // namespace ai::application
