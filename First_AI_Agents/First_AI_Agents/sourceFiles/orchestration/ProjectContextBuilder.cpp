#include "ai/orchestration/ProjectContextBuilder.hpp"

#include "ai/storage/FileStorage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ai::orchestration {

namespace {

constexpr std::uintmax_t maxContextFileSize = 512U * 1024U;

// std::filesystem::path's std::string constructor has no portable narrow
// encoding across platforms - on Windows it interprets the bytes using the
// current system ANSI codepage, not UTF-8. requestedPath here is UTF-8 text
// (a model/agent-supplied relative path), so constructing a path from it
// directly would silently corrupt any non-ASCII byte sequence. The
// char8_t/u8string constructor is the one std::filesystem::path overload
// the standard guarantees is UTF-8 regardless of locale.
std::filesystem::path pathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool equalsAny(const std::string& value, const std::initializer_list<const char*> names)
{
    return std::any_of(names.begin(), names.end(), [&value](const char* name) {
        return value == name;
    });
}

bool hasExcludedPathSegment(const std::filesystem::path& relativePath)
{
    for (const auto& part : relativePath) {
        const std::string name = toLower(part.string());
        if (equalsAny(name, {
                ".git",
                "build",
                "out",
                ".vs",
                ".idea",
                "workspace",
                "ai_test_outputs",
                "vcpkg_installed",
                "__pycache__"})) {
            return true;
        }
    }

    return false;
}

bool looksLikeLocalSecret(const std::filesystem::path& path)
{
    const std::string filename = toLower(path.filename().string());
    return filename == ".env"
        || filename.ends_with(".env")
        || filename.find("secret") != std::string::npos
        || filename.find("apikey") != std::string::npos
        || filename.find("api_key") != std::string::npos
        || filename.find("token") != std::string::npos;
}

bool isVendoredDependency(const std::filesystem::path& relativePath)
{
    auto it = relativePath.begin();
    if (it == relativePath.end()) {
        return false;
    }

    const std::string first = toLower(it->string());
    ++it;
    if (it == relativePath.end()) {
        return false;
    }

    const std::string second = toLower(it->string());
    return first == "includes" && second == "nlohmann";
}

bool isIncludedTextFile(const std::filesystem::path& path)
{
    const std::string filename = path.filename().string();
    if (filename == "CMakeLists.txt"
        || filename == "CMakePresets.json"
        || filename == "vcpkg.json"
        || filename == "README.md"
        || filename == "CODEX_CONTEXT.md") {
        return true;
    }

    const std::string extension = toLower(path.extension().string());
    return equalsAny(extension, {".cpp", ".hpp", ".h", ".c", ".cmake", ".json", ".md", ".txt"});
}

std::string toGenericRelativePath(const std::filesystem::path& path)
{
    return path.generic_string();
}

bool isSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }

    for (const auto& part : path) {
        if (part.string() == "..") {
            return false;
        }
    }

    return true;
}

std::string normalizeSelectedPath(const std::string& pathText)
{
    const std::filesystem::path path {pathText};
    if (!isSafeRelativePath(path)) {
        throw std::runtime_error("ProjectContextBuilder rejected unsafe selected path: " + pathText);
    }

    return path.lexically_normal().generic_string();
}

bool pathContainsPath(
    const std::filesystem::path& parent,
    const std::filesystem::path& child)
{
    const std::filesystem::path normalizedParent = parent.lexically_normal();
    const std::filesystem::path normalizedChild = child.lexically_normal();

    auto parentIt = normalizedParent.begin();
    auto childIt = normalizedChild.begin();

    for (; parentIt != normalizedParent.end(); ++parentIt, ++childIt) {
        if (childIt == normalizedChild.end() || *parentIt != *childIt) {
            return false;
        }
    }

    return true;
}

bool shouldIncludeFile(
    const std::filesystem::path& absolutePath,
    const std::filesystem::path& relativePath)
{
    if (hasExcludedPathSegment(relativePath)
        || looksLikeLocalSecret(relativePath)
        || isVendoredDependency(relativePath)
        || relativePath.filename() == "TECHNICAL_REPORT.md") {
        return false;
    }

    if (!isIncludedTextFile(relativePath)) {
        return false;
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(absolutePath, error);
    return !error && size <= maxContextFileSize;
}

std::filesystem::path canonicalPathOrThrow(
    const std::filesystem::path& path,
    const std::string& errorPrefix)
{
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) {
        throw std::runtime_error(errorPrefix + ": " + error.message());
    }

    return canonical;
}

std::string selectedFileRejectionReason(
    const std::filesystem::path& absolutePath,
    const std::filesystem::path& relativePath)
{
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(absolutePath, error);
    if (error) {
        return "cannot read file status: " + error.message();
    }

    if (!std::filesystem::is_regular_file(status)) {
        return "path is not a regular file";
    }

    if (hasExcludedPathSegment(relativePath)) {
        return "path contains an excluded directory segment";
    }

    if (looksLikeLocalSecret(relativePath)) {
        return "filename looks like a local secret";
    }

    if (isVendoredDependency(relativePath)) {
        return "path points to a vendored dependency";
    }

    if (relativePath.filename() == "TECHNICAL_REPORT.md") {
        return "TECHNICAL_REPORT.md is excluded from project context";
    }

    if (!isIncludedTextFile(relativePath)) {
        return "file type is not included in project context";
    }

    const std::uintmax_t size = std::filesystem::file_size(absolutePath, error);
    if (error) {
        return "cannot read file size: " + error.message();
    }

    if (size > maxContextFileSize) {
        std::ostringstream message;
        message << "file size " << size << " exceeds context limit " << maxContextFileSize;
        return message.str();
    }

    return {};
}

