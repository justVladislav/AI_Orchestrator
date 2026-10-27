#include "ai/application/ProjectContextSnapshot.hpp"

#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <set>
#include <utility>

namespace ai::application {

namespace {

constexpr std::array<ProjectContextCategory, 5> kCanonicalOrder {
    ProjectContextCategory::ProjectMetadata,
    ProjectContextCategory::SelectedLineage,
    ProjectContextCategory::WorkLedger,
    ProjectContextCategory::WorkspaceCatalog,
    ProjectContextCategory::ProjectMemory,
};

// The order in which whole tail items are dropped when the sections together exceed the total. The project
// metadata and the selected lineage are deliberately absent: they are bounded only by their own limits.
constexpr std::array<ProjectContextCategory, 3> kCutOrder {
    ProjectContextCategory::ProjectMemory,
    ProjectContextCategory::WorkLedger,
    ProjectContextCategory::WorkspaceCatalog,
};

constexpr std::size_t kMaximumKindBytes = 64;

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

std::size_t canonicalIndex(ProjectContextCategory category) noexcept
{
    for (std::size_t index = 0; index < kCanonicalOrder.size(); ++index) {
        if (kCanonicalOrder[index] == category) return index;
    }
    return kCanonicalOrder.size();
}

// CRLF and lone CR become LF; trailing newlines are dropped so an item never ends a section with a blank line.
std::string normalizeText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            out.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
        } else {
            out.push_back(text[index]);
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

bool validUtf8(std::string_view text) noexcept
{
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t minimum = 0;
        std::uint32_t code = 0;
        if (lead < 0x80) {
            ++index;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            minimum = 0x80;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            minimum = 0x800;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            minimum = 0x10000;
            code = lead & 0x07;
        } else {
            return false;
        }
        if (index + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
        index += length;
    }
    return true;
}

bool safeKind(std::string_view kind) noexcept
{
    return !kind.empty() && kind.size() <= kMaximumKindBytes
        && std::all_of(kind.begin(), kind.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
           });
}

bool isBlank(std::string_view text) noexcept
{
    return std::all_of(text.begin(), text.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; });
}

bool hasControlCharacter(std::string_view text) noexcept
{
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch == 0 || (ch < 0x20 && ch != '\n' && ch != '\t');
    });
}

// Strict single-line text for identities and digests: any control character (NUL, newline, tab, DEL) is
// rejected, unlike multi-line prose where newline and tab are legitimate.
bool hasAnyControlCharacter(std::string_view text) noexcept
{
    return std::any_of(text.begin(), text.end(),
        [](unsigned char ch) { return ch < 0x20 || ch == 0x7F; });
}

bool validSingleLine(std::string_view value, std::size_t maximumBytes, bool allowEmpty) noexcept
{
    if (value.empty()) return allowEmpty;
    return value.size() <= maximumBytes && validUtf8(value) && !hasAnyControlCharacter(value)
        && std::isspace(static_cast<unsigned char>(value.front())) == 0
        && std::isspace(static_cast<unsigned char>(value.back())) == 0;
}

// Letters, digits and every non-ASCII byte belong to a word: "Dannye/Proekt" in any script is a relative
// path, "and/or" is prose; neither starts at a token boundary.
bool wordByte(char ch) noexcept
{
    const auto byte = static_cast<unsigned char>(ch);
    return byte >= 0x80 || std::isalnum(byte) != 0;
}

bool equalsLowerAscii(std::string_view text, std::string_view lowerCase) noexcept
{
    if (text.size() != lowerCase.size()) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(text[index])) != static_cast<unsigned char>(lowerCase[index])) {
            return false;
        }
    }
    return true;
}

