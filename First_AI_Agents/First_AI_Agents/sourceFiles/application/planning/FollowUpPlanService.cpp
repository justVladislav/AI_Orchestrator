#include "ai/application/planning/FollowUpPlanService.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace ai::application {
namespace {

constexpr const char* kGroupKeyDomain = "follow-up-request-group.v1";

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

std::vector<FollowUpFileView> fileViewsOf(const FollowUpComposerContext& context)
{
    std::vector<FollowUpFileView> views;
    views.reserve(context.files.size());
    for (const auto& file : context.files) {
        views.push_back({file.relativePath, file.safety, file.disposition, file.contentSha256, file.originalBytes,
            file.shownBytes, file.estimatedTokens, file.truncated});
    }
    return views;
}

// An explicitly chosen draft of the SAME project whose roster can be copied (the rules of the PC-3 Draft service).
common::Result<std::optional<RunDraft>> loadTemplate(
    const IRunDraftRepository& drafts, const ProjectId& projectId, const std::optional<RunDraftId>& templateId)
{
    using Result = common::Result<std::optional<RunDraft>>;
    if (!templateId.has_value()) return Result::ok(std::nullopt);
    const auto found = drafts.findById(*templateId);
    if (!found.success()) return Result::failure(*found.error());
    if (!found.value().has_value()) return fail<std::optional<RunDraft>>("follow_up.template_not_found", "The roster template draft was not found");
    const RunDraft& candidate = *found.value();
    if (candidate.projectId != projectId) {
        return fail<std::optional<RunDraft>>("follow_up.template_wrong_project", "The roster template belongs to another project");
    }
    if (candidate.launchMode != RunLaunchMode::CustomRoster || candidate.workflowId != kExecutionManifestWorkflowId) {
        return fail<std::optional<RunDraft>>("follow_up.template_not_manifest_roster",
            "The roster template must be a per-Run roster draft on the Manifest workflow");
    }
    if (candidate.roster.empty()) {
        return fail<std::optional<RunDraft>>("follow_up.template_without_roster", "The roster template has no roster");
    }
    return Result::ok(candidate);
}

struct Materialized {
    RunDraftGroup group;
    std::vector<RunDraft> drafts;
};

// The group and Drafts of a stored plan, the way RunDraftPlanService materialises any other plan: the Draft's name is its title,
// its contracts are the plan's final ones, it is a per-Run roster draft of the Manifest workflow, and only the roster template's
// roster and cost-bearing choices are copied. checkFollowUpDraftsAreThePlan holds the result against the plan again at the Confirm.
Materialized materialize(const FollowUpPlanRequest& request, const NextRunPlan& plan, const std::optional<RunDraft>& rosterTemplate,
    const std::vector<RunId>& sourceRunIds, bool applySuggestedParallelism, std::chrono::system_clock::time_point at)
{
    Materialized out;
    RunDraftGroup& group = out.group;
    group.id = generateUuid();
    group.projectId = request.projectId;
    group.origin = RunDraftGroupOrigin::FollowUpRequest;
    group.idempotencyKey = sha256Hex(std::string(kGroupKeyDomain) + "\n" + request.id);
    group.planTitle = plan.title;
    group.planFingerprint = plan.fingerprint;
    group.sourceRequestId = request.id;
    group.contextFingerprint = request.quote.contextFingerprint;
    group.sourceRunIds = sourceRunIds;
    group.createdAt = at;

    for (std::size_t ordinal = 0; ordinal < plan.drafts.size(); ++ordinal) {
        const NextRunPlanDraft& planned = plan.drafts[ordinal];
        RunDraft draft;
        draft.id = generateUuid();
        draft.name = planned.title;
        draft.projectId = request.projectId;
        draft.title = planned.title;
        draft.task = planned.task;
        draft.workflowId = kExecutionManifestWorkflowId;
        draft.launchMode = RunLaunchMode::CustomRoster;
        if (rosterTemplate.has_value()) {
            draft.roster = rosterTemplate->roster;
            draft.maxParallelism = rosterTemplate->maxParallelism;
            draft.providerBudget = rosterTemplate->providerBudget;
            draft.verificationRepairPolicy = rosterTemplate->verificationRepairPolicy;
        }
        // Only the suggested parallelism, only when asked for, and after the template so an explicit tick wins.
        if (applySuggestedParallelism && planned.staffing.has_value()) {
            draft.maxParallelism = static_cast<std::uint32_t>(planned.staffing->parallelism);
        }
        draft.interfaceContracts = planned.contracts;
        RunDraftProvenance provenance;
        provenance.groupId = group.id;
        provenance.ordinal = static_cast<std::uint32_t>(ordinal);
        provenance.planKey = planned.key;
        provenance.dependsOn = planned.dependsOn;
        provenance.createdContentFingerprint = nextRunPlanDraftContentFingerprint(
            draft.title, draft.task, draft.interfaceContracts, provenance.dependsOn);
        draft.provenance = std::move(provenance);
        draft.createdAt = at;
        draft.updatedAt = at;
        out.drafts.push_back(std::move(draft));
    }
    return out;
}

} // namespace

