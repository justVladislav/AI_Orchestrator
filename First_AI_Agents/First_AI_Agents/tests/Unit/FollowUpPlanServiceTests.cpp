// PC-5C: the follow-up plan service, the composer processor and its worker, over real in-memory repositories, a scripted source
// reader and a scripted provider. Every test builds its own harness; nothing here touches a file, the network or Qt.

#include "FollowUpServiceTestSupport.hpp"

#include "ai/application/CostCalculation.hpp"

#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace ai::application;
using namespace followup_service_test;
namespace common = ai::common;

namespace {

using S = FollowUpPlanRequestState;

std::string validReply(bool contracts = true) { return replyOfPlanJson(planJsonOf('c', 2, contracts)); }

// A brief of the same Run with another source state (a different baseline), still resolved: a head that moved on.
OutcomeBrief otherBrief()
{
    ManifestWorkLedgerEntry ledger;
    ledger.runId = kRun;
    ledger.projectId = kProject;
    ledger.terminalStatus = RunStatus::Completed;
    ledger.disposition = ManifestWorkLedgerDisposition::NoChangesNeeded;
    ledger.baselineFingerprint = std::string(64, 'b');
    OutcomeBriefFacts facts;
    facts.runId = kRun;
    facts.projectId = kProject;
    facts.workflowId = kExecutionManifestWorkflowId;
    facts.terminalStatus = RunStatus::Completed;
    facts.ledgerEntry = ledger;
    const auto result = deriveOutcomeBrief(facts);
    check(result.success(), "the other brief derives");
    return result.value();
}

// The same snapshot with one more fact: a different, still valid snapshot of the same Run.
ProjectContextSnapshot otherSnapshot()
{
    const std::string workspace(64, 'a');
    ProjectContextAssemblyInput input;
    input.projectId = kProject;
    input.selectedRunId = std::string(kRun);
    input.workspaceFingerprint = workspace;
    input.sections = {
        {ProjectContextCategory::ProjectMetadata, {item("Project: Demo", "project", kProject, true)}},
        {ProjectContextCategory::SelectedLineage, {item(std::string("selected run ") + kRun, "run", kRun, true)}},
        {ProjectContextCategory::WorkLedger, {item("resolved no change (edited)", "ledger_entry", kRun)}},
        {ProjectContextCategory::WorkspaceCatalog,
            {item("workspace identity", "workspace", "current", true, workspace), item("src/a.cpp", "workspace_file", "src/a.cpp")}}};
    const auto result = assembleProjectContextSnapshot(input);
    check(result.success(), "the other snapshot assembles");
    return result.value();
}

std::vector<FollowUpFileCandidate> manyFiles(std::size_t count)
{
    std::vector<FollowUpFileCandidate> files;
    for (std::size_t index = 0; index < count; ++index) {
        const std::string name = std::string("src/f") + (index < 10 ? "0" : "") + std::to_string(index) + ".cpp";
        files.push_back(candidate(name, "int f" + std::to_string(index) + "() { return " + std::to_string(index) + "; }\n"));
    }
    return files;
}

// ---- quote ------------------------------------------------------------------------------------------------------------------

void testAQuoteIsOneImmutableRecordAndSendsNothing()
{
    Harness h;
    const auto outcome = h.service.quote(h.intent());
    check(outcome.success(), "the quote succeeds: " + codeOf(outcome.error()));
    const auto& request = outcome.value().request;
    check(request.state == S::Quoted && request.projectId == kProject && request.sourceRunId == kRun
            && request.kind == FollowUpPlanKind::NextRun && request.maxDrafts == 2 && request.splitHint == FollowUpSplitHint::Auto,
        "the intent is stored as quoted");
    check(request.createdAt == at(0) && request.quoteExpiresAt == at(0) + kFollowUpQuoteLifetime && request.updatedAt == at(0),
        "one 15-minute deadline from the creation time");
    check(request.snapshotFingerprint == snapshotOf(kRun, kProject).fingerprint
            && request.outcomeSourceStateFingerprint == briefOf(kRun, kProject).sourceStateFingerprint,
        "the snapshot and the Outcome state are pinned");
    check(request.files.size() == 2 && request.files[0].relativePath == "src/a.cpp" && request.files[1].relativePath == "src/b.cpp"
            && request.files[0].contentSha256 == sha256Hex("int a() { return 1; }\n"),
        "the sent files are pinned with the sha256 of their complete bytes");
    check(request.quote.capMicros == kFollowUpSystemCeilingMicros && request.quote.worstCase.priceKnown
            && request.quote.providerIdentity.providerId == h.catalog.providerId
            && request.quote.worstCase.pricingIdentity.modelId == h.catalog.modelId,
        "the quote carries the ceiling, the price and the recipient");
    check(outcome.value().files.size() == 2 && outcome.value().files[0].disposition == FollowUpExcerptDisposition::Included,
        "the dialog gets every candidate with its disposition");
    check(sameRequest(h.stored(request.id), request), "...and exactly that is stored");
    check(h.client->calls() == 0 && h.factory.requests == 0 && h.executionsOf(request.id).empty(), "A quote calls no provider and logs nothing");

    // The bytes are the assembler's own: the quote's fingerprint seals exactly what a fresh assembly of the same sources gives.
    FollowUpComposerContextRequest again;
    again.sourceRunId = kRun;
    again.kind = FollowUpPlanKind::NextRun;
    again.splitHint = FollowUpSplitHint::Auto;
    again.maxDrafts = 2;
    again.goal = h.intent().goal;
    again.snapshot = snapshotOf(kRun, kProject);
    again.brief = briefOf(kRun, kProject);
    again.files = defaultFiles();
    for (auto& file : again.files) file.selected = true;
    const auto context = assembleFollowUpComposerContext(again);
    check(context.success() && context.value().contextFingerprint == request.quote.contextFingerprint,
        "The service assembled the provider bytes itself");
}

void testAQuoteRefusalIsTypedFreeAndStoresNothing()
{
    struct Case {
        const char* what;
        std::function<void(Harness&, FollowUpIntent&)> edit;
        const char* code;
    };
    const std::vector<Case> cases = {
        {"an empty goal", [](Harness&, FollowUpIntent& i) { i.goal.clear(); }, "follow_up.invalid_goal"},
        {"five drafts", [](Harness&, FollowUpIntent& i) { i.maxDrafts = 5; }, "follow_up.invalid_draft_limits"},
        {"many out of one", [](Harness&, FollowUpIntent& i) { i.splitHint = FollowUpSplitHint::Many; i.maxDrafts = 1; }, "follow_up.invalid_draft_limits"},
        {"a provider that does not exist", [](Harness&, FollowUpIntent& i) { i.providerId = "nope"; }, "follow_up.provider_unavailable"},
        {"a disabled provider", [](Harness& h, FollowUpIntent&) { h.editProvider([](Provider& p) { p.enabled = false; }); }, "follow_up.provider_unavailable"},
        {"a disabled model", [](Harness& h, FollowUpIntent&) { h.editModel([](Model& m) { m.enabled = false; }); }, "follow_up.model_unavailable"},
        {"a model of another provider", [](Harness& h, FollowUpIntent&) { h.editModel([](Model& m) { m.providerId = "other"; }); }, "follow_up.model_unavailable"},
        {"a project that does not exist", [](Harness&, FollowUpIntent& i) { i.projectId = "project-x"; }, "follow_up.project_not_found"},
        {"an archived project", [](Harness& h, FollowUpIntent&) { h.reader.edit([](auto& m) { m.project.status = ProjectStatus::Archived; }); }, "follow_up.project_inactive"},
        {"a Run of another project", [](Harness& h, FollowUpIntent&) { h.reader.edit([](auto& m) { m.runProjectId = "project-2"; }); }, "follow_up.run_project_mismatch"},
        {"a Run that does not exist", [](Harness&, FollowUpIntent& i) { i.sourceRunId = "run-x"; }, "follow_up.run_not_found"},
        {"a Run without an Outcome Brief", [](Harness& h, FollowUpIntent&) { h.reader.setMissingOutcome(true); }, "follow_up.outcome_missing"},
        {"a file that is no candidate", [](Harness&, FollowUpIntent& i) { i.selectedPaths = std::vector<std::string> {"src/none.cpp"}; }, "follow_up.unknown_file"},
        {"a price that is not known", [](Harness& h, FollowUpIntent&) { h.editModel([](Model& m) { m.inputPriceMicrosPerMillionTokens = 0; }); }, "follow_up.price_unknown"},
        {"a cap that cannot pay the minimum output", [](Harness&, FollowUpIntent& i) { i.capMicros = 1; }, "follow_up.cap_too_low"},
        {"a cap above the system ceiling", [](Harness&, FollowUpIntent& i) { i.capMicros = kFollowUpSystemCeilingMicros + 1; }, "follow_up.invalid_cap"},
        {"a cap of zero", [](Harness&, FollowUpIntent& i) { i.capMicros = 0; }, "follow_up.invalid_cap"},
        {"a storage fault", [](Harness& h, FollowUpIntent&) { h.reader.fail(common::Error {"persistence.query_failed", "boom"}); }, "persistence.query_failed"},
    };
    for (const auto& item : cases) {
        Harness h;
        auto intent = h.intent();
        item.edit(h, intent);
        requireCode(h.service.quote(intent), item.code, item.what);
        const auto listed = h.requests.listForProject(kProject, 10);
        check(listed.success() && listed.value().empty(), std::string(item.what) + ": nothing was stored");
        check(h.client->calls() == 0 && h.factory.requests == 0, std::string(item.what) + ": no provider was called");
    }

    // The Outcome gate is the assembler's: a resolved Run is not a recovery source.
    Harness h;
    auto recovery = h.intent();
    recovery.kind = FollowUpPlanKind::Recovery;
    const auto refused = h.service.quote(recovery);
    check(!refused.success() && refused.error()->code.rfind("follow_up.", 0) == 0, "A resolved Run is refused as a recovery source");
}

void testASecondQuoteReplacesTheFirstAndAnOpenRequestIsNeverDisplaced()
{
    Harness h;
    const auto first = h.quoted();
    h.nowMs = 1000;
    const auto second = h.service.quote(h.intent());
    check(second.success() && second.value().replacedQuoteId == std::optional<std::string>(first.id), "The new quote replaces the quoted one");
    const auto old = h.stored(first.id);
    check(old.state == S::Discarded && !followUpRequestMayBeCharged(old), "...which is a free discard");
    check(h.service.findOpenForSourceRun(kRun).value()->id == second.value().request.id, "...and the new one holds the slot");

    check(h.service.authorize(second.value().request.id).success(), "setup: authorize");
    const auto reads = h.reader.reads();
    requireCode(h.service.quote(h.intent()), "follow_up.request_open", "a quote while a request is authorized");
    check(h.stored(second.value().request.id).state == S::Authorized, "...the authorized request is untouched");
    check(h.reader.reads() == reads, "...and it was refused before any source was read");
}

void testTheFilesOfAQuoteAreTheOnesTheUserSelected()
{
    {
        Harness h(manyFiles(10));
        const auto outcome = h.service.quote(h.intent());
        check(outcome.success() && outcome.value().request.files.size() == kFollowUpComposerMaximumFiles,
            "By default the first eight eligible files are sent");
        std::size_t included = 0, limited = 0;
        for (const auto& file : outcome.value().files) {
            if (file.disposition == FollowUpExcerptDisposition::Included) ++included;
            if (file.disposition == FollowUpExcerptDisposition::FileLimit) ++limited;
        }
        check(included == 8 && limited == 2, "...and the other two are listed as over the limit");
        const auto described = h.service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun);
        check(described.success() && described.value().size() == outcome.value().files.size()
                && described.value()[9].disposition == FollowUpExcerptDisposition::FileLimit,
            "describeFiles shows the dialog the same list before any quote");
    }
    {
        Harness h(manyFiles(10));
        auto intent = h.intent();
        intent.selectedPaths = std::vector<std::string> {"src/f03.cpp"};
        const auto outcome = h.service.quote(intent);
        check(outcome.success() && outcome.value().request.files.size() == 1 && outcome.value().request.files[0].relativePath == "src/f03.cpp",
            "An explicit selection is exact");
        std::size_t notSelected = 0;
        for (const auto& file : outcome.value().files) notSelected += file.disposition == FollowUpExcerptDisposition::NotSelected ? 1 : 0;
        check(notSelected == 9, "...every other candidate is listed as not selected");
    }
    {
        auto files = defaultFiles();
        files.push_back(candidate("src/blob.bin", "", FollowUpFileSafety::Binary));
        Harness h(files);
        auto intent = h.intent();
        intent.selectedPaths = std::vector<std::string> {"src/a.cpp", "src/blob.bin"};
        const auto outcome = h.service.quote(intent);
        check(outcome.success() && outcome.value().request.files.size() == 1, "A binary file is never sent, even when selected");
        bool listed = false;
        for (const auto& file : outcome.value().files) {
            if (file.relativePath == "src/blob.bin") listed = file.disposition == FollowUpExcerptDisposition::Ineligible;
        }
        check(listed, "...and the dialog says why it is not");
    }
}

