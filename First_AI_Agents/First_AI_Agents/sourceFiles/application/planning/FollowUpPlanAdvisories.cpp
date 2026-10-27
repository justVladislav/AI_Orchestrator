#include "ai/application/planning/FollowUpPlanAdvisories.hpp"

#include "ai/application/agents/AgentToolTurnLoopRunner.hpp"
#include "ai/application/RunInterfaceContract.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace ai::application {
namespace {

static_assert(kFollowUpDraftMaxNamedFiles * 2 <= kAgentToolTurnLoopMaxTotalToolCalls,
    "A draft may name at most half of what the Planner can read in one session");

struct Catalog {
    std::vector<std::string_view> paths;
    bool truncated {false};
};

// The visible catalog is what the model was shown: the included `workspace_file` sources of the workspace catalog
// section (the omitted rows are not in `sources`).
Catalog visibleCatalog(const ProjectContextSnapshot& snapshot)
{
    Catalog catalog;
    for (const auto& section : snapshot.sections) {
        if (section.category != ProjectContextCategory::WorkspaceCatalog) continue;
        catalog.truncated = catalog.truncated || section.truncated;
        for (const auto& source : section.sources) {
            if (source.kind == "workspace_file") catalog.paths.push_back(source.id);
        }
    }
    return catalog;
}

void appendPathAdvisories(
    std::vector<FollowUpPlanAdvisory>& out,
    const RunInterfaceContract& contract,
    std::string_view draftKey,
    const Catalog& catalog)
{
    for (const auto& scope : contract.appliesToPaths) {
        const bool covered = std::any_of(catalog.paths.begin(), catalog.paths.end(), [&scope](std::string_view path) {
            return runContractScopeCoversPath(scope, path);
        });
        if (covered) continue;
        FollowUpPlanAdvisory advisory;
        advisory.code = std::string(kFollowUpAdvisoryPathNotInCatalog);
        advisory.message = "Contract " + contract.id + " applies to \"" + scope
            + "\", which covers no file of the visible workspace catalog"
            + (catalog.truncated ? " (the catalog was cut, so the file may exist but was not shown)" : "")
            + ". Check that the path exists or will be created by a draft.";
        advisory.contractId = contract.id;
        advisory.draftKey = std::string(draftKey);
        advisory.path = scope;
        advisory.catalogTruncated = catalog.truncated;
        out.push_back(std::move(advisory));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// File names
// ---------------------------------------------------------------------------------------------------------------

bool nameChar(char raw) noexcept
{
    const auto c = static_cast<unsigned char>(raw);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'
        || c == '/' || c == '+';
}

std::string lowerAscii(std::string_view value)
{
    std::string out(value);
    for (auto& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

std::string_view baseNameOf(std::string_view path) noexcept
{
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool knownExtension(const std::string& lowerExtension)
{
    static const std::set<std::string> extensions {"qml", "cpp", "hpp", "h", "c", "cc", "cs", "js", "mjs", "cjs", "ts", "tsx", "jsx",
        "json", "md", "txt", "cmake", "py", "rs", "go", "java", "kt", "html", "css", "scss", "xml", "yml", "yaml", "toml", "ini",
        "sql", "sh", "ps1", "bat", "qrc", "ui", "pro", "pri", "svg", "png"};
    return extensions.count(lowerExtension) != 0;
}

// A token that looks like a file name, normalised (no leading "./" or "/"), or an empty string.
std::string fileNameOf(std::string_view token)
{
    while (!token.empty() && (token.back() == '.' || token.back() == '-' || token.back() == '+')) token.remove_suffix(1);
    while (token.size() >= 2 && token[0] == '.' && token[1] == '/') token.remove_prefix(2);
    while (!token.empty() && token.front() == '/') token.remove_prefix(1);
    if (token.empty() || token.back() == '/') return {};
    const std::string_view base = baseNameOf(token);
    const auto dot = base.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= base.size()) return {};
    if (!knownExtension(lowerAscii(base.substr(dot + 1)))) return {};
    return std::string(token);
}

std::set<std::string> baseNameSet(const std::vector<std::string>& names)
{
    std::set<std::string> out;
    for (const auto& name : names) out.insert(lowerAscii(baseNameOf(name)));
    return out;
}

void addNames(std::vector<std::string>& into, std::string_view text)
{
    for (auto& name : extractFollowUpFileNames(text)) into.push_back(std::move(name));
}

} // namespace

std::vector<std::string> extractFollowUpFileNames(std::string_view text)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!nameChar(text[i])) { ++i; continue; }
        std::size_t j = i;
        while (j < text.size() && nameChar(text[j])) ++j;
        std::string name = fileNameOf(text.substr(i, j - i));
        i = j;
        if (name.empty()) continue;
        if (seen.insert(lowerAscii(name)).second) out.push_back(std::move(name));
    }
    return out;
}

std::vector<FollowUpPlanAdvisory> computeFollowUpPlanAdvisories(
    const NextRunPlan& plan,
    const ProjectContextSnapshot& snapshot,
    std::string_view goal,
    std::string_view shownContext)
{
    const Catalog catalog = visibleCatalog(snapshot);
    std::vector<FollowUpPlanAdvisory> advisories;

    for (const auto& contract : plan.sharedContracts) {
        const NextRunPlanDraft* only = nullptr;
        std::size_t users = 0;
        for (const auto& draft : plan.drafts) {
            if (std::find(draft.sharedContractIds.begin(), draft.sharedContractIds.end(), contract.id)
                == draft.sharedContractIds.end()) continue;
            ++users;
            only = &draft;
        }
        if (users == 1) {
            FollowUpPlanAdvisory advisory;
            advisory.code = std::string(kFollowUpAdvisorySingleUseSharedContract);
            advisory.message = "Shared contract " + contract.id + " is listed by only one draft (" + only->key
                + "). A shared contract is meant for a data shape that at least two drafts use; "
                  "move it into that draft's own contracts or share it with the other draft.";
            advisory.contractId = contract.id;
            advisory.draftKey = only->key;
            advisories.push_back(std::move(advisory));
        }
        appendPathAdvisories(advisories, contract, {}, catalog);
    }

    for (const auto& draft : plan.drafts) {
        // `contracts` is the draft's shared contracts first, then its own; only the own ones are this draft's to check.
        for (std::size_t index = draft.sharedContractCount; index < draft.contracts.size(); ++index) {
            appendPathAdvisories(advisories, draft.contracts[index], draft.key, catalog);
        }
    }

    // The names a draft's text mentions: title, task and its own contracts' title and text.
    const auto draftNames = [](const NextRunPlanDraft& draft) {
        std::vector<std::string> names;
        addNames(names, draft.title);
        addNames(names, draft.task);
        for (std::size_t index = draft.sharedContractCount; index < draft.contracts.size(); ++index) {
            addNames(names, draft.contracts[index].title);
            addNames(names, draft.contracts[index].text);
        }
        return names;
    };

    if (!goal.empty()) {
        const std::vector<std::string> goalNames = extractFollowUpFileNames(goal);

        std::vector<std::string> planNames;
        addNames(planNames, plan.title);
        for (const auto& contract : plan.sharedContracts) {
            addNames(planNames, contract.title);
            addNames(planNames, contract.text);
            for (const auto& scope : contract.appliesToPaths) addNames(planNames, scope);
        }
        for (const auto& draft : plan.drafts) {
            for (auto& name : draftNames(draft)) planNames.push_back(std::move(name));
            for (const auto& contract : draft.contracts) {
                for (const auto& scope : contract.appliesToPaths) addNames(planNames, scope);
            }
        }
        const std::set<std::string> planBases = baseNameSet(planNames);

        std::size_t reported = 0;
        for (const auto& name : goalNames) {
            if (planBases.count(lowerAscii(baseNameOf(name))) != 0) continue;
            if (reported++ >= kFollowUpMaxNameAdvisoriesPerCode) break;
            FollowUpPlanAdvisory advisory;
            advisory.code = std::string(kFollowUpAdvisoryGoalNameNotInPlan);
            advisory.message = "The goal names \"" + name + "\", but no draft's title, task or contract mentions it. "
                "The plan may have lost or renamed it; check the tasks before confirming.";
            advisory.path = name;
            advisories.push_back(std::move(advisory));
        }

        std::set<std::string> known = baseNameSet(goalNames);
        for (const auto& base : baseNameSet(extractFollowUpFileNames(shownContext))) known.insert(base);
        for (const auto path : catalog.paths) known.insert(lowerAscii(baseNameOf(path)));

        reported = 0;
        for (const auto& draft : plan.drafts) {
            std::set<std::string> reportedHere;
            for (const auto& name : draftNames(draft)) {
                const std::string base = lowerAscii(baseNameOf(name));
                if (known.count(base) != 0 || !reportedHere.insert(base).second) continue;
                if (reported++ >= kFollowUpMaxNameAdvisoriesPerCode) break;
                FollowUpPlanAdvisory advisory;
                advisory.code = std::string(kFollowUpAdvisoryTaskNameUnknown);
                advisory.message = "Draft " + draft.key + " names \"" + name + "\", which is neither in the goal nor in the project "
                    "context the planner was shown"
                    + std::string(catalog.truncated ? " (the catalog was cut, so the file may exist but was not shown)" : "")
                    + ". It may be a mistyped or invented name; check it, or ignore it if the draft is meant to create that file.";
                advisory.draftKey = draft.key;
                advisory.path = name;
                advisory.catalogTruncated = catalog.truncated;
                advisories.push_back(std::move(advisory));
            }
        }
        for (const auto& contract : plan.sharedContracts) {
            std::vector<std::string> names;
            addNames(names, contract.title);
            addNames(names, contract.text);
            std::set<std::string> reportedHere;
            for (const auto& name : names) {
                const std::string base = lowerAscii(baseNameOf(name));
                if (known.count(base) != 0 || !reportedHere.insert(base).second) continue;
                if (reported++ >= kFollowUpMaxNameAdvisoriesPerCode) break;
                FollowUpPlanAdvisory advisory;
                advisory.code = std::string(kFollowUpAdvisoryTaskNameUnknown);
                advisory.message = "Shared contract " + contract.id + " names \"" + name + "\", which is neither in the goal nor in the "
                    "project context the planner was shown. It may be a mistyped or invented name; check it.";
                advisory.contractId = contract.id;
                advisory.path = name;
                advisory.catalogTruncated = catalog.truncated;
                advisories.push_back(std::move(advisory));
            }
        }
    }

    for (const auto& draft : plan.drafts) {
        std::set<std::string> distinct;
        for (const auto& name : draftNames(draft)) distinct.insert(lowerAscii(name));
        if (distinct.size() > kFollowUpDraftMaxNamedFiles) {
            FollowUpPlanAdvisory advisory;
            advisory.code = std::string(kFollowUpAdvisoryDraftNamesManyFiles);
            advisory.message = "Draft " + draft.key + " names " + std::to_string(distinct.size()) + " files. The Planner reads each named file "
                "with one tool call and has at most " + std::to_string(kAgentToolTurnLoopMaxTotalToolCalls) + " calls per session, so a draft "
                "naming more than " + std::to_string(kFollowUpDraftMaxNamedFiles) + " files is likely to fail in planning, before any work. "
                "Split it, or drop the files it only mentions.";
            advisory.draftKey = draft.key;
            advisories.push_back(std::move(advisory));
        }
    }

    for (const auto& draft : plan.drafts) {
        if (!draft.staffing.has_value()) continue;
        std::vector<std::string> names;
        addNames(names, draft.task);
        for (std::size_t index = draft.sharedContractCount; index < draft.contracts.size(); ++index) {
            addNames(names, draft.contracts[index].text);
        }
        const std::size_t named = baseNameSet(names).size();
        if (named == 0 || draft.staffing->workers.size() <= named) continue;
        FollowUpPlanAdvisory advisory;
        advisory.code = std::string(kFollowUpAdvisoryStaffingExceedsNamedFiles);
        advisory.message = "Draft " + draft.key + " suggests " + std::to_string(draft.staffing->workers.size())
            + " workers but its task names only " + std::to_string(named) + " file(s). The suggestion is an estimate; "
              "more workers than files rarely helps.";
        advisory.draftKey = draft.key;
        advisories.push_back(std::move(advisory));
    }
    return advisories;
}

namespace {

nlohmann::json advisoryToJson(const FollowUpPlanAdvisory& advisory)
{
    nlohmann::json value = nlohmann::json::object();
    value["code"] = advisory.code;
    value["message"] = advisory.message;
    value["contractId"] = advisory.contractId;
    value["draftKey"] = advisory.draftKey;
    value["path"] = advisory.path;
    value["catalogTruncated"] = advisory.catalogTruncated;
    return value;
}

} // namespace

std::string serializeFollowUpPlanAdvisories(const std::vector<FollowUpPlanAdvisory>& advisories, std::size_t maxBytes)
{
    // Each element is dumped on its own, so the size of the whole array is known as it grows (a comma and the two brackets).
    std::string out = "[";
    bool first = true;
    for (const auto& advisory : advisories) {
        // The plan parser guarantees valid UTF-8, but a serialiser that can throw has no place on the path that stores a paid answer.
        const std::string element = advisoryToJson(advisory).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        const std::size_t needed = out.size() + (first ? 0 : 1) + element.size() + 1;
        if (needed > maxBytes) break;
        if (!first) out += ',';
        out += element;
        first = false;
    }
    out += ']';
    return out;
}

common::Result<std::vector<FollowUpPlanAdvisory>> parseFollowUpPlanAdvisories(std::string_view stored)
{
    using Result = common::Result<std::vector<FollowUpPlanAdvisory>>;
    const auto bad = [](const char* what) {
        return Result::failure({"follow_up.invalid_advisories", std::string("The stored advisories are not readable: ") + what});
    };
    const auto parsed = nlohmann::json::parse(stored.begin(), stored.end(), nullptr, false);
    if (!parsed.is_array()) return bad("not an array");
    std::vector<FollowUpPlanAdvisory> advisories;
    for (const auto& item : parsed) {
        if (!item.is_object() || item.size() != 6) return bad("an entry is not an object of six fields");
        const auto text = [&](const char* key, std::string& out) {
            const auto found = item.find(key);
            if (found == item.end() || !found->is_string()) return false;
            out = found->get<std::string>();
            return true;
        };
        FollowUpPlanAdvisory advisory;
        const auto flag = item.find("catalogTruncated");
        if (!text("code", advisory.code) || !text("message", advisory.message) || !text("contractId", advisory.contractId)
                || !text("draftKey", advisory.draftKey) || !text("path", advisory.path) || flag == item.end() || !flag->is_boolean()) {
            return bad("an entry has a missing or mistyped field");
        }
        advisory.catalogTruncated = flag->get<bool>();
        advisories.push_back(std::move(advisory));
    }
    return Result::ok(std::move(advisories));
}

} // namespace ai::application
