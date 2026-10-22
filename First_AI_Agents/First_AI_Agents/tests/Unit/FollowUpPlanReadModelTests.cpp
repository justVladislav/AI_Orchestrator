// PC-5D: the read model behind the follow-up dialog and the Outcome block - which Runs may be planned from (the 5A-1 gate over the
// Run's CURRENT brief), what holds the Run's slot, the status line's request, the model picker's advisory filter, the roster templates
// and the source-Run choices - and its guest refusal. Over the real in-memory repositories; nothing is written by it.

#include "FollowUpRequestTestSupport.hpp"

#include "ai/application/FollowUpPlanReadModel.hpp"
#include "ai/application/InMemoryFollowUpPlanRepository.hpp"
#include "ai/application/InMemoryOutcomeBriefRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace ai::application;
using namespace followup_test;

namespace {

constexpr const char* kProject = "project-1";

std::string codeOf(const std::optional<ai::common::Error>& error) { return error.has_value() ? error->code : std::string("<no error>"); }

OutcomeBrief briefFor(const std::string& runId, RunStatus status, const std::string& workflow = kExecutionManifestWorkflowId)
{
    OutcomeBriefFacts facts;
    facts.runId = runId;
    facts.projectId = kProject;
    facts.workflowId = workflow;
    facts.terminalStatus = status;
    {
        ManifestWorkLedgerEntry ledger;
        ledger.runId = runId;
        ledger.projectId = kProject;
        ledger.terminalStatus = status;
        ledger.disposition = status == RunStatus::Completed ? ManifestWorkLedgerDisposition::NoChangesNeeded
                                                            : ManifestWorkLedgerDisposition::NotProduced;
        ledger.baselineFingerprint = std::string(64, 'a');
        facts.ledgerEntry = ledger;
    }
    const auto derived = deriveOutcomeBrief(facts);
    check(derived.success(), "setup: the brief derives: " + (derived.success() ? std::string() : derived.error()->code));
    return derived.value();
}

class FixedRuns final : public IRunSourceLookup {
public:
    [[nodiscard]] ai::common::Result<std::optional<ProjectId>> findRunProject(const RunId& runId) const override
    {
        const auto found = projects.find(runId);
        return ai::common::Result<std::optional<ProjectId>>::ok(found == projects.end() ? std::nullopt : std::optional<ProjectId>(found->second));
    }
    [[nodiscard]] ai::common::Result<std::vector<RunSourceChoice>> listProjectRuns(const ProjectId& projectId) const override
    {
        std::vector<RunSourceChoice> result;
        for (const auto& choice : choices) if (projectId == kProject) result.push_back(choice);
        return ai::common::Result<std::vector<RunSourceChoice>>::ok(std::move(result));
    }
    std::map<RunId, ProjectId> projects;
    std::vector<RunSourceChoice> choices;
};

struct Fixture {
    std::map<RunId, OutcomeBriefRunOwner> owners;
    InMemoryOutcomeBriefRepository briefs {[this](const RunId& id) -> std::optional<OutcomeBriefRunOwner> {
        const auto found = owners.find(id);
        if (found == owners.end()) return std::nullopt;
        return found->second;
    }};
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository requests {[](const std::string&) -> std::optional<ProjectId> { return std::string(kProject); }, &drafts};
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    FixedRuns runs;
    Catalog catalog;
    FollowUpPlanReadModel readModel {briefs, requests, providers, models, drafts, runs};