// Defence in depth: a loader must already have dropped path-shaped free text; this is the last line. An
// absolute path is recognised by SHAPE, whatever characters its segments use:
//   - a drive path ("C:\x", "d:/x");
//   - a file: URI ("file:///home/u", "file://host/share");
//   - a rooted path: a slash or backslash at a token boundary that opens a segment ("/data", "/@x",
//     "/<non-ASCII name>", "\Windows", "\\server", "\\?\C:\x", "\\.\pipe\x");
//   - a home path ("~/x", "~\x").
// Not paths: URL separators ("http://x/y"), relative "./" and "../", words such as "and/or", a spaced
// division slash ("a / b"), and relative paths in any script.
bool hasAbsolutePathShape(std::string_view text) noexcept
{
    for (std::size_t index = 0; index + 2 < text.size(); ++index) {
        // "C:\" or "C:/" that does not continue a word (so "http://" is not a drive).
        if (std::isalpha(static_cast<unsigned char>(text[index])) != 0 && text[index + 1] == ':'
            && (text[index + 2] == '\\' || text[index + 2] == '/')
            && (index == 0 || !wordByte(text[index - 1]))) {
            return true;
        }
    }
    for (std::size_t index = 0; index + 7 <= text.size(); ++index) {
        if (equalsLowerAscii(text.substr(index, 7), "file://")
            && (index == 0 || !wordByte(text[index - 1]))) {
            return true;
        }
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        if (current != '/' && current != '\\' && current != '~') continue;
        const char before = index == 0 ? '\0' : text[index - 1];
        const char after = index + 1 < text.size() ? text[index + 1] : '\0';
        const bool boundary = index == 0
            || !(wordByte(before) || before == '.' || before == '_' || before == '-' || before == '/'
                 || before == '\\' || before == ':' || before == '~');
        if (!boundary) continue;
        if (current == '~') {
            if (after == '/' || after == '\\') return true;
            continue;
        }
        if (after != '\0' && std::isspace(static_cast<unsigned char>(after)) == 0) return true;
    }
    return false;
}

constexpr std::size_t kMaximumIdentityBytes = 256;
constexpr std::size_t kMaximumSourceIdBytes = 1'024;
constexpr std::size_t kMaximumDigestBytes = 256;
constexpr std::size_t kMaximumItemTextBytes = 64 * 1'024;

struct PreparedSection {
    ProjectContextCategory category {ProjectContextCategory::ProjectMetadata};
    std::vector<ProjectContextItem> items;     // normalized text, category stamped on the source
    std::vector<std::size_t> optional;         // indices of non-mandatory items, in supplied order
    std::uint64_t limit {0};
    std::size_t keep {0};                      // how many leading optional items are included
    bool fitsHeading {true};
    std::string text;
    std::vector<ProjectContextSourceRef> sources;
    std::uint64_t tokens {0};
    std::uint32_t included {0};
};

std::string renderSection(const PreparedSection& section, std::size_t keep)
{
    std::set<std::size_t> chosen(section.optional.begin(),
        section.optional.begin() + static_cast<std::ptrdiff_t>(keep));
    std::string text(projectContextSectionHeading(section.category));
    for (std::size_t index = 0; index < section.items.size(); ++index) {
        if (section.items[index].mandatory || chosen.contains(index)) {
            text += '\n';
            text += section.items[index].text;
        }
    }
    return text;
}

std::uint64_t tokensFor(const PreparedSection& section, std::size_t keep)
{
    return estimateChatTokens(renderSection(section, keep));
}

void finalizeSection(PreparedSection& section)
{
    section.text = renderSection(section, section.keep);
    section.tokens = estimateChatTokens(section.text);
    section.sources.clear();
    section.included = 0;
    std::set<std::size_t> chosen(section.optional.begin(),
        section.optional.begin() + static_cast<std::ptrdiff_t>(section.keep));
    for (std::size_t index = 0; index < section.items.size(); ++index) {
        if (section.items[index].mandatory || chosen.contains(index)) {
            section.sources.push_back(section.items[index].source);
            ++section.included;
        }
    }
}

nlohmann::json sourceJson(const ProjectContextSourceRef& source)
{
    return nlohmann::json {
        {"category", toString(source.category)},
        {"kind", source.kind},
        {"id", source.id},
        {"digest", source.digest}};
}

