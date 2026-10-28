// Unit tests for AG-17.7C's pure conversion pieces: the target-roster and confirmation
// fingerprints, the safe-automatic-Retry predicate, the durable LegacyConversionRecord (its
// integrity, its consistency with the Run it describes) and the strict `dropped` JSON. No
// repository and no provider: the repository atomicity and the commands have their own tests.
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/execution/LegacyConversionRecord.hpp"
#include "ai/application/execution/LegacyRosterAdapter.hpp"

#include <algorithm>
#include <chrono>
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

StageModelAssignment member(const std::string& stageType, const std::string& role, bool lead,
    bool workspace, std::uint32_t position)
{
    StageModelAssignment value;
    value.stageType = stageType;
    value.providerId = "provider-" + stageType;
    value.modelId = "model-" + stageType;
    value.role = role;
    value.leadArchitect = lead;
    value.workspaceAccess = workspace;
    value.position = position;
    value.timeout = 120000ms;
    return value;
}

// The real S4 shape: a lead without workspace access (attempts 2, instructions) + 3 workers.
std::vector<StageModelAssignment> proposalRoster()
{
    auto lead = member("architect", "Software Architect", true, false, 0);
    lead.maxAttempts = 2;
    lead.instructions = "Plan the work.";
    std::vector<StageModelAssignment> roster {lead};
    for (std::uint32_t index = 1; index <= 3; ++index) {
        roster.push_back(member("agent-" + std::to_string(index + 1), "Implementation specialist",
            false, true, index));
    }
    return roster;
}

// The retired read-only snapshot: a safe lone workspace lead, nothing dropped.
std::vector<StageModelAssignment> readOnlySnapshot()
{
    auto analysis = member("analysis", "Read-only workspace analysis agent", false, false, 0);
    analysis.agentName = "Read-only analysis";
    return {analysis};
}

LegacyRosterConversionPlan s4Plan() { return LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, proposalRoster()); }
LegacyRosterConversionPlan safeS2Plan()
{
    return LegacyRosterAdapter::buildConversionPlan(kRetiredReadOnlyAnalysisWorkflowId, readOnlySnapshot());
}

LegacyConversionRecord explicitRecord(const std::string& sourceRunId = "source-run",
    const std::string& runId = "new-run")
{
    const auto plan = s4Plan();
    require(plan.converts(), "test fixture: the proposal roster must convert");
    const auto draft = makeLegacyConversionDraft(sourceRunId, plan, LegacyConversionConfirmationMode::ExplicitUser);
    require(draft.success(), "test fixture: draft");
    const auto now = std::chrono::system_clock::now();
    return makeLegacyConversionRecord(draft.value(), runId, now, now);
}

LegacyConversionRecord safeRecord()
{
    const auto plan = safeS2Plan();
    const auto draft = makeLegacyConversionDraft("source-run", plan, LegacyConversionConfirmationMode::NotRequiredSafeRetry);
    require(draft.success(), "test fixture: safe draft");
    // A confirmation time handed in for a safe retry must be dropped by the builder.
    const auto now = std::chrono::system_clock::now();
    return makeLegacyConversionRecord(draft.value(), "new-run", now, now);
}

Run convertedRun(const LegacyConversionRecord& record, const std::vector<StageModelAssignment>& roster)
{
    Run run;
    run.id = record.runId;
    run.parentRunId = record.sourceRunId;
    run.workflowId = kExecutionManifestWorkflowId;
    run.assignmentSnapshot = roster;
    run.createdAt = record.createdAt;
    return run;
}

bool hasProblem(const std::optional<std::string>& problem) { return problem.has_value(); }

// Recomputes the confirmation fingerprint from the record's own fields. Most fields are covered
// by that fingerprint, so a test that breaks ONE rule must reseal the record - otherwise the
// fingerprint check rejects it first and the rule under test is never exercised.
LegacyConversionRecord resealed(LegacyConversionRecord record)
{
    record.confirmationFingerprint = LegacyRosterAdapter::confirmationFingerprint(record.sourceRunId,
        record.sourceWorkflowId, record.policyId, record.reasonCode, record.sourceSnapshotFingerprint,
        kExecutionManifestWorkflowId, record.targetRosterFingerprint, record.dropped);
    return record;
}

bool problemMentions(const std::optional<std::string>& problem, const char* part)
{
    return problem.has_value() && problem->find(part) != std::string::npos;
}

// ---- fingerprints -----------------------------------------------------------------

