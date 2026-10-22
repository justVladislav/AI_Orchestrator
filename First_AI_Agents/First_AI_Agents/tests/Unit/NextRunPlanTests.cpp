#include "ai/application/NextRunPlan.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ai::application;

// Allocation tracking for the memory-bound tests: every allocation of this executable goes through these
// replacements, which record the live bytes and their peak. A message of tens of megabytes must never make the
// scanner or the parser hold more than a small, fixed amount on top of the message itself.
namespace {

std::atomic<std::size_t> g_liveBytes {0};
std::atomic<std::size_t> g_peakBytes {0};
constexpr std::size_t kAllocationHeader = 16;  // keeps the 16-byte alignment of malloc

void* trackedAllocate(std::size_t size)
{
    void* raw = std::malloc(size + kAllocationHeader);
    if (raw == nullptr) throw std::bad_alloc();
    *static_cast<std::size_t*>(raw) = size;
    const std::size_t live = g_liveBytes.fetch_add(size) + size;
    std::size_t peak = g_peakBytes.load();
    while (live > peak && !g_peakBytes.compare_exchange_weak(peak, live)) {
    }
    return static_cast<char*>(raw) + kAllocationHeader;
}

void trackedFree(void* pointer) noexcept
{
    if (pointer == nullptr) return;
    void* raw = static_cast<char*>(pointer) - kAllocationHeader;
    g_liveBytes.fetch_sub(*static_cast<std::size_t*>(raw));
    std::free(raw);
}

} // namespace

void* operator new(std::size_t size) { return trackedAllocate(size); }
void* operator new[](std::size_t size) { return trackedAllocate(size); }
void operator delete(void* pointer) noexcept { trackedFree(pointer); }
void operator delete[](void* pointer) noexcept { trackedFree(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { trackedFree(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { trackedFree(pointer); }

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// The most extra memory `work` held at any moment, beyond what was live when it started.
std::size_t peakExtraBytes(const std::function<void()>& work)
{
    const std::size_t base = g_liveBytes.load();
    g_peakBytes.store(base);
    work();
    return g_peakBytes.load() - base;
}

// A legitimate scan holds the one reserved buffer of the bounded body (128 KiB + 1) and nothing else that grows
// with the input. The limit leaves a little room above that and sits well below TWO such buffers, so an append that
// overshoots the bound (which has to reallocate) is seen as well as a copy of the whole input.
constexpr std::size_t kBoundedPeak = 192 * 1024;

// The reference plan: two shared contracts (S1 for the model and the view, S2 only for the view), a draft without
// any optional array and a draft that depends on it, lists both shared contracts and has a contract of its own.
const std::string kValidPlan = R"json({
  "schemaVersion": "next-run-plan.v1",
  "title": "History page",
  "sharedContracts": [
    {"id": "S1", "kind": "interface_contract", "title": "Loss entry",
     "text": "property string metricId\nproperty real share", "appliesToPaths": ["qml/models"]},
    {"id": "S2", "kind": "constraint", "title": "File size",
     "text": "No file may exceed 300 lines.", "appliesToPaths": []}
  ],
  "drafts": [
    {"key": "model", "title": "Model task", "task": "Create the model.",
     "sharedContractIds": ["S1"]},
    {"key": "view", "title": "View task", "task": "Create the view.",
     "dependsOn": ["model"], "sharedContractIds": ["S1", "S2"],
     "contracts": [{"id": "V1", "kind": "interface_contract", "title": "View signal",
                    "text": "signal activated(string id)", "appliesToPaths": ["qml/pages"]}]}
  ]
})json";

// The same plan with every object's keys in another order, other whitespace and a CRLF-free single line.
const std::string kReorderedPlan =
    R"json({"drafts":[{"contracts":[],"sharedContractIds":["S1"],"task":"Create the model.","title":"Model task","key":"model"},)json"
    R"json({"contracts":[{"appliesToPaths":["qml/pages"],"text":"signal activated(string id)","title":"View signal","kind":"interface_contract","id":"V1"}],)json"
    R"json("sharedContractIds":["S1","S2"],"dependsOn":["model"],"task":"Create the view.","title":"View task","key":"view"}],)json"
    R"json("sharedContracts":[{"appliesToPaths":["qml/models"],"text":"property string metricId\nproperty real share","title":"Loss entry","kind":"interface_contract","id":"S1"},)json"
    R"json({"appliesToPaths":[],"text":"No file may exceed 300 lines.","title":"File size","kind":"constraint","id":"S2"}],)json"
    R"json("title":"History page","schemaVersion":"next-run-plan.v1"})json";

// Pinned: a change here means the stored identity of every previously created group changes.
const std::string kPinnedPlanFingerprint = "93e34bb4b16f966636b612a036fc84c212729b693e51814813fc4b75d04cf786";
const std::string kPinnedDraftFingerprint = "9a3fe2b3a3939b5adc068017312f54f76ebe2be457310eb03de406c9d3f4b41e";

std::string replaceOnce(std::string text, const std::string& from, const std::string& to)
{
    const auto position = text.find(from);
    require(position != std::string::npos, "test bug: pattern not found: " + from);
    require(text.find(from, position + 1) == std::string::npos, "test bug: pattern is not unique: " + from);
    text.replace(position, from.size(), to);
    return text;
}

NextRunPlan parsed(const std::string& json)
{
    auto result = parseNextRunPlan(json);
    require(result.success(), "The reference plan must be valid: " + (result.error() ? result.error()->message : ""));
    return result.value();
}

ai::common::Error rejection(const std::string& json)
{
    const auto result = parseNextRunPlan(json);
    require(!result.success() && result.error().has_value(), "The plan must be rejected");
    return *result.error();
}

std::string codeOf(const std::string& json) { return rejection(json).code; }

void requireCode(const std::string& json, const std::string& expected, const std::string& what)
{
    const auto error = rejection(json);
    require(error.code == expected, what + ": expected " + expected + " but got " + error.code + " (" + error.message + ")");
}

// A small plan generator for the table tests: drafts are {key, dependsOn}.
struct DraftSpec {
    std::string key;
    std::vector<std::string> dependsOn;
};

std::string planOf(const std::vector<DraftSpec>& drafts)
{
    std::string text = R"json({"schemaVersion":"next-run-plan.v1","title":"Plan","drafts":[)json";
    for (std::size_t i = 0; i < drafts.size(); ++i) {
        if (i > 0) text += ",";
        text += R"json({"key":")json" + drafts[i].key + R"json(","title":"Title )json" + std::to_string(i)
            + R"json(","task":"Do the work.","dependsOn":[)json";
        for (std::size_t d = 0; d < drafts[i].dependsOn.size(); ++d) {
            if (d > 0) text += ",";
            text += "\"" + drafts[i].dependsOn[d] + "\"";
        }
        text += "]}";
    }
    return text + "]}";
}

std::string contractJson(const std::string& id, const std::string& text, const std::string& kind = "interface_contract")
{
    return R"json({"id":")json" + id + R"json(","kind":")json" + kind + R"json(","title":"t","text":")json" + text
        + R"json(","appliesToPaths":[]})json";
}

std::string messageWith(const std::string& body)
{
    return "Here is the plan.\n\n```next-run-plan.v1\n" + body + "\n```\n\nTell me what to change.";
}

// ---------------------------------------------------------------------------------------------------------------

