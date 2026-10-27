#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace {

using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;

void requireManifestRepository(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct ManifestWorkspace {
    fs::path root {fs::temp_directory_path()
        / ("ai-orchestrator-manifest-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~ManifestWorkspace() { std::error_code ignored; fs::remove_all(root, ignored); }
};

ExecutionManifest manifestFor(const RunId& runId)
{
    ExecutionManifest value;
    value.runId = runId;
    value.requestedMaxParallelism = 2;
    value.requestedTokenBudget = 12'345;
    value.requestedCostMicros = 654'321;
    value.policy.maxTasks = 5;
    value.policy.maxDepth = 3;
    value.policy.maxParallelism = 2;
    value.policy.maxOwnedPathsPerTask = 9;
    value.policy.maxAcceptanceCriteriaPerTask = 7;
    value.policy.maxContextReferencesPerTask = 6;
    value.policy.maximumCapability = WorkspaceCapability::Propose;
    value.policy.tokenBudgetCeiling = 20'000;
    value.policy.costMicrosCeiling = 700'000;

    ExecutionManifestTask first;
    first.id = "analysis";
    first.objective = "Inspect persisted behavior";
    first.role = "analyst";
    first.ownedPaths = {fs::path("source") / "read"};
    first.acceptanceCriteria = {"Evidence captured"};
    first.contextReferences = {"context/backend/CODEX_CONTEXT.md"};
    first.requestedCapability = WorkspaceCapability::Inspect;

    ExecutionManifestTask second;
    second.id = "proposal";
    second.objective = "Prepare a bounded proposal";
    second.role = "implementer";
    second.dependencies = {"analysis"};
    second.ownedPaths = {fs::path("source") / "write"};
    // AG-15: a real Run (bb56e001-eac0-4b59-8d0b-d037c6bc1031) froze a
    // RunWorkspaceScope with real planned_create entries, proving the
    // in-memory manifest had these six fields populated at freeze time -
    // yet the reloaded manifest showed none, because the manifest
    // serialization (now sqlite/execution/SqliteExecutionManifestRepository.cpp)
    // only ever wrote ownedPaths. Exercise every field so a partial
    // round-trip cannot pass silently again.
    second.plannedCreatePaths = {fs::path("source") / "created"};
    second.requiredReadPaths = {fs::path("source") / "read"};
    second.referencePaths = {fs::path("docs") / "reference.md"};
    second.excludedPaths = {
        {fs::path("docs") / "excluded.md", "Out of scope for this task"}};
    second.deferredPaths = {
        {fs::path("docs") / "deferred.md", "Deferred to a later Run"}};
    second.baselineOnlyReadPaths = {fs::path("source") / "read"};
    second.acceptanceCriteria = {"Round-trip is lossless"};
    second.contextReferences = {"REMAINING_GAPS_EXECUTION_PLAN.md"};
    second.requestedCapability = WorkspaceCapability::Propose;
    // A real Run showed this never round-tripped at all: reopening a frozen
    // manifest silently dropped every task's structured acceptanceContract,
    // which surfaced downstream as an unrecoverable combined-verification
    // failure with no diagnostic (the contract verification needed was
    // already gone from what got persisted). Exercise every field, including
    // a non-empty verificationArguments item, so a partial round-trip cannot
    // pass silently.
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "lint-clean";
    requirement.description = "qml lint reports no new warnings";
    requirement.requiredOutputs = {"qml/CMakeLists.txt"};
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {
        {"buildDirectory", "out/build/x64-debug"}, {"target", "all_qmllint"}};
    requirement.required = true;
    // AG-18.2: the Backend-resolved selection identity that produced the
    // arguments above must survive a restart unchanged - a restarted
    // process reads back the same immutable binding, never rediscovering
    // or re-resolving it.
    requirement.verificationProfileId = "local-qt-debug";
    requirement.verificationBuildProfileId = "local-qt-debug";
    requirement.verificationTargetId = "all_qmllint";
    requirement.qmlLintDiagnosticPolicyId =
        std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
    ExecutionAcceptanceContract contract;
    contract.requirements = {std::move(requirement)};
    second.acceptanceContract = std::move(contract);
    value.tasks = {std::move(first), std::move(second)};
    return value;
}

void seedRun(const std::shared_ptr<SqliteDatabase>& database)
{
    const auto now = std::chrono::system_clock::now();
    SqliteProjectRepository projects(database);
    Project project {"manifest-project", "Manifest project", "Phase 2",
        database->path().parent_path() / "project", ProjectStatus::Active, now, now};
    requireManifestRepository(projects.insert(project).success(), "Could not seed project");

    SqliteRunRepository runs(database);
    Run run;
    run.id = "manifest-run";
    run.projectId = project.id;
    run.title = "Manifest persistence";
    run.task = "Round-trip frozen manifest";
    run.workflowId = "mock-sequential-v1";
    run.createdAt = now;
    requireManifestRepository(runs.insertRun(run).success(), "Could not seed Run");
}

void testManifestRoundTripAndDatabaseImmutability()
{
    ManifestWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        requireManifestRepository(opened.success(), "Could not open v28 database");
        seedRun(opened.value());
        SqliteRunRepository runs(opened.value());
        SqliteExecutionManifestRepository manifests(opened.value());
        ExecutionManifestService service(manifests, runs);
        auto manifest = manifestFor("manifest-run");
        const auto trustedPolicy = manifest.policy;
        const auto frozen = service.freeze(std::move(manifest), trustedPolicy);
        requireManifestRepository(frozen.success(), "Could not freeze SQLite manifest");
    }

    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        requireManifestRepository(reopened.success(), "Could not reopen v28 database");
        SqliteExecutionManifestRepository manifests(reopened.value());
        const auto read = manifests.findForRun("manifest-run");
        requireManifestRepository(read.success() && read.value().has_value(),
            "Manifest did not survive reopen");
        const auto& value = *read.value();
        requireManifestRepository(value.schemaVersion == kExecutionManifestSchemaVersion,
            "Schema version changed during round-trip");
        requireManifestRepository(value.tasks.size() == 2
                && value.tasks[1].dependencies == std::vector<std::string>{"analysis"},
            "Task DAG changed during round-trip");
        requireManifestRepository(value.tasks[0].ownedPaths.front() == fs::path("source/read"),
            "Owned path changed during round-trip");
        const auto& roundTrippedSecond = value.tasks[1];
        requireManifestRepository(
            roundTrippedSecond.plannedCreatePaths == std::vector<fs::path>{fs::path("source/created")},
            "plannedCreatePaths did not survive reopen");
        requireManifestRepository(
            roundTrippedSecond.requiredReadPaths == std::vector<fs::path>{fs::path("source/read")},
            "requiredReadPaths did not survive reopen");
        requireManifestRepository(
            roundTrippedSecond.referencePaths == std::vector<fs::path>{fs::path("docs/reference.md")},
            "referencePaths did not survive reopen");
        requireManifestRepository(
            roundTrippedSecond.excludedPaths.size() == 1
                && roundTrippedSecond.excludedPaths.front().path == fs::path("docs/excluded.md")
                && roundTrippedSecond.excludedPaths.front().reason == "Out of scope for this task",
            "excludedPaths did not survive reopen");
        requireManifestRepository(
            roundTrippedSecond.deferredPaths.size() == 1
                && roundTrippedSecond.deferredPaths.front().path == fs::path("docs/deferred.md")
                && roundTrippedSecond.deferredPaths.front().reason == "Deferred to a later Run",
            "deferredPaths did not survive reopen");
        requireManifestRepository(
            roundTrippedSecond.baselineOnlyReadPaths == std::vector<fs::path>{fs::path("source/read")},
            "baselineOnlyReadPaths did not survive reopen");
        requireManifestRepository(value.policy.maxOwnedPathsPerTask == 9
                && value.policy.tokenBudgetCeiling == 20'000
                && value.requestedCostMicros == 654'321,
            "Policy or budget snapshot changed during round-trip");
        requireManifestRepository(!value.tasks[0].acceptanceContract.has_value(),
            "A task with no acceptanceContract must stay absent, not gain one");
        requireManifestRepository(value.tasks[1].acceptanceContract.has_value(),
            "acceptanceContract did not survive reopen");
        const auto& roundTrippedContract = *value.tasks[1].acceptanceContract;
        requireManifestRepository(roundTrippedContract.requirements.size() == 1,
            "acceptanceContract requirement count changed during round-trip");
        const auto& roundTrippedRequirement = roundTrippedContract.requirements.front();
        requireManifestRepository(roundTrippedRequirement.id == "lint-clean"
                && roundTrippedRequirement.description
                    == "qml lint reports no new warnings"
                && roundTrippedRequirement.requiredOutputs
                    == std::vector<std::string> {"qml/CMakeLists.txt"}
                && roundTrippedRequirement.verificationKind
                    == ExecutionVerificationKind::QmlLint
                && roundTrippedRequirement.required,
            "acceptanceContract requirement fields changed during round-trip");
        requireManifestRepository(roundTrippedRequirement.verificationArguments.size() == 2
                && roundTrippedRequirement.verificationArguments[0].name == "buildDirectory"
                && roundTrippedRequirement.verificationArguments[0].value
                    == "out/build/x64-debug"
                && roundTrippedRequirement.verificationArguments[1].name == "target"
                && roundTrippedRequirement.verificationArguments[1].value == "all_qmllint",
            "acceptanceContract verificationArguments changed during round-trip");
        requireManifestRepository(
            roundTrippedRequirement.verificationProfileId == "local-qt-debug"
                && roundTrippedRequirement.verificationBuildProfileId == "local-qt-debug"
                && roundTrippedRequirement.verificationTargetId == "all_qmllint"
                && roundTrippedRequirement.qmlLintDiagnosticPolicyId
                    == std::optional<std::string>(
                        std::string(kQmlLintDiagnosticPolicyAdvisoryV1)),
            "AG-18.2 verification selection identity did not survive reopen - a restarted "
            "process must read back the same immutable binding and QML-lint policy, never "
            "rediscover either");
    }

    sqlite3* raw = nullptr;
    requireManifestRepository(
        sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw SQLite verification connection");
    const int updateResult = sqlite3_exec(raw,
        "UPDATE execution_manifests SET schema_version='tampered' "
        "WHERE run_id='manifest-run';", nullptr, nullptr, nullptr);
    requireManifestRepository(updateResult == SQLITE_CONSTRAINT,
        "Database did not enforce execution manifest immutability");
    sqlite3_close_v2(raw);
}

// AG-18.2 hardening follow-up: ExecutionManifestService::freeze() is a
// public method - it must reject an unbound draft at the freeze boundary
// itself, not merely rely on the Planner's own call path having already run
// ManifestVerificationProfileBinder first. A requirement with a selected
// profile/build-profile id but still-empty verificationArguments (the exact
// shape a fresh, never-bound provider submission has) must never be
// persisted as a frozen Manifest by any caller.
void testFreezeRejectsUnboundDraftVerificationSelection()
{
    ManifestWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(opened.success(), "Could not open database");
    seedRun(opened.value());
    SqliteRunRepository runs(opened.value());
    SqliteExecutionManifestRepository manifests(opened.value());
    ExecutionManifestService service(manifests, runs);

    auto manifest = manifestFor("manifest-run");
    // Overwrite the already-resolved fixture requirement with a draft-shaped
    // one: a selected profile/build-profile/target id, but no
    // ManifestVerificationProfileBinder has run, so verificationArguments
    // is still empty - exactly what freeze() must now reject.
    ExecutionAcceptanceRequirement draftRequirement;
    draftRequirement.id = "unbound";
    draftRequirement.description = "Never resolved by the binder";
    draftRequirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    draftRequirement.required = true;
    draftRequirement.verificationProfileId = "local-qt-debug";
    draftRequirement.verificationBuildProfileId = "local-qt-debug";
    ExecutionAcceptanceContract draftContract;
    draftContract.requirements = {std::move(draftRequirement)};
    manifest.tasks[1].acceptanceContract = std::move(draftContract);

    const auto trustedPolicy = manifest.policy;
    const auto frozen = service.freeze(std::move(manifest), trustedPolicy);
    requireManifestRepository(!frozen.success(),
        "freeze() accepted a manifest whose verification selection was never resolved by "
        "ManifestVerificationProfileBinder - the freeze boundary must reject an unbound draft "
        "regardless of which caller reaches it");

    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(read.success() && !read.value().has_value(),
        "A rejected unbound-draft freeze attempt must not persist any manifest row");
}

// P1 #6: fail-closed rejection of a fresh cmake_target_consistency
// submission (ExecutionManifestParser.cpp) must never break reading or
// validating an already-persisted legacy Manifest that already froze this
// kind before the restriction existed - freeze() itself (the SQLite
// round-trip's write boundary) and the internal verifier both keep working
// against a fully-resolved, Backend-shaped legacy requirement.
void testCmakeTargetConsistencyLegacyRequirementRoundTrips()
{
    ManifestWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        requireManifestRepository(opened.success(), "Could not open database");
        seedRun(opened.value());
        SqliteRunRepository runs(opened.value());
        SqliteExecutionManifestRepository manifests(opened.value());
        ExecutionManifestService service(manifests, runs);

        auto manifest = manifestFor("manifest-run");
        ExecutionAcceptanceRequirement requirement;
        requirement.id = "registration";
        requirement.description = "CMake target registration is consistent";
        requirement.verificationKind = ExecutionVerificationKind::CMakeTargetConsistency;
        requirement.required = true;
        requirement.verificationTargetId = "Dashboard_Content";
        requirement.verificationArguments = {
            {"definitionFile", "CMakeLists.txt"},
            {"registrationFile", "qml/CMakeLists.txt"},
            {"target", "Dashboard_Content"}};
        ExecutionAcceptanceContract contract;
        contract.requirements = {std::move(requirement)};
        manifest.tasks[1].acceptanceContract = std::move(contract);

        const auto trustedPolicy = manifest.policy;
        const auto frozen = service.freeze(std::move(manifest), trustedPolicy);
        requireManifestRepository(frozen.success(),
            "A legacy/trusted cmake_target_consistency requirement must still freeze "
            "successfully - only fresh Planner submissions are rejected");
    }

    const auto reopened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(reopened.success(), "Could not reopen database");
    SqliteExecutionManifestRepository manifests(reopened.value());
    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(read.success() && read.value().has_value(),
        "Legacy cmake_target_consistency manifest did not survive reopen");
    const auto& roundTrippedRequirement =
        read.value()->tasks[1].acceptanceContract->requirements.front();
    requireManifestRepository(
        roundTrippedRequirement.verificationKind
                == ExecutionVerificationKind::CMakeTargetConsistency
            && roundTrippedRequirement.verificationArguments.size() == 3,
        "Legacy cmake_target_consistency requirement changed shape during round-trip");
}

// AG-15 fix regression: a manifest persisted before this fix never wrote
// plannedCreatePaths/requiredReadPaths/referencePaths/excludedPaths/
// deferredPaths/baselineOnlyReadPaths at all. Reading such a legacy row back
// must stay backward compatible - defaulting the six fields to empty -
// rather than throwing manifest.corrupt_snapshot.
void testLegacyManifestJsonMissingAg15FieldsStillReads()
{
    ManifestWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(opened.success(), "Could not open database");
    seedRun(opened.value());

    constexpr const char* legacyManifestJson = R"json({
        "schemaVersion": "execution-manifest.v1",
        "runId": "manifest-run",
        "requestedMaxParallelism": 1,
        "requestedTokenBudget": null,
        "requestedCostMicros": null,
        "policy": {
            "maxTasks": 5, "maxDepth": 3, "maxParallelism": 2,
            "maxOwnedPathsPerTask": 9, "maxAcceptanceCriteriaPerTask": 7,
            "maxContextReferencesPerTask": 6, "maximumCapability": "propose",
            "tokenBudgetCeiling": null, "costMicrosCeiling": null
        },
        "tasks": [{
            "id": "legacy", "objective": "Pre-fix persisted task",
            "role": "analyst", "dependencies": [],
            "ownedPaths": ["source/read"], "acceptanceCriteria": [],
            "contextReferences": [], "requestedCapability": "inspect"
        }]
    })json";

    sqlite3* raw = nullptr;
    requireManifestRepository(
        sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw SQLite connection");
    sqlite3_stmt* statement = nullptr;
    requireManifestRepository(
        sqlite3_prepare_v2(raw,
            "INSERT INTO execution_manifests (run_id, schema_version, manifest_json, "
            "created_at_ms) VALUES ('manifest-run', 'execution-manifest.v1', ?, 0);",
            -1, &statement, nullptr) == SQLITE_OK,
        "Could not prepare legacy manifest insert");
    sqlite3_bind_text(statement, 1, legacyManifestJson, -1, SQLITE_TRANSIENT);
    requireManifestRepository(sqlite3_step(statement) == SQLITE_DONE,
        "Could not insert legacy-shaped manifest row");
    sqlite3_finalize(statement);
    sqlite3_close_v2(raw);

    SqliteExecutionManifestRepository manifests(opened.value());
    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(read.success() && read.value().has_value(),
        "A legacy manifest row missing AG-15 fields must still read successfully");
    const auto& task = read.value()->tasks.front();
    requireManifestRepository(
        task.plannedCreatePaths.empty() && task.requiredReadPaths.empty()
            && task.referencePaths.empty() && task.excludedPaths.empty()
            && task.deferredPaths.empty() && task.baselineOnlyReadPaths.empty(),
        "A legacy manifest row must default missing AG-15 fields to empty, not fail");
}