// ---- authorize --------------------------------------------------------------------------------------------------------------

void testAuthorizeChecksAgainAndNudgesTheProcessor()
{
    Harness h;
    const auto request = h.quoted();
    h.nowMs = 2000;
    const auto reads = h.reader.reads();
    const auto authorized = h.service.authorize(request.id);
    check(authorized.success() && authorized.value().state == S::Authorized && authorized.value().authorizedAt == at(2000),
        "A quote is authorized");
    check(h.reader.reads() > reads, "The sources were read again for the authorisation");
    check(h.nudged == std::vector<std::string> {request.id}, "The processor is nudged once");
    check(h.client->calls() == 0 && h.executionsOf(request.id).empty(), "Authorising calls no provider");
    requireCode(h.service.authorize(request.id), "follow_up.state_changed", "authorizing twice");
    check(h.nudged.size() == 1, "...and nudges nothing the second time");
    requireCode(h.service.authorize("nope"), "follow_up.not_found", "an unknown request");
}

// The cap and the file selection of a quote are part of what is authorised: the checks run again with exactly those, never with
// defaults.
void testTheCapAndTheSelectionSurviveTheAuthorisationAndTheClaim()
{
    Harness h;
    auto intent = h.intent();
    intent.capMicros = 200'000;
    intent.selectedPaths = std::vector<std::string> {"src/b.cpp"};
    const auto quote = h.service.quote(intent);
    check(quote.success() && quote.value().request.quote.capMicros == 200'000 && quote.value().request.files.size() == 1
            && quote.value().request.files[0].relativePath == "src/b.cpp",
        "setup: a quote with a user cap and one of two files");
    const auto id = quote.value().request.id;
    const auto authorized = h.service.authorize(id);
    check(authorized.success(), "The authorisation re-derives the quote with the stored cap and selection: " + codeOf(authorized.error()));
    h.client->push(okReply(validReply()));
    const auto done = h.processor.processAuthorized(id);
    check(done.success() && done.value().state == S::PlanReady && done.value().quote.capMicros == 200'000
            && done.value().files.size() == 1,
        "...and so does the claim: " + codeOf(done.error()));
    const auto sent = h.client->lastRequest().userContent;
    check(sent.find("int b() { return 2; }") != std::string::npos && sent.find("int a() { return 1; }") == std::string::npos,
        "Only the selected file's content went to the provider");
}

void testAnAuthorisationAfterTheDeadlineExpiresTheRequest()
{
    Harness h;
    const auto request = h.quoted();
    h.nowMs = kQuoteLifetimeMs - 1;
    check(h.service.authorize(request.id).success(), "Just before the deadline it works");

    Harness late;
    const auto old = late.quoted();
    late.nowMs = kQuoteLifetimeMs;
    requireCode(late.service.authorize(old.id), "follow_up.quote_expired", "at the deadline");
    const auto expired = late.stored(old.id);
    check(expired.state == S::Expired && !followUpRequestMayBeCharged(expired), "The request is expired and free");
    check(late.nudged.empty(), "...and nothing was nudged");
}

void testEveryDriftBeforeTheCallIsAFreeStale()
{
    struct Case {
        const char* what;
        std::function<void(Harness&)> edit;
        const char* code;
    };
    const std::vector<Case> cases = {
        {"the project is archived", [](Harness& h) { h.reader.edit([](auto& m) { m.project.status = ProjectStatus::Archived; }); }, "follow_up.stale_project"},
        {"the project is gone", [](Harness& h) { h.reader.edit([](auto& m) { m.project.id = "other-project"; }); }, "follow_up.stale_project"},
        {"the source Run is gone", [](Harness& h) { h.reader.fail(common::Error {"follow_up.run_not_found", "gone"}); }, "follow_up.stale_source"},
        {"the Run moved to another project", [](Harness& h) { h.reader.edit([](auto& m) { m.runProjectId = "project-2"; }); }, "follow_up.stale_source"},
        {"the Outcome head moved on", [](Harness& h) { h.reader.edit([](auto& m) { m.brief = otherBrief(); }); }, "follow_up.stale_outcome"},
        {"the Outcome brief is missing", [](Harness& h) { h.reader.setMissingOutcome(true); }, "follow_up.stale_outcome"},
        {"the provider is disabled", [](Harness& h) { h.editProvider([](Provider& p) { p.enabled = false; }); }, "follow_up.stale_provider"},
        {"the model is disabled", [](Harness& h) { h.editModel([](Model& m) { m.enabled = false; }); }, "follow_up.stale_provider"},
        {"the base URL was edited in place", [](Harness& h) { h.editProvider([](Provider& p) { p.baseUrl += "x"; }); }, "follow_up.stale_provider"},
        {"the credential reference was replaced", [](Harness& h) { h.editProvider([](Provider& p) { p.credentialReference = "credential-b"; }); }, "follow_up.stale_provider"},
        {"the kind of the provider changed", [](Harness& h) { h.editProvider([](Provider& p) { p.kind = "openai"; }); }, "follow_up.stale_provider"},
        {"a pinned file changed", [](Harness& h) { h.reader.edit([](auto& m) { m.candidates[0] = candidate("src/a.cpp", "int a() { return 99; }\n"); }); }, "follow_up.stale_files"},
        {"a pinned file vanished", [](Harness& h) { h.reader.edit([](auto& m) { m.candidates.erase(m.candidates.begin()); }); }, "follow_up.stale_files"},
        {"a pinned file became unsafe", [](Harness& h) { h.reader.edit([](auto& m) { m.candidates[0] = candidate("src/a.cpp", "", FollowUpFileSafety::SecretDetected); }); }, "follow_up.stale_files"},
        {"the project snapshot changed", [](Harness& h) { h.reader.edit([](auto& m) { m.snapshot = otherSnapshot(); }); }, "follow_up.stale_context"},
        {"an output price changed", [](Harness& h) { h.editModel([](Model& m) { m.outputPriceMicrosPerMillionTokens += 1; }); }, "follow_up.stale_pricing"},
        {"the price is no longer known", [](Harness& h) { h.editModel([](Model& m) { m.inputPriceMicrosPerMillionTokens = 0; }); }, "follow_up.stale_pricing"},
        {"the window shrank (it is part of the pricing identity)", [](Harness& h) { h.editModel([](Model& m) { m.contextWindowTokens = 5000; }); }, "follow_up.stale_pricing"},
        {"the model's output limit shrank", [](Harness& h) { h.editModel([](Model& m) { m.maxOutputTokens = 4000; }); }, "follow_up.stale_pricing"},
    };
    for (const auto& item : cases) {
        Harness h;
        const auto request = h.quoted();
        item.edit(h);
        requireCode(h.service.authorize(request.id), item.code, item.what);
        const auto stale = h.stored(request.id);
        check(stale.state == S::Stale && !followUpRequestMayBeCharged(stale), std::string(item.what) + ": the request is a free stale");
        check(h.nudged.empty() && h.client->calls() == 0 && h.executionsOf(request.id).empty(),
            std::string(item.what) + ": nothing was nudged, called or logged");
    }

    // A fault is not a stale: the request stays quoted and the fault is reported as it is.
    Harness h;
    const auto request = h.quoted();
    h.reader.fail(common::Error {"persistence.query_failed", "boom"});
    requireCode(h.service.authorize(request.id), "persistence.query_failed", "a storage fault");
    check(h.stored(request.id).state == S::Quoted, "...the request is still quoted");
}

// ---- the processor ----------------------------------------------------------------------------------------------------------

void testOneAuthorizedRequestMakesExactlyOneCallAndStoresItsPlan()
{
    Harness h;
    const auto request = h.authorized();
    h.client->push(okReply(validReply()));
    h.nowMs = 5000;
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::PlanReady, "The request is plan_ready: " + codeOf(done.error()));
    const auto stored = h.stored(request.id);
    check(sameRequest(stored, done.value()), "...and that is what is stored");
    check(stored.claimedAt == at(5000) && followUpRequestMayBeCharged(stored) && stored.answer.has_value(),
        "It was claimed and is possibly charged");
    check(stored.answer->rawReply == validReply(), "The reply is stored byte for byte");
    const auto plan = parseFollowUpAnswerPlan(*stored.answer, stored.splitHint, stored.maxDrafts);
    check(plan.success() && stored.answer->planFingerprint == plan.value().fingerprint, "...with the plan's own fingerprint");
    const auto advisories = parseFollowUpPlanAdvisories(stored.answer->advisoriesJson);
    check(advisories.success() && advisories.value().size() == 2 && advisories.value()[0].code == kFollowUpAdvisoryPathNotInCatalog,
        "...and the advisories computed over the plan and the frozen catalog");

    // The call: exactly the stored bytes, limit and cache policy.
    check(h.client->calls() == 1, "ONE provider call");
    const auto sent = h.client->lastRequest();
    check(sent.systemPrompt == followUpComposerSystemPrompt()
            && followUpComposerContextFingerprint(sent.systemPrompt, sent.userContent) == stored.quote.contextFingerprint,
        "The provider got exactly the quoted bytes");
    check(sent.maxOutputTokens == stored.quote.effectiveMaxOutputTokens() && sent.promptCache == stored.quote.promptCache()
            && sent.providerId == h.catalog.providerId && sent.modelId == h.catalog.modelId && sent.apiModelId == "model-api",
        "...with the stored effective output, the stored cache policy and the quoted model");
    check(sent.disableTotalTimeout && sent.temperature == 0.2, "...one plain turn that only an explicit stop ends");

    // The cost: one execution row under its own source, no link to the source Run.
    const auto rows = h.executionsOf(request.id);
    check(rows.size() == 1, "One execution row");
    const auto& row = rows.front();
    ai::models::TokenUsage usage;
    usage.inputTokens = 30'000;
    usage.outputTokens = 2'000;
    const auto cost = computeSingleResponseCostMicros(modelOf(h.catalog), usage);
    check(row.source == ModelExecutionSource::FollowUp && row.sourceId == request.id && row.projectId == std::optional<std::string>(kProject)
            && row.role == "follow_up_composer" && row.status == ModelExecutionStatus::Completed && row.inputTokens == 30'000
            && row.outputTokens == 2'000 && row.costMicros == cost.costMicros && row.costMicros > 0
            && row.contextHash == stored.quote.contextFingerprint,
        "...under source follow_up with the provider's usage and its price");
    check(h.executionStore.inner.listExecutions(ModelExecutionSource::Run, kRun).value().empty(), "...and none under the source Run");

    // Never a second call.
    requireCode(h.processor.processAuthorized(request.id), "follow_up.state_changed", "a second pass over the same request");
    check(h.client->calls() == 1 && h.executionsOf(request.id).size() == 1, "...makes no call and writes no row");
}

