#include "ai/application/workspace/ProjectOutputApprovalService.hpp"

#include "ai/application/IdGenerator.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/workspace/RecordedWorkspaceExclusions.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

namespace ai::application {

namespace fs = std::filesystem;

namespace {

template <class T>
common::Result<T> refuse(const char* code, std::string message)
{
    return common::Result<T>::failure(common::Error {code, std::move(message)});
}

// The comparison key of every component of a path (ASCII case-folded on Windows): the one rule of the approvals.
std::vector<std::string> keyComponents(const fs::path& path)
{
    std::vector<std::string> components;
    for (const auto& component : path) {
        std::string key = detail::normalizedPathKey(component);
        if (!key.empty()) components.push_back(std::move(key));
    }
    return components;
}

bool startsWith(const std::vector<std::string>& value, const std::vector<std::string>& prefix)
{
    return prefix.size() <= value.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::vector<std::string> splitKey(const std::string& key)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= key.size()) {
        const auto end = key.find('/', start);
        parts.push_back(key.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

// At most 256 bytes (the column's CHECK), cut on a UTF-8 boundary.
std::string boundedActor(std::string value)
{
    constexpr std::size_t kMax = 256;
    if (value.size() <= kMax) return value;
    std::size_t cut = kMax;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U) --cut;
    value.resize(cut);
    return value;
}

} // namespace

ProjectOutputApprovalService::ProjectOutputApprovalService(IProjectRepository& projects,
    IProjectOutputApprovalRepository& approvals, const IProtectedStoragePathSource& protectedPaths,
    const IOutputDirectoryInspector& inspector, OutputDirectoryInspectionLimits limits)
    : projects_(projects)
    , approvals_(approvals)
    , protectedPaths_(protectedPaths)
    , inspector_(inspector)
    , limits_(limits)
{
}

common::Result<fs::path> ProjectOutputApprovalService::canonicalRootOf(const fs::path& root) const
{
    if (root.empty() || !root.is_absolute()) {
        return refuse<fs::path>(kOutputApprovalWorkspaceUnavailableCode, "The workspace root is not an absolute path");
    }
    std::error_code error;
    if (!fs::is_directory(detail::longPath(root), error) || error) {
        return refuse<fs::path>(kOutputApprovalWorkspaceUnavailableCode, "The workspace root is not an available folder");
    }
    fs::path canonical = detail::withoutLongPrefix(fs::canonical(detail::longPath(root), error));
    if (error || canonical.empty()) {
        return refuse<fs::path>(kOutputApprovalWorkspaceUnavailableCode, "The workspace root cannot be resolved");
    }
    return common::Result<fs::path>::ok(std::move(canonical));
}

common::Result<ProjectOutputApprovalService::LoadedProject> ProjectOutputApprovalService::loadProject(
    const ProjectId& projectId, bool requireActive) const
{
    using Result = common::Result<LoadedProject>;
    const auto found = projects_.findById(projectId);
    if (!found.success()) return Result::failure(*found.error());
    if (!found.value().has_value()) return refuse<LoadedProject>("project.not_found", "The project does not exist");
    if (requireActive && found.value()->status != ProjectStatus::Active) {
        return refuse<LoadedProject>(kOutputApprovalProjectNotActiveCode, "The project is not active");
    }
    const auto root = canonicalRootOf(found.value()->rootPath);
    if (!root.success()) return Result::failure(*root.error());
    LoadedProject loaded;
    loaded.project = *found.value();
    loaded.canonicalRoot = root.value();
    loaded.workspaceRoot = detail::genericUtf8(root.value());
    loaded.workspaceKey = outputApprovalWorkspaceKey(root.value());
    return Result::ok(std::move(loaded));
}

common::Result<void> ProjectOutputApprovalService::checkProtectedStorage(
    const fs::path& canonicalRoot, const fs::path& physical) const
{
    const auto listed = protectedPaths_.protectedPaths(canonicalRoot);
    if (!listed.success()) {
        return refuse<void>(kOutputApprovalProtectedUnavailableCode,
            "The application's protected storage paths could not be determined: " + listed.error()->message);
    }
    const std::vector<std::string> candidate = keyComponents(physical);
    for (const auto& entry : listed.value()) {
        if (entry.path.empty() || !entry.path.is_absolute()) {
            return refuse<void>(kOutputApprovalProtectedUnavailableCode, "A protected storage path is not absolute");
        }
        std::error_code error;
        const fs::path resolved = detail::canonicalizeWithMissingSuffix(entry.path, error);
        if (error) {
            return refuse<void>(kOutputApprovalProtectedUnavailableCode, "A protected storage path could not be resolved");
        }
        const std::vector<std::string> protectedKey = keyComponents(resolved);
        // Equal, or the approval would cover the protected path: both mean the approval contains the storage.
        const bool equalOrCovers = startsWith(protectedKey, candidate);
        // Inside a protected FOLDER (a file has no inside).
        const bool inside = entry.directory && startsWith(candidate, protectedKey);
        if (equalOrCovers || inside) {
            return refuse<void>(kOutputApprovalProtectedStorageCode,
                "The folder contains or lies inside storage the application keeps for itself");
        }
    }
    return common::Result<void>::ok();
}

common::Result<ProjectOutputApprovalService::PathFacts> ProjectOutputApprovalService::checkPath(
    const fs::path& canonicalRoot, const std::string& relativePath) const
{
    using Result = common::Result<PathFacts>;
    const auto parsed = RecordedWorkspaceExclusions::parsePaths({"path:" + relativePath});
    if (!parsed.success() || parsed.value().size() != 1) {
        return refuse<PathFacts>(kOutputApprovalInvalidPathCode,
            parsed.success() ? "The path is not a single folder" : parsed.error()->message);
    }
    const fs::path relative = parsed.value().front();
    const std::string canonicalRelative = detail::genericUtf8(relative);
    if (canonicalRelative != relativePath) {
        return refuse<PathFacts>(kOutputApprovalInvalidPathCode, "The path is not in its canonical form");
    }

    PathFacts facts;
    facts.relativePath = canonicalRelative;
    bool missing = false;
    fs::path walk = canonicalRoot;
    fs::path existing;
    for (const auto& component : relative) {
        walk /= component;
        if (missing) continue;
        std::error_code statusError;
        const fs::file_status status = fs::symlink_status(detail::longPath(walk), statusError);
        if (statusError && statusError != std::errc::no_such_file_or_directory && statusError != std::errc::not_a_directory) {
            return refuse<PathFacts>(kOutputApprovalPathUnresolvableCode, "A part of the path could not be examined");
        }
        if (statusError || status.type() == fs::file_type::not_found) {
            missing = true; // the rest is the future tail of a folder that does not exist yet
            continue;
        }
        std::error_code linkError;
        if (detail::isSymlinkOrReparsePoint(walk, linkError)) {
            return refuse<PathFacts>(kOutputApprovalReparsePointCode, "A part of the path is a link; links are not followed");
        }
        if (linkError) {
            return refuse<PathFacts>(kOutputApprovalPathUnresolvableCode, "A part of the path could not be examined");
        }
        if (status.type() != fs::file_type::directory) {
            return refuse<PathFacts>(kOutputApprovalNotADirectoryCode, "A part of the path is a file, not a folder");
        }
        existing /= component;
    }
    facts.exists = !missing;
    facts.existingPrefix = detail::genericUtf8(existing);

    std::error_code resolveError;
    facts.physical = detail::canonicalizeWithMissingSuffix(canonicalRoot / relative, resolveError);
    if (resolveError) return refuse<PathFacts>(kOutputApprovalPathUnresolvableCode, "The path could not be resolved");
    const fs::path physicalRelative = facts.physical.lexically_relative(canonicalRoot);
    if (physicalRelative.empty() || *physicalRelative.begin() == ".."
        || outputApprovalPathKey(detail::genericUtf8(physicalRelative)) != outputApprovalPathKey(canonicalRelative)) {
        return refuse<PathFacts>(kOutputApprovalEscapesRootCode, "The path does not stay inside the workspace root");
    }

    const auto storage = checkProtectedStorage(canonicalRoot, facts.physical);
    if (!storage.success()) return Result::failure(*storage.error());

    if (facts.exists) {
        std::error_code markerError;
        const fs::file_status marker = fs::symlink_status(detail::longPath(facts.physical / "CMakeLists.txt"), markerError);
        if (markerError && markerError != std::errc::no_such_file_or_directory) {
            return refuse<PathFacts>(kOutputApprovalPathUnresolvableCode, "The folder could not be examined");
        }
        if (!markerError && marker.type() != fs::file_type::not_found) {
            return refuse<PathFacts>(kOutputDirContainsCMakeSourcesCode,
                "A CMakeLists.txt lies directly in this folder, so it holds project sources and cannot be declared output");
        }
    }
    return Result::ok(std::move(facts));
}

common::Result<OutputApprovalPreview> ProjectOutputApprovalService::inspectCandidate(
    const LoadedProject& loaded, const std::string& relativePath, std::stop_token stopToken) const
{
    using Result = common::Result<OutputApprovalPreview>;
    const auto facts = checkPath(loaded.canonicalRoot, relativePath);
    if (!facts.success()) return Result::failure(*facts.error());

    OutputApprovalPreview preview;
    preview.projectId = loaded.project.id;
    preview.workspaceRoot = loaded.workspaceRoot;
    preview.relativePath = facts.value().relativePath;
    preview.exists = facts.value().exists;
    preview.existingPrefix = facts.value().existingPrefix;
    if (preview.exists) {
        preview.inspection = inspector_.inspect(facts.value().physical, limits_, stopToken);
        switch (preview.inspection.status) {
        case OutputInspectionStatus::Cancelled:
            return refuse<OutputApprovalPreview>(kOutputApprovalCancelledCode, "The inspection was cancelled");
        case OutputInspectionStatus::Failed:
            return refuse<OutputApprovalPreview>(kOutputDirInspectionFailedCode,
                "The folder could not be inspected: " + preview.inspection.failure);
        case OutputInspectionStatus::Incomplete:
            preview.requiresIncompleteConfirmation = true;
            break;
        case OutputInspectionStatus::Complete:
            break;
        }
        preview.requiresNestedConfirmation = preview.inspection.nestedSourceCount > 0;
    }
    const auto stored = approvals_.listForWorkspace(loaded.project.id, loaded.workspaceKey);
    if (!stored.success()) return Result::failure(*stored.error());
    const std::string key = outputApprovalPathKey(preview.relativePath);
    preview.alreadyApproved = std::any_of(stored.value().begin(), stored.value().end(),
        [&key](const ProjectOutputApproval& record) { return record.pathKey == key; });
    return Result::ok(std::move(preview));
}

common::Result<OutputApprovalPreview> ProjectOutputApprovalService::preview(
    const ProjectId& projectId, const std::string& relativePath, std::stop_token stopToken) const
{
    const auto loaded = loadProject(projectId, /*requireActive=*/true);
    if (!loaded.success()) return common::Result<OutputApprovalPreview>::failure(*loaded.error());
    return inspectCandidate(loaded.value(), relativePath, stopToken);
}

common::Result<ProjectOutputApprovalCommitResult> ProjectOutputApprovalService::approve(
    const OutputApprovalRequest& request, std::stop_token stopToken)
{
    using Result = common::Result<ProjectOutputApprovalCommitResult>;
    const auto loaded = loadProject(request.projectId, /*requireActive=*/true);
    if (!loaded.success()) return Result::failure(*loaded.error());
    if (request.expectedWorkspaceRoot.has_value()
        && outputApprovalWorkspaceKey(detail::pathFromUtf8(*request.expectedWorkspaceRoot)) != loaded.value().workspaceKey) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputApprovalStaleCode,
            "The project's folder is no longer the one the preview was made for");
    }
    if (request.expectedRelativePath.has_value()
        && outputApprovalPathKey(*request.expectedRelativePath) != outputApprovalPathKey(request.relativePath)) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputApprovalStaleCode,
            "The path is not the one the preview was made for");
    }

    // Everything is checked again here, and the folder is walked now, with no lock held in the database.
    const auto inspected = inspectCandidate(loaded.value(), request.relativePath, stopToken);
    if (!inspected.success()) return Result::failure(*inspected.error());
    const OutputApprovalPreview& facts = inspected.value();
    if (facts.requiresNestedConfirmation && !request.acknowledgeNestedSources) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputDirNestedSourcesUnconfirmedCode,
            "CMakeLists.txt files were found below this folder; they would be hidden too and must be confirmed");
    }
    if (facts.requiresIncompleteConfirmation && !request.acknowledgeIncomplete) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputDirInspectionIncompleteCode,
            "The folder could not be inspected completely; its content is only partly known and must be confirmed");
    }
    if (stopToken.stop_requested()) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputApprovalCancelledCode, "The approval was cancelled");
    }

    // The project may have changed while the folder was walked: the same root and an active project, or nothing is written.
    const auto again = loadProject(request.projectId, /*requireActive=*/true);
    if (!again.success() || again.value().workspaceKey != loaded.value().workspaceKey) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputApprovalStaleCode,
            "The project changed while the folder was inspected");
    }

    ProjectOutputApproval candidate;
    candidate.id = generateUuid();
    candidate.projectId = loaded.value().project.id;
    candidate.workspaceRoot = loaded.value().workspaceRoot;
    candidate.workspaceKey = loaded.value().workspaceKey;
    candidate.relativePath = facts.relativePath;
    candidate.pathKey = outputApprovalPathKey(facts.relativePath);
    candidate.inspectionComplete = !facts.requiresIncompleteConfirmation;
    candidate.nestedSourcesAcknowledged = facts.requiresNestedConfirmation && request.acknowledgeNestedSources;
    candidate.approvedBy = boundedActor(request.approvedBy);
    candidate.approvedAt = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());

    // The set rule runs inside the repository's atomic command, against the set it reads there.
    const ProjectOutputApprovalSetValidator validator =
        [candidate](const std::vector<ProjectOutputApproval>& current) -> common::Result<void> {
        const std::vector<std::string> mine = splitKey(candidate.pathKey);
        for (const auto& record : current) {
            const std::vector<std::string> other = splitKey(record.pathKey);
            if (startsWith(mine, other)) {
                return refuse<void>(kOutputApprovalNestedCode, "A folder that contains this one is already approved");
            }
            if (startsWith(other, mine)) {
                return refuse<void>(kOutputApprovalCoversExistingCode, "This folder would contain a folder that is already approved");
            }
        }
        std::vector<std::string> exclusions;
        for (const auto& record : current) exclusions.push_back("path:" + record.relativePath);
        exclusions.push_back("path:" + candidate.relativePath);
        const auto valid = RecordedWorkspaceExclusions::parsePaths(exclusions);
        if (!valid.success()) return refuse<void>(kOutputApprovalInvalidCode, valid.error()->message);
        return common::Result<void>::ok();
    };
    // The last cooperative point, after the second read of the project and the preparation, with nothing that can wait in between.
    // A commit that has begun is not stopped: it ends with its real result.
    if (stopToken.stop_requested()) {
        return refuse<ProjectOutputApprovalCommitResult>(kOutputApprovalCancelledCode, "The approval was cancelled");
    }
    return approvals_.commit(candidate, validator);
}

