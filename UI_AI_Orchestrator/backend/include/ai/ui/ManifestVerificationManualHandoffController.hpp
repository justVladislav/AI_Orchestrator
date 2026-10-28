#pragma once

#include <ai/application/proposals/ChangeProposalService.hpp>
#include <ai/application/proposals/ChangeSetApplyService.hpp>
#include <ai/application/verification/ManifestApplyVerificationGate.hpp>
#include <ai/application/verification/ManifestVerificationManualHandoffReadModelService.hpp>
#include <ai/application/verification/ManifestVerificationManualHandoffService.hpp>
#include <ai/application/proposals/ProposalReviewService.hpp>

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>
#include <stop_token>

/**
 * QML-P4R.5 package 2: the Qt adapter for the free, user-authored manual
 * lint-repair handoff - a dedicated controller (never merged into
 * ManifestController) mirroring package 1's own decision to keep
 * ManifestVerificationManualHandoffReadModelService a separate Backend
 * service rather than folding it into ExecutionManifestReadModelService.
 *
 * Every property below is a direct, unmodified projection of
 * ManifestVerificationManualHandoffView - this controller never computes
 * authority, lineage, or eligibility itself, and never interprets an
 * errorText string as semantics (QML must not either). isApplicable is
 * false whenever the selected Run is outside the two anchor workflows
 * materialize() itself ever accepts - the whole manual-handoff surface must
 * stay hidden then, distinct from isApplicable && !hasHandoff (an eligible
 * Run with no handoff yet, where Create is the only offered action).
 *
 * Async and cancellable, mirroring ManifestController's own
 * prepare/repair-continuation pattern exactly: create()/reverify()/discard()
 * each run the real Backend call (materialize()/importAndReverify()/
 * discardHandoff(), all of which may invoke real, already-local qmllint/
 * cmake commands and therefore take genuine wall-clock time) via
 * QtConcurrent::run with a std::stop_source, share one busy flag and one
 * QFutureWatcher (only one such command against a handoff makes sense at a
 * time), and on completion ALWAYS re-read every field from the durable
 * read-model view - the worker's own result is notification data only,
 * never the UI's source of truth, so no auto-start of work ever happens
 * from a read, a reload, or a file-watcher.
 *
 * openCandidateLocation() and copyCandidatePathToClipboard() are the two
 * exceptions: they resolve the real absolute candidate directory path
 * (ManifestVerificationManualHandoffService::resolveCandidateRootForAnchor,
 * the ONE place this service ever hands out a real path) and act on it
 * entirely in C++ (QDesktopServices::openUrl / QGuiApplication::clipboard())
 * - the path itself never reaches QML as a string, matching
 * RunController::openArtifactsFolder()'s own established precedent. Both
 * are synchronous: resolving a handoff's already-known storage key is a
 * fast, local repository lookup, never real verification work, so neither
 * needs the async/cancellable treatment the other three commands do.
 *
 * Every error surfaced through errorText is a bounded, secret-free "code:
 * message" string reusing the exact Backend Result::Error - never an
 * absolute path, file content, or provider payload, matching every other
 * controller in this codebase.
 *
 * Post-P4R.6 human review/Apply hardening: reviewMembers/selectMember/
 * readOriginalContent/readProposedContent/approveAndApplyMember/rejectMember
 * give a manual successor - which never has an AgentToolSession - the same
 * exact-member human review and explicit per-proposal Apply/Reject that
 * AgentSessionController already provides for a session-backed Run. Every
 * one of those calls is a direct pass-through to the SAME production
 * application services AgentSessionController itself holds
 * (ChangeProposalService for content/detail, ChangeSetApplyService::approve/
 * apply, ProposalReviewService::assertNotRejected/reject,
 * ManifestApplyVerificationGate::evaluate() for AG-16) - never a second,
 * independently-maintained copy of AG-07/AG-16 logic, and never a
 * synthesized AgentToolSession. approveAndApplyMember()/rejectMember() are
 * deliberately SYNCHRONOUS (matching AgentSessionController::
 * approveAndApply()/rejectProposal() precedent exactly): they are a handful
 * of fast file writes/DB rows, never a real local build/lint/test
 * subprocess the way create()/reverify() are. Both re-read the durable read
 * model via refresh() on completion, so a stale QML selection is never
 * itself authorization - the Backend's own approve()/apply() calls
 * re-evaluate ManifestApplyVerificationGate immediately before mutating
 * anything, exactly as they already do for AgentSessionController.
 */
