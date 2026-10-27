#include "ai/application/ManifestProposalSetVerifier.hpp"

#include "ai/application/ExecutionManifestDependencyGraph.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/verification/VerificationToolchain.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <functional>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

bool safeToken(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
           });
}

bool safeRelative(std::string_view value)
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

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void replaceAll(std::string& value, std::string_view needle,
    std::string_view replacement)
{
    if (needle.empty()) return;
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

// Manifest verification reliability hardening: a line's usefulness tier -
// lower number is more useful. A real error line must never be displaced by
// a long, otherwise-noisy command/context line just because that line
// happens to contain a loose substring like "cmake" (e.g. a compiler
// invocation whose include path contains "...\CMakeFiles\..." - the exact
// bug this rewrite fixes: the old flat keyword filter treated that path
// fragment as evidence of a "cmake" diagnostic line). Every tier here uses
// a precise marker, never a bare "cmake" or "ninja" substring match, except
// the deliberately-last-resort tier 5.
enum class DiagnosticLineTier {
    None = 0,
    // 1: fatal error / compiler error code / ninja's own terminal markers.
    Fatal = 1,
    // 2: QML syntax/type/missing-property/unresolved-type diagnostics.
    QmlDiagnostic = 2,
    // 3: ordinary "error"/"failed" mentions.
    OrdinaryError = 3,
    // 4: CMake's own diagnostic summary lines (precise "CMake Error"/
    // "CMake Warning" prefixes, or the CMAKE_OBJECT_PATH_MAX marker),
    // never a bare "cmake" substring.
    CMakeSummary = 4,
    // 5: last-resort - a bare "ninja:" or "cmake" mention with nothing
    // better available, so the excerpt is never empty when there is at
    // least some build-tool-related content to point to.
    LowValue = 5,
};

bool containsCaseInsensitive(std::string_view foldedHaystack, std::string_view needle)
{
    return foldedHaystack.find(needle) != std::string_view::npos;
}

// Recognizes a qmllint-style bracket category tag, e.g. "[syntax]",
// "[missing-property]", "[unresolved-type]", "[unqualified]" - a lowercase
// word (letters/hyphens only) wrapped in brackets. Deliberately generic
// (not an enumerated list of every qmllint category) so a category this
// codebase has not seen before still classifies correctly, while still
// never matching on a bare "cmake"/"ninja" substring.
bool containsQmlDiagnosticTag(std::string_view line)
{
    std::size_t open = 0;
    while ((open = line.find('[', open)) != std::string_view::npos) {
        const auto close = line.find(']', open + 1);
        if (close == std::string_view::npos) return false;
        const auto inner = line.substr(open + 1, close - open - 1);
        if (!inner.empty() && inner.size() <= 32
            && std::all_of(inner.begin(), inner.end(), [](unsigned char ch) {
                   return (ch >= 'a' && ch <= 'z') || ch == '-';
               })) {
            return true;
        }
        open = close + 1;
    }
    return false;
}

// Matches a compiler error code such as "error C1083"/"error C2065" -
// "error" followed by whitespace, then an uppercase or lowercase 'c'
// immediately followed by a digit. Distinguishes a genuine MSVC diagnostic
// from an incidental "error" substring elsewhere in a line.
bool containsCompilerErrorCode(std::string_view line)
{
    const auto folded = std::string(line);
    const auto lowered = lower(folded);
    std::size_t pos = 0;
    while ((pos = lowered.find("error", pos)) != std::string_view::npos) {
        std::size_t cursor = pos + 5;
        while (cursor < lowered.size() && lowered[cursor] == ' ') ++cursor;
        if (cursor + 1 < lowered.size() && lowered[cursor] == 'c'
            && std::isdigit(static_cast<unsigned char>(lowered[cursor + 1]))) {
            return true;
        }
        pos += 5;
    }
    return false;
}

DiagnosticLineTier classifyDiagnosticLine(const std::string& sanitized)
{
    const auto folded = lower(sanitized);
    // Tier 1: fatal error / compiler error code / ninja's own terminal
    // markers - checked with FULL-LINE precise markers, never a bare
    // "cmake" substring.
    if (containsCaseInsensitive(folded, "fatal error")
        || containsCompilerErrorCode(sanitized)
        || sanitized.find("FAILED:") != std::string::npos
        || containsCaseInsensitive(folded, "ninja: build stopped")) {
        return DiagnosticLineTier::Fatal;
    }
    // Tier 2: QML syntax/type/missing-property/unresolved-type diagnostics.
    if (containsCaseInsensitive(folded, "type name must be upper case")
        || containsQmlDiagnosticTag(sanitized)) {
        return DiagnosticLineTier::QmlDiagnostic;
    }
    // Tier 3: ordinary error/failed mentions.
    if (containsCaseInsensitive(folded, "error")
        || containsCaseInsensitive(folded, "failed")) {
        return DiagnosticLineTier::OrdinaryError;
    }
    // Tier 4: CMake's own diagnostic summary - precise prefixes/markers,
    // never a bare "cmake" substring (a compiler invocation line
    // referencing a "...\CMakeFiles\..." path must never match here).
    if (sanitized.starts_with("CMake Error") || sanitized.starts_with("CMake Warning")
        || sanitized.find("CMAKE_OBJECT_PATH_MAX") != std::string::npos) {
        return DiagnosticLineTier::CMakeSummary;
    }
    // Tier 5: last resort - a bare "ninja:"/"cmake" mention, kept only so
    // the excerpt is never empty when nothing more specific exists.
    if (containsCaseInsensitive(folded, "ninja:") || containsCaseInsensitive(folded, "cmake")) {
        return DiagnosticLineTier::LowValue;
    }
    return DiagnosticLineTier::None;
}

// Manifest verification reliability hardening follow-up (lineage-aware
// repair materialization incident): named, explicit bounds for the excerpt
// selection below - a real Run showed the previous fixed "last 3 lines of
// the best tier" rule could hide an EARLIER actionable warning (e.g. from a
// different file) behind later, less useful repeats of the same warning.
constexpr std::size_t kMaximumExcerptBytes = 2048;
constexpr std::size_t kMaximumExcerptLines = kMaximumRecordedFindingsPerDiagnostic;
// Upper bound on how many findings a baseline-aware QML-lint comparison will
// hold per side. Real output is already bounded by the command runner's byte
// limit (exceeding it is a distinct verification.output_overflow failure);
// beyond this count the comparison would no longer be exact, so it fails
// closed with verification.qml_findings_overflow instead of comparing samples.
constexpr std::size_t kMaximumComparableQmlFindings = 1024;
// Deterministic, bounded, leak-safe - never raw command output, never an
// absolute path, never a secret. Appended only when the cap actually
// dropped or cut something, so its presence alone reliably tells a repair
// worker (or a human reading the report) that further actionable content
// exists beyond what fit in this bounded excerpt.
constexpr std::string_view kExcerptTruncationMarker =
    " [truncated: additional actionable diagnostics omitted]";

// Real-Run incident (Run c573539c-ddf1-4ded-bd70-a60d2ba475a7, 2026-09-11):
// extracts a per-file grouping key from an already-sanitized diagnostic
// line, so the bounded selection below can guarantee every distinct file
// gets at least one line instead of losing an entire file's warnings to
// truncation. qmllint/compiler diagnostics are consistently
// "<path>:<line>:<col>: message" (optionally prefixed by a wrapper like
// "Warning: "), so the key is everything before the first ":<digit>" - the
// start of that location marker. A line with no such marker (e.g. a bare
// "FAILED: ..." line) has no identifiable file, so the whole line becomes
// its own key, keeping it in its own single-line group rather than being
// silently merged with an unrelated line.
std::string diagnosticLineFileKey(const std::string& line)
{
    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        if (line[index] == ':' && std::isdigit(static_cast<unsigned char>(line[index + 1]))) {
            return line.substr(0, index);
        }
    }
    return line;
}

struct DiagnosticExcerpt {
    std::string text;
    // Bounded (kMaximumExcerptLines) - parsed from exactly the lines in `text`.
    // This is what is persisted/displayed.
    std::vector<ManifestVerificationDiagnosticFinding> findings;
    // Every parsable finding in the (sanitized, deduplicated) output, never
    // truncated by the excerpt bounds. A baseline/candidate delta is only
    // sound on COMPLETE sets: two independently truncated samples of the same
    // output are not comparable (see qmlLintBaselineDeltaCheck).
    std::vector<ManifestVerificationDiagnosticFinding> allFindings;
};

