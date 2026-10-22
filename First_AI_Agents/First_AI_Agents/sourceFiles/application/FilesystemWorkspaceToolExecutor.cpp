#include "ai/application/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/WorkspacePathGuard.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ai::application {

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ─── AgentToolResult construction helpers ────────────────────────────────────

AgentToolResult makeOk(const AgentToolCall& call,
                        std::string          contentJson,
                        bool                 truncated = false)
{
    AgentToolResult r;
    r.callId        = call.callId;
    r.toolName      = call.toolName;
    r.success       = true;
    r.truncated     = truncated;
    r.returnedBytes = contentJson.size();
    r.contentJson   = std::move(contentJson);
    return r;
}

AgentToolResult makeFail(const AgentToolCall& call,
                          std::string          code)
{
    AgentToolResult r;
    r.callId    = call.callId;
    r.toolName  = call.toolName;
    r.success   = false;
    r.errorCode = std::move(code);
    return r;
}

// ─── Strict JSON argument parsing ────────────────────────────────────────────

// Returns the parsed JSON object, or throws std::runtime_error on any violation:
//   - not a JSON object
//   - unknown key (not in allowedKeys)
//   - value not a JSON string
//   - required key absent or empty
// The caller must wrap this in a try/catch.
struct ToolArgs {
    std::string path;   // always present after successful parse
    std::string query;  // workspace_search_text only
    std::string glob;   // workspace_find_files only
};

ToolArgs parseArgs(const std::string& argumentsJson,
                   const std::set<std::string>& requiredKeys,
                   const std::set<std::string>& optionalKeys)
{
    const json doc = json::parse(argumentsJson); // throws on bad JSON
    if (!doc.is_object()) throw std::runtime_error("arguments must be a JSON object");

    // Reject unknown keys.
    const std::set<std::string> allAllowed = [&]{
        std::set<std::string> s = requiredKeys;
        s.insert(optionalKeys.begin(), optionalKeys.end());
        return s;
    }();
    for (const auto& [k, v] : doc.items()) {
        if (!allAllowed.count(k))
            throw std::runtime_error("unknown argument key");
        if (!v.is_string())
            throw std::runtime_error("argument value must be a string");
    }

    // Check required keys are present and non-empty.
    ToolArgs args;
    for (const auto& k : requiredKeys) {
        if (!doc.contains(k))
            throw std::runtime_error("missing required argument");
        const std::string val = doc[k].get<std::string>();
        if (val.empty())
            throw std::runtime_error("required argument must not be empty");
        if (k == "path")  args.path  = val;
        if (k == "query") args.query = val;
        if (k == "glob")  args.glob  = val;
    }
    return args;
}

// ─── Security utilities ───────────────────────────────────────────────────────

// Returns true if the byte buffer begins with a NUL byte anywhere in the
// first probeSizeBytes — a reliable binary-file indicator.
bool isBinary(const char* data, std::size_t len)
{
    const std::size_t probe = std::min(len, std::size_t{512});
    for (std::size_t i = 0; i < probe; ++i)
        if (data[i] == '\0') return true;
    return false;
}

