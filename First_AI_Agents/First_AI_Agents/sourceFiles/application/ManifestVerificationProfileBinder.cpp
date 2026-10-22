#include "ai/application/ManifestVerificationProfileBinder.hpp"

#include "ai/application/QmlLintDiagnosticPolicy.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

common::Result<void> failIdentity(std::string code, std::string message)
{
    return common::Result<void>::failure({std::move(code), std::move(message)});
}

common::Result<void> failBind(std::string code, std::string message,
    std::vector<ExecutionManifestFinding>& findings, const std::string& taskId)
{
    findings.push_back({code, message, taskId});
    return common::Result<void>::failure({std::move(code), std::move(message)});
}

// Same bounded-token rule ExecutionAcceptanceContract.cpp's own safeToken
// uses for preset/buildPreset/target values - a stable id is exactly as
// constrained as the raw value it ultimately resolves into.
bool safeToken(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

// A candidate-relative build path must never be absolute, empty-but-claimed,
// path-traversing, or unbounded. Empty is valid (means "not available").
bool safeCandidateRelativePath(const std::string& value)
{
    if (value.empty()) return true;
    if (value.size() > kVerificationEnvironmentMaxRelativePathBytes) return false;
    const fs::path path(value);
    if (path.is_absolute()) return false;
    for (const auto& part : path) {
        const auto text = part.generic_string();
        if (text == "..") return false;
    }
    return true;
}

const VerificationProfile* findProfile(
    const VerificationEnvironmentSnapshot& snapshot, const std::string& profileId)
{
    for (const auto& profile : snapshot.profiles) {
        if (profile.profileId == profileId) return &profile;
    }
    return nullptr;
}

const VerificationBuildPresetDescriptor* findBuildPreset(
    const VerificationProfile& profile, const std::string& buildProfileId)
{
    for (const auto& buildPreset : profile.buildPresets) {
        if (buildPreset.name == buildProfileId) return &buildPreset;
    }
    return nullptr;
}

bool targetKnownAnywhere(const VerificationEnvironmentSnapshot& snapshot, const std::string& target)
{
    for (const auto& profile : snapshot.profiles) {
        for (const auto& buildPreset : profile.buildPresets) {
            if (!buildPreset.allowedTargetsConfirmed) continue;
            if (std::find(buildPreset.allowedTargets.begin(), buildPreset.allowedTargets.end(), target)
                != buildPreset.allowedTargets.end()) {
                return true;
            }
        }
    }
    for (const auto& unlinked : snapshot.unlinkedBuildPresets) {
        if (!unlinked.allowedTargetsConfirmed) continue;
        if (std::find(unlinked.allowedTargets.begin(), unlinked.allowedTargets.end(), target)
            != unlinked.allowedTargets.end()) {
            return true;
        }
    }
    return false;
}

} // namespace

common::Result<void> ManifestVerificationProfileBinder::validateSnapshotIdentity(
    const VerificationEnvironmentSnapshot& snapshot,
    const RunId& runId,
    const ProjectId& projectId,
    const std::string& baselineFingerprint)
{
    if (snapshot.schemaVersion != kVerificationEnvironmentSnapshotSchemaVersion) {
        return failIdentity("verification_environment.snapshot_corrupt",
            "Verification environment snapshot schema is not supported");
    }
    if (snapshot.runId != runId || snapshot.projectId != projectId) {
        return failIdentity("verification_environment.identity_mismatch",
            "Verification environment snapshot does not belong to this Run/project");
    }
    if (snapshot.baselineFingerprint != baselineFingerprint) {
        return failIdentity("verification_environment.baseline_mismatch",
            "Verification environment snapshot was discovered against a different workspace baseline");
    }
    return validateSnapshotStructure(snapshot);
}

