// Unit tests for RunWorkspaceScopeService (AG-15): the pure freeze/expansion
// assembly logic, entirely in-memory - no SQLite. Repository round-trip
// coverage lives in RunWorkspaceScopeRepositoryTests.cpp.
#include "ai/application/execution/RunWorkspaceScopeService.hpp"

#include "ai/application/inmemory/InMemoryRunWorkspaceScopeRepository.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace {

using namespace ai::application;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

WorkspaceBaselineSnapshot makeBaseline(std::vector<std::pair<std::string, std::string>> filesAndHashes)
{
    WorkspaceBaselineSnapshot baseline;
    for (auto& [path, hash] : filesAndHashes) {
        WorkspaceBaselineEntry entry;
        entry.relativePath = path;
        entry.byteSize = 10;
        entry.sha256 = hash;
        baseline.files.push_back(std::move(entry));
    }
    return baseline;
}

ExecutionManifestTask makeTask(const std::string& id)
{
    ExecutionManifestTask task;
    task.id = id;
    task.objective = "Objective for " + id;
    task.role = "worker";
    task.requestedCapability = WorkspaceCapability::Propose;
    return task;
}

WorkspaceReadPolicy makePolicy()
{
    return WorkspaceReadPolicy::secureDefaults(std::filesystem::path("C:/workspace"));
}

void freezeTaskScope(RunWorkspaceScopeService& service,
    const WorkspaceBaselineSnapshot& baseline, const std::string& taskId = "implement")
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = {makeTask(taskId)};
    req(service.freezeForManifest("run-1", manifest, baseline).success(),
        "Could not seed the frozen Run scope");
}

void testFreezeAttachesRealBaselineHashesAndDerivesDependencyEdges()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto define = makeTask("define-shared");
    define.ownedPaths = {std::filesystem::path("src/shared.hpp")};
    auto implement = makeTask("implement");
    implement.dependencies = {"define-shared"};
    implement.requiredReadPaths = {std::filesystem::path("src/shared.hpp")};
    implement.plannedCreatePaths = {std::filesystem::path("src/new_file.cpp")};
    manifest.tasks = {std::move(define), std::move(implement)};

    const auto baseline = makeBaseline({{"src/shared.hpp", "hash-shared"}});
    const auto frozen = service.freezeForManifest("run-1", manifest, baseline);
    req(frozen.success(), "freezeForManifest failed on a valid manifest");

    const auto& scope = frozen.value();
    req(scope.entries.size() == 3, "Expected 3 scope entries (owned + required_read + planned_create)");
    const auto findEntry = [&](const std::string& path) -> const RunWorkspaceScopeEntry* {
        for (const auto& entry : scope.entries) {
            if (entry.relativePath == path) return &entry;
        }
        return nullptr;
    };
    const auto* shared = findEntry("src/shared.hpp");
    req(shared != nullptr, "src/shared.hpp entry missing");
    req(shared->baselineHash.has_value() && *shared->baselineHash == "hash-shared",
        "Baseline hash was not attached from the real captured snapshot");
    const auto* created = findEntry("src/new_file.cpp");
    req(created != nullptr && !created->baselineHash.has_value(),
        "A planned_create path must never carry a baseline hash");

    req(scope.dependencyEdges.size() == 1
            && scope.dependencyEdges.front().dependentTaskId == "implement"
            && scope.dependencyEdges.front().providerTaskId == "define-shared"
            && scope.dependencyEdges.front().relativePath == "src/shared.hpp",
        "Backend-derived dependency edge was not built from required_read + owned_write");

    const auto found = service.findForRun("run-1");
    req(found.success() && found.value().has_value(), "Frozen scope was not persisted");
}

void testFreezeRejectsPlannedCreateThatAlreadyExistsInBaseline()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto task = makeTask("implement");
    task.plannedCreatePaths = {std::filesystem::path("src/already-exists.cpp")};
    manifest.tasks = {std::move(task)};

    const auto baseline = makeBaseline({{"src/already-exists.cpp", "hash-1"}});
    const auto frozen = service.freezeForManifest("run-1", manifest, baseline);
    req(!frozen.success(), "A planned_create path already in baseline was accepted");
    req(frozen.error()->code == "workspace_scope.planned_create_path_already_exists",
        "Expected the planned_create_path_already_exists failure code");

    const auto found = service.findForRun("run-1");
    req(found.success() && !found.value().has_value(),
        "No scope must be persisted when freeze validation fails");
}

