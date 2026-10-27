// Unit tests for AG-17.0's ManifestMandatoryVerificationPolicy
// (manifest_mandatory_verification_v1): the Backend-owned derivation and
// idempotent merge of the verification requirements a Manifest's own write
// scope requires. Pure function tests over already-constructed
// VerificationEnvironmentSnapshot / ExecutionManifest values: no provider, no
// SQLite, no command execution.
#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/ExecutionManifestValidator.hpp"
#include "ai/application/verification/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/verification/ManifestVerificationProfileBinder.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// ---- fixtures ---------------------------------------------------------------

void addProfile(VerificationEnvironmentSnapshot& snapshot, const std::string& profileId,
    const std::vector<std::string>& targets, bool targetsConfirmed = true,
    bool targetsComplete = true, bool buildPresetsConfirmed = true,
    const std::string& resolvedBinaryDir = "build/debug")
{
    VerificationProfile profile;
    profile.profileId = profileId;
    profile.configurePresetName = profileId;
    profile.resolvedBinaryDir = resolvedBinaryDir;
    profile.origin = VerificationProfileOrigin::Discovered;
    profile.buildPresetsConfirmed = buildPresetsConfirmed;
    VerificationBuildPresetDescriptor buildPreset;
    buildPreset.name = profileId;
    buildPreset.source = VerificationPresetFileSource::UserPresets;
    buildPreset.declaredConfigurePreset = profileId;
    buildPreset.allowedTargetsConfirmed = targetsConfirmed;
    buildPreset.allowedTargetsComplete = targetsComplete;
    if (targetsConfirmed) buildPreset.allowedTargets = targets;
    profile.buildPresets.push_back(std::move(buildPreset));
    snapshot.profiles.push_back(std::move(profile));
}

VerificationEnvironmentSnapshot snapshotWith(
    const std::vector<std::string>& targets, bool complete = true)
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = "run-1";
    snapshot.projectId = "project-1";
    snapshot.baselineFingerprint = "baseline-1";
    snapshot.state = VerificationEnvironmentState::ExistingProfile;
    addProfile(snapshot, "local-qt-debug", targets, true, complete);
    return snapshot;
}

ExecutionAcceptanceRequirement inspectRequirement(const std::string& id)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = id;
    requirement.description = "The task's outputs exist";
    requirement.verificationKind = ExecutionVerificationKind::InspectPaths;
    requirement.required = true;
    return requirement;
}

ExecutionManifestTask taskWriting(const std::string& id, std::vector<std::string> writes,
    std::vector<std::string> dependencies = {})
{
    ExecutionManifestTask task;
    task.id = id;
    task.objective = "objective " + id;
    task.role = "worker";
    task.dependencies = std::move(dependencies);
    for (auto& path : writes) task.ownedPaths.emplace_back(std::move(path));
    task.requestedCapability = WorkspaceCapability::Propose;
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(inspectRequirement("inspect-" + id));
    task.acceptanceContract = std::move(contract);
    return task;
}

ExecutionManifest manifestOf(std::vector<ExecutionManifestTask> tasks)
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.tasks = std::move(tasks);
    return manifest;
}

ExecutionAcceptanceRequirement plannerLint(const std::string& id, const std::string& target,
    bool required = true)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = id;
    requirement.description = "Planner-declared QML lint";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.required = required;
    requirement.verificationProfileId = "local-qt-debug";
    requirement.verificationBuildProfileId = "local-qt-debug";
    requirement.verificationTargetId = target;
    return requirement;
}

ExecutionAcceptanceRequirement plannerBuild(const std::string& id, bool required = true)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = id;
    requirement.description = "Planner-declared build";
    requirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    requirement.required = required;
    requirement.verificationProfileId = "local-qt-debug";
    requirement.verificationBuildProfileId = "local-qt-debug";
    return requirement;
}

ExecutionManifestTask& taskById(ExecutionManifest& manifest, const std::string& id)
{
    const auto it = std::find_if(manifest.tasks.begin(), manifest.tasks.end(),
        [&](const ExecutionManifestTask& task) { return task.id == id; });
    require(it != manifest.tasks.end(), "test fixture: task not found");
    return *it;
}

std::size_t countKind(const ExecutionManifest& manifest, ExecutionVerificationKind kind,
    std::optional<ExecutionRequirementOrigin> origin = std::nullopt)
{
    std::size_t count = 0;
    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements) {
            if (requirement.verificationKind == kind
                && (!origin.has_value() || requirement.origin == *origin)) {
                ++count;
            }
        }
    }
    return count;
}

const ExecutionAcceptanceRequirement* findRequirement(
    const ExecutionManifest& manifest, const std::string& id, std::string* holder = nullptr)
{
    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements) {
            if (requirement.id == id) {
                if (holder != nullptr) *holder = task.id;
                return &requirement;
            }
        }
    }
    return nullptr;
}

const ManifestMandatoryVerificationDecision& decisionFor(
    const ExecutionManifest& manifest, ExecutionVerificationKind kind)
{
    require(manifest.mandatoryVerification.has_value(), "no mandatory verification record");
    for (const auto& decision : manifest.mandatoryVerification->decisions) {
        if (decision.kind == kind) return decision;
    }
    throw std::runtime_error("decision for kind missing from the record");
}

std::string optionalText(const std::optional<std::string>& value)
{
    return value.has_value() ? *value : std::string("-");
}

