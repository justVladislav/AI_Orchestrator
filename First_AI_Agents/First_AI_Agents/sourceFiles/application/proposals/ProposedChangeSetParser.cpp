#include "ai/application/proposals/ProposedChangeSetParser.hpp"

#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <unordered_set>

namespace ai::application {

// The provider-facing terminal payload version is declared with all other
// shared bounds in ProposedChangeSetParser.hpp. expected_original_hash is
// deliberately absent from this contract: the Backend computes every
// Modify/Delete/Rename precondition hash itself (AgentToolTurnLoopRunner's
// preflightTrustedHashes, through the same audited workspace_hash_file path
// a model's own call would use) rather than trusting a model-supplied or
// model-computed value. A real Run showed why a nullable-but-model-suppliable
// hash field was not enough on its own: the model independently reasoned
// "a new file has no prior content, so I will hash empty content" and
// resubmitted the exact same sha256("") verbatim even after an explicit
// bounded-repair correction - the wire contract itself must make the field
// impossible to supply, not just optional.
namespace {

using json = nlohmann::json;

common::Result<ProposedChangeSet> fail(std::string code, std::string msg)
{
    return common::Result<ProposedChangeSet>::failure({std::move(code), std::move(msg)});
}

// Backend-independent exact-field-set enforcement, applied regardless of
// whether the provider's own strict schema (see kProposeChangeSetInputSchema)
// actually constrained the model's output - the provider-side schema is a
// reliability aid, never the trust boundary. Never echoes an unexpected
// field's own name (only which object it was found on), so a model/provider-
// controlled key can never reach a stored or displayed diagnostic.
common::Result<void> requireExactFields(
    const json& obj,
    const char* location,
    std::initializer_list<const char*> fields)
{
    if (!obj.is_object()) {
        return common::Result<void>::failure(
            {"proposal.invalid_field_type",
             std::string(location) + " must be an object"});
    }
    std::unordered_set<std::string> allowed(fields.begin(), fields.end());
    for (const auto& [key, unused] : obj.items()) {
        static_cast<void>(unused);
        if (!allowed.contains(key)) {
            return common::Result<void>::failure(
                {"proposal.unexpected_field",
                 std::string(location) + " contains an unexpected field"});
        }
    }
    for (const auto* field : fields) {
        if (!obj.contains(field)) {
            return common::Result<void>::failure(
                {"proposal.missing_field",
                 std::string(location) + " is missing required field: " + field});
        }
    }
    return common::Result<void>::ok();
}

// Extract a single ```json ... ``` fence from the raw output.
// Returns empty optional on clean raw-JSON input (no fence markers detected).
// Returns error if there are extra fences or extra content around the fence.
common::Result<std::optional<std::string_view>> extractJsonFence(std::string_view raw)
{
    // Count occurrences of the opening fence marker
    std::string_view openFence = "```json";
    // The closing marker must start its own line, matching how the fence is
    // always emitted. Matching a bare "```" would also match one appearing
    // mid-sentence inside a JSON string value (e.g. the model's own prose
    // mentioning "a ```json fence"), which ends the search right there and
    // truncates otherwise well-formed JSON into a bogus parse failure.
    std::string_view closeFence = "\n```";

    const auto firstOpen = raw.find(openFence);
    if (firstOpen == std::string_view::npos) {
        // No fence at all — treat as raw JSON.
        return common::Result<std::optional<std::string_view>>::ok(std::nullopt);
    }

    // There is at least one fence marker.  Validate that it is the ONLY one
    // and that there is no content outside the fence.
    const auto contentStart = firstOpen + openFence.size();
    // Skip optional newline after opening fence.
    const std::string_view remaining = raw.substr(contentStart);
    const auto closePos = remaining.find(closeFence);
    if (closePos == std::string_view::npos) {
        return common::Result<std::optional<std::string_view>>::failure(
            {"proposal.parse_error", "Unterminated ```json fence"});
    }

    const std::string_view fenceContent = remaining.substr(0, closePos);
    const std::string_view afterClose = remaining.substr(closePos + closeFence.size());

    // Verify no second fence
    if (afterClose.find(openFence) != std::string_view::npos) {
        return common::Result<std::optional<std::string_view>>::failure(
            {"proposal.multiple_fences", "More than one ```json fence found in model output"});
    }

    // Verify nothing meaningful before the first fence
    auto isWhitespace = [](std::string_view sv) {
        for (char c : sv) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
        }
        return true;
    };

