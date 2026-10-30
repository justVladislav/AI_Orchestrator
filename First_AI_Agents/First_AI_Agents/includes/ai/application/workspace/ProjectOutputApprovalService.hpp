#pragma once

#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/workspace/OutputApprovalPolicySource.hpp"
#include "ai/application/workspace/OutputDirectoryInspector.hpp"
#include "ai/application/workspace/ProjectOutputApproval.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ai::application {

// P3a: the backend rules for a user's explicit approval of an output folder. P3c wires policyFor() (as IOutputApprovalPolicySource) to
// the consumers of a workspace; nothing yet creates an approval from a controller or a tool. An approval never grants a right - it
// only adds an exclusion for NEW Runs.

// ---- typed refusals (the repository adds output_approval.limit_reached / .conflict / .storage_failed) ----
inline constexpr const char* kOutputApprovalProjectNotActiveCode = "output_approval.project_not_active";
inline constexpr const char* kOutputApprovalWorkspaceUnavailableCode = "output_approval.workspace_unavailable";
inline constexpr const char* kOutputApprovalInvalidPathCode = "output_approval.invalid_path";
inline constexpr const char* kOutputApprovalEscapesRootCode = "output_approval.escapes_root";
inline constexpr const char* kOutputApprovalReparsePointCode = "output_approval.reparse_point";
inline constexpr const char* kOutputApprovalNotADirectoryCode = "output_approval.not_a_directory";
inline constexpr const char* kOutputApprovalPathUnresolvableCode = "output_approval.path_unresolvable";
inline constexpr const char* kOutputApprovalProtectedStorageCode = "output_approval.protected_storage";
inline constexpr const char* kOutputApprovalProtectedUnavailableCode = "output_approval.protected_unavailable";
inline constexpr const char* kOutputApprovalNestedCode = "output_approval.nested";
inline constexpr const char* kOutputApprovalCoversExistingCode = "output_approval.covers_existing";
inline constexpr const char* kOutputApprovalStaleCode = "output_approval.stale";
inline constexpr const char* kOutputApprovalCancelledCode = "output_approval.cancelled";
// policyFor(): a stored approval that is not usable as it stands (corrupt row, broken set, a path that became unsafe).
inline constexpr const char* kOutputApprovalInvalidCode = "output_approval.invalid";
inline constexpr const char* kOutputDirContainsCMakeSourcesCode = "output_dir.contains_cmake_sources";
inline constexpr const char* kOutputDirNestedSourcesUnconfirmedCode = "output_dir.nested_sources_unconfirmed";
inline constexpr const char* kOutputDirInspectionIncompleteCode = "output_dir.inspection_incomplete";
inline constexpr const char* kOutputDirInspectionFailedCode = "output_dir.inspection_failed";

// A file or folder of the application's own storage that no approval may equal, cover or (for a folder) lie under.
struct ProtectedStoragePath {
    std::filesystem::path path; // absolute; need not exist
    bool directory {false};
};

// The trusted list, supplied by the composition root - never by the UI, a project, a preset or an agent. A refusal to answer is an
// error, and an error makes the approval refused: an unknown list is not an empty list.
class IProtectedStoragePathSource {
public:
    virtual ~IProtectedStoragePathSource() = default;
    [[nodiscard]] virtual common::Result<std::vector<ProtectedStoragePath>> protectedPaths(
        const std::filesystem::path& workspaceRoot) const = 0;
};

class FunctionProtectedStoragePathSource final : public IProtectedStoragePathSource {
public:
    using Function = std::function<common::Result<std::vector<ProtectedStoragePath>>(const std::filesystem::path&)>;
    explicit FunctionProtectedStoragePathSource(Function function) : function_(std::move(function)) {}
    [[nodiscard]] common::Result<std::vector<ProtectedStoragePath>> protectedPaths(
        const std::filesystem::path& workspaceRoot) const override
    {
        return function_(workspaceRoot);
    }

private:
    Function function_;
};

struct OutputApprovalPreview {
    ProjectId projectId;
    std::string workspaceRoot;  // the canonical root taken from the project record
    std::string relativePath;   // the strict form of the path
    bool exists {false};        // the folder exists now (it need not: a first configure creates it)
    std::string existingPrefix; // the longest existing part of the path, relative to the root ("" = only the root)
    OutputDirectoryInspection inspection; // meaningful only when exists
    // What approve() will ask the user to confirm. They are independent: they confirm different facts.
    bool requiresNestedConfirmation {false};     // CMakeLists.txt files were found below the folder (the count may be a lower bound)
    bool requiresIncompleteConfirmation {false}; // the walk could not see everything (every count is a lower bound)
    bool alreadyApproved {false};                // this exact path is approved for this root
};