void testFreezeRejectsOwnedWriteMissingFromBaseline()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto task = makeTask("implement");
    task.ownedPaths = {std::filesystem::path("src/does-not-exist.cpp")};
    manifest.tasks = {std::move(task)};

    const auto baseline = makeBaseline({});
    const auto frozen = service.freezeForManifest("run-1", manifest, baseline);
    req(!frozen.success(), "An owned_write path missing from baseline was accepted");
    req(frozen.error()->code == "workspace_scope.owned_write_path_missing_from_baseline",
        "Expected the owned_write_path_missing_from_baseline failure code");
}

void testFreezeIsIdempotentOnRetry()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = {makeTask("coordinate")};

    const auto baseline = makeBaseline({});
    req(service.freezeForManifest("run-1", manifest, baseline).success(), "First freeze failed");
    const auto second = service.freezeForManifest("run-1", manifest, baseline);
    req(second.success(), "A retried freeze for the same Run must be a benign idempotent success");
}

void testRequestExpansionAllowsSafeRequiredReadDependency()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"src/discovered.hpp", "hash-discovered"}});
    freezeTaskScope(service, baseline);

    const auto result = service.requestExpansion(
        "run-1", "implement", "src/discovered.hpp", WorkspaceScopeRole::RequiredRead,
        "Discovered this header is a real dependency while reading src/a.cpp",
        policy, baseline, /*maxExpansionsForTask=*/5);
    req(result.success(), "A safe, genuinely bounded expansion request was rejected");
    req(result.value().baselineHash.has_value() && *result.value().baselineHash == "hash-discovered",
        "Expansion baseline hash was not attached from the real captured snapshot");

    const auto listed = service.listExpansionsForRun("run-1");
    req(listed.success() && listed.value().size() == 1, "Approved expansion was not persisted");
}

void testRequestExpansionRejectsWriteRole()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();

    const auto result = service.requestExpansion(
        "run-1", "implement", "src/new.cpp", WorkspaceScopeRole::OwnedWrite,
        "Trying to sneak a write target in", policy, std::nullopt, 5);
    req(!result.success(), "An expansion requesting a write-capable role was accepted");
    req(result.error()->code == "workspace_scope.expansion_role_not_allowed",
        "Expected the expansion_role_not_allowed failure code");
}

void testRequestExpansionRejectsGloballyExcludedPath()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();

    const auto result = service.requestExpansion(
        "run-1", "implement", ".env", WorkspaceScopeRole::RequiredRead,
        "Wants to read a secret file", policy, std::nullopt, 5);
    req(!result.success(), "An expansion targeting a globally-excluded path was accepted");
    req(result.error()->code == "workspace_scope.expansion_path_denied",
        "Expected the expansion_path_denied failure code");
}

void testRequestExpansionRejectsPathTraversal()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();

    const auto result = service.requestExpansion(
        "run-1", "implement", "../outside", WorkspaceScopeRole::RequiredRead,
        "Wants to escape the workspace", policy, std::nullopt, 5);
    req(!result.success(), "An expansion targeting a path-traversal escape was accepted");
    req(result.error()->code == "workspace_scope.expansion_path_denied",
        "Expected the expansion_path_denied failure code");
}

void testRequestExpansionEnforcesHardLimit()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({
        {"src/file0.hpp", "hash-0"},
        {"src/file1.hpp", "hash-1"},
        {"src/file2.hpp", "hash-2"},
    });
    freezeTaskScope(service, baseline);

    for (int i = 0; i < 2; ++i) {
        const auto result = service.requestExpansion(
            "run-1", "implement", "src/file" + std::to_string(i) + ".hpp",
            WorkspaceScopeRole::RequiredRead, "Genuinely discovered dependency",
            policy, baseline, /*maxExpansionsForTask=*/2);
        req(result.success(), "An expansion within the bounded limit was rejected");
    }
    const auto overLimit = service.requestExpansion(
        "run-1", "implement", "src/file2.hpp", WorkspaceScopeRole::RequiredRead,
        "One too many", policy, baseline, /*maxExpansionsForTask=*/2);
    req(!overLimit.success(), "An expansion beyond the bounded per-task limit was accepted");
    req(overLimit.error()->code == "workspace_scope.expansion_limit_exceeded",
        "Expected the expansion_limit_exceeded failure code");
}

