// PC-3C: the Prompt Workshop's Backend service - preview, then confirm by token, for a chat message or a manual
// paste; idempotency, roster template, source Runs, project isolation. Provider-free: the service holds only the
// Draft, Project and Chat repositories and a read-only "project of this Run" lookup, so these tests need nothing
// else (fakes for the chat repository and the Run lookup, the in-memory Draft and Project repositories).

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/IRunSourceLookup.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/planning/RunDraftPlanService.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ai::application;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// The chat repository is the one port with no in-memory adapter; a map is all these tests need.
class FakeChatRepository final : public IChatRepository {
public:
    ai::common::Result<ChatConversation> insertConversation(ChatConversation value) override
    {
        conversations_[value.id] = value;
        return ai::common::Result<ChatConversation>::ok(std::move(value));
    }
    ai::common::Result<ChatConversation> updateConversation(ChatConversation value) override
    {
        conversations_[value.id] = value;
        return ai::common::Result<ChatConversation>::ok(std::move(value));
    }
    ai::common::Result<void> removeConversation(const ConversationId& id) override
    {
        conversations_.erase(id);
        return ai::common::Result<void>::ok();
    }
    [[nodiscard]] ai::common::Result<std::optional<ChatConversation>> findConversation(const ConversationId& id) const override
    {
        const auto it = conversations_.find(id);
        return ai::common::Result<std::optional<ChatConversation>>::ok(
            it == conversations_.end() ? std::nullopt : std::optional<ChatConversation>(it->second));
    }
    [[nodiscard]] ai::common::Result<std::vector<ChatConversation>> listConversations() const override
    {
        std::vector<ChatConversation> all;
        for (const auto& entry : conversations_) all.push_back(entry.second);
        return ai::common::Result<std::vector<ChatConversation>>::ok(std::move(all));
    }
    ai::common::Result<PreparedChatTurn> insertPreparedTurn(
        ChatConversation conversation, PreparedChatTurn turn) override
    {
        conversations_[conversation.id] = std::move(conversation);
        messages_[turn.userMessage.id] = turn.userMessage;
        messages_[turn.assistantMessage.id] = turn.assistantMessage;
        return ai::common::Result<PreparedChatTurn>::ok(std::move(turn));
    }
    ai::common::Result<ChatMessage> insertMessage(ChatMessage value) override
    {
        messages_[value.id] = value;
        return ai::common::Result<ChatMessage>::ok(std::move(value));
    }
    ai::common::Result<ChatMessage> updateMessage(ChatMessage value) override
    {
        messages_[value.id] = value;
        return ai::common::Result<ChatMessage>::ok(std::move(value));
    }
    [[nodiscard]] ai::common::Result<std::optional<ChatMessage>> findMessage(const ChatMessageId& id) const override
    {
        const auto it = messages_.find(id);
        return ai::common::Result<std::optional<ChatMessage>>::ok(
            it == messages_.end() ? std::nullopt : std::optional<ChatMessage>(it->second));
    }
    [[nodiscard]] ai::common::Result<std::vector<ChatMessage>> listMessages(const ConversationId& conversationId) const override
    {
        std::vector<ChatMessage> all;
        for (const auto& entry : messages_) {
            if (entry.second.conversationId == conversationId) all.push_back(entry.second);
        }
        return ai::common::Result<std::vector<ChatMessage>>::ok(std::move(all));
    }

private:
    std::map<ConversationId, ChatConversation> conversations_;
    std::map<ChatMessageId, ChatMessage> messages_;
};

// Wraps a repository and lets a test play the loser of a race: the first N lookups by idempotency key claim that no group
// exists yet (the winner had not written it when the loser looked), and an insert can be made to fail with any code.
class RacingDraftRepository final : public IRunDraftRepository {
public:
    explicit RacingDraftRepository(IRunDraftRepository& inner) : inner_(inner) {}

    mutable int hideKeyLookups {0};
    std::optional<std::string> failInsertGroupWith;

    ai::common::Result<RunDraft> insert(RunDraft draft) override { return inner_.insert(std::move(draft)); }
    ai::common::Result<RunDraft> update(RunDraft draft) override { return inner_.update(std::move(draft)); }
    [[nodiscard]] ai::common::Result<std::optional<RunDraft>> findById(const RunDraftId& id) const override
    {
        return inner_.findById(id);
    }
    [[nodiscard]] ai::common::Result<std::vector<RunDraft>> listAll() const override { return inner_.listAll(); }
    ai::common::Result<void> remove(const RunDraftId& id) override { return inner_.remove(id); }
    ai::common::Result<std::vector<RunDraft>> insertGroup(RunDraftGroup group, std::vector<RunDraft> drafts) override
    {
        if (failInsertGroupWith.has_value()) {
            return ai::common::Result<std::vector<RunDraft>>::failure({*failInsertGroupWith, "injected failure"});
        }
        return inner_.insertGroup(std::move(group), std::move(drafts));
    }
    [[nodiscard]] ai::common::Result<std::optional<RunDraftGroup>> findGroupById(const RunDraftGroupId& id) const override
    {
        return inner_.findGroupById(id);
    }
    [[nodiscard]] ai::common::Result<std::optional<RunDraftGroup>> findGroupByIdempotencyKey(
        const std::string& key) const override
    {
        if (hideKeyLookups > 0) {
            --hideKeyLookups;
            return ai::common::Result<std::optional<RunDraftGroup>>::ok(std::nullopt);
        }
        return inner_.findGroupByIdempotencyKey(key);
    }
    [[nodiscard]] ai::common::Result<std::vector<RunDraft>> listGroupDrafts(const RunDraftGroupId& id) const override
    {
        return inner_.listGroupDrafts(id);
    }

private:
    IRunDraftRepository& inner_;
};

// Two shared contracts (S1 for both drafts, S2 for the view only), a draft without optional arrays and a dependent
// draft that lists both shared contracts and has a contract of its own.
const std::string kPlan = R"json({
  "schemaVersion": "next-run-plan.v1",
  "title": "History page",
  "sharedContracts": [
    {"id": "S1", "kind": "interface_contract", "title": "Loss entry",
     "text": "property string metricId\nproperty real share", "appliesToPaths": ["qml/models"]},
    {"id": "S2", "kind": "constraint", "title": "File size",
     "text": "No file may exceed 300 lines.", "appliesToPaths": []}
  ],
  "drafts": [
    {"key": "model", "title": "Model task", "task": "Create the model.", "sharedContractIds": ["S1"]},
    {"key": "view", "title": "View task", "task": "Create the view.",
     "dependsOn": ["model"], "sharedContractIds": ["S1", "S2"],
     "contracts": [{"id": "V1", "kind": "interface_contract", "title": "View signal",
                    "text": "signal activated(string id)", "appliesToPaths": ["qml/pages"]}]}
  ]
})json";

std::string envelope(const std::string& json) { return "Here is the plan.\n\n```next-run-plan.v1\n" + json + "\n```\n\nTell me what to change."; }

const std::chrono::system_clock::time_point kStart {std::chrono::milliseconds(1'700'000'000'000)};

// Runs known to the service, and what project each belongs to.
class FakeRunLookup final : public IRunSourceLookup {
public:
    std::map<RunId, ProjectId> runs;
    std::map<RunId, RunSourceChoice> choices;   // what listProjectRuns offers, by Run id (project from `runs`)
    std::optional<ai::common::Error> failWith;
    mutable int lookups {0};

    [[nodiscard]] ai::common::Result<std::optional<ProjectId>> findRunProject(const RunId& runId) const override
    {
        ++lookups;
        if (failWith.has_value()) return ai::common::Result<std::optional<ProjectId>>::failure(*failWith);
        const auto it = runs.find(runId);
        return ai::common::Result<std::optional<ProjectId>>::ok(
            it == runs.end() ? std::nullopt : std::optional<ProjectId>(it->second));
    }

    [[nodiscard]] ai::common::Result<std::vector<RunSourceChoice>> listProjectRuns(const ProjectId& projectId) const override
    {
        if (failWith.has_value()) return ai::common::Result<std::vector<RunSourceChoice>>::failure(*failWith);
        std::vector<RunSourceChoice> listed;
        for (const auto& entry : choices) {
            const auto owner = runs.find(entry.first);
            if (owner != runs.end() && owner->second == projectId) listed.push_back(entry.second);
        }
        return ai::common::Result<std::vector<RunSourceChoice>>::ok(std::move(listed));
    }
};