common::Result<void> ManifestVerificationProfileBinder::validateSnapshotStructure(
    const VerificationEnvironmentSnapshot& snapshot)
{
    const bool hasProfiles = !snapshot.profiles.empty();
    const bool claimsExistingProfile = snapshot.state == VerificationEnvironmentState::ExistingProfile;
    if (claimsExistingProfile != hasProfiles) {
        return failIdentity("verification_environment.snapshot_corrupt",
            "Verification environment snapshot state disagrees with its own profile content");
    }
    if (snapshot.profiles.size() > kVerificationEnvironmentMaxProfiles
        || snapshot.unlinkedBuildPresets.size() > kVerificationEnvironmentMaxUnlinkedBuildPresets) {
        return failIdentity("verification_environment.snapshot_corrupt",
            "Verification environment snapshot exceeds a trusted bounded collection ceiling");
    }

    std::unordered_set<std::string> profileIds;
    for (const auto& profile : snapshot.profiles) {
        if (!safeToken(profile.profileId) || !profileIds.emplace(profile.profileId).second) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "Verification environment snapshot contains an invalid or duplicate profile id");
        }
        // AG-18.1's own contract: "profileId is currently just
        // configurePresetName ... kept as its own field so a future
        // BackendConfigured profile can assign an id that is not literally a
        // CMake preset name." AG-18.2 only ever binds Discovered profiles
        // (VerificationProfileOrigin::BackendConfigured is not yet
        // populated by any producer), so this identity must hold exactly
        // for every profile this binder can actually select.
        if (profile.origin == VerificationProfileOrigin::Discovered
            && profile.profileId != profile.configurePresetName) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "A discovered profile's id must equal its own configure preset name");
        }
        // declaredBinaryDir is the raw, unexpanded string as authored (may
        // legitimately be absolute, contain macros other than ${sourceDir},
        // or otherwise not be relocatable) - AG-18.1 deliberately never
        // claims it is safe to join against a candidate overlay root, and it
        // is never rendered into a prompt or command by this binder. Only
        // resolvedBinaryDir (used for prompt/argument construction) must be
        // a genuinely safe candidate-relative path; an unresolved declared
        // path is not itself a corruption signal.
        if (!safeCandidateRelativePath(profile.resolvedBinaryDir)) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "Verification environment snapshot contains an unsafe or unbounded build path");
        }
        if (profile.declaredBinaryDir.size() > kVerificationEnvironmentMaxRelativePathBytes) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "Verification environment snapshot contains an unbounded declared build path");
        }
        if (profile.buildPresets.size() > kVerificationEnvironmentMaxBuildPresetsPerProfile) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "Verification environment snapshot exceeds the trusted build-preset ceiling");
        }
        std::unordered_set<std::string> buildProfileIds;
        for (const auto& buildPreset : profile.buildPresets) {
            if (!safeToken(buildPreset.name) || !buildProfileIds.emplace(buildPreset.name).second) {
                return failIdentity("verification_environment.snapshot_corrupt",
                    "Verification environment snapshot contains an invalid or duplicate build "
                    "profile id");
            }
            // AG-18.1's own contract: VerificationProfile::buildPresets is
            // "every build preset that declares [this configure preset] as
            // its own configurePreset (linked configure/build identity, not
            // two independent flat lists a later package would have to
            // re-link itself)". A build preset present here whose declared
            // link disagrees with the profile it is nested under would let
            // the binder resolve a build against the wrong configure
            // preset's toolchain/cache.
            if (buildPreset.declaredConfigurePreset != profile.configurePresetName) {
                return failIdentity("verification_environment.snapshot_corrupt",
                    "A linked build preset's declared configure preset does not match its "
                    "own profile");
            }
            if (buildPreset.allowedTargets.size() > kVerificationEnvironmentMaxTargetsPerPreset
                || buildPreset.declaredTargets.size() > kVerificationEnvironmentMaxTargetsPerPreset) {
                return failIdentity("verification_environment.snapshot_corrupt",
                    "Verification environment snapshot exceeds the trusted target ceiling");
            }
            // "An empty list with allowedTargetsConfirmed=false is unknown,
            // never 'all targets allowed'" (AG-18.1's own doc comment) - a
            // snapshot that claims an unconfirmed allow-list is non-empty
            // contradicts its own stated confidence and must never be
            // treated as if some targets were actually confirmed.
            if (!buildPreset.allowedTargetsConfirmed && !buildPreset.allowedTargets.empty()) {
                return failIdentity("verification_environment.snapshot_corrupt",
                    "An unconfirmed target allow-list must not carry any target entries");
            }
            // AG-17.0: a catalog cannot be complete without being confirmed.
            if (buildPreset.allowedTargetsComplete && !buildPreset.allowedTargetsConfirmed) {
                return failIdentity("verification_environment.snapshot_corrupt",
                    "A target catalog cannot be complete without being confirmed");
            }
            std::unordered_set<std::string> targetIds;
            for (const auto& target : buildPreset.allowedTargets) {
                if (!safeToken(target) || !targetIds.emplace(target).second) {
                    return failIdentity("verification_environment.snapshot_corrupt",
                        "Verification environment snapshot contains an invalid or duplicate "
                        "target id");
                }
            }
        }
    }
    for (const auto& unlinked : snapshot.unlinkedBuildPresets) {
        if (unlinked.allowedTargets.size() > kVerificationEnvironmentMaxTargetsPerPreset
            || unlinked.declaredTargets.size() > kVerificationEnvironmentMaxTargetsPerPreset) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "Verification environment snapshot exceeds the trusted target ceiling");
        }
        if (!unlinked.allowedTargetsConfirmed && !unlinked.allowedTargets.empty()) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "An unconfirmed target allow-list must not carry any target entries");
        }
        if (unlinked.allowedTargetsComplete && !unlinked.allowedTargetsConfirmed) {
            return failIdentity("verification_environment.snapshot_corrupt",
                "A target catalog cannot be complete without being confirmed");
        }
        std::unordered_set<std::string> targetIds;
        for (const auto& target : unlinked.allowedTargets) {
            if (!safeToken(target) || !targetIds.emplace(target).second) {
                return failIdentity("verification_environment.snapshot_corrupt",
                    "Verification environment snapshot contains an invalid or duplicate target id");
            }
        }
    }
    return common::Result<void>::ok();
}

