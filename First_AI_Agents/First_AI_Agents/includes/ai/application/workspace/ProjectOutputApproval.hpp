#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ai::application {

// P3a: a user's explicit decision that a folder of one project workspace is generated output and may be excluded from the analysis of
// NEW Runs. A record is evidence of that decision for exactly (project, workspace root, relative path); it is never a Run's frozen
// area (a Run freezes its own `path:` exclusions in its baseline artifact) and never grants anything - it only adds an exclusion.

// The most approvals one (project, workspace root) pair can hold. With the ~27 static rules this keeps the recorded exclusions of a
// baseline far below its 128-entry format limit.
inline constexpr std::size_t kProjectOutputApprovalLimit = 8;

// Error codes of the storage port (the service adds the checks of its own, see ProjectOutputApprovalService.hpp).
inline constexpr const char* kOutputApprovalLimitReachedCode = "output_approval.limit_reached";
inline constexpr const char* kOutputApprovalConflictCode = "output_approval.conflict";
inline constexpr const char* kOutputApprovalStorageFailedCode = "output_approval.storage_failed";

struct ProjectOutputApproval {
    std::string id;
    ProjectId projectId;
    // The canonical absolute workspace root the decision was made for (UTF-8, generic separators) and its comparison key.
    std::string workspaceRoot;
    std::string workspaceKey;
    // The strict RecordedWorkspaceExclusions form of the folder, relative to that root, and its comparison key.
    std::string relativePath;
    std::string pathKey;
    // What the inspection at approval time could and could not establish, and what the user confirmed (display facts).
    bool inspectionComplete {false};
    bool nestedSourcesAcknowledged {false};
    // Best-effort name of the operating-system user; may be empty. Not an authenticated identity.
    std::string approvedBy;
    std::chrono::system_clock::time_point approvedAt {};
};

// THE rule for comparing approval roots and paths, shared by the repositories and the service: the generic UTF-8 spelling, ASCII
// case-folded on Windows (the same "ASCII case-insensitive, exact otherwise" rule as the workspace read policy), exact elsewhere.
[[nodiscard]] inline std::string outputApprovalPathKey(const std::string& relativePathUtf8)
{
    return detail::normalizedPathKey(detail::pathFromUtf8(relativePathUtf8));
}

[[nodiscard]] inline std::string outputApprovalWorkspaceKey(const std::filesystem::path& canonicalRoot)
{
    std::string key = detail::normalizedPathKey(canonicalRoot);
    while (key.size() > 1 && key.back() == '/') key.pop_back();
    return key;
}

enum class ProjectOutputApprovalCommitOutcome {
    Inserted,
    AlreadyApproved, // the exact path was already approved: the stored record is returned unchanged
};

struct ProjectOutputApprovalCommitResult {
    ProjectOutputApprovalCommitOutcome outcome {ProjectOutputApprovalCommitOutcome::Inserted};
    ProjectOutputApproval approval;
};

// Checks the whole set that would result from adding the candidate (the validator captures the candidate itself). It receives the
// records the pair already has. It runs INSIDE the repository's atomic command, so it must be pure and fast: no file system, and no
// call into this or any other repository.
using ProjectOutputApprovalSetValidator =
    std::function<common::Result<void>(const std::vector<ProjectOutputApproval>& current)>;

/**
 * Persistence port of the approvals.
 *
 * commit() is the only way to add a record and it is ONE atomic command: read the pair's whole set, answer an exact repeat of the
 * path (same path_key) with the stored record unchanged - even when the pair is full - then check the limit and the validator
 * against current + candidate, then insert. Two concurrent commits can therefore never both pass a check that only holds for one of
 * them. A read failure is an error, never an empty set.
 */
class IProjectOutputApprovalRepository {
public:
    virtual ~IProjectOutputApprovalRepository() = default;

    [[nodiscard]] virtual common::Result<ProjectOutputApprovalCommitResult> commit(
        const ProjectOutputApproval& candidate, const ProjectOutputApprovalSetValidator& validator) = 0;

    // One consistent read of the records of a (project, workspace key) pair, ordered by path_key.
    [[nodiscard]] virtual common::Result<std::vector<ProjectOutputApproval>> listForWorkspace(
        const ProjectId& projectId, const std::string& workspaceKey) const = 0;

    // All records of a project, whatever root they belong to, ordered by (workspace_key, path_key).
    [[nodiscard]] virtual common::Result<std::vector<ProjectOutputApproval>> listForProject(const ProjectId& projectId) const = 0;

    // Deletes one record of this project. false = no such record in this project (another project's record is never touched).
    virtual common::Result<bool> revoke(const ProjectId& projectId, const std::string& approvalId) = 0;
};

} // namespace ai::application