void testEnvelopeScan()
{
    const std::string tag = "next-run-plan.v1";
    struct Case {
        const char* what;
        std::string content;
        NextRunPlanEnvelopeState expected;
    };
    const std::string tooLargeBody(kNextRunPlanMaxBytes + 1, 'x');
    const std::string maxBody(kNextRunPlanMaxBytes, 'x');
    const std::vector<Case> cases {
        {"empty text", "", NextRunPlanEnvelopeState::None},
        {"plain prose", "There is no plan here.", NextRunPlanEnvelopeState::None},
        {"one envelope", "```" + tag + "\n{}\n```", NextRunPlanEnvelopeState::One},
        {"one envelope inside prose", messageWith("{}"), NextRunPlanEnvelopeState::One},
        {"CRLF line endings", "```" + tag + "\r\n{}\r\n```\r\n", NextRunPlanEnvelopeState::One},
        {"opening fence indented three spaces", "   ```" + tag + "\n{}\n   ```", NextRunPlanEnvelopeState::One},
        {"opening fence indented four spaces is code", "    ```" + tag + "\n{}\n    ```", NextRunPlanEnvelopeState::None},
        {"spaces around the info string", "``` " + tag + " \n{}\n```", NextRunPlanEnvelopeState::One},
        {"closing fence may be longer", "```" + tag + "\n{}\n`````", NextRunPlanEnvelopeState::One},
        {"a longer envelope fence", "````" + tag + "\n{}\n````", NextRunPlanEnvelopeState::One},
        {"another tag", "```json\n{}\n```", NextRunPlanEnvelopeState::None},
        {"extra words after the tag", "```" + tag + " extra\n{}\n```", NextRunPlanEnvelopeState::None},
        {"tag is case sensitive", "```NEXT-RUN-PLAN.V1\n{}\n```", NextRunPlanEnvelopeState::None},
        {"tilde fences are not envelopes", "~~~" + tag + "\n{}\n~~~", NextRunPlanEnvelopeState::None},
        {"inline code is not a fence", "```" + tag + "```\n{}", NextRunPlanEnvelopeState::None},
        {"two backticks are not a fence", "``" + tag + "\n{}\n``", NextRunPlanEnvelopeState::None},
        {"a tilde block hides a backtick envelope", "~~~text\n```" + tag + "\n{}\n```\n~~~", NextRunPlanEnvelopeState::None},
        // A backtick line inside a tilde block does not close it, so the envelope after it stays hidden...
        {"a tilde block is not closed by backticks", "~~~\n```\n```" + tag + "\n{}\n```\n~~~", NextRunPlanEnvelopeState::None},
        // ...and a tilde line inside a backtick block does not close that one.
        {"a backtick block is not closed by tildes", "```text\n~~~\n```" + tag + "\n{}\n```\n", NextRunPlanEnvelopeState::None},
        // A line with inline code is no fence, so it must not hide the real envelope that follows it.
        {"inline code does not open a block", "Use ```x``` inline.\n```" + tag + "\n{}\n```", NextRunPlanEnvelopeState::One},
        {"inline code at the start of a line does not open a block", "```x``` and more\n```" + tag + "\n{}\n```", NextRunPlanEnvelopeState::One},
        {"a backtick block hides a tilde line", "```text\n~~~\n```\n```" + tag + "\n{}\n```", NextRunPlanEnvelopeState::One},
        {"a mention inside a sentence", "Use ```" + tag + " for plans.", NextRunPlanEnvelopeState::None},
        {"inside a longer outer fence",
            "````markdown\n```" + tag + "\n{}\n```\n````", NextRunPlanEnvelopeState::None},
        {"inside another fenced block", "```text\n```" + tag + "\n{}\n```\n", NextRunPlanEnvelopeState::None},
        {"two envelopes", "```" + tag + "\n{}\n```\n\n```" + tag + "\n{}\n```", NextRunPlanEnvelopeState::Multiple},
        {"one envelope and another block",
            "```json\n{}\n```\n```" + tag + "\n{}\n```", NextRunPlanEnvelopeState::One},
        {"unterminated envelope still counts as one", "```" + tag + "\n{}", NextRunPlanEnvelopeState::One},
        {"body of exactly the limit", "```" + tag + "\n" + maxBody + "\n```", NextRunPlanEnvelopeState::One},
        {"body one byte over the limit", "```" + tag + "\n" + tooLargeBody + "\n```", NextRunPlanEnvelopeState::TooLarge},
        // The newline between two lines is part of the body.
        {"two lines of exactly the limit with their newline",
            "```" + tag + "\n" + std::string(kNextRunPlanMaxBytes / 2, 'x') + "\n" + std::string(kNextRunPlanMaxBytes / 2 - 1, 'x') + "\n```",
            NextRunPlanEnvelopeState::One},
        {"two lines one byte over the limit with their newline",
            "```" + tag + "\n" + std::string(kNextRunPlanMaxBytes / 2, 'x') + "\n" + std::string(kNextRunPlanMaxBytes / 2, 'x') + "\n```",
            NextRunPlanEnvelopeState::TooLarge},
    };
    for (const auto& item : cases) {
        const auto state = scanNextRunPlanEnvelope(item.content);
        require(state == item.expected,
            std::string(item.what) + ": state is " + toString(state) + " but " + toString(item.expected) + " was expected");
    }
    require(std::string(toString(NextRunPlanEnvelopeState::None)) == "none"
            && std::string(toString(NextRunPlanEnvelopeState::One)) == "one"
            && std::string(toString(NextRunPlanEnvelopeState::Multiple)) == "multiple"
            && std::string(toString(NextRunPlanEnvelopeState::TooLarge)) == "too_large",
        "The state names are stable");
}

void testExtractFromAMessage()
{
    const std::string tag = "next-run-plan.v1";
    const auto body = extractNextRunPlanJsonFromMessage(messageWith("{\n  \"a\": 1\n}"));
    require(body.success() && body.value() == "{\n  \"a\": 1\n}", "The body is exactly the lines between the fences");
    const auto crlf = extractNextRunPlanJsonFromMessage("```" + tag + "\r\n{\"a\":\r\n1}\r\n```");
    require(crlf.success() && crlf.value() == "{\"a\":\n1}", "CRLF becomes LF in the extracted body");
    const auto longer = extractNextRunPlanJsonFromMessage("````" + tag + "\n{}\n```\n{}\n````");
    require(longer.success() && longer.value() == "{}\n```\n{}", "A shorter inner fence does not close a longer one");
    const auto empty = extractNextRunPlanJsonFromMessage("```" + tag + "\n```");
    require(empty.success() && empty.value().empty(), "An empty envelope extracts an empty body (the parse then fails)");

    const auto missing = extractNextRunPlanJsonFromMessage("no plan");
    require(!missing.success() && missing.error()->code == "next_run_plan.envelope_missing", "No envelope");
    const auto many = extractNextRunPlanJsonFromMessage(messageWith("{}") + "\n" + messageWith("{}"));
    require(!many.success() && many.error()->code == "next_run_plan.envelope_multiple", "Two envelopes are ambiguous");
    const auto open = extractNextRunPlanJsonFromMessage("```" + tag + "\n{}");
    require(!open.success() && open.error()->code == "next_run_plan.envelope_unterminated", "An unclosed envelope");
    const auto indentedClose = extractNextRunPlanJsonFromMessage("```" + tag + "\n{}\n    ```");
    require(!indentedClose.success() && indentedClose.error()->code == "next_run_plan.envelope_unterminated",
        "A closing fence indented four spaces is code, not a fence");
    const auto closeWithText = extractNextRunPlanJsonFromMessage("```" + tag + "\n{}\n``` trailing");
    require(!closeWithText.success() && closeWithText.error()->code == "next_run_plan.envelope_unterminated",
        "A closing fence carries no text");
    const auto openThenCode = extractNextRunPlanJsonFromMessage("```" + tag + "\n{}\n```json\n{}");
    require(!openThenCode.success() && openThenCode.error()->code == "next_run_plan.envelope_unterminated",
        "A different fence does not close the envelope");
    const auto large = extractNextRunPlanJsonFromMessage(
        "```" + tag + "\n" + std::string(kNextRunPlanMaxBytes + 1, 'x') + "\n```");
    require(!large.success() && large.error()->code == "next_run_plan.too_large", "A body over the limit");
    const auto atLimit = extractNextRunPlanJsonFromMessage(
        "```" + tag + "\n" + std::string(kNextRunPlanMaxBytes, 'x') + "\n```");
    require(atLimit.success() && atLimit.value().size() == kNextRunPlanMaxBytes, "A body of exactly the limit");
}

void testExtractFromAPaste()
{
    const auto bare = extractNextRunPlanJsonFromPaste("  \n\t{\"a\": 1}\n  ");
    require(bare.success() && bare.value() == "{\"a\": 1}", "A bare JSON document is trimmed and accepted");
    const auto fenced = extractNextRunPlanJsonFromPaste(messageWith("{\"a\": 1}"));
    require(fenced.success() && fenced.value() == "{\"a\": 1}", "A fenced envelope is accepted like in a message");
    for (const char* wrong : {"", "   ", "hello", "[1]", "```json\n{}\n```", "```\n{}\n```"}) {
        const auto result = extractNextRunPlanJsonFromPaste(wrong);
        require(!result.success() && result.error()->code == "next_run_plan.envelope_missing",
            std::string("Neither an envelope nor a bare object: ") + wrong);
    }
    const auto many = extractNextRunPlanJsonFromPaste(messageWith("{}") + messageWith("{}"));
    require(!many.success() && many.error()->code == "next_run_plan.envelope_multiple", "Two pasted envelopes");
    const auto open = extractNextRunPlanJsonFromPaste("```next-run-plan.v1\n{}");
    require(!open.success() && open.error()->code == "next_run_plan.envelope_unterminated", "An unclosed pasted envelope");
    const auto large = extractNextRunPlanJsonFromPaste("{" + std::string(kNextRunPlanMaxBytes, ' ') + "}");
    require(!large.success() && large.error()->code == "next_run_plan.too_large", "A bare document over the limit");
    const auto limit = extractNextRunPlanJsonFromPaste("{" + std::string(kNextRunPlanMaxBytes - 2, ' ') + "}");
    require(limit.success(), "A bare document of exactly the limit");
    // A bare document that merely contains an envelope-looking line inside a string is still bare.
    const auto insideString = extractNextRunPlanJsonFromPaste("{\"a\": \"```json\"}");
    require(insideString.success(), "A fence-like text inside a JSON string does not make it an envelope");
}

