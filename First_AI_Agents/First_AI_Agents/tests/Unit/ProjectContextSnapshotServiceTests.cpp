#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/application/context/ProjectContextSnapshotService.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ai::application;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class TemporaryWorkspace final {
public:
    explicit TemporaryWorkspace(std::string name)
        : path_(std::filesystem::temp_directory_path() / ("ai-orchestrator-pc1b-" + std::move(name)))
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (!std::filesystem::create_directories(path_, error) || error) {
            throw std::runtime_error("Could not create the PC-1B test workspace");
        }
    }

    ~TemporaryWorkspace()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

    void write(const std::filesystem::path& relativePath, std::string content) const
    {
        const auto absolute = path_ / relativePath;
        std::error_code error;
        std::filesystem::create_directories(absolute.parent_path(), error);
        std::ofstream stream(absolute, std::ios::binary | std::ios::trunc);
        stream << content;
        if (!stream) throw std::runtime_error("Could not write the PC-1B test fixture");
    }

private:
    std::filesystem::path path_;
};

class ScriptedLedger final : public IManifestWorkLedgerRepository {
public:
    ai::common::Result<void> insertEntry(ManifestWorkLedgerEntry) override
    {
        ++writeCalls;
        return ai::common::Result<void>::failure({"test.write_forbidden", "Unexpected write"});
    }

    ai::common::Result<std::optional<ManifestWorkLedgerEntry>> findEntry(const RunId& runId) const override
    {
        if (readError.has_value()) {
            return ai::common::Result<std::optional<ManifestWorkLedgerEntry>>::failure(*readError);
        }
        const auto found = byRun.find(runId);
        return ai::common::Result<std::optional<ManifestWorkLedgerEntry>>::ok(
            found == byRun.end() ? std::nullopt : std::optional(found->second));
    }

    ai::common::Result<std::vector<ManifestWorkLedgerEntry>> listRecentForProject(
        const ProjectId&, std::size_t limit) const override
    {
        if (readError.has_value()) {
            return ai::common::Result<std::vector<ManifestWorkLedgerEntry>>::failure(*readError);
        }
        auto result = recent;
        if (result.size() > limit) result.resize(limit);
        return ai::common::Result<std::vector<ManifestWorkLedgerEntry>>::ok(std::move(result));
    }

    ai::common::Result<void> appendTransition(ManifestWorkLedgerReviewTransition) override
    {
        ++writeCalls;
        return ai::common::Result<void>::failure({"test.write_forbidden", "Unexpected write"});
    }

    ai::common::Result<std::vector<ManifestWorkLedgerReviewTransition>> listTransitionsForRun(
        const RunId& runId) const override
    {
        if (readError.has_value()) {
            return ai::common::Result<std::vector<ManifestWorkLedgerReviewTransition>>::failure(*readError);
        }
        const auto found = transitions.find(runId);
        return ai::common::Result<std::vector<ManifestWorkLedgerReviewTransition>>::ok(
            found == transitions.end() ? std::vector<ManifestWorkLedgerReviewTransition> {} : found->second);
    }

    std::map<RunId, ManifestWorkLedgerEntry> byRun;
    std::vector<ManifestWorkLedgerEntry> recent;
    std::map<RunId, std::vector<ManifestWorkLedgerReviewTransition>> transitions;
    std::optional<ai::common::Error> readError;
    mutable std::size_t writeCalls {0};
};

class ScriptedResolutions final : public IManifestVerificationManualHandoffResolutionReader {
public:
    ai::common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>> resolutionsForRuns(
        const std::vector<Run>& runs) const override
    {
        ++calls;
        if (error.has_value()) {
            return ai::common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>::failure(*error);
        }
        if (rows.has_value()) {
            return ai::common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>::ok(*rows);
        }
        std::vector<ManifestVerificationManualHandoffRunResolution> result;
        for (const auto& run : runs) {
            const auto found = values.find(run.id);
            result.push_back({run.id, found == values.end()
                    ? ManifestVerificationManualHandoffResolution::NotApplicable : found->second});
        }
        return ai::common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>::ok(
            std::move(result));
    }

    std::map<RunId, ManifestVerificationManualHandoffResolution> values;
    std::optional<std::vector<ManifestVerificationManualHandoffRunResolution>> rows;
    std::optional<ai::common::Error> error;
    mutable std::size_t calls {0};
};

class ScriptedMemory final : public IProjectMemoryRepository {
public:
    ai::common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment) override
    {
        ++writeCalls;
        return ai::common::Result<ProjectMemorySegment>::failure({"test.write_forbidden", "Unexpected write"});
    }

    ai::common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId&) const override
    {
        ++listCalls;
        if (readError.has_value()) {
            return ai::common::Result<std::vector<ProjectMemorySegment>>::failure(*readError);
        }
        return ai::common::Result<std::vector<ProjectMemorySegment>>::ok(segments);
    }

    ai::common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(
        const ProjectMemorySegmentId&) const override
    {
        return ai::common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }

    ai::common::Result<void> removeMemorySegments(const ProjectId&) override
    {
        ++writeCalls;
        return ai::common::Result<void>::failure({"test.write_forbidden", "Unexpected write"});
    }

    std::vector<ProjectMemorySegment> segments;
    std::optional<ai::common::Error> readError;
    mutable std::size_t listCalls {0};
    mutable std::size_t writeCalls {0};
};

Project project(std::string id, const std::filesystem::path& root)
{
    Project value;
    value.id = std::move(id);
    value.name = "Context project";
    value.description = "Curated intent";
    value.rootPath = root;
    value.status = ProjectStatus::Active;
    return value;
}