std::string ManifestVerificationProfileBinder::renderTrustedCatalog(
    const VerificationEnvironmentSnapshot& snapshot)
{
    std::ostringstream out;
    out << "# Trusted verification environment catalog\n"
        << "This catalog is the ONLY source of valid verification profile/build-profile/target "
           "ids. Select ids exactly as advertised here; never invent a profile, build-profile, "
           "or target id, and never submit a raw CMake preset name, build directory, executable, "
           "or command argument directly - those are Backend-resolved from your selection. "
           "inspect_paths requires no selection at all. A build/QML-lint requirement may only "
           "select a profile that advertises the required capability as confirmed below.\n\n";

    if (snapshot.state != VerificationEnvironmentState::ExistingProfile
        || snapshot.profiles.empty()) {
        out << "No verification profile is currently available for this workspace. Use only "
               "inspect_paths for this Run's verification requirements; do not invent a profile, "
               "build-profile, or target id.\n";
        return out.str();
    }

    for (const auto& profile : snapshot.profiles) {
        out << "- profileId: " << profile.profileId
            << " (source: " << toString(profile.configureSource) << ")\n"
            << "  buildPresetsConfirmed: " << (profile.buildPresetsConfirmed ? "true" : "false") << "\n"
            << "  candidateRelativeBuildDirectory: "
            << (profile.resolvedBinaryDir.empty() ? "not available" : profile.resolvedBinaryDir) << "\n";
        if (profile.buildPresets.empty()) {
            out << "  buildProfiles: none\n";
        } else {
            out << "  buildProfiles:\n";
            for (const auto& buildPreset : profile.buildPresets) {
                out << "    - buildProfileId: " << buildPreset.name << "\n"
                    << "      allowedTargetsConfirmed: "
                    << (buildPreset.allowedTargetsConfirmed ? "true" : "false") << "\n"
                    << "      allowedTargets: [";
                for (std::size_t i = 0; i < buildPreset.allowedTargets.size(); ++i) {
                    if (i != 0) out << ", ";
                    out << buildPreset.allowedTargets[i];
                }
                out << "]\n";
            }
        }
    }
    return out.str();
}