// AG-17.0: a requirement's origin and the Manifest's frozen mandatory-
// verification record round-trip exactly; the reader never re-derives them.
void testMandatoryVerificationRecordAndOriginRoundTrip()
{
    ManifestWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        requireManifestRepository(opened.success(), "Could not open database");
        seedRun(opened.value());
        SqliteRunRepository runs(opened.value());
        SqliteExecutionManifestRepository manifests(opened.value());
        ExecutionManifestService service(manifests, runs);

        auto manifest = manifestFor("manifest-run");
        manifest.tasks[1].acceptanceContract->requirements.front().origin =
            ExecutionRequirementOrigin::BackendMandatory;
        ManifestMandatoryVerificationRecord record;
        ManifestMandatoryVerificationDecision lint;
        lint.kind = ExecutionVerificationKind::QmlLint;
        lint.status = MandatoryVerificationDecisionStatus::Injected;
        lint.reasonCode = "qml_paths_written";
        lint.requirementId = "lint-clean";
        lint.taskId = "proposal";
        lint.profileId = "local-qt-debug";
        lint.buildProfileId = "local-qt-debug";
        lint.targetId = "all_qmllint";
        ManifestMandatoryVerificationDecision build;
        build.kind = ExecutionVerificationKind::CMakeBuild;
        build.status = MandatoryVerificationDecisionStatus::NotApplicable;
        build.reasonCode = "no_build_profile";
        record.decisions = {lint, build};
        manifest.mandatoryVerification = std::move(record);

        const auto trustedPolicy = manifest.policy;
        requireManifestRepository(service.freeze(std::move(manifest), trustedPolicy).success(),
            "Could not freeze a manifest carrying a mandatory verification record");
    }
    const auto reopened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(reopened.success(), "Could not reopen database");
    SqliteExecutionManifestRepository manifests(reopened.value());
    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(read.success() && read.value().has_value(),
        "Manifest with a mandatory verification record did not survive reopen");
    const auto& requirement = read.value()->tasks[1].acceptanceContract->requirements.front();
    requireManifestRepository(requirement.origin == ExecutionRequirementOrigin::BackendMandatory,
        "backend_mandatory origin did not survive a round trip");
    requireManifestRepository(read.value()->mandatoryVerification.has_value(),
        "The frozen mandatory verification record was lost");
    const auto& record = *read.value()->mandatoryVerification;
    requireManifestRepository(record.policyId == kManifestMandatoryVerificationPolicyId
            && record.mode == kManifestMandatoryVerificationModeAuto && record.decisions.size() == 2,
        "The mandatory verification record header or decision count changed");
    const auto& lint = record.decisions[0];
    requireManifestRepository(lint.kind == ExecutionVerificationKind::QmlLint
            && lint.status == MandatoryVerificationDecisionStatus::Injected
            && lint.reasonCode == "qml_paths_written" && lint.requirementId == "lint-clean"
            && lint.taskId == "proposal" && lint.profileId == "local-qt-debug"
            && lint.buildProfileId == "local-qt-debug" && lint.targetId == "all_qmllint",
        "The lint decision changed during the round trip");
    const auto& build = record.decisions[1];
    requireManifestRepository(build.kind == ExecutionVerificationKind::CMakeBuild
            && build.status == MandatoryVerificationDecisionStatus::NotApplicable
            && build.reasonCode == "no_build_profile" && build.requirementId.empty(),
        "The build decision changed during the round trip");
}