// Returns true if data is well-formed UTF-8.
bool isValidUtf8(const char* data, std::size_t len)
{
    for (std::size_t i = 0; i < len; ) {
        const auto b = static_cast<unsigned char>(data[i]);
        int extra = 0;
        if      (b < 0x80)             { ++i; continue; }
        else if ((b & 0xE0) == 0xC0)  extra = 1;
        else if ((b & 0xF0) == 0xE0)  extra = 2;
        else if ((b & 0xF8) == 0xF0)  extra = 3;
        else return false;
        if (i + static_cast<std::size_t>(extra) >= len) return false;
        for (int j = 1; j <= extra; ++j) {
            if ((static_cast<unsigned char>(data[i + j]) & 0xC0) != 0x80)
                return false;
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

// Length of the longest contiguous run of base64-alphabet characters in
// body, ignoring line breaks (real PEM bodies wrap at 64 chars). Used to
// tell an actual exported key apart from a shortened placeholder like
// "MIIEowIB..." (real base64 key material can never contain '.', since it
// isn't in the base64 alphabet - inserting one to dodge this check would
// corrupt the key itself).
std::size_t longestBase64Run(std::string_view body)
{
    std::size_t best = 0;
    std::size_t current = 0;
    for (const char c : body) {
        if (c == '\n' || c == '\r') continue;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=') {
            ++current;
            best = std::max(best, current);
        } else {
            current = 0;
        }
    }
    return best;
}

// Length of the contiguous run of chars satisfying isAllowed starting at
// content[start]. Used to require a real credential-shaped suffix after a
// token prefix, instead of flagging the bare prefix wherever it appears -
// e.g. inside a regex literal (`ghp_[a-zA-Z0-9]{36}`) or documentation
// prose naming the prefix itself.
template <typename Predicate>
std::size_t runLength(std::string_view content, std::size_t start, Predicate isAllowed)
{
    std::size_t n = 0;
    while (start + n < content.size() && isAllowed(content[start + n]))
        ++n;
    return n;
}

// Returns true if content contains a known secret pattern.
// Prefixes are chosen to be specific enough to avoid common false positives.
bool containsSecret(std::string_view content)
{
    // PEM private key / certificate request markers. Require a real
    // base64-shaped body, not just the header text, so this file's own
    // documentation/tests can mention or use a truncated example key
    // without tripping the scanner.
    static constexpr std::array<std::string_view, 7> PEM = {
        "-----BEGIN RSA PRIVATE KEY-----",
        "-----BEGIN EC PRIVATE KEY-----",
        "-----BEGIN DSA PRIVATE KEY-----",
        "-----BEGIN PRIVATE KEY-----",
        "-----BEGIN OPENSSH PRIVATE KEY-----",
        "-----BEGIN ENCRYPTED PRIVATE KEY-----",
        "-----BEGIN CERTIFICATE REQUEST-----",
    };
    static constexpr std::size_t kMinPemBodyRun = 40;
    for (const auto& p : PEM) {
        const auto pos = content.find(p);
        if (pos == std::string_view::npos) continue;
        const auto bodyStart = pos + p.size();
        const auto bodyEnd = content.find("-----END", bodyStart);
        const std::string_view body = content.substr(
            bodyStart, bodyEnd == std::string_view::npos ? std::string_view::npos : bodyEnd - bodyStart);
        if (longestBase64Run(body) >= kMinPemBodyRun) return true;
    }

    // Common token prefixes that are long and unambiguous enough, but only
    // once followed by an actual token-shaped suffix - real tokens are far
    // longer than this minimum, while a bare mention of the prefix (in a
    // regex pattern, comment, or doc) is immediately followed by a
    // non-token character.
    static constexpr std::array<std::string_view, 6> TOKENS = {
        "ghp_",         // GitHub personal access token
        "gho_",         // GitHub OAuth token
        "github_pat_",  // GitHub fine-grained PAT
        "glpat-",       // GitLab PAT
        "xoxb-",        // Slack bot token
        "xoxp-",        // Slack user token
    };
    static constexpr std::size_t kMinTokenBodyLength = 20;
    const auto isTokenBodyChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    };
    for (const auto& t : TOKENS) {
        auto pos = content.find(t);
        while (pos != std::string_view::npos) {
            if (runLength(content, pos + t.size(), isTokenBodyChar) >= kMinTokenBodyLength)
                return true;
            pos = content.find(t, pos + 1);
        }
    }

    // AWS access key: AKIA followed by exactly 16 uppercase-alnum chars.
    // A suffix containing "EXAMPLE" is AWS's own well-known documentation
    // placeholder (e.g. AKIAIOSFODNN7EXAMPLE), not a real credential.
    auto pos = content.find("AKIA");
    while (pos != std::string_view::npos) {
        if (pos + 20 <= content.size()) {
            const std::string_view suffix = content.substr(pos + 4, 16);
            const bool shaped = std::all_of(suffix.begin(), suffix.end(), [](char c) {
                return std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c));
            });
            if (shaped && suffix.find("EXAMPLE") == std::string_view::npos) return true;
        }
        pos = content.find("AKIA", pos + 1);
    }
    return false;
}

// ─── Path helpers ─────────────────────────────────────────────────────────────

// Returns the path relative to workspace root as a forward-slash string.
// Both paths must be canonical and absolute.
std::string relativeTo(const fs::path& canonical, const fs::path& root)
{
    return detail::genericUtf8(canonical.lexically_relative(root));
}

