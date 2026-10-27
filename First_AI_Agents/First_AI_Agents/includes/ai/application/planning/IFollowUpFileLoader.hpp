#pragma once

#include "ai/application/FollowUpComposerContext.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ai::application {

// PC-5C-2a: the port that turns candidate PATHS into candidate FILES - the loader that the pure context assembler (5A-2) assumes.
//
// `relativePaths` are canonical (followUpCandidatePathIsCanonical) and unique; the caller guarantees that. The result has ONE candidate
// per input path, in the same order, all with `selected = false`. A file that cannot or must not be sent is NOT an error: it is a
// candidate whose `safety` says why (UnsafePath, Symlink, Binary, InvalidUtf8, SecretDetected, TooLarge, ReadFailed) and which carries
// no content and no digest. Only a Eligible candidate has `content` (the COMPLETE bytes, at most
// kFollowUpComposerMaximumCandidateBytes) and the sha256 of exactly those bytes.
//
// Failure is reserved for what makes EVERY file unreadable: `follow_up.workspace_unavailable` (the trusted root is not an absolute
// readable directory). `workspaceRoot` must come from trusted configuration (the Run's frozen workspace or the project root), never from
// a model.
class IFollowUpFileLoader {
public:
    virtual ~IFollowUpFileLoader() = default;

    [[nodiscard]] virtual common::Result<std::vector<FollowUpFileCandidate>> load(
        const std::filesystem::path& workspaceRoot, const std::vector<std::string>& relativePaths) const = 0;
};

} // namespace ai::application
