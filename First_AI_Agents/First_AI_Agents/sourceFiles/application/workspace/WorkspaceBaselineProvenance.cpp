#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace ai::application {

namespace fs = std::filesystem;

namespace {

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

// Shared with pruneExcludedPrefixes() below so the two can never drift: a
// snapshot's fingerprint is always exactly this deterministic function of
// its (already sorted) file list.
std::string computeBaselineFingerprint(const std::vector<WorkspaceBaselineEntry>& sortedFiles)
{
    std::ostringstream material;
    for (const auto& entry : sortedFiles)
        material << entry.relativePath << '\0' << entry.byteSize << '\0' << entry.sha256 << '\n';
    return sha256Hex(material.str());
}

bool isReparsePoint(const fs::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(detail::longPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(detail::longPath(path), error));
#endif
}

std::string lower(std::string value)
{
    // ASCII-only fold, independent of the process locale (the same rule as before).
    return detail::asciiLower(std::move(value));
}

bool sameName(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

bool containsName(const std::vector<std::string>& values, std::string_view value)
{
    return std::any_of(values.begin(), values.end(), [&](const std::string& candidate) {
        return sameName(candidate, value);
    });
}

bool isWithin(const fs::path& candidate, const fs::path& parent)
{
    auto candidateIt = candidate.begin();
    for (auto parentIt = parent.begin(); parentIt != parent.end();
         ++parentIt, ++candidateIt) {
        if (candidateIt == candidate.end()) return false;
#ifdef _WIN32
        if (lower(detail::genericUtf8(*candidateIt))
            != lower(detail::genericUtf8(*parentIt))) return false;
#else
        if (*candidateIt != *parentIt) return false;
#endif
    }
    return true;
}

bool pathDenied(const fs::path& relative, const WorkspaceReadPolicy& policy)
{
    if (WorkspaceReadPolicyValidator::matchesDeniedDirectorySequence(relative, policy))
        return true;
    const auto normalized = relative.lexically_normal();
    for (const auto& denied : policy.deniedPaths) {
        const auto deniedNormalized = denied.lexically_normal();
        auto left = normalized.begin();
        bool prefix = true;
        for (auto right = deniedNormalized.begin(); right != deniedNormalized.end();
             ++right, ++left) {
            if (left == normalized.end()
                || !sameName(detail::genericUtf8(*left), detail::genericUtf8(*right))) {
                prefix = false;
                break;
            }
        }
        if (prefix) return true;
    }
    return false;
}

std::string fileLimitMessage(std::string_view relativePath,
    std::optional<std::uintmax_t> size, std::uint64_t limit,
    std::string_view limitName = "per-file limit")
{
    return "Policy-visible workspace file "
        + nlohmann::json(std::string(relativePath)).dump()
        + (size ? " is " + std::to_string(*size) + " bytes"
                : " has an unavailable size")
        + "; " + std::string(limitName) + " is " + std::to_string(limit) + " bytes";
}

common::Result<std::string> readBounded(
    const fs::path& path, std::uint64_t maxBytes, std::string_view errorPrefix,
    std::string_view diagnosticRelativePath = {})
{
    std::error_code error;
    const auto size = fs::file_size(detail::longPath(path), error);
    if (error || size > maxBytes) return fail<std::string>(
        std::string(errorPrefix) + ".file_limit",
        diagnosticRelativePath.empty()
            ? "Workspace file is unavailable or exceeds the configured byte limit"
            : fileLimitMessage(diagnosticRelativePath,
                  error ? std::nullopt : std::optional<std::uintmax_t>(size), maxBytes));
    std::ifstream stream(detail::longPath(path), std::ios::binary);
    if (!stream) return fail<std::string>(
        std::string(errorPrefix) + ".read_failed",
        "Workspace file could not be opened");
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!content.empty()) {
        stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    }
    if (!stream && !content.empty()) return fail<std::string>(
        std::string(errorPrefix) + ".read_failed",
        "Workspace file could not be read completely");
    return common::Result<std::string>::ok(std::move(content));
}

std::vector<std::string> exclusions(const WorkspaceReadPolicy& policy)
{
    std::vector<std::string> values;
    for (const auto& value : policy.deniedDirectoryNames)
        values.push_back("directory-name:" + value);
    for (const auto& value : policy.deniedDirectorySequences)
        values.push_back("directory-sequence:" + detail::genericUtf8(value));
    for (const auto& value : policy.deniedFileNames)
        values.push_back("file-name:" + value);
    for (const auto& value : policy.deniedExtensions)
        values.push_back("extension:" + value);
    for (const auto& value : policy.deniedPaths)
        values.push_back("path:" + detail::genericUtf8(value));
    std::sort(values.begin(), values.end());
    return values;
}

void readRepositoryRevision(
    const fs::path& root, WorkspaceBaselineSnapshot& snapshot)
{
    const auto git = root / ".git";
    std::error_code error;
    if (!fs::is_directory(detail::longPath(git), error) || error || isReparsePoint(git)) return;
    snapshot.repositoryRevisionAvailable = true;
    const auto canonicalGit = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(git), error));
    if (error) return;
    auto readGitFile = [&](const fs::path& relative, std::uint64_t maxBytes) {
        const auto unresolved = git / relative;
        const auto canonical = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(unresolved), error));
        if (error || !isWithin(canonical, canonicalGit) || isReparsePoint(unresolved)) {
            return fail<std::string>("workspace_baseline.repository_unsafe",
                "Repository revision input is unsafe");
        }
        return readBounded(canonical, maxBytes, "workspace_baseline.repository");
    };
    const auto head = readGitFile("HEAD", 512);
    if (!head.success()) return;
    std::string value = head.value();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    if (value.starts_with("ref: ")) {
        const auto reference = value.substr(5);
        if (reference.empty() || reference.find("..") != std::string::npos
            || reference.find('\\') != std::string::npos
            || reference.find(':') != std::string::npos
            || reference.front() == '/'
            || !std::all_of(reference.begin(), reference.end(), [](unsigned char ch) {
                   return std::isalnum(ch) || ch == '/' || ch == '_'
                       || ch == '-' || ch == '.';
               })) return;
        const auto resolved = readGitFile(fs::path(reference), 128);
        if (!resolved.success()) return;
        value = resolved.value();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
    }
    const bool hex = value.size() >= 40 && value.size() <= 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
    if (hex) snapshot.repositoryRevision = lower(std::move(value));
}