// The scanner and the size checks must stay bounded however large the message is: an envelope body is kept only up
// to 128 KiB + 1, an info string or a trailing run is never copied, and a document over the limit is refused before
// any copy of it is made. Each case builds its (tens of megabytes) input BEFORE the measured call.
void testScanningAndSizeChecksStayBoundedForHugeInput()
{
    const std::string tag = "next-run-plan.v1";
    constexpr std::size_t huge = 24 * 1024 * 1024;
    struct Case {
        const char* what;
        std::function<std::string()> build;
        NextRunPlanEnvelopeState state;
        const char* extractionCode;   // empty = the extraction succeeds
    };
    const std::vector<Case> cases {
        {"one giant line in a closed envelope",
            [&] { return "```" + tag + "\n" + std::string(huge, 'x') + "\n```"; },
            NextRunPlanEnvelopeState::TooLarge, "next_run_plan.too_large"},
        {"three giant lines",
            [&] { return "```" + tag + "\n" + std::string(huge / 3, 'x') + "\n" + std::string(huge / 3, 'y') + "\n" + std::string(huge / 3, 'z') + "\n```"; },
            NextRunPlanEnvelopeState::TooLarge, "next_run_plan.too_large"},
        // The first line leaves exactly one byte of room: the bound is a TOTAL over all lines, not per line.
        {"a line that nearly fills the bound, then a giant line",
            [&] { return "```" + tag + "\n" + std::string(kNextRunPlanMaxBytes - 1, 'a') + "\n" + std::string(huge, 'b') + "\n```"; },
            NextRunPlanEnvelopeState::TooLarge, "next_run_plan.too_large"},
        {"a body that fills the bound exactly, then more lines",
            [&] { return "```" + tag + "\n" + std::string(kNextRunPlanMaxBytes, 'a') + "\n" + std::string(huge, 'b') + "\n" + std::string(huge, 'c') + "\n```"; },
            NextRunPlanEnvelopeState::TooLarge, "next_run_plan.too_large"},
        {"a giant unterminated envelope",
            [&] { return "```" + tag + "\n" + std::string(huge, 'x'); },
            NextRunPlanEnvelopeState::TooLarge, "next_run_plan.too_large"},
        {"millions of empty lines inside the envelope",
            [&] { return "```" + tag + "\n" + std::string(huge / 4, '\n') + "```"; },
            NextRunPlanEnvelopeState::TooLarge, "next_run_plan.too_large"},
        {"a giant info string on an ordinary fence",
            [&] { return "```" + std::string(huge, 'a') + "\n{}\n```"; },
            NextRunPlanEnvelopeState::None, "next_run_plan.envelope_missing"},
        {"a giant run of spaces after the tag",
            [&] { return "```" + tag + std::string(huge, ' ') + "\n{}\n```"; },
            NextRunPlanEnvelopeState::One, ""},
        {"a giant run of spaces after the closing fence",
            [&] { return "```" + tag + "\n{}\n```" + std::string(huge, ' '); },
            NextRunPlanEnvelopeState::One, ""},
        {"giant prose without any fence",
            [&] { return std::string(huge, 'p'); },
            NextRunPlanEnvelopeState::None, "next_run_plan.envelope_missing"},
    };
    for (const auto& item : cases) {
        const std::string message = item.build();
        NextRunPlanEnvelopeState state = NextRunPlanEnvelopeState::None;
        const std::size_t scanPeak = peakExtraBytes([&] { state = scanNextRunPlanEnvelope(message); });
        require(state == item.state, std::string(item.what) + ": the state is " + toString(state));
        require(scanPeak < kBoundedPeak, std::string(item.what) + ": the scan held " + std::to_string(scanPeak) + " extra bytes");

        std::string code;
        std::string body;
        const std::size_t extractPeak = peakExtraBytes([&] {
            const auto result = extractNextRunPlanJsonFromMessage(message);
            if (result.success()) body = result.value(); else code = result.error()->code;
        });
        require(code == item.extractionCode, std::string(item.what) + ": extraction gave '" + code + "'");
        require(item.extractionCode[0] != '\0' || body == "{}", std::string(item.what) + ": the extracted body is wrong");
        require(extractPeak < kBoundedPeak, std::string(item.what) + ": the extraction held " + std::to_string(extractPeak) + " extra bytes");
    }

    // A manual paste and the parser refuse a document over the limit before copying it.
    const std::string bare = "{" + std::string(huge, ' ') + "}";
    std::string code;
    std::size_t peak = peakExtraBytes([&] {
        const auto result = extractNextRunPlanJsonFromPaste(bare);
        code = result.success() ? "" : result.error()->code;
    });
    require(code == "next_run_plan.too_large" && peak < kBoundedPeak, "A giant bare paste is refused without a copy");
    peak = peakExtraBytes([&] {
        const auto result = parseNextRunPlan(bare);
        code = result.success() ? "" : result.error()->code;
    });
    require(code == "next_run_plan.too_large" && peak < kBoundedPeak, "A giant document is refused without a copy");
    const std::string pasteWithEnvelope = "x\n```" + tag + "\n" + std::string(huge, 'x') + "\n```\n";
    peak = peakExtraBytes([&] {
        const auto result = extractNextRunPlanJsonFromPaste(pasteWithEnvelope);
        code = result.success() ? "" : result.error()->code;
    });
    require(code == "next_run_plan.too_large" && peak < kBoundedPeak, "A giant pasted envelope is refused without a copy");

    // Sanity: the measurement itself works - a plain copy of the message is seen.
    std::size_t copyPeak = peakExtraBytes([&] { const std::string copy = pasteWithEnvelope; (void)copy; });
    require(copyPeak >= pasteWithEnvelope.size(), "The allocation tracking must see a copy of the input");
}

void testAValidPlan()
{
    const auto plan = parsed(kValidPlan);
    require(plan.schemaVersion == "next-run-plan.v1" && plan.title == "History page", "Plan header");
    require(plan.sharedContracts.size() == 2 && plan.sharedContracts[0].id == "S1"
            && plan.sharedContracts[1].kind == RunContractKind::Constraint,
        "The shared contracts are kept in authoring order");
    require(plan.drafts.size() == 2 && plan.drafts[0].key == "model" && plan.drafts[1].key == "view",
        "The drafts keep their authoring order (the ordinal)");
    const auto& model = plan.drafts[0];
    const auto& view = plan.drafts[1];
    require(model.title == "Model task" && model.task == "Create the model." && model.dependsOn.empty(),
        "A draft without optional arrays has empty ones");
    require(model.sharedContractIds == std::vector<std::string> {"S1"} && model.sharedContractCount == 1
            && model.contracts.size() == 1 && model.contracts[0].id == "S1",
        "The model receives exactly the shared contract it lists");
    require(view.dependsOn == std::vector<std::string> {"model"}, "The dependency is kept");
    require(view.sharedContractCount == 2 && view.contracts.size() == 3 && view.contracts[0].id == "S1"
            && view.contracts[1].id == "S2" && view.contracts[2].id == "V1",
        "The view receives the shared contracts in the order it lists them, then its own");
    require(view.contracts[0].text == "property string metricId\nproperty real share"
            && view.contracts[0].appliesToPaths == std::vector<std::string> {"qml/models"},
        "Shared contract bodies reach the draft verbatim");
    require(plan.warnings.empty(), "A fully referenced plan has no warnings");
    require(plan.fingerprint.size() == 64, "The fingerprint is a sha256");
    require(plan.fingerprint == kPinnedPlanFingerprint, "The plan fingerprint changed: " + plan.fingerprint);

    // The merged list is what the Draft will carry: it has already passed the PC-2 normalizer.
    const auto again = normalizeRunInterfaceContracts(view.contracts);
    require(again.success() && serializeRunInterfaceContracts(again.value()) == serializeRunInterfaceContracts(view.contracts),
        "The merged contracts are canonical");
}

void testSharedContractsAreAttachedOnlyWhereListed()
{
    std::string text = replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1"]})json", R"json("sharedContractIds": []})json");
    auto plan = parsed(text);
    require(plan.drafts[0].contracts.empty() && plan.drafts[0].sharedContractCount == 0,
        "A draft that lists no shared contract receives none (they are not copied to every draft)");
    require(plan.drafts[1].contracts.size() == 3, "The other draft is unaffected");
    require(plan.warnings.empty(), "S1 is still listed by the second draft");

    // Order is the draft's own listing order, not the declaration order.
    text = replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json", R"json("sharedContractIds": ["S2", "S1"])json");
    plan = parsed(text);
    require(plan.drafts[1].contracts[0].id == "S2" && plan.drafts[1].contracts[1].id == "S1"
            && plan.drafts[1].contracts[2].id == "V1",
        "The listing order decides the delivery order");
    require(plan.fingerprint != parsed(kValidPlan).fingerprint, "The order is part of the plan identity");
}