// The name checks and the staffing sanity check run in the processor with the request's goal and the exact bytes the model was sent;
// their findings are stored with the answer like the catalog advisories (advisory only: the plan is plan_ready either way).
void testNameAndStaffingAdvisoriesAreStoredWithThePlan()
{
    Harness h;
    const auto request = h.authorized();
    const std::string staffing = "\"staffing\":{\"workers\":[{\"role\":\"implementation\",\"focus\":\"a\"},"
                                 "{\"role\":\"implementation\",\"focus\":\"b\"},{\"role\":\"implementation\",\"focus\":\"c\"}],"
                                 "\"parallelism\":2,\"reason\":\"r\"}";
    const std::string plan = "{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"drafts\":["
        "{\"key\":\"k0\",\"title\":\"One\",\"task\":\"Create Ghost.qml.\"," + staffing + "},"
        "{\"key\":\"k1\",\"title\":\"Two\",\"task\":\"Wire it.\",\"dependsOn\":[\"k0\"]}]}";
    h.client->push(okReply(replyOfPlanJson(plan)));
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::PlanReady, "The request is plan_ready: " + codeOf(done.error()));
    const auto stored = h.stored(request.id);
    const auto advisories = parseFollowUpPlanAdvisories(stored.answer->advisoriesJson);
    check(advisories.success(), "The stored advisories are readable");
    bool unknownName = false;
    bool tooManyWorkers = false;
    for (const auto& advisory : advisories.value()) {
        if (advisory.code == kFollowUpAdvisoryTaskNameUnknown && advisory.draftKey == "k0" && advisory.path == "Ghost.qml") unknownName = true;
        if (advisory.code == kFollowUpAdvisoryStaffingExceedsNamedFiles && advisory.draftKey == "k0") tooManyWorkers = true;
    }
    check(unknownName, "A name that is neither in the goal nor in the context the model was sent is stored as task_name_unknown");
    check(tooManyWorkers, "Three suggested workers for one named file is stored as staffing_exceeds_named_files");
    const auto parsedPlan = parseFollowUpAnswerPlan(*stored.answer, stored.splitHint, stored.maxDrafts);
    check(parsedPlan.success() && parsedPlan.value().drafts[0].staffing.has_value()
            && parsedPlan.value().drafts[0].staffing->parallelism == 2,
        "The stored plan reads back with its staffing hint, and the advisories changed nothing about it");
}

void testAnAnswerThatIsNotOneValidPlanIsStoredAsAFailureAfterBeingPaidFor()
{
    struct Case {
        const char* what;
        std::string reply;
        const char* code;
        bool replyKept;
    };
    const std::string two = validReply(false) + validReply(false);
    const std::vector<Case> cases = {
        {"no envelope", "Here is my plan in prose only.", "next_run_plan.envelope_missing", true},
        {"two envelopes", two, "next_run_plan.envelope_multiple", true},
        {"broken JSON", "```next-run-plan.v1\n{not json\n```\n", "next_run_plan.invalid_json", true},
        {"an empty plan", replyOfPlanJson("{}"), "next_run_plan.schema_version", true},
        {"three drafts where two were authorised", replyOfPlanJson(planJsonOf('c', 3)), "follow_up.plan_draft_count_not_authorised", true},
        {"a reply with a NUL byte", validReply(false) + std::string(1, '\0'), "follow_up.reply_not_storable", false},
        {"an empty reply", "", "follow_up.reply_not_storable", false},
    };
    for (const auto& item : cases) {
        Harness h;
        const auto request = h.authorized();
        h.client->push(okReply(item.reply));
        const auto done = h.processor.processAuthorized(request.id);
        check(done.success() && done.value().state == S::Failed, std::string(item.what) + ": the request failed: " + codeOf(done.error()));
        const auto stored = h.stored(request.id);
        check(stored.failure.has_value() && stored.failure->code == item.code, std::string(item.what) + ": the typed code " + item.code);
        check(stored.failure->rawReply.has_value() == item.replyKept
                && (!item.replyKept || *stored.failure->rawReply == item.reply),
            std::string(item.what) + ": the reply is kept exactly when it is storable text");
        check(!stored.answer.has_value() && followUpRequestMayBeCharged(stored), std::string(item.what) + ": no plan, but possibly charged");
        const auto rows = h.executionsOf(request.id);
        check(h.client->calls() == 1 && rows.size() == 1 && rows[0].status == ModelExecutionStatus::Completed && rows[0].costMicros > 0,
            std::string(item.what) + ": the call was made and its cost is logged");
    }
}

void testAProviderFailureIsStoredAndNeverRetried()
{
    Harness h;
    const auto request = h.authorized();
    h.client->push(errorReply("provider.rate_limited", "slow down"));
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::Failed && done.value().failure->code == "provider.rate_limited"
            && !done.value().failure->rawReply.has_value(),
        "A provider failure is a failed request with its code");
    const auto rows = h.executionsOf(request.id);
    check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Failed && rows[0].costMicros == 0
            && rows[0].errorCode == "provider.rate_limited",
        "...and a failed execution row without a cost");
    check(h.client->calls() == 1, "...after exactly one call");
    requireCode(h.processor.processAuthorized(request.id), "follow_up.state_changed", "a retry of the same request");
    check(h.client->calls() == 1, "A retry is only a new, explicitly authorised request");

    // A provider message the request could not store as it is becomes a readable one.
    Harness odd;
    const auto second = odd.authorized();
    odd.client->push(errorReply("", std::string("bad \xFF\xFE bytes")));
    const auto failed = odd.processor.processAuthorized(second.id);
    check(failed.success() && failed.value().failure->code == "follow_up.composition_failed"
            && failed.value().failure->message.find("not readable") != std::string::npos,
        "An empty code and unreadable text are replaced, not stored raw");
}

void testAClientThatCannotBeResolvedFailsTheRequestWithoutACall()
{
    Harness h;
    const auto request = h.authorized();
    h.factory.failure = common::Error {"credential.missing", "no key"};
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::Failed && done.value().failure->code == "credential.missing",
        "A missing credential is a failed request");
    check(h.client->calls() == 0 && h.executionsOf(request.id).size() == 1
            && h.executionsOf(request.id)[0].status == ModelExecutionStatus::Failed,
        "...with no call and a failed execution row");
}

void testDescribeFilesAppliesTheSameGateAsAQuote()
{
    {
        Harness h;
        h.reader.edit([](auto& m) { m.project.status = ProjectStatus::Archived; });
        requireCode(h.service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun), "follow_up.project_inactive", "an archived project");
    }
    {
        Harness h;
        h.reader.edit([](auto& m) { m.runProjectId = "project-2"; });
        requireCode(h.service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun), "follow_up.run_project_mismatch", "a Run of another project");
    }
    {
        Harness h;
        const auto refused = h.service.describeFiles(kProject, kRun, FollowUpPlanKind::Recovery);
        check(!refused.success() && refused.error()->code.rfind("follow_up.", 0) == 0, "A resolved Run is no recovery source");
        h.reader.setMissingOutcome(true);
        requireCode(h.service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun), "follow_up.outcome_missing", "no brief yet");
    }
}

void testTheCostRecordIsWrittenBeforeTheCall()
{
    Harness h;
    const auto request = h.authorized();
    h.executionStore.failInserts = true;
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::Failed && done.value().failure->code == "follow_up.execution_log_failed",
        "No call is made when the execution row cannot be written");
    check(h.client->calls() == 0 && h.factory.requests == 0, "...the provider was never reached");
    check(followUpRequestMayBeCharged(h.stored(request.id)), "...the claim was made, so the request still says it may be charged");
}

void testADriftAtTheClaimIsAFreeStaleAndNoCallIsMade()
{
    {
        Harness h;
        const auto request = h.authorized();
        h.reader.edit([](auto& m) { m.candidates[0] = candidate("src/a.cpp", "int a() { return 7; }\n"); });
        requireCode(h.processor.processAuthorized(request.id), "follow_up.stale_files", "a file changed after the authorisation");
        const auto stale = h.stored(request.id);
        check(stale.state == S::Stale && !followUpRequestMayBeCharged(stale) && !stale.claimedAt, "The request is a free stale, never claimed");
        check(h.client->calls() == 0 && h.factory.requests == 0 && h.executionsOf(request.id).empty(), "...no call, no client, no row");
    }
    {
        Harness h;
        const auto request = h.authorized();
        h.editModel([](Model& m) { m.outputPriceMicrosPerMillionTokens += 1; });
        requireCode(h.processor.processAuthorized(request.id), "follow_up.stale_pricing", "a price changed after the authorisation");
        check(h.stored(request.id).state == S::Stale && h.client->calls() == 0, "...the same for a price");
    }
    {
        Harness h;
        const auto request = h.authorized();
        h.nowMs = kQuoteLifetimeMs;
        requireCode(h.processor.processAuthorized(request.id), "follow_up.quote_expired", "an authorisation past its 15 minutes");
        const auto expired = h.stored(request.id);
        check(expired.state == S::Expired && !followUpRequestMayBeCharged(expired) && h.client->calls() == 0, "...is expired and free");
    }
    {
        Harness h;
        const auto request = h.authorized();
        h.reader.fail(common::Error {"persistence.query_failed", "boom"});
        requireCode(h.processor.processAuthorized(request.id), "persistence.query_failed", "a storage fault at the claim");
        check(h.stored(request.id).state == S::Authorized && h.client->calls() == 0, "...leaves the request authorized, no call");
    }
}