common::Result<std::string> readPolicyFile(
    const WorkspaceReadPolicy& policy, const fs::path& relative)
{
    const auto validated = WorkspaceReadPolicyValidator::validateRelativePath(
        relative, policy);
    if (!validated.success()) return fail<std::string>(
        "workspace_preflight.invalid_path",
        "Preflight configuration contains an unsafe workspace-relative path");
    std::error_code error;
    const auto root = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(policy.workspaceRoot), error));
    if (error) return fail<std::string>("workspace_preflight.invalid_root",
        "Workspace root could not be resolved");
    const auto target = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(root / validated.value()), error));
    if (error || !isWithin(target, root) || isReparsePoint(target)) {
        return fail<std::string>("workspace_preflight.path_escape",
            "Preflight input failed canonical containment verification");
    }
    return readBounded(target, policy.maxFileBytes, "workspace_preflight");
}

bool exactFields(
    const nlohmann::json& object, std::initializer_list<std::string_view> fields)
{
    if (!object.is_object() || object.size() != fields.size()) return false;
    return std::all_of(fields.begin(), fields.end(), [&](std::string_view field) {
        return object.contains(std::string(field));
    });
}

bool safeToken(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
           });
}

bool safeRelativeString(std::string_view value)
{
    if (value.empty() || value.size() > 4096 || value.front() == '/'
        || value.back() == '/' || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(start,
            (end == std::string_view::npos ? value.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

bool sha256String(std::string_view value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
           });
}

} // namespace

common::Result<WorkspaceBaselineSnapshot> WorkspaceBaselineProvenance::capture(
    const WorkspaceReadPolicy& policy, const WorkspaceBaselineLimits& limits)
{
    const auto policyResult = WorkspaceReadPolicyValidator::validateTrustedPolicy(policy);
    if (!policyResult.success()) return fail<WorkspaceBaselineSnapshot>(
        policyResult.error()->code, policyResult.error()->message);
    if (limits.maxFiles == 0 || limits.maxTotalBytes == 0
        || limits.maxManifestBytes == 0 || limits.maxChangedPaths == 0) {
        return fail<WorkspaceBaselineSnapshot>("workspace_baseline.invalid_limit",
            "Workspace baseline limits must be greater than zero");
    }

    std::error_code error;
    const auto root = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(policy.workspaceRoot), error));
    if (error || !fs::is_directory(detail::longPath(root), error) || error || isReparsePoint(root)) {
        return fail<WorkspaceBaselineSnapshot>("workspace_baseline.invalid_root",
            "Workspace root is unavailable or unsafe");
    }

    std::map<std::string, WorkspaceBaselineEntry> entries;
    const std::vector<fs::path> roots = policy.allowedRoots.empty()
        ? std::vector<fs::path> {fs::path {}} : policy.allowedRoots;
    std::uint64_t totalBytes = 0;
    for (const auto& allowed : roots) {
        const auto unresolvedStart = root / allowed;
        if (!fs::exists(detail::longPath(unresolvedStart), error) || error) {
            return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.invalid_allowed_root",
                "Configured allowed baseline root is unavailable");
        }
        const auto start = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(unresolvedStart), error));
        if (error || !isWithin(start, root) || isReparsePoint(unresolvedStart)
            || !fs::is_directory(detail::longPath(start), error) || error) {
            return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.invalid_allowed_root",
                "Allowed baseline root failed canonical containment verification");
        }
        // Both sides of every relative path in the extended-length form: the entries below inherit the prefix of `start`.
        const fs::path longRoot = detail::longPath(root);
        fs::recursive_directory_iterator iterator(
            detail::longPath(start), fs::directory_options::none, error), end;
        if (error) return fail<WorkspaceBaselineSnapshot>(
            "workspace_baseline.scan_failed",
            "Workspace baseline could not be enumerated");
        for (; iterator != end; iterator.increment(error)) {
            if (error) return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.scan_failed",
                "Workspace baseline could not be enumerated");
            const auto relative = iterator->path().lexically_relative(longRoot);
            const auto name = detail::genericUtf8(iterator->path().filename());
            const bool deniedByName = containsName(
                policy.deniedDirectoryNames, name)
                || containsName(policy.deniedFileNames, name)
                || containsName(policy.deniedExtensions,
                    detail::genericUtf8(iterator->path().extension()));
            const bool deniedByPath = pathDenied(relative, policy);
            if (deniedByName || deniedByPath) {
                if (iterator->is_directory(error)) iterator.disable_recursion_pending();
                if (error) return fail<WorkspaceBaselineSnapshot>(
                    "workspace_baseline.scan_failed",
                    "Workspace baseline entry type could not be inspected");
                continue;
            }
            if (isReparsePoint(iterator->path())) return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.reparse_point",
                "Policy-visible workspace contains a symlink or reparse point");
            const bool regular = iterator->is_regular_file(error);
            if (error) return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.scan_failed",
                "Workspace baseline entry type could not be inspected");
            if (!regular) continue;
            const auto relativePath = detail::genericUtf8(relative);
            const auto size = iterator->file_size(error);
            if (error || size > policy.maxFileBytes) {
                return fail<WorkspaceBaselineSnapshot>(
                    "workspace_baseline.file_limit",
                    fileLimitMessage(relativePath,
                        error ? std::nullopt : std::optional<std::uintmax_t>(size),
                        policy.maxFileBytes));
            }
            const auto remainingBytes = limits.maxTotalBytes - (std::min)(
                limits.maxTotalBytes, totalBytes);
            if (size > remainingBytes) {
                return fail<WorkspaceBaselineSnapshot>(
                    "workspace_baseline.file_limit",
                    fileLimitMessage(relativePath, size, remainingBytes,
                        "remaining baseline byte budget")
                        + " (total baseline limit: "
                        + std::to_string(limits.maxTotalBytes) + " bytes)");
            }
            const auto content = readBounded(
                iterator->path(), policy.maxFileBytes, "workspace_baseline", relativePath);
            if (!content.success()) return fail<WorkspaceBaselineSnapshot>(
                content.error()->code, content.error()->message);
            WorkspaceBaselineEntry entry;
            entry.relativePath = relativePath;
            entry.byteSize = static_cast<std::uint64_t>(content.value().size());
            entry.sha256 = sha256Hex(content.value());
            const auto [found, inserted] = entries.emplace(entry.relativePath, std::move(entry));
            if (inserted) totalBytes += static_cast<std::uint64_t>(content.value().size());
            if (entries.size() > limits.maxFiles || totalBytes > limits.maxTotalBytes) {
                return fail<WorkspaceBaselineSnapshot>(
                    "workspace_baseline.workspace_limit",
                    "Policy-visible workspace exceeds baseline count or byte limits");
            }
        }
    }

    WorkspaceBaselineSnapshot snapshot;
    snapshot.totalBytes = totalBytes;
    snapshot.exclusions = exclusions(policy);
    for (auto& [path, entry] : entries) snapshot.files.push_back(std::move(entry));
    snapshot.fingerprint = computeBaselineFingerprint(snapshot.files);
    readRepositoryRevision(root, snapshot);
    return common::Result<WorkspaceBaselineSnapshot>::ok(std::move(snapshot));
}

