// Unit tests for AG-17.7A's LegacyRosterAdapter: the pure decision function that says
// whether a finished legacy roster Run can be re-launched on the Manifest workflow.
// Fixtures mirror the REAL shapes found in the production database on 2026-10-02
// (the retired read-only snapshot, the four-member proposal roster with a lead that
// carries maxAttempts 2) so the policy is proven against what users actually ran.
// No provider, no SQLite, no repository reads inside the adapter itself.
#include "ai/application/ExecutionAssignmentValidation.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/LegacyRosterAdapter.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/ProviderCatalogService.hpp"
#include "ai/application/WorkflowRouter.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ai::application;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

StageModelAssignment member(const std::string& stageType, const std::string& providerId,
    const std::string& modelId, const std::string& role, bool lead, bool workspace,
    std::uint32_t position)
{
    StageModelAssignment value;
    value.stageType = stageType;
    value.providerId = providerId;
    value.modelId = modelId;
    value.role = role;
    value.leadArchitect = lead;
    value.workspaceAccess = workspace;
    value.position = position;
    value.timeout = 120000ms;
    return value;
}

// The retired read-only runner's persisted rows: one "analysis" agent, no lead flag,
// workspaceAccess false (the column did not exist), one attempt.
std::vector<StageModelAssignment> readOnlySnapshot(
    const std::string& provider = "provider-1", const std::string& model = "model-1")
{
    auto analysis = member("analysis", provider, model,
        "Read-only workspace analysis agent", false, false, 0);
    analysis.agentName = "Read-only analysis";
    return {analysis};
}

// A lone workspace lead exactly as migrateRetiredReadOnlyAssignments builds it.
std::vector<StageModelAssignment> loneWorkspaceLead(
    const std::string& provider = "provider-1", const std::string& model = "model-1")
{
    auto lead = member("architect", provider, model, "Workspace analysis agent", true, true, 0);
    return {lead};
}

// S1: 26 real Runs - a lone text architect (the architect retry default is 2).
std::vector<StageModelAssignment> loneTextArchitect()
{
    auto lead = member("architect", "provider-1", "model-1", "Software Architect", true, false, 0);
    lead.maxAttempts = 2;
    return {lead};
}

// S3: 6 real Runs - a text roster.
std::vector<StageModelAssignment> textRoster()
{
    auto lead = member("architect", "provider-1", "model-1", "Software Architect", true, false, 0);
    lead.maxAttempts = 2;
    auto workerA = member("agent-2", "provider-1", "model-2", "Implementation specialist",
        false, false, 1);
    workerA.maxAttempts = 2;
    auto workerB = member("agent-3", "provider-2", "model-3", "Documentation specialist",
        false, false, 2);
    workerB.maxAttempts = 2;
    return {lead, workerA, workerB};
}

// S4: 7 real Runs - a lead without workspace access (maxAttempts 2, instructions) and
// three workspace workers.
std::vector<StageModelAssignment> proposalRoster(
    const std::string& leadProvider = "provider-1", const std::string& leadModel = "model-1",
    const std::string& workerProvider = "provider-2", const std::string& workerModel = "model-2")
{
    auto lead = member("architect", leadProvider, leadModel, "Software Architect", true, false, 0);
    lead.maxAttempts = 2;
    lead.instructions = "Plan the work and keep tasks independent.";
    std::vector<StageModelAssignment> roster {lead};
    for (std::uint32_t index = 1; index <= 3; ++index) {
        auto worker = member("agent-" + std::to_string(index + 1), workerProvider, workerModel,
            "Implementation specialist", false, true, index);
        worker.instructions = "Implement only your slice.";
        roster.push_back(worker);
    }
    return roster;
}

