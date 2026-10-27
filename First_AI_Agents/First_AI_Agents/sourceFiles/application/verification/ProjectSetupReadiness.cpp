#include "ai/application/verification/ProjectSetupReadiness.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/VerificationEnvironmentProbes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace ai::application {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxPresetFileBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxIncludedPresetFiles = 16;
constexpr int kMaxIncludeDepth = 4;
constexpr int kMaxInheritDepth = 8;
constexpr std::size_t kMaxCacheScanBytes = 8 * 1024 * 1024;

std::string bounded(std::string text)
{
    if (text.size() > kProjectSetupMaxMessageBytes) {
        text.resize(kProjectSetupMaxMessageBytes);
        // Do not leave a cut-off multi-byte character at the end.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xC0) text.pop_back();
    }
    return text;
}

std::string lowerAscii(std::string text)
{
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

bool containsText(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---- UTF-8 handling for process output ----

std::size_t utf8SequenceLength(const std::string& text, std::size_t at)
{
    const auto byte = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    if (byte < 0x80) return 1;
    if (byte >= 0xC2 && byte <= 0xDF) length = 2;
    else if (byte >= 0xE0 && byte <= 0xEF) length = 3;
    else if (byte >= 0xF0 && byte <= 0xF4) length = 4;
    else return 0;
    if (at + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i) {
        if ((static_cast<unsigned char>(text[at + i]) & 0xC0) != 0x80) return 0;
    }
    return length;
}

bool isValidUtf8(const std::string& text)
{
    for (std::size_t at = 0; at < text.size();) {
        const auto length = utf8SequenceLength(text, at);
        if (length == 0) return false;
        at += length;
    }
    return true;
}

std::string convertConsoleLineToUtf8(const std::string& line)
{
    if (isValidUtf8(line)) return line;
#ifdef _WIN32
    // cl.exe and link.exe write in the console's OEM code page (CP866 on a Russian system); cmake's own text is UTF-8.
    const int wideCount = MultiByteToWideChar(CP_OEMCP, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
    if (wideCount > 0) {
        std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, line.data(), static_cast<int>(line.size()), wide.data(), wideCount);
        const int utf8Count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount, nullptr, 0, nullptr, nullptr);
        if (utf8Count > 0) {
            std::string converted(static_cast<std::size_t>(utf8Count), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount, converted.data(), utf8Count, nullptr, nullptr);
            return converted;
        }
    }
#endif
    std::string replaced;
    for (const char ch : line) replaced.push_back(static_cast<unsigned char>(ch) < 0x80 ? ch : '?');
    return replaced;
}

// ---- preset files ----

std::optional<nlohmann::json> readJsonBounded(const fs::path& path)
{
    std::error_code error;
    const auto size = fs::file_size(detail::longPath(path), error);
    if (error || size > kMaxPresetFileBytes) return std::nullopt;
    std::ifstream stream(detail::longPath(path), std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    auto parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return parsed;
}

bool isInside(const fs::path& root, const fs::path& candidate)
{
    std::error_code error;
    const auto canonicalRoot = fs::weakly_canonical(root, error);
    if (error) return false;
    const auto canonicalCandidate = fs::weakly_canonical(candidate, error);
    if (error) return false;
    auto rootIt = canonicalRoot.begin();
    auto candidateIt = canonicalCandidate.begin();
    for (; rootIt != canonicalRoot.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == canonicalCandidate.end()) return false;
        if (detail::asciiLower(detail::genericUtf8(*rootIt)) != detail::asciiLower(detail::genericUtf8(*candidateIt))) {
            return false;
        }
    }
    return true;
}

void collectConfigurePresets(
    const fs::path& root,
    const fs::path& file,
    int depth,
    std::size_t& filesRead,
    std::map<std::string, nlohmann::json>& presets,
    std::vector<fs::path>* filesOut = nullptr)
{
    if (depth > kMaxIncludeDepth || filesRead >= kMaxIncludedPresetFiles || !isInside(root, file)) return;
    const auto document = readJsonBounded(file);
    if (!document.has_value()) return;
    ++filesRead;
    if (filesOut != nullptr) filesOut->push_back(file);
    // An included file is read first, so the including file's same-named preset wins (CMake's own precedence).
    if (document->contains("include") && (*document)["include"].is_array()) {
        for (const auto& entry : (*document)["include"]) {
            if (entry.is_string()) {
                collectConfigurePresets(root, file.parent_path() / detail::pathFromUtf8(entry.get<std::string>()),
                    depth + 1, filesRead, presets, filesOut);
            }
        }
    }
    if (document->contains("configurePresets") && (*document)["configurePresets"].is_array()) {
        for (const auto& item : (*document)["configurePresets"]) {
            if (item.is_object() && item.contains("name") && item["name"].is_string()) {
                presets[item["name"].get<std::string>()] = item;
            }
        }
    }
}

// A cache variable or an architecture is either a plain string or {"type": ..., "value": ...}.
std::optional<std::string> stringOrValue(const nlohmann::json& node)
{
    if (node.is_string()) return node.get<std::string>();
    if (node.is_object() && node.contains("value") && node["value"].is_string()) return node["value"].get<std::string>();
    return std::nullopt;
}

struct PresetFacts {
    std::string generator;
    std::string architecture;
    std::vector<std::string> otherText; // toolchain file and cache variable values
    std::vector<std::string> toolchainFiles; // as authored (may contain ${sourceDir})
};

void collectFacts(
    const std::map<std::string, nlohmann::json>& presets,
    const std::string& name,
    int depth,
    std::set<std::string>& visited,
    PresetFacts& facts)
{
    if (depth > kMaxInheritDepth || !visited.insert(name).second) return;
    const auto found = presets.find(name);
    if (found == presets.end()) return;
    const auto& preset = found->second;
    if (facts.generator.empty() && preset.contains("generator") && preset["generator"].is_string()) {
        facts.generator = preset["generator"].get<std::string>();
    }
    if (facts.architecture.empty() && preset.contains("architecture")) {
        if (const auto value = stringOrValue(preset["architecture"])) facts.architecture = *value;
    }
    if (preset.contains("toolchainFile") && preset["toolchainFile"].is_string()) {
        facts.otherText.push_back(preset["toolchainFile"].get<std::string>());
        facts.toolchainFiles.push_back(preset["toolchainFile"].get<std::string>());
    }
    if (preset.contains("cacheVariables") && preset["cacheVariables"].is_object()) {
        for (const auto& [key, value] : preset["cacheVariables"].items()) {
            if (const auto text = stringOrValue(value)) {
                facts.otherText.push_back(*text);
                if (key == "CMAKE_TOOLCHAIN_FILE") facts.toolchainFiles.push_back(*text);
            }
        }
    }
    if (preset.contains("inherits")) {
        const auto& inherits = preset["inherits"];
        if (inherits.is_string()) {
            collectFacts(presets, inherits.get<std::string>(), depth + 1, visited, facts);
        } else if (inherits.is_array()) {
            for (const auto& parent : inherits) {
                if (parent.is_string()) collectFacts(presets, parent.get<std::string>(), depth + 1, visited, facts);
            }
        }
    }
}

std::string unsupportedMessage(const std::string& preset, const std::string& what)
{
    return "The CMake preset \"" + preset + "\" is " + what
        + ", but the automatic verification setup supports MSVC x64 only (Visual Studio, 64-bit). Choose a preset that uses an "
          "MSVC 64-bit Qt kit, or verify this project by hand.";
}

} // namespace