// A handful of well-known single-word wrappers qmllint/compilers prefix a
// diagnostic line with, ahead of the "<file>:<line>:<column>:" location
// marker. Stripped only from the structured finding's fileKey (a group
// header shown to a repair worker) - the flat excerpt string used for the
// authoritative diagnostic.message is never altered by this.
std::string stripDiagnosticWrapperPrefix(std::string value)
{
    // Real Run incident (Run bb56e001-eac0-4b59-8d0b-d037c6bc1031, 2026-09-15,
    // surfaced via MW-P5.4's "Continue from failed verification"): qmllint
    // also emits "Info: " for its lowest-severity findings (e.g. unused
    // imports) - missing it here left the wrapper word attached to the
    // fileKey (e.g. "Info: <candidate>/qml/AppShell.qml"), which then failed
    // ManifestVerificationAttributionService's candidate-relative-path check
    // with `attribution_path_invalid` for the exact same reason a missing
    // wrapper always would.
    static constexpr std::array<std::string_view, 4> kWrappers {
        "Warning: ", "Error: ", "Note: ", "Info: "};
    for (const auto& wrapper : kWrappers) {
        if (value.starts_with(wrapper)) return value.substr(wrapper.size());
    }

    // Real-Run incident (Run fea29bc8-3f5e-4d05-a511-ecc2475cde5b,
    // 2026-09-23): Qt 6.11's qmllint output can concatenate a hint line and
    // the following diagnostic without a newline, producing a key such as
    // "You first have to give the element an idWarning: <candidate>/x.qml".
    // The diagnostic itself still has a trusted, leak-safe wrapper followed
    // immediately by the verifier-authored <candidate> marker. Recover only
    // that narrow shape; an arbitrary prefix containing <candidate> without
    // a known diagnostic wrapper remains untouched and therefore continues
    // to fail closed in attribution.
    static constexpr std::array<std::string_view, 2> kCandidateMarkers {
        "<candidate>/", "<candidate>\\"};
    for (const auto& wrapper : kWrappers) {
        for (const auto& marker : kCandidateMarkers) {
            const std::string token = std::string(wrapper) + std::string(marker);
            if (const auto position = value.rfind(token); position != std::string::npos) {
                return value.substr(position + wrapper.size());
            }
        }
    }
    return value;
}

// Real-Run incident (Run 8081057a-a669-4f58-abec-952244624d74, 2026-09-11):
// parses one already-sanitized, already leak-safe selected line into a
// structured finding, when it matches the qmllint/compiler
// "<file>:<line>:<column>: <text> [<tag>]" shape - reusing
// diagnosticLineFileKey's exact split point so a finding's fileKey always
// matches the same grouping key the excerpt's own per-file selection
// already used. A line that does not match this shape (for example a bare
// "FAILED: ..." command-failure summary) returns nullopt; it still remains
// part of the flat excerpt text, just not structured further.
std::optional<ManifestVerificationDiagnosticFinding> parseDiagnosticFinding(
    const std::string& line)
{
    const auto key = diagnosticLineFileKey(line);
    if (key.size() + 1 >= line.size() || line[key.size()] != ':') return std::nullopt;

    std::size_t cursor = key.size() + 1;
    const auto lineStart = cursor;
    while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor]))) ++cursor;
    if (cursor == lineStart) return std::nullopt;
    int lineNumber = 0;
    if (std::from_chars(line.data() + lineStart, line.data() + cursor, lineNumber).ec
        != std::errc {}) return std::nullopt;

    if (cursor >= line.size() || line[cursor] != ':') return std::nullopt;
    ++cursor;
    const auto columnStart = cursor;
    while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor]))) ++cursor;
    if (cursor == columnStart) return std::nullopt;
    int columnNumber = 0;
    if (std::from_chars(line.data() + columnStart, line.data() + cursor, columnNumber).ec
        != std::errc {}) return std::nullopt;

    if (cursor < line.size() && line[cursor] == ':') ++cursor;
    while (cursor < line.size() && line[cursor] == ' ') ++cursor;

    std::string tag;
    const auto closeBracket = line.rfind(']');
    if (closeBracket != std::string::npos) {
        const auto openBracket = line.rfind('[', closeBracket);
        if (openBracket != std::string::npos && openBracket >= cursor) {
            const auto inner = line.substr(openBracket + 1, closeBracket - openBracket - 1);
            if (!inner.empty() && inner.size() <= 32
                && std::all_of(inner.begin(), inner.end(), [](unsigned char ch) {
                       return (ch >= 'a' && ch <= 'z') || ch == '-';
                   })) {
                tag = inner;
            }
        }
    }

    ManifestVerificationDiagnosticFinding finding;
    finding.fileKey = stripDiagnosticWrapperPrefix(key);

    // Real-Run incident (Run bb56e001-eac0-4b59-8d0b-d037c6bc1031, 2026-09-15,
    // surfaced via MW-P5.4's "Continue from failed verification"): qmllint
    // sometimes emits a CONTINUATION line for the diagnostic immediately
    // above it - e.g. "Warning: <candidate>/x.qml:4:1: Warnings occurred
    // while importing module \"Foo\": [import]" followed by
    // "Warning: 4:1: Failed to import Foo. ... [import]" - which repeats the
    // SAME line:column but never repeats the file path. diagnosticLineFileKey's
    // leading ":<digit>" scan matches that bare "4:1:" shape too, but because
    // the scan stops at the FIRST ":<digit>" pair, the "key" it extracts for
    // a line shaped this way is always exactly the repeated line number's own
    // digits (here "4") - never a real path fragment, with or without a
    // wrapper prefix. That bogus fileKey then failed
    // ManifestVerificationAttributionService's candidate-relative-path check
    // with `attribution_path_invalid`, permanently blocking (the attribution
    // row is immutable) MW-P5.4's continuation for the entire Run over one
    // unattributable continuation line, even though the file it actually
    // concerns was already correctly identified by the diagnostic line right
    // above it. A genuine file reference is never purely digits, so this is a
    // precise, narrow rejection - unlike requiring the "<candidate>" leak-safe
    // marker outright, it does not also reject a fixture/tool line that
    // legitimately names a bare relative filename with no working-directory
    // prefix to sanitize.
    if (!finding.fileKey.empty() && std::all_of(finding.fileKey.begin(), finding.fileKey.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return std::nullopt;
    }

    finding.line = lineNumber;
    finding.column = columnNumber;
    finding.tag = tag;
    finding.text = line.substr(cursor);
    return finding;
}

// Per-FILE bounded selection (see the incident notes in
// leakSafeDiagnosticExcerpt below): given one grouping key per entry, returns
// the ascending indices of at most kMaximumExcerptLines entries, guaranteeing
// every selected file at least its first-seen entry before any file gets a
// second one. Callers only invoke this when there are more entries than the
// cap.
std::vector<std::size_t> selectPerFileIndices(const std::vector<std::string>& fileKeys)
{
    std::vector<std::string> fileOrder;
    std::unordered_map<std::string, std::vector<std::size_t>> indicesByFile;
    for (std::size_t index = 0; index < fileKeys.size(); ++index) {
        const auto [iterator, inserted] = indicesByFile.try_emplace(fileKeys[index]);
        if (inserted) fileOrder.push_back(fileKeys[index]);
        iterator->second.push_back(index);
    }

    // Which files get their guaranteed line. When every affected file fits
    // within the line cap, all of them do. When there are MORE distinct files
    // than the cap (representing every one is then mathematically
    // impossible), split the FILE list itself early+late - the same head/tail
    // spirit the previous line-level selection used - so an early and a late
    // affected file both remain represented, rather than silently favoring
    // only whichever files happened to be discovered first.
    std::vector<std::string> guaranteedFiles;
    if (fileOrder.size() <= kMaximumExcerptLines) {
        guaranteedFiles = fileOrder;
    } else {
        const std::size_t headCount = kMaximumExcerptLines / 2;
        const std::size_t tailCount = kMaximumExcerptLines - headCount;
        guaranteedFiles.insert(
            guaranteedFiles.end(), fileOrder.begin(), fileOrder.begin() + headCount);
        guaranteedFiles.insert(
            guaranteedFiles.end(), fileOrder.end() - tailCount, fileOrder.end());
    }

    std::vector<bool> taken(fileKeys.size(), false);
    std::vector<std::size_t> order;
    // Pass 1: one guaranteed entry (the first-seen one) per selected file.
    for (const auto& file : guaranteedFiles) {
        const auto firstIndex = indicesByFile.at(file).front();
        taken[firstIndex] = true;
        order.push_back(firstIndex);
    }
    // Pass 2: fill any remaining budget round-robin across the same selected
    // files (each file's next not-yet-taken entry, one round at a time) so a
    // single very-verbose file's later entries can never crowd out a second
    // finding from another file before every selected file has had a fair
    // turn.
    for (std::size_t round = 1; order.size() < kMaximumExcerptLines; ++round) {
        bool addedAny = false;
        for (const auto& file : guaranteedFiles) {
            if (order.size() >= kMaximumExcerptLines) break;
            const auto& fileIndices = indicesByFile.at(file);
            if (round >= fileIndices.size()) continue;
            const auto index = fileIndices[round];
            if (taken[index]) continue;
            taken[index] = true;
            order.push_back(index);
            addedAny = true;
        }
        if (!addedAny) break;
    }
    // Restore original relative order for a readable, deterministic result.
    std::sort(order.begin(), order.end());
    return order;
}