void testAnUnreferencedSharedContractWarns()
{
    const std::string text = replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json",
        R"json("sharedContractIds": ["S1"])json");
    const auto plan = parsed(text);
    require(plan.warnings.size() == 1 && plan.warnings[0].code == "next_run_plan.unreferenced_shared_contract"
            && plan.warnings[0].message.find("S2") != std::string::npos,
        "A shared contract no draft lists is a warning that names it, not an error");
    require(plan.drafts[1].contracts.size() == 2, "...and it reaches no draft");
    require(!plan.fingerprint.empty(), "The plan is otherwise valid");
}

void testNormalizationOfTextAndContracts()
{
    auto plan = parsed(replaceOnce(kValidPlan, R"json("task": "Create the view.")json",
        R"json("task": "Line one.\r\nLine two.\rLine three.\twith a tab")json"));
    require(plan.drafts[1].task == "Line one.\nLine two.\nLine three.\twith a tab", "The task keeps tabs and gets LF line endings");

    plan = parsed(replaceOnce(kValidPlan, R"json("title": "View task")json", R"json("title": "   View task \t")json"));
    require(plan.drafts[1].title == "View task", "Titles are trimmed");
    plan = parsed(replaceOnce(kValidPlan, R"json("title": "History page")json", R"json("title": "  History page  ")json"));
    require(plan.title == "History page", "The plan title is trimmed");

    plan = parsed(replaceOnce(kValidPlan, R"json(["qml/pages"])json", R"json(["qml\\pages\\", "./QML/PAGES", "docs"])json"));
    require(plan.drafts[1].contracts[2].appliesToPaths == std::vector<std::string> {"qml/pages", "docs"},
        "Contract scopes go through the PC-2 normalizer");
    plan = parsed(replaceOnce(kValidPlan, R"json("text": "signal activated(string id)")json",
        R"json("text": "line one\r\nline two")json"));
    require(plan.drafts[1].contracts[2].text == "line one\nline two", "Contract texts get LF line endings");

    // The same plan written differently has the same identity.
    require(parsed(kReorderedPlan).fingerprint == parsed(kValidPlan).fingerprint,
        "Key order and whitespace do not change the fingerprint");
    require(parsed(replaceOnce(kValidPlan, R"json("task": "Create the view.")json",
                       R"json("task": "Line one.\r\nLine two.")json")).fingerprint
            == parsed(replaceOnce(kValidPlan, R"json("task": "Create the view.")json",
                       R"json("task": "Line one.\nLine two.")json")).fingerprint,
        "CRLF and LF in a task give the same identity");
    require(parsed(kValidPlan).fingerprint == parsed(kValidPlan).fingerprint, "Parsing is deterministic");
    require(parsed(kValidPlan + std::string(40, ' ') + "\n").fingerprint == parsed(kValidPlan).fingerprint,
        "Trailing whitespace after the document is not content");
}

void testTheFingerprintCoversEveryFieldThatReachesADraft()
{
    const std::string base = parsed(kValidPlan).fingerprint;
    const std::vector<std::pair<std::string, std::string>> edits {
        {R"json("title": "History page")json", R"json("title": "History page 2")json"},
        {R"json("title": "Model task")json", R"json("title": "Model task 2")json"},
        {R"json("task": "Create the model.")json", R"json("task": "Create the model!")json"},
        {R"json("dependsOn": ["model"])json", R"json("dependsOn": [])json"},
        {R"json("sharedContractIds": ["S1"]})json", R"json("sharedContractIds": []})json"},
        {R"json("text": "No file may exceed 300 lines.")json", R"json("text": "No file may exceed 301 lines.")json"},
        {R"json("title": "File size")json", R"json("title": "File sizes")json"},
        {R"json("kind": "constraint")json", R"json("kind": "interface_contract")json"},
        {R"json("appliesToPaths": ["qml/models"])json", R"json("appliesToPaths": ["qml/model"])json"},
        {R"json("id": "V1")json", R"json("id": "V2")json"},
        {R"json("text": "signal activated(string id)")json", R"json("text": "signal activated(int id)")json"},
        {R"json("appliesToPaths": ["qml/pages"])json", R"json("appliesToPaths": [])json"},
        {R"json("title": "View signal")json", R"json("title": "View signals")json"},
    };
    for (const auto& edit : edits) {
        const auto changed = parseNextRunPlan(replaceOnce(kValidPlan, edit.first, edit.second));
        require(changed.success(), "The edited plan stays valid: " + edit.first);
        require(changed.value().fingerprint != base, "The fingerprint must change for: " + edit.first);
    }
    // Renaming a key (and the dependency on it) is a different plan.
    const auto renamedKey = parsed(replaceOnce(replaceOnce(kValidPlan, R"json("key": "model")json", R"json("key": "model2")json"),
        R"json("dependsOn": ["model"])json", R"json("dependsOn": ["model2"])json"));
    require(renamedKey.fingerprint != base, "A draft key is part of the plan identity");
    // Reordering the drafts changes the ordinals and so the identity.
    const auto swapped = parsed(planOf({{"b", {}}, {"a", {}}}));
    require(swapped.fingerprint != parsed(planOf({{"a", {}}, {"b", {}}})).fingerprint, "Draft order is part of the identity");
    // A shared contract that nobody lists is still part of the plan.
    const auto withExtra = parsed(replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json",
        R"json("sharedContractIds": ["S1"])json"));
    require(withExtra.fingerprint != base, "Dropping a reference changes the identity");
}

void testTheDraftContentFingerprint()
{
    const auto plan = parsed(kValidPlan);
    const auto& view = plan.drafts[1];
    const std::string base = nextRunPlanDraftContentFingerprint(view.title, view.task, view.contracts, view.dependsOn);
    require(base.size() == 64 && base == nextRunPlanDraftContentFingerprint(view.title, view.task, view.contracts, view.dependsOn),
        "The content fingerprint is a deterministic sha256");
    require(base == kPinnedDraftFingerprint, "The draft content fingerprint changed: " + base);

    require(nextRunPlanDraftContentFingerprint("View task!", view.task, view.contracts, view.dependsOn) != base, "title");
    require(nextRunPlanDraftContentFingerprint(view.title, view.task + " ", view.contracts, view.dependsOn) != base, "task");
    require(nextRunPlanDraftContentFingerprint(view.title, view.task, view.contracts, {}) != base, "dependencies");
    require(nextRunPlanDraftContentFingerprint(view.title, view.task, view.contracts, {"model", "extra"}) != base, "more dependencies");
    auto fewer = view.contracts;
    fewer.pop_back();
    require(nextRunPlanDraftContentFingerprint(view.title, view.task, fewer, view.dependsOn) != base, "a removed contract");
    auto edited = view.contracts;
    edited[2].text += "x";
    require(nextRunPlanDraftContentFingerprint(view.title, view.task, edited, view.dependsOn) != base, "an edited contract");
    auto rescoped = view.contracts;
    rescoped[2].appliesToPaths = {"docs"};
    require(nextRunPlanDraftContentFingerprint(view.title, view.task, rescoped, view.dependsOn) != base, "an edited scope");
    auto reordered = view.contracts;
    std::swap(reordered[0], reordered[1]);
    require(nextRunPlanDraftContentFingerprint(view.title, view.task, reordered, view.dependsOn) != base, "contract order");
    // The draft key is a handle inside the plan, not content.
    const auto renamedPlan = parsed(replaceOnce(kValidPlan, R"json("key": "view")json", R"json("key": "view2")json"));
    const auto& view2 = renamedPlan.drafts[1];
    require(nextRunPlanDraftContentFingerprint(view2.title, view2.task, view2.contracts, view2.dependsOn) == base,
        "Renaming a key does not change the content");
}

void testSizeAndEncodingAreCheckedBeforeParsing()
{
    requireCode(std::string(kNextRunPlanMaxBytes + 1, 'x'), "next_run_plan.too_large", "over the limit, not even JSON");
    const std::string atLimit = kValidPlan + std::string(kNextRunPlanMaxBytes - kValidPlan.size(), ' ');
    require(atLimit.size() == kNextRunPlanMaxBytes && parseNextRunPlan(atLimit).success(), "Exactly the limit is accepted");
    requireCode(atLimit + " ", "next_run_plan.too_large", "one byte over the limit");

    requireCode(replaceOnce(kValidPlan, "History page", "History \xFF page"), "next_run_plan.invalid_utf8", "bad byte");
    requireCode(replaceOnce(kValidPlan, "History page", "History \xC0\xAF page"), "next_run_plan.invalid_utf8", "overlong");
    requireCode(replaceOnce(kValidPlan, "History page", "History \xED\xA0\x80 page"), "next_run_plan.invalid_utf8", "surrogate");
    require(parseNextRunPlan(replaceOnce(kValidPlan, "History page", "\xD0\x98\xD1\x81\xD1\x82\xD0\xBE\xD1\x80\xD0\xB8\xD1\x8F")).success(),
        "Valid UTF-8 text is accepted");
}