void testCancellingBeforeTheClaimIsFreeAndNothingIsCalled()
{
    Harness h;
    const auto request = h.authorized();
    const auto cancelled = h.service.cancel(request.id);
    check(cancelled.success() && cancelled.value().state == S::Cancelled && !followUpRequestMayBeCharged(cancelled.value()),
        "Cancelling an authorized request is free");
    requireCode(h.processor.processAuthorized(request.id), "follow_up.state_changed", "processing it afterwards");
    check(h.client->calls() == 0, "...makes no call");

    Harness quotedOnly;
    const auto fresh = quotedOnly.quoted();
    check(quotedOnly.service.cancel(fresh.id).value().state == S::Cancelled, "A quote can be cancelled too");
}

void testStoppingTheRunningCallCancelsTheRequestAsPossiblyCharged()
{
    Harness h;
    const auto request = h.authorized();
    h.client->push(blockingReply());
    auto running = std::async(std::launch::async, [&] { return h.processor.processAuthorized(request.id); });
    h.client->waitEntered(1);
    const auto asked = h.service.cancel(request.id);
    check(asked.success() && asked.value().state == S::Composing, "Cancel asks the running call to stop and returns");
    const auto done = running.get();
    check(done.success() && done.value().state == S::Cancelled && followUpRequestMayBeCharged(done.value()),
        "The request is cancelled, possibly charged: " + codeOf(done.error()));
    const auto rows = h.executionsOf(request.id);
    check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Cancelled && rows[0].errorCode == "provider.cancelled",
        "...and its execution row is cancelled");
    check(h.client->calls() == 1, "...after one call");

    // A stop that is already there prevents the call altogether.
    Harness early;
    const auto second = early.authorized();
    std::stop_source stop;
    stop.request_stop();
    const auto prevented = early.processor.processAuthorized(second.id, stop.get_token());
    check(prevented.success() && prevented.value().state == S::Cancelled && early.client->calls() == 0 && early.factory.requests == 0,
        "A stop before the call prevents it (the claim was made, so it is still possibly charged)");
    check(followUpRequestMayBeCharged(prevented.value()), "...and says so");

    // An orphaned composing request (no call running here) is cancelled directly.
    Harness orphan;
    const auto third = orphan.authorized();
    FollowUpTransition claim;
    claim.event = FollowUpPlanEvent::Claim;
    claim.expectedState = S::Authorized;
    check(orphan.requests.transition(third.id, claim, at(10)).success(), "setup: an orphaned claim");
    const auto direct = orphan.service.cancel(third.id);
    check(direct.success() && direct.value().state == S::Cancelled, "Cancel with no call running settles the request itself");
}

void testTwoPassesOverOneRequestMakeOneCall()
{
    Harness h;
    const auto request = h.authorized();
    h.client->push(okReply(validReply()));
    auto first = std::async(std::launch::async, [&] { return h.processor.processAuthorized(request.id); });
    auto second = std::async(std::launch::async, [&] { return h.processor.processAuthorized(request.id); });
    const auto a = first.get();
    const auto b = second.get();
    check(h.client->calls() == 1, "However the two interleave, the provider is called once");
    check((a.success() ? 1 : 0) + (b.success() ? 1 : 0) == 1, "...and exactly one pass settles the request");
    check(h.stored(request.id).state == S::PlanReady && h.executionsOf(request.id).size() == 1, "...one plan, one row");
}

// While one pass is inside the provider call, another pass over the same request is refused at once (and still makes no call).
void testASecondPassWhileTheFirstIsInTheCallIsRefused()
{
    Harness h;
    const auto request = h.authorized();
    h.client->push(blockingReply());
    auto running = std::async(std::launch::async, [&] { return h.processor.processAuthorized(request.id); });
    h.client->waitEntered(1);
    requireCode(h.processor.processAuthorized(request.id), "follow_up.already_running", "a second pass during the call");
    check(h.client->calls() == 1, "...makes no call");
    check(h.service.cancel(request.id).success(), "setup: stop the first pass");
    const auto done = running.get();
    check(done.success() && done.value().state == S::Cancelled, "The first pass settles the request");
    // The registration is gone with the pass: nothing stays "running".
    check(!h.processor.requestStop(request.id), "A finished pass leaves nothing to stop");
}

void testStartupRecoveryInterruptsExpiresAndReportsTheRest()
{
    Harness h;
    const auto make = [&](const std::string& id, const std::string& run, std::int64_t createdMs, S target) {
        check(h.requests.insertQuoted(quotedRequest(h.catalog, id, kProject, run, at(createdMs))).success(), "setup: insert " + id);
        FollowUpTransition authorize;
        authorize.event = FollowUpPlanEvent::Authorize;
        authorize.expectedState = S::Quoted;
        authorize.contextFingerprint = h.stored(id).quote.contextFingerprint;
        check(h.requests.transition(id, authorize, at(createdMs + 10)).success(), "setup: authorize " + id);
        if (target == S::Composing) {
            FollowUpTransition claim;
            claim.event = FollowUpPlanEvent::Claim;
            claim.expectedState = S::Authorized;
            check(h.requests.transition(id, claim, at(createdMs + 20)).success(), "setup: claim " + id);
        }
    };
    make("composing-1", "run-1", 0, S::Composing);
    make("old-1", "run-2", 0, S::Authorized);
    make("live-1", "run-3", 10 * 60 * 1000, S::Authorized);
    h.nowMs = 16 * 60 * 1000;

    const auto report = h.processor.recoverAtStartup();
    check(report.success() && report.value().interrupted == 1 && report.value().expired == 1
            && report.value().toProcess == std::vector<std::string> {"live-1"},
        "An orphan is interrupted, an old authorisation expires, a live one is reported: " + codeOf(report.error()));
    const auto orphan = h.stored("composing-1");
    check(orphan.state == S::Interrupted && followUpRequestMayBeCharged(orphan), "The orphaned call is interrupted and possibly charged");
    const auto old = h.stored("old-1");
    check(old.state == S::Expired && !followUpRequestMayBeCharged(old), "The old authorisation expired, free");
    check(h.stored("live-1").state == S::Authorized && h.client->calls() == 0, "The live one waits for the owner; recovery calls no provider");

    const auto again = h.processor.recoverAtStartup();
    check(again.success() && again.value().interrupted == 0 && again.value().expired == 0 && again.value().toProcess.size() == 1,
        "A second pass changes nothing");
}

void testTheWorkerProcessesWhatIsNudgedAndWhatStartupFinds()
{
    {
        Harness h;
        FollowUpComposerRunner runner(h.processor);
        h.service.setAuthorizedNudge([&](const std::string& id) { runner.nudge(id); });
        h.client->push(okReply(validReply()));
        const auto request = h.quoted();
        check(h.service.authorize(request.id).success(), "setup: authorize");
        runner.waitUntilIdle();
        check(h.stored(request.id).state == S::PlanReady && h.client->calls() == 1, "An authorisation is processed on the worker");
    }
    {
        Harness h;
        h.service.setAuthorizedNudge({});
        const auto request = h.authorized();
        h.client->push(okReply(validReply()));
        FollowUpComposerRunner runner(h.processor);
        runner.recoverAtStartup();
        runner.waitUntilIdle();
        check(h.stored(request.id).state == S::PlanReady && h.client->calls() == 1, "The startup pass claims a live authorisation ONCE");
        runner.recoverAtStartup();
        runner.waitUntilIdle();
        check(h.client->calls() == 1, "...and a second startup pass pays nothing again");
    }
    {
        Harness h;
        h.service.setAuthorizedNudge({});
        const auto request = h.authorized();
        h.client->push(blockingReply());
        {
            FollowUpComposerRunner runner(h.processor);
            runner.nudge(request.id);
            h.client->waitEntered(1);
        }   // the destructor stops the worker and the call it is running
        const auto stored = h.stored(request.id);
        check(stored.state == S::Cancelled && followUpRequestMayBeCharged(stored), "Shutting the worker down stops a running call");
    }
}

void testAGuestRuntimeReadsAndWritesNothing()
{
    const common::Error refusal {"follow_up.not_the_owner", "Another runtime owns the database"};
    Harness h(defaultFiles(), refusal);
    const auto reads = h.reader.reads();
    requireCode(h.service.quote(h.intent()), "follow_up.not_the_owner", "quote");
    requireCode(h.service.authorize("x"), "follow_up.not_the_owner", "authorize");
    requireCode(h.service.cancel("x"), "follow_up.not_the_owner", "cancel");
    requireCode(h.service.discard("x"), "follow_up.not_the_owner", "discard");
    requireCode(h.service.confirm("x", std::string(64, 'a')), "follow_up.not_the_owner", "confirm");
    requireCode(h.service.preview("x"), "follow_up.not_the_owner", "preview");
    requireCode(h.service.find("x"), "follow_up.not_the_owner", "find");
    requireCode(h.service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun), "follow_up.not_the_owner", "describeFiles");
    requireCode(h.processor.processAuthorized("x"), "follow_up.not_the_owner", "processAuthorized");
    requireCode(h.processor.recoverAtStartup(), "follow_up.not_the_owner", "recoverAtStartup");
    check(h.reader.reads() == reads && h.client->calls() == 0 && h.factory.requests == 0, "A guest read no source and called no provider");
    check(h.requests.listForProject(kProject, 10).value().empty(), "...and wrote no request");
}

// ---- the paid usage is never lost ---------------------------------------------------------------------------------------------

ai::models::TokenUsage reportedUsage()
{
    ai::models::TokenUsage usage;
    usage.inputTokens = 30'000;
    usage.outputTokens = 2'000;
    return usage;
}

// The accounting the processor must store for `reportedUsage()` at the fixture prices.
FollowUpAccounting expectedAccounting(const Harness& h)
{
    const auto cost = computeSingleResponseCostMicros(modelOf(h.catalog), reportedUsage());
    FollowUpAccounting accounting;
    accounting.inputTokens = 30'000;
    accounting.outputTokens = 2'000;
    accounting.costMicros = cost.costMicros;
    accounting.priceKnown = cost.priceKnown;
    return accounting;
}

void testTheUsageOfAPaidResponseIsStoredWithTheRequest()
{
    Harness h;
    const auto request = h.authorized();
    h.client->push(okReply(validReply()));
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::PlanReady && done.value().accounting.has_value(), "A stored plan carries its accounting");
    auto expected = expectedAccounting(h);
    check(expected.costMicros > 0, "setup: the fixture usage has a price");
    auto stored = *h.stored(request.id).accounting;
    stored.latencyMs = 0;   // the wall clock of the scripted call is not part of the claim
    check(stored == expected, "...the provider's usage and the price of THAT usage");

    // An invalid answer was paid for too: it carries the accounting.
    Harness invalid;
    const auto second = invalid.authorized();
    invalid.client->push(okReply("prose only"));
    const auto failed = invalid.processor.processAuthorized(second.id);
    check(failed.success() && failed.value().state == S::Failed && failed.value().accounting.has_value()
            && failed.value().accounting->inputTokens == 30'000,
        "A failed answer that followed a response carries its accounting");

    // A provider that never answered has nothing to account: no response, no usage.
    Harness refused;
    const auto third = refused.authorized();
    refused.client->push(errorReply("provider.rate_limited", "slow down"));
    const auto none = refused.processor.processAuthorized(third.id);
    check(none.success() && none.value().state == S::Failed && !none.value().accounting.has_value(),
        "A provider failure carries no accounting");
}