struct Fixture {
    InMemoryProjectRepository projects;
    InMemoryRunDraftRepository drafts;
    FakeChatRepository chats;
    std::chrono::system_clock::time_point current {kStart};
    FakeRunLookup runs;
    RunDraftPlanService service {drafts, projects, chats, runs, [this] { return current; }};
    std::string projectId {"project-1"};
    std::string conversationId {"conversation-1"};
    int counter {0};

    Fixture()
    {
        addProject(projectId, "Project one", ProjectStatus::Active);
        ChatConversation conversation;
        conversation.id = conversationId;
        conversation.projectId = projectId;
        conversation.title = "Plan";
        require(chats.insertConversation(conversation).success(), "setup: the conversation");
    }

    void addProject(const std::string& id, const std::string& name, ProjectStatus status)
    {
        Project project;
        project.id = id;
        project.name = name;
        project.rootPath = std::filesystem::path("C:/projects") / id;
        project.status = status;
        require(projects.insert(project).success(), "setup: project " + id);
    }

    ChatMessage addMessage(const std::string& id, ChatRole role, ChatMessageStatus status, const std::string& content,
        const std::string& conversation = {})
    {
        ChatMessage message;
        message.id = id;
        message.conversationId = conversation.empty() ? conversationId : conversation;
        message.role = role;
        message.status = status;
        message.content = content;
        require(chats.insertMessage(message).success(), "setup: message " + id);
        return message;
    }

    ChatMessage planMessage(const std::string& id = "message-1", const std::string& json = kPlan)
    {
        return addMessage(id, ChatRole::Assistant, ChatMessageStatus::Sent, envelope(json));
    }

    RunDraft templateDraft(const std::string& id = "template-1")
    {
        RunDraft draft;
        draft.id = id;
        draft.name = "Template";
        draft.projectId = projectId;
        draft.title = "Template title";
        draft.task = "Template task";
        draft.workflowId = kExecutionManifestWorkflowId;
        draft.launchMode = RunLaunchMode::CustomRoster;
        draft.executionProfileId = "profile-1";
        for (std::uint32_t i = 0; i < 3; ++i) {
            StageModelAssignment entry;
            entry.stageType = i == 0 ? "lead" : "worker-" + std::to_string(i);
            entry.providerId = "provider-1";
            entry.modelId = "model-" + std::to_string(i);
            entry.timeout = std::chrono::milliseconds(90'000);
            entry.agentName = "Agent " + std::to_string(i);
            entry.role = i == 0 ? "lead" : "worker";
            entry.instructions = "Instructions " + std::to_string(i);
            entry.position = i;
            entry.leadArchitect = i == 0;
            entry.maxAttempts = 2;
            entry.workspaceAccess = i != 0;
            draft.roster.push_back(entry);
        }
        draft.maxParallelism = 7;
        draft.workspacePath = std::filesystem::path("C:/projects/template-workspace");
        draft.verificationRepairPolicy = VerificationRepairPolicy::Disabled;
        draft.providerBudget.maxTurns = 33;
        draft.providerBudget.maxWallTimeMs = 600'000;
        RunInterfaceContract contract;
        contract.id = "T1";
        contract.title = "Template contract";
        contract.text = "must not be copied";
        draft.interfaceContracts = {contract};
        draft.createdAt = kStart;
        draft.updatedAt = kStart;
        require(drafts.insert(draft).success(), "setup: the template draft");
        return draft;
    }

    std::size_t draftCount() const { return drafts.listAll().value().size(); }
};

std::string codeOf(const ai::common::Result<RunDraftPlanPreview>& result)
{
    require(!result.success() && result.error().has_value(), "The preview must fail");
    return result.error()->code;
}

std::string codeOf(const ai::common::Result<RunDraftPlanCreation>& result)
{
    require(!result.success() && result.error().has_value(), "The creation must fail");
    return result.error()->code;
}

std::string codeOf(const ai::common::Result<std::vector<RunSourceChoice>>& result)
{
    require(!result.success() && result.error().has_value(), "The source-Run listing must fail");
    return result.error()->code;
}

std::string codeOf(const ai::common::Result<std::vector<RunDraft>>& result)
{
    require(!result.success() && result.error().has_value(), "The listing must fail");
    return result.error()->code;
}

// A write is always the confirm of a preview: preview the message, then confirm the token it returned.
ai::common::Result<RunDraftPlanCreation> confirmMessage(RunDraftPlanService& service, const std::string& conversationId,
    const std::string& messageId, const std::optional<RunDraftId>& rosterTemplate = std::nullopt,
    const std::vector<RunId>& sourceRunIds = {})
{
    const auto preview = service.previewFromMessage(conversationId, messageId);
    if (!preview.success()) return ai::common::Result<RunDraftPlanCreation>::failure(*preview.error());
    return service.createFromPreview(preview.value().previewToken, rosterTemplate, sourceRunIds);
}

bool sameRoster(const std::vector<StageModelAssignment>& left, const std::vector<StageModelAssignment>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].stageType != right[i].stageType || left[i].providerId != right[i].providerId
                || left[i].modelId != right[i].modelId || left[i].timeout != right[i].timeout
                || left[i].agentName != right[i].agentName || left[i].instructions != right[i].instructions
                || left[i].position != right[i].position || left[i].maxAttempts != right[i].maxAttempts
                || left[i].workspaceAccess != right[i].workspaceAccess) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------

void testThePerMessageState()
{
    const auto message = [](ChatRole role, ChatMessageStatus status, const std::string& content) {
        ChatMessage value;
        value.id = "m";
        value.conversationId = "c";
        value.role = role;
        value.status = status;
        value.content = content;
        return value;
    };
    const std::string one = envelope(kPlan);
    require(planEnvelopeStateOf(message(ChatRole::Assistant, ChatMessageStatus::Sent, one)) == NextRunPlanEnvelopeState::One,
        "A completed Assistant message with one envelope");
    require(planEnvelopeStateOf(message(ChatRole::Assistant, ChatMessageStatus::Sent, one + "\n" + one)) == NextRunPlanEnvelopeState::Multiple,
        "...two envelopes");
    require(planEnvelopeStateOf(message(ChatRole::Assistant, ChatMessageStatus::Sent, "```next-run-plan.v1\n" + std::string(kNextRunPlanMaxBytes + 1, 'x') + "\n```"))
            == NextRunPlanEnvelopeState::TooLarge,
        "...a body over the limit");
    require(planEnvelopeStateOf(message(ChatRole::Assistant, ChatMessageStatus::Sent, "An ordinary reply."))
            == NextRunPlanEnvelopeState::None,
        "...an ordinary reply");
    require(planEnvelopeStateOf(message(ChatRole::Assistant, ChatMessageStatus::Sent, "Mention of next-run-plan.v1 in prose."))
            == NextRunPlanEnvelopeState::None,
        "...a mention of the tag in prose");
    // Whatever the text says, only a completed Assistant message offers the action.
    for (const auto status : {ChatMessageStatus::Generating, ChatMessageStatus::Failed, ChatMessageStatus::Stopped}) {
        require(planEnvelopeStateOf(message(ChatRole::Assistant, status, one)) == NextRunPlanEnvelopeState::None,
            "An unfinished Assistant message offers nothing");
    }
    for (const auto role : {ChatRole::User, ChatRole::System, ChatRole::Error}) {
        require(planEnvelopeStateOf(message(role, ChatMessageStatus::Sent, one)) == NextRunPlanEnvelopeState::None,
            "Only an Assistant message offers the action");
    }
}

void testAPreviewWritesNothing()
{
    Fixture fx;
    fx.planMessage();
    const auto preview = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(preview.success(), "The preview of a valid plan: " + (preview.error() ? preview.error()->message : ""));
    const auto& value = preview.value();
    require(value.projectId == fx.projectId && value.conversationId == fx.conversationId
            && value.origin == RunDraftGroupOrigin::ChatMessage && value.messageId == std::optional<std::string>("message-1")
            && !value.previewToken.empty() && !value.existingGroupId.has_value(),
        "The preview names its source and carries the token that confirms it");
    require(value.plan.title == "History page" && value.plan.drafts.size() == 2 && value.plan.fingerprint.size() == 64,
        "...and carries the validated plan");
    require(fx.draftCount() == 0 && !fx.drafts.findGroupByIdempotencyKey(std::string(64, 'a')).value().has_value(),
        "A preview writes nothing");

    // A plan with a loose end is previewed with its warning.
    const std::string unused = std::string(kPlan).replace(kPlan.find("\"S1\", \"S2\""), 10, "\"S1\"");
    fx.planMessage("message-2", unused);
    const auto warned = fx.service.previewFromMessage(fx.conversationId, "message-2");
    require(warned.success() && warned.value().plan.warnings.size() == 1
            && warned.value().plan.warnings[0].code == "next_run_plan.unreferenced_shared_contract",
        "A warning reaches the preview");
    require(fx.draftCount() == 0, "...and still nothing is written");
}

