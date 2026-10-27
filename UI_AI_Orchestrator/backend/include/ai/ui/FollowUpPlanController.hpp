#pragma once

#include <ai/application/planning/FollowUpPlanReadModel.hpp>
#include <ai/application/planning/FollowUpPlanService.hpp>

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <vector>

/**
 * PC-5D: the Qt boundary of the opt-in paid next-Run composer (`Plan next Run...` / `Plan recovery...`).
 *
 * It copies what FollowUpPlanService and FollowUpPlanReadModel say into QVariantMaps and relays the user's explicit commands; it
 * classifies nothing (the gate, the free-or-possibly-charged rule, the state, the quote and every refusal are the Backend's) and it
 * can neither start a Run nor call a provider: the one paid call is made by the Backend's own worker, only for a request the user
 * authorised, and this controller merely asks to authorise. Nothing here writes a Run, a proposal, an Apply, the Work Ledger or an
 * Outcome Brief. The only things it can cause to be written are the request rows and, through the atomic Confirm, ONE Draft group.
 *
 * QML receives METADATA only: the Backend's words for a state, paths, sizes, token counts, flags, costs, fingerprints - never the
 * content of a file, never an absolute path. The one piece of provider-authored text it can read is a stored reply
 * (storedReply(), shown read-only and selectable) and the plan of a `plan_ready` request (preview(), the same shape as the Prompt
 * Workshop's preview, rendered by the same dialog).
 *
 * Asynchronous: describeFiles(), requestQuote() and authorize() re-read the source Run's workspace (hash and read files) and take the
 * shared database mutex, so each runs on a worker thread, ONE AT A TIME (an accepted call returns its id, a refused one 0 and a
 * reason in lastError). The worker touches no QObject or QVariant: the projection is built on the UI thread when it finishes. The
 * result arrives through operationFinished(id, operation, result); `result.ok == false` carries errorCode / errorMessage. The
 * destructor waits for a running call (none of these reads can be interrupted); the services must outlive this controller.
 *
 * Synchronous (short local SQLite reads or ONE transaction, like the Prompt Workshop): the status of the selected Run, the picker
 * data, cancel, discard, preview, storedReply and confirm.
 *
 * `status` is always a complete map:
 *   availability: "not_applicable" (no current Outcome Brief: show nothing) | "evaluated" | "unavailable" (a fault, errorCode /
 *                 errorMessage; also a guest runtime: `follow_up.not_the_owner`)
 *   runId, projectId,
 *   nextRun / recovery: {offered, allowed, reasonCode, reasonMessage}   (the Backend's gate, plus `follow_up.request_open`)
 *   request: the newest request of the Run (see requestMap) or an empty map, and `requestPresent`.
 * requestMap: {id, projectId, sourceRunId, kind, state, open, terminal, mayBeCharged, goal, maxDrafts, splitHint, createdAt,
 *   quoteExpiresAt, authorizedAt, claimedAt, finishedAt, quote:{providerId, providerKind, baseUrl, modelId, apiModelId, promptCache,
 *   estimatedInputTokens, maxOutputTokens, worstCaseCostMicros, capMicros, priceKnown, contextFingerprint}, files:[{path,
 *   sha256Short, originalBytes, shownBytes, estimatedTokens, truncated}], failure:{code, message, hasStoredReply} (failed only),
 *   accounting:{inputTokens, outputTokens, costMicros, priceKnown} (when a response arrived), draftGroupId, planFingerprint,
 *   shortPlanFingerprint}.
 * While the newest request is `authorized` or `composing` the status is re-read every kPollMs; it is never polled otherwise.
 */