void testRequestExpansionRejectsBlankReason()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();

    const auto result = service.requestExpansion(
        "run-1", "implement", "src/discovered.hpp", WorkspaceScopeRole::RequiredRead,
        "", policy, std::nullopt, 5);
    req(!result.success(), "A blank expansion reason was accepted");
    req(result.error()->code == "workspace_scope.expansion_invalid_reason",
        "Expected the expansion_invalid_reason failure code");
}

void testRequestExpansionRequiresFrozenScopeAndExistingFile()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"src/discovered.hpp", "hash"}});

    const auto notFrozen = service.requestExpansion(
        "run-1", "implement", "src/discovered.hpp", WorkspaceScopeRole::RequiredRead,
        "Discovered dependency", policy, baseline, 5);
    req(!notFrozen.success() && notFrozen.error()->code == "workspace_scope.not_frozen",
        "Expansion without an immutable parent scope was accepted");

    freezeTaskScope(service, baseline);
    const auto missing = service.requestExpansion(
        "run-1", "implement", "src/missing.hpp", WorkspaceScopeRole::RequiredRead,
        "Discovered dependency", policy, baseline, 5);
    req(!missing.success() && missing.error()->code == "workspace_scope.expansion_path_missing",
        "Expansion of a nonexistent, unplanned file was accepted");
}

void testRequestExpansionCannotActivateExcludedPath()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"docs/private.md", "hash"}});
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto task = makeTask("implement");
    task.excludedPaths.push_back({"docs/private.md", "Out of scope"});
    manifest.tasks = {std::move(task)};
    req(service.freezeForManifest("run-1", manifest, baseline).success(),
        "Could not freeze excluded-path fixture");

    const auto result = service.requestExpansion(
        "run-1", "implement", "docs/private.md", WorkspaceScopeRole::RequiredRead,
        "Changed my mind", policy, baseline, 5);
    req(!result.success()
            && result.error()->code == "workspace_scope.expansion_excluded_path",
        "A frozen excluded path was activated by expansion");
}

void testRequestExpansionRetryIsIdempotentButRoleCannotChange()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"src/discovered.hpp", "hash"}});
    freezeTaskScope(service, baseline);

    const auto first = service.requestExpansion(
        "run-1", "implement", "src/discovered.hpp", WorkspaceScopeRole::RequiredRead,
        "Discovered dependency", policy, baseline, 5);
    const auto retry = service.requestExpansion(
        "run-1", "implement", "src/discovered.hpp", WorkspaceScopeRole::RequiredRead,
        "Retry", policy, baseline, 5);
    req(first.success() && retry.success() && first.value().id == retry.value().id,
        "Same-role expansion retry did not return the durable original record");

    const auto conflict = service.requestExpansion(
        "run-1", "implement", "src/discovered.hpp", WorkspaceScopeRole::ReferenceOnly,
        "Try another role", policy, baseline, 5);
    req(!conflict.success()
            && conflict.error()->code == "workspace_scope.expansion_role_conflict",
        "An existing expansion was silently reinterpreted under another role");
}

// Planner recovery hardening for manifest.missing_scope_dependency: the
// finding used to say only "A task requiring another task's output must
// declare that task as a dependency", naming neither the consumer, the
// producer, nor the path - a repair session had nothing concrete to act on
// and repeated the same invalid graph until planner.repair_exhausted. The
// finding must now name all three, using only the already-normalized
// workspace-relative path (never an absolute path or the workspace root).

void testMissingScopeDependencyNamesConsumerProducerAndPathForOwnedPath()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto producer = makeTask("define-shared");
    producer.ownedPaths = {std::filesystem::path("src/Shared.hpp")};
    auto consumer = makeTask("implement");
    consumer.requiredReadPaths = {std::filesystem::path("src/Shared.hpp")};
    // consumer.dependencies is deliberately left empty - the defect this
    // closes.
    manifest.tasks = {std::move(producer), std::move(consumer)};

    const auto baseline = makeBaseline({{"src/Shared.hpp", "hash-shared"}});
    const auto findings =
        RunWorkspaceScopeService::validateManifestAgainstBaseline(manifest, baseline);

    const auto found = std::find_if(findings.begin(), findings.end(),
        [](const ExecutionManifestFinding& finding) {
            return finding.code == "manifest.missing_scope_dependency";
        });
    req(found != findings.end(), "Expected a manifest.missing_scope_dependency finding");
    req(found->taskId.has_value() && *found->taskId == "implement",
        "The finding must be attributed to the consumer task");
    req(found->message
            == "Task 'implement' requires path 'src/shared.hpp' produced by task "
               "'define-shared' and must include 'define-shared' in dependencies.",
        "Diagnostic message must name consumer, producer, and the normalized path exactly");

    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto frozen = service.freezeForManifest("run-1", manifest, baseline);
    req(!frozen.success() && frozen.error()->code == "manifest.missing_scope_dependency",
        "freezeForManifest must still fail closed on the same finding");
}

