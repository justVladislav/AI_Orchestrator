#include <ai/ui/ManifestVerificationManualHandoffController.hpp>

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

ManifestVerificationManualHandoffController::ManifestVerificationManualHandoffController(
    ai::application::ManifestVerificationManualHandoffReadModelService& readModel,
    ai::application::ManifestVerificationManualHandoffService& manualHandoffService,
    ai::application::ChangeProposalService& proposals,
    ai::application::ChangeSetApplyService& applyService,
    ai::application::ProposalReviewService& reviewService,
    ai::application::ManifestApplyVerificationGate& verificationGate,
    QObject* parent)
    : QObject(parent), readModel_(readModel), manualHandoffService_(manualHandoffService),
      proposals_(proposals), applyService_(applyService), reviewService_(reviewService),
      verificationGate_(verificationGate)
{
    connect(&commandWatcher_,
        &QFutureWatcher<ManualHandoffCommandOutcome>::finished,
        this, &ManifestVerificationManualHandoffController::finishCommand);
}

ManifestVerificationManualHandoffController::~ManifestVerificationManualHandoffController()
{
    if (activeCommand_) activeCommand_->stopSource.request_stop();
    commandWatcher_.waitForFinished();
}

void ManifestVerificationManualHandoffController::clear()
{
    isApplicable_ = false;
    verificationAnchorRunId_.clear();
    verificationAnchorStatus_.clear();
    hasHandoff_ = false;
    handoffId_.clear();
    originManifestRunId_.clear();
    originManifestStatus_.clear();
    candidateHealth_.clear();
    allowedRelativePaths_.clear();
    editableChangedRelativePaths_.clear();
    resetRequiredRelativePaths_.clear();
    resetResultText_.clear();
    initialScopeDiagnostics_.clear();
    recordedFindings_.clear();
    attempts_.clear();
    hasFinalVerification_ = false;
    finalProposalSafetyStatus_.clear();
    finalBuildLintTestStatus_.clear();
    finalAcceptanceStatus_.clear();
    canApply_ = false;
    applyReasonCode_.clear();
    applyReasonMessage_.clear();
    canCreate_ = false;
    createReasonCode_.clear();
    createReasonMessage_.clear();
    reviewMembers_.clear();
    selectedMemberProposalId_.clear();
    selectedMemberDetail_.clear();
    applyResultText_.clear();
}

QVariantMap ManifestVerificationManualHandoffController::toReviewMemberVariant(
    const ai::application::ManifestVerificationManualHandoffReviewMemberView& member)
{
    QVariantList changes;
    changes.reserve(static_cast<qsizetype>(member.changes.size()));
    for (const auto& change : member.changes) {
        changes.push_back(QVariantMap {
            {QStringLiteral("changeId"), QString::fromStdString(change.changeId)},
            {QStringLiteral("operation"), QString::fromLatin1(change.operation.c_str())},
            {QStringLiteral("relativePath"), QString::fromStdString(change.relativePath)},
            {QStringLiteral("hasExpectedHash"), change.hasExpectedHash}});
    }
    QVariantList findings;
    findings.reserve(static_cast<qsizetype>(member.findings.size()));
    for (const auto& finding : member.findings) {
        findings.push_back(QVariantMap {
            {QStringLiteral("code"), QString::fromStdString(finding.code)},
            {QStringLiteral("message"), QString::fromStdString(finding.message)},
            {QStringLiteral("changeId"), QString::fromStdString(finding.changeId)}});
    }
    return QVariantMap {
        {QStringLiteral("proposalId"), QString::fromStdString(member.proposalId)},
        {QStringLiteral("owningRunId"), QString::fromStdString(member.owningRunId)},
        {QStringLiteral("summary"), QString::fromStdString(member.summary)},
        {QStringLiteral("state"), QString::fromLatin1(member.state.c_str())},
        {QStringLiteral("alreadyApplied"), member.alreadyApplied},
        {QStringLiteral("reviewRejected"), member.reviewRejected},
        {QStringLiteral("reviewRejectedReason"), QString::fromStdString(member.reviewRejectedReason)},
        {QStringLiteral("applyAllowed"), member.applyAllowed},
        {QStringLiteral("applyBlockCode"), QString::fromStdString(member.applyBlockCode)},
        {QStringLiteral("applyBlockMessage"), QString::fromStdString(member.applyBlockMessage)},
        {QStringLiteral("changes"), changes},
        {QStringLiteral("findings"), findings}};
}

