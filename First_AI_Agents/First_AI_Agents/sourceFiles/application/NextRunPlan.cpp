#include "ai/application/NextRunPlan.hpp"

#include "ai/application/ProjectContextSnapshot.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace ai::application {

namespace {

using Json = nlohmann::json;
using Contracts = std::vector<RunInterfaceContract>;

constexpr std::size_t kMaxJsonDepth = 8;
constexpr std::size_t kMaxEchoedKeyBytes = 64;

common::Error makeError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure(makeError(std::move(code), std::move(message)));
}

std::string_view trimAsciiSpace(std::string_view value) noexcept
{
    const auto isSpace = [](char ch) { return ch == ' ' || ch == '\t'; };
    while (!value.empty() && isSpace(value.front())) value.remove_prefix(1);
    while (!value.empty() && isSpace(value.back())) value.remove_suffix(1);
    return value;
}

std::string_view trimJsonWhitespace(std::string_view value) noexcept
{
    const auto isSpace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; };
    while (!value.empty() && isSpace(value.front())) value.remove_prefix(1);
    while (!value.empty() && isSpace(value.back())) value.remove_suffix(1);
    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// Envelope scan
// ---------------------------------------------------------------------------------------------------------------

// The info string is never stored: only whether it IS the envelope tag matters, and a fence line can be as long as
// the whole message.
struct Fence {
    char marker {'`'};
    std::size_t length {0};
    bool envelopeTag {false};
};

std::optional<Fence> openingFence(std::string_view line)
{
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    if (indent > 3 || indent >= line.size()) return std::nullopt;
    const char marker = line[indent];
    if (marker != '`' && marker != '~') return std::nullopt;
    std::size_t end = indent;
    while (end < line.size() && line[end] == marker) ++end;
    const std::size_t length = end - indent;
    if (length < 3) return std::nullopt;
    const std::string_view info = trimAsciiSpace(line.substr(end));
    // A backtick info string may not contain a backtick: such a line is inline code, not a fence.
    if (marker == '`' && info.find('`') != std::string_view::npos) return std::nullopt;
    return Fence {marker, length, marker == '`' && info == kNextRunPlanSchemaVersion};
}

bool closesFence(std::string_view line, const Fence& open)
{
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    if (indent > 3) return false;
    std::size_t end = indent;
    while (end < line.size() && line[end] == open.marker) ++end;
    if (end - indent < open.length) return false;
    return trimAsciiSpace(line.substr(end)).empty();
}

struct ScanResult {
    std::size_t envelopes {0};
    bool firstTerminated {false};
    std::string firstBody;          // never more than kNextRunPlanMaxBytes + 1 bytes: see appendBounded
    std::size_t firstBodySize {0};  // the real size, however large
};

// Appends at most as much of `piece` as still fits under kNextRunPlanMaxBytes + 1 bytes in total. One byte over the
// limit is enough to know the body is too large; the rest is only counted, never copied, so a single line of
// hundreds of megabytes costs no memory.
void appendBounded(std::string& out, std::string_view piece)
{
    constexpr std::size_t kCapacity = kNextRunPlanMaxBytes + 1;
    if (out.size() >= kCapacity) return;
    out.append(piece.substr(0, kCapacity - out.size()));
}