void testTheTargetRosterFingerprintCertifiesTheConvertedRosterOnly()
{
    const auto plan = s4Plan();
    require(plan.targetRosterFingerprint.size() == 64
            && plan.targetRosterFingerprint == LegacyRosterAdapter::rosterFingerprint(plan.roster),
        "A converting plan must carry the fingerprint of its own roster");
    // A pinned golden: the canonical form is audit vocabulary. Changing it needs a new policy id.
    require(plan.targetRosterFingerprint == "c84c0bcbcf64656565538723a9607fa752553b56ac9771fb86baec985485ed2f",
        ("The canonical target-roster fingerprint changed: " + plan.targetRosterFingerprint).c_str());

    auto shuffled = plan.roster;
    std::reverse(shuffled.begin(), shuffled.end());
    require(LegacyRosterAdapter::rosterFingerprint(shuffled) == plan.targetRosterFingerprint,
        "The roster fingerprint must not depend on the order the roster was handed over in");
    auto changed = plan.roster;
    changed[1].instructions = "different";
    require(LegacyRosterAdapter::rosterFingerprint(changed) != plan.targetRosterFingerprint,
        "A changed roster must change the fingerprint");

    require(LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId,
                [] { auto text = proposalRoster(); text[1].workspaceAccess = false; return text; }())
                .targetRosterFingerprint.empty(),
        "A plan that does not convert has no target roster to certify");
    require(plan.targetRosterFingerprint != plan.sourceSnapshotFingerprint,
        "The target roster and the source snapshot are different things");
}

void testTheConfirmationFingerprintBindsEveryInputAndTheSourceRun()
{
    const auto plan = s4Plan();
    const auto base = LegacyRosterAdapter::confirmationFingerprint("source-run", plan);
    require(base.size() == 64 && !plan.dropped.empty(), "A converting plan has a confirmation fingerprint");
    require(base == LegacyRosterAdapter::confirmationFingerprint("source-run", plan),
        "The confirmation fingerprint must be deterministic");

    // The headline requirement: not transferable between two Runs with an identical snapshot.
    require(LegacyRosterAdapter::confirmationFingerprint("another-source-run", plan) != base,
        "A confirmation must be bound to ONE source Run, not to a snapshot alone");

    const auto fingerprint = [&](const LegacyRosterConversionPlan& edited) {
        return LegacyRosterAdapter::confirmationFingerprint("source-run", edited);
    };
    const std::vector<std::pair<const char*, std::function<void(LegacyRosterConversionPlan&)>>> edits {
        {"source workflow", [](auto& p) { p.sourceWorkflowId = kRetiredReadOnlyAnalysisWorkflowId; }},
        {"policy id", [](auto& p) { p.policyId = "legacy-roster-adapter.v2"; }},
        {"reason code", [](auto& p) { p.reasonCode = kLegacyRosterReasonConvertedLoneWorkspaceLead; }},
        {"source snapshot fingerprint", [](auto& p) { p.sourceSnapshotFingerprint[0] = p.sourceSnapshotFingerprint[0] == 'a' ? 'b' : 'a'; }},
        {"target workflow", [](auto& p) { p.targetWorkflowId = "execution-manifest-v2"; }},
        {"target roster fingerprint", [](auto& p) { p.targetRosterFingerprint[0] = p.targetRosterFingerprint[0] == 'a' ? 'b' : 'a'; }},
        {"dropped member", [](auto& p) { p.dropped[0].memberId += "x"; }},
        {"dropped setting", [](auto& p) { p.dropped[0].setting = "fallback"; }},
        {"dropped value", [](auto& p) { p.dropped[0].legacyValue += "0"; }},
        {"dropped reason", [](auto& p) { p.dropped[0].reason += "."; }},
        {"dropped count", [](auto& p) { p.dropped.pop_back(); }},
        {"dropped order", [](auto& p) { std::swap(p.dropped[0], p.dropped[1]); }},
    };
    for (const auto& [name, edit] : edits) {
        auto edited = plan;
        edit(edited);
        require(fingerprint(edited) != base,
            (std::string("The confirmation fingerprint ignored a change of ") + name).c_str());
    }

    auto notConverting = plan;
    notConverting.disposition = LegacyRosterDisposition::StayLegacy;
    require(LegacyRosterAdapter::confirmationFingerprint("source-run", notConverting).empty(),
        "A plan that does not convert has nothing to confirm");
}

// ---- the safe automatic Retry predicate ---------------------------------------------