void testCreatingFromAMessageBuildsTheGroup()
{
    Fixture fx;
    const ChatMessage message = fx.planMessage();
    fx.current = kStart + std::chrono::seconds(5);
    const auto created = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(created.success(), "A valid plan creates: " + (created.error() ? created.error()->code + " " + created.error()->message : ""));
    const auto& group = created.value().group;
    require(!created.value().alreadyExisted && created.value().drafts.size() == 2, "Two new drafts");

    require(group.projectId == fx.projectId && group.origin == RunDraftGroupOrigin::ChatMessage
            && group.planTitle == "History page" && group.planFingerprint.size() == 64
            && group.sourceConversationId == std::optional<std::string>(fx.conversationId)
            && group.sourceMessageId == std::optional<std::string>("message-1")
            && group.sourceMessageFingerprint == std::optional<std::string>(sha256Hex(message.content))
            && !group.contextFingerprint.has_value() && group.sourceRunIds.empty()
            && group.createdAt == kStart + std::chrono::seconds(5),
        "The group records its source: project, message, message content fingerprint; no snapshot, no source runs");
    const auto stored = fx.drafts.findGroupById(group.id);
    require(stored.success() && stored.value().has_value() && stored.value()->idempotencyKey == group.idempotencyKey,
        "...and is stored");
    const auto replanned = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(replanned.success() && group.planFingerprint == replanned.value().plan.fingerprint
            && stored.value()->planFingerprint == group.planFingerprint && stored.value()->planTitle == "History page",
        "The group carries the fingerprint and title of the plan it was made from");

    const auto& model = created.value().drafts[0];
    const auto& view = created.value().drafts[1];
    for (const auto& draft : created.value().drafts) {
        require(draft.name == draft.title, "The draft's name is its title");
        require(draft.projectId == fx.projectId && draft.launchMode == RunLaunchMode::CustomRoster
                && draft.workflowId == kExecutionManifestWorkflowId && !draft.executionProfileId.has_value()
                && !draft.workspacePath.has_value() && draft.roster.empty() && draft.maxParallelism == 4
                && draft.verificationRepairPolicy == VerificationRepairPolicy::OneBoundedWave
                && !hasExplicitProviderBudgetLimits(draft.providerBudget)
                && draft.createdAt == group.createdAt && draft.updatedAt == group.createdAt,
            "A plan draft is a per-Run Manifest roster draft with an empty roster and the defaults");
        require(draft.provenance.has_value() && draft.provenance->groupId == group.id, "...inside the group");
        require(draft.provenance->createdContentFingerprint
                == nextRunPlanDraftContentFingerprint(draft.title, draft.task, draft.interfaceContracts, draft.provenance->dependsOn),
            "...sealed with the fingerprint of its content");
    }
    require(model.title == "Model task" && model.task == "Create the model." && model.provenance->ordinal == 0
            && model.provenance->planKey == "model" && model.provenance->dependsOn.empty(),
        "The first draft");
    require(view.title == "View task" && view.provenance->ordinal == 1 && view.provenance->planKey == "view"
            && view.provenance->dependsOn == std::vector<std::string> {"model"},
        "The second draft and its advisory dependency");
    // Shared contracts are attached only where listed, in the draft's own order, then its own contract.
    require(model.interfaceContracts.size() == 1 && model.interfaceContracts[0].id == "S1"
            && view.interfaceContracts.size() == 3 && view.interfaceContracts[0].id == "S1"
            && view.interfaceContracts[1].id == "S2" && view.interfaceContracts[2].id == "V1"
            && view.interfaceContracts[0].text == "property string metricId\nproperty real share",
        "The shared contracts reach exactly the drafts that list them, verbatim");

    const auto listed = fx.drafts.listGroupDrafts(group.id);
    require(listed.success() && listed.value().size() == 2 && listed.value()[0].id == model.id && listed.value()[1].id == view.id,
        "The drafts are stored in order");
}

// What was previewed is the response WITH the context it was generated with. If the stored attachment of the response is
// not the same one at the confirm (a different snapshot, one that appeared, one that disappeared), the preview is stale:
// the group's provenance must never name a context other than the one the user was shown. (SQLite makes an attachment
// immutable; this guards the service against any other repository.)
void testAContextThatChangesBetweenPreviewAndConfirmIsStale()
{
    Fixture fx;
    ChatProjectContextAttachment context;
    context.projectId = fx.projectId;
    context.snapshotFingerprint = std::string(64, 'c');

    auto changing = fx.planMessage("changing-context-plan");
    changing.projectContext = context;
    require(fx.chats.updateMessage(changing).success(), "setup: attach the first snapshot");
    const auto first = fx.service.previewFromMessage(fx.conversationId, changing.id);
    require(first.success(), "setup: preview with the first snapshot");
    changing.projectContext->snapshotFingerprint = std::string(64, 'e');
    require(fx.chats.updateMessage(changing).success(), "setup: another snapshot is attached");
    require(codeOf(fx.service.createFromPreview(first.value().previewToken)) == "run_draft_plan.stale_preview"
            && fx.draftCount() == 0,
        "A different attached snapshot at the confirm must make the preview stale");

    auto appearing = fx.planMessage("appearing-context-plan");
    const auto second = fx.service.previewFromMessage(fx.conversationId, appearing.id);
    require(second.success(), "setup: preview of a response without a context");
    appearing.projectContext = context;
    require(fx.chats.updateMessage(appearing).success(), "setup: a context appears");
    require(codeOf(fx.service.createFromPreview(second.value().previewToken)) == "run_draft_plan.stale_preview"
            && fx.draftCount() == 0,
        "A context that appeared after the preview must make it stale");

    auto vanishing = fx.planMessage("vanishing-context-plan");
    vanishing.projectContext = context;
    require(fx.chats.updateMessage(vanishing).success(), "setup: a context is attached");
    const auto third = fx.service.previewFromMessage(fx.conversationId, vanishing.id);
    require(third.success(), "setup: preview with a context");
    vanishing.projectContext.reset();
    require(fx.chats.updateMessage(vanishing).success(), "setup: the context disappears");
    require(codeOf(fx.service.createFromPreview(third.value().previewToken)) == "run_draft_plan.stale_preview"
            && fx.draftCount() == 0,
        "A context that disappeared after the preview must make it stale");
}

void testOnlyTheActuallyAttachedSnapshotFingerprintReachesTheGroup()
{
    Fixture fx;
    auto attached = fx.planMessage("attached-plan");
    ChatProjectContextAttachment context;
    context.projectId = fx.projectId;
    context.snapshotFingerprint = std::string(64, 'c');
    attached.projectContext = context;
    require(fx.chats.updateMessage(attached).success(), "setup: attach context provenance");
    const auto created = confirmMessage(fx.service, fx.conversationId, attached.id);
    require(created.success()
            && created.value().group.contextFingerprint == std::optional<std::string>(std::string(64, 'c')),
        "A group from Chat must copy the exact snapshot fingerprint persisted on its source response");

    auto foreign = fx.planMessage("foreign-context-plan");
    context.projectId = "another-project";
    context.snapshotFingerprint = std::string(64, 'd');
    foreign.projectContext = context;
    require(fx.chats.updateMessage(foreign).success(), "setup: attach foreign context provenance");
    require(codeOf(fx.service.previewFromMessage(fx.conversationId, foreign.id))
            == "run_draft_plan.context_project_mismatch",
        "A response generated with another project's context must not author drafts in this project");
}