FollowUpPlanService::FollowUpPlanService(
    IFollowUpPlanRequestRepository& requests,
    IFollowUpPlanConfirmPort& confirmPort,
    const IFollowUpSourceReader& source,
    const IProviderRepository& providers,
    const IModelRepository& models,
    const IRunDraftRepository& drafts,
    Clock clock,
    std::optional<common::Error> refusal)
    : requests_(requests)
    , confirm_(confirmPort)
    , source_(source)
    , providers_(providers)
    , models_(models)
    , drafts_(drafts)
    , clock_(std::move(clock))
    , refusal_(std::move(refusal))
{
}

void FollowUpPlanService::setAuthorizedNudge(std::function<void(const FollowUpPlanRequestId&)> nudge)
{
    std::lock_guard lock(hooksMutex_);
    nudge_ = std::move(nudge);
}

void FollowUpPlanService::setComposingCancel(std::function<bool(const FollowUpPlanRequestId&)> cancel)
{
    std::lock_guard lock(hooksMutex_);
    cancelComposing_ = std::move(cancel);
}

std::chrono::system_clock::time_point FollowUpPlanService::now() const
{
    return clock_ ? clock_() : std::chrono::system_clock::now();
}

common::Result<void> FollowUpPlanService::checkOwner() const
{
    if (refusal_.has_value()) return common::Result<void>::failure(*refusal_);
    return common::Result<void>::ok();
}

FollowUpAdmissionPorts FollowUpPlanService::admissionPorts() const
{
    return FollowUpAdmissionPorts {source_, providers_, models_};
}

common::Result<FollowUpPlanRequest> FollowUpPlanService::load(const FollowUpPlanRequestId& id) const
{
    const auto found = requests_.findById(id);
    if (!found.success()) return common::Result<FollowUpPlanRequest>::failure(*found.error());
    if (!found.value().has_value()) return fail<FollowUpPlanRequest>("follow_up.not_found", "No such follow-up request");
    return common::Result<FollowUpPlanRequest>::ok(*found.value());
}

common::Result<FollowUpPlanRequest> FollowUpPlanService::step(
    const FollowUpPlanRequest& request, FollowUpPlanEvent event, const std::string& contextFingerprint)
{
    FollowUpTransition transition;
    transition.event = event;
    transition.expectedState = request.state;
    transition.contextFingerprint = contextFingerprint;
    return requests_.transition(request.id, std::move(transition), now());
}

common::Result<std::vector<FollowUpFileView>> FollowUpPlanService::describeFiles(
    const ProjectId& projectId, const RunId& sourceRunId, FollowUpPlanKind kind) const
{
    using Result = common::Result<std::vector<FollowUpFileView>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    // The files do not depend on the goal or the model, so a fixed valid goal and a one-draft window stand in for them; the Outcome
    // gate and the file rules are the real ones.
    auto material = source_.read(projectId, sourceRunId);
    if (!material.success()) return Result::failure(*material.error());
    if (material.value().project.status != ProjectStatus::Active) {
        return fail<std::vector<FollowUpFileView>>("follow_up.project_inactive", "The project is not Active");
    }
    if (material.value().runProjectId != projectId) {
        return fail<std::vector<FollowUpFileView>>("follow_up.run_project_mismatch", "The source Run belongs to another project");
    }
    // A list too long to show is refused whole, never shown cut short.
    if (material.value().candidateRefusal.has_value()) return Result::failure(*material.value().candidateRefusal);
    FollowUpComposerContextRequest request;
    request.sourceRunId = sourceRunId;
    request.kind = kind;
    request.splitHint = FollowUpSplitHint::Auto;
    request.maxDrafts = 1;
    request.goal = "Describe the candidate files.";
    request.snapshot = material.value().snapshot;
    request.brief = material.value().brief;
    request.files = std::move(material.value().candidates);   // moved: the bytes can be 64 MiB and are not copied
    for (auto& file : request.files) file.selected = file.safety == FollowUpFileSafety::Eligible;
    const auto context = assembleFollowUpComposerContext(std::move(request));
    if (!context.success()) return Result::failure(*context.error());
    return Result::ok(fileViewsOf(context.value()));
}