// A Manifest frozen before AG-17.0 has neither key: it reads back with every
// requirement planner_declared and no record, and is never re-derived.
void testLegacyManifestWithoutOriginOrRecordReadsAsPlannerDeclared()
{
    ManifestWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(opened.success(), "Could not open database");
    seedRun(opened.value());

    constexpr const char* legacyManifestJson = R"json({
        "schemaVersion": "execution-manifest.v1",
        "runId": "manifest-run",
        "requestedMaxParallelism": 1,
        "requestedTokenBudget": null,
        "requestedCostMicros": null,
        "policy": {
            "maxTasks": 5, "maxDepth": 3, "maxParallelism": 2,
            "maxOwnedPathsPerTask": 9, "maxAcceptanceCriteriaPerTask": 7,
            "maxContextReferencesPerTask": 6, "maximumCapability": "propose",
            "tokenBudgetCeiling": null, "costMicrosCeiling": null
        },
        "tasks": [{
            "id": "legacy", "objective": "Pre-AG-17.0 persisted task",
            "role": "analyst", "dependencies": [],
            "ownedPaths": ["qml/A.qml"], "acceptanceCriteria": [],
            "contextReferences": [], "requestedCapability": "propose",
            "acceptanceContract": {
                "schemaVersion": "execution-acceptance.v1",
                "requirements": [{
                    "id": "lint", "description": "lint", "requiredOutputs": [],
                    "verificationKind": "qml_lint",
                    "verificationArguments": [
                        {"name": "buildDirectory", "value": "build/debug"},
                        {"name": "target", "value": "all_qmllint"}],
                    "required": true,
                    "verificationProfileId": "debug",
                    "verificationBuildProfileId": "debug",
                    "verificationTargetId": "all_qmllint"
                }]
            }
        }]
    })json";

    sqlite3* raw = nullptr;
    requireManifestRepository(
        sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw SQLite connection");
    sqlite3_stmt* statement = nullptr;
    requireManifestRepository(
        sqlite3_prepare_v2(raw,
            "INSERT INTO execution_manifests (run_id, schema_version, manifest_json, "
            "created_at_ms) VALUES ('manifest-run', 'execution-manifest.v1', ?, 0);",
            -1, &statement, nullptr) == SQLITE_OK,
        "Could not prepare legacy manifest insert");
    sqlite3_bind_text(statement, 1, legacyManifestJson, -1, SQLITE_TRANSIENT);
    requireManifestRepository(sqlite3_step(statement) == SQLITE_DONE,
        "Could not insert legacy-shaped manifest row");
    sqlite3_finalize(statement);
    sqlite3_close_v2(raw);

    SqliteExecutionManifestRepository manifests(opened.value());
    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(read.success() && read.value().has_value(),
        "A legacy manifest without origin/mandatoryVerification must still read");
    requireManifestRepository(!read.value()->mandatoryVerification.has_value(),
        "A legacy manifest must read with no mandatory verification record");
    // AG-17.1: a pre-routing row has neither a routing record nor an assignee.
    requireManifestRepository(!read.value()->routing.has_value()
            && !read.value()->tasks.front().assigneeMemberId.has_value(),
        "A legacy manifest must read with no routing record and no assignee");
    requireManifestRepository(
        read.value()->tasks.front().acceptanceContract->requirements.front().origin
            == ExecutionRequirementOrigin::PlannerDeclared,
        "A legacy requirement without an origin must read as planner_declared");
}

