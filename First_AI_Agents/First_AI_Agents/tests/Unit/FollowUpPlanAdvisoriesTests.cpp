#include "ai/application/FollowUpPlanAdvisories.hpp"

#include "ai/application/RunInterfaceContract.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ai::application;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------------------------------------------
// Fixtures. Plans go through the REAL parser (the advisories run over parsed plans), snapshots through the real
// assembler.
// ---------------------------------------------------------------------------------------------------------------

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

// `bulkyFiles` > 0 adds that many long catalog rows (src/generated/f<i>.cpp) so the 9 000-token catalog share cuts them.
ProjectContextSnapshot snapshot(const std::vector<std::string>& files, int bulkyFiles = 0)
{
    const std::string workspace(64, 'a');
    ProjectContextAssemblyInput input;
    input.projectId = "project-1";
    input.selectedRunId = "run-1";
    input.workspaceFingerprint = workspace;
    std::vector<ProjectContextItem> catalog = {item("workspace identity", "workspace", "current", true, workspace)};
    for (const auto& path : files) catalog.push_back(item(path + "; bytes=1", "workspace_file", path));
    for (int index = 0; index < bulkyFiles; ++index) {
        const std::string path = "src/generated/f" + std::to_string(index) + ".cpp";
        catalog.push_back(item(path + " " + std::string(180, 'c'), "workspace_file", path));
    }
    input.sections = {
        {ProjectContextCategory::ProjectMetadata, {item("Project: Demo", "project", "project-1", true)}},
        {ProjectContextCategory::SelectedLineage, {item("selected run run-1", "run", "run-1", true)}},
        // Items whose ids look like paths but are not workspace files: they must never count as catalog.
        // The assembler accepts any safe kind in any section, so a `workspace_file` source outside the catalog section
        // is possible and must not be mistaken for a catalog row.
        {ProjectContextCategory::WorkLedger,
            {item("old work", "ledger_entry", "legacy/old.cpp"), item("ghost", "workspace_file", "ghost/file.cpp")}},
        {ProjectContextCategory::WorkspaceCatalog, std::move(catalog)}};
    const auto result = assembleProjectContextSnapshot(input);
    require(result.success(), "Snapshot fixture must assemble");
    return result.value();
}

const std::vector<std::string>& standardFiles()
{
    static const std::vector<std::string> files = {"src/a.cpp", "src/ui/Main.qml", "docs/readme.md"};
    return files;
}

std::string jsonString(const std::string& value) { return "\"" + value + "\""; }

std::string scopesJson(const std::vector<std::string>& scopes)
{
    std::string out = "[";
    for (std::size_t index = 0; index < scopes.size(); ++index) {
        if (index != 0) out += ",";
        std::string escaped;
        for (const char ch : scopes[index]) {
            if (ch == '\\') escaped += "\\\\";
            else escaped.push_back(ch);
        }
        out += jsonString(escaped);
    }
    return out + "]";
}

std::string contractJson(const std::string& id, const std::vector<std::string>& scopes)
{
    return "{\"id\":" + jsonString(id) + ",\"kind\":\"interface_contract\",\"title\":\"Title " + id
        + "\",\"text\":\"Exact names for " + id + ".\",\"appliesToPaths\":" + scopesJson(scopes) + "}";
}

std::string idsJson(const std::vector<std::string>& ids)
{
    std::string out = "[";
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) out += ",";
        out += jsonString(ids[index]);
    }
    return out + "]";
}

std::string draftJson(const std::string& key, const std::vector<std::string>& sharedIds,
    const std::vector<std::string>& own)
{
    std::string contracts = "[";
    for (std::size_t index = 0; index < own.size(); ++index) {
        if (index != 0) contracts += ",";
        contracts += own[index];
    }
    contracts += "]";
    return "{\"key\":" + jsonString(key) + ",\"title\":\"Draft " + key
        + "\",\"task\":\"First inspect the files, then change them.\",\"dependsOn\":[],\"sharedContractIds\":"
        + idsJson(sharedIds) + ",\"contracts\":" + contracts + "}";
}

NextRunPlan plan(const std::vector<std::string>& sharedContracts, const std::vector<std::string>& drafts)
{
    const auto join = [](const std::vector<std::string>& parts) {
        std::string out = "[";
        for (std::size_t index = 0; index < parts.size(); ++index) {
            if (index != 0) out += ",";
            out += parts[index];
        }
        return out + "]";
    };
    const std::string json = "{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"sharedContracts\":"
        + join(sharedContracts) + ",\"drafts\":" + join(drafts) + "}";
    const auto parsed = parseNextRunPlan(json);
    require(parsed.success(), "Plan fixture must parse: " + (parsed.success() ? std::string() : parsed.error()->message));
    return parsed.value();
}