void testCreatingIsIdempotent()
{
    Fixture fx;
    const ChatMessage message = fx.planMessage();
    const auto first = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(first.success(), "setup: the first create");
    require(fx.draftCount() == 2, "setup: two drafts");

    // The preview now says where a create would land.
    const auto preview = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(preview.success() && preview.value().existingGroupId == std::optional<std::string>(first.value().group.id),
        "The preview names the group that already exists");

    // A double click: the same group, nothing written.
    fx.current += std::chrono::minutes(1);
    const auto second = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(second.success() && second.value().alreadyExisted && second.value().group.id == first.value().group.id
            && second.value().drafts.size() == 2 && second.value().drafts[0].id == first.value().drafts[0].id,
        "A repeated create returns the existing group");
    require(fx.draftCount() == 2, "...and writes nothing");

    // The roster template is not part of the identity, and a repeat never rewrites the rosters.
    fx.templateDraft();
    const auto third = confirmMessage(fx.service, fx.conversationId, "message-1", RunDraftId("template-1"));
    require(third.success() && third.value().alreadyExisted && third.value().group.id == first.value().group.id,
        "A repeat with a template still returns the existing group");
    for (const auto& draft : fx.drafts.listGroupDrafts(first.value().group.id).value()) {
        require(draft.roster.empty() && draft.maxParallelism == 4, "...and its rosters are untouched");
    }
    // A regenerated or edited message has another content fingerprint: a new group.
    ChatMessage edited = message;
    edited.content += "\nEdited.";
    require(fx.chats.updateMessage(edited).success(), "setup: edit the message");
    const auto afterEdit = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(afterEdit.success() && !afterEdit.value().alreadyExisted && afterEdit.value().group.id != first.value().group.id,
        "An edited message makes a new group");
    require(fx.draftCount() == 5, "...with its own drafts");

    // A different plan in the same message is a different plan fingerprint too (the content changed above anyway).
    // Another message with the same plan text: another message id, another group.
    fx.planMessage("message-2");
    const auto other = confirmMessage(fx.service, fx.conversationId, "message-2");
    require(other.success() && !other.value().alreadyExisted && other.value().group.id != first.value().group.id,
        "The same plan in another message is another group");

    // After the user removes every draft of a group, the same message can be imported again.
    for (const auto& draft : first.value().drafts) require(fx.drafts.remove(draft.id).success(), "setup: remove a draft");
    require(!fx.drafts.findGroupById(first.value().group.id).value().has_value(), "The emptied group is gone");
    ChatMessage restored = edited;
    restored.content = message.content;
    require(fx.chats.updateMessage(restored).success(), "setup: restore the message text");
    const auto again = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(again.success() && !again.value().alreadyExisted && again.value().group.id != first.value().group.id,
        "After the group is gone the same message creates a fresh one");
}