QVariantMap ManifestVerificationManualHandoffController::findReviewMember(
    const QString& proposalId) const
{
    for (const auto& entry : reviewMembers_) {
        const auto map = entry.toMap();
        if (map.value(QStringLiteral("proposalId")).toString() == proposalId) return map;
    }
    return {};
}

std::optional<ai::application::ManifestVerificationManualHandoffReviewMemberView>
ManifestVerificationManualHandoffController::resolveFreshAuthoritativeMember(
    const QString& proposalId)
{
    if (selectedRunId_.isEmpty() || proposalId.isEmpty()) {
        errorText_ = QStringLiteral("manual_handoff.review_member_not_found: ")
            + QStringLiteral("This proposal is not part of the handoff's authoritative "
                              "verified set");
        return std::nullopt;
    }

    const auto view = readModel_.viewForRun(selectedRunId_.toStdString());
    if (!view.success()) {
        errorText_ = QString::fromStdString(view.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(view.error()->message);
        return std::nullopt;
    }
    if (!view.value().has_value()) {
        errorText_ = QStringLiteral("manual_handoff.review_member_not_found: ")
            + QStringLiteral("This proposal is not part of the handoff's authoritative "
                              "verified set");
        return std::nullopt;
    }

    const auto id = proposalId.toStdString();
    for (const auto& member : view.value()->reviewMembers) {
        if (member.proposalId == id) return member;
    }
    errorText_ = QStringLiteral("manual_handoff.review_member_not_found: ")
        + QStringLiteral("This proposal is not part of the handoff's current authoritative "
                          "verified set");
    return std::nullopt;
}

void ManifestVerificationManualHandoffController::populate(
    const ai::application::ManifestVerificationManualHandoffView& view)
{
    isApplicable_ = true;
    verificationAnchorRunId_ = QString::fromStdString(view.verificationAnchorRunId);
    verificationAnchorStatus_ = QString::fromLatin1(view.verificationAnchorStatus.c_str());
    hasHandoff_ = view.hasHandoff;
    handoffId_ = QString::fromStdString(view.handoffId);
    originManifestRunId_ = QString::fromStdString(view.originManifestRunId);
    originManifestStatus_ = QString::fromLatin1(view.originManifestStatus.c_str());
    candidateHealth_ = QString::fromLatin1(view.candidateHealth.c_str());

    QStringList allowedPaths;
    allowedPaths.reserve(static_cast<int>(view.allowedRelativePaths.size()));
    for (const auto& path : view.allowedRelativePaths) allowedPaths << QString::fromStdString(path);
    allowedRelativePaths_ = std::move(allowedPaths);

    QStringList editableChangedPaths;
    editableChangedPaths.reserve(static_cast<int>(view.editableChangedRelativePaths.size()));
    for (const auto& path : view.editableChangedRelativePaths) {
        editableChangedPaths << QString::fromStdString(path);
    }
    editableChangedRelativePaths_ = std::move(editableChangedPaths);

    QStringList resetRequiredPaths;
    resetRequiredPaths.reserve(static_cast<int>(view.resetRequiredRelativePaths.size()));
    for (const auto& path : view.resetRequiredRelativePaths) {
        resetRequiredPaths << QString::fromStdString(path);
    }
    resetRequiredRelativePaths_ = std::move(resetRequiredPaths);

    QStringList initialScopeDiagnostics;
    initialScopeDiagnostics.reserve(static_cast<int>(view.initialScopeDiagnostics.size()));
    for (const auto& diagnostic : view.initialScopeDiagnostics) {
        initialScopeDiagnostics << QString::fromStdString(diagnostic);
    }
    initialScopeDiagnostics_ = std::move(initialScopeDiagnostics);

    {
        const auto& recorded = view.recordedFindings;
        QVariantList rows;
        rows.reserve(static_cast<qsizetype>(recorded.findings.size()));
        for (const auto& row : recorded.findings) {
            rows.push_back(QVariantMap {
                {QStringLiteral("text"), QString::fromStdString(row.text)},
                {QStringLiteral("severity"), QString::fromLatin1(row.severity.c_str())},
                {QStringLiteral("relativePath"), QString::fromStdString(row.relativePath)},
                {QStringLiteral("authority"), QString::fromLatin1(row.authority.c_str())},
                {QStringLiteral("diagnosticCode"), QString::fromStdString(row.diagnosticCode)},
                {QStringLiteral("line"), row.line},
                {QStringLiteral("column"), row.column},
                {QStringLiteral("tag"), QString::fromStdString(row.tag)}});
        }
        QStringList outsidePaths;
        outsidePaths.reserve(static_cast<int>(recorded.outsideScopePaths.size()));
        for (const auto& path : recorded.outsideScopePaths) outsidePaths << QString::fromStdString(path);
        recordedFindings_ = QVariantMap {
            {QStringLiteral("hasFindings"), !recorded.findings.empty() || recorded.debtCount > 0},
            {QStringLiteral("findings"), rows},
            {QStringLiteral("debtCount"), static_cast<qulonglong>(recorded.debtCount)},
            {QStringLiteral("verdict"), QString::fromLatin1(recorded.verdict.c_str())},
            {QStringLiteral("verdictCode"), QString::fromStdString(recorded.verdictCode)},
            {QStringLiteral("verdictMessage"), QString::fromStdString(recorded.verdictMessage)},
            {QStringLiteral("outsideScopePaths"), outsidePaths},
            {QStringLiteral("classificationErrorCode"),
                QString::fromStdString(recorded.classificationErrorCode)},
            {QStringLiteral("classificationErrorMessage"),
                QString::fromStdString(recorded.classificationErrorMessage)}};
    }

    QVariantList attempts;
    attempts.reserve(static_cast<int>(view.attempts.size()));
    for (const auto& attempt : view.attempts) {
        QVariantList diagnostics;
        diagnostics.reserve(static_cast<qsizetype>(attempt.verificationDiagnostics.size()));
        for (const auto& diagnostic : attempt.verificationDiagnostics) {
            diagnostics.push_back(QString::fromStdString(diagnostic));
        }
        QVariantList actionableDiagnostics;
        actionableDiagnostics.reserve(static_cast<qsizetype>(attempt.actionableDiagnostics.size()));
        for (const auto& diagnostic : attempt.actionableDiagnostics) {
            actionableDiagnostics.push_back(QString::fromStdString(diagnostic));
        }
        QVariantList outsideScopeDiagnostics;
        outsideScopeDiagnostics.reserve(
            static_cast<qsizetype>(attempt.outsideScopeDiagnostics.size()));
        for (const auto& diagnostic : attempt.outsideScopeDiagnostics) {
            outsideScopeDiagnostics.push_back(QString::fromStdString(diagnostic));
        }
        QVariantList generalDiagnostics;
        generalDiagnostics.reserve(static_cast<qsizetype>(attempt.generalDiagnostics.size()));
        for (const auto& diagnostic : attempt.generalDiagnostics) {
            generalDiagnostics.push_back(QString::fromStdString(diagnostic));
        }
        QVariantList newActionableDiagnostics;
        newActionableDiagnostics.reserve(
            static_cast<qsizetype>(attempt.newActionableDiagnostics.size()));
        for (const auto& diagnostic : attempt.newActionableDiagnostics) {
            newActionableDiagnostics.push_back(QString::fromStdString(diagnostic));
        }
        QVariantList newOutsideManifestOrContextDiagnostics;
        newOutsideManifestOrContextDiagnostics.reserve(
            static_cast<qsizetype>(attempt.newOutsideManifestOrContextDiagnostics.size()));
        for (const auto& diagnostic : attempt.newOutsideManifestOrContextDiagnostics) {
            newOutsideManifestOrContextDiagnostics.push_back(QString::fromStdString(diagnostic));
        }
        QVariantList unchangedBaselineDebtDiagnostics;
        unchangedBaselineDebtDiagnostics.reserve(
            static_cast<qsizetype>(attempt.unchangedBaselineDebtDiagnostics.size()));
        for (const auto& diagnostic : attempt.unchangedBaselineDebtDiagnostics) {
            unchangedBaselineDebtDiagnostics.push_back(QString::fromStdString(diagnostic));
        }
        attempts.push_back(QVariantMap {
            {QStringLiteral("attemptRunId"), QString::fromStdString(attempt.attemptRunId)},
            {QStringLiteral("attemptOrdinal"), static_cast<quint64>(attempt.attemptOrdinal)},
            {QStringLiteral("attemptStatus"), QString::fromLatin1(attempt.attemptStatus.c_str())},
            {QStringLiteral("resultKind"), QString::fromLatin1(attempt.resultKind.c_str())},
            {QStringLiteral("rejectionCode"), QString::fromStdString(attempt.rejectionCode)},
            {QStringLiteral("rejectionMessage"), QString::fromStdString(attempt.rejectionMessage)},
            {QStringLiteral("hasVerification"), attempt.hasVerification},
            {QStringLiteral("proposalSafetyStatus"),
                QString::fromLatin1(attempt.proposalSafetyStatus.c_str())},
            {QStringLiteral("buildLintTestStatus"),
                QString::fromLatin1(attempt.buildLintTestStatus.c_str())},
            {QStringLiteral("acceptanceStatus"),
                QString::fromLatin1(attempt.acceptanceStatus.c_str())},
            {QStringLiteral("verificationDiagnostics"), diagnostics},
            {QStringLiteral("actionableDiagnostics"), actionableDiagnostics},
            {QStringLiteral("outsideScopeDiagnostics"), outsideScopeDiagnostics},
            {QStringLiteral("generalDiagnostics"), generalDiagnostics},
            {QStringLiteral("newActionableDiagnostics"), newActionableDiagnostics},
            {QStringLiteral("newOutsideManifestOrContextDiagnostics"),
                newOutsideManifestOrContextDiagnostics},
            {QStringLiteral("unchangedBaselineDebtDiagnostics"), unchangedBaselineDebtDiagnostics},
            {QStringLiteral("isAuthoritative"), attempt.isAuthoritative}});
    }
    attempts_ = std::move(attempts);

    hasFinalVerification_ = view.hasFinalVerification;
    finalProposalSafetyStatus_ = QString::fromLatin1(view.finalProposalSafetyStatus.c_str());
    finalBuildLintTestStatus_ = QString::fromLatin1(view.finalBuildLintTestStatus.c_str());
    finalAcceptanceStatus_ = QString::fromLatin1(view.finalAcceptanceStatus.c_str());
    canApply_ = view.canApply;
    applyReasonCode_ = QString::fromStdString(view.applyReasonCode);
    applyReasonMessage_ = QString::fromStdString(view.applyReasonMessage);
    canCreate_ = view.canCreate;
    createReasonCode_ = QString::fromStdString(view.createReasonCode);
    createReasonMessage_ = QString::fromStdString(view.createReasonMessage);

    QVariantList reviewMembers;
    reviewMembers.reserve(static_cast<qsizetype>(view.reviewMembers.size()));
    for (const auto& member : view.reviewMembers) {
        reviewMembers.push_back(toReviewMemberVariant(member));
    }
    reviewMembers_ = std::move(reviewMembers);

    // Post-P4R.6 human review/Apply hardening: preserve a selection across a
    // reload the same way AgentSessionController::finishLoad() preserves
    // selectedChangeSetId_ across its own background reload - a just-
    // applied/rejected member's detail must keep showing its fresh state in
    // place rather than silently blanking the panel. A member no longer
    // present (never expected in practice - the exact set is immutable once
    // frozen - but never assumed) clears the selection instead of showing
    // stale detail.
    if (!selectedMemberProposalId_.isEmpty()) {
        const auto refreshed = findReviewMember(selectedMemberProposalId_);
        if (!refreshed.isEmpty()) {
            selectedMemberDetail_ = refreshed;
        } else {
            selectedMemberProposalId_.clear();
            selectedMemberDetail_.clear();
        }
    }
}

bool ManifestVerificationManualHandoffController::selectRun(const QString& runId)
{
    if (runId != selectedRunId_) {
        applyResultText_.clear();
        resetResultText_.clear();
    }
    selectedRunId_ = runId;
    return refresh();
}

bool ManifestVerificationManualHandoffController::refresh()
{
    if (selectedRunId_.isEmpty()) {
        clear();
        errorText_.clear();
        emit stateChanged();
        return true;
    }

    const auto view = readModel_.viewForRun(selectedRunId_.toStdString());
    if (!view.success()) {
        clear();
        errorText_ = QString::fromStdString(view.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(view.error()->message);
        emit stateChanged();
        return false;
    }
    if (!view.value().has_value()) {
        // Outside the two anchor workflows materialize() itself ever
        // accepts - never any manual-handoff action for this Run.
        clear();
        errorText_.clear();
        emit stateChanged();
        return true;
    }

    populate(*view.value());
    errorText_.clear();
    emit stateChanged();
    return true;
}

bool ManifestVerificationManualHandoffController::create()
{
    if (selectedRunId_.isEmpty() || busy_) return false;
    applyResultText_.clear();
    resetResultText_.clear();
    if (hasHandoff_) {
        errorText_ = QStringLiteral("manual_handoff.already_exists: ")
            + QStringLiteral("A manual handoff already exists for this Run");
        emit stateChanged();
        return false;
    }
    if (!canCreate_) {
        const QString code = createReasonCode_.isEmpty()
            ? QStringLiteral("manual_handoff.create_not_allowed") : createReasonCode_;
        const QString message = createReasonMessage_.isEmpty()
            ? QStringLiteral("The Backend did not authorize creating a manual handoff for this Run")
            : createReasonMessage_;
        errorText_ = code + QStringLiteral(": ") + message;
        emit stateChanged();
        return false;
    }

    const std::string anchorRunId = selectedRunId_.toStdString();
    const QString commandRunId = selectedRunId_;
    activeCommand_ = std::make_shared<ActiveCommand>();
    activeCommand_->runId = commandRunId;
    auto activeCommand = activeCommand_;
    auto* service = &manualHandoffService_;
    busy_ = true;
    errorText_.clear();
    emit stateChanged();
    commandWatcher_.setFuture(QtConcurrent::run(
        [anchorRunId, commandRunId, activeCommand, service] {
            ManualHandoffCommandOutcome outcome;
            outcome.commandRunId = commandRunId;
            const auto materialized = service->materialize(
                anchorRunId, activeCommand->stopSource.get_token());
            if (!materialized.success()) outcome.error = *materialized.error();
            return outcome;
        }));
    return true;
}

bool ManifestVerificationManualHandoffController::reverify()
{
    if (selectedRunId_.isEmpty() || busy_) return false;
    applyResultText_.clear();
    resetResultText_.clear();
    if (!hasHandoff_ || handoffId_.isEmpty()) {
        errorText_ = QStringLiteral("manual_handoff.not_found: ")
            + QStringLiteral("No manual handoff exists yet for this Run");
        emit stateChanged();
        return false;
    }

    const std::string handoffId = handoffId_.toStdString();
    const QString commandRunId = selectedRunId_;
    activeCommand_ = std::make_shared<ActiveCommand>();
    activeCommand_->runId = commandRunId;
    auto activeCommand = activeCommand_;
    auto* service = &manualHandoffService_;
    busy_ = true;
    errorText_.clear();
    emit stateChanged();
    commandWatcher_.setFuture(QtConcurrent::run(
        [handoffId, commandRunId, activeCommand, service] {
            ManualHandoffCommandOutcome outcome;
            outcome.commandRunId = commandRunId;
            const auto imported = service->importAndReverify(
                handoffId, activeCommand->stopSource.get_token());
            if (!imported.success()) outcome.error = *imported.error();
            return outcome;
        }));
    return true;
}

bool ManifestVerificationManualHandoffController::discard()
{
    if (selectedRunId_.isEmpty() || busy_) return false;
    applyResultText_.clear();
    resetResultText_.clear();
    if (!hasHandoff_ || handoffId_.isEmpty()) {
        errorText_ = QStringLiteral("manual_handoff.not_found: ")
            + QStringLiteral("No manual handoff exists yet for this Run");
        emit stateChanged();
        return false;
    }

    // discardHandoff() itself is the sole authority on whether an active
    // attempt blocks this (manual_handoff.discard_attempt_active) - never
    // pre-guessed here from any locally cached attempt status. Its stop
    // token is observed at Backend-owned boundaries up to the irreversible
    // database delete; after that commit, best-effort filesystem cleanup
    // completes and the durable success is never misreported as cancelled.
    const std::string handoffId = handoffId_.toStdString();
    const QString commandRunId = selectedRunId_;
    activeCommand_ = std::make_shared<ActiveCommand>();
    activeCommand_->runId = commandRunId;
    auto activeCommand = activeCommand_;
    auto* service = &manualHandoffService_;
    busy_ = true;
    errorText_.clear();
    emit stateChanged();
    commandWatcher_.setFuture(QtConcurrent::run(
        [handoffId, commandRunId, activeCommand, service] {
            ManualHandoffCommandOutcome outcome;
            outcome.commandRunId = commandRunId;
            const auto discarded = service->discardHandoff(
                handoffId, activeCommand->stopSource.get_token());
            if (!discarded.success()) outcome.error = *discarded.error();
            return outcome;
        }));
    return true;
}

bool ManifestVerificationManualHandoffController::resetNonEditableChanges()
{
    if (selectedRunId_.isEmpty() || busy_) return false;
    applyResultText_.clear();
    resetResultText_.clear();
    if (!hasHandoff_ || handoffId_.isEmpty()) {
        errorText_ = QStringLiteral("manual_handoff.not_found: ")
            + QStringLiteral("No manual handoff exists yet for this Run");
        emit stateChanged();
        return false;
    }
    // resetNonEditableChanges() itself is the sole authority on the exact
    // reset-required set; this guard only avoids spending a real filesystem
    // pass when QML's own cached projection is already stale and has
    // nothing to reset - the Backend call would otherwise correctly refuse
    // with the identical manual_handoff.no_non_editable_changes code.
    if (resetRequiredRelativePaths_.isEmpty()) {
        errorText_ = QStringLiteral("manual_handoff.no_non_editable_changes: ")
            + QStringLiteral(
                "The candidate contains no outside-scope or unsupported changes to reset");
        emit stateChanged();
        return false;
    }

    const std::string handoffId = handoffId_.toStdString();
    const QString commandRunId = selectedRunId_;
    activeCommand_ = std::make_shared<ActiveCommand>();
    activeCommand_->runId = commandRunId;
    auto activeCommand = activeCommand_;
    auto* service = &manualHandoffService_;
    busy_ = true;
    errorText_.clear();
    emit stateChanged();
    commandWatcher_.setFuture(QtConcurrent::run(
        [handoffId, commandRunId, activeCommand, service] {
            ManualHandoffCommandOutcome outcome;
            outcome.commandRunId = commandRunId;
            const auto reset = service->resetNonEditableChanges(
                handoffId, activeCommand->stopSource.get_token());
            if (!reset.success()) {
                outcome.error = *reset.error();
            } else {
                outcome.resetSummary = QStringLiteral(
                    "Reset %1 non-editable path(s); %2 authorized edit(s) preserved")
                    .arg(static_cast<int>(reset.value().resetPaths.size()))
                    .arg(static_cast<int>(reset.value().editablePreservedPaths.size()));
            }
            return outcome;
        }));
    return true;
}

bool ManifestVerificationManualHandoffController::cancel()
{
    if (!busy_ || !activeCommand_) return false;
    const bool requested = activeCommand_->stopSource.request_stop();
    if (requested) emit stateChanged();
    return requested;
}

void ManifestVerificationManualHandoffController::finishCommand()
{
    const auto outcome = commandWatcher_.result();
    const bool outcomeMatchesSelection = outcome.commandRunId == selectedRunId_;
    const bool succeeded = !outcome.error.has_value();
    busy_ = false;
    activeCommand_.reset();
    QString durableError;
    if (outcome.error.has_value()) {
        durableError = QString::fromStdString(outcome.error->code) + QStringLiteral(": ")
            + QString::fromStdString(outcome.error->message);
    }
    // Re-read every decision/lineage field from durable Backend storage. The
    // worker's own result is notification data, never the UI's source of
    // truth.
    const bool refreshed = refresh();
    // A command may finish after the user selects another Run. Refresh the
    // current selection, but never paste the old Run's notification error
    // onto it. If the authoritative refresh itself failed, that newer read
    // failure also wins over worker notification data.
    if (outcomeMatchesSelection && refreshed && !durableError.isEmpty())
        errorText_ = durableError;
    if (outcomeMatchesSelection && refreshed && succeeded && outcome.resetSummary.has_value())
        resetResultText_ = *outcome.resetSummary;
    emit stateChanged();
    emit commandFinished(outcome.commandRunId, succeeded);
    emit durableStateChanged(outcome.commandRunId);
}

bool ManifestVerificationManualHandoffController::openCandidateLocation()
{
    if (selectedRunId_.isEmpty()) {
        errorText_ = QStringLiteral("client.no_run_selected: No Run is selected");
        emit stateChanged();
        return false;
    }
    if (busy_) {
        errorText_ = QStringLiteral("manual_handoff.command_busy: ")
            + QStringLiteral("A manual-handoff command is already running");
        emit stateChanged();
        return false;
    }
    if (!hasHandoff_) {
        errorText_ = QStringLiteral("manual_handoff.not_found: ")
            + QStringLiteral("No manual handoff exists yet for this Run");
        emit stateChanged();
        return false;
    }
    const auto root = manualHandoffService_.resolveCandidateRootForAnchor(
        selectedRunId_.toStdString());
    if (!root.success()) {
        errorText_ = QString::fromStdString(root.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(root.error()->message);
        emit stateChanged();
        return false;
    }
    const QUrl target = QUrl::fromLocalFile(QString::fromStdWString(root.value().wstring()));
    if (!QDesktopServices::openUrl(target)) {
        errorText_ = QStringLiteral("manual_handoff.open_failed: ")
            + QStringLiteral("No desktop application could open the candidate location");
        emit stateChanged();
        return false;
    }
    errorText_.clear();
    emit stateChanged();
    return true;
}

bool ManifestVerificationManualHandoffController::copyCandidatePathToClipboard()
{
    if (selectedRunId_.isEmpty()) {
        errorText_ = QStringLiteral("client.no_run_selected: No Run is selected");
        emit stateChanged();
        return false;
    }
    if (busy_) {
        errorText_ = QStringLiteral("manual_handoff.command_busy: ")
            + QStringLiteral("A manual-handoff command is already running");
        emit stateChanged();
        return false;
    }
    if (!hasHandoff_) {
        errorText_ = QStringLiteral("manual_handoff.not_found: ")
            + QStringLiteral("No manual handoff exists yet for this Run");
        emit stateChanged();
        return false;
    }
    const auto root = manualHandoffService_.resolveCandidateRootForAnchor(
        selectedRunId_.toStdString());
    if (!root.success()) {
        errorText_ = QString::fromStdString(root.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(root.error()->message);
        emit stateChanged();
        return false;
    }
    QGuiApplication::clipboard()->setText(QString::fromStdWString(root.value().wstring()));
    errorText_.clear();
    emit stateChanged();
    return true;
}

bool ManifestVerificationManualHandoffController::selectMember(const QString& proposalId)
{
    if (proposalId.isEmpty()) {
        selectedMemberProposalId_.clear();
        selectedMemberDetail_.clear();
        errorText_.clear();
        emit stateChanged();
        return true;
    }
    const auto member = resolveFreshAuthoritativeMember(proposalId);
    if (!member.has_value()) {
        emit stateChanged();
        return false;
    }
    selectedMemberProposalId_ = proposalId;
    selectedMemberDetail_ = toReviewMemberVariant(*member);
    errorText_.clear();
    emit stateChanged();
    return true;
}

QString ManifestVerificationManualHandoffController::readOriginalContent(
    const QString& proposalId, const QString& changeId)
{
    // Authorization boundary: proposalId must be a genuine, current member
    // of THIS handoff's authoritative verified set before any content is
    // read - an arbitrary typed id from QML (belonging to some other
    // proposal/handoff/Run) is refused here rather than merely relied on
    // QML "not offering" it.
    if (!resolveFreshAuthoritativeMember(proposalId).has_value()) {
        emit stateChanged();
        return {};
    }
    const auto content = proposals_.readOriginalContent(
        proposalId.toStdString(), changeId.toStdString());
    if (!content.success()) {
        errorText_ = QString::fromStdString(content.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(content.error()->message);
        emit stateChanged();
        return {};
    }
    errorText_.clear();
    emit stateChanged();
    return QString::fromStdString(content.value());
}

QString ManifestVerificationManualHandoffController::readProposedContent(
    const QString& proposalId, const QString& changeId)
{
    // Same authorization boundary as readOriginalContent() above.
    if (!resolveFreshAuthoritativeMember(proposalId).has_value()) {
        emit stateChanged();
        return {};
    }
    const auto content = proposals_.readProposedContent(
        proposalId.toStdString(), changeId.toStdString());
    if (!content.success()) {
        errorText_ = QString::fromStdString(content.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(content.error()->message);
        emit stateChanged();
        return {};
    }
    errorText_.clear();
    emit stateChanged();
    return QString::fromStdString(content.value());
}

bool ManifestVerificationManualHandoffController::approveAndApplyMember(
    const QString& proposalId)
{
    applyResultText_.clear();
    resetResultText_.clear();
    if (busy_) {
        errorText_ = QStringLiteral("manual_handoff.command_busy: ")
            + QStringLiteral("A manual-handoff command is already running");
        emit stateChanged();
        return false;
    }
    if (!resolveFreshAuthoritativeMember(proposalId).has_value()) {
        emit stateChanged();
        return false;
    }

    const auto id = proposalId.toStdString();

    // Defense-in-depth, mirroring AG-15's own diagnostic-read precedent:
    // ChangeSetApplyService::approve()/apply() themselves unconditionally
    // re-run this identical AG-16 evaluation immediately before mutating
    // anything and remain the sole real authority - this check is never a
    // replacement for that. It exists only so a proposal already known to
    // be ineligible is refused here, before approve() records a durable
    // ApprovalRecord for it, rather than only after apply() fails.
    const auto eligibility = verificationGate_.evaluate(id);
    if (!eligibility.allowed) {
        errorText_ = QString::fromStdString(eligibility.reasonCode) + QStringLiteral(": ")
            + QString::fromStdString(eligibility.reasonMessage);
        emit stateChanged();
        return false;
    }

    // AG-07: refuse before even recording a fresh approval - the identical
    // enforcement point AgentSessionController::approveAndApply() uses. On
    // failure this deliberately does NOT call refresh() - exactly like
    // AgentSessionController's own fail() path - since refresh() clears
    // errorText_ on a successful re-read and would silently discard the
    // very error being reported.
    const auto notRejected = reviewService_.assertNotRejected(id);
    if (!notRejected.success()) {
        errorText_ = QString::fromStdString(notRejected.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(notRejected.error()->message);
        emit stateChanged();
        return false;
    }

    std::optional<std::string> approvedBy;
    const QString osUser = qEnvironmentVariable("USERNAME");
    if (!osUser.isEmpty()) approvedBy = osUser.toStdString();

    const auto approved = applyService_.approve(id, approvedBy);
    if (!approved.success()) {
        errorText_ = QString::fromStdString(approved.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(approved.error()->message);
        emit stateChanged();
        return false;
    }

    // AG-16 is re-evaluated fresh by apply() itself immediately before any
    // workspace mutation - never trusted from reviewMembers' own cached
    // applyAllowed, which may already be stale by the time this call runs.
    const auto applied = applyService_.apply(id);
    if (!applied.success()) {
        errorText_ = QString::fromStdString(applied.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(applied.error()->message);
        emit stateChanged();
        return false;
    }

    applyResultText_ = QStringLiteral("Applied %1 file(s) to workspace")
        .arg(static_cast<int>(applied.value().appliedCount));
    if (applied.value().provenanceErrorCode.has_value()) {
        applyResultText_ += QStringLiteral(
            ". Warning: post-apply provenance was not recorded (%1)")
            .arg(QString::fromStdString(*applied.value().provenanceErrorCode));
    }

    // Re-read every field from durable Backend storage - never the local
    // call's own result - exactly like finishCommand() already does for
    // create()/reverify()/discard(). refresh() itself clears errorText_ and
    // emits stateChanged() on this success path.
    refresh();
    emit durableStateChanged(selectedRunId_);
    return true;
}

bool ManifestVerificationManualHandoffController::rejectMember(
    const QString& proposalId, const QString& reason)
{
    applyResultText_.clear();
    resetResultText_.clear();
    if (busy_) {
        errorText_ = QStringLiteral("manual_handoff.command_busy: ")
            + QStringLiteral("A manual-handoff command is already running");
        emit stateChanged();
        return false;
    }
    if (!resolveFreshAuthoritativeMember(proposalId).has_value()) {
        emit stateChanged();
        return false;
    }

    std::optional<std::string> rejectedBy;
    const QString osUser = qEnvironmentVariable("USERNAME");
    if (!osUser.isEmpty()) rejectedBy = osUser.toStdString();

    std::optional<std::string> boundedReason;
    if (!reason.isEmpty()) boundedReason = reason.toStdString();

    const auto rejected = reviewService_.reject(
        proposalId.toStdString(), std::move(rejectedBy), std::move(boundedReason));
    if (!rejected.success()) {
        errorText_ = QString::fromStdString(rejected.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(rejected.error()->message);
        emit stateChanged();
        return false;
    }

    refresh();
    emit durableStateChanged(selectedRunId_);
    return true;
}
