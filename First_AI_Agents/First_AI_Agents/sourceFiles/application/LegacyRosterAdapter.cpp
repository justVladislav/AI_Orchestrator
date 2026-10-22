#include "ai/application/LegacyRosterAdapter.hpp"

#include "ai/application/ExecutionAssignmentValidation.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <sstream>

namespace ai::application {

namespace {

constexpr const char* kConvertedLoneWorkspaceLead = kLegacyRosterReasonConvertedLoneWorkspaceLead;
constexpr const char* kConvertedProposalRoster = kLegacyRosterReasonConvertedProposalRoster;
constexpr const char* kTextRosterStaysLegacy = "legacy_roster.text_roster_stays_legacy";
constexpr const char* kUnsupportedSourceWorkflow = "legacy_roster.unsupported_source_workflow";
constexpr const char* kEmptySnapshot = "legacy_roster.empty_snapshot";
constexpr const char* kMultipleLeads = "legacy_roster.multiple_lead_architects";
constexpr const char* kNoLead = "legacy_roster.no_lead_architect";
constexpr const char* kWorkspaceLeadInRoster = "legacy_roster.workspace_lead_in_roster";
constexpr const char* kManifestRosterInvalid = "legacy_roster.manifest_roster_invalid";

constexpr const char* kAttemptsReason =
    "The Manifest workflow makes one attempt per task and never retries a paid call "
    "automatically; a user Retry creates a new Run";
constexpr const char* kFallbackReason =
    "The Manifest workflow never switches provider or model on its own";
constexpr const char* kDependenciesReason =
    "Task dependencies are declared by the Planner in the frozen Manifest, not configured "
    "per roster member";
constexpr const char* kInstructionsReasonLead =
    "The lead only plans: its instructions are not sent to the Planner or to any worker "
    "(they stay in the roster and in the planning fingerprint)";
constexpr const char* kInstructionsReasonSingle =
    "A single-assignment Manifest does not send the member's instructions to the Planner "
    "or to the task session (they stay in the roster and in the planning fingerprint)";

void appendField(std::ostringstream& out, std::string_view name, std::string_view value)
{
    out << name.size() << ':' << name << '=' << value.size() << ':' << value << '\n';
}

// Every field a Run's assignment snapshot persists, in a fixed order. The canonical form of a
// member is shared by the source-snapshot and the target-roster fingerprints.
void appendMember(std::ostringstream& material, const StageModelAssignment& member)
{
    appendField(material, "stage-type", member.stageType);
    appendField(material, "provider-id", member.providerId);
    appendField(material, "model-id", member.modelId);
    appendField(material, "timeout-ms", std::to_string(member.timeout.count()));
    appendField(material, "agent-name", member.agentName);
    appendField(material, "role", member.role);
    appendField(material, "instructions", member.instructions);
    appendField(material, "position", std::to_string(member.position));
    appendField(material, "lead", member.leadArchitect ? "1" : "0");
    appendField(material, "workspace", member.workspaceAccess ? "1" : "0");
    appendField(material, "dependency-count", std::to_string(member.dependencies.size()));
    for (const auto& dependency : member.dependencies) {
        appendField(material, "dependency", dependency);
    }
    appendField(material, "max-attempts", std::to_string(member.maxAttempts));
    appendField(material, "fallback-provider-id", member.fallbackProviderId.value_or(""));
    appendField(material, "fallback-model-id", member.fallbackModelId.value_or(""));
}

std::vector<StageModelAssignment> inPositionOrder(std::vector<StageModelAssignment> members)
{
    std::stable_sort(members.begin(), members.end(),
        [](const StageModelAssignment& left, const StageModelAssignment& right) {
            if (left.position != right.position) return left.position < right.position;
            return left.stageType < right.stageType;
        });
    return members;
}

struct LeadResolution {
    std::size_t index {0};
    const char* problemCode {nullptr};
    const char* problemMessage {nullptr};
};

// Mirrors the legacy runner's architectOf(): the flagged lead, else the canonical
// "architect" stage. A lone member is the lead whatever its flag says (exactly what
// ManifestRouting::fromAssignments does for a single assignment).
LeadResolution resolveLead(const std::vector<StageModelAssignment>& members)
{
    LeadResolution result;
    if (members.size() == 1) return result;

    std::size_t flagged = 0;
    std::size_t flaggedIndex = 0;
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (members[index].leadArchitect) {
            ++flagged;
            flaggedIndex = index;
        }
    }
    if (flagged > 1) {
        result.problemCode = kMultipleLeads;
        result.problemMessage = "The roster names more than one lead Architect";
        return result;
    }
    if (flagged == 1) {
        result.index = flaggedIndex;
        return result;
    }