std::string canonicalSerialization(const ProjectContextSnapshot& snapshot)
{
    nlohmann::json sections = nlohmann::json::array();
    for (const auto& section : snapshot.sections) {
        nlohmann::json sources = nlohmann::json::array();
        for (const auto& source : section.sources) sources.push_back(sourceJson(source));
        sections.push_back(nlohmann::json {
            {"category", toString(section.category)},
            {"authority", toString(section.authority)},
            {"text", section.text},
            {"estimatedTokens", section.estimatedTokens},
            {"truncated", section.truncated},
            {"includedItems", section.includedItems},
            {"omittedItems", section.omittedItems},
            {"sources", std::move(sources)}});
    }
    nlohmann::json budgetSections = nlohmann::json::array();
    for (const auto& entry : snapshot.budget.sections) {
        budgetSections.push_back(nlohmann::json {
            {"category", toString(entry.category)},
            {"limitTokens", entry.limitTokens},
            {"usedTokens", entry.usedTokens},
            {"truncated", entry.truncated},
            {"omittedItems", entry.omittedItems}});
    }
    const nlohmann::json document {
        {"schemaVersion", snapshot.schemaVersion},
        {"policyId", snapshot.policyId},
        {"projectId", snapshot.projectId},
        {"selectedRunId", snapshot.selectedRunId.has_value() ? nlohmann::json(*snapshot.selectedRunId)
                                                              : nlohmann::json(nullptr)},
        {"workspaceFingerprint", snapshot.workspaceFingerprint},
        {"budget", nlohmann::json {
            {"policyId", snapshot.budget.policyId},
            {"totalLimitTokens", snapshot.budget.totalLimitTokens},
            {"usedTokens", snapshot.budget.usedTokens},
            {"truncated", snapshot.budget.truncated},
            {"sections", std::move(budgetSections)}}},
        {"sections", std::move(sections)}};
    // nlohmann objects are key-sorted, so the dump is canonical; strict mode would throw on invalid UTF-8,
    // which validUtf8() already rejected for every input string.
    return document.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

} // namespace

const char* toString(ProjectContextCategory category) noexcept
{
    switch (category) {
    case ProjectContextCategory::ProjectMetadata: return "project_metadata";
    case ProjectContextCategory::SelectedLineage: return "selected_lineage";
    case ProjectContextCategory::WorkLedger: return "work_ledger";
    case ProjectContextCategory::WorkspaceCatalog: return "workspace_catalog";
    case ProjectContextCategory::ProjectMemory: return "project_memory";
    }
    return "unknown";
}

const char* toString(ProjectContextAuthority authority) noexcept
{
    switch (authority) {
    case ProjectContextAuthority::HumanCurated: return "human_curated";
    case ProjectContextAuthority::Authoritative: return "authoritative";
    case ProjectContextAuthority::Derived: return "derived";
    }
    return "unknown";
}

ProjectContextAuthority projectContextAuthorityOf(ProjectContextCategory category) noexcept
{
    switch (category) {
    case ProjectContextCategory::ProjectMetadata: return ProjectContextAuthority::HumanCurated;
    case ProjectContextCategory::SelectedLineage:
    case ProjectContextCategory::WorkLedger:
    case ProjectContextCategory::WorkspaceCatalog: return ProjectContextAuthority::Authoritative;
    case ProjectContextCategory::ProjectMemory: return ProjectContextAuthority::Derived;
    }
    return ProjectContextAuthority::Derived;
}

std::string_view projectContextSectionHeading(ProjectContextCategory category) noexcept
{
    switch (category) {
    case ProjectContextCategory::ProjectMetadata: return "## Project";
    case ProjectContextCategory::SelectedLineage: return "## Selected Run lineage";
    case ProjectContextCategory::WorkLedger: return "## Work Ledger";
    case ProjectContextCategory::WorkspaceCatalog: return "## Workspace catalog";
    case ProjectContextCategory::ProjectMemory:
        return "## Project Memory (derived narrative, not authoritative)";
    }
    return "## Unknown";
}

std::uint64_t ProjectContextBudget::forCategory(ProjectContextCategory category) const noexcept
{
    switch (category) {
    case ProjectContextCategory::ProjectMetadata: return projectMetadataTokens;
    case ProjectContextCategory::SelectedLineage: return selectedLineageTokens;
    case ProjectContextCategory::WorkLedger: return workLedgerTokens;
    case ProjectContextCategory::WorkspaceCatalog: return workspaceCatalogTokens;
    case ProjectContextCategory::ProjectMemory: return projectMemoryTokens;
    }
    return 0;
}