void testMalformedJsonIsRejected()
{
    for (const char* bad : {"", "   ", "{", "}", "not json", "{\"a\":}", "{'a': 1}", "// comment\n{}", "{} {}", "{},",
             "{\"schemaVersion\": \"next-run-plan.v1\",}"}) {
        requireCode(bad, "next_run_plan.invalid_json", std::string("malformed: ") + bad);
    }
    requireCode(kValidPlan + "x", "next_run_plan.invalid_json", "trailing garbage");
    requireCode(replaceOnce(kValidPlan, "\"History page\"", "\"History page\" // note"), "next_run_plan.invalid_json", "comment");
    requireCode(replaceOnce(kValidPlan, "History page", "History \\ud800 page"), "next_run_plan.invalid_json", "lone surrogate escape");
    for (const char* notObject : {"[]", "[1]", "1", "\"x\"", "null", "true"}) {
        requireCode(notObject, "next_run_plan.invalid_field", std::string("not an object: ") + notObject);
    }
}

void testDuplicateAndDeepJsonIsRejected()
{
    requireCode(replaceOnce(kValidPlan, R"json("title": "History page",)json",
                    R"json("title": "History page", "title": "Other",)json"),
        "next_run_plan.duplicate_json_key", "duplicate plan key");
    requireCode(replaceOnce(kValidPlan, R"json({"key": "model",)json", R"json({"key": "model", "key": "x",)json"),
        "next_run_plan.duplicate_json_key", "duplicate draft key");
    requireCode(replaceOnce(kValidPlan, R"json({"id": "V1",)json", R"json({"id": "V1", "id": "V9",)json"),
        "next_run_plan.duplicate_json_key", "duplicate contract key");
    requireCode(replaceOnce(kValidPlan, R"json({"id": "S2",)json", R"json({"id": "S2", "kind": "constraint", "kind": "constraint",)json"),
        "next_run_plan.duplicate_json_key", "duplicate key in a shared contract");
    // An empty key is a key: two of them in one object are a duplicate whatever the key's text is.
    requireCode(R"json({"":1,"":2})json", "next_run_plan.duplicate_json_key", "an empty key twice");
    requireCode(R"json({"a":{"":1,"":2}})json", "next_run_plan.duplicate_json_key", "an empty key twice in a nested object");
    requireCode(replaceOnce(kValidPlan, R"json({"key": "model",)json", R"json({"": 1, "": 2, "key": "model",)json"),
        "next_run_plan.duplicate_json_key", "an empty key twice in a draft");
    requireCode(R"json({"":1})json", "next_run_plan.unknown_field", "one empty key is no duplicate, only an unknown field");
    // The message echoes a bounded piece of the key, however long the key is.
    const std::string longKey(20000, 'k');
    const auto longDuplicate = rejection("{\"" + longKey + "\":1,\"" + longKey + "\":2}");
    require(longDuplicate.code == "next_run_plan.duplicate_json_key" && longDuplicate.message.size() < 200
            && longDuplicate.message.find("kkkk") != std::string::npos,
        "A repeated long key is named in a bounded message: " + std::to_string(longDuplicate.message.size()));
    requireCode(R"json({"":{"":1},"":2})json", "next_run_plan.duplicate_json_key", "an empty key repeated after a nested one");
    // The same key in two different objects is fine.
    require(parseNextRunPlan(kValidPlan).success(), "Equal keys in different objects are legal");
    const auto duplicate = rejection(replaceOnce(kValidPlan, R"json("title": "History page",)json",
        R"json("title": "History page", "title": "Other",)json"));
    require(duplicate.message.find("title") != std::string::npos, "The message names the duplicated key");

    const std::string header = R"json({"schemaVersion":"next-run-plan.v1","title":"t","drafts":[],"x":)json";
    requireCode(header + "[[[[[[[[1]]]]]]]]}", "next_run_plan.too_deep", "nested too deep");
    // One level less passes the depth check and is then refused as an unknown field.
    requireCode(header + "[[[[[[[1]]]]]]]}", "next_run_plan.unknown_field", "deepest allowed nesting");
}

void testSchemaVersionAndFields()
{
    requireCode(replaceOnce(kValidPlan, R"json("schemaVersion": "next-run-plan.v1",)json", ""), "next_run_plan.schema_version", "missing");
    requireCode(replaceOnce(kValidPlan, "next-run-plan.v1", "next-run-plan.v2"), "next_run_plan.schema_version", "other version");
    requireCode(replaceOnce(kValidPlan, "next-run-plan.v1", "Next-Run-Plan.v1"), "next_run_plan.schema_version", "case");
    requireCode(replaceOnce(kValidPlan, R"json("schemaVersion": "next-run-plan.v1")json", R"json("schemaVersion": 1)json"),
        "next_run_plan.schema_version", "a number");
    requireCode(replaceOnce(kValidPlan, R"json("schemaVersion": "next-run-plan.v1")json", R"json("schemaVersion": null)json"),
        "next_run_plan.schema_version", "null");

    // A plan has no field that could choose cost, scope or start anything.
    for (const char* forbidden : {"projectId", "project", "roster", "provider", "providerId", "model", "modelId", "budget",
             "providerBudget", "workspace", "workspacePath", "launch", "start", "autoStart", "launchMode", "workflowId",
             "executionProfileId", "maxParallelism", "name", "status", "extra"}) {
        const std::string field = std::string("\"") + forbidden + "\": \"x\", ";
        requireCode(replaceOnce(kValidPlan, "{\n  \"schemaVersion\"", "{\n  " + field + "\"schemaVersion\""),
            "next_run_plan.unknown_field", std::string("plan field ") + forbidden);
        requireCode(replaceOnce(kValidPlan, R"json({"key": "model",)json", R"json({)json" + field + R"json("key": "model",)json"),
            "next_run_plan.unknown_field", std::string("draft field ") + forbidden);
        requireCode(replaceOnce(kValidPlan, R"json({"id": "V1",)json", R"json({)json" + field + R"json("id": "V1",)json"),
            "next_run_plan.unknown_field", std::string("contract field ") + forbidden);
    }
    const auto unknown = rejection(replaceOnce(kValidPlan, R"json({"key": "model",)json", R"json({"roster": 1, "key": "model",)json"));
    require(unknown.message.find("roster") != std::string::npos && unknown.message.find("plan.drafts[0]") != std::string::npos,
        "The message names the field and where it is");

    const std::vector<std::pair<std::string, std::string>> missing {
        {R"json("title": "History page",)json", ""},
        {R"json("title": "Model task",)json", ""},
        {R"json("task": "Create the model.",)json", ""},
        {R"json({"key": "model", )json", "{"},
        {R"json("kind": "interface_contract", "title": "Loss entry",)json", R"json("title": "Loss entry",)json"},
        {R"json("kind": "constraint", )json", ""},
        {R"json(, "appliesToPaths": [])json", ""},
        {R"json("title": "File size",)json", ""},
        {R"json({"id": "S1", )json", "{"},
        {R"json("text": "No file may exceed 300 lines.", )json", ""},
    };
    for (const auto& edit : missing) {
        requireCode(replaceOnce(kValidPlan, edit.first, edit.second), "next_run_plan.missing_field", "missing: " + edit.first);
    }
    requireCode(replaceOnce(kValidPlan, R"json("drafts": [)json", R"json("draftz": [)json"), "next_run_plan.unknown_field", "renamed drafts");
    const auto noDrafts = R"json({"schemaVersion":"next-run-plan.v1","title":"t"})json";
    requireCode(noDrafts, "next_run_plan.missing_field", "no drafts field");
}

