#include "ai/application/context/ProjectContextSnapshotService.hpp"

#include "ai/application/Sha256.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {
namespace {

// The mandatory workspace item lists at most this many top-level directories (then a count of the rest), so
// it can never outgrow the catalog budget.
constexpr std::size_t kProjectContextMaximumSummaryDirectories = 48;

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

std::int64_t epochMilliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

void appendUnique(std::vector<std::string>& values, std::vector<std::string> additions)
{
    for (auto& value : additions) {
        if (std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(std::move(value));
        }
    }
}

ProjectContextItem contextItem(
    std::string text,
    std::string kind,
    std::string id,
    bool mandatory = false,
    std::string digest = {})
{
    ProjectContextItem item;
    item.text = std::move(text);
    item.source.kind = std::move(kind);
    item.source.id = std::move(id);
    item.source.digest = digest.empty() ? sha256Hex(item.text) : std::move(digest);
    item.mandatory = mandatory;
    return item;
}

void appendSection(ProjectContextAssemblyInput& input, ProjectContextCategory category,
    std::vector<ProjectContextItem> items)
{
    if (items.empty()) return;
    ProjectContextSectionInput section;
    section.category = category;
    section.items = std::move(items);
    input.sections.push_back(std::move(section));
}

std::string join(const std::vector<std::string>& values)
{
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out << ',';
        out << values[index];
    }
    return out.str();
}

constexpr const char* kOmittedMarker = "[omitted by context safety policy]";