Run run(std::string id, std::string projectId, std::optional<RunId> parent = std::nullopt)
{
    Run value;
    value.id = std::move(id);
    value.projectId = std::move(projectId);
    value.parentRunId = std::move(parent);
    value.title = "Run";
    value.task = "Task";
    value.workflowId = "execution-manifest-v1";
    value.status = RunStatus::Failed;
    return value;
}

ManifestWorkLedgerEntry ledgerEntry(std::string runId, std::string projectId, std::int64_t timeMs)
{
    ManifestWorkLedgerEntry entry;
    entry.runId = std::move(runId);
    entry.projectId = std::move(projectId);
    entry.terminalStatus = RunStatus::Failed;
    entry.disposition = ManifestWorkLedgerDisposition::ManifestReady;
    entry.baselineFingerprint = std::string(64, 'a');
    entry.manifestSchemaVersion = "execution-manifest.v1";
    entry.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(timeMs));
    return entry;
}

ProjectContextSnapshot build(ProjectContextSnapshotService& service,
    ProjectContextSnapshotRequest request)
{
    const auto result = service.build(request);
    require(result.success(), result.success() ? "" : result.error()->message.c_str());
    return result.value();
}

std::string failureCode(ProjectContextSnapshotService& service,
    ProjectContextSnapshotRequest request)
{
    const auto result = service.build(request);
    require(!result.success(), "The PC-1B build was expected to fail");
    return result.error()->code;
}

const ProjectContextSection* section(
    const ProjectContextSnapshot& snapshot, ProjectContextCategory category)
{
    for (const auto& value : snapshot.sections) {
        if (value.category == category) return &value;
    }
    return nullptr;
}

void testBuildUsesSelectedWorkspaceAndLoadsTypedAuthorityWithoutWrites()
{
    TemporaryWorkspace projectRoot("selected-project-root");
    TemporaryWorkspace selectedRoot("selected-run-root");
    projectRoot.write("project-only.txt", "PROJECT ROOT FILE CONTENT MUST NOT LEAK");
    selectedRoot.write("src/run-only.cpp", "RUN WORKSPACE FILE CONTENT MUST NOT LEAK");

    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", projectRoot.path())).success(), "Project seed");
    Run parent = run("parent", "p1");
    parent.status = RunStatus::Completed;
    require(runs.insertRun(parent).success(), "Parent seed");
    Run selected = run("selected", "p1", "parent");
    selected.workspacePath = selectedRoot.path();
    require(runs.insertRun(selected).success(), "Selected seed");
    resolutions.values["selected"] =
        ManifestVerificationManualHandoffResolution::ResolvedManuallyApplied;

    auto selectedLedger = ledgerEntry("selected", "p1", 300);
    selectedLedger.terminalReason = "worker stopped at C:\\private\\candidate";
    selectedLedger.remainingWorkNote = "Finish the bounded follow-up";
    selectedLedger.taskDispositions.push_back({"task-a", "failed", std::nullopt, std::nullopt});
    selectedLedger.proposalIds = {"proposal-a"};
    selectedLedger.affectedRelativePaths = {"src/run-only.cpp"};
    ledger.byRun[selectedLedger.runId] = selectedLedger;
    ledger.recent = {selectedLedger, ledgerEntry("recent-b", "p1", 200),
        ledgerEntry("recent-a", "p1", 200)};
    ManifestWorkLedgerReviewTransition transition;
    transition.id = "transition-1";
    transition.runId = "selected";
    transition.changeSetId = "proposal-a";
    transition.kind = ManifestWorkLedgerTransitionKind::Rejected;
    transition.occurredAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(400));
    transition.reason = "Use the verified alternative";
    ledger.transitions["selected"] = {transition};

    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto first = build(service, {"p1", "selected", false});
    const auto* metadata = section(first, ProjectContextCategory::ProjectMetadata);
    const auto* lineage = section(first, ProjectContextCategory::SelectedLineage);
    const auto* workLedger = section(first, ProjectContextCategory::WorkLedger);
    const auto* catalog = section(first, ProjectContextCategory::WorkspaceCatalog);
    require(metadata && lineage && workLedger && catalog && first.sections.size() == 4,
        "PC-1B must assemble the four authoritative sections");
    require(lineage->text.find("selected run=selected") != std::string::npos
            && lineage->text.find("parent run=parent") != std::string::npos
            && lineage->text.find("resolved_manually_applied") != std::string::npos,
        "Selected lineage and Backend manual resolution must be present");
    require(catalog->text.find("src/run-only.cpp") != std::string::npos
            && catalog->text.find("project-only.txt") == std::string::npos,
        "The selected Run's frozen workspace must win over the project root");
    require(catalog->text.find("RUN WORKSPACE FILE CONTENT MUST NOT LEAK") == std::string::npos,
        "The workspace catalog must contain hashes/metadata, never file content");
    require(workLedger->text.find("Finish the bounded follow-up") != std::string::npos
            && workLedger->text.find("omittedNarrativeFields=1") != std::string::npos
            && workLedger->text.find("C:\\private") == std::string::npos,
        "Safe ledger prose must survive and unsafe prose must be counted, not leaked");
    require(workLedger->text.find("transition=transition-1") != std::string::npos
            && workLedger->text.find("reason=Use the verified alternative") != std::string::npos,
        "Typed review transition and safe reason must be included");
    require(ledger.writeCalls == 0 && resolutions.calls == 1,
        "The service is read-only and asks the resolution authority once");

    std::reverse(ledger.recent.begin(), ledger.recent.end());
    const auto second = build(service, {"p1", "selected", false});
    require(first.fingerprint == second.fingerprint,
        "Repository return order must not change a restart-stable fingerprint");
}

void testProjectRootBuildDoesNotInventLineageOrCallResolutionReader()
{
    TemporaryWorkspace root("project-root-only");
    root.write("README.md", "hello");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    require(!snapshot.selectedRunId.has_value()
            && section(snapshot, ProjectContextCategory::SelectedLineage) == nullptr
            && section(snapshot, ProjectContextCategory::WorkspaceCatalog)->text.find("README.md")
                != std::string::npos,
        "A project-only snapshot must use the project root without synthetic lineage");
    require(resolutions.calls == 0, "No selected lineage means no manual-resolution query");
}

