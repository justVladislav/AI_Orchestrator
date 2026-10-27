#include "ai/application/WorkspaceReadPolicy.hpp"
#include "ai/application/FilesystemPathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

common::Error policyError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

std::string normalizedComponent(const fs::path& component)
{
    // UTF-8 text, never generic_string(): that one throws for a name the ANSI code page cannot hold.
    std::string value = detail::genericUtf8(component);
#ifdef _WIN32
    value = detail::asciiLower(std::move(value));
#endif
    return value;
}

bool componentEquals(const fs::path& lhs, const fs::path& rhs)
{
    return normalizedComponent(lhs) == normalizedComponent(rhs);
}

bool pathStartsWith(const fs::path& path, const fs::path& prefix)
{
    const fs::path normalizedPath = path.lexically_normal();
    const fs::path normalizedPrefix = prefix.lexically_normal();
    auto pathIt = normalizedPath.begin();
    for (auto prefixIt = normalizedPrefix.begin();
         prefixIt != normalizedPrefix.end();
         ++prefixIt, ++pathIt) {
        if (pathIt == normalizedPath.end()
            || !componentEquals(*pathIt, *prefixIt)) {
            return false;
        }
    }
    return true;
}

bool containsNul(const std::string& value)
{
    return value.find('\0') != std::string::npos;
}

bool isDriveOrUncQualified(const std::string& value)
{
    if (value.starts_with("\\\\") || value.starts_with("//")
        || value.starts_with("\\") || value.starts_with("/")) {
        return true;
    }
    return value.size() >= 2
        && std::isalpha(static_cast<unsigned char>(value[0]))
        && value[1] == ':';
}

bool isReservedWindowsDevice(const fs::path& component)
{
    std::string value = normalizedComponent(component);
    const auto dot = value.find('.');
    if (dot != std::string::npos) value.resize(dot);
    if (value == "con" || value == "prn" || value == "aux"
        || value == "nul" || value == "clock$") {
        return true;
    }
    if (value.size() == 4
        && (value.starts_with("com") || value.starts_with("lpt"))
        && value[3] >= '1' && value[3] <= '9') {
        return true;
    }
    return false;
}

bool stringListContains(
    const std::vector<std::string>& values,
    const std::string& candidate)
{
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
#ifdef _WIN32
        if (value.size() != candidate.size()) return false;
        return std::equal(
            value.begin(), value.end(), candidate.begin(),
            [](unsigned char lhs, unsigned char rhs) {
                return std::tolower(lhs) == std::tolower(rhs);
            });
#else
        return value == candidate;
#endif
    });
}

} // namespace

WorkspaceReadPolicy WorkspaceReadPolicy::secureDefaults(fs::path trustedWorkspaceRoot)
{
    WorkspaceReadPolicy policy;
    policy.workspaceRoot = std::move(trustedWorkspaceRoot);
    policy.deniedDirectoryNames = {
        ".git", ".ssh", ".aws", ".azure", ".gnupg",
        ".vs", // Visual Studio caches, indexes and local session state.
        "build", "out", "install", ".cache",
        // Real-Run incident (Run bb56e001-eac0-4b59-8d0b-d037c6bc1031,
        // 2026-09-15/17): a manual diff of the durable workspace-baseline
        // artifact against a fresh capture of the real live workspace,
        // days later with zero source edits, showed the ONLY difference
        // was inside .qtcreator/ - CMakeLists.txt.user (rewritten on every
        // Qt Creator session) and a qtc-cmake-presets-<random-hash>/
        // directory (a fresh random hash every time Qt Creator regenerates
        // its preset cache). This IDE housekeeping churn made MW-P5's
        // baseline-drift fail-closed gate permanently false-positive for
        // any Qt Creator user, even with the workspace genuinely
        // untouched - excluded the same way build/out/install already are.
        ".qtcreator"};
    policy.deniedDirectorySequences = {
        "x64/Debug", "x64/Release", "Win32/Debug", "Win32/Release"};
    policy.deniedFileNames = {
        ".env", ".env.local", "credentials", "credentials.json",
        "secrets.json", "id_rsa", "id_ed25519"};
    policy.deniedExtensions = {
        ".pem", ".key", ".pfx", ".p12", ".kdbx"};
    return policy;
}