void testMissingScopeDependencyDetectsPlannedCreateProducer()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto producer = makeTask("generate-model");
    producer.plannedCreatePaths = {std::filesystem::path("src/Generated/Model.qml")};
    auto consumer = makeTask("wire-ui");
    consumer.requiredReadPaths = {std::filesystem::path("src/Generated/Model.qml")};
    manifest.tasks = {std::move(producer), std::move(consumer)};

    const auto baseline = makeBaseline({});
    const auto findings =
        RunWorkspaceScopeService::validateManifestAgainstBaseline(manifest, baseline);

    const auto found = std::find_if(findings.begin(), findings.end(),
        [](const ExecutionManifestFinding& finding) {
            return finding.code == "manifest.missing_scope_dependency";
        });
    req(found != findings.end(),
        "Expected a manifest.missing_scope_dependency finding for a plannedCreatePaths producer");
    req(found->taskId.has_value() && *found->taskId == "wire-ui",
        "The finding must be attributed to the consumer task");
    req(found->message
            == "Task 'wire-ui' requires path 'src/generated/model.qml' produced by task "
               "'generate-model' and must include 'generate-model' in dependencies.",
        "Diagnostic message must name consumer, producer, and the normalized path exactly");
}

void testMissingScopeDependencyMessageLeaksNoAbsolutePathOrRoot()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto producer = makeTask("define-shared");
    producer.ownedPaths = {std::filesystem::path("src/Shared.hpp")};
    auto consumer = makeTask("implement");
    consumer.requiredReadPaths = {std::filesystem::path("src/Shared.hpp")};
    manifest.tasks = {std::move(producer), std::move(consumer)};

    const auto baseline = makeBaseline({{"src/Shared.hpp", "hash-shared"}});
    const auto findings =
        RunWorkspaceScopeService::validateManifestAgainstBaseline(manifest, baseline);
    const auto found = std::find_if(findings.begin(), findings.end(),
        [](const ExecutionManifestFinding& finding) {
            return finding.code == "manifest.missing_scope_dependency";
        });
    req(found != findings.end(), "Expected a manifest.missing_scope_dependency finding");
    req(found->message.find(':') == std::string::npos,
        "Diagnostic message must never contain a Windows drive-letter absolute path");
    req(found->message.find("\\\\") == std::string::npos,
        "Diagnostic message must never contain a UNC absolute path");
    req(found->message.find("C:/workspace") == std::string::npos,
        "Diagnostic message must never contain the workspace root");
}

void testDeclaredScopeDependencyPassesValidation()
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto producer = makeTask("define-shared");
    producer.ownedPaths = {std::filesystem::path("src/shared.hpp")};
    auto consumer = makeTask("implement");
    consumer.requiredReadPaths = {std::filesystem::path("src/shared.hpp")};
    consumer.dependencies = {"define-shared"};
    manifest.tasks = {std::move(producer), std::move(consumer)};

    const auto baseline = makeBaseline({{"src/shared.hpp", "hash-shared"}});
    const auto findings =
        RunWorkspaceScopeService::validateManifestAgainstBaseline(manifest, baseline);
    req(std::none_of(findings.begin(), findings.end(),
            [](const ExecutionManifestFinding& finding) {
                return finding.code == "manifest.missing_scope_dependency";
            }),
        "A correctly declared producer dependency must not raise missing_scope_dependency");

    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto frozen = service.freezeForManifest("run-1", manifest, baseline);
    req(frozen.success(), "A correctly declared producer dependency must freeze successfully");
}