// The same bounded per-file sample, applied to structured findings. Used for
// what is PERSISTED/displayed after a baseline delta has already been decided
// on the complete sets - never as input to that decision.
std::vector<ManifestVerificationDiagnosticFinding> boundedFindingsPerFile(
    std::vector<ManifestVerificationDiagnosticFinding> findings)
{
    if (findings.size() <= kMaximumExcerptLines) return findings;
    std::vector<std::string> keys;
    keys.reserve(findings.size());
    for (const auto& finding : findings) keys.push_back(finding.fileKey);
    std::vector<ManifestVerificationDiagnosticFinding> selected;
    for (const auto index : selectPerFileIndices(keys)) {
        selected.push_back(std::move(findings[index]));
    }
    return selected;
}

DiagnosticExcerpt leakSafeDiagnosticExcerpt(
    std::string output, const fs::path& workingDirectory)
{
    replaceAll(output, workingDirectory.string(), "<candidate>");
    replaceAll(output, workingDirectory.generic_string(), "<candidate>");

    std::istringstream lines(output);
    // Bucketed by tier, each bucket in original line order.
    std::array<std::vector<std::string>, 5> tieredCandidates;
    std::string line;
    while (std::getline(lines, line)) {
        const auto folded = lower(line);
        if (folded.find("password") != std::string::npos
            || folded.find("secret") != std::string::npos
            || folded.find("api_key") != std::string::npos
            || folded.find("api-key") != std::string::npos
            || folded.find("authorization") != std::string::npos
            || folded.find("token=") != std::string::npos) {
            continue;
        }
        std::string sanitized;
        sanitized.reserve(line.size());
        for (std::size_t index = 0; index < line.size();) {
            const bool drivePath = index + 2 < line.size()
                && std::isalpha(static_cast<unsigned char>(line[index]))
                && line[index + 1] == ':'
                && (line[index + 2] == '\\' || line[index + 2] == '/');
            const bool uncPath = index + 1 < line.size()
                && line[index] == '\\' && line[index + 1] == '\\';
            if (drivePath || uncPath) {
                sanitized += "<absolute-path>";
                index += drivePath ? 3 : 2;
                while (index < line.size()
                    && !std::isspace(static_cast<unsigned char>(line[index]))
                    && line[index] != '\"' && line[index] != '\'') {
                    ++index;
                }
                continue;
            }
            const unsigned char ch = static_cast<unsigned char>(line[index++]);
            if (ch == '\t') sanitized.push_back(' ');
            else if (ch >= 0x20 && ch != 0x7f) sanitized.push_back(static_cast<char>(ch));
        }
        if (sanitized.empty()) continue;
        const auto tier = classifyDiagnosticLine(sanitized);
        if (tier == DiagnosticLineTier::None) continue;
        tieredCandidates[static_cast<std::size_t>(tier) - 1].push_back(std::move(sanitized));
    }
    // Manifest verification reliability hardening follow-up #2: tiers 1-3
    // (Fatal, QmlDiagnostic, OrdinaryError) are each independently
    // "significant" - collect the UNION of every non-empty one of them, in
    // tier-priority order, rather than only the single best tier. A real
    // Run showed a Ninja "FAILED:" marker (tier 1) could otherwise exclude
    // a genuinely different, equally actionable QML diagnostic (tier 2)
    // found in the SAME output, silently handing a repair worker only the
    // build-tool wrapper instead of the real cause. Tiers 4-5 (CMakeSummary,
    // LowValue) remain last-resort ONLY - included solely when nothing
    // significant exists at all, exactly as before.
    std::vector<std::string> candidates;
    for (std::size_t tierIndex = 0; tierIndex < 3; ++tierIndex) {
        for (auto& line : tieredCandidates[tierIndex]) candidates.push_back(std::move(line));
    }
    if (candidates.empty()) {
        for (std::size_t tierIndex = 3; tierIndex < tieredCandidates.size(); ++tierIndex) {
            if (!tieredCandidates[tierIndex].empty()) {
                candidates = std::move(tieredCandidates[tierIndex]);
                break;
            }
        }
    }
    if (candidates.empty()) return {};

    // Deduplicate across the collected candidates, preserving first-seen
    // order - a diagnostic tool repeating the identical line must never
    // consume two of the bounded line slots below.
    std::vector<std::string> deduped;
    deduped.reserve(candidates.size());
    {
        std::unordered_set<std::string> seen;
        for (auto& candidate : candidates) {
            if (seen.insert(candidate).second) deduped.push_back(std::move(candidate));
        }
    }

    // When the deduplicated set exceeds the line cap, select per FILE, not
    // per fixed head/tail position. Real-Run incident (Run
    // c573539c-ddf1-4ded-bd70-a60d2ba475a7, 2026-09-11): the previous plain
    // "half from the start, half from the end" rule lost an entire file's
    // warnings whenever they were all clustered in the omitted middle of a
    // multi-file qmllint run - the repair worker then saw findings for two
    // files but none at all for a third file it had equally broken,
    // "fixed" only what it could see, and the untouched file's warnings
    // still failed the very next verification. Every distinct file (by
    // diagnosticLineFileKey) is now guaranteed at least its first-seen
    // line before any file gets a second one, so a repair worker always
    // has at least one concrete, actionable finding per affected file.
    // Complete structured evidence, parsed BEFORE the bounded selection below
    // drops anything: see DiagnosticExcerpt::allFindings.
    DiagnosticExcerpt result;
    result.allFindings.reserve(deduped.size());
    for (const auto& dedupedLine : deduped) {
        if (auto finding = parseDiagnosticFinding(dedupedLine)) {
            result.allFindings.push_back(std::move(*finding));
        }
    }

    std::vector<std::string> selected;
    std::size_t omittedLineCount = 0;
    if (deduped.size() <= kMaximumExcerptLines) {
        selected = std::move(deduped);
    } else {
        std::vector<std::string> fileKeys;
        fileKeys.reserve(deduped.size());
        for (const auto& dedupedLine : deduped) {
            fileKeys.push_back(diagnosticLineFileKey(dedupedLine));
        }
        const auto order = selectPerFileIndices(fileKeys);
        selected.reserve(order.size());
        for (const auto index : order) selected.push_back(deduped[index]);
        omittedLineCount = deduped.size() - selected.size();
    }

    // Manifest verification reliability hardening follow-up #1: build the
    // excerpt against the FULL kMaximumExcerptBytes budget first (never
    // pre-reserving marker space that then goes unused when no truncation
    // actually occurs), then - only if a marker turns out to be needed -
    // trim the already-built excerpt to make room for it. This is what
    // actually guarantees excerpt.size() <= kMaximumExcerptBytes in every
    // case, including the one the pre-reservation approach missed: byte-cap
    // truncation discovered only while appending (omittedLineCount == 0,
    // i.e. every selected line fit under the LINE cap, but their combined
    // bytes still exceeded the BYTE cap) previously appended the marker
    // after already using the full byte budget, silently exceeding it.
    std::string excerpt;
    bool byteCapTruncated = false;
    for (const auto& line : selected) {
        const std::string separator = excerpt.empty() ? std::string() : std::string(" | ");
        if (excerpt.size() + separator.size() >= kMaximumExcerptBytes) {
            byteCapTruncated = true;
            break;
        }
        excerpt += separator;
        const auto remaining = kMaximumExcerptBytes - excerpt.size();
        if (line.size() > remaining) {
            excerpt.append(line, 0, remaining);
            byteCapTruncated = true;
            break;
        }
        excerpt += line;
    }
    if (omittedLineCount > 0 || byteCapTruncated) {
        const std::size_t markerSize = kExcerptTruncationMarker.size();
        const std::size_t keep =
            kMaximumExcerptBytes > markerSize ? kMaximumExcerptBytes - markerSize : 0;
        if (excerpt.size() > keep) excerpt.resize(keep);
        excerpt += std::string(kExcerptTruncationMarker);
    }

    // Structured findings are parsed from the exact same `selected` lines
    // the excerpt text above was built from - never a second, independent
    // pass over raw output - so they can never disagree with, or expose
    // content beyond, what the already leak-safe/bounded excerpt contains.
    result.text = std::move(excerpt);
    result.findings.reserve(selected.size());
    for (const auto& line : selected) {
        if (auto finding = parseDiagnosticFinding(line)) {
            result.findings.push_back(std::move(*finding));
        }
    }
    return result;
}

