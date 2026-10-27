#include "ai/application/verification/ManifestMandatoryVerificationPolicy.hpp"

#include "ai/application/ExecutionManifestDependencyGraph.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// File extensions and file names are compared ASCII-case-insensitively (a
// Windows workspace may name a file Main.QML). CMake TARGET names are never
// case-folded: see isEligibleQmlLintTarget.
std::string lowerExtension(const fs::path& path)
{
    return lowerAscii(path.extension().string());
}

bool isQmlPath(const fs::path& path)
{
    const auto extension = lowerExtension(path);
    return extension == ".qml" || extension == ".js" || extension == ".mjs"
        || lowerAscii(path.filename().string()) == "qmldir";
}

bool isCMakeFile(const fs::path& path)
{
    return lowerAscii(path.filename().string()) == "cmakelists.txt"
        || lowerExtension(path) == ".cmake";
}

bool isBuildPath(const fs::path& path)
{
    static const std::set<std::string> kExtensions {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".qrc"};
    return kExtensions.contains(lowerExtension(path)) || isCMakeFile(path);
}

std::vector<std::string> directoryComponents(const fs::path& path)
{
    std::vector<std::string> parts;
    for (const auto& part : path.lexically_normal()) {
        auto text = part.generic_string();
        if (text.empty() || text == ".") continue;
        parts.push_back(std::move(text));
    }
    return parts;
}

// True when every component of `ancestor` is the leading component of
// `descendant` (an empty `ancestor` is the workspace root and always matches).
bool isSameOrAncestorDirectory(const fs::path& ancestor, const fs::path& descendant)
{
    const auto ancestorParts = directoryComponents(ancestor);
    const auto descendantParts = directoryComponents(descendant);
    return ancestorParts.size() <= descendantParts.size()
        && std::equal(ancestorParts.begin(), ancestorParts.end(), descendantParts.begin());
}

std::vector<fs::path> writePaths(const ExecutionManifestTask& task)
{
    std::vector<fs::path> paths;
    paths.reserve(task.ownedPaths.size() + task.plannedCreatePaths.size());
    paths.insert(paths.end(), task.ownedPaths.begin(), task.ownedPaths.end());
    paths.insert(paths.end(), task.plannedCreatePaths.begin(), task.plannedCreatePaths.end());
    return paths;
}

struct ScopeImpact {
    std::set<std::string> qmlTasks;   // tasks whose writes impact QML
    std::set<std::string> buildTasks; // tasks whose writes impact the build
};

ScopeImpact analyzeScope(const ExecutionManifest& manifest)
{
    // Directories of every QML path any task writes: a CMake file written in
    // (or above) one of them is QML registration.
    std::vector<fs::path> qmlDirectories;
    for (const auto& task : manifest.tasks) {
        for (const auto& path : writePaths(task)) {
            if (isQmlPath(path)) qmlDirectories.push_back(path.parent_path());
        }
    }
    ScopeImpact impact;
    for (const auto& task : manifest.tasks) {
        for (const auto& path : writePaths(task)) {
            if (isBuildPath(path)) impact.buildTasks.insert(task.id);
            bool qml = isQmlPath(path);
            if (!qml && isCMakeFile(path)) {
                const auto directory = path.parent_path();
                qml = std::any_of(qmlDirectories.begin(), qmlDirectories.end(),
                    [&](const fs::path& qmlDirectory) {
                        return isSameOrAncestorDirectory(directory, qmlDirectory);
                    });
            }
            if (qml) impact.qmlTasks.insert(task.id);
        }
    }
    return impact;
}

// Kahn's algorithm over the effective dependency graph (Planner-declared plus
// Backend-derived edges), always taking the lexicographically smallest ready
// task id, so the order is a pure function of the Manifest. Any task a cycle
// would leave out (the validator rejects cycles before this runs) is appended
// in id order so the function stays total.
std::vector<std::string> canonicalTaskOrder(const ExecutionManifest& manifest)
{
    std::set<std::string> ids;
    for (const auto& task : manifest.tasks) ids.insert(task.id);
    std::map<std::string, std::set<std::string>> dependents;
    std::map<std::string, std::size_t> indegree;
    for (const auto& id : ids) indegree[id] = 0;
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& edge : deriveManifestDependencyGraph(manifest)) {
        if (edge.consumerTaskId == edge.producerTaskId
            || !ids.contains(edge.consumerTaskId) || !ids.contains(edge.producerTaskId)) {
            continue;
        }
        if (!seen.emplace(edge.producerTaskId, edge.consumerTaskId).second) continue;
        dependents[edge.producerTaskId].insert(edge.consumerTaskId);
        ++indegree[edge.consumerTaskId];
    }
    std::set<std::string> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.insert(id);
    }
    std::vector<std::string> order;
    std::set<std::string> placed;
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        placed.insert(id);
        for (const auto& consumer : dependents[id]) {
            if (--indegree[consumer] == 0) ready.insert(consumer);
        }
    }
    for (const auto& id : ids) {
        if (!placed.contains(id)) order.push_back(id);
    }
    return order;
}