// A byte-comparable rendering of everything the policy can touch.
std::string dump(const ExecutionManifest& manifest)
{
    std::ostringstream out;
    for (const auto& task : manifest.tasks) {
        out << "task:" << task.id << '[';
        if (task.acceptanceContract.has_value()) {
            for (const auto& requirement : task.acceptanceContract->requirements) {
                out << requirement.id << ',' << toString(requirement.verificationKind) << ','
                    << requirement.required << ',' << optionalText(requirement.verificationProfileId)
                    << ',' << optionalText(requirement.verificationBuildProfileId) << ','
                    << optionalText(requirement.verificationTargetId) << ','
                    << requirement.verificationBootstrapPending << ','
                    << toString(requirement.origin) << ';';
            }
        } else {
            out << "no-contract";
        }
        out << "]\n";
    }
    if (manifest.mandatoryVerification.has_value()) {
        out << "policy:" << manifest.mandatoryVerification->policyId << ','
            << manifest.mandatoryVerification->mode << '\n';
        for (const auto& decision : manifest.mandatoryVerification->decisions) {
            out << toString(decision.kind) << ',' << toString(decision.status) << ','
                << decision.reasonCode << ',' << decision.requirementId << ',' << decision.taskId
                << ',' << decision.profileId << ',' << decision.buildProfileId << ','
                << decision.targetId << '\n';
        }
    }
    return out.str();
}

struct Outcome {
    bool success {false};
    std::string code;
    std::vector<ExecutionManifestFinding> findings;
};

Outcome applyPolicy(ExecutionManifest& manifest, const VerificationEnvironmentSnapshot& snapshot)
{
    Outcome outcome;
    const auto result = ManifestMandatoryVerificationPolicy::apply(
        manifest, snapshot, outcome.findings);
    outcome.success = result.success();
    if (!result.success()) outcome.code = result.error()->code;
    return outcome;
}

void requireFailure(ExecutionManifest manifest, const VerificationEnvironmentSnapshot& snapshot,
    const char* expectedCode, const char* message)
{
    const auto before = dump(manifest);
    const auto outcome = applyPolicy(manifest, snapshot);
    require(!outcome.success && outcome.code == expectedCode, message);
    require(outcome.findings.size() == 1 && outcome.findings.front().code == expectedCode,
        "A policy failure must append exactly one matching finding");
    require(dump(manifest) == before, "A policy failure must leave the Manifest untouched");
    require(!manifest.mandatoryVerification.has_value(),
        "A policy failure must not record a decision");
}

// ---- target eligibility -----------------------------------------------------

void testEligibleTargetPredicateIsExactAndCaseSensitive()
{
    using P = ManifestMandatoryVerificationPolicy;
    require(P::isEligibleQmlLintTarget("Dashboard_Content_qmllint"), "module target not eligible");
    require(P::isEligibleQmlLintTarget("all_qmllint"), "aggregate target not eligible by name");
    require(!P::isEligibleQmlLintTarget("_qmllint"), "an empty module part must not be eligible");
    require(!P::isEligibleQmlLintTarget("Dashboard_Content_qmllint_json"), "_json is eligible");
    require(!P::isEligibleQmlLintTarget("Dashboard_Content_qmllint_module"), "_module is eligible");
    require(!P::isEligibleQmlLintTarget("all_qmllint_json"), "all_qmllint_json is eligible");
    require(!P::isEligibleQmlLintTarget("Dashboard_Content_QMLLINT"), "case folding applied");
    require(!P::isEligibleQmlLintTarget("all"), "plain target eligible");
    require(!P::isEligibleQmlLintTarget("qmllint"), "bare qmllint eligible");
}

void testAggregateTargetIsPreferredOverModuleTarget()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    const auto snapshot = snapshotWith({"Dashboard_Content_qmllint", "all_qmllint", "all"});
    const auto outcome = applyPolicy(manifest, snapshot);
    require(outcome.success, "policy failed on a QML Manifest with a lint target");
    const auto& lint = decisionFor(manifest, ExecutionVerificationKind::QmlLint);
    require(lint.status == MandatoryVerificationDecisionStatus::Injected, "lint not injected");
    require(lint.targetId == "all_qmllint", "the aggregate target was not preferred");
    const auto* requirement = findRequirement(manifest, std::string(kMandatoryQmlLintRequirementId));
    require(requirement != nullptr && requirement->origin == ExecutionRequirementOrigin::BackendMandatory,
        "the injected lint requirement is missing or not backend_mandatory");
    require(requirement->required && requirement->verificationTargetId == std::optional<std::string>("all_qmllint"),
        "the injected lint requirement is not required or targets the wrong target");
}

void testJsonAndModuleSuffixTargetsAreNeverEligible()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    const auto snapshot = snapshotWith({"all", "Dashboard_Content_qmllint_json",
        "Dashboard_Content_qmllint_module", "all_qmllint_json", "all_qmllint_module"});
    const auto outcome = applyPolicy(manifest, snapshot);
    require(outcome.success, "policy failed although no target was eligible in a complete catalog");
    const auto& lint = decisionFor(manifest, ExecutionVerificationKind::QmlLint);
    require(lint.status == MandatoryVerificationDecisionStatus::NotApplicable
            && lint.reasonCode == kMandatoryReasonNoQmlLintTarget,
        "no eligible target in a complete catalog must be not_applicable/no_qml_lint_target");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint) == 0
            && countKind(manifest, ExecutionVerificationKind::CMakeBuild) == 0,
        "nothing may be injected when there is no eligible lint target");
}