void testLineageIsolationCycleDepthAndMissingParentsFailClosed()
{
    TemporaryWorkspace root("lineage-errors");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project p1 seed");
    require(projects.insert(project("p2", root.path())).success(), "Project p2 seed");

    {
        InMemoryRunRepository runs;
        require(runs.insertRun(run("foreign", "p2")).success(), "Foreign seed");
        ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
        require(failureCode(service, {"p1", "foreign", false})
                == "project_context.lineage_project_mismatch",
            "A selected Run from another project must be rejected");
    }
    {
        InMemoryRunRepository runs;
        require(runs.insertRun(run("foreign-parent", "p2")).success(), "Foreign parent seed");
        require(runs.insertRun(run("child", "p1", "foreign-parent")).success(), "Child seed");
        ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
        require(failureCode(service, {"p1", "child", false})
                == "project_context.lineage_project_mismatch",
            "A parent chain may not cross projects");
    }
    {
        InMemoryRunRepository runs;
        require(runs.insertRun(run("child", "p1", "missing")).success(), "Missing parent seed");
        ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
        require(failureCode(service, {"p1", "child", false})
                == "project_context.lineage_parent_missing",
            "A broken parent chain must fail closed");
    }
    {
        InMemoryRunRepository runs;
        require(runs.insertRun(run("a", "p1", "b")).success(), "Cycle a seed");
        require(runs.insertRun(run("b", "p1", "a")).success(), "Cycle b seed");
        ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
        require(failureCode(service, {"p1", "a", false}) == "project_context.lineage_cycle",
            "A lineage cycle must fail closed");
    }
    {
        InMemoryRunRepository runs;
        for (std::size_t index = 0; index <= kProjectContextMaximumLineageRuns; ++index) {
            const std::string id = "depth-" + std::to_string(index);
            const std::optional<RunId> parent = index == kProjectContextMaximumLineageRuns
                ? std::nullopt : std::optional("depth-" + std::to_string(index + 1));
            require(runs.insertRun(run(id, "p1", parent)).success(), "Depth seed");
        }
        ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
        require(failureCode(service, {"p1", "depth-0", false})
                == "project_context.lineage_depth_exceeded",
            "An over-deep lineage must fail before unbounded reads");
    }
}

void testLedgerAndResolutionIdentityAreValidated()
{
    TemporaryWorkspace root("identity-errors");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    require(runs.insertRun(run("selected", "p1")).success(), "Run seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);

    ledger.recent = {ledgerEntry("foreign-ledger", "p2", 1)};
    require(failureCode(service, {"p1", "selected", false})
            == "project_context.ledger_project_mismatch",
        "A recent query returning foreign-project evidence must fail closed");
    ledger.recent.clear();

    auto entry = ledgerEntry("selected", "p1", 1);
    ledger.byRun["selected"] = entry;
    ManifestWorkLedgerReviewTransition transition;
    transition.id = "t1";
    transition.runId = "another-run";
    transition.changeSetId = "proposal";
    ledger.transitions["selected"] = {transition};
    require(failureCode(service, {"p1", "selected", false})
            == "project_context.ledger_transition_mismatch",
        "A transition returned under another Run must fail closed");
    ledger.transitions.clear();

    resolutions.rows = std::vector<ManifestVerificationManualHandoffRunResolution> {};
    require(failureCode(service, {"p1", "selected", false})
            == "project_context.resolution_missing_run",
        "The manual-resolution authority must answer for every lineage Run");
    resolutions.rows = std::vector<ManifestVerificationManualHandoffRunResolution> {
        {"foreign", ManifestVerificationManualHandoffResolution::Unresolved}};
    require(failureCode(service, {"p1", "selected", false})
            == "project_context.resolution_foreign_run",
        "A foreign resolution row must be rejected");
}

void testProjectMemoryRequestAndAuthoritativeReadFailuresAreExplicit()
{
    TemporaryWorkspace root("explicit-errors");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    memory.readError = ai::common::Error {"storage.memory_unavailable", "Memory unavailable"};
    require(failureCode(service, {"p1", std::nullopt, true}) == "storage.memory_unavailable",
        "A requested Project Memory that cannot be read must fail the build");
    memory.readError.reset();
    ledger.readError = ai::common::Error {"storage.unavailable", "Ledger unavailable"};
    require(failureCode(service, {"p1", std::nullopt, false}) == "storage.unavailable",
        "An authoritative repository failure must propagate");
}

void testUnsafeProjectDescriptionIsOmittedWithItsDigest()
{
    TemporaryWorkspace root("description-guard");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    Project value = project("p1", root.path());
    value.description = "Open C:\\private\\notes.txt";
    require(projects.insert(value).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    const auto* metadata = section(snapshot, ProjectContextCategory::ProjectMetadata);
    require(metadata != nullptr
            && metadata->text.find("omitted by context safety policy") != std::string::npos
            && metadata->text.find("C:\\private") == std::string::npos,
        "Curated prose may not leak an absolute path");
    require(metadata->sources.size() == 2 && metadata->sources[1].digest.size() == 64,
        "An omitted curated note still carries a stable content digest");
}

std::string uuidLike(std::size_t n)
{
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08zx-1111-4222-8333-%012zx", n, n * 7919);
    return buffer;
}

std::size_t positionOf(const std::string& text, const std::string& needle)
{
    const auto position = text.find(needle);
    require(position != std::string::npos, ("Missing in the rendered section: " + needle).c_str());
    return position;
}

// Review: the budget cuts the TAIL, so newest-first ledger order and chronological transitions decide what
// survives; mutation showed neither was pinned.
void testLedgerIsNewestFirstAndTransitionsAreChronological()
{
    TemporaryWorkspace root("ledger-order");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    ledger.recent = {ledgerEntry("r100", "p1", 100), ledgerEntry("r300", "p1", 300), ledgerEntry("r200", "p1", 200)};
    ManifestWorkLedgerReviewTransition late;
    late.id = "t-late";
    late.runId = "r300";
    late.changeSetId = "proposal";
    late.kind = ManifestWorkLedgerTransitionKind::Applied;
    late.occurredAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(500));
    ManifestWorkLedgerReviewTransition early = late;
    early.id = "t-early";
    early.kind = ManifestWorkLedgerTransitionKind::Approved;
    early.occurredAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(400));
    ledger.transitions["r300"] = {late, early};

    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    const std::string& text = section(snapshot, ProjectContextCategory::WorkLedger)->text;
    require(positionOf(text, "run=r300; terminalStatus") < positionOf(text, "run=r200; terminalStatus")
            && positionOf(text, "run=r200; terminalStatus") < positionOf(text, "run=r100; terminalStatus"),
        "The newest Work Ledger entry must come first, because trimming drops the tail");
    require(positionOf(text, "transition=t-early") < positionOf(text, "transition=t-late"),
        "Review transitions must be chronological");
}