std::vector<FollowUpPlanAdvisory> advisories(const NextRunPlan& value, const ProjectContextSnapshot& shot)
{
    return computeFollowUpPlanAdvisories(value, shot);
}

std::string describe(const std::vector<FollowUpPlanAdvisory>& found)
{
    std::string out;
    for (const auto& advisory : found) {
        out += "[" + advisory.code + " c=" + advisory.contractId + " d=" + advisory.draftKey + " p=" + advisory.path + "]";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

void testASharedContractListedByOneDraftIsSingleUse()
{
    const auto value = plan(
        {contractJson("both", {}), contractJson("one", {}), contractJson("none", {})},
        {draftJson("first", {"both", "one"}, {}), draftJson("second", {"both"}, {})});
    const auto found = advisories(value, snapshot(standardFiles()));
    require(found.size() == 1, "Only the contract listed by exactly one draft is flagged: " + describe(found));
    const auto& advisory = found.front();
    require(advisory.code == "single_use_shared_contract" && advisory.contractId == "one"
            && advisory.draftKey == "first" && advisory.path.empty() && !advisory.catalogTruncated,
        "The advisory names the contract and the one draft that lists it");
    require(advisory.message.find("one") != std::string::npos && advisory.message.find("first") != std::string::npos,
        "The message names the contract and the draft");

    const auto lonely = plan({contractJson("solo", {})}, {draftJson("only", {"solo"}, {})});
    const auto lonelyFound = advisories(lonely, snapshot(standardFiles()));
    require(lonelyFound.size() == 1 && lonelyFound[0].draftKey == "only",
        "In a one-draft plan every shared contract is single-use");

    const auto unused = plan({contractJson("nobody", {})}, {draftJson("a", {}, {}), draftJson("b", {}, {})});
    require(advisories(unused, snapshot(standardFiles())).empty(),
        "A contract nobody lists is the parser's own warning, not a single-use advisory");

    const auto triple = plan({contractJson("wide", {})},
        {draftJson("a", {"wide"}, {}), draftJson("b", {"wide"}, {}), draftJson("c", {"wide"}, {})});
    require(advisories(triple, snapshot(standardFiles())).empty(), "Three users are not single-use");
}

void testScopesFollowThePc2PrefixRuleAgainstTheVisibleCatalog()
{
    // src/a.cpp, src/ui/Main.qml and docs/readme.md are visible.
    const auto covered = plan({}, {draftJson("first", {},
        {contractJson("covered", {"src", "src/a.cpp", "SRC/UI/main.qml", "./docs/", ".\\src\\ui"})})});
    require(covered.drafts[0].contracts[0].appliesToPaths.size() == 5, "The parser keeps five distinct scopes");
    const auto none = advisories(covered, snapshot(standardFiles()));
    require(none.empty(), "A directory, a file, another letter case and a normalized spelling are covered: " + describe(none));

    const auto uncovered = plan({}, {draftJson("first", {},
        {contractJson("uncovered", {"src/u", "src/a.cpp/x", "tests", "nope/file.cpp", "src/ui/Main.qml.bak"})})});
    const auto found = advisories(uncovered, snapshot(standardFiles()));
    require(found.size() == 5, "Every uncovered scope is reported once: " + describe(found));
    const std::vector<std::string> expected = {"src/u", "src/a.cpp/x", "tests", "nope/file.cpp", "src/ui/Main.qml.bak"};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        require(found[index].code == "path_not_in_catalog" && found[index].path == expected[index]
                && found[index].contractId == "uncovered" && found[index].draftKey == "first",
            "Scope order and attribution: " + describe(found));
        require(found[index].message.find(expected[index]) != std::string::npos
                && found[index].message.find("uncovered") != std::string::npos,
            "The message names the contract and the path");
    }
}

void testOnlyWorkspaceFileSourcesAreTheCatalog()
{
    const auto value = plan({}, {draftJson("first", {},
        {contractJson("lookalikes", {"legacy", "legacy/old.cpp", "current", "run-1", "project-1", "ghost"})})});
    const auto found = advisories(value, snapshot(standardFiles()));
    require(found.size() == 6,
        "A ledger id, the workspace identity, a Run id, a project id or a workspace_file source outside the catalog "
        "section are no catalog rows: " + describe(found));
}

void testAnEmptyScopeListAppliesToEveryTaskAndIsNeverFlagged()
{
    const auto value = plan({contractJson("all", {})}, {draftJson("a", {"all"}, {contractJson("mine", {})}),
        draftJson("b", {"all"}, {})});
    require(advisories(value, snapshot({})).empty(), "No scope, no path advisory - even with an empty catalog");
    const auto scoped = plan({}, {draftJson("a", {}, {contractJson("mine", {"src"})})});
    const auto found = advisories(scoped, snapshot({}));
    require(found.size() == 1 && found[0].code == "path_not_in_catalog",
        "An empty catalog covers no scope");
}

void testSharedContractsAreReportedOnceAndOwnContractsPerDraft()
{
    const auto value = plan(
        {contractJson("sh", {"missing/shared"})},
        {draftJson("first", {"sh"}, {contractJson("mine", {"missing/first"})}),
         draftJson("second", {"sh"}, {contractJson("other", {"missing/second"})})});
    const auto found = advisories(value, snapshot(standardFiles()));
    require(found.size() == 3, "One shared + one own per draft, no repeat per user: " + describe(found));
    require(found[0].contractId == "sh" && found[0].draftKey.empty() && found[0].path == "missing/shared",
        "A shared contract belongs to the plan, not to a draft");
    require(found[1].contractId == "mine" && found[1].draftKey == "first" && found[1].path == "missing/first",
        "An own contract is attributed to its draft");
    require(found[2].contractId == "other" && found[2].draftKey == "second" && found[2].path == "missing/second",
        "The second draft's own contract follows");
}

void testTheOrderIsSharedFirstThenDraftsAndSingleUseBeforeItsPaths()
{
    const auto value = plan(
        {contractJson("s1", {"gone/a", "gone/b"}), contractJson("s2", {"gone/c"})},
        {draftJson("first", {"s1", "s2"}, {contractJson("own1", {"gone/d"}), contractJson("own2", {"gone/e"})}),
         draftJson("second", {"s2"}, {})});
    const auto found = advisories(value, snapshot(standardFiles()));
    std::vector<std::string> sequence;
    for (const auto& advisory : found) {
        sequence.push_back(advisory.code + ":" + advisory.contractId + ":" + advisory.draftKey + ":" + advisory.path);
    }
    const std::vector<std::string> expected = {
        "single_use_shared_contract:s1:first:",
        "path_not_in_catalog:s1::gone/a",
        "path_not_in_catalog:s1::gone/b",
        "path_not_in_catalog:s2::gone/c",
        "path_not_in_catalog:own1:first:gone/d",
        "path_not_in_catalog:own2:first:gone/e"};
    require(sequence == expected, "Deterministic order, got " + describe(found));
    require(advisories(value, snapshot(standardFiles())).size() == found.size()
            && describe(advisories(value, snapshot(standardFiles()))) == describe(found),
        "The same inputs give the same advisories");
}

void testATruncatedCatalogFlagsTheAdvisoryNeverRefuses()
{
    const auto bulky = snapshot(standardFiles(), 400);
    std::set<std::string> visible;
    bool truncated = false;
    for (const auto& section : bulky.sections) {
        if (section.category != ProjectContextCategory::WorkspaceCatalog) continue;
        truncated = section.truncated;
        for (const auto& source : section.sources) {
            if (source.kind == "workspace_file") visible.insert(source.id);
        }
    }
    require(truncated && visible.count("src/generated/f0.cpp") == 1 && visible.count("src/generated/f399.cpp") == 0,
        "The bulky fixture cuts the catalog tail");

    const auto value = plan({}, {draftJson("first", {},
        {contractJson("generated", {"src/generated/f0.cpp", "src/generated/f399.cpp"})})});
    const auto found = advisories(value, bulky);
    require(found.size() == 1 && found[0].path == "src/generated/f399.cpp" && found[0].catalogTruncated,
        "Only the omitted file is flagged, and the flag says the catalog was cut: " + describe(found));
    require(found[0].message.find("cut") != std::string::npos, "The message tells the reader the catalog was cut");

    const auto complete = advisories(plan({}, {draftJson("first", {}, {contractJson("c", {"nowhere"})})}),
        snapshot(standardFiles()));
    require(complete.size() == 1 && !complete[0].catalogTruncated && complete[0].message.find("cut") == std::string::npos,
        "A complete catalog is not described as cut");
}

void testTheScopeRuleIsTheSameFunctionPc2UsesToDeliverContracts()
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"src", "src"}, {"src", "src/a.cpp"}, {"src", "src/ui/Main.qml"}, {"src", "srcx/a.cpp"}, {"src", "sr"},
        {"src", "SRC/A.CPP"}, {"SRC", "src/a.cpp"}, {"src/ui", "src/ui"}, {"src/ui", "src\\ui\\Main.qml"},
        {"src\\ui", "src/ui/Main.qml"}, {"./src", "src/a.cpp"}, {"src", "./src/a.cpp"}, {"src/a.cpp", "src"},
        {"src/a.cpp", "src/a.cpp/x"}, {"src/u", "src/ui"}, {"docs", "docs"}, {"docs", "doc"}, {"a/b", "a/b/c/d"},
        {"a/b", "a/bc"}, {"Straße", "straße/x"}, {"ÄB", "äb/x"}, {"a", "A/b"}, {"", "a"}};
    for (const auto& [scope, path] : cases) {
        RunInterfaceContract contract;
        contract.id = "c";
        contract.appliesToPaths = {scope};
        const std::vector<RunInterfaceContract> contracts = {contract};
        const bool delivered = !selectRunContractsForTaskPaths(contracts, {path}).empty();
        require(runContractScopeCoversPath(scope, path) == delivered,
            "The scope rule drifted from PC-2 delivery for scope '" + scope + "' path '" + path + "'");
    }
}

