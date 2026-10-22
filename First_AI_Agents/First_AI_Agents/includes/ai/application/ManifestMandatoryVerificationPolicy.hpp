#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/VerificationEnvironmentSnapshotTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// AG-17.0: Backend-owned mandatory verification requirements
// (manifest_mandatory_verification_v1, mode "auto").
//
// Why this exists: the Planner authors acceptance criteria, and real Runs
// (Stage 4B.1 and the OpenAI Stage 4B.2) froze Manifests that wrote QML but
// carried no qml_lint criterion although the user's task required lint. A
// required check must not be able to disappear because a model did not carry
// it over, so the Backend derives it from three Backend-owned facts only: the
// frozen Verification Environment Snapshot, the Manifest's own frozen write
// scope, and this policy. Never from the user's task text, never from a word
// heuristic over "lint"/"build"/"test", never from provider output, never from
// UI logic.
//
// apply() is a pure function: it reads no database, calls no provider, runs
// no command, never mutates the snapshot and does not depend on QML/UI. It is
// called by ExecutionManifestPlanner strictly AFTER structural/semantic
// validation and strictly BEFORE ManifestVerificationProfileBinder::
// bindManifest, so an injected requirement is only ever a draft SELECTION
// (profile / build profile / target ids taken from the snapshot) that the one
// existing binder resolves exactly like a Planner-declared one.
//
// Triggers, from the Manifest's write scope (ownedPaths + plannedCreatePaths):
//   - qml_lint   : a written .qml/.js/.mjs/qmldir path, or a written CMake file
//                  (CMakeLists.txt / *.cmake) that is in the same directory as,
//                  or an ancestor directory of, a QML path the same Manifest
//                  writes (QML registration);
//   - cmake_build: a written C/C++ source or header, .qrc, or CMake file; and
//                  always as the prerequisite of an applied qml_lint.
//
// QML-lint target selection (exact, case-sensitive CMake target names):
//   eligible = ends with "_qmllint" with a non-empty module part ("_qmllint_json"
//   and "_qmllint_module" do not end with it, so they are never eligible);
//   1. exactly one profile/build-preset carrying "all_qmllint" -> it;
//   2. more than one carrying it                                -> ambiguous;
//   3. else a catalog that is not provably complete             -> incomplete
//      (a hidden "all_qmllint" would outrank any module target);
//   4. else exactly one eligible module target                  -> it;
//   5. else several                                             -> ambiguous;
//   6. else (complete catalog, none)                            -> not_applicable.
//
// Equivalence is strict: an existing requirement satisfies the policy only if
// it has the same kind, required=true, not bootstrap-pending and the same
// selected profile/build-profile/target. A different Planner lint target never
// satisfies it - the Backend adds its own and keeps the Planner's. One
// required cmake_build and one qml_lint are enforced for the WHOLE Manifest,
// on one deterministic anchor task (the last applicable task in canonical
// topological order, taskId as the stable tie-break), never one per task.
//
// Every failure is typed (verification_policy.*), happens before freeze, is
// NOT repairable by a second provider turn (isNonRepairableSubmissionFailure)
// and is not a verification failure of a frozen Manifest.

inline constexpr std::string_view kMandatoryVerificationFailurePrefix = "verification_policy.";
inline constexpr const char* kVerificationPolicyLintTargetAmbiguous =
    "verification_policy.lint_target_ambiguous";
inline constexpr const char* kVerificationPolicyTargetCatalogIncomplete =
    "verification_policy.target_catalog_incomplete";
inline constexpr const char* kVerificationPolicyConflictingRequirement =
    "verification_policy.conflicting_requirement";
inline constexpr const char* kVerificationPolicyBuildPrerequisiteUnavailable =
    "verification_policy.build_prerequisite_unavailable";
inline constexpr const char* kVerificationPolicyProfileUnresolvable =
    "verification_policy.profile_unresolvable";

// Reserved, Manifest-unique ids of the requirements the Backend adds.
inline constexpr std::string_view kMandatoryQmlLintRequirementId = "backend-mandatory-qml-lint";
inline constexpr std::string_view kMandatoryCMakeBuildRequirementId = "backend-mandatory-cmake-build";

inline constexpr std::string_view kAggregateQmlLintTarget = "all_qmllint";
inline constexpr std::string_view kQmlLintTargetSuffix = "_qmllint";