// Manifest verification reliability hardening: a conservative, documented
// Windows path-budget preflight - checked BEFORE any CMake command runs,
// never after. This is explicitly NOT a mathematical guarantee for every
// possible CMake target/source file name; kReservedForBuildArtifacts
// reserves headroom for "\CMakeFiles\<target>.dir\<nested-source>.obj" (or
// ".obj.d"/generator bookkeeping files), which can easily add 100+
// characters beyond the resolved build directory itself. The real,
// definitive protection is commandCheck()'s own CMAKE_OBJECT_PATH_MAX
// detection above, which inspects the actual configure output; this
// preflight only avoids spending a build attempt at all when the budget is
// already, obviously exceeded.
constexpr std::size_t kWindowsMaxPath = 260;
constexpr std::size_t kReservedForBuildArtifacts = 130;

std::optional<ManifestVerificationCheckResult> overlayPathBudgetCheck(
    const ExecutionAcceptanceRequirement& requirement,
    const fs::path& candidateRoot,
    const std::optional<std::string>& resolvedBinaryDir,
    std::string_view commandIdentity)
{
#ifdef _WIN32
    std::size_t length = candidateRoot.native().size();
    if (resolvedBinaryDir.has_value()) length += 1 + resolvedBinaryDir->size();
    if (length + kReservedForBuildArtifacts <= kWindowsMaxPath) return std::nullopt;
    ManifestVerificationCheckResult check;
    check.profileId = requirement.id;
    check.kind = requirement.verificationKind;
    check.commandIdentity = std::string(commandIdentity);
    check.status = ExecutionVerificationStatus::Failed;
    check.diagnostics.push_back({"verification.overlay_path_too_long",
        "The verification candidate path exceeds a safe Windows/CMake path budget",
        requirement.id});
    return check;
#else
    static_cast<void>(requirement);
    static_cast<void>(candidateRoot);
    static_cast<void>(resolvedBinaryDir);
    static_cast<void>(commandIdentity);
    return std::nullopt;
#endif
}

enum class QmlWarningScope {
    None,
    InTaskScope,
    OutsideTaskScopeOnly,
};

std::vector<fs::path> qmlVerificationScopeForTask(
    const ExecutionManifestTask& task,
    const std::vector<ManifestDependencyEdge>& dependencyGraph)
{
    std::vector<fs::path> scope;
    std::unordered_set<std::string> seen;
    const auto append = [&](const fs::path& path) {
        const auto key = detail::normalizedPathKey(path);
        if (seen.insert(key).second) scope.push_back(path);
    };

    for (const auto& path : task.ownedPaths) append(path);
    for (const auto& path : task.plannedCreatePaths) append(path);

    // An integration task commonly owns only CMake/context files while its
    // command-backed qml_lint criterion verifies QML outputs produced by its
    // dependencies. The shared dependency graph is the only safe authority
    // for widening that criterion's attribution boundary: it excludes
    // baseline-only reads, unresolved reads, self-reads, and plain declared
    // task edges with no path evidence. Never accept arbitrary required reads
    // as QML scope.
    for (const auto& edge : dependencyGraph) {
        if (edge.origin != ManifestDependencyEdgeOrigin::BackendDerived
            || edge.consumerTaskId != task.id || !edge.relativePath.has_value()) {
            continue;
        }
        append(fs::path(*edge.relativePath));
    }
    return scope;
}

// qmllint targets commonly cover an entire QML module while one Manifest
// task owns only a bounded subset of that module. A warning in an unrelated,
// immutable baseline file must not make that task impossible to complete, but
// an unattributed warning must still fail closed. Attribute warning blocks by
// their most recent .qml source-location line; paths are compared only against
// the task's frozen exact write scope plus Backend-proven dependency-output
// paths, and are never surfaced from this helper.
QmlWarningScope classifyQmlWarningScope(
    const std::string& output,
    const std::vector<fs::path>* taskVerificationScope)
{
    const auto foldedOutput = lower(output);
    const bool hasWarning = foldedOutput.find("warning:") != std::string::npos
        || foldedOutput.find("[unqualified]") != std::string::npos;
    if (!hasWarning) return QmlWarningScope::None;
    if (taskVerificationScope == nullptr) return QmlWarningScope::InTaskScope;

    std::vector<std::string> qmlScopeKeys;
    std::unordered_map<std::string, std::size_t> qmlBasenameCounts;
    for (const auto& path : *taskVerificationScope) {
        auto key = lower(path.generic_string());
        std::replace(key.begin(), key.end(), '\\', '/');
        if (key.ends_with(".qml")) {
            auto basename = lower(path.filename().generic_string());
            ++qmlBasenameCounts[basename];
            qmlScopeKeys.push_back(std::move(key));
        }
    }
    // A QML-lint criterion with neither an owned QML file nor a QML output
    // proven by the frozen dependency graph has no safe attribution boundary.
    // Preserve the historical fail-closed behavior.
    if (qmlScopeKeys.empty()) return QmlWarningScope::InTaskScope;

    std::optional<bool> lastSourceInScope;
    bool sawOutsideWarning = false;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        auto folded = lower(line);
        std::replace(folded.begin(), folded.end(), '\\', '/');
        const bool sourceLocation = folded.find(".qml:") != std::string::npos
            || folded.find(".qml(") != std::string::npos;
        if (sourceLocation) {
            lastSourceInScope = std::any_of(qmlScopeKeys.begin(), qmlScopeKeys.end(),
                [&](const std::string& key) { return folded.find(key) != std::string::npos; });
            if (!*lastSourceInScope) {
                // Some qmllint generators print only a basename. Accept that
                // attribution only when the basename is unique inside the
                // frozen task scope; duplicate basenames remain ambiguous and
                // therefore fail closed below.
                for (const auto& [basename, count] : qmlBasenameCounts) {
                    if (count == 1 && folded.find(basename) != std::string::npos) {
                        lastSourceInScope = true;
                        break;
                    }
                }
            }
        }

        const bool warningLine = folded.find("warning:") != std::string::npos
            || folded.find("[unqualified]") != std::string::npos;
        if (!warningLine) continue;
        if (!lastSourceInScope.has_value() || *lastSourceInScope) {
            return QmlWarningScope::InTaskScope;
        }
        sawOutsideWarning = true;
    }
    // The initial broad scan proved at least one warning exists. If it could
    // not be attributed by line parsing, fail closed instead of hiding it.
    return sawOutsideWarning ? QmlWarningScope::OutsideTaskScopeOnly
                             : QmlWarningScope::InTaskScope;
}