void testAnInconsistentPlanIsReadAsFarAsPossible()
{
    auto value = plan({contractJson("sh", {"gone"})}, {draftJson("a", {"sh"}, {contractJson("mine", {"gone2"})})});
    value.drafts[0].sharedContractCount = 99; // beyond its contract list
    auto found = advisories(value, snapshot(standardFiles()));
    require(describe(found).find("mine") == std::string::npos,
        "No contract is the draft's own when the shared count covers the whole list");

    value.drafts[0].sharedContractCount = 0;
    value.drafts[0].sharedContractIds.clear();
    found = advisories(value, snapshot(standardFiles()));
    require(found.size() == 3, "Without a count every listed contract is the draft's own: " + describe(found));
}

// PC-5C: the stored form (`FollowUpPlanAnswer::advisoriesJson`).
std::vector<FollowUpPlanAdvisory> sampleAdvisories(std::size_t count)
{
    std::vector<FollowUpPlanAdvisory> found;
    for (std::size_t index = 0; index < count; ++index) {
        FollowUpPlanAdvisory advisory;
        advisory.code = index % 2 == 0 ? std::string(kFollowUpAdvisoryPathNotInCatalog) : std::string(kFollowUpAdvisorySingleUseSharedContract);
        advisory.message = "Message " + std::to_string(index) + " \"quoted\" \\ and\nnewline and \xC3\xA9";
        advisory.contractId = "C" + std::to_string(index);
        advisory.draftKey = index % 3 == 0 ? "" : "draft-" + std::to_string(index);
        advisory.path = index % 2 == 0 ? "src/p" + std::to_string(index) : "";
        advisory.catalogTruncated = index % 2 == 1;
        found.push_back(std::move(advisory));
    }
    return found;
}

