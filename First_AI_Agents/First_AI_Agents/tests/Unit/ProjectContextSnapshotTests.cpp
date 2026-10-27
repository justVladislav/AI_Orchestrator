#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ai::application;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

ProjectContextItem item(std::string text, std::string kind, std::string id, bool mandatory = false,
    std::string digest = {})
{
    ProjectContextItem value;
    value.text = std::move(text);
    value.source.kind = std::move(kind);
    value.source.id = std::move(id);
    value.source.digest = std::move(digest);
    value.mandatory = mandatory;
    return value;
}

ProjectContextSectionInput section(ProjectContextCategory category, std::vector<ProjectContextItem> items)
{
    ProjectContextSectionInput value;
    value.category = category;
    value.items = std::move(items);
    return value;
}

std::string filler(char letter, std::size_t length)
{
    return std::string(length, letter);
}

// The assembly input has no budget (production always uses the fixed v1 limits); the tests that trim carry a
// custom budget next to it and go through the explicit-budget entry point. A default budget IS v1.
struct CaseInput : ProjectContextAssemblyInput {
    ProjectContextBudget budget;
};

template <typename T>
concept HasBudgetMember = requires(T value) { value.budget; };

const std::string kWorkspaceFingerprint = std::string(64, 'a');

// Deliberately NOT in canonical order: the assembler must reorder.
CaseInput baseInput()
{
    CaseInput input;
    input.projectId = "project-1";
    input.selectedRunId = "run-7";
    input.workspaceFingerprint = kWorkspaceFingerprint;
    input.sections = {
        section(ProjectContextCategory::ProjectMemory,
            {item("memory one", "memory_segment", "mem-1"), item("memory two", "memory_segment", "mem-2")}),
        section(ProjectContextCategory::WorkLedger,
            {item("ledger newest", "ledger_entry", "run-9"), item("ledger older", "ledger_entry", "run-8")}),
        section(ProjectContextCategory::WorkspaceCatalog,
            {item("workspace identity", "workspace", "current", true, kWorkspaceFingerprint),
                item("src/a.qml 100", "workspace_file", "src/a.qml"),
                item("src/b.qml 200", "workspace_file", "src/b.qml")}),
        section(ProjectContextCategory::SelectedLineage,
            {item("selected run run-7 parent run-3", "run", "run-7", true)}),
        section(ProjectContextCategory::ProjectMetadata,
            {item("Project: Demo", "project", "project-1", true), item("Description: demo", "project_note", "description")})};
    return input;
}

ProjectContextSnapshot build(const CaseInput& input)
{
    const auto result = assembleProjectContextSnapshotWithCustomBudget(input, input.budget);
    require(result.success(), result.success() ? "" : result.error()->message.c_str());
    return result.value();
}

std::string failureCode(const CaseInput& input)
{
    const auto result = assembleProjectContextSnapshotWithCustomBudget(input, input.budget);
    require(!result.success(), "The assembly was expected to fail");
    return result.error()->code;
}

// The production entry point (fixed v1 limits).
std::string productionFailureCode(const ProjectContextAssemblyInput& input)
{
    const auto result = assembleProjectContextSnapshot(input);
    require(!result.success(), "The production assembly was expected to fail");
    return result.error()->code;
}

const ProjectContextSection* find(const ProjectContextSnapshot& snapshot, ProjectContextCategory category)
{
    for (const auto& value : snapshot.sections) {
        if (value.category == category) return &value;
    }
    return nullptr;
}

std::uint64_t headingTokens(ProjectContextCategory category)
{
    return estimateChatTokens(projectContextSectionHeading(category));
}

// ---------------------------------------------------------------------------------------------------------

void testSectionsAreEmittedInCanonicalOrderWithAuthorityByCategory()
{
    const auto snapshot = build(baseInput());
    require(snapshot.schemaVersion == "project-context-snapshot.v1"
            && snapshot.policyId == "project_context_budget_v1" && snapshot.budget.policyId == snapshot.policyId,
        "Schema or policy id changed");
    require(snapshot.sections.size() == 5, "Every supplied section must be emitted");
    const ProjectContextCategory expected[] = {ProjectContextCategory::ProjectMetadata,
        ProjectContextCategory::SelectedLineage, ProjectContextCategory::WorkLedger,
        ProjectContextCategory::WorkspaceCatalog, ProjectContextCategory::ProjectMemory};
    for (std::size_t index = 0; index < 5; ++index) {
        require(snapshot.sections[index].category == expected[index], "Sections must follow the canonical order");
        require(snapshot.sections[index].authority == projectContextAuthorityOf(expected[index]),
            "Authority is a property of the category");
    }
    require(snapshot.sections[0].authority == ProjectContextAuthority::HumanCurated
            && snapshot.sections[1].authority == ProjectContextAuthority::Authoritative
            && snapshot.sections[2].authority == ProjectContextAuthority::Authoritative
            && snapshot.sections[3].authority == ProjectContextAuthority::Authoritative
            && snapshot.sections[4].authority == ProjectContextAuthority::Derived,
        "Only Project Memory is derived and only the description is human curated");
    require(snapshot.sections[4].text.find("derived narrative, not authoritative") != std::string::npos,
        "Project Memory must be explicitly labelled as derived in its rendering");
    require(snapshot.sections[0].text.rfind("## Project", 0) == 0, "A section starts with its heading");
}