void testTheRosterTemplate()
{
    Fixture fx;
    fx.planMessage();
    const RunDraft tpl = fx.templateDraft();
    const auto created = confirmMessage(fx.service, fx.conversationId, "message-1", RunDraftId("template-1"));
    require(created.success(), "A plan with a roster template: " + (created.error() ? created.error()->code : ""));
    for (const auto& draft : created.value().drafts) {
        // Copied: the roster and the choices made with it.
        require(sameRoster(draft.roster, tpl.roster) && draft.maxParallelism == 7
                && draft.providerBudget.maxTurns == std::optional<std::uint64_t>(33)
                && draft.providerBudget.maxWallTimeMs == std::optional<std::uint64_t>(600'000)
                && draft.verificationRepairPolicy == VerificationRepairPolicy::Disabled,
            "The roster, parallelism, provider limits and repair policy are copied");
        // Not copied: anything else of the template.
        require(!draft.workspacePath.has_value() && !draft.executionProfileId.has_value()
                && draft.title != tpl.title && draft.task != tpl.task && draft.projectId == fx.projectId
                && draft.interfaceContracts.size() >= 1 && draft.interfaceContracts[0].id != "T1"
                && draft.name == draft.title,
            "Workspace, execution profile, objective and contracts are never copied");
    }
    // The template itself is untouched.
    const auto after = fx.drafts.findById("template-1");
    require(after.success() && after.value().has_value() && sameRoster(after.value()->roster, tpl.roster)
            && after.value()->title == "Template title" && !after.value()->provenance.has_value(),
        "The template is not changed");

    // What can serve as a template.
    const auto refuse = [&](const std::string& what, const RunDraft& draft, const std::string& code) {
        Fixture other;
        other.planMessage();
        RunDraft copy = draft;
        copy.id = "bad-template";
        copy.projectId = other.projectId;
        other.addProject("project-2", "Other", ProjectStatus::Active);
        require(other.drafts.insert(copy).success(), "setup: " + what);
        require(codeOf(confirmMessage(other.service, other.conversationId, "message-1", RunDraftId("bad-template"))) == code,
            what + ": expected " + code);
        require(other.draftCount() == 1, what + ": a refused template writes nothing");
    };
    RunDraft foreign = tpl;
    {
        Fixture other;
        other.planMessage();
        other.addProject("project-2", "Other", ProjectStatus::Active);
        foreign.id = "foreign";
        foreign.projectId = "project-2";
        require(other.drafts.insert(foreign).success(), "setup: a template of another project");
        require(codeOf(confirmMessage(other.service, other.conversationId, "message-1", RunDraftId("foreign")))
                == "run_draft_plan.template_wrong_project",
            "A template of another project is refused");
        require(other.draftCount() == 1, "...and nothing is written");
    }
    RunDraft mock = tpl;
    mock.launchMode = RunLaunchMode::Mock;
    refuse("a mock draft", mock, "run_draft_plan.template_not_manifest_roster");
    RunDraft profile = tpl;
    profile.launchMode = RunLaunchMode::SavedExecutionProfile;
    refuse("a saved-profile draft", profile, "run_draft_plan.template_not_manifest_roster");
    RunDraft legacy = tpl;
    legacy.workflowId = "execution-profile-v1";
    refuse("a legacy roster draft", legacy, "run_draft_plan.template_not_manifest_roster");
    RunDraft empty = tpl;
    empty.roster.clear();
    refuse("a draft without a roster", empty, "run_draft_plan.template_without_roster");
    // A NEW group validates its template (message-1 already has a group: a repeat never looks at the template).
    fx.planMessage("message-fresh");
    require(codeOf(confirmMessage(fx.service, fx.conversationId, "message-fresh", RunDraftId("missing")))
            == "run_draft_plan.template_not_found",
        "An unknown template is refused");

    // The choices offered to the UI.
    // The created drafts now carry a roster themselves, so they qualify as templates too: the template and the two
    // plan drafts, and nothing else.
    const auto choices = fx.service.listRosterTemplates(fx.conversationId);
    require(choices.success() && choices.value().size() == 3, "The template and the two drafts that got its roster");
    std::vector<std::string> offered;
    for (const auto& draft : choices.value()) offered.push_back(draft.id);
    std::vector<std::string> expected {"template-1", created.value().drafts[0].id, created.value().drafts[1].id};
    std::sort(offered.begin(), offered.end());
    std::sort(expected.begin(), expected.end());
    require(offered == expected, "Only per-Run Manifest roster drafts with a roster of the conversation's project are offered");
}

void testAManualImportTakesARosterTemplateToo()
{
    Fixture fx;
    const RunDraft tpl = fx.templateDraft();
    const auto preview = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(preview.success(), "setup: a paste preview");
    const auto created = fx.service.createFromPreview(preview.value().previewToken, RunDraftId("template-1"));
    require(created.success(), "A confirm with a roster template: " + (created.error() ? created.error()->code : ""));
    require(created.value().drafts.size() == 2 && fx.draftCount() == 3, "Two new drafts next to the template");
    for (const auto& draft : created.value().drafts) {
        require(sameRoster(draft.roster, tpl.roster) && draft.maxParallelism == 7
                && draft.verificationRepairPolicy == VerificationRepairPolicy::Disabled
                && draft.providerBudget.maxTurns == std::optional<std::uint64_t>(33)
                && !draft.workspacePath.has_value(),
            "A manual import copies the roster and its choices, and nothing else");
    }

    // The same refusals as for a message: unknown, and another project's draft - both before anything is written.
    const auto second = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(second.success(), "setup: another preview");
    require(codeOf(fx.service.createFromPreview(second.value().previewToken, RunDraftId("no-such-draft")))
            == "run_draft_plan.template_not_found" && fx.draftCount() == 3,
        "An unknown template is refused on confirm");
    fx.addProject("project-2", "Other", ProjectStatus::Active);
    RunDraft foreign = tpl;
    foreign.id = "foreign";
    foreign.projectId = "project-2";
    require(fx.drafts.insert(foreign).success(), "setup: a template of another project");
    require(codeOf(fx.service.createFromPreview(second.value().previewToken, RunDraftId("foreign")))
            == "run_draft_plan.template_wrong_project" && fx.draftCount() == 4,
        "A template of another project is refused on confirm");
    // The preview survives a refused confirm and still works without a template.
    require(fx.service.createFromPreview(second.value().previewToken).success() && fx.draftCount() == 6,
        "A refused confirm leaves the preview usable");
}

void testTheSourceMustBeAFinishedAssistantMessageOfTheConversation()
{
    Fixture fx;
    fx.addMessage("user-1", ChatRole::User, ChatMessageStatus::Sent, envelope(kPlan));
    fx.addMessage("assistant-failed", ChatRole::Assistant, ChatMessageStatus::Failed, envelope(kPlan));
    fx.addMessage("assistant-generating", ChatRole::Assistant, ChatMessageStatus::Generating, envelope(kPlan));
    fx.addMessage("assistant-stopped", ChatRole::Assistant, ChatMessageStatus::Stopped, envelope(kPlan));
    fx.addMessage("system-1", ChatRole::System, ChatMessageStatus::Sent, envelope(kPlan));
    for (const char* id : {"user-1", "assistant-failed", "assistant-generating", "assistant-stopped", "system-1"}) {
        require(codeOf(fx.service.previewFromMessage(fx.conversationId, id)) == "run_draft_plan.message_not_eligible",
            std::string("Not eligible: ") + id);
        require(codeOf(confirmMessage(fx.service, fx.conversationId, id)) == "run_draft_plan.message_not_eligible",
            std::string("Not eligible to create: ") + id);
    }
    require(codeOf(fx.service.previewFromMessage(fx.conversationId, "nope")) == "run_draft_plan.message_not_found", "No such message");

    // A message of ANOTHER conversation is not this conversation's to read.
    ChatConversation foreign;
    foreign.id = "conversation-2";
    foreign.projectId = fx.projectId;
    require(fx.chats.insertConversation(foreign).success(), "setup: another conversation");
    fx.addMessage("foreign-1", ChatRole::Assistant, ChatMessageStatus::Sent, envelope(kPlan), "conversation-2");
    require(codeOf(fx.service.previewFromMessage(fx.conversationId, "foreign-1")) == "run_draft_plan.message_not_found",
        "A message of another conversation is reported as missing");
    require(codeOf(confirmMessage(fx.service, fx.conversationId, "foreign-1")) == "run_draft_plan.message_not_found",
        "...also to create");
    require(fx.draftCount() == 0, "Nothing was written");
}

void testTheProjectComesFromTheConversationAndMustBeActive()
{
    Fixture fx;
    fx.planMessage();
    require(codeOf(fx.service.previewFromMessage("no-such-conversation", "message-1")) == "run_draft_plan.conversation_not_found",
        "An unknown conversation");

    ChatConversation loose;
    loose.id = "conversation-loose";
    require(fx.chats.insertConversation(loose).success(), "setup: a conversation without a project");
    fx.addMessage("loose-1", ChatRole::Assistant, ChatMessageStatus::Sent, envelope(kPlan), "conversation-loose");
    require(codeOf(fx.service.previewFromMessage("conversation-loose", "loose-1")) == "run_draft_plan.conversation_without_project",
        "A conversation without a project");
    require(codeOf(fx.service.previewFromPaste("conversation-loose", kPlan)) == "run_draft_plan.conversation_without_project",
        "...also for a paste");
    require(codeOf(confirmMessage(fx.service, "conversation-loose", "loose-1")) == "run_draft_plan.conversation_without_project",
        "...and to create");

    ChatConversation dangling;
    dangling.id = "conversation-dangling";
    dangling.projectId = "project-that-is-gone";
    require(fx.chats.insertConversation(dangling).success(), "setup: a conversation of an unknown project");
    require(codeOf(fx.service.previewFromPaste("conversation-dangling", kPlan)) == "run_draft_plan.project_not_found",
        "A conversation whose project is gone");

    // An archived project takes no new drafts.
    auto project = fx.projects.findById(fx.projectId).value().value();
    project.status = ProjectStatus::Archived;
    require(fx.projects.update(project).success(), "setup: archive the project");
    require(codeOf(fx.service.previewFromMessage(fx.conversationId, "message-1")) == "run_draft_plan.project_inactive", "Archived");
    require(codeOf(confirmMessage(fx.service, fx.conversationId, "message-1")) == "run_draft_plan.project_inactive",
        "...also to create");
    require(codeOf(fx.service.listRosterTemplates(fx.conversationId)) == "run_draft_plan.project_inactive", "...and to list templates");
    project.status = ProjectStatus::Unavailable;
    require(fx.projects.update(project).success(), "setup: make the project unavailable");
    require(codeOf(fx.service.previewFromPaste(fx.conversationId, kPlan)) == "run_draft_plan.project_inactive", "Unavailable");
    require(fx.draftCount() == 0, "Nothing was written");
}

void testBadPlansFailWithTheirOwnCodes()
{
    Fixture fx;
    const auto expect = [&](const std::string& what, const std::string& content, const std::string& code) {
        fx.addMessage("m-" + std::to_string(++fx.counter), ChatRole::Assistant, ChatMessageStatus::Sent, content);
        const std::string id = "m-" + std::to_string(fx.counter);
        require(codeOf(fx.service.previewFromMessage(fx.conversationId, id)) == code, what + " (preview)");
        require(codeOf(confirmMessage(fx.service, fx.conversationId, id)) == code, what + " (create)");
        require(fx.draftCount() == 0, what + ": nothing written");
    };
    expect("no envelope", "A reply without a plan.", "next_run_plan.envelope_missing");
    expect("two envelopes", envelope(kPlan) + "\n" + envelope(kPlan), "next_run_plan.envelope_multiple");
    expect("an unclosed envelope", "```next-run-plan.v1\n" + kPlan, "next_run_plan.envelope_unterminated");
    expect("an oversized envelope", "```next-run-plan.v1\n" + std::string(kNextRunPlanMaxBytes + 1, 'x') + "\n```", "next_run_plan.too_large");
    expect("broken JSON", envelope("{"), "next_run_plan.invalid_json");
    expect("another schema version", envelope(std::string(kPlan).replace(kPlan.find("next-run-plan.v1"), 16, "next-run-plan.v2")),
        "next_run_plan.schema_version");
    expect("a field a plan may not have", envelope(std::string(kPlan).replace(kPlan.find("\"title\": \"History page\""), 0, "\"roster\": [], ")),
        "next_run_plan.unknown_field");
    expect("a dependency cycle",
        envelope(std::string(kPlan).replace(kPlan.find("\"sharedContractIds\": [\"S1\"]}"), 0, "\"dependsOn\": [\"view\"], ")),
        "next_run_plan.dependency_cycle");
    expect("a contract the Run rules refuse",
        envelope(std::string(kPlan).replace(kPlan.find("qml/pages"), 9, "../pages")), "run_contracts.invalid_scope");
}

void testAManualPasteIsPreviewedThenConfirmedByToken()
{
    Fixture fx;
    const auto preview = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(preview.success(), "A bare JSON paste: " + (preview.error() ? preview.error()->code : ""));
    require(preview.value().origin == RunDraftGroupOrigin::ManualImport && !preview.value().previewToken.empty()
            && !preview.value().messageId.has_value() && preview.value().plan.drafts.size() == 2,
        "The preview carries a token and no message");
    require(fx.draftCount() == 0, "A preview writes nothing");
    const auto fenced = fx.service.previewFromPaste(fx.conversationId, envelope(kPlan));
    require(fenced.success() && fenced.value().previewToken != preview.value().previewToken
            && fenced.value().plan.fingerprint == preview.value().plan.fingerprint,
        "A fenced paste gives the same plan and another token");

    const std::string token = preview.value().previewToken;
    const auto created = fx.service.createFromPreview(token);
    require(created.success() && !created.value().alreadyExisted, "Confirming by token creates");
    const auto& group = created.value().group;
    require(group.origin == RunDraftGroupOrigin::ManualImport && !group.sourceConversationId.has_value()
            && !group.sourceMessageId.has_value() && !group.sourceMessageFingerprint.has_value()
            && !group.contextFingerprint.has_value() && group.planTitle == "History page",
        "A manual import records no message and no context fingerprint");
    require(created.value().drafts.size() == 2 && fx.draftCount() == 2, "...with its drafts");

    // A double click on the same preview: the same group.
    const auto again = fx.service.createFromPreview(token);
    require(again.success() && again.value().alreadyExisted && again.value().group.id == group.id && fx.draftCount() == 2,
        "Confirming the same preview twice returns the same group");

    // A NEW preview of the same text is a deliberate second copy.
    const auto copy = fx.service.createFromPreview(fenced.value().previewToken);
    require(copy.success() && !copy.value().alreadyExisted && copy.value().group.id != group.id && fx.draftCount() == 4,
        "A new preview of the same plan creates a separate copy");

    require(codeOf(fx.service.createFromPreview("not-a-token")) == "run_draft_plan.preview_not_found", "An unknown token");

    // Bad pasted text.
    require(codeOf(fx.service.previewFromPaste(fx.conversationId, "hello")) == "next_run_plan.envelope_missing", "Not a plan");
    require(codeOf(fx.service.previewFromPaste(fx.conversationId, "{\"title\": \"x\"}")) == "next_run_plan.schema_version",
        "Not THIS plan");
    require(codeOf(fx.service.previewFromPaste(fx.conversationId, "{\"a\": 1}")) == "next_run_plan.unknown_field",
        "A field a plan may not have");
    require(fx.draftCount() == 4, "Nothing more was written");
}

void testPreviewsExpireAndAreBounded()
{
    Fixture fx;
    const auto preview = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(preview.success(), "setup: a preview");
    // Just before the lifetime ends it still works; at the lifetime it does not.
    fx.current = kStart + RunDraftPlanService::kPreviewLifetime - std::chrono::seconds(1);
    const auto fresh = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(fresh.success(), "setup: a later preview");
    const auto early = fx.service.createFromPreview(preview.value().previewToken);
    require(early.success(), "A preview one second before its lifetime ends is still good");
    fx.current = kStart + RunDraftPlanService::kPreviewLifetime;
    require(codeOf(fx.service.createFromPreview(preview.value().previewToken)) == "run_draft_plan.preview_expired",
        "At its lifetime a preview has expired");
    require(codeOf(fx.service.createFromPreview(preview.value().previewToken)) == "run_draft_plan.preview_not_found",
        "...and is dropped afterwards");
    require(fx.service.createFromPreview(fresh.value().previewToken).success(), "A newer preview is still good");

    // A preview that has reached its lifetime is dropped as soon as the next one is made (not just refused later).
    Fixture swept;
    const auto stale = swept.service.previewFromPaste(swept.conversationId, kPlan);
    require(stale.success(), "setup: a preview to go stale");
    swept.current = kStart + RunDraftPlanService::kPreviewLifetime;
    const auto next = swept.service.previewFromPaste(swept.conversationId, kPlan);
    require(next.success(), "setup: the preview that sweeps");
    require(codeOf(swept.service.createFromPreview(stale.value().previewToken)) == "run_draft_plan.preview_not_found",
        "A preview at its lifetime is dropped when the next preview is made");
    Fixture longGone;
    const auto ancient = longGone.service.previewFromPaste(longGone.conversationId, kPlan);
    require(ancient.success(), "setup: an old preview");
    longGone.current = kStart + 2 * RunDraftPlanService::kPreviewLifetime;
    require(longGone.service.previewFromPaste(longGone.conversationId, kPlan).success(), "setup: a later preview");
    require(codeOf(longGone.service.createFromPreview(ancient.value().previewToken)) == "run_draft_plan.preview_not_found",
        "...and so is one long past it");

    // The store is bounded: the oldest preview goes first.
    Fixture bounded;
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < RunDraftPlanService::kMaxPreviews + 1; ++i) {
        bounded.current = kStart + std::chrono::seconds(static_cast<long long>(i));
        const auto made = bounded.service.previewFromPaste(bounded.conversationId, kPlan);
        require(made.success(), "setup: a bounded-store preview");
        tokens.push_back(made.value().previewToken);
    }
    require(codeOf(bounded.service.createFromPreview(tokens.front())) == "run_draft_plan.preview_not_found",
        "The oldest preview was dropped to keep the store bounded");
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        require(bounded.service.createFromPreview(tokens[i]).success(), "The others are kept");
    }
}

