#include "ai/application/planning/FollowUpPlanReadModel.hpp"

#include "ai/application/planning/FollowUpComposerQuote.hpp"
#include "ai/application/execution/IWorkflowRunner.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>

namespace ai::application {
namespace {

template <typename T>
common::Result<T> forward(const common::Error& error)
{
    return common::Result<T>::failure(error);
}

FollowUpKindEligibility judge(
    bool offered, FollowUpPlanKind kind, const FollowUpSourceFacts& facts, const std::optional<FollowUpPlanRequest>& open)
{
    FollowUpKindEligibility result;
    result.offered = offered;
    if (!offered) return result;
    const auto gate = checkFollowUpSourceEligibility(kind, facts);
    if (!gate.success()) {
        result.reasonCode = gate.error()->code;
        result.reasonMessage = gate.error()->message;
        return result;
    }
    if (open.has_value()) {
        result.reasonCode = "follow_up.request_open";
        result.reasonMessage = std::string("The source Run already has a '") + toString(open->state) + "' request";
        return result;
    }
    result.allowed = true;
    return result;
}

} // namespace

FollowUpPlanReadModel::FollowUpPlanReadModel(
    const IOutcomeBriefRepository& briefs,
    const IFollowUpPlanRequestRepository& requests,
    const IProviderRepository& providers,
    const IModelRepository& models,
    const IRunDraftRepository& drafts,
    const IRunSourceLookup& runs,
    std::optional<common::Error> refusal)
    : briefs_(briefs)
    , requests_(requests)
    , providers_(providers)
    , models_(models)
    , drafts_(drafts)
    , runs_(runs)
    , refusal_(std::move(refusal))
{
}

common::Result<void> FollowUpPlanReadModel::checkOwner() const
{
    if (refusal_.has_value()) return common::Result<void>::failure(*refusal_);
    return common::Result<void>::ok();
}

common::Result<FollowUpRunEligibility> FollowUpPlanReadModel::eligibility(const RunId& runId) const
{
    using Result = common::Result<FollowUpRunEligibility>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());

    FollowUpRunEligibility view;
    view.runId = runId;
    if (runId.empty()) return Result::ok(std::move(view));

    const auto head = briefs_.findHead(runId);
    if (!head.success()) return forward<FollowUpRunEligibility>(*head.error());
    if (!head.value().has_value()) return Result::ok(std::move(view));
    const OutcomeBrief& brief = head.value()->brief;

    const auto open = requests_.findOpenForSourceRun(runId);
    if (!open.success()) return forward<FollowUpRunEligibility>(*open.error());
    view.openRequest = open.value();

    // The newest request of this Run in any state (one row, whatever the Run's history). A discarded one is dismissed.
    const auto latest = requests_.findLatestForSourceRun(runId);
    if (!latest.success()) return forward<FollowUpRunEligibility>(*latest.error());
    if (latest.value().has_value() && latest.value()->state != FollowUpPlanRequestState::Discarded) view.latestRequest = latest.value();

    FollowUpSourceFacts facts;
    facts.workflow = followUpSourceWorkflowFromId(brief.workflowId);
    facts.terminalStatus = brief.terminalStatus;
    facts.briefState = brief.state;
    facts.hasContinuationChild = brief.continuationChildRunId.has_value();
    facts.hasManualHandoff = brief.manualHandoffId.has_value();

    // A Failed Run is offered the recovery plan (its own wording); every other Run, and a Failed anchor whose manual handoff was fully
    // applied, is offered the next-Run plan. Whether the gate lets it through is the 5A-1 whitelist's, not this function's.
    const bool failed = brief.terminalStatus == RunStatus::Failed;
    const bool nextRunOffered = !failed || brief.state == OutcomeBriefState::ResolvedManuallyApplied;

    view.availability = FollowUpRunEligibility::Availability::Evaluated;
    view.projectId = brief.projectId;
    view.nextRun = judge(nextRunOffered, FollowUpPlanKind::NextRun, facts, view.openRequest);
    view.recovery = judge(failed, FollowUpPlanKind::Recovery, facts, view.openRequest);
    return Result::ok(std::move(view));
}