common::Result<WorkspaceBaselineDifference> WorkspaceBaselineProvenance::compare(
    const WorkspaceBaselineSnapshot& baseline,
    const WorkspaceBaselineSnapshot& current,
    const WorkspaceBaselineLimits& limits)
{
    std::map<std::string, WorkspaceBaselineEntry> before;
    std::map<std::string, WorkspaceBaselineEntry> after;
    for (const auto& entry : baseline.files) before.emplace(entry.relativePath, entry);
    for (const auto& entry : current.files) after.emplace(entry.relativePath, entry);

    WorkspaceBaselineDifference difference;
    difference.currentFingerprint = current.fingerprint;
    for (const auto& [path, entry] : before) {
        const auto found = after.find(path);
        if (found == after.end()) difference.deletedPaths.push_back(path);
        else if (found->second.byteSize != entry.byteSize
                 || found->second.sha256 != entry.sha256)
            difference.modifiedPaths.push_back(path);
    }
    for (const auto& [path, entry] : after) {
        static_cast<void>(entry);
        if (!before.contains(path)) difference.addedPaths.push_back(path);
    }
    const auto changed = difference.addedPaths.size()
        + difference.modifiedPaths.size() + difference.deletedPaths.size();
    if (changed > limits.maxChangedPaths) return fail<WorkspaceBaselineDifference>(
        "workspace_baseline.diff_limit",
        "Workspace drift exceeds the bounded changed-path limit");
    difference.classification = changed == 0
        ? "unchanged" : "manual_or_external_drift";
    return common::Result<WorkspaceBaselineDifference>::ok(std::move(difference));
}