// ─── File I/O ────────────────────────────────────────────────────────────────

// Reads up to maxBytes bytes from the file at path.
// Returns empty string on I/O error (caller checks std::error_code).
std::string readBytes(const fs::path& path, std::size_t maxBytes,
                      bool& truncated, std::error_code& ec)
{
    truncated = false;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) { ec = std::make_error_code(std::errc::io_error); return {}; }

    std::error_code szEc;
    const auto fileSize = fs::file_size(path, szEc);
    if (!szEc && fileSize > maxBytes) truncated = true;

    std::string buf;
    buf.resize(maxBytes);
    ifs.read(buf.data(), static_cast<std::streamsize>(maxBytes));
    buf.resize(static_cast<std::size_t>(ifs.gcount()));
    return buf;
}

// ─── Timestamp ───────────────────────────────────────────────────────────────

// Returns the last-write time as seconds since Unix epoch (integer string).
std::string formatMtime(const fs::path& path, std::error_code& ec)
{
    const auto ftime = fs::last_write_time(path, ec);
    if (ec) return "0";
    // MSVC's _File_time_clock::to_sys() was not available in older toolsets;
    // compute the epoch delta at runtime via the two clocks' now() values.
    const auto fileDelta   = std::chrono::file_clock::now().time_since_epoch();
    const auto systemDelta = std::chrono::system_clock::now().time_since_epoch();
    const auto offset      = systemDelta - fileDelta;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          ftime.time_since_epoch() + offset).count();
    return std::to_string(secs);
}

// ─── Glob matching ────────────────────────────────────────────────────────────

// Matches a portable glob. '*' and '?' do not cross a path separator; '**'
// does. A '**/' segment may also match zero directories, so '**/*.qml'
// includes both top-level and nested QML files. Case-insensitive on Windows.
bool matchGlob(std::string pattern, std::string candidate)
{
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
    std::replace(candidate.begin(), candidate.end(), '\\', '/');
#ifdef _WIN32
    std::transform(pattern.begin(), pattern.end(), pattern.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::transform(candidate.begin(), candidate.end(), candidate.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#endif

    const std::size_t columns = candidate.size() + 1;
    std::vector<signed char> memo((pattern.size() + 1) * columns, -1);
    const auto matches = [&](auto&& self, std::size_t pi, std::size_t ci) -> bool {
        auto& cached = memo[pi * columns + ci];
        if (cached != -1) return cached == 1;

        bool result = false;
        if (pi == pattern.size()) {
            result = ci == candidate.size();
        } else if (pattern[pi] == '*') {
            const bool doubleStar = pi + 1 < pattern.size() && pattern[pi + 1] == '*';
            if (doubleStar && pi + 2 < pattern.size() && pattern[pi + 2] == '/') {
                // Zero directory segments, or one/more complete segments.
                result = self(self, pi + 3, ci);
                for (std::size_t separator = ci;
                     !result && separator < candidate.size(); ++separator) {
                    if (candidate[separator] == '/') {
                        result = self(self, pi + 3, separator + 1);
                    }
                }
            } else if (doubleStar) {
                result = self(self, pi + 2, ci)
                    || (ci < candidate.size() && self(self, pi, ci + 1));
            } else {
                result = self(self, pi + 1, ci)
                    || (ci < candidate.size() && candidate[ci] != '/'
                        && self(self, pi, ci + 1));
            }
        } else if (ci < candidate.size()) {
            result = (pattern[pi] == '?' && candidate[ci] != '/')
                || pattern[pi] == candidate[ci];
            if (result) result = self(self, pi + 1, ci + 1);
        }

        cached = result ? 1 : 0;
        return result;
    };
    return matches(matches, 0, 0);
}

// ─── Sorting helpers ──────────────────────────────────────────────────────────

bool pathLessCI(const fs::path& a, const fs::path& b)
{
#ifdef _WIN32
    const auto as = a.generic_string();
    const auto bs = b.generic_string();
    return std::lexicographical_compare(
        as.begin(), as.end(), bs.begin(), bs.end(),
        [](unsigned char lhs, unsigned char rhs){
            return std::tolower(lhs) < std::tolower(rhs);
        });
#else
    return a.generic_string() < b.generic_string();
#endif
}

bool nameLessCI(const std::string& a, const std::string& b)
{
#ifdef _WIN32
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char lhs, unsigned char rhs){
            return std::tolower(lhs) < std::tolower(rhs);
        });
#else
    return a < b;
#endif
}

