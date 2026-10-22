#pragma once

#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/VerificationEnvironmentSnapshotTypes.hpp"
#include "ai/application/VerificationToolchain.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ai::application {

// AG-18.4B-R1: does this project have a configured, buildable verification setup - proven, not presumed?
//
// The check extends the existing preset discovery and the existing configure / target probes; it is not a second pipeline.
// "Ready" means exactly: the selected configure preset exists, the toolchain for it is MSVC x64 and could be prepared, a real
// `cmake --preset` configure exited 0, and the selected build preset then reported its targets. A found preset name, a
// successful listing, or a completed Run alone prove none of that.
//
// Failure codes are stable and typed; every message ends with what the user can do about it. The check never spends provider
// money, never edits project files (the configure writes only the preset's own build directory) and never persists anything -
// persistence is the caller's (ProjectReadinessService).
inline constexpr const char* kProjectSetupPresetNotFound = "project_setup.preset_not_found";
inline constexpr const char* kProjectSetupSelectionRequired = "project_setup.preset_selection_required";
inline constexpr const char* kProjectSetupToolchainUnsupported = "project_setup.toolchain_unsupported";
inline constexpr const char* kProjectSetupToolchainUnavailable = "project_setup.toolchain_unavailable";
inline constexpr const char* kProjectSetupBuildDirectoryUnresolvable = "project_setup.build_directory_unresolvable";
inline constexpr const char* kProjectSetupConfigureFailed = "project_setup.configure_failed";
inline constexpr const char* kProjectSetupTargetsUnconfirmed = "project_setup.targets_unconfirmed";
inline constexpr const char* kProjectSetupCancelled = "project_setup.cancelled";

inline constexpr std::size_t kProjectSetupMaxMessageBytes = 1800;

struct ProjectSetupSelection {
    std::string configurePreset;
    std::string buildPreset;

    [[nodiscard]] bool operator==(const ProjectSetupSelection&) const = default;
    [[nodiscard]] bool empty() const noexcept { return configurePreset.empty() || buildPreset.empty(); }
};

struct ProjectSetupCheck {
    bool ready {false};
    // project_setup.* when not ready.
    std::string failureCode;
    std::string failureMessage;
    // The pair that was checked (empty when the check stopped before one was chosen).
    ProjectSetupSelection selection;
    // The selected configure preset's build directory, relative to the project (the evidence is only usable while its
    // CMakeCache.txt exists). A check refuses a preset whose directory cannot be resolved this way.
    std::string binaryDirRelative;
    // Confirmed targets of the selected build preset, and whether the list is provably the whole catalog.
    std::vector<std::string> targets;
    bool targetsComplete {false};
    // The toolchain the evidence was gathered with (empty when it was never prepared).
    std::string toolchainIdentity;
    // preset_selection_required: every usable "configure / build" pair, for the user to choose from.
    std::vector<ProjectSetupSelection> candidates;
};

class ProjectSetupReadiness final {
public:
    ProjectSetupReadiness() = delete;

    using ToolchainProvider = std::function<VerificationToolchainEnvironment()>;

    // Runs the whole check against `projectRoot`. `saved` is the pair the user (or an earlier check) selected; without one a
    // single usable pair is chosen automatically and several are refused (the user chooses - never a guess, never a paid
    // planning step with an undefined choice). `toolchain` defaults to VerificationToolchain::current().
    [[nodiscard]] static ProjectSetupCheck check(
        const std::filesystem::path& projectRoot,
        const std::optional<ProjectSetupSelection>& saved,
        IManifestVerificationCommandRunner& commandRunner,
        std::stop_token stopToken,
        const ToolchainProvider& toolchain = {});

    // ---- pure / file-only pieces (public so each is tested on its own) ----

    // The pair to check. `unsupportedReason` names why a configure preset cannot be used (MinGW, ARM, 32-bit); it is asked only
    // for pairs that exist. Errors carry the project_setup.* code and, for a refusal that needs the user, the candidates.
    [[nodiscard]] static common::Result<ProjectSetupSelection> chooseSelection(
        const VerificationEnvironmentSnapshot& catalog,
        const std::optional<ProjectSetupSelection>& saved,
        const std::function<std::optional<std::string>(const std::string& configurePreset)>& unsupportedReason,
        std::vector<ProjectSetupSelection>* candidates);

    // Why the selected configure preset is not an MSVC x64 setup, read from the preset files at `projectRoot` (generator,
    // architecture, toolchain file, compiler and Qt-prefix cache variables; `inherits` and `include` are followed, bounded and
    // never outside the project). nullopt = nothing says it is not MSVC x64.
    [[nodiscard]] static std::optional<std::string> unsupportedToolchainReason(
        const std::filesystem::path& projectRoot, const std::string& configurePreset);

    // The preset files (CMakePresets.json, CMakeUserPresets.json and what they `include`, inside the project only) plus the
    // toolchain files the presets name, when they can be resolved. Existing files only; these are part of the readiness
    // fingerprint.
    [[nodiscard]] static std::vector<std::filesystem::path> presetInputFiles(const std::filesystem::path& projectRoot);

    // Same question asked of a configured build tree's CMakeCache.txt: the compiler that CMake actually chose.
    [[nodiscard]] static std::optional<std::string> unsupportedCompilerReason(const std::filesystem::path& cacheFile);

    // Text safe to store and show: each line is valid UTF-8 (a line in the console's OEM code page is converted), control
    // characters are dropped, and only the last `maxBytes` are kept, cut at a line boundary.
    [[nodiscard]] static std::string displayableTail(const std::string& processOutput, std::size_t maxBytes);
};

} // namespace ai::application
