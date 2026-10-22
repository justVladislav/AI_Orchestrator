#include "ai/ui/AgentSessionController.hpp"

#include <ai/application/ChangeProposalTypes.hpp>
#include <filesystem>

#include <QDateTime>
#include <QLocale>
#include <QVariantList>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {

QString sessionStateText(ai::application::AgentToolSessionState state)
{
    switch (state) {
    case ai::application::AgentToolSessionState::Active: return QStringLiteral("active");
    case ai::application::AgentToolSessionState::Completed: return QStringLiteral("completed");
    case ai::application::AgentToolSessionState::Interrupted: return QStringLiteral("interrupted");
    }
    return QStringLiteral("unknown");
}

} // namespace

AgentSessionController::AgentSessionController(
    ai::application::AgentToolReadModelService& readModel,
    ai::application::ChangeProposalService& proposals,
    ai::application::ChangeSetApplyService& applyService,
    ai::application::ProposalReviewService& reviewService,
    ai::application::ManifestApplyVerificationGate& verificationGate,
    QObject* parent)
    : QObject(parent)
    , readModel_(readModel)
    , proposals_service_(proposals)
    , applyService_(applyService)
    , reviewService_(reviewService)
    , verificationGate_(verificationGate)
    , turns_(this)
    , toolCalls_(this)
    , proposals_(this)
{
    connect(
        &loadWatcher_,
        &QFutureWatcher<SessionLoadOutcome>::finished,
        this,
        &AgentSessionController::finishLoad);
}

AgentSessionController::~AgentSessionController()
{
    // The background task captures only the Application service reference.
    // BackendBridge destroys controllers before BackendRuntime, but waiting
    // here also makes that lifetime dependency explicit and race-free.
    if (loadInFlight_) {
        loadWatcher_.future().waitForFinished();
    }
}

QString AgentSessionController::sessionState() const
{
    return current_.has_value() ? sessionStateText(current_->session.state) : QString();
}

QString AgentSessionController::sessionStartedText() const
{
    if (!current_.has_value()) return {};
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        current_->session.createdAt.time_since_epoch());
    return QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC).toLocalTime(),
        QLocale::ShortFormat);
}

bool AgentSessionController::selectRun(const QString& runId)
{
    selectedRunId_ = runId;
    clearSession();

    if (runId.isEmpty()) {
        // Invalidate an already-running request. QtConcurrent cannot cancel a
        // native SQLite call, so its result is simply discarded on return.
        latestRequestId_ = ++nextRequestId_;
        pendingLoad_.reset();
        busy_ = false;
        clearError();
        emit stateChanged();
        return true;
    }
    return queueLoad(true);
}

bool AgentSessionController::refresh()
{
    if (selectedRunId_.isEmpty()) return true;
    return queueLoad(selectedSessionId_.isEmpty());
}

bool AgentSessionController::queueLoad(bool rediscoverSession)
{
    SessionLoadRequest request;
    request.requestId = ++nextRequestId_;
    request.runId = selectedRunId_.toStdString();
    if (!rediscoverSession && !selectedSessionId_.isEmpty()) {
        request.sessionId = selectedSessionId_.toStdString();
    }

    latestRequestId_ = request.requestId;
    // Coalesce any number of refresh signals while SQLite is busy. Keeping
    // only the newest request prevents an event burst from building a queue
    // of stale full-session projections.
    pendingLoad_ = std::move(request);
    if (!busy_) {
        busy_ = true;
        emit stateChanged();
    }
    if (!loadInFlight_) {
        startPendingLoad();
    }
    return true;
}

void AgentSessionController::startPendingLoad()
{
    if (loadInFlight_ || !pendingLoad_.has_value()) return;

    SessionLoadRequest request = std::move(*pendingLoad_);
    pendingLoad_.reset();
    loadInFlight_ = true;
    auto* readModel = &readModel_;
    auto* reviewService = &reviewService_;

    loadWatcher_.setFuture(QtConcurrent::run(
        [readModel, reviewService, request = std::move(request)]() mutable {
            SessionLoadOutcome outcome;
            outcome.requestId = request.requestId;
            outcome.runId = request.runId;

            std::string sessionId;
            if (request.sessionId.has_value()) {
                sessionId = *request.sessionId;
            } else {
                const auto sessions = readModel->listForRun(request.runId);
                if (!sessions.success()) {
                    outcome.error = *sessions.error();
                    return outcome;
                }
                // No agent tool session is a valid empty projection. The
                // next RunEvent will schedule another discovery request.
                if (sessions.value().empty()) return outcome;
                sessionId = sessions.value().front().sessionId;
            }

            auto projection = readModel->session(sessionId);
            if (!projection.success()) {
                outcome.error = *projection.error();
                return outcome;
            }
            outcome.projection = std::move(projection.value());

            // AG-07 reviewer-rejection enrichment, on the same background
            // load as the projection above so the GUI thread never issues
            // one query per proposal. Best-effort: a lookup failure for one
            // proposal just leaves it absent from the map (no rejection
            // shown) rather than failing the whole session load - the
            // reviewer-rejection overlay is supplementary to the session
            // itself, not required for the panel to be useful.
            for (const auto& proposal : outcome.projection->proposals) {
                const auto rejection = reviewService->findRejection(proposal.header.changeSetId);
                if (rejection.success() && rejection.value().has_value()) {
                    outcome.rejections.emplace(
                        proposal.header.changeSetId, *rejection.value());
                }
            }
            return outcome;
        }));
}