std::string ProjectSetupReadiness::displayableTail(const std::string& processOutput, std::size_t maxBytes)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= processOutput.size()) {
        const auto end = processOutput.find('\n', start);
        std::string line = processOutput.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (end == std::string::npos && line.empty()) break;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        line = convertConsoleLineToUtf8(line);
        std::string clean;
        for (const char ch : line) {
            if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\t') clean.push_back(ch);
        }
        lines.push_back(std::move(clean));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::vector<std::string> kept;
    std::size_t total = 0;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::size_t cost = it->size() + (kept.empty() ? 0 : 1);
        if (total + cost > maxBytes) {
            if (kept.empty() && maxBytes > 0) {
                // One very long line: keep its end, without cutting a character in half.
                std::string piece = it->substr(it->size() - maxBytes);
                std::size_t skip = 0;
                while (skip < piece.size() && (static_cast<unsigned char>(piece[skip]) & 0xC0) == 0x80) ++skip;
                kept.push_back(piece.substr(skip));
            }
            break;
        }
        kept.push_back(*it);
        total += cost;
    }
    std::string result;
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
        if (!result.empty()) result.push_back('\n');
        result += *it;
    }
    return result;
}

std::vector<fs::path> ProjectSetupReadiness::presetInputFiles(const fs::path& projectRoot)
{
    std::map<std::string, nlohmann::json> presets;
    std::size_t filesRead = 0;
    std::vector<fs::path> files;
    collectConfigurePresets(projectRoot, projectRoot / "CMakePresets.json", 0, filesRead, presets, &files);
    collectConfigurePresets(projectRoot, projectRoot / "CMakeUserPresets.json", 0, filesRead, presets, &files);

    // Every configure preset's toolchain file, not only the selected one's: the fingerprint is taken before a pair is chosen.
    std::set<std::string> toolchainFiles;
    for (const auto& [name, preset] : presets) {
        PresetFacts facts;
        std::set<std::string> visited;
        collectFacts(presets, name, 0, visited, facts);
        toolchainFiles.insert(facts.toolchainFiles.begin(), facts.toolchainFiles.end());
    }
    constexpr std::string_view sourceDir = "${sourceDir}";
    for (const auto& authored : toolchainFiles) {
        std::string text = authored;
        if (text.rfind(sourceDir, 0) == 0) text = text.substr(sourceDir.size());
        if (text.find('$') != std::string::npos || text.empty()) continue; // another macro: not resolvable here
        while (!text.empty() && (text.front() == '/' || text.front() == '\\') && authored.rfind(sourceDir, 0) == 0) text.erase(0, 1);
        fs::path path = detail::pathFromUtf8(text);
        if (path.is_relative()) path = projectRoot / path;
        std::error_code error;
        if (fs::is_regular_file(detail::longPath(path), error)) files.push_back(path);
    }
    return files;
}

