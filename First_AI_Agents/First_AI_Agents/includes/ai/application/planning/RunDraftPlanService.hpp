#pragma once

#include "ai/application/conversations/ChatTypes.hpp"
#include "ai/application/conversations/IChatRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/planning/IRunDraftRepository.hpp"
#include "ai/application/execution/IRunSourceLookup.hpp"
#include "ai/application/planning/NextRunPlan.hpp"
#include "ai/application/planning/RunDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// PC-3C: the Prompt Workshop's Backend. It turns a `next-run-plan.v1` plan - read from an Assistant message or
// pasted by hand - into a group of reviewed Run Drafts. Provider-free by construction: it holds only the Draft,
// Project and Chat repositories and a read-only "which project does this Run belong to" lookup, so it cannot call a
// model, start a Run or touch a Run (no RunManager, no Run writes). Drafts are written through
// IRunDraftRepository::insertGroup only; nothing is queued.
//
// ONE way to write, for both sources: preview, then confirm with the opaque token the preview returned.
//  - previewFromMessage / previewFromPaste validate the plan, write nothing and keep the validated plan under a token
//    in a small, bounded, expiring store (16 entries, 30 minutes, oldest dropped first). A message preview also
//    remembers the sha256 of the message text it was made from.
//  - createFromPreview is the only write. It confirms exactly the plan the user was shown: for a chat message it
//    first re-reads the message and refuses (`run_draft_plan.stale_preview`) when its text is no longer the one the
//    preview was made from, so an edited or regenerated reply can never be saved unseen. The Backend never creates
//    drafts from a message without a preview it issued.
//  - Idempotency (project, source) is decided on the confirm side: a message group is keyed by (project, message id,
//    message text, plan), so a SECOND preview of the unchanged message and confirming it returns the same group; a
//    manual group is keyed by (project, preview token), so confirming the same token twice returns the same group
//    while a NEW preview of the same paste deliberately makes a separate copy.
//  - A repeat is answered BEFORE anything else mutable is looked at: once the source (token, conversation, project,
//    message text) checks out and the key names a stored group, that group is returned as is. The roster template and
//    the source Runs of the repeat are not part of the identity and are not even loaded, so a template that was
//    deleted or edited since can never turn a repeated confirm into an error. They are validated only before a NEW
//    insert, and the unique key still resolves a race between two confirms.
//
// Project isolation: the project always comes from the conversation (it must be bound to one and the project must be
// Active); a roster template and every source Run must belong to that same project.

// What a preview shows. Nothing was written.
struct RunDraftPlanPreview {
    ProjectId projectId;
    ConversationId conversationId;
    RunDraftGroupOrigin origin {RunDraftGroupOrigin::ManualImport};
    NextRunPlan plan;                                // validated, normalized, fingerprinted; its warnings included
    std::string previewToken;                        // ALWAYS set: the only way to confirm this exact preview
    std::optional<ChatMessageId> messageId;          // message path only
    // Set when the same source already produced a group that still has drafts: confirming would return it unchanged.
    std::optional<RunDraftGroupId> existingGroupId;
};

struct RunDraftPlanCreation {
    RunDraftGroup group;
    std::vector<RunDraft> drafts;   // ordinal order
    // True when the same source had already produced this group: nothing was written and no roster was touched.
    bool alreadyExisted {false};
};

// What the per-message action needs to know, computed from the stored message and never by QML: `None` unless the
// message is a completed (Sent) Assistant message; otherwise the envelope state of its text.
[[nodiscard]] NextRunPlanEnvelopeState planEnvelopeStateOf(const ChatMessage& message);

class RunDraftPlanService final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    // Preview tokens expire after this long and at most this many are kept (the oldest is dropped first).
    static constexpr std::chrono::minutes kPreviewLifetime {30};
    static constexpr std::size_t kMaxPreviews = 16;

    // `runs` must outlive the service; the service only asks it for the project of a Run.
    RunDraftPlanService(IRunDraftRepository& drafts, IProjectRepository& projects, IChatRepository& chats,
        const IRunSourceLookup& runs, Clock clock = {});

    // Failure codes (all `run_draft_plan.*` unless noted): conversation_not_found, conversation_without_project,
    // project_not_found, project_inactive, message_not_found, message_not_eligible (not a completed Assistant
    // message of that conversation), stale_preview (the message text changed since the preview), template_not_found,
    // template_wrong_project, template_not_manifest_roster, template_without_roster, source_run_not_found,
    // source_run_wrong_project, duplicate_source_run, too_many_source_runs, preview_not_found, preview_expired,
    // project_changed (the conversation moved to another project between the preview and the confirm),
    // context_project_mismatch (the source response was generated with another project's attached snapshot). The plan
    // itself fails with the `next_run_plan.*` and `run_contracts.*` codes of parseNextRunPlan / the envelope
    // extraction; the write with `run_draft_group.*`.
    [[nodiscard]] common::Result<RunDraftPlanPreview> previewFromMessage(
        const ConversationId& conversationId, const ChatMessageId& messageId);
    [[nodiscard]] common::Result<RunDraftPlanPreview> previewFromPaste(
        const ConversationId& conversationId, std::string_view pastedText);

    // The only write. `rosterTemplate`: an explicitly chosen draft of the same project to copy the roster,
    // maxParallelism, provider limits and verificationRepairPolicy from - nothing else (never workspace, project,
    // objective, contracts or execution profile); without one the drafts get an empty roster. `sourceRunIds`: Runs
    // the user names as the source of the plan (at most kRunDraftGroupMaxSourceRuns, each once, each of the
    // conversation's project); they are recorded on the group and never taken from the plan. Neither is part of the
    // identity: see the class comment for what a repeat does with them.
    common::Result<RunDraftPlanCreation> createFromPreview(const std::string& previewToken,
        const std::optional<RunDraftId>& rosterTemplate = std::nullopt, const std::vector<RunId>& sourceRunIds = {});

    // The drafts of the conversation's project that can serve as a roster template: per-Run Manifest roster drafts
    // with a non-empty roster, in the drafts' usual "recent first" order.
    [[nodiscard]] common::Result<std::vector<RunDraft>> listRosterTemplates(const ConversationId& conversationId) const;

    // The Runs of the conversation's project the user may name as the source of a plan: newest first, at most
    // kMaxSourceRunChoices. Read-only; the picker offers nothing the confirm would refuse for its project.
    static constexpr std::size_t kMaxSourceRunChoices = 50;
    [[nodiscard]] common::Result<std::vector<RunSourceChoice>> listSourceRunChoices(
        const ConversationId& conversationId) const;

private:
    struct StoredPreview {
        ProjectId projectId;
        ConversationId conversationId;
        RunDraftGroupOrigin origin {RunDraftGroupOrigin::ManualImport};
        std::optional<ChatMessageId> messageId;                  // message path only
        std::optional<std::string> messageContentFingerprint;    // message path only: sha256 of the text previewed
        std::optional<std::string> contextFingerprint;           // message path only: exact attached PC-1 identity
        NextRunPlan plan;
        std::chrono::system_clock::time_point createdAt;
    };

    [[nodiscard]] std::chrono::system_clock::time_point now() const;
    void prunePreviewsLocked();
    // Stores the preview and returns its new token.
    [[nodiscard]] std::string rememberPreview(StoredPreview stored);

    IRunDraftRepository& drafts_;
    IProjectRepository& projects_;
    IChatRepository& chats_;
    const IRunSourceLookup& runs_;
    Clock clock_;

    mutable std::mutex mutex_;
    std::map<std::string, StoredPreview> previews_;
};

} // namespace ai::application