ManifestVerificationCheckResult commandCheck(
    const ExecutionAcceptanceRequirement& requirement,
    ManifestVerificationCommandRequest command,
    IManifestVerificationCommandRunner& runner,
    std::stop_token stopToken,
    const std::vector<fs::path>* qmlTaskVerificationScope = nullptr,
    // True when this QML-lint result is evidence for a baseline-aware delta
    // (a source-only baseline verification, or either side of the live
    // baseline/candidate comparison): structuredFindings then carries EVERY
    // parsable finding instead of the bounded excerpt sample, because two
    // independently truncated samples of the same output are not comparable.
    // Output with more findings than kMaximumComparableQmlFindings yields
    // verification.qml_findings_overflow (fail-closed) instead of a sample.
    bool completeQmlFindings = false)
{
    ManifestVerificationCheckResult check;
    check.profileId = requirement.id;
    check.kind = requirement.verificationKind;
    check.commandIdentity = command.commandIdentity;
    const auto result = runner.run(command, stopToken);
    check.exitCode = result.exitCode;
    check.durationMilliseconds = result.durationMilliseconds;
    check.boundedOutputHash = sha256Hex(result.boundedOutput);
    if (result.cancelled) {
        check.status = ExecutionVerificationStatus::Failed;
        check.diagnostics.push_back({"verification.cancelled",
            "Authorized verification command was cancelled", requirement.id});
    } else if (!result.started) {
        check.status = ExecutionVerificationStatus::NotVerifiable;
        check.diagnostics.push_back({
            result.errorCode.empty() ? "verification.tool_unavailable" : result.errorCode,
            "Authorized verification tool could not be started", requirement.id});
    } else if (result.timedOut) {
        check.status = ExecutionVerificationStatus::Failed;
        check.diagnostics.push_back({"verification.timeout",
            "Authorized verification command exceeded its time limit", requirement.id});
    } else if (result.outputTruncated) {
        check.status = ExecutionVerificationStatus::Failed;
        check.diagnostics.push_back({"verification.output_overflow",
            "Authorized verification output exceeded its byte limit", requirement.id});
    } else if (!result.exitCode.has_value() || *result.exitCode != 0) {
        check.status = ExecutionVerificationStatus::Failed;
        auto message = std::string(
            "Authorized verification command returned a non-zero exit status");
        auto excerpt = leakSafeDiagnosticExcerpt(
            result.boundedOutput, command.workingDirectory);
        if (!excerpt.text.empty()) message += "; diagnostic excerpt: " + excerpt.text;
        check.diagnostics.push_back({"verification.command_failed",
            std::move(message), requirement.id, std::move(excerpt.findings)});
    } else if (result.boundedOutput.find("CMAKE_OBJECT_PATH_MAX") != std::string::npos) {
        // Manifest verification reliability hardening: CMake itself can
        // exit 0 (a warning, not a configure error) while reporting that an
        // object file cannot be safely placed under the resolved build
        // directory - the exact class of failure a too-long candidate
        // overlay path produces. Never continue toward an actual build in
        // that case; fail closed with the same stable code the path-budget
        // preflight uses, before MSVC would otherwise fail with an opaque
        // "Cannot open compiler generated file" error.
        check.status = ExecutionVerificationStatus::Failed;
        check.diagnostics.push_back({"verification.overlay_path_too_long",
            "The verification candidate path exceeds a safe Windows/CMake path budget",
            requirement.id});
    } else {
        const auto warningScope = requirement.verificationKind
                == ExecutionVerificationKind::QmlLint
            ? classifyQmlWarningScope(result.boundedOutput, qmlTaskVerificationScope)
            : QmlWarningScope::None;
        check.status = warningScope == QmlWarningScope::InTaskScope
            ? ExecutionVerificationStatus::Failed
            : ExecutionVerificationStatus::Passed;
        if (warningScope == QmlWarningScope::InTaskScope) {
            auto message = std::string("QML lint emitted one or more warnings");
            auto excerpt = leakSafeDiagnosticExcerpt(
                result.boundedOutput, command.workingDirectory);
            if (!excerpt.text.empty()) message += "; diagnostic excerpt: " + excerpt.text;
            if (completeQmlFindings
                && excerpt.allFindings.size() > kMaximumComparableQmlFindings) {
                check.diagnostics.push_back({"verification.qml_findings_overflow",
                    "QML lint reported more findings than can be compared exactly against the "
                    "baseline", requirement.id});
            } else {
                check.diagnostics.push_back({"verification.qml_warning",
                    std::move(message), requirement.id,
                    completeQmlFindings ? std::move(excerpt.allFindings)
                                        : std::move(excerpt.findings)});
            }
        } else if (warningScope == QmlWarningScope::OutsideTaskScopeOnly) {
            // Preserve the same bounded, leak-safe structured provenance as
            // an in-scope warning even though this criterion passes. A later
            // candidate can contain both an in-scope warning and these exact
            // baseline-only warnings in one qmllint diagnostic; attribution
            // needs this evidence to prove the latter are pre-existing debt
            // instead of misclassifying them as candidate-introduced paths.
            auto excerpt = leakSafeDiagnosticExcerpt(
                result.boundedOutput, command.workingDirectory);
            if (completeQmlFindings
                && excerpt.allFindings.size() > kMaximumComparableQmlFindings) {
                // Carried on the still-Passed check: a baseline consumer sees an
                // unclassifiable diagnostic and treats the evidence as unavailable.
                check.diagnostics.push_back({"verification.qml_findings_overflow",
                    "QML lint reported more findings than can be compared exactly against the "
                    "baseline", requirement.id});
            } else {
                check.diagnostics.push_back({
                    "verification.qml_warning_outside_task_scope",
                    "QML lint warnings were confined to files outside this task's frozen verification scope",
                    requirement.id,
                    completeQmlFindings ? std::move(excerpt.allFindings)
                                        : std::move(excerpt.findings)});
            }
        }
    }
    return check;
}

ManifestVerificationCheckResult cmakeBuildCheck(
    const ExecutionAcceptanceRequirement& requirement,
    const fs::path& root,
    IManifestVerificationCommandRunner& runner,
    std::stop_token stopToken)
{
    ManifestVerificationCheckResult check;
    const auto preset = argument(requirement, "preset");
    const auto buildPreset = argument(requirement, "buildPreset");
    if (!preset.has_value() || !buildPreset.has_value()
        || !safeToken(*preset) || !safeToken(*buildPreset)) {
        check.profileId = requirement.id;
        check.kind = requirement.verificationKind;
        check.status = ExecutionVerificationStatus::NotVerifiable;
        check.commandIdentity = "cmake_configure_build";
        check.diagnostics.push_back({"verification.invalid_profile_arguments",
            "CMake build profile arguments are missing or invalid", requirement.id});
    } else if (const auto budgetCheck = overlayPathBudgetCheck(
            requirement, root, argument(requirement, "buildDirectory"),
            "cmake.configure.preset");
        budgetCheck.has_value()) {
        check = *budgetCheck;
    } else {
        ManifestVerificationCommandRequest configure;
        configure.commandIdentity = "cmake.configure.preset";
        configure.executable = "cmake";
        configure.arguments = {"--preset", *preset};
        configure.workingDirectory = root;
        check = commandCheck(requirement, configure, runner, stopToken);
        if (check.status == ExecutionVerificationStatus::Passed) {
            const auto configureHash = check.boundedOutputHash;
            const auto configureDuration = check.durationMilliseconds;
            ManifestVerificationCommandRequest build;
            build.commandIdentity = "cmake.build.preset";
            build.executable = "cmake";
            build.arguments = {"--build", "--preset", *buildPreset};
            build.workingDirectory = root;
            check = commandCheck(requirement, build, runner, stopToken);
            check.commandIdentity = "cmake.configure.preset+cmake.build.preset";
            check.boundedOutputHash = sha256Hex(
                configureHash + "\n" + check.boundedOutputHash);
            check.durationMilliseconds += configureDuration;
        }
    }
    return check;
}