std::optional<std::string> lastApplicable(
    const std::vector<std::string>& order, const std::set<std::string>& applicable)
{
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (applicable.contains(*it)) return *it;
    }
    return std::nullopt;
}

struct CatalogEntry {
    std::string profileId;
    std::string buildProfileId;
    std::string target; // empty for a build-only pair
};

struct Catalog {
    // True only when every considered profile confirmed its build presets and
    // every build preset's target catalog is confirmed AND provably complete.
    bool complete {true};
    bool anyProfileBuildUnconfirmed {false};
    std::vector<CatalogEntry> aggregateTargets; // all_qmllint
    std::vector<CatalogEntry> moduleTargets;    // other eligible *_qmllint
    std::vector<CatalogEntry> buildPairs;       // every confirmed (profile, build preset)
};

Catalog analyzeCatalog(const VerificationEnvironmentSnapshot& snapshot)
{
    Catalog catalog;
    for (const auto& profile : snapshot.profiles) {
        if (!profile.buildPresetsConfirmed) {
            catalog.complete = false;
            catalog.anyProfileBuildUnconfirmed = true;
            continue;
        }
        for (const auto& buildPreset : profile.buildPresets) {
            catalog.buildPairs.push_back({profile.profileId, buildPreset.name, {}});
            if (!(buildPreset.allowedTargetsConfirmed && buildPreset.allowedTargetsComplete)) {
                catalog.complete = false;
            }
            for (const auto& target : buildPreset.allowedTargets) {
                if (target == kAggregateQmlLintTarget) {
                    catalog.aggregateTargets.push_back(
                        {profile.profileId, buildPreset.name, target});
                } else if (ManifestMandatoryVerificationPolicy::isEligibleQmlLintTarget(target)) {
                    catalog.moduleTargets.push_back(
                        {profile.profileId, buildPreset.name, target});
                }
            }
        }
    }
    return catalog;
}

enum class LintSelectionKind { Selected, Ambiguous, Incomplete, None };

struct LintSelection {
    LintSelectionKind kind {LintSelectionKind::None};
    CatalogEntry entry;
};

LintSelection selectLintTarget(const Catalog& catalog)
{
    if (catalog.aggregateTargets.size() == 1) {
        return {LintSelectionKind::Selected, catalog.aggregateTargets.front()};
    }
    if (catalog.aggregateTargets.size() > 1) return {LintSelectionKind::Ambiguous, {}};
    if (!catalog.complete) return {LintSelectionKind::Incomplete, {}};
    if (catalog.moduleTargets.size() == 1) {
        return {LintSelectionKind::Selected, catalog.moduleTargets.front()};
    }
    if (catalog.moduleTargets.size() > 1) return {LintSelectionKind::Ambiguous, {}};
    return {LintSelectionKind::None, {}};
}

const VerificationProfile* findProfile(
    const VerificationEnvironmentSnapshot& snapshot, const std::string& profileId)
{
    for (const auto& profile : snapshot.profiles) {
        if (profile.profileId == profileId) return &profile;
    }
    return nullptr;
}

struct Selection {
    ExecutionVerificationKind kind {ExecutionVerificationKind::QmlLint};
    std::string profileId;
    std::string buildProfileId;
    std::string target; // empty for cmake_build
};

bool sameSelection(const ExecutionAcceptanceRequirement& requirement, const Selection& selection)
{
    const std::optional<std::string> target = selection.target.empty()
        ? std::nullopt : std::optional<std::string>(selection.target);
    return requirement.verificationKind == selection.kind
        && !requirement.verificationBootstrapPending
        && requirement.verificationProfileId == std::optional<std::string>(selection.profileId)
        && requirement.verificationBuildProfileId
            == std::optional<std::string>(selection.buildProfileId)
        && requirement.verificationTargetId == target;
}