bool hasDropped(const LegacyRosterConversionPlan& plan, const std::string& memberId,
    const std::string& setting, const std::string& legacyValue)
{
    return std::any_of(plan.dropped.begin(), plan.dropped.end(),
        [&](const LegacyRosterDroppedSetting& item) {
            return item.memberId == memberId && item.setting == setting
                && item.legacyValue == legacyValue && !item.reason.empty();
        });
}

void requireNothingIsConverted(const LegacyRosterConversionPlan& plan, const char* message)
{
    require(!plan.converts() && plan.roster.empty() && plan.dropped.empty()
            && plan.targetWorkflowId.empty() && !plan.reasonCode.empty()
            && !plan.reasonMessage.empty() && !plan.sourceSnapshotFingerprint.empty()
            && plan.policyId == kLegacyRosterAdapterPolicyId,
        message);
}

bool sameAssignment(const StageModelAssignment& left, const StageModelAssignment& right)
{
    return left.stageType == right.stageType && left.providerId == right.providerId
        && left.modelId == right.modelId && left.timeout == right.timeout
        && left.agentName == right.agentName && left.role == right.role
        && left.instructions == right.instructions && left.position == right.position
        && left.leadArchitect == right.leadArchitect
        && left.workspaceAccess == right.workspaceAccess
        && left.dependencies == right.dependencies && left.maxAttempts == right.maxAttempts
        && left.fallbackProviderId == right.fallbackProviderId
        && left.fallbackModelId == right.fallbackModelId;
}

bool samePlan(const LegacyRosterConversionPlan& left, const LegacyRosterConversionPlan& right)
{
    if (left.policyId != right.policyId || left.sourceWorkflowId != right.sourceWorkflowId
        || left.sourceSnapshotFingerprint != right.sourceSnapshotFingerprint
        || left.shape != right.shape || left.disposition != right.disposition
        || left.reasonCode != right.reasonCode || left.reasonMessage != right.reasonMessage
        || left.targetWorkflowId != right.targetWorkflowId
        || left.roster.size() != right.roster.size() || left.dropped.size() != right.dropped.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.roster.size(); ++index) {
        if (!sameAssignment(left.roster[index], right.roster[index])) return false;
    }
    for (std::size_t index = 0; index < left.dropped.size(); ++index) {
        const auto& a = left.dropped[index];
        const auto& b = right.dropped[index];
        if (a.memberId != b.memberId || a.setting != b.setting || a.legacyValue != b.legacyValue
            || a.reason != b.reason) {
            return false;
        }
    }
    return true;
}

// ---- conversions ------------------------------------------------------------------

void testRetiredReadOnlySnapshotConvertsToASingleAssignmentRun()
{
    const auto plan = LegacyRosterAdapter::buildConversionPlan(
        kRetiredReadOnlyAnalysisWorkflowId, readOnlySnapshot());
    require(plan.disposition == LegacyRosterDisposition::ConvertToManifestSingle
            && plan.shape == LegacyRosterShape::LoneWorkspaceLead && plan.converts(),
        "A retired read-only snapshot must convert to a single-assignment Manifest Run");
    require(plan.reasonCode == "legacy_roster.converted_lone_workspace_lead"
            && plan.policyId == "legacy-roster-adapter.v1"
            && plan.sourceWorkflowId == "read-only-analysis-v1"
            && plan.targetWorkflowId == kExecutionManifestWorkflowId
            && plan.sourceSnapshotFingerprint.size() == 64,
        "The decision must carry a versioned policy id, a stable reason code, the source "
        "workflow and the source snapshot fingerprint");
    require(plan.roster.size() == 1, "A lone agent converts to a one-member roster");
    const auto& converted = plan.roster.front();
    require(converted.stageType == "analysis" && converted.providerId == "provider-1"
            && converted.modelId == "model-1" && converted.leadArchitect
            && converted.workspaceAccess && converted.maxAttempts == 1
            && !converted.fallbackProviderId.has_value() && converted.dependencies.empty()
            && converted.timeout == 120000ms && converted.agentName == "Read-only analysis"
            && converted.role == "Read-only workspace analysis agent",
        "The retired runner's semantics (lead, workspace tools, one attempt) must be restored "
        "and its provider/model/role/timeout kept");
    require(plan.dropped.empty(),
        "Restoring a retired runner's own semantics is not a user-visible drop");
}

