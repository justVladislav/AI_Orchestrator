#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kVerificationEnvironmentSnapshotSchemaVersion =
    "verification-environment-snapshot.v1";

// Bounded caps, matching the same "Backend-authored context is always
// bounded" convention as ExecutionManifestPolicy/ManifestWorkLedgerTypes
// elsewhere in this codebase.
inline constexpr std::size_t kVerificationEnvironmentMaxProfiles = 64;
inline constexpr std::size_t kVerificationEnvironmentMaxBuildPresetsPerProfile = 16;
inline constexpr std::size_t kVerificationEnvironmentMaxUnlinkedBuildPresets = 64;
inline constexpr std::size_t kVerificationEnvironmentMaxTargetsPerPreset = 32;
inline constexpr std::size_t kVerificationEnvironmentMaxTargetProbes = 16;
inline constexpr std::size_t kVerificationEnvironmentMaxRelativePathBytes = 1024;
inline constexpr std::size_t kVerificationEnvironmentMaxDiagnosticBytes = 2000;

// See AG-18 in ARCHITECTURE_GAPS.md and "Verification environment ownership
// and first-project bootstrap" in AI_ORCHESTRATOR_TARGET_ARCHITECTURE.md.
// AG-18.1's own discovery (VerificationEnvironmentDiscoveryService) can only
// ever produce ExistingProfile (a selectable profile was found) or
// SetupRequired (none was, or the workspace could not be confirmed
// consistent with its own baseline) - it has no visibility into which
// Manifest tasks, if any, will end up authorized to create the missing
// preset files, so it never itself produces ProposedBootstrap. That
// reclassification, informed by the frozen Manifest's own task scope, is
// AG-18.3's job. All three values are defined now so later packages never
// need a second enum.
enum class VerificationEnvironmentState {
    ExistingProfile,
    ProposedBootstrap,
    SetupRequired,
};

[[nodiscard]] const char* toString(VerificationEnvironmentState value) noexcept;
[[nodiscard]] std::optional<VerificationEnvironmentState>
verificationEnvironmentStateFromString(std::string_view value) noexcept;

// Which of the two well-known CMake preset files actually defines a
// discovered preset, established only when direct-parse enrichment matched
// its name in that file's own top-level array. When a preset name appears
// in both files, CMakeUserPresets.json wins - the same precedence real
// `cmake --list-presets` itself applies. Unknown means the preset's
// existence was confirmed by the authoritative cmake probe but enrichment
// found it in neither file (e.g. it arrived through an `include` this
// Backend deliberately does not follow) - this is an honest "provenance not
// established" value, never defaulted away to a guessed source.
enum class VerificationPresetFileSource {
    ProjectPresets, // CMakePresets.json
    UserPresets,    // CMakeUserPresets.json
    Unknown,
};

[[nodiscard]] const char* toString(VerificationPresetFileSource value) noexcept;
[[nodiscard]] std::optional<VerificationPresetFileSource>
verificationPresetFileSourceFromString(std::string_view value) noexcept;

// Where a VerificationProfile came from. AG-18.1 only ever produces
// Discovered; BackendConfigured (an explicit Backend/user-confirmed setting,
// not read from any workspace file) is defined now so the profile contract
// does not need to change shape again once a later package populates it.
enum class VerificationProfileOrigin {
    Discovered,
    BackendConfigured,
};

[[nodiscard]] const char* toString(VerificationProfileOrigin value) noexcept;
[[nodiscard]] std::optional<VerificationProfileOrigin>
verificationProfileOriginFromString(std::string_view value) noexcept;

// One CMake build preset. `name` is confirmed selectable by the
// authoritative `cmake --list-presets=build` probe - never merely present
// in a JSON file. declaredConfigurePreset/declaredTargets are best-effort
// enrichment from directly parsing CMakePresets.json/CMakeUserPresets.json:
// no `inherits` merging, no macro expansion. declaredTargets is the
// preset's own raw `targets` array when present (bounded), never a target
// this Backend invented.
struct VerificationBuildPresetDescriptor {
    std::string name;
    std::string displayName;
    VerificationPresetFileSource source {VerificationPresetFileSource::Unknown};
    std::string declaredConfigurePreset;
    std::vector<std::string> declaredTargets;
    // Bounded Backend-owned allow-list exposed to later binding. When the
    // preset declares targets, those declarations are the allow-list. When
    // it does not, discovery may obtain the list from an already-configured
    // build via the read-only `--target help` probe. An empty list with
    // allowedTargetsConfirmed=false is unknown, never "all targets allowed".
    bool allowedTargetsConfirmed {false};
    std::vector<std::string> allowedTargets;
    // AG-17.0: true only when allowedTargets is PROVABLY the whole target
    // catalog: confirmed, and neither the declared `targets` array nor the
    // configured-target probe was cut off by
    // kVerificationEnvironmentMaxTargetsPerPreset. Before this field a
    // truncated list was indistinguishable from a complete one (both read
    // allowedTargetsConfirmed=true), so "no QML-lint target in the snapshot"
    // could mean "the project has none" or "it fell past the cap". An absent
    // value in a pre-AG-17.0 persisted snapshot is false (unknown), never
    // true. Declared last so positional initializers keep their meaning.
    bool allowedTargetsComplete {false};
};