// AG-17.1: a three-member roster (lead + two workspace workers) as the Backend
// would freeze it from the Run's assignment snapshot.
ManifestRoutingRecord rosterRoutingRecord()
{
    const auto member = [](const char* id, const char* provider, const char* model, bool lead,
                           bool workspace, std::uint32_t position, const char* instructions) {
        StageModelAssignment value;
        value.stageType = id;
        value.providerId = provider;
        value.modelId = model;
        value.role = id;
        value.leadArchitect = lead;
        value.workspaceAccess = workspace;
        value.instructions = instructions;
        value.position = position;
        value.timeout = std::chrono::milliseconds(120000);
        return value;
    };
    return ManifestRouting::fromAssignments({
        member("architect", "openai", "gpt-a", true, false, 0, "Plan."),
        member("qml-worker", "openai", "gpt-b", false, true, 1, "Write QML."),
        member("cpp-worker", "anthropic", "claude-a", false, true, 2, "Write C++.")});
}

void testRoutingRecordAndAssigneeRoundTrip()
{
    const auto record = rosterRoutingRecord();
    ManifestWorkspace workspace;
    {
        const auto opened = SqliteDatabase::open(workspace.database);
        requireManifestRepository(opened.success(), "Could not open database");
        seedRun(opened.value());
        SqliteRunRepository runs(opened.value());
        SqliteExecutionManifestRepository manifests(opened.value());
        ExecutionManifestService service(manifests, runs);
        auto manifest = manifestFor("manifest-run");
        manifest.routing = record;
        manifest.tasks[0].assigneeMemberId = "qml-worker"; // Inspect
        manifest.tasks[1].assigneeMemberId = "cpp-worker"; // Propose
        const auto trustedPolicy = manifest.policy;
        const auto frozen = service.freeze(std::move(manifest), trustedPolicy);
        requireManifestRepository(frozen.success(),
            "Could not freeze a valid roster routing record with assignees");
    }
    const auto reopened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(reopened.success(), "Could not reopen database");
    SqliteExecutionManifestRepository manifests(reopened.value());
    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(read.success() && read.value().has_value(),
        "Manifest with a routing record did not survive reopen");
    requireManifestRepository(read.value()->routing.has_value(), "The routing record was lost");
    const auto& got = *read.value()->routing;
    requireManifestRepository(got.schemaVersion == record.schemaVersion && got.mode == record.mode
            && got.fingerprint == record.fingerprint && got.members.size() == record.members.size(),
        "The routing record header changed during the round trip");
    for (std::size_t index = 0; index < record.members.size(); ++index) {
        const auto& a = got.members[index];
        const auto& b = record.members[index];
        requireManifestRepository(a.memberId == b.memberId && a.providerId == b.providerId
                && a.modelId == b.modelId && a.role == b.role && a.leadArchitect == b.leadArchitect
                && a.workspaceAccess == b.workspaceAccess && a.instructionsHash == b.instructionsHash
                && a.position == b.position && a.timeoutMs == b.timeoutMs,
            "A routing member changed during the round trip");
    }
    requireManifestRepository(!ManifestRouting::recordProblem(got).has_value(),
        "The reloaded routing record is no longer self-consistent");
    requireManifestRepository(read.value()->tasks[0].assigneeMemberId == std::optional<std::string>("qml-worker")
            && read.value()->tasks[1].assigneeMemberId == std::optional<std::string>("cpp-worker"),
        "A task's assigneeMemberId did not survive the round trip");
}