common::Result<WorkspaceBaselineSnapshot> WorkspaceBaselineProvenance::pruneExcludedPrefixes(
    const WorkspaceBaselineSnapshot& snapshot,
    const std::vector<std::string>& excludedPrefixes)
{
    if (excludedPrefixes.empty()) {
        return fail<WorkspaceBaselineSnapshot>("workspace_baseline.invalid_prefix",
            "At least one excluded prefix is required");
    }
    for (const auto& prefix : excludedPrefixes) {
        if (prefix.empty() || prefix.front() == '/' || prefix.back() != '/'
            || prefix.find('\\') != std::string::npos || prefix.find(':') != std::string::npos
            || prefix.find("..") != std::string::npos) {
            return fail<WorkspaceBaselineSnapshot>("workspace_baseline.invalid_prefix",
                "An excluded baseline prefix is empty, unsafe, or not directory-shaped");
        }
    }
    WorkspaceBaselineSnapshot pruned;
    pruned.schemaVersion = snapshot.schemaVersion;
    pruned.exclusions = snapshot.exclusions;
    pruned.repositoryRevisionAvailable = snapshot.repositoryRevisionAvailable;
    pruned.repositoryRevision = snapshot.repositoryRevision;
    // snapshot.files is already sorted by relativePath (capture()'s own
    // invariant) - filtering preserves that order, so no re-sort is needed
    // before recomputing the fingerprint the same way capture() would.
    for (const auto& entry : snapshot.files) {
        const bool excluded = std::any_of(excludedPrefixes.begin(), excludedPrefixes.end(),
            [&](const std::string& prefix) { return entry.relativePath.starts_with(prefix); });
        if (excluded) continue;
        pruned.totalBytes += entry.byteSize;
        pruned.files.push_back(entry);
    }
    pruned.fingerprint = computeBaselineFingerprint(pruned.files);
    return common::Result<WorkspaceBaselineSnapshot>::ok(std::move(pruned));
}