// Stable decision reason codes (ManifestMandatoryVerificationDecision::reasonCode).
inline constexpr const char* kMandatoryReasonNoQmlPaths = "no_qml_paths";
inline constexpr const char* kMandatoryReasonNoBuildPaths = "no_build_paths";
inline constexpr const char* kMandatoryReasonQmlPathsWritten = "qml_paths_written";
inline constexpr const char* kMandatoryReasonBuildPathsWritten = "build_paths_written";
inline constexpr const char* kMandatoryReasonQmlLintPrerequisite = "qml_lint_prerequisite";
inline constexpr const char* kMandatoryReasonNoQmlLintTarget = "no_qml_lint_target";
inline constexpr const char* kMandatoryReasonNoBuildProfile = "no_build_profile";
inline constexpr const char* kMandatoryReasonSetupRequired = "environment_setup_required";

class ManifestMandatoryVerificationPolicy final {
public:
    ManifestMandatoryVerificationPolicy() = delete;

    // True for a target the policy may select as the QML-lint target: its exact
    // name ends with "_qmllint" and has a non-empty module part. No case folding.
    [[nodiscard]] static bool isEligibleQmlLintTarget(std::string_view target) noexcept;

    // Derives, merges and records the mandatory requirements on `manifest`
    // against an already-loaded snapshot. All analysis happens before any
    // mutation, so a failure leaves `manifest` exactly as it was. On success
    // manifest.mandatoryVerification is set (always, even when nothing was
    // added). Idempotent: applying it again to its own result changes nothing.
    // On failure appends exactly one ExecutionManifestFinding and returns the
    // matching failure; the codes are the kVerificationPolicy* constants.
    [[nodiscard]] static common::Result<void> apply(
        ExecutionManifest& manifest,
        const VerificationEnvironmentSnapshot& snapshot,
        std::vector<ExecutionManifestFinding>& findings);

    // Cross-field integrity of a frozen decision record against the Manifest's
    // OWN tasks. nullopt = consistent (or no record). Otherwise a bounded
    // Backend message. A record must never reference something that is not
    // there or whose provenance contradicts it:
    //   - not_triggered / not_applicable decisions name no requirement/task;
    //   - injected / satisfied_by_existing decisions name a requirement and a
    //     task, the task exists in this Manifest, holds that requirement id,
    //     the requirement's kind equals the decision's kind, and its origin
    //     agrees (injected -> backend_mandatory, satisfied_by_existing ->
    //     planner_declared);
    //   - at most one decision per kind.
    // The Manifest validator enforces this at freeze
    // (manifest.mandatory_verification_inconsistent). The reverse direction (a
    // backend_mandatory requirement without a record) is deliberately not
    // enforced: a Backend-built subset Manifest may keep a requirement's
    // provenance on the requirement itself while omitting the record.
    [[nodiscard]] static std::optional<std::string> recordIntegrityProblem(
        const ExecutionManifest& manifest);

    // The record a Backend-built SUBSET Manifest (continuation repair copies
    // only the repair-owner tasks, each with its frozen requirements and their
    // origin) may carry from its source. The source record is carried
    // verbatim, field for field, ONLY when it is fully consistent with the
    // subset's own tasks; otherwise nullopt. There is no partial projection
    // (that would misstate the policy's decision for the subset) and nothing
    // is ever derived again. The authoritative verification of a continuation
    // still runs over the SOURCE Manifest's tasks.
    [[nodiscard]] static std::optional<ManifestMandatoryVerificationRecord> inheritRecord(
        const std::optional<ManifestMandatoryVerificationRecord>& sourceRecord,
        const ExecutionManifest& subsetManifest);

    // Canonical, deterministic text of everything in the snapshot that can
    // change this policy's outcome (policy id plus per-preset catalog
    // confirmation/completeness). Part of the planning-input fingerprint so a
    // changed policy or catalog is never mistaken for an unchanged replay.
    // nullptr (no snapshot) yields a stable "no snapshot" value.
    [[nodiscard]] static std::string fingerprintMaterial(
        const VerificationEnvironmentSnapshot* snapshot);
};

[[nodiscard]] bool isMandatoryVerificationPolicyFailureCode(std::string_view code) noexcept;

} // namespace ai::application