void testOnlyASafeLoneWorkspaceLeadMayRetryWithoutConfirmation()
{
    const auto safe = safeS2Plan();
    require(LegacyRosterAdapter::isSafeAutomaticRetry(safe)
            && safe.shape == LegacyRosterShape::LoneWorkspaceLead && safe.dropped.empty(),
        "A retired read-only snapshot is a safe automatic Retry");

    // Each of the four conditions, broken on its own, must close the safe branch.
    auto notSingle = safe;
    notSingle.disposition = LegacyRosterDisposition::ConvertToManifestRoster;
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(notSingle), "disposition is part of the safe condition");
    auto notLone = safe;
    notLone.shape = LegacyRosterShape::ProposalRoster;
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(notLone), "shape is part of the safe condition");
    auto withDropped = safe;
    withDropped.dropped.push_back({"analysis", "instructions", "3 characters", "reason"});
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(withDropped), "an empty dropped list is part of the safe condition");
    auto otherTarget = safe;
    otherTarget.targetWorkflowId = "execution-profile-v1";
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(otherTarget), "the target workflow is part of the safe condition");

    // Nothing else is ever safe: not a proposal roster, not a text roster, not a refusal.
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(s4Plan()), "A proposal roster is never a safe automatic Retry");
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(LegacyRosterConversionPlan {}), "An empty plan is not safe");
    auto text = member("architect", "Software Architect", true, false, 0);
    require(!LegacyRosterAdapter::isSafeAutomaticRetry(
                LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, {text})),
        "A lone text architect stays legacy and is never safe");

    // A lone workspace lead from a profile Run with instructions drops them: not safe.
    auto lone = member("architect", "Workspace agent", true, true, 0);
    lone.instructions = "Be careful.";
    const auto withInstructions = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, {lone});
    require(withInstructions.converts() && !withInstructions.dropped.empty()
            && !LegacyRosterAdapter::isSafeAutomaticRetry(withInstructions),
        "A lone workspace lead with instructions needs a confirmation");
}

// ---- the draft and the record -------------------------------------------------------

void testDraftsAndRecordsCarryExactlyTheDecisionAndNeverClaimAFalseConfirmation()
{
    const auto plan = s4Plan();
    const auto draft = makeLegacyConversionDraft("source-run", plan, LegacyConversionConfirmationMode::ExplicitUser);
    require(draft.success() && draft.value().sourceRunId == "source-run"
            && draft.value().policyId == "legacy-roster-adapter.v1"
            && draft.value().reasonCode == "legacy_roster.converted_proposal_roster"
            && draft.value().sourceWorkflowId == kExecutionProfileWorkflowId
            && draft.value().dropped.size() == plan.dropped.size()
            && draft.value().targetRosterFingerprint == plan.targetRosterFingerprint
            && draft.value().confirmationFingerprint == LegacyRosterAdapter::confirmationFingerprint("source-run", plan),
        "A draft carries the decision, the fingerprints and the dropped list");

    auto stays = plan;
    stays.disposition = LegacyRosterDisposition::StayLegacy;
    require(!makeLegacyConversionDraft("source-run", stays, LegacyConversionConfirmationMode::ExplicitUser).success(),
        "A plan that does not convert has nothing to record");
    require(!makeLegacyConversionDraft("source-run", plan, LegacyConversionConfirmationMode::NotRequiredSafeRetry).success(),
        "A proposal roster can never be recorded as a safe automatic Retry");

    const auto when = std::chrono::system_clock::now();
    const auto explicitOne = makeLegacyConversionRecord(draft.value(), "new-run", when, when);
    require(explicitOne.confirmedAt.has_value() && explicitOne.runId == "new-run"
            && explicitOne.confirmationMode == LegacyConversionConfirmationMode::ExplicitUser,
        "An explicit confirmation carries the time the Backend stamped");
    require(!safeRecord().confirmedAt.has_value()
            && safeRecord().confirmationMode == LegacyConversionConfirmationMode::NotRequiredSafeRetry,
        "A safe automatic Retry must never claim a confirmation, whatever time is handed in");
    require(std::string(toString(LegacyConversionConfirmationMode::ExplicitUser)) == "explicit_user"
            && std::string(toString(LegacyConversionConfirmationMode::NotRequiredSafeRetry))
                == "not_required_safe_retry"
            && legacyConversionConfirmationModeFromString("explicit_user")
                == LegacyConversionConfirmationMode::ExplicitUser
            && !legacyConversionConfirmationModeFromString("confirmed").has_value(),
        "The confirmation modes are persisted vocabulary and must stay stable");
}