void testAPreviewIsRecheckedAgainstTheProjectAtConfirm()
{
    Fixture fx;
    const auto preview = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(preview.success(), "setup: a preview");

    // The conversation moves to another project: the preview is for the old one and is refused.
    fx.addProject("project-2", "Other", ProjectStatus::Active);
    auto conversation = fx.chats.findConversation(fx.conversationId).value().value();
    conversation.projectId = "project-2";
    require(fx.chats.updateConversation(conversation).success(), "setup: move the conversation");
    require(codeOf(fx.service.createFromPreview(preview.value().previewToken)) == "run_draft_plan.project_changed",
        "A preview made for one project is not confirmed into another");
    require(fx.draftCount() == 0, "...and nothing is written");

    // Back in its project, archived meanwhile.
    conversation.projectId = fx.projectId;
    require(fx.chats.updateConversation(conversation).success(), "setup: move it back");
    auto project = fx.projects.findById(fx.projectId).value().value();
    project.status = ProjectStatus::Archived;
    require(fx.projects.update(project).success(), "setup: archive the project");
    require(codeOf(fx.service.createFromPreview(preview.value().previewToken)) == "run_draft_plan.project_inactive",
        "A project archived between the preview and the confirm takes no drafts");

    // Conversation deleted.
    project.status = ProjectStatus::Active;
    require(fx.projects.update(project).success(), "setup: reactivate");
    require(fx.chats.removeConversation(fx.conversationId).success(), "setup: delete the conversation");
    require(codeOf(fx.service.createFromPreview(preview.value().previewToken)) == "run_draft_plan.conversation_not_found",
        "A deleted conversation cannot confirm");
    require(fx.draftCount() == 0, "Nothing was written");
}

void testTemplateChoicesBelongToTheConversationsProject()
{
    Fixture fx;
    fx.templateDraft("template-b");
    fx.addProject("project-2", "Other", ProjectStatus::Active);
    RunDraft foreign = fx.templateDraft("template-a");
    // Move template-a to project-2 by rebuilding it there.
    require(fx.drafts.remove("template-a").success(), "setup: remove");
    foreign.projectId = "project-2";
    require(fx.drafts.insert(foreign).success(), "setup: a template of another project");
    RunDraft manual = foreign;
    manual.id = "manual";
    manual.projectId = fx.projectId;
    manual.roster.clear();
    require(fx.drafts.insert(manual).success(), "setup: a draft without a roster");
    RunDraft mock = fx.templateDraft("mock");
    mock.launchMode = RunLaunchMode::Mock;
    require(fx.drafts.update(mock).success(), "setup: a mock draft");
    RunDraft legacy = fx.templateDraft("legacy");
    legacy.workflowId = "execution-profile-v1";
    require(fx.drafts.update(legacy).success(), "setup: a draft on a legacy workflow");

    const auto choices = fx.service.listRosterTemplates(fx.conversationId);
    require(choices.success() && choices.value().size() == 1 && choices.value()[0].id == "template-b",
        "Only eligible drafts of the conversation's project are offered");
}


void testTheSameMessageInAnotherProjectIsAnotherGroup()
{
    Fixture fx;
    fx.planMessage();
    const auto first = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(first.success(), "setup: the first create");

    // The conversation moves to another active project: the identity includes the project.
    fx.addProject("project-2", "Other", ProjectStatus::Active);
    auto conversation = fx.chats.findConversation(fx.conversationId).value().value();
    conversation.projectId = "project-2";
    require(fx.chats.updateConversation(conversation).success(), "setup: move the conversation");
    const auto second = confirmMessage(fx.service, fx.conversationId, "message-1");
    require(second.success() && !second.value().alreadyExisted && second.value().group.id != first.value().group.id
            && second.value().group.projectId == "project-2" && fx.draftCount() == 4,
        "The same message in another project creates its own group there");
    for (const auto& draft : second.value().drafts) require(draft.projectId == "project-2", "...with drafts of that project");
    require(first.value().group.projectId == fx.projectId, "...and the first group stays in its project");
}