common::Result<std::vector<FollowUpModelChoice>> FollowUpPlanReadModel::modelChoices() const
{
    using Result = common::Result<std::vector<FollowUpModelChoice>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    const auto providers = providers_.listAll();
    if (!providers.success()) return Result::failure(*providers.error());
    const auto models = models_.listAll();
    if (!models.success()) return Result::failure(*models.error());

    std::vector<FollowUpModelChoice> choices;
    for (const auto& provider : providers.value()) {
        for (const auto& model : models.value()) {
            if (model.providerId != provider.id) continue;
            FollowUpModelChoice choice;
            choice.providerId = provider.id;
            choice.providerName = provider.name;
            choice.providerKind = provider.kind;
            choice.modelId = model.id;
            choice.displayName = model.displayName;
            choice.contextWindowTokens = model.contextWindowTokens;
            choice.maxOutputTokens = model.maxOutputTokens;
            choice.inputPriceMicrosPerMillionTokens = model.inputPriceMicrosPerMillionTokens;
            choice.outputPriceMicrosPerMillionTokens = model.outputPriceMicrosPerMillionTokens;
            choice.hasCredentialReference = provider.credentialReference.has_value() && !provider.credentialReference->empty();
            // The ORDER of the reasons is the order the admission reports them: provider, model, then what the quote needs.
            if (!provider.enabled) choice.reasonCode = "provider_disabled";
            else if (!model.enabled) choice.reasonCode = "model_disabled";
            else if (model.inputPriceMicrosPerMillionTokens == 0 || model.outputPriceMicrosPerMillionTokens == 0) choice.reasonCode = "price_unknown";
            else if (model.maxOutputTokens < kFollowUpMinimumOutputTokens) choice.reasonCode = "output_limit_too_small";
            choice.usable = choice.reasonCode.empty();
            choices.push_back(std::move(choice));
        }
    }
    return Result::ok(std::move(choices));
}

common::Result<std::vector<RunDraft>> FollowUpPlanReadModel::rosterTemplates(const ProjectId& projectId) const
{
    using Result = common::Result<std::vector<RunDraft>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    auto all = drafts_.listAll();
    if (!all.success()) return Result::failure(*all.error());
    std::vector<RunDraft> eligible;
    for (auto& draft : all.value()) {
        if (draft.projectId == projectId && draft.launchMode == RunLaunchMode::CustomRoster
                && draft.workflowId == kExecutionManifestWorkflowId && !draft.roster.empty()) {
            eligible.push_back(std::move(draft));
        }
    }
    return Result::ok(std::move(eligible));
}

common::Result<std::vector<RunSourceChoice>> FollowUpPlanReadModel::sourceRunChoices(
    const ProjectId& projectId, const RunId& requiredRunId) const
{
    using Result = common::Result<std::vector<RunSourceChoice>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    auto listed = runs_.listProjectRuns(projectId);
    if (!listed.success()) return Result::failure(*listed.error());
    std::vector<RunSourceChoice> choices = std::move(listed.value());
    std::stable_sort(choices.begin(), choices.end(), [](const RunSourceChoice& left, const RunSourceChoice& right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.id < right.id;
    });
    if (requiredRunId.empty()) {
        if (choices.size() > kMaxSourceRunChoices) choices.resize(kMaxSourceRunChoices);
        return Result::ok(std::move(choices));
    }

    const auto required = std::find_if(choices.begin(), choices.end(), [&requiredRunId](const RunSourceChoice& choice) {
        return choice.id == requiredRunId;
    });
    if (required == choices.end()) {
        return Result::failure({"persistence.invalid_data", "The follow-up source Run is absent from its project's Run list"});
    }
    if (static_cast<std::size_t>(std::distance(choices.begin(), required)) >= kMaxSourceRunChoices) {
        RunSourceChoice locked = *required;
        choices.resize(kMaxSourceRunChoices - 1);
        choices.push_back(std::move(locked));
    } else if (choices.size() > kMaxSourceRunChoices) {
        choices.resize(kMaxSourceRunChoices);
    }
    return Result::ok(std::move(choices));
}

} // namespace ai::application