void testRecordIntegrityRejectsEveryDefect()
{
    require(!hasProblem(legacyConversionRecordProblem(explicitRecord())),
        "A genuine explicit record must be consistent");
    require(!hasProblem(legacyConversionRecordProblem(safeRecord())),
        "A genuine safe-retry record must be consistent");

    require(!hasProblem(legacyConversionRecordProblem(resealed(explicitRecord()))),
        "Resealing an untouched record must change nothing, or the isolation below proves nothing");

    // Each defect breaks exactly one rule. `reseal` keeps the confirmation fingerprint consistent
    // with the broken field, so ONLY the rule named by `expected` can reject the record.
    struct Defect {
        const char* name;
        const char* expected;
        bool reseal;
        std::function<void(LegacyConversionRecord&)> mutate;
    };
    const std::vector<Defect> defects {
        {"empty run id", "no valid Run", true, [](auto& r) { r.runId.clear(); }},
        {"empty source run id", "no valid Run", true, [](auto& r) { r.sourceRunId.clear(); }},
        {"an oversized run id", "no valid Run", true, [](auto& r) { r.runId = std::string(129, 'r'); }},
        {"an oversized source run id", "no valid Run", true, [](auto& r) { r.sourceRunId = std::string(129, 's'); }},
        {"a record that is its own source", "its own source", true, [](auto& r) { r.sourceRunId = r.runId; }},
        {"unknown source workflow", "unknown source workflow", true,
            [](auto& r) { r.sourceWorkflowId = "execution-manifest-v1"; }},
        {"empty policy id", "no known adapter policy", true, [](auto& r) { r.policyId.clear(); }},
        {"an oversized policy id", "no known adapter policy", true, [](auto& r) { r.policyId = std::string(65, 'p'); }},
        {"a policy id with a control character", "no known adapter policy", true,
            [](auto& r) { r.policyId = "legacy-roster-adapter.v1\n"; }},
        // Well formed, but not a policy this build knows: the set of policies is closed.
        {"a well-formed policy id from a future adapter", "no known adapter policy", true,
            [](auto& r) { r.policyId = "legacy-roster-adapter.v2"; }},
        {"a policy id that only looks like the known one", "no known adapter policy", true,
            [](auto& r) { r.policyId = "legacy-roster-adapter.v1 "; }},
        {"the same setting of one member dropped twice", "same setting of a member twice", true,
            [](auto& r) { r.dropped.push_back(r.dropped[0]); }},
        {"a confirmation that precedes the record", "precedes the record", false,
            [](auto& r) { r.confirmedAt = r.createdAt - std::chrono::seconds(2); }},
        {"a reason code that is not a conversion", "not a conversion", true,
            [](auto& r) { r.reasonCode = "legacy_roster.text_roster_stays_legacy"; }},
        {"a retired read-only Run as a proposal roster", "retired read-only", true,
            [](auto& r) { r.sourceWorkflowId = kRetiredReadOnlyAnalysisWorkflowId; }},
        {"a non-hex source fingerprint", "not a SHA-256", true,
            [](auto& r) { r.sourceSnapshotFingerprint = std::string(64, 'Z'); }},
        {"an upper-case source fingerprint", "not a SHA-256", true,
            [](auto& r) { r.sourceSnapshotFingerprint = std::string(64, 'A'); }},
        {"a short target fingerprint", "not a SHA-256", true, [](auto& r) { r.targetRosterFingerprint = "abc"; }},
        {"a non-hex target fingerprint", "not a SHA-256", true,
            [](auto& r) { r.targetRosterFingerprint = std::string(64, 'g'); }},
        {"a short confirmation fingerprint", "not a SHA-256", false,
            [](auto& r) { r.confirmationFingerprint = "abc"; }},
        {"too many dropped entries", "too many dropped", true, [](auto& r) {
            const auto first = r.dropped[0];
            r.dropped.assign(kLegacyConversionMaxDroppedEntries + 1, first);
        }},
        {"an explicit record without a time", "carries no confirmation time", false,
            [](auto& r) { r.confirmedAt.reset(); }},
        {"a tampered dropped value", "does not match", false, [](auto& r) { r.dropped[0].legacyValue = "9"; }},
        {"a tampered fingerprint", "does not match", false,
            [](auto& r) { r.confirmationFingerprint = std::string(64, 'a'); }},
    };
    for (const auto& defect : defects) {
        auto record = explicitRecord();
        defect.mutate(record);
        if (defect.reseal) record = resealed(std::move(record));
        const auto problem = legacyConversionRecordProblem(record);
        require(problemMentions(problem, defect.expected),
            (std::string("A defective record was not rejected by its own rule: ") + defect.name).c_str());
    }

    auto safeWithTime = safeRecord();
    safeWithTime.confirmedAt = std::chrono::system_clock::now();
    require(problemMentions(legacyConversionRecordProblem(safeWithTime), "must not claim a confirmation time"),
        "A safe automatic Retry must never carry a confirmation time");
    auto safeWithDropped = safeRecord();
    safeWithDropped.dropped.push_back({"analysis", "instructions", "3 characters", "reason"});
    require(problemMentions(legacyConversionRecordProblem(resealed(safeWithDropped)),
                "only a safe lone workspace lead"),
        "A conversion that dropped anything cannot skip the confirmation");
    auto safeProposal = explicitRecord();
    safeProposal.confirmationMode = LegacyConversionConfirmationMode::NotRequiredSafeRetry;
    safeProposal.confirmedAt.reset();
    safeProposal.dropped.clear();
    require(problemMentions(legacyConversionRecordProblem(resealed(safeProposal)),
                "only a safe lone workspace lead"),
        "A proposal roster can never skip the confirmation, even with nothing dropped");
}