void AgentSessionController::finishLoad()
{
    SessionLoadOutcome outcome = loadWatcher_.future().takeResult();
    loadInFlight_ = false;

    // A newer selection/refresh supersedes this result. Start only the latest
    // coalesced request and never flash stale session data into the panel.
    if (outcome.requestId == latestRequestId_
        && outcome.runId == selectedRunId_.toStdString()) {
        if (outcome.error.has_value()) {
            fail(*outcome.error);
        } else if (outcome.projection.has_value()) {
            // AG-16.2: preserved across the reassignment below so a proposal
            // selected before this reload (e.g. the one just applied/
            // rejected) can be re-derived against the freshly-loaded
            // current_->proposals immediately after - the panel stays open
            // showing live Applied/blocked state in place, instead of
            // silently blanking on every refresh().
            const QString previouslySelected = selectedChangeSetId_;
            current_ = std::move(outcome.projection);
            selectedSessionId_ = QString::fromStdString(current_->session.sessionId);
            turns_.replace(current_->turns);
            toolCalls_.replace(current_->toolCalls);
            proposals_.replace(current_->proposals);
            {
                QHash<QString, ai::application::ProposalReviewRejection> rejections;
                rejections.reserve(static_cast<int>(outcome.rejections.size()));
                for (const auto& [changeSetId, rejection] : outcome.rejections) {
                    rejections.insert(QString::fromStdString(changeSetId), rejection);
                }
                proposals_.setReviewRejections(std::move(rejections));
            }
            if (previouslySelected.isEmpty() || !rebuildSelectedProposalDetail(previouslySelected)) {
                // Nothing was selected, or the previously-selected proposal
                // is gone from this Run entirely (a genuinely different
                // session/Run was loaded) - rebuildSelectedProposalDetail()
                // itself already cleared selectedProposalDetail_/
                // selectedChangeSetId_ in the latter case.
                selectedProposalDetail_.clear();
                selectedChangeSetId_.clear();
            }
            clearError();
            emit stateChanged();
        } else {
            // Most runs never create an agent-tool session.
            clearSession();
            clearError();
            emit stateChanged();
        }
    }

    if (pendingLoad_.has_value()) {
        startPendingLoad();
        return;
    }
    if (busy_) {
        busy_ = false;
        emit stateChanged();
    }
}

void AgentSessionController::clearSession()
{
    selectedSessionId_.clear();
    current_.reset();
    turns_.replace({});
    toolCalls_.replace({});
    proposals_.replace({});
    selectedProposalDetail_.clear();
    selectedChangeSetId_.clear();
}

bool AgentSessionController::selectProposal(const QString& changeSetId)
{
    if (changeSetId.isEmpty()) {
        selectedProposalDetail_.clear();
        selectedChangeSetId_.clear();
        emit stateChanged();
        return true;
    }
    if (!current_.has_value()) return fail({"agent_session.no_session", "No session is selected"});

    if (!rebuildSelectedProposalDetail(changeSetId))
        return fail({"proposal.not_found", "Proposal not found in the loaded session"});
    clearError();
    emit stateChanged();
    return true;
}

