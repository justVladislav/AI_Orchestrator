#include "ai/application/execution/RunInterfaceContract.hpp"

#include "ai/application/context/ProjectContextSnapshot.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace ai::application {

namespace {

using Contracts = std::vector<RunInterfaceContract>;

common::Result<Contracts> fail(std::string code, std::string message)
{
    return common::Result<Contracts>::failure({std::move(code), std::move(message)});
}

std::string lowerAscii(std::string_view value)
{
    std::string out(value);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

bool isAsciiAlpha(char ch) noexcept { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }
bool isAsciiDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

bool validContractId(std::string_view id) noexcept
{
    if (id.empty() || id.size() > kRunInterfaceContractMaxIdLength || !isAsciiAlpha(id.front())) return false;
    return std::all_of(id.begin(), id.end(), [](char ch) {
        return isAsciiAlpha(ch) || isAsciiDigit(ch) || ch == '_' || ch == '.' || ch == '-';
    });
}

// U+0085, U+2028 and U+2029 end a line for many renderers although they are not ASCII controls.
bool containsUnicodeLineBreak(std::string_view text) noexcept
{
    return text.find("\xC2\x85") != std::string_view::npos || text.find("\xE2\x80\xA8") != std::string_view::npos
        || text.find("\xE2\x80\xA9") != std::string_view::npos;
}

bool hasAsciiControl(std::string_view text, bool allowNewlineAndTab) noexcept
{
    return std::any_of(text.begin(), text.end(), [allowNewlineAndTab](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        if (allowNewlineAndTab && (ch == '\n' || ch == '\t')) return false;
        return ch < 0x20 || ch == 0x7F;
    });
}

std::string trimmed(std::string_view value)
{
    const auto isSpace = [](char ch) { return ch == ' ' || ch == '\t'; };
    while (!value.empty() && isSpace(value.front())) value.remove_prefix(1);
    while (!value.empty() && isSpace(value.back())) value.remove_suffix(1);
    return std::string(value);
}

std::string normalizeLineEndings(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

bool onlyWhitespace(std::string_view text) noexcept
{
    return std::all_of(text.begin(), text.end(), [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n'; });
}

// Returns the normalized scope, or an empty string when the scope is not acceptable.
std::string normalizeScope(std::string_view raw)
{
    if (raw.empty() || raw.size() > kRunInterfaceContractMaxScopeBytes || !projectContextIsValidUtf8(raw)
        || hasAsciiControl(raw, false) || containsUnicodeLineBreak(raw)) {
        return {};
    }
    // ',' and ';' separate scopes in the free-text form the New Run dialog edits, so a scope holding one could not
    // survive a save -> Edit & rerun round trip: it would come back as two scopes with a different delivery area.
    if (raw.find_first_of("*?<>|\":,;") != std::string_view::npos) return {};
    std::string text(raw);
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.front() == '/') return {};

    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('/', start);
        if (end == std::string::npos) end = text.size();
        const std::string segment = text.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") continue;
        // Windows resolves a trailing space or dot away, so such a segment would name a different path than it
        // spells. This also rejects ".." (and "..." and so on), so no scope can climb out of the workspace.
        if (segment.back() == ' ' || segment.back() == '.') return {};
        if (!out.empty()) out.push_back('/');
        out += segment;
    }
    // The dialog trims the spaces around each piece of that free-text form, so a scope that starts with a space
    // would come back changed (a trailing space is already refused per segment above).
    if (!out.empty() && out.front() == ' ') return {};
    return out;
}

std::string pathKey(std::string_view path)
{
    std::string key(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    return lowerAscii(key);
}

bool scopeCovers(const std::string& scopeKey, const std::string& pathKeyValue)
{
    if (pathKeyValue == scopeKey) return true;
    return pathKeyValue.size() > scopeKey.size() && pathKeyValue.compare(0, scopeKey.size(), scopeKey) == 0
        && pathKeyValue[scopeKey.size()] == '/';
}

std::string appliesToLabel(const RunInterfaceContract& contract)
{
    if (contract.appliesToPaths.empty()) return "all tasks";
    std::string label;
    for (const auto& scope : contract.appliesToPaths) {
        if (!label.empty()) label += ", ";
        label += scope;
    }
    return label;
}

void renderBlock(std::ostringstream& out, const RunInterfaceContract& contract, bool withScope)
{
    out << "\n## " << contract.id << " (" << (contract.kind == RunContractKind::Constraint ? "constraint" : "interface contract")
        << "): " << contract.title << "\n";
    if (withScope) out << "Applies to: " << appliesToLabel(contract) << "\n";
    // The byte count lets a reader (and a test) tell the verbatim body from anything that merely looks like a marker
    // inside it.
    out << "[contract " << contract.id << " begin, " << contract.text.size() << " bytes]\n"
        << contract.text << "\n[contract " << contract.id << " end]\n";
}

} // namespace

const char* toString(RunContractKind kind) noexcept
{
    switch (kind) {
    case RunContractKind::InterfaceContract: return "interface_contract";
    case RunContractKind::Constraint: return "constraint";
    }
    return "interface_contract";
}

bool runContractKindFromString(std::string_view value, RunContractKind& out) noexcept
{
    if (value == "interface_contract") out = RunContractKind::InterfaceContract;
    else if (value == "constraint") out = RunContractKind::Constraint;
    else return false;
    return true;
}

common::Result<Contracts> normalizeRunInterfaceContracts(Contracts contracts)
{
    if (contracts.size() > kRunInterfaceContractMaxCount) {
        return fail("run_contracts.too_many",
            "A Run accepts at most " + std::to_string(kRunInterfaceContractMaxCount) + " interface contracts");
    }
    std::set<std::string> seenIds;
    std::size_t totalText = 0;
    for (auto& contract : contracts) {
        if (!validContractId(contract.id)) {
            return fail("run_contracts.invalid_id",
                "A contract id must start with a letter and use only letters, digits, '_', '.' and '-' (at most "
                    + std::to_string(kRunInterfaceContractMaxIdLength) + " characters)");
        }
        if (!seenIds.insert(lowerAscii(contract.id)).second) {
            return fail("run_contracts.duplicate_id", "Contract id '" + contract.id + "' is used twice");
        }
        // An enum can hold a value no enumerator names (a caller that casts an unchecked number). It is refused,
        // never mapped to a kind, so a malformed payload cannot silently change what a contract means.
        if (contract.kind != RunContractKind::InterfaceContract && contract.kind != RunContractKind::Constraint) {
            return fail("run_contracts.invalid_kind",
                "Contract '" + contract.id + "' has an unknown kind (use interface_contract or constraint)");
        }

        contract.title = trimmed(contract.title);
        if (contract.title.empty() || contract.title.size() > kRunInterfaceContractMaxTitleBytes
            || !projectContextIsValidUtf8(contract.title) || hasAsciiControl(contract.title, false)
            || containsUnicodeLineBreak(contract.title)) {
            return fail("run_contracts.invalid_title",
                "Contract '" + contract.id + "' needs a non-empty single-line title of at most "
                    + std::to_string(kRunInterfaceContractMaxTitleBytes) + " bytes");
        }

        if (!projectContextIsValidUtf8(contract.text)) {
            return fail("run_contracts.invalid_text", "Contract '" + contract.id + "' text is not valid UTF-8");
        }
        contract.text = normalizeLineEndings(contract.text);
        if (hasAsciiControl(contract.text, true) || containsUnicodeLineBreak(contract.text)
            || onlyWhitespace(contract.text)) {
            return fail("run_contracts.invalid_text",
                "Contract '" + contract.id + "' text must be non-empty and free of control characters");
        }
        if (contract.text.size() > kRunInterfaceContractMaxTextBytes) {
            return fail("run_contracts.text_too_long",
                "Contract '" + contract.id + "' text is longer than "
                    + std::to_string(kRunInterfaceContractMaxTextBytes) + " bytes");
        }
        totalText += contract.text.size();
        if (totalText > kRunInterfaceContractMaxTotalTextBytes) {
            return fail("run_contracts.total_too_long",
                "All contract texts together are longer than "
                    + std::to_string(kRunInterfaceContractMaxTotalTextBytes) + " bytes");
        }

        std::vector<std::string> scopes;
        std::set<std::string> seenScopes;
        for (const auto& rawScope : contract.appliesToPaths) {
            const std::string scope = normalizeScope(rawScope);
            if (scope.empty()) {
                return fail("run_contracts.invalid_scope",
                    "Contract '" + contract.id
                        + "' has a scope that is not a relative path prefix (no wildcards, no ',' or ';', no "
                          "absolute path, no '..', no empty entry, no leading space)");
            }
            if (seenScopes.insert(lowerAscii(scope)).second) scopes.push_back(scope);
        }
        if (scopes.size() > kRunInterfaceContractMaxScopesPerContract) {
            return fail("run_contracts.too_many_scopes",
                "Contract '" + contract.id + "' names more than "
                    + std::to_string(kRunInterfaceContractMaxScopesPerContract) + " scopes");
        }
        contract.appliesToPaths = std::move(scopes);
    }
    return common::Result<Contracts>::ok(std::move(contracts));
}

std::string runInterfaceContractsFingerprint(const Contracts& contracts)
{
    if (contracts.empty()) return {};
    nlohmann::json list = nlohmann::json::array();
    for (const auto& contract : contracts) {
        nlohmann::json item;
        item["id"] = contract.id;
        item["kind"] = toString(contract.kind);
        item["title"] = contract.title;
        item["text"] = contract.text;
        item["scopes"] = contract.appliesToPaths;
        list.push_back(std::move(item));
    }
    nlohmann::json root;
    root["schema"] = std::string(kRunInterfaceContractsSchemaVersion);
    root["contracts"] = std::move(list);
    return sha256Hex(root.dump());
}

std::string serializeRunInterfaceContracts(const Contracts& contracts)
{
    nlohmann::json list = nlohmann::json::array();
    for (const auto& contract : contracts) {
        nlohmann::json item;
        item["id"] = contract.id;
        item["kind"] = toString(contract.kind);
        item["title"] = contract.title;
        item["text"] = contract.text;
        item["appliesToPaths"] = contract.appliesToPaths;
        list.push_back(std::move(item));
    }
    return list.dump();
}

common::Result<Contracts> parseRunInterfaceContracts(std::string_view stored)
{
    const auto corrupt = [](const char* what) {
        return fail("persistence.invalid_data", std::string("Stored Run interface contracts are corrupt: ") + what);
    };
    if (stored.empty()) return common::Result<Contracts>::ok(Contracts {});
    const auto document = nlohmann::json::parse(stored.begin(), stored.end(), nullptr, false);
    if (document.is_discarded() || !document.is_array()) return corrupt("not a JSON array");

    static const std::set<std::string> expectedKeys {"id", "kind", "title", "text", "appliesToPaths"};
    Contracts parsed;
    for (const auto& item : document) {
        if (!item.is_object() || item.size() != expectedKeys.size()) return corrupt("unexpected entry shape");
        for (const auto& key : expectedKeys) {
            if (!item.contains(key)) return corrupt("a field is missing");
        }
        const auto& scopes = item.at("appliesToPaths");
        if (!item.at("id").is_string() || !item.at("kind").is_string() || !item.at("title").is_string()
            || !item.at("text").is_string() || !scopes.is_array()) {
            return corrupt("a field has the wrong type");
        }
        RunInterfaceContract contract;
        contract.id = item.at("id").get<std::string>();
        if (!runContractKindFromString(item.at("kind").get_ref<const std::string&>(), contract.kind)) {
            return corrupt("unknown kind");
        }
        contract.title = item.at("title").get<std::string>();
        contract.text = item.at("text").get<std::string>();
        for (const auto& scope : scopes) {
            if (!scope.is_string()) return corrupt("a scope is not a string");
            contract.appliesToPaths.push_back(scope.get<std::string>());
        }
        parsed.push_back(std::move(contract));
    }

    auto normalizedList = normalizeRunInterfaceContracts(parsed);
    if (!normalizedList.success()) return corrupt("the contracts are no longer valid");
    // The stored form is the canonical form. Anything the normalizer would still change was not written by us.
    if (serializeRunInterfaceContracts(normalizedList.value()) != serializeRunInterfaceContracts(parsed)) {
        return corrupt("not in canonical form");
    }
    return normalizedList;
}

bool runContractScopeCoversPath(std::string_view scope, std::string_view path)
{
    return scopeCovers(pathKey(scope), pathKey(path));
}

std::vector<const RunInterfaceContract*> selectRunContractsForTaskPaths(
    const Contracts& contracts, const std::vector<std::string>& taskPaths)
{
    std::vector<std::string> keys;
    keys.reserve(taskPaths.size());
    for (const auto& path : taskPaths) keys.push_back(pathKey(path));

    std::vector<const RunInterfaceContract*> selected;
    for (const auto& contract : contracts) {
        if (contract.appliesToPaths.empty()) {
            selected.push_back(&contract);
            continue;
        }
        const bool touched = std::any_of(contract.appliesToPaths.begin(), contract.appliesToPaths.end(),
            [&keys](const std::string& scope) {
                const std::string scopeKey = pathKey(scope);
                return std::any_of(keys.begin(), keys.end(),
                    [&scopeKey](const std::string& key) { return scopeCovers(scopeKey, key); });
            });
        if (touched) selected.push_back(&contract);
    }
    return selected;
}

std::string renderRunContractsForWorker(const std::vector<const RunInterfaceContract*>& selected)
{
    if (selected.empty()) return {};
    std::ostringstream out;
    out << "# Run interface contracts -- authoritative, copy exactly\n"
        << "The Run author fixed the blocks below. They are not suggestions and not a summary: every property, "
           "signal, field and type name in them is exact, and the text between the begin and end markers is "
           "delivered byte for byte. Do not rename, drop or reinterpret anything in them. If the task objective, "
           "an acceptance criterion or a dependency output disagrees with a block, the block wins; if you "
           "cannot satisfy it, say so instead of changing it.\n";
    for (const auto* contract : selected) renderBlock(out, *contract, false);
    return out.str();
}

std::string renderRunContractsForPlanner(const Contracts& contracts)
{
    if (contracts.empty()) return {};
    std::ostringstream out;
    out << "# Run interface contracts and constraints (authoritative, fixed by the Run author)\n"
        << "The Backend delivers each block below VERBATIM, byte for byte, to every worker whose task owns, "
           "creates, must read or references a path under the block's \"Applies to\" scope (to every worker "
           "when it says \"all tasks\"). Do not copy, shorten or rewrite them in task objectives or acceptance "
           "criteria: the workers receive the exact text from the Backend. Plan the tasks so that every "
           "producer and every consumer of a block is a task the block reaches, and never plan a task that "
           "contradicts one.\n";
    for (const auto& contract : contracts) renderBlock(out, contract, true);
    return out.str();
}

} // namespace ai::application