void testLoneWorkspaceLeadOfAProfileRunConvertsAndReportsDroppedSettings()
{
    auto source = loneWorkspaceLead();
    source.front().role.clear();
    source.front().maxAttempts = 3;
    source.front().fallbackProviderId = "provider-9";
    source.front().fallbackModelId = "model-9";
    source.front().instructions = "Be careful.";
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.disposition == LegacyRosterDisposition::ConvertToManifestSingle
            && plan.reasonCode == "legacy_roster.converted_lone_workspace_lead",
        "A lone workspace lead of an execution-profile Run must convert");
    require(plan.roster.size() == 1 && plan.roster.front().maxAttempts == 1
            && !plan.roster.front().fallbackProviderId.has_value()
            && !plan.roster.front().fallbackModelId.has_value()
            && plan.roster.front().role == "architect"
            && plan.roster.front().instructions == "Be careful.",
        "Attempts and fallback are normalized, an empty role defaults to the stage id and "
        "the instruction text is preserved");
    require(plan.dropped.size() == 3 && hasDropped(plan, "architect", "maxAttempts", "3")
            && hasDropped(plan, "architect", "fallback", "provider-9/model-9")
            && hasDropped(plan, "architect", "instructions", "11 characters"),
        "Every setting the single-assignment Run will not act on must be reported, with "
        "instruction text reduced to its length");
    require(plan.dropped[0].setting == "maxAttempts" && plan.dropped[1].setting == "fallback"
            && plan.dropped[2].setting == "instructions",
        "Dropped settings must come in a fixed order");
}

void testProposalRosterConvertsAndReportsExactlyTheLeadsDroppedSettings()
{
    const auto source = proposalRoster();
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.disposition == LegacyRosterDisposition::ConvertToManifestRoster
            && plan.shape == LegacyRosterShape::ProposalRoster
            && plan.reasonCode == "legacy_roster.converted_proposal_roster"
            && plan.targetWorkflowId == kExecutionManifestWorkflowId,
        "A proposal roster must convert to a Manifest roster");
    require(plan.roster.size() == 4 && plan.roster[0].stageType == "architect"
            && plan.roster[0].leadArchitect && !plan.roster[0].workspaceAccess
            && plan.roster[0].maxAttempts == 1 && plan.roster[1].stageType == "agent-2"
            && plan.roster[2].stageType == "agent-3" && plan.roster[3].stageType == "agent-4",
        "The roster must keep its members in position order with the lead normalized");
    for (std::size_t index = 1; index < plan.roster.size(); ++index) {
        require(!plan.roster[index].leadArchitect && plan.roster[index].workspaceAccess
                && plan.roster[index].instructions == "Implement only your slice."
                && plan.roster[index].providerId == "provider-2"
                && plan.roster[index].modelId == "model-2",
            "A worker keeps its provider, model, workspace access and instructions");
    }
    require(plan.dropped.size() == 2
            && hasDropped(plan, "architect", "maxAttempts", "2")
            && hasDropped(plan, "architect", "instructions", "41 characters"),
        "The real proposal roster drops exactly the lead's attempts and instructions");

    require(validateManifestRoster(plan.roster).success(),
        "The converted roster must pass the Manifest roster rules");
    const auto record = ManifestRouting::fromAssignments(plan.roster);
    require(!ManifestRouting::recordProblem(record).has_value()
            && ManifestRouting::assignableMemberIds(record).size() == 3,
        "The converted roster must freeze into a consistent routing record with three "
        "assignable workers");
}