// ─── Tool result size guard ────────────────────────────────────────────────────

// Checks that the serialized result JSON fits within the policy limit.
// Returns the content unchanged on success; fails with workspace.result_too_large
// if it doesn't fit.
std::optional<std::string> checkResultSize(const std::string& content,
                                            std::size_t maxResultBytes)
{
    if (content.size() <= maxResultBytes) return content;
    return std::nullopt;
}

// ─── Individual tool handlers ────────────────────────────────────────────────

AgentToolResult executeStat(const AgentToolCall&    call,
                              const WorkspaceReadPolicy& policy,
                              const fs::path&        canonicalRoot)
{
    auto guard = WorkspacePathGuard::validate(
        json::parse(call.argumentsJson)["path"].get<std::string>(), policy);
    if (!guard.success())
        return makeFail(call, guard.error()->code);

    const fs::path target = guard.value();
    std::error_code ec;
    const bool exists = fs::exists(target, ec);

    json result;
    result["relativePath"] = relativeTo(target, canonicalRoot);
    result["exists"]        = !ec && exists;

    if (!ec && exists) {
        const auto status = fs::symlink_status(target, ec);
        if (!ec) {
            result["isFile"]      = fs::is_regular_file(status);
            result["isDirectory"] = fs::is_directory(status);
            result["isSymlink"]   = fs::is_symlink(status);
            if (fs::is_regular_file(status)) {
                const auto sz = fs::file_size(target, ec);
                result["sizeBytes"] = ec ? 0 : static_cast<std::int64_t>(sz);
            }
        }
        result["lastModifiedUnixSec"] = formatMtime(target, ec);
    }

    const std::string content = result.dump();
    if (!checkResultSize(content, policy.maxResultBytes))
        return makeFail(call, "workspace.result_too_large");
    return makeOk(call, content);
}

AgentToolResult executeHash(const AgentToolCall&    call,
                              const WorkspaceReadPolicy& policy,
                              const fs::path&        canonicalRoot)
{
    auto guard = WorkspacePathGuard::validate(
        json::parse(call.argumentsJson)["path"].get<std::string>(), policy);
    if (!guard.success())
        return makeFail(call, guard.error()->code);

    const fs::path target = guard.value();
    std::error_code ec;
    if (!fs::is_regular_file(target, ec))
        return makeFail(call, "workspace_hash.not_a_file");

    const auto fileSize = fs::file_size(target, ec);
    if (ec) return makeFail(call, "workspace_hash.io_error");
    if (fileSize > policy.maxFileBytes)
        return makeFail(call, "workspace_hash.file_too_large");

    bool truncated = false;
    const std::string content = readBytes(target, policy.maxFileBytes, truncated, ec);
    if (ec) return makeFail(call, "workspace_hash.io_error");

    json result;
    result["relativePath"] = relativeTo(target, canonicalRoot);
    result["algorithm"]    = "sha256";
    result["hash"]         = sha256Hex(content);
    result["sizeBytes"]    = static_cast<std::int64_t>(fileSize);

    const std::string out = result.dump();
    if (!checkResultSize(out, policy.maxResultBytes))
        return makeFail(call, "workspace.result_too_large");
    return makeOk(call, out);
}