    void addRun(const std::string& id, RunStatus status, const std::string& workflow = kExecutionManifestWorkflowId)
    {
        owners[id] = OutcomeBriefRunOwner {kProject, status};
        runs.projects[id] = kProject;
        check(briefs.append(briefFor(id, status, workflow), OutcomeBriefExpectedHead::none()).success(), "setup: brief of " + id);
    }
    void quote(const std::string& requestId, const std::string& runId, std::int64_t at_ = 0)
    {
        check(requests.insertQuoted(quotedRequest(catalog, requestId, kProject, runId, at(at_))).success(), "setup: quote " + requestId);
    }
};

void testTheGateDecidesWhatEachRunIsOfferedAndAllowed()
{
    Fixture f;
    f.addRun("run-ok", RunStatus::Completed);
    f.addRun("run-failed", RunStatus::Failed);
    f.addRun("run-cancelled", RunStatus::Cancelled);

    const auto ok = f.readModel.eligibility("run-ok");
    check(ok.success() && ok.value().availability == FollowUpRunEligibility::Availability::Evaluated, "a Completed Run is evaluated: " + codeOf(ok.error()));
    check(ok.value().nextRun.offered && ok.value().nextRun.allowed && ok.value().nextRun.reasonCode.empty(), "a resolved Completed Run is offered and allowed a next-Run plan");
    check(!ok.value().recovery.offered && !ok.value().recovery.allowed, "...and is not offered a recovery plan");
    check(ok.value().projectId == kProject, "the project comes from the brief");

    const auto failed = f.readModel.eligibility("run-failed");
    check(failed.success() && failed.value().recovery.offered && failed.value().recovery.allowed,
        "a Failed Run nothing owns is offered and allowed the recovery plan: " + codeOf(failed.error()));
    check(!failed.value().nextRun.offered && !failed.value().nextRun.allowed, "...and is not offered a next-Run plan");

    const auto cancelled = f.readModel.eligibility("run-cancelled");
    check(cancelled.success() && cancelled.value().nextRun.offered && !cancelled.value().nextRun.allowed
            && cancelled.value().nextRun.reasonCode.rfind("follow_up.", 0) == 0 && !cancelled.value().nextRun.reasonMessage.empty(),
        "a Cancelled Run is refused by the gate with its own typed code: " + cancelled.value().nextRun.reasonCode);
    check(!cancelled.value().recovery.offered, "...and recovery is not offered for it either");

}

void testARunWithoutABriefAndAnEmptyIdShowNothing()
{
    Fixture f;
    const auto unknown = f.readModel.eligibility("no-such-run");
    check(unknown.success() && unknown.value().availability == FollowUpRunEligibility::Availability::NotApplicable
            && !unknown.value().nextRun.offered && !unknown.value().recovery.offered && !unknown.value().openRequest.has_value(),
        "an unknown Run is not applicable, never an error: " + codeOf(unknown.error()));
    const auto empty = f.readModel.eligibility("");
    check(empty.success() && empty.value().availability == FollowUpRunEligibility::Availability::NotApplicable, "an empty id is not applicable");
}

void testAnOpenRequestHoldsTheSlotAndTheLatestOneIsTheStatusLine()
{
    Fixture f;
    f.addRun("run-ok", RunStatus::Completed);
    f.quote("req-1", "run-ok");

    auto view = f.readModel.eligibility("run-ok");
    check(view.success() && view.value().openRequest.has_value() && view.value().openRequest->id == "req-1", "the quoted request holds the slot");
    check(view.value().nextRun.offered && !view.value().nextRun.allowed && view.value().nextRun.reasonCode == "follow_up.request_open",
        "...so the plan is not allowed and says why: " + view.value().nextRun.reasonCode);
    check(view.value().latestRequest.has_value() && view.value().latestRequest->id == "req-1", "...and it is also the status line's request");

    // A cancelled request frees the slot but stays visible as the latest one.
    check(f.requests.transition("req-1", plainStep(FollowUpPlanEvent::Cancel, FollowUpPlanRequestState::Quoted), at(1)).success(), "setup: cancel");
    view = f.readModel.eligibility("run-ok");
    check(view.success() && !view.value().openRequest.has_value() && view.value().nextRun.allowed, "a cancelled request frees the slot");
    check(view.value().latestRequest.has_value() && view.value().latestRequest->state == FollowUpPlanRequestState::Cancelled,
        "...and stays visible as the latest one");

    // A newer request replaces it in the status line; a DISCARDED newest request shows nothing (the user dismissed it).
    f.quote("req-2", "run-ok", 10);
    view = f.readModel.eligibility("run-ok");
    check(view.value().latestRequest.has_value() && view.value().latestRequest->id == "req-2", "the newest request is the status line's");
    check(f.requests.transition("req-2", plainStep(FollowUpPlanEvent::Discard, FollowUpPlanRequestState::Quoted), at(11)).success(), "setup: discard");
    view = f.readModel.eligibility("run-ok");
    check(view.success() && !view.value().latestRequest.has_value() && view.value().nextRun.allowed,
        "a discarded newest request shows nothing and an older terminal one is not dug up");
}

void testAGuestReadsNothing()
{
    Fixture f;
    f.addRun("run-ok", RunStatus::Completed);
    const ai::common::Error refusal {"follow_up.not_the_owner", "guest"};
    const FollowUpPlanReadModel guest(f.briefs, f.requests, f.providers, f.models, f.drafts, f.runs, refusal);
    check(!guest.eligibility("run-ok").success() && guest.eligibility("run-ok").error()->code == "follow_up.not_the_owner", "eligibility refuses");
    check(!guest.modelChoices().success() && guest.modelChoices().error()->code == "follow_up.not_the_owner", "modelChoices refuses");
    check(!guest.rosterTemplates(kProject).success() && guest.rosterTemplates(kProject).error()->code == "follow_up.not_the_owner", "rosterTemplates refuses");
    check(!guest.sourceRunChoices(kProject).success() && guest.sourceRunChoices(kProject).error()->code == "follow_up.not_the_owner", "sourceRunChoices refuses");
}

void testTheModelPickerShowsWhyAModelIsNotOffered()
{
    Fixture f;
    const auto add = [&](const std::string& providerId, bool providerEnabled, const std::string& modelId, bool modelEnabled,
                         std::uint64_t input, std::uint64_t output, std::uint64_t maxOutput) {
        if (!f.providers.findById(providerId).value().has_value()) {
            Provider provider = providerOf(f.catalog);
            provider.id = providerId;
            provider.name = "Name " + providerId;
            provider.enabled = providerEnabled;
            check(f.providers.insert(provider).success(), "setup: provider " + providerId);
        }
        Model model = modelOf(f.catalog);
        model.id = modelId;
        model.providerId = providerId;
        model.displayName = "Model " + modelId;
        model.enabled = modelEnabled;
        model.inputPriceMicrosPerMillionTokens = input;
        model.outputPriceMicrosPerMillionTokens = output;
        model.maxOutputTokens = maxOutput;
        check(f.models.insert(model).success(), "setup: model " + modelId);
    };
    add("p-on", true, "m-good", true, 3'000'000, 15'000'000, 64'000);
    add("p-on", true, "m-off", false, 3'000'000, 15'000'000, 64'000);
    add("p-on", true, "m-free", true, 0, 15'000'000, 64'000);
    add("p-on", true, "m-noout", true, 3'000'000, 0, 64'000);
    add("p-on", true, "m-small", true, 3'000'000, 15'000'000, 2'048);
    add("p-off", false, "m-orphan-provider", true, 3'000'000, 15'000'000, 64'000);

    const auto choices = f.readModel.modelChoices();
    check(choices.success(), "the choices read: " + codeOf(choices.error()));
    std::map<std::string, FollowUpModelChoice> byId;
    for (const auto& choice : choices.value()) byId[choice.modelId] = choice;
    check(byId.size() == 6, "every model is listed, usable or not");
    check(byId["m-good"].usable && byId["m-good"].reasonCode.empty() && byId["m-good"].providerName == "Name p-on" && byId["m-good"].displayName == "Model m-good",
        "a priced, enabled model of an enabled provider is usable");
    check(!byId["m-off"].usable && byId["m-off"].reasonCode == "model_disabled", "a disabled model says model_disabled");
    check(!byId["m-free"].usable && byId["m-free"].reasonCode == "price_unknown", "a zero input price is price_unknown, never 'free'");
    check(!byId["m-noout"].usable && byId["m-noout"].reasonCode == "price_unknown", "a zero output price is price_unknown");
    check(!byId["m-small"].usable && byId["m-small"].reasonCode == "output_limit_too_small", "an output limit under the minimum answer is output_limit_too_small");
    check(!byId["m-orphan-provider"].usable && byId["m-orphan-provider"].reasonCode == "provider_disabled", "a disabled provider is named first");
}

void testRosterTemplatesAndSourceRunsAreTheProjectsOwn()
{
    Fixture f;
    const auto draft = [&](const std::string& id, const std::string& projectId, RunLaunchMode mode, bool withRoster) {
        RunDraft value;
        value.id = id;
        value.name = id;
        value.projectId = projectId;
        value.title = id;
        value.task = "task";
        value.workflowId = kExecutionManifestWorkflowId;
        value.launchMode = mode;
        if (withRoster) {
            StageModelAssignment assignment;
            assignment.providerId = f.catalog.providerId;
            assignment.modelId = f.catalog.modelId;
            assignment.agentName = "worker";
            assignment.role = "implementation";
            assignment.position = 0;
            value.roster = {assignment};
        }
        value.maxParallelism = 2;
        value.createdAt = at(1);
        value.updatedAt = at(1);
        check(f.drafts.insert(value).success(), "setup: draft " + id);
    };
    draft("good", kProject, RunLaunchMode::CustomRoster, true);
    draft("other-project", "project-2", RunLaunchMode::CustomRoster, true);
    draft("mock", kProject, RunLaunchMode::Mock, true);
    draft("no-roster", kProject, RunLaunchMode::CustomRoster, false);
    const auto templates = f.readModel.rosterTemplates(kProject);
    check(templates.success() && templates.value().size() == 1 && templates.value()[0].id == "good",
        "only a same-project, custom-roster, non-empty-roster Draft lends its roster: " + codeOf(templates.error()));

    for (int index = 0; index < 60; ++index) {
        f.runs.choices.push_back(RunSourceChoice {"run-" + std::to_string(1000 + index), "title", RunStatus::Completed, at(index)});
    }
    const auto sources = f.readModel.sourceRunChoices(kProject);
    check(sources.success() && sources.value().size() == FollowUpPlanReadModel::kMaxSourceRunChoices, "the choices are bounded to the newest 50");
    check(sources.value().front().id == "run-1059" && sources.value().back().id == "run-1010", "...newest first");

    const auto withOldSource = f.readModel.sourceRunChoices(kProject, "run-1000");
    check(withOldSource.success() && withOldSource.value().size() == FollowUpPlanReadModel::kMaxSourceRunChoices,
        "the locked source does not grow the bounded picker");
    check(withOldSource.value().front().id == "run-1059" && withOldSource.value()[48].id == "run-1011"
            && withOldSource.value().back().id == "run-1000",
        "an old immutable source Run replaces the last ordinary choice and stays visible");
    const auto missingSource = f.readModel.sourceRunChoices(kProject, "no-such-run");
    check(!missingSource.success() && missingSource.error()->code == "persistence.invalid_data",
        "a required Run absent from its project's list fails closed");
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"testTheGateDecidesWhatEachRunIsOfferedAndAllowed", testTheGateDecidesWhatEachRunIsOfferedAndAllowed},
        {"testARunWithoutABriefAndAnEmptyIdShowNothing", testARunWithoutABriefAndAnEmptyIdShowNothing},
        {"testAnOpenRequestHoldsTheSlotAndTheLatestOneIsTheStatusLine", testAnOpenRequestHoldsTheSlotAndTheLatestOneIsTheStatusLine},
        {"testAGuestReadsNothing", testAGuestReadsNothing},
        {"testTheModelPickerShowsWhyAModelIsNotOffered", testTheModelPickerShowsWhyAModelIsNotOffered},
        {"testRosterTemplatesAndSourceRunsAreTheProjectsOwn", testRosterTemplatesAndSourceRunsAreTheProjectsOwn},
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
    if (failures == 0) std::cout << "FollowUpPlanReadModel tests passed\n";
    return failures == 0 ? 0 : 1;
}