common::Result<void> ManifestVerificationProfileBinder::bindManifest(
    ExecutionManifest& manifest,
    const VerificationEnvironmentSnapshot& snapshot,
    std::vector<ExecutionManifestFinding>& findings)
{
    for (auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (auto& requirement : task.acceptanceContract->requirements) {
            // AG-18.3: a requirement legitimately deferred pending a same-
            // Run bootstrap carries no profile/build/target selection yet
            // by design (see ManifestBootstrapValidator) - it is resolved
            // later, against the candidate overlay, by
            // bindDeferredBootstrapRequirements, never here.
            if (requirement.verificationBootstrapPending) continue;
            switch (requirement.verificationKind) {
            case ExecutionVerificationKind::InspectPaths:
                continue;
            case ExecutionVerificationKind::CMakeBuild: {
                const auto* profile = findProfile(snapshot, *requirement.verificationProfileId);
                if (profile == nullptr) {
                    return failBind("manifest.verification_profile_unknown",
                        "Selected verification profile is not in the trusted catalog",
                        findings, task.id);
                }
                const auto* buildPreset =
                    findBuildPreset(*profile, *requirement.verificationBuildProfileId);
                if (buildPreset == nullptr) {
                    return failBind("manifest.verification_build_profile_unknown",
                        "Selected build profile is not linked to the selected verification profile",
                        findings, task.id);
                }
                if (!profile->buildPresetsConfirmed) {
                    return failBind("manifest.verification_build_unavailable",
                        "The selected profile's build presets were not confirmed by discovery",
                        findings, task.id);
                }
                requirement.verificationArguments = {
                    {"preset", profile->configurePresetName},
                    {"buildPreset", buildPreset->name}};
                // Manifest verification reliability hardening: best-effort,
                // never required (unlike QmlLint below) - when discovery did
                // capture a candidate-relative resolved build directory for
                // this profile, thread it through so the combined verifier's
                // Windows path-budget preflight can check it before running
                // the real configure/build commands. Absent only when
                // discovery could not resolve one; the configure/build
                // commands themselves never depended on this argument.
                if (!profile->resolvedBinaryDir.empty()) {
                    requirement.verificationArguments.push_back(
                        {"buildDirectory", profile->resolvedBinaryDir});
                }
                break;
            }
            case ExecutionVerificationKind::QmlLint: {
                const auto* profile = findProfile(snapshot, *requirement.verificationProfileId);
                if (profile == nullptr) {
                    return failBind("manifest.verification_profile_unknown",
                        "Selected verification profile is not in the trusted catalog",
                        findings, task.id);
                }
                const auto* buildPreset =
                    findBuildPreset(*profile, *requirement.verificationBuildProfileId);
                if (buildPreset == nullptr) {
                    return failBind("manifest.verification_build_profile_unknown",
                        "Selected build profile is not linked to the selected verification profile",
                        findings, task.id);
                }
                if (!profile->buildPresetsConfirmed) {
                    return failBind("manifest.verification_build_unavailable",
                        "The selected profile's build presets were not confirmed by discovery",
                        findings, task.id);
                }
                if (profile->resolvedBinaryDir.empty()) {
                    return failBind("manifest.verification_build_directory_unavailable",
                        "The selected profile has no candidate-relative build directory available",
                        findings, task.id);
                }
                if (!buildPreset->allowedTargetsConfirmed) {
                    return failBind("manifest.verification_target_unknown",
                        "The selected build profile's target allow-list was not confirmed by discovery",
                        findings, task.id);
                }
                const auto targetFound = std::find(buildPreset->allowedTargets.begin(),
                    buildPreset->allowedTargets.end(), *requirement.verificationTargetId);
                if (targetFound == buildPreset->allowedTargets.end()) {
                    return failBind("manifest.verification_target_unknown",
                        "Selected target is not in the selected build profile's confirmed "
                        "allow-list", findings, task.id);
                }
                requirement.verificationArguments = {
                    {"buildDirectory", profile->resolvedBinaryDir},
                    {"target", *targetFound}};
                requirement.qmlLintDiagnosticPolicyId =
                    std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
                break;
            }
            case ExecutionVerificationKind::CMakeTargetConsistency: {
                // definitionFile/registrationFile are already present
                // (task-owned, provider-authored, already shape-validated
                // by validateExecutionVerificationDraftSelections) - only
                // the target identity is Backend-resolved here, from the
                // union of every confirmed allow-list in the snapshot: this
                // check never runs a build itself (see
                // ManifestProposalSetVerifier's internalCheck), so it has no
                // single "selected build profile" to scope the target to.
                if (!targetKnownAnywhere(snapshot, *requirement.verificationTargetId)) {
                    return failBind("manifest.verification_target_unknown",
                        "Selected target is not in any confirmed verification target allow-list",
                        findings, task.id);
                }
                requirement.verificationArguments.push_back(
                    {"target", *requirement.verificationTargetId});
                break;
            }
            }
            const auto resolvedShape = validateExecutionVerificationProfileArguments(requirement);
            if (!resolvedShape.success()) {
                return failBind(resolvedShape.error()->code, resolvedShape.error()->message,
                    findings, task.id);
            }
        }
    }
    return common::Result<void>::ok();
}