struct ExistingMatch {
    // First (canonical order) requirement with the same selection and
    // required=true.
    const ExecutionManifestTask* task {nullptr};
    const ExecutionAcceptanceRequirement* requirement {nullptr};
    // A requirement with this selection exists but is required=false, or the
    // reserved id is held by a requirement that is not this selection.
    std::optional<std::string> conflict;
};

ExistingMatch findExisting(
    const ExecutionManifest& manifest,
    const std::vector<std::string>& order,
    const Selection& selection,
    std::string_view reservedId)
{
    ExistingMatch match;
    std::map<std::string, const ExecutionManifestTask*> byId;
    for (const auto& task : manifest.tasks) byId[task.id] = &task;
    for (const auto& id : order) {
        const auto* task = byId.at(id);
        if (!task->acceptanceContract.has_value()) continue;
        for (const auto& requirement : task->acceptanceContract->requirements) {
            const bool same = sameSelection(requirement, selection);
            if (requirement.id == reservedId && !(same && requirement.required)) {
                if (!match.conflict.has_value()) {
                    match.conflict = "Requirement id '" + std::string(reservedId)
                        + "' is reserved by the Backend mandatory verification policy but is "
                          "held by a requirement with different semantics (task '"
                        + task->id + "')";
                }
            } else if (same && !requirement.required) {
                if (!match.conflict.has_value()) {
                    match.conflict = "Task '" + task->id + "' declares requirement '"
                        + requirement.id + "' with the Backend-required verification but "
                          "required=false";
                }
            } else if (same && requirement.required && match.requirement == nullptr) {
                match.task = task;
                match.requirement = &requirement;
            }
        }
    }
    return match;
}

ExecutionAcceptanceRequirement makeRequirement(
    const Selection& selection, std::string_view id, const char* description)
{
    ExecutionAcceptanceRequirement requirement;
    requirement.id = std::string(id);
    requirement.description = description;
    requirement.verificationKind = selection.kind;
    requirement.required = true;
    requirement.verificationProfileId = selection.profileId;
    requirement.verificationBuildProfileId = selection.buildProfileId;
    if (!selection.target.empty()) requirement.verificationTargetId = selection.target;
    requirement.verificationBootstrapPending = false;
    requirement.origin = ExecutionRequirementOrigin::BackendMandatory;
    return requirement;
}

constexpr const char* kQmlLintDescription =
    "Backend-required QML lint (manifest_mandatory_verification_v1): the combined "
    "candidate is linted after every task completes. No worker action is required.";
constexpr const char* kCMakeBuildDescription =
    "Backend-required clean configure and build (manifest_mandatory_verification_v1): "
    "run once for the combined candidate after every task completes. No worker action "
    "is required.";

common::Result<void> fail(
    std::vector<ExecutionManifestFinding>& findings,
    const char* code,
    std::string message,
    std::optional<std::string> taskId = std::nullopt)
{
    findings.push_back({code, message, std::move(taskId)});
    return common::Result<void>::failure({code, std::move(message)});
}

struct PlannedInjection {
    std::string taskId;
    ExecutionAcceptanceRequirement requirement;
};

ManifestMandatoryVerificationDecision decision(
    ExecutionVerificationKind kind, MandatoryVerificationDecisionStatus status,
    const char* reasonCode)
{
    ManifestMandatoryVerificationDecision value;
    value.kind = kind;
    value.status = status;
    value.reasonCode = reasonCode;
    return value;
}

void fillIdentity(ManifestMandatoryVerificationDecision& value, const Selection& selection)
{
    value.profileId = selection.profileId;
    value.buildProfileId = selection.buildProfileId;
    value.targetId = selection.target;
}

constexpr std::size_t kMaximumRecordDecisions = 8;
constexpr std::size_t kMaximumRecordTextBytes = 256;

