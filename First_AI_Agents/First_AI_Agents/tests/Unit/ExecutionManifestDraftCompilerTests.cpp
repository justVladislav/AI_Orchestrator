#include "ai/application/ExecutionManifestDependencyGraph.hpp"
#include "ai/application/ExecutionManifestDraftCompiler.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// MW-P3A package 2 ("Backend Compiler + Read Semantics"): coverage for
// ExecutionManifestDraftCompiler in isolation, plus its integration with
// deriveManifestDependencyGraph - the exact combination that must resolve
// the real Runs behind MW-P3A:
// - 3ffb52df-98ca-472f-9b1c-537ab134cf18: an absent file must compile to
//   plannedCreatePaths, never ownedPaths, without the provider ever
//   choosing;
// - a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b: a baselineRequiredReadPaths entry
//   must never derive a cross-task dependency edge, even when the exact
//   same normalized path is also another task's writePaths entry.

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

WorkspaceBaselineSnapshot baselineWith(std::vector<std::string> relativePaths)
{
    WorkspaceBaselineSnapshot baseline;
    baseline.fingerprint = "test-fingerprint";
    for (auto& path : relativePaths) {
        WorkspaceBaselineEntry entry;
        entry.relativePath = std::move(path);
        entry.byteSize = 128;
        entry.sha256 = "deadbeef";
        baseline.files.push_back(std::move(entry));
    }
    return baseline;
}

ExecutionAcceptanceContract inspectPathsContract(
    const std::string& requirementId, const std::string& requiredOutput)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = requirementId;
    requirement.description = "Verify " + requirementId;
    requirement.requiredOutputs = {requiredOutput};
    requirement.verificationKind = ExecutionVerificationKind::InspectPaths;
    requirement.required = true;
    ExecutionAcceptanceContract contract;
    contract.requirements = {std::move(requirement)};
    return contract;
}

ManifestDraftWorkTask workTask(
    const std::string& id, const std::string& writePath)
{
    ManifestDraftWorkTask task;
    task.id = id;
    task.objective = "Complete " + id;
    task.role = "worker";
    task.writePaths = {writePath};
    task.requestedCapability = ManifestDraftCapability::Propose;
    task.acceptanceContract = inspectPathsContract("verify-" + id, writePath);
    return task;
}

const ExecutionManifestTask* findTask(const ExecutionManifest& manifest, const std::string& id)
{
    const auto it = std::find_if(manifest.tasks.begin(), manifest.tasks.end(),
        [&](const auto& task) { return task.id == id; });
    return it == manifest.tasks.end() ? nullptr : &(*it);
}

bool hasFinding(const std::vector<ExecutionManifestFinding>& findings, const std::string& code)
{
    return std::any_of(findings.begin(), findings.end(),
        [&](const auto& finding) { return finding.code == code; });
}

bool hasEdge(
    const std::vector<ManifestDependencyEdge>& edges,
    const std::string& consumerTaskId, const std::string& producerTaskId)
{
    return std::any_of(edges.begin(), edges.end(), [&](const auto& edge) {
        return edge.consumerTaskId == consumerTaskId && edge.producerTaskId == producerTaskId;
    });
}

void testWritePathClassifiedByBaselineExistence()
{
    ExecutionManifestDraft draft;
    auto task = workTask("build", "src/existing.cpp");
    task.writePaths = {"src/existing.cpp", "src/new-file.cpp"};
    draft.workTasks = {task};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({"src/existing.cpp"})});

    require(result.findings.empty(), "A well-formed compile must produce no findings");
    const auto* compiled = findTask(result.manifest, "build");
    require(compiled != nullptr, "Compiled task must exist");
    require(compiled->ownedPaths.size() == 1
        && compiled->ownedPaths[0] == std::filesystem::path("src/existing.cpp"),
        "An existing baseline path must classify as owned_write");
    require(compiled->plannedCreatePaths.size() == 1
        && compiled->plannedCreatePaths[0] == std::filesystem::path("src/new-file.cpp"),
        "An absent baseline path must classify as planned_create - the provider never "
        "chooses, closing Run 3ffb52df-98ca-472f-9b1c-537ab134cf18's misclassification");
}