AgentToolResult executeRead(const AgentToolCall&    call,
                              const WorkspaceReadPolicy& policy,
                              const fs::path&        canonicalRoot)
{
    auto guard = WorkspacePathGuard::validate(
        json::parse(call.argumentsJson)["path"].get<std::string>(), policy);
    if (!guard.success())
        return makeFail(call, guard.error()->code);

    const fs::path target = guard.value();
    std::error_code ec;
    if (!fs::is_regular_file(target, ec))
        return makeFail(call, "workspace_read.not_a_file");

    bool truncated = false;
    const std::string raw = readBytes(target, policy.maxFileBytes, truncated, ec);
    if (ec) return makeFail(call, "workspace_read.io_error");

    if (isBinary(raw.data(), raw.size()))
        return makeFail(call, "workspace_read.binary_file");
    if (!isValidUtf8(raw.data(), raw.size()))
        return makeFail(call, "workspace_read.not_utf8");
    if (containsSecret(raw))
        return makeFail(call, "workspace_read.secret_detected");

    const auto originalSize = fs::file_size(target, ec);
    json result;
    result["relativePath"] = relativeTo(target, canonicalRoot);
    result["content"]      = raw;
    result["sizeBytes"]    = ec ? 0 : static_cast<std::int64_t>(originalSize);
    result["truncated"]    = truncated;
    result["encoding"]     = "utf-8";

    const std::string out = result.dump();
    if (!checkResultSize(out, policy.maxResultBytes))
        return makeFail(call, "workspace.result_too_large");
    return makeOk(call, out, truncated);
}

AgentToolResult executeList(const AgentToolCall&    call,
                              const WorkspaceReadPolicy& policy,
                              const fs::path&        canonicalRoot)
{
    auto guard = WorkspacePathGuard::validate(
        json::parse(call.argumentsJson)["path"].get<std::string>(), policy);
    if (!guard.success())
        return makeFail(call, guard.error()->code);

    const fs::path target = guard.value();
    std::error_code ec;
    if (!fs::is_directory(target, ec))
        return makeFail(call, "workspace_list.not_a_directory");

    // Collect entries into a vector for deterministic sort.
    struct Entry { std::string name; bool isFile; bool isDir; bool isSym; std::int64_t size; };
    std::vector<Entry> entries;

    const auto di = fs::directory_iterator(target, fs::directory_options::skip_permission_denied, ec);
    if (ec) return makeFail(call, "workspace_list.io_error");

    for (const auto& e : di) {
        const auto stat = e.symlink_status(ec);
        if (ec) { ec.clear(); continue; }
        const bool isFile = fs::is_regular_file(stat);
        const bool isDir  = fs::is_directory(stat);
        const bool isSym  = fs::is_symlink(stat);
        std::int64_t sz   = 0;
        if (isFile) {
            const auto fsz = e.file_size(ec);
            if (!ec) sz = static_cast<std::int64_t>(fsz);
            ec.clear();
        }
        entries.push_back({detail::genericUtf8(e.path().filename()), isFile, isDir, isSym, sz});
    }

    // Sort by name, case-insensitive on Windows.
    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b){ return nameLessCI(a.name, b.name); });

    const bool truncated = entries.size() > policy.maxDirectoryEntries;
    if (truncated) entries.resize(policy.maxDirectoryEntries);

    json arr = json::array();
    for (const auto& e : entries) {
        json obj;
        obj["name"]        = e.name;
        obj["isFile"]      = e.isFile;
        obj["isDirectory"] = e.isDir;
        obj["isSymlink"]   = e.isSym;
        if (e.isFile) obj["sizeBytes"] = e.size;
        arr.push_back(std::move(obj));
    }

    json result;
    result["relativePath"]  = relativeTo(target, canonicalRoot);
    result["entries"]       = std::move(arr);
    result["truncated"]     = truncated;

    const std::string out = result.dump();
    if (!checkResultSize(out, policy.maxResultBytes))
        return makeFail(call, "workspace.result_too_large");
    return makeOk(call, out, truncated);
}