struct OutputApprovalRequest {
    ProjectId projectId;
    std::string relativePath;
    // What the preview the user saw said. When present and different now, approve() answers output_approval.stale and writes nothing.
    std::optional<std::string> expectedWorkspaceRoot;
    std::optional<std::string> expectedRelativePath;
    bool acknowledgeNestedSources {false};
    bool acknowledgeIncomplete {false};
    std::string approvedBy; // best-effort OS user name; may be empty
};

struct OutputApprovalView {
    ProjectOutputApproval approval;
    bool appliesToCurrentRoot {false}; // false: it belongs to another root of the project and is dormant
};

class ProjectOutputApprovalService final : public IOutputApprovalPolicySource {
public:
    ProjectOutputApprovalService(IProjectRepository& projects, IProjectOutputApprovalRepository& approvals,
        const IProtectedStoragePathSource& protectedPaths, const IOutputDirectoryInspector& inspector,
        OutputDirectoryInspectionLimits limits = {});

    // Read-only: creates nothing, writes nothing. The workspace root is the project's own record, never an argument.
    [[nodiscard]] common::Result<OutputApprovalPreview> preview(
        const ProjectId& projectId, const std::string& relativePath, std::stop_token stopToken = {}) const;

    // Repeats every check (the preview is information, not a pass). A repeat of an approved path returns the stored record
    // unchanged. Nothing is held in the database while the folder is walked.
    [[nodiscard]] common::Result<ProjectOutputApprovalCommitResult> approve(
        const OutputApprovalRequest& request, std::stop_token stopToken = {});

    // true = the record was removed. Never touches another project's record; allowed for an archived project.
    common::Result<bool> revoke(const ProjectId& projectId, const std::string& approvalId);

    // Every approval of the project, also those of earlier roots (dormant, appliesToCurrentRoot == false).
    [[nodiscard]] common::Result<std::vector<OutputApprovalView>> list(const ProjectId& projectId) const;

    // The producer of a NEW Run's policy (IOutputApprovalPolicySource): (project, the Run's frozen workspace root) -> the read policy. One consistent read of the
    // set, strict parsing, the cheap path checks again for every path (links, file instead of folder, root CMakeLists.txt, the current
    // protected storage) - and no walk. An empty set is exactly secureDefaults(workspaceRoot). A storage failure, a broken row or an
    // approval that is no longer safe is a typed error; it is never turned into "no exclusions".
    [[nodiscard]] common::Result<WorkspaceReadPolicy> policyFor(
        const ProjectId& projectId, const std::filesystem::path& workspaceRoot) const override;

private:
    struct PathFacts {
        std::string relativePath;
        std::filesystem::path physical; // the canonical absolute location (missing tail appended lexically)
        bool exists {false};
        std::string existingPrefix;
    };
    struct LoadedProject {
        Project project;
        std::filesystem::path canonicalRoot;
        std::string workspaceRoot; // generic UTF-8
        std::string workspaceKey;
    };

    [[nodiscard]] common::Result<LoadedProject> loadProject(const ProjectId& projectId, bool requireActive) const;
    [[nodiscard]] common::Result<std::filesystem::path> canonicalRootOf(const std::filesystem::path& root) const;
    [[nodiscard]] common::Result<PathFacts> checkPath(
        const std::filesystem::path& canonicalRoot, const std::string& relativePath) const;
    [[nodiscard]] common::Result<void> checkProtectedStorage(
        const std::filesystem::path& canonicalRoot, const std::filesystem::path& physical) const;
    [[nodiscard]] common::Result<OutputApprovalPreview> inspectCandidate(
        const LoadedProject& loaded, const std::string& relativePath, std::stop_token stopToken) const;

    IProjectRepository& projects_;
    IProjectOutputApprovalRepository& approvals_;
    const IProtectedStoragePathSource& protectedPaths_;
    const IOutputDirectoryInspector& inspector_;
    OutputDirectoryInspectionLimits limits_;
};

} // namespace ai::application
