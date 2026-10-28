#include "ai/application/agents/AgentToolTurnLoopRunner.hpp"
#include "ai/application/CostCalculation.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/proposals/ProposedChangeSetParser.hpp"
#include "ai/application/execution/RunWorkspaceScopeService.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {

namespace {

// A worker whose Backend-forced last turn produced an empty change set: the turns ran out, the
// task did not decide there was nothing to do. File-local on purpose (no shared header changes).
constexpr const char* kWorkerTurnBudgetExhaustedCode = "worker.turn_budget_exhausted";

bool noChangesSummaryContradictsOutcome(const std::string& summary)
{
    std::string lower = summary;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    // This is deliberately a narrow fail-closed guard for explicit deferral,
    // not a general natural-language classifier. These phrases say that the
    // requested work is still absent or must be performed in a later pass;
    // accepting them together with changes[] would turn an admitted failure
    // to implement the task into a successful NoChangesNeeded outcome.
    static constexpr std::string_view contradictions[] = {
        "changes were not made",
        "changes have not been made",
        "changes were not applied",
        "no changes were made",
        "requires a separate apply",
        "separate apply pass",
        "need to add these files",
        "required files are missing",
        "изменения в файлы не внесены",
        "изменения не внесены",
        "требуемые для задачи",
        "отсутствуют требуемые",
        "отдельный apply-проход",
        "отдельного apply-прохода",
        "нужно добавить эти",
    };
    return std::any_of(std::begin(contradictions), std::end(contradictions),
        [&lower](std::string_view phrase) { return lower.find(phrase) != std::string::npos; });
}

// proposal-submission.v2: strict-compatible (OpenAI Responses / Anthropic
// Messages strict subset - every object has additionalProperties:false and
// every one of its properties listed in required; no allOf/anyOf/oneOf/
// if/then/else; every schema node, including const/enum ones, has an
// explicit "type"). Deliberately has no expected_original_hash property at
// all: the Backend computes every Modify/Delete/Rename precondition hash
// itself (see ProposedChangeSetParser.cpp's kProposalSubmissionSchemaVersion
// doc comment for why a merely-optional/nullable hash field was not enough).
const char* kProposeChangeSetInputSchema = R"JSON({
  "type": "object",
  "properties": {
    "schemaVersion": { "type": "string", "const": "proposal-submission.v2" },
    "summary": {
      "type": "string",
      "minLength": 1,
      "maxLength": 8192,
      "description": "One paragraph describing your findings/recommendation."
    },
    "changes": {
      "type": "array",
      "maxItems": 200,
      "description": "May be empty only when inspection confirms the requested task is already satisfied; never defer required work to a later apply pass.",
      "items": {
        "type": "object",
        "properties": {
          "change_id": { "type": "string", "minLength": 1, "maxLength": 128, "description": "Short stable id." },
          "operation": { "type": "string", "enum": ["create", "modify", "delete", "rename"] },
          "relative_path": { "type": "string", "minLength": 1, "maxLength": 4096, "description": "Path relative to the project root." },
          "source_path": { "type": ["string", "null"], "minLength": 1, "maxLength": 4096, "description": "Rename only: the original workspace-relative location. The JSON literal null for every other operation - never omitted." },
          "proposed_content": { "type": ["string", "null"], "maxLength": 8388608, "description": "Create/Modify: the complete UTF-8 file text (an empty string is a valid empty marker file). The JSON literal null for Delete/Rename - never omitted. You never supply a hash of any kind here or anywhere else in this object; the Backend computes and verifies it independently." }
        },
        "required": ["change_id", "operation", "relative_path", "source_path", "proposed_content"],
        "additionalProperties": false
      }
    }
  },
  "required": ["schemaVersion", "summary", "changes"],
  "additionalProperties": false
})JSON";

const char* kTaskNoChangesInputSchema = R"JSON({
  "type":"object",
  "properties":{
    "schemaVersion":{"const":"manifest-worker-no-changes.v1"},
    "reason":{"enum":["already_satisfied_in_baseline","satisfied_by_dependency_proposals"]},
    "summary":{"type":"string","minLength":1,"maxLength":4096},
    "criterionEvidence":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","properties":{
      "requirementId":{"type":"string","minLength":1,"maxLength":4096},
      "inspectedPath":{"type":"string","minLength":1,"maxLength":4096},
      "observation":{"type":"string","minLength":1,"maxLength":4096}},
      "required":["requirementId","inspectedPath","observation"],"additionalProperties":false}}
  },
  "required":["schemaVersion","reason","summary","criterionEvidence"],
  "additionalProperties":false
})JSON";

// AG-15: role is deliberately restricted to the two read-only roles - an
// expansion can never grant a new write target (see
// RunWorkspaceScopeExpansion's own doc comment and
// RunWorkspaceScopeService::requestExpansion, which independently rejects
// any other role even if this schema were somehow bypassed).
const char* kRequestScopeExpansionInputSchema = R"JSON({
  "type":"object",
  "properties":{
    "relativePath":{"type":"string","minLength":1,"maxLength":4096},
    "role":{"type":"string","enum":["required_read","reference_only"]},
    "reason":{"type":"string","minLength":1,"maxLength":512}
  },
  "required":["relativePath","role","reason"],
  "additionalProperties":false
})JSON";

// Verification-repair reliability hardening: no role field, unlike
// kRequestScopeExpansionInputSchema - a diagnostic read is always read-only,
// by construction, never a role choice. findingCode is a bounded audit
// annotation identifying which verification finding this read addresses
// (e.g. "verification.qml_warning") - never cross-validated against the
// Run's actual findings (see RunWorkspaceScopeService::requestDiagnosticRead's
// own doc comment for why).
const char* kRequestDiagnosticReadInputSchema = R"JSON({
  "type":"object",
  "properties":{
    "relativePath":{"type":"string","minLength":1,"maxLength":4096},
    "findingCode":{"type":"string","minLength":1,"maxLength":128},
    "reason":{"type":"string","minLength":1,"maxLength":512}
  },
  "required":["relativePath","findingCode","reason"],
  "additionalProperties":false
})JSON";

const char* kTaskVerificationInputSchema = R"JSON({
  "type":"object",
  "properties":{
    "schemaVersion":{"const":"manifest-task-verification.v1"},
    "summary":{"type":"string","minLength":1,"maxLength":4096},
    "criterionEvidence":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","properties":{
      "requirementId":{"type":"string","minLength":1,"maxLength":4096},
      "inspectedPath":{"type":"string","minLength":1,"maxLength":4096},
      "observation":{"type":"string","minLength":1,"maxLength":4096},
      "status":{"enum":["passed","failed","not_verifiable"]}},
      "required":["requirementId","inspectedPath","observation","status"],"additionalProperties":false}}
  },
  "required":["schemaVersion","summary","criterionEvidence"],
  "additionalProperties":false
})JSON";

// The parser this feeds (ProposedChangeSetParser) is deliberately strict:
// the FINAL message must be either raw JSON or exactly one ```json fence
// with only whitespace before/after it - any other character anywhere
// outside the fence is rejected as "proposal.extra_content". This is a
// security/reliability boundary (Stage 1), not something to relax here.
// Models very commonly violate a single soft "and nothing else" instruction
// by prefacing structured output with a sentence of narration ("Based on my
// analysis, ..."), so the rule below is stated up front, restated at the
// point of the fence itself, and given a concrete counter-example.
const char* const kWorkspaceToolsSystemPromptImpl =
    "You are a code analysis and change-proposal agent. Your read-only "
    "workspace TOOLS (list directories, read files, search text, hash "
    "files, stat paths, find files) can never write, modify, create, or "
    "delete anything on disk. Use them only to inspect the project and "
    "understand the task.\n\n"
    "This does NOT mean you are unable to create, modify, or delete files. "
    "You do that by calling the propose_change_set TOOL with the change "
    "described in its \"changes\" argument, not by asking the user to run "
    "a command themselves. A separate, human-approved apply step (outside "
    "this session) is what actually writes the change to disk - your job "
    "is only to produce the correct proposal. Never refuse a file-creation "
    "or file-edit request on the grounds that you \"only have read-only "
    "tools\", and never instruct the user to run mkdir, New-Item, git, or "
    "any other command themselves - that is exactly the wrong output. "
    "There is no explicit \"create folder\" operation: to create a folder, "
    "propose a \"create\" change for a file inside it (e.g. "
    "\"Test/.gitkeep\"); the folder is created automatically when the "
    "change is applied.\n\n"
    "CRITICAL OUTPUT RULE: when your analysis is complete, call the "
    "propose_change_set tool exactly once, as your terminal action. Its "
    "arguments always contain exactly these three fields, none ever "
    "omitted: schemaVersion (the literal proposal-submission.v2), summary "
    "(one paragraph describing your findings/recommendation), and changes "
    "(the array of file changes). Every change object always contains "
    "exactly these five fields, none ever omitted: change_id, operation "
    "(create/modify/delete/rename), relative_path, source_path, and "
    "proposed_content. source_path and proposed_content are always present "
    "as keys, but their value is the JSON literal null whenever the field "
    "does not apply to that operation - never an empty string standing in "
    "for null, never omitted: create requires proposed_content (a real "
    "string; an empty string is a valid empty marker file) and source_path "
    "is null; modify requires proposed_content and source_path is null; "
    "delete requires both proposed_content and source_path to be null; "
    "rename requires source_path (the file's original workspace-relative "
    "location) and proposed_content is null. You never supply, compute, "
    "guess, or reference any file hash anywhere in this submission - there "
    "is no hash field in this contract at all. The Backend independently "
    "derives and verifies every Modify/Delete/Rename precondition hash "
    "itself before your proposal is validated; you do not need to call "
    "workspace_hash_file for this and it has no effect on your submission "
    "if you do. The \"changes\" array may be empty only when inspection "
    "confirms that the requested task is already satisfied and no file "
    "needs to be created, modified, deleted, or renamed. "
    "Never return an empty array while saying that required work is missing, "
    "was not performed, or should happen in a separate apply pass: include that "
    "work in changes now. Never include secrets, credentials, or absolute "
    "filesystem paths in any argument. Do not also restate or describe "
    "the proposal in a text message - the tool call itself IS your "
    "answer; once you call it, stop.\n\n"
    "Fallback only, if you are truly unable to call a tool: reply with a "
    "single ```json code fence and nothing else - no words before or "
    "after it - containing exactly the same {\"schemaVersion\": "
    "\"proposal-submission.v2\", \"summary\": ..., \"changes\": [...]} "
    "object described above.";

// Best-effort recovery for a model that ignores the system prompt's "final
// message is ONLY the fence" rule and adds narration before and/or after
// it. ProposedChangeSetParser (Stage 1) is deliberately strict about that -
// it is a shared security/reliability boundary used by other callers too,
// so it is not weakened here. Instead, when a ```json fence is findable
// inside the raw text, only that fence (open marker through close marker)
// is handed to the parser; a model's wording quirk then no longer wastes
// an otherwise-correct analysis. If no fence is found at all, the original
// text is returned unchanged so the parser still reports its normal, clear
// "no fence found" error rather than this function silently swallowing it.
std::string extractJsonFenceIfPresent(const std::string& text)
{
    constexpr std::string_view kFenceOpen = "```json";
    // The real closing marker always starts its own line (same as the
    // fence shape demonstrated in kWorkspaceToolsSystemPromptImpl above).
    // Matching a bare "```" instead is wrong whenever the model's own
    // prose - e.g. an acknowledgement like "next time I'll answer with a
    // ```json fence" - happens to mention the fence inline: that first,
    // mid-sentence occurrence would end the extraction right there,
    // truncating the JSON mid-string and turning perfectly parseable
    // output into a spurious "not valid JSON" failure.
    constexpr std::string_view kFenceClose = "\n```";
    const auto start = text.find(kFenceOpen);
    if (start == std::string::npos) return text;
    const auto closeStart = text.find(kFenceClose, start + kFenceOpen.size());
    if (closeStart == std::string::npos) return text;
    const auto end = closeStart + kFenceClose.size();
    return text.substr(start, end - start);
}

// A workspace_hash_file result's contentJson is
// {"relativePath": "...", "algorithm": "sha256", "hash": "<hex>", "sizeBytes": N}
// (FilesystemWorkspaceToolExecutor::executeHash) - relativePath is already
// canonical/root-relative as the executor itself resolved it, which is a more
// reliable correlation key than re-deriving one from the call's own raw
// argumentsJson. Best-effort: any unexpected shape (a future tool version, a
// malformed/truncated result) is silently ignored rather than crashing the
// loop - a missing evidence entry simply fails closed later at
// ChangeProposalValidator::checkHashEvidence instead.
void recordObservedHashIfPresent(
    const AgentToolResult& result,
    std::unordered_map<std::string, std::string>& observedFileHashes)
{
    if (!result.success || result.toolName != "workspace_hash_file") return;
    try {
        const auto doc = nlohmann::json::parse(result.contentJson);
        if (!doc.is_object()) return;
        const auto pathIt = doc.find("relativePath");
        const auto hashIt = doc.find("hash");
        if (pathIt == doc.end() || hashIt == doc.end()) return;
        if (!pathIt->is_string() || !hashIt->is_string()) return;
        observedFileHashes[detail::normalizedPathKey(
            detail::pathFromUtf8(pathIt->get<std::string>()))]
            = hashIt->get<std::string>();
    } catch (const std::exception&) {
        // Malformed JSON: no evidence recorded for this call.
    }
}

// Backend-owned hash preflight: the provider-facing propose_change_set
// contract no longer accepts an expected_original_hash from the model at
// all (see kProposeChangeSetInputSchema/ProposedChangeSetParser - a model
// that still sends one is rejected as an unexpected field). Every
// Modify/Delete/Rename precondition hash is instead computed by the Backend
// itself, through the exact same audited executeToolCall() path a model's
// own workspace_hash_file call would use - identical WorkspacePathGuard,
// owned-path policy, and audit-trail enforcement, never a raw filesystem
// read. A real Run showed why: three Create changes all carried
// sha256(""), the model's own placeholder for "no prior content to hash",
// and a bounded repair attempt reproduced the exact same mistake verbatim -
// no prompt wording closes a gap the wire contract itself still allows.
//
// This performs its own local, throwaway parse of rawModelOutput purely to
// discover which paths need hashing before the real, authoritative parse
// (with full trusted metadata) runs moments later inside
// ChangeProposalService::createFromModelOutput(). A parse failure here
// changes nothing observable - that same failure surfaces from the real
// parse with its normal error code. A path whose hash cannot be obtained
// (missing file, denied by policy, I/O error) is simply left absent from
// observedFileHashes; ChangeProposalValidator's existing
// proposal.missing_hash_evidence check then fails that change closed
// exactly as it already does for a genuinely-unobserved path - this
// deliberately never becomes a new error code, and never turns a
// filesystem/policy failure into a model-format repair.
struct TrustedHashPreflightOutcome {
    bool success {true};
    bool cancelled {false};
    std::string errorCode;
    std::string errorMessage;
    std::unordered_map<std::string, std::string> observedFileHashes;
};

