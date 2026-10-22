#include "ai/application/FollowUpSourceReader.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace ai::application {
namespace {

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

std::string utf8Of(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace

std::vector<std::string> followUpCandidatePathsOf(const std::vector<ProposedFileChange>& changes)
{
    // identity -> the bytewise smallest spelling that has it
    std::map<std::string, std::string> byIdentity;
    for (const auto& change : changes) {
        std::string path;
        try {
            path = utf8Of(change.relativePath);
        } catch (const std::exception&) {
            continue;
        }
        if (!followUpCandidatePathIsCanonical(path)) continue;
        const auto key = followUpCandidatePathIdentity(path);
        const auto found = byIdentity.find(key);
        if (found == byIdentity.end()) byIdentity.emplace(key, std::move(path));
        else if (path < found->second) found->second = std::move(path);
    }
    std::vector<std::string> paths;
    paths.reserve(byIdentity.size());
    for (auto& [key, path] : byIdentity) {
        static_cast<void>(key);
        paths.push_back(std::move(path));
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

FollowUpSourceReader::FollowUpSourceReader(
    const IProjectRepository& projects,
    const IRunRepository& runs,
    const IOutcomeBriefRepository& briefs,
    const ProjectContextSnapshotService& snapshots,
    const IChangeProposalRepository& proposals,
    const IFollowUpFileLoader& files)
    : projects_(projects)
    , runs_(runs)
    , briefs_(briefs)
    , snapshots_(snapshots)
    , proposals_(proposals)
    , files_(files)
{
}

common::Result<FollowUpSourceMaterial> FollowUpSourceReader::read(const ProjectId& projectId, const RunId& sourceRunId) const
{
    using Result = common::Result<FollowUpSourceMaterial>;

    const auto project = projects_.findById(projectId);
    if (!project.success()) return Result::failure(*project.error());
    if (!project.value().has_value()) return fail<FollowUpSourceMaterial>("follow_up.project_not_found", "No such project");
    const auto run = runs_.findRun(sourceRunId);
    if (!run.success()) return Result::failure(*run.error());
    if (!run.value().has_value()) return fail<FollowUpSourceMaterial>("follow_up.run_not_found", "No such Run");

    FollowUpSourceMaterial material;
    material.project = *project.value();
    material.runProjectId = run.value()->projectId;
    // The two facts the caller refuses on before it uses anything else: nothing further is read, and in particular no workspace.
    if (material.project.status != ProjectStatus::Active || material.runProjectId != projectId) return Result::ok(std::move(material));

    // The brief first: it is cheap, and a Run without a current brief cannot be planned from, whatever its workspace holds.
    const auto head = briefs_.findHead(sourceRunId);
    if (!head.success()) return Result::failure(*head.error());
    if (!head.value().has_value()) {
        return fail<FollowUpSourceMaterial>("follow_up.outcome_missing", "The Run has no current Outcome Brief");
    }
    material.brief = head.value()->brief;

    // The candidates: every file change of every proposal of the Run, read from the Run's own frozen workspace. Their PATHS are
    // counted first - cheap, and before the snapshot hashes the workspace or one file is opened: more than the dialog can show is
    // refused whole, never cut short.
    const auto sets = proposals_.listForRun(sourceRunId);
    if (!sets.success()) return Result::failure(*sets.error());
    std::vector<ChangeSetId> ids;
    ids.reserve(sets.value().size());
    for (const auto& set : sets.value()) ids.push_back(set.changeSetId);
    std::vector<ProposedFileChange> changes;
    if (!ids.empty()) {
        const auto grouped = proposals_.listChangesForSets(ids);
        if (!grouped.success()) return Result::failure(*grouped.error());
        for (const auto& id : ids) {   // the change sets' own order (oldest first), then each set's order
            const auto found = grouped.value().find(id);
            if (found == grouped.value().end()) continue;
            changes.insert(changes.end(), found->second.begin(), found->second.end());
        }
    }
    const std::vector<std::string> paths = followUpCandidatePathsOf(changes);
    if (paths.size() > kFollowUpMaximumCandidates) {
        material.candidateRefusal = common::Error {"follow_up.too_many_candidate_files",
            "The source Run has more candidate files than a follow-up can show (" + std::to_string(kFollowUpMaximumCandidates) + ")"};
        return Result::ok(std::move(material));
    }

    // The snapshot hashes the workspace: the expensive read.
    ProjectContextSnapshotRequest snapshotRequest;
    snapshotRequest.projectId = projectId;
    snapshotRequest.selectedRunId = sourceRunId;
    auto snapshot = snapshots_.build(snapshotRequest);
    if (!snapshot.success()) return Result::failure(*snapshot.error());
    material.snapshot = std::move(snapshot.value());

    const std::filesystem::path workspaceRoot = run.value()->workspacePath.value_or(material.project.rootPath);
    auto candidates = files_.load(workspaceRoot, paths);
    if (!candidates.success()) return Result::failure(*candidates.error());
    material.candidates = std::move(candidates.value());
    return Result::ok(std::move(material));
}

} // namespace ai::application