class ManifestVerificationManualHandoffController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY stateChanged)
    Q_PROPERTY(bool isApplicable READ isApplicable NOTIFY stateChanged)
    Q_PROPERTY(QString verificationAnchorRunId READ verificationAnchorRunId NOTIFY stateChanged)
    Q_PROPERTY(QString verificationAnchorStatus READ verificationAnchorStatus NOTIFY stateChanged)
    Q_PROPERTY(bool hasHandoff READ hasHandoff NOTIFY stateChanged)
    Q_PROPERTY(QString handoffId READ handoffId NOTIFY stateChanged)
    Q_PROPERTY(QString originManifestRunId READ originManifestRunId NOTIFY stateChanged)
    Q_PROPERTY(QString originManifestStatus READ originManifestStatus NOTIFY stateChanged)
    Q_PROPERTY(QString candidateHealth READ candidateHealth NOTIFY stateChanged)
    Q_PROPERTY(QStringList allowedRelativePaths READ allowedRelativePaths NOTIFY stateChanged)
    // Provider-free reset hardening: the current candidate's own diff against
    // its immutable original snapshot, direct/unmodified from
    // ManifestVerificationManualHandoffView::inspectCandidateEdits()'s own
    // projection. QML never derives this itself - it only decides whether to
    // offer "Reset non-editable changes" from resetRequiredRelativePaths
    // being non-empty.
    Q_PROPERTY(QStringList editableChangedRelativePaths
        READ editableChangedRelativePaths NOTIFY stateChanged)
    Q_PROPERTY(QStringList resetRequiredRelativePaths
        READ resetRequiredRelativePaths NOTIFY stateChanged)
    // Set only after a successful resetNonEditableChanges() call, cleared by
    // every other command and by selectRun()/refresh() picking a different
    // Run - mirrors applyResultText's own one-shot notification discipline.
    Q_PROPERTY(QString resetResultText READ resetResultText NOTIFY stateChanged)
    // What the verification anchor Run itself failed on, restricted to this
    // handoff's editable scope - available before any Reverify attempt
    // exists. Direct projection of the read model's initialScopeDiagnostics.
    Q_PROPERTY(QStringList initialScopeDiagnostics
        READ initialScopeDiagnostics NOTIFY stateChanged)
    // The recorded findings of the failed verification, WITHOUT the editable-scope filter and available
    // before a handoff exists (REMAIN_TO_DO 6a (b)+(c)). Direct projection of the read model's
    // recordedFindings, never reclassified here or in QML. Keys: hasFindings (bool), findings (list of
    // {text, severity, relativePath, authority, diagnosticCode, line, column, tag}), debtCount, verdict,
    // verdictCode, verdictMessage, outsideScopePaths (list), classificationErrorCode/Message. Not
    // authority for Apply, Create or Reverify.
    Q_PROPERTY(QVariantMap recordedFindings READ recordedFindings NOTIFY stateChanged)
    Q_PROPERTY(QVariantList attempts READ attempts NOTIFY stateChanged)
    Q_PROPERTY(bool hasFinalVerification READ hasFinalVerification NOTIFY stateChanged)
    Q_PROPERTY(QString finalProposalSafetyStatus READ finalProposalSafetyStatus NOTIFY stateChanged)
    Q_PROPERTY(QString finalBuildLintTestStatus READ finalBuildLintTestStatus NOTIFY stateChanged)
    Q_PROPERTY(QString finalAcceptanceStatus READ finalAcceptanceStatus NOTIFY stateChanged)
    Q_PROPERTY(bool canApply READ canApply NOTIFY stateChanged)
    Q_PROPERTY(QString applyReasonCode READ applyReasonCode NOTIFY stateChanged)
    Q_PROPERTY(QString applyReasonMessage READ applyReasonMessage NOTIFY stateChanged)
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY stateChanged)
    Q_PROPERTY(QString createReasonCode READ createReasonCode NOTIFY stateChanged)
    Q_PROPERTY(QString createReasonMessage READ createReasonMessage NOTIFY stateChanged)
    // Post-P4R.6 human review/Apply hardening: the authoritative attempt's
    // exact verified proposal set, one entry per member (empty whenever
    // hasFinalVerification is false). Each entry is a QVariantMap with the
    // same field shape selectedMemberDetail below uses, minus its own
    // "changes"/"findings" arrays' full detail already present here too -
    // see populate()'s own doc comment for the exact keys.
    Q_PROPERTY(QVariantList reviewMembers READ reviewMembers NOTIFY stateChanged)
    Q_PROPERTY(QString selectedMemberProposalId READ selectedMemberProposalId NOTIFY stateChanged)
    // The selected member's full detail (changes/findings/apply eligibility/
    // review-rejection state) - a direct copy of its own reviewMembers entry,
    // never independently recomputed. Empty QVariantMap when nothing is
    // selected.
    Q_PROPERTY(QVariantMap selectedMemberDetail READ selectedMemberDetail NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    // A successful Apply is distinct from errorText: workspace writes may
    // already be durable even when the best-effort post-Apply provenance
    // artifact failed. Preserve and surface that warning without claiming
    // that Apply rolled back (ApplyChangeSetResult's explicit contract).
    Q_PROPERTY(QString applyResultText READ applyResultText NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)
    Q_PROPERTY(QString activeCommandRunId READ activeCommandRunId NOTIFY stateChanged)

public:
    // proposals/applyService/reviewService/verificationGate are the SAME
    // production application-service instances AgentSessionController
    // itself holds (BackendRuntime composes exactly one of each) - never a
    // second instance, never a duplicated AG-07/AG-16 decision.
    explicit ManifestVerificationManualHandoffController(
        ai::application::ManifestVerificationManualHandoffReadModelService& readModel,
        ai::application::ManifestVerificationManualHandoffService& manualHandoffService,
        ai::application::ChangeProposalService& proposals,
        ai::application::ChangeSetApplyService& applyService,
        ai::application::ProposalReviewService& reviewService,
        ai::application::ManifestApplyVerificationGate& verificationGate,
        QObject* parent = nullptr);
    ~ManifestVerificationManualHandoffController() override;

    [[nodiscard]] QString selectedRunId() const { return selectedRunId_; }
    [[nodiscard]] bool isApplicable() const noexcept { return isApplicable_; }
    [[nodiscard]] QString verificationAnchorRunId() const { return verificationAnchorRunId_; }
    [[nodiscard]] QString verificationAnchorStatus() const { return verificationAnchorStatus_; }
    [[nodiscard]] bool hasHandoff() const noexcept { return hasHandoff_; }
    [[nodiscard]] QString handoffId() const { return handoffId_; }
    [[nodiscard]] QString originManifestRunId() const { return originManifestRunId_; }
    [[nodiscard]] QString originManifestStatus() const { return originManifestStatus_; }
    [[nodiscard]] QString candidateHealth() const { return candidateHealth_; }
    [[nodiscard]] QStringList allowedRelativePaths() const { return allowedRelativePaths_; }
    [[nodiscard]] QStringList editableChangedRelativePaths() const
    {
        return editableChangedRelativePaths_;
    }
    [[nodiscard]] QStringList resetRequiredRelativePaths() const
    {
        return resetRequiredRelativePaths_;
    }
    [[nodiscard]] QString resetResultText() const { return resetResultText_; }
    [[nodiscard]] QStringList initialScopeDiagnostics() const { return initialScopeDiagnostics_; }
    [[nodiscard]] QVariantMap recordedFindings() const { return recordedFindings_; }
    [[nodiscard]] QVariantList attempts() const { return attempts_; }
    [[nodiscard]] bool hasFinalVerification() const noexcept { return hasFinalVerification_; }
    [[nodiscard]] QString finalProposalSafetyStatus() const { return finalProposalSafetyStatus_; }
    [[nodiscard]] QString finalBuildLintTestStatus() const { return finalBuildLintTestStatus_; }
    [[nodiscard]] QString finalAcceptanceStatus() const { return finalAcceptanceStatus_; }
    [[nodiscard]] bool canApply() const noexcept { return canApply_; }
    [[nodiscard]] QString applyReasonCode() const { return applyReasonCode_; }
    [[nodiscard]] QString applyReasonMessage() const { return applyReasonMessage_; }
    [[nodiscard]] bool canCreate() const noexcept { return canCreate_; }
    [[nodiscard]] QString createReasonCode() const { return createReasonCode_; }
    [[nodiscard]] QString createReasonMessage() const { return createReasonMessage_; }
    [[nodiscard]] QVariantList reviewMembers() const { return reviewMembers_; }
    [[nodiscard]] QString selectedMemberProposalId() const { return selectedMemberProposalId_; }
    [[nodiscard]] QVariantMap selectedMemberDetail() const { return selectedMemberDetail_; }
    [[nodiscard]] QString errorText() const { return errorText_; }
    [[nodiscard]] QString applyResultText() const { return applyResultText_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool canCancel() const noexcept
    {
        return busy_ && activeCommand_ && !activeCommand_->stopSource.stop_requested();
    }
    [[nodiscard]] QString activeCommandRunId() const
    {
        return activeCommand_ ? activeCommand_->runId : QString();
    }

    // Re-reads the read model for a newly selected Run. Never starts any
    // command - a read never spends work. Clears every field (isApplicable
    // included) with no error when runId is empty.
    Q_INVOKABLE bool selectRun(const QString& runId);
    // Re-reads the read model for the currently selected Run. Same call
    // finishCommand() itself makes after every async command completes, so
    // this is also the "manual reload" action for the currently open panel.
    Q_INVOKABLE bool refresh();

    // Materializes a new handoff for the selected Run. Refused synchronously
    // (no busy transition) when a handoff already exists or canCreate is
    // false - QML must never offer this action itself when canCreate is
    // false, but this guard exists so an already-stale QML state can never
    // spend real work either.
    Q_INVOKABLE bool create();
    // Re-verifies the current handoff's live (possibly user-edited)
    // candidate against its immutable original contract. Refused
    // synchronously when no handoff exists yet.
    Q_INVOKABLE bool reverify();
    // Discards the current handoff. discardHandoff() itself is the sole
    // authority on whether an active attempt blocks this - this call never
    // pre-guesses that client-side, only faithfully surfaces whatever the
    // Backend denies. Refused synchronously when no handoff exists yet.
    Q_INVOKABLE bool discard();
    // Provider-free reset hardening: restores every non-editable candidate
    // drift (a forbidden edit, a brand-new path, a deleted frozen path) from
    // the pristine frozen candidate, while preserving every authorized edit
    // byte-for-byte. Refused synchronously (no busy transition) when no
    // handoff exists, or resetRequiredRelativePaths is already empty -
    // resetNonEditableChanges() itself is the sole authority on the exact
    // set; this guard only avoids spending a real filesystem pass on a
    // stale QML state that has nothing to reset. Never edits the live
    // project, never calls a provider/Planner, never creates an attempt Run.
    Q_INVOKABLE bool resetNonEditableChanges();
    // Requests cancellation of whichever of create()/reverify()/discard()/
    // resetNonEditableChanges() is currently running. False when nothing is
    // busy or cancellation was already requested; canCancel is the direct
    // QML-facing answer.
    Q_INVOKABLE bool cancel();

    // Opens the current handoff's real candidate directory in the OS file
    // explorer. The absolute path is resolved and consumed entirely in C++;
    // QML never receives it. False (with errorText set) when no handoff
    // exists yet, the path fails containment, or no desktop application
    // could open it.
    Q_INVOKABLE bool openCandidateLocation();
    // Copies the current handoff's real candidate directory path to the OS
    // clipboard, for a user who prefers to paste it into their own editor's
    // "Open Folder" dialog rather than rely on openCandidateLocation()'s OS
    // file-explorer launch. Same resolution/safety discipline as
    // openCandidateLocation().
    Q_INVOKABLE bool copyCandidatePathToClipboard();

    // Post-P4R.6 human review/Apply hardening: selects one authoritative
    // review member by its real proposal id. Re-resolves fresh against
    // readModel_ (never the cached reviewMembers_, which may already be
    // stale) - an unknown, stale, or superseded id fails with
    // "manual_handoff.review_member_not_found" rather than silently
    // clearing or guessing. Pass an empty string to clear the selection.
    Q_INVOKABLE bool selectMember(const QString& proposalId);
    // Lazily fetches one file's original/proposed content for a diff
    // preview - the same typed (proposalId, changeId) pair-keyed read
    // ChangeProposalService::readOriginalContent/readProposedContent
    // already provides AgentSessionController; QML never receives or
    // constructs a filesystem path. proposalId must resolve fresh against
    // the CURRENT authoritative review set (resolveFreshAuthoritativeMember)
    // before either call reaches ChangeProposalService - an arbitrary typed
    // id belonging to some other proposal/handoff/Run is refused here, never
    // merely relied on QML "not offering" it. Returns an empty string on
    // failure; check errorText.
    Q_INVOKABLE QString readOriginalContent(const QString& proposalId, const QString& changeId);
    Q_INVOKABLE QString readProposedContent(const QString& proposalId, const QString& changeId);
    // UI-facing "Approve and apply" for one authoritative review member:
    // proposalId is first re-resolved fresh against the CURRENT
    // authoritative review set (never the cached reviewMembers_), then
    // ManifestApplyVerificationGate::evaluate() is re-checked directly here
    // - defense-in-depth, mirroring AG-15's own diagnostic-read precedent:
    // ChangeSetApplyService::approve()/apply() themselves unconditionally
    // re-run the identical AG-16 evaluation immediately before mutating
    // anything and remain the sole real authority, so this earlier check
    // never replaces that; it only avoids recording a durable
    // ApprovalRecord for a proposal already known to be ineligible. Then
    // ProposalReviewService::assertNotRejected(), then
    // ChangeSetApplyService::approve()/apply() - the identical sequence
    // AgentSessionController::approveAndApply() already uses. Refuses
    // synchronously with "manual_handoff.review_member_not_found" (stale/
    // unknown proposal) or the gate's own reasonCode (ineligible) before any
    // mutation. Always calls refresh() on success so the panel re-renders
    // durable state - never the worker's own in-memory result.
    Q_INVOKABLE bool approveAndApplyMember(const QString& proposalId);
    // UI-facing "Reject / discard" for one authoritative review member -
    // proposalId is first re-resolved fresh against the CURRENT
    // authoritative review set (same rule as approveAndApplyMember above),
    // then ProposalReviewService::reject(), identical to
    // AgentSessionController::rejectProposal(). A currently AG-16-ineligible
    // member may still be rejected (rejection never requires eligibility).
    Q_INVOKABLE bool rejectMember(const QString& proposalId, const QString& reason);

signals:
    void stateChanged();
    // Emitted after every async command completes (success or failure). The
    // originating Run id is explicit because application selection may have
    // changed while the worker was running; QML must never attribute the
    // notification to whichever Run happens to be selected later. Every
    // durable field is already current on the properties above by the time
    // this fires.
    void commandFinished(const QString& commandRunId, bool succeeded);
    // Dirty notification only. Consumers must re-read their own Backend
    // projection; no command result is carried as authoritative state.
    void durableStateChanged(const QString& verificationAnchorRunId);

private:
    void clear();
    void populate(const ai::application::ManifestVerificationManualHandoffView& view);
    void finishCommand();
    // Post-P4R.6 human review/Apply hardening: builds one reviewMembers/
    // selectedMemberDetail-shaped QVariantMap from an already-loaded review
    // member view - the single place both call sites (populate() and
    // selectMember()) construct this shape from, so the two can never drift
    // apart.
    [[nodiscard]] static QVariantMap toReviewMemberVariant(
        const ai::application::ManifestVerificationManualHandoffReviewMemberView& member);
    // Finds proposalId's own entry in reviewMembers_ by its "proposalId"
    // key. Returns an empty (default-constructed) QVariantMap when absent -
    // callers must check isEmpty() before trusting the result, exactly like
    // every other "not found" case in this class. Reads only the already-
    // cached projection from the last refresh()/populate() - used solely by
    // populate() itself to preserve a selection across a reload. Never the
    // authorization boundary for a mutating command or a content read; see
    // resolveFreshAuthoritativeMember() below for that.
    [[nodiscard]] QVariantMap findReviewMember(const QString& proposalId) const;
    // Post-hardening: the sole authorization boundary for
    // readOriginalContent()/readProposedContent()/approveAndApplyMember()/
    // rejectMember()/selectMember(). Re-reads readModel_.viewForRun() fresh
    // (never the cached reviewMembers_, which may already be stale by the
    // time a command actually runs) and returns proposalId's own member from
    // THAT fresh result. Returns std::nullopt (with errorText_ set to a
    // stable "manual_handoff.review_member_not_found" code, or the exact
    // Backend read failure) when proposalId is empty, no Run is selected, no
    // handoff/authoritative attempt exists, or proposalId is not a member of
    // the CURRENT authoritative verified set - an arbitrary, stale,
    // superseded, or cross-handoff/cross-Run proposal id can never pass this
    // check merely because QML happened to submit it.
    [[nodiscard]] std::optional<ai::application::ManifestVerificationManualHandoffReviewMemberView>
    resolveFreshAuthoritativeMember(const QString& proposalId);

    struct ManualHandoffCommandOutcome {
        QString commandRunId;
        std::optional<ai::common::Error> error;
        // Set only by resetNonEditableChanges()'s own worker on success -
        // finishCommand() copies this into resetResultText_ after the
        // authoritative refresh() below, exactly like approveAndApplyMember()
        // sets applyResultText_ from ApplyChangeSetResult. Every other
        // command leaves this unset.
        std::optional<QString> resetSummary;
    };
    struct ActiveCommand {
        QString runId;
        std::stop_source stopSource;
    };

    ai::application::ManifestVerificationManualHandoffReadModelService& readModel_;
    ai::application::ManifestVerificationManualHandoffService& manualHandoffService_;
    ai::application::ChangeProposalService& proposals_;
    ai::application::ChangeSetApplyService& applyService_;
    ai::application::ProposalReviewService& reviewService_;
    ai::application::ManifestApplyVerificationGate& verificationGate_;

    QString selectedRunId_;
    bool isApplicable_ {false};
    QString verificationAnchorRunId_;
    QString verificationAnchorStatus_;
    bool hasHandoff_ {false};
    QString handoffId_;
    QString originManifestRunId_;
    QString originManifestStatus_;
    QString candidateHealth_;
    QStringList allowedRelativePaths_;
    QStringList editableChangedRelativePaths_;
    QStringList resetRequiredRelativePaths_;
    QString resetResultText_;
    QStringList initialScopeDiagnostics_;
    QVariantMap recordedFindings_;
    QVariantList attempts_;
    bool hasFinalVerification_ {false};
    QString finalProposalSafetyStatus_;
    QString finalBuildLintTestStatus_;
    QString finalAcceptanceStatus_;
    bool canApply_ {false};
    QString applyReasonCode_;
    QString applyReasonMessage_;
    bool canCreate_ {false};
    QString createReasonCode_;
    QString createReasonMessage_;
    QVariantList reviewMembers_;
    QString selectedMemberProposalId_;
    QVariantMap selectedMemberDetail_;
    QString errorText_;
    QString applyResultText_;
    bool busy_ {false};
    // One pointer, as in the original package-2 layout: Run identity and
    // stop source live together so async identity hardening does not enlarge
    // the already stack-heavy BackendBridge object.
    std::shared_ptr<ActiveCommand> activeCommand_;
    QFutureWatcher<ManualHandoffCommandOutcome> commandWatcher_;
};