void testCoordinationTaskCompilesToEmptyNoneCapabilityTask()
{
    ExecutionManifestDraft draft;
    ManifestDraftCoordinationTask coordination;
    coordination.id = "gate";
    coordination.objective = "Coordinate";
    coordination.role = "coordinator";
    draft.coordinationTasks = {coordination};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({})});

    const auto* compiled = findTask(result.manifest, "gate");
    require(compiled != nullptr, "Compiled coordination task must exist");
    require(compiled->requestedCapability == WorkspaceCapability::None,
        "A coordination task must compile to requestedCapability None");
    require(compiled->ownedPaths.empty() && compiled->acceptanceCriteria.empty()
        && !compiled->acceptanceContract.has_value(),
        "A compiled coordination task must own no path and claim no acceptance");
}

void testBaselineRequiredReadPathNotInBaselineIsRejected()
{
    ExecutionManifestDraft draft;
    auto task = workTask("build", "src/app.cpp");
    task.baselineRequiredReadPaths = {"docs/MISSING.md"};
    draft.workTasks = {task};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({"src/app.cpp"})});

    require(hasFinding(result.findings, "manifest.baseline_required_read_path_not_in_baseline"),
        "A baselineRequiredReadPaths entry absent from the frozen baseline must be rejected");
}

void testProposedDependencyReadPathWithNoProducerIsRejected()
{
    ExecutionManifestDraft draft;
    auto task = workTask("build", "src/app.cpp");
    task.proposedDependencyReadPaths = {"src/nobody-writes-this.cpp"};
    draft.workTasks = {task};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({"src/app.cpp"})});

    require(hasFinding(result.findings, "manifest.proposed_dependency_read_path_has_no_producer"),
        "A proposedDependencyReadPaths entry naming no real producer must be rejected");
}

void testProposedDependencyReadPathOfOwnWritePathIsHarmlesslyIgnored()
{
    // A path that duplicates this same task's own writePaths is a harmless
    // redundancy (the worker can already read anything it writes), not an
    // independent cross-task dependency claim - the compiler must not
    // reject it (canonicalizeRedundantActiveScopeRoles resolves the
    // redundancy right after compilation), and deriveManifestDependencyGraph
    // never derives a self-edge from it either way.
    ExecutionManifestDraft draft;
    auto task = workTask("build", "src/app.cpp");
    task.proposedDependencyReadPaths = {"src/app.cpp"};
    draft.workTasks = {task};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({"src/app.cpp"})});

    require(!hasFinding(result.findings, "manifest.proposed_dependency_read_path_has_no_producer"),
        "A proposedDependencyReadPaths entry naming the task's own writePaths must be "
        "silently tolerated, not rejected");
}

void testBaselineOnlyReadNeverDerivesEdgeEvenWhenAlsoAnotherTasksWritePath()
{
    // Real-Run incident (a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b): task A needs
    // the CURRENT content of QML_PROJECT_CONTEXT.md; task B will later
    // rewrite that same file as part of its own real work. This must
    // compile to a single edge B -> ??? none from A at all, never a false
    // A -> B edge, and never a cycle with a genuine B -> A edge (task B
    // also needs task A's newly created PageHost.qml).
    ExecutionManifestDraft draft;
    auto taskA = workTask("component", "qml/PageHost.qml");
    taskA.baselineRequiredReadPaths = {"QML_PROJECT_CONTEXT.md"};

    auto taskB = workTask("integration", "QML_PROJECT_CONTEXT.md");
    taskB.proposedDependencyReadPaths = {"qml/PageHost.qml"};
    taskB.dependencies = {"component"};

    draft.workTasks = {taskA, taskB};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({"QML_PROJECT_CONTEXT.md"})});
    require(result.findings.empty(), "This is a legitimate, fully resolvable shape");

    const auto edges = deriveManifestDependencyGraph(result.manifest);
    require(!hasEdge(edges, "component", "integration"),
        "task A's baseline-only read of QML_PROJECT_CONTEXT.md must never derive an edge "
        "onto task B, even though task B's writePaths also claims that same path");
    require(hasEdge(edges, "integration", "component"),
        "task B's proposedDependencyReadPaths on task A's new PageHost.qml must derive a "
        "real edge");
}

