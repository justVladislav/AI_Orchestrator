#pragma once

// PC-5B test fixtures shared by the pure-rule tests, the in-memory contract run and the SQLite tests. Everything is built
// with the REAL functions (the context assembler, the ACD-4 quote), so a request in these tests is what 5C would store.

#include "ai/application/FollowUpComposerContext.hpp"
#include "ai/application/FollowUpComposerQuote.hpp"
#include "ai/application/FollowUpPlanRequest.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/Sha256.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace followup_test {

using namespace ai::application;

inline void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

inline std::string hex64(char digit) { return std::string(64, digit); }

// Fixed whole-millisecond timestamps.
inline std::chrono::system_clock::time_point at(std::int64_t offsetMs)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'000 + offsetMs));
}

inline constexpr std::int64_t kQuoteLifetimeMs = 15 * 60 * 1000;

inline ProjectContextItem item(std::string text, std::string kind, std::string id, bool mandatory = false,
    std::string digest = {})
{
    ProjectContextItem value;
    value.text = std::move(text);
    value.source.kind = std::move(kind);
    value.source.id = std::move(id);
    value.source.digest = std::move(digest);
    value.mandatory = mandatory;
    return value;
}

inline ProjectContextSnapshot snapshotOf(const std::string& runId, const std::string& projectId)
{
    const std::string workspace(64, 'a');
    ProjectContextAssemblyInput input;
    input.projectId = projectId;
    input.selectedRunId = runId;
    input.workspaceFingerprint = workspace;
    input.sections = {
        {ProjectContextCategory::ProjectMetadata, {item("Project: Demo", "project", projectId, true)}},
        {ProjectContextCategory::SelectedLineage, {item("selected run " + runId, "run", runId, true)}},
        {ProjectContextCategory::WorkLedger, {item("resolved no change", "ledger_entry", runId)}},
        {ProjectContextCategory::WorkspaceCatalog,
            {item("workspace identity", "workspace", "current", true, workspace),
             item("src/a.cpp", "workspace_file", "src/a.cpp")}}};
    const auto result = assembleProjectContextSnapshot(input);
    check(result.success(), "Snapshot fixture must assemble");
    return result.value();
}

inline OutcomeBrief briefOf(const std::string& runId, const std::string& projectId)
{
    ManifestWorkLedgerEntry ledger;
    ledger.runId = runId;
    ledger.projectId = projectId;
    ledger.terminalStatus = RunStatus::Completed;
    ledger.disposition = ManifestWorkLedgerDisposition::NoChangesNeeded;
    ledger.baselineFingerprint = std::string(64, 'a');
    OutcomeBriefFacts facts;
    facts.runId = runId;
    facts.projectId = projectId;
    facts.workflowId = kExecutionManifestWorkflowId;
    facts.terminalStatus = RunStatus::Completed;
    facts.ledgerEntry = ledger;
    const auto result = deriveOutcomeBrief(facts);
    check(result.success(), "Brief fixture must derive");
    return result.value();
}

// The REAL sealed context of a source Run.
inline FollowUpComposerContext contextOf(const std::string& runId, const std::string& projectId,
    std::uint32_t maxDrafts = 2, FollowUpSplitHint hint = FollowUpSplitHint::Auto)
{
    FollowUpComposerContextRequest request;
    request.sourceRunId = runId;
    request.kind = FollowUpPlanKind::NextRun;
    request.splitHint = hint;
    request.maxDrafts = maxDrafts;
    request.goal = "Plan the next focused implementation step.";
    request.snapshot = snapshotOf(runId, projectId);
    request.brief = briefOf(runId, projectId);
    const auto result = assembleFollowUpComposerContext(request);
    check(result.success(), "Context fixture must assemble");
    return result.value();
}

// Existing rows in a SQLite database; in-memory contracts only need the ids.
struct Catalog {
    std::string providerId {"provider-1"};
    std::string modelId {"model-1"};
    std::string providerKind {"anthropic"};
    std::string baseUrl {"https://api.example.test/v1"};
    std::optional<std::string> credentialReference {std::string("credential-a")};
};

inline Provider providerOf(const Catalog& catalog)
{
    Provider provider;
    provider.id = catalog.providerId;
    provider.kind = catalog.providerKind;
    provider.baseUrl = catalog.baseUrl;
    provider.credentialReference = catalog.credentialReference;
    return provider;
}

inline Model modelOf(const Catalog& catalog)
{
    Model model;
    model.id = catalog.modelId;
    model.providerId = catalog.providerId;
    model.apiModelId = "model-api";
    model.contextWindowTokens = 200'000;
    model.maxOutputTokens = 64'000;
    model.inputPriceMicrosPerMillionTokens = 3'000'000;
    model.outputPriceMicrosPerMillionTokens = 15'000'000;
    model.cacheWritePriceMicrosPerMillionTokens = 3'750'000;
    model.cacheReadPriceMicrosPerMillionTokens = 300'000;
    model.cacheWriteOneHourPriceMicrosPerMillionTokens = 6'000'000;
    return model;
}