// ---------------------------------------------------------------------
// Verification-repair reliability hardening: audited diagnostic read scope
// (RunWorkspaceScopeService::requestDiagnosticRead). The real incident this
// closes: a Planner froze qml/app/Theme.qml as excluded even though owned
// QML files referenced Theme, so a verification-repair worker could not
// inspect the authoritative singleton declaration and guessed another
// invalid property name instead.
// ---------------------------------------------------------------------

void testDiagnosticReadOfExistingBaselineFileSucceeds()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/Theme.qml", "verification.qml_warning",
        "Theme.iconLarge is unknown; need the real property names", policy, baseline,
        /*maxDiagnosticReadsForTask=*/5);
    req(result.success(), "A safe, bounded diagnostic read of an existing baseline file was rejected");
    req(result.value().relativePath == "qml/app/theme.qml",
        "Diagnostic read grant did not carry the normalized path");
    req(result.value().baselineHash.has_value() && *result.value().baselineHash == "hash-theme",
        "Diagnostic read grant baseline hash was not attached from the real captured snapshot");
    req(result.value().findingCode == "verification.qml_warning",
        "Diagnostic read grant did not carry the supplied finding code");
}

void testDiagnosticReadNeverMutatesFrozenScopeOrGrantsWriteAccess()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto task = makeTask("implement");
    task.excludedPaths.push_back({"qml/app/Theme.qml", "Design system, not owned by this task"});
    manifest.tasks = {std::move(task)};
    const auto frozen = service.freezeForManifest("run-1", manifest, baseline);
    req(frozen.success(), "Could not freeze excluded-path fixture");
    const auto entryCountBefore = frozen.value().entries.size();

    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/Theme.qml", "verification.qml_warning",
        "Need the real Theme property names", policy, baseline, 5);
    req(result.success(), "Diagnostic read of a Planner-excluded baseline file was rejected");

    const auto refrozen = service.findForRun("run-1");
    req(refrozen.success() && refrozen.value().has_value(), "Frozen scope disappeared");
    req(refrozen.value()->entries.size() == entryCountBefore,
        "A diagnostic read must never add, remove, or replace a frozen scope entry");
    const auto themeEntry = std::find_if(
        refrozen.value()->entries.begin(), refrozen.value()->entries.end(),
        [](const RunWorkspaceScopeEntry& entry) { return entry.relativePath == "qml/app/theme.qml"; });
    req(themeEntry != refrozen.value()->entries.end()
            && themeEntry->role == WorkspaceScopeRole::Excluded,
        "The original Excluded entry must remain exactly Excluded - never mutated or replaced");
    req(themeEntry->role != WorkspaceScopeRole::OwnedWrite
            && themeEntry->role != WorkspaceScopeRole::PlannedCreate,
        "A diagnostic read must never grant write ownership");
}

void testDiagnosticReadIsDurablyRecorded()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    req(service.requestDiagnosticRead(
            "run-1", "implement", "qml/app/Theme.qml", "verification.qml_warning",
            "Need the real Theme property names", policy, baseline, 5)
            .success(),
        "Diagnostic read request failed");

    const auto listed = service.listDiagnosticReadsForRun("run-1");
    req(listed.success() && listed.value().size() == 1, "Diagnostic read grant was not durably recorded");
    req(listed.value().front().taskId == "implement"
            && listed.value().front().relativePath == "qml/app/theme.qml"
            && listed.value().front().findingCode == "verification.qml_warning",
        "Durable diagnostic read record lost its identifying fields");
}

void testDiagnosticReadRejectsAbsolutePath()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", "C:/workspace/qml/app/Theme.qml", "verification.qml_warning",
        "Trying an absolute path", policy, baseline, 5);
    req(!result.success(), "An absolute diagnostic-read path was accepted");
    req(result.error()->code == "workspace_scope.diagnostic_read_invalid_path",
        "Expected the diagnostic_read_invalid_path failure code");
}

void testDiagnosticReadRejectsParentTraversal()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", "../outside/secret.txt", "verification.qml_warning",
        "Trying to escape the workspace", policy, baseline, 5);
    req(!result.success(), "A parent-traversal diagnostic-read path was accepted");
    req(result.error()->code == "workspace_scope.diagnostic_read_invalid_path",
        "Expected the diagnostic_read_invalid_path failure code");
}