std::optional<std::string> checkRecordAgainstTasks(
    const ManifestMandatoryVerificationRecord& record,
    const std::vector<ExecutionManifestTask>& tasks)
{
    if (record.policyId.empty() || record.policyId.size() > kMaximumRecordTextBytes
        || record.mode.empty() || record.mode.size() > kMaximumRecordTextBytes) {
        return std::string("The mandatory verification record header is empty or unbounded");
    }
    if (record.decisions.size() > kMaximumRecordDecisions) {
        return std::string("The mandatory verification record has too many decisions");
    }
    std::set<ExecutionVerificationKind> kinds;
    for (const auto& decision : record.decisions) {
        if (!kinds.insert(decision.kind).second) {
            return std::string("The mandatory verification record has two decisions for one kind");
        }
        const bool references = decision.status == MandatoryVerificationDecisionStatus::Injected
            || decision.status == MandatoryVerificationDecisionStatus::SatisfiedByExisting;
        if (!references) {
            if (!decision.requirementId.empty() || !decision.taskId.empty()) {
                return std::string("A not_triggered/not_applicable decision must not name a "
                                   "requirement or task");
            }
            continue;
        }
        if (decision.requirementId.empty() || decision.taskId.empty()
            || decision.requirementId.size() > kMaximumRecordTextBytes
            || decision.taskId.size() > kMaximumRecordTextBytes) {
            return std::string("An injected/satisfied decision must name a bounded requirement "
                               "and task");
        }
        const auto task = std::find_if(tasks.begin(), tasks.end(),
            [&](const ExecutionManifestTask& candidate) { return candidate.id == decision.taskId; });
        if (task == tasks.end()) {
            return "The mandatory verification record names task '" + decision.taskId
                + "', which is not in this Manifest";
        }
        const ExecutionAcceptanceRequirement* requirement = nullptr;
        if (task->acceptanceContract.has_value()) {
            for (const auto& candidate : task->acceptanceContract->requirements) {
                if (candidate.id == decision.requirementId) {
                    requirement = &candidate;
                    break;
                }
            }
        }
        if (requirement == nullptr) {
            return "The mandatory verification record names requirement '"
                + decision.requirementId + "', which task '" + decision.taskId
                + "' does not hold";
        }
        if (requirement->verificationKind != decision.kind) {
            return "The mandatory verification record's kind disagrees with requirement '"
                + decision.requirementId + "'";
        }
        const auto expected = decision.status == MandatoryVerificationDecisionStatus::Injected
            ? ExecutionRequirementOrigin::BackendMandatory
            : ExecutionRequirementOrigin::PlannerDeclared;
        if (requirement->origin != expected) {
            return "The mandatory verification record's status contradicts the origin of "
                   "requirement '" + decision.requirementId + "'";
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> ManifestMandatoryVerificationPolicy::recordIntegrityProblem(
    const ExecutionManifest& manifest)
{
    if (!manifest.mandatoryVerification.has_value()) return std::nullopt;
    return checkRecordAgainstTasks(*manifest.mandatoryVerification, manifest.tasks);
}

std::optional<ManifestMandatoryVerificationRecord> ManifestMandatoryVerificationPolicy::inheritRecord(
    const std::optional<ManifestMandatoryVerificationRecord>& sourceRecord,
    const ExecutionManifest& subsetManifest)
{
    if (!sourceRecord.has_value()) return std::nullopt;
    if (checkRecordAgainstTasks(*sourceRecord, subsetManifest.tasks).has_value()) return std::nullopt;
    return sourceRecord;
}

bool ManifestMandatoryVerificationPolicy::isEligibleQmlLintTarget(
    std::string_view target) noexcept
{
    return target.size() > kQmlLintTargetSuffix.size() && target.ends_with(kQmlLintTargetSuffix);
}

std::string ManifestMandatoryVerificationPolicy::fingerprintMaterial(
    const VerificationEnvironmentSnapshot* snapshot)
{
    std::ostringstream out;
    out << kManifestMandatoryVerificationPolicyId << ';' << kManifestMandatoryVerificationModeAuto;
    if (snapshot == nullptr) {
        out << ";no-snapshot";
        return out.str();
    }
    out << ";state=" << toString(snapshot->state);
    for (const auto& profile : snapshot->profiles) {
        out << ";profile=" << profile.profileId << ':' << (profile.buildPresetsConfirmed ? 1 : 0);
        for (const auto& buildPreset : profile.buildPresets) {
            out << ";preset=" << buildPreset.name << ':' << (buildPreset.allowedTargetsConfirmed ? 1 : 0)
                << (buildPreset.allowedTargetsComplete ? 1 : 0);
        }
    }
    return out.str();
}

common::Result<void> ManifestMandatoryVerificationPolicy::apply(
    ExecutionManifest& manifest,
    const VerificationEnvironmentSnapshot& snapshot,
    std::vector<ExecutionManifestFinding>& findings)
{
    ManifestMandatoryVerificationRecord record;
    ManifestMandatoryVerificationDecision lintDecision =
        decision(ExecutionVerificationKind::QmlLint,
            MandatoryVerificationDecisionStatus::NotTriggered, kMandatoryReasonNoQmlPaths);
    ManifestMandatoryVerificationDecision buildDecision =
        decision(ExecutionVerificationKind::CMakeBuild,
            MandatoryVerificationDecisionStatus::NotTriggered, kMandatoryReasonNoBuildPaths);

    const auto impact = analyzeScope(manifest);
    const bool qmlTriggered = !impact.qmlTasks.empty();
    const bool buildTriggered = !impact.buildTasks.empty();
    const auto finish = [&]() {
        record.decisions = {std::move(lintDecision), std::move(buildDecision)};
        manifest.mandatoryVerification = std::move(record);
        return common::Result<void>::ok();
    };

    if (!qmlTriggered && !buildTriggered) return finish();

    // Without a usable profile there is nothing the Backend can bind; the
    // existing setup-required / bootstrap lifecycle owns that case, so the
    // policy records the reason and does not interfere with it.
    if (snapshot.state != VerificationEnvironmentState::ExistingProfile) {
        if (qmlTriggered) {
            lintDecision = decision(ExecutionVerificationKind::QmlLint,
                MandatoryVerificationDecisionStatus::NotApplicable, kMandatoryReasonSetupRequired);
        }
        if (buildTriggered) {
            buildDecision = decision(ExecutionVerificationKind::CMakeBuild,
                MandatoryVerificationDecisionStatus::NotApplicable, kMandatoryReasonSetupRequired);
        }
        return finish();
    }

    const auto order = canonicalTaskOrder(manifest);
    const auto catalog = analyzeCatalog(snapshot);

    // ---- 1. Resolve what the policy requires (no mutation yet) -------------
    std::optional<Selection> lintSelection;
    std::optional<Selection> buildSelection;
    std::optional<std::string> lintAnchor;
    std::optional<std::string> buildAnchor;

    if (qmlTriggered) {
        const auto selected = selectLintTarget(catalog);
        switch (selected.kind) {
        case LintSelectionKind::Ambiguous:
            return fail(findings, kVerificationPolicyLintTargetAmbiguous,
                "The verification environment offers more than one eligible QML lint target "
                "and no single 'all_qmllint'; the Backend will not guess one");
        case LintSelectionKind::Incomplete:
            return fail(findings, kVerificationPolicyTargetCatalogIncomplete,
                "This Manifest writes QML, but the verification environment's target catalog "
                "is not provably complete (it may have been cut off by the per-preset target "
                "limit), so the absence or ambiguity of a QML lint target cannot be trusted");
        case LintSelectionKind::None:
            lintDecision = decision(ExecutionVerificationKind::QmlLint,
                MandatoryVerificationDecisionStatus::NotApplicable, kMandatoryReasonNoQmlLintTarget);
            break;
        case LintSelectionKind::Selected:
            lintSelection = Selection {ExecutionVerificationKind::QmlLint,
                selected.entry.profileId, selected.entry.buildProfileId, selected.entry.target};
            lintAnchor = lastApplicable(order, impact.qmlTasks);
            break;
        }
    }

    if (lintSelection.has_value()) {
        // The lint is only meaningful after a clean configure/build of the same
        // profile and build preset.
        const auto* profile = findProfile(snapshot, lintSelection->profileId);
        if (profile == nullptr || !profile->buildPresetsConfirmed) {
            return fail(findings, kVerificationPolicyBuildPrerequisiteUnavailable,
                "The selected QML lint target's profile has no confirmed build preset to "
                "build before linting");
        }
        // The binder resolves a qml_lint requirement against the candidate-
        // relative build directory and rejects a profile without one; a
        // selection that cannot bind must fail here, typed and non-repairable,
        // not later as a binder rejection that would be sent to a paid repair.
        if (profile->resolvedBinaryDir.empty()) {
            return fail(findings, kVerificationPolicyBuildPrerequisiteUnavailable,
                "The selected QML lint target's profile has no candidate-relative build "
                "directory available, so the lint cannot be bound");
        }
        buildSelection = Selection {ExecutionVerificationKind::CMakeBuild,
            lintSelection->profileId, lintSelection->buildProfileId, {}};
        buildAnchor = lintAnchor;
    } else if (buildTriggered) {
        if (catalog.buildPairs.size() == 1) {
            const auto& pair = catalog.buildPairs.front();
            buildSelection = Selection {ExecutionVerificationKind::CMakeBuild,
                pair.profileId, pair.buildProfileId, {}};
            buildAnchor = lastApplicable(order, impact.buildTasks);
        } else if (catalog.buildPairs.size() > 1 || catalog.anyProfileBuildUnconfirmed) {
            return fail(findings, kVerificationPolicyProfileUnresolvable,
                "This Manifest changes buildable sources, but the verification environment "
                "does not identify exactly one build profile to require; the Backend will not "
                "guess one");
        } else {
            buildDecision = decision(ExecutionVerificationKind::CMakeBuild,
                MandatoryVerificationDecisionStatus::NotApplicable, kMandatoryReasonNoBuildProfile);
        }
    }

    // ---- 2. Merge against what the Manifest already declares ---------------
    std::vector<PlannedInjection> injections;

    const auto merge = [&](const Selection& selection, std::string_view reservedId,
                           const char* description, const std::optional<std::string>& anchor,
                           const char* injectedReason,
                           ManifestMandatoryVerificationDecision& out) -> common::Result<void> {
        const auto existing = findExisting(manifest, order, selection, reservedId);
        if (existing.conflict.has_value()) {
            return fail(findings, kVerificationPolicyConflictingRequirement, *existing.conflict);
        }
        out = decision(selection.kind, MandatoryVerificationDecisionStatus::Injected,
            injectedReason);
        fillIdentity(out, selection);
        if (existing.requirement != nullptr) {
            out.requirementId = existing.requirement->id;
            out.taskId = existing.task->id;
            // The Backend's own earlier requirement reads back as Injected so a
            // repeated evaluation reproduces the identical record.
            if (existing.requirement->origin != ExecutionRequirementOrigin::BackendMandatory) {
                out.status = MandatoryVerificationDecisionStatus::SatisfiedByExisting;
            }
            return common::Result<void>::ok();
        }
        if (!anchor.has_value()) {
            return fail(findings, kVerificationPolicyConflictingRequirement,
                "No task in this Manifest can carry the Backend-required requirement");
        }
        out.requirementId = std::string(reservedId);
        out.taskId = *anchor;
        injections.push_back({*anchor, makeRequirement(selection, reservedId, description)});
        return common::Result<void>::ok();
    };

    if (lintSelection.has_value()) {
        const auto merged = merge(*lintSelection, kMandatoryQmlLintRequirementId,
            kQmlLintDescription, lintAnchor, kMandatoryReasonQmlPathsWritten, lintDecision);
        if (!merged.success()) return merged;
    }
    if (buildSelection.has_value()) {
        const char* reason = impact.buildTasks.empty()
            ? kMandatoryReasonQmlLintPrerequisite : kMandatoryReasonBuildPathsWritten;
        const auto merged = merge(*buildSelection, kMandatoryCMakeBuildRequirementId,
            kCMakeBuildDescription, buildAnchor, reason, buildDecision);
        if (!merged.success()) return merged;
    }

    // ---- 3. Capacity check, then mutate -----------------------------------
    std::map<std::string, std::size_t> additions;
    for (const auto& injection : injections) ++additions[injection.taskId];
    for (const auto& [taskId, count] : additions) {
        const auto it = std::find_if(manifest.tasks.begin(), manifest.tasks.end(),
            [&](const ExecutionManifestTask& task) { return task.id == taskId; });
        const std::size_t current = it != manifest.tasks.end() && it->acceptanceContract.has_value()
            ? it->acceptanceContract->requirements.size() : 0;
        if (current + count > kMaximumAcceptanceRequirements) {
            return fail(findings, kVerificationPolicyConflictingRequirement,
                "The task that must carry the Backend-required verification already has the "
                "maximum number of acceptance requirements",
                taskId);
        }
    }
    for (auto& injection : injections) {
        auto it = std::find_if(manifest.tasks.begin(), manifest.tasks.end(),
            [&](const ExecutionManifestTask& task) { return task.id == injection.taskId; });
        if (it == manifest.tasks.end()) continue;
        if (!it->acceptanceContract.has_value()) {
            ExecutionAcceptanceContract contract;
            it->acceptanceContract = std::move(contract);
        }
        it->acceptanceContract->requirements.push_back(std::move(injection.requirement));
    }
    return finish();
}

bool isMandatoryVerificationPolicyFailureCode(std::string_view code) noexcept
{
    return code.starts_with(kMandatoryVerificationFailurePrefix);
}

} // namespace ai::application
