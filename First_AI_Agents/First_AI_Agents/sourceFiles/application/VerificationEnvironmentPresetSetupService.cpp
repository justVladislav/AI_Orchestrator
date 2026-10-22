#include "ai/application/VerificationEnvironmentPresetSetupService.hpp"

#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/WorkspacePathGuard.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace ai::application {

namespace {

common::Error makeError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

// Matches ExecutionAcceptanceContract.cpp's own `safeToken` convention -
// these preset names later flow into ExecutionVerificationArgument's own
// `preset`/`buildPreset` values and must pass that exact same check at
// Manifest admission, so generating a name that check would itself reject
// would be self-defeating. Additionally rejects a leading '.'/'-' (never
// legitimate for a preset name and avoids any ambiguity with a CLI flag if
// this string is ever echoed into a diagnostic).
bool safePresetName(std::string_view value)
{
    if (value.empty() || value.size() > 128) return false;
    if (value.front() == '.' || value.front() == '-') return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

// Candidate-relative, forward-slash-only binary directory (no leading
// "${sourceDir}/" - this service always adds that itself), adapted from
// ExecutionAcceptanceContract.cpp's own `safeRelativePath` charset rule.
bool safeRelativeBinaryDir(std::string_view value)
{
    if (value.empty() || value.size() > 256) return false;
    if (value.front() == '/' || value.back() == '/') return false;
    if (value.find('\\') != std::string_view::npos) return false;
    if (value.find(':') != std::string_view::npos) return false;
    if (value.find('$') != std::string_view::npos) return false; // no nested macros

    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(
            start, (end == std::string_view::npos ? value.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return false;
        const bool safePart = std::all_of(part.begin(), part.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
        });
        if (!safePart) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

// A bounded, control-character-free string - the first pass over
// qtPrefixPath before the real filesystem checks below.
bool safeFreeformField(std::string_view value, std::size_t maxLength)
{
    if (value.empty() || value.size() > maxLength) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

// A real, existence-checked Qt installation prefix - never merely a
// non-empty string. Rejects a relative path, a path that does not exist or
// is not a directory, one that cannot be resolved, and one that lacks its
// own `lib/cmake/Qt6/Qt6Config.cmake` - the exact file a real
// `find_package(Qt6 ...)` call against this prefix itself requires, so a
// prefix that passes this check is genuinely usable, not merely
// plausible-looking.
common::Result<void> validateQtPrefixPath(const std::string& qtPrefixPath)
{
    if (!safeFreeformField(qtPrefixPath, 1024)) {
        return common::Result<void>::failure(makeError(
            "verification_setup.missing_qt_prefix_path",
            "A Qt installation prefix path is required"));
    }
    const auto prefix = detail::pathFromUtf8(qtPrefixPath);
    if (!prefix.is_absolute()) {
        return common::Result<void>::failure(makeError(
            "verification_setup.qt_prefix_not_absolute",
            "Qt prefix path must be an absolute path"));
    }
    std::error_code statusError;
    if (!std::filesystem::exists(prefix, statusError) || statusError
        || !std::filesystem::is_directory(prefix, statusError) || statusError) {
        return common::Result<void>::failure(makeError(
            "verification_setup.qt_prefix_not_found",
            "Qt prefix path does not exist or is not a directory"));
    }
    std::error_code canonicalError;
    const auto canonicalPrefix = std::filesystem::weakly_canonical(prefix, canonicalError);
    if (canonicalError) {
        return common::Result<void>::failure(makeError(
            "verification_setup.qt_prefix_unavailable",
            "Qt prefix path could not be resolved"));
    }
    const auto qt6Config = canonicalPrefix / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake";
    std::error_code configError;
    if (!std::filesystem::is_regular_file(qt6Config, configError) || configError) {
        return common::Result<void>::failure(makeError(
            "verification_setup.qt_prefix_missing_cmake_package",
            "Qt prefix path does not contain a Qt6 CMake package "
            "(expected lib/cmake/Qt6/Qt6Config.cmake)"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateRequest(const CreateLocalVerificationPresetRequest& request)
{
    if (request.generator != "Ninja") {
        return common::Result<void>::failure(makeError(
            "verification_setup.unknown_generator",
            "Only the Ninja generator is currently supported"));
    }
    if (request.configuration != "Debug" && request.configuration != "Release") {
        return common::Result<void>::failure(makeError(
            "verification_setup.unknown_configuration",
            "Configuration must be Debug or Release"));
    }
    const auto qtPrefixValid = validateQtPrefixPath(request.qtPrefixPath);
    if (!qtPrefixValid.success()) {
        return qtPrefixValid;
    }
    if (!safePresetName(request.configurePresetName)) {
        return common::Result<void>::failure(makeError(
            "verification_setup.invalid_configure_preset_name",
            "Configure preset name must be a short, safe identifier"));
    }
    if (!safePresetName(request.buildPresetName)) {
        return common::Result<void>::failure(makeError(
            "verification_setup.invalid_build_preset_name",
            "Build preset name must be a short, safe identifier"));
    }
    if (!safeRelativeBinaryDir(request.binaryDirRelative)) {
        return common::Result<void>::failure(makeError(
            "verification_setup.invalid_binary_directory",
            "Binary directory must be a safe, candidate-relative path"));
    }
    return common::Result<void>::ok();
}

// A uniquely-named temp sibling in the SAME directory as targetPath, so the
// final rename stays on one volume and is atomic.
std::filesystem::path tempSiblingPath(const std::filesystem::path& targetPath)
{
    return targetPath.parent_path()
        / (targetPath.filename().string() + ".tmp-" + generateShortToken());
}

// Creates tempPath with true exclusive-create semantics (CREATE_NEW on
// Windows, O_CREAT|O_EXCL on POSIX) and writes content to it. Deliberately
// NOT a plain truncating std::ofstream open, which would silently succeed
// even if something else had already claimed that exact temp name between
// tempSiblingPath()'s random-token generation and this call - exclusive
// creation closes that window too, matching the same never-silently-
// overwrite discipline the final rename already applies to targetPath
// itself.
#ifndef _WIN32
common::Result<void> createTempFileExclusive(
    const std::filesystem::path& tempPath, const std::string& content)
{
    const int fd = ::open(tempPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        return common::Result<void>::failure(makeError(
            "verification_setup.write_failed", "Could not exclusively create a temporary file"));
    }
    const ssize_t written = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (written < 0 || static_cast<std::size_t>(written) != content.size()) {
        std::error_code cleanupError;
        std::filesystem::remove(tempPath, cleanupError);
        return common::Result<void>::failure(makeError(
            "verification_setup.write_failed", "Could not write the temporary file"));
    }
    return common::Result<void>::ok();
}
#endif

// Writes content to an exclusively-created temp sibling, then renames it
// onto targetPath with true exclusive-create semantics itself - the rename
// fails if targetPath already exists, closing the TOCTOU window between an
// earlier existence check and this call. std::filesystem::rename is NOT
// used here: on Windows its implementation calls MoveFileExW with
// MOVEFILE_REPLACE_EXISTING, which would silently overwrite an existing
// target - exactly what this contract forbids.
common::Result<void> writeFileExclusive(
    const std::filesystem::path& targetPath, const std::string& content)
{
    const auto tempPath = tempSiblingPath(targetPath);
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return common::Result<void>::failure(makeError(
            "verification_setup.write_failed", "Could not exclusively create a temporary file"));
    }
    bool writeOk = content.size() <= (std::numeric_limits<DWORD>::max)();
    DWORD written = 0;
    if (writeOk) {
        writeOk = ::WriteFile(handle, content.data(), static_cast<DWORD>(content.size()),
            &written, nullptr) && written == content.size() && ::FlushFileBuffers(handle);
    }
    if (!writeOk) {
        ::CloseHandle(handle);
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
        return common::Result<void>::failure(makeError(
            "verification_setup.write_failed", "Could not write the temporary file"));
    }

    ::CloseHandle(handle);
    const BOOL renamed = ::MoveFileExW(tempPath.c_str(), targetPath.c_str(), 0);
    const DWORD lastError = renamed ? ERROR_SUCCESS : ::GetLastError();
    if (!renamed) {
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
        std::error_code existsError;
        if (lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS
            || std::filesystem::exists(targetPath, existsError)) {
            return common::Result<void>::failure(makeError(
                "verification_setup.preset_already_exists",
                "A CMakeUserPresets.json already exists at this location"));
        }
        return common::Result<void>::failure(makeError(
            "verification_setup.write_failed", "Could not finalize the preset file"));
    }
#else
    const auto tempWritten = createTempFileExclusive(tempPath, content);
    if (!tempWritten.success()) {
        return tempWritten;
    }

    std::error_code linkError;
    std::filesystem::create_hard_link(tempPath, targetPath, linkError);
    if (linkError) {
        std::error_code cleanupError;
        std::filesystem::remove(tempPath, cleanupError);
        if (linkError == std::errc::file_exists) {
            return common::Result<void>::failure(makeError(
                "verification_setup.preset_already_exists",
                "A CMakeUserPresets.json already exists at this location"));
        }
        return common::Result<void>::failure(makeError(
            "verification_setup.write_failed", "Could not finalize the preset file"));
    }
    std::error_code removeTempError;
    std::filesystem::remove(tempPath, removeTempError);
#endif
    return common::Result<void>::ok();
}

} // namespace

common::Result<std::string> VerificationEnvironmentPresetSetupService::composePresetDocument(
    const CreateLocalVerificationPresetRequest& request)
{
    const auto validated = validateRequest(request);
    if (!validated.success()) {
        return common::Result<std::string>::failure(*validated.error());
    }

    nlohmann::ordered_json configurePreset;
    configurePreset["name"] = request.configurePresetName;
    configurePreset["displayName"] = "Local Qt " + request.configuration;
    configurePreset["generator"] = request.generator;
    configurePreset["binaryDir"] = "${sourceDir}/" + request.binaryDirRelative;
    nlohmann::ordered_json cacheVariables;
    cacheVariables["CMAKE_BUILD_TYPE"] = request.configuration;
    cacheVariables["CMAKE_PREFIX_PATH"] = request.qtPrefixPath;
    configurePreset["cacheVariables"] = std::move(cacheVariables);

    nlohmann::ordered_json buildPreset;
    buildPreset["name"] = request.buildPresetName;
    buildPreset["configurePreset"] = request.configurePresetName;

    nlohmann::ordered_json document;
    document["version"] = 6;
    document["configurePresets"] = nlohmann::ordered_json::array({std::move(configurePreset)});
    document["buildPresets"] = nlohmann::ordered_json::array({std::move(buildPreset)});

    return common::Result<std::string>::ok(document.dump(2) + "\n");
}

common::Result<std::filesystem::path> VerificationEnvironmentPresetSetupService::resolveTargetPath(
    const std::filesystem::path& projectRoot)
{
    std::error_code existsError;
    if (!std::filesystem::exists(projectRoot, existsError) || existsError) {
        return common::Result<std::filesystem::path>::failure(makeError(
            "verification_setup.project_root_not_found", "Project path does not exist"));
    }
    if (!std::filesystem::is_directory(projectRoot, existsError) || existsError) {
        return common::Result<std::filesystem::path>::failure(makeError(
            "verification_setup.project_root_not_directory",
            "Project path is not a directory"));
    }

    // The same canonical-containment plus Windows symlink/reparse-point
    // guard audited workspace tools already trust - CMakeUserPresets.json is
    // in neither of secureDefaults()'s deny lists, so only genuine
    // escape/reparse attempts are ever rejected here.
    const auto policy = WorkspaceReadPolicy::secureDefaults(projectRoot);
    const auto guarded = WorkspacePathGuard::validate("CMakeUserPresets.json", policy);
    if (!guarded.success()) {
        return common::Result<std::filesystem::path>::failure(*guarded.error());
    }
    return common::Result<std::filesystem::path>::ok(guarded.value());
}

common::Result<CreateLocalVerificationPresetResult>
VerificationEnvironmentPresetSetupService::createLocalPreset(
    const CreateLocalVerificationPresetRequest& request)
{
    const auto document = composePresetDocument(request);
    if (!document.success()) {
        return common::Result<CreateLocalVerificationPresetResult>::failure(*document.error());
    }

    const auto targetPath = resolveTargetPath(request.projectRoot);
    if (!targetPath.success()) {
        return common::Result<CreateLocalVerificationPresetResult>::failure(*targetPath.error());
    }

    // Friendlier early-exit before spending a temp-file write - the actual
    // safety boundary against a concurrent creator is the exclusive rename
    // inside writeFileExclusive(), not this check.
    std::error_code existsError;
    if (std::filesystem::exists(targetPath.value(), existsError)) {
        return common::Result<CreateLocalVerificationPresetResult>::failure(makeError(
            "verification_setup.preset_already_exists",
            "A CMakeUserPresets.json already exists at this location"));
    }

    const auto written = writeFileExclusive(targetPath.value(), document.value());
    if (!written.success()) {
        return common::Result<CreateLocalVerificationPresetResult>::failure(*written.error());
    }

    CreateLocalVerificationPresetResult result;
    result.writtenPath = targetPath.value();
    result.writtenJson = document.value();
    return common::Result<CreateLocalVerificationPresetResult>::ok(std::move(result));
}

common::Result<void> VerificationEnvironmentPresetSetupService::discardGeneratedPreset(
    const CreateLocalVerificationPresetResult& created)
{
    const auto& targetPath = created.writtenPath;
    if (targetPath.filename() != "CMakeUserPresets.json" || created.writtenJson.empty()) {
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_refused",
            "Refusing a compensation request without an exact generated-preset identity"));
    }
#ifdef _WIN32
    // Open the exact leaf without following a reparse point, verify and
    // delete through the same non-shared handle. This closes the otherwise
    // unavoidable verify-close-path-delete substitution window.
    const HANDLE handle = ::CreateFileW(targetPath.c_str(), GENERIC_READ | DELETE,
        0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return common::Result<void>::ok();
        }
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_failed", "Could not open the generated preset for compensation"));
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo {};
    if (!::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tagInfo, sizeof(tagInfo))
        || (tagInfo.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        ::CloseHandle(handle);
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_refused",
            "Refusing to remove a non-regular-file at the preset path"));
    }
    LARGE_INTEGER size {};
    const bool safeType = ::GetFileSizeEx(handle, &size)
        && size.QuadPart >= 0
        && static_cast<unsigned long long>(size.QuadPart) == created.writtenJson.size()
        && created.writtenJson.size() <= (std::numeric_limits<DWORD>::max)();
    std::string currentBytes(created.writtenJson.size(), '\0');
    DWORD bytesRead = 0;
    const bool sameBytes = safeType
        && ::ReadFile(handle, currentBytes.data(), static_cast<DWORD>(currentBytes.size()),
            &bytesRead, nullptr)
        && bytesRead == currentBytes.size()
        && currentBytes == created.writtenJson;
    if (!sameBytes) {
        ::CloseHandle(handle);
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_identity_conflict",
            "Generated preset changed before compensation; it was not removed"));
    }
    FILE_DISPOSITION_INFO disposition {TRUE};
    const BOOL markedForDeletion = ::SetFileInformationByHandle(
        handle, FileDispositionInfo, &disposition, sizeof(disposition));
    ::CloseHandle(handle);
    if (!markedForDeletion) {
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_failed", "Could not remove the generated preset file"));
    }
    return common::Result<void>::ok();
#else
    std::error_code existsError;
    if (!std::filesystem::exists(targetPath, existsError) || existsError) {
        return common::Result<void>::ok(); // already gone - nothing to compensate
    }
    std::error_code typeError;
    if (!std::filesystem::is_regular_file(targetPath, typeError) || typeError) {
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_refused",
            "Refusing to remove a non-regular-file at the preset path"));
    }
    std::error_code sizeError;
    const auto currentSize = std::filesystem::file_size(targetPath, sizeError);
    if (sizeError || currentSize != created.writtenJson.size()) {
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_identity_conflict",
            "Generated preset changed before compensation; it was not removed"));
    }
    std::ifstream current(targetPath, std::ios::binary);
    const std::string currentBytes((std::istreambuf_iterator<char>(current)),
        std::istreambuf_iterator<char>());
    if (!current || currentBytes != created.writtenJson) {
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_identity_conflict",
            "Generated preset changed before compensation; it was not removed"));
    }
    current.close();
    std::error_code removeError;
    std::filesystem::remove(targetPath, removeError);
    if (removeError) {
        return common::Result<void>::failure(makeError(
            "verification_setup.discard_failed", "Could not remove the generated preset file"));
    }
    return common::Result<void>::ok();
#endif
}

} // namespace ai::application