TrustedHashPreflightOutcome preflightTrustedHashes(
    AgentToolExecutionService& toolExecution,
    const AgentToolSession& toolSession,
    const RunId& runId,
    const WorkspaceReadPolicy& policy,
    const std::string& rawModelOutput,
    std::stop_token stopToken,
    std::size_t maxTotalToolCalls,
    std::size_t& totalToolCalls,
    std::size_t& executedToolCalls,
    std::size_t& successfulInspectionCalls,
    std::size_t& totalReturnedBytesAccum)
{
    TrustedHashPreflightOutcome outcome;
    TrustedChangeSetMetadata throwaway;
    const auto parsed = ProposedChangeSetParser::parse(rawModelOutput, throwaway);
    if (!parsed.success()) return outcome;

    std::unordered_set<std::string> queuedThisCall;
    for (const auto& change : parsed.value().changes) {
        if (stopToken.stop_requested()) {
            outcome.success = false;
            outcome.cancelled = true;
            outcome.errorMessage = "Analysis cancelled during proposal hash preflight";
            return outcome;
        }
        std::optional<std::filesystem::path> subject;
        switch (change.operation) {
        case FileChangeOperation::Modify:
        case FileChangeOperation::Delete:
            subject = change.relativePath;
            break;
        case FileChangeOperation::Rename:
            subject = change.sourcePath;
            break;
        case FileChangeOperation::Create:
            break;
        }
        if (!subject.has_value()) continue;
        const auto key = detail::normalizedPathKey(*subject);
        if (!queuedThisCall.insert(key).second) {
            continue;
        }

        // Backend-owned calls consume the same defensive budget as provider-
        // requested calls. Check before dispatch so a rejected call never
        // touches the filesystem or creates a misleading audit row.
        if (totalToolCalls >= maxTotalToolCalls) {
            outcome.success = false;
            outcome.errorCode = "run.tool_call_budget_exceeded";
            outcome.errorMessage =
                "Analysis exceeded the maximum number of tool calls during proposal hash preflight";
            return outcome;
        }
        ++totalToolCalls;

        // Inverse of detail::pathFromUtf8: the one std::filesystem::path
        // conversion the standard guarantees is UTF-8 regardless of the host
        // locale/codepage, matching how relative_path/source_path were
        // originally decoded from the model's own JSON text.
        const auto u8 = subject->generic_u8string();
        const std::string argumentPath(
            reinterpret_cast<const char*>(u8.data()), u8.size());

        AgentToolCall call;
        call.callId = generateUuid();
        call.toolName = "workspace_hash_file";
        call.argumentsJson = nlohmann::json{{"path", argumentPath}}.dump();

        const auto execResult = toolExecution.executeToolCall(
            generateUuid(), toolSession.sessionId, runId, call, policy);
        if (!execResult.success()) {
            outcome.success = false;
            outcome.errorCode = execResult.error()->code;
            outcome.errorMessage = execResult.error()->message;
            return outcome;
        }
        ++executedToolCalls;
        totalReturnedBytesAccum += execResult.value().returnedBytes;
        if (execResult.value().success) ++successfulInspectionCalls;
        recordObservedHashIfPresent(
            execResult.value(), outcome.observedFileHashes);
    }
    if (stopToken.stop_requested()) {
        outcome.success = false;
        outcome.cancelled = true;
        outcome.errorMessage = "Analysis cancelled during proposal hash preflight";
    }
    return outcome;
}

struct NoChangesRequiredOutputPreflightOutcome {
    bool success {true};
    std::string errorCode;
    std::string errorMessage;
};

bool requiresWorkerInspectionEvidence(
    const std::string& requirementId,
    const std::unordered_map<std::string, ExecutionVerificationKind>& verificationKinds)
{
    const auto kind = verificationKinds.find(requirementId);
    // Legacy/direct callers predate verificationKindsByRequirement and used
    // read-backed requirements exclusively. Preserve that contract while
    // making the manifest path explicit: command-backed profiles belong to
    // ManifestProposalSetVerifier, never to a worker no-op assertion.
    return kind == verificationKinds.end()
        || kind->second == ExecutionVerificationKind::InspectPaths;
}

NoChangesRequiredOutputPreflightOutcome preflightNoChangesRequiredOutputs(
    AgentToolExecutionService& toolExecution,
    const AgentToolSession& toolSession,
    const AgentToolTurnLoopRequest& request,
    std::stop_token stopToken,
    std::size_t& totalToolCalls,
    std::size_t& executedToolCalls,
    std::size_t& successfulInspectionCalls,
    std::size_t& totalReturnedBytesAccum)
{
    NoChangesRequiredOutputPreflightOutcome outcome;
    std::unordered_set<std::string> checkedPaths;
    for (const auto& requirementId : request.requiredRequirementIds) {
        if (!requiresWorkerInspectionEvidence(
                requirementId, request.verificationKindsByRequirement)) {
            continue;
        }
        const auto outputs = request.requiredOutputsByRequirement.find(requirementId);
        if (outputs == request.requiredOutputsByRequirement.end()) continue;
        for (const auto& output : outputs->second) {
            if (stopToken.stop_requested()) {
                outcome.success = false;
                outcome.errorCode = "manifest.task_cancelled";
                outcome.errorMessage =
                    "Analysis cancelled during worker no-change output preflight";
                return outcome;
            }

            auto normalized = output;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            while (normalized.starts_with("./")) normalized.erase(0, 2);
            if (!checkedPaths.insert(normalized).second) continue;

            if (totalToolCalls >= request.sessionRequest.maxTotalToolCalls) {
                outcome.success = false;
                outcome.errorCode = "run.tool_call_budget_exceeded";
                outcome.errorMessage =
                    "Analysis exceeded the maximum number of tool calls during "
                    "worker no-change output preflight";
                return outcome;
            }
            ++totalToolCalls;

            AgentToolCall call;
            call.callId = generateUuid();
            call.toolName = "workspace_stat_path";
            call.argumentsJson = nlohmann::json{{"path", output}}.dump();
            const auto execResult = toolExecution.executeToolCall(
                generateUuid(), toolSession.sessionId, request.runId, call, request.policy);
            if (!execResult.success()) {
                outcome.success = false;
                outcome.errorCode = execResult.error()->code;
                outcome.errorMessage = execResult.error()->message;
                return outcome;
            }
            ++executedToolCalls;
            totalReturnedBytesAccum += execResult.value().returnedBytes;
            if (!execResult.value().success) {
                outcome.success = false;
                outcome.errorCode = "worker_no_changes.output_preflight_failed";
                outcome.errorMessage =
                    "Backend could not inspect a required no-change output";
                return outcome;
            }
            ++successfulInspectionCalls;

            try {
                const auto result = nlohmann::json::parse(execResult.value().contentJson);
                const auto exists = result.find("exists");
                if (!result.is_object() || exists == result.end()
                    || !exists->is_boolean()) {
                    outcome.success = false;
                    outcome.errorCode = "worker_no_changes.output_preflight_failed";
                    outcome.errorMessage =
                        "Backend received an invalid required-output stat result";
                    return outcome;
                }
                if (!exists->get<bool>()) {
                    outcome.success = false;
                    outcome.errorCode = "worker_no_changes.required_output_missing";
                    outcome.errorMessage =
                        "A required inspect_paths output is missing; missing owned output "
                        "is concrete work and cannot be a no-change outcome";
                    return outcome;
                }
            } catch (const std::exception&) {
                outcome.success = false;
                outcome.errorCode = "worker_no_changes.output_preflight_failed";
                outcome.errorMessage =
                    "Backend could not parse a required-output stat result";
                return outcome;
            }
        }
    }
    return outcome;
}

// AG-15: the one normalization key format shared between actually-inspected
// evidence (recordInspectedPathIfPresent below) and this task's
// requiredReadPathKeys / approved-expansion paths (see
// preflightRequiredReads / handleScopeExpansionCall) - a path compared under
// any other transform would never match, silently defeating the gate.
std::string normalizeInspectedPathKey(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value.empty()) value = ".";
    return value;
}

void recordInspectedPathIfPresent(
    const AgentToolResult& result,
    std::unordered_set<std::string>& inspectedPaths,
    std::unordered_set<std::string>& knownMissingPaths)
{
    if (!result.success) return;
    try {
        const auto doc = nlohmann::json::parse(result.contentJson);
        if (!doc.is_object()) return;
        for (const auto* field : {"relativePath", "searchPath"}) {
            const auto found = doc.find(field);
            if (found != doc.end() && found->is_string()) {
                const std::string value = normalizeInspectedPathKey(found->get<std::string>());
                inspectedPaths.emplace(value);
                const auto exists = doc.find("exists");
                if (exists != doc.end() && exists->is_boolean()
                    && !exists->get<bool>()) knownMissingPaths.emplace(value);
                return;
            }
        }
    } catch (const std::exception&) {
        // A successful tool with an unexpected future result shape simply
        // cannot back path-correlated evidence.
    }
}

// AG-15: local, non-terminal handling of a request_scope_expansion call -
// never routed through AgentToolExecutionService::executeToolCall since it
// is not a workspace/filesystem operation. Returns the tool result to feed
// back to the model plus, when a RequiredRead role was actually approved,
// the normalized key to add to this attempt's effective required-read set
// (see runTurnLoop's own per-attempt reset of attemptExpandedRequiredReadKeys).
struct ScopeExpansionCallOutcome {
    AgentToolResult result;
    std::optional<std::filesystem::path> approvedReadPath;
    std::optional<std::string> approvedRequiredReadKey;
};

ScopeExpansionCallOutcome handleScopeExpansionCall(
    const AgentToolCall& call,
    const AgentToolTurnLoopRequest& request,
    std::size_t attemptExpansionCallCount)
{
    ScopeExpansionCallOutcome outcome;
    outcome.result.callId = call.callId;
    outcome.result.toolName = call.toolName;

    if (request.runWorkspaceScopeService == nullptr) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.expansion_unavailable";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.expansion_unavailable"})";
        return outcome;
    }
    if (attemptExpansionCallCount >= request.maxScopeExpansions) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.expansion_limit_exceeded";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.expansion_limit_exceeded"})";
        return outcome;
    }

    std::string relativePath;
    std::string roleText;
    std::string reason;
    try {
        const auto doc = nlohmann::json::parse(call.argumentsJson);
        if (!doc.is_object()) throw std::runtime_error("not an object");
        relativePath = doc.at("relativePath").get<std::string>();
        roleText = doc.at("role").get<std::string>();
        reason = doc.at("reason").get<std::string>();
    } catch (const std::exception&) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.expansion_invalid_arguments";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.expansion_invalid_arguments"})";
        return outcome;
    }
    const auto role = workspaceScopeRoleFromString(roleText);
    if (!role.has_value()) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.expansion_role_not_allowed";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.expansion_role_not_allowed"})";
        return outcome;
    }

    WorkspaceReadPolicy expansionPolicy = request.scopeExpansionPolicy.workspaceRoot.empty()
        ? request.policy : request.scopeExpansionPolicy;
    // Backward-compatible direct runner callers only populate `policy`.
    // Their task-exact allowed roots are not a project-level deny boundary.
    if (request.scopeExpansionPolicy.workspaceRoot.empty()) {
        expansionPolicy.allowedRoots.clear();
        expansionPolicy.allowedRootsAreExactPaths = false;
    }
    auto expansion = request.runWorkspaceScopeService->requestExpansion(
        request.runId, request.taskId, relativePath, *role, reason,
        expansionPolicy, request.scopeBaseline, request.maxScopeExpansions);
    if (!expansion.success()) {
        outcome.result.success = false;
        outcome.result.errorCode = expansion.error()->code;
        nlohmann::json errorBody;
        errorBody["error"] = expansion.error()->code;
        outcome.result.contentJson = errorBody.dump();
        return outcome;
    }

    outcome.result.success = true;
    nlohmann::json body;
    body["relativePath"] = expansion.value().relativePath;
    body["role"] = roleText;
    body["granted"] = true;
    outcome.result.contentJson = body.dump();
    outcome.approvedReadPath = std::filesystem::path(expansion.value().relativePath);
    if (*role == WorkspaceScopeRole::RequiredRead) {
        outcome.approvedRequiredReadKey =
            normalizeInspectedPathKey(expansion.value().relativePath);
    }
    return outcome;
}

// Verification-repair reliability hardening: local, non-terminal handling of
// a request_diagnostic_read call, mirroring handleScopeExpansionCall's own
// shape but backed by RunWorkspaceScopeService::requestDiagnosticRead - see
// kRequestDiagnosticReadToolName's own doc comment for why this is a
// separate mechanism rather than an extra role on request_scope_expansion.
struct DiagnosticReadCallOutcome {
    AgentToolResult result;
    std::optional<std::filesystem::path> approvedReadPath;
    std::optional<std::string> approvedDiagnosticReadKey;
};

DiagnosticReadCallOutcome handleDiagnosticReadCall(
    const AgentToolCall& call,
    const AgentToolTurnLoopRequest& request,
    std::size_t attemptDiagnosticReadCallCount)
{
    DiagnosticReadCallOutcome outcome;
    outcome.result.callId = call.callId;
    outcome.result.toolName = call.toolName;

    // Independently re-checked regardless of whether this tool was actually
    // advertised (see run()'s own conditional push_back below) - a session
    // dispatched without diagnosticReadEligible must never have this call
    // honoured even if the model somehow still names the tool.
    if (request.runWorkspaceScopeService == nullptr || !request.diagnosticReadEligible) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.diagnostic_read_unavailable";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.diagnostic_read_unavailable"})";
        return outcome;
    }
    if (attemptDiagnosticReadCallCount >= request.maxDiagnosticReads) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.diagnostic_read_limit_exceeded";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.diagnostic_read_limit_exceeded"})";
        return outcome;
    }
    if (!request.scopeBaseline.has_value()) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.diagnostic_read_unavailable";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.diagnostic_read_unavailable"})";
        return outcome;
    }

    std::string relativePath;
    std::string findingCode;
    std::string reason;
    try {
        const auto doc = nlohmann::json::parse(call.argumentsJson);
        if (!doc.is_object()) throw std::runtime_error("not an object");
        relativePath = doc.at("relativePath").get<std::string>();
        findingCode = doc.at("findingCode").get<std::string>();
        reason = doc.at("reason").get<std::string>();
    } catch (const std::exception&) {
        outcome.result.success = false;
        outcome.result.errorCode = "workspace_scope.diagnostic_read_invalid_finding";
        outcome.result.contentJson =
            R"({"error":"workspace_scope.diagnostic_read_invalid_finding"})";
        return outcome;
    }

    WorkspaceReadPolicy expansionPolicy = request.scopeExpansionPolicy.workspaceRoot.empty()
        ? request.policy : request.scopeExpansionPolicy;
    if (request.scopeExpansionPolicy.workspaceRoot.empty()) {
        expansionPolicy.allowedRoots.clear();
        expansionPolicy.allowedRootsAreExactPaths = false;
    }
    auto grant = request.runWorkspaceScopeService->requestDiagnosticRead(
        request.runId, request.taskId, relativePath, findingCode, reason,
        expansionPolicy, *request.scopeBaseline, request.maxDiagnosticReads);
    if (!grant.success()) {
        outcome.result.success = false;
        outcome.result.errorCode = grant.error()->code;
        nlohmann::json errorBody;
        errorBody["error"] = grant.error()->code;
        outcome.result.contentJson = errorBody.dump();
        return outcome;
    }

    outcome.result.success = true;
    nlohmann::json body;
    body["relativePath"] = grant.value().relativePath;
    body["granted"] = true;
    outcome.result.contentJson = body.dump();
    outcome.approvedReadPath = std::filesystem::path(grant.value().relativePath);
    outcome.approvedDiagnosticReadKey = normalizeInspectedPathKey(grant.value().relativePath);
    return outcome;
}

// AG-15: this attempt's effective required-read paths still not proven
// read - the static frozen-scope list plus any RequiredRead expansion
// approved earlier in this SAME attempt, minus whatever was actually
// inspected successfully in this same attempt. Deterministic order
// (request.requiredReadPathKeys' own order, then any expanded keys not
// already in that list) so a repeated call with unchanged evidence always
// reports identically.
std::vector<std::string> missingRequiredReadKeys(
    const AgentToolTurnLoopRequest& request,
    const std::unordered_set<std::string>& attemptInspectedPaths,
    const std::unordered_set<std::string>& attemptExpandedRequiredReadKeys)
{
    std::vector<std::string> missing;
    std::unordered_set<std::string> seen;
    const auto consider = [&](const std::string& key) {
        if (!seen.insert(key).second) return;
        if (attemptInspectedPaths.find(key) == attemptInspectedPaths.end()) {
            missing.push_back(key);
        }
    };
    for (const auto& key : request.requiredReadPathKeys) consider(key);
    for (const auto& key : attemptExpandedRequiredReadKeys) consider(key);
    return missing;
}