void testWrongTypesAreRejected()
{
    const std::vector<std::pair<std::string, std::string>> edits {
        {R"json("title": "History page")json", R"json("title": 5)json"},
        {R"json("title": "Model task")json", R"json("title": null)json"},
        {R"json("task": "Create the model.")json", R"json("task": ["x"])json"},
        {R"json("key": "model")json", R"json("key": 1)json"},
        {R"json("dependsOn": ["model"])json", R"json("dependsOn": "model")json"},
        {R"json("dependsOn": ["model"])json", R"json("dependsOn": [1])json"},
        {R"json("dependsOn": ["model"])json", R"json("dependsOn": null)json"},
        {R"json("sharedContractIds": ["S1"]})json", R"json("sharedContractIds": "S1"})json"},
        {R"json("sharedContractIds": ["S1"]})json", R"json("sharedContractIds": [true]})json"},
        {R"json("appliesToPaths": ["qml/models"])json", R"json("appliesToPaths": "qml/models")json"},
        {R"json("appliesToPaths": ["qml/models"])json", R"json("appliesToPaths": [1])json"},
        {R"json("appliesToPaths": ["qml/models"])json", R"json("appliesToPaths": null)json"},
        {R"json("id": "V1")json", R"json("id": 1)json"},
        {R"json("kind": "constraint")json", R"json("kind": 2)json"},
        {R"json("text": "signal activated(string id)")json", R"json("text": {})json"},
        {R"json("contracts": [{)json", R"json("contracts": [[], {)json"},
    };
    for (const auto& edit : edits) {
        requireCode(replaceOnce(kValidPlan, edit.first, edit.second), "next_run_plan.invalid_field",
            "A wrongly typed value is rejected: " + edit.first + " -> " + edit.second);
    }
    const std::string draft = R"json({"key":"a","title":"t","task":"x"})json";
    const std::string head = R"json({"schemaVersion":"next-run-plan.v1","title":"t",)json";
    requireCode(head + R"json("sharedContracts":{},"drafts":[)json" + draft + "]}", "next_run_plan.invalid_field", "an object for the pool");
    requireCode(head + R"json("sharedContracts":"S1","drafts":[)json" + draft + "]}", "next_run_plan.invalid_field", "a string for the pool");
    requireCode(head + R"json("sharedContracts":[5],"drafts":[)json" + draft + "]}", "next_run_plan.invalid_field", "a number as a shared contract");
    requireCode(head + R"json("drafts":{})json" + "}", "next_run_plan.invalid_field", "an object for the drafts");
    requireCode(head + R"json("drafts":"a"})json", "next_run_plan.invalid_field", "a string for the drafts");
    requireCode(head + R"json("drafts":[1]})json", "next_run_plan.invalid_field", "a number as a draft");
    requireCode(head + R"json("drafts":[[]]})json", "next_run_plan.invalid_field", "an array as a draft");
    requireCode(head + R"json("drafts":[{"key":"a","title":"t","task":"x","contracts":{}}]})json", "next_run_plan.invalid_field",
        "an object for the contracts");
}

void testTitlesAndTasks()
{
    for (const char* bad : {"", "   ", "\\t", "line\\nbreak", "ctl\\u0001", "nul\\u0000", "sep\\u2028x", "sep\\u2029x", "nel\\u0085x"}) {
        requireCode(replaceOnce(kValidPlan, R"json("title": "View task")json", std::string("\"title\": \"") + bad + "\""),
            "next_run_plan.invalid_title", std::string("draft title ") + bad);
        requireCode(replaceOnce(kValidPlan, R"json("title": "History page")json", std::string("\"title\": \"") + bad + "\""),
            "next_run_plan.invalid_title", std::string("plan title ") + bad);
    }
    const std::string longest(kNextRunPlanMaxTitleBytes, 'a');
    require(parseNextRunPlan(replaceOnce(kValidPlan, R"json("title": "View task")json", "\"title\": \"" + longest + "\"")).success(),
        "A title of exactly the limit");
    requireCode(replaceOnce(kValidPlan, R"json("title": "View task")json", "\"title\": \"" + longest + "a\""),
        "next_run_plan.invalid_title", "one byte over");
    requireCode(replaceOnce(kValidPlan, R"json("title": "History page")json", "\"title\": \"" + longest + "a\""),
        "next_run_plan.invalid_title", "plan title one byte over");
    // The limit is in bytes of UTF-8, not characters.
    std::string twoByte;
    for (std::size_t i = 0; i < kNextRunPlanMaxTitleBytes / 2 + 1; ++i) twoByte += "\xD0\x96";
    requireCode(replaceOnce(kValidPlan, R"json("title": "View task")json", "\"title\": \"" + twoByte + "\""),
        "next_run_plan.invalid_title", "bytes, not characters");

    for (const char* bad : {"", "   ", "\\n\\t \\n", "ctl\\u0001", "nul\\u0000", "del\\u007f", "sep\\u2028x", "sep\\u2029x", "nel\\u0085x"}) {
        requireCode(replaceOnce(kValidPlan, R"json("task": "Create the view.")json", std::string("\"task\": \"") + bad + "\""),
            "next_run_plan.invalid_task", std::string("task ") + bad);
    }
    require(parseNextRunPlan(replaceOnce(kValidPlan, R"json("task": "Create the view.")json", R"json("task": "a\n\n\tb\r\nc")json")).success(),
        "Newlines and tabs are legal in a task");

    const std::string exact(kNextRunPlanMaxTaskBytes, 'x');
    require(parseNextRunPlan(replaceOnce(kValidPlan, R"json("task": "Create the view.")json", "\"task\": \"" + exact + "\"")).success(),
        "A task of exactly 16 KiB");
    requireCode(replaceOnce(kValidPlan, R"json("task": "Create the view.")json", "\"task\": \"" + exact + "x\""),
        "next_run_plan.task_too_long", "one byte over");
    std::string crlf;
    for (int i = 0; i < 5462; ++i) crlf += "x\\r\\n";
    const auto normalizedShorter = parsed(replaceOnce(kValidPlan, R"json("task": "Create the view.")json", "\"task\": \"" + crlf + "\""));
    require(normalizedShorter.drafts[1].task.size() == 5462 * 2, "The limit is measured after the line endings are normalized");
    requireCode(replaceOnce(kValidPlan, R"json("task": "Create the view.")json", "\"task\": \"" + std::string(kNextRunPlanMaxTaskBytes + 1, 'x') + "\""),
        "next_run_plan.task_too_long", "task over the limit");
}

void testDraftKeys()
{
    const auto key = [](const std::string& value) { return planOf({{value, {}}}); };
    for (const char* ok : {"a", "A", "0", "9x", "a.b-c_d", "model-1", "M"}) {
        require(parseNextRunPlan(key(ok)).success(), std::string("A legal key: ") + ok);
    }
    require(parseNextRunPlan(key(std::string(kNextRunPlanMaxKeyLength, 'k'))).success(), "A key of exactly 64 characters");
    for (const char* bad : {"", " ", "_a", "-a", ".a", "a b", "a/b", "a:b", "a\\\\b", "\xC3\xA9", "a\\u0000", "k\\n"}) {
        requireCode(key(bad), "next_run_plan.invalid_key", std::string("key '") + bad + "'");
    }
    requireCode(key(std::string(kNextRunPlanMaxKeyLength + 1, 'k')), "next_run_plan.invalid_key", "65 characters");
    requireCode(planOf({{"a", {}}, {"a", {}}}), "next_run_plan.duplicate_draft_key", "equal keys");
    requireCode(planOf({{"model", {}}, {"MODEL", {}}}), "next_run_plan.duplicate_draft_key", "keys differing only in case");
    require(parseNextRunPlan(planOf({{"a1", {}}, {"a2", {}}})).success(), "Different keys");
}

void testDraftCount()
{
    requireCode(R"json({"schemaVersion":"next-run-plan.v1","title":"t","drafts":[]})json", "next_run_plan.no_drafts", "no drafts");
    std::vector<DraftSpec> drafts;
    for (std::size_t i = 0; i < kNextRunPlanMaxDrafts; ++i) drafts.push_back({"d" + std::to_string(i), {}});
    const auto eight = parseNextRunPlan(planOf(drafts));
    require(eight.success() && eight.value().drafts.size() == 8, "Eight drafts are accepted");
    drafts.push_back({"d8", {}});
    requireCode(planOf(drafts), "next_run_plan.too_many_drafts", "nine drafts");
    require(parseNextRunPlan(planOf({{"only", {}}})).success(), "One draft is accepted");
}

