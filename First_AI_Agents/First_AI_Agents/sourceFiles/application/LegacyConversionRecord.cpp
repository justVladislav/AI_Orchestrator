#include "ai/application/LegacyConversionRecord.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestRouting.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <utility>

namespace ai::application {

namespace {

constexpr std::size_t kMaxIdBytes = 128;

constexpr std::array<const char*, 4> kSettings {"maxAttempts", "fallback", "dependencies", "instructions"};

// The adapter policies a durable record may name. A rule change needs a new policy id, and a new
// id is added HERE deliberately: an arbitrary well-formed string is never accepted as a policy.
constexpr std::array<const char*, 1> kKnownPolicies {kLegacyRosterAdapterPolicyId};

bool isKnownPolicy(std::string_view policyId)
{
    return std::any_of(kKnownPolicies.begin(), kKnownPolicies.end(),
        [&](const char* known) { return policyId == known; });
}

// Persistence keeps milliseconds (SQLite), so two instants are "the same" at that precision.
std::int64_t milliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

common::Error failure(const char* code, std::string message)
{
    return {code, std::move(message)};
}

bool hasControlCharacter(std::string_view value)
{
    return std::any_of(value.begin(), value.end(),
        [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

bool isLowerHexSha256(const std::string& value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

bool isAllDigits(std::string_view value, std::size_t maxLength)
{
    return !value.empty() && value.size() <= maxLength
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

bool isKnownSourceWorkflow(std::string_view workflowId)
{
    return workflowId == kExecutionProfileWorkflowId || workflowId == kRetiredReadOnlyAnalysisWorkflowId;
}

std::optional<std::string> droppedEntryProblem(const LegacyRosterDroppedSetting& entry)
{
    if (entry.memberId.empty() || entry.memberId.size() > kManifestRoutingMaxMemberIdBytes
        || hasControlCharacter(entry.memberId)) {
        return std::string("a dropped entry names no valid member");
    }
    if (std::find(kSettings.begin(), kSettings.end(), entry.setting) == kSettings.end()) {
        return std::string("a dropped entry names an unknown setting");
    }
    if (entry.legacyValue.empty() || entry.legacyValue.size() > kLegacyConversionMaxLegacyValueBytes
        || hasControlCharacter(entry.legacyValue)) {
        return std::string("a dropped entry has no valid legacy value");
    }
    if (entry.reason.empty() || entry.reason.size() > kLegacyConversionMaxReasonBytes
        || hasControlCharacter(entry.reason)) {
        return std::string("a dropped entry has no valid reason");
    }
    // The audit must never hold instruction text: an instructions entry is "<N> characters".
    constexpr std::string_view kSuffix = " characters";
    if (entry.setting == "instructions") {
        const std::string_view value = entry.legacyValue;
        if (value.size() <= kSuffix.size()
            || value.substr(value.size() - kSuffix.size()) != kSuffix
            || !isAllDigits(value.substr(0, value.size() - kSuffix.size()), 9)) {
            return std::string("a dropped instructions entry holds more than a length");
        }
    }
    if (entry.setting == "maxAttempts" && !isAllDigits(entry.legacyValue, 10)) {
        return std::string("a dropped maxAttempts entry is not a number");
    }
    return std::nullopt;
}

} // namespace

const char* toString(LegacyConversionConfirmationMode mode) noexcept
{
    switch (mode) {
    case LegacyConversionConfirmationMode::ExplicitUser: return "explicit_user";
    case LegacyConversionConfirmationMode::NotRequiredSafeRetry: return "not_required_safe_retry";
    }
    return "explicit_user";
}

std::optional<LegacyConversionConfirmationMode> legacyConversionConfirmationModeFromString(
    std::string_view value) noexcept
{
    if (value == "explicit_user") return LegacyConversionConfirmationMode::ExplicitUser;
    if (value == "not_required_safe_retry") return LegacyConversionConfirmationMode::NotRequiredSafeRetry;
    return std::nullopt;
}

common::Result<LegacyConversionDraft> makeLegacyConversionDraft(
    const RunId& sourceRunId,
    const LegacyRosterConversionPlan& plan,
    LegacyConversionConfirmationMode mode)
{
    if (!plan.converts()) {
        return common::Result<LegacyConversionDraft>::failure(failure(kLegacyConversionRecordInvalidCode,
            "A plan that does not convert has no conversion to record"));
    }
    if (mode == LegacyConversionConfirmationMode::NotRequiredSafeRetry
        && !LegacyRosterAdapter::isSafeAutomaticRetry(plan)) {
        return common::Result<LegacyConversionDraft>::failure(failure(kLegacyConversionRecordInvalidCode,
            "Only a safe lone workspace lead may be converted without a confirmation"));
    }
    LegacyConversionDraft draft;
    draft.sourceRunId = sourceRunId;
    draft.sourceWorkflowId = plan.sourceWorkflowId;
    draft.policyId = plan.policyId;
    draft.reasonCode = plan.reasonCode;
    draft.sourceSnapshotFingerprint = plan.sourceSnapshotFingerprint;
    draft.targetRosterFingerprint = plan.targetRosterFingerprint;
    draft.confirmationFingerprint = LegacyRosterAdapter::confirmationFingerprint(sourceRunId, plan);
    draft.dropped = plan.dropped;
    draft.mode = mode;
    return common::Result<LegacyConversionDraft>::ok(std::move(draft));
}

LegacyConversionRecord makeLegacyConversionRecord(
    const LegacyConversionDraft& draft,
    const RunId& runId,
    std::chrono::system_clock::time_point createdAt,
    std::optional<std::chrono::system_clock::time_point> confirmedAt)
{
    LegacyConversionRecord record;
    record.runId = runId;
    record.sourceRunId = draft.sourceRunId;
    record.sourceWorkflowId = draft.sourceWorkflowId;
    record.policyId = draft.policyId;
    record.reasonCode = draft.reasonCode;
    record.sourceSnapshotFingerprint = draft.sourceSnapshotFingerprint;
    record.targetRosterFingerprint = draft.targetRosterFingerprint;
    record.confirmationFingerprint = draft.confirmationFingerprint;
    record.dropped = draft.dropped;
    record.confirmationMode = draft.mode;
    // A safe automatic Retry never claims that a user confirmed anything.
    if (draft.mode == LegacyConversionConfirmationMode::ExplicitUser) record.confirmedAt = confirmedAt;
    record.createdAt = createdAt;
    return record;
}

std::chrono::system_clock::time_point legacyConversionConfirmationTime(
    std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point runCreatedAt) noexcept
{
    return now < runCreatedAt ? runCreatedAt : now;
}

std::optional<std::string> legacyConversionRecordProblem(const LegacyConversionRecord& record)
{
    if (record.runId.empty() || record.runId.size() > kMaxIdBytes
        || record.sourceRunId.empty() || record.sourceRunId.size() > kMaxIdBytes) {
        return std::string("a conversion record names no valid Run");
    }
    if (record.runId == record.sourceRunId) {
        return std::string("a conversion record cannot be its own source");
    }
    if (!isKnownSourceWorkflow(record.sourceWorkflowId)) {
        return std::string("a conversion record names an unknown source workflow");
    }
    if (!isKnownPolicy(record.policyId)) {
        return std::string("a conversion record names no known adapter policy");
    }
    if (record.reasonCode != kLegacyRosterReasonConvertedLoneWorkspaceLead
        && record.reasonCode != kLegacyRosterReasonConvertedProposalRoster) {
        return std::string("a conversion record carries a reason code that is not a conversion");
    }
    if (record.sourceWorkflowId == kRetiredReadOnlyAnalysisWorkflowId
        && record.reasonCode != kLegacyRosterReasonConvertedLoneWorkspaceLead) {
        return std::string("a retired read-only Run can only convert as a lone workspace lead");
    }
    if (!isLowerHexSha256(record.sourceSnapshotFingerprint)
        || !isLowerHexSha256(record.targetRosterFingerprint)
        || !isLowerHexSha256(record.confirmationFingerprint)) {
        return std::string("a conversion record fingerprint is not a SHA-256");
    }
    if (record.dropped.size() > kLegacyConversionMaxDroppedEntries) {
        return std::string("a conversion record lists too many dropped settings");
    }
    std::set<std::pair<std::string_view, std::string_view>> seenSettings;
    for (const auto& entry : record.dropped) {
        if (const auto problem = droppedEntryProblem(entry)) return problem;
        // One setting of one member is dropped at most once; a repeat is a forged or doubled list.
        if (!seenSettings.emplace(entry.memberId, entry.setting).second) {
            return std::string("a conversion record lists the same setting of a member twice");
        }
    }
    switch (record.confirmationMode) {
    case LegacyConversionConfirmationMode::ExplicitUser:
        if (!record.confirmedAt.has_value()) {
            return std::string("an explicit confirmation carries no confirmation time");
        }
        if (milliseconds(*record.confirmedAt) < milliseconds(record.createdAt)) {
            return std::string("an explicit confirmation precedes the record it belongs to");
        }
        break;
    case LegacyConversionConfirmationMode::NotRequiredSafeRetry:
        if (record.confirmedAt.has_value()) {
            return std::string("a safe automatic Retry must not claim a confirmation time");
        }
        if (!record.dropped.empty()
            || record.reasonCode != kLegacyRosterReasonConvertedLoneWorkspaceLead) {
            return std::string("only a safe lone workspace lead may skip the confirmation");
        }
        break;
    }
    if (record.confirmationFingerprint != LegacyRosterAdapter::confirmationFingerprint(
            record.sourceRunId, record.sourceWorkflowId, record.policyId, record.reasonCode,
            record.sourceSnapshotFingerprint, kExecutionManifestWorkflowId,
            record.targetRosterFingerprint, record.dropped)) {
        return std::string("the confirmation fingerprint does not match the record");
    }
    return std::nullopt;
}

std::optional<std::string> legacyConversionAggregateProblem(
    const Run& run, const LegacyConversionRecord& record)
{
    if (const auto problem = legacyConversionRecordProblem(record)) return problem;
    if (record.runId != run.id) {
        return std::string("the conversion record names another Run");
    }
    if (!run.parentRunId.has_value() || *run.parentRunId != record.sourceRunId) {
        return std::string("the Run's parent is not the conversion's source Run");
    }
    if (run.workflowId != kExecutionManifestWorkflowId || run.executionProfileId.has_value()) {
        return std::string("a converted Run must be a per-Run Manifest Run");
    }
    if (milliseconds(record.createdAt) != milliseconds(run.createdAt)) {
        return std::string("the conversion record was not created together with the Run");
    }
    if (record.targetRosterFingerprint != LegacyRosterAdapter::rosterFingerprint(run.assignmentSnapshot)) {
        return std::string("the Run's frozen roster is not the roster the conversion certifies");
    }

    // From here the frozen roster is the certified one, so the record's claims can be checked
    // against it: a self-consistent fingerprint alone cannot make a false audit true.
    const auto& roster = run.assignmentSnapshot;
    if (record.confirmationMode == LegacyConversionConfirmationMode::NotRequiredSafeRetry
        && roster.size() != 1) {
        return std::string("a safe automatic Retry certifies a roster of more than one member");
    }
    const bool singleMember = roster.size() == 1;
    if ((record.reasonCode == kLegacyRosterReasonConvertedLoneWorkspaceLead) != singleMember) {
        return std::string("the conversion's reason does not fit the size of the frozen roster");
    }
    for (const auto& entry : record.dropped) {
        const bool known = std::any_of(roster.begin(), roster.end(),
            [&](const StageModelAssignment& member) { return member.stageType == entry.memberId; });
        if (!known) {
            return std::string("a dropped setting names a member the frozen roster does not have");
        }
    }
    return std::nullopt;
}

common::Result<std::string> legacyConversionDroppedToJson(
    const std::vector<LegacyRosterDroppedSetting>& dropped)
{
    if (dropped.size() > kLegacyConversionMaxDroppedEntries) {
        return common::Result<std::string>::failure(failure(kLegacyConversionRecordInvalidCode,
            "A conversion record lists too many dropped settings"));
    }
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : dropped) {
        if (const auto problem = droppedEntryProblem(entry)) {
            return common::Result<std::string>::failure(failure(kLegacyConversionRecordInvalidCode, *problem));
        }
        array.push_back({{"memberId", entry.memberId}, {"setting", entry.setting},
            {"legacyValue", entry.legacyValue}, {"reason", entry.reason}});
    }
    try {
        std::string text = array.dump();
        if (text.size() > kLegacyConversionMaxDroppedJsonBytes) {
            return common::Result<std::string>::failure(failure(kLegacyConversionRecordInvalidCode,
                "The dropped settings of a conversion record exceed their size bound"));
        }
        return common::Result<std::string>::ok(std::move(text));
    } catch (const std::exception&) {
        // nlohmann refuses text that is not valid UTF-8.
        return common::Result<std::string>::failure(failure(kLegacyConversionRecordInvalidCode,
            "The dropped settings of a conversion record are not valid UTF-8 text"));
    }
}

common::Result<std::vector<LegacyRosterDroppedSetting>> legacyConversionDroppedFromJson(
    std::string_view json)
{
    const auto corrupt = [](std::string message) {
        return common::Result<std::vector<LegacyRosterDroppedSetting>>::failure(
            failure(kLegacyConversionRecordCorruptCode, std::move(message)));
    };
    if (json.size() > kLegacyConversionMaxDroppedJsonBytes) {
        return corrupt("The stored dropped settings exceed their size bound");
    }
    const nlohmann::json parsed = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return corrupt("The stored dropped settings are not a JSON array");
    }
    if (parsed.size() > kLegacyConversionMaxDroppedEntries) {
        return corrupt("The stored dropped settings list too many entries");
    }
    std::vector<LegacyRosterDroppedSetting> result;
    result.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_object() || item.size() != 4) {
            return corrupt("A stored dropped setting is not an object with exactly four fields");
        }
        LegacyRosterDroppedSetting entry;
        for (const char* key : {"memberId", "setting", "legacyValue", "reason"}) {
            const auto found = item.find(key);
            if (found == item.end() || !found->is_string()) {
                return corrupt("A stored dropped setting field is missing or not text");
            }
        }
        entry.memberId = item["memberId"].get<std::string>();
        entry.setting = item["setting"].get<std::string>();
        entry.legacyValue = item["legacyValue"].get<std::string>();
        entry.reason = item["reason"].get<std::string>();
        if (const auto problem = droppedEntryProblem(entry)) return corrupt(*problem);
        result.push_back(std::move(entry));
    }
    return common::Result<std::vector<LegacyRosterDroppedSetting>>::ok(std::move(result));
}

} // namespace ai::application