// A read outside the task's exact allow-list used to come back as the bare code, which told a
// model that kept probing (find_files, list_directory, a file that is not listed) neither what IS
// allowed nor that it had already read everything the task requires: in Real Runs ee528eed and
// 8b23fbba Claude spent five to eight turns on such probes and then missed its forced final turn.
// The Backend now answers with a small, bounded, workspace-relative object built only from its own
// trusted state (the attempt's read policy and evidence) - never from the denied path the model
// sent, so nothing the model typed is echoed back.
inline constexpr std::string_view kScopeDeniedErrorCode = "workspace_path.outside_allowed_roots";
inline constexpr std::size_t kScopeGuidanceMaxAllowedPaths = 24;
inline constexpr std::size_t kScopeGuidanceMaxUnreadPaths = 12;

std::string renderScopeDeniedGuidance(
    const AgentToolTurnLoopRequest& request,
    const WorkspaceReadPolicy& attemptReadPolicy,
    const std::unordered_set<std::string>& attemptInspectedPaths,
    const std::unordered_set<std::string>& attemptExpandedRequiredReadKeys)
{
    nlohmann::json guidance;
    guidance["error"] = std::string(kScopeDeniedErrorCode);
    std::string message =
        "Only the exact files listed in allowedReadPaths can be read in this task; searching, "
        "listing directories and reading any other file are not available.";
    if (request.runWorkspaceScopeService != nullptr) {
        message += " Use request_scope_expansion only for a file you genuinely need.";
    }
    std::unordered_map<std::string, std::string> originalByKey;
    auto allowed = nlohmann::json::array();
    for (const auto& root : attemptReadPolicy.allowedRoots) {
        const std::string path = root.generic_string();
        originalByKey.emplace(normalizeInspectedPathKey(path), path);
        if (allowed.size() < kScopeGuidanceMaxAllowedPaths) allowed.push_back(path);
    }
    guidance["allowedReadPaths"] = std::move(allowed);
    if (request.mode == AgentToolTurnLoopMode::Propose) {
        const auto missing = missingRequiredReadKeys(
            request, attemptInspectedPaths, attemptExpandedRequiredReadKeys);
        auto unread = nlohmann::json::array();
        for (const auto& key : missing) {
            if (unread.size() >= kScopeGuidanceMaxUnreadPaths) break;
            const auto original = originalByKey.find(key);
            unread.push_back(original != originalByKey.end() ? original->second : key);
        }
        guidance["requiredReadsComplete"] = missing.empty();
        guidance["unreadRequiredPaths"] = std::move(unread);
        if (missing.empty()) {
            guidance["nextAction"] = std::string(kProposeChangeSetToolName);
            message += " Every required read of this task is already done: call "
                       "propose_change_set now.";
        } else {
            guidance["nextAction"] = "read_unreadRequiredPaths";
            message += " Read the paths in unreadRequiredPaths, then call propose_change_set.";
        }
    }
    guidance["message"] = std::move(message);
    return guidance.dump();
}

// Verification-repair reliability hardening: the same-attempt inspection
// gate for request_diagnostic_read grants approved in THIS attempt -
// deliberately generic (no attempt to infer which specific identifier a
// diagnostic-read path is "for"; see request_diagnostic_read's own doc
// comment for why). Every diagnostic-read path granted this attempt must
// have been successfully read in this SAME attempt before a proposal is
// accepted - a grant that was requested but never read, or was read only in
// a different (e.g. primary) attempt, does not count. No static component
// (unlike missingRequiredReadKeys's requiredReadPathKeys): a task with no
// diagnostic-read grants this attempt trivially returns empty.
std::vector<std::string> missingDiagnosticReadKeys(
    const std::unordered_set<std::string>& attemptInspectedPaths,
    const std::unordered_set<std::string>& attemptExpandedDiagnosticReadKeys)
{
    std::vector<std::string> missing;
    for (const auto& key : attemptExpandedDiagnosticReadKeys) {
        if (attemptInspectedPaths.find(key) == attemptInspectedPaths.end()) {
            missing.push_back(key);
        }
    }
    return missing;
}

common::Result<ManifestTaskVerificationSubmission> parseTaskVerification(
    std::string_view rawJson,
    const std::vector<std::string>& requiredRequirementIds,
    const std::unordered_map<std::string, std::vector<std::string>>& requiredOutputs,
    const std::unordered_map<std::string, ExecutionVerificationKind>& verificationKinds,
    const std::unordered_set<std::string>& inspectedPaths,
    const std::unordered_set<std::string>& knownMissingPaths)
{
    auto fail = [](std::string code, std::string message) {
        return common::Result<ManifestTaskVerificationSubmission>::failure(
            {std::move(code), std::move(message)});
    };
    if (rawJson.size() > 256 * 1024) {
        return fail("task_verification.output_too_large",
            "Task verification submission exceeds the payload limit");
    }
    nlohmann::json object;
    try {
        object = nlohmann::json::parse(rawJson);
    } catch (const std::exception&) {
        return fail("task_verification.parse_error",
            "Task verification submission is not valid JSON");
    }
    const std::unordered_set<std::string> fields = {
        "schemaVersion", "summary", "criterionEvidence"};
    if (!object.is_object() || object.size() != fields.size()) {
        return fail("task_verification.invalid_shape",
            "Task verification submission does not match the schema");
    }
    for (const auto& [key, unused] : object.items()) {
        static_cast<void>(unused);
        if (!fields.contains(key)) return fail("task_verification.unexpected_field",
            "Task verification submission contains an unexpected field");
    }
    if (!object.at("schemaVersion").is_string()
        || object.at("schemaVersion") != "manifest-task-verification.v1"
        || !object.at("summary").is_string()
        || object.at("summary").get_ref<const std::string&>().empty()
        || object.at("summary").get_ref<const std::string&>().size() > 4096
        || !object.at("criterionEvidence").is_array()
        || object.at("criterionEvidence").empty()
        || object.at("criterionEvidence").size() > 64) {
        return fail("task_verification.invalid_field",
            "Task verification fields are missing, unsupported, or outside limits");
    }
    if (inspectedPaths.empty()) {
        return fail("task_verification.missing_inspection",
            "Task verification requires successful same-attempt inspection");
    }

    ManifestTaskVerificationSubmission submission;
    submission.summary = object.at("summary").get<std::string>();
    submission.inspectedRelativePaths.assign(inspectedPaths.begin(), inspectedPaths.end());
    std::sort(submission.inspectedRelativePaths.begin(),
        submission.inspectedRelativePaths.end());
    std::unordered_set<std::string> mapped;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        evidencedPathsByRequirement;
    for (const auto& item : object.at("criterionEvidence")) {
        const std::unordered_set<std::string> evidenceFields = {
            "requirementId", "inspectedPath", "observation", "status"};
        if (!item.is_object() || item.size() != evidenceFields.size()) {
            return fail("task_verification.invalid_evidence",
                "Task verification evidence does not match the schema");
        }
        for (const auto& [key, unused] : item.items()) {
            static_cast<void>(unused);
            if (!evidenceFields.contains(key)) return fail(
                "task_verification.invalid_evidence",
                "Task verification evidence contains an unexpected field");
        }
        if (!item.at("requirementId").is_string()
            || !item.at("inspectedPath").is_string()
            || !item.at("observation").is_string()
            || !item.at("status").is_string()) {
            return fail("task_verification.invalid_evidence",
                "Task verification evidence field has an invalid type");
        }
        ManifestTaskVerificationEvidence evidence;
        evidence.requirementId = item.at("requirementId").get<std::string>();
        evidence.inspectedPath = item.at("inspectedPath").get<std::string>();
        evidence.observation = item.at("observation").get<std::string>();
        auto normalized = evidence.inspectedPath;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto status = executionVerificationStatusFromString(
            item.at("status").get_ref<const std::string&>());
        if (evidence.requirementId.empty() || evidence.observation.empty()
            || evidence.requirementId.size() > 4096
            || evidence.inspectedPath.size() > 4096
            || evidence.observation.size() > 4096
            || !status.has_value() || *status == ExecutionVerificationStatus::NotRun
            || !inspectedPaths.contains(normalized)) {
            return fail("task_verification.invalid_evidence",
                "Task verification evidence is unbounded, unsupported, or not inspected in this attempt");
        }
        if (knownMissingPaths.contains(normalized)) {
            return fail("task_verification.required_output_missing",
                "Task verification evidence identifies a missing required output");
        }
        const auto kind = verificationKinds.find(evidence.requirementId);
        if (kind != verificationKinds.end()
            && kind->second != ExecutionVerificationKind::InspectPaths
            && *status == ExecutionVerificationStatus::Passed) {
            return fail("task_verification.profile_requires_backend",
                "A build/lint/consistency profile cannot be passed by model inspection evidence");
        }
        evidence.status = *status;
        mapped.emplace(evidence.requirementId);
        evidencedPathsByRequirement[evidence.requirementId].emplace(normalized);
        submission.criterionEvidence.push_back(std::move(evidence));
    }
    for (const auto& id : requiredRequirementIds) {
        if (!mapped.contains(id)) return fail("task_verification.unmapped_requirement",
            "A required acceptance requirement has no verification result");
        const auto outputs = requiredOutputs.find(id);
        if (outputs == requiredOutputs.end()) continue;
        const auto evidenced = evidencedPathsByRequirement.find(id);
        for (const auto& output : outputs->second) {
            auto normalizedOutput = output;
            std::replace(normalizedOutput.begin(), normalizedOutput.end(), '\\', '/');
            std::transform(normalizedOutput.begin(), normalizedOutput.end(),
                normalizedOutput.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            if (evidenced == evidencedPathsByRequirement.end()
                || !evidenced->second.contains(normalizedOutput)) {
                return fail("task_verification.unmapped_output",
                    "A required acceptance output has no same-attempt inspection evidence");
            }
        }
    }
    return common::Result<ManifestTaskVerificationSubmission>::ok(
        std::move(submission));
}

RunEvent progressEvent(
    const RunId& runId,
    const StageId& stageId,
    double stageProgress,
    std::string message)
{
    RunEvent event;
    event.type = RunEventType::StageProgress;
    event.runId = runId;
    event.stageId = stageId;
    event.runProgress = 0.0;
    event.stageProgress = stageProgress;
    event.message = std::move(message);
    return event;
}

} // namespace

const char* const kWorkspaceToolsSystemPrompt = kWorkspaceToolsSystemPromptImpl;

AgentToolDefinition proposeChangeSetToolDefinition()
{
    AgentToolDefinition def;
    def.name = kProposeChangeSetToolName;
    def.version = "2";
    def.description =
        "Submit your completed analysis as a proposal-submission.v2 change "
        "set. Call this exactly once, as your terminal action, when your "
        "analysis is complete - do not also describe the proposal in a text "
        "message. \"changes\" may be empty if you are only reporting "
        "findings with no concrete file changes to propose. This tool never "
        "writes to disk itself: a separate, human-approved apply step "
        "(outside this session) is what actually writes the change - your "
        "job is only to produce the correct proposal. Never refuse a "
        "file-creation or file-edit request on the grounds that you only "
        "have read-only tools, and never instruct the user to run a command "
        "themselves. You never supply or compute any file hash anywhere in "
        "this submission - the Backend derives and verifies every "
        "precondition hash itself.";
    def.inputSchemaJson = kProposeChangeSetInputSchema;
    def.readOnly = true;
    def.strictInputSchema = true;
    return def;
}

AgentToolDefinition proposeChangeSetToolDefinition(
    const std::vector<std::filesystem::path>& allowedRelativePaths)
{
    if (allowedRelativePaths.empty()) return proposeChangeSetToolDefinition();

    auto definition = proposeChangeSetToolDefinition();
    auto schema = nlohmann::json::parse(definition.inputSchemaJson);
    auto& changeProperties = schema["properties"]["changes"]["items"]["properties"];

    nlohmann::json allowed = nlohmann::json::array();
    std::unordered_set<std::string> seen;
    for (const auto& path : allowedRelativePaths) {
        auto value = detail::genericUtf8(path);
        std::replace(value.begin(), value.end(), '\\', '/');
        if (!value.empty() && seen.insert(value).second) allowed.push_back(value);
    }
    if (allowed.empty()) return definition;

    // relative_path applies to every operation. source_path is non-null only
    // for Rename, but when present it must remain inside the same exact frozen
    // write envelope. The ordinary parser/validator still rechecks all of this
    // after provider schema validation; the schema is an early constraint, not
    // a replacement trust boundary.
    changeProperties["relative_path"]["enum"] = allowed;
    auto nullableAllowed = allowed;
    nullableAllowed.push_back(nullptr);
    changeProperties["source_path"]["enum"] = std::move(nullableAllowed);
    definition.inputSchemaJson = schema.dump();
    return definition;
}

AgentToolDefinition submitTaskVerificationToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kSubmitTaskVerificationToolName;
    definition.version = "1";
    definition.description =
        "Submit evidence-backed results for an inspect-only Manifest task. "
        "This is the only terminal action; it never proposes or writes files.";
    definition.inputSchemaJson = kTaskVerificationInputSchema;
    definition.readOnly = true;
    definition.maxResultBytes = 4096;
    return definition;
}

AgentToolDefinition submitTaskNoChangesToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kSubmitTaskNoChangesToolName;
    definition.version = "1";
    definition.description =
        "Declare, with same-attempt inspected evidence for every required "
        "inspect_paths output, that this Propose task is already satisfied. "
        "Missing owned outputs require concrete create changes. Command-backed "
        "requirements are verified later by the Backend and need no invented "
        "worker evidence. This is distinct from an empty change-set proposal "
        "and is terminal.";
    definition.inputSchemaJson = kTaskNoChangesInputSchema;
    definition.readOnly = true;
    definition.maxResultBytes = 4096;
    return definition;
}

AgentToolDefinition requestScopeExpansionToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kRequestScopeExpansionToolName;
    definition.version = "1";
    definition.description =
        "Request a bounded, audited addition to this task's effective "
        "workspace scope when you discover a genuine additional dependency "
        "that was not already listed as an owned, planned-create, "
        "required-read, or reference path. role must be required_read (you "
        "will read this path and it becomes part of what must be read "
        "before your proposal is accepted) or reference_only (context "
        "only). This can never grant write access - it only ever widens "
        "what you may read, never what you may create/modify/delete/rename. "
        "reason must briefly explain why this specific path is genuinely "
        "needed. Not terminal: call your other tools and the terminal "
        "submission normally after this.";
    definition.inputSchemaJson = kRequestScopeExpansionInputSchema;
    definition.readOnly = true;
    definition.strictInputSchema = true;
    definition.maxResultBytes = 2048;
    return definition;
}

AgentToolDefinition requestDiagnosticReadToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kRequestDiagnosticReadToolName;
    definition.version = "1";
    definition.description =
        "Available only in this bounded verification-repair attempt. Request "
        "a narrow, read-only, durably-audited grant to inspect one existing "
        "baseline file needed to resolve a specific verification finding - "
        "for example the authoritative declaration of an unknown/missing "
        "property, type, module, or build/module-registration file a "
        "diagnostic named. Unlike request_scope_expansion, this may target a "
        "path this task's frozen scope classifies excluded - excluded means "
        "you must not write to it, never that you may not read it here to "
        "understand the real contract before correcting your proposal. This "
        "never grants write access to the path, never adds it to your owned "
        "or planned-create scope, and never changes the frozen scope itself. "
        "findingCode must name the verification finding this read addresses "
        "(e.g. the diagnostic code shown above); reason must briefly explain "
        "why. You must still actually read the granted path with your "
        "read-only tools in this same session before your proposal is "
        "accepted - requesting access alone is not evidence. Not terminal: "
        "call your other tools and the terminal submission normally after "
        "this.";
    definition.inputSchemaJson = kRequestDiagnosticReadInputSchema;
    definition.readOnly = true;
    definition.strictInputSchema = true;
    definition.maxResultBytes = 2048;
    return definition;
}