std::optional<std::string> ProjectSetupReadiness::unsupportedToolchainReason(
    const fs::path& projectRoot, const std::string& configurePreset)
{
    std::map<std::string, nlohmann::json> presets;
    std::size_t filesRead = 0;
    // CMakeUserPresets.json overrides CMakePresets.json, so it is collected second.
    collectConfigurePresets(projectRoot, projectRoot / "CMakePresets.json", 0, filesRead, presets);
    collectConfigurePresets(projectRoot, projectRoot / "CMakeUserPresets.json", 0, filesRead, presets);

    PresetFacts facts;
    std::set<std::string> visited;
    collectFacts(presets, configurePreset, 0, visited, facts);

    const auto generator = lowerAscii(facts.generator);
    if (containsText(generator, "mingw")) return unsupportedMessage(configurePreset, "a MinGW setup (generator \"" + facts.generator + "\")");
    const auto architecture = lowerAscii(facts.architecture);
    for (const char* unsupported : {"x86", "win32", "arm", "arm64", "arm64ec", "aarch64"}) {
        if (architecture == unsupported) {
            return unsupportedMessage(configurePreset, "set up for the \"" + facts.architecture + "\" architecture");
        }
    }
    for (const auto& text : facts.otherText) {
        const auto lowered = lowerAscii(text);
        if (containsText(lowered, "mingw")) return unsupportedMessage(configurePreset, "using a MinGW toolchain or Qt kit (\"" + text + "\")");
        if (containsText(lowered, "arm64") || containsText(lowered, "aarch64")) {
            return unsupportedMessage(configurePreset, "using an ARM toolchain or Qt kit (\"" + text + "\")");
        }
    }
    return std::nullopt;
}

std::optional<std::string> ProjectSetupReadiness::unsupportedCompilerReason(const fs::path& cacheFile)
{
    std::ifstream stream(detail::longPath(cacheFile), std::ios::binary);
    if (!stream) return std::nullopt;
    std::string cxx;
    std::string c;
    std::string line;
    std::size_t scanned = 0;
    while (std::getline(stream, line) && scanned < kMaxCacheScanBytes) {
        scanned += line.size() + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (line.rfind("CMAKE_CXX_COMPILER:", 0) == 0) cxx = line.substr(equals + 1);
        else if (line.rfind("CMAKE_C_COMPILER:", 0) == 0) c = line.substr(equals + 1);
    }
    const std::string compiler = !cxx.empty() ? cxx : c;
    if (compiler.empty()) return std::nullopt;
    std::string normalized = lowerAscii(compiler);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const auto slash = normalized.rfind('/');
    const std::string file = slash == std::string::npos ? normalized : normalized.substr(slash + 1);
    if (file != "cl.exe" && file != "cl") {
        return std::string("CMake chose the compiler \"") + compiler
            + "\", which is not MSVC. The automatic verification setup supports MSVC x64 only; choose a preset that uses "
              "an MSVC 64-bit Qt kit, or verify this project by hand.";
    }
    if (containsText(normalized, "/arm64/") || containsText(normalized, "/arm/") || containsText(normalized, "/x86/")) {
        return std::string("CMake chose the MSVC compiler \"") + compiler
            + "\", which is not the x64 one. The automatic verification setup supports MSVC x64 only.";
    }
    return std::nullopt;
}