void testAUsageTheLogWouldRefuseIsStoredWithoutItsUntrustedBreakdown()
{
    Harness h;
    const auto request = h.authorized();
    ScriptedReply reply = okReply(validReply());
    reply.ok->usage.cacheReadInputTokens = 90'000;   // more cache-read tokens than input tokens: a malformed report
    h.client->push(std::move(reply));
    const auto done = h.processor.processAuthorized(request.id);
    check(done.success() && done.value().state == S::PlanReady && done.value().accounting.has_value(),
        "A malformed usage does not stop the answer from being stored: " + codeOf(done.error()));
    const auto& accounting = *done.value().accounting;
    check(accounting.cacheReadInputTokens == 0 && accounting.cacheCreationInputTokens == 0 && !accounting.priceKnown
            && accounting.inputTokens == 30'000 && accounting.outputTokens == 2'000,
        "The breakdown that cannot be trusted is dropped and the price is marked unknown");
    const auto rows = h.executionsOf(request.id);
    check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Completed && !rows[0].costPriceKnown,
        "...and the execution log accepted it");
}

void testAFailedLogWriteIsRepeatedFromTheRequestWithoutANewCall()
{
    // A transient fault: the immediate retry from the stored accounting settles the row; nothing is reported.
    {
        Harness h;
        const auto request = h.authorized();
        h.executionStore.failNextSettlings = 1;
        h.client->push(okReply(validReply()));
        const auto done = h.processor.processAuthorized(request.id);
        check(done.success() && done.value().state == S::PlanReady, "A transient log fault is repaired in the same pass: " + codeOf(done.error()));
        const auto rows = h.executionsOf(request.id);
        check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Completed && rows[0].costMicros == expectedAccounting(h).costMicros,
            "...and the row holds the cost");
        check(h.client->calls() == 1, "...after one call");
    }

    // A lasting fault: the error is reported, the answer and its accounting are stored, the row stays unfinished.
    Harness h;
    const auto request = h.authorized();
    h.executionStore.failSettling = true;
    h.client->push(okReply(validReply()));
    const auto done = h.processor.processAuthorized(request.id);
    check(!done.success() && done.error()->code == "persistence.update_failed", "The failed write of the cost record is reported");
    const auto stored = h.stored(request.id);
    check(stored.state == S::PlanReady && stored.answer.has_value() && stored.accounting.has_value(),
        "...and the answer that was paid for is stored, WITH its accounting");
    const auto before = h.executionsOf(request.id);
    check(before.size() == 1 && before[0].status == ModelExecutionStatus::Running && before[0].costMicros == 0,
        "...while the execution row is still running with no cost");

    // The durable repair: nothing is lost, and the provider is not asked again.
    const auto stillBroken = h.processor.reconcileAccounting();
    check(!stillBroken.success() && stillBroken.error()->code == "persistence.update_failed", "While the log stays broken the repair reports it");
    h.executionStore.failSettling = false;
    const auto repaired = h.processor.reconcileAccounting();
    check(repaired.success() && repaired.value() == 1, "Once the log works, the repair writes the row: " + codeOf(repaired.error()));
    const auto after = h.executionsOf(request.id);
    check(after.size() == 1 && after[0].status == ModelExecutionStatus::Completed && after[0].inputTokens == 30'000
            && after[0].outputTokens == 2'000 && after[0].costMicros == expectedAccounting(h).costMicros
            && after[0].outputContent == validReply(),
        "...with the usage, the cost and the reply of the stored request");
    const auto again = h.processor.reconcileAccounting();
    check(again.success() && again.value() == 0, "A second repair has nothing to do");
    check(h.client->calls() == 1, "The provider was called exactly once through all of it");
}

// A stop that arrives while the provider is answering does not recall a COMPLETE reply: the request is cancelled, and the usage and
// price of that reply are kept - in the execution row and in the request.
void testAStopAfterACompleteReplyStillAccountsForIt()
{
    Harness h;
    const auto request = h.authorized();
    ScriptedReply reply = okReply(validReply());
    reply.blockUntilStop = true;
    reply.answerAfterStop = true;
    h.client->push(std::move(reply));
    auto running = std::async(std::launch::async, [&] { return h.processor.processAuthorized(request.id); });
    h.client->waitEntered(1);
    check(h.service.cancel(request.id).success(), "setup: stop the call");
    const auto done = running.get();
    check(done.success() && done.value().state == S::Cancelled && followUpRequestMayBeCharged(done.value()) && !done.value().answer.has_value(),
        "The request is cancelled and keeps no plan: " + codeOf(done.error()));
    check(done.value().accounting.has_value() && done.value().accounting->inputTokens == 30'000 && done.value().accounting->costMicros > 0,
        "...but it keeps the accounting of the reply that was paid for");
    const auto rows = h.executionsOf(request.id);
    check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Cancelled && rows[0].inputTokens == 30'000 && rows[0].costMicros > 0,
        "...and so does the execution row");
}

// The repair writes the outcome the settled request implies: a stopped call is cancelled, and a row that never started is started
// first (the call it records did happen).
void testTheRepairWritesTheOutcomeTheRequestImplies()
{
    Harness stopped;
    const auto request = stopped.authorized();
    stopped.executionStore.failSettling = true;
    ScriptedReply reply = okReply(validReply());
    reply.blockUntilStop = true;
    reply.answerAfterStop = true;
    stopped.client->push(std::move(reply));
    auto running = std::async(std::launch::async, [&] { return stopped.processor.processAuthorized(request.id); });
    stopped.client->waitEntered(1);
    check(stopped.service.cancel(request.id).success(), "setup: stop the call");
    const auto done = running.get();
    check(!done.success() && stopped.stored(request.id).state == S::Cancelled && stopped.stored(request.id).accounting.has_value(),
        "setup: the log write failed after a stopped call that was answered");
    stopped.executionStore.failSettling = false;
    check(stopped.processor.reconcileAccounting().success(), "The repair runs");
    const auto rows = stopped.executionsOf(request.id);
    check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Cancelled && rows[0].errorCode == "provider.cancelled"
            && rows[0].inputTokens == 30'000 && rows[0].costMicros > 0,
        "A stopped call is repaired as CANCELLED, with its usage and price");

    Harness ready;
    const auto plan = ready.planReady();
    CreateModelExecutionRequest pending;
    pending.source = ModelExecutionSource::FollowUp;
    pending.sourceId = plan.id;
    pending.projectId = kProject;
    pending.providerId = ready.catalog.providerId;
    pending.modelId = ready.catalog.modelId;
    pending.role = "follow_up_composer";
    pending.position = 1;   // the real row of the request holds position 0, attempt 1
    pending.requestHash = "r";
    pending.contextHash = "c";
    const auto created = ready.executions.createExecution(pending);
    check(created.success(), "setup: a row that never started");
    const auto repaired = ready.processor.reconcileAccounting();
    check(repaired.success() && repaired.value() == 1, "A row that never started is repaired: " + codeOf(repaired.error()));
    const auto row = ready.executionStore.inner.findExecution(created.value().id).value().value();
    check(row.status == ModelExecutionStatus::Completed && row.costMicros > 0, "...started and completed with the request's accounting");
}

void testTheCostOfAPaidAnswerIsRestoredAtStartup()
{
    Harness h;
    const auto request = h.authorized();
    h.executionStore.failSettling = true;
    h.client->push(okReply(validReply()));
    check(!h.processor.processAuthorized(request.id).success(), "setup: the log write failed");
    h.executionStore.failSettling = false;

    // A restarted process: a NEW processor over the same stores finds the unfinished row and writes it from the request.
    FollowUpComposerProcessor restarted(h.requests, h.reader, h.providers, h.models, h.factory, h.executions);
    const auto report = restarted.recoverAtStartup();
    check(report.success() && report.value().accountingRepaired == 1 && !report.value().accountingError.has_value(),
        "Startup recovery repairs the row: " + codeOf(report.error()));
    const auto rows = h.executionsOf(request.id);
    check(rows.size() == 1 && rows[0].status == ModelExecutionStatus::Completed && rows[0].costMicros == expectedAccounting(h).costMicros,
        "The cost of the paid answer is in the log after the restart");
    check(h.client->calls() == 1, "...and no provider was called");

    // A stopped call that was answered anyway is repaired as cancelled. A request whose call was never answered is owned by the
    // follow-up startup pass too: generic recovery deliberately skips this source so it can never erase paid accounting.
    Harness cancelled;
    const auto second = cancelled.authorized();
    cancelled.executionStore.failSettling = true;
    cancelled.client->push(blockingReply());
    auto running = std::async(std::launch::async, [&] { return cancelled.processor.processAuthorized(second.id); });
    cancelled.client->waitEntered(1);
    check(cancelled.service.cancel(second.id).success(), "setup: stop the call");
    const auto stopped = running.get();
    check(!stopped.success() && cancelled.stored(second.id).state == S::Cancelled && !cancelled.stored(second.id).accounting.has_value(),
        "A stop with no response has no accounting, even when the log write failed");
    const auto none = cancelled.processor.reconcileAccounting();
    check(none.success() && none.value() == 0, "...so accounting-only repair has nothing to invent");
    const auto failedRecovery = cancelled.processor.recoverAtStartup();
    check(failedRecovery.success() && failedRecovery.value().executionsInterrupted == 0
            && failedRecovery.value().executionError.has_value()
            && failedRecovery.value().executionError->code == "persistence.update_failed",
        "The specialised owner reports a row it could not close without hiding the other recovery results");
    check(cancelled.executionsOf(second.id)[0].status == ModelExecutionStatus::Running,
        "Its unfinished row remains available for a later specialised retry");
    check(cancelled.executions.recoverInterruptedExecutions().success(), "Generic recovery still completes its own pass");
    check(cancelled.executionsOf(second.id)[0].status == ModelExecutionStatus::Running,
        "Generic recovery cannot steal the failed follow-up repair");

    cancelled.executionStore.failSettling = false;
    const auto recovered = cancelled.processor.recoverAtStartup();
    check(recovered.success() && recovered.value().executionsInterrupted == 1
            && !recovered.value().executionError.has_value(),
        "The follow-up startup owner closes the no-response row: " + codeOf(recovered.error()));
    const auto noResponseRows = cancelled.executionsOf(second.id);
    check(noResponseRows.size() == 1 && noResponseRows[0].status == ModelExecutionStatus::Failed
            && noResponseRows[0].errorCode == kModelExecutionInterruptedErrorCode
            && noResponseRows[0].inputTokens == 0 && noResponseRows[0].costMicros == 0,
        "The no-response row is an unpriced interruption, not a fabricated paid result");
}