bool AgentSessionController::rebuildSelectedProposalDetail(const QString& changeSetId)
{
    if (!current_.has_value() || changeSetId.isEmpty()) {
        selectedProposalDetail_.clear();
        selectedChangeSetId_.clear();
        return false;
    }

    const auto id = changeSetId.toStdString();
    const auto found = std::find_if(current_->proposals.begin(), current_->proposals.end(),
        [&](const auto& p) { return p.header.changeSetId == id; });
    if (found == current_->proposals.end()) {
        selectedProposalDetail_.clear();
        selectedChangeSetId_.clear();
        return false;
    }

    QVariantList changes;
    for (const auto& change : found->changes) {
        changes.push_back(QVariantMap {
            {QStringLiteral("changeId"), QString::fromStdString(change.changeId)},
            {QStringLiteral("operation"),
                QString::fromLatin1(ai::application::toString(change.operation))},
            // relativePath is always relative to the project root - never
            // an absolute filesystem path (enforced before persistence).
            {QStringLiteral("relativePath"),
                QString::fromStdString(change.relativePath.generic_string())},
            {QStringLiteral("sourcePath"), change.sourcePath.has_value()
                ? QString::fromStdString(change.sourcePath->generic_string()) : QString()},
            {QStringLiteral("hasExpectedHash"), change.expectedOriginalHash.has_value()}
        });
    }
    QVariantList findings;
    for (const auto& finding : found->findings) {
        findings.push_back(QVariantMap {
            {QStringLiteral("code"), QString::fromStdString(finding.code)},
            {QStringLiteral("message"), QString::fromStdString(finding.message)},
            {QStringLiteral("changeId"), QString::fromStdString(finding.changeId)}
        });
    }
    // AG-07: read-through the same review service the panel's rejectProposal
    // command uses, rather than re-deriving this from proposalsModel's rows -
    // this method already makes its own synchronous backend calls for
    // readOriginalContent-style detail, matching that established pattern.
    const auto rejection = reviewService_.findRejection(id);
    const bool reviewRejected = rejection.success() && rejection.value().has_value();
    const auto successor = std::find_if(
        current_->proposals.begin(), current_->proposals.end(), [&](const auto& proposal) {
            return proposal.header.supersedesChangeSetId.has_value()
                && *proposal.header.supersedesChangeSetId == id;
        });

    // AG-16.2: the single authoritative apply-eligibility decision, read
    // fresh on every rebuild - never cached across a reload, never computed
    // here. evaluate() is read-only and cheap (a handful of already-indexed
    // repository reads), safe to call synchronously on the GUI thread, the
    // same way findRejection() above already is.
    const auto eligibility = verificationGate_.evaluate(id);

    selectedProposalDetail_ = QVariantMap {
        {QStringLiteral("changeSetId"), changeSetId},
        {QStringLiteral("summary"), QString::fromStdString(found->header.summary)},
        {QStringLiteral("agentRole"), QString::fromStdString(found->header.agentRole)},
        {QStringLiteral("state"), QString::fromLatin1(ai::application::toString(found->header.state))},
        {QStringLiteral("changes"), changes},
        {QStringLiteral("findings"), findings},
        {QStringLiteral("supersedesChangeSetId"),
            found->header.supersedesChangeSetId.has_value()
                ? QString::fromStdString(*found->header.supersedesChangeSetId) : QString()},
        {QStringLiteral("supersededByChangeSetId"),
            successor != current_->proposals.end()
                ? QString::fromStdString(successor->header.changeSetId) : QString()},
        {QStringLiteral("superseded"), successor != current_->proposals.end()},
        {QStringLiteral("reviewRejected"), reviewRejected},
        {QStringLiteral("reviewRejectedReason"), reviewRejected && rejection.value()->reason.has_value()
            ? QString::fromStdString(*rejection.value()->reason) : QString()},
        {QStringLiteral("applyAllowed"), eligibility.allowed},
        {QStringLiteral("applyBlockCode"), QString::fromStdString(eligibility.reasonCode)},
        {QStringLiteral("applyBlockMessage"), QString::fromStdString(eligibility.reasonMessage)}
    };
    selectedChangeSetId_ = changeSetId;
    return true;
}

QString AgentSessionController::readOriginalContent(
    const QString& changeSetId, const QString& changeId)
{
    const auto content = proposals_service_.readOriginalContent(
        changeSetId.toStdString(), changeId.toStdString());
    if (!content.success()) {
        fail(*content.error());
        return {};
    }
    return QString::fromStdString(content.value());
}

QString AgentSessionController::readProposedContent(
    const QString& changeSetId, const QString& changeId)
{
    const auto content = proposals_service_.readProposedContent(
        changeSetId.toStdString(), changeId.toStdString());
    if (!content.success()) {
        fail(*content.error());
        return {};
    }
    return QString::fromStdString(content.value());
}