common::Result<FollowUpQuoteOutcome> FollowUpPlanService::quote(const FollowUpIntent& intent)
{
    using Result = common::Result<FollowUpQuoteOutcome>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());

    // A request that is further along than `quoted` holds the slot and is never displaced: say so BEFORE reading the sources (which
    // can hash a whole workspace). The insert below stays the authority - it decides in one transaction.
    const auto open = requests_.findOpenForSourceRun(intent.sourceRunId);
    if (!open.success()) return Result::failure(*open.error());
    if (open.value().has_value() && open.value()->state != FollowUpPlanRequestState::Quoted) {
        return fail<FollowUpQuoteOutcome>("follow_up.request_open",
            std::string("The source Run already has a '") + toString(open.value()->state) + "' request");
    }

    const auto admitted = admitFollowUpIntent(admissionPorts(), intent);
    if (!admitted.success()) return Result::failure(*admitted.error());
    const FollowUpAdmitted& value = admitted.value();

    FollowUpPlanRequest request;
    request.id = generateUuid();
    request.projectId = intent.projectId;
    request.sourceRunId = intent.sourceRunId;
    request.kind = intent.kind;
    request.goal = intent.goal;
    request.maxDrafts = intent.maxDrafts;
    request.splitHint = intent.splitHint;
    request.outcomeSourceStateFingerprint = value.material.brief.sourceStateFingerprint;
    request.snapshotFingerprint = value.material.snapshot.fingerprint;
    request.files = followUpPinnedFilesOf(value.context);
    request.quote = value.quote;
    request.state = FollowUpPlanRequestState::Quoted;
    request.createdAt = followUpFloorToMilliseconds(now());
    request.quoteExpiresAt = request.createdAt + kFollowUpQuoteLifetime;
    request.updatedAt = request.createdAt;

    const auto inserted = requests_.insertQuoted(std::move(request));
    if (!inserted.success()) return Result::failure(*inserted.error());
    FollowUpQuoteOutcome outcome;
    outcome.request = inserted.value().request;
    outcome.replacedQuoteId = inserted.value().discardedQuoteId;
    outcome.files = fileViewsOf(value.context);
    return Result::ok(std::move(outcome));
}

common::Result<FollowUpPlanRequest> FollowUpPlanService::authorize(const FollowUpPlanRequestId& id)
{
    using Result = common::Result<FollowUpPlanRequest>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    const auto loaded = load(id);
    if (!loaded.success()) return loaded;
    const FollowUpPlanRequest& request = loaded.value();
    if (request.state != FollowUpPlanRequestState::Quoted) {
        return fail<FollowUpPlanRequest>("follow_up.state_changed",
            std::string("The request is '") + toString(request.state) + "', not 'quoted'");
    }
    if (followUpFloorToMilliseconds(now()) >= request.quoteExpiresAt) {
        static_cast<void>(step(request, FollowUpPlanEvent::Expire));
        return fail<FollowUpPlanRequest>("follow_up.quote_expired", "The quote has expired");
    }

    const auto admitted = admitStoredFollowUpRequest(admissionPorts(), request);
    if (!admitted.success()) {
        const auto error = *admitted.error();
        if (isFollowUpStaleCode(error.code)) {
            // A drift before the call: the request is stale (free) and the user quotes again. If the mark itself fails the
            // request simply stays quoted and the refusal is still the answer.
            static_cast<void>(step(request, FollowUpPlanEvent::MarkStale));
        }
        return Result::failure(error);
    }

    auto authorized = step(request, FollowUpPlanEvent::Authorize, admitted.value().context.contextFingerprint);
    if (!authorized.success()) {
        if (authorized.error()->code == "follow_up.quote_expired") static_cast<void>(step(request, FollowUpPlanEvent::Expire));
        return authorized;
    }
    std::function<void(const FollowUpPlanRequestId&)> nudge;
    {
        std::lock_guard lock(hooksMutex_);
        nudge = nudge_;
    }
    if (nudge) nudge(id);
    return authorized;
}

common::Result<FollowUpPlanRequest> FollowUpPlanService::cancel(const FollowUpPlanRequestId& id)
{
    using Result = common::Result<FollowUpPlanRequest>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    const auto loaded = load(id);
    if (!loaded.success()) return loaded;
    const FollowUpPlanRequest& request = loaded.value();
    if (request.state == FollowUpPlanRequestState::Composing) {
        std::function<bool(const FollowUpPlanRequestId&)> cancelRunning;
        {
            std::lock_guard lock(hooksMutex_);
            cancelRunning = cancelComposing_;
        }
        // A call that is running in this process is asked to stop and settles the request itself when it returns.
        if (cancelRunning && cancelRunning(id)) return loaded;
    }
    return step(request, FollowUpPlanEvent::Cancel);
}