AgentToolResult executeFind(const AgentToolCall&    call,
                              const WorkspaceReadPolicy& policy,
                              const fs::path&        canonicalRoot)
{
    const auto rawArgs = json::parse(call.argumentsJson);
    const std::string rawPath = rawArgs["path"].get<std::string>();
    const std::string glob    = rawArgs["glob"].get<std::string>();

    if (glob.empty())
        return makeFail(call, "workspace_find.empty_glob");

    auto guard = WorkspacePathGuard::validate(rawPath, policy);
    if (!guard.success())
        return makeFail(call, guard.error()->code);

    const fs::path searchRoot = guard.value();
    std::error_code ec;
    if (!fs::is_directory(searchRoot, ec))
        return makeFail(call, "workspace_find.not_a_directory");

    std::vector<std::string> matches;
    const auto rdi = fs::recursive_directory_iterator(
        searchRoot, fs::directory_options::skip_permission_denied, ec);
    if (ec) return makeFail(call, "workspace_find.io_error");

    const bool pathPattern = glob.find('/') != std::string::npos
        || glob.find('\\') != std::string::npos;
    for (const auto& entry : rdi) {
        if (entry.is_symlink()) continue; // never follow symlinks
        if (!entry.is_regular_file()) continue;
        const std::string candidate = pathPattern
            ? detail::genericUtf8(entry.path().lexically_relative(searchRoot))
            : detail::genericUtf8(entry.path().filename());
        if (matchGlob(glob, candidate))
            matches.push_back(relativeTo(entry.path(), canonicalRoot));
    }

    // Deterministic order.
    std::sort(matches.begin(), matches.end(), [](const std::string& a, const std::string& b){
        return nameLessCI(a, b);
    });

    const bool truncated = matches.size() > policy.maxSearchMatches;
    if (truncated) matches.resize(policy.maxSearchMatches);

    json result;
    result["searchPath"]  = relativeTo(searchRoot, canonicalRoot);
    result["glob"]        = glob;
    result["matches"]     = matches;
    result["truncated"]   = truncated;
    result["matchCount"]  = matches.size();

    const std::string out = result.dump();
    if (!checkResultSize(out, policy.maxResultBytes))
        return makeFail(call, "workspace.result_too_large");
    return makeOk(call, out, truncated);
}

AgentToolResult executeSearch(const AgentToolCall&    call,
                                const WorkspaceReadPolicy& policy,
                                const fs::path&        canonicalRoot)
{
    const auto rawArgs = json::parse(call.argumentsJson);
    const std::string rawPath = rawArgs["path"].get<std::string>();
    const std::string query   = rawArgs["query"].get<std::string>();

    if (query.empty())
        return makeFail(call, "workspace_search.empty_query");

    auto guard = WorkspacePathGuard::validate(rawPath, policy);
    if (!guard.success())
        return makeFail(call, guard.error()->code);

    const fs::path searchRoot = guard.value();
    std::error_code ec;
    if (!fs::is_directory(searchRoot, ec) && !fs::is_regular_file(searchRoot, ec))
        return makeFail(call, "workspace_search.invalid_path");

    // Collect files to search (sorted for determinism).
    std::vector<fs::path> files;
    if (fs::is_regular_file(searchRoot, ec)) {
        files.push_back(searchRoot);
    } else {
        for (const auto& entry : fs::recursive_directory_iterator(
                 searchRoot, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) { ec.clear(); break; }
            if (entry.is_symlink() || !entry.is_regular_file()) continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end(), pathLessCI);
    }

    struct Match { std::string file; int line; std::string snippet; };
    std::vector<Match> matches;
    bool truncated = false;

    for (const auto& filePath : files) {
        if (matches.size() >= policy.maxSearchMatches) { truncated = true; break; }

        // Size guard: skip oversized files.
        const auto fsz = fs::file_size(filePath, ec);
        if (ec || fsz > policy.maxFileBytes) { ec.clear(); continue; }

        bool dummy = false;
        const std::string content = readBytes(filePath, policy.maxFileBytes, dummy, ec);
        if (ec) { ec.clear(); continue; }

        if (isBinary(content.data(), content.size())) continue;
        if (!isValidUtf8(content.data(), content.size())) continue;
        if (containsSecret(content)) continue; // entire file skipped

        // Line-by-line search (literal, case-sensitive).
        std::istringstream iss(content);
        std::string line;
        int lineNo = 0;
        while (std::getline(iss, line)) {
            ++lineNo;
            if (matches.size() >= policy.maxSearchMatches) { truncated = true; break; }
            if (line.find(query) == std::string::npos) continue;

            // Snippet: truncate long lines to 200 chars.
            std::string snippet = line;
            if (snippet.size() > 200) {
                snippet.resize(200);
                snippet += "...";
            }
            matches.push_back({relativeTo(filePath, canonicalRoot), lineNo, std::move(snippet)});
        }
    }

    json arr = json::array();
    for (const auto& m : matches) {
        json obj;
        obj["file"]    = m.file;
        obj["line"]    = m.line;
        obj["snippet"] = m.snippet;
        arr.push_back(std::move(obj));
    }

    json result;
    result["searchPath"]  = relativeTo(searchRoot, canonicalRoot);
    result["query"]       = query;
    result["matches"]     = std::move(arr);
    result["truncated"]   = truncated;
    result["matchCount"]  = matches.size();

    const std::string out = result.dump();
    if (!checkResultSize(out, policy.maxResultBytes))
        return makeFail(call, "workspace.result_too_large");
    return makeOk(call, out, truncated);
}