bool sameAdvisory(const FollowUpPlanAdvisory& a, const FollowUpPlanAdvisory& b)
{
    return a.code == b.code && a.message == b.message && a.contractId == b.contractId && a.draftKey == b.draftKey && a.path == b.path
        && a.catalogTruncated == b.catalogTruncated;
}

void testTheStoredFormRoundTripsEveryField()
{
    const auto original = sampleAdvisories(5);
    const std::string stored = serializeFollowUpPlanAdvisories(original, 65536);
    const auto read = parseFollowUpPlanAdvisories(stored);
    require(read.success() && read.value().size() == original.size(), "The stored advisories read back");
    for (std::size_t index = 0; index < original.size(); ++index) {
        require(sameAdvisory(read.value()[index], original[index]), "Every field of advisory " + std::to_string(index) + " survives");
    }
    require(serializeFollowUpPlanAdvisories({}, 65536) == "[]" && parseFollowUpPlanAdvisories("[]").success()
            && parseFollowUpPlanAdvisories("[]").value().empty(),
        "No advisories is the empty array");
}

void testTheWriterKeepsTheLongestPrefixThatFits()
{
    const auto original = sampleAdvisories(6);
    const std::string all = serializeFollowUpPlanAdvisories(original, 1'000'000);
    // Every budget from "nothing fits" up to "everything fits": the result is always a valid array within the budget, made of a
    // PREFIX of the advisories in their order, and it is the longest prefix that fits.
    for (std::size_t budget = 2; budget <= all.size() + 1; ++budget) {
        const std::string stored = serializeFollowUpPlanAdvisories(original, budget);
        require(stored.size() <= budget, "The stored text never exceeds its budget (" + std::to_string(budget) + ")");
        const auto read = parseFollowUpPlanAdvisories(stored);
        require(read.success(), "A cut result is still a valid array (" + std::to_string(budget) + ")");
        for (std::size_t index = 0; index < read.value().size(); ++index) {
            require(sameAdvisory(read.value()[index], original[index]), "...and an unmodified prefix");
        }
        if (read.value().size() < original.size()) {
            const std::string longer = serializeFollowUpPlanAdvisories(
                std::vector<FollowUpPlanAdvisory>(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(read.value().size() + 1)), 1'000'000);
            require(longer.size() > budget, "...the next advisory would not have fitted (" + std::to_string(budget) + ")");
        }
    }
    require(serializeFollowUpPlanAdvisories(original, 1'000'000) == all && all.size() < 1'000'000, "A big budget keeps everything");
    require(serializeFollowUpPlanAdvisories(original, 1) == "[]", "A budget that fits nothing still gives a valid empty array");
}

