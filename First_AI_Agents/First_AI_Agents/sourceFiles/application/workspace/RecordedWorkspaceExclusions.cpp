#include "ai/application/workspace/RecordedWorkspaceExclusions.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <algorithm>
#include <string_view>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxRecordedPathBytes = 512;

constexpr std::string_view kPathKind = "path:";
constexpr std::string_view kStaticKinds[] = {
    "directory-name:", "directory-sequence:", "file-name:", "extension:"};

common::Error invalid(std::string detail)
{
    return {kRecordedExclusionInvalidCode,
        "A recorded workspace exclusion cannot be restored: " + std::move(detail)};
}

bool wellFormedUtf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        char32_t code = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; code = lead & 0x1F; }
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; code = lead & 0x0F; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; code = lead & 0x07; }
        else return false;
        if (i + extra >= text.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if ((extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000)
            || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

bool reservedDeviceName(std::string_view component)
{
    std::string stem = detail::asciiLower(std::string(component));
    const auto dot = stem.find('.');
    if (dot != std::string::npos) stem.resize(dot);
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "clock$") {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt"))
        && stem[3] >= '1' && stem[3] <= '9';
}

// Empty = valid, otherwise the reason.
std::string componentProblem(std::string_view component)
{
    if (component.empty()) return "an empty path component";
    if (component == "." || component == "..") return "a '.' or '..' path component";
    if (component.back() == '.' || component.back() == ' ') {
        return "a path component that ends in '.' or a space";
    }
    for (const unsigned char ch : component) {
        if (ch < 0x20 || ch == 0x7F) return "a control character";
        if (ch == ':' ) return "a ':' (drive-relative path or alternate data stream)";
        if (ch == '\\' ) return "a '\\' separator";
        if (ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            return "a wildcard or reserved character";
        }
    }
    if (reservedDeviceName(component)) return "a reserved Windows device name";
    return {};
}

std::vector<std::string> splitComponents(std::string_view value)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto slash = value.find('/', start);
        parts.emplace_back(value.substr(start,
            slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return parts;
}

std::string foldComponent(const std::string& component)
{
#ifdef _WIN32
    return detail::asciiLower(component);
#else
    return component;
#endif
}

bool isPrefixOf(const std::vector<std::string>& shorter, const std::vector<std::string>& longer)
{
    if (shorter.size() > longer.size()) return false;
    return std::equal(shorter.begin(), shorter.end(), longer.begin());
}

} // namespace

common::Result<std::vector<fs::path>> RecordedWorkspaceExclusions::parsePaths(
    const std::vector<std::string>& exclusions)
{
    using Result = common::Result<std::vector<fs::path>>;
    std::vector<fs::path> paths;
    std::vector<std::vector<std::string>> folded;

    for (const auto& entry : exclusions) {
        const std::string_view view(entry);
        if (!view.starts_with(kPathKind)) {
            const bool known = std::any_of(std::begin(kStaticKinds), std::end(kStaticKinds),
                [&](std::string_view kind) { return view.starts_with(kind); });
            if (!known) return Result::failure(invalid("an unrecognised exclusion kind"));
            continue;
        }

        const std::string_view value = view.substr(kPathKind.size());
        if (value.empty()) return Result::failure(invalid("an empty path"));
        if (value.size() > kMaxRecordedPathBytes) return Result::failure(invalid("a path that is too long"));
        if (!wellFormedUtf8(value)) return Result::failure(invalid("a path that is not valid UTF-8"));
        if (value.front() == '/' || value.back() == '/') {
            return Result::failure(invalid("a leading or trailing '/'"));
        }

        auto parts = splitComponents(value);
        for (const auto& part : parts) {
            const auto problem = componentProblem(part);
            if (!problem.empty()) return Result::failure(invalid(problem));
        }

        std::vector<std::string> key;
        key.reserve(parts.size());
        for (const auto& part : parts) key.push_back(foldComponent(part));
        for (const auto& other : folded) {
            if (isPrefixOf(other, key) || isPrefixOf(key, other)) {
                return Result::failure(invalid("duplicate or nested paths"));
            }
        }
        folded.push_back(std::move(key));
        paths.push_back(detail::pathFromUtf8(std::string(value)));
    }
    return Result::ok(std::move(paths));
}

common::Result<WorkspaceReadPolicy> RecordedWorkspaceExclusions::policyFor(
    fs::path workspaceRoot, const std::vector<std::string>& exclusions)
{
    auto paths = parsePaths(exclusions);
    if (!paths.success()) return common::Result<WorkspaceReadPolicy>::failure(*paths.error());
    auto policy = WorkspaceReadPolicy::secureDefaults(std::move(workspaceRoot));
    policy.deniedPaths = std::move(paths.value());
    return common::Result<WorkspaceReadPolicy>::ok(std::move(policy));
}

} // namespace ai::application