void testAssemblyIsDeterministicAndSourcesAreFlattenedInOrder()
{
    const auto first = build(baseInput());
    const auto second = build(baseInput());
    require(first.fingerprint == second.fingerprint && first.fingerprint.size() == 64,
        "Equal inputs must give an equal 64 character fingerprint");
    for (std::size_t index = 0; index < first.sections.size(); ++index) {
        require(first.sections[index].text == second.sections[index].text, "Section text must be byte-identical");
    }
    std::vector<ProjectContextSourceRef> flat;
    for (const auto& value : first.sections) {
        for (const auto& source : value.sources) {
            require(source.category == value.category, "A source carries the category of its section");
            flat.push_back(source);
        }
    }
    require(flat.size() == first.sources.size() && flat.size() == 10, "The flat source list must hold every included source");
    for (std::size_t index = 0; index < flat.size(); ++index) {
        require(flat[index].kind == first.sources[index].kind && flat[index].id == first.sources[index].id,
            "The flat source list must follow the section order");
    }
    std::uint64_t used = 0;
    for (const auto& value : first.sections) used += value.estimatedTokens;
    require(first.budget.usedTokens == used && first.budget.totalLimitTokens == 24'000 && !first.budget.truncated
            && first.budget.sections.size() == 5,
        "The budget report must add up and show no truncation for a small input");
}