void testDroppedEntriesNeverHoldInstructionTextOrControlCharacters()
{
    const auto problem = [](LegacyRosterDroppedSetting entry) {
        auto record = explicitRecord();
        record.dropped = {std::move(entry)};
        // The fingerprint is recomputed so only the entry itself can be the defect.
        record.confirmationFingerprint = LegacyRosterAdapter::confirmationFingerprint(
            record.sourceRunId, record.sourceWorkflowId, record.policyId, record.reasonCode,
            record.sourceSnapshotFingerprint, kExecutionManifestWorkflowId,
            record.targetRosterFingerprint, record.dropped);
        return legacyConversionRecordProblem(record).has_value();
    };
    require(!problem({"architect", "instructions", "41 characters", "r"}),
        "An instructions entry is a length");
    require(problem({"architect", "instructions", "Plan the work and keep the secret", "r"}),
        "An instructions entry must never hold text");
    require(problem({"architect", "instructions", "41 characters of secret", "r"}),
        "An instructions entry must hold nothing but the length");
    require(problem({"architect", "instructions", " characters", "r"}), "A length needs digits");
    require(problem({"architect", "maxAttempts", "two", "r"}), "An attempts entry is a number");
    require(!problem({"architect", "maxAttempts", "2", "r"}), "An attempts entry may be a number");
    require(problem({"architect", "fallback", "p/m\nsecond line", "r"}), "No control characters in a value");
    require(problem({"architect", "fallback", "p/m", "reason\n"}), "No control characters in a reason");
    require(problem({"architect", "colour", "blue", "r"}), "An unknown setting is refused");
    require(problem({"", "fallback", "p/m", "r"}), "A dropped entry names its member");
    require(problem({std::string(81, 'a'), "fallback", "p/m", "r"}), "A member id is bounded");
    require(problem({"architect", "fallback", std::string(kLegacyConversionMaxLegacyValueBytes + 1, 'a'), "r"}),
        "A legacy value is bounded");
    require(problem({"architect", "fallback", "p/m", std::string(kLegacyConversionMaxReasonBytes + 1, 'a')}),
        "A reason is bounded");
}

void testTheRecordMustMatchTheRunItDescribes()
{
    const auto record = explicitRecord();
    const auto plan = s4Plan();
    auto run = convertedRun(record, plan.roster);
    require(!hasProblem(legacyConversionAggregateProblem(run, record)),
        "A record and the Run it describes must be consistent");

    auto otherRun = run;
    otherRun.id = "another-run";
    require(hasProblem(legacyConversionAggregateProblem(otherRun, record)), "The record names another Run");
    auto noParent = run;
    noParent.parentRunId.reset();
    require(hasProblem(legacyConversionAggregateProblem(noParent, record)), "A converted Run has a parent");
    auto wrongParent = run;
    wrongParent.parentRunId = "someone-else";
    require(hasProblem(legacyConversionAggregateProblem(wrongParent, record)),
        "The parent must be the conversion's source Run");
    auto legacyRun = run;
    legacyRun.workflowId = kExecutionProfileWorkflowId;
    require(hasProblem(legacyConversionAggregateProblem(legacyRun, record)), "A converted Run is a Manifest Run");
    auto profileRun = run;
    profileRun.executionProfileId = "profile-1";
    require(hasProblem(legacyConversionAggregateProblem(profileRun, record)),
        "A converted Run has no saved execution profile");
    auto otherRoster = run;
    otherRoster.assignmentSnapshot[1].instructions = "edited after the fact";
    require(hasProblem(legacyConversionAggregateProblem(otherRoster, record)),
        "The Run's frozen roster must be exactly the roster the record certifies");
    auto fewer = run;
    fewer.assignmentSnapshot.pop_back();
    require(hasProblem(legacyConversionAggregateProblem(fewer, record)), "A member cannot go missing");

    auto defective = record;
    defective.reasonCode = "legacy_roster.empty_snapshot";
    require(hasProblem(legacyConversionAggregateProblem(run, defective)),
        "The aggregate check includes the record's own integrity");
}

// A record and a Run that agree with each other and with the plan they came from; a test then
// falsifies exactly one claim. `forge` re-derives the roster fingerprint from the Run and reseals
// the confirmation fingerprint, so the record stays SELF-CONSISTENT - the only thing wrong with it
// is that its claim is false, and only the claim checks can notice.
struct Consistent {
    LegacyConversionRecord record;
    Run run;
};

Consistent consistentFor(const LegacyRosterConversionPlan& plan, LegacyConversionConfirmationMode mode)
{
    require(plan.converts(), "test fixture: the plan converts");
    const auto draft = makeLegacyConversionDraft("source-run", plan, mode);
    require(draft.success(), "test fixture: draft");
    // Whole milliseconds, so the persistence-precision cases below have an exact starting point.
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    auto record = makeLegacyConversionRecord(draft.value(), "new-run", now, now);
    auto run = convertedRun(record, plan.roster);
    return {std::move(record), std::move(run)};
}

Consistent forge(Consistent value)
{
    value.record.targetRosterFingerprint = LegacyRosterAdapter::rosterFingerprint(value.run.assignmentSnapshot);
    value.record = resealed(std::move(value.record));
    return value;
}