void testBootstrapDeclarationAndBudgetsPassThroughCompiler()
{
    ExecutionManifestDraft draft;
    draft.workTasks = {workTask("build", "CMakePresets.json")};
    draft.requestedMaxParallelism = 3;
    draft.requestedTokenBudget = 40000;
    draft.requestedCostMicros = 2000000;
    ManifestBootstrapDeclaration declaration;
    declaration.bootstrapTaskId = "build";
    declaration.setupPaths = {"CMakePresets.json"};
    draft.bootstrapDeclaration = declaration;

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({})});

    require(result.manifest.requestedMaxParallelism == 3, "requestedMaxParallelism must pass through");
    require(result.manifest.requestedTokenBudget.value_or(0) == 40000, "requestedTokenBudget must pass through");
    require(result.manifest.requestedCostMicros.value_or(0) == 2000000, "requestedCostMicros must pass through");
    require(result.manifest.bootstrapDeclaration.has_value()
        && result.manifest.bootstrapDeclaration->bootstrapTaskId == "build",
        "bootstrapDeclaration must pass through");
    require(result.manifest.schemaVersion == std::string(kExecutionManifestSchemaVersion),
        "Compiled schemaVersion must be execution-manifest.v1, not the draft's own schema version");
}

} // namespace

// AG-17.4: the provider's only routing statement is copied verbatim; the compiler
// neither validates nor invents it (the validator judges it against the frozen
// routing record), and a coordination task never carries one.
void testAssigneeIsCopiedVerbatimToTheCompiledTask()
{
    ExecutionManifestDraft draft;
    auto named = workTask("ui", "src/ui.qml");
    named.assigneeMemberId = "qml-worker";
    draft.workTasks = {named, workTask("core", "src/core.cpp")}; // second names none
    ManifestDraftCoordinationTask coordination;
    coordination.id = "gate";
    coordination.objective = "Coordinate";
    coordination.role = "coordinator";
    draft.coordinationTasks = {coordination};

    const auto result = ExecutionManifestDraftCompiler::compile(
        {draft, RunId{"run-1"}, baselineWith({})});

    require(result.findings.empty(), "Naming an assignee must not add a compiler finding");
    const auto* ui = findTask(result.manifest, "ui");
    const auto* core = findTask(result.manifest, "core");
    const auto* gate = findTask(result.manifest, "gate");
    require(ui != nullptr && core != nullptr && gate != nullptr, "Compiled tasks must exist");
    require(ui->assigneeMemberId == "qml-worker",
        "A named assignee must reach the compiled task unchanged");
    require(!core->assigneeMemberId.has_value(),
        "A task without an assignee must compile without one (the validator reports it in a roster)");
    require(!gate->assigneeMemberId.has_value(),
        "A compiled coordination task never carries an assignee");
}

int main()
{
    try {
        testWritePathClassifiedByBaselineExistence();
        testCoordinationTaskCompilesToEmptyNoneCapabilityTask();
        testBaselineRequiredReadPathNotInBaselineIsRejected();
        testProposedDependencyReadPathWithNoProducerIsRejected();
        testProposedDependencyReadPathOfOwnWritePathIsHarmlesslyIgnored();
        testBaselineOnlyReadNeverDerivesEdgeEvenWhenAlsoAnotherTasksWritePath();
        testBootstrapDeclarationAndBudgetsPassThroughCompiler();
        testAssigneeIsCopiedVerbatimToTheCompiledTask();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