void testFreezeRejectsATamperedRoutingRecord()
{
    ManifestWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(opened.success(), "Could not open database");
    seedRun(opened.value());
    SqliteRunRepository runs(opened.value());
    SqliteExecutionManifestRepository manifests(opened.value());
    ExecutionManifestService service(manifests, runs);
    auto manifest = manifestFor("manifest-run");
    manifest.routing = rosterRoutingRecord();
    manifest.routing->members[1].role = "tampered"; // the fingerprint is now stale
    manifest.tasks[0].assigneeMemberId = "qml-worker";
    manifest.tasks[1].assigneeMemberId = "cpp-worker";
    const auto trustedPolicy = manifest.policy;
    const auto frozen = service.freeze(std::move(manifest), trustedPolicy);
    requireManifestRepository(!frozen.success() && frozen.error()->code == "manifest.routing_invalid",
        "A routing record whose fingerprint does not match must not freeze");
    requireManifestRepository(!manifests.findForRun("manifest-run").value().has_value(),
        "A rejected Manifest must leave nothing persisted");
}

void testCorruptRoutingModeFailsClosed()
{
    ManifestWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(opened.success(), "Could not open database");
    seedRun(opened.value());
    SqliteRunRepository runs(opened.value());
    SqliteExecutionManifestRepository manifests(opened.value());
    ExecutionManifestService service(manifests, runs);
    auto manifest = manifestFor("manifest-run");
    manifest.routing = rosterRoutingRecord();
    manifest.tasks[0].assigneeMemberId = "qml-worker";
    manifest.tasks[1].assigneeMemberId = "cpp-worker";
    const auto trustedPolicy = manifest.policy;
    requireManifestRepository(service.freeze(std::move(manifest), trustedPolicy).success(),
        "Could not freeze the manifest");

    sqlite3* raw = nullptr;
    requireManifestRepository(
        sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw SQLite connection");
    requireManifestRepository(sqlite3_exec(raw,
        "DROP TRIGGER execution_manifests_reject_update;", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not drop the immutability trigger for the test");
    requireManifestRepository(sqlite3_exec(raw,
        "UPDATE execution_manifests SET manifest_json = replace(manifest_json, "
        "'\"mode\":\"roster\"', '\"mode\":\"made_up\"');",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt the routing mode");
    sqlite3_close_v2(raw);
    requireManifestRepository(!manifests.findForRun("manifest-run").success(),
        "A routing record with an unknown mode must fail closed, not default");
}

void testCorruptRequirementOriginFailsClosed()
{
    ManifestWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireManifestRepository(opened.success(), "Could not open database");
    seedRun(opened.value());
    SqliteRunRepository runs(opened.value());
    SqliteExecutionManifestRepository manifests(opened.value());
    ExecutionManifestService service(manifests, runs);
    auto manifest = manifestFor("manifest-run");
    const auto trustedPolicy = manifest.policy;
    requireManifestRepository(service.freeze(std::move(manifest), trustedPolicy).success(),
        "Could not freeze the manifest");

    sqlite3* raw = nullptr;
    requireManifestRepository(
        sqlite3_open(workspace.database.string().c_str(), &raw) == SQLITE_OK,
        "Could not open raw SQLite connection");
    requireManifestRepository(sqlite3_exec(raw,
        "DROP TRIGGER execution_manifests_reject_update;", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not drop the immutability trigger for the test");
    requireManifestRepository(sqlite3_exec(raw,
        "UPDATE execution_manifests SET manifest_json = replace(manifest_json, "
        "'\"origin\":\"planner_declared\"', '\"origin\":\"made_up\"');",
        nullptr, nullptr, nullptr) == SQLITE_OK, "Could not corrupt the origin");
    sqlite3_close_v2(raw);

    const auto read = manifests.findForRun("manifest-run");
    requireManifestRepository(!read.success(),
        "A requirement with an unknown origin must fail closed, not default");
}

} // namespace

int runExecutionManifestRepositoryTests()
{
    testManifestRoundTripAndDatabaseImmutability();
    testFreezeRejectsUnboundDraftVerificationSelection();
    testCmakeTargetConsistencyLegacyRequirementRoundTrips();
    testLegacyManifestJsonMissingAg15FieldsStillReads();
    testMandatoryVerificationRecordAndOriginRoundTrip();
    testLegacyManifestWithoutOriginOrRecordReadsAsPlannerDeclared();
    testCorruptRequirementOriginFailsClosed();
    testRoutingRecordAndAssigneeRoundTrip();
    testFreezeRejectsATamperedRoutingRecord();
    testCorruptRoutingModeFailsClosed();
    return 0;
}