void testDiagnosticReadRejectsRepositoryRootAndDirectory()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    const auto emptyPath = service.requestDiagnosticRead(
        "run-1", "implement", "", "verification.qml_warning",
        "Trying the repository root", policy, baseline, 5);
    req(!emptyPath.success()
            && emptyPath.error()->code == "workspace_scope.diagnostic_read_invalid_path",
        "An empty (repository-root) diagnostic-read path was accepted");

    const auto directoryPath = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app", "verification.qml_warning",
        "Trying a directory, not a file", policy, baseline, 5);
    req(!directoryPath.success()
            && directoryPath.error()->code == "workspace_scope.diagnostic_read_not_in_baseline",
        "A directory-shaped diagnostic-read path (never a baseline file entry) was accepted");
}

void testDiagnosticReadRejectsMissingBaselinePath()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/DoesNotExist.qml", "verification.qml_warning",
        "This file does not actually exist in the baseline", policy, baseline, 5);
    req(!result.success(), "A diagnostic read of a nonexistent baseline path was accepted");
    req(result.error()->code == "workspace_scope.diagnostic_read_not_in_baseline",
        "Expected the diagnostic_read_not_in_baseline failure code");
}

void testDiagnosticReadRejectsBackendPolicyDeniedPath()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    // ".env" is denied by WorkspaceReadPolicy::secureDefaults's own
    // deny-list (see testRequestExpansionRejectsGloballyExcludedPath above)
    // - a genuine Backend security denial, structurally distinct from a
    // Planner-authored task exclusion.
    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", ".env", "verification.qml_warning",
        "Trying to read a secret file", policy, baseline, 5);
    req(!result.success(), "A Backend-security-denied diagnostic-read path was accepted");
    req(result.error()->code == "workspace_scope.diagnostic_read_policy_denied",
        "Expected the diagnostic_read_policy_denied failure code - never diagnostic_read_invalid_path, "
        "which would wrongly conflate a security denial with a path-shape problem");
}

void testDiagnosticReadEnforcesLimit()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({
        {"qml/app/Theme.qml", "hash-0"},
        {"qml/app/Palette.qml", "hash-1"},
        {"qml/app/Metrics.qml", "hash-2"},
    });
    freezeTaskScope(service, baseline);

    const std::vector<std::string> paths = {
        "qml/app/Theme.qml", "qml/app/Palette.qml"};
    for (const auto& path : paths) {
        const auto result = service.requestDiagnosticRead(
            "run-1", "implement", path, "verification.qml_warning",
            "Genuinely needed for this finding", policy, baseline, /*maxDiagnosticReadsForTask=*/2);
        req(result.success(), "A diagnostic read within the bounded limit was rejected");
    }
    const auto overLimit = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/Metrics.qml", "verification.qml_warning",
        "One too many", policy, baseline, /*maxDiagnosticReadsForTask=*/2);
    req(!overLimit.success(), "A diagnostic read beyond the bounded per-task limit was accepted");
    req(overLimit.error()->code == "workspace_scope.diagnostic_read_limit_exceeded",
        "Expected the diagnostic_read_limit_exceeded failure code");
}

void testDiagnosticReadRetryIsIdempotent()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});
    freezeTaskScope(service, baseline);

    const auto first = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/Theme.qml", "verification.qml_warning",
        "First request", policy, baseline, 5);
    const auto retry = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/Theme.qml", "verification.qml_warning",
        "A different reason text on retry", policy, baseline, 5);
    req(first.success() && retry.success() && first.value().id == retry.value().id,
        "A duplicate diagnostic-read request did not deterministically return the original grant");

    const auto listed = service.listDiagnosticReadsForRun("run-1");
    req(listed.success() && listed.value().size() == 1,
        "A duplicate diagnostic-read request must never record a second grant");
}