// Review: the identity of a selected-lineage ledger entry was not checked by any test.
void testSelectedLineageLedgerEntryIdentityIsValidated()
{
    TemporaryWorkspace root("lineage-ledger-identity");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    require(runs.insertRun(run("selected", "p1")).success(), "Run seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);

    ledger.byRun["selected"] = ledgerEntry("selected", "p2", 1);
    require(failureCode(service, {"p1", "selected", false}) == "project_context.ledger_project_mismatch",
        "A lineage ledger entry of another project must be rejected");
    ledger.byRun["selected"] = ledgerEntry("some-other-run", "p1", 1);
    require(failureCode(service, {"p1", "selected", false}) == "project_context.ledger_project_mismatch",
        "A lineage ledger entry stored under another Run id must be rejected");
}

void testUnknownInactiveAndMalformedRequestsFailClosed()
{
    TemporaryWorkspace root("request-errors");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    Project archived = project("archived", root.path());
    archived.status = ProjectStatus::Archived;
    Project unavailable = project("unavailable", root.path());
    unavailable.status = ProjectStatus::Unavailable;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    require(projects.insert(archived).success(), "Archived seed");
    require(projects.insert(unavailable).success(), "Unavailable seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    require(failureCode(service, {"nope", std::nullopt, false}) == "project_context.project_not_found", "Unknown project");
    require(failureCode(service, {"archived", std::nullopt, false}) == "project_context.project_not_active", "Archived project");
    require(failureCode(service, {"unavailable", std::nullopt, false}) == "project_context.project_not_active", "Unavailable project");
    require(failureCode(service, {"", std::nullopt, false}) == "project_context.invalid_request", "Empty project id");
    require(failureCode(service, {"p1", std::string(), false}) == "project_context.invalid_request", "Empty selected Run id");
    require(failureCode(service, {"p1", "missing-run", false}) == "project_context.selected_run_not_found",
        "A selected Run that does not exist");
}

// Review: every lineage row was mandatory, so a long retry chain failed the whole snapshot with a generic
// mandatory_exceeds_budget at about 59 ancestors. Only the selected Run is identity; ancestors are trimmed.
void testLongLineageKeepsTheSelectedRunAndTrimsTheOldestAncestors()
{
    TemporaryWorkspace root("long-lineage");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    for (std::size_t index = 0; index < kProjectContextMaximumLineageRuns; ++index) {
        Run value = run(uuidLike(index), "p1",
            index + 1 < kProjectContextMaximumLineageRuns ? std::optional<RunId>(uuidLike(index + 1)) : std::nullopt);
        require(runs.insertRun(value).success(), "Lineage seed");
    }
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", uuidLike(0), false});
    const auto* lineage = section(snapshot, ProjectContextCategory::SelectedLineage);
    require(lineage != nullptr && lineage->truncated && lineage->omittedItems > 0 && lineage->includedItems >= 2,
        "A maximum-depth lineage must be trimmed, not rejected");
    require(lineage->text.find("selected run=" + uuidLike(0)) != std::string::npos
            && lineage->text.find("run=" + uuidLike(0) + "; manualResolution=") != std::string::npos,
        "The selected Run's identity and resolution must survive the trimming");
    // The nearest included ancestor still names its parent in "parent=...", so look for the rows themselves.
    require(lineage->text.find("run=" + uuidLike(kProjectContextMaximumLineageRuns - 1) + ";") == std::string::npos,
        "The oldest ancestor's own rows are the first to go");
    require(lineage->estimatedTokens <= 5'000, "The lineage stays inside its limit");
}

// Review: the project name was not sanitized like the description, so a path-like name failed everything.
void testPathLikeProjectNameIsOmittedNotFatal()
{
    TemporaryWorkspace root("path-like-name");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    Project value = project("p1", root.path());
    value.name = "C:\\work\\app";
    require(projects.insert(value).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    const auto* metadata = section(snapshot, ProjectContextCategory::ProjectMetadata);
    require(metadata != nullptr && metadata->text.find("Project id=p1") != std::string::npos
            && metadata->text.find("name=[omitted by context safety policy]") != std::string::npos
            && metadata->text.find("C:\\work") == std::string::npos,
        "A path-like project name is omitted while the project identity stays");
}

// Review: each catalog row repeated its 64-character sha256 (about a third of the row), so only about 250 files
// fitted and the alphabetical tail vanished without a trace. The hash stays as the row digest, and the mandatory
// identity item carries a per-directory summary.
void testLargeCatalogHasNoHashesInTextAndKeepsADirectorySummary()
{
    TemporaryWorkspace root("large-catalog");
    for (int index = 0; index < 1'600; ++index) {
        const std::string directory = index < 600 ? "alpha" : (index < 1'200 ? "beta" : "zeta");
        root.write(directory + "/file-" + std::to_string(index) + ".txt", "content " + std::to_string(index));
    }
    root.write("README.md", "top-level file");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    const auto* catalog = section(snapshot, ProjectContextCategory::WorkspaceCatalog);
    require(catalog != nullptr && catalog->truncated && catalog->omittedItems > 0,
        "A 1600 file workspace must overflow the catalog limit and say so");
    require(catalog->estimatedTokens <= 9'000, "The catalog stays inside its limit");
    require(catalog->text.find("sha256=") == std::string::npos, "File hashes must not be repeated in the text");
    require(catalog->text.find("top-level directories:") != std::string::npos
            && catalog->text.find("alpha; files=600") != std::string::npos
            && catalog->text.find("beta; files=600") != std::string::npos
            && catalog->text.find("zeta; files=400") != std::string::npos
            && catalog->text.find(".; files=1") != std::string::npos,
        "The omitted tail must stay visible through the per-directory summary");
    require(catalog->text.find("workspaceFingerprint=") != std::string::npos && !catalog->sources.empty()
            && catalog->sources.front().kind == "workspace",
        "The workspace identity item is first and survives the trimming");
    bool digestsOk = true;
    for (const auto& source : catalog->sources) {
        if (source.kind == "workspace_file" && source.digest.size() != 64) digestsOk = false;
    }
    require(digestsOk, "Every included file row keeps its sha256 as the source digest");
    require(catalog->includedItems > 600, "Without the hash a row costs about a third, so far more files fit");
}

// The directory summary in the mandatory identity item is bounded and counts what it leaves out.
void testDirectorySummaryIsBoundedAndCountsTheRest()
{
    TemporaryWorkspace root("many-directories");
    for (int index = 0; index < 60; ++index) {
        root.write("dir-" + std::string(index < 10 ? "0" : "") + std::to_string(index) + "/f.txt", "x");
    }
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    const auto* catalog = section(snapshot, ProjectContextCategory::WorkspaceCatalog);
    require(catalog != nullptr, "Catalog section");
    require(catalog->text.find("dir-00; files=1") != std::string::npos
            && catalog->text.find("dir-47; files=1") != std::string::npos
            && catalog->text.find("dir-48; files=1") == std::string::npos
            && catalog->text.find("directoriesOmitted=12") != std::string::npos,
        "The summary lists at most 48 directories and counts the other 12");
}

// The fingerprint is a function of content and identity, not of where the workspace lives.
void testFingerprintDoesNotDependOnTheWorkspaceLocation()
{
    TemporaryWorkspace first("location-one");
    TemporaryWorkspace second("location-two-with-a-longer-name");
    first.write("src/a.qml", "same");
    second.write("src/a.qml", "same");
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    InMemoryProjectRepository projectsA;
    InMemoryProjectRepository projectsB;
    require(projectsA.insert(project("p1", first.path())).success(), "Project A seed");
    require(projectsB.insert(project("p1", second.path())).success(), "Project B seed");
    ProjectContextSnapshotService serviceA(projectsA, runs, ledger, resolutions, memory);
    ProjectContextSnapshotService serviceB(projectsB, runs, ledger, resolutions, memory);
    require(build(serviceA, {"p1", std::nullopt, false}).fingerprint
            == build(serviceB, {"p1", std::nullopt, false}).fingerprint,
        "The same content in two locations must give the same fingerprint");
}

void testMissingWorkspaceRootFailsClosedWithTheCaptureError()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", std::filesystem::temp_directory_path() / "ai-orchestrator-pc1b-does-not-exist")).success(),
        "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    require(failureCode(service, {"p1", std::nullopt, false}) == "workspace_baseline.invalid_root",
        "An unavailable workspace must fail with the capture error, never with a partial context");
}

ProjectMemorySegment memorySegment(std::string id, std::string projectId, std::string summary, std::int64_t timeMs,
    std::string runId = "run-x")
{
    ProjectMemorySegment value;
    value.id = std::move(id);
    value.projectId = std::move(projectId);
    value.runId = std::move(runId);
    value.summary = std::move(summary);
    value.sourceTokenCount = 100;
    value.summaryTokenCount = 40;
    value.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(timeMs));
    return value;
}

// PC-1C: derived Project Memory is opt-in, labelled derived, newest first, and read-only.
void testProjectMemoryIsOptInDerivedAndNewestFirst()
{
    TemporaryWorkspace root("memory-optin");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    memory.segments = {memorySegment("m-old", "p1", "OLD_SUMMARY_TEXT", 100),
        memorySegment("m-new", "p1", "NEW_SUMMARY_TEXT", 300), memorySegment("m-mid", "p1", "MID_SUMMARY_TEXT", 200)};
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);

    const auto without = build(service, {"p1", std::nullopt, false});
    require(section(without, ProjectContextCategory::ProjectMemory) == nullptr && memory.listCalls == 0,
        "Without the opt-in the memory is neither shown nor read");

    const auto with = build(service, {"p1", std::nullopt, true});
    const auto* derived = section(with, ProjectContextCategory::ProjectMemory);
    require(derived != nullptr && derived->authority == ProjectContextAuthority::Derived
            && derived->text.find("derived narrative, not authoritative") != std::string::npos,
        "Project Memory must be present and labelled derived");
    require(derived->text.find("projectMemorySegments=3") != std::string::npos,
        "A status item states how many segments exist");
    require(positionOf(derived->text, "NEW_SUMMARY_TEXT") < positionOf(derived->text, "MID_SUMMARY_TEXT")
            && positionOf(derived->text, "MID_SUMMARY_TEXT") < positionOf(derived->text, "OLD_SUMMARY_TEXT"),
        "The newest segment comes first, because trimming drops the tail");
    bool digestOk = false;
    for (const auto& source : derived->sources) {
        if (source.kind == "memory_segment" && source.id == "m-new") {
            digestOk = source.digest == sha256Hex("NEW_SUMMARY_TEXT");
        }
    }
    require(digestOk, "A segment's digest is the sha256 of its FULL summary");
    require(memory.writeCalls == 0 && memory.listCalls == 1, "The memory repository is only read, once");

    require(with.fingerprint != without.fingerprint, "Requesting the memory changes the fingerprint");
    const auto again = build(service, {"p1", std::nullopt, true});
    std::reverse(memory.segments.begin(), memory.segments.end());
    const auto reversed = build(service, {"p1", std::nullopt, true});
    require(again.fingerprint == with.fingerprint && reversed.fingerprint == with.fingerprint,
        "Repeating the build or reordering the repository result must not change the fingerprint");
}

// A real segment is far larger than the 2 000 token memory section: it must be shown as a bounded excerpt.
void testProjectMemoryExcerptIsBoundedAndPathLinesAreDropped()
{
    TemporaryWorkspace root("memory-excerpt");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    const std::string full = "Result: all good\nsee C:\\private\\x.log\n/etc/passwd contents\ntail " + std::string(5'000, 'x');
    memory.segments = {memorySegment("m1", "p1", full, 100)};
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    auto snapshot = build(service, {"p1", std::nullopt, true});
    const auto* derived = section(snapshot, ProjectContextCategory::ProjectMemory);
    require(derived != nullptr, "Memory section");
    require(derived->text.find("Result: all good") != std::string::npos
            && derived->text.find("C:\\private") == std::string::npos
            && derived->text.find("/etc/passwd") == std::string::npos
            && derived->text.find("omittedLines=2") != std::string::npos,
        "Path-bearing lines are dropped and counted, the rest stays");
    require(derived->text.find("excerpt=") != std::string::npos && derived->text.find(" bytes") != std::string::npos
            && derived->text.find(std::string(kProjectContextMemoryExcerptBytes + 1, 'x')) == std::string::npos,
        "A long segment is clipped with an explicit excerpt note");
    require(derived->estimatedTokens <= 2'000, "The memory section stays inside its limit");
    bool digestOk = false;
    for (const auto& source : derived->sources) {
        if (source.id == "m1") digestOk = source.digest == sha256Hex(full);
    }
    require(digestOk, "The digest covers the whole original summary, not the excerpt");

    // Many large segments: only a prefix of whole items fits; the rest is counted as omitted.
    memory.segments.clear();
    for (int index = 0; index < 12; ++index) {
        memory.segments.push_back(memorySegment("big-" + std::to_string(index), "p1", std::string(8'000, 'y'), 1'000 + index));
    }
    snapshot = build(service, {"p1", std::nullopt, true});
    derived = section(snapshot, ProjectContextCategory::ProjectMemory);
    require(derived != nullptr && derived->truncated && derived->omittedItems > 0 && derived->includedItems >= 2
            && derived->estimatedTokens <= 2'000,
        "Twelve big segments are trimmed whole, newest first, inside the memory limit");
    require(derived->text.find("big-11") != std::string::npos,
        "The newest of the big segments is the first one shown");
}

// Model-authored text must never fail the whole snapshot: control characters are cleaned, invalid UTF-8 and
// path-only text are omitted with a marker. The same cleaning protects the ledger prose.
void testUntrustedProseNeverFailsTheSnapshot()
{
    TemporaryWorkspace root("untrusted-prose");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    memory.segments = {
        memorySegment("m-escape", "p1", std::string("line \x1b[31mred\x07 end"), 400),
        memorySegment("m-bad", "p1", std::string("bad \xC3\x28 bytes"), 300),
        memorySegment("m-paths", "p1", "C:\\a\\b\n/var/log/x", 200),
        memorySegment("m-empty", "p1", "   ", 100)};
    auto entry = ledgerEntry("r1", "p1", 10);
    entry.terminalReason = std::string("ok \x1b[0m done");
    entry.remainingWorkNote = std::string("bad \xC3\x28 note");
    ledger.recent = {entry};
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, true});
    const std::string& derived = section(snapshot, ProjectContextCategory::ProjectMemory)->text;
    require(derived.find('\x1b') == std::string::npos && derived.find('\x07') == std::string::npos
            && derived.find("red") != std::string::npos,
        "Control characters in a segment are cleaned, the text stays");
    require(derived.find("[omitted: not valid text]") != std::string::npos
            && derived.find("[no usable text]") != std::string::npos,
        "Invalid text and path-only text get explicit markers");
    const std::string& ledgerText = section(snapshot, ProjectContextCategory::WorkLedger)->text;
    require(ledgerText.find('\x1b') == std::string::npos && ledgerText.find("terminalReason=ok") != std::string::npos
            && ledgerText.find("omittedNarrativeFields=1") != std::string::npos,
        "Ledger prose is cleaned the same way and an invalid field is counted, not fatal");
}

// Equal timestamps must still give one fixed order, whatever order the repository returned them in.
void testProjectMemoryOrderIsTotalForEqualTimestamps()
{
    TemporaryWorkspace root("memory-ties");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    memory.segments = {memorySegment("seg-a", "p1", "TEXT_A", 500), memorySegment("seg-b", "p1", "TEXT_B", 500),
        memorySegment("seg-c", "p1", "TEXT_C", 500)};
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto first = build(service, {"p1", std::nullopt, true});
    std::reverse(memory.segments.begin(), memory.segments.end());
    const auto second = build(service, {"p1", std::nullopt, true});
    std::swap(memory.segments[0], memory.segments[1]);
    const auto third = build(service, {"p1", std::nullopt, true});
    require(first.fingerprint == second.fingerprint && first.fingerprint == third.fingerprint,
        "Segments with equal timestamps must have one fixed order");
    const std::string& text = section(first, ProjectContextCategory::ProjectMemory)->text;
    require(positionOf(text, "segment=seg-c;") < positionOf(text, "segment=seg-b;")
            && positionOf(text, "segment=seg-b;") < positionOf(text, "segment=seg-a;"),
        "Ties are broken by segment id, descending");
}

// The excerpt is cut on a UTF-8 boundary: a multi-byte character straddling the limit must not be split.
void testProjectMemoryExcerptNeverSplitsAUtf8Character()
{
    TemporaryWorkspace root("memory-utf8-cut");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    // 1535 ASCII bytes, then a two-byte character whose first byte would be the 1536th, then more text.
    memory.segments = {memorySegment("m1", "p1",
        std::string(kProjectContextMemoryExcerptBytes - 1, 'x') + "\xC3\xA9" + std::string(100, 'y'), 100)};
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, true});
    const std::string& text = section(snapshot, ProjectContextCategory::ProjectMemory)->text;
    require(projectContextIsValidUtf8(text), "A clipped excerpt must stay well-formed UTF-8");
    require(text.find("excerpt=1535 of 1637 bytes") != std::string::npos,
        "The cut backs off to the character boundary and says so");
}

// Blank prose is not a narrative: it is counted as omitted, not rendered as an empty field.
void testBlankProseIsOmittedAndCounted()
{
    TemporaryWorkspace root("blank-prose");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    Project value = project("p1", root.path());
    value.description = "   \n\t ";
    require(projects.insert(value).success(), "Project seed");
    auto entry = ledgerEntry("r1", "p1", 10);
    entry.remainingWorkNote = "  \n  ";
    ledger.recent = {entry};
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, false});
    require(section(snapshot, ProjectContextCategory::ProjectMetadata)->text.find("description=[omitted by context safety policy]")
                != std::string::npos,
        "A blank description is omitted with the marker");
    const std::string& ledgerText = section(snapshot, ProjectContextCategory::WorkLedger)->text;
    require(ledgerText.find("omittedNarrativeFields=1") != std::string::npos
            && ledgerText.find("remainingWork=") == std::string::npos,
        "A blank ledger field is counted as omitted, never rendered empty");
}

