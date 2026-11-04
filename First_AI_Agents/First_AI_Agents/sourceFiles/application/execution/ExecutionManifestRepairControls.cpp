#include "ai/application/execution/ExecutionManifestRepairControls.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {

namespace {

std::string describeNumber(std::uint64_t value)
{
    return std::to_string(value);
}

std::string describeNumber(const std::optional<std::uint64_t>& value)
{
    return value.has_value() ? std::to_string(*value) : std::string("null");
}

bool hasFindingCode(
    const std::vector<ExecutionManifestFinding>& findings, std::string_view code)
{
    return std::any_of(findings.begin(), findings.end(),
        [&](const auto& finding) { return finding.code == code; });
}

template <typename Value>
void restoreField(
    std::string_view field,
    std::string_view ownFindingCode,
    const Value& primaryValue,
    Value& repairValue,
    const std::vector<ExecutionManifestFinding>& primaryFindings,
    std::vector<std::string>& audit)
{
    if (primaryValue == repairValue || hasFindingCode(primaryFindings, ownFindingCode)) return;
    audit.push_back("Planner repair changed " + std::string(field) + " from "
        + describeNumber(primaryValue) + " (primary) to " + describeNumber(repairValue)
        + "; restored the primary value " + describeNumber(primaryValue));
    repairValue = primaryValue;
}

// Same comparable form as the Planner's own normalizeComparablePath (separator- and case-
// insensitive); kept private here because that helper lives in the Planner's anonymous namespace.
// path::generic_string() converts through the Windows ANSI code page and THROWS for a character it lacks
// (a perfectly valid docs/Ж表.md), so the key and the audit text are built from the UTF-8 form instead.
std::string comparableKey(const std::filesystem::path& path)
{
    std::string key = detail::genericUtf8(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    return detail::asciiLower(std::move(key));
}

// One task-local note list (excludedPaths or deferredPaths) versus the SAME task's active reads. The
// two public passes below differ only in which list they clean and how the audit line names it.
std::vector<std::string> removeNotesOfPathsTheTaskReads(
    ExecutionManifest& manifest,
    std::vector<ManifestExcludedOrDeferredPath> ExecutionManifestTask::* notes,
    std::string_view noteListName)
{
    std::vector<std::string> audit;
    for (auto& task : manifest.tasks) {
        auto& entries = task.*notes;
        if (entries.empty()) continue;

        // required_read > reference_only: the first emplace wins, so insert required reads first.
        std::unordered_map<std::string, std::pair<std::string, std::string>> readByKey;
        for (const auto& path : task.requiredReadPaths)
            readByKey.emplace(comparableKey(path),
                std::pair<std::string, std::string> {detail::genericUtf8(path), "required_read"});
        for (const auto& path : task.referencePaths)
            readByKey.emplace(comparableKey(path),
                std::pair<std::string, std::string> {detail::genericUtf8(path), "reference_only"});

        // A written path never loses its note here: that contradiction stays a hard validator
        // error whatever order the passes run in.
        std::unordered_set<std::string> writeKeys;
        for (const auto& path : task.ownedPaths) writeKeys.insert(comparableKey(path));
        for (const auto& path : task.plannedCreatePaths) writeKeys.insert(comparableKey(path));

        std::unordered_set<std::string> reported;
        std::erase_if(entries, [&](const ManifestExcludedOrDeferredPath& entry) {
            const auto key = comparableKey(entry.path);
            const auto read = readByKey.find(key);
            if (read == readByKey.end() || writeKeys.contains(key)) return false;
            if (reported.insert(key).second) {
                audit.push_back("task '" + task.id + "': '" + read->second.first + "' is read as "
                    + read->second.second + ", so its " + std::string(noteListName)
                    + " entry was removed");
            }
            return true;
        });
    }
    return audit;
}

} // namespace

std::vector<std::string> canonicalizeExcludedReadConflicts(ExecutionManifest& manifest)
{
    return removeNotesOfPathsTheTaskReads(manifest, &ExecutionManifestTask::excludedPaths, "excludedPaths");
}

std::vector<std::string> canonicalizeDeferredReadConflicts(ExecutionManifest& manifest)
{
    return removeNotesOfPathsTheTaskReads(manifest, &ExecutionManifestTask::deferredPaths, "deferredPaths");
}

bool isGraphWideRepair(const std::vector<ExecutionManifestFinding>& primaryFindings)
{
    return std::any_of(primaryFindings.begin(), primaryFindings.end(), [](const auto& finding) {
        return !finding.taskId.has_value()
            || finding.code == "manifest.dependency_cycle"
            || finding.code == "manifest.duplicate_task_id"
            || finding.code == "manifest.ownership_overlap";
    });
}

std::vector<std::string> restoreNonAuthoritativeRepairControls(
    const ExecutionManifestDraft& primary,
    ExecutionManifestDraft& repair,
    const std::vector<ExecutionManifestFinding>& primaryFindings)
{
    std::vector<std::string> audit;
    if (primaryFindings.empty() || isGraphWideRepair(primaryFindings)) return audit;

    restoreField("requestedMaxParallelism", "manifest.parallelism_exceeded",
        primary.requestedMaxParallelism, repair.requestedMaxParallelism, primaryFindings, audit);
    restoreField("requestedTokenBudget", "manifest.token_budget_exceeded",
        primary.requestedTokenBudget, repair.requestedTokenBudget, primaryFindings, audit);
    restoreField("requestedCostMicros", "manifest.cost_budget_exceeded",
        primary.requestedCostMicros, repair.requestedCostMicros, primaryFindings, audit);
    return audit;
}

} // namespace ai::application