common::Result<VerificationBootstrapCandidateIdentity>
ManifestVerificationProfileBinder::bindDeferredBootstrapRequirements(
    ExecutionManifest& manifest,
    const VerificationEnvironmentSnapshot& candidateSnapshot,
    std::vector<ExecutionManifestFinding>& findings)
{
    auto failCandidate = [&](std::string code, std::string message,
            const std::string& taskId) {
        findings.push_back({code, message, taskId});
        return common::Result<VerificationBootstrapCandidateIdentity>::failure(
            {std::move(code), std::move(message)});
    };

    if (candidateSnapshot.state != VerificationEnvironmentState::ExistingProfile
        || candidateSnapshot.profiles.size() != 1) {
        return failCandidate("verification_environment.bootstrap_candidate_invalid",
            "The candidate overlay must rediscover to exactly one usable verification profile",
            manifest.bootstrapDeclaration.has_value()
                ? manifest.bootstrapDeclaration->bootstrapTaskId : std::string {});
    }
    const auto& profile = candidateSnapshot.profiles.front();
    if (!profile.buildPresetsConfirmed || profile.buildPresets.size() != 1) {
        return failCandidate("verification_environment.bootstrap_candidate_invalid",
            "The candidate overlay's profile must confirm exactly one build preset",
            manifest.bootstrapDeclaration.has_value()
                ? manifest.bootstrapDeclaration->bootstrapTaskId : std::string {});
    }
    const auto& buildPreset = profile.buildPresets.front();

    VerificationBootstrapCandidateIdentity identity;
    identity.candidateProfileId = profile.profileId;
    identity.candidateBuildProfileId = buildPreset.name;

    bool boundAny = false;
    for (auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (auto& requirement : task.acceptanceContract->requirements) {
            if (!requirement.verificationBootstrapPending) continue;
            switch (requirement.verificationKind) {
            case ExecutionVerificationKind::CMakeBuild:
                requirement.verificationArguments = {
                    {"preset", profile.configurePresetName},
                    {"buildPreset", buildPreset.name}};
                // See bindManifest's identical CMakeBuild branch: best-effort
                // path-budget hint, never required.
                if (!profile.resolvedBinaryDir.empty()) {
                    requirement.verificationArguments.push_back(
                        {"buildDirectory", profile.resolvedBinaryDir});
                }
                break;
            case ExecutionVerificationKind::QmlLint: {
                if (profile.resolvedBinaryDir.empty()) {
                    return failCandidate("verification_environment.bootstrap_candidate_invalid",
                        "The candidate overlay's profile has no candidate-relative build "
                        "directory available", task.id);
                }
                if (!buildPreset.allowedTargetsConfirmed) {
                    return failCandidate("verification_environment.bootstrap_candidate_invalid",
                        "The candidate overlay's build preset has no confirmed target catalog",
                        task.id);
                }
                // AG-18.3 hardening (blocker 3): the real project may
                // legitimately expose many configured targets - the
                // catalog itself (allowedTargets) is no longer required to
                // be a singleton. The QML-lint-eligible target is the
                // build preset's own DECLARED (requested) targets
                // intersected with the REAL confirmed catalog; an
                // unrelated real target never makes an otherwise
                // unambiguous declared target invalid, and a declared
                // target absent from the real catalog is never trusted on
                // its own.
                std::vector<std::string> eligible;
                for (const auto& declared : buildPreset.declaredTargets) {
                    if (std::find(buildPreset.allowedTargets.begin(),
                            buildPreset.allowedTargets.end(), declared)
                        != buildPreset.allowedTargets.end()) {
                        eligible.push_back(declared);
                    }
                }
                if (eligible.size() != 1) {
                    return failCandidate("verification_environment.bootstrap_target_ambiguous",
                        eligible.empty()
                            ? "No requested target for this build preset is present in the "
                              "real confirmed target catalog"
                            : "More than one requested target for this build preset is present "
                              "in the real confirmed target catalog",
                        task.id);
                }
                identity.candidateTargetId = eligible.front();
                requirement.verificationArguments = {
                    {"buildDirectory", profile.resolvedBinaryDir},
                    {"target", eligible.front()}};
                requirement.qmlLintDiagnosticPolicyId =
                    std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
                break;
            }
            case ExecutionVerificationKind::InspectPaths:
            case ExecutionVerificationKind::CMakeTargetConsistency:
                // ManifestBootstrapValidator/validateExecutionVerificationDraftSelections
                // already reject verificationBootstrapPending for these
                // kinds before freeze - unreachable in a manifest that
                // actually got this far.
                return failCandidate("verification_environment.bootstrap_candidate_invalid",
                    "This verification kind can never be deferred pending bootstrap", task.id);
            }
            requirement.verificationBootstrapPending = false;
            const auto resolvedShape = validateExecutionVerificationProfileArguments(requirement);
            if (!resolvedShape.success()) {
                return failCandidate(resolvedShape.error()->code, resolvedShape.error()->message,
                    task.id);
            }
            boundAny = true;
        }
    }
    if (!boundAny) {
        return failCandidate("verification_environment.bootstrap_candidate_invalid",
            "No deferred bootstrap-pending requirement was found to bind", std::string {});
    }
    return common::Result<VerificationBootstrapCandidateIdentity>::ok(std::move(identity));
}

} // namespace ai::application