common::Result<WorkspaceCMakePreflightResult>
WorkspaceBaselineProvenance::runConfiguredCMakePreflight(
    const WorkspaceReadPolicy& policy)
{
    const fs::path configPath = ".ai-orchestrator/preflight.json";
    std::error_code error;
    if (!fs::exists(detail::longPath(policy.workspaceRoot / configPath), error)) {
        return common::Result<WorkspaceCMakePreflightResult>::ok({});
    }
    if (error) return fail<WorkspaceCMakePreflightResult>(
        "workspace_preflight.config_unavailable",
        "Workspace preflight configuration could not be inspected");
    const auto raw = readPolicyFile(policy, configPath);
    if (!raw.success()) return fail<WorkspaceCMakePreflightResult>(
        raw.error()->code, raw.error()->message);

    const auto parsed = nlohmann::json::parse(raw.value(), nullptr, false);
    if (parsed.is_discarded() || !exactFields(
            parsed, {"schemaVersion", "cmakeTargetConsistency"})
        || !parsed["schemaVersion"].is_string()
        || parsed["schemaVersion"].get<std::string>() != kWorkspacePreflightSchemaVersion
        || !parsed["cmakeTargetConsistency"].is_array()
        || parsed["cmakeTargetConsistency"].size() > 16) {
        return fail<WorkspaceCMakePreflightResult>(
            "workspace_preflight.invalid_config",
            "Workspace preflight configuration is malformed or unsupported");
    }

    WorkspaceCMakePreflightResult result;
    result.configured = true;
    std::set<std::string> ids;
    for (const auto& check : parsed["cmakeTargetConsistency"]) {
        if (!exactFields(check, {"id", "definitionFile", "registrationFile", "target"})
            || !check["id"].is_string() || !check["definitionFile"].is_string()
            || !check["registrationFile"].is_string() || !check["target"].is_string()) {
            return fail<WorkspaceCMakePreflightResult>(
                "workspace_preflight.invalid_config",
                "Workspace preflight check has invalid fields");
        }
        const auto id = check["id"].get<std::string>();
        const auto definition = check["definitionFile"].get<std::string>();
        const auto registration = check["registrationFile"].get<std::string>();
        const auto target = check["target"].get<std::string>();
        if (!safeToken(id) || !safeToken(target) || !ids.insert(id).second) {
            return fail<WorkspaceCMakePreflightResult>(
                "workspace_preflight.invalid_config",
                "Workspace preflight identifiers are invalid or duplicated");
        }
        const auto left = readPolicyFile(policy, fs::path(definition));
        const auto right = readPolicyFile(policy, fs::path(registration));
        if (!left.success() || !right.success()) {
            return fail<WorkspaceCMakePreflightResult>(
                "workspace_preflight.input_unavailable",
                "Workspace preflight input could not be read safely");
        }
        if (left.value().find(target) == std::string::npos
            || right.value().find(target) == std::string::npos) {
            return fail<WorkspaceCMakePreflightResult>(
                "workspace_preflight.cmake_target_mismatch",
                "Configured CMake target is inconsistent across the selected files");
        }
        result.passedCheckIds.push_back(id);
    }
    return common::Result<WorkspaceCMakePreflightResult>::ok(std::move(result));
}

