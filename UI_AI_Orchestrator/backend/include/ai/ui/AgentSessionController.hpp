#pragma once

#include "ai/ui/AgentProposalListModel.hpp"
#include "ai/ui/AgentToolCallListModel.hpp"
#include "ai/ui/AgentTurnListModel.hpp"

#include <ai/application/AgentToolReadModelService.hpp>
#include <ai/application/ChangeProposalService.hpp>
#include <ai/application/ChangeSetApplyService.hpp>
#include <ai/application/ManifestApplyVerificationGate.hpp>
#include <ai/application/ProposalReviewService.hpp>
#include <ai/application/ProposalReviewTypes.hpp>

#include <QFutureWatcher>
#include <QObject>
#include <QVariantMap>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

/**
 * Qt read-model adapter for AgentToolReadModelService: the active tool
 * session, its provider turns, its audited tool calls, and its change
 * proposals (with validation findings and, on demand, original/proposed
 * file content for a diff preview).
 *
 * Safety boundary (matches AgentToolReadModelService's guarantee): nothing
 * this controller exposes to QML is an absolute filesystem path, a raw tool
 * argument/result payload, a secret, or a filesystem handle. File content
 * exposed by readOriginalContent()/readProposedContent() is plain UTF-8
 * text keyed by (changeSetId, changeId) - QML never receives or constructs
 * a path to reach it.
 *
 * Pull-based by design: there is no dedicated agent-session execution
 * event stream yet (unlike ComparisonExecutionManager), so this controller
 * is refreshed explicitly - selectRun()/refresh() - rather than pushed to.
 * RunsPage re-selects the run whenever RunDetailsPanel's own runData
 * changes, which is frequent enough to track an in-progress session.
 */
class AgentSessionController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* turnsModel READ turnsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* toolCallsModel READ toolCallsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* proposalsModel READ proposalsModel CONSTANT)
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY stateChanged)
    Q_PROPERTY(QString selectedSessionId READ selectedSessionId NOTIFY stateChanged)
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY stateChanged)
    Q_PROPERTY(QString sessionState READ sessionState NOTIFY stateChanged)
    Q_PROPERTY(QString sessionStartedText READ sessionStartedText NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedProposalDetail READ selectedProposalDetail NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString applyResultText READ applyResultText NOTIFY stateChanged)

public:
    // verificationGate is AG-16.1's read-only, authoritative apply-eligibility
    // evaluator - the exact same one ChangeSetApplyService itself re-checks
    // immediately before every workspace mutation. Required (not optional):
    // this controller's selectedProposalDetail always projects a real
    // Backend decision, never a UI-computed guess - see selectProposal()'s
    // own doc comment.
    AgentSessionController(
        ai::application::AgentToolReadModelService& readModel,
        ai::application::ChangeProposalService& proposals,
        ai::application::ChangeSetApplyService& applyService,
        ai::application::ProposalReviewService& reviewService,
        ai::application::ManifestApplyVerificationGate& verificationGate,
        QObject* parent = nullptr);
    ~AgentSessionController() override;

    [[nodiscard]] QAbstractItemModel* turnsModel() noexcept { return &turns_; }
    [[nodiscard]] QAbstractItemModel* toolCallsModel() noexcept { return &toolCalls_; }
    [[nodiscard]] QAbstractItemModel* proposalsModel() noexcept { return &proposals_; }
    [[nodiscard]] QString selectedRunId() const { return selectedRunId_; }
    [[nodiscard]] QString selectedSessionId() const { return selectedSessionId_; }
    [[nodiscard]] bool hasSession() const noexcept { return current_.has_value(); }
    [[nodiscard]] QString sessionState() const;
    [[nodiscard]] QString sessionStartedText() const;
    [[nodiscard]] QVariantMap selectedProposalDetail() const { return selectedProposalDetail_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] QString errorCode() const { return errorCode_; }
    [[nodiscard]] QString errorText() const { return errorText_; }
    [[nodiscard]] QString applyResultText() const { return applyResultText_; }

    // Loads the most recent agent tool session for a run (if any). Clears
    // the current session (no error) when the run has none - most runs
    // never start one.
    Q_INVOKABLE bool selectRun(const QString& runId);
    Q_INVOKABLE bool refresh();
    // Populates selectedProposalDetail with the chosen proposal's structural
    // file-change list and validation findings, plus (AG-16.2) the
    // authoritative apply-eligibility projection: applyAllowed (bool),
    // applyBlockCode/applyBlockMessage (the exact reasonCode/reasonMessage
    // ManifestApplyVerificationGate::evaluate() returned - always set, even
    // when applyAllowed is true, so a caller never has to infer "why" from
    // an empty string). This is a read-only Backend decision, re-derived
    // fresh on every call - QML must never compute or cache its own
    // eligibility. Pass an empty string to clear the selection.
    Q_INVOKABLE bool selectProposal(const QString& changeSetId);
    // Lazily fetches one file's original/proposed content for a diff
    // preview. Returns an empty string on failure; check errorText.
    Q_INVOKABLE QString readOriginalContent(const QString& changeSetId, const QString& changeId);
    Q_INVOKABLE QString readProposedContent(const QString& changeSetId, const QString& changeId);
    // Applies all changes in the proposal to the real workspace at
    // projectRootPath (absolute path supplied by the trusted backend caller).
    // On success, reloads the session to reflect the Applied state. Requires
    // a prior approveAndApply()/ChangeSetApplyService::approve() call - see
    // that method - kept as a distinct entry point for callers that already
    // hold a valid approval (e.g. a retry) rather than needing a fresh one.
    // Refuses with "proposal.rejected_by_reviewer" (via ProposalReviewService,
    // checked before ChangeSetApplyService::apply()) if a human already
    // rejected this proposal, even if it was approved earlier - see AG-07.
    Q_INVOKABLE bool applyProposal(const QString& changeSetId);
    // UI-facing "Approve and apply": records a durable approval for this
    // exact, currently ready-for-review snapshot (best-effort OS username
    // attached, not an authenticated identity), then applies it. This is
    // what the confirmation dialog's confirm button calls. Same
    // reviewed-rejection refusal as applyProposal() above, checked first.
    Q_INVOKABLE bool approveAndApply(const QString& changeSetId);
    // UI-facing "Reject/discard": records a durable human rejection distinct
    // from validator ChangeSetState::Rejected (AG-07). reason is optional -
    // pass an empty string for none. Idempotent (rejecting an
    // already-rejected proposal succeeds and returns the existing record).
    // Fails "proposal.not_ready_for_review" outside ReadyForReview and
    // "proposal.review_reason_too_large" for an over-limit reason - see
    // ProposalReviewService::reject(). Reloads the session on success so the
    // rejection appears immediately in proposalsModel/selectedProposalDetail.
    Q_INVOKABLE bool rejectProposal(const QString& changeSetId, const QString& reason);