void testSeveralModuleTargetsWithoutAggregateAreAmbiguous()
{
    requireFailure(manifestOf({taskWriting("t1", {"qml/A.qml"})}),
        snapshotWith({"A_qmllint", "B_qmllint", "all"}),
        kVerificationPolicyLintTargetAmbiguous, "several module targets were not ambiguous");
}

void testSingleModuleTargetIsSelectedWhenCatalogIsComplete()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    require(applyPolicy(manifest, snapshotWith({"A_qmllint", "all"})).success, "policy failed");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).targetId == "A_qmllint",
        "the single module target was not selected");
}

void testAggregateCarriedByTwoPresetsIsAmbiguous()
{
    auto snapshot = snapshotWith({"all_qmllint"});
    addProfile(snapshot, "local-qt-release", {"all_qmllint"});
    requireFailure(manifestOf({taskWriting("t1", {"qml/A.qml"})}), snapshot,
        kVerificationPolicyLintTargetAmbiguous, "two aggregate carriers were not ambiguous");
}

// ---- catalog completeness ---------------------------------------------------

void testCompleteCatalogWithoutLintIsNotApplicable()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    require(applyPolicy(manifest, snapshotWith({"all"})).success, "policy failed");
    const auto& lint = decisionFor(manifest, ExecutionVerificationKind::QmlLint);
    require(lint.status == MandatoryVerificationDecisionStatus::NotApplicable, "not not_applicable");
}

void testIncompleteCatalogWithoutLintFailsClosed()
{
    requireFailure(manifestOf({taskWriting("t1", {"qml/A.qml"})}),
        snapshotWith({"all"}, /*complete=*/false),
        kVerificationPolicyTargetCatalogIncomplete,
        "an incomplete catalog without lint must be target_catalog_incomplete");
}

void testIncompleteCatalogWithOnlyAModuleTargetFailsClosed()
{
    // A hidden all_qmllint would outrank any module target, so a module target
    // found in a catalog that may be cut off cannot be trusted either.
    requireFailure(manifestOf({taskWriting("t1", {"qml/A.qml"})}),
        snapshotWith({"A_qmllint"}, /*complete=*/false),
        kVerificationPolicyTargetCatalogIncomplete,
        "an incomplete catalog with only a module target must fail closed");
}

void testIncompleteCatalogStillSelectsAFoundAggregate()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    require(applyPolicy(manifest, snapshotWith({"all_qmllint"}, /*complete=*/false)).success,
        "a found all_qmllint must be selectable even when the catalog is incomplete");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).targetId == "all_qmllint",
        "the found aggregate was not selected");
}

void testUnconfirmedBuildPresetsAreIncompleteForQmlAndUnresolvableForBuild()
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.state = VerificationEnvironmentState::ExistingProfile;
    addProfile(snapshot, "local-qt-debug", {}, false, false, /*buildPresetsConfirmed=*/false);
    requireFailure(manifestOf({taskWriting("t1", {"qml/A.qml"})}), snapshot,
        kVerificationPolicyTargetCatalogIncomplete, "unconfirmed build presets were not incomplete");
    requireFailure(manifestOf({taskWriting("t1", {"src/main.cpp"})}), snapshot,
        kVerificationPolicyProfileUnresolvable,
        "a build trigger with unconfirmed build presets must be profile_unresolvable");
}

void testQmlWithoutBuildPresetDirectoryFailsTyped()
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.state = VerificationEnvironmentState::ExistingProfile;
    addProfile(snapshot, "local-qt-debug", {"all_qmllint"}, true, true, true, /*resolvedBinaryDir=*/"");
    requireFailure(manifestOf({taskWriting("t1", {"qml/A.qml"})}), snapshot,
        kVerificationPolicyBuildPrerequisiteUnavailable,
        "a lint the binder could not bind must be build_prerequisite_unavailable");
}

// ---- merge, equivalence, conflicts ------------------------------------------

void testDifferentPlannerLintTargetDoesNotSatisfyAndIsKept()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    taskById(manifest, "t1").acceptanceContract->requirements.push_back(
        plannerLint("planner-lint", "Dashboard_Content_qmllint"));
    require(applyPolicy(manifest, snapshotWith({"Dashboard_Content_qmllint", "all_qmllint"})).success,
        "policy failed");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).status
            == MandatoryVerificationDecisionStatus::Injected,
        "a different Planner lint target must not satisfy the strict policy");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint) == 2,
        "the Planner's lint must be kept next to the Backend's");
    const auto* planner = findRequirement(manifest, "planner-lint");
    require(planner != nullptr && planner->origin == ExecutionRequirementOrigin::PlannerDeclared,
        "the Planner's own requirement must stay planner_declared");
}

void testExactPlannerCriteriaSatisfyWithoutDuplicates()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"}), taskWriting("t2", {"qml/B.qml"})});
    taskById(manifest, "t1").acceptanceContract->requirements.push_back(
        plannerLint("planner-lint", "all_qmllint"));
    taskById(manifest, "t2").acceptanceContract->requirements.push_back(plannerBuild("planner-build"));
    require(applyPolicy(manifest, snapshotWith({"all_qmllint", "all"})).success, "policy failed");
    const auto& lint = decisionFor(manifest, ExecutionVerificationKind::QmlLint);
    const auto& build = decisionFor(manifest, ExecutionVerificationKind::CMakeBuild);
    require(lint.status == MandatoryVerificationDecisionStatus::SatisfiedByExisting
            && lint.requirementId == "planner-lint" && lint.taskId == "t1",
        "an exact Planner lint must satisfy the policy and be named in the record");
    require(build.status == MandatoryVerificationDecisionStatus::SatisfiedByExisting
            && build.requirementId == "planner-build" && build.taskId == "t2",
        "an exact Planner build (on another task) must satisfy the build prerequisite");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint) == 1
            && countKind(manifest, ExecutionVerificationKind::CMakeBuild) == 1,
        "a satisfied policy must not add a duplicate");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint,
                ExecutionRequirementOrigin::BackendMandatory) == 0
            && countKind(manifest, ExecutionVerificationKind::CMakeBuild,
                ExecutionRequirementOrigin::BackendMandatory) == 0,
        "no requirement may become backend_mandatory when the Planner's already satisfied it");
}