    std::size_t canonical = 0;
    std::size_t canonicalIndex = 0;
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (members[index].stageType == "architect") {
            ++canonical;
            canonicalIndex = index;
        }
    }
    if (canonical == 1) {
        result.index = canonicalIndex;
        return result;
    }
    result.problemCode = kNoLead;
    result.problemMessage = "The roster names no lead Architect";
    return result;
}

// A legacy value as it is shown and recorded: bounded, so the plan, the user's summary, the
// confirmation fingerprint and the durable record all hold the very same text. A longer value is
// cut at a UTF-8 character boundary and ends with an explicit "..." marker.
constexpr std::size_t kMaxLegacyValueBytes = 256;

std::string bounded(std::string value)
{
    if (value.size() <= kMaxLegacyValueBytes) return value;
    std::size_t cut = kMaxLegacyValueBytes - 3;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut;
    value.resize(cut);
    value += "...";
    return value;
}

std::string joined(const std::vector<std::string>& values)
{
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ", ";
        result += value;
    }
    return bounded(std::move(result));
}

void collectDropped(
    const StageModelAssignment& member,
    const char* instructionsReason,
    std::vector<LegacyRosterDroppedSetting>& dropped)
{
    if (member.maxAttempts > 1) {
        dropped.push_back({member.stageType, "maxAttempts",
            std::to_string(member.maxAttempts), kAttemptsReason});
    }
    if (member.fallbackProviderId.has_value() || member.fallbackModelId.has_value()) {
        dropped.push_back({member.stageType, "fallback",
            bounded(member.fallbackProviderId.value_or("") + "/" + member.fallbackModelId.value_or("")),
            kFallbackReason});
    }
    if (!member.dependencies.empty()) {
        dropped.push_back({member.stageType, "dependencies",
            joined(member.dependencies), kDependenciesReason});
    }
    if (instructionsReason != nullptr && !member.instructions.empty()) {
        dropped.push_back({member.stageType, "instructions",
            std::to_string(member.instructions.size()) + " characters", instructionsReason});
    }
}

StageModelAssignment convertedMember(StageModelAssignment member, bool lead)
{
    member.leadArchitect = lead;
    member.maxAttempts = 1;
    member.fallbackProviderId.reset();
    member.fallbackModelId.reset();
    member.dependencies.clear();
    // validateRunRoster (the per-Run roster path) requires a role; an empty one has
    // always meant "the stage id" (ManifestRouting::effectiveRole).
    if (member.role.empty()) member.role = ManifestRouting::effectiveRole(member);
    return member;
}

} // namespace

const char* toString(LegacyRosterShape shape) noexcept
{
    switch (shape) {
    case LegacyRosterShape::Unclassified: return "unclassified";
    case LegacyRosterShape::LoneWorkspaceLead: return "lone_workspace_lead";
    case LegacyRosterShape::ProposalRoster: return "proposal_roster";
    case LegacyRosterShape::LoneTextArchitect: return "lone_text_architect";
    case LegacyRosterShape::TextRoster: return "text_roster";
    case LegacyRosterShape::MixedRoster: return "mixed_roster";
    }
    return "unclassified";
}

const char* toString(LegacyRosterDisposition disposition) noexcept
{
    switch (disposition) {
    case LegacyRosterDisposition::ConvertToManifestSingle: return "convert_to_manifest_single";
    case LegacyRosterDisposition::ConvertToManifestRoster: return "convert_to_manifest_roster";
    case LegacyRosterDisposition::StayLegacy: return "stay_legacy";
    case LegacyRosterDisposition::NotConvertible: return "not_convertible";
    }
    return "not_convertible";
}

std::vector<StageModelAssignment> LegacyRosterAdapter::migrateRetiredReadOnlyAssignments(
    std::vector<StageModelAssignment> assignments)
{
    if (assignments.empty()) return assignments;

    std::sort(assignments.begin(), assignments.end(),
        [](const auto& left, const auto& right) { return left.position < right.position; });

    // The retired runner was a single workspace-tool agent. Persisted rows predate
    // workspaceAccess, so schema v23 read them as false; explicitly restore that
    // semantic while adapting the run to the unified runner.
    for (auto& assignment : assignments) {
        assignment.leadArchitect = false;
        assignment.workspaceAccess = true;
        assignment.maxAttempts = 1;
        assignment.fallbackProviderId.reset();
        assignment.fallbackModelId.reset();
    }
    assignments.front().leadArchitect = true;
    if (assignments.front().agentName.empty())
        assignments.front().agentName = "Read-only analysis";
    if (assignments.front().role.empty())
        assignments.front().role = "Workspace analysis agent";
    return assignments;
}

