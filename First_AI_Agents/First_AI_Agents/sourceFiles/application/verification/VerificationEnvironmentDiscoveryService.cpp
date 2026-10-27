#include "ai/application/verification/VerificationEnvironmentDiscoveryService.hpp"
#include "ai/application/verification/VerificationEnvironmentProbes.hpp"
#include "ai/application/workspace/WorkspacePathGuard.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace ai::application {

namespace fs = std::filesystem;

namespace {

// A malformed/oversized preset file must never break discovery - it only
// ever costs the enrichment (or baseline-consistency check) that file would
// have provided. 2 MiB is far beyond any real CMakePresets.json while still
// bounding the read.
constexpr std::uintmax_t kMaxPresetFileBytes = 2 * 1024 * 1024;
constexpr const char* kProjectPresetsFileName = "CMakePresets.json";
constexpr const char* kUserPresetsFileName = "CMakeUserPresets.json";

std::string diagnostic(std::string text)
{
    if (text.size() > kVerificationEnvironmentMaxDiagnosticBytes) {
        text.resize(kVerificationEnvironmentMaxDiagnosticBytes);
    }
    return text;
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

struct PresetFilePresence {
    bool projectPresetExists {false};
    bool userPresetExists {false};
    bool inspectionFailed {false};
};

PresetFilePresence inspectPresetFilePresence(const fs::path& root)
{
    PresetFilePresence presence;
    std::error_code projectError;
    presence.projectPresetExists = regularFileExists(
        root / kProjectPresetsFileName, projectError);
    std::error_code userError;
    presence.userPresetExists = regularFileExists(
        root / kUserPresetsFileName, userError);
    presence.inspectionFailed = static_cast<bool>(projectError)
        || static_cast<bool>(userError);
    return presence;
}

// Best-effort bounded read. Absent, oversized, or unreadable files simply
// return nullopt - this is enrichment, never the authority for whether a
// preset exists (see the class doc comment).
std::optional<std::string> readBounded(const fs::path& path)
{
    std::error_code sizeError;
    const auto size = fs::file_size(path, sizeError);
    if (sizeError || size > kMaxPresetFileBytes) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) return std::nullopt;
    return buffer.str();
}

// Requires a full re-capture of the policy-visible workspace to compare as
// "unchanged" against the Run's own already-captured WorkspaceBaselineSnapshot.
// The baseline is captured moments before discovery runs
// (ManifestWorkflowRunner::run()), but discovery reads the live filesystem
// independently; without this check a workspace edited in that narrow
// window would be discovered against content the baseline never actually
// saw, while the snapshot still claimed that baseline's fingerprint. This
// is deliberately a full recapture, not just the two preset files: a
// preset can pull additional definitions in via CMake's own `include`
// mechanism, which this Backend does not follow, so only a full
// re-comparison reliably catches drift in whatever a preset transitively
// depends on.
std::optional<std::string> checkWorkspaceStillMatchesBaseline(
    const WorkspaceReadPolicy& inspectPolicy, const WorkspaceBaselineSnapshot& baseline)
{
    const auto current = WorkspaceBaselineProvenance::capture(inspectPolicy);
    if (!current.success()) {
        return std::string("The workspace could not be re-inspected to confirm it still matches "
            "the captured baseline");
    }
    const auto compared = WorkspaceBaselineProvenance::compare(baseline, current.value());
    if (!compared.success()) {
        return std::string("The workspace could not be compared against the captured baseline");
    }
    if (compared.value().classification != "unchanged") {
        return std::string("The workspace changed since the baseline was captured");
    }
    return std::nullopt;
}

// Best-effort reduction of a declared binaryDir to a *candidate-relative*
// path: only when the raw string uses `${sourceDir}` as a prefix with no
// other unresolved `${...}` macro remaining, with the macro itself stripped
// away. Deliberately never resolved against the real live workspace, and an
// already-absolute declaredBinaryDir is left unresolved - neither is safe
// to join against whatever isolated candidate-overlay root a later package
// actually builds in. Not a general CMake macro engine - the one other
// macro it replaces is `${presetName}` (the preset's own name, when that is
// a plain path segment); anything else (inherits, environment variables,
// ...) is left unresolved rather than guessed.
bool safeTargetId(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

std::string resolveBinaryDir(std::string declared, const std::string& presetName)
{
    // `${presetName}` is the one other macro presets use for a build directory ("${sourceDir}/build/${presetName}"); it is
    // replaced with the preset's own name when that name is a plain path segment.
    constexpr std::string_view presetNameMacro = "${presetName}";
    if (declared.find(presetNameMacro) != std::string::npos) {
        const bool plainSegment = !presetName.empty() && presetName.find_first_of("/\\:$*?\"<>|{}[]") == std::string::npos
            && presetName != "." && presetName != "..";
        if (!plainSegment) return {};
        for (auto at = declared.find(presetNameMacro); at != std::string::npos; at = declared.find(presetNameMacro)) {
            declared.replace(at, presetNameMacro.size(), presetName);
        }
    }
    if (declared.empty() || fs::path(declared).is_absolute()) return {};
    constexpr std::string_view sourceDirMacro = "${sourceDir}";
    if (!declared.starts_with(sourceDirMacro)) return {};
    std::string rest = declared.substr(sourceDirMacro.size());
    if (rest.size() < 2 || (rest.front() != '/' && rest.front() != '\\')
        || (rest[1] == '/' || rest[1] == '\\')) {
        return {};
    }
    rest.erase(0, 1);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    if (rest.empty() || rest.size() > kVerificationEnvironmentMaxRelativePathBytes
        || rest.front() == '/' || rest.back() == '/' || rest.find(':') != std::string::npos
        || rest.find('$') != std::string::npos) {
        return {};
    }
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto end = rest.find('/', start);
        const auto part = std::string_view(rest).substr(start,
            (end == std::string::npos ? rest.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return {};
        for (const unsigned char ch : part) {
            if (ch < 0x20 || ch == 0x7f || ch == '*' || ch == '?' || ch == '['
                || ch == ']' || ch == '{' || ch == '}') {
                return {};
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return rest;
}

struct PresetEnrichment {
    std::string declaredBinaryDir;         // configure presets only
    std::string declaredConfigurePreset;   // build presets only
    std::vector<std::string> declaredTargets; // build presets only
    // AG-17.0: true when the preset's own `targets` array had more than
    // kVerificationEnvironmentMaxTargetsPerPreset entries, so declaredTargets
    // above is only its first slice. Without this, a truncated declaration
    // passed the declaredTargets.size()==allowedTargets.size() "confirmed"
    // comparison and read as a complete catalog.
    bool declaredTargetsTruncated {false};
    VerificationPresetFileSource source {VerificationPresetFileSource::Unknown};
};

// Parses one file's own top-level configurePresets/buildPresets arrays only
// - no `include` following, no `inherits` merging, no macro expansion of
// binaryDir. Silently yields no entries on any parse/shape problem; the
// caller already treats every entry here as best-effort.
void collectEnrichment(
    const fs::path& path,
    VerificationPresetFileSource source,
    std::map<std::string, PresetEnrichment>& configureEnrichment,
    std::map<std::string, PresetEnrichment>& buildEnrichment)
{
    const auto raw = readBounded(path);
    if (!raw.has_value()) return;
    const auto parsed = nlohmann::json::parse(*raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return;

    if (parsed.contains("configurePresets") && parsed["configurePresets"].is_array()) {
        std::size_t count = 0;
        for (const auto& item : parsed["configurePresets"]) {
            if (++count > kVerificationEnvironmentMaxProfiles) break;
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
            PresetEnrichment entry;
            entry.source = source;
            if (item.contains("binaryDir") && item["binaryDir"].is_string()) {
                entry.declaredBinaryDir = item["binaryDir"].get<std::string>();
            }
            configureEnrichment[item["name"].get<std::string>()] = std::move(entry);
        }
    }
    if (parsed.contains("buildPresets") && parsed["buildPresets"].is_array()) {
        std::size_t count = 0;
        for (const auto& item : parsed["buildPresets"]) {
            if (++count > kVerificationEnvironmentMaxUnlinkedBuildPresets) break;
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
            PresetEnrichment entry;
            entry.source = source;
            if (item.contains("configurePreset") && item["configurePreset"].is_string()) {
                entry.declaredConfigurePreset = item["configurePreset"].get<std::string>();
            }
            if (item.contains("targets") && item["targets"].is_array()) {
                std::size_t targetCount = 0;
                for (const auto& target : item["targets"]) {
                    if (++targetCount > kVerificationEnvironmentMaxTargetsPerPreset) {
                        entry.declaredTargetsTruncated = true;
                        break;
                    }
                    if (target.is_string()) entry.declaredTargets.push_back(target.get<std::string>());
                }
            }
            buildEnrichment[item["name"].get<std::string>()] = std::move(entry);
        }
    }
}

// Extracts every double-quoted token that starts a (trimmed) line of
// `cmake --list-presets` output, e.g. `  "name"       - Display Name`. Lines
// without a leading quote (headers, blank lines) are skipped. This is the
// authoritative source of which preset names actually exist - it reflects
// cmake's own hidden/condition/include resolution, which a raw JSON parse
// cannot safely replicate.
std::vector<std::string> parsePresetNames(std::string_view output)
{
    std::vector<std::string> names;
    std::size_t lineStart = 0;
    while (lineStart <= output.size()) {
        const auto lineEnd = output.find('\n', lineStart);
        const std::string_view line = lineEnd == std::string_view::npos
            ? output.substr(lineStart)
            : output.substr(lineStart, lineEnd - lineStart);
        const auto firstNonSpace = line.find_first_not_of(" \t\r");
        if (firstNonSpace != std::string_view::npos && line[firstNonSpace] == '"') {
            const auto closing = line.find('"', firstNonSpace + 1);
            if (closing != std::string_view::npos && closing > firstNonSpace + 1) {
                if (names.size() < kVerificationEnvironmentMaxProfiles) {
                    names.emplace_back(line.substr(firstNonSpace + 1, closing - firstNonSpace - 1));
                }
            }
        }
        if (lineEnd == std::string_view::npos) break;
        lineStart = lineEnd + 1;
    }
    return names;
}

std::string displayNameFromLine(std::string_view output, std::string_view name)
{
    const auto marker = "\"" + std::string(name) + "\"";
    const auto position = output.find(marker);
    if (position == std::string_view::npos) return {};
    auto rest = output.substr(position + marker.size());
    const auto lineEnd = rest.find('\n');
    if (lineEnd != std::string_view::npos) rest = rest.substr(0, lineEnd);
    auto dash = rest.find('-');
    if (dash == std::string_view::npos) return {};
    rest = rest.substr(dash + 1);
    const auto firstNonSpace = rest.find_first_not_of(" \t\r");
    if (firstNonSpace == std::string_view::npos) return {};
    auto lastNonSpace = rest.find_last_not_of(" \t\r");
    return std::string(rest.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1));
}

struct PresetProbeOutcome {
    bool usable {false}; // true only on a clean, non-cancelled, non-timed-out exit 0
    std::vector<std::string> names;
    std::string output;
};

PresetProbeOutcome runListPresets(
    IManifestVerificationCommandRunner& commandRunner,
    const fs::path& workspaceRoot,
    std::string_view listArgument,
    std::stop_token stopToken)
{
    ManifestVerificationCommandRequest request;
    request.commandIdentity = std::string("cmake.list.") .append(listArgument);
    request.executable = "cmake";
    request.arguments = {std::string("--list-presets=").append(listArgument)};
    request.workingDirectory = workspaceRoot;
    request.timeout = std::chrono::seconds(15);
    request.maxOutputBytes = 64 * 1024;
    const auto outcome = commandRunner.run(request, stopToken);
    PresetProbeOutcome result;
    if (outcome.cancelled || stopToken.stop_requested() || !outcome.started
        || outcome.timedOut || outcome.outputTruncated
        || !outcome.exitCode.has_value() || *outcome.exitCode != 0) {
        return result;
    }
    result.usable = true;
    result.output = outcome.boundedOutput;
    result.names = parsePresetNames(outcome.boundedOutput);
    return result;
}

struct ParsedConfiguredTargets {
    std::vector<std::string> targets;
    // AG-17.0: true when at least one further valid, distinct target existed
    // beyond kVerificationEnvironmentMaxTargetsPerPreset. Which targets were
    // kept then depends on cmake's own output order, so a truncated list must
    // never be read as a complete catalog.
    bool truncated {false};
};

ParsedConfiguredTargets parseConfiguredTargets(std::string_view output)
{
    std::set<std::string> unique;
    bool truncated = false;
    std::size_t lineStart = 0;
    while (lineStart <= output.size()) {
        const auto lineEnd = output.find('\n', lineStart);
        auto line = lineEnd == std::string_view::npos
            ? output.substr(lineStart)
            : output.substr(lineStart, lineEnd - lineStart);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos) {
            line.remove_prefix(first);
            std::string_view candidate;
            if (line.starts_with("... ")) {
                line.remove_prefix(4);
                const auto end = line.find_first_of(" \t\r");
                candidate = line.substr(0, end);
            } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
                candidate = line.substr(0, colon);
                const auto last = candidate.find_last_not_of(" \t\r");
                if (last != std::string_view::npos) candidate = candidate.substr(0, last + 1);
            }
            if (safeTargetId(candidate)) {
                if (unique.size() >= kVerificationEnvironmentMaxTargetsPerPreset) {
                    // Already holding the cap: a further DISTINCT target
                    // proves the list is cut off; a repeat of a held one
                    // does not.
                    if (unique.find(std::string(candidate)) == unique.end()) {
                        truncated = true;
                        break;
                    }
                } else {
                    unique.emplace(candidate);
                }
            }
        }
        if (lineEnd == std::string_view::npos) break;
        lineStart = lineEnd + 1;
    }
    return {{unique.begin(), unique.end()}, truncated};
}

struct TargetProbeOutcome {
    bool confirmed {false};
    // AG-17.0: confirmed AND not cut off by the per-preset target cap.
    bool complete {false};
    std::vector<std::string> targets;
};

TargetProbeOutcome runConfiguredTargetProbe(
    IManifestVerificationCommandRunner& commandRunner,
    const fs::path& workspaceRoot,
    std::string_view buildPreset,
    std::stop_token stopToken)
{
    ManifestVerificationCommandRequest request;
    request.commandIdentity = "cmake.list.configured_targets";
    request.executable = "cmake";
    request.arguments = {"--build", "--preset", std::string(buildPreset), "--target", "help"};
    request.workingDirectory = workspaceRoot;
    request.timeout = std::chrono::seconds(15);
    request.maxOutputBytes = 64 * 1024;
    const auto outcome = commandRunner.run(request, stopToken);
    if (outcome.cancelled || stopToken.stop_requested() || !outcome.started
        || outcome.timedOut || outcome.outputTruncated
        || !outcome.exitCode.has_value() || *outcome.exitCode != 0) {
        return {};
    }
    auto parsed = parseConfiguredTargets(outcome.boundedOutput);
    if (parsed.targets.empty()) return {};
    return {true, !parsed.truncated, std::move(parsed.targets)};
}

// AG-18.3 hardening (blocker 3): the real, bounded, argument-vector-only
// configure probe a candidate overlay needs before any target can be
// genuinely confirmed - never a shell string, never the live workspace's
// own build tree. Fixed timeout/output cap, matching the other cmake
// probes in this file exactly.
bool runCandidateConfigureProbe(
    IManifestVerificationCommandRunner& commandRunner,
    const fs::path& candidateRoot,
    const std::string& configurePresetName,
    std::stop_token stopToken)
{
    ManifestVerificationCommandRequest request;
    request.commandIdentity = "cmake.candidate.configure";
    request.executable = "cmake";
    request.arguments = {"--preset", configurePresetName};
    request.workingDirectory = candidateRoot;
    request.timeout = std::chrono::seconds(180);
    request.maxOutputBytes = 256 * 1024;
    const auto outcome = commandRunner.run(request, stopToken);
    return !outcome.cancelled && !stopToken.stop_requested() && outcome.started
        && !outcome.timedOut && outcome.exitCode.has_value() && *outcome.exitCode == 0;
}

bool configuredBuildTreeExists(
    const fs::path& workspaceRoot, const std::string& candidateRelativeBinaryDir)
{
    if (candidateRelativeBinaryDir.empty()) return false;
    auto policy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    policy.deniedPaths.clear();
    policy.deniedDirectoryNames.clear();
    policy.deniedDirectorySequences.clear();
    policy.deniedFileNames.clear();
    policy.deniedExtensions.clear();
    const auto cache = WorkspacePathGuard::validate(
        candidateRelativeBinaryDir + "/CMakeCache.txt", policy);
    if (!cache.success()) return false;
    std::error_code error;
    return fs::is_regular_file(fs::status(cache.value(), error)) && !error;
}

// The provider-free profile/build-preset discovery core, shared by
// discoverAndRecord (against the Run's live workspace, baseline-drift
// checked on both sides by the caller) and discoverForCandidateRoot
// (against an isolated overlay root, no baseline concept at all). Returns
// only state/profiles/unlinkedBuildPresets/diagnostic populated - the
// caller stamps runId/projectId/baselineFingerprint/discoveredAt itself.
VerificationEnvironmentSnapshot discoverProfilesAndPresets(
    const fs::path& root,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken)
{
    VerificationEnvironmentSnapshot snapshot;

    const auto presence = inspectPresetFilePresence(root);
    const bool projectPresetExists = presence.projectPresetExists;
    const bool userPresetExists = presence.userPresetExists;

    if (presence.inspectionFailed) {
        snapshot.state = VerificationEnvironmentState::SetupRequired;
        snapshot.diagnostic = diagnostic(
            "The workspace CMake preset file(s) could not be inspected");
        return snapshot;
    }
    if (!projectPresetExists && !userPresetExists) {
        snapshot.state = VerificationEnvironmentState::SetupRequired;
        snapshot.diagnostic = diagnostic(
            "No CMakePresets.json or CMakeUserPresets.json exists in the workspace baseline");
        return snapshot;
    }

    const auto configureProbe = runListPresets(commandRunner, root, "configure", stopToken);
    if (!configureProbe.usable) {
        snapshot.state = VerificationEnvironmentState::SetupRequired;
        snapshot.diagnostic = diagnostic(
            "cmake could not be started, timed out, or failed while listing configure presets");
        return snapshot;
    }
    if (configureProbe.names.empty()) {
        snapshot.state = VerificationEnvironmentState::SetupRequired;
        snapshot.diagnostic = diagnostic("No selectable CMake configure preset was found");
        return snapshot;
    }

    std::map<std::string, PresetEnrichment> configureEnrichment;
    std::map<std::string, PresetEnrichment> buildEnrichment;
    // CMakeUserPresets.json is collected second so its entries overwrite
    // any same-named CMakePresets.json entry - the same precedence real
    // CMake itself applies.
    if (projectPresetExists) {
        collectEnrichment(root / kProjectPresetsFileName,
            VerificationPresetFileSource::ProjectPresets, configureEnrichment, buildEnrichment);
    }
    if (userPresetExists) {
        collectEnrichment(root / kUserPresetsFileName,
            VerificationPresetFileSource::UserPresets, configureEnrichment, buildEnrichment);
    }

    std::map<std::string, std::size_t> profileIndexByName;
    for (const auto& name : configureProbe.names) {
        VerificationProfile profile;
        profile.profileId = name;
        profile.configurePresetName = name;
        profile.configureDisplayName = displayNameFromLine(configureProbe.output, name);
        const auto found = configureEnrichment.find(name);
        if (found != configureEnrichment.end()) {
            profile.configureSource = found->second.source;
            profile.declaredBinaryDir = found->second.declaredBinaryDir;
            profile.resolvedBinaryDir = resolveBinaryDir(found->second.declaredBinaryDir, name);
        }
        profileIndexByName[name] = snapshot.profiles.size();
        snapshot.profiles.push_back(std::move(profile));
    }

    // The build probe is only worth attempting when discovery is still
    // live and a configure preset genuinely exists - a cancelled/already-
    // stopping caller must not see a second cmake process spawned on its
    // way out.
    const bool attemptBuildProbe = !stopToken.stop_requested();
    const auto buildProbe = attemptBuildProbe
        ? runListPresets(commandRunner, root, "build", stopToken)
        : PresetProbeOutcome {};

    if (buildProbe.usable) {
        std::size_t targetProbeCount = 0;
        bool targetCatalogIncomplete = false;
        for (const auto& name : buildProbe.names) {
            VerificationBuildPresetDescriptor descriptor;
            descriptor.name = name;
            descriptor.displayName = displayNameFromLine(buildProbe.output, name);
            const auto found = buildEnrichment.find(name);
            std::optional<std::size_t> linkedProfileIndex;
            bool declaredTargetsTruncated = false;
            if (found != buildEnrichment.end()) {
                descriptor.source = found->second.source;
                descriptor.declaredConfigurePreset = found->second.declaredConfigurePreset;
                descriptor.declaredTargets = found->second.declaredTargets;
                declaredTargetsTruncated = found->second.declaredTargetsTruncated;
                const auto profileFound = profileIndexByName.find(
                    found->second.declaredConfigurePreset);
                if (profileFound != profileIndexByName.end()) {
                    linkedProfileIndex = profileFound->second;
                }
            }
            if (!descriptor.declaredTargets.empty()) {
                std::set<std::string> allowed;
                for (const auto& target : descriptor.declaredTargets) {
                    if (safeTargetId(target)) allowed.emplace(target);
                }
                descriptor.allowedTargets.assign(allowed.begin(), allowed.end());
                descriptor.allowedTargetsConfirmed = !descriptor.allowedTargets.empty()
                    && descriptor.allowedTargets.size() == descriptor.declaredTargets.size();
                if (!descriptor.allowedTargetsConfirmed) {
                    descriptor.allowedTargets.clear();
                }
                // AG-17.0: a declared list cut at the cap still compares equal
                // to its own (already cut) allow-list above, so completeness
                // needs the explicit truncation flag.
                descriptor.allowedTargetsComplete =
                    descriptor.allowedTargetsConfirmed && !declaredTargetsTruncated;
            } else if (linkedProfileIndex.has_value()) {
                const auto& profile = snapshot.profiles[*linkedProfileIndex];
                if (targetProbeCount < kVerificationEnvironmentMaxTargetProbes
                    && !stopToken.stop_requested()
                    && configuredBuildTreeExists(root, profile.resolvedBinaryDir)) {
                    ++targetProbeCount;
                    auto targetProbe = runConfiguredTargetProbe(
                        commandRunner, root, descriptor.name, stopToken);
                    descriptor.allowedTargetsConfirmed = targetProbe.confirmed;
                    descriptor.allowedTargetsComplete = targetProbe.complete;
                    descriptor.allowedTargets = std::move(targetProbe.targets);
                }
                if (!descriptor.allowedTargetsConfirmed) targetCatalogIncomplete = true;
            }
            if (linkedProfileIndex.has_value()) {
                auto& buildPresets = snapshot.profiles[*linkedProfileIndex].buildPresets;
                if (buildPresets.size() < kVerificationEnvironmentMaxBuildPresetsPerProfile) {
                    buildPresets.push_back(std::move(descriptor));
                }
            } else if (snapshot.unlinkedBuildPresets.size()
                    < kVerificationEnvironmentMaxUnlinkedBuildPresets) {
                snapshot.unlinkedBuildPresets.push_back(std::move(descriptor));
            }
        }
        snapshot.state = VerificationEnvironmentState::ExistingProfile;
        if (targetCatalogIncomplete) {
            snapshot.diagnostic = diagnostic(
                "One or more build presets have no confirmed configured-target catalog");
        }
    } else {
        // A confirmed configure preset is still a real, usable profile -
        // build presets simply could not be confirmed this time. Recorded
        // honestly (both a diagnostic and a typed per-profile flag) rather
        // than silently losing the fact that discovery was only partially
        // successful; a later package must not assume build/QML-lint
        // verification is available merely because these profiles exist.
        for (auto& profile : snapshot.profiles) profile.buildPresetsConfirmed = false;
        snapshot.state = VerificationEnvironmentState::ExistingProfile;
        snapshot.diagnostic = diagnostic(
            "cmake could not be started, timed out, or failed while listing build presets; "
            "configure preset(s) were still confirmed");
    }
    return snapshot;
}

} // namespace

namespace detail {

VerificationEnvironmentSnapshot probePresetCatalog(
    const fs::path& root, IManifestVerificationCommandRunner& commandRunner, std::stop_token stopToken)
{
    return discoverProfilesAndPresets(root, commandRunner, stopToken);
}

ConfiguredTargetsProbe probeConfiguredTargets(
    IManifestVerificationCommandRunner& commandRunner,
    const fs::path& root,
    std::string_view buildPreset,
    std::stop_token stopToken)
{
    auto probe = runConfiguredTargetProbe(commandRunner, root, buildPreset, stopToken);
    return {probe.confirmed, probe.complete, std::move(probe.targets)};
}

} // namespace detail

VerificationEnvironmentDiscoveryService::VerificationEnvironmentDiscoveryService(
    IVerificationEnvironmentRepository& repository)
    : repository_(repository)
{
}

common::Result<VerificationEnvironmentSnapshot>
VerificationEnvironmentDiscoveryService::discoverAndRecord(
    const RunId& runId,
    const ProjectId& projectId,
    const fs::path& workspaceRoot,
    const WorkspaceBaselineSnapshot& baseline,
    const WorkspaceReadPolicy& inspectPolicy,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken,
    const std::optional<VerificationProfileSelection>& selection)
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = projectId;
    snapshot.baselineFingerprint = baseline.fingerprint;
    snapshot.discoveredAt = std::chrono::system_clock::now();

    if (const auto driftReason = checkWorkspaceStillMatchesBaseline(inspectPolicy, baseline);
        driftReason.has_value()) {
        // Fail fast, never invoke cmake against content the baseline never
        // actually saw.
        snapshot.state = VerificationEnvironmentState::SetupRequired;
        snapshot.diagnostic = diagnostic(*driftReason);
    } else {
        auto core = discoverProfilesAndPresets(workspaceRoot, commandRunner, stopToken);
        if (selection.has_value() && core.state == VerificationEnvironmentState::ExistingProfile) {
            // The user chose one pair: only that pair is offered to the Planner and the mandatory-verification policy. A pair that
            // is no longer there leaves the catalog as it is (the readiness check reports the missing pair before this point).
            auto chosen = std::find_if(core.profiles.begin(), core.profiles.end(), [&](const VerificationProfile& profile) {
                return profile.configurePresetName == selection->configurePreset
                    && std::any_of(profile.buildPresets.begin(), profile.buildPresets.end(),
                        [&](const VerificationBuildPresetDescriptor& buildPreset) {
                            return buildPreset.name == selection->buildPreset;
                        });
            });
            if (chosen != core.profiles.end()) {
                VerificationProfile kept = std::move(*chosen);
                kept.buildPresets.erase(std::remove_if(kept.buildPresets.begin(), kept.buildPresets.end(),
                                            [&](const VerificationBuildPresetDescriptor& buildPreset) {
                                                return buildPreset.name != selection->buildPreset;
                                            }),
                    kept.buildPresets.end());
                core.profiles.clear();
                core.profiles.push_back(std::move(kept));
                core.unlinkedBuildPresets.clear();
                core.diagnostic = diagnostic("Restricted to the project's selected presets \"" + selection->configurePreset
                    + "\" / \"" + selection->buildPreset + "\"");
            }
        }
        snapshot.state = core.state;
        snapshot.profiles = std::move(core.profiles);
        snapshot.unlinkedBuildPresets = std::move(core.unlinkedBuildPresets);
        snapshot.diagnostic = std::move(core.diagnostic);
    }

    // The first comparison prevents discovery from starting against stale
    // source. This final comparison binds the completed discovery result to
    // the same immutable baseline: any edit while CMake or the bounded JSON
    // reads were in flight discards the catalog instead of freezing a mixed
    // snapshot. Changes after this point cannot affect the already-captured
    // result and remain covered by the existing post-freeze preflight.
    if (const auto driftReason = checkWorkspaceStillMatchesBaseline(inspectPolicy, baseline);
        driftReason.has_value()) {
        snapshot.state = VerificationEnvironmentState::SetupRequired;
        snapshot.profiles.clear();
        snapshot.unlinkedBuildPresets.clear();
        snapshot.diagnostic = diagnostic(*driftReason);
    }

    const auto recorded = repository_.recordSnapshot(snapshot);
    if (!recorded.success()) {
        if (recorded.error()->code == "verification_environment.already_recorded") {
            const auto existing = findForRun(runId);
            if (existing.success() && existing.value().has_value()) {
                return common::Result<VerificationEnvironmentSnapshot>::ok(*existing.value());
            }
        }
        return common::Result<VerificationEnvironmentSnapshot>::failure(*recorded.error());
    }
    return common::Result<VerificationEnvironmentSnapshot>::ok(std::move(snapshot));
}

common::Result<std::optional<VerificationEnvironmentSnapshot>>
VerificationEnvironmentDiscoveryService::findForRun(const RunId& runId) const
{
    return repository_.findForRun(runId);
}

common::Result<bool> VerificationEnvironmentDiscoveryService::hasPresetFileAtWorkspaceRoot(
    const fs::path& workspaceRoot)
{
    const auto presence = inspectPresetFilePresence(workspaceRoot);
    if (presence.inspectionFailed) {
        return common::Result<bool>::failure({
            "verification_environment.preset_probe_failed",
            "The workspace CMake preset file(s) could not be inspected"});
    }
    return common::Result<bool>::ok(
        presence.projectPresetExists || presence.userPresetExists);
}

common::Result<VerificationEnvironmentSnapshot>
VerificationEnvironmentDiscoveryService::discoverForCandidateRoot(
    const fs::path& candidateRoot,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken)
{
    auto snapshot = discoverProfilesAndPresets(candidateRoot, commandRunner, stopToken);
    snapshot.discoveredAt = std::chrono::system_clock::now();

    // AG-18.3 hardening (blocker 3): a preset's own JSON-declared `targets`
    // array is provider-proposed configuration, never proof - discovery
    // shared with the live-workspace path may already have copied it
    // straight into allowedTargets/allowedTargetsConfirmed (legitimate
    // there, since a real developer's own already-configured build tree
    // backs it up when one exists). A candidate overlay has no such
    // pre-existing build tree; every declared target is re-confirmed here
    // against a REAL configure and `--target help` probe run fresh inside
    // the isolated candidate root, and declaredTargets is kept only as the
    // "requested" hint ManifestVerificationProfileBinder intersects against
    // this now-real allowedTargets catalog - never trusted on its own.
    if (snapshot.state == VerificationEnvironmentState::ExistingProfile) {
        std::size_t targetProbeCount = 0;
        for (auto& profile : snapshot.profiles) {
            if (!profile.buildPresetsConfirmed || stopToken.stop_requested()) continue;
            bool configured = false;
            for (auto& buildPreset : profile.buildPresets) {
                buildPreset.allowedTargetsConfirmed = false;
                buildPreset.allowedTargetsComplete = false;
                buildPreset.allowedTargets.clear();
                if (stopToken.stop_requested()
                    || targetProbeCount >= kVerificationEnvironmentMaxTargetProbes) {
                    continue;
                }
                if (!configured) {
                    configured = runCandidateConfigureProbe(
                        commandRunner, candidateRoot, profile.configurePresetName, stopToken);
                }
                if (!configured || profile.resolvedBinaryDir.empty()) continue;
                // The resolved build directory must be safely contained
                // inside the candidate overlay - never anywhere else.
                if (!configuredBuildTreeExists(candidateRoot, profile.resolvedBinaryDir)) continue;
                ++targetProbeCount;
                auto probe = runConfiguredTargetProbe(
                    commandRunner, candidateRoot, buildPreset.name, stopToken);
                buildPreset.allowedTargetsConfirmed = probe.confirmed;
                buildPreset.allowedTargetsComplete = probe.complete;
                buildPreset.allowedTargets = std::move(probe.targets);
            }
        }
    }

    return common::Result<VerificationEnvironmentSnapshot>::ok(std::move(snapshot));
}

} // namespace ai::application