void testGenericExecutionRecoveryNeverOwnsFollowUpRows()
{
    Harness h;
    const auto create = [&](ModelExecutionSource source, const std::string& sourceId) {
        CreateModelExecutionRequest request;
        request.source = source;
        request.sourceId = sourceId;
        request.projectId = kProject;
        request.providerId = h.catalog.providerId;
        request.modelId = h.catalog.modelId;
        request.role = source == ModelExecutionSource::FollowUp ? "follow_up_composer" : "chat_assistant";
        request.requestHash = std::string(64, 'a');
        request.contextHash = std::string(64, 'b');
        const auto created = h.executions.createExecution(request);
        check(created.success() && h.executions.startExecution(created.value().id).success(), "setup: unfinished execution");
        return created.value().id;
    };
    const auto followUpId = create(ModelExecutionSource::FollowUp, "request-owned-elsewhere");
    const auto chatId = create(ModelExecutionSource::Chat, "chat-owned-generically");

    check(h.executions.recoverInterruptedExecutions().success(), "Generic recovery succeeds");
    const auto followUp = h.executionStore.inner.findExecution(followUpId).value().value();
    const auto chat = h.executionStore.inner.findExecution(chatId).value().value();
    check(followUp.status == ModelExecutionStatus::Running,
        "Generic recovery must leave follow-up rows to the request owner even when that owner is absent");
    check(chat.status == ModelExecutionStatus::Failed && chat.errorCode == kModelExecutionInterruptedErrorCode,
        "Generic recovery still closes the sources it owns");

    const auto specialised = h.processor.recoverAtStartup();
    check(specialised.success() && specialised.value().executionError.has_value()
            && specialised.value().executionError->code == "persistence.invalid_data",
        "The specialised owner reports a missing immutable source request as corrupt persistence");
    check(h.executionStore.inner.findExecution(followUpId).value()->status == ModelExecutionStatus::Running,
        "It leaves an orphaned execution untouched for diagnosis instead of inventing a no-response outcome");
}

// ---- recovery has no ceiling ----------------------------------------------------------------------------------------------------

void testStartupRecoveryWalksEveryRequestNotJustTheFirstPage()
{
    Harness h;
    constexpr int kPerGroup = 620;   // more than one page of 500, three different ways
    const auto base = quotedRequest(h.catalog, "base", kProject, "run-0", at(0));
    int runIndex = 0;
    const auto insert = [&](const std::string& id, std::int64_t createdMs) {
        auto request = base;
        request.id = id;
        request.sourceRunId = "run-" + std::to_string(++runIndex);
        request.createdAt = at(createdMs);
        request.quoteExpiresAt = request.createdAt + kFollowUpQuoteLifetime;
        request.updatedAt = request.createdAt;
        check(h.requests.insertQuoted(request).success(), "setup: insert " + id);
        FollowUpTransition authorize;
        authorize.event = FollowUpPlanEvent::Authorize;
        authorize.expectedState = S::Quoted;
        authorize.contextFingerprint = request.quote.contextFingerprint;
        check(h.requests.transition(id, authorize, at(createdMs)).success(), "setup: authorize " + id);
        return id;
    };
    const auto pad = [](int value) {
        std::string text = std::to_string(value);
        return std::string(5 - text.size(), '0') + text;
    };
    // composing: the same creation time for all of them (the id alone orders them), then claimed.
    for (int i = 0; i < kPerGroup; ++i) {
        const auto id = insert("composing-" + pad(i), 0);
        FollowUpTransition claim;
        claim.event = FollowUpPlanEvent::Claim;
        claim.expectedState = S::Authorized;
        check(h.requests.transition(id, claim, at(0)).success(), "setup: claim " + id);
    }
    // authorized, old: they expire.
    for (int i = 0; i < kPerGroup; ++i) insert("old-" + pad(i), 0);
    // authorized, live: they stay and are reported, all of them, in order.
    const std::int64_t liveCreated = 10 * 60 * 1000;
    for (int i = 0; i < kPerGroup; ++i) insert("live-" + pad(i), liveCreated);
    h.nowMs = 16 * 60 * 1000;

    const auto report = h.processor.recoverAtStartup();
    check(report.success(), "recovery succeeds: " + codeOf(report.error()));
    check(report.value().interrupted == kPerGroup, "EVERY orphaned composing request is interrupted, not the first 500: "
            + std::to_string(report.value().interrupted));
    check(report.value().expired == kPerGroup, "...every old authorisation expires: " + std::to_string(report.value().expired));
    check(report.value().toProcess.size() == kPerGroup && report.value().toProcess.front() == "live-" + pad(0)
            && report.value().toProcess.back() == "live-" + pad(kPerGroup - 1),
        "...and every live one is reported, oldest first, ties by id: " + std::to_string(report.value().toProcess.size()));
    check(h.requests.listByState(S::Composing, 1).value().empty() && h.requests.listByState(S::Authorized, 500).value().size() == 500,
        "Nothing is left composing; the live ones are still authorized");
    check(h.client->calls() == 0, "Recovery calls no provider");
}

// ---- an expired request is expired whatever the sources are doing ------------------------------------------------------------

void testAnExpiredRequestReadsNoSourceAndNothingCanHideItsExpiry()
{
    // authorize
    {
        Harness h;
        const auto request = h.quoted();
        h.reader.fail(common::Error {"persistence.query_failed", "the loader is down"});
        h.providers.failReads = true;
        h.nowMs = kQuoteLifetimeMs;
        const auto reads = h.reader.reads();
        const auto catalogReads = h.providers.reads.load();
        requireCode(h.service.authorize(request.id), "follow_up.quote_expired", "an expired quote with a broken loader and catalog");
        check(h.stored(request.id).state == S::Expired && !followUpRequestMayBeCharged(h.stored(request.id)), "...the request is expired and free");
        check(h.reader.reads() == reads && h.providers.reads.load() == catalogReads, "...and no source and no catalog was read");
        check(h.client->calls() == 0 && h.nudged.empty(), "...nothing was called or nudged");
    }
    // the processor
    {
        Harness h;
        const auto request = h.authorized();
        h.reader.fail(common::Error {"persistence.query_failed", "the loader is down"});
        h.providers.failReads = true;
        h.nowMs = kQuoteLifetimeMs;
        const auto reads = h.reader.reads();
        const auto catalogReads = h.providers.reads.load();
        requireCode(h.processor.processAuthorized(request.id), "follow_up.quote_expired", "an expired authorisation with a broken loader and catalog");
        check(h.stored(request.id).state == S::Expired && !followUpRequestMayBeCharged(h.stored(request.id)), "...the request is expired and free");
        check(h.reader.reads() == reads && h.providers.reads.load() == catalogReads, "...and no source and no catalog was read");
        check(h.client->calls() == 0 && h.factory.requests == 0 && h.executionsOf(request.id).empty(), "...no call, no client, no row");
    }
    // while the same broken loader does NOT hide a live request's problem: it is a fault, and the request is left alone
    {
        Harness h;
        const auto request = h.authorized();
        h.reader.fail(common::Error {"persistence.query_failed", "the loader is down"});
        requireCode(h.processor.processAuthorized(request.id), "persistence.query_failed", "a live request with a broken loader");
        check(h.stored(request.id).state == S::Authorized, "...is left authorized");
    }
}

// ---- the stale causes come in the promised order ------------------------------------------------------------------------------

void testWhenSeveralThingsChangedTheFirstCauseInTheContractIsReported()
{
    // project -> source -> outcome -> provider -> files -> context -> pricing: each pair below changes a LATER cause too, and the earlier
    // one must win.
    struct Change {
        const char* name;
        std::function<void(Harness&)> apply;
    };
    const Change project {"the project is archived", [](Harness& h) { h.reader.edit([](auto& m) { m.project.status = ProjectStatus::Archived; }); }};
    const Change source {"the Run moved to another project", [](Harness& h) { h.reader.edit([](auto& m) { m.runProjectId = "project-2"; }); }};
    const Change outcome {"the Outcome head moved on", [](Harness& h) { h.reader.edit([](auto& m) { m.brief = otherBrief(); }); }};
    const Change provider {"the provider is disabled", [](Harness& h) { h.editProvider([](Provider& p) { p.enabled = false; }); }};
    const Change recipient {"the base URL was edited", [](Harness& h) { h.editProvider([](Provider& p) { p.baseUrl += "x"; }); }};
    const Change files {"a pinned file changed", [](Harness& h) { h.reader.edit([](auto& m) { m.candidates[0] = candidate("src/a.cpp", "int a() { return 99; }\n"); }); }};
    const Change context {"the project snapshot changed", [](Harness& h) { h.reader.edit([](auto& m) { m.snapshot = otherSnapshot(); }); }};
    const Change pricing {"an output price changed", [](Harness& h) { h.editModel([](Model& m) { m.outputPriceMicrosPerMillionTokens += 1; }); }};
    const Change window {"the window shrank", [](Harness& h) { h.editModel([](Model& m) { m.contextWindowTokens = 5000; }); }};
    const Change unknownPrice {"the price became unknown", [](Harness& h) { h.editModel([](Model& m) { m.inputPriceMicrosPerMillionTokens = 0; }); }};
    struct Pair {
        Change first;
        Change later;
        const char* code;
    };
    const std::vector<Pair> pairs = {
        {project, source, "follow_up.stale_project"}, {project, outcome, "follow_up.stale_project"},
        {project, provider, "follow_up.stale_project"}, {project, pricing, "follow_up.stale_project"},
        {source, outcome, "follow_up.stale_source"}, {source, provider, "follow_up.stale_source"},
        {source, files, "follow_up.stale_source"},
        {outcome, provider, "follow_up.stale_outcome"}, {outcome, files, "follow_up.stale_outcome"},
        {outcome, context, "follow_up.stale_outcome"}, {outcome, pricing, "follow_up.stale_outcome"},
        {provider, files, "follow_up.stale_provider"}, {provider, context, "follow_up.stale_provider"},
        {provider, pricing, "follow_up.stale_provider"}, {provider, unknownPrice, "follow_up.stale_provider"},
        {recipient, files, "follow_up.stale_provider"}, {recipient, pricing, "follow_up.stale_provider"},
        {files, context, "follow_up.stale_files"}, {files, pricing, "follow_up.stale_files"},
        {context, pricing, "follow_up.stale_context"}, {context, window, "follow_up.stale_context"},
        {pricing, window, "follow_up.stale_pricing"}, {pricing, unknownPrice, "follow_up.stale_pricing"},
        {window, unknownPrice, "follow_up.stale_pricing"},
    };
    for (const auto& pair : pairs) {
        // At the authorisation ...
        Harness h;
        const auto request = h.quoted();
        // The later cause is applied FIRST, so that nothing depends on the order of the edits.
        pair.later.apply(h);
        pair.first.apply(h);
        requireCode(h.service.authorize(request.id), pair.code, std::string(pair.first.name) + " AND " + pair.later.name + " (authorize)");
        // ... and at the claim.
        Harness c;
        const auto live = c.authorized();
        pair.later.apply(c);
        pair.first.apply(c);
        requireCode(c.processor.processAuthorized(live.id), pair.code, std::string(pair.first.name) + " AND " + pair.later.name + " (claim)");
        check(c.stored(live.id).state == S::Stale && c.client->calls() == 0, "...the request is a free stale and nothing was called");
    }
}

// ---- the worker reports what it cannot fix ---------------------------------------------------------------------------------------