void testEveryDroppedSettingIsReportedInMemberOrder()
{
    auto source = proposalRoster();
    source[0].fallbackProviderId = "fb-provider";
    source[0].fallbackModelId = "fb-model";
    source[0].dependencies = {"agent-2"};
    source[2].dependencies = {"agent-2", "agent-4"};
    source[2].maxAttempts = 3;
    source[2].fallbackProviderId = "w-provider";
    source[2].fallbackModelId = "w-model";
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.converts(), "The roster must still convert");
    require(plan.dropped.size() == 7, "Every dropped setting must be listed");
    require(plan.dropped[0].memberId == "architect" && plan.dropped[0].setting == "maxAttempts"
            && plan.dropped[1].setting == "fallback" && plan.dropped[2].setting == "dependencies"
            && plan.dropped[3].setting == "instructions" && plan.dropped[4].memberId == "agent-3"
            && plan.dropped[4].setting == "maxAttempts" && plan.dropped[5].setting == "fallback"
            && plan.dropped[6].setting == "dependencies",
        "Dropped settings must be ordered by member position and then by setting");
    require(hasDropped(plan, "architect", "fallback", "fb-provider/fb-model")
            && hasDropped(plan, "architect", "dependencies", "agent-2")
            && hasDropped(plan, "agent-3", "dependencies", "agent-2, agent-4"),
        "Legacy values must be rendered readably");
    for (const auto& assignment : plan.roster) {
        require(assignment.dependencies.empty() && assignment.maxAttempts == 1
                && !assignment.fallbackProviderId.has_value()
                && !assignment.fallbackModelId.has_value(),
            "No converted member may keep a setting the Manifest workflow ignores");
    }
}

void testAnIncompleteFallbackIsStillReportedAsDropped()
{
    auto modelOnly = loneWorkspaceLead();
    modelOnly.front().fallbackModelId = "model-9";
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, modelOnly);
    require(plan.converts() && hasDropped(plan, "architect", "fallback", "/model-9")
            && !plan.roster.front().fallbackModelId.has_value(),
        "A half-configured fallback must be reported and removed, never silently carried over");

    auto providerOnly = loneWorkspaceLead();
    providerOnly.front().fallbackProviderId = "provider-9";
    const auto other = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, providerOnly);
    require(other.converts() && hasDropped(other, "architect", "fallback", "provider-9/")
            && !other.roster.front().fallbackProviderId.has_value(),
        "A half-configured fallback must be reported whichever half is set");
}

void testASingleMemberWhoseIdCannotBeFrozenIsNotConvertible()
{
    // validateManifestRoster accepts any one-member roster; the routing record's own
    // integrity check is the only guard against a member id the record cannot hold.
    auto source = loneWorkspaceLead();
    source.front().stageType = std::string(81, 'a');
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.reasonCode == "legacy_roster.manifest_roster_invalid",
        "A member id the routing record cannot hold must not reach a Run");
    requireNothingIsConverted(plan, "An unfreezable member must produce no conversion");
}

void testALoneMemberIsTheLeadWhateverItsFlagSays()
{
    // ManifestRouting::fromAssignments makes the sole member the lead regardless of its
    // flag; the adapter must agree. A lone workspace member that is neither flagged nor
    // named "architect" (the legacy runner would refuse it for having no lead) converts.
    auto source = loneWorkspaceLead();
    source.front().stageType = "analysis";
    source.front().leadArchitect = false;
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.disposition == LegacyRosterDisposition::ConvertToManifestSingle
            && plan.roster.size() == 1 && plan.roster.front().stageType == "analysis"
            && plan.roster.front().leadArchitect,
        "A lone member is the lead whatever its flag says, and the plan must flag it");
}