common::Result<FollowUpPlanRequest> FollowUpPlanService::discard(const FollowUpPlanRequestId& id)
{
    using Result = common::Result<FollowUpPlanRequest>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    const auto loaded = load(id);
    if (!loaded.success()) return loaded;
    return step(loaded.value(), FollowUpPlanEvent::Discard);
}

common::Result<std::optional<FollowUpPlanRequest>> FollowUpPlanService::find(const FollowUpPlanRequestId& id) const
{
    using Result = common::Result<std::optional<FollowUpPlanRequest>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    return requests_.findById(id);
}

common::Result<std::optional<FollowUpPlanRequest>> FollowUpPlanService::findOpenForSourceRun(const RunId& runId) const
{
    using Result = common::Result<std::optional<FollowUpPlanRequest>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    return requests_.findOpenForSourceRun(runId);
}

common::Result<std::vector<FollowUpPlanRequest>> FollowUpPlanService::listForProject(
    const ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<FollowUpPlanRequest>>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    return requests_.listForProject(projectId, limit);
}

common::Result<FollowUpPlanPreview> FollowUpPlanService::preview(const FollowUpPlanRequestId& id) const
{
    using Result = common::Result<FollowUpPlanPreview>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    const auto loaded = load(id);
    if (!loaded.success()) return Result::failure(*loaded.error());
    FollowUpPlanPreview view;
    view.request = loaded.value();
    const bool hasPlan = view.request.state == FollowUpPlanRequestState::PlanReady
        || view.request.state == FollowUpPlanRequestState::DraftsSaved
        || (view.request.state == FollowUpPlanRequestState::Discarded && view.request.answer.has_value());
    if (hasPlan && view.request.answer.has_value()) {
        // The plan is what the STORED reply parses to - the same function that admitted it, the one the Confirm uses again.
        auto plan = parseFollowUpAnswerPlan(*view.request.answer, view.request.splitHint, view.request.maxDrafts);
        if (!plan.success()) return Result::failure(*plan.error());
        view.plan = std::move(plan.value());
        auto advisories = parseFollowUpPlanAdvisories(view.request.answer->advisoriesJson);
        if (!advisories.success()) return Result::failure(*advisories.error());
        view.advisories = std::move(advisories.value());
    }
    return Result::ok(std::move(view));
}

common::Result<FollowUpConfirmResult> FollowUpPlanService::confirm(
    const FollowUpPlanRequestId& id, const std::string& expectedPlanFingerprint, const FollowUpConfirmOptions& options)
{
    using Result = common::Result<FollowUpConfirmResult>;
    if (const auto owner = checkOwner(); !owner.success()) return Result::failure(*owner.error());
    const auto loaded = load(id);
    if (!loaded.success()) return Result::failure(*loaded.error());
    const FollowUpPlanRequest& request = loaded.value();
    const auto stamp = followUpFloorToMilliseconds(now());

    // Anything but a ready plan is the port's decision: a repeat (drafts_saved) returns the SAME group, every other state is refused
    // by its own code. Nothing is materialised for those.
    if (request.state != FollowUpPlanRequestState::PlanReady || !request.answer.has_value()) {
        return confirm_.confirmDrafts(id, expectedPlanFingerprint, RunDraftGroup {}, {}, stamp);
    }

    auto plan = parseFollowUpAnswerPlan(*request.answer, request.splitHint, request.maxDrafts);
    if (!plan.success()) return Result::failure(*plan.error());

    if (options.extraSourceRunIds.size() + 1 > kRunDraftGroupMaxSourceRuns) {
        return fail<FollowUpConfirmResult>("follow_up.too_many_source_runs", "Too many source Runs were named");
    }
    std::vector<RunId> sourceRuns {request.sourceRunId};
    std::set<RunId> seen {request.sourceRunId};
    for (const auto& runId : options.extraSourceRunIds) {
        if (!seen.insert(runId).second) {
            return fail<FollowUpConfirmResult>("follow_up.duplicate_source_run", "A source Run is named twice: " + runId);
        }
        sourceRuns.push_back(runId);
    }
    const auto rosterTemplate = loadTemplate(drafts_, request.projectId, options.rosterTemplate);
    if (!rosterTemplate.success()) return Result::failure(*rosterTemplate.error());

    auto materialized = materialize(request, plan.value(), rosterTemplate.value(), sourceRuns, options.applySuggestedParallelism, stamp);
    return confirm_.confirmDrafts(id, expectedPlanFingerprint, std::move(materialized.group), std::move(materialized.drafts), stamp);
}

} // namespace ai::application