std::string LegacyRosterAdapter::sourceSnapshotFingerprint(
    std::string_view sourceWorkflowId,
    const std::vector<StageModelAssignment>& sourceSnapshot)
{
    std::ostringstream material;
    appendField(material, "policy", kLegacyRosterAdapterPolicyId);
    appendField(material, "source-workflow", sourceWorkflowId);
    for (const auto& member : inPositionOrder(sourceSnapshot)) appendMember(material, member);
    return sha256Hex(material.str());
}

std::string LegacyRosterAdapter::rosterFingerprint(const std::vector<StageModelAssignment>& roster)
{
    std::ostringstream material;
    appendField(material, "policy", kLegacyRosterAdapterPolicyId);
    appendField(material, "domain", "target-roster");
    for (const auto& member : inPositionOrder(roster)) appendMember(material, member);
    return sha256Hex(material.str());
}

std::string LegacyRosterAdapter::confirmationFingerprint(
    std::string_view sourceRunId,
    std::string_view sourceWorkflowId,
    std::string_view policyId,
    std::string_view reasonCode,
    std::string_view sourceSnapshotFingerprint,
    std::string_view targetWorkflowId,
    std::string_view targetRosterFingerprint,
    const std::vector<LegacyRosterDroppedSetting>& dropped)
{
    std::ostringstream material;
    appendField(material, "domain", "legacy-conversion-confirmation");
    appendField(material, "source-run-id", sourceRunId);
    appendField(material, "source-workflow", sourceWorkflowId);
    appendField(material, "policy", policyId);
    appendField(material, "reason-code", reasonCode);
    appendField(material, "source-snapshot-fingerprint", sourceSnapshotFingerprint);
    appendField(material, "target-workflow", targetWorkflowId);
    appendField(material, "target-roster-fingerprint", targetRosterFingerprint);
    appendField(material, "dropped-count", std::to_string(dropped.size()));
    for (const auto& item : dropped) {
        appendField(material, "dropped-member", item.memberId);
        appendField(material, "dropped-setting", item.setting);
        appendField(material, "dropped-value", item.legacyValue);
        appendField(material, "dropped-reason", item.reason);
    }
    return sha256Hex(material.str());
}

std::string LegacyRosterAdapter::confirmationFingerprint(
    std::string_view sourceRunId, const LegacyRosterConversionPlan& plan)
{
    if (!plan.converts()) return {};
    return confirmationFingerprint(sourceRunId, plan.sourceWorkflowId, plan.policyId,
        plan.reasonCode, plan.sourceSnapshotFingerprint, plan.targetWorkflowId,
        plan.targetRosterFingerprint, plan.dropped);
}

bool LegacyRosterAdapter::isSafeAutomaticRetry(const LegacyRosterConversionPlan& plan) noexcept
{
    return plan.disposition == LegacyRosterDisposition::ConvertToManifestSingle
        && plan.shape == LegacyRosterShape::LoneWorkspaceLead
        && plan.dropped.empty()
        && plan.targetWorkflowId == kExecutionManifestWorkflowId;
}