// A repository that ignores the contract (unsorted, over the limit, a short total): the service still shows
// the newest 32 in the fixed order and never reports a total below what it holds.
class MisbehavingMemory final : public IProjectMemoryRepository {
public:
    ai::common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment) override
    {
        return ai::common::Result<ProjectMemorySegment>::failure({"test.write_forbidden", "Unexpected write"});
    }
    ai::common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId&) const override
    {
        ++fullReads;
        return ai::common::Result<std::vector<ProjectMemorySegment>>::ok(segments);
    }
    ai::common::Result<ProjectMemoryRecentPage> listRecentMemorySegments(const ProjectId&, std::size_t limit) const override
    {
        lastLimit = limit;
        ProjectMemoryRecentPage page;
        page.newest = segments;  // deliberately unsorted and not limited
        page.totalCount = reportedTotal;
        return ai::common::Result<ProjectMemoryRecentPage>::ok(std::move(page));
    }
    ai::common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(const ProjectMemorySegmentId&) const override
    {
        return ai::common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }
    ai::common::Result<void> removeMemorySegments(const ProjectId&) override
    {
        return ai::common::Result<void>::failure({"test.write_forbidden", "Unexpected write"});
    }

    std::vector<ProjectMemorySegment> segments;
    std::uint64_t reportedTotal {0};
    mutable std::size_t lastLimit {0};
    mutable std::size_t fullReads {0};
};