    const std::string_view beforeFence = raw.substr(0, firstOpen);
    if (!isWhitespace(beforeFence)) {
        return common::Result<std::optional<std::string_view>>::failure(
            {"proposal.extra_content", "Content found before ```json fence"});
    }
    if (!isWhitespace(afterClose)) {
        return common::Result<std::optional<std::string_view>>::failure(
            {"proposal.extra_content", "Content found after closing ``` of json fence"});
    }

    return common::Result<std::optional<std::string_view>>::ok(fenceContent);
}

// Require a JSON field of a given type; return error if missing or wrong type.
template<typename Fn>
common::Result<void> requireField(
    const json& obj,
    const char* field,
    const char* expectedType,
    Fn check)
{
    if (!obj.contains(field)) {
        return common::Result<void>::failure(
            {"proposal.missing_field", std::string("Required field missing: ") + field});
    }
    if (!check(obj[field])) {
        return common::Result<void>::failure(
            {"proposal.invalid_field_type",
             std::string("Field '") + field + "' must be " + expectedType});
    }
    return common::Result<void>::ok();
}

// Retrieve a required string field.
common::Result<std::string> getString(const json& obj, const char* field)
{
    const auto check = requireField(obj, field, "string", [](const json& v) { return v.is_string(); });
    if (!check.success()) return common::Result<std::string>::failure(*check.error());
    return common::Result<std::string>::ok(obj[field].get<std::string>());
}

// Retrieve an optional string field without silently coercing malformed
// provider output into an absent value.
common::Result<std::optional<std::string>> getOptionalString(
    const json& obj,
    const char* field)
{
    if (!obj.contains(field) || obj[field].is_null()) {
        return common::Result<std::optional<std::string>>::ok(std::nullopt);
    }
    if (!obj[field].is_string()) {
        return common::Result<std::optional<std::string>>::failure(
            {"proposal.invalid_field_type",
             std::string("Field '") + field + "' must be a string or null"});
    }
    return common::Result<std::optional<std::string>>::ok(
        obj[field].get<std::string>());
}

common::Result<ProposedFileChange> parseChange(const json& entry, std::size_t idx)
{
    auto makeError = [&](std::string code, std::string msg) {
        return common::Result<ProposedFileChange>::failure(
            {std::move(code), "changes[" + std::to_string(idx) + "]: " + std::move(msg)});
    };

    const std::string location = "changes[" + std::to_string(idx) + "]";
    // Exactly these five fields, never expected_original_hash: a model/
    // provider that still sends one (the old contract's field) is rejected
    // here as an unexpected field, before any of its other content is even
    // read - see kProposalSubmissionSchemaVersion's doc comment above.
    const auto fields = requireExactFields(entry, location.c_str(),
        {"change_id", "operation", "relative_path", "source_path", "proposed_content"});
    if (!fields.success()) {
        return common::Result<ProposedFileChange>::failure(*fields.error());
    }

    // change_id
    if (!entry.contains("change_id") || !entry["change_id"].is_string()) {
        return makeError("proposal.missing_field", "change_id required and must be a string");
    }
    std::string changeId = entry["change_id"].get<std::string>();
    if (changeId.empty()) {
        return makeError("proposal.empty_change_id", "change_id must not be empty");
    }
    if (changeId.size() > kProposalSubmissionMaxChangeIdBytes) {
        return makeError("proposal.field_too_large", "change_id exceeds the Backend byte limit");
    }

    // operation
    if (!entry.contains("operation") || !entry["operation"].is_string()) {
        return makeError("proposal.missing_field", "operation required and must be a string");
    }
    const std::string opStr = entry["operation"].get<std::string>();
    FileChangeOperation op;
    try {
        op = fileChangeOperationFromString(opStr);
    } catch (...) {
        return makeError("proposal.invalid_operation",
                         "Unknown file operation value");
    }

    // relative_path
    if (!entry.contains("relative_path") || !entry["relative_path"].is_string()) {
        return makeError("proposal.missing_field", "relative_path required and must be a string");
    }
    const std::string relPathStr = entry["relative_path"].get<std::string>();
    if (relPathStr.empty()) {
        return makeError("proposal.missing_field", "relative_path must not be empty");
    }
    if (relPathStr.size() > kProposalSubmissionMaxPathBytes) {
        return makeError("proposal.field_too_large", "relative_path exceeds the Backend byte limit");
    }

    ProposedFileChange change;
    change.changeId   = std::move(changeId);
    change.operation  = op;
    // relative_path/source_path are UTF-8 text straight from the model's
    // JSON output - detail::pathFromUtf8 is the one std::filesystem::path
    // constructor the standard guarantees is UTF-8 regardless of the host
    // locale/codepage; the plain std::string constructor is not (see its
    // doc comment).
    change.relativePath = detail::pathFromUtf8(relPathStr);

    // source_path follows the v2 wire contract exactly: Rename supplies one
    // non-empty string; every other operation supplies JSON null. Do not
    // launder a strict-invalid empty-string placeholder into trusted absence.
    change.sourcePath = std::nullopt;
    if (entry.contains("source_path") && !entry["source_path"].is_null()) {
        if (!entry["source_path"].is_string()) {
            return makeError("proposal.invalid_field_type", "source_path must be a string");
        }
        const std::string sp = entry["source_path"].get<std::string>();
        if (sp.empty()) {
            return makeError("proposal.invalid_field_value", "source_path must not be empty");
        }
        if (sp.size() > kProposalSubmissionMaxPathBytes) {
            return makeError("proposal.field_too_large", "source_path exceeds the Backend byte limit");
        }
        change.sourcePath = detail::pathFromUtf8(sp);
    }
    if (op == FileChangeOperation::Rename && !change.sourcePath.has_value()) {
        return makeError("proposal.invalid_field_value", "Rename requires source_path to be a string");
    }
    if (op != FileChangeOperation::Rename && change.sourcePath.has_value()) {
        return makeError("proposal.invalid_field_value", "source_path must be null for this operation");
    }

    // expected_original_hash is never read from provider/model output at
    // all (requireExactFields above already rejects it as unexpected if
    // present) - it stays nullopt here unconditionally. The Backend's own
    // preflight (AgentToolTurnLoopRunner::preflightTrustedHashes) and
    // ChangeProposalService::bindObservedHashes() are what set it, from a
    // real audited workspace_hash_file result, before validation ever runs.
    change.expectedOriginalHash = std::nullopt;

    // proposed_content
    auto proposedContent = getOptionalString(entry, "proposed_content");
    if (!proposedContent.success()) {
        return makeError(
            proposedContent.error()->code,
            proposedContent.error()->message);
    }
    change.proposedContent = std::move(proposedContent.value());
    if (change.proposedContent.has_value()
        && change.proposedContent->size() > kProposalSubmissionMaxContentBytes) {
        return makeError("proposal.field_too_large", "proposed_content exceeds the Backend byte limit");
    }
    const bool writesContent = op == FileChangeOperation::Create
        || op == FileChangeOperation::Modify;
    if (writesContent && !change.proposedContent.has_value()) {
        return makeError("proposal.invalid_field_value", "Create/Modify requires proposed_content to be a string");
    }
    if (!writesContent && change.proposedContent.has_value()) {
        return makeError("proposal.invalid_field_value", "proposed_content must be null for this operation");
    }

    return common::Result<ProposedFileChange>::ok(std::move(change));
}

} // namespace