common::Result<std::string> WorkspaceBaselineProvenance::renderJson(
    const WorkspaceBaselineSnapshot& snapshot,
    const WorkspaceBaselineLimits& limits)
{
    nlohmann::json output = {
        {"schemaVersion", snapshot.schemaVersion},
        {"fingerprint", snapshot.fingerprint},
        {"fileCount", snapshot.files.size()},
        {"totalBytes", snapshot.totalBytes},
        {"repositoryRevisionAvailable", snapshot.repositoryRevisionAvailable},
        {"repositoryRevision", snapshot.repositoryRevision.has_value()
                ? nlohmann::json(*snapshot.repositoryRevision) : nlohmann::json(nullptr)},
        {"exclusions", snapshot.exclusions},
        {"files", nlohmann::json::array()}
    };
    for (const auto& entry : snapshot.files) {
        output["files"].push_back({
            {"path", entry.relativePath},
            {"byteSize", entry.byteSize},
            {"sha256", entry.sha256}
        });
    }
    auto rendered = output.dump(2);
    if (rendered.size() > limits.maxManifestBytes) return fail<std::string>(
        "workspace_baseline.manifest_limit",
        "Workspace baseline manifest exceeds its serialized byte limit");
    return common::Result<std::string>::ok(std::move(rendered));
}

common::Result<WorkspaceBaselineSnapshot> WorkspaceBaselineProvenance::parseJson(
    std::string_view json, const WorkspaceBaselineLimits& limits)
{
    if (json.empty() || json.size() > limits.maxManifestBytes) {
        return fail<WorkspaceBaselineSnapshot>("workspace_baseline.manifest_limit",
            "Workspace baseline manifest exceeds its serialized byte limit");
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded()
        || !exactFields(parsed, {"schemaVersion", "fingerprint", "fileCount",
               "totalBytes", "repositoryRevisionAvailable", "repositoryRevision",
               "exclusions", "files"})
        || !parsed["schemaVersion"].is_string()
        || parsed["schemaVersion"].get<std::string>() != kWorkspaceBaselineSchemaVersion
        || !parsed["fingerprint"].is_string()
        || !parsed["fileCount"].is_number_unsigned()
        || !parsed["totalBytes"].is_number_unsigned()
        || !parsed["repositoryRevisionAvailable"].is_boolean()
        || !(parsed["repositoryRevision"].is_null()
             || parsed["repositoryRevision"].is_string())
        || !parsed["exclusions"].is_array() || !parsed["files"].is_array()) {
        return fail<WorkspaceBaselineSnapshot>("workspace_baseline.invalid_manifest",
            "Workspace baseline manifest is malformed or unsupported");
    }
    if (parsed["files"].size() > limits.maxFiles
        || parsed["exclusions"].size() > 128
        || parsed["fileCount"].get<std::uint64_t>() != parsed["files"].size()) {
        return fail<WorkspaceBaselineSnapshot>("workspace_baseline.invalid_manifest",
            "Workspace baseline manifest count is invalid");
    }

    WorkspaceBaselineSnapshot snapshot;
    snapshot.fingerprint = parsed["fingerprint"].get<std::string>();
    snapshot.repositoryRevisionAvailable =
        parsed["repositoryRevisionAvailable"].get<bool>();
    if (parsed["repositoryRevision"].is_string()) {
        snapshot.repositoryRevision = parsed["repositoryRevision"].get<std::string>();
        if (!sha256String(*snapshot.repositoryRevision)
            && !(snapshot.repositoryRevision->size() == 40
                 && std::all_of(snapshot.repositoryRevision->begin(),
                     snapshot.repositoryRevision->end(), [](unsigned char ch) {
                         return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
                     }))) {
            return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.invalid_manifest",
                "Workspace baseline repository revision is invalid");
        }
    }
    if (!sha256String(snapshot.fingerprint)) return fail<WorkspaceBaselineSnapshot>(
        "workspace_baseline.invalid_manifest",
        "Workspace baseline fingerprint is invalid");
    for (const auto& exclusion : parsed["exclusions"]) {
        if (!exclusion.is_string()) return fail<WorkspaceBaselineSnapshot>(
            "workspace_baseline.invalid_manifest",
            "Workspace baseline exclusion is invalid");
        auto value = exclusion.get<std::string>();
        if (value.empty() || value.size() > 4096) return fail<WorkspaceBaselineSnapshot>(
            "workspace_baseline.invalid_manifest",
            "Workspace baseline exclusion is invalid");
        snapshot.exclusions.push_back(std::move(value));
    }

    std::ostringstream material;
    std::string previousPath;
    std::uint64_t totalBytes = 0;
    for (const auto& item : parsed["files"]) {
        if (!exactFields(item, {"path", "byteSize", "sha256"})
            || !item["path"].is_string() || !item["byteSize"].is_number_unsigned()
            || !item["sha256"].is_string()) {
            return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.invalid_manifest",
                "Workspace baseline file entry is invalid");
        }
        WorkspaceBaselineEntry entry;
        entry.relativePath = item["path"].get<std::string>();
        entry.byteSize = item["byteSize"].get<std::uint64_t>();
        entry.sha256 = item["sha256"].get<std::string>();
        if (!safeRelativeString(entry.relativePath) || !sha256String(entry.sha256)
            || (!previousPath.empty() && previousPath >= entry.relativePath)
            || entry.byteSize > limits.maxTotalBytes - (std::min)(
                limits.maxTotalBytes, totalBytes)) {
            return fail<WorkspaceBaselineSnapshot>(
                "workspace_baseline.invalid_manifest",
                "Workspace baseline file entry is unsafe or out of order");
        }
        previousPath = entry.relativePath;
        totalBytes += entry.byteSize;
        material << entry.relativePath << '\0' << entry.byteSize << '\0'
                 << entry.sha256 << '\n';
        snapshot.files.push_back(std::move(entry));
    }
    snapshot.totalBytes = totalBytes;
    if (parsed["totalBytes"].get<std::uint64_t>() != totalBytes
        || sha256Hex(material.str()) != snapshot.fingerprint) {
        return fail<WorkspaceBaselineSnapshot>(
            "workspace_baseline.identity_mismatch",
            "Workspace baseline manifest identity does not match its entries");
    }
    return common::Result<WorkspaceBaselineSnapshot>::ok(std::move(snapshot));
}