void testDependencies()
{
    requireCode(planOf({{"a", {"nope"}}}), "next_run_plan.unknown_dependency", "an unknown key");
    requireCode(planOf({{"a", {"A"}}, {"A2", {}}}), "next_run_plan.unknown_dependency", "a key in another case");
    requireCode(planOf({{"a", {"a"}}}), "next_run_plan.self_dependency", "a self edge");
    requireCode(planOf({{"a", {}}, {"b", {"a", "a"}}}), "next_run_plan.duplicate_dependency", "a repeated dependency");
    requireCode(planOf({{"a", {"b"}}, {"b", {"a"}}}), "next_run_plan.dependency_cycle", "a two-draft cycle");
    requireCode(planOf({{"a", {"b"}}, {"b", {"c"}}, {"c", {"a"}}}), "next_run_plan.dependency_cycle", "a three-draft cycle");
    requireCode(planOf({{"x", {}}, {"a", {"b"}}, {"b", {"c"}}, {"c", {"d"}}, {"d", {"b"}}}), "next_run_plan.dependency_cycle",
        "a cycle that does not include the first draft");
    const auto cycle = rejection(planOf({{"a", {"b"}}, {"b", {"a"}}}));
    require(cycle.message.find("'a'") != std::string::npos || cycle.message.find("'b'") != std::string::npos,
        "The cycle message names a draft on the cycle");
    const auto unknown = rejection(planOf({{"a", {}}, {"b", {"zzz"}}}));
    require(unknown.message.find("plan.drafts[1].dependsOn[0]") != std::string::npos && unknown.message.find("zzz") != std::string::npos,
        "The message names the location and the key");

    // A diamond, a chain and a forward reference are all legal DAGs.
    require(parseNextRunPlan(planOf({{"a", {}}, {"b", {"a"}}, {"c", {"a"}}, {"d", {"b", "c"}}})).success(), "A diamond");
    require(parseNextRunPlan(planOf({{"a", {"b"}}, {"b", {"c"}}, {"c", {}}})).success(), "Forward references");
    require(parseNextRunPlan(planOf({{"a", {}}, {"b", {}}, {"c", {}}})).success(), "Independent drafts");
    const auto plan = parsed(planOf({{"a", {"c", "b"}}, {"b", {}}, {"c", {}}}));
    require(plan.drafts[0].dependsOn == std::vector<std::string> {"c", "b"}, "The dependency order is kept as written");
}

void testSharedContractReferences()
{
    requireCode(replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json", R"json("sharedContractIds": ["S1", "S9"])json"),
        "next_run_plan.unknown_shared_contract", "an unknown shared id");
    requireCode(replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json", R"json("sharedContractIds": ["s1"])json"),
        "next_run_plan.unknown_shared_contract", "a shared id in another case");
    requireCode(replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json", R"json("sharedContractIds": ["S1", "S1"])json"),
        "next_run_plan.duplicate_shared_reference", "a repeated reference");
    requireCode(replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1"]})json", R"json("sharedContractIds": ["V1"]})json"),
        "next_run_plan.unknown_shared_contract", "a LOCAL contract id is not a shared id");
    const auto unknown = rejection(replaceOnce(kValidPlan, R"json("sharedContractIds": ["S1", "S2"])json", R"json("sharedContractIds": ["S1", "S9"])json"));
    require(unknown.message.find("plan.drafts[1].sharedContractIds[1]") != std::string::npos && unknown.message.find("S9") != std::string::npos,
        "The message names the location and the id");
    // No shared contracts at all is a legal plan.
    require(parseNextRunPlan(planOf({{"a", {}}})).success(), "A plan without shared contracts");
    requireCode(R"json({"schemaVersion":"next-run-plan.v1","title":"t","drafts":[{"key":"a","title":"t","task":"x","sharedContractIds":["S1"]}]})json",
        "next_run_plan.unknown_shared_contract", "a reference with no shared contracts declared");
}

void testContractRulesAreThoseOfPc2()
{
    // Shared contracts are validated on their own, with the plan location in the message.
    auto error = rejection(replaceOnce(kValidPlan, R"json("appliesToPaths": ["qml/models"])json", R"json("appliesToPaths": ["qml/mod,els"])json"));
    require(error.code == "run_contracts.invalid_scope" && error.message.find("sharedContracts") != std::string::npos,
        "A scope with a comma in a shared contract: " + error.code + " " + error.message);
    requireCode(replaceOnce(kValidPlan, R"json("kind": "interface_contract", "title": "Loss entry")json", R"json("kind": "iface", "title": "Loss entry")json"),
        "run_contracts.invalid_kind", "an unknown shared kind");
    requireCode(replaceOnce(kValidPlan, R"json("kind": "constraint")json", R"json("kind": "")json"), "run_contracts.invalid_kind", "an empty kind");
    requireCode(replaceOnce(kValidPlan, R"json({"id": "S2")json", R"json({"id": "s1")json"), "run_contracts.duplicate_id",
        "shared ids equal ignoring case");
    requireCode(replaceOnce(kValidPlan, R"json({"id": "S2")json", R"json({"id": "2bad")json"), "run_contracts.invalid_id", "a bad shared id");

    // Local contracts of a draft.
    requireCode(replaceOnce(kValidPlan, R"json("kind": "interface_contract", "title": "View signal")json", R"json("kind": "bogus", "title": "View signal")json"),
        "run_contracts.invalid_kind", "an unknown local kind");
    requireCode(replaceOnce(kValidPlan, R"json(["qml/pages"])json", R"json(["../x"])json"), "run_contracts.invalid_scope", "a local scope with ..");
    requireCode(replaceOnce(kValidPlan, R"json(["qml/pages"])json", R"json([" lead"])json"), "run_contracts.invalid_scope", "a leading space");
    requireCode(replaceOnce(kValidPlan, R"json("text": "signal activated(string id)")json", R"json("text": "")json"), "run_contracts.invalid_text",
        "an empty local text");
    requireCode(replaceOnce(kValidPlan, R"json("title": "View signal")json", R"json("title": "")json"), "run_contracts.invalid_title",
        "an empty local title");

    // A local id equal to a shared id this draft lists is a duplicate, and the message names the draft.
    error = rejection(replaceOnce(kValidPlan, R"json("id": "V1")json", R"json("id": "S1")json"));
    require(error.code == "run_contracts.duplicate_id" && error.message.find("view") != std::string::npos,
        "A local id equal to a listed shared id: " + error.code + " " + error.message);
    requireCode(replaceOnce(kValidPlan, R"json("id": "V1")json", R"json("id": "s2")json"), "run_contracts.duplicate_id",
        "the comparison ignores case");
    // ...but not when the draft does not list that shared contract.
    const auto unlisted = parsed(replaceOnce(replaceOnce(kValidPlan, R"json("id": "V1")json", R"json("id": "S2")json"),
        R"json("sharedContractIds": ["S1", "S2"])json", R"json("sharedContractIds": ["S1"])json"));
    require(unlisted.drafts[1].contracts.size() == 2 && unlisted.drafts[1].contracts[1].id == "S2"
            && unlisted.drafts[1].contracts[1].text == "signal activated(string id)",
        "A local id may equal a shared id the draft does not list: it is the draft's own contract");
}

void testMergedContractLimitsApplyPerDraft()
{
    const auto sharedPlan = [](std::size_t shared, std::size_t local, const std::string& body) {
        std::string text = R"json({"schemaVersion":"next-run-plan.v1","title":"t","sharedContracts":[)json";
        for (std::size_t i = 0; i < shared; ++i) text += (i ? "," : "") + contractJson("S" + std::to_string(i), body);
        text += R"json(],"drafts":[{"key":"a","title":"t","task":"x","sharedContractIds":[)json";
        for (std::size_t i = 0; i < shared; ++i) text += (i ? ",\"S" : "\"S") + std::to_string(i) + "\"";
        text += R"json(],"contracts":[)json";
        for (std::size_t i = 0; i < local; ++i) text += (i ? "," : "") + contractJson("L" + std::to_string(i), body);
        return text + "]}]}";
    };
    const auto sixteen = parseNextRunPlan(sharedPlan(16, 0, "x"));
    require(sixteen.success() && sixteen.value().drafts[0].contracts.size() == 16, "16 shared contracts are the maximum");
    requireCode(sharedPlan(16, 1, "x"), "run_contracts.too_many", "16 shared plus one local is over the PC-2 maximum");
    requireCode(sharedPlan(17, 0, "x"), "run_contracts.too_many", "17 shared contracts");
    require(parseNextRunPlan(sharedPlan(8, 8, "x")).success(), "8 + 8 merged contracts");

    const std::string big(kRunInterfaceContractMaxTextBytes, 'x');
    require(parseNextRunPlan(sharedPlan(2, 2, big)).success(), "Exactly 16 KiB of merged text");
    auto tooMuch = sharedPlan(2, 3, big);
    const auto error = rejection(tooMuch);
    require(error.code == "run_contracts.total_too_long" && error.message.find("'a'") != std::string::npos,
        "The merged total is checked per draft and names it: " + error.code + " " + error.message);
    requireCode(sharedPlan(1, 0, big + "x"), "run_contracts.text_too_long", "a single contract text over 4 KiB");

    // The pool is not a Run: only the contracts a draft LISTS count against that draft's limits. Five shared
    // contracts of 4 KiB are 20 KiB together (more than one Run may carry), and still fine when every draft takes one.
    const auto spreadPlan = [&](int pool) {
        std::string text = R"json({"schemaVersion":"next-run-plan.v1","title":"t","sharedContracts":[)json";
        for (int i = 0; i < pool; ++i) text += (i ? "," : "") + contractJson("S" + std::to_string(i), big);
        text += R"json(],"drafts":[)json";
        for (int i = 0; i < pool; ++i) {
            text += std::string(i ? "," : "") + R"json({"key":"d)json" + std::to_string(i)
                + R"json(","title":"t","task":"x","sharedContractIds":["S)json" + std::to_string(i) + R"json("]})json";
        }
        return text + "]}";
    };
    const auto spread = parseNextRunPlan(spreadPlan(5));
    require(spread.success() && spread.value().sharedContracts.size() == 5
            && spread.value().drafts[4].contracts.size() == 1 && spread.value().warnings.empty(),
        "A pool larger than one Run may carry is fine when no draft lists more than a Run allows");
    // The same pool listed by ONE draft is rejected there, and the message names that draft.
    std::string all = R"json({"schemaVersion":"next-run-plan.v1","title":"t","sharedContracts":[)json";
    for (int i = 0; i < 5; ++i) all += (i ? "," : "") + contractJson("S" + std::to_string(i), big);
    all += R"json(],"drafts":[{"key":"greedy","title":"t","task":"x","sharedContractIds":["S0","S1","S2","S3","S4"]}]})json";
    const auto greedy = rejection(all);
    require(greedy.code == "run_contracts.total_too_long" && greedy.message.find("'greedy'") != std::string::npos,
        "One draft listing the whole pool is over its own limit: " + greedy.code + " " + greedy.message);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// The optional staffing hint (6a (a)): a model-free suggestion, lenient by design.
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kGoodStaffing =
    R"json({"workers": [{"role": "implementation", "focus": "qml/models/History.qml"},
                        {"role": "documentation", "focus": "the README section"}],
            "parallelism": 2, "reason": "Two independent files."})json";