void testExcludedPathMayReceiveDiagnosticReadButOrdinaryExpansionStillRefusesIt()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{"qml/app/Theme.qml", "hash-theme"}});

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto task = makeTask("implement");
    task.excludedPaths.push_back({"qml/app/Theme.qml", "Design system, not owned by this task"});
    manifest.tasks = {std::move(task)};
    req(service.freezeForManifest("run-1", manifest, baseline).success(),
        "Could not freeze excluded-path fixture");

    // The ordinary, worker-facing mechanism still refuses this path exactly
    // as before this package - a task-level Excluded classification is not
    // silently reinterpreted as a normal required_read scope-growth target.
    const auto ordinaryExpansion = service.requestExpansion(
        "run-1", "implement", "qml/app/Theme.qml", WorkspaceScopeRole::RequiredRead,
        "Trying the ordinary mechanism instead", policy, baseline, 5);
    req(!ordinaryExpansion.success()
            && ordinaryExpansion.error()->code == "workspace_scope.expansion_excluded_path",
        "Ordinary request_scope_expansion must still refuse a frozen Excluded path");

    // The repair-only diagnostic-read mechanism is the one explicit,
    // durably-audited exception.
    const auto diagnosticRead = service.requestDiagnosticRead(
        "run-1", "implement", "qml/app/Theme.qml", "verification.qml_warning",
        "The bounded verification-repair attempt for this Excluded declaration",
        policy, baseline, 5);
    req(diagnosticRead.success(),
        "The audited diagnostic-read exception must be able to reach an Excluded baseline file");
}

void testSecurityDeniedPathCannotReceiveDiagnosticReadEvenWhenAlsoExcluded()
{
    InMemoryRunWorkspaceScopeRepository repository;
    RunWorkspaceScopeService service(repository);
    const auto policy = makePolicy();
    const auto baseline = makeBaseline({{".env", "hash-env"}});

    ExecutionManifest manifest;
    manifest.runId = "run-1";
    auto task = makeTask("implement");
    // Frozen Excluded (a Planner decision) AND independently denied by the
    // authoritative WorkspaceReadPolicy (a Backend security decision) - the
    // diagnostic-read exception overrides only the former, never the latter.
    task.excludedPaths.push_back({".env", "Secret file"});
    manifest.tasks = {std::move(task)};
    req(service.freezeForManifest("run-1", manifest, baseline).success(),
        "Could not freeze excluded-path fixture");

    const auto result = service.requestDiagnosticRead(
        "run-1", "implement", ".env", "verification.qml_warning",
        "Trying to read a secret file even though it is also Planner-excluded",
        policy, baseline, 5);
    req(!result.success()
            && result.error()->code == "workspace_scope.diagnostic_read_policy_denied",
        "A genuine Backend security denial must never be overridden by the diagnostic-read "
        "exception, even for a path that is also Planner-excluded");
}

} // namespace

int runRunWorkspaceScopeServiceTests()
{
    testFreezeAttachesRealBaselineHashesAndDerivesDependencyEdges();
    testFreezeRejectsPlannedCreateThatAlreadyExistsInBaseline();
    testFreezeRejectsOwnedWriteMissingFromBaseline();
    testFreezeIsIdempotentOnRetry();
    testRequestExpansionAllowsSafeRequiredReadDependency();
    testRequestExpansionRejectsWriteRole();
    testRequestExpansionRejectsGloballyExcludedPath();
    testRequestExpansionRejectsPathTraversal();
    testRequestExpansionEnforcesHardLimit();
    testRequestExpansionRejectsBlankReason();
    testRequestExpansionRequiresFrozenScopeAndExistingFile();
    testRequestExpansionCannotActivateExcludedPath();
    testRequestExpansionRetryIsIdempotentButRoleCannotChange();
    testMissingScopeDependencyNamesConsumerProducerAndPathForOwnedPath();
    testMissingScopeDependencyDetectsPlannedCreateProducer();
    testMissingScopeDependencyMessageLeaksNoAbsolutePathOrRoot();
    testDeclaredScopeDependencyPassesValidation();
    testDiagnosticReadOfExistingBaselineFileSucceeds();
    testDiagnosticReadNeverMutatesFrozenScopeOrGrantsWriteAccess();
    testDiagnosticReadIsDurablyRecorded();
    testDiagnosticReadRejectsAbsolutePath();
    testDiagnosticReadRejectsParentTraversal();
    testDiagnosticReadRejectsRepositoryRootAndDirectory();
    testDiagnosticReadRejectsMissingBaselinePath();
    testDiagnosticReadRejectsBackendPolicyDeniedPath();
    testDiagnosticReadEnforcesLimit();
    testDiagnosticReadRetryIsIdempotent();
    testExcludedPathMayReceiveDiagnosticReadButOrdinaryExpansionStillRefusesIt();
    testSecurityDeniedPathCannotReceiveDiagnosticReadEvenWhenAlsoExcluded();
    return 0;
}
