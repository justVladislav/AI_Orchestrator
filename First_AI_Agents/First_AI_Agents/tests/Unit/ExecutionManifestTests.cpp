#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/ExecutionManifestParser.hpp"
#include "ai/application/ExecutionManifestValidator.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace ai::application;

void requireManifest(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

ExecutionAcceptanceContract acceptanceContract(
    const std::string& id, const std::string& requiredOutput)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = id;
    requirement.description = "Verify " + id;
    requirement.requiredOutputs = {requiredOutput};
    requirement.verificationKind = ExecutionVerificationKind::InspectPaths;
    requirement.required = true;
    ExecutionAcceptanceContract contract;
    contract.requirements = {std::move(requirement)};
    return contract;
}

ExecutionManifest validManifest(const RunId& runId)
{
    ExecutionManifest manifest;
    manifest.runId = runId;
    manifest.requestedMaxParallelism = 2;
    manifest.requestedTokenBudget = 40'000;
    manifest.requestedCostMicros = 2'000'000;
    manifest.policy.maxTasks = 8;
    manifest.policy.maxDepth = 4;
    manifest.policy.maxParallelism = 3;
    manifest.policy.maximumCapability = WorkspaceCapability::Propose;
    manifest.policy.tokenBudgetCeiling = 50'000;
    manifest.policy.costMicrosCeiling = 3'000'000;

    ExecutionManifestTask inspect;
    inspect.id = "inspect";
    inspect.objective = "Inspect the existing contract";
    inspect.role = "analyst";
    inspect.ownedPaths = {"src/read-model"};
    inspect.acceptanceCriteria = {"Current behavior is documented"};
    inspect.acceptanceContract = acceptanceContract("inspect-current-behavior", "src/read-model");
    inspect.contextReferences = {"CODEX_CONTEXT.md#phase-2"};
    inspect.requestedCapability = WorkspaceCapability::Inspect;

    ExecutionManifestTask implement;
    implement.id = "implement";
    implement.objective = "Propose the bounded implementation";
    implement.role = "implementer";
    implement.dependencies = {"inspect"};
    implement.ownedPaths = {"src/execution"};
    implement.acceptanceCriteria = {"Contract tests pass"};
    implement.acceptanceContract = acceptanceContract("implementation-contract", "src/execution");
    implement.contextReferences = {"REMAINING_GAPS_EXECUTION_PLAN.md#phase-2"};
    implement.requestedCapability = WorkspaceCapability::Propose;

    manifest.tasks = {std::move(inspect), std::move(implement)};
    return manifest;
}

bool hasFinding(
    const std::vector<ExecutionManifestFinding>& findings,
    const std::string& code)
{
    return std::any_of(findings.begin(), findings.end(), [&](const auto& finding) {
        return finding.code == code;
    });
}

// --- Package B: owned-path diagnostics fixtures -----------------------------
//
// These exercise ExecutionManifestValidator::validate() directly (the pure
// static function), not through ExecutionManifestService, since owned-path
// diagnostics do not depend on Run/policy trust-boundary substitution.

const ExecutionManifestFinding* findFinding(
    const std::vector<ExecutionManifestFinding>& findings,
    const std::string& code)
{
    const auto it = std::find_if(findings.begin(), findings.end(),
        [&](const auto& finding) { return finding.code == code; });
    return it == findings.end() ? nullptr : &(*it);
}

std::size_t countFindings(
    const std::vector<ExecutionManifestFinding>& findings,
    const std::string& code)
{
    return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(),
        [&](const auto& finding) { return finding.code == code; }));
}

ExecutionManifestTask ownedPathTask(
    const std::string& id,
    std::vector<std::filesystem::path> ownedPaths)
{
    ExecutionManifestTask task;
    task.id = id;
    task.objective = "Objective for " + id;
    task.role = "worker";
    task.ownedPaths = std::move(ownedPaths);
    task.acceptanceCriteria = {"Acceptance for " + id};
    task.acceptanceContract = acceptanceContract("acceptance-" + id, "README.md");
    task.contextReferences = {"REMAIN_TO_DO.md"};
    task.requestedCapability = WorkspaceCapability::Inspect;
    return task;
}

// requestedMaxParallelism/policy are left at their defaults (1 and the
// ExecutionManifestPolicy defaults), which are already within every ceiling
// they are checked against - only manifest.empty_tasks and the owned-path
// findings under test are meant to fire.
ExecutionManifest ownedPathManifest(std::vector<ExecutionManifestTask> tasks)
{
    ExecutionManifest manifest;
    manifest.runId = "owned-path-run";
    manifest.tasks = std::move(tasks);
    return manifest;
}

void testValidManifestFreezesExactlyOnce()
{
    InMemoryRunRepository runs;
    Run run;
    run.id = "manifest-run";
    run.projectId = "project";
    run.title = "Phase 2";
    run.task = "Freeze contract";
    run.workflowId = "mock-sequential-v1";
    run.createdAt = std::chrono::system_clock::now();
    requireManifest(runs.insertRun(run).success(), "Could not seed in-memory Run");

    InMemoryExecutionManifestRepository manifests;
    ExecutionManifestService service(manifests, runs);
    auto manifest = validManifest(run.id);
    const auto trustedPolicy = manifest.policy;
    requireManifest(service.validate(manifest, trustedPolicy).empty(), "Valid manifest was rejected");

    const auto frozen = service.freeze(manifest, trustedPolicy);
    requireManifest(frozen.success(), "Valid manifest did not freeze");
    requireManifest(
        frozen.value().createdAt.time_since_epoch().count() != 0,
        "Service did not assign an authoritative creation timestamp");

    const auto read = service.findForRun(run.id);
    requireManifest(read.success() && read.value().has_value(), "Frozen manifest was not readable");
    requireManifest(read.value()->tasks.size() == 2, "Frozen task graph changed");
    requireManifest(
        read.value()->policy.tokenBudgetCeiling == 50'000,
        "Trusted policy snapshot changed");

    const auto duplicate = service.freeze(std::move(manifest), trustedPolicy);
    requireManifest(!duplicate.success(), "Second freeze unexpectedly replaced the snapshot");
    requireManifest(
        duplicate.error()->code == "manifest.already_frozen",
        "Second freeze returned the wrong typed error");
}