common::Result<bool> ProjectOutputApprovalService::revoke(const ProjectId& projectId, const std::string& approvalId)
{
    const auto loaded = projects_.findById(projectId);
    if (!loaded.success()) return common::Result<bool>::failure(*loaded.error());
    if (!loaded.value().has_value()) return refuse<bool>("project.not_found", "The project does not exist");
    return approvals_.revoke(projectId, approvalId);
}

common::Result<std::vector<OutputApprovalView>> ProjectOutputApprovalService::list(const ProjectId& projectId) const
{
    using Result = common::Result<std::vector<OutputApprovalView>>;
    const auto project = projects_.findById(projectId);
    if (!project.success()) return Result::failure(*project.error());
    if (!project.value().has_value()) return refuse<std::vector<OutputApprovalView>>("project.not_found", "The project does not exist");
    std::string currentKey;
    const auto root = canonicalRootOf(project.value()->rootPath);
    if (root.success()) currentKey = outputApprovalWorkspaceKey(root.value());
    const auto stored = approvals_.listForProject(projectId);
    if (!stored.success()) return Result::failure(*stored.error());
    std::vector<OutputApprovalView> views;
    for (const auto& record : stored.value()) {
        views.push_back({record, !currentKey.empty() && record.workspaceKey == currentKey});
    }
    return Result::ok(std::move(views));
}