// Review (PC-1C): the snapshot must ask for the newest N, not for every segment of the project.
void testSnapshotAsksTheRepositoryForABoundedPage()
{
    TemporaryWorkspace root("memory-bounded");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    MisbehavingMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    for (int index = 0; index < 40; ++index) {
        memory.segments.push_back(memorySegment("s-" + std::to_string(index), "p1", "short " + std::to_string(index), index));
    }
    std::swap(memory.segments[0], memory.segments[39]);  // not sorted
    memory.reportedTotal = 7;                            // a short total from a buggy repository
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto snapshot = build(service, {"p1", std::nullopt, true});
    require(memory.lastLimit == kProjectContextRecentMemoryLimit && memory.fullReads == 0,
        "The snapshot must request the bounded page and never read every segment");
    const std::string& text = section(snapshot, ProjectContextCategory::ProjectMemory)->text;
    require(text.find("projectMemorySegments=40") != std::string::npos,
        "The reported total never falls below the rows actually returned");
    require(text.find("segment=s-39;") != std::string::npos && text.find("segment=s-8;") != std::string::npos
            && text.find("segment=s-7;") == std::string::npos
            && positionOf(text, "segment=s-39;") < positionOf(text, "segment=s-38;"),
        "Only the newest 32 are shown, in the fixed order, even if the repository returned more and unsorted");
}

