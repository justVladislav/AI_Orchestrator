#pragma once

#include "ai/common/Result.hpp"

#include <filesystem>
#include <string>

namespace ai::application {

// AG-18.4B (reopened 2026-09-22): explicit Backend-owned verification-
// environment setup at Project-creation time. See "Verification environment
// ownership and first-project bootstrap" in
// AI_ORCHESTRATOR_TARGET_ARCHITECTURE.md and AG-18.4B's own contract in
// ARCHITECTURE_GAPS.md. This package implements ONLY the "create a minimal
// local Qt/CMake preset" mode - AG-18.4C (a true scoped setup-Run launcher
// that proposes setup files through the ordinary Manifest/ChangeProposal
// pipeline) remains separate and unimplemented; never describe this service
// as satisfying it.
//
// Stateless, provider-free, and deliberately does NOT extend
// Project.description or any other Project/SQLite field - the generated
// CMakeUserPresets.json file on disk IS the durable state, discoverable
// exactly like any manually-authored preset by
// VerificationEnvironmentDiscoveryService. Every field is validated here,
// never trusted from a caller (including the Qt adapter) - a Backend-owned
// allow-list for generator/configuration, a safe-token charset for preset
// names (matching ExecutionAcceptanceContract's own `safeToken` convention,
// since these names later flow into ExecutionVerificationArgument's
// `preset`/`buildPreset` values and must pass that same check at Manifest
// admission), a safe candidate-relative charset for the binary directory,
// and a real, existence-checked Qt installation prefix (absolute,
// resolvable, and containing its own `lib/cmake/Qt6/Qt6Config.cmake` -
// never merely a non-empty string). No API surface exists for arbitrary
// cache variables or secret/credential fields - only the two Backend-
// derived CMAKE_BUILD_TYPE/CMAKE_PREFIX_PATH variables are ever written.
//
// Never writes silently: composePresetDocument() never writes to disk (it
// performs the same bounded, synchronous local reads validateRequest()
// always does - Qt-prefix existence/package checks included - never a
// network or provider call) so the Qt adapter/QML can render an exact
// preview and target path, with the same validation errors the eventual
// write would produce, before any confirmation. createLocalPreset() - the
// only method that writes - must be called only after the user has
// explicitly confirmed that exact preview. This service performs no
// confirmation gating itself; that is the caller's responsibility.
struct CreateLocalVerificationPresetRequest {
    std::filesystem::path projectRoot;
    // Backend-owned allow-list, validated here - currently only "Ninja".
    std::string generator;
    // Backend-owned allow-list, validated here - "Debug" or "Release".
    std::string configuration;
    // An absolute, user-selected Qt installation prefix (e.g.
    // "C:/Qt/6.11.1/msvc2022_64"), written verbatim as CMAKE_PREFIX_PATH.
    // Validated as a real, existing, absolute directory containing its own
    // `lib/cmake/Qt6/Qt6Config.cmake` - see validateRequest() in the .cpp.
    std::string qtPrefixPath;
    std::string configurePresetName;
    std::string buildPresetName;
    // Candidate-relative, forward-slash binary directory WITHOUT the
    // "${sourceDir}/" prefix - this service always prepends that macro
    // itself, so a caller can never smuggle a different macro or an
    // absolute path through this field. e.g. "build/local-qt-debug".
    std::string binaryDirRelative;
};

struct CreateLocalVerificationPresetResult {
    std::filesystem::path writtenPath;
    std::string writtenJson;
};

class VerificationEnvironmentPresetSetupService {
public:
    // Validates every typed field - including a real, synchronous,
    // filesystem-backed check of qtPrefixPath (see the struct's own doc
    // comment) - and returns the exact UTF-8 JSON bytes createLocalPreset()
    // would write for the same request. Never writes to disk. Cheap enough
    // for a live UI preview on every keystroke; a real Qt install's own
    // `lib/cmake/Qt6/Qt6Config.cmake` presence is a handful of stat calls,
    // never a provider or subprocess call - only the separate, explicit
    // "Test configuration" action ever runs a real cmake probe.
    [[nodiscard]] static common::Result<std::string> composePresetDocument(
        const CreateLocalVerificationPresetRequest& request);

    // Validates that projectRoot exists, is a directory, and that
    // "<projectRoot>/CMakeUserPresets.json" passes WorkspacePathGuard's
    // canonical-containment and Windows symlink/reparse-point checks (the
    // same guard audited workspace tools use) - then returns that exact
    // canonical target path. Read-only; never checks whether the file
    // itself already exists (see createLocalPreset for that).
    [[nodiscard]] static common::Result<std::filesystem::path> resolveTargetPath(
        const std::filesystem::path& projectRoot);

    // The only method that writes to disk. Fails closed with a stable code
    // and writes nothing when: any field fails validation; projectRoot does
    // not exist/is not a directory; the target path fails the same
    // WorkspacePathGuard checks resolveTargetPath performs; or
    // CMakeUserPresets.json already exists (never overwritten, merged, or
    // even read - "verification_setup.preset_already_exists"). Writes via
    // an exclusively-created (CREATE_NEW on Windows, O_CREAT|O_EXCL on
    // POSIX - never a plain truncating open, which would leave a window for
    // something else to have already claimed that exact temp name) unique
    // temp sibling file, then an exclusive rename that itself fails if the
    // target now exists - see the .cpp. The caller (ProjectsPage.qml) must
    // treat this as one half of a combined operation with Project-row
    // creation: on a later Project-creation failure, call
    // discardGeneratedPreset() with this exact result to avoid
    // leaving a real file behind that no Project references.
    [[nodiscard]] static common::Result<CreateLocalVerificationPresetResult> createLocalPreset(
        const CreateLocalVerificationPresetRequest& request);

    // Identity-bound compensating delete for a preset THIS service itself
    // just wrote via createLocalPreset(). The current file must still be the
    // exact regular CMakeUserPresets.json bytes returned by that create; a
    // replaced or edited file is never deleted. This deliberately accepts
    // the full typed result, not an arbitrary path.
    [[nodiscard]] static common::Result<void> discardGeneratedPreset(
        const CreateLocalVerificationPresetResult& created);
};

} // namespace ai::application
