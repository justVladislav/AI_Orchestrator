#include "ai/application/ManifestVerificationEnvironmentPreflight.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>

namespace ai::application {

namespace fs = std::filesystem;

namespace {

common::Result<void> fail(std::string code, std::string message)
{
    return common::Result<void>::failure({std::move(code), std::move(message)});
}

std::optional<std::string> argument(
    const ExecutionAcceptanceRequirement& requirement, std::string_view name)
{
    const auto found = std::find_if(requirement.verificationArguments.begin(),
        requirement.verificationArguments.end(), [&](const auto& item) {
            return item.name == name;
        });
    return found == requirement.verificationArguments.end()
        ? std::nullopt : std::optional<std::string>(found->value);
}

std::string normalized(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    while (value.starts_with("./")) value.erase(0, 2);
    while (value.ends_with('/')) value.pop_back();
    return value;
}

bool ownedPathCovers(const fs::path& ownedPath, std::string_view relativePath)
{
    const auto owned = normalized(ownedPath.generic_string());
    const auto candidate = normalized(std::string(relativePath));
    return candidate == owned
        || (candidate.size() > owned.size() && candidate.starts_with(owned)
            && candidate[owned.size()] == '/');
}

bool anyTaskMayChange(
    const std::vector<ExecutionManifestTask>& tasks, std::string_view relativePath)
{
    return std::any_of(tasks.begin(), tasks.end(), [&](const auto& task) {
        return std::any_of(task.ownedPaths.begin(), task.ownedPaths.end(),
            [&](const auto& ownedPath) {
                return ownedPathCovers(ownedPath, relativePath);
            });
    });
}

common::Result<std::string> runProbe(
    IManifestVerificationCommandRunner& runner,
    ManifestVerificationCommandRequest command,
    std::stop_token stopToken)
{
    const auto outcome = runner.run(command, stopToken);
    if (outcome.cancelled || stopToken.stop_requested()) {
        return common::Result<std::string>::failure({
            "verification.environment_cancelled",
            "Verification-environment preflight was cancelled"});
    }
    if (!outcome.started) {
        return common::Result<std::string>::failure({
            "verification.environment_tool_unavailable",
            "CMake could not be started for verification-environment preflight"});
    }
    if (outcome.timedOut) {
        return common::Result<std::string>::failure({
            "verification.environment_probe_timeout",
            "Verification-environment preflight exceeded its time limit"});
    }
    if (outcome.outputTruncated) {
        return common::Result<std::string>::failure({
            "verification.environment_probe_overflow",
            "Verification-environment preflight output exceeded its limit"});
    }
    if (!outcome.exitCode.has_value() || *outcome.exitCode != 0) {
        return common::Result<std::string>::failure({
            "verification.environment_probe_failed",
            "CMake rejected the verification-environment probe"});
    }
    return common::Result<std::string>::ok(outcome.boundedOutput);
}

bool outputNames(std::string_view output, const std::string& name)
{
    return output.find('"' + name + '"') != std::string_view::npos;
}

bool regularFileExists(const fs::path& path, std::error_code& error)
{
    error.clear();
    const auto status = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        return false;
    }
    return !error && fs::is_regular_file(status);
}

} // namespace

common::Result<void> ManifestVerificationEnvironmentPreflight::validate(
    const ManifestVerificationEnvironmentPreflightRequest& request,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken)
{
    std::set<std::string> configurePresets;
    std::set<std::string> buildPresets;
    bool hasQmlLint = false;
    bool hasRequiredCMakeBuild = false;

    for (const auto& task : request.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements) {
            if (requirement.verificationKind == ExecutionVerificationKind::CMakeBuild) {
                hasRequiredCMakeBuild = hasRequiredCMakeBuild || requirement.required;
                const auto preset = argument(requirement, "preset");
                const auto buildPreset = argument(requirement, "buildPreset");
                if (preset.has_value()) configurePresets.insert(*preset);
                if (buildPreset.has_value()) buildPresets.insert(*buildPreset);
            } else if (requirement.verificationKind
                == ExecutionVerificationKind::QmlLint) {
                hasQmlLint = true;
            }
        }
    }

    if (!configurePresets.empty() || !buildPresets.empty()) {
        std::error_code presetError;
        const bool projectPresetExists = regularFileExists(
            request.workspaceRoot / "CMakePresets.json", presetError);
        if (presetError) {
            return fail("verification.configure_preset_unavailable",
                "The workspace CMake preset file could not be inspected");
        }
        const bool userPresetExists = regularFileExists(
            request.workspaceRoot / "CMakeUserPresets.json", presetError);
        if (presetError) {
            return fail("verification.configure_preset_unavailable",
                "The workspace CMake user-preset file could not be inspected");
        }
        const bool presetFileExists = projectPresetExists || userPresetExists;
        const bool presetMayBeProposed = anyTaskMayChange(
                request.tasks, "CMakePresets.json")
            || anyTaskMayChange(request.tasks, "CMakeUserPresets.json");
        if (!presetFileExists) {
            if (!presetMayBeProposed) {
                return fail("verification.configure_preset_unavailable",
                    "No CMake preset file exists in the workspace baseline");
            }
        } else if (!presetMayBeProposed) {
            ManifestVerificationCommandRequest configureList;
            configureList.commandIdentity = "cmake.list.configure_presets";
            configureList.executable = "cmake";
            configureList.arguments = {"--list-presets=configure"};
            configureList.workingDirectory = request.workspaceRoot;
            configureList.timeout = std::chrono::seconds(15);
            configureList.maxOutputBytes = 64 * 1024;
            const auto configureOutput = runProbe(
                commandRunner, std::move(configureList), stopToken);
            if (!configureOutput.success())
                return common::Result<void>::failure(*configureOutput.error());
            for (const auto& preset : configurePresets) {
                if (!outputNames(configureOutput.value(), preset)) {
                    return fail("verification.configure_preset_unavailable",
                        "A configured CMake configure preset is unavailable");
                }
            }

            ManifestVerificationCommandRequest buildList;
            buildList.commandIdentity = "cmake.list.build_presets";
            buildList.executable = "cmake";
            buildList.arguments = {"--list-presets=build"};
            buildList.workingDirectory = request.workspaceRoot;
            buildList.timeout = std::chrono::seconds(15);
            buildList.maxOutputBytes = 64 * 1024;
            const auto buildOutput = runProbe(
                commandRunner, std::move(buildList), stopToken);
            if (!buildOutput.success())
                return common::Result<void>::failure(*buildOutput.error());
            for (const auto& preset : buildPresets) {
                if (!outputNames(buildOutput.value(), preset)) {
                    return fail("verification.build_preset_unavailable",
                        "A configured CMake build preset is unavailable");
                }
            }
        }
    }

    // Proposal overlays intentionally contain source inputs only. A baseline
    // build directory (and its absolute-path CMakeCache.txt) is never trusted
    // or copied into the candidate. Consequently qml_lint must be preceded by
    // a Backend-authorized fresh configure/build profile in the same Manifest.
    if (hasQmlLint && (!hasRequiredCMakeBuild
        || configurePresets.empty() || buildPresets.empty())) {
        return fail("verification.clean_configure_prerequisite_missing",
            "QML lint requires a CMake build profile that configures the clean candidate");
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