// Baseline-aware counterpart of commandCheck()'s ordinary QmlLint handling.
// It is used for every QML-lint criterion when a source-only baseline overlay
// is supplied, regardless of whether the owning task has a static .qml scope.
// Candidate and baseline use the identical command and the one shared
// multiset resolver. Baseline CMake preparation is lazy, because a clean
// candidate needs no comparison at all.
ManifestVerificationCheckResult qmlLintBaselineDeltaCheck(
    const ExecutionAcceptanceRequirement& requirement,
    const ManifestVerificationCommandRequest& candidateCommand,
    const fs::path& baselineRoot,
    IManifestVerificationCommandRunner& runner,
    std::stop_token stopToken,
    const std::unordered_set<std::string>& manifestOwnedNormalizedPaths,
    const std::function<bool()>& ensureBaselinePrerequisite,
    const ManifestProposalSetVerification* precomputedBaseline)
{
    // Both sides are compared on their COMPLETE finding sets. Comparing the
    // bounded excerpt samples is unsound: a baseline with many warnings is
    // truncated to a few per file, while a candidate that fixed most of them
    // fits entirely, so its retained warnings in untouched files looked "new"
    // (Runs 030ce232 / 9a344ee3: a better fix made the Run MORE likely to fail).
    auto candidateCheck = commandCheck(
        requirement, candidateCommand, runner, stopToken, nullptr, true);
    if (candidateCheck.status != ExecutionVerificationStatus::Failed
        || candidateCheck.diagnostics.size() != 1
        || candidateCheck.diagnostics.front().code != "verification.qml_warning"
        || candidateCheck.diagnostics.front().structuredFindings.empty()) {
        // Not a genuine, safely-evidenced warning-bearing candidate exit
        // (Passed; a real infrastructure failure with its own distinct
        // diagnostic; or a warning-bearing exit whose output carried no
        // parseable file:line:column evidence at all) - nothing this
        // hardening can safely reclassify. Stays exactly as fail-closed as
        // before this change.
        return candidateCheck;
    }
    std::vector<ManifestVerificationDiagnosticFinding> candidateFindings =
        candidateCheck.diagnostics.front().structuredFindings;

    bool baselineEvidenceAvailable = false;
    std::vector<ManifestVerificationDiagnosticFinding> baselineFindings;
    if (precomputedBaseline != nullptr) {
        const auto check = std::find_if(precomputedBaseline->checks.begin(),
            precomputedBaseline->checks.end(), [&](const ManifestVerificationCheckResult& item) {
                return item.profileId == requirement.id
                    && item.kind == ExecutionVerificationKind::QmlLint;
            });
        if (check != precomputedBaseline->checks.end()
            && (check->status == ExecutionVerificationStatus::Passed
                || check->status == ExecutionVerificationStatus::Failed)) {
            bool sawUnclassifiableDiagnostic = false;
            for (const auto& diagnostic : precomputedBaseline->diagnostics) {
                if (!diagnostic.requirementId.has_value()
                    || *diagnostic.requirementId != requirement.id) continue;
                if ((diagnostic.code == "verification.qml_warning"
                        || diagnostic.code == "verification.qml_warning_outside_task_scope")
                    && !diagnostic.structuredFindings.empty()) {
                    baselineFindings.insert(baselineFindings.end(),
                        diagnostic.structuredFindings.begin(),
                        diagnostic.structuredFindings.end());
                } else {
                    sawUnclassifiableDiagnostic = true;
                }
            }
            baselineEvidenceAvailable = !sawUnclassifiableDiagnostic
                && (check->status == ExecutionVerificationStatus::Passed
                    || !baselineFindings.empty());
        }
    } else {
        if (!ensureBaselinePrerequisite()) {
            candidateCheck.diagnostics.push_back({"verification.qml_baseline_unavailable",
                "QML lint baseline evidence is unavailable because its required build prerequisite did not pass",
                requirement.id});
            return candidateCheck;
        }

        ManifestVerificationCommandRequest baselineCommand = candidateCommand;
        baselineCommand.workingDirectory = baselineRoot;
        const auto baselineCheck = commandCheck(
            requirement, baselineCommand, runner, stopToken, nullptr, true);
        const bool baselinePassedClean =
            baselineCheck.status == ExecutionVerificationStatus::Passed;
        const bool baselinePassedWithFindings =
            baselineCheck.status == ExecutionVerificationStatus::Failed
            && baselineCheck.diagnostics.size() == 1
            && baselineCheck.diagnostics.front().code == "verification.qml_warning"
            && !baselineCheck.diagnostics.front().structuredFindings.empty();
        baselineEvidenceAvailable = baselinePassedClean || baselinePassedWithFindings;
        if (baselinePassedWithFindings) {
            baselineFindings = baselineCheck.diagnostics.front().structuredFindings;
        }
    }

    if (!baselineEvidenceAvailable) {
        candidateCheck.diagnostics.push_back({"verification.qml_baseline_unavailable",
            "QML lint baseline evidence could not be classified as a clean or warning-bearing result",
            requirement.id});
        return candidateCheck;
    }

    const auto frozenPolicyId = requirement.qmlLintDiagnosticPolicyId.has_value()
        ? std::optional<std::string_view>(*requirement.qmlLintDiagnosticPolicyId)
        : std::nullopt;
    const auto delta = resolveQmlLintCriterionDelta(baselineFindings,
        baselineEvidenceAvailable, candidateFindings, manifestOwnedNormalizedPaths,
        frozenPolicyId);

    ManifestVerificationCheckResult check;
    check.profileId = requirement.id;
    check.kind = requirement.verificationKind;
    check.commandIdentity = candidateCheck.commandIdentity;
    check.exitCode = candidateCheck.exitCode;
    check.boundedOutputHash = candidateCheck.boundedOutputHash;
    check.durationMilliseconds = candidateCheck.durationMilliseconds;
    check.status = delta.passed
        ? ExecutionVerificationStatus::Passed
        : ExecutionVerificationStatus::Failed;

    std::vector<ManifestVerificationDiagnosticFinding> debtFindings;
    std::vector<ManifestVerificationDiagnosticFinding> inScopeHardFindings;
    std::vector<ManifestVerificationDiagnosticFinding> inScopeAdvisoryFindings;
    std::vector<ManifestVerificationDiagnosticFinding> outsideManifestFindings;
    for (const auto& entry : delta.entries) {
        switch (entry.provenance) {
        case QmlLintFindingProvenance::PreExistingVerificationDebt:
            debtFindings.push_back(entry.finding);
            break;
        case QmlLintFindingProvenance::CandidateIntroducedInScope:
            // QML-lint advisory-severity policy (2026-09-28): only a
            // Manifest-owned, in-scope finding can ever be Advisory; split it
            // into its own durable, non-blocking diagnostic rather than the
            // blocking bucket below. See QmlLintCriterionDelta::passed's own
            // doc comment for the matching pass/fail carve-out this mirrors.
            (entry.severity == QmlLintFindingSeverity::Advisory
                ? inScopeAdvisoryFindings
                : inScopeHardFindings)
                .push_back(entry.finding);
            break;
        case QmlLintFindingProvenance::Unattributable:
            // Unattributable evidence is never guessed into a specific
            // bucket the operator could mistake for a proven classification -
            // including never into the Advisory bucket, regardless of the
            // finding's own tag - folding it into the same blocking
            // diagnostic as a genuine in-scope Hard finding keeps it
            // fail-closed without inventing a false path attribution.
            inScopeHardFindings.push_back(entry.finding);
            break;
        case QmlLintFindingProvenance::CandidateIntroducedOutsideManifest:
            outsideManifestFindings.push_back(entry.finding);
            break;
        case QmlLintFindingProvenance::ResolvedBaselineDebt:
            break; // resolveQmlLintCriterionDelta() never emits this kind
        }
    }
    // The pass/fail decision above was made on the complete sets. What is
    // persisted/displayed stays bounded per diagnostic (same per-file sample
    // the excerpt uses), so a large project cannot bloat the durable record.
    if (!debtFindings.empty()) {
        check.diagnostics.push_back({"verification.qml_warning_baseline_debt",
            "QML lint warnings are confined to unchanged pre-existing baseline debt",
            requirement.id, boundedFindingsPerFile(std::move(debtFindings))});
    }
    if (!inScopeHardFindings.empty()) {
        check.diagnostics.push_back({"verification.qml_warning",
            "QML lint emitted one or more warnings", requirement.id,
            boundedFindingsPerFile(std::move(inScopeHardFindings))});
    }
    if (!inScopeAdvisoryFindings.empty()) {
        // Stable diagnostic code, current-policy compatibility marker (see
        // usesBaselineAwareQmlLintPolicy). Always attached even when the
        // check itself is Passed (delta.passed above already accounts for
        // these entries never forcing Failed) - an advisory finding is
        // durable and operator-visible, never silently dropped merely
        // because it did not block anything.
        check.diagnostics.push_back({"verification.qml_warning_advisory",
            "QML lint emitted one or more advisory-severity warnings that do not block this "
            "check", requirement.id,
            boundedFindingsPerFile(std::move(inScopeAdvisoryFindings))});
    }
    if (!outsideManifestFindings.empty()) {
        check.diagnostics.push_back({"verification.qml_warning_outside_manifest",
            "QML lint reported a new warning outside the frozen Manifest's write scope",
            requirement.id, boundedFindingsPerFile(std::move(outsideManifestFindings))});
    }
    return check;
}

ManifestVerificationCheckResult internalCheck(
    const ExecutionAcceptanceRequirement& requirement,
    const fs::path& overlayRoot)
{
    ManifestVerificationCheckResult check;
    check.profileId = requirement.id;
    check.kind = requirement.verificationKind;
    check.commandIdentity = toString(requirement.verificationKind);
    check.status = ExecutionVerificationStatus::Passed;
    if (requirement.verificationKind == ExecutionVerificationKind::InspectPaths) {
        for (const auto& output : requirement.requiredOutputs) {
            std::error_code error;
            if (!safeRelative(output) || !fs::exists(detail::longPath(overlayRoot / detail::pathFromUtf8(output)), error) || error) {
                check.status = ExecutionVerificationStatus::Failed;
                check.diagnostics.push_back({"verification.required_output_missing",
                    "A required output is absent from the combined candidate", requirement.id});
                break;
            }
        }
        check.boundedOutputHash = sha256Hex(requirement.id + ":inspect_paths");
        return check;
    }

    const auto definition = argument(requirement, "definitionFile");
    const auto registration = argument(requirement, "registrationFile");
    const auto target = argument(requirement, "target");
    if (!definition.has_value() || !registration.has_value() || !target.has_value()
        || !safeRelative(*definition) || !safeRelative(*registration)
        || !safeToken(*target) || *definition == *registration) {
        // Manifest verification reliability hardening: this internal check
        // remains a literal substring search (see below) and is retained
        // only for legacy/trusted requirements that predate the fail-closed
        // rejection of new cmake_target_consistency submissions in
        // ExecutionManifestParser - a substring coincidence inside a
        // comment or an unrelated word is never strong proof of target
        // identity, and definitionFile/registrationFile naming the same
        // file twice would trivially "pass" without checking anything.
        check.status = ExecutionVerificationStatus::NotVerifiable;
        check.diagnostics.push_back({"verification.invalid_profile_arguments",
            "CMake consistency profile arguments are missing, invalid, or identical",
            requirement.id});
        return check;
    }
    auto read = [](const fs::path& path) -> std::optional<std::string> {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error || size > 1024 * 1024) return std::nullopt;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return std::nullopt;
        std::string content(static_cast<std::size_t>(size), '\0');
        if (!content.empty()) stream.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream && !content.empty()) return std::nullopt;
        return content;
    };
    const auto left = read(overlayRoot / *definition);
    const auto right = read(overlayRoot / *registration);
    if (!left.has_value() || !right.has_value()) {
        check.status = ExecutionVerificationStatus::NotVerifiable;
        check.diagnostics.push_back({"verification.input_unavailable",
            "CMake consistency input could not be read", requirement.id});
        return check;
    }
    check.boundedOutputHash = sha256Hex(*left + "\n" + *right);
    if (left->find(*target) == std::string::npos
        || right->find(*target) == std::string::npos) {
        check.status = ExecutionVerificationStatus::Failed;
        check.diagnostics.push_back({"verification.cmake_target_mismatch",
            "The expected CMake target is not consistent across both files", requirement.id});
    }
    return check;
}

} // namespace