void testInvalidManifestReportsIndependentViolationsBeforePersistence()
{
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    ExecutionManifestService service(manifests, runs);
    auto manifest = validManifest("missing-run");
    auto trustedPolicy = manifest.policy;
    manifest.requestedMaxParallelism = 9;
    manifest.requestedTokenBudget = 60'000;
    // A planner tries to self-authorize Propose, but the trusted envelope is
    // Inspect-only and must replace the candidate's embedded value.
    manifest.policy.maximumCapability = WorkspaceCapability::Propose;
    trustedPolicy.maximumCapability = WorkspaceCapability::Inspect;
    manifest.tasks[0].dependencies = {"implement"};
    manifest.tasks[1].ownedPaths = {"src/read-model/detail"};

    const auto findings = service.validate(manifest, trustedPolicy);
    requireManifest(hasFinding(findings, "manifest.parallelism_exceeded"),
        "Parallelism violation was not reported");
    requireManifest(hasFinding(findings, "manifest.token_budget_exceeded"),
        "Token budget violation was not reported");
    requireManifest(hasFinding(findings, "manifest.capability_exceeded"),
        "Capability violation was not reported");
    requireManifest(hasFinding(findings, "manifest.dependency_cycle"),
        "Dependency cycle was not reported");
    requireManifest(hasFinding(findings, "manifest.ownership_overlap"),
        "Path ownership overlap was not reported");

    const auto frozen = service.freeze(manifest, trustedPolicy);
    requireManifest(!frozen.success(), "Invalid manifest was persisted");
    const auto absent = manifests.findForRun(manifest.runId);
    requireManifest(absent.success() && !absent.value().has_value(),
        "Invalid manifest left a partial snapshot");
}

void testUnknownRunFailsClosed()
{
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    ExecutionManifestService service(manifests, runs);
    auto manifest = validManifest("unknown-run");
    const auto trustedPolicy = manifest.policy;
    const auto result = service.freeze(std::move(manifest), trustedPolicy);
    requireManifest(!result.success() && result.error()->code == "manifest.run_not_found",
        "Unknown Run was not rejected before persistence");
}

// --- Package B: owned-path diagnostics regressions --------------------------