std::vector<std::string> projectContextPathFragments(const std::filesystem::path& absolutePath)
{
    std::vector<std::string> fragments;
    const auto add = [&fragments](std::string value) {
        while (value.size() > 1 && (value.back() == '/' || value.back() == '\\')) value.pop_back();
        if (value.empty()) return;
        if (std::find(fragments.begin(), fragments.end(), value) == fragments.end()) {
            fragments.push_back(std::move(value));
        }
    };
    const std::string native = absolutePath.string();
    std::string forward = native;
    std::replace(forward.begin(), forward.end(), '\\', '/');
    std::string backward = native;
    std::replace(backward.begin(), backward.end(), '/', '\\');
    add(native);
    add(absolutePath.generic_string());
    add(std::move(forward));
    add(std::move(backward));
    return fragments;
}

bool projectContextContainsForbiddenPath(
    std::string_view text,
    const std::vector<std::string>& forbiddenFragments) noexcept
{
    if (hasAbsolutePathShape(text)) return true;
    return std::any_of(forbiddenFragments.begin(), forbiddenFragments.end(), [&](const auto& fragment) {
        return !fragment.empty() && text.find(fragment) != std::string_view::npos;
    });
}

bool projectContextIsValidUtf8(std::string_view text) noexcept
{
    return validUtf8(text);
}

std::string shortProjectContextFingerprint(std::string_view fingerprint)
{
    return std::string(fingerprint.substr(0, 12));
}