std::string WorkspaceBaselineProvenance::renderDiscoveryCatalog(
    const WorkspaceBaselineSnapshot& snapshot, std::size_t maxEntries, std::size_t maxBytes)
{
    std::ostringstream out;
    out << "# Workspace discovery catalog (" << snapshot.files.size()
        << " files, " << snapshot.totalBytes << " bytes total, path/extension/size/hash)\n";
    std::string rendered = out.str();
    if (rendered.size() > maxBytes) return {};

    std::size_t included = 0;
    for (const auto& entry : snapshot.files) {
        if (included >= maxEntries) break;
        const auto dot = entry.relativePath.find_last_of('.');
        const auto slash = entry.relativePath.find_last_of('/');
        const std::string extension = (dot != std::string::npos
                && (slash == std::string::npos || dot > slash))
            ? entry.relativePath.substr(dot)
            : std::string();
        std::string line = entry.relativePath + "\t" + extension + "\t"
            + std::to_string(entry.byteSize) + "\t" + entry.sha256 + "\n";
        if (rendered.size() + line.size() > maxBytes) break;
        rendered += line;
        ++included;
    }
    if (included < snapshot.files.size()) {
        auto marker = [&] {
            return "# Catalog truncated: included " + std::to_string(included)
                + " of " + std::to_string(snapshot.files.size())
                + " files; use inspection tools for omitted paths.\n";
        };
        std::string truncation = marker();
        while (rendered.size() + truncation.size() > maxBytes && included > 0) {
            const auto previousNewline = rendered.rfind('\n', rendered.size() - 2);
            if (previousNewline == std::string::npos) break;
            rendered.resize(previousNewline + 1);
            --included;
            truncation = marker();
        }
        if (rendered.size() + truncation.size() <= maxBytes)
            rendered += truncation;
    }
    return rendered;
}

} // namespace ai::application