void testTheWorkerReportsEveryRealErrorAndNotTheExpectedRaces()
{
    Harness h;
    std::mutex mutex;
    std::vector<std::string> codes;
    FollowUpComposerRunner runner(h.processor);
    runner.setErrorSink([&](const common::Error& error) {
        std::lock_guard lock(mutex);
        codes.push_back(error.code);
    });
    h.service.setAuthorizedNudge({});

    // A lasting log fault after a paid answer reaches the sink.
    const auto request = h.authorized();
    h.executionStore.failSettling = true;
    h.client->push(okReply(validReply()));
    runner.nudge(request.id);
    runner.waitUntilIdle();
    {
        std::lock_guard lock(mutex);
        check(codes == std::vector<std::string> {"persistence.update_failed"}, "A failed write after a paid call is reported, not dropped");
    }

    // A request that is no longer authorized is a race, not an error.
    runner.nudge(request.id);
    runner.waitUntilIdle();
    {
        std::lock_guard lock(mutex);
        check(codes.size() == 1, "A nudge for a request that moved on reports nothing");
    }

    // The startup repair reports a row it still cannot write, and writes it once it can.
    runner.recoverAtStartup();
    runner.waitUntilIdle();
    {
        std::lock_guard lock(mutex);
        check(codes.size() == 2 && codes[1] == "persistence.update_failed", "The startup repair reports a row it cannot write");
    }
    h.executionStore.failSettling = false;
    runner.recoverAtStartup();
    runner.waitUntilIdle();
    check(h.executionsOf(request.id)[0].status == ModelExecutionStatus::Completed, "...and writes it once the log works");
    {
        std::lock_guard lock(mutex);
        check(codes.size() == 2, "...without a further report");
    }

    // The second recovery error channel (an unanswered call whose row cannot be
    // interrupted) reaches the same sink and remains retryable too.
    check(h.service.discard(request.id).success(), "setup: release the request slot");
    const auto unanswered = h.authorized();
    h.executionStore.failSettling = true;
    h.client->push(blockingReply());
    runner.nudge(unanswered.id);
    h.client->waitEntered(2);
    check(h.service.cancel(unanswered.id).success(), "setup: stop the unanswered call");
    runner.waitUntilIdle();
    {
        std::lock_guard lock(mutex);
        check(codes.size() == 3 && codes[2] == "persistence.update_failed",
            "The failed terminal write of the live call is reported");
    }
    runner.recoverAtStartup();
    runner.waitUntilIdle();
    {
        std::lock_guard lock(mutex);
        check(codes.size() == 4 && codes[3] == "persistence.update_failed",
            "The startup no-response repair reports its execution error through the runner");
    }
    h.executionStore.failSettling = false;
    runner.recoverAtStartup();
    runner.waitUntilIdle();
    check(h.executionsOf(unanswered.id)[0].status == ModelExecutionStatus::Failed,
        "The runner retries and closes the unanswered row once storage works");
}

// ---- preview, discard, confirm ----------------------------------------------------------------------------------------------

RunDraft rosterTemplate(const Harness& h, const std::string& id, const std::string& projectId = kProject)
{
    RunDraft draft;
    draft.id = id;
    draft.name = "Template";
    draft.projectId = projectId;
    draft.title = "Template";
    draft.task = "Template task";
    draft.workflowId = kExecutionManifestWorkflowId;
    draft.launchMode = RunLaunchMode::CustomRoster;
    StageModelAssignment assignment;
    assignment.providerId = h.catalog.providerId;
    assignment.modelId = h.catalog.modelId;
    assignment.agentName = "worker";
    assignment.role = "implementation";
    assignment.position = 0;
    draft.roster = {assignment};
    draft.maxParallelism = 3;
    draft.createdAt = at(1);
    draft.updatedAt = at(1);
    return draft;
}

void testThePreviewShowsTheStoredPlanAndConfirmSavesExactlyIt()
{
    Harness h;
    const auto ready = h.planReady();
    const auto view = h.service.preview(ready.id);
    check(view.success() && view.value().plan.has_value() && view.value().plan->drafts.size() == 2
            && view.value().plan->fingerprint == ready.answer->planFingerprint && view.value().advisories.size() == 2,
        "The preview is the stored plan with its advisories: " + codeOf(view.error()));

    requireCode(h.service.confirm(ready.id, std::string(64, '9')), "follow_up.plan_changed", "another plan than the previewed one");
    check(h.stored(ready.id).state == S::PlanReady && h.drafts.listAll().value().empty(), "...nothing was saved");

    h.nowMs = 9000;
    const auto saved = h.service.confirm(ready.id, ready.answer->planFingerprint);
    check(saved.success() && saved.value().created && saved.value().request.state == S::DraftsSaved, "Confirm saves the group: " + codeOf(saved.error()));
    const auto& group = saved.value().group;
    check(group.origin == RunDraftGroupOrigin::FollowUpRequest && group.sourceRequestId == std::optional<std::string>(ready.id)
            && group.contextFingerprint == std::optional<std::string>(ready.quote.contextFingerprint)
            && group.planFingerprint == ready.answer->planFingerprint && group.sourceRunIds == std::vector<std::string> {kRun}
            && group.projectId == kProject && group.createdAt == at(9000),
        "The group names its request, its sealed context, its plan and its source Run");
    check(saved.value().drafts.size() == 2 && saved.value().drafts[0].title == "Title k0" && saved.value().drafts[1].provenance->dependsOn == std::vector<std::string> {"k0"}
            && saved.value().drafts[0].interfaceContracts.size() == 2 && saved.value().drafts[0].roster.empty(),
        "The Drafts are the plan: titles, dependencies, final contracts, an empty roster without a template");
    check(h.client->calls() == 1, "Confirm never calls the provider again");

    const auto again = h.service.confirm(ready.id, ready.answer->planFingerprint);
    check(again.success() && !again.value().created && again.value().group.id == group.id && h.drafts.listAll().value().size() == 2,
        "A second Confirm returns the SAME group and writes nothing");
    check(h.service.quote(h.intent()).success(), "The slot is free again after the Confirm");
}

void testConfirmOptionsAreCheckedBeforeAnythingIsSaved()
{
    {
        Harness h;
        const auto ready = h.planReady();
        check(h.drafts.insert(rosterTemplate(h, "template-1")).success(), "setup: a template");
        FollowUpConfirmOptions options;
        options.rosterTemplate = "template-1";
        const auto saved = h.service.confirm(ready.id, ready.answer->planFingerprint, options);
        check(saved.success() && saved.value().drafts[0].roster.size() == 1 && saved.value().drafts[0].maxParallelism == 3
                && saved.value().drafts[1].roster.size() == 1,
            "A roster template's roster and parallelism are copied to every Draft");
    }
    struct Case {
        const char* what;
        std::function<void(Harness&, FollowUpConfirmOptions&)> edit;
        const char* code;
    };
    const std::vector<Case> cases = {
        {"a template that does not exist", [](Harness&, FollowUpConfirmOptions& o) { o.rosterTemplate = "nope"; }, "follow_up.template_not_found"},
        {"a template of another project", [](Harness& h, FollowUpConfirmOptions& o) { static_cast<void>(h.drafts.insert(rosterTemplate(h, "t", "project-2"))); o.rosterTemplate = "t"; }, "follow_up.template_wrong_project"},
        {"a template that is not a per-Run roster", [](Harness& h, FollowUpConfirmOptions& o) { auto t = rosterTemplate(h, "t"); t.launchMode = RunLaunchMode::Mock; static_cast<void>(h.drafts.insert(t)); o.rosterTemplate = "t"; }, "follow_up.template_not_manifest_roster"},
        {"a template without a roster", [](Harness& h, FollowUpConfirmOptions& o) { auto t = rosterTemplate(h, "t"); t.roster.clear(); static_cast<void>(h.drafts.insert(t)); o.rosterTemplate = "t"; }, "follow_up.template_without_roster"},
        {"a source Run named twice", [](Harness&, FollowUpConfirmOptions& o) { o.extraSourceRunIds = {"run-2", "run-2"}; }, "follow_up.duplicate_source_run"},
        {"the request's own Run named again", [](Harness&, FollowUpConfirmOptions& o) { o.extraSourceRunIds = {kRun}; }, "follow_up.duplicate_source_run"},
        {"too many source Runs", [](Harness&, FollowUpConfirmOptions& o) { for (int i = 0; i < 16; ++i) o.extraSourceRunIds.push_back("run-x" + std::to_string(i)); }, "follow_up.too_many_source_runs"},
        {"a source Run that does not exist", [](Harness&, FollowUpConfirmOptions& o) { o.extraSourceRunIds = {"ghost"}; }, "follow_up.source_run_not_found"},
    };
    for (const auto& item : cases) {
        Harness h;
        const auto ready = h.planReady();
        FollowUpConfirmOptions options;
        item.edit(h, options);
        const auto before = h.drafts.listAll().value().size();   // a template the case made, never a Draft of the plan
        requireCode(h.service.confirm(ready.id, ready.answer->planFingerprint, options), item.code, item.what);
        check(h.stored(ready.id).state == S::PlanReady && h.drafts.listAll().value().size() == before,
            std::string(item.what) + ": the plan is still ready and no Draft or group was saved");
    }
    {
        Harness h;
        const auto ready = h.planReady();
        FollowUpConfirmOptions options;
        options.extraSourceRunIds = {"run-2"};
        const auto saved = h.service.confirm(ready.id, ready.answer->planFingerprint, options);
        check(saved.success() && saved.value().group.sourceRunIds == std::vector<std::string> {kRun, "run-2"},
            "Further source Runs are added after the request's own");
    }
}

void testTheSuggestedParallelismIsCopiedOnlyWhenAskedFor()
{
    const std::string staffing = "\"staffing\":{\"workers\":[{\"role\":\"implementation\",\"focus\":\"a\"},"
                                 "{\"role\":\"implementation\",\"focus\":\"b\"}],\"parallelism\":2,\"reason\":\"r\"}";
    const std::string plan = "{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"drafts\":["
        "{\"key\":\"k0\",\"title\":\"One\",\"task\":\"Do a.\"," + staffing + "},"
        "{\"key\":\"k1\",\"title\":\"Two\",\"task\":\"Do b.\",\"dependsOn\":[\"k0\"]}]}";
    const auto confirmWith = [&plan](bool withTemplate, bool apply) {
        Harness h;
        const auto ready = h.planReady(replyOfPlanJson(plan));
        if (withTemplate) check(h.drafts.insert(rosterTemplate(h, "template-1")).success(), "setup: a template");
        FollowUpConfirmOptions options;
        if (withTemplate) options.rosterTemplate = "template-1";
        options.applySuggestedParallelism = apply;
        const auto saved = h.service.confirm(ready.id, ready.answer->planFingerprint, options);
        check(saved.success() && saved.value().drafts.size() == 2, "setup: confirmed: " + codeOf(saved.error()));
        return std::make_pair(saved.value().drafts[0], saved.value().drafts[1]);
    };
    {
        const auto drafts = confirmWith(false, false);
        check(drafts.first.maxParallelism == 4 && drafts.second.maxParallelism == 4 && drafts.first.roster.empty(),
            "Without the option the hint changes nothing: the default parallelism, no roster");
    }
    {
        const auto drafts = confirmWith(false, true);
        check(drafts.first.maxParallelism == 2 && drafts.second.maxParallelism == 4 && drafts.first.roster.empty() && drafts.second.roster.empty(),
            "With the option only the Draft that has a hint gets its parallelism; still no roster is created");
    }
    {
        const auto drafts = confirmWith(true, false);
        check(drafts.first.maxParallelism == 3 && drafts.second.maxParallelism == 3,
            "A template alone keeps its own parallelism for every Draft");
    }
    {
        const auto drafts = confirmWith(true, true);
        check(drafts.first.maxParallelism == 2 && drafts.second.maxParallelism == 3 && drafts.first.roster.size() == 1
                && drafts.second.roster.size() == 1,
            "Template and option: the hint wins for its Draft, the other Draft keeps the template's value, the template's roster stays");
    }
    {
        Harness h;
        const auto ready = h.planReady(replyOfPlanJson(plan));
        FollowUpConfirmOptions first;
        first.applySuggestedParallelism = true;
        const auto saved = h.service.confirm(ready.id, ready.answer->planFingerprint, first);
        check(saved.success(), "setup: confirmed");
        const auto again = h.service.confirm(ready.id, ready.answer->planFingerprint);
        check(again.success() && !again.value().created && again.value().drafts[0].maxParallelism == 2,
            "A repeated Confirm returns the stored group as it is and ignores the option");
    }
}