class FollowUpPlanController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyOperation READ busyOperation NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    static constexpr int kPollMs = 1000;
    static constexpr int kSelectionReadDelayMs = 200;

    FollowUpPlanController(ai::application::FollowUpPlanService& service,
        const ai::application::FollowUpPlanReadModel& readModel, QObject* parent = nullptr);
    ~FollowUpPlanController() override;

    [[nodiscard]] QString selectedRunId() const { return selectedRunId_; }
    [[nodiscard]] QVariantMap status() const { return status_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] QString busyOperation() const { return busyOperation_; }
    [[nodiscard]] QString lastError() const { return lastError_; }

    // Selects a Run (empty = none) and reads its status now.
    Q_INVOKABLE void selectRun(const QString& runId);
    // Selects a Run WITHOUT touching the database now (the previous Run's status is dropped at once; the read follows after
    // kSelectionReadDelayMs if that Run is still the selected one), like OutcomeBriefController::selectRunSoon.
    Q_INVOKABLE void selectRunSoon(const QString& runId);
    // Re-reads the selected Run's status now. statusChanged() is emitted only when what is shown actually changed.
    Q_INVOKABLE void refresh();

    // {ok, models:[{providerId, providerName, providerKind, modelId, displayName, usable, reasonCode, contextWindowTokens,
    //  maxOutputTokens, inputPriceMicrosPerMillionTokens, outputPriceMicrosPerMillionTokens, hasCredentialReference}]} or
    // {ok:false, errorCode, errorMessage}. A failure is never an empty list.
    Q_INVOKABLE QVariantMap modelChoices();
    // {ok, templates:[{draftId, name, title, rosterSize, maxParallelism}]} / {ok, runs:[{runId, title, status, createdAt}]}.
    Q_INVOKABLE QVariantMap rosterTemplates(const QString& projectId);
    // `requiredRunId` is the immutable source of a follow-up plan. It is always returned in the bounded picker so QML can show it
    // selected and locked even when it is older than the newest 50 Runs.
    Q_INVOKABLE QVariantMap sourceRunChoices(const QString& projectId, const QString& requiredRunId);

    // The candidate files of the source Run and what happens to each by default (the admission gate, minus the model).
    // Result: {ok, files:[{path, safety, disposition, eligible, includedByDefault, sha256Short, originalBytes, shownBytes,
    // estimatedTokens, truncated}]}.
    Q_INVOKABLE quint64 describeFiles(const QString& projectId, const QString& sourceRunId, const QString& kind);
    // ONE immutable quote; stores a `quoted` request (replacing the Run's earlier `quoted` one). `intent`: {projectId,
    // sourceRunId, kind ("next_run" | "recovery"), goal, maxDrafts, splitHint ("one" | "many" | "auto"), providerId, modelId,
    // capMicros (absent = the system ceiling), selectedPaths (absent = every Eligible candidate)}. Nothing is sent. Result: {ok,
    // request, replacedQuoteId, files}.
    Q_INVOKABLE quint64 requestQuote(const QVariantMap& intent);
    // quoted -> authorized after the admission checks ran again; the Backend's worker then makes the ONE paid call. This is the
    // only command that can lead to a charge, and it returns before the call is made. Result: {ok, request}.
    Q_INVOKABLE quint64 authorize(const QString& requestId);

    // quoted | authorized -> cancelled (free); composing -> asks the running call to stop (it becomes cancelled, possibly charged,
    // when it returns). {ok, request} or {ok:false, errorCode, errorMessage}.
    Q_INVOKABLE QVariantMap cancel(const QString& requestId);
    // quoted -> discarded (free); plan_ready -> discarded (the paid plan is dropped).
    Q_INVOKABLE QVariantMap discard(const QString& requestId);

    // The stored plan of a plan_ready / drafts_saved request in the shape of the Prompt Workshop's preview, with `previewToken` =
    // the request id (an opaque handle to confirm()), origin "follow_up_request", sourceRunId, the plan's own warnings AND the
    // request's advisories merged into `warnings` ([{code, message}]), `existingGroupId` for a request that already saved its group,
    // `request` (requestMap) and `planFingerprint`.
    Q_INVOKABLE QVariantMap preview(const QString& requestId);
    // The stored reply of a failed request's invalid answer, as text, for read-only display: {ok, requestId, code, message, reply}.
    Q_INVOKABLE QVariantMap storedReply(const QString& requestId);
    // The explicit Confirm: ONE atomic operation saving the Draft group of the stored plan. `planFingerprint` is the plan the user
    // was shown (preview()). `rosterTemplateDraftId` empty = an empty roster. `extraSourceRunIds` ADD advisory source Runs; the
    // request's own Run is always one of the group's. A repeat returns the same group and writes nothing. Result: the Prompt
    // Workshop's creation map {ok, alreadyExisted, groupId, projectId, origin, planTitle, planFingerprint, sourceRunIds[],
    // drafts:[{draftId, ordinal, planKey, title, dependsOn[], contractCount, rosterSize}]}. `applySuggestedParallelism` (default false) copies
    // the plan's suggested parallelism into the Drafts that carry a staffing hint, after the template's value; nothing else of the hint.
    Q_INVOKABLE QVariantMap confirm(const QString& requestId, const QString& planFingerprint,
        const QString& rosterTemplateDraftId = {}, const QStringList& extraSourceRunIds = {}, bool applySuggestedParallelism = false);

    Q_INVOKABLE void clearError();

    // The pure copy of a request into the map QML receives. Public so that a test can pin the whole shape.
    [[nodiscard]] static QVariantMap requestMap(const ai::application::FollowUpPlanRequest& request);
    // The pure copy of the read model's view into `status`.
    [[nodiscard]] static QVariantMap statusMap(const ai::application::FollowUpRunEligibility& view);

signals:
    void statusChanged();
    void busyChanged();
    void lastErrorChanged();
    void operationFailed(const QString& code, const QString& message);
    // The result of describeFiles / requestQuote / authorize (`operation` is that name).
    void operationFinished(quint64 operationId, const QString& operation, const QVariantMap& result);
    // Drafts were written by a confirm (not emitted for a repeated confirm that found the group already there).
    void draftsCreated(const QString& groupId, int draftCount);

private:
    struct AsyncOutcome {
        quint64 id {0};
        QString operation;
        std::optional<ai::common::Error> error;
        std::vector<ai::application::FollowUpFileView> files;
        std::optional<ai::application::FollowUpQuoteOutcome> quote;
        std::optional<ai::application::FollowUpPlanRequest> request;
    };

    [[nodiscard]] QVariantMap fail(const ai::common::Error& error);
    void reportError(const ai::common::Error& error);
    [[nodiscard]] quint64 refuse(const QString& code, const QString& message);
    [[nodiscard]] quint64 startAsync(const QString& operation, std::function<void(AsyncOutcome&)> work);
    void finishAsync();
    void setBusy(bool value, const QString& operation);
    void applyStatus(QVariantMap status);
    void updatePolling();
    [[nodiscard]] QVariantMap readStatus(const QString& runId);

    ai::application::FollowUpPlanService& service_;
    const ai::application::FollowUpPlanReadModel& readModel_;
    QString selectedRunId_;
    QVariantMap status_;
    bool busy_ {false};
    QString busyOperation_;
    QString lastError_;
    quint64 nextOperationId_ {0};
    QFutureWatcher<AsyncOutcome> watcher_;
    QTimer poll_;
    QTimer selectionRead_;
};