common::Result<ProposedChangeSet> ProposedChangeSetParser::parse(
    std::string_view rawModelOutput,
    const TrustedChangeSetMetadata& trustedMetadata)
{
    if (rawModelOutput.size() > kProposalSubmissionMaxRawOutputBytes) {
        return fail(
            "proposal.output_too_large",
            "Model output exceeds the parser byte limit");
    }

    // --- Step 1: extract JSON text (raw or from a single ```json fence) -------
    const auto fenceResult = extractJsonFence(rawModelOutput);
    if (!fenceResult.success()) {
        return common::Result<ProposedChangeSet>::failure(*fenceResult.error());
    }

    std::string_view jsonText = fenceResult.value().has_value()
                                  ? *fenceResult.value()
                                  : rawModelOutput;

    // --- Step 2: JSON decode ---------------------------------------------------
    json obj;
    try {
        obj = json::parse(jsonText);
    } catch (const std::exception&) {
        // Parser diagnostics may include fragments of the untrusted payload.
        // Keep the public error generic so a secret cannot be echoed before
        // the dedicated secret scan has had a chance to run.
        return fail("proposal.parse_error", "Model output is not valid JSON");
    }

    if (!obj.is_object()) {
        return fail("proposal.parse_error", "Top-level JSON value must be an object");
    }

    // --- Step 3: required top-level fields ------------------------------------
    // Exact field set, independent of whatever the provider's own strict
    // schema already constrained - the Backend parser is the trust boundary,
    // not the provider's schema enforcement.
    const auto topFields = requireExactFields(obj, "top-level submission",
        {"schemaVersion", "summary", "changes"});
    if (!topFields.success()) {
        return common::Result<ProposedChangeSet>::failure(*topFields.error());
    }
    auto schemaVersionResult = getString(obj, "schemaVersion");
    if (!schemaVersionResult.success()) {
        return fail(schemaVersionResult.error()->code, schemaVersionResult.error()->message);
    }
    if (schemaVersionResult.value() != kProposalSubmissionSchemaVersion) {
        return fail("proposal.unsupported_schema",
            "Proposal submission schema version is not supported");
    }

    auto summaryResult = getString(obj, "summary");
    if (!summaryResult.success()) return fail(summaryResult.error()->code, summaryResult.error()->message);
    if (summaryResult.value().empty()) {
        return fail("proposal.empty_summary", "summary field must not be empty");
    }
    if (summaryResult.value().size() > kProposalSubmissionMaxSummaryBytes) {
        return fail("proposal.field_too_large", "summary exceeds the Backend byte limit");
    }

    if (!obj["changes"].is_array()) {
        return fail("proposal.invalid_field_type", "changes must be an array");
    }

    // --- Step 4: parse changes ------------------------------------------------
    const auto& changesArr = obj["changes"];
    if (changesArr.size() > kProposalSubmissionMaxChanges) {
        return fail("proposal.too_many_changes",
                    "changes array exceeds maximum allowed size of "
                        + std::to_string(kProposalSubmissionMaxChanges));
    }

    std::vector<ProposedFileChange> changes;
    changes.reserve(changesArr.size());
    for (std::size_t i = 0; i < changesArr.size(); ++i) {
        auto changeResult = parseChange(changesArr[i], i);
        if (!changeResult.success()) {
            return common::Result<ProposedChangeSet>::failure(*changeResult.error());
        }
        changes.push_back(std::move(changeResult.value()));
    }

    // --- Step 5: assemble with trusted metadata -------------------------------
    ProposedChangeSet cs;
    cs.schemaVersion    = "proposal.v1";
    cs.changeSetId      = generateUuid();
    cs.runId            = trustedMetadata.runId;
    cs.projectId        = trustedMetadata.projectId;
    cs.providerId       = trustedMetadata.providerId;
    cs.modelId          = trustedMetadata.modelId;
    cs.modelExecutionId = trustedMetadata.modelExecutionId;
    cs.agentRole        = trustedMetadata.agentRole;
    cs.baseRevision     = trustedMetadata.baseRevision;
    cs.supersedesChangeSetId = trustedMetadata.supersedesChangeSetId;
    cs.summary          = std::move(summaryResult.value());
    cs.changes          = std::move(changes);
    cs.createdAt        = std::chrono::system_clock::now();
    cs.state            = ChangeSetState::Received;
    // agent_role from model is IGNORED; trusted metadata wins.
    // policySnapshotHash and artifactRelativeRoot are set later by the service.

    return common::Result<ProposedChangeSet>::ok(std::move(cs));
}

} // namespace ai::application