namespace {

// A mandatory item is an identity role, never a caller privilege (see ProjectContextItem::mandatory).
bool mandatoryRoleAllowed(const ProjectContextAssemblyInput& input, ProjectContextCategory category,
    const ProjectContextSourceRef& source)
{
    switch (category) {
    case ProjectContextCategory::ProjectMetadata:
        return source.kind == "project" && source.id == input.projectId;
    case ProjectContextCategory::SelectedLineage:
        return input.selectedRunId.has_value() && (source.kind == "run" || source.kind == "manual_resolution")
            && source.id == *input.selectedRunId;
    case ProjectContextCategory::WorkspaceCatalog:
        return source.kind == "workspace" && source.digest == input.workspaceFingerprint;
    case ProjectContextCategory::WorkLedger:
    case ProjectContextCategory::ProjectMemory: return false;
    }
    return false;
}

common::Result<ProjectContextSnapshot> assembleWithBudget(
    const ProjectContextAssemblyInput& input, const ProjectContextBudget& budget)
{
    using Snapshot = ProjectContextSnapshot;
    if (budget.totalTokens == 0) {
        return fail<Snapshot>("project_context.invalid_request", "The token budget must not be empty");
    }
    if (!validSingleLine(input.projectId, kMaximumIdentityBytes, false)
        || !validSingleLine(input.workspaceFingerprint, kMaximumIdentityBytes, false)
        || (input.selectedRunId.has_value()
            && !validSingleLine(*input.selectedRunId, kMaximumIdentityBytes, false))) {
        return fail<Snapshot>("project_context.invalid_request",
            "The project, selected Run and workspace identities must be non-empty single-line text");
    }
    if (projectContextContainsForbiddenPath(input.projectId, input.forbiddenFragments)
        || projectContextContainsForbiddenPath(input.workspaceFingerprint, input.forbiddenFragments)
        || (input.selectedRunId.has_value()
            && projectContextContainsForbiddenPath(*input.selectedRunId, input.forbiddenFragments))) {
        return fail<Snapshot>("project_context.absolute_path",
            "An absolute or forbidden path was found in the project context identity");
    }

    // 1. Validate and normalize every input section and item (independent of the budget).
    std::vector<PreparedSection> prepared;
    std::set<std::pair<std::string, std::string>> seenSources;
    std::set<ProjectContextCategory> seenCategories;
    bool hasProjectIdentity = false;
    bool hasWorkspaceIdentity = false;
    bool hasSelectedRunIdentity = false;
    for (const auto& sectionInput : input.sections) {
        if (canonicalIndex(sectionInput.category) >= kCanonicalOrder.size()) {
            return fail<Snapshot>("project_context.invalid_request", "Unknown project context category");
        }
        if (sectionInput.category == ProjectContextCategory::SelectedLineage && !input.selectedRunId.has_value()
            && !sectionInput.items.empty()) {
            return fail<Snapshot>("project_context.invalid_request",
                "A selected lineage needs a selected Run id");
        }
        if (!seenCategories.insert(sectionInput.category).second) {
            return fail<Snapshot>("project_context.duplicate_section",
                std::string("Project context category supplied twice: ") + toString(sectionInput.category));
        }
        if (sectionInput.items.empty()) continue;  // nothing to say: the section is simply absent
        PreparedSection section;
        section.category = sectionInput.category;
        section.limit = budget.forCategory(sectionInput.category);
        for (const auto& item : sectionInput.items) {
            ProjectContextItem normalized = item;
            normalized.text = normalizeText(item.text);
            normalized.source.category = sectionInput.category;
            if (isBlank(normalized.text) || normalized.text.size() > kMaximumItemTextBytes
                || !validUtf8(normalized.text) || hasControlCharacter(normalized.text)
                || !safeKind(normalized.source.kind)
                || !validSingleLine(normalized.source.id, kMaximumSourceIdBytes, false)
                || !validSingleLine(normalized.source.digest, kMaximumDigestBytes, true)) {
                return fail<Snapshot>("project_context.invalid_item",
                    std::string("Invalid project context item in ") + toString(sectionInput.category));
            }
            if (normalized.mandatory) {
                if (!mandatoryRoleAllowed(input, sectionInput.category, normalized.source)) {
                    return fail<Snapshot>("project_context.invalid_mandatory",
                        std::string("A mandatory item is not an identity role in ")
                            + toString(sectionInput.category));
                }
                if (sectionInput.category == ProjectContextCategory::ProjectMetadata) hasProjectIdentity = true;
                if (sectionInput.category == ProjectContextCategory::WorkspaceCatalog) hasWorkspaceIdentity = true;
                if (sectionInput.category == ProjectContextCategory::SelectedLineage
                    && normalized.source.kind == "run") {
                    hasSelectedRunIdentity = true;
                }
            }
            if (!seenSources.emplace(normalized.source.kind, normalized.source.id).second) {
                return fail<Snapshot>("project_context.duplicate_source",
                    "A project context source identity was supplied twice");
            }
            if (projectContextContainsForbiddenPath(normalized.text, input.forbiddenFragments)
                || projectContextContainsForbiddenPath(normalized.source.id, input.forbiddenFragments)
                || projectContextContainsForbiddenPath(normalized.source.digest, input.forbiddenFragments)) {
                return fail<Snapshot>("project_context.absolute_path",
                    std::string("An absolute or forbidden path was found in ") + toString(sectionInput.category));
            }
            section.items.push_back(std::move(normalized));
        }
        for (std::size_t index = 0; index < section.items.size(); ++index) {
            if (!section.items[index].mandatory) section.optional.push_back(index);
        }
        prepared.push_back(std::move(section));
    }
    // The snapshot must state what it describes: project, workspace and (when selected) Run identity.
    if (!hasProjectIdentity || !hasWorkspaceIdentity
        || (input.selectedRunId.has_value() && !hasSelectedRunIdentity)) {
        return fail<Snapshot>("project_context.missing_identity",
            "The project context lacks a mandatory project, workspace or selected-Run identity item");
    }
    std::sort(prepared.begin(), prepared.end(), [](const PreparedSection& left, const PreparedSection& right) {
        return canonicalIndex(left.category) < canonicalIndex(right.category);
    });

    // 2. Per-section limit: the mandatory items always stay, then the longest prefix of the optional items
    //    that still fits (rendered tokens grow monotonically with every appended item).
    for (auto& section : prepared) {
        const bool hasMandatory = section.optional.size() != section.items.size();
        if (tokensFor(section, 0) > section.limit) {
            if (hasMandatory) {
                return fail<Snapshot>("project_context.mandatory_exceeds_budget",
                    std::string("The mandatory items of ") + toString(section.category)
                        + " exceed its token limit");
            }
            section.fitsHeading = false;  // not even the heading fits: the whole section is omitted
            section.keep = 0;
            continue;
        }
        std::size_t low = 0;
        std::size_t high = section.optional.size();
        while (low < high) {
            const std::size_t middle = low + (high - low + 1) / 2;
            if (tokensFor(section, middle) <= section.limit) low = middle;
            else high = middle - 1;
        }
        section.keep = low;
    }

    // 3. Total limit: drop whole tail items, Project Memory first, then the Work Ledger, then the catalog.
    const auto totalTokens = [&prepared]() {
        std::uint64_t total = 0;
        for (const auto& section : prepared) {
            if (section.fitsHeading) total += tokensFor(section, section.keep);
        }
        return total;
    };
    while (totalTokens() > budget.totalTokens) {
        bool dropped = false;
        for (const auto category : kCutOrder) {
            for (auto& section : prepared) {
                if (section.category == category && section.fitsHeading && section.keep > 0) {
                    --section.keep;
                    dropped = true;
                    break;
                }
            }
            if (dropped) break;
        }
        if (!dropped) {
            return fail<Snapshot>("project_context.budget_unsatisfiable",
                "The mandatory project context does not fit the total token limit");
        }
    }

    // 4. Build the snapshot.
    Snapshot snapshot;
    snapshot.projectId = input.projectId;
    snapshot.selectedRunId = input.selectedRunId;
    snapshot.workspaceFingerprint = input.workspaceFingerprint;
    snapshot.policyId = std::string(budget.isV1() ? kProjectContextBudgetPolicyId
                                                  : kProjectContextCustomBudgetPolicyId);
    snapshot.budget.policyId = snapshot.policyId;
    snapshot.budget.totalLimitTokens = budget.totalTokens;
    for (auto& section : prepared) {
        const std::uint32_t total = static_cast<std::uint32_t>(section.items.size());
        ProjectContextSectionBudgetReport report;
        report.category = section.category;
        report.limitTokens = section.limit;
        if (!section.fitsHeading) {
            report.truncated = true;
            report.omittedItems = total;
            snapshot.budget.sections.push_back(report);
            snapshot.budget.truncated = true;
            continue;
        }
        finalizeSection(section);
        ProjectContextSection built;
        built.category = section.category;
        built.authority = projectContextAuthorityOf(section.category);
        built.text = section.text;
        built.sources = section.sources;
        built.estimatedTokens = section.tokens;
        built.includedItems = section.included;
        built.omittedItems = total - section.included;
        built.truncated = built.omittedItems > 0;
        report.usedTokens = section.tokens;
        report.truncated = built.truncated;
        report.omittedItems = built.omittedItems;
        snapshot.budget.usedTokens += section.tokens;
        snapshot.budget.truncated = snapshot.budget.truncated || built.truncated;
        for (const auto& source : built.sources) snapshot.sources.push_back(source);
        snapshot.budget.sections.push_back(report);
        snapshot.sections.push_back(std::move(built));
    }
    snapshot.fingerprint = sha256Hex(canonicalSerialization(snapshot));
    return common::Result<Snapshot>::ok(std::move(snapshot));
}

} // namespace