void testTheRecordsClaimsMustHoldAgainstTheFrozenRoster()
{
    const auto proposal = s4Plan();
    const auto lone = safeS2Plan();
    require(proposal.roster.size() > 1 && lone.roster.size() == 1, "test fixture: roster sizes");

    // The genuine pairs are consistent, including a safe Retry.
    for (const auto& genuine : {consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser),
             consistentFor(lone, LegacyConversionConfirmationMode::ExplicitUser),
             consistentFor(lone, LegacyConversionConfirmationMode::NotRequiredSafeRetry)}) {
        require(!hasProblem(legacyConversionAggregateProblem(genuine.run, genuine.record)),
            "A genuine record and its Run must be consistent");
    }
    const auto untouched = forge(consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser));
    require(!hasProblem(legacyConversionAggregateProblem(untouched.run, untouched.record)),
        "Forging without changing anything must change nothing, or the cases below prove nothing");

    const auto reject = [](const Consistent& value, const char* expected, const char* message) {
        require(problemMentions(legacyConversionAggregateProblem(value.run, value.record), expected), message);
    };

    // A proposal-roster claim over a single-member roster, and a lone-workspace-lead claim over
    // a multi-member roster (explicit mode: the safe-Retry rule is not what rejects them).
    auto proposalOverOne = consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser);
    proposalOverOne.run.assignmentSnapshot = lone.roster;
    reject(forge(proposalOverOne), "does not fit the size of the frozen roster",
        "A proposal-roster conversion cannot certify a single-member roster");
    auto loneOverMany = consistentFor(lone, LegacyConversionConfirmationMode::ExplicitUser);
    loneOverMany.run.assignmentSnapshot = proposal.roster;
    reject(forge(loneOverMany), "does not fit the size of the frozen roster",
        "A lone-workspace-lead conversion cannot certify a multi-member roster");

    // A safe automatic Retry must certify exactly one member - not merely carry an empty `dropped`.
    auto safeOverMany = consistentFor(lone, LegacyConversionConfirmationMode::NotRequiredSafeRetry);
    safeOverMany.run.assignmentSnapshot = proposal.roster;
    reject(forge(safeOverMany), "more than one member",
        "A safe automatic Retry must certify a single-member roster");

    // Every dropped setting belongs to a member the frozen roster really has.
    auto ghost = consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser);
    ghost.record.dropped.push_back({"ghost-member", "instructions", "3 characters", "reason"});
    reject(forge(ghost), "names a member the frozen roster does not have",
        "A dropped setting of a member that is not in the roster is a false audit");
    // ... and the same setting of two DIFFERENT members is fine.
    auto twoMembers = consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser);
    const auto firstMember = twoMembers.record.dropped[0].memberId;
    const auto otherMember = std::find_if(proposal.roster.begin(), proposal.roster.end(),
        [&](const StageModelAssignment& member) { return member.stageType != firstMember; });
    require(otherMember != proposal.roster.end(), "test fixture: a second member");
    auto sameSetting = twoMembers.record.dropped[0];
    sameSetting.memberId = otherMember->stageType;
    twoMembers.record.dropped.push_back(sameSetting);
    twoMembers = forge(std::move(twoMembers));
    require(!hasProblem(legacyConversionAggregateProblem(twoMembers.run, twoMembers.record)),
        "The same setting dropped from two different members is two facts, not a duplicate");

    // The record is created with its Run: the same instant at persistence (millisecond) precision.
    auto otherCreation = consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser);
    otherCreation.run.createdAt += std::chrono::seconds(5);
    reject(otherCreation, "not created together with the Run",
        "A record whose creation time is not the Run's was not written with it");
    auto sameMillisecond = consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser);
    sameMillisecond.run.createdAt += std::chrono::microseconds(500);
    require(!hasProblem(legacyConversionAggregateProblem(sameMillisecond.run, sameMillisecond.record)),
        "Sub-millisecond differences are below the persistence precision and must be accepted");
    auto nextMillisecond = consistentFor(proposal, LegacyConversionConfirmationMode::ExplicitUser);
    nextMillisecond.run.createdAt += std::chrono::milliseconds(1);
    reject(nextMillisecond, "not created together with the Run",
        "A whole millisecond of difference is a real difference");
}