common::Result<ProjectSetupSelection> ProjectSetupReadiness::chooseSelection(
    const VerificationEnvironmentSnapshot& catalog,
    const std::optional<ProjectSetupSelection>& saved,
    const std::function<std::optional<std::string>(const std::string&)>& unsupportedReason,
    std::vector<ProjectSetupSelection>* candidates)
{
    using Result = common::Result<ProjectSetupSelection>;
    if (candidates != nullptr) candidates->clear();

    if (saved.has_value()) {
        for (const auto& profile : catalog.profiles) {
            if (profile.configurePresetName != saved->configurePreset) continue;
            for (const auto& buildPreset : profile.buildPresets) {
                if (buildPreset.name == saved->buildPreset) return Result::ok(*saved);
            }
        }
        return Result::failure({kProjectSetupPresetNotFound,
            bounded("The selected presets \"" + saved->configurePreset + "\" / \"" + saved->buildPreset
                + "\" are no longer available in this project. Select a configure and build preset that exist in "
                  "CMakePresets.json or CMakeUserPresets.json, then check again.")});
    }

    std::vector<ProjectSetupSelection> all;
    for (const auto& profile : catalog.profiles) {
        for (const auto& buildPreset : profile.buildPresets) {
            all.push_back({profile.configurePresetName, buildPreset.name});
        }
    }
    if (all.empty()) {
        return Result::failure({kProjectSetupPresetNotFound,
            bounded("The project has no configure preset with a build preset that refers to it. Create the local verification "
                    "preset (New project > Verification setup) or add a build preset for your configure preset, then check "
                    "again.")});
    }
    std::vector<ProjectSetupSelection> supported;
    std::optional<std::string> firstReason;
    for (const auto& pair : all) {
        const auto reason = unsupportedReason ? unsupportedReason(pair.configurePreset) : std::nullopt;
        if (reason.has_value()) {
            if (!firstReason.has_value()) firstReason = reason;
        } else {
            supported.push_back(pair);
        }
    }
    if (supported.empty()) {
        return Result::failure({kProjectSetupToolchainUnsupported, bounded(*firstReason)});
    }
    if (supported.size() > 1) {
        if (candidates != nullptr) *candidates = supported;
        std::string list;
        for (const auto& pair : supported) list += "\n  - " + pair.configurePreset + " / " + pair.buildPreset;
        return Result::failure({kProjectSetupSelectionRequired,
            bounded("The project has several usable configure/build preset pairs and the application will not guess which "
                    "one to use:" + list + "\nSelect one for this project, then check again.")});
    }
    return Result::ok(supported.front());
}