// The reference plan with a staffing value on the "model" draft.
std::string planWithStaffing(const std::string& staffingJson)
{
    return replaceOnce(kValidPlan, R"json("task": "Create the model.",)json",
        R"json("task": "Create the model.", "staffing": )json" + staffingJson + ",");
}

void testAStaffingHintIsParsedAndPartOfThePlanIdentity()
{
    const auto plan = parsed(planWithStaffing(kGoodStaffing));
    require(plan.warnings.empty(), "A valid staffing hint produces no warning");
    require(plan.drafts[0].staffing.has_value() && !plan.drafts[1].staffing.has_value(),
        "Only the draft that carries a staffing hint has one");
    const auto& staffing = *plan.drafts[0].staffing;
    require(staffing.workers.size() == 2 && staffing.workers[0].role == "implementation"
            && staffing.workers[0].focus == "qml/models/History.qml" && staffing.workers[1].role == "documentation"
            && staffing.parallelism == 2 && staffing.reason == "Two independent files.",
        "The hint is carried as written");
    require(plan.fingerprint != kPinnedPlanFingerprint && plan.fingerprint == parsed(planWithStaffing(kGoodStaffing)).fingerprint,
        "A present hint is part of the plan fingerprint, deterministically");
    require(parsed(planWithStaffing(replaceOnce(std::string(kGoodStaffing), "\"parallelism\": 2", "\"parallelism\": 1")))
                    .fingerprint != plan.fingerprint,
        "Another parallelism is another plan");
    require(parsed(kValidPlan).fingerprint == kPinnedPlanFingerprint,
        "A plan without a hint keeps the fingerprint it always had");
    const auto padded = parsed(planWithStaffing(replaceOnce(std::string(kGoodStaffing),
        "\"Two independent files.\"", "\"  Two independent files.  \"")));
    require(padded.drafts[0].staffing->reason == "Two independent files." && padded.fingerprint == plan.fingerprint,
        "Surrounding blanks of the one-line texts are trimmed before the identity is computed");
}

void testAnInvalidStaffingHintIsDroppedNotFatal()
{
    const std::string baseFingerprint = parsed(kValidPlan).fingerprint;
    const auto good = [](const std::string& from, const std::string& to) { return replaceOnce(std::string(kGoodStaffing), from, to); };
    const std::vector<std::pair<std::string, std::string>> variants {
        {"not an object", "[1]"},
        {"null", "null"},
        {"unknown field (a model)", good("\"parallelism\": 2,", "\"parallelism\": 2, \"model\": \"gpt-6-sol\",")},
        {"missing reason", R"json({"workers": [{"role": "implementation", "focus": "a"}], "parallelism": 1})json"},
        {"no workers", R"json({"workers": [], "parallelism": 1, "reason": "x"})json"},
        {"workers not an array", R"json({"workers": 2, "parallelism": 1, "reason": "x"})json"},
        {"five workers", R"json({"workers": [{"role":"implementation","focus":"a"},{"role":"implementation","focus":"a"},)json"
                         R"json({"role":"implementation","focus":"a"},{"role":"implementation","focus":"a"},)json"
                         R"json({"role":"implementation","focus":"a"}], "parallelism": 1, "reason": "x"})json"},
        {"unknown role", good("\"documentation\"", "\"architect\"")},
        {"unknown worker field", good("\"focus\": \"the README section\"", "\"focus\": \"the README section\", \"model\": \"x\"")},
        {"multi-line focus", good("the README section", "line one\\nline two")},
        {"empty focus", good("the README section", "   ")},
        {"focus too long", good("the README section", std::string(kNextRunPlanMaxStaffingFocusBytes + 1, 'f'))},
        {"parallelism zero", good("\"parallelism\": 2", "\"parallelism\": 0")},
        {"parallelism above the workers", good("\"parallelism\": 2", "\"parallelism\": 3")},
        {"parallelism fractional", good("\"parallelism\": 2", "\"parallelism\": 1.5")},
        {"parallelism a string", good("\"parallelism\": 2", "\"parallelism\": \"2\"")},
        {"empty reason", good("Two independent files.", " ")},
        {"reason too long", good("Two independent files.", std::string(kNextRunPlanMaxStaffingReasonBytes + 1, 'r'))},
        {"multi-line reason", good("Two independent files.", "a\\nb")},
    };
    for (const auto& [what, staffingJson] : variants) {
        const auto result = parseNextRunPlan(planWithStaffing(staffingJson));
        require(result.success(), "A bad staffing hint must not fail the plan: " + what + " ("
            + (result.error() ? result.error()->message : std::string()) + ")");
        const auto& plan = result.value();
        require(!plan.drafts[0].staffing.has_value(), "The bad hint is dropped: " + what);
        std::size_t ignored = 0;
        for (const auto& warning : plan.warnings) {
            if (warning.code == "next_run_plan.staffing_ignored") ++ignored;
        }
        require(ignored == 1, "Exactly one staffing_ignored warning: " + what);
        require(plan.fingerprint == baseFingerprint, "A dropped hint leaves the plan identity untouched: " + what);
    }
}

void testTheStaffingLimitsAreExactlyTheDocumentedOnes()
{
    std::string four = R"json({"workers": [)json";
    for (std::size_t i = 0; i < kNextRunPlanMaxSuggestedWorkers; ++i) {
        if (i > 0) four += ",";
        four += R"json({"role":"implementation","focus":")json" + std::string(kNextRunPlanMaxStaffingFocusBytes, 'f') + "\"}";
    }
    four += R"json(], "parallelism": 4, "reason": ")json" + std::string(kNextRunPlanMaxStaffingReasonBytes, 'r') + "\"}";
    const auto plan = parsed(planWithStaffing(four));
    require(plan.warnings.empty() && plan.drafts[0].staffing.has_value()
            && plan.drafts[0].staffing->workers.size() == kNextRunPlanMaxSuggestedWorkers
            && plan.drafts[0].staffing->parallelism == kNextRunPlanMaxSuggestedWorkers,
        "The maximum worker count, focus length, reason length and a parallelism equal to the workers are all accepted");
}

int main()
{
    try {
        testEnvelopeScan();
        testExtractFromAMessage();
        testExtractFromAPaste();
        testScanningAndSizeChecksStayBoundedForHugeInput();
        testAValidPlan();
        testSharedContractsAreAttachedOnlyWhereListed();
        testAnUnreferencedSharedContractWarns();
        testNormalizationOfTextAndContracts();
        testTheFingerprintCoversEveryFieldThatReachesADraft();
        testTheDraftContentFingerprint();
        testSizeAndEncodingAreCheckedBeforeParsing();
        testMalformedJsonIsRejected();
        testDuplicateAndDeepJsonIsRejected();
        testSchemaVersionAndFields();
        testWrongTypesAreRejected();
        testTitlesAndTasks();
        testDraftKeys();
        testDraftCount();
        testDependencies();
        testSharedContractReferences();
        testContractRulesAreThoseOfPc2();
        testMergedContractLimitsApplyPerDraft();
        testAStaffingHintIsParsedAndPartOfThePlanIdentity();
        testAnInvalidStaffingHintIsDroppedNotFatal();
        testTheStaffingLimitsAreExactlyTheDocumentedOnes();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