void testTheLeadNeedNotBeTheFirstMember()
{
    // Real rosters all put the lead first; the policy must not depend on that.
    auto source = proposalRoster();
    source[0].position = 2;
    source[2].position = 0;
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.converts() && plan.roster.size() == 4 && plan.roster[0].stageType == "agent-3"
            && plan.roster[1].stageType == "agent-2" && plan.roster[2].stageType == "architect"
            && plan.roster[3].stageType == "agent-4",
        "The roster keeps position order whatever the lead's position is");
    for (const auto& assignment : plan.roster) {
        require(assignment.leadArchitect == (assignment.stageType == "architect"),
            "Exactly the configured lead must be flagged as the lead");
    }
    require(hasDropped(plan, "architect", "maxAttempts", "2")
            && hasDropped(plan, "architect", "instructions", "41 characters")
            && plan.dropped.size() == 2,
        "The lead's own settings are the ones reported, wherever it sits");
}

void testTheCanonicalArchitectStageIsTheLeadWhenNoFlagIsSet()
{
    auto source = proposalRoster();
    source[0].leadArchitect = false;
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(plan.converts() && plan.roster[0].stageType == "architect" && plan.roster[0].leadArchitect,
        "Like the legacy runner, the canonical architect stage is the lead when no member is flagged");
}

// ---- shapes that stay legacy ------------------------------------------------------

void testTextShapesStayOnTheLegacyRunner()
{
    const auto lone = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, loneTextArchitect());
    require(lone.shape == LegacyRosterShape::LoneTextArchitect
            && lone.disposition == LegacyRosterDisposition::StayLegacy
            && lone.reasonCode == "legacy_roster.text_roster_stays_legacy",
        "A lone text architect (S1) must stay legacy");
    requireNothingIsConverted(lone, "A lone text architect must produce no conversion");

    const auto text = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, textRoster());
    require(text.shape == LegacyRosterShape::TextRoster
            && text.disposition == LegacyRosterDisposition::StayLegacy
            && text.reasonCode == "legacy_roster.text_roster_stays_legacy",
        "A text roster (S3) must stay legacy");
    requireNothingIsConverted(text, "A text roster must produce no conversion");

    auto mixed = proposalRoster();
    mixed[3].workspaceAccess = false;
    const auto mixedPlan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, mixed);
    require(mixedPlan.shape == LegacyRosterShape::MixedRoster
            && mixedPlan.disposition == LegacyRosterDisposition::StayLegacy,
        "A roster mixing workspace and text workers must stay legacy, never be half converted");
    requireNothingIsConverted(mixedPlan, "A mixed roster must produce no conversion");

    auto textWithWorkspaceLead = textRoster();
    textWithWorkspaceLead[0].workspaceAccess = true;
    const auto leadOnly = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, textWithWorkspaceLead);
    require(leadOnly.shape == LegacyRosterShape::TextRoster
            && leadOnly.disposition == LegacyRosterDisposition::StayLegacy,
        "Workers decide the shape: text workers keep the roster legacy whatever the lead's flag");
}

// ---- snapshots that cannot be converted -------------------------------------------