void testPlannerRequiredFalseIsAConflict()
{
    auto lintManifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    taskById(lintManifest, "t1").acceptanceContract->requirements.push_back(
        plannerLint("planner-lint", "all_qmllint", /*required=*/false));
    requireFailure(lintManifest, snapshotWith({"all_qmllint"}),
        kVerificationPolicyConflictingRequirement, "required=false lint was not a conflict");

    auto buildManifest = manifestOf({taskWriting("t1", {"src/main.cpp"})});
    taskById(buildManifest, "t1").acceptanceContract->requirements.push_back(
        plannerBuild("planner-build", /*required=*/false));
    requireFailure(buildManifest, snapshotWith({"all"}),
        kVerificationPolicyConflictingRequirement, "required=false build was not a conflict");
}

void testReservedIdHeldByDifferentSemanticsIsAConflict()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    taskById(manifest, "t1").acceptanceContract->requirements.push_back(
        plannerLint(std::string(kMandatoryQmlLintRequirementId), "Dashboard_Content_qmllint"));
    requireFailure(manifest, snapshotWith({"Dashboard_Content_qmllint", "all_qmllint"}),
        kVerificationPolicyConflictingRequirement, "a reserved-id collision was not a conflict");
}

void testAnchorWithoutCapacityIsAConflict()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    auto& requirements = taskById(manifest, "t1").acceptanceContract->requirements;
    while (requirements.size() < kMaximumAcceptanceRequirements) {
        requirements.push_back(inspectRequirement("filler-" + std::to_string(requirements.size())));
    }
    requireFailure(manifest, snapshotWith({"all_qmllint"}),
        kVerificationPolicyConflictingRequirement, "a full anchor contract was not a conflict");
}

void testAnchorWithoutAContractGetsOne()
{
    auto task = taskWriting("t1", {"qml/A.qml"});
    task.acceptanceContract.reset();
    auto manifest = manifestOf({std::move(task)});
    require(applyPolicy(manifest, snapshotWith({"all_qmllint"})).success, "policy failed");
    require(manifest.tasks.front().acceptanceContract.has_value()
            && manifest.tasks.front().acceptanceContract->requirements.size() == 2,
        "an anchor without a contract must receive the lint and its build prerequisite");
}

// ---- whole-Manifest uniqueness and anchor -----------------------------------

void testOneBuildAndOneLintForTheWholeManifest()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"}),
        taskWriting("t2", {"qml/B.qml"}), taskWriting("t3", {"qml/C.qml", "src/c.cpp"})});
    require(applyPolicy(manifest, snapshotWith({"all_qmllint", "all"})).success, "policy failed");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint) == 1,
        "more than one qml_lint was added for several QML tasks");
    require(countKind(manifest, ExecutionVerificationKind::CMakeBuild) == 1,
        "more than one cmake_build was added for several QML tasks");
    std::string lintHolder;
    std::string buildHolder;
    findRequirement(manifest, std::string(kMandatoryQmlLintRequirementId), &lintHolder);
    findRequirement(manifest, std::string(kMandatoryCMakeBuildRequirementId), &buildHolder);
    require(lintHolder == "t3" && buildHolder == "t3",
        "lint and its build prerequisite must share the deterministic anchor");
}

void testAnchorIsLastInTopologicalOrderNotLastById()
{
    // "z" has no dependency, "a" depends on it: canonical order is z, a, so the
    // last applicable task is "a" - lexicographic order alone would pick "z".
    auto manifest = manifestOf({taskWriting("a", {"qml/A.qml"}, {"z"}), taskWriting("z", {"qml/Z.qml"})});
    require(applyPolicy(manifest, snapshotWith({"all_qmllint"})).success, "policy failed");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).taskId == "a",
        "the anchor must be the last applicable task in topological order");
}

void testAnchorIsStableUnderTaskPermutation()
{
    auto forward = manifestOf({taskWriting("t1", {"qml/A.qml"}), taskWriting("t2", {"qml/B.qml"}),
        taskWriting("t3", {"docs/readme.md"})});
    auto reversed = manifestOf({taskWriting("t3", {"docs/readme.md"}), taskWriting("t2", {"qml/B.qml"}),
        taskWriting("t1", {"qml/A.qml"})});
    const auto snapshot = snapshotWith({"all_qmllint"});
    require(applyPolicy(forward, snapshot).success && applyPolicy(reversed, snapshot).success,
        "policy failed");
    require(decisionFor(forward, ExecutionVerificationKind::QmlLint).taskId
            == decisionFor(reversed, ExecutionVerificationKind::QmlLint).taskId,
        "the anchor depends on the declaration order of tasks");
    require(decisionFor(forward, ExecutionVerificationKind::QmlLint).taskId == "t2",
        "the anchor must be the last QML-writing task (t3 writes no QML)");
}

// ---- triggers ----------------------------------------------------------------

