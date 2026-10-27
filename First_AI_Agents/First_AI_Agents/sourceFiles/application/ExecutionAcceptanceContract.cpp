#include "ai/application/ExecutionAcceptanceContract.hpp"

#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ai::application {

namespace {

bool blank(std::string_view value)
{
    return value.empty() || std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

bool boundedNonblank(std::string_view value)
{
    return !blank(value) && value.size() <= kMaximumAcceptanceStringBytes;
}

bool safeToken(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

bool safeRelativePath(std::string_view value)
{
    if (!boundedNonblank(value) || value.front() == '/' || value.back() == '/'
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(start,
            (end == std::string_view::npos ? value.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (const unsigned char ch : part) {
            if (ch < 0x20 || ch == 0x7f || ch == '*' || ch == '?' || ch == '['
                || ch == ']' || ch == '{' || ch == '}') return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

common::Result<void> failure(std::string code, std::string message)
{
    return common::Result<void>::failure({std::move(code), std::move(message)});
}

bool validStatus(ExecutionVerificationStatus status)
{
    switch (status) {
    case ExecutionVerificationStatus::NotRun:
    case ExecutionVerificationStatus::Passed:
    case ExecutionVerificationStatus::Failed:
    case ExecutionVerificationStatus::NotVerifiable: return true;
    }
    return false;
}

bool validKind(ExecutionVerificationKind kind)
{
    switch (kind) {
    case ExecutionVerificationKind::InspectPaths:
    case ExecutionVerificationKind::CMakeBuild:
    case ExecutionVerificationKind::QmlLint:
    case ExecutionVerificationKind::CMakeTargetConsistency: return true;
    }
    return false;
}

std::optional<std::string_view> argumentValue(
    const ExecutionAcceptanceRequirement& requirement, std::string_view name)
{
    const auto found = std::find_if(requirement.verificationArguments.begin(),
        requirement.verificationArguments.end(), [&](const auto& argument) {
            return argument.name == name;
        });
    if (found == requirement.verificationArguments.end()) return std::nullopt;
    return found->value;
}

common::Result<void> invalidProfile(std::string message)
{
    return failure("acceptance.invalid_verification_arguments", std::move(message));
}

} // namespace

const char* toString(ExecutionVerificationKind value) noexcept
{
    switch (value) {
    case ExecutionVerificationKind::InspectPaths: return "inspect_paths";
    case ExecutionVerificationKind::CMakeBuild: return "cmake_build";
    case ExecutionVerificationKind::QmlLint: return "qml_lint";
    case ExecutionVerificationKind::CMakeTargetConsistency:
        return "cmake_target_consistency";
    }
    return "inspect_paths";
}

std::optional<ExecutionVerificationKind> executionVerificationKindFromString(
    std::string_view value) noexcept
{
    if (value == "inspect_paths") return ExecutionVerificationKind::InspectPaths;
    if (value == "cmake_build") return ExecutionVerificationKind::CMakeBuild;
    if (value == "qml_lint") return ExecutionVerificationKind::QmlLint;
    if (value == "cmake_target_consistency")
        return ExecutionVerificationKind::CMakeTargetConsistency;
    return std::nullopt;
}

const char* toString(ExecutionRequirementOrigin value) noexcept
{
    switch (value) {
    case ExecutionRequirementOrigin::PlannerDeclared: return "planner_declared";
    case ExecutionRequirementOrigin::BackendMandatory: return "backend_mandatory";
    }
    return "planner_declared";
}

std::optional<ExecutionRequirementOrigin> executionRequirementOriginFromString(
    std::string_view value) noexcept
{
    if (value == "planner_declared") return ExecutionRequirementOrigin::PlannerDeclared;
    if (value == "backend_mandatory") return ExecutionRequirementOrigin::BackendMandatory;
    return std::nullopt;
}

const char* toString(ExecutionVerificationStatus value) noexcept
{
    switch (value) {
    case ExecutionVerificationStatus::NotRun: return "not_run";
    case ExecutionVerificationStatus::Passed: return "passed";
    case ExecutionVerificationStatus::Failed: return "failed";
    case ExecutionVerificationStatus::NotVerifiable: return "not_verifiable";
    }
    return "not_run";
}

std::optional<ExecutionVerificationStatus> executionVerificationStatusFromString(
    std::string_view value) noexcept
{
    if (value == "not_run") return ExecutionVerificationStatus::NotRun;
    if (value == "passed") return ExecutionVerificationStatus::Passed;
    if (value == "failed") return ExecutionVerificationStatus::Failed;
    if (value == "not_verifiable") return ExecutionVerificationStatus::NotVerifiable;
    return std::nullopt;
}

common::Result<void> validateExecutionAcceptanceContract(
    const ExecutionAcceptanceContract& contract)
{
    if (contract.schemaVersion != kExecutionAcceptanceSchemaVersion) {
        return failure("acceptance.unsupported_schema",
            "Acceptance contract schema version is not supported");
    }
    if (contract.requirements.empty()
        || contract.requirements.size() > kMaximumAcceptanceRequirements) {
        return failure("acceptance.requirement_count",
            "Acceptance contract requirement count is outside the supported range");
    }

    std::unordered_set<std::string> ids;
    for (const auto& requirement : contract.requirements) {
        if (!boundedNonblank(requirement.id)) {
            return failure("acceptance.invalid_requirement_id",
                "Acceptance requirement id must be nonblank and bounded");
        }
        if (!ids.emplace(requirement.id).second) {
            return failure("acceptance.duplicate_requirement_id",
                "Acceptance requirement ids must be unique");
        }
        if (!boundedNonblank(requirement.description)) {
            return failure("acceptance.invalid_description",
                "Acceptance requirement description must be nonblank and bounded");
        }
        if (!validKind(requirement.verificationKind)) {
            return failure("acceptance.unknown_verification_kind",
                "Acceptance verification kind is not supported");
        }
        if (requirement.requiredOutputs.size()
            > kMaximumRequiredOutputsPerRequirement) {
            return failure("acceptance.output_limit",
                "Acceptance requirement exceeds the required-output limit");
        }
        std::unordered_set<std::string> outputs;
        for (const auto& output : requirement.requiredOutputs) {
            if (!safeRelativePath(output)) {
                return failure("acceptance.invalid_output_path",
                    "Acceptance output must be a canonical workspace-relative path");
            }
            if (!outputs.emplace(output).second) {
                return failure("acceptance.duplicate_output_path",
                    "Acceptance output paths must be unique per requirement");
            }
        }
        if (requirement.verificationArguments.size() > kMaximumVerificationArguments) {
            return failure("acceptance.argument_limit",
                "Acceptance requirement exceeds the verification-argument limit");
        }
        std::unordered_set<std::string> argumentNames;
        for (const auto& argument : requirement.verificationArguments) {
            if (!boundedNonblank(argument.name)
                || argument.value.size() > kMaximumAcceptanceStringBytes) {
                return failure("acceptance.invalid_argument",
                    "Verification arguments must use bounded names and values");
            }
            if (!argumentNames.emplace(argument.name).second) {
                return failure("acceptance.duplicate_argument",
                    "Verification argument names must be unique per requirement");
            }
        }
    }
    return common::Result<void>::ok();
}

common::Result<void> validateExecutionVerificationProfileArguments(
    const ExecutionAcceptanceRequirement& requirement)
{
    if (requirement.verificationKind != ExecutionVerificationKind::QmlLint
        && requirement.qmlLintDiagnosticPolicyId.has_value()) {
        return invalidProfile(
            "Only qml_lint may carry a QML-lint diagnostic policy id");
    }
    switch (requirement.verificationKind) {
    case ExecutionVerificationKind::InspectPaths:
        if (!requirement.verificationArguments.empty()) {
            return invalidProfile(
                "inspect_paths requires an empty verificationArguments array");
        }
        return common::Result<void>::ok();
    case ExecutionVerificationKind::CMakeBuild: {
        // AG-18.3: a legitimately deferred-pending-bootstrap requirement is
        // its own narrow final state at freeze() - empty
        // verificationArguments with no selected ids, distinguished from an
        // ordinary unbound draft ONLY by this flag, which
        // ManifestBootstrapValidator's full manifest-level predicate must
        // have already allowed. Never accepted merely because the flag is
        // set; the caller (ExecutionManifestValidator::validate) always
        // runs that manifest-level check too.
        if (requirement.verificationBootstrapPending) {
            if (!requirement.verificationArguments.empty()
                || requirement.verificationProfileId.has_value()
                || requirement.verificationBuildProfileId.has_value()) {
                return invalidProfile(
                    "A bootstrap-pending cmake_build requirement must carry no arguments or "
                    "selected ids");
            }
            return common::Result<void>::ok();
        }
        const auto preset = argumentValue(requirement, "preset");
        const auto buildPreset = argumentValue(requirement, "buildPreset");
        // Manifest verification reliability hardening: buildDirectory is an
        // optional third argument - a best-effort, Backend-resolved
        // candidate-relative build-directory hint (see
        // ManifestVerificationProfileBinder's CMakeBuild branch) the
        // combined verifier's Windows path-budget preflight uses before
        // running the real configure/build commands. Never required (a
        // profile with no resolved binary directory still binds with
        // exactly the 2-argument shape below), and never itself part of the
        // configure/build command line.
        const auto buildDirectory = argumentValue(requirement, "buildDirectory");
        const bool hasThreeArguments = requirement.verificationArguments.size() == 3;
        if ((requirement.verificationArguments.size() != 2 && !hasThreeArguments)
            || !preset.has_value() || !buildPreset.has_value()
            || !safeToken(*preset) || !safeToken(*buildPreset)
            || (hasThreeArguments
                && (!buildDirectory.has_value() || !safeRelativePath(*buildDirectory)))) {
            return invalidProfile(
                "cmake_build requires preset and buildPreset with safe token values, plus an "
                "optional safe relative buildDirectory hint");
        }
        return common::Result<void>::ok();
    }
    case ExecutionVerificationKind::QmlLint: {
        if (requirement.verificationBootstrapPending) {
            if (!requirement.verificationArguments.empty()
                || requirement.verificationProfileId.has_value()
                || requirement.verificationBuildProfileId.has_value()
                || requirement.verificationTargetId.has_value()) {
                return invalidProfile(
                    "A bootstrap-pending qml_lint requirement must carry no arguments or "
                    "selected ids");
            }
            return common::Result<void>::ok();
        }
        const auto buildDirectory = argumentValue(requirement, "buildDirectory");
        const auto target = argumentValue(requirement, "target");
        if (requirement.verificationArguments.size() != 2
            || !buildDirectory.has_value() || !target.has_value()
            || !safeRelativePath(*buildDirectory) || !safeToken(*target)) {
            return invalidProfile(
                "qml_lint requires exactly buildDirectory and target with safe values");
        }
        if (requirement.qmlLintDiagnosticPolicyId.has_value()
            && !isSupportedQmlLintDiagnosticPolicyId(
                *requirement.qmlLintDiagnosticPolicyId)) {
            return invalidProfile("qml_lint diagnostic policy id is not supported");
        }
        return common::Result<void>::ok();
    }
    case ExecutionVerificationKind::CMakeTargetConsistency: {
        if (requirement.verificationBootstrapPending) {
            return invalidProfile(
                "cmake_target_consistency can never be deferred pending bootstrap");
        }
        const auto definitionFile = argumentValue(requirement, "definitionFile");
        const auto registrationFile = argumentValue(requirement, "registrationFile");
        const auto target = argumentValue(requirement, "target");
        if (requirement.verificationArguments.size() != 3
            || !definitionFile.has_value() || !registrationFile.has_value()
            || !target.has_value() || !safeRelativePath(*definitionFile)
            || !safeRelativePath(*registrationFile) || !safeToken(*target)
            || *definitionFile == *registrationFile) {
            return invalidProfile(
                "cmake_target_consistency requires exactly definitionFile, "
                "registrationFile, and target with safe, distinct file values");
        }
        return common::Result<void>::ok();
    }
    }
    return failure("acceptance.unknown_verification_kind",
        "Acceptance verification kind is not supported");
}

common::Result<void> validateExecutionVerificationDraftSelections(
    const ExecutionAcceptanceRequirement& requirement)
{
    auto invalidSelection = [](std::string message) {
        return failure("acceptance.invalid_verification_selection", std::move(message));
    };
    auto providerOwned = [] {
        return failure("acceptance.provider_owned_verification_argument",
            "This verification kind's environment arguments are Backend-resolved from a "
            "selected profile/target id and must never be submitted directly");
    };
    if (requirement.qmlLintDiagnosticPolicyId.has_value()) {
        return providerOwned();
    }
    const bool hasProfile = requirement.verificationProfileId.has_value();
    const bool hasBuildProfile = requirement.verificationBuildProfileId.has_value();
    const bool hasTarget = requirement.verificationTargetId.has_value();
    switch (requirement.verificationKind) {
    case ExecutionVerificationKind::InspectPaths:
        if (requirement.verificationBootstrapPending) {
            return invalidSelection("inspect_paths can never be deferred pending bootstrap");
        }
        if (hasProfile || hasBuildProfile || hasTarget) {
            return invalidSelection(
                "inspect_paths must not select a verification profile, build profile, or target");
        }
        if (!requirement.verificationArguments.empty()) return providerOwned();
        return common::Result<void>::ok();
    case ExecutionVerificationKind::CMakeBuild:
        if (!requirement.verificationArguments.empty()) return providerOwned();
        if (hasTarget) {
            return invalidSelection("cmake_build must not select a verificationTargetId");
        }
        // AG-18.3: a deferred-pending-bootstrap selection is the one
        // legitimate case where NEITHER id is selected yet - the profile
        // does not exist until the declared bootstrap's proposals are
        // composed into the candidate overlay. Full admission (is a safe
        // bootstrap actually declared and dependency-ordered for this
        // task?) is ManifestBootstrapValidator's job, not this per-
        // requirement shape check.
        if (requirement.verificationBootstrapPending) {
            if (hasProfile || hasBuildProfile) {
                return invalidSelection(
                    "A bootstrap-pending cmake_build requirement must not also select a "
                    "profile or build profile id");
            }
            return common::Result<void>::ok();
        }
        if (!hasProfile || !safeToken(*requirement.verificationProfileId)) {
            return invalidSelection("cmake_build requires a valid verificationProfileId");
        }
        if (!hasBuildProfile || !safeToken(*requirement.verificationBuildProfileId)) {
            return invalidSelection("cmake_build requires a valid verificationBuildProfileId");
        }
        return common::Result<void>::ok();
    case ExecutionVerificationKind::QmlLint:
        if (!requirement.verificationArguments.empty()) return providerOwned();
        if (requirement.verificationBootstrapPending) {
            if (hasProfile || hasBuildProfile || hasTarget) {
                return invalidSelection(
                    "A bootstrap-pending qml_lint requirement must not also select a profile, "
                    "build profile, or target id");
            }
            return common::Result<void>::ok();
        }
        if (!hasProfile || !safeToken(*requirement.verificationProfileId)) {
            return invalidSelection("qml_lint requires a valid verificationProfileId");
        }
        if (!hasBuildProfile || !safeToken(*requirement.verificationBuildProfileId)) {
            return invalidSelection("qml_lint requires a valid verificationBuildProfileId");
        }
        if (!hasTarget || !safeToken(*requirement.verificationTargetId)) {
            return invalidSelection("qml_lint requires a valid verificationTargetId");
        }
        return common::Result<void>::ok();
    case ExecutionVerificationKind::CMakeTargetConsistency: {
        if (requirement.verificationBootstrapPending) {
            return invalidSelection(
                "cmake_target_consistency can never be deferred pending bootstrap");
        }
        if (hasProfile || hasBuildProfile) {
            return invalidSelection(
                "cmake_target_consistency must not select a verification profile or build profile");
        }
        if (!hasTarget || !safeToken(*requirement.verificationTargetId)) {
            return invalidSelection("cmake_target_consistency requires a valid verificationTargetId");
        }
        const auto definitionFile = argumentValue(requirement, "definitionFile");
        const auto registrationFile = argumentValue(requirement, "registrationFile");
        if (requirement.verificationArguments.size() != 2 || !definitionFile.has_value()
            || !registrationFile.has_value() || !safeRelativePath(*definitionFile)
            || !safeRelativePath(*registrationFile)
            || *definitionFile == *registrationFile) {
            return invalidSelection(
                "cmake_target_consistency requires exactly definitionFile and registrationFile "
                "with distinct safe relative-path values");
        }
        return common::Result<void>::ok();
    }
    }
    return failure("acceptance.unknown_verification_kind",
        "Acceptance verification kind is not supported");
}

common::Result<void> validateExecutionVerificationRequirementState(
    const ExecutionAcceptanceRequirement& requirement)
{
    const auto draft = validateExecutionVerificationDraftSelections(requirement);
    if (draft.success()) return draft;
    const auto resolved = validateExecutionVerificationProfileArguments(requirement);
    if (resolved.success()) return resolved;
    // Neither state is valid - surface the draft-shape diagnostic, since a
    // requirement reaching validate() from the Planner's own pre-binding
    // call site (the far more common caller) is still in draft form.
    return draft;
}

common::Result<void> validateManifestProposalSetVerification(
    const ManifestProposalSetVerification& verification)
{
    if (verification.schemaVersion != kManifestProposalSetVerificationSchemaVersion) {
        return failure("verification.unsupported_schema",
            "Proposal-set verification schema version is not supported");
    }
    if (!boundedNonblank(verification.workspaceIdentity)
        || !boundedNonblank(verification.baselineFingerprint)
        || !boundedNonblank(verification.overlayFingerprint)) {
        return failure("verification.invalid_identity",
            "Proposal-set verification identity fields must be nonblank and bounded");
    }
    if (verification.orderedProposalIds.size()
        != verification.orderedProposalContentHashes.size()) {
        return failure("verification.proposal_identity_mismatch",
            "Proposal ids and content hashes must have equal cardinality");
    }
    if (verification.orderedProposalIds.size() > 64
        || verification.checks.size() > kMaximumAcceptanceRequirements
        || verification.criteria.size() > kMaximumAcceptanceRequirements) {
        return failure("verification.output_limit",
            "Verification proposal/check/criterion count exceeds the supported limit");
    }
    if (verification.diagnostics.size() > kMaximumVerificationDiagnostics
        || verification.artifactRelativePaths.size()
            > kMaximumVerificationArtifactReferences) {
        return failure("verification.output_limit",
            "Verification diagnostics or artifacts exceed the supported limit");
    }
    if (!validStatus(verification.proposalSafetyStatus)
        || !validStatus(verification.buildLintTestStatus)
        || !validStatus(verification.acceptanceStatus)) {
        return failure("verification.invalid_status",
            "Verification contains an unsupported status value");
    }
    std::unordered_set<std::string> criterionIds;
    for (std::size_t index = 0; index < verification.orderedProposalIds.size(); ++index) {
        if (!boundedNonblank(verification.orderedProposalIds[index])
            || !boundedNonblank(verification.orderedProposalContentHashes[index])) {
            return failure("verification.invalid_proposal_identity",
                "Verification proposal identities must be nonblank and bounded");
        }
    }
    for (const auto& check : verification.checks) {
        if (!boundedNonblank(check.profileId)
            || !boundedNonblank(check.commandIdentity)
            || !validKind(check.kind) || !validStatus(check.status)
            || check.boundedOutputHash.size() > kMaximumAcceptanceStringBytes
            || check.diagnostics.size() > kMaximumVerificationDiagnostics
            || (check.artifactRelativePath.has_value()
                && !safeRelativePath(*check.artifactRelativePath))) {
            return failure("verification.invalid_check",
                "Verification check is unsupported or outside bounded limits");
        }
    }
    auto validDiagnostic = [](const ManifestVerificationDiagnostic& diagnostic) {
        return boundedNonblank(diagnostic.code)
            && boundedNonblank(diagnostic.message)
            && (!diagnostic.requirementId.has_value()
                || boundedNonblank(*diagnostic.requirementId));
    };
    if (!std::all_of(verification.diagnostics.begin(), verification.diagnostics.end(),
            validDiagnostic)) {
        return failure("verification.invalid_diagnostic",
            "Verification diagnostics must be nonblank and bounded");
    }
    for (const auto& criterion : verification.criteria) {
        if (!boundedNonblank(criterion.requirementId)
            || !criterionIds.emplace(criterion.requirementId).second) {
            return failure("verification.invalid_criterion",
                "Verification criterion ids must be bounded and unique");
        }
        if (!validStatus(criterion.status)) {
            return failure("verification.invalid_status",
                "Verification criterion contains an unsupported status");
        }
        if (criterion.evidenceReferences.size()
                > kMaximumVerificationArtifactReferences
            || std::any_of(criterion.evidenceReferences.begin(),
                criterion.evidenceReferences.end(), [](const std::string& value) {
                    return !boundedNonblank(value);
                })) {
            return failure("verification.invalid_evidence_reference",
                "Verification evidence references must be nonblank and bounded");
        }
    }
    for (const auto& artifact : verification.artifactRelativePaths) {
        if (!safeRelativePath(artifact)) {
            return failure("verification.invalid_artifact_path",
                "Verification artifacts must use canonical workspace-relative paths");
        }
    }
    return common::Result<void>::ok();
}

bool requiredAcceptancePassed(
    const ManifestProposalSetVerification& verification) noexcept
{
    if (verification.proposalSafetyStatus != ExecutionVerificationStatus::Passed
        || verification.buildLintTestStatus != ExecutionVerificationStatus::Passed
        || verification.acceptanceStatus != ExecutionVerificationStatus::Passed) {
        return false;
    }
    return std::all_of(verification.criteria.begin(), verification.criteria.end(),
        [](const ManifestAcceptanceCriterionResult& result) {
            return !result.required || result.status == ExecutionVerificationStatus::Passed;
        });
}

namespace {

nlohmann::json toJson(const ManifestVerificationDiagnosticFinding& f)
{
    return nlohmann::json {
        {"fileKey", f.fileKey},
        {"line", f.line},
        {"column", f.column},
        {"tag", f.tag},
        {"text", f.text}};
}

ManifestVerificationDiagnosticFinding findingFromJson(const nlohmann::json& j)
{
    ManifestVerificationDiagnosticFinding f;
    if (j.contains("fileKey") && j["fileKey"].is_string()) f.fileKey = j["fileKey"].get<std::string>();
    if (j.contains("line") && j["line"].is_number_integer()) f.line = j["line"].get<int>();
    if (j.contains("column") && j["column"].is_number_integer()) f.column = j["column"].get<int>();
    if (j.contains("tag") && j["tag"].is_string()) f.tag = j["tag"].get<std::string>();
    if (j.contains("text") && j["text"].is_string()) f.text = j["text"].get<std::string>();
    return f;
}

nlohmann::json toJson(const ManifestVerificationDiagnostic& d)
{
    nlohmann::json j {{"code", d.code}, {"message", d.message}};
    j["requirementId"] = d.requirementId.has_value()
        ? nlohmann::json(*d.requirementId) : nlohmann::json(nullptr);
    nlohmann::json structuredFindings = nlohmann::json::array();
    for (const auto& f : d.structuredFindings) structuredFindings.push_back(toJson(f));
    j["structuredFindings"] = std::move(structuredFindings);
    return j;
}

ManifestVerificationDiagnostic diagnosticFromJson(const nlohmann::json& j)
{
    ManifestVerificationDiagnostic d;
    if (j.contains("code") && j["code"].is_string()) d.code = j["code"].get<std::string>();
    if (j.contains("message") && j["message"].is_string()) d.message = j["message"].get<std::string>();
    if (j.contains("requirementId") && j["requirementId"].is_string())
        d.requirementId = j["requirementId"].get<std::string>();
    if (j.contains("structuredFindings") && j["structuredFindings"].is_array()) {
        for (const auto& f : j["structuredFindings"]) {
            if (f.is_object()) d.structuredFindings.push_back(findingFromJson(f));
        }
    }
    return d;
}

nlohmann::json toJson(const ManifestVerificationCheckResult& c)
{
    nlohmann::json j {
        {"profileId", c.profileId},
        {"kind", toString(c.kind)},
        {"status", toString(c.status)},
        {"commandIdentity", c.commandIdentity},
        {"boundedOutputHash", c.boundedOutputHash},
        {"durationMilliseconds", c.durationMilliseconds}};
    j["exitCode"] = c.exitCode.has_value() ? nlohmann::json(*c.exitCode) : nlohmann::json(nullptr);
    j["artifactRelativePath"] = c.artifactRelativePath.has_value()
        ? nlohmann::json(*c.artifactRelativePath) : nlohmann::json(nullptr);
    nlohmann::json diagnostics = nlohmann::json::array();
    for (const auto& d : c.diagnostics) diagnostics.push_back(toJson(d));
    j["diagnostics"] = std::move(diagnostics);
    return j;
}

ManifestVerificationCheckResult checkFromJson(const nlohmann::json& j)
{
    ManifestVerificationCheckResult c;
    if (j.contains("profileId") && j["profileId"].is_string())
        c.profileId = j["profileId"].get<std::string>();
    if (j.contains("kind") && j["kind"].is_string()) {
        if (const auto kind = executionVerificationKindFromString(j["kind"].get<std::string>()))
            c.kind = *kind;
    }
    if (j.contains("status") && j["status"].is_string()) {
        if (const auto status = executionVerificationStatusFromString(j["status"].get<std::string>()))
            c.status = *status;
    }
    if (j.contains("commandIdentity") && j["commandIdentity"].is_string())
        c.commandIdentity = j["commandIdentity"].get<std::string>();
    if (j.contains("boundedOutputHash") && j["boundedOutputHash"].is_string())
        c.boundedOutputHash = j["boundedOutputHash"].get<std::string>();
    if (j.contains("durationMilliseconds") && j["durationMilliseconds"].is_number_unsigned())
        c.durationMilliseconds = j["durationMilliseconds"].get<std::uint64_t>();
    if (j.contains("exitCode") && j["exitCode"].is_number_integer())
        c.exitCode = j["exitCode"].get<int>();
    if (j.contains("artifactRelativePath") && j["artifactRelativePath"].is_string())
        c.artifactRelativePath = j["artifactRelativePath"].get<std::string>();
    if (j.contains("diagnostics") && j["diagnostics"].is_array()) {
        for (const auto& d : j["diagnostics"]) {
            if (d.is_object()) c.diagnostics.push_back(diagnosticFromJson(d));
        }
    }
    return c;
}

nlohmann::json toJson(const ManifestAcceptanceCriterionResult& c)
{
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& ref : c.evidenceReferences) evidence.push_back(ref);
    return nlohmann::json {
        {"requirementId", c.requirementId},
        {"required", c.required},
        {"status", toString(c.status)},
        {"evidenceReferences", std::move(evidence)}};
}

ManifestAcceptanceCriterionResult criterionFromJson(const nlohmann::json& j)
{
    ManifestAcceptanceCriterionResult c;
    if (j.contains("requirementId") && j["requirementId"].is_string())
        c.requirementId = j["requirementId"].get<std::string>();
    if (j.contains("required") && j["required"].is_boolean()) c.required = j["required"].get<bool>();
    if (j.contains("status") && j["status"].is_string()) {
        if (const auto status = executionVerificationStatusFromString(j["status"].get<std::string>()))
            c.status = *status;
    }
    if (j.contains("evidenceReferences") && j["evidenceReferences"].is_array()) {
        for (const auto& ref : j["evidenceReferences"]) {
            if (ref.is_string()) c.evidenceReferences.push_back(ref.get<std::string>());
        }
    }
    return c;
}

} // namespace

std::string renderManifestProposalSetVerificationJson(
    const ManifestProposalSetVerification& verification)
{
    nlohmann::json orderedProposalIds = nlohmann::json::array();
    for (const auto& id : verification.orderedProposalIds) orderedProposalIds.push_back(id);
    nlohmann::json orderedProposalContentHashes = nlohmann::json::array();
    for (const auto& hash : verification.orderedProposalContentHashes)
        orderedProposalContentHashes.push_back(hash);
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& check : verification.checks) checks.push_back(toJson(check));
    nlohmann::json criteria = nlohmann::json::array();
    for (const auto& criterion : verification.criteria) criteria.push_back(toJson(criterion));
    nlohmann::json diagnostics = nlohmann::json::array();
    for (const auto& d : verification.diagnostics) diagnostics.push_back(toJson(d));
    nlohmann::json artifactRelativePaths = nlohmann::json::array();
    for (const auto& path : verification.artifactRelativePaths) artifactRelativePaths.push_back(path);

    const nlohmann::json j {
        {"schemaVersion", verification.schemaVersion},
        {"workspaceIdentity", verification.workspaceIdentity},
        {"baselineFingerprint", verification.baselineFingerprint},
        {"orderedProposalIds", std::move(orderedProposalIds)},
        {"orderedProposalContentHashes", std::move(orderedProposalContentHashes)},
        {"overlayFingerprint", verification.overlayFingerprint},
        {"proposalSafetyStatus", toString(verification.proposalSafetyStatus)},
        {"buildLintTestStatus", toString(verification.buildLintTestStatus)},
        {"acceptanceStatus", toString(verification.acceptanceStatus)},
        {"checks", std::move(checks)},
        {"criteria", std::move(criteria)},
        {"diagnostics", std::move(diagnostics)},
        {"artifactRelativePaths", std::move(artifactRelativePaths)}};
    return j.dump();
}

// AG-14.2: fails the whole parse - never silently shrinks the result - on
// any array element that is not the expected object shape, and on any
// string-array element that is not a string. A corrupt element in an
// otherwise well-formed document is corruption, not "this one item is
// missing."
common::Result<ManifestProposalSetVerification> parseManifestProposalSetVerificationJson(
    std::string_view json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return common::Result<ManifestProposalSetVerification>::failure(
            {"verification.corrupt_identity", "Malformed verification identity JSON"});
    }
    const auto corruptIdentity = [] {
        return common::Result<ManifestProposalSetVerification>::failure(
            {"verification.corrupt_identity", "Malformed verification identity JSON"});
    };
    ManifestProposalSetVerification v;
    if (parsed.contains("schemaVersion") && parsed["schemaVersion"].is_string())
        v.schemaVersion = parsed["schemaVersion"].get<std::string>();
    if (parsed.contains("workspaceIdentity") && parsed["workspaceIdentity"].is_string())
        v.workspaceIdentity = parsed["workspaceIdentity"].get<std::string>();
    if (parsed.contains("baselineFingerprint") && parsed["baselineFingerprint"].is_string())
        v.baselineFingerprint = parsed["baselineFingerprint"].get<std::string>();
    if (parsed.contains("orderedProposalIds") && parsed["orderedProposalIds"].is_array()) {
        for (const auto& id : parsed["orderedProposalIds"]) {
            if (!id.is_string()) return corruptIdentity();
            v.orderedProposalIds.push_back(id.get<std::string>());
        }
    }
    if (parsed.contains("orderedProposalContentHashes")
            && parsed["orderedProposalContentHashes"].is_array()) {
        for (const auto& hash : parsed["orderedProposalContentHashes"]) {
            if (!hash.is_string()) return corruptIdentity();
            v.orderedProposalContentHashes.push_back(hash.get<std::string>());
        }
    }
    if (parsed.contains("overlayFingerprint") && parsed["overlayFingerprint"].is_string())
        v.overlayFingerprint = parsed["overlayFingerprint"].get<std::string>();
    if (parsed.contains("proposalSafetyStatus") && parsed["proposalSafetyStatus"].is_string()) {
        if (const auto status = executionVerificationStatusFromString(
                parsed["proposalSafetyStatus"].get<std::string>()))
            v.proposalSafetyStatus = *status;
    }
    if (parsed.contains("buildLintTestStatus") && parsed["buildLintTestStatus"].is_string()) {
        if (const auto status = executionVerificationStatusFromString(
                parsed["buildLintTestStatus"].get<std::string>()))
            v.buildLintTestStatus = *status;
    }
    if (parsed.contains("acceptanceStatus") && parsed["acceptanceStatus"].is_string()) {
        if (const auto status = executionVerificationStatusFromString(
                parsed["acceptanceStatus"].get<std::string>()))
            v.acceptanceStatus = *status;
    }
    if (parsed.contains("checks") && parsed["checks"].is_array()) {
        for (const auto& check : parsed["checks"]) {
            if (!check.is_object()) return corruptIdentity();
            v.checks.push_back(checkFromJson(check));
        }
    }
    if (parsed.contains("criteria") && parsed["criteria"].is_array()) {
        for (const auto& criterion : parsed["criteria"]) {
            if (!criterion.is_object()) return corruptIdentity();
            v.criteria.push_back(criterionFromJson(criterion));
        }
    }
    if (parsed.contains("diagnostics") && parsed["diagnostics"].is_array()) {
        for (const auto& d : parsed["diagnostics"]) {
            if (!d.is_object()) return corruptIdentity();
            v.diagnostics.push_back(diagnosticFromJson(d));
        }
    }
    if (parsed.contains("artifactRelativePaths") && parsed["artifactRelativePaths"].is_array()) {
        for (const auto& path : parsed["artifactRelativePaths"]) {
            if (!path.is_string()) return corruptIdentity();
            v.artifactRelativePaths.push_back(path.get<std::string>());
        }
    }
    const auto validated = validateManifestProposalSetVerification(v);
    if (!validated.success()) {
        return common::Result<ManifestProposalSetVerification>::failure(*validated.error());
    }
    return common::Result<ManifestProposalSetVerification>::ok(std::move(v));
}

} // namespace ai::application