void testTheLoserOfARaceGetsTheWinnersGroup()
{
    Fixture fx;
    fx.planMessage();
    RacingDraftRepository racing(fx.drafts);
    RunDraftPlanService service(racing, fx.projects, fx.chats, fx.runs, [&fx] { return fx.current; });

    const auto winner = confirmMessage(service, fx.conversationId, "message-1");
    require(winner.success() && !winner.value().alreadyExisted, "setup: the winner writes the group");
    require(fx.draftCount() == 2, "setup: two drafts");

    // The loser looked before the winner wrote: its own insert hits the unique key and it picks up the winner's group.
    const auto loserPreview = service.previewFromMessage(fx.conversationId, "message-1");
    require(loserPreview.success(), "setup: the loser's preview");
    racing.hideKeyLookups = 1;
    const auto loser = service.createFromPreview(loserPreview.value().previewToken);
    require(loser.success() && loser.value().alreadyExisted && loser.value().group.id == winner.value().group.id
            && loser.value().drafts.size() == 2 && fx.draftCount() == 2,
        "The loser of a race gets the winner's group and writes nothing");

    // The same for a manual import confirmed twice at once.
    const auto preview = service.previewFromPaste(fx.conversationId, kPlan);
    require(preview.success(), "setup: a paste preview");
    const auto manualWinner = service.createFromPreview(preview.value().previewToken);
    require(manualWinner.success() && fx.draftCount() == 4, "setup: the manual winner");
    racing.hideKeyLookups = 1;
    const auto manualLoser = service.createFromPreview(preview.value().previewToken);
    require(manualLoser.success() && manualLoser.value().alreadyExisted
            && manualLoser.value().group.id == manualWinner.value().group.id && fx.draftCount() == 4,
        "...also for a manual import");

    // If the winner's group cannot be found after all, the duplicate-key failure is reported, not hidden.
    const auto lostPreview = service.previewFromMessage(fx.conversationId, "message-1");
    require(lostPreview.success(), "setup: a preview before the lookups are hidden");
    racing.hideKeyLookups = 2;
    const auto lost = service.createFromPreview(lostPreview.value().previewToken);
    require(codeOf(lost) == "run_draft_group.duplicate_idempotency_key" && fx.draftCount() == 4,
        "A duplicate key with no group to return is an error");

    // Only a duplicate key is a lost race: any other failure is an error even when the source already has a group.
    const auto failingPreview = service.previewFromMessage(fx.conversationId, "message-1");
    require(failingPreview.success(), "setup: a preview before the lookups are hidden");
    racing.hideKeyLookups = 1;
    racing.failInsertGroupWith = "run_draft_group.invalid_drafts";
    require(codeOf(service.createFromPreview(failingPreview.value().previewToken)) == "run_draft_group.invalid_drafts",
        "A failure that is not a duplicate key is not answered with the existing group");
    racing.failInsertGroupWith.reset();
    racing.hideKeyLookups = 0;

    // Any other write failure is passed through unchanged.
    Fixture other;
    other.planMessage();
    RacingDraftRepository failing(other.drafts);
    RunDraftPlanService failingService(failing, other.projects, other.chats, other.runs);
    failing.failInsertGroupWith = "run_draft_group.invalid";
    require(codeOf(confirmMessage(failingService, other.conversationId, "message-1")) == "run_draft_group.invalid",
        "Another write failure is forwarded with its own code");
    failing.failInsertGroupWith = "run_draft_group.duplicate_id";
    require(codeOf(confirmMessage(failingService, other.conversationId, "message-1")) == "run_draft_group.duplicate_id",
        "...also one that merely looks like a duplicate");
    require(other.draftCount() == 0, "...and nothing was written");
}


// P1: what is saved is exactly what the user was shown.
void testAMessageIsConfirmedOnlyThroughItsPreview()
{
    Fixture fx;
    const ChatMessage original = fx.planMessage();
    const auto preview = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(preview.success() && !preview.value().previewToken.empty(), "A message preview carries the token that confirms it");
    const std::string token = preview.value().previewToken;

    // The message id is not a way in: the Backend writes only against a token it issued.
    require(codeOf(fx.service.createFromPreview("message-1")) == "run_draft_plan.preview_not_found",
        "A message id is not a confirmation token");
    require(fx.draftCount() == 0, "...and nothing was written");

    // The reply is regenerated or edited after the user looked at the preview. Even a change that leaves a valid plan
    // (here a different task) must not be saved: it is not what was shown.
    ChatMessage changed = original;
    const std::string::size_type at = changed.content.find("Create the model.");
    require(at != std::string::npos, "setup: the task text is in the message");
    changed.content.replace(at, 17, "Create something else entirely.");
    require(fx.chats.updateMessage(changed).success(), "setup: the message changes after the preview");
    require(codeOf(fx.service.createFromPreview(token)) == "run_draft_plan.stale_preview",
        "A message changed since its preview is refused as stale");
    require(fx.draftCount() == 0, "...and nothing was written");

    // Strict: ANY change of the text is stale, also one outside the plan (the group identity includes the text).
    require(fx.chats.updateMessage(original).success(), "setup: the original text is back");
    ChatMessage appended = original;
    appended.content += "\nOne more sentence.";
    require(fx.chats.updateMessage(appended).success(), "setup: text is appended after the preview");
    require(codeOf(fx.service.createFromPreview(token)) == "run_draft_plan.stale_preview",
        "A change outside the plan is stale too");

    // A new preview shows what is there now, and confirming it saves exactly that.
    require(fx.chats.updateMessage(changed).success(), "setup: the changed text again");
    const auto fresh = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(fresh.success() && fresh.value().previewToken != token
            && fresh.value().plan.drafts[0].task == "Create something else entirely.",
        "A new preview shows the new plan under a new token");
    const auto created = fx.service.createFromPreview(fresh.value().previewToken);
    require(created.success() && !created.value().alreadyExisted && created.value().drafts[0].task == "Create something else entirely.",
        "Confirming it saves what it showed");
    require(created.value().group.sourceMessageFingerprint == std::optional<std::string>(sha256Hex(changed.content)),
        "...and the group records the text of the message that was previewed");
    require(codeOf(fx.service.createFromPreview(token)) == "run_draft_plan.stale_preview" && fx.draftCount() == 2,
        "The old token stays stale");

    // A double click on the confirmed preview, or a second preview of the unchanged message, finds the same group.
    const auto doubleClick = fx.service.createFromPreview(fresh.value().previewToken);
    require(doubleClick.success() && doubleClick.value().alreadyExisted && doubleClick.value().group.id == created.value().group.id,
        "Confirming the same preview twice returns the same group");
    const auto second = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(second.success() && second.value().previewToken != fresh.value().previewToken
            && second.value().existingGroupId == std::optional<std::string>(created.value().group.id),
        "A second preview of the unchanged message names the stored group");
    const auto viaSecond = fx.service.createFromPreview(second.value().previewToken);
    require(viaSecond.success() && viaSecond.value().alreadyExisted && viaSecond.value().group.id == created.value().group.id
            && fx.draftCount() == 2,
        "...and confirming it writes nothing");

    // A message that stops being a completed Assistant message after its preview cannot be confirmed.
    fx.planMessage("message-2");
    const auto third = fx.service.previewFromMessage(fx.conversationId, "message-2");
    require(third.success(), "setup: a preview of another message");
    ChatMessage failed = fx.chats.findMessage("message-2").value().value();
    failed.status = ChatMessageStatus::Failed;
    require(fx.chats.updateMessage(failed).success(), "setup: the message fails");
    require(codeOf(fx.service.createFromPreview(third.value().previewToken)) == "run_draft_plan.message_not_eligible"
            && fx.draftCount() == 2,
        "A message that is no longer a completed reply is refused");
}

// P2: a repeat is answered from the stored group, before the mutable template is looked at.
void testARepeatIsAnsweredFromTheStoredGroup()
{
    Fixture fx;
    fx.planMessage();
    fx.templateDraft();
    const auto first = confirmMessage(fx.service, fx.conversationId, "message-1", RunDraftId("template-1"));
    require(first.success() && !first.value().alreadyExisted, "setup: the group is created with the template");
    const std::size_t lookupsBefore = static_cast<std::size_t>(fx.runs.lookups);

    // The template is deleted afterwards: the repeat still returns the stored group, not an error.
    require(fx.drafts.remove("template-1").success(), "setup: the template draft is deleted");
    const auto afterDelete = confirmMessage(fx.service, fx.conversationId, "message-1", RunDraftId("template-1"));
    require(afterDelete.success() && afterDelete.value().alreadyExisted && afterDelete.value().group.id == first.value().group.id
            && afterDelete.value().drafts.size() == 2,
        "A repeat after the template was deleted returns the stored group");

    // ...or edited into something that could no longer serve, or never existed, or the source Runs are nonsense.
    RunDraft broken = fx.templateDraft("template-1");
    broken.launchMode = RunLaunchMode::Mock;
    require(fx.drafts.update(broken).success(), "setup: the template becomes a mock draft");
    require(confirmMessage(fx.service, fx.conversationId, "message-1", RunDraftId("template-1")).value().alreadyExisted,
        "...also after it was edited into an unusable draft");
    require(confirmMessage(fx.service, fx.conversationId, "message-1", RunDraftId("never-existed")).value().alreadyExisted,
        "...also with a template that never existed");
    const auto withRuns = confirmMessage(fx.service, fx.conversationId, "message-1", std::nullopt, {"no-such-run", "no-such-run"});
    require(withRuns.success() && withRuns.value().alreadyExisted && withRuns.value().group.sourceRunIds.empty(),
        "...and source Runs are not looked at either");
    require(static_cast<std::size_t>(fx.runs.lookups) == lookupsBefore, "A repeat makes no Run lookup");
    require(fx.draftCount() == 3, "Nothing was written (the first group's drafts and the re-created template)");

    // The same for a manual import confirmed again with a template that is gone.
    const auto preview = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(preview.success(), "setup: a paste preview");
    require(fx.drafts.remove("template-1").success(), "setup: remove the template");
    fx.templateDraft("template-2");
    const auto manual = fx.service.createFromPreview(preview.value().previewToken, RunDraftId("template-2"));
    require(manual.success() && !manual.value().alreadyExisted, "setup: the manual group is created with a template");
    require(fx.drafts.remove("template-2").success(), "setup: the template is deleted");
    const auto manualAgain = fx.service.createFromPreview(preview.value().previewToken, RunDraftId("template-2"));
    require(manualAgain.success() && manualAgain.value().alreadyExisted && manualAgain.value().group.id == manual.value().group.id,
        "A repeated manual confirm with a deleted template returns the stored group");

    // A NEW source still validates its template: nothing is written without one.
    fx.planMessage("message-2");
    const std::size_t before = fx.draftCount();
    require(codeOf(confirmMessage(fx.service, fx.conversationId, "message-2", RunDraftId("template-2")))
            == "run_draft_plan.template_not_found" && fx.draftCount() == before,
        "A new group with a deleted template is refused");
}