void testTriggersFromTheWriteScope()
{
    const auto snapshot = snapshotWith({"all_qmllint", "all"});
    const auto lintStatus = [&](std::vector<std::string> writes) {
        auto manifest = manifestOf({taskWriting("t1", std::move(writes))});
        require(applyPolicy(manifest, snapshot).success, "policy failed");
        return std::make_pair(
            decisionFor(manifest, ExecutionVerificationKind::QmlLint).status,
            decisionFor(manifest, ExecutionVerificationKind::CMakeBuild).status);
    };
    using Status = MandatoryVerificationDecisionStatus;

    require(lintStatus({"qml/Page.qml"}).first == Status::Injected, ".qml does not trigger lint");
    require(lintStatus({"qml/Page.QML"}).first == Status::Injected,
        "file extension matching must be case-insensitive");
    require(lintStatus({"qml/util.js"}).first == Status::Injected, ".js does not trigger lint");
    require(lintStatus({"qml/qmldir"}).first == Status::Injected, "qmldir does not trigger lint");

    const auto cppOnly = lintStatus({"src/main.cpp"});
    require(cppOnly.first == Status::NotTriggered && cppOnly.second == Status::Injected,
        "C++ must trigger the build only");
    const auto cmakeOnly = lintStatus({"qml/CMakeLists.txt"});
    require(cmakeOnly.first == Status::NotTriggered && cmakeOnly.second == Status::Injected,
        "a CMake edit without any QML path must trigger the build only");
    const auto docsOnly = lintStatus({"docs/readme.md"});
    require(docsOnly.first == Status::NotTriggered && docsOnly.second == Status::NotTriggered,
        "a documentation-only Manifest must trigger nothing");
}

void testCMakeRegistrationCountsOnlyNextToWrittenQml()
{
    const auto snapshot = snapshotWith({"all_qmllint"});
    // The CMake file sits above the QML file: it is QML registration, so its
    // task is an applicable (and here the last) anchor candidate.
    auto registration = manifestOf({taskWriting("a2", {"qml/V.qml"}),
        taskWriting("z1", {"qml/CMakeLists.txt"})});
    require(applyPolicy(registration, snapshot).success, "policy failed");
    require(decisionFor(registration, ExecutionVerificationKind::QmlLint).taskId == "z1",
        "a CMake file in the directory of written QML must count as QML registration");
    // An unrelated CMake file must not become an anchor candidate.
    auto unrelated = manifestOf({taskWriting("a2", {"qml/V.qml"}),
        taskWriting("z1", {"src/CMakeLists.txt"})});
    require(applyPolicy(unrelated, snapshot).success, "policy failed");
    require(decisionFor(unrelated, ExecutionVerificationKind::QmlLint).taskId == "a2",
        "a CMake file unrelated to the QML paths was treated as QML registration");
}

void testQmlAndCppWithoutLintTargetStillRequiresBuild()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml", "src/a.cpp"})});
    require(applyPolicy(manifest, snapshotWith({"all"})).success, "policy failed");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).status
            == MandatoryVerificationDecisionStatus::NotApplicable, "lint was not not_applicable");
    const auto& build = decisionFor(manifest, ExecutionVerificationKind::CMakeBuild);
    require(build.status == MandatoryVerificationDecisionStatus::Injected
            && build.reasonCode == kMandatoryReasonBuildPathsWritten,
        "the C++ write must still require the single build profile");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint) == 0, "a lint was injected");
}

void testBuildOnlyWithSeveralBuildProfilesIsUnresolvable()
{
    auto snapshot = snapshotWith({"all"});
    addProfile(snapshot, "local-qt-release", {"all"});
    requireFailure(manifestOf({taskWriting("t1", {"src/main.cpp"})}), snapshot,
        kVerificationPolicyProfileUnresolvable,
        "several build profiles must not be guessed for a build-only trigger");
}

void testLintSelectionDeterminesTheBuildProfile()
{
    // Two build profiles, but only the debug one carries all_qmllint: the
    // prerequisite build must use exactly that profile and build preset.
    auto snapshot = snapshotWith({"all_qmllint", "all"});
    addProfile(snapshot, "local-qt-release", {"all"});
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml", "src/a.cpp"})});
    require(applyPolicy(manifest, snapshot).success, "policy failed");
    const auto& build = decisionFor(manifest, ExecutionVerificationKind::CMakeBuild);
    require(build.profileId == "local-qt-debug" && build.buildProfileId == "local-qt-debug",
        "the build prerequisite must follow the selected lint's profile");
}

void testSetupRequiredAddsNothingAndRecordsWhy()
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.state = VerificationEnvironmentState::SetupRequired;
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml", "src/a.cpp"})});
    require(applyPolicy(manifest, snapshot).success, "policy failed on a SetupRequired snapshot");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).reasonCode
                == kMandatoryReasonSetupRequired
            && decisionFor(manifest, ExecutionVerificationKind::CMakeBuild).reasonCode
                == kMandatoryReasonSetupRequired,
        "SetupRequired must be recorded as not_applicable/environment_setup_required");
    require(countKind(manifest, ExecutionVerificationKind::QmlLint) == 0
            && countKind(manifest, ExecutionVerificationKind::CMakeBuild) == 0,
        "nothing may be injected without a usable profile");
}