void testUnsupportedSourcesAndMalformedSnapshotsAreNotConvertible()
{
    for (const char* workflow : {"execution-manifest-v1", "mock-sequential-v1", "unknown-v9", ""}) {
        const auto plan = LegacyRosterAdapter::buildConversionPlan(workflow, proposalRoster());
        require(plan.disposition == LegacyRosterDisposition::NotConvertible
                && plan.reasonCode == "legacy_roster.unsupported_source_workflow",
            "Only execution-profile and retired read-only Runs have a legacy roster to convert");
        requireNothingIsConverted(plan, "An unsupported source must produce no conversion");
    }

    const auto empty = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, {});
    require(empty.reasonCode == "legacy_roster.empty_snapshot",
        "An empty snapshot is not convertible");
    requireNothingIsConverted(empty, "An empty snapshot must produce no conversion");

    auto twoLeads = proposalRoster();
    twoLeads[1].leadArchitect = true;
    const auto multipleLeads = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, twoLeads);
    require(multipleLeads.reasonCode == "legacy_roster.multiple_lead_architects",
        "Two flagged leads are ambiguous");
    requireNothingIsConverted(multipleLeads, "An ambiguous lead must produce no conversion");

    auto noLead = proposalRoster();
    noLead[0].leadArchitect = false;
    noLead[0].stageType = "planner";
    const auto missingLead = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, noLead);
    require(missingLead.reasonCode == "legacy_roster.no_lead_architect",
        "A roster without a flagged or canonical lead is not convertible");
    requireNothingIsConverted(missingLead, "A roster without a lead must produce no conversion");

    auto duplicateId = proposalRoster();
    duplicateId[2].stageType = "agent-2";
    const auto duplicates = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, duplicateId);
    require(duplicates.reasonCode == "legacy_roster.manifest_roster_invalid",
        "A duplicate member id must fail the roster rules instead of reaching a Run");
    requireNothingIsConverted(duplicates, "A duplicate member must produce no conversion");

    auto duplicatePosition = proposalRoster();
    duplicatePosition[2].position = duplicatePosition[1].position;
    const auto samePosition = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, duplicatePosition);
    require(samePosition.reasonCode == "legacy_roster.manifest_roster_invalid",
        "A duplicate position must fail the roster rules");
    requireNothingIsConverted(samePosition, "A duplicate position must produce no conversion");

    auto workspaceLead = proposalRoster();
    workspaceLead[0].workspaceAccess = true;
    const auto leadInRoster = LegacyRosterAdapter::buildConversionPlan(
        kExecutionProfileWorkflowId, workspaceLead);
    require(leadInRoster.shape == LegacyRosterShape::ProposalRoster
            && leadInRoster.reasonCode == "legacy_roster.workspace_lead_in_roster",
        "A multi-member roster whose lead has workspace access is ambiguous: the lead only plans");
    requireNothingIsConverted(leadInRoster, "A workspace lead in a roster must produce no conversion");
}

// ---- audit and purity -------------------------------------------------------------

void testTheDecisionReadsOnlyTheSnapshotAndNeverChangesIt()
{
    const auto source = proposalRoster();
    const auto before = source;
    const auto first = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    const auto second = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, source);
    require(samePlan(first, second), "The decision must be deterministic");
    require(source.size() == before.size(), "The source snapshot must not change");
    for (std::size_t index = 0; index < source.size(); ++index) {
        require(sameAssignment(source[index], before[index]), "The source snapshot must not change");
    }

    auto shuffled = source;
    std::reverse(shuffled.begin(), shuffled.end());
    const auto reordered = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, shuffled);
    require(samePlan(first, reordered),
        "The decision depends on the snapshot's content, not on the order it was handed over in");
}