common::Result<ProjectContextSnapshot> assembleProjectContextSnapshot(
    const ProjectContextAssemblyInput& input)
{
    return assembleWithBudget(input, ProjectContextBudget {});
}

common::Result<ProjectContextSnapshot> assembleProjectContextSnapshotWithCustomBudget(
    const ProjectContextAssemblyInput& input, const ProjectContextBudget& budget)
{
    return assembleWithBudget(input, budget);
}

common::Result<std::string> computeProjectContextSnapshotFingerprint(const ProjectContextSnapshot& snapshot)
{
    try {
        return common::Result<std::string>::ok(sha256Hex(canonicalSerialization(snapshot)));
    } catch (const std::exception&) {
        // The canonical serialization is strict about UTF-8: a snapshot carrying invalid text has no fingerprint.
        return common::Result<std::string>::failure({"project_context.invalid_text",
            "The project-context snapshot carries text that is not valid UTF-8"});
    }
}

common::Result<void> validateProjectContextSnapshotIntegrity(const ProjectContextSnapshot& snapshot)
{
    const auto expected = computeProjectContextSnapshotFingerprint(snapshot);
    if (!expected.success()) return common::Result<void>::failure(*expected.error());
    if (snapshot.fingerprint != expected.value()) {
        return common::Result<void>::failure({"project_context.fingerprint_mismatch",
            "The project-context snapshot does not match its own fingerprint"});
    }
    for (std::size_t index = 0; index < snapshot.sections.size(); ++index) {
        const auto& section = snapshot.sections[index];
        if (section.authority != projectContextAuthorityOf(section.category)) {
            return common::Result<void>::failure({"project_context.invalid_structure",
                "A project-context section carries an authority its category does not have"});
        }
        if (index > 0 && static_cast<int>(snapshot.sections[index - 1].category) >= static_cast<int>(section.category)) {
            return common::Result<void>::failure({"project_context.invalid_structure",
                "The project-context sections are not in canonical order or repeat a category"});
        }
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