// ─── Known-tool argument schemas ─────────────────────────────────────────────

// Returns false when the tool name is unknown.
bool toolKnown(std::string_view name)
{
    static const std::set<std::string_view> known = {
        "workspace_stat_path", "workspace_hash_file", "workspace_read_file",
        "workspace_list_directory", "workspace_find_files", "workspace_search_text"};
    return known.count(name) > 0;
}

// Strict argument schema validation: parse JSON and verify keys match the schema
// defined in ToolRegistry for the given tool.
// Returns an error string on failure, or empty string on success.
std::string validateArgs(const std::string& argumentsJson, std::string_view toolName)
{
    try {
        const json doc = json::parse(argumentsJson);
        if (!doc.is_object()) return "arguments must be a JSON object";

        // Determine allowed/required keys per tool.
        std::set<std::string> required = {"path"};
        std::set<std::string> optional;
        if (toolName == "workspace_search_text")  required.insert("query");
        if (toolName == "workspace_find_files")   required.insert("glob");

        for (const auto& [k, v] : doc.items()) {
            if (!required.count(k) && !optional.count(k))
                return "unknown argument key";
            if (!v.is_string())
                return "argument value must be a string";
        }
        for (const auto& k : required) {
            if (!doc.contains(k))
                return "missing required argument: " + k;
            if (doc[k].get<std::string>().empty())
                return "required argument must not be empty";
        }
    } catch (const std::exception&) {
        return "malformed JSON";
    }
    return {};
}

} // namespace

// ─── FilesystemWorkspaceToolExecutor::execute() ──────────────────────────────

common::Result<AgentToolResult> FilesystemWorkspaceToolExecutor::execute(
    const AgentToolCall&       call,
    const WorkspaceReadPolicy& policy)
try {
    // Guard 1: known tool.
    if (!toolKnown(call.toolName))
        return common::Result<AgentToolResult>::ok(
            makeFail(call, "tool_call.unknown_tool"));

    // Guard 2: strict argument schema.
    const auto argErr = validateArgs(call.argumentsJson, call.toolName);
    if (!argErr.empty())
        return common::Result<AgentToolResult>::ok(
            makeFail(call, "workspace.invalid_arguments"));

    // Guard 3: trusted policy must be internally consistent.
    const auto policyCheck = WorkspaceReadPolicyValidator::validateTrustedPolicy(policy);
    if (!policyCheck.success())
        return common::Result<AgentToolResult>::ok(
            makeFail(call, policyCheck.error()->code));

    // Canonical workspace root — needed to produce relative result paths.
    std::error_code ec;
    const fs::path canonicalRoot = fs::weakly_canonical(policy.workspaceRoot, ec);
    if (ec)
        return common::Result<AgentToolResult>::ok(
            makeFail(call, "workspace_path.root_inaccessible"));

    // Dispatch.
    AgentToolResult result;
    if (call.toolName == "workspace_stat_path")
        result = executeStat(call, policy, canonicalRoot);
    else if (call.toolName == "workspace_hash_file")
        result = executeHash(call, policy, canonicalRoot);
    else if (call.toolName == "workspace_read_file")
        result = executeRead(call, policy, canonicalRoot);
    else if (call.toolName == "workspace_list_directory")
        result = executeList(call, policy, canonicalRoot);
    else if (call.toolName == "workspace_find_files")
        result = executeFind(call, policy, canonicalRoot);
    else // workspace_search_text
        result = executeSearch(call, policy, canonicalRoot);

    return common::Result<AgentToolResult>::ok(std::move(result));

} catch (const std::exception&) {
    // No exceptions escape execute().
    AgentToolResult r = makeFail(call, "workspace.internal_error");
    return common::Result<AgentToolResult>::ok(std::move(r));
}

} // namespace ai::application