void testInspectOnlyManifestIsNotTriggered()
{
    auto manifest = manifestOf({taskWriting("t1", {})});
    manifest.tasks.front().requestedCapability = WorkspaceCapability::Inspect;
    require(applyPolicy(manifest, snapshotWith({"all_qmllint"})).success, "policy failed");
    require(decisionFor(manifest, ExecutionVerificationKind::QmlLint).status
                == MandatoryVerificationDecisionStatus::NotTriggered
            && decisionFor(manifest, ExecutionVerificationKind::CMakeBuild).status
                == MandatoryVerificationDecisionStatus::NotTriggered,
        "an inspect-only Manifest must trigger nothing");
}

// ---- idempotence and binding ------------------------------------------------

void testDoubleApplyIsByteEquivalent()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"}), taskWriting("t2", {"src/b.cpp"})});
    taskById(manifest, "t2").acceptanceContract->requirements.push_back(plannerBuild("planner-build"));
    const auto snapshot = snapshotWith({"all_qmllint", "all"});
    require(applyPolicy(manifest, snapshot).success, "first policy run failed");
    const auto first = dump(manifest);
    require(applyPolicy(manifest, snapshot).success, "second policy run failed");
    require(dump(manifest) == first, "applying the policy twice changed the Manifest");

    auto injectedOnly = manifestOf({taskWriting("t1", {"qml/A.qml"})});
    require(applyPolicy(injectedOnly, snapshot).success, "first policy run failed");
    const auto injectedFirst = dump(injectedOnly);
    require(applyPolicy(injectedOnly, snapshot).success, "second policy run failed");
    require(dump(injectedOnly) == injectedFirst, "an injected Manifest was not stable on reapply");
    require(decisionFor(injectedOnly, ExecutionVerificationKind::QmlLint).status
            == MandatoryVerificationDecisionStatus::Injected,
        "the Backend's own earlier requirement must read back as Injected");
}

void testInjectedRequirementsBindAndStayStableAfterBinding()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"}), taskWriting("t2", {"qml/B.qml"})});
    const auto snapshot = snapshotWith({"all_qmllint", "all"});
    require(applyPolicy(manifest, snapshot).success, "policy failed");

    std::vector<ExecutionManifestFinding> findings;
    const auto bound = ManifestVerificationProfileBinder::bindManifest(manifest, snapshot, findings);
    require(bound.success(), "an injected requirement did not bind through the one existing binder");
    const auto* lint = findRequirement(manifest, std::string(kMandatoryQmlLintRequirementId));
    const auto* build = findRequirement(manifest, std::string(kMandatoryCMakeBuildRequirementId));
    require(lint != nullptr && build != nullptr, "injected requirements disappeared");
    require(!lint->verificationArguments.empty() && lint->qmlLintDiagnosticPolicyId.has_value()
            && !build->verificationArguments.empty(),
        "binding did not resolve the injected requirements");
    require(validateExecutionVerificationProfileArguments(*lint).success()
            && validateExecutionVerificationProfileArguments(*build).success(),
        "a bound injected requirement failed the final-shape validation");

    // Re-evaluating an already bound Manifest changes nothing.
    const auto before = dump(manifest);
    require(applyPolicy(manifest, snapshot).success, "policy failed on a bound Manifest");
    require(dump(manifest) == before, "reapplying the policy to a bound Manifest changed it");
}

// ---- record integrity, validation and inheritance ---------------------------

// Two QML tasks (anchor t2: the later one in canonical order) and a C++ task
// with a Planner build the policy recognises: one injected lint, one
// satisfied-by-existing build, so both reference decisions are exercised.
ExecutionManifest appliedManifest()
{
    auto manifest = manifestOf({taskWriting("t1", {"qml/A.qml"}),
        taskWriting("t2", {"qml/B.qml"}), taskWriting("t3", {"src/c.cpp"})});
    taskById(manifest, "t3").acceptanceContract->requirements.push_back(plannerBuild("planner-build"));
    require(applyPolicy(manifest, snapshotWith({"all_qmllint", "all"})).success,
        "test fixture: policy failed");
    return manifest;
}

void testRecordIntegrityAcceptsEveryPolicyOutput()
{
    const auto applied = appliedManifest();
    require(!ManifestMandatoryVerificationPolicy::recordIntegrityProblem(applied).has_value(),
        "A record produced by the policy must be self-consistent");
    require(decisionFor(applied, ExecutionVerificationKind::QmlLint).status
                == MandatoryVerificationDecisionStatus::Injected
            && decisionFor(applied, ExecutionVerificationKind::CMakeBuild).status
                == MandatoryVerificationDecisionStatus::SatisfiedByExisting,
        "test fixture: expected one injected and one satisfied decision");

    auto notTriggered = manifestOf({taskWriting("t1", {"docs/readme.md"})});
    require(applyPolicy(notTriggered, snapshotWith({"all_qmllint"})).success, "policy failed");
    require(!ManifestMandatoryVerificationPolicy::recordIntegrityProblem(notTriggered).has_value(),
        "A not_triggered record must be consistent");
    ExecutionManifest withoutRecord = manifestOf({taskWriting("t1", {"docs/readme.md"})});
    require(!ManifestMandatoryVerificationPolicy::recordIntegrityProblem(withoutRecord).has_value(),
        "A Manifest without a record has nothing to contradict");
}