void testTheReaderIsStrict()
{
    const std::string good = serializeFollowUpPlanAdvisories(sampleAdvisories(1), 65536);
    require(parseFollowUpPlanAdvisories(good).success(), "setup: the control reads");
    const std::vector<std::pair<const char*, std::string>> bad = {
        {"not JSON", "[not json"},
        {"an object", "{}"},
        {"a string", "\"x\""},
        {"an entry that is not an object", "[1]"},
        {"an entry with a missing field", R"([{"code":"c","message":"m","contractId":"x","draftKey":"","path":""}])"},
        {"an entry with an extra field", R"([{"code":"c","message":"m","contractId":"x","draftKey":"","path":"","catalogTruncated":false,"x":1}])"},
        {"a mistyped string field", R"([{"code":1,"message":"m","contractId":"x","draftKey":"","path":"","catalogTruncated":false}])"},
        {"a mistyped flag", R"([{"code":"c","message":"m","contractId":"x","draftKey":"","path":"","catalogTruncated":"no"}])"},
        {"a null field", R"([{"code":"c","message":"m","contractId":null,"draftKey":"","path":"","catalogTruncated":false}])"},
    };
    for (const auto& [what, text] : bad) {
        const auto read = parseFollowUpPlanAdvisories(text);
        require(!read.success() && read.error()->code == "follow_up.invalid_advisories", std::string("The reader refuses ") + what);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// File-name checks and the staffing sanity check (6a (a) hardening).
// ---------------------------------------------------------------------------------------------------------------

std::string taskDraftJson(const std::string& key, const std::string& task, const std::string& staffing = {})
{
    return "{\"key\":" + jsonString(key) + ",\"title\":\"Draft " + key + "\",\"task\":" + jsonString(task)
        + (staffing.empty() ? std::string() : ",\"staffing\":" + staffing) + "}";
}

std::vector<FollowUpPlanAdvisory> withCode(const std::vector<FollowUpPlanAdvisory>& all, std::string_view code)
{
    std::vector<FollowUpPlanAdvisory> out;
    for (const auto& advisory : all) {
        if (advisory.code == code) out.push_back(advisory);
    }
    return out;
}

void testFileNamesAreExtractedByASingleRule()
{
    const auto names = extractFollowUpFileNames(
        "Create `SettingsSection.qml`, then ./qml/views/SettingsView.qml; also settingssection.qml again, e.g. i.e. v1.2, "
        "the dir src/feature/ and CMakeLists.txt. **Notes.md**, (a.cpp) end.");
    const std::vector<std::string> expected {"SettingsSection.qml", "qml/views/SettingsView.qml", "CMakeLists.txt", "Notes.md", "a.cpp"};
    require(names == expected, "Names are file-shaped tokens with a known extension, unique ignoring case, as first written");
    require(extractFollowUpFileNames("").empty() && extractFollowUpFileNames("no files here.").empty(),
        "Prose without a file name has none");
    require(extractFollowUpFileNames(".gitignore x.unknownext tail.").empty(),
        "A dot file and an unknown extension are not names");
}

void testALostAndAGarbledNameAreReported()
{
    // The 5E canary: the goal asked for three files; the plan lost two and wrote a non-existent one.
    const std::string goal = "Create SettingsSection.qml, SettingsRow.qml and SettingsNavigation.qml for the settings view.";
    const auto value = plan({}, {taskDraftJson("one", "First inspect qml/views/SettingsView.qml, then create SettingsSection.qml and Setion.qml."),
                                  taskDraftJson("two", "Wire the section into the view.")});
    const auto found = computeFollowUpPlanAdvisories(value, snapshot(standardFiles()), goal, goal);

    const auto lost = withCode(found, kFollowUpAdvisoryGoalNameNotInPlan);
    require(lost.size() == 2 && lost[0].path == "SettingsRow.qml" && lost[1].path == "SettingsNavigation.qml"
            && lost[0].draftKey.empty() && !lost[0].message.empty(),
        "The two goal names no draft mentions are reported in goal order");
    const auto unknown = withCode(found, kFollowUpAdvisoryTaskNameUnknown);
    require(unknown.size() == 2 && unknown[0].draftKey == "one" && unknown[0].path == "qml/views/SettingsView.qml"
            && unknown[1].path == "Setion.qml",
        "A name the planner was not given is reported once per draft (SettingsView.qml is not in this catalog or goal, Setion.qml is invented)");
    require(withCode(found, kFollowUpAdvisoryStaffingExceedsNamedFiles).empty(), "No staffing, no staffing advisory");
    // Order: the goal advisories come before the per-draft ones.
    std::size_t firstUnknown = found.size();
    std::size_t lastLost = 0;
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (found[i].code == kFollowUpAdvisoryTaskNameUnknown && firstUnknown == found.size()) firstUnknown = i;
        if (found[i].code == kFollowUpAdvisoryGoalNameNotInPlan) lastLost = i;
    }
    require(lastLost < firstUnknown, "goal_name_not_in_plan precedes task_name_unknown");
}

void testANameTheModelWasShownIsNeverUnknown()
{
    const std::string goal = "Fix the settings view.";
    const auto value = plan({}, {taskDraftJson("one", "Inspect src/ui/Main.qml and docs/readme.md, shown.txt and Brief.cpp, then edit Mystery.qml.")});
    const auto found = computeFollowUpPlanAdvisories(
        value, snapshot(standardFiles()), goal, "The outcome brief mentions shown.txt and QML/Brief.CPP.");
    const auto unknown = withCode(found, kFollowUpAdvisoryTaskNameUnknown);
    require(unknown.size() == 1 && unknown[0].path == "Mystery.qml",
        "Catalog names (by base name), shown-context names and names ignoring ASCII case are known; only Mystery.qml is not");
}

void testTheNameChecksNeedAGoalAndNeverRefuse()
{
    const auto value = plan({}, {taskDraftJson("one", "Edit Mystery.qml.")});
    require(computeFollowUpPlanAdvisories(value, snapshot(standardFiles())).empty(),
        "Without a goal there are no name advisories (the old two-argument call is unchanged)");
    const auto cut = computeFollowUpPlanAdvisories(value, snapshot(standardFiles(), 400), "Change things.", "Change things.");
    const auto unknown = withCode(cut, kFollowUpAdvisoryTaskNameUnknown);
    require(unknown.size() == 1 && unknown[0].catalogTruncated,
        "A cut catalog is said so on the advisory: the file may exist but was not shown");
}

void testASharedContractTextNameIsCheckedToo()
{
    const auto shared = std::string("{\"id\":\"S1\",\"kind\":\"interface_contract\",\"title\":\"Ghost.qml shape\",")
        + "\"text\":\"Defined in Ghost.qml and src/ui/Main.qml.\",\"appliesToPaths\":[]}";
    const std::string json = std::string("{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"sharedContracts\":[") + shared
        + "],\"drafts\":[{\"key\":\"one\",\"title\":\"One\",\"task\":\"Use it.\",\"sharedContractIds\":[\"S1\"]},"
          "{\"key\":\"two\",\"title\":\"Two\",\"task\":\"Use it too.\",\"sharedContractIds\":[\"S1\"]}]}";
    const auto parsedPlan = parseNextRunPlan(json);
    require(parsedPlan.success(), "fixture");
    const auto found = computeFollowUpPlanAdvisories(parsedPlan.value(), snapshot(standardFiles()), "Do it.", "Do it.");
    const auto unknown = withCode(found, kFollowUpAdvisoryTaskNameUnknown);
    require(unknown.size() == 1 && unknown[0].contractId == "S1" && unknown[0].draftKey.empty() && unknown[0].path == "Ghost.qml",
        "A shared contract's invented name is reported once, on the contract, not once per draft");
}

// The same unknown name in a draft's title, task and own contract is one finding for that draft, not three.
void testAnUnknownNameIsReportedOncePerDraftWhereverItAppears()
{
    const std::string own = "{\"id\":\"O1\",\"kind\":\"constraint\",\"title\":\"About ghost.QML\",\"text\":\"Keep Ghost.qml small.\",\"appliesToPaths\":[]}";
    const std::string json = std::string("{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"drafts\":[")
        + "{\"key\":\"one\",\"title\":\"Fix Ghost.qml\",\"task\":\"Edit Ghost.qml now.\",\"contracts\":[" + own + "]}]}";
    const auto parsedPlan = parseNextRunPlan(json);
    require(parsedPlan.success(), "fixture");
    const auto found = computeFollowUpPlanAdvisories(parsedPlan.value(), snapshot(standardFiles()), "Do it.", "Do it.");
    const auto unknown = withCode(found, kFollowUpAdvisoryTaskNameUnknown);
    require(unknown.size() == 1 && unknown[0].draftKey == "one" && unknown[0].path == "Ghost.qml",
        "One advisory for the name, ignoring ASCII case, however many places of the draft mention it");
}

// Run 249d9e18: a draft that names more files than the Planner can read in one session is flagged before it is ever started.
void testADraftNamingMoreFilesThanThePlannerCanReadIsFlagged()
{
    const auto fileList = [](int count, const std::string& prefix) {
        std::string text = "Inspect";
        for (int i = 0; i < count; ++i) text += " " + prefix + std::to_string(i) + ".qml";
        return text + ".";
    };
    const int limit = static_cast<int>(kFollowUpDraftMaxNamedFiles);
    // An own contract of the "mixed" draft names five more files, so task + contract together are over the limit.
    const std::string own = "{\"id\":\"O1\",\"kind\":\"constraint\",\"title\":\"Rule\",\"text\":\"" + fileList(5, "Extra")
        + "\",\"appliesToPaths\":[]}";
    const std::string json = std::string("{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"drafts\":[")
        + taskDraftJson("exact", fileList(limit, "A")) + ","
        + taskDraftJson("over", fileList(limit + 1, "B")) + ","
        + "{\"key\":\"mixed\",\"title\":\"Mixed\",\"task\":" + jsonString(fileList(limit - 4, "C")) + ",\"contracts\":[" + own + "]},"
        // the same 24 names twice, once in other case: distinct names are counted, not mentions
        + "{\"key\":\"repeat\",\"title\":\"Repeat\",\"task\":" + jsonString(fileList(limit, "D")) + ",\"contracts\":[{\"id\":\"O2\",\"kind\":\"constraint\","
          "\"title\":\"Same files\",\"text\":\"" + fileList(limit, "d") + "\",\"appliesToPaths\":[]}]}]}";
    const auto parsedPlan = parseNextRunPlan(json);
    require(parsedPlan.success(), "fixture: " + (parsedPlan.success() ? std::string() : parsedPlan.error()->message));
    const auto found = withCode(computeFollowUpPlanAdvisories(parsedPlan.value(), snapshot(standardFiles())), kFollowUpAdvisoryDraftNamesManyFiles);
    require(found.size() == 2 && found[0].draftKey == "over" && found[1].draftKey == "mixed",
        "Exactly the drafts over the limit are flagged: one more than the limit, and one whose task plus own contract go over; "
        "the draft at the limit and the one naming the same names twice are not");
    require(found[0].message.find(std::to_string(limit + 1)) != std::string::npos && found[0].message.find("48") != std::string::npos,
        "The advisory says how many files the draft names and what the Planner can read");
    require(found[0].path.empty() && found[0].contractId.empty(), "It is about the draft, not a file or a contract");
}

void testNameAdvisoriesAreCapped()
{
    std::string task = "Edit";
    for (int i = 0; i < 30; ++i) task += " Invented" + std::to_string(i) + ".qml";
    const auto value = plan({}, {taskDraftJson("one", task + ".")});
    const auto found = computeFollowUpPlanAdvisories(value, snapshot(standardFiles()), "Do it.", "Do it.");
    require(withCode(found, kFollowUpAdvisoryTaskNameUnknown).size() == kFollowUpMaxNameAdvisoriesPerCode,
        "A flood of unknown names is cut at the documented cap");
}

void testStaffingAboveTheNamedFilesIsFlagged()
{
    const auto staffing = [](int workers) {
        std::string list;
        for (int i = 0; i < workers; ++i) {
            if (i > 0) list += ",";
            list += "{\"role\":\"implementation\",\"focus\":\"part " + std::to_string(i) + "\"}";
        }
        return "{\"workers\":[" + list + "],\"parallelism\":1,\"reason\":\"r\"}";
    };
    const auto value = plan({}, {taskDraftJson("one", "Edit src/a.cpp only.", staffing(3)),
                                  taskDraftJson("two", "Edit a.cpp, b.cpp and c.cpp.", staffing(3)),
                                  taskDraftJson("three", "Think about the design.", staffing(4)),
                                  taskDraftJson("four", "Edit src/a.cpp.")});
    const auto found = withCode(computeFollowUpPlanAdvisories(value, snapshot(standardFiles())), kFollowUpAdvisoryStaffingExceedsNamedFiles);
    require(found.size() == 1 && found[0].draftKey == "one",
        "Three workers for one named file is flagged; three for three files, a draft naming no file and a draft without a hint are not");
}

int main()
{
    // Every test runs even if an earlier one fails, so one run reports every broken guarantee.
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAILED " << name << ": " << error.what() << '\n';
        }
    };
    run("testASharedContractListedByOneDraftIsSingleUse", testASharedContractListedByOneDraftIsSingleUse);
    run("testScopesFollowThePc2PrefixRuleAgainstTheVisibleCatalog", testScopesFollowThePc2PrefixRuleAgainstTheVisibleCatalog);
    run("testOnlyWorkspaceFileSourcesAreTheCatalog", testOnlyWorkspaceFileSourcesAreTheCatalog);
    run("testAnEmptyScopeListAppliesToEveryTaskAndIsNeverFlagged", testAnEmptyScopeListAppliesToEveryTaskAndIsNeverFlagged);
    run("testSharedContractsAreReportedOnceAndOwnContractsPerDraft", testSharedContractsAreReportedOnceAndOwnContractsPerDraft);
    run("testTheOrderIsSharedFirstThenDraftsAndSingleUseBeforeItsPaths", testTheOrderIsSharedFirstThenDraftsAndSingleUseBeforeItsPaths);
    run("testATruncatedCatalogFlagsTheAdvisoryNeverRefuses", testATruncatedCatalogFlagsTheAdvisoryNeverRefuses);
    run("testTheScopeRuleIsTheSameFunctionPc2UsesToDeliverContracts", testTheScopeRuleIsTheSameFunctionPc2UsesToDeliverContracts);
    run("testAnInconsistentPlanIsReadAsFarAsPossible", testAnInconsistentPlanIsReadAsFarAsPossible);
    run("testTheStoredFormRoundTripsEveryField", testTheStoredFormRoundTripsEveryField);
    run("testTheWriterKeepsTheLongestPrefixThatFits", testTheWriterKeepsTheLongestPrefixThatFits);
    run("testTheReaderIsStrict", testTheReaderIsStrict);
    run("testFileNamesAreExtractedByASingleRule", testFileNamesAreExtractedByASingleRule);
    run("testALostAndAGarbledNameAreReported", testALostAndAGarbledNameAreReported);
    run("testANameTheModelWasShownIsNeverUnknown", testANameTheModelWasShownIsNeverUnknown);
    run("testTheNameChecksNeedAGoalAndNeverRefuse", testTheNameChecksNeedAGoalAndNeverRefuse);
    run("testASharedContractTextNameIsCheckedToo", testASharedContractTextNameIsCheckedToo);
    run("testNameAdvisoriesAreCapped", testNameAdvisoriesAreCapped);
    run("testADraftNamingMoreFilesThanThePlannerCanReadIsFlagged", testADraftNamingMoreFilesThanThePlannerCanReadIsFlagged);
    run("testAnUnknownNameIsReportedOncePerDraftWhereverItAppears", testAnUnknownNameIsReportedOncePerDraftWhereverItAppears);
    run("testStaffingAboveTheNamedFilesIsFlagged", testStaffingAboveTheNamedFilesIsFlagged);
    if (failures != 0) {
        std::cerr << "FollowUpPlanAdvisories tests failed: " << failures << " test(s)\n";
        return 1;
    }
    std::cout << "FollowUpPlanAdvisories tests passed\n";
    return 0;
}