// Equal timestamps from a repository that does not honour the order: the service's own tiebreak (id descending)
// keeps the snapshot, and so its fingerprint, independent of the order the rows arrived in.
void testServiceTiebreakDoesNotDependOnTheRepositoryOrder()
{
    TemporaryWorkspace root("memory-misordered-ties");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    MisbehavingMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    memory.segments = {memorySegment("seg-a", "p1", "TEXT_A", 500), memorySegment("seg-b", "p1", "TEXT_B", 500),
        memorySegment("seg-c", "p1", "TEXT_C", 500)};
    memory.reportedTotal = 3;
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);
    const auto first = build(service, {"p1", std::nullopt, true});
    std::swap(memory.segments[0], memory.segments[2]);
    const auto second = build(service, {"p1", std::nullopt, true});
    std::swap(memory.segments[1], memory.segments[2]);
    const auto third = build(service, {"p1", std::nullopt, true});
    require(first.fingerprint == second.fingerprint && first.fingerprint == third.fingerprint,
        "Equal timestamps must not make the snapshot depend on the repository's row order");
    const std::string& text = section(first, ProjectContextCategory::ProjectMemory)->text;
    require(positionOf(text, "segment=seg-c;") < positionOf(text, "segment=seg-b;")
            && positionOf(text, "segment=seg-b;") < positionOf(text, "segment=seg-a;"),
        "Ties are broken by segment id, descending");
}