void testRecordIntegrityRejectsEveryContradiction()
{
    const auto expectProblem = [](const std::function<void(ExecutionManifest&)>& tamper,
                                  const char* message) {
        auto manifest = appliedManifest();
        tamper(manifest);
        require(ManifestMandatoryVerificationPolicy::recordIntegrityProblem(manifest).has_value(),
            message);
    };
    const auto lintDecisionOf = [](ExecutionManifest& manifest) -> ManifestMandatoryVerificationDecision& {
        for (auto& decision : manifest.mandatoryVerification->decisions) {
            if (decision.kind == ExecutionVerificationKind::QmlLint) return decision;
        }
        throw std::runtime_error("test fixture: no lint decision");
    };
    const auto buildDecisionOf = [](ExecutionManifest& manifest) -> ManifestMandatoryVerificationDecision& {
        for (auto& decision : manifest.mandatoryVerification->decisions) {
            if (decision.kind == ExecutionVerificationKind::CMakeBuild) return decision;
        }
        throw std::runtime_error("test fixture: no build decision");
    };

    expectProblem([&](ExecutionManifest& manifest) {
        auto& contract = *taskById(manifest, lintDecisionOf(manifest).taskId).acceptanceContract;
        contract.requirements.erase(std::remove_if(contract.requirements.begin(),
            contract.requirements.end(), [&](const ExecutionAcceptanceRequirement& item) {
                return item.id == lintDecisionOf(manifest).requirementId;
            }), contract.requirements.end());
    }, "A decision naming a requirement that is gone was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        lintDecisionOf(manifest).taskId = "no-such-task";
    }, "A decision naming a task outside the Manifest was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        lintDecisionOf(manifest).taskId = "t1"; // exists, but does not hold the requirement
    }, "A decision naming a task that does not hold the requirement was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        const auto id = lintDecisionOf(manifest).requirementId;
        for (auto& task : manifest.tasks) {
            if (!task.acceptanceContract.has_value()) continue;
            for (auto& item : task.acceptanceContract->requirements) {
                if (item.id == id) item.origin = ExecutionRequirementOrigin::PlannerDeclared;
            }
        }
    }, "An injected decision over a planner_declared requirement was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        const auto id = buildDecisionOf(manifest).requirementId;
        for (auto& task : manifest.tasks) {
            if (!task.acceptanceContract.has_value()) continue;
            for (auto& item : task.acceptanceContract->requirements) {
                if (item.id == id) item.origin = ExecutionRequirementOrigin::BackendMandatory;
            }
        }
    }, "A satisfied_by_existing decision over a backend_mandatory requirement was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        // A kind no other decision uses, so only the mismatch can be the cause.
        lintDecisionOf(manifest).kind = ExecutionVerificationKind::InspectPaths;
    }, "A decision whose kind disagrees with its requirement was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        auto& build = buildDecisionOf(manifest);
        build.status = MandatoryVerificationDecisionStatus::NotApplicable; // still names a requirement
    }, "A not_applicable decision that still names a requirement was accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        manifest.mandatoryVerification->decisions.push_back(
            manifest.mandatoryVerification->decisions.front());
    }, "Two decisions for one kind were accepted");
    expectProblem([&](ExecutionManifest& manifest) {
        manifest.mandatoryVerification->policyId.clear();
    }, "A record without a policy id was accepted");
}

void testValidatorRejectsAnInconsistentRecordAtTheFreezeBoundary()
{
    const auto hasFinding = [](const ExecutionManifest& manifest) {
        const auto findings = ExecutionManifestValidator::validate(manifest, nullptr);
        return std::any_of(findings.begin(), findings.end(), [](const ExecutionManifestFinding& item) {
            return item.code == "manifest.mandatory_verification_inconsistent";
        });
    };
    auto manifest = appliedManifest();
    require(!hasFinding(manifest), "The validator flagged a consistent record");
    for (auto& decision : manifest.mandatoryVerification->decisions) {
        if (decision.kind == ExecutionVerificationKind::QmlLint) decision.requirementId = "dangling";
    }
    require(hasFinding(manifest),
        "The validator must reject a record that names a requirement the Manifest does not hold");
}

void testInheritRecordCarriesVerbatimOnlyWhenTheSubsetIsSelfConsistent()
{
    const auto source = appliedManifest();
    const auto lint = decisionFor(source, ExecutionVerificationKind::QmlLint);
    const auto build = decisionFor(source, ExecutionVerificationKind::CMakeBuild);
    require(lint.taskId == "t2" && build.taskId == "t3",
        "test fixture: unexpected anchors");

    const auto subsetOf = [&](std::vector<std::string> ids) {
        ExecutionManifest subset;
        subset.runId = "child";
        for (const auto& id : ids) {
            for (const auto& task : source.tasks) {
                if (task.id == id) subset.tasks.push_back(task);
            }
        }
        return subset;
    };

    // Both anchors present: carried verbatim, field for field, and the copied
    // requirements keep their Backend origin.
    auto inside = subsetOf({"t2", "t3"});
    inside.mandatoryVerification = ManifestMandatoryVerificationPolicy::inheritRecord(
        source.mandatoryVerification, inside);
    require(inside.mandatoryVerification.has_value(),
        "A record whose requirements are all present in the subset must be carried");
    ExecutionManifest sourceView = subsetOf({"t2", "t3"});
    sourceView.mandatoryVerification = source.mandatoryVerification;
    require(dump(inside) == dump(sourceView), "The carried record is not field-equivalent");
    const auto* carried = findRequirement(inside, std::string(kMandatoryQmlLintRequirementId));
    require(carried != nullptr && carried->origin == ExecutionRequirementOrigin::BackendMandatory,
        "A copied Backend requirement lost its origin");
    require(countKind(inside, ExecutionVerificationKind::QmlLint,
                ExecutionRequirementOrigin::BackendMandatory) == 1,
        "The subset must hold exactly the one Backend requirement it copied, no duplicate");
    require(!ManifestMandatoryVerificationPolicy::recordIntegrityProblem(inside).has_value(),
        "The inherited record must be consistent with the subset");

    // One named anchor outside the subset: no partial projection - omitted.
    auto missingBuildAnchor = subsetOf({"t2"});
    require(!ManifestMandatoryVerificationPolicy::inheritRecord(
                source.mandatoryVerification, missingBuildAnchor).has_value(),
        "A record naming a task outside the subset must be omitted, not carried dangling");
    missingBuildAnchor.mandatoryVerification = std::nullopt;
    require(countKind(missingBuildAnchor, ExecutionVerificationKind::QmlLint,
                ExecutionRequirementOrigin::BackendMandatory) == 1,
        "Omitting the record must not strip a copied requirement's provenance");
    require(!ManifestMandatoryVerificationPolicy::inheritRecord(
                source.mandatoryVerification, subsetOf({"t1"})).has_value(),
        "A subset holding neither anchor must not inherit the record");
    require(!ManifestMandatoryVerificationPolicy::inheritRecord(std::nullopt, inside).has_value(),
        "Nothing is inherited from a source without a record");
}