ScanResult scanEnvelopes(std::string_view content)
{
    ScanResult result;
    bool inFence = false;
    bool capturing = false;
    std::size_t capturedLines = 0;
    Fence open;
    std::size_t pos = 0;
    while (true) {
        const std::size_t newline = content.find('\n', pos);
        const bool last = newline == std::string_view::npos;
        std::string_view line = content.substr(pos, (last ? content.size() : newline) - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (!inFence) {
            if (auto fence = openingFence(line)) {
                inFence = true;
                open = std::move(*fence);
                const bool isEnvelope = open.envelopeTag;
                if (isEnvelope) ++result.envelopes;
                capturing = isEnvelope && result.envelopes == 1;
                // One allocation of exactly the bound: the buffer never grows afterwards, so the memory a scan
                // holds is a constant (and an append that overshot the bound would have to reallocate).
                if (capturing) result.firstBody.reserve(kNextRunPlanMaxBytes + 1);
            }
        } else if (closesFence(line, open)) {
            if (capturing) result.firstTerminated = true;
            inFence = false;
            capturing = false;
        } else if (capturing) {
            if (capturedLines > 0) {
                ++result.firstBodySize;
                appendBounded(result.firstBody, "\n");
            }
            result.firstBodySize += line.size();
            appendBounded(result.firstBody, line);
            ++capturedLines;
        }

        if (last) break;
        pos = newline + 1;
    }
    return result;
}

NextRunPlanEnvelopeState stateOf(const ScanResult& scan)
{
    if (scan.envelopes == 0) return NextRunPlanEnvelopeState::None;
    if (scan.envelopes > 1) return NextRunPlanEnvelopeState::Multiple;
    if (scan.firstBodySize > kNextRunPlanMaxBytes) return NextRunPlanEnvelopeState::TooLarge;
    return NextRunPlanEnvelopeState::One;
}

common::Result<std::string> extractFromScan(ScanResult scan)
{
    switch (stateOf(scan)) {
    case NextRunPlanEnvelopeState::None:
        return fail<std::string>("next_run_plan.envelope_missing",
            "No fenced block tagged next-run-plan.v1 was found");
    case NextRunPlanEnvelopeState::Multiple:
        return fail<std::string>("next_run_plan.envelope_multiple",
            "More than one next-run-plan.v1 block was found; keep exactly one");
    case NextRunPlanEnvelopeState::TooLarge:
        return fail<std::string>("next_run_plan.too_large",
            "The next-run-plan.v1 block is larger than " + std::to_string(kNextRunPlanMaxBytes) + " bytes");
    case NextRunPlanEnvelopeState::One:
        break;
    }
    if (!scan.firstTerminated) {
        return fail<std::string>("next_run_plan.envelope_unterminated",
            "The next-run-plan.v1 block is not closed by a matching fence");
    }
    // The body is usually a few KiB; do not hand the caller the whole reserved bound.
    scan.firstBody.shrink_to_fit();
    return common::Result<std::string>::ok(std::move(scan.firstBody));
}

// ---------------------------------------------------------------------------------------------------------------
// Strict JSON: duplicate keys and depth are refused before the document is built
// ---------------------------------------------------------------------------------------------------------------

// nlohmann::json keeps the LAST of two equal keys without a word, which would let a plan say one thing to a human
// who reads the first and another to the Backend. This SAX pass refuses the document instead.
struct StrictSax {
    using number_integer_t = Json::number_integer_t;
    using number_unsigned_t = Json::number_unsigned_t;
    using number_float_t = Json::number_float_t;
    using string_t = Json::string_t;
    using binary_t = Json::binary_t;

    std::vector<std::set<std::string>> scopes;
    // A flag of its own: the key may be empty (`{"":1,"":2}`), so its text cannot say whether one was seen.
    bool duplicateKeySeen {false};
    std::string duplicateKey;   // the (truncated) text of that key, for the message only
    bool tooDeep {false};
    std::size_t errorPosition {0};

    bool null() { return true; }
    bool boolean(bool) { return true; }
    bool number_integer(number_integer_t) { return true; }
    bool number_unsigned(number_unsigned_t) { return true; }
    bool number_float(number_float_t, const string_t&) { return true; }
    bool string(string_t&) { return true; }
    bool binary(binary_t&) { return true; }
    bool start_object(std::size_t) { return open(); }
    bool end_object() { scopes.pop_back(); return true; }
    bool start_array(std::size_t) { return open(); }
    bool end_array() { scopes.pop_back(); return true; }
    bool key(string_t& name)
    {
        if (scopes.back().insert(name).second) return true;
        duplicateKeySeen = true;
        duplicateKey = name.substr(0, kMaxEchoedKeyBytes);
        return false;
    }
    bool parse_error(std::size_t position, const std::string&, const Json::exception&)
    {
        errorPosition = position;
        return false;
    }

private:
    bool open()
    {
        if (scopes.size() >= kMaxJsonDepth) {
            tooDeep = true;
            return false;
        }
        scopes.emplace_back();
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Text hygiene (the same rules the PC-2 contracts use for titles and texts)
// ---------------------------------------------------------------------------------------------------------------

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

bool isAsciiAlnum(char ch) noexcept
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

bool validKey(std::string_view key) noexcept
{
    if (key.empty() || key.size() > kNextRunPlanMaxKeyLength || !isAsciiAlnum(key.front())) return false;
    return std::all_of(key.begin(), key.end(),
        [](char ch) { return isAsciiAlnum(ch) || ch == '_' || ch == '.' || ch == '-'; });
}

std::string lowerAscii(std::string_view value)
{
    std::string out(value);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Typed access
// ---------------------------------------------------------------------------------------------------------------

std::string indexed(const std::string& path, std::size_t position)
{
    return path + "[" + std::to_string(position) + "]";
}

std::optional<common::Error> rejectUnknownKeys(
    const Json& object, const std::set<std::string>& allowed, const std::string& path)
{
    for (const auto& item : object.items()) {
        if (allowed.count(item.key()) == 0) {
            return makeError("next_run_plan.unknown_field",
                "Unknown field '" + item.key().substr(0, kMaxEchoedKeyBytes) + "' in " + path);
        }
    }
    return std::nullopt;
}

common::Result<const Json*> requireField(const Json& object, const char* name, const std::string& path)
{
    const auto it = object.find(name);
    if (it == object.end()) {
        return fail<const Json*>("next_run_plan.missing_field", "Missing field '" + std::string(name) + "' in " + path);
    }
    return common::Result<const Json*>::ok(&*it);
}

common::Result<std::string> requireString(const Json& object, const char* name, const std::string& path)
{
    const auto field = requireField(object, name, path);
    if (!field.success()) return fail<std::string>(field.error()->code, field.error()->message);
    if (!field.value()->is_string()) {
        return fail<std::string>("next_run_plan.invalid_field", path + "." + name + " must be a string");
    }
    return common::Result<std::string>::ok(field.value()->get<std::string>());
}

// An optional array of strings (absent = empty).
common::Result<std::vector<std::string>> optionalStringArray(
    const Json& object, const char* name, const std::string& path)
{
    using Strings = std::vector<std::string>;
    const auto it = object.find(name);
    if (it == object.end()) return common::Result<Strings>::ok(Strings {});
    if (!it->is_array()) {
        return fail<Strings>("next_run_plan.invalid_field", path + "." + name + " must be an array of strings");
    }
    Strings values;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_string()) {
            return fail<Strings>("next_run_plan.invalid_field",
                indexed(path + "." + name, i) + " must be a string");
        }
        values.push_back((*it)[i].get<std::string>());
    }
    return common::Result<Strings>::ok(std::move(values));
}

common::Result<std::string> validatedTitle(std::string raw, const std::string& path)
{
    const std::string title(trimAsciiSpace(raw));
    if (title.empty() || title.size() > kNextRunPlanMaxTitleBytes || hasAsciiControl(title, false)
            || containsUnicodeLineBreak(title)) {
        return fail<std::string>("next_run_plan.invalid_title",
            path + " needs a non-empty single-line title of at most " + std::to_string(kNextRunPlanMaxTitleBytes)
                + " bytes");
    }
    return common::Result<std::string>::ok(title);
}

common::Result<std::string> validatedTask(const std::string& raw, const std::string& path)
{
    std::string task = normalizeLineEndings(raw);
    if (hasAsciiControl(task, true) || containsUnicodeLineBreak(task) || onlyWhitespace(task)) {
        return fail<std::string>("next_run_plan.invalid_task",
            path + " must be non-empty text without control characters");
    }
    if (task.size() > kNextRunPlanMaxTaskBytes) {
        return fail<std::string>("next_run_plan.task_too_long",
            path + " is longer than " + std::to_string(kNextRunPlanMaxTaskBytes) + " bytes");
    }
    return common::Result<std::string>::ok(std::move(task));
}

// The optional staffing hint. Any violation returns a failure whose message says why; the CALLER turns it into a warning and drops
// the hint (it is advisory and the plan is paid for), so nothing here can fail a plan.
common::Result<NextRunPlanStaffing> parseStaffing(const Json& value, const std::string& path)
{
    using StaffingResult = common::Result<NextRunPlanStaffing>;
    const auto bad = [&path](const std::string& why) {
        return fail<NextRunPlanStaffing>("next_run_plan.invalid_staffing", path + ": " + why);
    };
    if (!value.is_object()) return bad("it must be an object");
    static const std::set<std::string> staffingKeys {"workers", "parallelism", "reason"};
    for (const auto& entry : value.items()) {
        if (staffingKeys.count(entry.key()) == 0) {
            return bad("unknown field '" + entry.key().substr(0, kMaxEchoedKeyBytes) + "'");
        }
    }
    const auto workersIt = value.find("workers");
    const auto parallelismIt = value.find("parallelism");
    const auto reasonIt = value.find("reason");
    if (workersIt == value.end() || parallelismIt == value.end() || reasonIt == value.end()) {
        return bad("workers, parallelism and reason are all required");
    }
    if (!workersIt->is_array() || workersIt->empty() || workersIt->size() > kNextRunPlanMaxSuggestedWorkers) {
        return bad("workers must be an array of 1-" + std::to_string(kNextRunPlanMaxSuggestedWorkers) + " entries");
    }
    NextRunPlanStaffing staffing;
    static const std::set<std::string> workerKeys {"role", "focus"};
    for (std::size_t i = 0; i < workersIt->size(); ++i) {
        const Json& worker = (*workersIt)[i];
        const std::string where = indexed(path + ".workers", i);
        if (!worker.is_object()) return bad(where + " must be an object");
        for (const auto& entry : worker.items()) {
            if (workerKeys.count(entry.key()) == 0) {
                return bad(where + " has the unknown field '" + entry.key().substr(0, kMaxEchoedKeyBytes) + "'");
            }
        }
        const auto role = worker.find("role");
        const auto focus = worker.find("focus");
        if (role == worker.end() || focus == worker.end() || !role->is_string() || !focus->is_string()) {
            return bad(where + " needs the string fields role and focus");
        }
        const std::string roleText = role->get<std::string>();
        if (roleText != "implementation" && roleText != "documentation") {
            return bad(where + ".role must be \"implementation\" or \"documentation\"");
        }
        const std::string focusText(trimAsciiSpace(focus->get<std::string>()));
        if (focusText.empty() || focusText.size() > kNextRunPlanMaxStaffingFocusBytes || hasAsciiControl(focusText, false)
                || containsUnicodeLineBreak(focusText)) {
            return bad(where + ".focus must be one non-empty line of at most "
                + std::to_string(kNextRunPlanMaxStaffingFocusBytes) + " bytes");
        }
        staffing.workers.push_back({roleText, focusText});
    }
    if (!parallelismIt->is_number_integer() || parallelismIt->get<long long>() < 1
            || static_cast<unsigned long long>(parallelismIt->get<long long>()) > staffing.workers.size()) {
        return bad("parallelism must be an integer from 1 to the number of workers");
    }
    staffing.parallelism = static_cast<std::size_t>(parallelismIt->get<long long>());
    if (!reasonIt->is_string()) return bad("reason must be a string");
    const std::string reasonText(trimAsciiSpace(reasonIt->get<std::string>()));
    if (reasonText.empty() || reasonText.size() > kNextRunPlanMaxStaffingReasonBytes || hasAsciiControl(reasonText, false)
            || containsUnicodeLineBreak(reasonText)) {
        return bad("reason must be one non-empty line of at most " + std::to_string(kNextRunPlanMaxStaffingReasonBytes) + " bytes");
    }
    staffing.reason = reasonText;
    return StaffingResult::ok(std::move(staffing));
}

common::Result<RunInterfaceContract> parseContract(const Json& item, const std::string& path)
{
    if (!item.is_object()) {
        return fail<RunInterfaceContract>("next_run_plan.invalid_field", path + " must be an object");
    }
    static const std::set<std::string> allowed {"id", "kind", "title", "text", "appliesToPaths"};
    if (const auto unknown = rejectUnknownKeys(item, allowed, path)) {
        return common::Result<RunInterfaceContract>::failure(*unknown);
    }
    RunInterfaceContract contract;
    for (const char* name : {"id", "kind", "title", "text"}) {
        const auto value = requireString(item, name, path);
        if (!value.success()) return common::Result<RunInterfaceContract>::failure(*value.error());
        const std::string field = name;
        if (field == "id") contract.id = value.value();
        else if (field == "title") contract.title = value.value();
        else if (field == "text") contract.text = value.value();
        else if (!runContractKindFromString(value.value(), contract.kind)) {
            return fail<RunInterfaceContract>("run_contracts.invalid_kind",
                path + ".kind is not a known kind (use interface_contract or constraint)");
        }
    }
    const auto scopes = requireField(item, "appliesToPaths", path);
    if (!scopes.success()) return common::Result<RunInterfaceContract>::failure(*scopes.error());
    const auto paths = optionalStringArray(item, "appliesToPaths", path);
    if (!paths.success()) return common::Result<RunInterfaceContract>::failure(*paths.error());
    contract.appliesToPaths = paths.value();
    return common::Result<RunInterfaceContract>::ok(std::move(contract));
}

common::Result<Contracts> parseContractArray(const Json& object, const char* name, const std::string& path)
{
    const auto it = object.find(name);
    if (it == object.end()) return common::Result<Contracts>::ok(Contracts {});
    if (!it->is_array()) {
        return fail<Contracts>("next_run_plan.invalid_field", path + "." + name + " must be an array");
    }
    Contracts contracts;
    for (std::size_t i = 0; i < it->size(); ++i) {
        auto contract = parseContract((*it)[i], indexed(path + "." + name, i));
        if (!contract.success()) return common::Result<Contracts>::failure(*contract.error());
        contracts.push_back(std::move(contract.value()));
    }
    return common::Result<Contracts>::ok(std::move(contracts));
}

// A contract failure keeps its run_contracts.* code; only the location is added.
common::Error withLocation(const common::Error& error, const std::string& location)
{
    return makeError(error.code, location + ": " + error.message);
}

// ---------------------------------------------------------------------------------------------------------------
// Fingerprints
// ---------------------------------------------------------------------------------------------------------------

Json contractJson(const RunInterfaceContract& contract)
{
    Json item;
    item["id"] = contract.id;
    item["kind"] = toString(contract.kind);
    item["title"] = contract.title;
    item["text"] = contract.text;
    item["scopes"] = contract.appliesToPaths;
    return item;
}

Json contractsJson(Contracts::const_iterator begin, Contracts::const_iterator end)
{
    Json list = Json::array();
    for (auto it = begin; it != end; ++it) list.push_back(contractJson(*it));
    return list;
}

std::string planFingerprint(const NextRunPlan& plan)
{
    Json root;
    root["schema"] = std::string(kNextRunPlanSchemaVersion);
    root["title"] = plan.title;
    root["sharedContracts"] = contractsJson(plan.sharedContracts.begin(), plan.sharedContracts.end());
    Json drafts = Json::array();
    for (const auto& draft : plan.drafts) {
        Json item;
        item["key"] = draft.key;
        item["title"] = draft.title;
        item["task"] = draft.task;
        item["dependsOn"] = draft.dependsOn;
        item["sharedContractIds"] = draft.sharedContractIds;
        // Only the draft's OWN contracts: the shared ones are already covered by the plan and the ids above.
        item["contracts"] = contractsJson(
            draft.contracts.begin() + static_cast<std::ptrdiff_t>(draft.sharedContractCount), draft.contracts.end());
        // Only when present: a plan without staffing keeps the fingerprint it always had.
        if (draft.staffing.has_value()) {
            Json staffing;
            Json workers = Json::array();
            for (const auto& worker : draft.staffing->workers) workers.push_back({{"role", worker.role}, {"focus", worker.focus}});
            staffing["workers"] = std::move(workers);
            staffing["parallelism"] = draft.staffing->parallelism;
            staffing["reason"] = draft.staffing->reason;
            item["staffing"] = std::move(staffing);
        }
        drafts.push_back(std::move(item));
    }
    root["drafts"] = std::move(drafts);
    return sha256Hex(root.dump());
}

// ---------------------------------------------------------------------------------------------------------------
// Cross-draft checks
// ---------------------------------------------------------------------------------------------------------------

std::optional<common::Error> checkDependencies(const std::vector<NextRunPlanDraft>& drafts)
{
    std::map<std::string, std::size_t> indexByKey;
    for (std::size_t i = 0; i < drafts.size(); ++i) indexByKey.emplace(drafts[i].key, i);

    std::vector<std::vector<std::size_t>> edges(drafts.size());
    for (std::size_t i = 0; i < drafts.size(); ++i) {
        std::set<std::string> seen;
        for (std::size_t d = 0; d < drafts[i].dependsOn.size(); ++d) {
            const std::string& dependency = drafts[i].dependsOn[d];
            const std::string where = indexed("plan.drafts", i) + ".dependsOn[" + std::to_string(d) + "]";
            if (dependency == drafts[i].key) {
                return makeError("next_run_plan.self_dependency", where + " names the draft itself");
            }
            const auto found = indexByKey.find(dependency);
            if (found == indexByKey.end()) {
                return makeError("next_run_plan.unknown_dependency",
                    where + " names '" + dependency.substr(0, kMaxEchoedKeyBytes) + "', which is not a draft key");
            }
            if (!seen.insert(dependency).second) {
                return makeError("next_run_plan.duplicate_dependency", where + " repeats a dependency");
            }
            edges[i].push_back(found->second);
        }
    }

    // 0 = unvisited, 1 = on the current path, 2 = done.
    std::vector<int> state(drafts.size(), 0);
    std::optional<common::Error> cycle;
    const auto visit = [&](auto&& self, std::size_t node) -> void {
        if (cycle) return;
        state[node] = 1;
        for (const std::size_t next : edges[node]) {
            if (state[next] == 1) {
                cycle = makeError("next_run_plan.dependency_cycle",
                    "The dependencies of draft '" + drafts[next].key + "' lead back to itself");
                return;
            }
            if (state[next] == 0) self(self, next);
            if (cycle) return;
        }
        state[node] = 2;
    };
    for (std::size_t i = 0; i < drafts.size() && !cycle; ++i) {
        if (state[i] == 0) visit(visit, i);
    }
    return cycle;
}

} // namespace

const char* toString(NextRunPlanEnvelopeState state) noexcept
{
    switch (state) {
    case NextRunPlanEnvelopeState::None: return "none";
    case NextRunPlanEnvelopeState::One: return "one";
    case NextRunPlanEnvelopeState::Multiple: return "multiple";
    case NextRunPlanEnvelopeState::TooLarge: return "too_large";
    }
    return "none";
}

NextRunPlanEnvelopeState scanNextRunPlanEnvelope(std::string_view messageContent)
{
    return stateOf(scanEnvelopes(messageContent));
}

common::Result<std::string> extractNextRunPlanJsonFromMessage(std::string_view messageContent)
{
    return extractFromScan(scanEnvelopes(messageContent));
}

common::Result<std::string> extractNextRunPlanJsonFromPaste(std::string_view pastedText)
{
    auto scan = scanEnvelopes(pastedText);
    if (scan.envelopes > 0) return extractFromScan(std::move(scan));

    const std::string_view bare = trimJsonWhitespace(pastedText);
    if (bare.empty() || bare.front() != '{') {
        return fail<std::string>("next_run_plan.envelope_missing",
            "Paste one block fenced as next-run-plan.v1, or the bare JSON document starting with '{'");
    }
    if (bare.size() > kNextRunPlanMaxBytes) {
        return fail<std::string>("next_run_plan.too_large",
            "The plan is larger than " + std::to_string(kNextRunPlanMaxBytes) + " bytes");
    }
    return common::Result<std::string>::ok(std::string(bare));
}

common::Result<NextRunPlan> parseNextRunPlan(std::string_view json)
{
    using PlanResult = common::Result<NextRunPlan>;
    const auto refuse = [](const common::Error& error) { return PlanResult::failure(error); };

    if (json.size() > kNextRunPlanMaxBytes) {
        return fail<NextRunPlan>("next_run_plan.too_large",
            "The plan is larger than " + std::to_string(kNextRunPlanMaxBytes) + " bytes");
    }
    if (!projectContextIsValidUtf8(json)) {
        return fail<NextRunPlan>("next_run_plan.invalid_utf8", "The plan is not valid UTF-8");
    }

    const std::string text(json);
    StrictSax sax;
    if (!Json::sax_parse(text, &sax)) {
        if (sax.tooDeep) {
            return fail<NextRunPlan>("next_run_plan.too_deep",
                "The plan is nested deeper than " + std::to_string(kMaxJsonDepth) + " levels");
        }
        if (sax.duplicateKeySeen) {
            return fail<NextRunPlan>("next_run_plan.duplicate_json_key",
                "The JSON key '" + sax.duplicateKey + "' appears twice in one object");
        }
        return fail<NextRunPlan>("next_run_plan.invalid_json",
            "The plan is not valid JSON (error near byte " + std::to_string(sax.errorPosition) + ")");
    }
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        return fail<NextRunPlan>("next_run_plan.invalid_json", "The plan is not valid JSON");
    }

    const std::string root = "plan";
    if (!document.is_object()) {
        return fail<NextRunPlan>("next_run_plan.invalid_field", "The plan must be a JSON object");
    }
    static const std::set<std::string> planKeys {"schemaVersion", "title", "sharedContracts", "drafts"};
    if (const auto unknown = rejectUnknownKeys(document, planKeys, root)) return refuse(*unknown);

    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_string() || version->get<std::string>() != kNextRunPlanSchemaVersion) {
        return fail<NextRunPlan>("next_run_plan.schema_version",
            "plan.schemaVersion must be the string \"" + std::string(kNextRunPlanSchemaVersion) + "\"");
    }

    NextRunPlan plan;
    const auto planTitleRaw = requireString(document, "title", root);
    if (!planTitleRaw.success()) return refuse(*planTitleRaw.error());
    auto planTitle = validatedTitle(planTitleRaw.value(), "plan.title");
    if (!planTitle.success()) return refuse(*planTitle.error());
    plan.title = std::move(planTitle.value());

    // The shared pool is validated contract by contract. The PC-2 count and total-size limits belong to what a
    // Run carries, so they are applied to each draft's MERGED list below, not to the pool (which the plan size
    // bounds): a pool larger than any one Run may take is fine as long as no draft lists more than a Run allows.
    auto sharedRaw = parseContractArray(document, "sharedContracts", root);
    if (!sharedRaw.success()) return refuse(*sharedRaw.error());
    std::set<std::string> poolIds;
    for (std::size_t i = 0; i < sharedRaw.value().size(); ++i) {
        const std::string where = indexed("plan.sharedContracts", i);
        auto single = normalizeRunInterfaceContracts({sharedRaw.value()[i]});
        if (!single.success()) return refuse(withLocation(*single.error(), where));
        RunInterfaceContract canonical = std::move(single.value().front());
        if (!poolIds.insert(lowerAscii(canonical.id)).second) {
            return fail<NextRunPlan>("run_contracts.duplicate_id",
                where + ": Contract id '" + canonical.id + "' is used twice (ids are compared ignoring case)");
        }
        plan.sharedContracts.push_back(std::move(canonical));
    }

    const auto draftsField = requireField(document, "drafts", root);
    if (!draftsField.success()) return refuse(*draftsField.error());
    const Json& draftArray = *draftsField.value();
    if (!draftArray.is_array()) {
        return fail<NextRunPlan>("next_run_plan.invalid_field", "plan.drafts must be an array");
    }
    if (draftArray.size() < kNextRunPlanMinDrafts) {
        return fail<NextRunPlan>("next_run_plan.no_drafts", "A plan needs at least one draft");
    }
    if (draftArray.size() > kNextRunPlanMaxDrafts) {
        return fail<NextRunPlan>("next_run_plan.too_many_drafts",
            "A plan accepts at most " + std::to_string(kNextRunPlanMaxDrafts) + " drafts");
    }

    std::map<std::string, std::size_t> sharedById;
    for (std::size_t i = 0; i < plan.sharedContracts.size(); ++i) sharedById.emplace(plan.sharedContracts[i].id, i);
    std::set<std::string> referencedShared;
    std::set<std::string> seenKeys;

    static const std::set<std::string> draftKeys {"key", "title", "task", "dependsOn", "sharedContractIds", "contracts", "staffing"};
    for (std::size_t i = 0; i < draftArray.size(); ++i) {
        const Json& item = draftArray[i];
        const std::string path = indexed("plan.drafts", i);
        if (!item.is_object()) return fail<NextRunPlan>("next_run_plan.invalid_field", path + " must be an object");
        if (const auto unknown = rejectUnknownKeys(item, draftKeys, path)) return refuse(*unknown);

        NextRunPlanDraft draft;
        const auto key = requireString(item, "key", path);
        if (!key.success()) return refuse(*key.error());
        if (!validKey(key.value())) {
            return fail<NextRunPlan>("next_run_plan.invalid_key",
                path + ".key must be 1-" + std::to_string(kNextRunPlanMaxKeyLength)
                    + " ASCII letters, digits, '_', '.' or '-', starting with a letter or digit");
        }
        if (!seenKeys.insert(lowerAscii(key.value())).second) {
            return fail<NextRunPlan>("next_run_plan.duplicate_draft_key",
                "The draft key '" + key.value() + "' is used twice (keys are compared ignoring case)");
        }
        draft.key = key.value();

        const auto title = requireString(item, "title", path);
        if (!title.success()) return refuse(*title.error());
        auto checkedTitle = validatedTitle(title.value(), path + ".title");
        if (!checkedTitle.success()) return refuse(*checkedTitle.error());
        draft.title = std::move(checkedTitle.value());

        const auto task = requireString(item, "task", path);
        if (!task.success()) return refuse(*task.error());
        auto checkedTask = validatedTask(task.value(), path + ".task");
        if (!checkedTask.success()) return refuse(*checkedTask.error());
        draft.task = std::move(checkedTask.value());

        auto dependsOn = optionalStringArray(item, "dependsOn", path);
        if (!dependsOn.success()) return refuse(*dependsOn.error());
        draft.dependsOn = std::move(dependsOn.value());

        auto sharedIds = optionalStringArray(item, "sharedContractIds", path);
        if (!sharedIds.success()) return refuse(*sharedIds.error());
        draft.sharedContractIds = std::move(sharedIds.value());

        auto local = parseContractArray(item, "contracts", path);
        if (!local.success()) return refuse(*local.error());

        // Shared contracts first, in the order this draft lists them, then its own: ONE list, ONE normalization.
        Contracts merged;
        std::set<std::string> listed;
        for (std::size_t s = 0; s < draft.sharedContractIds.size(); ++s) {
            const std::string& id = draft.sharedContractIds[s];
            const std::string where = path + ".sharedContractIds[" + std::to_string(s) + "]";
            const auto found = sharedById.find(id);
            if (found == sharedById.end()) {
                return fail<NextRunPlan>("next_run_plan.unknown_shared_contract",
                    where + " names '" + id.substr(0, kMaxEchoedKeyBytes) + "', which is not a shared contract id");
            }
            if (!listed.insert(id).second) {
                return fail<NextRunPlan>("next_run_plan.duplicate_shared_reference",
                    where + " repeats a shared contract");
            }
            referencedShared.insert(id);
            merged.push_back(plan.sharedContracts[found->second]);
        }
        draft.sharedContractCount = merged.size();
        for (auto& contract : local.value()) merged.push_back(std::move(contract));
        auto finalContracts = normalizeRunInterfaceContracts(std::move(merged));
        if (!finalContracts.success()) {
            return refuse(withLocation(*finalContracts.error(), path + " ('" + draft.key + "') contracts"));
        }
        draft.contracts = std::move(finalContracts.value());
        if (const auto staffingField = item.find("staffing"); staffingField != item.end()) {
            auto staffing = parseStaffing(*staffingField, path + ".staffing");
            if (staffing.success()) {
                draft.staffing = std::move(staffing.value());
            } else {
                plan.warnings.push_back({"next_run_plan.staffing_ignored",
                    "The staffing hint was ignored and the draft kept: " + staffing.error()->message});
            }
        }
        plan.drafts.push_back(std::move(draft));
    }

    if (const auto dependencyError = checkDependencies(plan.drafts)) return refuse(*dependencyError);

    for (const auto& contract : plan.sharedContracts) {
        if (referencedShared.count(contract.id) == 0) {
            plan.warnings.push_back({"next_run_plan.unreferenced_shared_contract",
                "The shared contract '" + contract.id + "' is not listed by any draft, so it reaches no Run"});
        }
    }

    plan.fingerprint = planFingerprint(plan);
    return PlanResult::ok(std::move(plan));
}

std::string nextRunPlanDraftContentFingerprint(std::string_view title, std::string_view task,
    const std::vector<RunInterfaceContract>& contracts, const std::vector<std::string>& dependsOn)
{
    Json root;
    root["schema"] = std::string(kNextRunPlanSchemaVersion) + "/draft-content";
    root["title"] = std::string(title);
    root["task"] = std::string(task);
    root["contracts"] = contractsJson(contracts.begin(), contracts.end());
    root["dependsOn"] = dependsOn;
    return sha256Hex(root.dump());
}

} // namespace ai::application