common::Result<WorkspaceReadPolicy> ProjectOutputApprovalService::policyFor(
    const ProjectId& projectId, const fs::path& workspaceRoot) const
{
    using Result = common::Result<WorkspaceReadPolicy>;
    const auto root = canonicalRootOf(workspaceRoot);
    if (!root.success()) return Result::failure(*root.error());
    const std::string key = outputApprovalWorkspaceKey(root.value());

    const auto stored = approvals_.listForWorkspace(projectId, key);
    if (!stored.success()) {
        // A read failure is never "no exclusions".
        return Result::failure(*stored.error());
    }
    if (stored.value().empty()) return Result::ok(WorkspaceReadPolicy::secureDefaults(workspaceRoot));

    const auto invalid = [](const ProjectOutputApproval& record, const std::string& why) {
        return refuse<WorkspaceReadPolicy>(kOutputApprovalInvalidCode, "The approval of '" + record.relativePath + "' is not usable: " + why);
    };
    std::vector<std::string> exclusions;
    for (const auto& record : stored.value()) {
        // The row must still say what it was keyed by: the root it names, the key of that root and the root asked for are one root,
        // and the path and its key are one path. A row that disagrees with itself is rejected, never repaired into a valid one.
        const fs::path recordedRoot = detail::pathFromUtf8(record.workspaceRoot);
        if (record.projectId != projectId) return invalid(record, "it belongs to another project");
        if (record.workspaceRoot.empty() || !recordedRoot.is_absolute()) return invalid(record, "its workspace root is not an absolute path");
        if (record.workspaceKey != key || outputApprovalWorkspaceKey(recordedRoot) != key) {
            return invalid(record, "its workspace root and its workspace key do not name the requested root");
        }
        if (record.pathKey != outputApprovalPathKey(record.relativePath)) {
            return invalid(record, "its stored key does not match its path");
        }
        exclusions.push_back("path:" + record.relativePath);
    }
    auto policy = RecordedWorkspaceExclusions::policyFor(workspaceRoot, exclusions);
    if (!policy.success()) return refuse<WorkspaceReadPolicy>(kOutputApprovalInvalidCode, policy.error()->message);
    for (const auto& record : stored.value()) {
        const auto checked = checkPath(root.value(), record.relativePath);
        if (!checked.success()) return invalid(record, checked.error()->code + ": " + checked.error()->message);
    }
    return policy;
}

} // namespace ai::application