void testSourceFingerprintBindsEveryFieldOfTheSnapshot()
{
    const auto base = proposalRoster();
    const auto baseline = LegacyRosterAdapter::sourceSnapshotFingerprint(kExecutionProfileWorkflowId, base);
    require(baseline.size() == 64
            && baseline == LegacyRosterAdapter::sourceSnapshotFingerprint(kExecutionProfileWorkflowId, base),
        "The fingerprint must be a stable SHA-256");
    // A pinned golden: the canonical form is part of the audit contract. Changing it
    // needs a new policy id (legacy-roster-adapter.v2), never an edit of this value.
    require(baseline == "21e40058ca22b9d5bec209c24de613b8fd6c9c491be465a92801004f7b6ec91d",
        ("The canonical fingerprint of the real proposal roster changed: " + baseline).c_str());

    const std::vector<std::pair<const char*, std::function<void(std::vector<StageModelAssignment>&)>>> edits {
        {"stage type", [](auto& roster) { roster[1].stageType = "agent-9"; }},
        {"provider", [](auto& roster) { roster[1].providerId = "other"; }},
        {"model", [](auto& roster) { roster[1].modelId = "other"; }},
        {"timeout", [](auto& roster) { roster[1].timeout = 1ms; }},
        {"agent name", [](auto& roster) { roster[1].agentName = "Named"; }},
        {"role", [](auto& roster) { roster[1].role = "Other role"; }},
        {"instructions", [](auto& roster) { roster[1].instructions += " "; }},
        {"position", [](auto& roster) { roster[3].position = 9; }},
        {"lead", [](auto& roster) { roster[1].leadArchitect = true; }},
        {"workspace", [](auto& roster) { roster[1].workspaceAccess = false; }},
        {"dependency", [](auto& roster) { roster[1].dependencies = {"architect"}; }},
        {"attempts", [](auto& roster) { roster[1].maxAttempts = 2; }},
        {"fallback provider", [](auto& roster) { roster[1].fallbackProviderId = "fb"; }},
        {"fallback model", [](auto& roster) { roster[1].fallbackModelId = "fb"; }},
        {"membership", [](auto& roster) { roster.pop_back(); }},
    };
    for (const auto& [name, edit] : edits) {
        auto changed = base;
        edit(changed);
        require(LegacyRosterAdapter::sourceSnapshotFingerprint(kExecutionProfileWorkflowId, changed)
                != baseline,
            (std::string("The fingerprint ignored a change of ") + name).c_str());
    }
    require(LegacyRosterAdapter::sourceSnapshotFingerprint("read-only-analysis-v1", base) != baseline,
        "The fingerprint must bind the source workflow id");
}

void testStableMachineStringsOfShapesAndDispositions()
{
    require(std::string(toString(LegacyRosterShape::LoneWorkspaceLead)) == "lone_workspace_lead"
            && std::string(toString(LegacyRosterShape::ProposalRoster)) == "proposal_roster"
            && std::string(toString(LegacyRosterShape::LoneTextArchitect)) == "lone_text_architect"
            && std::string(toString(LegacyRosterShape::TextRoster)) == "text_roster"
            && std::string(toString(LegacyRosterShape::MixedRoster)) == "mixed_roster"
            && std::string(toString(LegacyRosterShape::Unclassified)) == "unclassified",
        "Shape names are persisted audit vocabulary and must stay stable");
    require(std::string(toString(LegacyRosterDisposition::ConvertToManifestSingle))
                    == "convert_to_manifest_single"
            && std::string(toString(LegacyRosterDisposition::ConvertToManifestRoster))
                    == "convert_to_manifest_roster"
            && std::string(toString(LegacyRosterDisposition::StayLegacy)) == "stay_legacy"
            && std::string(toString(LegacyRosterDisposition::NotConvertible)) == "not_convertible",
        "Disposition names are persisted audit vocabulary and must stay stable");
}

// ---- the retired read-only migration moved here unchanged -------------------------

void testRetiredReadOnlyMigrationRestoresTheRetiredRunnersSemantics()
{
    require(LegacyRosterAdapter::migrateRetiredReadOnlyAssignments({}).empty(),
        "An empty snapshot migrates to an empty one");

    auto second = member("second", "p", "m", "Second", true, false, 5);
    second.maxAttempts = 4;
    second.fallbackProviderId = "fb-p";
    second.fallbackModelId = "fb-m";
    auto first = member("first", "p", "m", "", true, false, 1);
    const auto migrated = LegacyRosterAdapter::migrateRetiredReadOnlyAssignments({second, first});
    require(migrated.size() == 2 && migrated[0].stageType == "first" && migrated[1].stageType == "second",
        "The migration orders members by position");
    require(migrated[0].leadArchitect && !migrated[1].leadArchitect,
        "Only the first member becomes the lead");
    for (const auto& assignment : migrated) {
        require(assignment.workspaceAccess && assignment.maxAttempts == 1
                && !assignment.fallbackProviderId.has_value()
                && !assignment.fallbackModelId.has_value(),
            "Every member regains workspace access with one attempt and no fallback");
    }
    require(migrated[0].agentName == "Read-only analysis"
            && migrated[0].role == "Workspace analysis agent" && migrated[1].role == "Second",
        "An empty name/role is defaulted on the lead; configured values are never overwritten");
}