void testTheBackendClockNeverStampsAConfirmationBeforeTheRunItConfirms()
{
    const auto created = std::chrono::system_clock::now();
    const auto later = created + std::chrono::seconds(3);
    const auto earlier = created - std::chrono::seconds(3);
    require(legacyConversionConfirmationTime(later, created) == later,
        "A confirmation after the Run is stamped with the Backend's own clock");
    require(legacyConversionConfirmationTime(created, created) == created,
        "A confirmation at the very instant of the Run is allowed");
    require(legacyConversionConfirmationTime(earlier, created) == created,
        "A clock that stepped backwards must not stamp a confirmation before its Run");

    // What the clamp is for: the record's own integrity rule accepts the clamped time and refuses
    // the raw backwards one.
    auto record = explicitRecord();
    record.createdAt = created;
    record.confirmedAt = legacyConversionConfirmationTime(earlier, record.createdAt);
    require(!hasProblem(legacyConversionRecordProblem(record)),
        "The clamped confirmation time must satisfy the record's integrity rule");
    record.confirmedAt = earlier;
    require(problemMentions(legacyConversionRecordProblem(record), "precedes the record"),
        "The raw backwards confirmation time is exactly what the rule refuses");
}

// ---- the dropped JSON ----------------------------------------------------------------

void testDroppedJsonRoundTripsAndFailsClosedWhenCorrupt()
{
    const auto plan = s4Plan();
    const auto json = legacyConversionDroppedToJson(plan.dropped);
    require(json.success() && json.value().size() <= kLegacyConversionMaxDroppedJsonBytes
            && json.value().find("Plan the work") == std::string::npos,
        "Writing the dropped list must succeed, stay bounded and hold no instruction text");
    const auto back = legacyConversionDroppedFromJson(json.value());
    require(back.success() && back.value().size() == plan.dropped.size()
            && back.value()[0].memberId == plan.dropped[0].memberId
            && back.value()[0].setting == plan.dropped[0].setting
            && back.value()[0].legacyValue == plan.dropped[0].legacyValue
            && back.value()[0].reason == plan.dropped[0].reason
            && back.value()[1].setting == plan.dropped[1].setting,
        "The dropped list must round-trip exactly, in order");
    require(legacyConversionDroppedFromJson("[]").success()
            && legacyConversionDroppedFromJson("[]").value().empty(),
        "An empty list is a valid list");

    const auto corrupt = [](const std::string& text, const char* message) {
        const auto parsed = legacyConversionDroppedFromJson(text);
        require(!parsed.success() && parsed.error()->code == kLegacyConversionRecordCorruptCode, message);
    };
    const std::string entry =
        R"({"memberId":"architect","setting":"fallback","legacyValue":"p/m","reason":"r"})";
    require(legacyConversionDroppedFromJson("[" + entry + "]").success(), "test fixture: a valid entry");
    corrupt("", "Empty text is not a dropped list");
    corrupt("not json", "Text that is not JSON is corrupt");
    corrupt("{}", "An object is not a dropped list");
    corrupt("[1]", "An element that is not an object is corrupt");
    corrupt(R"([{"memberId":"a"}])", "An element with missing fields is corrupt");
    corrupt(R"([{"memberId":"architect","setting":"fallback","legacyValue":"p/m","reason":"r","extra":"x"}])",
        "An element with an extra field is corrupt");
    corrupt(R"([{"memberId":"architect","setting":"fallback","legacyValue":3,"reason":"r"}])",
        "A non-text field is corrupt");
    corrupt(R"([{"memberId":"architect","setting":"colour","legacyValue":"p/m","reason":"r"}])",
        "An unknown setting is corrupt");
    corrupt(R"([{"memberId":"architect","setting":"instructions","legacyValue":"secret text","reason":"r"}])",
        "Instruction text in the audit is corrupt");
    corrupt("[" + entry + "," + std::string("not-json"), "A truncated document is corrupt");

    std::string many = "[";
    for (std::size_t index = 0; index <= kLegacyConversionMaxDroppedEntries; ++index) {
        if (index > 0) many += ",";
        many += entry;
    }
    many += "]";
    corrupt(many, "Too many entries are corrupt");
    corrupt("[" + entry + std::string(kLegacyConversionMaxDroppedJsonBytes, ' ') + "]",
        "An oversized document is corrupt");
}

void testWritingAnOutOfBoundsDroppedListIsRefusedNotTruncated()
{
    std::vector<LegacyRosterDroppedSetting> tooMany(
        kLegacyConversionMaxDroppedEntries + 1, {"architect", "fallback", "p/m", "r"});
    const auto many = legacyConversionDroppedToJson(tooMany);
    require(!many.success() && many.error()->code == kLegacyConversionRecordInvalidCode,
        "Too many dropped entries must be refused");
    const auto longValue = legacyConversionDroppedToJson(
        {{"architect", "fallback", std::string(kLegacyConversionMaxLegacyValueBytes + 1, 'a'), "r"}});
    require(!longValue.success() && longValue.error()->code == kLegacyConversionRecordInvalidCode,
        "An over-long value must be refused, never silently cut");
    const auto notUtf8 = legacyConversionDroppedToJson({{"architect", "fallback", "p/\xFF", "r"}});
    require(!notUtf8.success() && notUtf8.error()->code == kLegacyConversionRecordInvalidCode,
        "Text that is not valid UTF-8 must be refused");

    // Every entry within its own bounds, yet the whole document over the size bound: 256 entries
    // with maximal member ids, values and reasons.
    std::vector<LegacyRosterDroppedSetting> maximal(kLegacyConversionMaxDroppedEntries,
        {std::string(80, 'm'), "fallback", std::string(kLegacyConversionMaxLegacyValueBytes, 'v'),
            std::string(kLegacyConversionMaxReasonBytes, 'r')});
    const auto huge = legacyConversionDroppedToJson(maximal);
    require(!huge.success() && huge.error()->code == kLegacyConversionRecordInvalidCode,
        "A document over the size bound must be refused even when every entry is within its bounds");
    maximal.resize(40);
    require(legacyConversionDroppedToJson(maximal).success(),
        "A document inside the size bound must be accepted");
}