signals:
    void stateChanged();
    void errorOccurred(const QString& code, const QString& message);

private:
    struct SessionLoadRequest {
        std::uint64_t requestId {0};
        std::string runId;
        std::optional<std::string> sessionId;
    };

    struct SessionLoadOutcome {
        std::uint64_t requestId {0};
        std::string runId;
        std::optional<ai::application::AgentToolSessionReadModel> projection;
        // Best-effort AG-07 enrichment keyed by changeSetId, populated on the
        // same background load as projection so no separate GUI-thread N+1
        // query is needed. A changeSetId missing from this map after a
        // successful load simply has no reviewer rejection - see
        // finishLoad()/startPendingLoad().
        std::unordered_map<std::string, ai::application::ProposalReviewRejection> rejections;
        std::optional<ai::common::Error> error;
    };

    // Database-backed read projections run outside the GUI thread. Only the
    // immutable outcome is applied to the Qt models after it returns.
    bool queueLoad(bool rediscoverSession);
    void startPendingLoad();
    void finishLoad();
    void clearSession();
    bool fail(const ai::common::Error& error);
    void clearError();
    void rebuildSelectedProposalDetailAfterFailure(const QString& changeSetId);
    // AG-16.2: the single place that builds selectedProposalDetail_ from an
    // already-loaded current_->proposals entry, including the
    // ManifestApplyVerificationGate projection - shared by selectProposal()
    // (a fresh GUI-thread selection) and finishLoad() (re-deriving the same
    // detail for whatever was selected before a background reload, so a
    // post-apply/post-reject/post-RunEvent refresh keeps the panel showing
    // live state in place instead of silently blanking it). Returns false
    // (and clears selectedProposalDetail_) if changeSetId is no longer
    // present in current_->proposals.
    bool rebuildSelectedProposalDetail(const QString& changeSetId);

    ai::application::AgentToolReadModelService& readModel_;
    ai::application::ChangeProposalService& proposals_service_;
    ai::application::ChangeSetApplyService& applyService_;
    ai::application::ProposalReviewService& reviewService_;
    ai::application::ManifestApplyVerificationGate& verificationGate_;
    AgentTurnListModel turns_;
    AgentToolCallListModel toolCalls_;
    AgentProposalListModel proposals_;
    QString selectedRunId_;
    QString selectedSessionId_;
    QString errorCode_;
    QString errorText_;
    QString applyResultText_;
    bool busy_ {false};
    bool loadInFlight_ {false};
    std::uint64_t nextRequestId_ {0};
    std::uint64_t latestRequestId_ {0};
    std::optional<SessionLoadRequest> pendingLoad_;
    QFutureWatcher<SessionLoadOutcome> loadWatcher_;
    QVariantMap selectedProposalDetail_;
    // AG-16.2: the changeSetId selectedProposalDetail_ was last built for
    // (empty when nothing is selected) - lets finishLoad() re-derive the
    // same detail after a background reload instead of unconditionally
    // blanking the panel. Kept in sync with selectedProposalDetail_ by
    // rebuildSelectedProposalDetail() alone.
    QString selectedChangeSetId_;
    std::optional<ai::application::AgentToolSessionReadModel> current_;
};
