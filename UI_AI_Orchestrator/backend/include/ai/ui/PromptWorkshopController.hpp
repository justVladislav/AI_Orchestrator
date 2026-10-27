#pragma once

#include <ai/application/planning/RunDraftPlanService.hpp>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

/**
 * PC-3C: the Qt boundary of the Prompt Workshop - next-run plan -> reviewed Run Draft groups.
 *
 * Everything here is a thin, synchronous projection of RunDraftPlanService (local SQLite reads and one short
 * transaction; no provider, no Run, no workspace access), so the calls follow RunDraftController: a Q_INVOKABLE
 * returns a QVariantMap {ok, ...} and a failure also sets lastError and emits operationFailed(code, message).
 * QML parses, validates and trims nothing: the plan text goes to the Backend as-is and comes back as typed fields.
 *
 * Rules, all pinned by tests:
 *  - a preview writes nothing; a create writes Drafts only and never starts, queues or creates a Run;
 *  - ONE way to write: preview (from a message or a paste), then createFromPreview with the opaque `previewToken`
 *    the preview returned. The UI cannot swap the plan between the preview and the write, and a chat message whose
 *    text changed after the preview is refused (`run_draft_plan.stale_preview`) instead of saved unseen;
 *  - the project always comes from the conversation (never an argument), and a roster template and every source
 *    Run must belong to it;
 *  - `draftsCreated` is emitted only when drafts were actually written, never for a repeated create that returned
 *    the existing group; BackendBridge uses it to refresh the Drafts list.
 *
 * Preview map: {ok, projectId, conversationId, origin ("chat_message" | "manual_import"), previewToken (always
 * set), messageId, existingGroupId, planTitle, planFingerprint, shortFingerprint, sharedContractCount,
 * drafts: [{key, title, task, dependsOn[], sharedContractIds[], sharedContractCount, contracts[]}],
 * warnings: [{code, message}]}.
 * Creation map: {ok, alreadyExisted, groupId, projectId, origin, planTitle, planFingerprint, sourceRunIds[], drafts:
 * [{draftId, ordinal, planKey, title, dependsOn[], contractCount, rosterSize}]}.
 */
class PromptWorkshopController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit PromptWorkshopController(ai::application::RunDraftPlanService& service, QObject* parent = nullptr);

    [[nodiscard]] QString lastError() const { return lastError_; }

    // Writes nothing. `messageId` must be a completed Assistant message of `conversationId`.
    Q_INVOKABLE QVariantMap previewFromMessage(const QString& conversationId, const QString& messageId);
    // Writes nothing but a small bounded preview store entry; the returned `previewToken` confirms it.
    Q_INVOKABLE QVariantMap previewFromPaste(const QString& conversationId, const QString& pastedText);

    // The only write, for both sources. `rosterTemplateDraftId` empty = drafts with an empty roster; otherwise a
    // same-project roster draft to copy the roster, maxParallelism, provider limits and repair policy from.
    // `sourceRunIds`: Runs of the same project the user names as the source of the plan (at most 16, each once).
    // A repeated confirm returns the stored group without looking at the template or the source Runs.
    Q_INVOKABLE QVariantMap createFromPreview(const QString& previewToken,
        const QString& rosterTemplateDraftId = {}, const QStringList& sourceRunIds = {});

    // {ok: true, templates: [{draftId, name, title, rosterSize, maxParallelism}]} for the conversation's project, or
    // {ok: false, errorCode, errorMessage}. A failure is never an empty list: "no templates" and "the templates could
    // not be read" are different answers.
    Q_INVOKABLE QVariantMap rosterTemplates(const QString& conversationId);

    // {ok: true, runs: [{runId, title, status, createdAt}]} - the newest Runs (at most 50) of the conversation's
    // project that can be named as the source of a plan - or {ok: false, errorCode, errorMessage}. Read-only.
    Q_INVOKABLE QVariantMap sourceRunChoices(const QString& conversationId);

    Q_INVOKABLE void clearError();

signals:
    void lastErrorChanged();
    void operationFailed(const QString& code, const QString& message);
    // Drafts were written (not emitted for a repeated create that found the group already there).
    void draftsCreated(const QString& groupId, int draftCount);

private:
    [[nodiscard]] QVariantMap fail(const ai::common::Error& error);
    void reportError(const ai::common::Error& error);

    ai::application::RunDraftPlanService& service_;
    QString lastError_;
};