void testInvalidOwnedPathCategoryEmpty()
{
    auto manifest = ownedPathManifest(
        {ownedPathTask("t-empty", {std::filesystem::path {}})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.invalid_owned_path");
    requireManifest(finding != nullptr, "Empty owned path was not rejected");
    requireManifest(finding->taskId.has_value() && *finding->taskId == "t-empty",
        "Empty-path finding was not attached to its task");
    requireManifest(
        finding->message
            == "Owned path rejected because the path is empty; "
               "only clean workspace-relative paths are accepted",
        "Empty-path category message did not match the expected diagnostic");
}

void testInvalidOwnedPathCategoryRepositoryRootDot()
{
    auto manifest = ownedPathManifest(
        {ownedPathTask("t-dot", {std::filesystem::path(".")})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.invalid_owned_path");
    requireManifest(finding != nullptr, "Repository-root/dot owned path was not rejected");
    requireManifest(
        finding->message
            == "Owned path rejected because the path contains a repository-root "
               "(\".\") segment; only clean workspace-relative paths are accepted",
        "Repository-root/dot category message did not match the expected diagnostic");
}

void testInvalidOwnedPathCategoryAbsoluteOrDriveQualified()
{
    auto manifest = ownedPathManifest(
        {ownedPathTask("t-abs", {std::filesystem::path("C:/Users/attacker/secret")})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.invalid_owned_path");
    requireManifest(finding != nullptr, "Drive-qualified owned path was not rejected");
    requireManifest(
        finding->message
            == "Owned path rejected because the path is absolute or "
               "drive/UNC-qualified; only clean workspace-relative paths are accepted",
        "Absolute/drive-qualified category message did not match the expected diagnostic");
}

void testInvalidOwnedPathCategoryParentTraversal()
{
    auto manifest = ownedPathManifest(
        {ownedPathTask("t-traverse", {std::filesystem::path("../outside/secrets")})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.invalid_owned_path");
    requireManifest(finding != nullptr, "Parent-traversal owned path was not rejected");
    requireManifest(
        finding->message
            == "Owned path rejected because the path contains a parent-directory "
               "(\"..\") traversal segment; only clean workspace-relative paths are accepted",
        "Parent-traversal category message did not match the expected diagnostic");
}

void testInvalidOwnedPathCategoryNonCanonicalSeparatorForm()
{
    auto manifest = ownedPathManifest(
        {ownedPathTask("t-trailing", {std::filesystem::path("src/app/")})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.invalid_owned_path");
    requireManifest(finding != nullptr, "Trailing-separator owned path was not rejected");
    requireManifest(
        finding->message
            == "Owned path rejected because the path contains an empty segment "
               "from a trailing or repeated separator; only clean workspace-relative "
               "paths are accepted",
        "Non-canonical-separator category message did not match the expected diagnostic");
}

void testInvalidOwnedPathMessagesNeverLeakRawAbsoluteInput()
{
    const std::string sensitiveMarker = "very/secret/absolute/marker/path";
    auto manifest = ownedPathManifest(
        {ownedPathTask("t-leak", {std::filesystem::path("C:/" + sensitiveMarker)})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.invalid_owned_path");
    requireManifest(finding != nullptr, "Absolute owned path was not rejected");
    requireManifest(finding->message.find(sensitiveMarker) == std::string::npos,
        "Finding message leaked the raw absolute path's content");
    requireManifest(finding->message.find("C:") == std::string::npos,
        "Finding message leaked the raw drive-qualified prefix");
    requireManifest(finding->message.find("absolute or drive") != std::string::npos,
        "Finding message lost its safe category description");
}

void testOwnershipOverlapExactPathNamesBothTasks()
{
    auto manifest = ownedPathManifest({
        ownedPathTask("task-a", {std::filesystem::path("src/shared")}),
        ownedPathTask("task-b", {std::filesystem::path("src/shared")})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.ownership_overlap");
    requireManifest(finding != nullptr, "Exact-path overlap was not reported");
    requireManifest(finding->taskId.has_value() && *finding->taskId == "task-b",
        "Overlap finding was not attached to the later-processed task");
    requireManifest(finding->message.find("task-a") != std::string::npos
            && finding->message.find("task-b") != std::string::npos,
        "Overlap message did not unambiguously name both conflicting task IDs");
    requireManifest(finding->message.find("src/shared") != std::string::npos,
        "Overlap message dropped the already-validated safe path");
}

void testOwnershipOverlapParentChildNamesBothTasks()
{
    auto manifest = ownedPathManifest({
        ownedPathTask("task-parent", {std::filesystem::path("src")}),
        ownedPathTask("task-child", {std::filesystem::path("src/app")})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.ownership_overlap");
    requireManifest(finding != nullptr, "Parent/child overlap was not reported");
    requireManifest(finding->taskId.has_value() && *finding->taskId == "task-child",
        "Overlap finding was not attached to the child task");
    requireManifest(finding->message.find("task-parent") != std::string::npos
            && finding->message.find("task-child") != std::string::npos,
        "Overlap message did not unambiguously name both conflicting task IDs");
    requireManifest(finding->message.find("'src'") != std::string::npos
            && finding->message.find("src/app") != std::string::npos,
        "Overlap message dropped one of the already-validated safe paths");
}

void testOwnedPathFindingOrderAndTaskAssociationAreDeterministic()
{
    auto manifest = ownedPathManifest({
        ownedPathTask("t1", {std::filesystem::path(".")}),
        ownedPathTask("t2", {std::filesystem::path("src/shared")}),
        ownedPathTask("t3", {std::filesystem::path("src/shared")})});

    const auto first = ExecutionManifestValidator::validate(manifest);
    requireManifest(first.size() == 2, "Unexpected finding count for the mixed fixture");
    requireManifest(
        first[0].code == "manifest.invalid_owned_path"
            && first[0].taskId.has_value() && *first[0].taskId == "t1",
        "First finding was not the invalid owned path attributed to t1");
    requireManifest(
        first[1].code == "manifest.ownership_overlap"
            && first[1].taskId.has_value() && *first[1].taskId == "t3",
        "Second finding was not the ownership overlap attributed to t3");

    const auto second = ExecutionManifestValidator::validate(manifest);
    requireManifest(second.size() == first.size(),
        "Repeated validation of the same manifest produced a different finding count");
    for (std::size_t index = 0; index < first.size(); ++index) {
        requireManifest(
            first[index].code == second[index].code
                && first[index].message == second[index].message
                && first[index].taskId == second[index].taskId,
            "Repeated validation of the same manifest was not deterministic");
    }
}

void testEmptyTasksStillReturnsExactlyEmptyTasksCode()
{
    auto manifest = ownedPathManifest({});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(findings.size() == 1, "Empty-tasks manifest produced unexpected findings");
    requireManifest(findings.front().code == "manifest.empty_tasks",
        "Empty-tasks manifest did not return exactly manifest.empty_tasks");
}

nlohmann::json parserManifest(const std::string& ownedPath)
{
    return {
        {"schemaVersion", "execution-manifest.v1"},
        {"tasks", nlohmann::json::array({{
            {"id", "task"}, {"objective", "Implement bounded work"},
            {"role", "worker"}, {"dependencies", nlohmann::json::array()},
            {"ownedPaths", nlohmann::json::array({ownedPath})},
            {"plannedCreatePaths", nlohmann::json::array()},
            {"requiredReadPaths", nlohmann::json::array()},
            {"referencePaths", nlohmann::json::array()},
            {"excludedPaths", nlohmann::json::array()},
            {"deferredPaths", nlohmann::json::array()},
            {"acceptanceCriteria", nlohmann::json::array({"It works"})},
            {"contextReferences", nlohmann::json::array()},
            {"requestedCapability", "propose"}}})},
        {"requestedMaxParallelism", 1},
        {"bootstrapDeclaration", nullptr}};
}

void testRawOwnedPathsAreRejectedBeforeFilesystemConversion()
{
    std::string nulPath("secret", 6);
    nulPath.push_back('\0');
    nulPath += "tail";
    const std::vector<std::string> invalid {
        "", "/", "C:/secret-marker", "//server/share", ".", "..",
        "src//app", "src/app/", "src\\app", nulPath, "src/*.cpp",
        "src/[ab].cpp", "src/<bad>.cpp"};
    for (const auto& path : invalid) {
        const auto parsed = ExecutionManifestParser::parse(
            parserManifest(path).dump(), "raw-path-run");
        requireManifest(!parsed.success()
                && parsed.error()->code == "manifest.invalid_owned_path",
            "Unsafe raw owned path crossed the parser boundary");
        requireManifest(parsed.error()->message.find("secret-marker") == std::string::npos,
            "Raw path content leaked into parser diagnostics");
    }
    const auto canonical = ExecutionManifestParser::parse(
        parserManifest("src/app/Main.cpp").dump(), "raw-path-run");
    requireManifest(canonical.success()
            && canonical.value().tasks.front().ownedPaths.front().generic_string()
                == "src/app/Main.cpp",
        "Canonical forward-slash owned path regressed");
}

void testStructuredAcceptanceContractParsesAndRejectsUnsafeVariants()
{
    auto document = parserManifest("src/app");
    document["tasks"][0]["acceptanceContract"] = {
        {"schemaVersion", "execution-acceptance.v1"},
        {"requirements", nlohmann::json::array({{
            {"id", "qml-lint"}, {"description", "QML lint is warning-free"},
            {"requiredOutputs", nlohmann::json::array({"qml/AppShell.qml"})},
            {"verificationKind", "qml_lint"},
            {"verificationProfileId", "local-qt-debug"},
            {"verificationBuildProfileId", "local-qt-debug"},
            {"verificationTargetId", "all_qmllint"},
            {"verificationArguments", nlohmann::json::array()},
            {"required", true}, {"verificationBootstrapPending", false}}})}};
    const auto parsed = ExecutionManifestParser::parse(document.dump(), "contract-run");
    requireManifest(parsed.success()
            && parsed.value().tasks.front().acceptanceContract.has_value()
            && parsed.value().tasks.front().acceptanceContract->requirements.front().id
                == "qml-lint",
        "Structured acceptance contract did not parse");
    requireManifest(executionVerificationKindFromString(
            toString(ExecutionVerificationKind::QmlLint))
            == ExecutionVerificationKind::QmlLint,
        "Verification-kind string round trip failed");

    auto invalidVersion = document;
    invalidVersion["tasks"][0]["acceptanceContract"]["schemaVersion"] = "future.v9";
    const auto versionResult = ExecutionManifestParser::parse(
        invalidVersion.dump(), "contract-run");
    requireManifest(!versionResult.success()
            && versionResult.error()->code == "acceptance.unsupported_schema",
        "Unknown acceptance schema was accepted");

    auto duplicate = document;
    duplicate["tasks"][0]["acceptanceContract"]["requirements"].push_back(
        duplicate["tasks"][0]["acceptanceContract"]["requirements"][0]);
    const auto duplicateResult = ExecutionManifestParser::parse(
        duplicate.dump(), "contract-run");
    requireManifest(!duplicateResult.success()
            && duplicateResult.error()->code == "acceptance.duplicate_requirement_id",
        "Duplicate acceptance requirement id was accepted");

    auto unsafeOutput = document;
    unsafeOutput["tasks"][0]["acceptanceContract"]["requirements"][0]
        ["requiredOutputs"][0] = "C:/private/secret-marker";
    const auto unsafeResult = ExecutionManifestParser::parse(
        unsafeOutput.dump(), "contract-run");
    requireManifest(!unsafeResult.success()
            && unsafeResult.error()->code == "acceptance.invalid_output_path"
            && unsafeResult.error()->message.find("secret-marker") == std::string::npos,
        "Unsafe acceptance output did not fail with a safe diagnostic");
}

void testVerificationProfileArgumentsFailBeforeManifestFreeze()
{
    // AG-18.2: a provider may no longer submit raw preset/buildPreset/
    // buildDirectory/target values in verificationArguments for cmake_build/
    // qml_lint at all - even a "target-only" attempt is a forbidden raw
    // environment override, not merely an incomplete argument set.
    auto rawOverride = parserManifest("qml/components/navigation");
    rawOverride["tasks"][0]["acceptanceContract"] = {
        {"schemaVersion", "execution-acceptance.v1"},
        {"requirements", nlohmann::json::array({{
            {"id", "build-and-lint"},
            {"description", "Project builds and QML lint is warning-free"},
            {"requiredOutputs", nlohmann::json::array()},
            {"verificationKind", "cmake_build"},
            {"verificationProfileId", "local-qt-debug"},
            {"verificationBuildProfileId", "local-qt-debug"},
            {"verificationTargetId", nullptr},
            {"verificationArguments", nlohmann::json::array({{
                {"name", "target"}, {"value", "all_qmllint"}}})},
            {"required", true}, {"verificationBootstrapPending", false}}})}};
    const auto rejectedOverride = ExecutionManifestParser::parse(
        rawOverride.dump(), "verification-profile-run");
    requireManifest(!rejectedOverride.success()
            && rejectedOverride.error()->code
                == "acceptance.provider_owned_verification_argument",
        "A raw verificationArguments override on cmake_build reached Manifest freeze");

    // Missing the required verificationProfileId selection must also fail
    // before freeze, distinctly from a raw-argument override.
    auto missingSelection = rawOverride;
    missingSelection["tasks"][0]["acceptanceContract"]["requirements"][0]
        ["verificationProfileId"] = nullptr;
    missingSelection["tasks"][0]["acceptanceContract"]["requirements"][0]
        ["verificationArguments"] = nlohmann::json::array();
    const auto rejectedMissing = ExecutionManifestParser::parse(
        missingSelection.dump(), "verification-profile-run");
    requireManifest(!rejectedMissing.success()
            && rejectedMissing.error()->code == "acceptance.invalid_verification_selection",
        "A cmake_build requirement missing verificationProfileId reached Manifest freeze");

    // A correctly-shaped draft selection (empty verificationArguments, both
    // ids present) parses - whether the selected ids are actually known to
    // any catalog is ManifestVerificationProfileBinder's job at Planner
    // time, not the raw parser's.
    auto wellFormed = rawOverride;
    wellFormed["tasks"][0]["acceptanceContract"]["requirements"][0]
        ["verificationArguments"] = nlohmann::json::array();
    const auto accepted = ExecutionManifestParser::parse(
        wellFormed.dump(), "verification-profile-run");
    requireManifest(accepted.success(),
        "A correctly shaped draft cmake_build selection was rejected");

    auto programmatic = validManifest("programmatic-profile-run");
    auto& requirement =
        programmatic.tasks.front().acceptanceContract->requirements.front();
    requirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    requirement.verificationArguments = {{"target", "all_qmllint"}};
    const auto findings = ExecutionManifestValidator::validate(programmatic);
    requireManifest(hasFinding(findings,
            "acceptance.provider_owned_verification_argument"),
        "A programmatically constructed raw-argument override bypassed Manifest validation");

    const std::vector<ExecutionAcceptanceRequirement> validProfiles {
        {"inspect", "Inspect outputs", {"README.md"},
            ExecutionVerificationKind::InspectPaths, {}, true},
        {"build", "Build project", {}, ExecutionVerificationKind::CMakeBuild,
            {{"preset", "windows-vcpkg"},
             {"buildPreset", "windows-vcpkg-debug"}}, true},
        {"lint", "Lint QML", {}, ExecutionVerificationKind::QmlLint,
            {{"buildDirectory", "build/windows-vcpkg"},
             {"target", "all_qmllint"}}, true},
        {"registration", "Check registration", {},
            ExecutionVerificationKind::CMakeTargetConsistency,
            {{"definitionFile", "CMakeLists.txt"},
             {"registrationFile", "qml/CMakeLists.txt"},
             {"target", "Dashboard_Content"}}, true},
    };
    for (const auto& profile : validProfiles) {
        requireManifest(validateExecutionVerificationProfileArguments(profile).success(),
            "A valid Backend-known verification profile was rejected");
    }

    auto unknownPolicy = validProfiles[2];
    unknownPolicy.qmlLintDiagnosticPolicyId = "qml_lint_future_v99";
    requireManifest(!validateExecutionVerificationProfileArguments(unknownPolicy).success(),
        "An unknown frozen QML-lint diagnostic policy id was accepted");

    auto providerPolicy = validProfiles[2];
    providerPolicy.verificationArguments.clear();
    providerPolicy.verificationProfileId = "local-qt-debug";
    providerPolicy.verificationBuildProfileId = "local-qt-debug";
    providerPolicy.verificationTargetId = "all_qmllint";
    providerPolicy.qmlLintDiagnosticPolicyId =
        std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
    const auto providerPolicyResult =
        validateExecutionVerificationDraftSelections(providerPolicy);
    requireManifest(!providerPolicyResult.success()
            && providerPolicyResult.error()->code
                == "acceptance.provider_owned_verification_argument",
        "A provider-authored QML-lint diagnostic policy id was accepted");

    auto nonQmlPolicy = validProfiles[0];
    nonQmlPolicy.qmlLintDiagnosticPolicyId =
        std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
    requireManifest(!validateExecutionVerificationProfileArguments(nonQmlPolicy).success(),
        "A non-QML verification requirement carrying QML policy identity was accepted");
}

// P1 #2: a fresh submission using cmake_target_consistency is rejected
// before it can ever reach freeze/dispatch, with a dedicated stable code -
// even though the requirement is otherwise perfectly well-shaped (this is
// not a shape rejection, it is a trust rejection: there is no Backend-owned
// trusted catalog of admissible definition/registration/target triples
// yet).
void testCmakeTargetConsistencySubmissionRejectedAtParser()
{
    auto document = parserManifest("qml/components/navigation");
    document["tasks"][0]["acceptanceContract"] = {
        {"schemaVersion", "execution-acceptance.v1"},
        {"requirements", nlohmann::json::array({{
            {"id", "registration"},
            {"description", "CMake target registration is consistent"},
            {"requiredOutputs", nlohmann::json::array()},
            {"verificationKind", "cmake_target_consistency"},
            {"verificationProfileId", nullptr},
            {"verificationBuildProfileId", nullptr},
            {"verificationTargetId", "Dashboard_Content"},
            {"verificationArguments", nlohmann::json::array({
                {{"name", "definitionFile"}, {"value", "CMakeLists.txt"}},
                {{"name", "registrationFile"}, {"value", "qml/CMakeLists.txt"}}})},
            {"required", true}, {"verificationBootstrapPending", false}}})}};
    const auto parsed = ExecutionManifestParser::parse(document.dump(), "contract-run");
    requireManifest(!parsed.success()
            && parsed.error()->code == "acceptance.untrusted_cmake_consistency_selection",
        "A fresh cmake_target_consistency submission was not rejected fail-closed");
}

// P1 #7: definitionFile and registrationFile naming the same file would
// trivially "pass" a literal substring self-comparison without checking
// anything - both the draft and the resolved shape validators reject this
// for a new trusted contract.
void testCmakeTargetConsistencyIdenticalFilesAreRejected()
{
    ExecutionAcceptanceRequirement draft;
    draft.id = "registration";
    draft.description = "Check registration";
    draft.verificationKind = ExecutionVerificationKind::CMakeTargetConsistency;
    draft.verificationTargetId = "Dashboard_Content";
    draft.verificationArguments = {
        {"definitionFile", "qml/CMakeLists.txt"}, {"registrationFile", "qml/CMakeLists.txt"}};
    draft.required = true;
    requireManifest(!validateExecutionVerificationDraftSelections(draft).success(),
        "A draft cmake_target_consistency selection with identical definitionFile and "
        "registrationFile was accepted");

    ExecutionAcceptanceRequirement resolved = draft;
    resolved.verificationArguments.push_back({"target", "Dashboard_Content"});
    requireManifest(!validateExecutionVerificationProfileArguments(resolved).success(),
        "A resolved cmake_target_consistency selection with identical definitionFile and "
        "registrationFile was accepted");
}

// Two-task manifest with the second task carrying a full nested
// acceptanceContract/requirement/verificationArguments chain, so every
// requireExactFields() nesting level (top-level, task, acceptanceContract,
// requirement, verificationArguments item) can be exercised by erasing one
// field at a time from a document that is otherwise fully valid.
nlohmann::json diagnosticsManifest()
{
    return {
        {"schemaVersion", "execution-manifest.v1"},
        {"tasks", nlohmann::json::array({
            {
                {"id", "task-a"}, {"objective", "First task"},
                {"role", "worker"}, {"dependencies", nlohmann::json::array()},
                {"ownedPaths", nlohmann::json::array({"src/a"})},
                {"plannedCreatePaths", nlohmann::json::array()},
                {"requiredReadPaths", nlohmann::json::array()},
                {"referencePaths", nlohmann::json::array()},
                {"excludedPaths", nlohmann::json::array()},
                {"deferredPaths", nlohmann::json::array()},
                {"acceptanceCriteria", nlohmann::json::array({"It works"})},
                {"contextReferences", nlohmann::json::array()},
                {"requestedCapability", "propose"}
            },
            {
                {"id", "task-b"}, {"objective", "Second task"},
                {"role", "worker"}, {"dependencies", nlohmann::json::array()},
                {"ownedPaths", nlohmann::json::array({"src/b"})},
                {"plannedCreatePaths", nlohmann::json::array()},
                {"requiredReadPaths", nlohmann::json::array()},
                {"referencePaths", nlohmann::json::array()},
                {"excludedPaths", nlohmann::json::array()},
                {"deferredPaths", nlohmann::json::array()},
                {"acceptanceCriteria", nlohmann::json::array({"It works"})},
                {"contextReferences", nlohmann::json::array()},
                {"requestedCapability", "propose"},
                {"acceptanceContract", {
                    {"schemaVersion", "execution-acceptance.v1"},
                    {"requirements", nlohmann::json::array({
                        {
                            {"id", "req-1"}, {"description", "Verify it"},
                            {"requiredOutputs", nlohmann::json::array({"src/b/out.txt"})},
                            {"verificationKind", "inspect_paths"},
                            {"verificationProfileId", nullptr},
                            {"verificationBuildProfileId", nullptr},
                            {"verificationTargetId", nullptr},
                            {"verificationArguments", nlohmann::json::array({
                                {{"name", "arg-name"}, {"value", "arg-value"}}
                            })},
                            {"required", true}, {"verificationBootstrapPending", false}
                        }
                    })}
                }}
            }
        })},
        {"requestedMaxParallelism", 1},
        {"bootstrapDeclaration", nullptr}
    };
}

void testMissingFieldDiagnosticsIncludeSafeStructuralLocation()
{
    {
        auto doc = diagnosticsManifest();
        doc.erase("tasks");
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.missing_field",
            "Missing top-level tasks field was accepted");
        requireManifest(parsed.error()->message.find("tasks") != std::string::npos
                && parsed.error()->message.find("tasks[") == std::string::npos,
            "Top-level missing-field diagnostic did not name the bare top-level field");
    }
    {
        auto doc = diagnosticsManifest();
        doc["tasks"][0].erase("requestedCapability");
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.missing_field",
            "Missing task-level field was accepted");
        requireManifest(
            parsed.error()->message.find("tasks[0].requestedCapability") != std::string::npos,
            "Task-level missing-field diagnostic did not name tasks[0].requestedCapability");
    }
    {
        auto doc = diagnosticsManifest();
        doc["tasks"][1]["acceptanceContract"].erase("schemaVersion");
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.missing_field",
            "Missing acceptanceContract-level field was accepted");
        requireManifest(parsed.error()->message.find(
                "tasks[1].acceptanceContract.schemaVersion") != std::string::npos,
            "acceptanceContract-level missing-field diagnostic did not name the nested location");
    }
    {
        auto doc = diagnosticsManifest();
        doc["tasks"][1]["acceptanceContract"]["requirements"][0].erase("required");
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.missing_field",
            "Missing requirement-level field was accepted");
        requireManifest(parsed.error()->message.find(
                "tasks[1].acceptanceContract.requirements[0].required") != std::string::npos,
            "requirement-level missing-field diagnostic did not name the nested location");
    }
    {
        auto doc = diagnosticsManifest();
        doc["tasks"][1]["acceptanceContract"]["requirements"][0]
            ["verificationArguments"][0].erase("value");
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.missing_field",
            "Missing verificationArguments-item field was accepted");
        requireManifest(parsed.error()->message.find(
                "tasks[1].acceptanceContract.requirements[0].verificationArguments[0].value")
                != std::string::npos,
            "verificationArguments-item missing-field diagnostic did not name the nested location");
    }
}

void testMissingFieldDiagnosticsNeverLeakSensitiveMarkersOrUnexpectedFieldNames()
{
    const std::string marker = "SENSITIVE_MARKER_9f3a";
    {
        auto doc = diagnosticsManifest();
        doc[marker] = "irrelevant";
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.unexpected_field",
            "Unexpected top-level field was accepted");
        requireManifest(parsed.error()->message.find(marker) == std::string::npos,
            "Unexpected-field diagnostic leaked the unknown field name");
    }
    {
        auto doc = diagnosticsManifest();
        doc["tasks"][1]["acceptanceContract"]["requirements"][0][marker] = "irrelevant";
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.unexpected_field",
            "Unexpected nested field was accepted");
        requireManifest(parsed.error()->message.find(marker) == std::string::npos,
            "Nested unexpected-field diagnostic leaked the unknown field name");
    }
    {
        // A field VALUE containing a sensitive marker must never leak either -
        // missing-field diagnostics only ever name Backend-known field paths,
        // never submitted values.
        auto doc = diagnosticsManifest();
        doc["tasks"][1]["acceptanceContract"]["requirements"][0]["description"] = marker;
        doc["tasks"][1]["acceptanceContract"]["requirements"][0].erase("required");
        const auto parsed = ExecutionManifestParser::parse(doc.dump(), "diag-run");
        requireManifest(!parsed.success() && parsed.error()->code == "manifest.missing_field",
            "Missing field was accepted despite an unrelated sensitive value present");
        requireManifest(parsed.error()->message.find(marker) == std::string::npos,
            "Missing-field diagnostic leaked an unrelated field's submitted value");
    }
}

void testNoneCapabilityRequiresEmptyCoordinationNode()
{
    auto manifest = ownedPathManifest({ownedPathTask(
        "none-with-claims", {std::filesystem::path("src")})});
    manifest.tasks.front().requestedCapability = WorkspaceCapability::None;
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(hasFinding(findings, "manifest.none_task_not_empty"),
        "None-capability task was allowed to claim work/evidence");
}

// Real-Run incident (Run 2f26ecc5-2723-4f9c-98fd-9ff317f13387, 2026-09-11):
// an inspect-capability task's worker session can only self-verify via
// submit_task_verification, self-reporting evidence from its own read-only
// inspection - it never actually runs a build or lint command. A
// command-backed verificationKind (cmake_build/qml_lint/
// cmake_target_consistency) attached to such a task can never legitimately
// be reported Passed, so the task fails deterministically on every attempt
// regardless of the real project state or the worker's competence.

void testInspectTaskWithCommandBackedVerificationIsRejected()
{
    auto task = ownedPathTask("verify-task", {});
    task.acceptanceContract->requirements.front().verificationKind =
        ExecutionVerificationKind::CMakeBuild;
    auto manifest = ownedPathManifest({std::move(task)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.inspect_task_command_verification");
    requireManifest(finding != nullptr,
        "An inspect-capability task declaring a command-backed verificationKind was accepted");
    requireManifest(finding->taskId.has_value() && *finding->taskId == "verify-task",
        "Finding must be attributed to the offending task");
    requireManifest(finding->message.find("verify-task") != std::string::npos
            && finding->message.find("acceptance-verify-task") != std::string::npos
            && finding->message.find("cmake_build") != std::string::npos,
        "Finding message must name the offending task, requirement id, and verificationKind");
}

void testInspectTaskReportsEveryOffendingRequirementNotJustFirst()
{
    auto task = ownedPathTask("verify-task", {});
    task.acceptanceContract->requirements.front().verificationKind =
        ExecutionVerificationKind::CMakeBuild;
    ExecutionAcceptanceRequirement second;
    second.id = "second-req";
    second.description = "Second requirement";
    second.verificationKind = ExecutionVerificationKind::QmlLint;
    second.required = true;
    task.acceptanceContract->requirements.push_back(std::move(second));
    auto manifest = ownedPathManifest({std::move(task)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(
        countFindings(findings, "manifest.inspect_task_command_verification") == 2,
        "Every offending requirement must be reported so the single bounded Planner "
        "repair can fix all of them in one pass, not just the first");
}

void testInspectTaskWithOnlyInspectPathsIsStillAccepted()
{
    auto manifest = ownedPathManifest({ownedPathTask("verify-task", {})});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(
        !hasFinding(findings, "manifest.inspect_task_command_verification"),
        "A legitimate inspect_paths-only requirement on an inspect-capability task "
        "must never be rejected by this rule");
}

void testProposeTaskWithCommandBackedVerificationIsStillAccepted()
{
    auto task = ownedPathTask("build-task", {std::filesystem::path("src/app.cpp")});
    task.requestedCapability = WorkspaceCapability::Propose;
    task.acceptanceContract->requirements.front().verificationKind =
        ExecutionVerificationKind::CMakeBuild;
    auto manifest = ownedPathManifest({std::move(task)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(
        !hasFinding(findings, "manifest.inspect_task_command_verification"),
        "A command-backed requirement on a propose-capability task is legitimate - "
        "the Backend's own combined verification evaluates it - and must never be "
        "rejected by this inspect-only rule");
}

// --- AG-15: workspace-scope role validation ---------------------------------

void testConflictingPathRoleWithinTaskIsRejected()
{
    auto task = ownedPathTask("t1", {std::filesystem::path("src/a.cpp")});
    task.requiredReadPaths = {std::filesystem::path("src/a.cpp")};
    auto manifest = ownedPathManifest({std::move(task)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(hasFinding(findings, "manifest.conflicting_path_role"),
        "A path declared under two roles in the same task was accepted");
    const auto finding = std::find_if(findings.begin(), findings.end(), [](const auto& value) {
        return value.code == "manifest.conflicting_path_role";
    });
    requireManifest(finding != findings.end()
            && finding->message.find("src/a.cpp") != std::string::npos
            && finding->message.find("owned_write") != std::string::npos
            && finding->message.find("required_read") != std::string::npos,
        "Conflicting-role finding did not name its normalized path and both roles");
}

void testPlannedCreateOverlapsOwnedWriteAcrossTasksIsRejected()
{
    auto taskA = ownedPathTask("task-a", {std::filesystem::path("src/shared")});
    auto taskB = ownedPathTask("task-b", {});
    taskB.plannedCreatePaths = {std::filesystem::path("src/shared")};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(hasFinding(findings, "manifest.ownership_overlap"),
        "planned_create overlapping another task's owned_write was accepted - "
        "AG-15 treats owned_write and planned_create as one exclusive write claim");
}

void testRequiredReadPathsMaySharePathsAcrossTasks()
{
    // Unlike write roles, multiple tasks legitimately required_read the same
    // file (e.g. a shared header) - this must never be reported as overlap.
    auto taskA = ownedPathTask("task-a", {});
    taskA.requiredReadPaths = {std::filesystem::path("src/shared.hpp")};
    auto taskB = ownedPathTask("task-b", {});
    taskB.requiredReadPaths = {std::filesystem::path("src/shared.hpp")};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(!hasFinding(findings, "manifest.ownership_overlap"),
        "Two tasks required_read-ing the same path was incorrectly reported as overlap");
}

void testGloballyExcludedPathIsRejectedWhenDenyPolicyProvided()
{
    auto task = ownedPathTask("t1", {});
    task.requiredReadPaths = {std::filesystem::path(".env")};
    auto manifest = ownedPathManifest({std::move(task)});
    const auto policy = WorkspaceReadPolicy::secureDefaults(std::filesystem::path("C:/workspace"));
    const auto findings = ExecutionManifestValidator::validate(manifest, &policy);
    requireManifest(hasFinding(findings, "manifest.excluded_path_activated"),
        "A globally-excluded path (.env) was accepted as an active workspace-scope path");
}

void testGloballyExcludedPathIsSkippedWhenNoDenyPolicyProvided()
{
    // Structural-only call sites (no baseline/policy available yet) must not
    // be forced to supply a WorkspaceReadPolicy.
    auto task = ownedPathTask("t1", {});
    task.requiredReadPaths = {std::filesystem::path(".env")};
    auto manifest = ownedPathManifest({std::move(task)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(!hasFinding(findings, "manifest.excluded_path_activated"),
        "Global-exclusion check ran despite no WorkspaceReadPolicy being supplied");
}

void testExcludedAndDeferredPathsRoundTripThroughParser()
{
    auto doc = parserManifest("src/a.cpp");
    doc["tasks"][0]["excludedPaths"] = nlohmann::json::array({
        nlohmann::json {{"path", "vendor/lib.cpp"}, {"reason", "Third-party, out of scope"}}});
    doc["tasks"][0]["deferredPaths"] = nlohmann::json::array({
        nlohmann::json {{"path", "src/future.cpp"}, {"reason", "Follow-up Run"}}});
    const auto parsed = ExecutionManifestParser::parse(doc.dump(), "excluded-deferred-run");
    requireManifest(parsed.success(), "Manifest with excludedPaths/deferredPaths was rejected");
    const auto& task = parsed.value().tasks.front();
    requireManifest(task.excludedPaths.size() == 1
            && task.excludedPaths.front().path == std::filesystem::path("vendor/lib.cpp")
            && task.excludedPaths.front().reason == "Third-party, out of scope",
        "excludedPaths did not round-trip through the parser");
    requireManifest(task.deferredPaths.size() == 1
            && task.deferredPaths.front().path == std::filesystem::path("src/future.cpp")
            && task.deferredPaths.front().reason == "Follow-up Run",
        "deferredPaths did not round-trip through the parser");
}

void testBlankExcludedPathReasonIsRejected()
{
    auto doc = parserManifest("src/a.cpp");
    doc["tasks"][0]["excludedPaths"] = nlohmann::json::array({
        nlohmann::json {{"path", "vendor/lib.cpp"}, {"reason", ""}}});
    const auto parsed = ExecutionManifestParser::parse(doc.dump(), "blank-reason-run");
    requireManifest(!parsed.success(), "A blank excludedPaths reason was accepted");
}

void testRequiredReadPathsRoundTripThroughParser()
{
    auto doc = parserManifest("src/a.cpp");
    doc["tasks"][0]["requiredReadPaths"] = nlohmann::json::array({"src/dependency.hpp"});
    doc["tasks"][0]["referencePaths"] = nlohmann::json::array({"docs/notes.md"});
    doc["tasks"][0]["plannedCreatePaths"] = nlohmann::json::array({"src/new_file.cpp"});
    const auto parsed = ExecutionManifestParser::parse(doc.dump(), "required-read-run");
    requireManifest(parsed.success(), "Manifest with the new AG-15 path lists was rejected");
    const auto& task = parsed.value().tasks.front();
    requireManifest(task.requiredReadPaths == std::vector<std::filesystem::path> {
            std::filesystem::path("src/dependency.hpp")},
        "requiredReadPaths did not round-trip through the parser");
    requireManifest(task.referencePaths == std::vector<std::filesystem::path> {
            std::filesystem::path("docs/notes.md")},
        "referencePaths did not round-trip through the parser");
    requireManifest(task.plannedCreatePaths == std::vector<std::filesystem::path> {
            std::filesystem::path("src/new_file.cpp")},
        "plannedCreatePaths did not round-trip through the parser");
}

void testUnsafeRequiredReadPathIsRejectedByParser()
{
    auto doc = parserManifest("src/a.cpp");
    doc["tasks"][0]["requiredReadPaths"] = nlohmann::json::array({"../secret"});
    const auto parsed = ExecutionManifestParser::parse(doc.dump(), "unsafe-required-read-run");
    requireManifest(!parsed.success() && parsed.error()->code == "manifest.invalid_required_read_path",
        "An unsafe requiredReadPaths entry crossed the parser boundary");
}

// Real-Run incident (Runs fcd484b8-.../cebcd3cf-.../c5f7d3da-...,
// 2026-09-11): manifest.dependency_cycle used to name only the single task
// where DFS re-entry was detected ("Manifest dependency graph must be
// acyclic"), giving a repair session nothing concrete to act on. The finding
// must now name the exact cycle chain (task -> task -> ... -> the same task
// again), not just one task in it.
void testDependencyCycleReportsFullChain()
{
    auto taskA = ownedPathTask("task-a", {std::filesystem::path("src/a")});
    taskA.dependencies = {"task-b"};
    auto taskB = ownedPathTask("task-b", {std::filesystem::path("src/b")});
    taskB.dependencies = {"task-c"};
    auto taskC = ownedPathTask("task-c", {std::filesystem::path("src/c")});
    taskC.dependencies = {"task-a"};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB), std::move(taskC)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.dependency_cycle");
    requireManifest(finding != nullptr, "Dependency cycle was not reported");
    requireManifest(
        finding->message.find("task-a -> task-b -> task-c -> task-a") != std::string::npos,
        "Cycle finding must name the exact chain (task-a -> task-b -> task-c -> task-a), not "
        "just one task in it");
}

// MW-P1 (real-Run incident, three OpenAI "Stage 2B" Runs, 2026-09-13): two
// tasks each requiring a path the other produces form a real cycle from
// BACKEND-DERIVED edges alone - even with NO declared dependency at all.
// Before this fix, cycle detection only walked task.dependencies, so this
// exact shape passed the Planner's bounded repair chance undetected and
// only surfaced much later in RunWorkspaceScopeService, after the manifest
// was already immutably frozen and no repair chance remained.
void testDependencyCycleFromPurelyDerivedEdgesIsCaught()
{
    auto taskA = ownedPathTask("task-a", {std::filesystem::path("src/a.qml")});
    taskA.requiredReadPaths = {std::filesystem::path("src/b.qml")};
    auto taskB = ownedPathTask("task-b", {std::filesystem::path("src/b.qml")});
    taskB.requiredReadPaths = {std::filesystem::path("src/a.qml")};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.dependency_cycle");
    requireManifest(finding != nullptr,
        "A cycle formed purely by backend-derived edges (no declared dependency at all) "
        "was not caught");
    requireManifest(finding->message.find("backend_derived: src/b.qml") != std::string::npos
            && finding->message.find("backend_derived: src/a.qml") != std::string::npos,
        "Cycle finding did not name the exact derived path for each edge");
}

// A mixed cycle - one edge declared, one derived - must still be reported
// with the correct origin/path per edge, not just the task-id chain.
void testMixedDeclaredAndDerivedCycleReportsEachEdgeOrigin()
{
    auto taskA = ownedPathTask("task-a", {std::filesystem::path("src/a.qml")});
    taskA.dependencies = {"task-b"};
    auto taskB = ownedPathTask("task-b", {std::filesystem::path("src/b.qml")});
    taskB.requiredReadPaths = {std::filesystem::path("src/a.qml")};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    const auto* finding = findFinding(findings, "manifest.dependency_cycle");
    requireManifest(finding != nullptr, "Mixed declared/derived cycle was not caught");
    requireManifest(
        finding->message.find("task-a -> task-b [planner_declared]") != std::string::npos,
        "Declared edge in the cycle was not attributed as planner_declared");
    requireManifest(
        finding->message.find("task-b -> task-a [backend_derived: src/a.qml]") != std::string::npos,
        "Derived edge in the cycle was not attributed with its exact path");
}

// Two distinct requiredReadPaths from the same consumer to the same
// producer must not be treated as two separate cycle edges - the
// underlying graph edge is the same either way, and the message names one
// representative path, not a combinatorial explosion of duplicates.
void testMultiplePathsProducingSameEdgeDoNotDuplicateCycleReport()
{
    auto taskA = ownedPathTask("task-a",
        {std::filesystem::path("src/a1.qml"), std::filesystem::path("src/a2.qml")});
    taskA.requiredReadPaths = {std::filesystem::path("src/b.qml")};
    auto taskB = ownedPathTask("task-b", {std::filesystem::path("src/b.qml")});
    taskB.requiredReadPaths =
        {std::filesystem::path("src/a1.qml"), std::filesystem::path("src/a2.qml")};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(countFindings(findings, "manifest.dependency_cycle") == 1,
        "Multiple paths producing the same graph edge must not multiply cycle findings");
}

// An acyclic manifest combining both declared and derived edges must still
// pass cleanly - this fix only widens what counts as an edge for CYCLE
// detection, never tightens what counts as acyclic.
void testAcyclicGraphWithDeclaredAndDerivedEdgesIsAccepted()
{
    auto taskA = ownedPathTask("task-a", {std::filesystem::path("src/a.qml")});
    auto taskB = ownedPathTask("task-b", {std::filesystem::path("src/b.qml")});
    taskB.requiredReadPaths = {std::filesystem::path("src/a.qml")};
    auto taskC = ownedPathTask("task-c", {std::filesystem::path("src/c.qml")});
    taskC.dependencies = {"task-b"};
    auto manifest = ownedPathManifest({std::move(taskA), std::move(taskB), std::move(taskC)});
    const auto findings = ExecutionManifestValidator::validate(manifest);
    requireManifest(findFinding(findings, "manifest.dependency_cycle") == nullptr,
        "An acyclic combined declared/derived graph must not be reported as cyclic");
}

} // namespace

int main()
{
    try {
    testValidManifestFreezesExactlyOnce();
    testInvalidManifestReportsIndependentViolationsBeforePersistence();
    testUnknownRunFailsClosed();
    testInvalidOwnedPathCategoryEmpty();
    testInvalidOwnedPathCategoryRepositoryRootDot();
    testInvalidOwnedPathCategoryAbsoluteOrDriveQualified();
    testInvalidOwnedPathCategoryParentTraversal();
    testInvalidOwnedPathCategoryNonCanonicalSeparatorForm();
    testInvalidOwnedPathMessagesNeverLeakRawAbsoluteInput();
    testOwnershipOverlapExactPathNamesBothTasks();
    testOwnershipOverlapParentChildNamesBothTasks();
    testOwnedPathFindingOrderAndTaskAssociationAreDeterministic();
    testEmptyTasksStillReturnsExactlyEmptyTasksCode();
    testRawOwnedPathsAreRejectedBeforeFilesystemConversion();
    testStructuredAcceptanceContractParsesAndRejectsUnsafeVariants();
    testVerificationProfileArgumentsFailBeforeManifestFreeze();
    testCmakeTargetConsistencySubmissionRejectedAtParser();
    testCmakeTargetConsistencyIdenticalFilesAreRejected();
    testMissingFieldDiagnosticsIncludeSafeStructuralLocation();
    testMissingFieldDiagnosticsNeverLeakSensitiveMarkersOrUnexpectedFieldNames();
    testNoneCapabilityRequiresEmptyCoordinationNode();
    testInspectTaskWithCommandBackedVerificationIsRejected();
    testInspectTaskReportsEveryOffendingRequirementNotJustFirst();
    testInspectTaskWithOnlyInspectPathsIsStillAccepted();
    testProposeTaskWithCommandBackedVerificationIsStillAccepted();
    testConflictingPathRoleWithinTaskIsRejected();
    testPlannedCreateOverlapsOwnedWriteAcrossTasksIsRejected();
    testRequiredReadPathsMaySharePathsAcrossTasks();
    testGloballyExcludedPathIsRejectedWhenDenyPolicyProvided();
    testGloballyExcludedPathIsSkippedWhenNoDenyPolicyProvided();
    testExcludedAndDeferredPathsRoundTripThroughParser();
    testBlankExcludedPathReasonIsRejected();
    testRequiredReadPathsRoundTripThroughParser();
    testUnsafeRequiredReadPathIsRejectedByParser();
    testDependencyCycleReportsFullChain();
    testDependencyCycleFromPurelyDerivedEdgesIsCaught();
    testMixedDeclaredAndDerivedCycleReportsEachEdgeOrigin();
    testMultiplePathsProducingSameEdgeDoNotDuplicateCycleReport();
    testAcyclicGraphWithDeclaredAndDerivedEdgesIsAccepted();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
