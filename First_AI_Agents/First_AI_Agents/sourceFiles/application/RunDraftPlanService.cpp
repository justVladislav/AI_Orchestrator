#include "ai/application/RunDraftPlanService.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/RunDraftGroup.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace ai::application {

namespace {

constexpr std::string_view kIdempotencyDomain = "next-run-plan-group.v1";

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

template <typename T, typename U>
common::Result<T> forward(const common::Result<U>& failed)
{
    return common::Result<T>::failure(*failed.error());
}

struct ConversationContext {
    ChatConversation conversation;
    Project project;
};

// The project of a plan always comes from the conversation: it must be bound to one and the project must be Active.
common::Result<ConversationContext> loadContext(
    const IChatRepository& chats, const IProjectRepository& projects, const ConversationId& conversationId)
{
    const auto conversation = chats.findConversation(conversationId);
    if (!conversation.success()) return forward<ConversationContext>(conversation);
    if (!conversation.value().has_value()) {
        return fail<ConversationContext>("run_draft_plan.conversation_not_found", "The conversation was not found");
    }
    if (!conversation.value()->projectId.has_value()) {
        return fail<ConversationContext>("run_draft_plan.conversation_without_project",
            "Next-run drafts belong to a project: select a project for this conversation first");
    }
    const auto project = projects.findById(*conversation.value()->projectId);
    if (!project.success()) return forward<ConversationContext>(project);
    if (!project.value().has_value()) {
        return fail<ConversationContext>("run_draft_plan.project_not_found", "The conversation's project was not found");
    }
    if (project.value()->status != ProjectStatus::Active) {
        return fail<ConversationContext>("run_draft_plan.project_inactive", "The conversation's project is not active");
    }
    return common::Result<ConversationContext>::ok({*conversation.value(), *project.value()});
}

// A completed Assistant message of THIS conversation.
common::Result<ChatMessage> loadEligibleMessage(
    const IChatRepository& chats, const ConversationId& conversationId, const ChatMessageId& messageId)
{
    const auto found = chats.findMessage(messageId);
    if (!found.success()) return forward<ChatMessage>(found);
    // A message of another conversation is reported like a missing one: it is not this conversation's to read.
    if (!found.value().has_value() || found.value()->conversationId != conversationId) {
        return fail<ChatMessage>("run_draft_plan.message_not_found", "The message was not found in this conversation");
    }
    const ChatMessage& message = *found.value();
    if (message.role != ChatRole::Assistant || message.status != ChatMessageStatus::Sent) {
        return fail<ChatMessage>("run_draft_plan.message_not_eligible",
            "Only a completed Assistant message can be turned into drafts");
    }
    return common::Result<ChatMessage>::ok(message);
}

struct LoadedMessagePlan {
    std::string contentFingerprint;   // sha256 of the message text: an edited or regenerated message differs
    std::optional<ProjectId> contextProjectId;
    std::optional<std::string> contextFingerprint;
    NextRunPlan plan;
};

common::Result<LoadedMessagePlan> loadMessagePlan(
    const IChatRepository& chats, const ConversationId& conversationId, const ChatMessageId& messageId)
{
    const auto message = loadEligibleMessage(chats, conversationId, messageId);
    if (!message.success()) return forward<LoadedMessagePlan>(message);
    const auto json = extractNextRunPlanJsonFromMessage(message.value().content);
    if (!json.success()) return forward<LoadedMessagePlan>(json);
    auto plan = parseNextRunPlan(json.value());
    if (!plan.success()) return forward<LoadedMessagePlan>(plan);
    LoadedMessagePlan loaded;
    loaded.contentFingerprint = sha256Hex(message.value().content);
    if (message.value().projectContext.has_value()) {
        loaded.contextProjectId = message.value().projectContext->projectId;
        loaded.contextFingerprint = message.value().projectContext->snapshotFingerprint;
    }
    loaded.plan = std::move(plan.value());
    return common::Result<LoadedMessagePlan>::ok(std::move(loaded));
}

std::string messageKey(const ProjectId& projectId, const ChatMessageId& messageId, const std::string& contentFingerprint,
    const std::string& planFingerprint)
{
    return sha256Hex(std::string(kIdempotencyDomain) + "\nchat_message\n" + projectId + "\n" + messageId + "\n"
        + contentFingerprint + "\n" + planFingerprint);
}

std::string pasteKey(const ProjectId& projectId, const std::string& previewToken)
{
    return sha256Hex(std::string(kIdempotencyDomain) + "\nmanual_import\n" + projectId + "\n" + previewToken);
}

// An explicitly chosen draft of the SAME project whose roster can be copied.
common::Result<std::optional<RunDraft>> loadTemplate(
    const IRunDraftRepository& drafts, const ProjectId& projectId, const std::optional<RunDraftId>& templateId)
{
    using Result = common::Result<std::optional<RunDraft>>;
    if (!templateId.has_value()) return Result::ok(std::nullopt);
    const auto found = drafts.findById(*templateId);
    if (!found.success()) return Result::failure(*found.error());
    if (!found.value().has_value()) {
        return fail<std::optional<RunDraft>>("run_draft_plan.template_not_found", "The roster template draft was not found");
    }
    const RunDraft& candidate = *found.value();
    if (candidate.projectId != projectId) {
        return fail<std::optional<RunDraft>>("run_draft_plan.template_wrong_project",
            "The roster template belongs to another project");
    }
    if (candidate.launchMode != RunLaunchMode::CustomRoster || candidate.workflowId != kExecutionManifestWorkflowId) {
        return fail<std::optional<RunDraft>>("run_draft_plan.template_not_manifest_roster",
            "The roster template must be a per-Run roster draft on the Manifest workflow");
    }
    if (candidate.roster.empty()) {
        return fail<std::optional<RunDraft>>("run_draft_plan.template_without_roster", "The roster template has no roster");
    }
    return Result::ok(candidate);
}

// The Runs the user named as the source of the plan: at most kRunDraftGroupMaxSourceRuns, each once, each of the
// conversation's project. The count is checked before any lookup.
common::Result<void> validateSourceRuns(
    const IRunSourceLookup& runs, const ProjectId& projectId, const std::vector<RunId>& sourceRunIds)
{
    if (sourceRunIds.size() > kRunDraftGroupMaxSourceRuns) {
        return fail<void>("run_draft_plan.too_many_source_runs",
            "At most " + std::to_string(kRunDraftGroupMaxSourceRuns) + " source Runs can be named");
    }
    std::set<RunId> seen;
    for (const RunId& runId : sourceRunIds) {
        if (!seen.insert(runId).second) {
            return fail<void>("run_draft_plan.duplicate_source_run", "A source Run is named twice: " + runId);
        }
        const auto project = runs.findRunProject(runId);
        if (!project.success()) return forward<void>(project);
        if (!project.value().has_value()) {
            return fail<void>("run_draft_plan.source_run_not_found", "The source Run was not found: " + runId);
        }
        if (*project.value() != projectId) {
            return fail<void>("run_draft_plan.source_run_wrong_project",
                "The source Run belongs to another project: " + runId);
        }
    }
    return common::Result<void>::ok();
}

// The group a source already produced, if any, with its drafts.
common::Result<std::optional<RunDraftPlanCreation>> findExistingCreation(
    const IRunDraftRepository& drafts, const std::string& idempotencyKey)
{
    using Result = common::Result<std::optional<RunDraftPlanCreation>>;
    auto group = drafts.findGroupByIdempotencyKey(idempotencyKey);
    if (!group.success()) return Result::failure(*group.error());
    if (!group.value().has_value()) return Result::ok(std::nullopt);
    auto members = drafts.listGroupDrafts(group.value()->id);
    if (!members.success()) return Result::failure(*members.error());
    return Result::ok(RunDraftPlanCreation {std::move(*group.value()), std::move(members.value()), true});
}

struct Source {
    RunDraftGroupOrigin origin {RunDraftGroupOrigin::ManualImport};
    std::optional<std::string> conversationId;
    std::optional<std::string> messageId;
    std::optional<std::string> messageFingerprint;
    std::optional<std::string> contextFingerprint;
};

// Writes the NEW group of `plan` (the caller has already established that the source has none). Two confirms racing
// for the same source: the loser's insert hits the unique key and it returns the winner's group instead of failing.
common::Result<RunDraftPlanCreation> insertNewGroup(IRunDraftRepository& repository, const Project& project,
    const Source& source, const std::string& idempotencyKey, const NextRunPlan& plan,
    const std::optional<RunDraft>& rosterTemplate, const std::vector<RunId>& sourceRunIds,
    std::chrono::system_clock::time_point timestamp)
{
    RunDraftGroup group;
    group.id = generateUuid();
    group.projectId = project.id;
    group.origin = source.origin;
    group.idempotencyKey = idempotencyKey;
    group.planTitle = plan.title;
    group.planFingerprint = plan.fingerprint;
    group.sourceConversationId = source.conversationId;
    group.sourceMessageId = source.messageId;
    group.sourceMessageFingerprint = source.messageFingerprint;
    // Present only when the source Assistant turn actually persisted the exact PC-1 snapshot sent to the provider.
    group.contextFingerprint = source.contextFingerprint;
    group.sourceRunIds = sourceRunIds;
    group.createdAt = timestamp;

    std::vector<RunDraft> drafts;
    drafts.reserve(plan.drafts.size());
    for (std::size_t ordinal = 0; ordinal < plan.drafts.size(); ++ordinal) {
        const NextRunPlanDraft& planned = plan.drafts[ordinal];
        RunDraft draft;
        draft.id = generateUuid();
        draft.name = planned.title;   // the draft's name is its title, deterministically
        draft.projectId = project.id;
        draft.title = planned.title;
        draft.task = planned.task;
        // Contracts exist only on a per-Run roster on the Manifest workflow, so a plan draft is always one - also
        // with an empty roster, which the user fills in when New Run opens it.
        draft.workflowId = kExecutionManifestWorkflowId;
        draft.launchMode = RunLaunchMode::CustomRoster;
        if (rosterTemplate.has_value()) {
            // Only these four things are copied: the roster and the cost-bearing choices made with it.
            draft.roster = rosterTemplate->roster;
            draft.maxParallelism = rosterTemplate->maxParallelism;
            draft.providerBudget = rosterTemplate->providerBudget;
            draft.verificationRepairPolicy = rosterTemplate->verificationRepairPolicy;
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
        draft.createdAt = timestamp;
        draft.updatedAt = timestamp;
        drafts.push_back(std::move(draft));
    }

    auto inserted = repository.insertGroup(group, std::move(drafts));
    if (!inserted.success()) {
        if (inserted.error()->code == "run_draft_group.duplicate_idempotency_key") {
            auto winner = findExistingCreation(repository, idempotencyKey);
            if (winner.success() && winner.value().has_value()) {
                return common::Result<RunDraftPlanCreation>::ok(std::move(*winner.value()));
            }
        }
        return forward<RunDraftPlanCreation>(inserted);
    }
    return common::Result<RunDraftPlanCreation>::ok({std::move(group), std::move(inserted.value()), false});
}

} // namespace

NextRunPlanEnvelopeState planEnvelopeStateOf(const ChatMessage& message)
{
    if (message.role != ChatRole::Assistant || message.status != ChatMessageStatus::Sent) {
        return NextRunPlanEnvelopeState::None;
    }
    // The tag has to appear in the text for any envelope to exist: skip the line scan of the ordinary reply.
    if (message.content.find(kNextRunPlanSchemaVersion) == std::string::npos) return NextRunPlanEnvelopeState::None;
    return scanNextRunPlanEnvelope(message.content);
}

RunDraftPlanService::RunDraftPlanService(IRunDraftRepository& drafts, IProjectRepository& projects,
    IChatRepository& chats, const IRunSourceLookup& runs, Clock clock)
    : drafts_(drafts)
    , projects_(projects)
    , chats_(chats)
    , runs_(runs)
    , clock_(std::move(clock))
{
}

std::chrono::system_clock::time_point RunDraftPlanService::now() const
{
    return clock_ ? clock_() : std::chrono::system_clock::now();
}

void RunDraftPlanService::prunePreviewsLocked()
{
    const auto current = now();
    for (auto it = previews_.begin(); it != previews_.end();) {
        it = current - it->second.createdAt >= kPreviewLifetime ? previews_.erase(it) : std::next(it);
    }
    while (previews_.size() >= kMaxPreviews) {
        const auto oldest = std::min_element(previews_.begin(), previews_.end(),
            [](const auto& left, const auto& right) { return left.second.createdAt < right.second.createdAt; });
        previews_.erase(oldest);
    }
}

std::string RunDraftPlanService::rememberPreview(StoredPreview stored)
{
    std::string token = generateUuid();
    stored.createdAt = now();
    std::lock_guard lock(mutex_);
    prunePreviewsLocked();
    previews_[token] = std::move(stored);
    return token;
}

common::Result<RunDraftPlanPreview> RunDraftPlanService::previewFromMessage(
    const ConversationId& conversationId, const ChatMessageId& messageId)
{
    const auto context = loadContext(chats_, projects_, conversationId);
    if (!context.success()) return forward<RunDraftPlanPreview>(context);
    auto loaded = loadMessagePlan(chats_, conversationId, messageId);
    if (!loaded.success()) return forward<RunDraftPlanPreview>(loaded);
    if (loaded.value().contextProjectId.has_value()
        && *loaded.value().contextProjectId != context.value().project.id) {
        return fail<RunDraftPlanPreview>("run_draft_plan.context_project_mismatch",
            "The response was generated with context from another project and cannot author drafts here");
    }

    RunDraftPlanPreview preview;
    preview.projectId = context.value().project.id;
    preview.conversationId = conversationId;
    preview.origin = RunDraftGroupOrigin::ChatMessage;
    preview.messageId = messageId;
    preview.plan = std::move(loaded.value().plan);
    const auto existing = drafts_.findGroupByIdempotencyKey(
        messageKey(preview.projectId, messageId, loaded.value().contentFingerprint, preview.plan.fingerprint));
    if (!existing.success()) return forward<RunDraftPlanPreview>(existing);
    if (existing.value().has_value()) preview.existingGroupId = existing.value()->id;

    StoredPreview stored;
    stored.projectId = preview.projectId;
    stored.conversationId = conversationId;
    stored.origin = RunDraftGroupOrigin::ChatMessage;
    stored.messageId = messageId;
    stored.messageContentFingerprint = loaded.value().contentFingerprint;
    stored.contextFingerprint = loaded.value().contextFingerprint;
    stored.plan = preview.plan;
    preview.previewToken = rememberPreview(std::move(stored));
    return common::Result<RunDraftPlanPreview>::ok(std::move(preview));
}

common::Result<RunDraftPlanPreview> RunDraftPlanService::previewFromPaste(
    const ConversationId& conversationId, std::string_view pastedText)
{
    const auto context = loadContext(chats_, projects_, conversationId);
    if (!context.success()) return forward<RunDraftPlanPreview>(context);
    const auto json = extractNextRunPlanJsonFromPaste(pastedText);
    if (!json.success()) return forward<RunDraftPlanPreview>(json);
    auto plan = parseNextRunPlan(json.value());
    if (!plan.success()) return forward<RunDraftPlanPreview>(plan);

    RunDraftPlanPreview preview;
    preview.projectId = context.value().project.id;
    preview.conversationId = conversationId;
    preview.origin = RunDraftGroupOrigin::ManualImport;
    preview.plan = std::move(plan.value());

    StoredPreview stored;
    stored.projectId = preview.projectId;
    stored.conversationId = conversationId;
    stored.origin = RunDraftGroupOrigin::ManualImport;
    stored.plan = preview.plan;
    preview.previewToken = rememberPreview(std::move(stored));
    return common::Result<RunDraftPlanPreview>::ok(std::move(preview));
}

common::Result<RunDraftPlanCreation> RunDraftPlanService::createFromPreview(const std::string& previewToken,
    const std::optional<RunDraftId>& rosterTemplate, const std::vector<RunId>& sourceRunIds)
{
    StoredPreview stored;
    {
        std::lock_guard lock(mutex_);
        const auto it = previews_.find(previewToken);
        if (it == previews_.end()) {
            return fail<RunDraftPlanCreation>("run_draft_plan.preview_not_found",
                "The preview is unknown (or was dropped): preview the plan again");
        }
        if (now() - it->second.createdAt >= kPreviewLifetime) {
            previews_.erase(it);
            return fail<RunDraftPlanCreation>("run_draft_plan.preview_expired",
                "The preview has expired: preview the plan again");
        }
        stored = it->second;
    }

    // The source is checked first: still the conversation's project (and active), and - for a message - still the
    // very text the user was shown.
    const auto context = loadContext(chats_, projects_, stored.conversationId);
    if (!context.success()) return forward<RunDraftPlanCreation>(context);
    if (context.value().project.id != stored.projectId) {
        return fail<RunDraftPlanCreation>("run_draft_plan.project_changed",
            "The conversation moved to another project since the preview: preview the plan again");
    }
    Source source;
    source.origin = stored.origin;
    std::string idempotencyKey;
    if (stored.origin == RunDraftGroupOrigin::ChatMessage) {
        const auto message = loadEligibleMessage(chats_, stored.conversationId, *stored.messageId);
        if (!message.success()) return forward<RunDraftPlanCreation>(message);
        if (sha256Hex(message.value().content) != *stored.messageContentFingerprint) {
            return fail<RunDraftPlanCreation>("run_draft_plan.stale_preview",
                "The message changed since the preview: preview the plan again");
        }
        const auto currentContextFingerprint = message.value().projectContext.has_value()
            ? std::optional<std::string> {message.value().projectContext->snapshotFingerprint}
            : std::nullopt;
        if (currentContextFingerprint != stored.contextFingerprint) {
            return fail<RunDraftPlanCreation>("run_draft_plan.stale_preview",
                "The message's attached Project Context changed since the preview: preview the plan again");
        }
        source.conversationId = stored.conversationId;
        source.messageId = stored.messageId;
        source.messageFingerprint = stored.messageContentFingerprint;
        source.contextFingerprint = stored.contextFingerprint;
        idempotencyKey = messageKey(
            stored.projectId, *stored.messageId, *stored.messageContentFingerprint, stored.plan.fingerprint);
    } else {
        idempotencyKey = pasteKey(stored.projectId, previewToken);
    }

    // A repeat is answered from the stored group; the template and the source Runs are not looked at.
    auto existing = findExistingCreation(drafts_, idempotencyKey);
    if (!existing.success()) return forward<RunDraftPlanCreation>(existing);
    if (existing.value().has_value()) return common::Result<RunDraftPlanCreation>::ok(std::move(*existing.value()));

    const auto roster = loadTemplate(drafts_, stored.projectId, rosterTemplate);
    if (!roster.success()) return forward<RunDraftPlanCreation>(roster);
    const auto runsOk = validateSourceRuns(runs_, stored.projectId, sourceRunIds);
    if (!runsOk.success()) return forward<RunDraftPlanCreation>(runsOk);

    return insertNewGroup(drafts_, context.value().project, source, idempotencyKey, stored.plan, roster.value(),
        sourceRunIds, now());
}

common::Result<std::vector<RunDraft>> RunDraftPlanService::listRosterTemplates(const ConversationId& conversationId) const
{
    const auto context = loadContext(chats_, projects_, conversationId);
    if (!context.success()) return forward<std::vector<RunDraft>>(context);
    auto all = drafts_.listAll();
    if (!all.success()) return forward<std::vector<RunDraft>>(all);
    std::vector<RunDraft> eligible;
    for (auto& draft : all.value()) {
        if (draft.projectId == context.value().project.id && draft.launchMode == RunLaunchMode::CustomRoster
                && draft.workflowId == kExecutionManifestWorkflowId && !draft.roster.empty()) {
            eligible.push_back(std::move(draft));
        }
    }
    return common::Result<std::vector<RunDraft>>::ok(std::move(eligible));
}

common::Result<std::vector<RunSourceChoice>> RunDraftPlanService::listSourceRunChoices(
    const ConversationId& conversationId) const
{
    const auto context = loadContext(chats_, projects_, conversationId);
    if (!context.success()) return forward<std::vector<RunSourceChoice>>(context);
    auto listed = runs_.listProjectRuns(context.value().project.id);
    if (!listed.success()) return forward<std::vector<RunSourceChoice>>(listed);
    std::vector<RunSourceChoice> choices = std::move(listed.value());
    std::stable_sort(choices.begin(), choices.end(), [](const RunSourceChoice& left, const RunSourceChoice& right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.id < right.id;
    });
    if (choices.size() > kMaxSourceRunChoices) choices.resize(kMaxSourceRunChoices);
    return common::Result<std::vector<RunSourceChoice>>::ok(std::move(choices));
}

} // namespace ai::application