LegacyRosterConversionPlan LegacyRosterAdapter::buildConversionPlan(
    std::string_view sourceWorkflowId,
    const std::vector<StageModelAssignment>& sourceSnapshot)
{
    LegacyRosterConversionPlan plan;
    plan.policyId = kLegacyRosterAdapterPolicyId;
    plan.sourceWorkflowId = std::string(sourceWorkflowId);
    plan.sourceSnapshotFingerprint = sourceSnapshotFingerprint(sourceWorkflowId, sourceSnapshot);

    const auto reject = [&plan](const char* code, std::string message) {
        plan.disposition = LegacyRosterDisposition::NotConvertible;
        plan.reasonCode = code;
        plan.reasonMessage = std::move(message);
        return plan;
    };

    std::vector<StageModelAssignment> members;
    if (sourceWorkflowId == kRetiredReadOnlyAnalysisWorkflowId) {
        members = migrateRetiredReadOnlyAssignments(sourceSnapshot);
    } else if (sourceWorkflowId == kExecutionProfileWorkflowId) {
        members = sourceSnapshot;
    } else {
        return reject(kUnsupportedSourceWorkflow,
            "Only an execution-profile-v1 or a retired read-only-analysis-v1 Run has a legacy "
            "roster to convert");
    }
    if (members.empty()) {
        return reject(kEmptySnapshot, "The source Run has no frozen assignment snapshot");
    }
    members = inPositionOrder(std::move(members));

    const auto lead = resolveLead(members);
    if (lead.problemCode != nullptr) return reject(lead.problemCode, lead.problemMessage);

    const bool single = members.size() == 1;
    if (single) {
        if (!members.front().workspaceAccess) {
            plan.shape = LegacyRosterShape::LoneTextArchitect;
            plan.disposition = LegacyRosterDisposition::StayLegacy;
            plan.reasonCode = kTextRosterStaysLegacy;
            plan.reasonMessage =
                "A lone agent without workspace access is a single text completion; the Manifest "
                "workflow only plans and proposes workspace changes, so it stays on the legacy "
                "roster runner";
            return plan;
        }
        plan.shape = LegacyRosterShape::LoneWorkspaceLead;
    } else {
        std::size_t workers = 0;
        std::size_t workspaceWorkers = 0;
        for (std::size_t index = 0; index < members.size(); ++index) {
            if (index == lead.index) continue;
            ++workers;
            if (members[index].workspaceAccess) ++workspaceWorkers;
        }
        if (workspaceWorkers == 0 || workspaceWorkers != workers) {
            plan.shape = workspaceWorkers == 0 ? LegacyRosterShape::TextRoster
                                               : LegacyRosterShape::MixedRoster;
            plan.disposition = LegacyRosterDisposition::StayLegacy;
            plan.reasonCode = kTextRosterStaysLegacy;
            plan.reasonMessage = workspaceWorkers == 0
                ? "No worker has workspace access: this roster is a text pipeline, so it stays on "
                  "the legacy roster runner"
                : "Some workers have no workspace access, and the Manifest workflow can only "
                  "assign workspace tasks, so this roster stays on the legacy roster runner";
            return plan;
        }
        plan.shape = LegacyRosterShape::ProposalRoster;
        if (members[lead.index].workspaceAccess) {
            return reject(kWorkspaceLeadInRoster,
                "The lead Architect has workspace access, but in a Manifest roster the lead only "
                "plans; edit the roster to decide whether it should plan or execute");
        }
    }

    plan.roster.reserve(members.size());
    for (std::size_t index = 0; index < members.size(); ++index) {
        const bool isLead = single || index == lead.index;
        // Only the lead's (and a lone member's) instructions are not consumed; a roster
        // worker's instructions reach its task session.
        const char* instructionsReason = single ? kInstructionsReasonSingle
            : (isLead ? kInstructionsReasonLead : nullptr);
        collectDropped(members[index], instructionsReason, plan.dropped);
        plan.roster.push_back(convertedMember(members[index], isLead));
    }

    const auto rosterValid = validateManifestRoster(plan.roster);
    if (!rosterValid.success()) {
        plan.dropped.clear();
        plan.roster.clear();
        return reject(kManifestRosterInvalid,
            rosterValid.error()->code + ": " + rosterValid.error()->message);
    }
    if (const auto problem = ManifestRouting::recordProblem(
            ManifestRouting::fromAssignments(plan.roster))) {
        plan.dropped.clear();
        plan.roster.clear();
        return reject(kManifestRosterInvalid, *problem);
    }

    plan.targetWorkflowId = kExecutionManifestWorkflowId;
    plan.targetRosterFingerprint = rosterFingerprint(plan.roster);
    if (single) {
        plan.disposition = LegacyRosterDisposition::ConvertToManifestSingle;
        plan.reasonCode = kConvertedLoneWorkspaceLead;
        plan.reasonMessage =
            "A lone workspace agent re-launches as a single-assignment Manifest Run: it plans "
            "and executes through the Planner, validation and freeze path";
    } else {
        plan.disposition = LegacyRosterDisposition::ConvertToManifestRoster;
        plan.reasonCode = kConvertedProposalRoster;
        plan.reasonMessage =
            "A proposal roster re-launches as a Manifest roster: the lead plans and the workers "
            "are an available pool the Planner assigns tasks to";
    }
    return plan;
}

} // namespace ai::application