void testFingerprintChangesWithEveryDurableInput()
{
    const auto reference = build(baseInput()).fingerprint;
    const auto differs = [&reference](CaseInput changed, const char* what) {
        require(build(changed).fingerprint != reference, what);
    };
    {
        auto input = baseInput();
        input.projectId = "project-2";
        input.sections[4].items[0].source.id = "project-2";
        differs(input, "A different project must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.selectedRunId = "run-8";
        input.sections[3].items[0].source.id = "run-8";
        differs(input, "A different selected Run must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.selectedRunId.reset();
        input.sections.erase(input.sections.begin() + 3);
        differs(input, "No selected Run must differ from a selected Run");
    }
    {
        auto input = baseInput();
        input.workspaceFingerprint = std::string(64, 'b');
        input.sections[2].items[0].source.digest = std::string(64, 'b');
        differs(input, "A workspace change must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.sections[1].items[0].text = "ledger newest, changed";
        differs(input, "A ledger text change must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.sections[1].items[0].source.digest = "rev-2";
        differs(input, "A source digest change must change the fingerprint");
    }
    {
        auto input = baseInput();
        std::swap(input.sections[1].items[0], input.sections[1].items[1]);
        differs(input, "Item order is part of the content and must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.sections[4].items[1].text = "Description: edited";
        differs(input, "A description change must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.sections[2].items.erase(input.sections[2].items.begin() + 1);
        differs(input, "A dropped catalog row must change the fingerprint");
    }
    {
        auto input = baseInput();
        input.budget.projectMemoryTokens = 1'999;
        differs(input, "The budget limits are part of the fingerprint");
    }
}

void testNewlineStyleDoesNotChangeTheFingerprint()
{
    auto crlf = baseInput();
    crlf.sections[1].items[0].text = "line one\r\nline two\r\n\r\n";
    auto lf = baseInput();
    lf.sections[1].items[0].text = "line one\nline two";
    auto cr = baseInput();
    cr.sections[1].items[0].text = "line one\rline two";
    const auto a = build(crlf);
    require(a.fingerprint == build(lf).fingerprint && a.fingerprint == build(cr).fingerprint,
        "CRLF, LF and CR must normalize to the same snapshot");
    require(find(a, ProjectContextCategory::WorkLedger)->text.find('\r') == std::string::npos,
        "No carriage return may survive normalization");
}

void testEmptySectionIsOmittedButDuplicateCategoryIsRejected()
{
    auto input = baseInput();
    input.sections[0].items.clear();  // Project Memory has nothing to say
    const auto snapshot = build(input);
    require(find(snapshot, ProjectContextCategory::ProjectMemory) == nullptr && snapshot.sections.size() == 4,
        "A section without items must be absent");

    auto duplicate = baseInput();
    duplicate.sections.push_back(section(ProjectContextCategory::WorkLedger, {}));
    require(failureCode(duplicate) == "project_context.duplicate_section",
        "A category supplied twice must be rejected even when one copy is empty");
}

void testSectionLimitKeepsAPrefixOfWholeItems()
{
    auto input = baseInput();
    const std::string a = "ledger-a " + filler('x', 300);
    const std::string b = "ledger-b " + filler('y', 300);
    const std::string c = "ledger-c " + filler('z', 300);
    const std::string huge = "ledger-d " + filler('h', 3000);
    const std::string tiny = "ledger-e tiny";
    input.sections[1].items = {item(a, "ledger_entry", "l-a"), item(b, "ledger_entry", "l-b"),
        item(c, "ledger_entry", "l-c"), item(huge, "ledger_entry", "l-d"), item(tiny, "ledger_entry", "l-e")};
    const std::string heading(projectContextSectionHeading(ProjectContextCategory::WorkLedger));
    input.budget.workLedgerTokens = estimateChatTokens(heading + "\n" + a + "\n" + b + "\n" + c);

    const auto snapshot = build(input);
    const auto* ledger = find(snapshot, ProjectContextCategory::WorkLedger);
    require(ledger != nullptr && ledger->includedItems == 3 && ledger->omittedItems == 2 && ledger->truncated,
        "Exactly the items that fit must stay and the rest must be counted as omitted");
    require(ledger->text.find("ledger-c") != std::string::npos && ledger->text.find("ledger-d") == std::string::npos,
        "The item that does not fit must be omitted");
    require(ledger->text.find("ledger-e") == std::string::npos,
        "A later small item must NOT jump the queue: trimming keeps a prefix");
    require(ledger->text.find(a) != std::string::npos && ledger->text.find(c) != std::string::npos,
        "An included item must be whole, never cut");
    require(ledger->estimatedTokens <= input.budget.workLedgerTokens && ledger->sources.size() == 3,
        "The section must stay inside its limit and list only included sources");
    require(snapshot.budget.truncated && snapshot.budget.sections[2].truncated
            && snapshot.budget.sections[2].omittedItems == 2,
        "The budget report must carry the truncation facts");
}

void testMandatoryItemsAreNeverDroppedAndNeverExceedTheirLimit()
{
    auto input = baseInput();
    const std::string heading(projectContextSectionHeading(ProjectContextCategory::ProjectMetadata));
    input.budget.projectMetadataTokens = estimateChatTokens(heading + "\nProject: Demo");
    const auto snapshot = build(input);
    const auto* metadata = find(snapshot, ProjectContextCategory::ProjectMetadata);
    require(metadata != nullptr && metadata->includedItems == 1 && metadata->omittedItems == 1
            && metadata->text.find("Project: Demo") != std::string::npos
            && metadata->text.find("Description") == std::string::npos,
        "The optional description may go, the mandatory identity must stay");

    input.budget.projectMetadataTokens = estimateChatTokens(heading + "\nProject: Demo") - 1;
    require(failureCode(input) == "project_context.mandatory_exceeds_budget",
        "Mandatory items that exceed their limit must fail instead of being dropped");
}

void testTotalLimitCutsMemoryThenLedgerThenCatalogAndNeverIdentity()
{
    auto input = baseInput();
    // Roomy per-section limits so only the total matters.
    input.budget.projectMetadataTokens = 5'000;
    input.budget.selectedLineageTokens = 5'000;
    input.budget.workLedgerTokens = 5'000;
    input.budget.workspaceCatalogTokens = 5'000;
    input.budget.projectMemoryTokens = 5'000;
    input.budget.totalTokens = 24'000;
    const auto full = build(input);
    const std::uint64_t all = full.budget.usedTokens;

    input.budget.totalTokens = all - 1;
    const auto firstCut = build(input);
    const auto* memory = find(firstCut, ProjectContextCategory::ProjectMemory);
    require(memory != nullptr && memory->omittedItems >= 1
            && find(firstCut, ProjectContextCategory::WorkLedger)->omittedItems == 0
            && find(firstCut, ProjectContextCategory::WorkspaceCatalog)->omittedItems == 0,
        "The total limit must cut Project Memory first");
    require(firstCut.budget.usedTokens <= input.budget.totalTokens, "The total limit must be respected");

    const std::uint64_t withoutMemoryItems = all - find(full, ProjectContextCategory::ProjectMemory)->estimatedTokens
        + headingTokens(ProjectContextCategory::ProjectMemory);
    input.budget.totalTokens = withoutMemoryItems - 1;
    const auto secondCut = build(input);
    require(find(secondCut, ProjectContextCategory::ProjectMemory)->includedItems == 0
            && find(secondCut, ProjectContextCategory::WorkLedger)->omittedItems >= 1
            && find(secondCut, ProjectContextCategory::WorkspaceCatalog)->omittedItems == 0,
        "After the memory is gone the Work Ledger tail must go before the catalog");

    // Identity survives every cut: lower the total until only mandatory content could remain.
    input.budget.totalTokens = 40;
    require(failureCode(input) == "project_context.budget_unsatisfiable",
        "When only identity is left and it still does not fit the total, the assembly must fail");
    input.budget.totalTokens = 24'000;
    const auto relaxed = build(input);
    require(find(relaxed, ProjectContextCategory::SelectedLineage)->text.find("selected run run-7") != std::string::npos
            && find(relaxed, ProjectContextCategory::ProjectMetadata)->text.find("Project: Demo") != std::string::npos,
        "Project and selected-Run identity must be present");
}

void testSectionWhoseHeadingDoesNotFitIsOmittedAndReported()
{
    auto input = baseInput();
    input.budget.projectMemoryTokens = 1;
    const auto snapshot = build(input);
    require(find(snapshot, ProjectContextCategory::ProjectMemory) == nullptr,
        "A section that cannot even render its heading must be absent");
    const auto& report = snapshot.budget.sections.back();
    require(report.category == ProjectContextCategory::ProjectMemory && report.truncated && report.omittedItems == 2
            && report.usedTokens == 0 && snapshot.budget.truncated,
        "The omission must still be visible in the budget report");
    for (const auto& source : snapshot.sources) {
        require(source.kind != "memory_segment", "An omitted section must contribute no source");
    }
}

void testInvalidItemsAndRequestsAreRejected()
{
    const auto mutate = [](auto change) {
        auto input = baseInput();
        change(input);
        return failureCode(input);
    };
    require(mutate([](auto& input) { input.projectId.clear(); }) == "project_context.invalid_request", "Empty project id");
    require(mutate([](auto& input) { input.workspaceFingerprint.clear(); }) == "project_context.invalid_request",
        "Empty workspace fingerprint");
    require(mutate([](auto& input) { input.selectedRunId = std::string(); }) == "project_context.invalid_request",
        "An empty selected Run id");
    require(mutate([](auto& input) { input.sections[1].items[0].text.clear(); }) == "project_context.invalid_item",
        "Empty text must be rejected");
    require(mutate([](auto& input) { input.sections[1].items[0].text = " \r\n\t\n"; }) == "project_context.invalid_item",
        "Whitespace-only text must be rejected");
    require(mutate([](auto& input) { input.sections[1].items[0].text = std::string("a\0b", 3); })
            == "project_context.invalid_item", "A NUL character");
    require(mutate([](auto& input) { input.sections[1].items[0].text = "bad \xC3\x28 utf8"; })
            == "project_context.invalid_item", "Invalid UTF-8");
    require(mutate([](auto& input) { input.sections[1].items[0].text = "overlong \xC0\x80 nul"; })
            == "project_context.invalid_item", "An overlong UTF-8 sequence");
    require(mutate([](auto& input) { input.sections[1].items[0].text = "surrogate \xED\xA0\x80 half"; })
            == "project_context.invalid_item", "A UTF-8 encoded surrogate");
    require(mutate([](auto& input) { input.sections[1].items[0].text = "truncated \xE2\x82"; })
            == "project_context.invalid_item", "A truncated UTF-8 sequence");
    {
        auto valid = baseInput();
        valid.sections[1].items[0].text = "euro \xE2\x82\xAC, e-acute \xC3\xA9 and \xF0\x9F\x99\x82";
        require(build(valid).sections.size() == 5, "Valid multi-byte UTF-8 must be accepted");
    }
    require(mutate([](auto& input) { input.sections[1].items[0].source.kind = "Ledger-Entry"; })
            == "project_context.invalid_item", "A kind that is not a lowercase token");
    require(mutate([](auto& input) { input.sections[1].items[0].source.id.clear(); })
            == "project_context.invalid_item", "An empty source id");
    require(mutate([](auto& input) { input.sections[2].items[1].source = input.sections[1].items[0].source; })
            == "project_context.duplicate_source", "The same source identity twice");
}

void testAbsolutePathsAreRejectedButRelativeAndUrlShapesAreFine()
{
    const auto withText = [](std::string text) {
        auto input = baseInput();
        input.sections[1].items[0].text = std::move(text);
        return input;
    };
    for (const char* bad : {"opened C:\\Users\\vlad\\proj", "opened C:/work/proj", "share \\\\server\\share\\x",
             "see /Users/vlad/proj", "in (/home/build/x)", "see /workspace/repo", "from /data/set",
             "mac /private/var/tmp/x", "root /tmp", "d:\\a"}) {
        require(failureCode(withText(bad)) == "project_context.absolute_path", bad);
    }
    for (const char* fine : {"see https://example.com/a", "src/models/A.qml changed", "and/or the ratio 1:2",
             "relative ./src/a.qml", "relative ../src/a.qml", "tex:/not a drive", "config/home/dir",
             "divide a / b"}) {
        require(build(withText(fine)).sections.size() == 5, fine);
    }
    // Path in a source id or digest is rejected too.
    auto source = baseInput();
    source.sections[1].items[0].source.id = "C:\\x\\y";
    require(failureCode(source) == "project_context.absolute_path", "A path in a source id");

    auto identity = baseInput();
    identity.selectedRunId = "/workspace/run-7";
    require(failureCode(identity) == "project_context.absolute_path", "A path in a selected Run id");

    // The workspace root in every spelling is forbidden even if its shape would pass.
    const auto fragments = projectContextPathFragments(std::filesystem::path("D:\\work\\Repo One"));
    require(fragments.size() >= 2, "Both slash spellings must be offered");
    for (const auto& fragment : fragments) {
        auto input = withText("note " + fragment + " here");
        input.forbiddenFragments = fragments;
        require(failureCode(input) == "project_context.absolute_path", "A forbidden root spelling");
    }
    auto root = baseInput();
    root.forbiddenFragments = {"secret-token-123"};
    root.sections[1].items[0].text = "it says secret-token-123 inside";
    require(failureCode(root) == "project_context.absolute_path", "A forbidden fragment");
}

// Review finding: the guard recognised a segment start only from a short ASCII set, so "/@private",
// "/<non-ASCII name>", "file:///home/u" and Windows rooted/device paths passed.
void testPathGuardCoversUnusualSegmentsUrisRootedAndDevicePaths()
{
    const std::string cyrillic = "\xD0\x94\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5";  // "Dannye" in Cyrillic
    const auto withText = [](std::string text) {
        auto input = baseInput();
        input.sections[1].items[0].text = std::move(text);
        return input;
    };
    const std::vector<std::string> bad = {
        "see /@private/notes", "see /" + cyrillic + "/" + cyrillic, "open file:///home/user/project",
        "open FILE:///C:/x", "open file://server/share/x", "rooted \\Windows\\System32",
        "device \\\\?\\C:\\x", "pipe \\\\.\\pipe\\x", "unc \\\\server\\share", "home ~/notes",
        "home (~\\notes)", "var=/etc/hosts", "quote \"/opt/app\"", "line one\n/tmp/leak", "plus /+x",
        "dollar /$HOME", "tilde /~user", "dot-dir /.hidden/x", "percent /%20x"};
    for (const auto& text : bad) {
        require(failureCode(withText(text)) == "project_context.absolute_path", text.c_str());
    }
    const std::vector<std::string> fine = {
        cyrillic + "/" + cyrillic + "/file.qml", "src/" + cyrillic + "/a.qml", "relative .\\src\\a.qml",
        "a~b and ~tilde", "see http://example.com/a/b", "see ftp://host/x", "ratio 1/2 and a / b",
        "date 2026/01/15", "and/or TCP/IP I/O", "src\\models\\A.qml", "Press & Weld (v2) / beta"};
    for (const auto& text : fine) {
        require(build(withText(text)).sections.size() == 5, text.c_str());
    }
}

// Review finding: identities only had a UTF-8 check, so NUL / newline / tab passed as "valid UTF-8".
void testIdentitiesAndDigestsAreStrictSingleLineText()
{
    const auto mutate = [](auto change) {
        auto input = baseInput();
        change(input);
        return failureCode(input);
    };
    const std::string invalidRequest = "project_context.invalid_request";
    require(mutate([](auto& input) { input.projectId = "project\n1"; }) == invalidRequest, "Newline in projectId");
    require(mutate([](auto& input) { input.projectId = std::string("project\0" "1", 9); }) == invalidRequest, "NUL in projectId");
    require(mutate([](auto& input) { input.projectId = "project\t1"; }) == invalidRequest, "Tab in projectId");
    require(mutate([](auto& input) { input.projectId = " project-1"; }) == invalidRequest, "Leading space in projectId");
    require(mutate([](auto& input) { input.projectId = std::string(257, 'p'); }) == invalidRequest, "Over-long projectId");
    require(mutate([](auto& input) { input.selectedRunId = "run\n7"; }) == invalidRequest, "Newline in selectedRunId");
    require(mutate([](auto& input) { input.workspaceFingerprint = "aa\tbb"; }) == invalidRequest, "Tab in workspaceFingerprint");
    require(mutate([](auto& input) { input.workspaceFingerprint = "aa\x7F" "bb"; }) == invalidRequest, "DEL in workspaceFingerprint");
    require(mutate([](auto& input) { input.sections[1].items[0].source.id = "ledger\nentry"; })
            == "project_context.invalid_item", "Newline in a source id");
    require(mutate([](auto& input) { input.sections[1].items[0].source.id = "ledger\tentry"; })
            == "project_context.invalid_item", "Tab in a source id");
    require(mutate([](auto& input) { input.sections[1].items[0].source.digest = "abc\ndef"; })
            == "project_context.invalid_item", "Newline in a digest");
    require(mutate([](auto& input) { input.sections[1].items[0].source.digest = std::string(257, 'd'); })
            == "project_context.invalid_item", "Over-long digest");
    require(mutate([](auto& input) { input.sections[1].items[0].source.id = std::string(1'025, 'i'); })
            == "project_context.invalid_item", "Over-long source id");
    require(mutate([](auto& input) { input.sections[1].items[0].text = std::string(64 * 1024 + 1, 'x'); })
            == "project_context.invalid_item", "Over-long item text");
    // Multi-line prose stays legitimate, tab included.
    auto prose = baseInput();
    prose.sections[1].items[0].text = "line one\n\tindented line two";
    require(build(prose).sections.size() == 5, "Multi-line item text must be accepted");
}

// Review finding: "mandatory" was a caller privilege and the identity roles were not required.
void testMandatoryIsAnIdentityRoleAndTheIdentityIsRequired()
{
    const auto mutate = [](auto change) {
        auto input = baseInput();
        change(input);
        return failureCode(input);
    };
    const std::string invalidMandatory = "project_context.invalid_mandatory";
    require(mutate([](auto& input) { input.sections[1].items[0].mandatory = true; }) == invalidMandatory,
        "A ledger item must not be mandatory");
    require(mutate([](auto& input) { input.sections[0].items[0].mandatory = true; }) == invalidMandatory,
        "A Project Memory item must not be mandatory");
    require(mutate([](auto& input) { input.sections[2].items[1].mandatory = true; }) == invalidMandatory,
        "A catalog file row must not be mandatory");
    require(mutate([](auto& input) { input.sections[4].items[1].mandatory = true; }) == invalidMandatory,
        "The description must not be mandatory");
    require(mutate([](auto& input) { input.sections[4].items[0].source.id = "another-project"; }) == invalidMandatory,
        "A mandatory project item must carry the project id");
    require(mutate([](auto& input) { input.sections[3].items[0].source.id = "run-8"; }) == invalidMandatory,
        "A mandatory lineage item must carry the selected Run id");
    require(mutate([](auto& input) { input.sections[2].items[0].source.digest = std::string(64, 'b'); }) == invalidMandatory,
        "A mandatory workspace item must carry the workspace fingerprint");

    const std::string missing = "project_context.missing_identity";
    require(mutate([](auto& input) { input.sections[4].items[0].mandatory = false; }) == missing,
        "The project identity is required");
    require(mutate([](auto& input) { input.sections[2].items[0].mandatory = false; }) == missing,
        "The workspace identity is required");
    require(mutate([](auto& input) { input.sections[3].items[0].mandatory = false; }) == missing,
        "The selected-Run identity is required when a Run is selected");
    require(mutate([](auto& input) { input.sections.erase(input.sections.begin() + 3); }) == missing,
        "A selected Run without its lineage section is incomplete");
    require(mutate([](auto& input) { input.sections.erase(input.sections.begin() + 2); }) == missing,
        "A snapshot without the workspace section is incomplete");
    require(mutate([](auto& input) { input.selectedRunId.reset(); }) == "project_context.invalid_request",
        "A lineage section needs a selected Run id");

    // Without a selected Run the lineage section is simply absent, and that is complete.
    auto noRun = baseInput();
    noRun.selectedRunId.reset();
    noRun.sections.erase(noRun.sections.begin() + 3);
    require(build(noRun).sections.size() == 4, "A project-only snapshot needs no lineage");

    // The selected Run's resolution row may also be mandatory; a parent row is an ordinary optional item.
    auto roles = baseInput();
    roles.sections[3].items.push_back(item("run=run-7; manualResolution=unresolved", "manual_resolution", "run-7", true));
    roles.sections[3].items.push_back(item("parent run run-3", "run", "run-3"));
    const auto built = build(roles);
    require(find(built, ProjectContextCategory::SelectedLineage)->includedItems == 3,
        "The selected Run's resolution row and an optional parent row are accepted");
}

// Review finding: the budget policy was a mutable input, so a context far above 24 000 tokens could still be
// labelled project_context_budget_v1.
void testProductionAssemblyAlwaysUsesTheFixedV1Budget()
{
    static_assert(!HasBudgetMember<ProjectContextAssemblyInput>, "The production input must not carry a budget");
    static_assert(HasBudgetMember<CaseInput>, "Only the test wrapper carries a budget");

    auto input = baseInput();
    std::vector<ProjectContextItem> files;
    files.push_back(item("workspace identity", "workspace", "current", true, kWorkspaceFingerprint));
    for (int index = 0; index < 4'000; ++index) {
        files.push_back(item("src/module/file-" + std::to_string(index) + ".qml; bytes=1", "workspace_file",
            "src/module/file-" + std::to_string(index) + ".qml"));
    }
    input.sections[2].items = files;

    const ProjectContextAssemblyInput& production = input;
    const auto fixed = assembleProjectContextSnapshot(production);
    require(fixed.success(), "The production assembly must succeed");
    require(fixed.value().policyId == "project_context_budget_v1" && fixed.value().budget.policyId == "project_context_budget_v1"
            && fixed.value().budget.totalLimitTokens == 24'000 && fixed.value().budget.usedTokens <= 24'000
            && fixed.value().budget.truncated,
        "The production snapshot is bounded by the fixed v1 limits and says so");
    for (const auto& value : fixed.value().sections) {
        require(value.estimatedTokens <= ProjectContextBudget {}.forCategory(value.category),
            "Every section stays inside its v1 limit");
    }

    // A custom limit set is possible only through the explicit entry point and is labelled as such.
    input.budget.totalTokens = 10'000'000;
    input.budget.workspaceCatalogTokens = 10'000'000;
    const auto custom = build(input);
    require(custom.policyId == "project_context_budget_custom" && custom.budget.policyId == "project_context_budget_custom"
            && custom.budget.usedTokens > 24'000,
        "A non-v1 budget must never carry the v1 policy id");
    require(custom.fingerprint != fixed.value().fingerprint, "The policy is part of the fingerprint");

    // The explicit entry point with exactly the v1 limits IS the v1 policy and the production result.
    const CaseInput v1 = [&] { auto copy = input; copy.budget = ProjectContextBudget {}; return copy; }();
    require(build(v1).fingerprint == fixed.value().fingerprint && build(v1).policyId == "project_context_budget_v1",
        "Explicit v1 limits must equal the production assembly");

    auto empty = baseInput();
    empty.budget.totalTokens = 0;
    require(failureCode(empty) == "project_context.invalid_request", "An empty total budget is invalid");
    ProjectContextAssemblyInput invalid = baseInput();
    invalid.projectId.clear();
    require(productionFailureCode(invalid) == "project_context.invalid_request",
        "The production entry point validates its input too");
}

// Loaders decide that untrusted prose is unusable with the same UTF-8 rule as the assembler.
void testPublicUtf8HelperMatchesTheAssemblerRule()
{
    require(projectContextIsValidUtf8("") && projectContextIsValidUtf8("plain ascii")
            && projectContextIsValidUtf8("euro \xE2\x82\xAC e-acute \xC3\xA9 smile \xF0\x9F\x99\x82"),
        "Well-formed UTF-8 must be accepted");
    require(!projectContextIsValidUtf8("bad \xC3\x28") && !projectContextIsValidUtf8("overlong \xC0\x80")
            && !projectContextIsValidUtf8("surrogate \xED\xA0\x80") && !projectContextIsValidUtf8("cut \xE2\x82")
            && !projectContextIsValidUtf8("lone \x80 continuation"),
        "Malformed UTF-8 must be rejected");
}

void testFingerprintIsPinnedForRestartStability()
{
    // The canonical serialization is part of the contract: a change to it changes every stored fingerprint.
    // If this test fails after an intentional format change, bump the schema version and update the value.
    const auto snapshot = build(baseInput());
    if (snapshot.fingerprint != "a10cfb6a1d49355f4e07cdefd9827b100a729e89736a0875ccf37507fa4b275b") {
        throw std::runtime_error("The pinned fingerprint of the reference input changed: " + snapshot.fingerprint);
    }
    require(shortProjectContextFingerprint(snapshot.fingerprint) == snapshot.fingerprint.substr(0, 12)
            && shortProjectContextFingerprint(snapshot.fingerprint).size() == 12,
        "The short fingerprint is the first 12 characters");
}

// A snapshot handed over by a caller is only as good as its fingerprint matches the content it carries: the fingerprint
// is recomputed, and the structure the renderer relies on (canonical order, no repeats, authority fixed by category)
// is checked. Before this, a text edited under the old fingerprint went unnoticed.
void testIntegrityRecomputesTheFingerprintFromTheContent()
{
    const auto snapshot = build(baseInput());
    const auto computed = computeProjectContextSnapshotFingerprint(snapshot);
    require(computed.success() && computed.value() == snapshot.fingerprint,
        "The public fingerprint must be exactly the one the assembler stores");
    require(validateProjectContextSnapshotIntegrity(snapshot).success(), "An assembled snapshot is intact");

    const auto codeOfIntegrity = [](const ProjectContextSnapshot& value) {
        const auto result = validateProjectContextSnapshotIntegrity(value);
        require(!result.success(), "The altered snapshot must be refused");
        return result.error()->code;
    };

    // The text of a section changes but keeps its size (so its token estimate stays), under the old fingerprint.
    auto edited = snapshot;
    char& last = edited.sections.front().text.back();
    last = last == 'x' ? 'y' : 'x';
    require(edited.sections.front().text.size() == snapshot.sections.front().text.size()
            && codeOfIntegrity(edited) == "project_context.fingerprint_mismatch",
        "A same-size edit of a section text under the old fingerprint must be detected");

    // Every other fact that is rendered, counted or fingerprinted.
    const std::vector<std::pair<const char*, void (*)(ProjectContextSnapshot&)>> alterations {
        {"project id", [](ProjectContextSnapshot& value) { value.projectId = "another-project"; }},
        {"selected run", [](ProjectContextSnapshot& value) { value.selectedRunId.reset(); }},
        {"workspace fingerprint", [](ProjectContextSnapshot& value) { value.workspaceFingerprint = std::string(64, 'e'); }},
        {"policy id", [](ProjectContextSnapshot& value) { value.policyId = "project_context_budget_custom"; }},
        {"schema version", [](ProjectContextSnapshot& value) { value.schemaVersion = "project-context-snapshot.v2"; }},
        {"budget", [](ProjectContextSnapshot& value) { value.budget.usedTokens += 1; }},
        {"section token estimate", [](ProjectContextSnapshot& value) { value.sections.front().estimatedTokens += 1; }},
        {"section source", [](ProjectContextSnapshot& value) { value.sections.front().sources.front().id = "other"; }},
        {"the fingerprint itself", [](ProjectContextSnapshot& value) { value.fingerprint = std::string(64, 'f'); }}};
    for (const auto& [name, alter] : alterations) {
        auto altered = snapshot;
        alter(altered);
        require(codeOfIntegrity(altered) == "project_context.fingerprint_mismatch", name);
    }

    // A caller that re-seals the fingerprint still cannot hand over a structure the renderer must not trust.
    const auto resealed = [](ProjectContextSnapshot value) {
        const auto fingerprint = computeProjectContextSnapshotFingerprint(value);
        require(fingerprint.success(), "A well-formed snapshot has a fingerprint");
        value.fingerprint = fingerprint.value();
        return value;
    };
    require(validateProjectContextSnapshotIntegrity(resealed(edited)).success(),
        "A re-sealed snapshot of valid structure is internally consistent (the fingerprint identifies content, it is not a secret)");
    auto authority = snapshot;
    authority.sections.back().authority = ProjectContextAuthority::Authoritative;   // derived Project Memory claims authority
    require(codeOfIntegrity(resealed(authority)) == "project_context.invalid_structure",
        "A section whose authority is not the one its category fixes must be refused even when re-sealed");
    auto repeated = snapshot;
    repeated.sections.push_back(repeated.sections.back());
    require(codeOfIntegrity(resealed(repeated)) == "project_context.invalid_structure",
        "A repeated category must be refused even when re-sealed");
    auto shuffled = snapshot;
    std::swap(shuffled.sections.front(), shuffled.sections.back());
    require(codeOfIntegrity(resealed(shuffled)) == "project_context.invalid_structure",
        "Sections out of canonical order must be refused even when re-sealed");

    auto malformed = snapshot;
    malformed.sections.front().text += "\xC3\x28";
    require(!computeProjectContextSnapshotFingerprint(malformed).success()
            && codeOfIntegrity(malformed) == "project_context.invalid_text",
        "Text that is not valid UTF-8 has no fingerprint and is refused");
}

} // namespace

int main()
{
    try {
        testSectionsAreEmittedInCanonicalOrderWithAuthorityByCategory();
        testAssemblyIsDeterministicAndSourcesAreFlattenedInOrder();
        testFingerprintChangesWithEveryDurableInput();
        testNewlineStyleDoesNotChangeTheFingerprint();
        testEmptySectionIsOmittedButDuplicateCategoryIsRejected();
        testSectionLimitKeepsAPrefixOfWholeItems();
        testMandatoryItemsAreNeverDroppedAndNeverExceedTheirLimit();
        testTotalLimitCutsMemoryThenLedgerThenCatalogAndNeverIdentity();
        testSectionWhoseHeadingDoesNotFitIsOmittedAndReported();
        testInvalidItemsAndRequestsAreRejected();
        testAbsolutePathsAreRejectedButRelativeAndUrlShapesAreFine();
        testPathGuardCoversUnusualSegmentsUrisRootedAndDevicePaths();
        testIdentitiesAndDigestsAreStrictSingleLineText();
        testMandatoryIsAnIdentityRoleAndTheIdentityIsRequired();
        testProductionAssemblyAlwaysUsesTheFixedV1Budget();
        testPublicUtf8HelperMatchesTheAssemblerRule();
        testFingerprintIsPinnedForRestartStability();
        testIntegrityRecomputesTheFingerprintFromTheContent();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