inline FollowUpPinnedFile pinnedFile(std::string path = "src/a.cpp", char digit = 'b')
{
    return {std::move(path), hex64(digit), 100, 100, 40, false};
}

// A quoted request exactly as the service would store it: a real context, a real ACD-4 quote.
inline FollowUpPlanRequest quotedRequest(const Catalog& catalog, std::string id, const std::string& projectId,
    const std::string& runId, std::chrono::system_clock::time_point createdAt = at(0))
{
    const auto context = contextOf(runId, projectId);
    const auto quote = quoteFollowUpComposerRequest(context, providerOf(catalog), modelOf(catalog), std::nullopt);
    check(quote.success(), "The fixture quote must succeed: " + (quote.success() ? std::string() : quote.error()->code));
    FollowUpPlanRequest request;
    request.id = std::move(id);
    request.projectId = projectId;
    request.sourceRunId = runId;
    request.kind = FollowUpPlanKind::NextRun;
    request.goal = "Plan the next focused implementation step.";
    request.maxDrafts = 2;
    request.splitHint = FollowUpSplitHint::Auto;
    request.outcomeSourceStateFingerprint = hex64('1');
    request.snapshotFingerprint = snapshotOf(runId, projectId).fingerprint;
    request.files = {pinnedFile()};
    request.quote = quote.value();
    request.state = FollowUpPlanRequestState::Quoted;
    request.createdAt = createdAt;
    request.quoteExpiresAt = createdAt + kFollowUpQuoteLifetime;
    request.updatedAt = createdAt;
    return request;
}

// A REAL next-run-plan.v1 of `drafts` drafts (keys k0, k1, ...; each depends on the one before). `variant` is part of the plan
// title, so two variants are two plans with two fingerprints.
// `contracts`: one shared contract (S1) that every draft lists, and one contract of the first draft's own (O1), so a draft's final
// contracts are [S1, O1] for k0 and [S1] for the others.
inline std::string planJsonOf(char variant, std::size_t drafts, bool contracts = false)
{
    std::string json = std::string("{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan ") + variant + "\",";
    if (contracts) {
        json += "\"sharedContracts\":[{\"id\":\"S1\",\"kind\":\"interface_contract\",\"title\":\"Shared shape\","
                "\"text\":\"property string name\",\"appliesToPaths\":[\"src/models\"]}],";
    }
    json += "\"drafts\":[";
    for (std::size_t index = 0; index < drafts; ++index) {
        const std::string key = "k" + std::to_string(index);
        if (index > 0) json += ",";
        json += "{\"key\":\"" + key + "\",\"title\":\"Title " + key + "\",\"task\":\"Task for " + key + " (" + variant + ")\"";
        if (index > 0) json += ",\"dependsOn\":[\"k" + std::to_string(index - 1) + "\"]";
        if (contracts) {
            json += ",\"sharedContractIds\":[\"S1\"]";
            if (index == 0) {
                json += ",\"contracts\":[{\"id\":\"O1\",\"kind\":\"constraint\",\"title\":\"Own rule\","
                        "\"text\":\"keep it small\",\"appliesToPaths\":[\"src/views\"]}]";
            }
        }
        json += "}";
    }
    return json + "]}";
}

inline std::string replyOfPlanJson(const std::string& json)
{
    return "Here is the plan.\n```next-run-plan.v1\n" + json + "\n```\n";
}

// The answer the composer would return: the reply with ONE envelope and the parsed plan's own fingerprint.
inline FollowUpPlanAnswer answerOf(char variant = 'c', std::size_t drafts = 2, bool contracts = false)
{
    const std::string json = planJsonOf(variant, drafts, contracts);
    const auto plan = parseNextRunPlan(json);
    check(plan.success(), "The fixture plan must parse: " + (plan.success() ? std::string() : plan.error()->code));
    return {replyOfPlanJson(json), plan.value().fingerprint, "[]"};
}

// The plan an answer carries, for a request that authorised `maxDrafts` with the `auto` hint.
inline NextRunPlan planOfAnswer(const FollowUpPlanAnswer& answer, std::uint32_t maxDrafts = 4)
{
    const auto plan = parseFollowUpAnswerPlan(answer, FollowUpSplitHint::Auto, maxDrafts);
    check(plan.success(), "The fixture answer must carry a valid plan: " + (plan.success() ? std::string() : plan.error()->code));
    return plan.value();
}