// P3: the Runs the user names as the source of the plan are recorded, and must belong to the project.
void testSourceRunsAreCheckedAgainstTheProjectAndRecorded()
{
    Fixture fx;
    fx.planMessage();
    fx.addProject("project-2", "Other", ProjectStatus::Active);
    fx.runs.runs = {{"run-1", fx.projectId}, {"run-2", fx.projectId}, {"run-other", "project-2"}};
    const auto preview = fx.service.previewFromMessage(fx.conversationId, "message-1");
    require(preview.success(), "setup: a preview");
    const std::string token = preview.value().previewToken;
    const auto refused = [&](const std::vector<RunId>& ids, const char* code, const char* what) {
        require(codeOf(fx.service.createFromPreview(token, std::nullopt, ids)) == code, what);
        require(fx.draftCount() == 0, "...and nothing was written");
    };

    refused({"run-1", "run-other"}, "run_draft_plan.source_run_wrong_project", "A Run of another project is refused");
    refused({"run-1", "no-such-run"}, "run_draft_plan.source_run_not_found", "An unknown Run is refused");
    refused({""}, "run_draft_plan.source_run_not_found", "An empty Run id is refused");
    refused({"run-1", "run-2", "run-1"}, "run_draft_plan.duplicate_source_run", "A Run named twice is refused");

    // The count is checked before any lookup.
    std::vector<RunId> many;
    for (int i = 0; i < 17; ++i) many.push_back("many-" + std::to_string(i));
    const int lookupsBefore = fx.runs.lookups;
    refused(many, "run_draft_plan.too_many_source_runs", "More than 16 source Runs are refused");
    require(fx.runs.lookups == lookupsBefore, "...before a single Run is looked up");

    // A storage failure of the lookup is reported as it is, never as "not found".
    fx.runs.failWith = ai::common::Error {"persistence.read_failed", "disk"};
    refused({"run-1"}, "persistence.read_failed", "A failing lookup is forwarded");
    fx.runs.failWith.reset();

    // The preview survived every refusal; the valid names are recorded in the order given.
    const auto created = fx.service.createFromPreview(token, std::nullopt, {"run-2", "run-1"});
    require(created.success() && !created.value().alreadyExisted
            && created.value().group.sourceRunIds == std::vector<RunId> {"run-2", "run-1"},
        "The named Runs are recorded on the group, in order");
    const auto stored = fx.drafts.findGroupById(created.value().group.id);
    require(stored.success() && stored.value().has_value() && stored.value()->sourceRunIds == std::vector<RunId> {"run-2", "run-1"},
        "...and stored");
    require(fx.draftCount() == 2, "Two drafts");

    // Exactly 16 are allowed (a manual import, so the message group above does not answer for it).
    for (int i = 1; i <= 16; ++i) fx.runs.runs["r" + std::to_string(i)] = fx.projectId;
    std::vector<RunId> sixteen;
    for (int i = 1; i <= 16; ++i) sixteen.push_back("r" + std::to_string(i));
    const auto manual = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(manual.success(), "setup: a paste preview");
    const auto big = fx.service.createFromPreview(manual.value().previewToken, std::nullopt, sixteen);
    require(big.success() && big.value().group.sourceRunIds == sixteen, "Sixteen source Runs are allowed");

    // None named: an empty list (the default).
    const auto none = fx.service.previewFromPaste(fx.conversationId, kPlan);
    require(none.success(), "setup: another paste preview");
    const auto plain = fx.service.createFromPreview(none.value().previewToken);
    require(plain.success() && plain.value().group.sourceRunIds.empty(), "Without source Runs the list is empty");
}

// PC-3D: what the source-Run picker offers.
void testSourceRunChoicesAreTheProjectsNewestRuns()
{
    Fixture fx;
    fx.addProject("project-2", "Other", ProjectStatus::Active);
    const auto addRun = [&](const std::string& id, const std::string& project, int seconds) {
        fx.runs.runs[id] = project;
        RunSourceChoice choice;
        choice.id = id;
        choice.title = "Run " + id;
        choice.status = RunStatus::Completed;
        choice.createdAt = kStart + std::chrono::seconds(seconds);
        fx.runs.choices[id] = choice;
    };
    for (int i = 0; i < 55; ++i) addRun("r" + std::to_string(1000 + i), fx.projectId, i);
    addRun("foreign", "project-2", 999);
    // Two Runs created in the same instant are ordered by id so the list never shuffles.
    addRun("tie-b", fx.projectId, 500);
    addRun("tie-a", fx.projectId, 500);

    const auto choices = fx.service.listSourceRunChoices(fx.conversationId);
    require(choices.success() && choices.value().size() == RunDraftPlanService::kMaxSourceRunChoices
            && RunDraftPlanService::kMaxSourceRunChoices == 50,
        "At most 50 Runs are offered");
    require(choices.value()[0].id == "tie-a" && choices.value()[1].id == "tie-b" && choices.value()[2].id == "r1054",
        "Newest first, ties by id");
    for (const auto& choice : choices.value()) {
        require(choice.id != "foreign", "A Run of another project is never offered");
    }
    require(choices.value().back().createdAt >= kStart + std::chrono::seconds(7),
        "The oldest ones are the ones cut off");

    // Everything the picker offers is accepted by the confirm.
    fx.planMessage();
    const std::vector<RunId> firstThree {choices.value()[0].id, choices.value()[1].id, choices.value()[2].id};
    require(confirmMessage(fx.service, fx.conversationId, "message-1", std::nullopt, firstThree).success(),
        "Offered Runs can be named");

    // Same authority rules as every other call: a conversation with no (active) project offers nothing.
    require(codeOf(fx.service.listSourceRunChoices("no-such-conversation")) == "run_draft_plan.conversation_not_found",
        "An unknown conversation");
    auto project = fx.projects.findById(fx.projectId).value().value();
    project.status = ProjectStatus::Archived;
    require(fx.projects.update(project).success(), "setup: archive the project");
    require(codeOf(fx.service.listSourceRunChoices(fx.conversationId)) == "run_draft_plan.project_inactive",
        "An archived project");
    project.status = ProjectStatus::Active;
    require(fx.projects.update(project).success(), "setup: reactivate");
    fx.runs.failWith = ai::common::Error {"persistence.read_failed", "disk"};
    require(codeOf(fx.service.listSourceRunChoices(fx.conversationId)) == "persistence.read_failed",
        "A storage failure is forwarded");
}

} // namespace

int main()
{
    try {
        testThePerMessageState();
        testAPreviewWritesNothing();
        testCreatingFromAMessageBuildsTheGroup();
        testOnlyTheActuallyAttachedSnapshotFingerprintReachesTheGroup();
        testAContextThatChangesBetweenPreviewAndConfirmIsStale();
        testCreatingIsIdempotent();
        testTheRosterTemplate();
        testTheSourceMustBeAFinishedAssistantMessageOfTheConversation();
        testTheProjectComesFromTheConversationAndMustBeActive();
        testBadPlansFailWithTheirOwnCodes();
        testAManualPasteIsPreviewedThenConfirmedByToken();
        testPreviewsExpireAndAreBounded();
        testAPreviewIsRecheckedAgainstTheProjectAtConfirm();
        testTemplateChoicesBelongToTheConversationsProject();
        testAManualImportTakesARosterTemplateToo();
        testTheSameMessageInAnotherProjectIsAnotherGroup();
        testTheLoserOfARaceGetsTheWinnersGroup();
        testAMessageIsConfirmedOnlyThroughItsPreview();
        testARepeatIsAnsweredFromTheStoredGroup();
        testSourceRunsAreCheckedAgainstTheProjectAndRecorded();
        testSourceRunChoicesAreTheProjectsNewestRuns();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
