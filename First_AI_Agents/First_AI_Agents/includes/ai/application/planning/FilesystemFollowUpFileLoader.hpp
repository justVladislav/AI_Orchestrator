#pragma once

#include "ai/application/planning/IFollowUpFileLoader.hpp"

#include <filesystem>
#include <functional>

namespace ai::application {

// PC-5C-2a: the read-only filesystem loader behind the follow-up composer's file excerpts. It runs, for EVERY candidate and in this
// order, the checks the design promises before a byte can reach a third-party provider; the first one that fails decides the verdict and
// nothing of the file is kept:
//   1. the lexical read policy (denied directories such as .git / build / out, credential file names and extensions, reserved device
//      names, `..`, absolute and drive-qualified paths) and the OS-level path guard (canonical containment in the trusted root, a
//      reparse point / junction below it)                                                              -> UnsafePath / Symlink
//      (that is a first, early refusal - it is NOT what the bytes are trusted on: a path can be swapped between a check and an open)
//   2. - 5. ONE handle, opened WITHOUT following a link (Windows: CreateFileW + FILE_FLAG_OPEN_REPARSE_POINT, shared for reading only, so
//      the file cannot be written, renamed or deleted while it is open; POSIX: open + O_NOFOLLOW). Everything below is asked of THAT
//      handle, never of the path again:
//   2. the handle is not a link or reparse point, and the path the handle really is (GetFinalPathNameByHandleW / /proc/self/fd) is the
//      expected path inside the canonical root - a junction swapped into a parent directory is caught here   -> Symlink
//   3. it is a regular file (not a directory, device or pipe)                                                 -> ReadFailed
//   4. its size, from the handle, is within kFollowUpComposerMaximumCandidateBytes (nothing larger is read)    -> TooLarge
//   5. the COMPLETE bytes are read from the handle once and must be as many as the size just seen, and one more read must find the
//      end of the file (a file that grew under the read is not trusted)                                       -> ReadFailed
//   6. no NUL byte                                                                                        -> Binary
//   7. valid UTF-8                                                                                        -> InvalidUtf8
//   8. the proposal secret scanner (the same one the assembler repeats) over the path and the bytes; a scan that cannot complete
//      counts as a hit                                                                                    -> SecretDetected
// Only a file that passes all eight is Eligible and carries its content and the sha256 of those exact bytes. It never writes, never
// follows a path outside the root and never throws; a path it cannot vet is excluded, not guessed.
//
// A reparse point that is not a link (for example a cloud-sync placeholder) is refused like a link: the loader cannot tell what its
// bytes are. A file another process holds open for WRITING cannot be opened for the read (the share mode refuses it) and is ReadFailed.
//
// TEST SEAM. The two hooks exist so that a test can do, DETERMINISTICALLY, what an attacker would do in the gap; production passes
// none. beforeOpen runs after the path checks and before the handle is opened (the old check-then-open window); afterOpen runs with the
// handle open, before the first thing is asked of it.
struct FollowUpFileLoaderHooks {
    std::function<void(const std::filesystem::path&)> beforeOpen;
    std::function<void(const std::filesystem::path&)> afterOpen;
};

class FilesystemFollowUpFileLoader final : public IFollowUpFileLoader {
public:
    explicit FilesystemFollowUpFileLoader(FollowUpFileLoaderHooks hooks = {});

    using IFollowUpFileLoader::load;

    // The policy is used as given - never replaced by secureDefaults - and its workspaceRoot is the root. A path under its deniedPaths
    // gets the UnsafePath verdict from the lexical policy check, before any handle is opened (the hooks never see it), and carries
    // neither content nor digest; the other candidates are judged exactly as before.
    [[nodiscard]] common::Result<std::vector<FollowUpFileCandidate>> load(
        const WorkspaceReadPolicy& policy, const std::vector<std::string>& relativePaths) const override;

private:
    FollowUpFileLoaderHooks hooks_;
};

} // namespace ai::application