// The Drafts of `plan` the way RunDraftPlanService materialises them (title = name, the plan's contracts, a per-Run Manifest roster,
// the content fingerprint of the plan's draft), minimal otherwise: for the pure rules, which never store them.
inline std::vector<RunDraft> draftsOfPlan(const NextRunPlan& plan, const std::string& groupId, const std::string& projectId)
{
    std::vector<RunDraft> drafts;
    for (std::size_t ordinal = 0; ordinal < plan.drafts.size(); ++ordinal) {
        const auto& planned = plan.drafts[ordinal];
        RunDraft draft;
        draft.id = groupId + "-draft-" + std::to_string(ordinal);
        draft.name = planned.title;
        draft.projectId = projectId;
        draft.title = planned.title;
        draft.task = planned.task;
        draft.workflowId = kExecutionManifestWorkflowId;
        draft.launchMode = RunLaunchMode::CustomRoster;
        draft.interfaceContracts = planned.contracts;
        RunDraftProvenance provenance;
        provenance.groupId = groupId;
        provenance.ordinal = static_cast<std::uint32_t>(ordinal);
        provenance.planKey = planned.key;
        provenance.dependsOn = planned.dependsOn;
        provenance.createdContentFingerprint =
            nextRunPlanDraftContentFingerprint(planned.title, planned.task, planned.contracts, planned.dependsOn);
        draft.provenance = std::move(provenance);
        drafts.push_back(std::move(draft));
    }
    return drafts;
}

inline FollowUpTransition authorizeStep(const FollowUpPlanRequest& request)
{
    FollowUpTransition step;
    step.event = FollowUpPlanEvent::Authorize;
    step.expectedState = FollowUpPlanRequestState::Quoted;
    step.contextFingerprint = request.quote.contextFingerprint;
    return step;
}

inline FollowUpTransition plainStep(FollowUpPlanEvent event, FollowUpPlanRequestState expected)
{
    FollowUpTransition step;
    step.event = event;
    step.expectedState = expected;
    return step;
}

// What the provider reported for the paid call and what it costs: 30 000 input and 2 000 output tokens at the fixture prices.
inline FollowUpAccounting accountingOf()
{
    FollowUpAccounting accounting;
    accounting.inputTokens = 30'000;
    accounting.outputTokens = 2'000;
    accounting.costMicros = 120'000;
    accounting.priceKnown = true;
    accounting.latencyMs = 1'500;
    return accounting;
}

inline FollowUpTransition planStep(FollowUpPlanAnswer answer)
{
    FollowUpTransition step = plainStep(FollowUpPlanEvent::PlanSucceeded, FollowUpPlanRequestState::Composing);
    step.answer = std::move(answer);
    step.accounting = accountingOf();
    return step;
}

inline FollowUpTransition failStep(FollowUpPlanFailure failure)
{
    FollowUpTransition step = plainStep(FollowUpPlanEvent::CompositionFailed, FollowUpPlanRequestState::Composing);
    step.failure = std::move(failure);
    return step;
}

// Field-by-field equality of two requests (the quote has no operator==).
inline bool sameQuote(const FollowUpComposerQuote& a, const FollowUpComposerQuote& b)
{
    return a.policyId == b.policyId && a.contextFingerprint == b.contextFingerprint
        && a.providerIdentity == b.providerIdentity && a.providerIdentityFingerprint == b.providerIdentityFingerprint
        && a.worstCase.pricingIdentity == b.worstCase.pricingIdentity
        && a.worstCase.pricingIdentityFingerprint == b.worstCase.pricingIdentityFingerprint
        && a.worstCase.estimatedInputTokens == b.worstCase.estimatedInputTokens
        && a.worstCase.cacheableInputTokens == b.worstCase.cacheableInputTokens
        && a.worstCase.maximumOutputTokens == b.worstCase.maximumOutputTokens
        && a.worstCase.estimatedCostMicros == b.worstCase.estimatedCostMicros
        && a.worstCase.priceKnown == b.worstCase.priceKnown && a.capMicros == b.capMicros;
}

inline bool sameRequest(const FollowUpPlanRequest& a, const FollowUpPlanRequest& b)
{
    return a.id == b.id && a.projectId == b.projectId && a.sourceRunId == b.sourceRunId && a.kind == b.kind
        && a.goal == b.goal && a.maxDrafts == b.maxDrafts && a.splitHint == b.splitHint
        && a.outcomeSourceStateFingerprint == b.outcomeSourceStateFingerprint
        && a.snapshotFingerprint == b.snapshotFingerprint && a.files == b.files && sameQuote(a.quote, b.quote)
        && a.state == b.state && a.createdAt == b.createdAt && a.quoteExpiresAt == b.quoteExpiresAt
        && a.updatedAt == b.updatedAt && a.authorizedAt == b.authorizedAt && a.claimedAt == b.claimedAt
        && a.finishedAt == b.finishedAt && a.answer == b.answer && a.failure == b.failure
        && a.draftGroupId == b.draftGroupId && a.accounting == b.accounting;
}

inline std::string codeOf(const std::optional<ai::common::Error>& error)
{
    return error.has_value() ? error->code : std::string("<no error>");
}

template <typename T>
void requireCode(const ai::common::Result<T>& result, std::string_view code, const std::string& what)
{
    if (result.success()) throw std::runtime_error(what + ": expected " + std::string(code) + " but the call succeeded");
    if (result.error()->code != code) {
        throw std::runtime_error(what + ": expected " + std::string(code) + " but got " + result.error()->code + " ("
            + result.error()->message + ")");
    }
}

} // namespace followup_test