ManifestVerificationCommandOutcome SystemManifestVerificationCommandRunner::run(
    const ManifestVerificationCommandRequest& request,
    std::stop_token stopToken)
{
    ManifestVerificationCommandOutcome outcome;
    const auto startedAt = std::chrono::steady_clock::now();
#ifdef _WIN32
    if (request.executable != "cmake" || request.arguments.size() > 16
        || request.maxOutputBytes == 0) {
        outcome.errorCode = "verification.command_denied";
        return outcome;
    }
    auto quote = [](const std::wstring& value) {
        std::wstring result = L"\"";
        std::size_t slashes = 0;
        for (const wchar_t ch : value) {
            if (ch == L'\\') { ++slashes; continue; }
            if (ch == L'\"') {
                result.append(slashes * 2 + 1, L'\\');
                result.push_back(ch);
                slashes = 0;
                continue;
            }
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
        result.append(slashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    };
    auto widen = [](const std::string& value) {
        if (value.empty()) return std::wstring {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return std::wstring {};
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count);
        return result;
    };
    std::wstring command = L"cmake";
    for (const auto& argumentValue : request.arguments)
        command += L" " + quote(widen(argumentValue));

    SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)
        || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        outcome.errorCode = "verification.pipe_failed";
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        return outcome;
    }
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process {};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const auto cwd = request.workingDirectory.wstring();
    // The MSVC / Windows SDK environment is prepared for this child only (AG-18.4B-R1); when it cannot be prepared the child
    // keeps the application's own environment exactly as before, and the readiness check reports the reason by name.
    // Only the commands that configure or build need it: listing presets (or asking for a version) must work, and must not
    // wait for Visual Studio, on a computer that has none.
    const bool needsToolchain = std::none_of(request.arguments.begin(), request.arguments.end(),
        [](const std::string& argumentValue) {
            return argumentValue.rfind("--list-presets", 0) == 0 || argumentValue == "--version"
                || argumentValue == "--help" || argumentValue == "-E" || argumentValue == "-P";
        });
    const auto toolchain = needsToolchain ? VerificationToolchain::current() : VerificationToolchainEnvironment {};
    LPVOID childEnvironment = nullptr;
    DWORD creationFlags = CREATE_NO_WINDOW;
    if (toolchain.prepared && !toolchain.inherited && !toolchain.environmentBlock.empty()) {
        childEnvironment = const_cast<wchar_t*>(toolchain.environmentBlock.data());
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr,
        nullptr, TRUE, creationFlags, childEnvironment, cwd.c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        outcome.errorCode = "verification.process_start_failed";
        return outcome;
    }
    outcome.started = true;
    auto drainAvailableOutput = [&]() {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)
                || available == 0) {
                break;
            }
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(readPipe, buffer,
                    (std::min)(available, static_cast<DWORD>(sizeof(buffer))),
                    &read, nullptr) || read == 0) {
                break;
            }
            const auto remaining = request.maxOutputBytes
                - (std::min)(request.maxOutputBytes, outcome.boundedOutput.size());
            const auto keep = (std::min)(remaining, static_cast<std::size_t>(read));
            outcome.boundedOutput.append(buffer, keep);
            if (keep != read) outcome.outputTruncated = true;
        }
    };
    bool done = false;
    while (!done) {
        drainAvailableOutput();
        if (stopToken.stop_requested()) {
            outcome.cancelled = true;
            TerminateProcess(process.hProcess, 2);
            done = true;
        } else if (std::chrono::steady_clock::now() - startedAt > request.timeout) {
            outcome.timedOut = true;
            TerminateProcess(process.hProcess, 3);
            done = true;
        } else {
            const auto wait = WaitForSingleObject(process.hProcess, 25);
            done = wait == WAIT_OBJECT_0;
        }
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    // A short-lived process can publish its entire output immediately before
    // signalling completion. Drain once more after all inherited write handles
    // have closed so successful probes cannot be mistaken for empty output.
    drainAvailableOutput();
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.hProcess, &exitCode))
        outcome.exitCode = static_cast<int>(exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
#else
    static_cast<void>(request);
    static_cast<void>(stopToken);
    outcome.errorCode = "verification.process_runner_unavailable";
#endif
    outcome.durationMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count());
    return outcome;
}