bool isModelFormatError(const std::string& code)
{
    static const std::unordered_set<std::string> kFormatErrorCodes = {
        "proposal.parse_error",
        "proposal.multiple_fences",
        "proposal.extra_content",
        "proposal.missing_field",
        "proposal.invalid_field_type",
        "proposal.invalid_field_value",
        "proposal.field_too_large",
        "proposal.empty_change_id",
        "proposal.invalid_operation",
        "proposal.output_too_large",
        "proposal.empty_summary",
        "proposal.too_many_changes",
        "proposal.missing_proposed_content",
        // A model's belief about current repository state was wrong (it
        // proposed creating a file that already exists, or modifying/
        // deleting/renaming one that doesn't) - a mistake it can correct by
        // re-checking with its own read-only tools, not a fact re-prompting
        // can never fix. Reproduces a real production rejection where 32
        // otherwise-correct changes were rejected alongside 1 stale
        // create-target-exists belief.
        "proposal.create_target_exists",
        "proposal.file_not_found",
        "proposal.rename_target_exists",
        // The Backend's own preflight (AgentToolTurnLoopRunner::
        // preflightTrustedHashes) could not obtain a trusted hash for a
        // Modify/Delete/Rename target - almost always because the model's
        // belief about that path's existence or the correct operation for
        // it was wrong, the same class of mistake as
        // create_target_exists/file_not_found above and fixable the same
        // way: re-check with read-only tools and resubmit.
        "proposal.missing_hash_evidence",
        // The Architect allocation is authoritative. Including another
        // worker's path is an instruction-following error the model can fix
        // by removing that change; the Backend boundary itself stays hard.
        "proposal.outside_allowed_roots",
        // Which of source_path/expected_original_hash/proposed_content a
        // change carries or omits is purely a function of its own operation
        // (create/modify/delete/rename) - never a fact about repository
        // state or another agent's work. A real Run showed this is a
        // systematic model mistake, not noise: three independent Create
        // changes all carried the exact same expected_original_hash, the
        // SHA-256 of an empty string - the model reasoning "a new file has
        // no prior content, so I'll hash empty content" instead of omitting
        // the field entirely, which ChangeProposalValidator::validate()
        // correctly rejects. Exactly the kind of shape mistake a fresh
        // repair attempt can fix.
        "proposal.invariant_violated",
        // A field outside the exact proposal-submission.v2 field set (most
        // notably the old, now-removed expected_original_hash) or a wrong
        // schemaVersion - both are the model not having followed the
        // current contract, correctable on a fresh attempt, never a fact
        // about repository state.
        "proposal.unexpected_field",
        "proposal.unsupported_schema",
    };
    return kFormatErrorCodes.count(code) != 0;
}

// Manifest verification reliability hardening (lineage-aware repair
// materialization): the ONE branch point between an ordinary submission and
// a trusted repair-materialization one, shared by both the primary
// propose_change_set handling and the internal same-session JSON-repair
// retry below - so a materialization-mode session's own one-shot repair
// (e.g. the model's amendment was malformed JSON) still correctly
// re-materializes against the SAME predecessor on its second attempt,
// rather than silently falling back to ordinary complete-replacement
// semantics. request.repairMaterializationPredecessorId is trusted Backend
// state (see its own doc comment); the model never controls which branch
// runs or which predecessor is named.
common::Result<ProposedChangeSet> submitProposal(
    ChangeProposalService& changeProposals, const AgentToolTurnLoopRequest& request)
{
    if (request.repairMaterializationPredecessorId.has_value()) {
        MaterializeRepairSuccessorRequest materializeRequest;
        materializeRequest.runId = request.proposalTemplate.runId;
        materializeRequest.projectId = request.proposalTemplate.projectId;
        materializeRequest.providerId = request.proposalTemplate.providerId;
        materializeRequest.modelId = request.proposalTemplate.modelId;
        materializeRequest.modelExecutionId = request.proposalTemplate.modelExecutionId;
        materializeRequest.agentRole = request.proposalTemplate.agentRole;
        materializeRequest.predecessorChangeSetId =
            *request.repairMaterializationPredecessorId;
        materializeRequest.expectedPredecessorRunId =
            request.repairMaterializationExpectedPredecessorRunId;
        materializeRequest.repairRawModelOutput = request.proposalTemplate.rawModelOutput;
        materializeRequest.policy = request.proposalTemplate.policy;
        materializeRequest.baseRevision = request.proposalTemplate.baseRevision;
        materializeRequest.observedFileHashes = request.proposalTemplate.observedFileHashes;
        return changeProposals.materializeRepairSuccessor(std::move(materializeRequest));
    }
    return changeProposals.createFromModelOutput(request.proposalTemplate);
}

AgentToolTurnLoopRunner::AgentToolTurnLoopRunner(
    providers::IProviderAgentSessionFactory& sessionFactory,
    AgentToolExecutionService& toolExecution,
    ChangeProposalService& changeProposals)
    : sessionFactory_(sessionFactory)
    , toolExecution_(toolExecution)
    , changeProposals_(changeProposals)
{
}