// ---- fingerprint and classification -----------------------------------------

void testFingerprintMaterialReflectsPolicyAndCatalogCompleteness()
{
    using P = ManifestMandatoryVerificationPolicy;
    const auto complete = snapshotWith({"all_qmllint"}, true);
    const auto incomplete = snapshotWith({"all_qmllint"}, false);
    const auto none = P::fingerprintMaterial(nullptr);
    const auto a = P::fingerprintMaterial(&complete);
    require(a == P::fingerprintMaterial(&complete), "fingerprint material is not deterministic");
    require(a != P::fingerprintMaterial(&incomplete),
        "catalog completeness is missing from the fingerprint material");
    require(a != none, "a snapshot and no snapshot share fingerprint material");
    require(a.find(std::string(kManifestMandatoryVerificationPolicyId)) != std::string::npos
            && none.find(std::string(kManifestMandatoryVerificationPolicyId)) != std::string::npos,
        "the policy id is missing from the fingerprint material");
}

void testPolicyFailureCodesAreClassifiedNonRepairable()
{
    for (const auto* code : {kVerificationPolicyLintTargetAmbiguous,
             kVerificationPolicyTargetCatalogIncomplete, kVerificationPolicyConflictingRequirement,
             kVerificationPolicyBuildPrerequisiteUnavailable, kVerificationPolicyProfileUnresolvable}) {
        require(isMandatoryVerificationPolicyFailureCode(code), "a policy code is not recognised");
    }
    require(!isMandatoryVerificationPolicyFailureCode("manifest.verification_profile_unknown")
            && !isMandatoryVerificationPolicyFailureCode("planner.missing_submission"),
        "an ordinary repairable code was classified as a policy failure");
}

} // namespace

int runManifestMandatoryVerificationPolicyTests()
{
    try {
        testEligibleTargetPredicateIsExactAndCaseSensitive();
        testAggregateTargetIsPreferredOverModuleTarget();
        testJsonAndModuleSuffixTargetsAreNeverEligible();
        testSeveralModuleTargetsWithoutAggregateAreAmbiguous();
        testSingleModuleTargetIsSelectedWhenCatalogIsComplete();
        testAggregateCarriedByTwoPresetsIsAmbiguous();
        testCompleteCatalogWithoutLintIsNotApplicable();
        testIncompleteCatalogWithoutLintFailsClosed();
        testIncompleteCatalogWithOnlyAModuleTargetFailsClosed();
        testIncompleteCatalogStillSelectsAFoundAggregate();
        testUnconfirmedBuildPresetsAreIncompleteForQmlAndUnresolvableForBuild();
        testQmlWithoutBuildPresetDirectoryFailsTyped();
        testDifferentPlannerLintTargetDoesNotSatisfyAndIsKept();
        testExactPlannerCriteriaSatisfyWithoutDuplicates();
        testPlannerRequiredFalseIsAConflict();
        testReservedIdHeldByDifferentSemanticsIsAConflict();
        testAnchorWithoutCapacityIsAConflict();
        testAnchorWithoutAContractGetsOne();
        testOneBuildAndOneLintForTheWholeManifest();
        testAnchorIsLastInTopologicalOrderNotLastById();
        testAnchorIsStableUnderTaskPermutation();
        testTriggersFromTheWriteScope();
        testCMakeRegistrationCountsOnlyNextToWrittenQml();
        testQmlAndCppWithoutLintTargetStillRequiresBuild();
        testBuildOnlyWithSeveralBuildProfilesIsUnresolvable();
        testLintSelectionDeterminesTheBuildProfile();
        testSetupRequiredAddsNothingAndRecordsWhy();
        testInspectOnlyManifestIsNotTriggered();
        testDoubleApplyIsByteEquivalent();
        testInjectedRequirementsBindAndStayStableAfterBinding();
        testRecordIntegrityAcceptsEveryPolicyOutput();
        testRecordIntegrityRejectsEveryContradiction();
        testValidatorRejectsAnInconsistentRecordAtTheFreezeBoundary();
        testInheritRecordCarriesVerbatimOnlyWhenTheSubsetIsSelfConsistent();
        testFingerprintMaterialReflectsPolicyAndCatalogCompleteness();
        testPolicyFailureCodesAreClassifiedNonRepairable();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ManifestMandatoryVerificationPolicy tests failed: " << exception.what() << '\n';
        return 1;
    }
}
