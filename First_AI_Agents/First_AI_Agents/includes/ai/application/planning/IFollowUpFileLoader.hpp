#pragma once

#include "ai/application/planning/FollowUpComposerContext.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
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
// readable directory). The root comes from trusted configuration (the Run's frozen workspace or the project root), never from a model.
//
// P3c: what may be read is decided by a WHOLE read policy - today's secureDefaults plus the output folders the user approved - handed in
// by the caller, so that the catalogue of one follow-up read and its candidate files are judged under one and the same area. A path
// under policy.deniedPaths is a candidate whose `safety` is UnsafePath, without content or digest, and is never opened. The root is the
// policy's own workspaceRoot. There is deliberately no default that turns a policy back into a root: that would drop its deniedPaths.
class IFollowUpFileLoader {
public:
    virtual ~IFollowUpFileLoader() = default;

    [[nodiscard]] virtual common::Result<std::vector<FollowUpFileCandidate>> load(
        const WorkspaceReadPolicy& policy, const std::vector<std::string>& relativePaths) const = 0;

    // The previous call, for a caller that has no approvals to apply (a standalone use, a fixture): exactly today's secureDefaults of
    // that root.
    [[nodiscard]] common::Result<std::vector<FollowUpFileCandidate>> load(
        const std::filesystem::path& workspaceRoot, const std::vector<std::string>& relativePaths) const
    {
        return load(WorkspaceReadPolicy::secureDefaults(workspaceRoot), relativePaths);
    }
};

} // namespace ai::application