ProjectSetupCheck ProjectSetupReadiness::check(
    const fs::path& projectRoot,
    const std::optional<ProjectSetupSelection>& saved,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken,
    const ToolchainProvider& toolchain)
{
    ProjectSetupCheck result;
    const auto fail = [&result](const char* code, std::string message) {
        result.ready = false;
        result.failureCode = code;
        result.failureMessage = bounded(std::move(message));
        return result;
    };
    const auto cancelled = [&] {
        return fail(kProjectSetupCancelled, "The check was cancelled before it finished. Check again when you are ready.");
    };
    if (stopToken.stop_requested()) return cancelled();

    const auto catalog = detail::probePresetCatalog(projectRoot, commandRunner, stopToken);
    if (stopToken.stop_requested()) return cancelled();
    if (catalog.state != VerificationEnvironmentState::ExistingProfile) {
        return fail(kProjectSetupPresetNotFound,
            (catalog.diagnostic.empty() ? std::string("No usable CMake preset was found in the project.") : catalog.diagnostic)
                + " Create the local verification preset (New project > Verification setup) or add a CMakePresets.json, then "
                  "check again.");
    }

    const auto chosen = chooseSelection(catalog, saved,
        [&projectRoot](const std::string& configurePreset) {
            return unsupportedToolchainReason(projectRoot, configurePreset);
        },
        &result.candidates);
    if (!chosen.success()) return fail(chosen.error()->code.c_str(), chosen.error()->message);
    result.selection = chosen.value();
    // A saved pair is still checked against the toolchain rule: a preset edited to MinGW after it was chosen is refused.
    if (const auto reason = unsupportedToolchainReason(projectRoot, result.selection.configurePreset)) {
        return fail(kProjectSetupToolchainUnsupported, *reason);
    }

    const VerificationProfile* selectedProfile = nullptr;
    for (const auto& profile : catalog.profiles) {
        if (profile.configurePresetName == result.selection.configurePreset) selectedProfile = &profile;
    }
    if (selectedProfile == nullptr || selectedProfile->resolvedBinaryDir.empty()) {
        // The Run's own discovery finds a build tree's targets only through a build directory it can resolve; a preset whose
        // directory it cannot would pass here and still fail after the Planner was paid.
        return fail(kProjectSetupBuildDirectoryUnresolvable,
            "The build directory of the preset \"" + result.selection.configurePreset
                + "\" cannot be determined (binaryDir is \"" + (selectedProfile ? selectedProfile->declaredBinaryDir : std::string())
                + "\"). Use a binaryDir under ${sourceDir}, for example \"${sourceDir}/build/${presetName}\", then check again.");
    }
    result.binaryDirRelative = selectedProfile->resolvedBinaryDir;

    const auto prepared = toolchain ? toolchain() : VerificationToolchain::current();
    if (!prepared.prepared) {
        return fail(kProjectSetupToolchainUnavailable, prepared.message + " [" + prepared.errorCode + "]");
    }
    result.toolchainIdentity = prepared.identity;

    ManifestVerificationCommandRequest configure;
    configure.commandIdentity = "cmake.setup.configure";
    configure.executable = "cmake";
    configure.arguments = {"--preset", result.selection.configurePreset};
    configure.workingDirectory = projectRoot;
    configure.timeout = std::chrono::seconds(180);
    configure.maxOutputBytes = 256 * 1024;
    const auto configured = commandRunner.run(configure, stopToken);
    if (configured.cancelled || stopToken.stop_requested()) return cancelled();
    if (!configured.started) {
        return fail(kProjectSetupConfigureFailed,
            "cmake could not be started (" + configured.errorCode
                + "). Install CMake and make sure it is on the PATH, then check again.");
    }
    if (configured.timedOut || !configured.exitCode.has_value() || *configured.exitCode != 0) {
        const std::string tail = displayableTail(configured.boundedOutput, 1100);
        std::string message = configured.timedOut
            ? "cmake --preset " + result.selection.configurePreset + " did not finish within 3 minutes."
            : "cmake --preset " + result.selection.configurePreset + " failed (exit code "
                + std::to_string(configured.exitCode.value_or(-1)) + ").";
        if (!tail.empty()) message += "\n" + tail;
        if (containsText(tail, "kernel32.lib") || containsText(tail, "LNK1104")) {
            message += "\nThe Windows SDK libraries could not be reached: install a Windows 10 or 11 SDK with the Visual Studio "
                       "Installer.";
        }
        message += "\nFix the cause above, then check again.";
        return fail(kProjectSetupConfigureFailed, std::move(message));
    }

    {
        const auto cache = projectRoot / detail::pathFromUtf8(result.binaryDirRelative) / "CMakeCache.txt";
        if (const auto reason = unsupportedCompilerReason(cache)) return fail(kProjectSetupToolchainUnsupported, *reason);
    }

    const auto targets = detail::probeConfiguredTargets(
        commandRunner, projectRoot, result.selection.buildPreset, stopToken);
    if (stopToken.stop_requested()) return cancelled();
    if (!targets.confirmed) {
        return fail(kProjectSetupTargetsUnconfirmed,
            "The project configured, but the build preset \"" + result.selection.buildPreset
                + "\" did not report its targets (cmake --build --preset " + result.selection.buildPreset
                + " --target help). Check that the build preset refers to the configure preset \""
                + result.selection.configurePreset + "\" and that a build tool (Ninja) is installed, then check again.");
    }
    result.targets = targets.targets;
    result.targetsComplete = targets.complete;
    result.ready = true;
    return result;
}

} // namespace ai::application