// The default implementation of the port serves every repository that does not override it.
void testDefaultRecentReadIsNewestFirstWithTheTotal()
{
    ScriptedMemory memory;
    memory.segments = {memorySegment("a", "p1", "A", 100), memorySegment("b", "p1", "B", 300),
        memorySegment("c", "p1", "C", 300), memorySegment("d", "p1", "D", 200)};
    const auto page = memory.listRecentMemorySegments("p1", 3);
    require(page.success() && page.value().totalCount == 4 && page.value().newest.size() == 3
            && page.value().newest[0].id == "c" && page.value().newest[1].id == "b" && page.value().newest[2].id == "d",
        "The default recent read is newest first, ties by id descending, limited, with the total");
    const auto zero = memory.listRecentMemorySegments("p1", 0);
    require(zero.success() && zero.value().totalCount == 4 && zero.value().newest.empty(), "A zero limit keeps the total");
    memory.readError = ai::common::Error {"storage.memory_unavailable", "Memory unavailable"};
    const auto failed = memory.listRecentMemorySegments("p1", 3);
    require(!failed.success() && failed.error()->code == "storage.memory_unavailable",
        "The default recent read propagates the repository error");
}

void testProjectMemoryRequestFailsClosedAndIsIsolated()
{
    TemporaryWorkspace root("memory-closed");
    root.write("file.txt", "x");
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ScriptedLedger ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    require(projects.insert(project("p1", root.path())).success(), "Project seed");
    ProjectContextSnapshotService service(projects, runs, ledger, resolutions, memory);

    // Requested but empty: the section is still there and says so.
    const auto empty = build(service, {"p1", std::nullopt, true});
    const auto* section0 = section(empty, ProjectContextCategory::ProjectMemory);
    require(section0 != nullptr && section0->text.find("projectMemorySegments=0 (none recorded)") != std::string::npos,
        "A requested but empty memory is visible, never silently absent");

    memory.segments = {memorySegment("foreign", "p2", "other project", 1)};
    require(failureCode(service, {"p1", std::nullopt, true}) == "project_context.memory_project_mismatch",
        "A segment of another project must be rejected");
    memory.segments.clear();

    memory.readError = ai::common::Error {"storage.memory_unavailable", "Memory unavailable"};
    require(failureCode(service, {"p1", std::nullopt, true}) == "storage.memory_unavailable",
        "A failing memory read must fail the build when the memory was requested");
    require(build(service, {"p1", std::nullopt, false}).sections.size() >= 2,
        "A failing memory is irrelevant when it was not requested");

    memory.readError.reset();
    for (int index = 0; index < 40; ++index) {
        memory.segments.push_back(memorySegment("s-" + std::to_string(index), "p1", "short " + std::to_string(index), index));
    }
    const auto many = build(service, {"p1", std::nullopt, true});
    const std::string& text = section(many, ProjectContextCategory::ProjectMemory)->text;
    require(text.find("projectMemorySegments=40") != std::string::npos && text.find("newestListed=32") != std::string::npos
            && text.find("s-39") != std::string::npos && text.find("segment=s-7;") == std::string::npos,
        "At most the 32 newest segments are listed and the total is stated");
}

} // namespace

int main()
{
    try {
        testBuildUsesSelectedWorkspaceAndLoadsTypedAuthorityWithoutWrites();
        testProjectRootBuildDoesNotInventLineageOrCallResolutionReader();
        testLineageIsolationCycleDepthAndMissingParentsFailClosed();
        testLedgerAndResolutionIdentityAreValidated();
        testProjectMemoryRequestAndAuthoritativeReadFailuresAreExplicit();
        testUnsafeProjectDescriptionIsOmittedWithItsDigest();
        testLedgerIsNewestFirstAndTransitionsAreChronological();
        testSelectedLineageLedgerEntryIdentityIsValidated();
        testUnknownInactiveAndMalformedRequestsFailClosed();
        testLongLineageKeepsTheSelectedRunAndTrimsTheOldestAncestors();
        testPathLikeProjectNameIsOmittedNotFatal();
        testLargeCatalogHasNoHashesInTextAndKeepsADirectorySummary();
        testDirectorySummaryIsBoundedAndCountsTheRest();
        testProjectMemoryIsOptInDerivedAndNewestFirst();
        testProjectMemoryExcerptIsBoundedAndPathLinesAreDropped();
        testUntrustedProseNeverFailsTheSnapshot();
        testProjectMemoryOrderIsTotalForEqualTimestamps();
        testProjectMemoryExcerptNeverSplitsAUtf8Character();
        testBlankProseIsOmittedAndCounted();
        testSnapshotAsksTheRepositoryForABoundedPage();
        testServiceTiebreakDoesNotDependOnTheRepositoryOrder();
        testDefaultRecentReadIsNewestFirstWithTheTotal();
        testProjectMemoryRequestFailsClosedAndIsIsolated();
        testFingerprintDoesNotDependOnTheWorkspaceLocation();
        testMissingWorkspaceRootFailsClosedWithTheCaptureError();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