// Untrusted prose (ledger text, transition reasons, the project name / description, Project Memory) is cleaned
// before it can reach the assembler: not well-formed UTF-8 is unusable, and a control character other than
// newline and tab (a stray escape sequence in a tool output) becomes a space. A lone CR or CRLF becomes LF.
std::optional<std::string> cleanProse(std::string_view value)
{
    if (!projectContextIsValidUtf8(value)) return std::nullopt;
    std::string out;
    out.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto ch = static_cast<unsigned char>(value[index]);
        if (ch == '\r') {
            out.push_back('\n');
            if (index + 1 < value.size() && value[index + 1] == '\n') ++index;
        } else if (ch == '\n' || ch == '\t') {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x20 || ch == 0x7F) {
            out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

bool isBlankText(std::string_view value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}

// The usable text of an untrusted narrative, or nullopt (blank, not UTF-8, or carrying an absolute path /
// forbidden fragment): the caller then writes a marker and counts the omission, it never fails the snapshot.
std::optional<std::string> safeNarrative(
    std::string_view value, const std::vector<std::string>& forbiddenFragments)
{
    auto cleaned = cleanProse(value);
    if (!cleaned || isBlankText(*cleaned)) return std::nullopt;
    if (projectContextContainsForbiddenPath(*cleaned, forbiddenFragments)) return std::nullopt;
    return cleaned;
}

std::vector<ProjectContextItem> renderProject(
    const Project& project,
    const std::vector<std::string>& forbiddenFragments)
{
    // The name is free text like the description: a path-shaped name is omitted, never allowed to fail the
    // whole snapshot (the project id and status stay as the mandatory identity).
    const auto name = safeNarrative(project.name, forbiddenFragments);
    std::ostringstream identity;
    identity << "Project id=" << project.id << '\n'
             << "name=" << (name ? *name : std::string(kOmittedMarker))
             << '\n'
             << "status=" << toString(project.status);
    std::vector<ProjectContextItem> items;
    items.push_back(contextItem(identity.str(), "project", project.id, true));
    if (!project.description.empty()) {
        const auto description = safeNarrative(project.description, forbiddenFragments);
        items.push_back(contextItem(
            "description=" + (description ? *description : std::string(kOmittedMarker)),
            "project_note", "description", false, sha256Hex(project.description)));
    }
    return items;
}

common::Result<std::vector<Run>> loadLineage(
    IRunRepository& runs,
    const ProjectId& projectId,
    const RunId& selectedRunId)
{
    std::vector<Run> lineage;
    std::unordered_set<RunId> seen;
    RunId cursor = selectedRunId;
    while (true) {
        if (!seen.insert(cursor).second) {
            return fail<std::vector<Run>>(
                "project_context.lineage_cycle", "The selected Run lineage contains a cycle");
        }
        if (lineage.size() >= kProjectContextMaximumLineageRuns) {
            return fail<std::vector<Run>>("project_context.lineage_depth_exceeded",
                "The selected Run lineage exceeds the supported depth");
        }
        const auto found = runs.findRun(cursor);
        if (!found.success()) return fail<std::vector<Run>>(found.error()->code, found.error()->message);
        if (!found.value().has_value()) {
            return fail<std::vector<Run>>(
                lineage.empty() ? "project_context.selected_run_not_found"
                                : "project_context.lineage_parent_missing",
                lineage.empty() ? "The selected Run does not exist"
                                : "A parent in the selected Run lineage does not exist");
        }
        if (found.value()->projectId != projectId) {
            return fail<std::vector<Run>>("project_context.lineage_project_mismatch",
                "The selected Run lineage crosses the project boundary");
        }
        lineage.push_back(*found.value());
        if (!lineage.back().parentRunId.has_value()) break;
        cursor = *lineage.back().parentRunId;
    }
    return common::Result<std::vector<Run>>::ok(std::move(lineage));
}

common::Result<std::vector<ProjectContextItem>> renderLineage(
    const std::vector<Run>& lineage,
    IManifestVerificationManualHandoffResolutionReader& manualResolutions)
{
    const auto projected = manualResolutions.resolutionsForRuns(lineage);
    if (!projected.success()) {
        return fail<std::vector<ProjectContextItem>>(
            projected.error()->code, projected.error()->message);
    }
    std::unordered_map<RunId, ManifestVerificationManualHandoffResolution> byRun;
    for (const auto& row : projected.value()) {
        if (std::none_of(lineage.begin(), lineage.end(), [&](const Run& run) { return run.id == row.runId; })) {
            return fail<std::vector<ProjectContextItem>>("project_context.resolution_foreign_run",
                "Manual-resolution projection returned a Run outside the selected lineage");
        }
        if (!byRun.emplace(row.runId, row.resolution).second) {
            return fail<std::vector<ProjectContextItem>>("project_context.resolution_duplicate_run",
                "Manual-resolution projection returned the same Run twice");
        }
    }
    if (byRun.size() != lineage.size()) {
        return fail<std::vector<ProjectContextItem>>("project_context.resolution_missing_run",
            "Manual-resolution projection omitted a Run in the selected lineage");
    }

    std::vector<ProjectContextItem> items;
    items.reserve(lineage.size() * 2);
    for (std::size_t index = 0; index < lineage.size(); ++index) {
        const Run& run = lineage[index];
        std::ostringstream typed;
        typed << (index == 0 ? "selected" : "parent") << " run=" << run.id
              << "; status=" << toString(run.status)
              << "; workflow=" << run.workflowId
              << "; parent=" << (run.parentRunId.has_value() ? *run.parentRunId : "none")
              << "; workspace=" << (run.workspacePath.has_value() ? "frozen_run_workspace" : "project_root");
        // Only the selected Run's identity is mandatory. Ancestors are ordinary items, nearest first, so a long
        // chain is trimmed from its oldest end instead of failing the whole snapshot.
        const bool selected = index == 0;
        items.push_back(contextItem(typed.str(), "run", run.id, selected));

        const auto resolution = byRun.at(run.id);
        items.push_back(contextItem(
            "run=" + run.id + "; manualResolution=" + toString(resolution),
            "manual_resolution", run.id, selected));
    }
    return common::Result<std::vector<ProjectContextItem>>::ok(std::move(items));
}

std::vector<ProjectContextItem> renderWorkspace(const WorkspaceBaselineSnapshot& baseline)
{
    // Per top-level directory counts live in the mandatory identity item: the catalog below is trimmed from
    // its alphabetical tail, and this summary keeps the omitted directories visible.
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> directories;
    for (const auto& file : baseline.files) {
        const auto slash = file.relativePath.find('/');
        auto& entry = directories[slash == std::string::npos ? std::string(".") : file.relativePath.substr(0, slash)];
        ++entry.first;
        entry.second += file.byteSize;
    }
    std::ostringstream identity;
    identity << "workspaceFingerprint=" << baseline.fingerprint
             << "; schema=" << baseline.schemaVersion
             << "; files=" << baseline.files.size()
             << "; totalBytes=" << baseline.totalBytes;
    if (baseline.repositoryRevisionAvailable && baseline.repositoryRevision.has_value()) {
        identity << "; repositoryRevision=" << *baseline.repositoryRevision;
    }
    std::size_t listed = 0;
    for (const auto& [name, entry] : directories) {
        if (listed == kProjectContextMaximumSummaryDirectories) break;
        identity << (listed == 0 ? "\ntop-level directories:\n" : "\n")
                 << name << "; files=" << entry.first << "; bytes=" << entry.second;
        ++listed;
    }
    if (directories.size() > listed) identity << "\ndirectoriesOmitted=" << (directories.size() - listed);
    std::vector<ProjectContextItem> items;
    items.reserve(baseline.files.size() + 1);
    items.push_back(contextItem(identity.str(), "workspace", "current", true, baseline.fingerprint));
    // The file hash is NOT repeated in the text (about a third of the tokens of a row): it is the row's
    // source digest, so the snapshot fingerprint still covers every file's content.
    for (const auto& file : baseline.files) {
        std::ostringstream row;
        row << file.relativePath << "; bytes=" << file.byteSize;
        items.push_back(contextItem(
            row.str(), "workspace_file", file.relativePath, false, file.sha256));
    }
    return items;
}

std::vector<ProjectContextItem> renderLedgerEntry(
    const ManifestWorkLedgerEntry& entry,
    std::vector<ManifestWorkLedgerReviewTransition> transitions,
    const std::vector<std::string>& forbiddenFragments)
{
    std::vector<ProjectContextItem> items;
    std::ostringstream identity;
    identity << "run=" << entry.runId
             << "; terminalStatus=" << toString(entry.terminalStatus)
             << "; disposition=" << toString(entry.disposition)
             << "; baselineFingerprint=" << entry.baselineFingerprint
             << "; manifestSchema=" << (entry.manifestSchemaVersion.empty() ? "none" : entry.manifestSchemaVersion)
             << "; workspaceScopeSchema="
             << (entry.workspaceScopeSchemaVersion.empty() ? "none" : entry.workspaceScopeSchemaVersion)
             << "; recordedAtMs=" << epochMilliseconds(entry.createdAt);
    if (!entry.planningInputFingerprint.empty()) {
        identity << "; planningInputFingerprint=" << entry.planningInputFingerprint;
    }
    if (!entry.planningFailureSignature.empty()) {
        identity << "; planningFailureSignature=" << entry.planningFailureSignature;
    }
    items.push_back(contextItem(identity.str(), "ledger_entry", entry.runId));

    if (!entry.taskDispositions.empty()) {
        std::ostringstream tasks;
        tasks << "run=" << entry.runId << "; tasks=";
        for (std::size_t index = 0; index < entry.taskDispositions.size(); ++index) {
            const auto& task = entry.taskDispositions[index];
            if (index != 0) tasks << ',';
            tasks << task.taskId << ':' << task.status;
            if (task.changeSetId.has_value()) tasks << ":proposal=" << *task.changeSetId;
        }
        items.push_back(contextItem(tasks.str(), "ledger_tasks", entry.runId));
    }

    if (!entry.proposalLineage.empty() || !entry.proposalIds.empty()) {
        std::ostringstream proposals;
        proposals << "run=" << entry.runId << "; proposalIds=" << join(entry.proposalIds);
        if (!entry.proposalLineage.empty()) {
            proposals << "; lineage=";
            for (std::size_t index = 0; index < entry.proposalLineage.size(); ++index) {
                const auto& proposal = entry.proposalLineage[index];
                if (index != 0) proposals << ',';
                proposals << proposal.changeSetId << ':' << proposal.state;
                if (proposal.supersedesChangeSetId.has_value()) {
                    proposals << ":supersedes=" << *proposal.supersedesChangeSetId;
                }
            }
        }
        items.push_back(contextItem(proposals.str(), "ledger_proposals", entry.runId));
    }

    if (!entry.affectedRelativePaths.empty()) {
        items.push_back(contextItem(
            "run=" + entry.runId + "; affectedRelativePaths=" + join(entry.affectedRelativePaths),
            "ledger_paths", entry.runId));
    }

    if (entry.verification.has_value()) {
        const auto& verification = *entry.verification;
        std::ostringstream result;
        result << "run=" << entry.runId
               << "; verificationSchema=" << verification.schemaVersion
               << "; proposalSafety=" << toString(verification.proposalSafetyStatus)
               << "; buildLintTest=" << toString(verification.buildLintTestStatus)
               << "; acceptance=" << toString(verification.acceptanceStatus)
               << "; overlayFingerprint=" << verification.overlayFingerprint;
        items.push_back(contextItem(result.str(), "ledger_verification", entry.runId));
    }

    std::ostringstream narrative;
    std::uint32_t omittedNarrativeFields = 0;
    const auto appendNarrative = [&](std::string_view name, const std::string& value) {
        if (value.empty()) return;
        const auto safe = safeNarrative(value, forbiddenFragments);
        if (!safe) {
            ++omittedNarrativeFields;
            return;
        }
        if (narrative.tellp() > 0) narrative << '\n';
        narrative << name << '=' << *safe;
    };
    appendNarrative("terminalReason", entry.terminalReason);
    appendNarrative("verificationSummary", entry.verificationSummary);
    appendNarrative("remainingWork", entry.remainingWorkNote);
    appendNarrative("workspaceScopeSummary", entry.workspaceScopeSummary);
    if (omittedNarrativeFields > 0) {
        if (narrative.tellp() > 0) narrative << '\n';
        narrative << "omittedNarrativeFields=" << omittedNarrativeFields;
    }
    if (narrative.tellp() > 0) {
        items.push_back(contextItem(
            narrative.str(), "ledger_narrative", entry.runId));
    }

    std::sort(transitions.begin(), transitions.end(), [](const auto& left, const auto& right) {
        if (left.occurredAt != right.occurredAt) return left.occurredAt < right.occurredAt;
        return left.id < right.id;
    });
    for (const auto& transition : transitions) {
        std::ostringstream row;
        row << "run=" << entry.runId
            << "; transition=" << transition.id
            << "; changeSet=" << transition.changeSetId
            << "; kind=" << toString(transition.kind)
            << "; occurredAtMs=" << epochMilliseconds(transition.occurredAt);
        std::uint32_t omitted = 0;
        if (transition.actor.has_value()) {
            if (const auto safe = safeNarrative(*transition.actor, forbiddenFragments)) row << "; actor=" << *safe;
            else ++omitted;
        }
        if (transition.reason.has_value()) {
            if (const auto safe = safeNarrative(*transition.reason, forbiddenFragments)) row << "; reason=" << *safe;
            else ++omitted;
        }
        if (transition.postActionWorkspaceFingerprint.has_value()) {
            row << "; postActionWorkspaceFingerprint=" << *transition.postActionWorkspaceFingerprint;
        }
        if (omitted > 0) row << "; omittedNarrativeFields=" << omitted;
        items.push_back(contextItem(row.str(), "ledger_transition", transition.id));
    }
    return items;
}

common::Result<std::vector<ProjectContextItem>> loadLedger(
    IManifestWorkLedgerRepository& workLedger,
    const ProjectId& projectId,
    const std::vector<Run>& lineage,
    const std::vector<std::string>& forbiddenFragments)
{
    std::vector<ManifestWorkLedgerEntry> entries;
    std::unordered_set<RunId> seen;
    for (const auto& run : lineage) {
        const auto found = workLedger.findEntry(run.id);
        if (!found.success()) {
            return fail<std::vector<ProjectContextItem>>(found.error()->code, found.error()->message);
        }
        if (found.value().has_value()) {
            if (found.value()->runId != run.id || found.value()->projectId != projectId) {
                return fail<std::vector<ProjectContextItem>>("project_context.ledger_project_mismatch",
                    "A selected-lineage Work Ledger entry has inconsistent identity");
            }
            entries.push_back(*found.value());
            seen.insert(run.id);
        }
    }

    const auto recent = workLedger.listRecentForProject(projectId, kProjectContextRecentLedgerLimit);
    if (!recent.success()) {
        return fail<std::vector<ProjectContextItem>>(recent.error()->code, recent.error()->message);
    }
    auto recentEntries = recent.value();
    std::sort(recentEntries.begin(), recentEntries.end(), [](const auto& left, const auto& right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.runId > right.runId;
    });
    for (const auto& entry : recentEntries) {
        if (entry.projectId != projectId) {
            return fail<std::vector<ProjectContextItem>>("project_context.ledger_project_mismatch",
                "The recent Work Ledger query returned another project's entry");
        }
        if (!seen.insert(entry.runId).second) continue;
        entries.push_back(entry);
    }

    std::vector<ProjectContextItem> items;
    for (const auto& entry : entries) {
        const auto transitions = workLedger.listTransitionsForRun(entry.runId);
        if (!transitions.success()) {
            return fail<std::vector<ProjectContextItem>>(
                transitions.error()->code, transitions.error()->message);
        }
        for (const auto& transition : transitions.value()) {
            if (transition.runId != entry.runId) {
                return fail<std::vector<ProjectContextItem>>("project_context.ledger_transition_mismatch",
                    "A Work Ledger transition belongs to another Run");
            }
        }
        auto rendered = renderLedgerEntry(entry, transitions.value(), forbiddenFragments);
        items.insert(items.end(), std::make_move_iterator(rendered.begin()),
            std::make_move_iterator(rendered.end()));
    }
    return common::Result<std::vector<ProjectContextItem>>::ok(std::move(items));
}

// PC-1C. A segment is a deterministic EXCERPT: path-bearing lines are dropped and counted, the rest is clipped
// to kProjectContextMemoryExcerptBytes on a UTF-8 boundary with an explicit "excerpt=shown of total bytes"
// note. The row's source digest is the sha256 of the FULL original summary, so the fingerprint still covers
// the whole segment.
ProjectContextItem renderMemorySegment(
    const ProjectMemorySegment& segment, const std::vector<std::string>& forbiddenFragments)
{
    std::ostringstream header;
    header << "segment=" << segment.id << "; run=" << (segment.runId.empty() ? std::string("none") : segment.runId)
           << "; recordedAtMs=" << epochMilliseconds(segment.createdAt)
           << "; summaryTokens=" << segment.summaryTokenCount;
    std::string body;
    if (const auto cleaned = cleanProse(segment.summary)) {
        std::string kept;
        std::size_t omittedLines = 0;
        for (std::size_t start = 0; start <= cleaned->size();) {
            const auto end = cleaned->find('\n', start);
            const std::string_view line = std::string_view(*cleaned).substr(
                start, (end == std::string::npos ? cleaned->size() : end) - start);
            if (projectContextContainsForbiddenPath(line, forbiddenFragments)) {
                ++omittedLines;
            } else {
                kept.append(line);
                kept.push_back('\n');
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        while (!kept.empty() && std::isspace(static_cast<unsigned char>(kept.back())) != 0) kept.pop_back();
        const std::size_t total = kept.size();
        const bool clipped = total > kProjectContextMemoryExcerptBytes;
        if (clipped) {
            std::size_t cut = kProjectContextMemoryExcerptBytes;
            while (cut > 0 && (static_cast<unsigned char>(kept[cut]) & 0xC0) == 0x80) --cut;
            kept.resize(cut);
            while (!kept.empty() && std::isspace(static_cast<unsigned char>(kept.back())) != 0) kept.pop_back();
        }
        if (omittedLines > 0) header << "; omittedLines=" << omittedLines;
        if (clipped) header << "; excerpt=" << kept.size() << " of " << total << " bytes";
        if (isBlankText(kept)) body = "[no usable text]";
        else if (projectContextContainsForbiddenPath(kept, forbiddenFragments)) body = kOmittedMarker;
        else body = kept;
    } else {
        body = "[omitted: not valid text]";
    }
    return contextItem(header.str() + "\n" + body, "memory_segment", segment.id, false, sha256Hex(segment.summary));
}

common::Result<std::vector<ProjectContextItem>> loadProjectMemory(
    IProjectMemoryRepository& memory,
    const ProjectId& projectId,
    const std::vector<std::string>& forbiddenFragments)
{
    // Bounded read: the repository returns at most the newest N segments plus the project's total, so a
    // long-lived project is never loaded in full just to keep 32 excerpts.
    const auto listed = memory.listRecentMemorySegments(projectId, kProjectContextRecentMemoryLimit);
    if (!listed.success()) {
        return fail<std::vector<ProjectContextItem>>(listed.error()->code, listed.error()->message);
    }
    auto segments = listed.value().newest;
    const std::uint64_t totalSegments = std::max<std::uint64_t>(listed.value().totalCount, segments.size());
    for (const auto& segment : segments) {
        if (segment.projectId != projectId) {
            return fail<std::vector<ProjectContextItem>>("project_context.memory_project_mismatch",
                "The Project Memory query returned another project's segment");
        }
    }
    std::sort(segments.begin(), segments.end(), [](const auto& left, const auto& right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.id > right.id;
    });
    // Defensive: whatever a repository returns, only the newest N are shown, in the fixed order above.
    const std::size_t listedCount = std::min(segments.size(), kProjectContextRecentMemoryLimit);
    std::ostringstream status;
    status << "projectMemorySegments=" << totalSegments;
    if (totalSegments == 0) {
        status << " (none recorded)";
    } else {
        status << "; newestListed=" << listedCount << "; excerptBytesPerSegment=" << kProjectContextMemoryExcerptBytes;
    }
    std::vector<ProjectContextItem> items;
    items.push_back(contextItem(status.str(), "memory_status", "project_memory"));
    for (std::size_t index = 0; index < listedCount; ++index) {
        items.push_back(renderMemorySegment(segments[index], forbiddenFragments));
    }
    return common::Result<std::vector<ProjectContextItem>>::ok(std::move(items));
}

} // namespace

ProjectContextSnapshotService::ProjectContextSnapshotService(
    IProjectRepository& projects,
    IRunRepository& runs,
    IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationManualHandoffResolutionReader& manualResolutions,
    IProjectMemoryRepository& projectMemory)
    : projects_(projects)
    , runs_(runs)
    , workLedger_(workLedger)
    , manualResolutions_(manualResolutions)
    , projectMemory_(projectMemory)
{
}

common::Result<ProjectContextSnapshot> ProjectContextSnapshotService::build(
    const ProjectContextSnapshotRequest& request) const
{
    if (request.projectId.empty()) {
        return fail<ProjectContextSnapshot>(
            "project_context.invalid_request", "Project id is required");
    }
    const auto projectResult = projects_.findById(request.projectId);
    if (!projectResult.success()) {
        return fail<ProjectContextSnapshot>(
            projectResult.error()->code, projectResult.error()->message);
    }
    if (!projectResult.value().has_value()) {
        return fail<ProjectContextSnapshot>(
            "project_context.project_not_found", "Project not found");
    }
    const Project& project = *projectResult.value();
    if (project.status != ProjectStatus::Active) {
        return fail<ProjectContextSnapshot>(
            "project_context.project_not_active", "Only an active project has current context");
    }

    std::vector<Run> lineage;
    if (request.selectedRunId.has_value()) {
        if (request.selectedRunId->empty()) {
            return fail<ProjectContextSnapshot>(
                "project_context.invalid_request", "Selected Run id cannot be empty");
        }
        auto loaded = loadLineage(runs_, project.id, *request.selectedRunId);
        if (!loaded.success()) {
            return fail<ProjectContextSnapshot>(loaded.error()->code, loaded.error()->message);
        }
        lineage = std::move(loaded.value());
    }

    const std::filesystem::path workspaceRoot = !lineage.empty() && lineage.front().workspacePath.has_value()
        ? *lineage.front().workspacePath : project.rootPath;
    std::vector<std::string> forbiddenFragments = projectContextPathFragments(project.rootPath);
    appendUnique(forbiddenFragments, projectContextPathFragments(workspaceRoot));

    const WorkspaceReadPolicy policy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto baseline = WorkspaceBaselineProvenance::capture(policy);
    if (!baseline.success()) {
        return fail<ProjectContextSnapshot>(baseline.error()->code, baseline.error()->message);
    }

    ProjectContextAssemblyInput assembly;
    assembly.projectId = project.id;
    assembly.selectedRunId = request.selectedRunId;
    assembly.workspaceFingerprint = baseline.value().fingerprint;
    assembly.forbiddenFragments = forbiddenFragments;
    appendSection(assembly, ProjectContextCategory::ProjectMetadata,
        renderProject(project, forbiddenFragments));
    if (!lineage.empty()) {
        auto renderedLineage = renderLineage(lineage, manualResolutions_);
        if (!renderedLineage.success()) {
            return fail<ProjectContextSnapshot>(
                renderedLineage.error()->code, renderedLineage.error()->message);
        }
        appendSection(assembly, ProjectContextCategory::SelectedLineage,
            std::move(renderedLineage.value()));
    }
    auto ledger = loadLedger(workLedger_, project.id, lineage, forbiddenFragments);
    if (!ledger.success()) {
        return fail<ProjectContextSnapshot>(ledger.error()->code, ledger.error()->message);
    }
    appendSection(assembly, ProjectContextCategory::WorkLedger, std::move(ledger.value()));
    appendSection(assembly, ProjectContextCategory::WorkspaceCatalog,
        renderWorkspace(baseline.value()));
    if (request.includeDerivedProjectMemory) {
        auto memory = loadProjectMemory(projectMemory_, project.id, forbiddenFragments);
        if (!memory.success()) {
            return fail<ProjectContextSnapshot>(memory.error()->code, memory.error()->message);
        }
        appendSection(assembly, ProjectContextCategory::ProjectMemory, std::move(memory.value()));
    }
    return assembleProjectContextSnapshot(assembly);
}

} // namespace ai::application