void testDiscardAndTheStatesThatCannotBeConfirmed()
{
    Harness h;
    const auto fresh = h.quoted();
    const auto dropped = h.service.discard(fresh.id);
    check(dropped.success() && dropped.value().state == S::Discarded && !followUpRequestMayBeCharged(dropped.value()), "Discarding a quote is free");

    Harness ready;
    const auto plan = ready.planReady();
    const auto discarded = ready.service.discard(plan.id);
    check(discarded.success() && discarded.value().state == S::Discarded && followUpRequestMayBeCharged(discarded.value())
            && discarded.value().answer.has_value(),
        "Discarding a ready plan is NOT free: it came from a paid call, and the answer stays");
    requireCode(ready.service.confirm(plan.id, plan.answer->planFingerprint), "follow_up.not_plan_ready", "confirming a discarded plan");
    check(ready.service.preview(plan.id).value().plan.has_value(), "...but the stored plan can still be read");

    requireCode(h.service.discard(fresh.id), "follow_up.invalid_transition", "discarding twice");
    requireCode(h.service.confirm(fresh.id, std::string(64, 'c')), "follow_up.not_plan_ready", "confirming a request that has no plan");
    requireCode(h.service.confirm("nope", std::string(64, 'c')), "follow_up.not_found", "an unknown request");
    check(h.drafts.listAll().value().empty(), "Nothing was saved");
}

// ---- the boundary -----------------------------------------------------------------------------------------------------------

std::string readSource(const std::string& relative)
{
    std::ifstream stream(std::string(FOLLOW_UP_SERVICE_SOURCE_DIR) + "/" + relative, std::ios::binary);
    check(stream.good(), "the source file can be read: " + relative);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// The service and the processor can neither start nor change what they must not: their headers and sources name no Run
// repository, no Run manager or workflow runner, no proposal / Apply / Work Ledger / Outcome Brief writer.
void testTheServiceAndTheProcessorNameNoRunOrProposalWriter()
{
    const std::vector<std::string> files = {"includes/ai/application/FollowUpPlanService.hpp", "includes/ai/application/FollowUpComposerProcessor.hpp",
        "includes/ai/application/FollowUpAdmission.hpp", "includes/ai/application/IFollowUpSourceReader.hpp",
        "sourceFiles/application/FollowUpPlanService.cpp", "sourceFiles/application/FollowUpComposerProcessor.cpp",
        "sourceFiles/application/FollowUpAdmission.cpp"};
    const std::vector<std::string> forbidden = {"RunManager", "IRunRepository", "IChangeProposalRepository", "IChangeProposalStore",
        "ChangeSetApplyService", "ChangeProposalService", "IManifestWorkLedgerRepository", "IOutcomeBriefRepository", "IOutcomeBriefTrigger",
        "OutcomeBriefReconciler", "ExecutionManifestExecutor", "startRun", "IWorkspaceStateRepository"};
    for (const auto& file : files) {
        // Only code counts: a comment may say what the file must NOT do.
        std::string text;
        {
            std::istringstream lines(readSource(file));
            std::string line;
            while (std::getline(lines, line)) {
                const auto comment = line.find("//");
                text += (comment == std::string::npos ? line : line.substr(0, comment)) + "\n";
            }
        }
        for (const auto& name : forbidden) {
            check(text.find(name) == std::string::npos, file + " must not mention " + name);
        }
        // The one thing taken from the workflow header is the Manifest workflow id constant, in the service source only.
        const bool includesRunner = text.find("IWorkflowRunner") != std::string::npos;
        check(!includesRunner || file == "sourceFiles/application/FollowUpPlanService.cpp",
            file + " must not include the workflow runner header");
    }
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"testAQuoteIsOneImmutableRecordAndSendsNothing", testAQuoteIsOneImmutableRecordAndSendsNothing},
        {"testAQuoteRefusalIsTypedFreeAndStoresNothing", testAQuoteRefusalIsTypedFreeAndStoresNothing},
        {"testASecondQuoteReplacesTheFirstAndAnOpenRequestIsNeverDisplaced", testASecondQuoteReplacesTheFirstAndAnOpenRequestIsNeverDisplaced},
        {"testTheFilesOfAQuoteAreTheOnesTheUserSelected", testTheFilesOfAQuoteAreTheOnesTheUserSelected},
        {"testAuthorizeChecksAgainAndNudgesTheProcessor", testAuthorizeChecksAgainAndNudgesTheProcessor},
        {"testTheCapAndTheSelectionSurviveTheAuthorisationAndTheClaim", testTheCapAndTheSelectionSurviveTheAuthorisationAndTheClaim},
        {"testAnAuthorisationAfterTheDeadlineExpiresTheRequest", testAnAuthorisationAfterTheDeadlineExpiresTheRequest},
        {"testEveryDriftBeforeTheCallIsAFreeStale", testEveryDriftBeforeTheCallIsAFreeStale},
        {"testOneAuthorizedRequestMakesExactlyOneCallAndStoresItsPlan", testOneAuthorizedRequestMakesExactlyOneCallAndStoresItsPlan},
        {"testNameAndStaffingAdvisoriesAreStoredWithThePlan", testNameAndStaffingAdvisoriesAreStoredWithThePlan},
        {"testAnAnswerThatIsNotOneValidPlanIsStoredAsAFailureAfterBeingPaidFor", testAnAnswerThatIsNotOneValidPlanIsStoredAsAFailureAfterBeingPaidFor},
        {"testAProviderFailureIsStoredAndNeverRetried", testAProviderFailureIsStoredAndNeverRetried},
        {"testAClientThatCannotBeResolvedFailsTheRequestWithoutACall", testAClientThatCannotBeResolvedFailsTheRequestWithoutACall},
        {"testTheUsageOfAPaidResponseIsStoredWithTheRequest", testTheUsageOfAPaidResponseIsStoredWithTheRequest},
        {"testAUsageTheLogWouldRefuseIsStoredWithoutItsUntrustedBreakdown", testAUsageTheLogWouldRefuseIsStoredWithoutItsUntrustedBreakdown},
        {"testAFailedLogWriteIsRepeatedFromTheRequestWithoutANewCall", testAFailedLogWriteIsRepeatedFromTheRequestWithoutANewCall},
        {"testAStopAfterACompleteReplyStillAccountsForIt", testAStopAfterACompleteReplyStillAccountsForIt},
        {"testTheRepairWritesTheOutcomeTheRequestImplies", testTheRepairWritesTheOutcomeTheRequestImplies},
        {"testTheCostOfAPaidAnswerIsRestoredAtStartup", testTheCostOfAPaidAnswerIsRestoredAtStartup},
        {"testGenericExecutionRecoveryNeverOwnsFollowUpRows", testGenericExecutionRecoveryNeverOwnsFollowUpRows},
        {"testStartupRecoveryWalksEveryRequestNotJustTheFirstPage", testStartupRecoveryWalksEveryRequestNotJustTheFirstPage},
        {"testAnExpiredRequestReadsNoSourceAndNothingCanHideItsExpiry", testAnExpiredRequestReadsNoSourceAndNothingCanHideItsExpiry},
        {"testWhenSeveralThingsChangedTheFirstCauseInTheContractIsReported", testWhenSeveralThingsChangedTheFirstCauseInTheContractIsReported},
        {"testTheWorkerReportsEveryRealErrorAndNotTheExpectedRaces", testTheWorkerReportsEveryRealErrorAndNotTheExpectedRaces},
        {"testDescribeFilesAppliesTheSameGateAsAQuote", testDescribeFilesAppliesTheSameGateAsAQuote},
        {"testTheCostRecordIsWrittenBeforeTheCall", testTheCostRecordIsWrittenBeforeTheCall},
        {"testADriftAtTheClaimIsAFreeStaleAndNoCallIsMade", testADriftAtTheClaimIsAFreeStaleAndNoCallIsMade},
        {"testCancellingBeforeTheClaimIsFreeAndNothingIsCalled", testCancellingBeforeTheClaimIsFreeAndNothingIsCalled},
        {"testStoppingTheRunningCallCancelsTheRequestAsPossiblyCharged", testStoppingTheRunningCallCancelsTheRequestAsPossiblyCharged},
        {"testTwoPassesOverOneRequestMakeOneCall", testTwoPassesOverOneRequestMakeOneCall},
        {"testASecondPassWhileTheFirstIsInTheCallIsRefused", testASecondPassWhileTheFirstIsInTheCallIsRefused},
        {"testStartupRecoveryInterruptsExpiresAndReportsTheRest", testStartupRecoveryInterruptsExpiresAndReportsTheRest},
        {"testTheWorkerProcessesWhatIsNudgedAndWhatStartupFinds", testTheWorkerProcessesWhatIsNudgedAndWhatStartupFinds},
        {"testAGuestRuntimeReadsAndWritesNothing", testAGuestRuntimeReadsAndWritesNothing},
        {"testThePreviewShowsTheStoredPlanAndConfirmSavesExactlyIt", testThePreviewShowsTheStoredPlanAndConfirmSavesExactlyIt},
        {"testTheSuggestedParallelismIsCopiedOnlyWhenAskedFor", testTheSuggestedParallelismIsCopiedOnlyWhenAskedFor},
        {"testConfirmOptionsAreCheckedBeforeAnythingIsSaved", testConfirmOptionsAreCheckedBeforeAnythingIsSaved},
        {"testDiscardAndTheStatesThatCannotBeConfirmed", testDiscardAndTheStatesThatCannotBeConfirmed},
        {"testTheServiceAndTheProcessorNameNoRunOrProposalWriter", testTheServiceAndTheProcessorNameNoRunOrProposalWriter},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& exception) {
            std::cerr << "FAILED " << name << ": " << exception.what() << '\n';
            ++failures;
        }
    }
    if (failures == 0) std::cout << "FollowUpPlanService tests passed\n";
    return failures == 0 ? 0 : 1;
}