common::Result<ManifestProposalSetVerification> ManifestProposalSetVerifier::verify(
    const ManifestProposalSetVerifierRequest& request,
    ChangeProposalService& proposals,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken)
{
    if (request.baselineOverlay != nullptr
        && (request.baselineOverlay->workspaceIdentity != request.overlay.workspaceIdentity
            || request.baselineOverlay->baselineFingerprint
                != request.overlay.baselineFingerprint
            || !request.baselineOverlay->orderedProposalIds.empty()
            || !request.baselineOverlay->orderedProposalContentHashes.empty())) {
        return fail<ManifestProposalSetVerification>(
            "verification.baseline_identity_mismatch",
            "QML lint baseline evidence is not the source-only overlay for this candidate identity");
    }
    if (request.baselineVerification != nullptr
        && (request.baselineOverlay == nullptr
            || request.baselineVerification->workspaceIdentity
                != request.baselineOverlay->workspaceIdentity
            || request.baselineVerification->baselineFingerprint
                != request.baselineOverlay->baselineFingerprint
            || request.baselineVerification->overlayFingerprint
                != request.baselineOverlay->overlayFingerprint
            || !request.baselineVerification->orderedProposalIds.empty()
            || !request.baselineVerification->orderedProposalContentHashes.empty())) {
        return fail<ManifestProposalSetVerification>(
            "verification.baseline_identity_mismatch",
            "Precomputed QML lint baseline evidence does not match its source-only overlay");
    }

    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = request.overlay.workspaceIdentity;
    verification.baselineFingerprint = request.overlay.baselineFingerprint;
    verification.orderedProposalIds = request.overlay.orderedProposalIds;
    verification.orderedProposalContentHashes =
        request.overlay.orderedProposalContentHashes;
    verification.overlayFingerprint = request.overlay.overlayFingerprint;

    // Combined safety check: the same target/source path may not be operated
    // by two different final proposals, even if each proposal was valid alone.
    std::unordered_map<std::string, ChangeSetId> claimed;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    for (const auto& proposalId : request.overlay.orderedProposalIds) {
        const auto changes = proposals.listChanges(proposalId);
        if (!changes.success()) return fail<ManifestProposalSetVerification>(
            "verification.proposal_unavailable",
            "Combined verifier could not load a proposal's operations");
        for (const auto& change : changes.value()) {
            std::vector<fs::path> paths {change.relativePath};
            if (change.sourcePath.has_value()) paths.push_back(*change.sourcePath);
            for (const auto& path : paths) {
                const auto key = detail::normalizedPathKey(path);
                const auto [found, inserted] = claimed.emplace(key, proposalId);
                if (!inserted && found->second != proposalId) {
                    verification.proposalSafetyStatus =
                        ExecutionVerificationStatus::Failed;
                    verification.diagnostics.push_back({
                        "verification.cross_proposal_conflict",
                        "Two final proposals claim the same candidate path", std::nullopt});
                }
            }
        }
    }

    bool anyBuildCheck = false;
    bool buildFailed = false;
    bool buildUnknown = false;

    ExecutionManifest scopeManifest;
    scopeManifest.tasks = request.tasks;
    const auto dependencyGraph = deriveManifestDependencyGraph(scopeManifest);

    // Post-Stage-3 QML-lint hardening: every path any task in the frozen
    // Manifest owns - the sole authority qmlLintBaselineDeltaCheck()
    // consults for CandidateIntroducedInScope vs
    // CandidateIntroducedOutsideManifest. Computed lazily, at most once, and
    // only for a baseline-aware Run whose candidate actually emits safely
    // parsed QML warnings. It never widens the Manifest write scope.
    std::optional<std::unordered_set<std::string>> manifestOwnedNormalizedPaths;
    const auto ensureManifestOwnedNormalizedPaths = [&]() -> const std::unordered_set<std::string>& {
        if (!manifestOwnedNormalizedPaths.has_value()) {
            std::unordered_set<std::string> owned;
            for (const auto& task : request.tasks) {
                for (const auto& path : task.ownedPaths) {
                    owned.insert(detail::normalizedPathKey(path));
                }
                for (const auto& path : task.plannedCreatePaths) {
                    owned.insert(detail::normalizedPathKey(path));
                }
            }
            manifestOwnedNormalizedPaths = std::move(owned);
        }
        return *manifestOwnedNormalizedPaths;
    };

    // Configure/build is the candidate's prerequisite, never a
    // relocated workspace cache. Run every CMakeBuild profile first while
    // retaining the original criterion/check ordering below. A required
    // configure/build failure makes all QML lint checks explicitly NotRun.
    std::unordered_map<const ExecutionAcceptanceRequirement*,
        ManifestVerificationCheckResult> cmakeBuildChecks;
    bool hasRequiredCMakeBuild = false;
    bool requiredCMakeBuildReady = true;
    for (const auto& task : request.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements) {
            if (requirement.verificationKind
                != ExecutionVerificationKind::CMakeBuild) continue;
            auto check = cmakeBuildCheck(
                requirement, request.overlay.root, commandRunner, stopToken);
            if (requirement.required) {
                hasRequiredCMakeBuild = true;
                requiredCMakeBuildReady = requiredCMakeBuildReady
                    && check.status == ExecutionVerificationStatus::Passed;
            }
            cmakeBuildChecks.emplace(&requirement, std::move(check));
        }
    }

    // Baseline configuration/build is intentionally lazy: it is needed only
    // if a candidate QML-lint command actually emits safely parsed warnings.
    // One preparation is shared by every QML-lint criterion in this verify()
    // call. A failed or unavailable required prerequisite never becomes
    // guessed baseline evidence.
    bool baselinePrerequisiteEvaluated = false;
    bool baselinePrerequisiteReady = false;
    const auto ensureBaselinePrerequisite = [&]() {
        if (baselinePrerequisiteEvaluated) return baselinePrerequisiteReady;
        baselinePrerequisiteEvaluated = true;
        if (request.baselineOverlay == nullptr) return false;
        baselinePrerequisiteReady = true;
        for (const auto& task : request.tasks) {
            if (!task.acceptanceContract.has_value()) continue;
            for (const auto& requirement : task.acceptanceContract->requirements) {
                if (requirement.verificationKind != ExecutionVerificationKind::CMakeBuild)
                    continue;
                const auto check = cmakeBuildCheck(requirement,
                    request.baselineOverlay->root, commandRunner, stopToken);
                if (requirement.required
                    && check.status != ExecutionVerificationStatus::Passed) {
                    baselinePrerequisiteReady = false;
                }
            }
        }
        return baselinePrerequisiteReady;
    };
    for (const auto& task : request.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        const auto contract = validateExecutionAcceptanceContract(
            *task.acceptanceContract);
        if (!contract.success()) return fail<ManifestProposalSetVerification>(
            contract.error()->code, contract.error()->message);
        for (const auto& requirement : task.acceptanceContract->requirements) {
            ManifestVerificationCheckResult check;
            if (requirement.verificationKind
                != ExecutionVerificationKind::InspectPaths) anyBuildCheck = true;
            if (requirement.verificationKind == ExecutionVerificationKind::InspectPaths
                || requirement.verificationKind
                    == ExecutionVerificationKind::CMakeTargetConsistency) {
                check = internalCheck(requirement, request.overlay.root);
            } else if (requirement.verificationKind
                == ExecutionVerificationKind::CMakeBuild) {
                anyBuildCheck = true;
                check = std::move(cmakeBuildChecks.at(&requirement));
            } else {
                anyBuildCheck = true;
                const auto directory = argument(requirement, "buildDirectory");
                const auto target = argument(requirement, "target");
                if (hasRequiredCMakeBuild && !requiredCMakeBuildReady) {
                    check.profileId = requirement.id;
                    check.kind = requirement.verificationKind;
                    check.status = ExecutionVerificationStatus::NotRun;
                    check.commandIdentity = "cmake.qml_lint_target";
                    check.diagnostics.push_back({"verification.prerequisite_failed",
                        "QML lint was not run because clean configure/build did not pass",
                        requirement.id});
                } else if (!directory.has_value() || !target.has_value()
                    || !safeRelative(*directory) || !safeToken(*target)) {
                    check.profileId = requirement.id;
                    check.kind = requirement.verificationKind;
                    check.status = ExecutionVerificationStatus::NotVerifiable;
                    check.commandIdentity = "cmake.qml_lint_target";
                    check.diagnostics.push_back({"verification.invalid_profile_arguments",
                        "QML lint profile arguments are missing or invalid", requirement.id});
                } else if (const auto budgetCheck = overlayPathBudgetCheck(
                        requirement, request.overlay.root, directory, "cmake.qml_lint_target");
                    budgetCheck.has_value()) {
                    check = *budgetCheck;
                } else {
                    ManifestVerificationCommandRequest lint;
                    lint.commandIdentity = "cmake.qml_lint_target";
                    lint.executable = "cmake";
                    lint.arguments = {"--build", *directory, "--target", *target};
                    lint.workingDirectory = request.overlay.root;
                    auto qmlTaskVerificationScope = qmlVerificationScopeForTask(
                        task, dependencyGraph);
                    if (request.baselineOverlay != nullptr) {
                        check = qmlLintBaselineDeltaCheck(requirement, lint,
                            request.baselineOverlay->root, commandRunner, stopToken,
                            ensureManifestOwnedNormalizedPaths(), ensureBaselinePrerequisite,
                            request.baselineVerification);
                    } else {
                        // A verification of an overlay with NO proposals is the
                        // source-only baseline evidence that a later
                        // baseline-aware candidate verification and the
                        // attribution service compare against (see
                        // ManifestProposalSetVerifierRequest::baselineVerification),
                        // so it must carry complete findings, not an excerpt sample.
                        const bool isBaselineEvidence =
                            request.overlay.orderedProposalIds.empty();
                        check = commandCheck(requirement, lint, commandRunner, stopToken,
                            &qmlTaskVerificationScope, isBaselineEvidence);
                    }
                }
            }
            if (check.status == ExecutionVerificationStatus::Failed) buildFailed = true;
            if (check.status == ExecutionVerificationStatus::NotVerifiable
                || check.status == ExecutionVerificationStatus::NotRun) buildUnknown = true;
            for (const auto& diagnostic : check.diagnostics)
                verification.diagnostics.push_back(diagnostic);
            // boundedOutputHash is empty for a check that never actually ran
            // a command (invalid/missing profile arguments caught before any
            // command was attempted - see internalCheck()'s
            // CMakeTargetConsistency branch and the CMakeBuild/QmlLint
            // argument-validation branches above). NotVerifiable genuinely
            // has no evidence to reference; wrapping an empty hash in a
            // single-element evidenceReferences entry produced a blank
            // reference that validateManifestProposalSetVerification
            // correctly rejects as verification.invalid_evidence_reference -
            // which hard-failed the entire verify() call (losing every
            // diagnostic collected so far) instead of the check simply
            // reporting NotVerifiable as intended.
            std::vector<std::string> evidenceReferences;
            if (!check.boundedOutputHash.empty()) {
                evidenceReferences.push_back(check.boundedOutputHash);
            }
            verification.criteria.push_back({requirement.id, requirement.required,
                check.status, std::move(evidenceReferences)});
            verification.checks.push_back(std::move(check));
        }
    }
    verification.buildLintTestStatus = !anyBuildCheck
        ? ExecutionVerificationStatus::Passed
        : buildFailed ? ExecutionVerificationStatus::Failed
        : buildUnknown ? ExecutionVerificationStatus::NotVerifiable
                       : ExecutionVerificationStatus::Passed;

    bool requiredFailed = false;
    bool requiredUnknown = false;
    for (const auto& result : verification.criteria) {
        if (!result.required) continue;
        requiredFailed = requiredFailed
            || result.status == ExecutionVerificationStatus::Failed;
        requiredUnknown = requiredUnknown
            || result.status == ExecutionVerificationStatus::NotRun
            || result.status == ExecutionVerificationStatus::NotVerifiable;
    }
    verification.acceptanceStatus = requiredFailed
        ? ExecutionVerificationStatus::Failed
        : requiredUnknown ? ExecutionVerificationStatus::NotVerifiable
                          : ExecutionVerificationStatus::Passed;
    if (verification.criteria.empty()) {
        verification.acceptanceStatus = ExecutionVerificationStatus::NotVerifiable;
        verification.diagnostics.push_back({"verification.acceptance_unmapped",
            "No structured acceptance requirements were available", std::nullopt});
    }
    const auto valid = validateManifestProposalSetVerification(verification);
    if (!valid.success()) return fail<ManifestProposalSetVerification>(
        valid.error()->code, valid.error()->message);
    return common::Result<ManifestProposalSetVerification>::ok(
        std::move(verification));
}

} // namespace ai::application