// One stable, Backend-owned verification profile: a configure preset
// confirmed to exist, together with every build preset that declares it as
// its own `configurePreset` (linked configure/build identity, not two
// independent flat lists a later package would have to re-link itself).
// profileId is currently just configurePresetName - the natural stable
// identifier already used throughout this codebase's ExecutionVerificationArgument
// `preset` values - kept as its own field so a future BackendConfigured
// profile can assign an id that is not literally a CMake preset name.
struct VerificationProfile {
    std::string profileId;
    std::string configurePresetName;
    std::string configureDisplayName;
    VerificationPresetFileSource configureSource {VerificationPresetFileSource::Unknown};
    // Raw, unexpanded binaryDir string as authored (empty when enrichment
    // could not find it) - never claimed to be the actual resolved path.
    std::string declaredBinaryDir;
    // Best-effort reduction of declaredBinaryDir to a *candidate-relative*
    // path: only ever performed when the raw string uses `${sourceDir}` as
    // its sole macro, with the macro itself stripped away. Deliberately
    // never an absolute host path - this snapshot's build location must
    // stay safe to join against whatever isolated candidate-overlay root a
    // later package actually builds in, never the real live workspace an
    // Apply-bound overlay must not read from or write into. Empty whenever
    // resolution was not attempted or was not safe (an already-absolute
    // declaredBinaryDir is not relocatable and is left unresolved here).
    std::string resolvedBinaryDir;
    VerificationProfileOrigin origin {VerificationProfileOrigin::Discovered};
    // False whenever this profile's configure preset was confirmed but the
    // build-preset probe for it failed/was unavailable/timed out - a later
    // package must not assume build or QML-lint verification is available
    // merely because the profile exists; check this first. True (default)
    // once buildPresets reflects a real, successful build-preset listing
    // (even when that listing is itself empty - "confirmed and empty" is
    // different from "not confirmed").
    bool buildPresetsConfirmed {true};
    std::vector<VerificationBuildPresetDescriptor> buildPresets;
};

// Backend-authored, immutable, per-Run discovery of the workspace's CMake
// verification environment. Discovered once, before the Planner ever runs,
// from provider-free sources only (cmake --list-presets, bounded direct
// parsing of the two well-known preset files at the workspace root). Frozen
// exactly like WorkspaceBaselineSnapshot/RunWorkspaceScope: one immutable
// row per Run, never mutated after insert - see
// IVerificationEnvironmentRepository::recordSnapshot.
struct VerificationEnvironmentSnapshot {
    std::string schemaVersion {std::string(kVerificationEnvironmentSnapshotSchemaVersion)};
    RunId runId;
    ProjectId projectId;
    // The owning Run's WorkspaceBaselineSnapshot::fingerprint, captured at
    // the same moment as this discovery - never re-derived later - so a
    // later reader can tell whether the workspace has since drifted from
    // what this snapshot was actually discovered against. Discovery itself
    // re-captures the full policy-visible workspace before and after
    // discovery and requires both captures to match this baseline - see
    // VerificationEnvironmentDiscoveryService.
    std::string baselineFingerprint;
    VerificationEnvironmentState state {VerificationEnvironmentState::SetupRequired};
    std::vector<VerificationProfile> profiles;
    // Build presets the authoritative cmake probe confirmed exist but whose
    // configurePreset link could not be established (enrichment missing/
    // unparseable, or it names a configure preset that was not itself
    // discovered) - kept for provenance/debugging rather than silently
    // dropped, never counted as part of any profile.
    std::vector<VerificationBuildPresetDescriptor> unlinkedBuildPresets;
    // Bounded, Backend-authored explanation - set for SetupRequired (why no
    // usable profile was found), and also set for ExistingProfile when
    // discovery was only partially successful (e.g. build presets could not
    // be listed even though a configure preset was confirmed). Empty only
    // when discovery was fully clean. Never provider text.
    std::string diagnostic;
    std::chrono::system_clock::time_point discoveredAt;
};

} // namespace ai::application
