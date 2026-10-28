#pragma once

#include "ai/common/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// Where the TEMPORARY execution copies of a workspace (verification overlays: a candidate tree built from the baseline plus the
// proposals, in which cmake / qmllint run) are built. Only these copies live there: the durable manual-handoff candidate stays next
// to the database, and nothing here ever changes what is verified (the content-identity checks of the overlay are untouched).
//
// A scratch root is usable only when ALL of this holds (the first failing check names the refusal):
//  - it is an absolute path without ".." parts;
//  - on Windows it is short enough for the build tools: root + "\m-" + 16 hex characters + build directory + the 130 characters the
//    verifier reserves for object files must stay within MAX_PATH (260). The build directory is the workspace's own, so the root gets
//    a fixed budget of kMaxVerificationScratchRootLength characters (a build directory of up to 46 characters fits);
//  - it is a directory, not a link or junction;
//  - it does not contain and is not contained in the real workspace;
//  - only the current user (and SYSTEM / Administrators) can write to it - a copy of the project's sources must not be tamperable by
//    another user (Windows: owner and DACL are checked; POSIX: owner is the caller and mode has no group/other bits);
//  - a probe file can be created and removed in it.
// A folder that does not exist is created PRIVATE (Windows: protected DACL for the current user, SYSTEM and Administrators; POSIX:
// 0700). Nothing outside the chosen root is ever created, and no shared "C:\aio-*" folder is created on anyone's behalf.
inline constexpr std::size_t kMaxVerificationScratchRootLength = 64;
inline constexpr const char* kVerificationScratchInvalidPathCode = "verification_scratch.invalid_path";
inline constexpr const char* kVerificationScratchTooLongCode = "verification_scratch.too_long";
inline constexpr const char* kVerificationScratchUnavailableCode = "verification_scratch.unavailable";
inline constexpr const char* kVerificationScratchUnsafeCode = "verification_scratch.unsafe_link";
inline constexpr const char* kVerificationScratchOverlapCode = "verification_scratch.overlaps_workspace";
inline constexpr const char* kVerificationScratchNotPrivateCode = "verification_scratch.not_private";
inline constexpr const char* kVerificationScratchNoAccessCode = "verification_scratch.no_access";

// Said at the end of every refusal: what the user can do about it.
inline constexpr const char* kVerificationScratchChoosePrompt =
    "Choose a short folder that only you can write to in Settings (Data and privacy, Verification folder).";

// What the system says about a folder's owner and access list, reduced to what the privacy decision needs. The Windows code fills it
// from GetNamedSecurityInfo; the decision itself (evaluateWindowsAccess) is a pure function of this value, so it is tested with
// made-up answers - a folder owned by another account cannot be created without privileges.
struct VerificationScratchAccessEntry {
    std::string sid;           // textual SID of an ACCESS ALLOWED entry (inherit-only entries included: they reach what is created inside)
    std::uint32_t mask {0};    // its access mask
};
struct VerificationScratchAccessDescriptor {
    bool aclPresent {true};    // false = NULL DACL = everyone has full access
    std::string ownerSid;
    std::string currentUserSid;
    std::vector<VerificationScratchAccessEntry> allowedEntries;
};

struct VerificationScratchPolicy {
    // Validates candidate and returns the canonical path to use (no extended-length prefix). A missing folder is created private when
    // createIfMissing is set (the default root and a root the user is setting) and reported as unavailable otherwise.
    // workspaceRoot, when known, is checked for overlap. adoptExistingDefault: the folder is OUR default ("<TEMP>/aio-v") - if it
    // already exists with an inherited, not private DACL and is owned by the caller, it is tightened instead of refused.
    // probeWritable: create and remove a probe file (off for a read-only status display).
    [[nodiscard]] static common::Result<std::filesystem::path> validate(
        const std::filesystem::path& candidate,
        const std::optional<std::filesystem::path>& workspaceRoot,
        bool createIfMissing,
        bool adoptExistingDefault = false,
        bool probeWritable = true);

    // The decision of the Windows privacy check: the owner must be the current user, SYSTEM or Administrators, there must be an access
    // list, and no entry that grants a write-like right (write data/append/EA/attributes, delete child, delete, change permissions or
    // owner, generic write/all) may belong to anyone but the current user, SYSTEM, Administrators, CREATOR OWNER or TrustedInstaller.
    [[nodiscard]] static bool evaluateWindowsAccess(const VerificationScratchAccessDescriptor& descriptor, std::string& reason);

    // True when only the current user, SYSTEM and Administrators can write to the directory (see above). `reason` says why not.
    [[nodiscard]] static bool isPrivateToCurrentUser(const std::filesystem::path& directory, std::string& reason);
};

} // namespace ai::application