common::Result<void> WorkspaceReadPolicyValidator::validateTrustedPolicy(
    const WorkspaceReadPolicy& policy)
{
    if (policy.workspaceRoot.empty() || !policy.workspaceRoot.is_absolute()) {
        return common::Result<void>::failure(policyError(
            "workspace_policy.invalid_root",
            "Workspace root must be a trusted absolute path"));
    }
    if (policy.maxFileBytes == 0 || policy.maxDirectoryEntries == 0
        || policy.maxSearchMatches == 0 || policy.maxResultBytes == 0) {
        return common::Result<void>::failure(policyError(
            "workspace_policy.invalid_limit",
            "Workspace read limits must be greater than zero"));
    }
    for (const auto& root : policy.allowedRoots) {
        if (root.empty() || root.is_absolute()
            || detail::genericUtf8(root.lexically_normal()).starts_with("..")) {
            return common::Result<void>::failure(policyError(
                "workspace_policy.invalid_allowed_root",
                "Allowed roots must be safe relative paths"));
        }
    }
    for (const auto& denied : policy.deniedPaths) {
        if (denied.empty() || denied.is_absolute()
            || detail::genericUtf8(denied.lexically_normal()).starts_with("..")) {
            return common::Result<void>::failure(policyError(
                "workspace_policy.invalid_denied_path",
                "Denied paths must be safe relative paths"));
        }
    }
    for (const auto& sequence : policy.deniedDirectorySequences) {
        if (sequence.empty() || sequence.is_absolute() || sequence.has_root_name()
            || std::any_of(sequence.begin(), sequence.end(), [](const fs::path& component) {
                   return component.empty() || component == "." || component == "..";
               })) {
            return common::Result<void>::failure(policyError(
                "workspace_policy.invalid_denied_path",
                "Denied directory sequences must contain safe relative components"));
        }
    }
    return common::Result<void>::ok();
}

bool WorkspaceReadPolicyValidator::matchesDeniedDirectorySequence(
    const fs::path& relativePath, const WorkspaceReadPolicy& policy)
{
    const auto normalized = relativePath.lexically_normal();
    for (auto start = normalized.begin(); start != normalized.end(); ++start) {
        for (const auto& sequence : policy.deniedDirectorySequences) {
            if (sequence.empty()) continue;
            auto candidate = start;
            auto part = sequence.begin();
            for (; part != sequence.end() && candidate != normalized.end(); ++part, ++candidate) {
                if (!componentEquals(*candidate, *part)) break;
            }
            if (part == sequence.end()) return true;
        }
    }
    return false;
}

common::Result<fs::path> WorkspaceReadPolicyValidator::validateRelativePath(
    const fs::path& modelPath,
    const WorkspaceReadPolicy& policy)
{
    const auto policyResult = validateTrustedPolicy(policy);
    if (!policyResult.success()) {
        return common::Result<fs::path>::failure(*policyResult.error());
    }

    const std::string raw = detail::genericUtf8(modelPath);
    if (raw.empty() || containsNul(raw) || isDriveOrUncQualified(raw)
        || modelPath.is_absolute() || raw.find(':') != std::string::npos) {
        return common::Result<fs::path>::failure(policyError(
            "workspace_path.unsafe",
            "Workspace path must be a safe relative path"));
    }

    const fs::path normalized = modelPath.lexically_normal();
    for (const auto& component : normalized) {
        const std::string value = normalizedComponent(component);
        if (value == ".." || value.empty()
            || isReservedWindowsDevice(component)) {
            return common::Result<fs::path>::failure(policyError(
                "workspace_path.unsafe",
                "Workspace path contains a prohibited component"));
        }
        if (stringListContains(policy.deniedDirectoryNames, value)) {
            return common::Result<fs::path>::failure(policyError(
                "workspace_path.denied",
                "Workspace path is denied by read policy"));
        }
    }

    const std::string filename = normalizedComponent(normalized.filename());
    if (matchesDeniedDirectorySequence(normalized, policy)
        || stringListContains(policy.deniedFileNames, filename)
        || stringListContains(
            policy.deniedExtensions,
            normalizedComponent(normalized.extension()))) {
        return common::Result<fs::path>::failure(policyError(
            "workspace_path.denied",
            "Workspace path is denied by read policy"));
    }

    if (!policy.allowedRoots.empty()) {
        const bool allowed = std::any_of(
            policy.allowedRoots.begin(), policy.allowedRoots.end(),
            [&](const fs::path& root) {
                if (!policy.allowedRootsAreExactPaths)
                    return pathStartsWith(normalized, root);
                return normalizedComponent(normalized)
                    == normalizedComponent(root.lexically_normal());
            });
        if (!allowed) {
            return common::Result<fs::path>::failure(policyError(
                "workspace_path.outside_allowed_roots",
                "Workspace path is outside allowed roots"));
        }
    }

    const bool denied = std::any_of(
        policy.deniedPaths.begin(), policy.deniedPaths.end(),
        [&](const fs::path& path) { return pathStartsWith(normalized, path); });
    if (denied) {
        return common::Result<fs::path>::failure(policyError(
            "workspace_path.denied",
            "Workspace path is denied by read policy"));
    }

    return common::Result<fs::path>::ok(normalized);
}

} // namespace ai::application