// ---- the plan is valid input for the ordinary router ------------------------------

void testConvertedRostersAreAcceptedByTheOrdinaryWorkflowRouter()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryExecutionProfileRepository profiles;
    ProviderCatalogService catalog {providers, models};
    const auto leadProvider = catalog.createProvider(
        {"Lead", "openai", "https://example.invalid", true, std::nullopt});
    const auto workerProvider = catalog.createProvider(
        {"Worker", "anthropic", "https://example.invalid", true, std::nullopt});
    require(leadProvider.success() && workerProvider.success(), "Catalog setup failed");
    const auto leadModel = catalog.createModel(
        {leadProvider.value(), "lead-model", "lead-model", 128000, 8192, 1000000, 3000000, 0, 0, true});
    const auto workerModel = catalog.createModel(
        {workerProvider.value(), "worker-model", "worker-model", 128000, 8192, 1000000, 3000000, 0, 0, true});
    require(leadModel.success() && workerModel.success(), "Catalog setup failed");

    WorkflowRouter router {profiles, providers, models};

    const auto readOnly = LegacyRosterAdapter::buildConversionPlan(
        kRetiredReadOnlyAnalysisWorkflowId, readOnlySnapshot(leadProvider.value(), leadModel.value()));
    const auto roster = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId,
        proposalRoster(leadProvider.value(), leadModel.value(), workerProvider.value(), workerModel.value()));
    require(readOnly.converts() && roster.converts(), "Both fixtures must convert");

    for (const auto* plan : {&readOnly, &roster}) {
        StartRunRequest request;
        request.projectId = "project-1";
        request.title = "Converted";
        request.task = "Task";
        request.workflowId = plan->targetWorkflowId;
        request.assignmentOverrides = plan->roster;
        const auto resolved = router.resolvePlan(request);
        require(resolved.success(),
            "A conversion plan must be accepted by the ordinary router without any special path");
        require(resolved.value().workflowId == kExecutionManifestWorkflowId
                && resolved.value().assignments.size() == plan->roster.size()
                && !resolved.value().executionProfileId.has_value(),
            "A converted Run must resolve to a per-Run Manifest roster, not a saved profile");
    }
}

} // namespace

int runLegacyRosterAdapterTests()
{
    try {
        testRetiredReadOnlySnapshotConvertsToASingleAssignmentRun();
        testLoneWorkspaceLeadOfAProfileRunConvertsAndReportsDroppedSettings();
        testProposalRosterConvertsAndReportsExactlyTheLeadsDroppedSettings();
        testEveryDroppedSettingIsReportedInMemberOrder();
        testALoneMemberIsTheLeadWhateverItsFlagSays();
        testTheLeadNeedNotBeTheFirstMember();
        testAnIncompleteFallbackIsStillReportedAsDropped();
        testASingleMemberWhoseIdCannotBeFrozenIsNotConvertible();
        testTheCanonicalArchitectStageIsTheLeadWhenNoFlagIsSet();
        testTextShapesStayOnTheLegacyRunner();
        testUnsupportedSourcesAndMalformedSnapshotsAreNotConvertible();
        testTheDecisionReadsOnlyTheSnapshotAndNeverChangesIt();
        testSourceFingerprintBindsEveryFieldOfTheSnapshot();
        testStableMachineStringsOfShapesAndDispositions();
        testRetiredReadOnlyMigrationRestoresTheRetiredRunnersSemantics();
        testConvertedRostersAreAcceptedByTheOrdinaryWorkflowRouter();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "LegacyRosterAdapter tests failed: " << exception.what() << '\n';
        return 1;
    }
}
