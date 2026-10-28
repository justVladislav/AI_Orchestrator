#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace ai::application::detail {

// std::filesystem::path's std::string constructor has no portable narrow
// encoding across platforms - on Windows it interprets the bytes using the
// current system ANSI codepage, not UTF-8. Text from JSON (model output via
// propose_change_set, e.g. relative_path/source_path) and from SQLite TEXT
// columns is always UTF-8, so constructing a path from that std::string
// directly silently corrupts any non-ASCII byte sequence (a Cyrillic
// "Тест/..." path ends up created on disk as "РўРµСЃС‚/..." - each UTF-8 byte
// pair misread as one CP1251 character). The char8_t/u8string constructor is
// the one std::filesystem::path overload the standard guarantees is UTF-8
// regardless of locale - use it for any path built from known-UTF-8 text.
inline std::filesystem::path pathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

// The form every filesystem call that may meet a long path should use. On Windows a plain path of 260 characters or more (and,
// for a directory, about 248) is refused by the Win32 file APIs unless the process opts in to long paths - which also depends on a
// Windows setting an application cannot rely on - so the call gets the extended-length form: absolute, normalised, preferred
// separators, "\\?\" prefix. It must be applied to std::filesystem calls AND to stream opens: a half-converted component fails on
// the call that was left out. Elsewhere it returns the path unchanged. (Results measured for ONE path structure are in
// REMAIN_TO_DO.md; this helper makes no promise about a "safe" length.)
inline std::filesystem::path longPath(const std::filesystem::path& path)
{
    if (path.empty()) return path;
#ifdef _WIN32
    std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
    absolute.make_preferred();
    const std::wstring value = absolute.native();
    if (value.rfind(LR"(\\?\)", 0) == 0) return absolute;
    if (value.rfind(LR"(\\)", 0) == 0) {
        return std::filesystem::path(std::wstring(LR"(\\?\UNC\)") + value.substr(2));
    }
    return std::filesystem::path(std::wstring(LR"(\\?\)") + value);
#else
    return path;
#endif
}

// The ordinary spelling of a path that may carry the extended-length prefix (canonical() of a long path may keep it). Containment
// compares path components, so a root without the prefix and a candidate with it would never match.
inline std::filesystem::path withoutLongPrefix(const std::filesystem::path& path)
{
#ifdef _WIN32
    const std::wstring value = path.native();
    if (value.rfind(LR"(\\?\UNC\)", 0) == 0) return std::filesystem::path(std::wstring(LR"(\\)") + value.substr(8));
    if (value.rfind(LR"(\\?\)", 0) == 0 && value.size() > 6 && value[5] == L':') return std::filesystem::path(value.substr(4));
    return path;
#else
    return path;
#endif
}

// The reverse direction: the UTF-8 text of a path in generic (forward-slash) form. path::string() and path::generic_string()
// convert through the system ANSI code page on Windows and THROW std::system_error ("No mapping for the Unicode character
// exists in the target multi-byte code page") for any character that code page lacks - a project folder named in Chinese on a
// Windows with a Russian or English ANSI page, or the reverse. Path COMPARISONS and keys must therefore use this, never
// string()/generic_string().
inline std::string genericUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Lower-cases ASCII letters only; every other byte (all bytes of a multi-byte UTF-8 character) is left as it is. The same
// "case-insensitive for ASCII, exact otherwise" rule the path checks always had, now independent of the process locale.
inline std::string asciiLower(std::string text)
{
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// Canonicalize every existing path component while allowing a future leaf
// (and its future parent directories) not to exist yet. Some Windows STL
// versions return ERROR_PATH_NOT_FOUND from weakly_canonical in that valid
// Create-file case, even though the operation is intended to append the
// missing suffix lexically.
inline std::filesystem::path canonicalizeWithMissingSuffix(
    const std::filesystem::path& input,
    std::error_code& error)
{
    namespace fs = std::filesystem;

    error.clear();
    fs::path existingPrefix = input;
    std::vector<fs::path> missingComponents;

    while (!existingPrefix.empty()) {
        std::error_code statusError;
        const fs::file_status status = fs::symlink_status(longPath(existingPrefix), statusError);
        if (!statusError && status.type() != fs::file_type::not_found)
            break;

        // Missing components are expected. Permission, device, and other I/O
        // errors must fail closed instead of being treated as a future path.
        if (statusError
            && statusError != std::errc::no_such_file_or_directory
            && statusError != std::errc::not_a_directory) {
            error = statusError;
            return {};
        }

        const fs::path component = existingPrefix.filename();
        const fs::path parent = existingPrefix.parent_path();
        if (component.empty() || parent == existingPrefix) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return {};
        }
        missingComponents.push_back(component);
        existingPrefix = parent;
    }

    if (existingPrefix.empty()) {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }

    fs::path resolved = withoutLongPrefix(fs::canonical(longPath(existingPrefix), error));
    if (error)
        return {};

    for (auto it = missingComponents.rbegin(); it != missingComponents.rend(); ++it)
        resolved /= *it;

    return resolved.lexically_normal();
}

// Canonical string key for correlating a relative path across independent
// producers/consumers that never share a single fs::path instance - e.g. a
// real workspace_hash_file tool result (AgentToolTurnLoopRunner, keyed by the
// executor's own relativeTo() output) versus a model-supplied relative_path
// on a ProposedFileChange (ChangeProposalValidator's hash-evidence check).
// Forward-slash generic form, case-folded on Windows to match that
// filesystem's default case-insensitive/case-preserving behaviour (NTFS) -
// the same convention FilesystemWorkspaceToolExecutor's pathLessCI already
// uses for directory-listing order. Never used for security/traversal
// decisions, only for evidence lookup: both sides must apply this exact
// function for a legitimate match to be found.
inline std::string normalizedPathKey(const std::filesystem::path& p)
{
    std::string s = genericUtf8(p);
#ifdef _WIN32
    s = asciiLower(std::move(s));
#endif
    return s;
}

} // namespace ai::application::detail