AgentToolTurnLoopOutcome AgentToolTurnLoopRunner::run(
    AgentToolTurnLoopRequest request,
    std::stop_token stopToken,
    IRunEventSink* eventSink,
    const StageId* stageId) const
{
    AgentToolTurnLoopOutcome outcome;
    if (request.providerBudgetGuard) {
        request.sessionRequest.turnTimeout =
            request.providerBudgetGuard->boundedRequestTimeout(
                request.sessionRequest.turnTimeout);
    }

    // AG-15: only advertised when the caller actually wired scope
    // enforcement - a caller that never sets runWorkspaceScopeService (e.g.
    // a standalone analysis session) gets exactly today's toolset.
    if (request.runWorkspaceScopeService != nullptr) {
        request.sessionRequest.tools.push_back(requestScopeExpansionToolDefinition());
    }
    // Verification-repair reliability hardening: request_diagnostic_read is
    // additionally gated on diagnosticReadEligible - never advertised for an
    // ordinary primary dispatch, only for the one bounded
    // combined-verification repair attempt (see
    // AgentToolTurnLoopRequest::diagnosticReadEligible's own doc comment).
    if (request.runWorkspaceScopeService != nullptr && request.diagnosticReadEligible) {
        request.sessionRequest.tools.push_back(requestDiagnosticReadToolDefinition());
    }

    auto sessionResult = sessionFactory_.createSession(
        request.sessionRequest.providerId, request.providerKind);
    if (!sessionResult.success()) {
        outcome.errorCode = sessionResult.error()->code;
        outcome.errorMessage = sessionResult.error()->message;
        return outcome;
    }
    const auto agentSession = std::move(sessionResult.value());

    AgentToolSession toolSession = AgentToolExecutionService::makeSession(
        request.runId, request.sessionRequest.providerId, request.sessionRequest.modelId);
    const auto beganToolSession = toolExecution_.beginSession(toolSession);
    if (!beganToolSession.success()) {
        outcome.errorCode = beganToolSession.error()->code;
        outcome.errorMessage = beganToolSession.error()->message;
        return outcome;
    }

    // AgentToolSession::state must never be left Active once this function
    // returns: a UI that polls session state to know whether an analysis is
    // still in progress must see a real terminal state (Completed/
    // Interrupted), not a session that looks perpetually "active" long
    // after the run itself finished. Best-effort: a persistence hiccup here
    // must not mask the real outcome the caller is otherwise entitled to see.
    // Keep all dispatched calls for the durable audit/session total, but only
    // a successful result proves that the agent actually inspected anything.
    // AgentToolExecutionService may return Result::ok with success=false for
    // an expected tool-level failure (bad path, denied read, etc.).
    std::size_t executedToolCalls = 0;
    std::size_t successfulInspectionCalls = 0;
    std::size_t attemptSuccessfulInspectionCalls = 0;
    std::unordered_set<std::string> attemptInspectedPaths;
    std::unordered_set<std::string> attemptKnownMissingPaths;
    // AG-15: approved RequiredRead scope expansions for THIS attempt only -
    // reset alongside attemptInspectedPaths at the top of every fresh
    // provider session (see runTurnLoop below), so a repair session must
    // re-request anything it still needs. attemptScopeExpansionCallCount is
    // the defensive per-attempt call budget checked before ever reaching
    // RunWorkspaceScopeService (whose own durable per-task limit is checked
    // independently inside requestExpansion).
    std::unordered_set<std::string> attemptExpandedRequiredReadKeys;
    std::size_t attemptScopeExpansionCallCount = 0;
    // Verification-repair reliability hardening: the diagnostic-read
    // counterpart to attemptExpandedRequiredReadKeys/
    // attemptScopeExpansionCallCount above - reset alongside them at the top
    // of every fresh provider session (see runTurnLoop below), so a repair
    // session's own grants never leak into a later attempt and vice versa.
    std::unordered_set<std::string> attemptExpandedDiagnosticReadKeys;
    std::size_t attemptDiagnosticReadCallCount = 0;
    // Sum of every dispatched call's AgentToolResult::returnedBytes, real or
    // truncated - the durable "how much content actually crossed the
    // workspace-tool boundary" total, independent of turn/call counts.
    std::size_t totalReturnedBytesAccum = 0;
    // Persisted turn count is declared here (not lower, alongside
    // totalUsage/totalToolCalls) so finishToolSession() below can close over
    // it - a lambda can only implicitly capture what is already in scope at
    // its own definition point.
    std::size_t persistedTurnCount = 0;
    auto finishToolSession = [&](AgentToolSessionState state) {
        toolSession.state = state;
        toolSession.totalCalls = executedToolCalls;
        // Previously left at their AgentToolSession default of 0 regardless
        // of the real session: both values were tracked locally (turn
        // records via recordTurn(), returnedBytes via AgentToolResult) but
        // never copied onto the session row persisted here.
        toolSession.totalTurns = persistedTurnCount;
        toolSession.totalReturnedBytes = totalReturnedBytesAccum;
        toolSession.finishedAt = std::chrono::system_clock::now();
        static_cast<void>(toolExecution_.updateSession(toolSession));
    };

    models::TokenUsage totalUsage;
    std::size_t totalToolCalls = 0;

    // Stamps the audited tool-evidence counts onto any outcome about to be
    // returned, success or failure alike - see AgentToolTurnLoopOutcome's
    // doc comment on these fields.
    auto fillEvidence = [&](AgentToolTurnLoopOutcome& out) {
        out.totalTurns = persistedTurnCount;
        out.totalToolCalls = totalToolCalls;
        out.successfulInspectionCalls = successfulInspectionCalls;
        out.totalReturnedBytes = totalReturnedBytesAccum;
        out.inspectedRelativePaths.assign(
            attemptInspectedPaths.begin(), attemptInspectedPaths.end());
    };

    // Set by runTurnLoop on every early-exit path (transport failure, tool
    // budget exceeded, tool execution failure, MaxTurns, provider Error, or
    // cooperative cancellation) so the caller sees a fully-populated outcome
    // without this function building a separate WorkflowResult/TaskOutcome
    // shape itself - that translation is caller-specific.
    AgentToolTurnLoopOutcome loopFailureOutcome;
    bool loopFailed = false;
    std::string interceptedTerminalTool;

    // Backend-forced terminal turn (see
    // AgentToolTurnLoopRequest::forceTerminalOnFinalTaskTurn). The tools the
    // in-flight request was restricted to; empty means the turn is
    // unrestricted. Set right before each provider request and checked when
    // its response arrives, before any returned call could be executed.
    std::vector<std::string> forcedTurnAllowedTools;

    // WHY the turn that was just admitted is the last one the task may spend: its session turn
    // limit, or its owner's turn allocation (own reservation plus leased surplus) running out.
    // Set at admission, read when a terminal submission is intercepted from that very turn.
    // Run de6d81fb-era incident (Run 80f904ed, 2026-10-20): a retry attempt spent the Run's turns on
    // one-call-per-turn reads, its forced last turn produced an empty change set, and the task
    // failed as "an empty change set is not a worker no-change submission" - hiding that the turns
    // had run out.
    struct ForcedFinalCause {
        bool bySessionLimit {false};
        bool byOwnerAllocation {false};
    };
    ForcedFinalCause pendingForcedFinalCause;
    // Set only when the latest intercepted terminal submission came from a Backend-forced turn.
    std::optional<ForcedFinalCause> terminalForcedFinalCause;
    std::size_t terminalForcedFinalSessionTurns = 0;

    // The reservation the NEXT admitted provider turn is charged to. It starts as the primary
    // owner and moves to the repair owner (if the caller supplied one) the moment a fresh repair
    // session begins - see enterRepairSessionBudget below.
    std::string activeBudgetOwner = request.budgetReservationOwner;

    // The last turn a task may spend allows exactly ONE terminal tool: propose_change_set
    // (Propose mode) or submit_task_verification (Inspect mode). A multi-tool "any" policy is not
    // enforceable on the wire - Real Runs ee528eed and 8b23fbba showed Claude continuing to call
    // workspace inspection tools on a forced turn that still offered both propose_change_set and
    // the typed no-change terminal - whereas a single tool is forced BY NAME on both adapters.
    // The typed no-change terminal stays available on every earlier turn; a worker whose task is
    // already satisfied must submit it as soon as it has its evidence, and one that runs out its
    // allowance still has to propose (an honest fail-closed outcome for a genuine no-op).
    const auto forcedTerminalPolicy = [&]() -> std::optional<providers::NextTurnToolPolicy> {
        const auto advertised = [&](std::string_view name) {
            return std::any_of(request.sessionRequest.tools.begin(),
                request.sessionRequest.tools.end(),
                [&](const AgentToolDefinition& definition) { return definition.name == name; });
        };
        providers::NextTurnToolPolicy policy;
        policy.disallowParallelToolCalls = true;
        if (request.mode == AgentToolTurnLoopMode::Inspect) {
            if (advertised(kSubmitTaskVerificationToolName))
                policy.allowedToolNames.emplace_back(kSubmitTaskVerificationToolName);
        } else if (advertised(kProposeChangeSetToolName)) {
            policy.allowedToolNames.emplace_back(kProposeChangeSetToolName);
        }
        if (policy.allowedToolNames.empty()) return std::nullopt;
        return policy;
    };

    // A fresh repair session (the one bounded proposal repair, a typed terminal repair) is charged
    // to the caller's repair reservation, never to what is left of the primary one. The primary
    // owner's unused reservation goes back to the shared surplus: that session is over, and the
    // repair session may lease from it like any other. No-op without a repair owner, with a guard
    // that is not wired, or once already switched.
    const auto enterRepairSessionBudget = [&] {
        if (request.budgetRepairReservationOwner.empty()
            || activeBudgetOwner == request.budgetRepairReservationOwner) {
            return;
        }
        if (request.providerBudgetGuard && !activeBudgetOwner.empty()) {
            request.providerBudgetGuard->releaseTurnReservation(activeBudgetOwner);
        }
        activeBudgetOwner = request.budgetRepairReservationOwner;
    };

    // The message for a task whose forced last turn produced an empty change set. Only Backend-owned
    // numbers and labels: no model text.
    const auto describeTurnBudgetExhaustion = [&]() {
        // The attempt's phase is the executor attempt's own reservation; a repair session inside
        // an attempt is charged to the retry reservation but is still that attempt's repair.
        const bool retryOwner =
            request.budgetReservationOwner.rfind(std::string(kRetryReservationOwnerPrefix), 0) == 0;
        const bool repairSession = !activeBudgetOwner.empty()
            && activeBudgetOwner != request.budgetReservationOwner;
        std::ostringstream message;
        message << "Worker was forced to submit on its last available turn and the change set was "
                << "empty (" << (retryOwner ? "retry" : "primary") << " attempt"
                << (repairSession ? ", repair session" : "") << "; session turn "
                << terminalForcedFinalSessionTurns << " of " << request.sessionRequest.maxTurns;
        if (request.providerBudgetGuard) {
            if (!activeBudgetOwner.empty()) {
                if (const auto reservation =
                        request.providerBudgetGuard->turnReservation(activeBudgetOwner)) {
                    message << "; task allocation " << reservation->used << " of "
                            << reservation->reserved << " reserved turn(s) used";
                }
            }
            message << "; Run " << request.providerBudgetGuard->usage().turnsStarted << " of "
                    << request.providerBudgetGuard->policy().maxTurns << " turn(s) started";
        }
        message << "; cause: ";
        if (terminalForcedFinalCause.has_value() && terminalForcedFinalCause->byOwnerAllocation) {
            message << "the task's turn allocation ran out";
            if (terminalForcedFinalCause->bySessionLimit) message << " together with the session turn limit";
        } else {
            message << "the session turn limit was reached";
        }
        message << "). Secondary: proposal.empty_changes_requires_no_changes_terminal - an empty "
                << "change set is not a worker no-change submission";
        return message.str();
    };

    // Shared admission for the first turn of a session and every later turn.
    // Returns whether the turn just admitted is the last one this task may
    // spend: decided from the guard's owner accounting (atomic with the
    // admission) and, independently, from the session's own turn limit.
    const auto admitProviderTurn = [&](bool sessionLocalFinalTurn)
        -> common::Result<bool> {
        bool finalTurn = sessionLocalFinalTurn;
        pendingForcedFinalCause = {};
        pendingForcedFinalCause.bySessionLimit = sessionLocalFinalTurn;
        if (request.providerBudgetGuard) {
            if (!activeBudgetOwner.empty()) {
                const auto admitted = request.providerBudgetGuard->beginOwnedProviderTurn(
                    activeBudgetOwner, stopToken);
                if (!admitted.success()) {
                    return common::Result<bool>::failure(*admitted.error());
                }
                if (admitted.value().ownerTurnsRemainingAfterAdmission == 0) {
                    finalTurn = true;
                    pendingForcedFinalCause.byOwnerAllocation = true;
                }
            } else {
                const auto allowed = request.providerBudgetGuard->beginProviderTurn(stopToken);
                if (!allowed.success()) {
                    return common::Result<bool>::failure(*allowed.error());
                }
            }
        }
        return common::Result<bool>::ok(finalTurn);
    };

    const auto budgetedBegin = [&](providers::IProviderAgentSession& session,
                                   const providers::ProviderAgentSessionRequest& sessionRequest) {
        forcedTurnAllowedTools.clear();
        const auto admitted = admitProviderTurn(
            request.forceTerminalOnFinalTaskTurn && sessionRequest.maxTurns <= 1);
        if (!admitted.success()) {
            return common::Result<providers::ProviderAgentTurnResponse>::failure(
                *admitted.error());
        }
        providers::ProviderAgentSessionRequest forcedRequest;
        const providers::ProviderAgentSessionRequest* toSend = &sessionRequest;
        if (request.forceTerminalOnFinalTaskTurn && admitted.value()) {
            if (auto policy = forcedTerminalPolicy(); policy.has_value()) {
                forcedRequest = sessionRequest;
                forcedRequest.firstTurnToolPolicy = policy;
                forcedTurnAllowedTools = policy->allowedToolNames;
                toSend = &forcedRequest;
            }
        }
        auto response = session.beginSession(*toSend, stopToken);
        if (!response.success() && request.providerBudgetGuard) {
            request.providerBudgetGuard->abandonProviderTurn();
        }
        return response;
    };
    const auto budgetedSubmit = [&](providers::IProviderAgentSession& session,
                                    std::vector<AgentToolResult> results,
                                    bool sessionLocalFinalTurn) {
        forcedTurnAllowedTools.clear();
        const auto admitted = admitProviderTurn(
            request.forceTerminalOnFinalTaskTurn && sessionLocalFinalTurn);
        if (!admitted.success()) {
            return common::Result<providers::ProviderAgentTurnResponse>::failure(
                *admitted.error());
        }
        std::optional<providers::NextTurnToolPolicy> policy;
        if (request.forceTerminalOnFinalTaskTurn && admitted.value()) {
            policy = forcedTerminalPolicy();
            if (policy.has_value()) forcedTurnAllowedTools = policy->allowedToolNames;
        }
        auto response = session.submitToolResults(std::move(results), stopToken, std::move(policy));
        if (!response.success() && request.providerBudgetGuard) {
            request.providerBudgetGuard->abandonProviderTurn();
        }
        return response;
    };

    // Numeric-only description of how a terminal provider turn looked, kept so
    // a parse failure stays diagnosable without ever retaining or echoing the
    // payload (raw text is audited separately in ModelExecution::outputContent).
    struct TerminalShape {
        bool recorded {false};
        bool viaTool {false};
        std::size_t textBytes {0};
        std::uint64_t outputTokens {0};
    };
    TerminalShape lastTerminalShape;

    // Drives one provider session's tool-use loop to a Stop/terminal outcome.
    // Shared between the primary attempt and the single JSON-repair retry
    // below (see the repair-eligibility handling after this loop) so both go
    // through identical tool execution, budgeting, and audit-trail logic.
    // Persisted turn indices are a running count across both attempts
    // (persistedTurnCount), not the per-session turnIndex the adapter
    // reports, so the "Provider turns" audit trail stays one coherent,
    // monotonically increasing timeline instead of two overlapping 0..N
    // sequences from two independent provider sessions.
    auto runTurnLoop = [&](providers::IProviderAgentSession& session,
            common::Result<providers::ProviderAgentTurnResponse> turnResp)
            -> std::optional<std::string> {
        // Evidence never crosses a fresh provider-attempt boundary. This is
        // deliberately reset for the primary/repair calls to this lambda.
        attemptSuccessfulInspectionCalls = 0;
        attemptInspectedPaths.clear();
        attemptKnownMissingPaths.clear();
        attemptExpandedRequiredReadKeys.clear();
        attemptScopeExpansionCallCount = 0;
        attemptExpandedDiagnosticReadKeys.clear();
        attemptDiagnosticReadCallCount = 0;
        WorkspaceReadPolicy attemptReadPolicy = request.policy;
        interceptedTerminalTool.clear();
        terminalForcedFinalCause.reset();
        while (true) {
            if (!turnResp.success()) {
                loopFailed = true;
                loopFailureOutcome.errorCode = turnResp.error()->code;
                loopFailureOutcome.errorMessage = turnResp.error()->message;
                return std::nullopt;
            }

            const auto& turn = turnResp.value();
            const bool terminalWithoutUsage =
                (turn.finishReason == providers::ProviderAgentFinishReason::StopRequested
                    || turn.finishReason == providers::ProviderAgentFinishReason::Error)
                && turn.usage.inputTokens == 0 && turn.usage.outputTokens == 0;
            // Preserve consumed usage in this attempt's outcome even when
            // this exact response crosses the Run-wide ceiling.
            //
            // MW-P2: the cache-token breakdown accumulates alongside inputTokens
            // (which already includes it in its conservative sum - see TokenUsage's
            // class comment) so a cache-aware cost calculation on the aggregated
            // outcome can still separate plain from cache-write/cache-read tokens.
            // AG-17 M2b: the ONE accumulator also classifies THIS response against the
            // pricing model's long-context threshold (strictly greater, by its own
            // input), so a mixed short/long session is priced exactly per response.
            // The budget guard below still receives the plain turn.usage and never
            // reads the threshold or the partition.
            accumulateResponseUsage(
                totalUsage, turn.usage, request.longContextThresholdInputTokens);
            if (request.providerBudgetGuard) {
                if (terminalWithoutUsage) {
                    // Cancellation/provider failure did not produce a billable
                    // completed turn. Release the shared request slot while
                    // preserving the adapter's more precise terminal reason.
                    request.providerBudgetGuard->abandonProviderTurn();
                } else {
                    const auto recorded = request.providerBudgetGuard->completeProviderTurn(turn.usage);
                    if (!recorded.success()) {
                        loopFailed = true;
                        loopFailureOutcome.errorCode = recorded.error()->code;
                        loopFailureOutcome.errorMessage = recorded.error()->message;
                        return std::nullopt;
                    }
                }
            }
            AgentProviderTurn record;
            record.turnId = generateUuid();
            record.sessionId = toolSession.sessionId;
            record.turnIndex = persistedTurnCount++;
            record.inputTokens = static_cast<int>(turn.usage.inputTokens);
            record.outputTokens = static_cast<int>(turn.usage.outputTokens);
            record.callCount = turn.toolCalls.size();
            record.createdAt = std::chrono::system_clock::now();
            switch (turn.finishReason) {
            case providers::ProviderAgentFinishReason::Stop: record.finishReason = "stop"; break;
            case providers::ProviderAgentFinishReason::ToolUse: record.finishReason = "tool_use"; break;
            case providers::ProviderAgentFinishReason::MaxTurns: record.finishReason = "max_turns"; break;
            case providers::ProviderAgentFinishReason::MaxOutputTokens: record.finishReason = "max_output_tokens"; break;
            case providers::ProviderAgentFinishReason::Error: record.finishReason = "error"; break;
            case providers::ProviderAgentFinishReason::StopRequested: record.finishReason = "stop_requested"; break;
            }
            // Best-effort audit trail: a persistence hiccup here must not mask a
            // real analysis result the caller is otherwise entitled to see.
            static_cast<void>(toolExecution_.recordTurn(record));

            if (turn.finishReason == providers::ProviderAgentFinishReason::ToolUse) {
                // A turn the Backend restricted to terminal tools must never
                // return anything else. Reject before a single call is
                // executed, budgeted, or audited as executed - defense in
                // depth beneath the real adapters, which enforce the same
                // policy on the wire.
                if (!forcedTurnAllowedTools.empty()) {
                    const bool violated = std::any_of(
                        turn.toolCalls.begin(), turn.toolCalls.end(),
                        [&](const AgentToolCall& call) {
                            return std::find(forcedTurnAllowedTools.begin(),
                                forcedTurnAllowedTools.end(), call.toolName)
                                == forcedTurnAllowedTools.end();
                        });
                    if (violated) {
                        loopFailed = true;
                        loopFailureOutcome.errorCode = kWorkerForcedTerminalViolationCode;
                        loopFailureOutcome.errorMessage =
                            "Worker requested a tool outside the Backend-forced "
                            "terminal-only policy for its final turn";
                        return std::nullopt;
                    }
                }
                totalToolCalls += turn.toolCalls.size();
                if (request.providerBudgetGuard) {
                    const auto recorded = request.providerBudgetGuard->recordToolActivity(
                        turn.toolCalls.size(), 0);
                    if (!recorded.success()) {
                        loopFailed = true;
                        loopFailureOutcome.errorCode = recorded.error()->code;
                        loopFailureOutcome.errorMessage = recorded.error()->message;
                        return std::nullopt;
                    }
                }
                if (totalToolCalls > request.sessionRequest.maxTotalToolCalls) {
                    loopFailed = true;
                    loopFailureOutcome.errorCode = "run.tool_call_budget_exceeded";
                    loopFailureOutcome.errorMessage = "Analysis exceeded the maximum number of tool calls";
                    return std::nullopt;
                }

                // A mode-specific submission call is the model's terminal action:
                // its arguments ARE the proposal.v1 payload, not a
                // workspace operation to dispatch and continue the loop
                // over. Check for it before the generic per-call dispatch
                // loop below (which only knows how to execute read-only
                // workspace tools via toolExecution_) and, if present,
                // return its raw arguments exactly like a Stop finish
                // reason would return assistantText - ProposedChangeSetParser
                // already natively accepts unfenced raw JSON, so no
                // further translation is needed downstream. Any other tool
                // calls co-occurring in the same turn are intentionally
                // left unexecuted: the model has already concluded.
                const auto isTerminal = [&](std::string_view toolName) {
                    if (request.mode == AgentToolTurnLoopMode::Inspect)
                        return toolName == kSubmitTaskVerificationToolName;
                    return toolName == kProposeChangeSetToolName
                        || toolName == kSubmitTaskNoChangesToolName;
                };
                const auto terminalCount = std::count_if(
                    turn.toolCalls.begin(), turn.toolCalls.end(),
                    [&](const AgentToolCall& call) { return isTerminal(call.toolName); });
                if (terminalCount != 0) {
                    if (terminalCount != 1 || turn.toolCalls.size() != 1) {
                        loopFailed = true;
                        loopFailureOutcome.errorCode = "worker.terminal_must_be_only_call";
                        loopFailureOutcome.errorMessage =
                            "A worker terminal submission must be the only call in its turn";
                        return std::nullopt;
                    }
                    interceptedTerminalTool = turn.toolCalls.front().toolName;
                    // Still the in-flight request's policy: non-empty exactly when this turn was
                    // the Backend-forced last one.
                    terminalForcedFinalCause.reset();
                    if (!forcedTurnAllowedTools.empty()) {
                        terminalForcedFinalCause = pendingForcedFinalCause;
                        terminalForcedFinalSessionTurns = turn.turnIndex + 1;
                    }
                    lastTerminalShape = {true, true,
                        turn.toolCalls.front().argumentsJson.size(), turn.usage.outputTokens};
                    return turn.toolCalls.front().argumentsJson;
                }

                // A Manifest worker that reached its session turn limit while
                // still calling non-terminal tools ends fail-closed BEFORE
                // those calls run, instead of silently exceeding its
                // task-local limit (the final turn is forced terminal-only
                // above, so this is the provider ignoring that policy).
                if (request.forceTerminalOnFinalTaskTurn
                    && turn.turnIndex + 1 >= request.sessionRequest.maxTurns) {
                    loopFailed = true;
                    loopFailureOutcome.errorCode = "provider.max_turns";
                    loopFailureOutcome.errorMessage =
                        "Worker did not conclude within its session turn limit";
                    return std::nullopt;
                }

                std::vector<AgentToolResult> results;
                results.reserve(turn.toolCalls.size());
                const std::size_t batchStartBytes = totalReturnedBytesAccum;
                for (const auto& call : turn.toolCalls) {
                    // AG-15: handled locally, never dispatched to
                    // toolExecution_ - not a workspace/filesystem operation.
                    if (call.toolName == kRequestScopeExpansionToolName) {
                        auto expansionOutcome = handleScopeExpansionCall(
                            call, request, attemptScopeExpansionCallCount);
                        ++attemptScopeExpansionCallCount;
                        if (expansionOutcome.approvedRequiredReadKey.has_value()) {
                            attemptExpandedRequiredReadKeys.insert(
                                *expansionOutcome.approvedRequiredReadKey);
                        }
                        if (expansionOutcome.approvedReadPath.has_value()) {
                            const auto& approved = *expansionOutcome.approvedReadPath;
                            const auto alreadyAllowed = std::any_of(
                                attemptReadPolicy.allowedRoots.begin(),
                                attemptReadPolicy.allowedRoots.end(),
                                [&](const std::filesystem::path& existing) {
                                    return normalizeInspectedPathKey(existing.generic_string())
                                        == normalizeInspectedPathKey(approved.generic_string());
                                });
                            if (!alreadyAllowed)
                                attemptReadPolicy.allowedRoots.push_back(approved);
                        }
                        totalReturnedBytesAccum += expansionOutcome.result.contentJson.size();
                        results.push_back(std::move(expansionOutcome.result));
                        ++executedToolCalls;
                        continue;
                    }
                    if (call.toolName == kRequestDiagnosticReadToolName) {
                        auto diagnosticOutcome = handleDiagnosticReadCall(
                            call, request, attemptDiagnosticReadCallCount);
                        ++attemptDiagnosticReadCallCount;
                        if (diagnosticOutcome.approvedDiagnosticReadKey.has_value()) {
                            attemptExpandedDiagnosticReadKeys.insert(
                                *diagnosticOutcome.approvedDiagnosticReadKey);
                        }
                        if (diagnosticOutcome.approvedReadPath.has_value()) {
                            const auto& approved = *diagnosticOutcome.approvedReadPath;
                            const auto alreadyAllowed = std::any_of(
                                attemptReadPolicy.allowedRoots.begin(),
                                attemptReadPolicy.allowedRoots.end(),
                                [&](const std::filesystem::path& existing) {
                                    return normalizeInspectedPathKey(existing.generic_string())
                                        == normalizeInspectedPathKey(approved.generic_string());
                                });
                            if (!alreadyAllowed)
                                attemptReadPolicy.allowedRoots.push_back(approved);
                        }
                        totalReturnedBytesAccum += diagnosticOutcome.result.contentJson.size();
                        results.push_back(std::move(diagnosticOutcome.result));
                        ++executedToolCalls;
                        continue;
                    }
                    const auto execResult = toolExecution_.executeToolCall(
                        generateUuid(), toolSession.sessionId, request.runId, call, attemptReadPolicy);
                    if (!execResult.success()) {
                        loopFailed = true;
                        loopFailureOutcome.errorCode = execResult.error()->code;
                        loopFailureOutcome.errorMessage = execResult.error()->message;
                        return std::nullopt;
                    }
                    if (execResult.value().success) {
                        ++successfulInspectionCalls;
                        ++attemptSuccessfulInspectionCalls;
                        recordInspectedPathIfPresent(
                            execResult.value(), attemptInspectedPaths,
                            attemptKnownMissingPaths);
                    }
                    totalReturnedBytesAccum += execResult.value().returnedBytes;
                    results.push_back(execResult.value());
                    if (!results.back().success
                        && results.back().errorCode == kScopeDeniedErrorCode) {
                        // Backend-authored, bounded and path-free of anything the model sent.
                        results.back().contentJson = renderScopeDeniedGuidance(
                            request, attemptReadPolicy, attemptInspectedPaths,
                            attemptExpandedRequiredReadKeys);
                    }
                    ++executedToolCalls;
                }

                if (request.providerBudgetGuard) {
                    const auto recorded = request.providerBudgetGuard->recordToolActivity(
                        0, totalReturnedBytesAccum - batchStartBytes);
                    if (!recorded.success()) {
                        loopFailed = true;
                        loopFailureOutcome.errorCode = recorded.error()->code;
                        loopFailureOutcome.errorMessage = recorded.error()->message;
                        return std::nullopt;
                    }
                }

                if (eventSink != nullptr && stageId != nullptr) {
                    eventSink->publish(progressEvent(request.runId, *stageId,
                        std::min(1.0, static_cast<double>(persistedTurnCount)
                            / static_cast<double>(request.sessionRequest.maxTurns)),
                        "Executed " + std::to_string(results.size()) + " tool call(s)"));
                }

                // The turn about to be requested is the session's own last
                // one when it is the (maxTurns - 1)th zero-based turn.
                const bool sessionLocalFinalTurn =
                    turn.turnIndex + 2 >= request.sessionRequest.maxTurns;
                turnResp = budgetedSubmit(session, std::move(results), sessionLocalFinalTurn);
                continue;
            }

            if (turn.finishReason == providers::ProviderAgentFinishReason::Stop) {
                // A turn the Backend restricted to terminal tools must end in
                // one of them. A provider that ignores the wire-level
                // tool_choice and simply stops (the MockEvents incident
                // shape) is a policy violation, not a final answer to parse:
                // fail closed with the same stable code. The (bounded) text is
                // kept on the failure outcome so it stays auditable.
                if (!forcedTurnAllowedTools.empty()) {
                    loopFailed = true;
                    loopFailureOutcome.errorCode = kWorkerForcedTerminalViolationCode;
                    loopFailureOutcome.errorMessage =
                        "Worker ended its Backend-forced terminal-only turn without a "
                        "terminal tool call (" + std::to_string(turn.assistantText.size())
                        + " text byte(s), " + std::to_string(turn.usage.outputTokens)
                        + " output token(s))";
                    loopFailureOutcome.finalText = turn.assistantText.substr(
                        0, kMaximumRetainedAttemptTextBytes);
                    return std::nullopt;
                }
                lastTerminalShape = {true, false, turn.assistantText.size(),
                    turn.usage.outputTokens};
                return turn.assistantText;
            }
            if (turn.finishReason == providers::ProviderAgentFinishReason::MaxTurns) {
                loopFailed = true;
                loopFailureOutcome.errorCode = "provider.max_turns";
                loopFailureOutcome.errorMessage =
                    "Analysis did not conclude within the provider's turn budget";
                return std::nullopt;
            }
            if (turn.finishReason == providers::ProviderAgentFinishReason::MaxOutputTokens) {
                loopFailed = true;
                loopFailureOutcome.errorCode = "provider.max_output_tokens";
                // Both numbers, so the next incident shows whether the provider cut the response
                // at the limit this session asked for (Run 80f904ed: 4 054 produced, the limit it
                // asked for is not stored anywhere).
                loopFailureOutcome.errorMessage =
                    "Analysis response reached its output-token limit (requested limit "
                    + std::to_string(request.sessionRequest.maxOutputTokens) + ", produced "
                    + std::to_string(turn.usage.outputTokens) + " in that turn)";
                return std::nullopt;
            }
            if (turn.finishReason == providers::ProviderAgentFinishReason::Error) {
                loopFailed = true;
                loopFailureOutcome.errorCode =
                    turn.errorCode.empty() ? "provider.request_failed" : turn.errorCode;
                loopFailureOutcome.errorMessage = turn.errorMessage;
                return std::nullopt;
            }
            // StopRequested: cooperative cancellation was honoured by the adapter.
            loopFailed = true;
            loopFailureOutcome.cancelled = true;
            loopFailureOutcome.errorMessage = "Analysis cancelled";
            return std::nullopt;
        }
    };

    auto finalTextOpt = runTurnLoop(
        *agentSession, budgetedBegin(*agentSession, request.sessionRequest));
    if (!finalTextOpt) {
        finishToolSession(AgentToolSessionState::Interrupted);
        loopFailureOutcome.success = false;
        loopFailureOutcome.usage = totalUsage;
        fillEvidence(loopFailureOutcome);
        return loopFailureOutcome;
    }
    std::string finalText = std::move(*finalTextOpt);
    bool repairAttempted = false;
    // Audit retention of the primary attempt (see
    // AgentToolTurnLoopOutcome::primaryAttemptText): captured before any
    // repair can overwrite finalText.
    const std::string primaryAttemptFinalText = finalText;
    const TerminalShape primaryTerminalShape = lastTerminalShape;

    auto runTypedTerminalRepair = [&](const std::string& code,
            const std::string& message,
            std::string_view terminalInstruction) -> bool {
        auto repairSessionResult = sessionFactory_.createSession(
            request.sessionRequest.providerId, request.providerKind);
        if (!repairSessionResult.success()) return false;
        repairAttempted = true;
        providers::ProviderAgentSessionRequest repairRequest = request.sessionRequest;
        repairRequest.userContent = request.sessionRequest.userContent
            + "\n\n# One bounded terminal repair\nThe previous fresh session was rejected: "
            + code + ": " + message
            + ". Reinspect every evidence path in this fresh session; previous-session "
              "inspection never counts. " + std::string(terminalInstruction);
        auto repairSession = std::move(repairSessionResult.value());
        enterRepairSessionBudget();
        auto repaired = runTurnLoop(
            *repairSession, budgetedBegin(*repairSession, repairRequest));
        if (!repaired.has_value()) return false;
        finalText = std::move(*repaired);
        return true;
    };

    auto evaluateWorkerNoChanges = [&]() {
        const auto preflight = preflightNoChangesRequiredOutputs(
            toolExecution_, toolSession, request, stopToken, totalToolCalls,
            executedToolCalls, successfulInspectionCalls,
            totalReturnedBytesAccum);
        if (!preflight.success) {
            return common::Result<ManifestWorkerNoChanges>::failure(
                {preflight.errorCode, preflight.errorMessage});
        }

        ParseManifestWorkerNoChangesRequest parseRequest;
        parseRequest.rawJson = finalText;
        // A no-op worker can attest only read-backed inspect_paths
        // requirements. Build/lint/consistency requirements remain
        // authoritative, but are evaluated later by the Backend combined
        // verifier and must not force the model to invent workspace paths
        // such as "build-log" merely to reach that verifier.
        for (const auto& requirementId : request.requiredRequirementIds) {
            if (!requiresWorkerInspectionEvidence(
                    requirementId, request.verificationKindsByRequirement)) {
                continue;
            }
            parseRequest.requiredRequirementIds.push_back(requirementId);
            const auto outputs =
                request.requiredOutputsByRequirement.find(requirementId);
            if (outputs != request.requiredOutputsByRequirement.end()) {
                parseRequest.requiredOutputsByRequirement.emplace(
                    requirementId, outputs->second);
            }
        }
        parseRequest.sameAttemptInspectedRelativePaths.assign(
            attemptInspectedPaths.begin(), attemptInspectedPaths.end());
        parseRequest.sameAttemptKnownMissingRelativePaths.assign(
            attemptKnownMissingPaths.begin(), attemptKnownMissingPaths.end());
        parseRequest.dependencyProposalContextPresent =
            request.dependencyProposalContextPresent;
        return parseManifestWorkerNoChanges(parseRequest);
    };

    auto workerNoChangesFailureIsRepairable = [](std::string_view code) {
        return code != "manifest.task_cancelled"
            && code != "run.tool_call_budget_exceeded"
            && code != "worker_no_changes.output_preflight_failed";
    };

    if (request.mode == AgentToolTurnLoopMode::Inspect) {
        auto evaluate = [&]() {
            if (interceptedTerminalTool != kSubmitTaskVerificationToolName) {
                return common::Result<ManifestTaskVerificationSubmission>::failure(
                    {"task_verification.missing_submission",
                     "Inspect worker stopped without submit_task_verification"});
            }
            return parseTaskVerification(
                finalText, request.requiredRequirementIds,
                request.requiredOutputsByRequirement,
                request.verificationKindsByRequirement,
                attemptInspectedPaths, attemptKnownMissingPaths);
        };
        auto verification = evaluate();
        if (!verification.success()) {
            const auto code = verification.error()->code;
            const auto message = verification.error()->message;
            if (runTypedTerminalRepair(code, message,
                    "Call submit_task_verification exactly once as the only terminal call.")) {
                verification = evaluate();
            }
        }
        if (verification.success()) {
            for (const auto& requiredId : request.requiredRequirementIds) {
                const bool passed = std::any_of(
                    verification.value().criterionEvidence.begin(),
                    verification.value().criterionEvidence.end(),
                    [&](const ManifestTaskVerificationEvidence& evidence) {
                        return evidence.requirementId == requiredId
                            && evidence.status == ExecutionVerificationStatus::Passed;
                    });
                const bool contradicted = std::any_of(
                    verification.value().criterionEvidence.begin(),
                    verification.value().criterionEvidence.end(),
                    [&](const ManifestTaskVerificationEvidence& evidence) {
                        return evidence.requirementId == requiredId
                            && evidence.status != ExecutionVerificationStatus::Passed;
                    });
                if (!passed || contradicted) {
                    verification = common::Result<ManifestTaskVerificationSubmission>::failure(
                        {"task_verification.required_not_passed",
                         "A required acceptance requirement failed or was not verifiable"});
                    break;
                }
            }
        }
        outcome.finalText = finalText;
        outcome.usage = totalUsage;
        outcome.repairAttempted = repairAttempted;
        fillEvidence(outcome);
        if (!verification.success()) {
            finishToolSession(AgentToolSessionState::Interrupted);
            outcome.errorCode = verification.error()->code;
            outcome.errorMessage = verification.error()->message;
            return outcome;
        }
        finishToolSession(AgentToolSessionState::Completed);
        outcome.success = true;
        outcome.taskVerification = std::move(verification.value());
        return outcome;
    }

    const auto proposalTerminalHasEmptyChanges = [&]() {
        if (interceptedTerminalTool != kProposeChangeSetToolName) return false;
        try {
            const auto object = nlohmann::json::parse(finalText);
            return object.is_object() && object.contains("changes")
                && object.at("changes").is_array() && object.at("changes").empty();
        } catch (const std::exception&) {
            return false;
        }
    };

    // A task that declares plannedCreatePaths has no valid no-op: a file that does not exist yet
    // can never be "already satisfied", and no evidence can make that true. The tool is not
    // advertised for such a task, but the loop does not rely on that - a typed no-change call, or
    // an empty propose_change_set standing in for one, is rejected here with a stable code, even
    // when an adapter or a scripted session returns it, and BEFORE anything is persisted. The
    // outer attempt (with its own reservation) decides whether the task is retried.
    const auto plannedCreateNoChangesFailure = [&]() {
        finishToolSession(AgentToolSessionState::Interrupted);
        outcome.finalText = finalText;
        outcome.usage = totalUsage;
        outcome.repairAttempted = repairAttempted;
        outcome.errorCode = kWorkerNoChangesPlannedCreateRequiredCode;
        outcome.errorMessage = interceptedTerminalTool == kSubmitTaskNoChangesToolName
            ? "This task must create the files named in its planned-create paths; a typed "
              "no-change submission cannot be accepted for it"
            : "This task must create the files named in its planned-create paths; an empty "
              "change set cannot be accepted for it";
        fillEvidence(outcome);
        return outcome;
    };
    const auto plannedCreateNoOpAttempted = [&] {
        return request.noChangesForbidden
            && (interceptedTerminalTool == kSubmitTaskNoChangesToolName
                || proposalTerminalHasEmptyChanges());
    };
    if (plannedCreateNoOpAttempted()) return plannedCreateNoChangesFailure();

    if (interceptedTerminalTool == kSubmitTaskNoChangesToolName) {
        auto noChanges = evaluateWorkerNoChanges();
        if (!noChanges.success()) {
            const auto code = noChanges.error()->code;
            const auto message = noChanges.error()->message;
            const std::string terminalInstruction =
                code == "worker_no_changes.required_output_missing"
                ? "A missing required output is concrete work. If it is inside the "
                  "authoritative owned paths, call propose_change_set with a create "
                  "operation and complete proposed_content. Do not submit no-changes "
                  "for missing files."
                : "Call submit_task_no_changes_needed with same-attempt evidence if the "
                  "task is already satisfied, or propose_change_set with concrete work.";
            if (workerNoChangesFailureIsRepairable(code)
                && runTypedTerminalRepair(code, message,
                    terminalInstruction)) {
                if (interceptedTerminalTool == kSubmitTaskNoChangesToolName)
                    noChanges = evaluateWorkerNoChanges();
            }
        }
        if (interceptedTerminalTool == kSubmitTaskNoChangesToolName) {
            outcome.finalText = finalText;
            outcome.usage = totalUsage;
            outcome.repairAttempted = repairAttempted;
            fillEvidence(outcome);
            if (!noChanges.success()) {
                finishToolSession(AgentToolSessionState::Interrupted);
                outcome.cancelled =
                    noChanges.error()->code == "manifest.task_cancelled";
                outcome.errorCode = noChanges.error()->code;
                outcome.errorMessage = noChanges.error()->message;
                return outcome;
            }
            finishToolSession(AgentToolSessionState::Completed);
            outcome.success = true;
            outcome.workerNoChanges = std::move(noChanges.value());
            return outcome;
        }
        // A valid repair may discover concrete work and switch to the
        // proposal terminal. It then continues through the ordinary proposal
        // parser/validator below; the repair budget remains consumed.
    }

    if (request.requireTypedNoChangesTerminal && proposalTerminalHasEmptyChanges()) {
        if (!repairAttempted) {
            static_cast<void>(runTypedTerminalRepair(
                "proposal.empty_changes_requires_no_changes_terminal",
                "An empty propose_change_set cannot represent an evidence-backed no-op",
                "Reinspect, then call submit_task_no_changes_needed with criterion evidence "
                "if already satisfied, or propose_change_set with concrete changes."));
        }
        if (interceptedTerminalTool == kSubmitTaskNoChangesToolName) {
            auto noChanges = evaluateWorkerNoChanges();
            outcome.finalText = finalText;
            outcome.usage = totalUsage;
            outcome.repairAttempted = repairAttempted;
            fillEvidence(outcome);
            if (noChanges.success()) {
                finishToolSession(AgentToolSessionState::Completed);
                outcome.success = true;
                outcome.workerNoChanges = std::move(noChanges.value());
            } else {
                finishToolSession(AgentToolSessionState::Interrupted);
                outcome.errorCode = noChanges.error()->code;
                outcome.errorMessage = noChanges.error()->message;
            }
            return outcome;
        }
        if (interceptedTerminalTool != kProposeChangeSetToolName
            || proposalTerminalHasEmptyChanges()) {
            finishToolSession(AgentToolSessionState::Interrupted);
            outcome.finalText = finalText;
            outcome.usage = totalUsage;
            outcome.repairAttempted = repairAttempted;
            outcome.errorCode = "proposal.empty_changes_requires_no_changes_terminal";
            outcome.errorMessage =
                "An empty change set is not a worker no-change submission";
            if (terminalForcedFinalCause.has_value()) {
                // The empty set was the Backend-forced LAST turn of an exhausted allocation, not a
                // voluntary "nothing to do": say that first and keep the empty-set code as a detail.
                outcome.errorCode = kWorkerTurnBudgetExhaustedCode;
                outcome.errorMessage = describeTurnBudgetExhaustion();
            }
            fillEvidence(outcome);
            return outcome;
        }
    }

    request.proposalTemplate.runId = request.runId;
    request.proposalTemplate.rawModelOutput = extractJsonFenceIfPresent(finalText);
    // Every proposal built from this turn loop is backed by this session's
    // own audited tool calls, never a free-form model-typed hash - see
    // ChangeProposalValidator::validate()'s doc comment. The model itself
    // never supplies or computes a hash (see preflightTrustedHashes above);
    // This call creates attempt-scoped evidence for every Modify/Delete/
    // Rename path. A later repair deliberately gets a fresh map and re-hashes
    // even an unchanged path, so evidence never crosses attempt boundaries.
    const WorkspaceReadPolicy& trustedHashPolicy = request.trustedHashPolicy.has_value()
        ? *request.trustedHashPolicy : request.policy;
    auto hashPreflight = preflightTrustedHashes(
        toolExecution_, toolSession, request.runId, trustedHashPolicy,
        request.proposalTemplate.rawModelOutput, stopToken,
        request.sessionRequest.maxTotalToolCalls, totalToolCalls,
        executedToolCalls, successfulInspectionCalls, totalReturnedBytesAccum);
    if (!hashPreflight.success) {
        finishToolSession(AgentToolSessionState::Interrupted);
        outcome.finalText = finalText;
        outcome.usage = totalUsage;
        outcome.cancelled = hashPreflight.cancelled;
        outcome.errorCode = hashPreflight.errorCode;
        outcome.errorMessage = hashPreflight.errorMessage;
        fillEvidence(outcome);
        return outcome;
    }
    request.proposalTemplate.requireHashEvidence = true;
    request.proposalTemplate.observedFileHashes =
        std::move(hashPreflight.observedFileHashes);

    auto proposal = submitProposal(changeProposals_, request);

    // A model whose final output isn't usable - either a hard parse-stage
    // failure (proposal.success() == false, most often because it wrote
    // prose instead of calling propose_change_set - it wrongly concludes its
    // read-only TOOLS mean it cannot propose changes at all, see
    // kWorkspaceToolsSystemPrompt) or a validation-stage Rejected proposal
    // (proposal still succeeds - createFromModelOutput() persists Rejected
    // proposals rather than failing - but every one of its findings is a
    // shape/belief mistake the model itself can fix) - fails
    // deterministically. Retrying with the *same* prompt would just repeat
    // the mistake, but the model has never actually seen its own error: give
    // it exactly one chance to see the failure and correct course. This is a
    // reliability/UX measure (LLM instruction-following is not 100%
    // guaranteed even with an explicit system prompt/schema) - it never
    // touches the parser or the validator. A Rejected proposal is only
    // eligible when EVERY finding is one isModelFormatError() admits; a
    // single non-repairable finding reflecting repository state or another
    // agent's concurrent work (secret findings, hash mismatches, other policy
    // rejections, proposal.cross_agent_conflict) means re-prompting cannot
    // fully fix this proposal, so it is left Rejected.
    std::string repairCode;
    std::string repairMessage;
    bool eligibleForRepair = false;
    // Package 3 (MANIFEST_EXECUTION_CORRECTNESS_FIX_PLAN.md): whether the
    // *original* Rejected proposal - before any repair - already contained
    // concrete file changes. A worker that abandons real rejected work and
    // returns an empty NoChangesNeeded from its one-shot repair must never
    // be allowed to launder that as success; see the abandonment check below.
    bool originalRejectedHadConcreteChanges = false;
    // Every finding the repair prompt must show the model, in stable
    // ordinal order - not just the first, even though repairCode/
    // repairMessage (the first finding) still selects which canned
    // correction-instruction paragraph to render below.
    std::vector<std::pair<std::string, std::string>> repairFindings;
    if (!proposal.success()) {
        eligibleForRepair = isModelFormatError(proposal.error()->code);
        repairCode = proposal.error()->code;
        repairMessage = proposal.error()->message;
        if (eligibleForRepair) repairFindings.emplace_back(repairCode, repairMessage);
    } else if (proposal.value().state == ChangeSetState::Rejected) {
        const auto& findings = proposal.value().validationFindings;
        eligibleForRepair = !findings.empty()
            && std::all_of(findings.begin(), findings.end(),
                   [](const ValidationFinding& f) { return isModelFormatError(f.code); });
        originalRejectedHadConcreteChanges = !proposal.value().changes.empty();
        if (eligibleForRepair) {
            repairCode = findings.front().code;
            repairMessage = findings.front().message;
            for (const auto& finding : findings) repairFindings.emplace_back(finding.code, finding.message);
        }
    } else if (proposal.value().state == ChangeSetState::NoChangesNeeded
            && (successfulInspectionCalls == 0
                || noChangesSummaryContradictsOutcome(proposal.value().summary))) {
        // The real incident this guards against (Run
        // f5058a12-746a-4ab6-8deb-ca5bfc5500ca, agents 3/4): a workspace-
        // enabled agent that never called a single read-only tool still
        // returned an empty changes[], which the caller cannot distinguish
        // from a genuine "I looked and there is nothing to do". An
        // unexamined empty answer is not proven evidence - give the model
        // one chance to actually inspect the workspace before failing.
        eligibleForRepair = true;
        if (successfulInspectionCalls == 0) {
            repairCode = "proposal.no_changes_missing_evidence";
            repairMessage =
                "No workspace inspection tool completed successfully before "
                "concluding no changes are needed";
            // The row above is already durably persisted as a plain
            // NoChangesNeeded change set indistinguishable from a genuinely-
            // inspected one. Annotate it before the repair retry runs so it
            // remains an honest record even if the repair session cannot start.
            // Best-effort: a persistence hiccup must not block the repair.
            static_cast<void>(changeProposals_.markMissingEvidence(
                proposal.value().changeSetId, repairCode, repairMessage));
        } else {
            repairCode = "proposal.no_changes_contradicts_summary";
            repairMessage =
                "The summary says required file changes are still missing, were not "
                "performed, or should be deferred, but changes[] is empty";
        }
        repairFindings.emplace_back(repairCode, repairMessage);
    } else if ((proposal.value().state == ChangeSetState::ReadyForReview
                   || proposal.value().state == ChangeSetState::NoChangesNeeded)
            && !missingRequiredReadKeys(request, attemptInspectedPaths,
                   attemptExpandedRequiredReadKeys).empty()) {
        // AG-15: same-attempt required-read gate. A proposal that would
        // otherwise be accepted must also prove every one of this task's
        // effective requiredReadPaths (frozen scope + any RequiredRead
        // expansions approved in THIS attempt) was actually read in this
        // same session - never inherited from the primary attempt, another
        // task, a previous Run, or an unsuccessful inspection (see
        // attemptInspectedPaths' own per-attempt reset in runTurnLoop). This
        // is exactly the same kind of model-fixable mistake as the other
        // branches above: give the model one bounded repair to go read what
        // it missed and resubmit, rather than fail outright on the first try.
        eligibleForRepair = true;
        const auto missing = missingRequiredReadKeys(
            request, attemptInspectedPaths, attemptExpandedRequiredReadKeys);
        std::ostringstream missingList;
        missingList << "The following required-read path(s) were not successfully "
                       "inspected in this session before propose_change_set was "
                       "called: ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) missingList << ", ";
            missingList << missing[i];
        }
        repairCode = "workspace_scope.required_read_not_inspected";
        repairMessage = missingList.str();
        repairFindings.emplace_back(repairCode, repairMessage);
    } else if ((proposal.value().state == ChangeSetState::ReadyForReview
                   || proposal.value().state == ChangeSetState::NoChangesNeeded)
            && !missingDiagnosticReadKeys(
                   attemptInspectedPaths, attemptExpandedDiagnosticReadKeys).empty()) {
        // Verification-repair reliability hardening: same-attempt
        // diagnostic-read inspection gate. Every request_diagnostic_read
        // grant approved in THIS attempt must have actually been read in
        // this same session before a proposal is accepted - a grant that
        // was requested but never read does not count as evidence that the
        // authoritative declaration was actually consulted before guessing
        // a replacement. Same bounded one-shot recovery as the required-read
        // gate above.
        eligibleForRepair = true;
        const auto missing = missingDiagnosticReadKeys(
            attemptInspectedPaths, attemptExpandedDiagnosticReadKeys);
        std::ostringstream missingList;
        missingList << "The following diagnostic-read path(s) were granted but not "
                       "successfully inspected in this session before propose_change_set "
                       "was called: ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) missingList << ", ";
            missingList << missing[i];
        }
        repairCode = "workspace_scope.diagnostic_read_not_inspected";
        repairMessage = missingList.str();
        repairFindings.emplace_back(repairCode, repairMessage);
    }

    // A first attempt that was actually persisted (Rejected or
    // NoChangesNeeded above - not the !proposal.success() hard parse
    // failure, which persists nothing to link from) gets superseded by the
    // repair's own row below, so "persist every proposal attempt... link
    // repaired/superseded attempts" holds even though only the final
    // attempt is ever reported as this call's outcome.
    if (eligibleForRepair && !repairAttempted && proposal.success()) {
        request.proposalTemplate.supersedesChangeSetId = proposal.value().changeSetId;
    }

    // Package 3: whether the repair session actually ran - stamped onto
    // every outcome this function returns from this point on so a caller
    // never mistakes "repair already consumed" for "worth an outer retry".
    if (eligibleForRepair && !repairAttempted) {
        std::ostringstream repairStream;
        // Package 2 (MANIFEST_EXECUTION_CORRECTNESS_FIX_PLAN.md): a repair
        // session has no memory of the primary attempt, so this prompt must
        // stand entirely on its own. Fixed order: (1) state this is a fresh
        // session, (2) the complete original authoritative allocation
        // verbatim (Package 1's owned-path envelope plus objective/
        // acceptance/context/dependencies - never reconstructed from prose),
        // (3) every validation finding, not only the first, (4) the previous
        // attempt inside a clearly delimited untrusted block, (5) specific
        // correction instructions, (6) the terminal tool-call requirement.
        repairStream
            << "This is a fresh session with no memory of your previous attempt at this "
               "task. The complete authoritative task allocation is repeated below "
               "verbatim - it is unchanged from your previous attempt.\n\n"
            << request.sessionRequest.userContent << "\n\n"
            << "# Validation findings from your previous attempt\n";
        for (const auto& [findingCode, findingMessage] : repairFindings) {
            repairStream << "- " << findingCode << ": " << findingMessage << "\n";
        }
        repairStream
            << "\n# Previous attempt output (untrusted historical reference only)\n"
               "---BEGIN PREVIOUS ATTEMPT (UNTRUSTED) ---\n"
            << finalText
            << "\n---END PREVIOUS ATTEMPT (UNTRUSTED)---\n\n";
        if (repairCode == "proposal.missing_proposed_content") {
            repairStream
                << "Your propose_change_set call was otherwise correct, but a create/modify "
                   "change was missing its \"proposed_content\" field. That field must always "
                   "carry the complete new file text for a create or modify change - never "
                   "omitted, never null, never a diff - unless you are deliberately proposing "
                   "an empty marker file, in which case use an explicit empty string.\n\n";
        } else if (repairCode == "proposal.create_target_exists"
                || repairCode == "proposal.file_not_found"
                || repairCode == "proposal.rename_target_exists") {
            repairStream
                << "Your propose_change_set call was otherwise correct, but at least one "
                   "change's assumption about the current file system was wrong (e.g. you "
                   "proposed creating a file that already exists, or modifying/deleting/"
                   "renaming a file that does not exist at that path). Re-check the actual "
                   "current state using your read-only tools (list/read/stat), then resubmit "
                   "propose_change_set with the correct operation (e.g. modify instead of "
                   "create) and content for that specific change - leave every other "
                   "already-correct change exactly as it was.\n\n";
        } else if (repairCode == "proposal.outside_allowed_roots") {
            repairStream
                << "Your change set included at least one path outside this agent's "
                   "Architect-owned boundary. Re-read the authoritative Current Task "
                   "allocation in the original request, remove every create/modify/delete/"
                   "rename outside its Owned paths, and resubmit only this agent's own "
                   "changes. You may inspect other paths for context, but you must never "
                   "include them in propose_change_set.\n\n";
        } else if (repairCode == "proposal.field_too_large") {
            repairStream
                << "At least one bounded proposal string exceeded the Backend limit. "
                   "Shorten the summary/change_id/path as applicable, or split oversized "
                   "file content into work that stays within the advertised contract, "
                   "then resubmit without changing unrelated valid entries.\n\n";
        } else if (repairCode == "proposal.invariant_violated"
                || repairCode == "proposal.invalid_field_value") {
            repairStream
                << "Your propose_change_set call included at least one change whose "
                   "source_path/proposed_content values did not match its own "
                   "operation. Exactly this per operation, with no exceptions: create "
                   "requires proposed_content to be a real string (an empty string is "
                   "a valid empty marker file) and source_path to be null; modify "
                   "requires proposed_content to be a real string and source_path to "
                   "be null; delete requires both proposed_content and source_path to "
                   "be null; rename requires source_path to be the file's original "
                   "workspace-relative location and proposed_content to be null. You "
                   "never include a hash of any kind anywhere in this submission - "
                   "there is no hash field in this contract. Correct every violating "
                   "change to match this exactly, then resubmit propose_change_set - "
                   "leave every other already-correct change exactly as it was.\n\n";
        } else if (repairCode == "proposal.unexpected_field"
                || repairCode == "proposal.unsupported_schema") {
            repairStream
                << "Your propose_change_set call included a field this contract does not "
                   "recognize, or used the wrong schemaVersion. The only top-level fields "
                   "are schemaVersion (must be exactly \"proposal-submission.v2\"), summary, "
                   "and changes; each change has exactly change_id, operation, relative_path, "
                   "source_path, and proposed_content - nothing else. In particular, there is "
                   "no hash field of any kind in this contract - never include "
                   "expected_original_hash or anything similar, even for a create, and never "
                   "invent a hash of empty content for a new file; you never compute or supply "
                   "a hash yourself. Resubmit propose_change_set with exactly this field set, "
                   "leaving every other already-correct change exactly as it was.\n\n";
        } else if (repairCode == "proposal.missing_hash_evidence") {
            repairStream
                << "The Backend could not independently confirm the current content of "
                   "at least one Modify, Delete, or Rename target (the rename source "
                   "for a Rename) before validating your proposal - most often because "
                   "that path does not actually exist there, or your assumption about "
                   "which operation applies to it was wrong. You never supply or "
                   "compute this hash yourself; instead, re-check the actual current "
                   "state of that path with your read-only tools (list/read/stat), "
                   "correct the operation or relative_path if your belief about it was "
                   "wrong, and resubmit propose_change_set - leave every other "
                   "already-correct change exactly as it was.\n\n";
        } else if (repairCode == "proposal.no_changes_missing_evidence") {
            repairStream
                << "You submitted an empty changes[] (no changes needed) without successfully "
                   "completing any read-only workspace tool (list/read/search/hash/stat) first. An "
                   "empty result is only meaningful after you have actually inspected the "
                   "files or directories this task is about. Use your workspace tools now "
                   "to inspect them, then call propose_change_set again - with concrete "
                   "changes if you find something to change, or again with an empty "
                   "changes[] only if your inspection genuinely confirms nothing needs to "
                   "change.\n\n";
        } else if (repairCode == "proposal.no_changes_contradicts_summary") {
            repairStream
                << "You inspected the workspace and explicitly reported that required work "
                   "is still missing or must be done later, but submitted an empty changes[]. "
                   "Do not defer this to another apply pass: propose every required create or "
                   "modify operation now, including complete proposed_content. Use an empty "
                   "changes[] only when the task is already fully satisfied.\n\n";
        } else if (repairCode == "workspace_scope.required_read_not_inspected") {
            repairStream
                << "Your propose_change_set call was otherwise correct, but at least one "
                   "path this task declares as required-read (named above in the finding) "
                   "was never successfully read in this session before you proposed changes. "
                   "This is a fresh session, so nothing from any earlier attempt counts - use "
                   "your read-only tools now to actually read every required-read path named "
                   "above, then resubmit propose_change_set with the same (or corrected) "
                   "changes. If you believe a named path is genuinely irrelevant to this task, "
                   "you must still read it before proceeding; if you discover a real "
                   "additional dependency not already listed, use request_scope_expansion "
                   "before resubmitting.\n\n";
        } else if (repairCode == "workspace_scope.diagnostic_read_not_inspected") {
            repairStream
                << "Your propose_change_set call was otherwise correct, but you requested "
                   "diagnostic read access to at least one path (named above in the "
                   "finding) via request_diagnostic_read and never actually read it in "
                   "this session before proposing changes. Requesting the grant is not "
                   "evidence by itself - use your read-only tools now to actually read "
                   "every diagnostic-read path named above, confirm your proposed change "
                   "matches what that authoritative declaration actually contains, then "
                   "resubmit propose_change_set.\n\n";
        } else {
            repairStream
                << "This almost always means you wrote prose (an explanation, an apology, "
                   "or instructions telling the user to run a command themselves) instead "
                   "of calling the propose_change_set tool. That is never correct output, "
                   "even if you believe you are unable to make the change yourself - "
                   "calling propose_change_set IS how you make it; never tell the user to "
                   "run a command.\n\n";
        }
        repairStream
            << "Call the propose_change_set tool now, as your terminal action, with "
               "corrected \"summary\" and \"changes\" arguments - do not write a text "
               "message instead.";

        auto repairSessionResult =
            sessionFactory_.createSession(request.sessionRequest.providerId, request.providerKind);
        if (repairSessionResult.success()) {
            // The repair budget is spent from this point on regardless of
            // how the repair itself concludes (success, still-Rejected, or a
            // hard loop failure below) - a caller must never schedule
            // another outer attempt on top of an already-consumed repair.
            repairAttempted = true;
            const auto repairSession = std::move(repairSessionResult.value());
            providers::ProviderAgentSessionRequest repairRequest = request.sessionRequest;
            repairRequest.userContent = repairStream.str();
            enterRepairSessionBudget();

            loopFailed = false;
            loopFailureOutcome = AgentToolTurnLoopOutcome {};
            auto repairedTextOpt = runTurnLoop(
                *repairSession, budgetedBegin(*repairSession, repairRequest));
            if (!repairedTextOpt) {
                finishToolSession(AgentToolSessionState::Interrupted);
                loopFailureOutcome.success = false;
                loopFailureOutcome.usage = totalUsage;
                loopFailureOutcome.repairAttempted = repairAttempted;
                fillEvidence(loopFailureOutcome);
                return loopFailureOutcome;
            }
            finalText = std::move(*repairedTextOpt);
            if (plannedCreateNoOpAttempted()) return plannedCreateNoChangesFailure();
            if (interceptedTerminalTool == kSubmitTaskNoChangesToolName) {
                outcome.finalText = finalText;
                outcome.usage = totalUsage;
                outcome.repairAttempted = repairAttempted;
                fillEvidence(outcome);
                if (originalRejectedHadConcreteChanges) {
                    finishToolSession(AgentToolSessionState::Interrupted);
                    outcome.errorCode = "proposal.repair_abandoned_changes";
                    outcome.errorMessage =
                        "A repair cannot abandon concrete rejected work through a no-change submission";
                    return outcome;
                }
                auto noChanges = evaluateWorkerNoChanges();
                if (!noChanges.success()) {
                    finishToolSession(AgentToolSessionState::Interrupted);
                    outcome.cancelled =
                        noChanges.error()->code == "manifest.task_cancelled";
                    outcome.errorCode = noChanges.error()->code;
                    outcome.errorMessage = noChanges.error()->message;
                    return outcome;
                }
                finishToolSession(AgentToolSessionState::Completed);
                outcome.success = true;
                outcome.workerNoChanges = std::move(noChanges.value());
                return outcome;
            }
            request.proposalTemplate.rawModelOutput = extractJsonFenceIfPresent(finalText);
            // Re-preflight against the repair's own complete submission. The
            // returned map is fresh even when it names the same path as the
            // primary attempt, preventing stale evidence reuse.
            hashPreflight = preflightTrustedHashes(
                toolExecution_, toolSession, request.runId, trustedHashPolicy,
                request.proposalTemplate.rawModelOutput, stopToken,
                request.sessionRequest.maxTotalToolCalls, totalToolCalls,
                executedToolCalls, successfulInspectionCalls,
                totalReturnedBytesAccum);
            if (!hashPreflight.success) {
                finishToolSession(AgentToolSessionState::Interrupted);
                outcome.finalText = finalText;
                outcome.usage = totalUsage;
                outcome.repairAttempted = repairAttempted;
                outcome.cancelled = hashPreflight.cancelled;
                outcome.errorCode = hashPreflight.errorCode;
                outcome.errorMessage = hashPreflight.errorMessage;
                fillEvidence(outcome);
                return outcome;
            }
            request.proposalTemplate.observedFileHashes =
                std::move(hashPreflight.observedFileHashes);
            proposal = submitProposal(changeProposals_, request);
        }
        // If the repair session itself couldn't even be created, fall
        // through: the original failure/rejection below is the real outcome
        // and must not be hidden behind a session-creation error instead.
    }

    outcome.finalText = finalText;
    outcome.usage = totalUsage;
    outcome.repairAttempted = repairAttempted;
    if (repairAttempted && !primaryAttemptFinalText.empty()
        && primaryAttemptFinalText != finalText) {
        outcome.primaryAttemptText = primaryAttemptFinalText.substr(
            0, kMaximumRetainedAttemptTextBytes);
    }
    fillEvidence(outcome);

    if (!proposal.success()) {
        finishToolSession(AgentToolSessionState::Interrupted);
        outcome.success = false;
        outcome.errorCode = proposal.error()->code;
        outcome.errorMessage = proposal.error()->message;
        if (outcome.errorCode == "proposal.parse_error") {
            // The public parser message is deliberately generic (it must not
            // echo untrusted payload). Add only numeric facts about the
            // terminal provider response(s) so an empty or prose-only Stop is
            // distinguishable from malformed JSON without retaining content.
            const auto describe = [](const TerminalShape& shape) {
                std::string text = shape.viaTool
                    ? "terminal tool call, " + std::to_string(shape.textBytes)
                        + " argument byte(s), "
                    : "Stop without a terminal tool call, " + std::to_string(shape.textBytes)
                        + " text byte(s), ";
                text += std::to_string(shape.outputTokens) + " output token(s)";
                return text;
            };
            if (lastTerminalShape.recorded) {
                outcome.errorMessage += " (";
                if (repairAttempted && primaryTerminalShape.recorded) {
                    outcome.errorMessage += "primary attempt: " + describe(primaryTerminalShape)
                        + "; repair attempt: " + describe(lastTerminalShape);
                } else {
                    outcome.errorMessage += "terminal response: " + describe(lastTerminalShape);
                }
                outcome.errorMessage += ")";
            }
        }
        return outcome;
    }

    // outcome.proposal is set here regardless of state (including Rejected -
    // see AgentToolTurnLoopOutcome's doc comment): a caller building
    // downstream-agent context still needs to see what was actually rejected
    // and why. Only ReadyForReview, and NoChangesNeeded backed by at least
    // one successful inspection call, count as a successful task outcome - a parsed-but-
    // Rejected proposal (whatever the repair attempt above did or did not
    // fix) or an unexamined empty answer must never be reported as success,
    // regardless of createFromModelOutput() itself having "succeeded" in the
    // sense of durably persisting the record.
    outcome.proposal = proposal.value();

    // Backstop for plannedCreatePaths: whatever path produced it, an accepted no-change outcome
    // is never a valid result for a task that must create files.
    if (request.noChangesForbidden
            && proposal.value().state == ChangeSetState::NoChangesNeeded) {
        return plannedCreateNoChangesFailure();
    }

    // Review follow-up to Package 3: a durably-persisted, non-empty Rejected
    // proposal must block an outer bounded retry regardless of whether
    // internal repair ever ran - repairAttempted alone is not a safe retry
    // gate. A finding outside isModelFormatError() (proposal.cross_agent_conflict,
    // for example) never makes it eligible for repair in the first place, so
    // repairAttempted stays false, yet the same real, concrete rejected work
    // exists and must not be silently retried away by a fresh outer attempt
    // whose own unrelated empty NoChangesNeeded would otherwise turn the
    // task green. Computed from whichever proposal is FINAL here - the
    // primary attempt when repair never ran, or the repaired one when it did
    // and still ended Rejected with real changes.
    outcome.nonEmptyRejected = proposal.value().state == ChangeSetState::Rejected
        && !proposal.value().changes.empty();

    // Package 3: a non-empty Rejected proposal proved the worker found
    // concrete work to do. If its one-shot repair abandons that work and
    // comes back with an empty NoChangesNeeded instead of a corrected
    // change set, that is not evidence the task is actually satisfied - it
    // is the worker giving up on rejected changes. This must never fall
    // through to the ordinary NoChangesNeeded success path below, even when
    // the repair session did perform real inspection.
    if (repairAttempted && originalRejectedHadConcreteChanges
            && proposal.value().state == ChangeSetState::NoChangesNeeded
            && proposal.value().changes.empty()) {
        finishToolSession(AgentToolSessionState::Interrupted);
        outcome.success = false;
        outcome.errorCode = "proposal.repair_abandoned_changes";
        outcome.errorMessage =
            "The original proposal was rejected while containing concrete file changes, but "
            "the repair attempt abandoned them and returned an empty change set instead of "
            "correcting the rejected changes";
        return outcome;
    }

    // AG-15: final same-attempt required-read gate, re-checked once more
    // after any repair - a repair session has its own fresh
    // attemptInspectedPaths/attemptExpandedRequiredReadKeys (reset at the
    // top of runTurnLoop) and might still not have read everything
    // required. No further repair is scheduled from here: the one-shot
    // repair budget above is already spent whenever eligibleForRepair was
    // ever true for this reason, so a proposal that still fails this gate
    // is a hard, terminal failure rather than a silently-accepted proposal.
    if (proposal.value().state == ChangeSetState::ReadyForReview
            || proposal.value().state == ChangeSetState::NoChangesNeeded) {
        const auto stillMissing = missingRequiredReadKeys(
            request, attemptInspectedPaths, attemptExpandedRequiredReadKeys);
        if (!stillMissing.empty()) {
            finishToolSession(AgentToolSessionState::Interrupted);
            outcome.success = false;
            outcome.errorCode = "workspace_scope.required_read_not_inspected";
            std::ostringstream missingList;
            missingList << "The following required-read path(s) were not successfully "
                           "inspected in this session before the proposal was accepted: ";
            for (std::size_t i = 0; i < stillMissing.size(); ++i) {
                if (i != 0) missingList << ", ";
                missingList << stillMissing[i];
            }
            outcome.errorMessage = missingList.str();
            return outcome;
        }
        // Verification-repair reliability hardening: final same-attempt
        // diagnostic-read inspection gate, mirroring the required-read gate
        // immediately above - re-checked once more after any repair, with
        // no further repair scheduled from here (this gate's own one-shot
        // repair budget, if it ever fired, is already spent).
        const auto stillMissingDiagnosticReads = missingDiagnosticReadKeys(
            attemptInspectedPaths, attemptExpandedDiagnosticReadKeys);
        if (!stillMissingDiagnosticReads.empty()) {
            finishToolSession(AgentToolSessionState::Interrupted);
            outcome.success = false;
            outcome.errorCode = "workspace_scope.diagnostic_read_not_inspected";
            std::ostringstream missingList;
            missingList << "The following diagnostic-read path(s) were granted but not "
                           "successfully inspected in this session before the proposal was "
                           "accepted: ";
            for (std::size_t i = 0; i < stillMissingDiagnosticReads.size(); ++i) {
                if (i != 0) missingList << ", ";
                missingList << stillMissingDiagnosticReads[i];
            }
            outcome.errorMessage = missingList.str();
            return outcome;
        }
    }

    switch (proposal.value().state) {
    case ChangeSetState::ReadyForReview:
        finishToolSession(AgentToolSessionState::Completed);
        outcome.success = true;
        return outcome;
    case ChangeSetState::NoChangesNeeded:
        if (noChangesSummaryContradictsOutcome(proposal.value().summary)) {
            finishToolSession(AgentToolSessionState::Interrupted);
            outcome.success = false;
            outcome.errorCode = "proposal.no_changes_contradicts_summary";
            outcome.errorMessage =
                "No file changes were proposed, but the summary explicitly says required "
                "work is missing, was not performed, or was deferred to another pass";
            return outcome;
        }
        if (successfulInspectionCalls == 0) {
            finishToolSession(AgentToolSessionState::Interrupted);
            outcome.success = false;
            outcome.errorCode = "proposal.no_changes_missing_evidence";
            outcome.errorMessage =
                "No file changes were proposed, but no workspace inspection tool "
                "completed successfully in this session - an empty changes[] is not "
                "a proven result without evidence the workspace was actually inspected";
            return outcome;
        }
        finishToolSession(AgentToolSessionState::Completed);
        outcome.success = true;
        return outcome;
    case ChangeSetState::Rejected:
    default: {
        // Received/Validated/Applied/ApplyFailed cannot come back from
        // createFromModelOutput() at this point; Rejected is the only other
        // terminal state it actually produces, handled the same way as an
        // unrecognized future state would be: fail closed rather than assume
        // success.
        finishToolSession(AgentToolSessionState::Interrupted);
        outcome.success = false;
        const auto& findings = proposal.value().validationFindings;
        outcome.errorCode = findings.empty() ? "proposal.rejected" : findings.front().code;
        outcome.errorMessage = findings.empty()
            ? "Change set was rejected during validation"
            : findings.front().message;
        return outcome;
    }
    }
}

} // namespace ai::application