std::filesystem::path resolveSelectedFilePath(
    const std::filesystem::path& canonicalProjectRoot,
    const std::string& requestedPath)
{
    const std::string normalized = normalizeSelectedPath(requestedPath);
    const std::filesystem::path absolutePath = canonicalProjectRoot / pathFromUtf8(normalized);
    const std::filesystem::path canonicalSelected = canonicalPathOrThrow(
        absolutePath,
        "ProjectContextBuilder cannot resolve selected path '" + requestedPath + "'");

    if (!pathContainsPath(canonicalProjectRoot, canonicalSelected)) {
        throw std::runtime_error(
            "ProjectContextBuilder rejected selected path '" + requestedPath
            + "' because it resolves outside the project root");
    }

    return canonicalSelected;
}

void appendSelectedFile(
    ai::storage::FileStorage& storage,
    const std::filesystem::path& canonicalProjectRoot,
    ProjectContext& context,
    const std::string& role,
    const std::string& requestedPath)
{
    const std::filesystem::path absolutePath = resolveSelectedFilePath(
        canonicalProjectRoot,
        requestedPath);

    std::error_code error;
    const std::filesystem::path relativePath = std::filesystem::relative(
        absolutePath,
        canonicalProjectRoot,
        error);
    if (error) {
        throw std::runtime_error(
            "ProjectContextBuilder cannot compute project-relative path for selected file '"
            + requestedPath
            + "': "
            + error.message());
    }

    const std::string rejectionReason = selectedFileRejectionReason(absolutePath, relativePath);
    if (!rejectionReason.empty()) {
        throw std::runtime_error(
            "ProjectContextBuilder rejected selected file '"
            + requestedPath
            + "': "
            + rejectionReason);
    }

    const auto content = storage.readText(absolutePath);
    if (!content.has_value()) {
        throw std::runtime_error(
            "ProjectContextBuilder cannot read selected file '" + requestedPath + "'");
    }

    const std::string normalized = toGenericRelativePath(relativePath);
    context.files.push_back({normalized, *content});
    context.manifest += "[" + role + "] " + normalized + "\n";

    context.context += "===== " + role + " FILE: " + normalized + " =====\n\n";
    context.context += *content;
    context.context += "\n\n===== END FILE: " + normalized + " =====\n\n";
}

} // namespace

ProjectContextBuilder::ProjectContextBuilder(std::filesystem::path projectRoot)
    : projectRoot_(std::move(projectRoot))
{
}

ProjectContext ProjectContextBuilder::build() const
{
    if (projectRoot_.empty() || !std::filesystem::exists(projectRoot_)) {
        throw std::runtime_error("ProjectContextBuilder project root does not exist.");
    }

    ai::storage::FileStorage storage;
    std::vector<ProjectFileSnapshot> snapshots;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(projectRoot_)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), projectRoot_);
        if (!shouldIncludeFile(entry.path(), relativePath)) {
            continue;
        }

        const auto content = storage.readText(entry.path());
        if (!content.has_value()) {
            continue;
        }

        snapshots.push_back({
            toGenericRelativePath(relativePath),
            *content
        });
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
        return left.relativePath < right.relativePath;
    });

    std::ostringstream manifest;
    std::ostringstream context;

    for (const auto& snapshot : snapshots) {
        manifest << snapshot.relativePath << '\n';
        context << "===== FILE: " << snapshot.relativePath << " =====\n\n";
        context << snapshot.content << "\n\n";
        context << "===== END FILE: " << snapshot.relativePath << " =====\n\n";
    }

    return ProjectContext {
        std::move(snapshots),
        manifest.str(),
        context.str()
    };
}

ProjectContext ProjectContextBuilder::buildSelected(const ProjectContextSelection& selection) const
{
    if (projectRoot_.empty() || !std::filesystem::exists(projectRoot_)) {
        throw std::runtime_error("ProjectContextBuilder project root does not exist.");
    }

    ai::storage::FileStorage storage;
    ProjectContext context;
    const std::filesystem::path canonicalProjectRoot = canonicalPathOrThrow(
        projectRoot_,
        "ProjectContextBuilder cannot resolve project root");

    context.manifest += "Selected context: " + selection.title + "\n\n";
    context.context += "Selected context: " + selection.title + "\n";
    for (const auto& note : selection.notes) {
        context.context += "- " + note + "\n";
    }
    context.context += "\n";

    for (const auto& file : selection.editableFiles) {
        appendSelectedFile(storage, canonicalProjectRoot, context, "EDITABLE", file);
    }

    for (const auto& file : selection.readOnlyFiles) {
        appendSelectedFile(storage, canonicalProjectRoot, context, "READ-ONLY", file);
    }

    return context;
}

} // namespace ai::orchestration