// ---- the adapter bounds what it reports -----------------------------------------------

void testLongLegacyValuesAreBoundedAtACharacterBoundary()
{
    auto roster = proposalRoster();
    // Many dependencies, each with a multi-byte character, to force a cut inside the list.
    for (int index = 0; index < 60; ++index) {
        roster[0].dependencies.push_back("agent-\xC3\xA9-" + std::to_string(index));
    }
    const auto plan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, roster);
    require(plan.converts(), "test fixture: the roster converts");
    const auto dependencies = std::find_if(plan.dropped.begin(), plan.dropped.end(),
        [](const LegacyRosterDroppedSetting& entry) { return entry.setting == "dependencies"; });
    require(dependencies != plan.dropped.end()
            && dependencies->legacyValue.size() <= kLegacyConversionMaxLegacyValueBytes
            && dependencies->legacyValue.size() > kLegacyConversionMaxLegacyValueBytes - 8
            && dependencies->legacyValue.substr(dependencies->legacyValue.size() - 3) == "...",
        "A long value must be bounded and end with an explicit marker");
    // Cut at a character boundary: the recorded text must still be valid UTF-8 JSON.
    require(legacyConversionDroppedToJson(plan.dropped).success(),
        "A bounded value must remain valid UTF-8");

    // The cut position (256 - 3 = 253) lands INSIDE a two-byte character: the value is
    // 251 x 'a', '/', then "\xC3\xA9" at bytes 252-253. A naive cut would leave a dangling lead
    // byte; the bound must step back to the character boundary instead.
    auto inside = proposalRoster();
    inside[0].fallbackProviderId = std::string(251, 'a');
    inside[0].fallbackModelId = "\xC3\xA9" + std::string(20, 'm');
    const auto insidePlan = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, inside);
    const auto insideFallback = std::find_if(insidePlan.dropped.begin(), insidePlan.dropped.end(),
        [](const LegacyRosterDroppedSetting& entry) { return entry.setting == "fallback"; });
    require(insideFallback != insidePlan.dropped.end()
            && insideFallback->legacyValue == std::string(251, 'a') + "/..."
            && legacyConversionDroppedToJson(insidePlan.dropped).success(),
        "A cut inside a multi-byte character must step back to the character boundary");

    // A value at the bound is untouched.
    auto exact = proposalRoster();
    exact[0].fallbackProviderId = std::string(100, 'p');
    exact[0].fallbackModelId = std::string(100, 'm');
    const auto atBound = LegacyRosterAdapter::buildConversionPlan(kExecutionProfileWorkflowId, exact);
    const auto fallback = std::find_if(atBound.dropped.begin(), atBound.dropped.end(),
        [](const LegacyRosterDroppedSetting& entry) { return entry.setting == "fallback"; });
    require(fallback != atBound.dropped.end() && fallback->legacyValue.size() == 201
            && fallback->legacyValue.find("...") == std::string::npos,
        "A value inside the bound must be reported exactly");
}

} // namespace

int runLegacyConversionRecordTests()
{
    try {
        testTheTargetRosterFingerprintCertifiesTheConvertedRosterOnly();
        testTheConfirmationFingerprintBindsEveryInputAndTheSourceRun();
        testOnlyASafeLoneWorkspaceLeadMayRetryWithoutConfirmation();
        testDraftsAndRecordsCarryExactlyTheDecisionAndNeverClaimAFalseConfirmation();
        testRecordIntegrityRejectsEveryDefect();
        testDroppedEntriesNeverHoldInstructionTextOrControlCharacters();
        testTheRecordMustMatchTheRunItDescribes();
        testTheRecordsClaimsMustHoldAgainstTheFrozenRoster();
        testTheBackendClockNeverStampsAConfirmationBeforeTheRunItConfirms();
        testDroppedJsonRoundTripsAndFailsClosedWhenCorrupt();
        testWritingAnOutOfBoundsDroppedListIsRefusedNotTruncated();
        testLongLegacyValuesAreBoundedAtACharacterBoundary();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "LegacyConversionRecord tests failed: " << exception.what() << '\n';
        return 1;
    }
}