bool AgentSessionController::applyProposal(const QString& changeSetId)
{
    applyResultText_.clear();
    if (changeSetId.isEmpty())
        return fail({"proposal.apply_invalid", "changeSetId is required"});

    // AG-07: a reviewer rejection blocks apply even when a prior approval
    // already exists (approve() may have run before the reject() call) -
    // this is the one production call site that reaches
    // ChangeSetApplyService::apply(), so this is the sole enforcement point.
    const auto notRejected = reviewService_.assertNotRejected(changeSetId.toStdString());
    if (!notRejected.success()) {
        rebuildSelectedProposalDetailAfterFailure(changeSetId);
        return fail(*notRejected.error());
    }

    const auto result = applyService_.apply(changeSetId.toStdString());

    if (!result.success()) {
        rebuildSelectedProposalDetailAfterFailure(changeSetId);
        return fail(*result.error());
    }

    applyResultText_ = QStringLiteral("Applied %1 file(s) to workspace")
        .arg(static_cast<int>(result.value().appliedCount));
    if (result.value().provenanceErrorCode) {
        applyResultText_ += QStringLiteral(". Warning: post-apply provenance was not recorded (%1)")
            .arg(QString::fromStdString(*result.value().provenanceErrorCode));
    }

    // Reload to reflect Applied state in the proposal list.
    refresh();
    return true;
}

bool AgentSessionController::approveAndApply(const QString& changeSetId)
{
    applyResultText_.clear();
    if (changeSetId.isEmpty())
        return fail({"proposal.apply_invalid", "changeSetId is required"});

    // AG-07: refuse before even recording a fresh approval - see the
    // matching comment in applyProposal() above for why this is the
    // enforcement point.
    const auto notRejected = reviewService_.assertNotRejected(changeSetId.toStdString());
    if (!notRejected.success()) {
        rebuildSelectedProposalDetailAfterFailure(changeSetId);
        return fail(*notRejected.error());
    }

    // Best-effort OS username annotation, not an authenticated identity -
    // this application has no login/user concept anywhere else either. Its
    // absence (headless/CI environments) is not an error.
    std::optional<std::string> approvedBy;
    const QString osUser = qEnvironmentVariable("USERNAME");
    if (!osUser.isEmpty())
        approvedBy = osUser.toStdString();

    const auto approved = applyService_.approve(changeSetId.toStdString(), approvedBy);
    if (!approved.success()) {
        rebuildSelectedProposalDetailAfterFailure(changeSetId);
        return fail(*approved.error());
    }

    const auto result = applyService_.apply(changeSetId.toStdString());
    if (!result.success()) {
        rebuildSelectedProposalDetailAfterFailure(changeSetId);
        return fail(*result.error());
    }

    applyResultText_ = QStringLiteral("Applied %1 file(s) to workspace")
        .arg(static_cast<int>(result.value().appliedCount));
    if (result.value().provenanceErrorCode) {
        applyResultText_ += QStringLiteral(". Warning: post-apply provenance was not recorded (%1)")
            .arg(QString::fromStdString(*result.value().provenanceErrorCode));
    }

    // Reload to reflect Applied state in the proposal list.
    refresh();
    return true;
}

bool AgentSessionController::rejectProposal(const QString& changeSetId, const QString& reason)
{
    applyResultText_.clear();
    if (changeSetId.isEmpty())
        return fail({"proposal.apply_invalid", "changeSetId is required"});

    // Best-effort OS username annotation, matching approveAndApply() above -
    // not an authenticated identity.
    std::optional<std::string> rejectedBy;
    const QString osUser = qEnvironmentVariable("USERNAME");
    if (!osUser.isEmpty())
        rejectedBy = osUser.toStdString();

    std::optional<std::string> boundedReason;
    if (!reason.isEmpty()) boundedReason = reason.toStdString();

    const auto rejected = reviewService_.reject(
        changeSetId.toStdString(), std::move(rejectedBy), std::move(boundedReason));
    if (!rejected.success())
        return fail(*rejected.error());

    // Reload so proposalsModel/selectedProposalDetail reflect the rejection
    // immediately - mirrors applyProposal()/approveAndApply()'s own refresh().
    refresh();
    return true;
}

bool AgentSessionController::fail(const ai::common::Error& error)
{
    errorCode_ = QString::fromStdString(error.code);
    errorText_ = QString::fromStdString(error.message);
    emit errorOccurred(errorCode_, errorText_);
    emit stateChanged();
    return false;
}

void AgentSessionController::clearError()
{
    errorCode_.clear();
    errorText_.clear();
}

void AgentSessionController::rebuildSelectedProposalDetailAfterFailure(
    const QString& changeSetId)
{
    if (!changeSetId.isEmpty() && selectedChangeSetId_ == changeSetId) {
        static_cast<void>(rebuildSelectedProposalDetail(changeSetId));
    }
}
