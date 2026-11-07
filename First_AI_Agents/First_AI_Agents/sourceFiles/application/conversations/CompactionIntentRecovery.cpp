#include "ai/application/conversations/CompactionIntentRecovery.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"
#include "ai/application/text/DiagnosticText.hpp"

#include <string>
#include <utility>

namespace ai::application {

namespace {

namespace accounting = compaction_accounting;

std::chrono::system_clock::time_point wholeMilliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()));
}

bool sameMillisecond(std::chrono::system_clock::time_point a, std::chrono::system_clock::time_point b)
{
    return wholeMilliseconds(a) == wholeMilliseconds(b);
}

bool isConfirmedChange(const CompactionIntentMutationOutcome& outcome) noexcept
{
    return outcome.mutation == CompactionIntentMutation::Applied && outcome.persist.result.success()
        && outcome.persist.state == CompactionPersistState::Committed;
}

// One pass over the rows. It owns the report it builds; nothing else is written.
class Pass final {
public:
    Pass(IProjectMemoryCompactionIntentRepository& intents, std::chrono::system_clock::time_point instant)
        : intents_(intents), instant_(wholeMilliseconds(instant))
    {
    }

    CompactionIntentRecoveryReport report;

    void unavailable(common::Error cause, std::optional<CompactionNativeDetail> native = std::nullopt)
    {
        sourceUnavailable_ = true;
        report.status = CompactionIntentRecoveryStatus::Unavailable;
        remember(cause, std::move(native));
        note(std::move(cause));
    }

    void handle(const ProjectMemoryCompactionIntent& listed)
    {
        ++report.inspected;
        const CompactionIntentIdentity expected = compactionIntentIdentityOf(listed);
        const CompactionIntentObservationOutcome seen = intents_.observeOperation(expected);
        switch (seen.observation) {
        case CompactionIntentObservation::NotObserved:
        case CompactionIntentObservation::Unreadable:
            unconfirmed(seen.error.value_or(common::Error {kIntentRecoveryUnconfirmedCode,
                            "The operation could not be read; nothing is concluded about it"}),
                seen.primaryNative, &seen.diagnostics, seen.diagnosticsIncomplete);
            return;
        case CompactionIntentObservation::NeitherPresent:
            // The row was listed a moment ago and is gone now (a deleted project, or a lost write). That proves nothing about the
            // request, the closing or the cost.
            unconfirmed(common::Error {kIntentRecoveryUnconfirmedCode,
                "The intent was listed but neither it nor a final record was found; nothing is concluded about it"});
            return;
        case CompactionIntentObservation::FinalRecordPresent:
            return;   // the operation completed and its intent is gone: consistent, nothing to write, nothing to count
        case CompactionIntentObservation::IntentPresent:
            onIntentOnly(expected, seen);
            return;
        case CompactionIntentObservation::Inconsistent:
            onInconsistent(expected, seen);
            return;
        }
    }

    void finish()
    {
        if (sourceUnavailable_) return;
        report.status = (report.unconfirmed == 0 && report.conflicts == 0) ? CompactionIntentRecoveryStatus::Complete
                                                                           : CompactionIntentRecoveryStatus::Partial;
    }

private:
    void note(common::Error cause)
    {
        accounting::appendDiagnostic(report.diagnostics, report.diagnosticsIncomplete,
            CompactionDiagnostic {CompactionDiagnosticStage::IntentRecovery, std::move(cause)});
    }

    void noteAll(const std::vector<CompactionDiagnostic>* causes, bool incomplete)
    {
        if (causes != nullptr) accounting::appendDiagnostics(report.diagnostics, report.diagnosticsIncomplete, inIntentRecoveryPhase(*causes));
        if (incomplete) report.diagnosticsIncomplete = true;
    }

    void remember(const common::Error& cause, std::optional<CompactionNativeDetail> native)
    {
        if (report.firstError.has_value()) return;
        common::Error bounded = cause;
        if (bounded.message.size() > kMaxCompactionDiagnosticMessageBytes) {
            const std::string clean = toValidUtf8Diagnostic(bounded.message);
            bounded.message = clean.substr(0, utf8SafePrefixLength(clean, kMaxCompactionDiagnosticMessageBytes));
            report.diagnosticsIncomplete = true;
        }
        report.firstError = std::move(bounded);
        report.firstNative = std::move(native);
    }

    void unconfirmed(common::Error cause, std::optional<CompactionNativeDetail> native = std::nullopt,
        const std::vector<CompactionDiagnostic>* causes = nullptr, bool incomplete = false)
    {
        ++report.unconfirmed;
        remember(cause, std::move(native));
        note(std::move(cause));
        noteAll(causes, incomplete);
    }

    void conflict(common::Error cause)
    {
        ++report.conflicts;
        remember(cause, std::nullopt);
        note(std::move(cause));
    }

    // The error of a write that did not confirm, kept as a cause. It becomes the first error only if the row stays unconfirmed.
    void noteWriteCause(const CompactionIntentMutationOutcome& write)
    {
        if (!write.persist.result.success()) {
            note(*write.persist.result.error());
            rememberWriteCause(*write.persist.result.error(), write.persist.primaryNative);
        }
        noteAll(&write.persist.diagnostics, write.persist.diagnosticsIncomplete);
    }

    // The native detail of a write's error, bound to that error and bounded like every cause. Independent of the row's later outcome.
    void rememberWriteCause(const common::Error& cause, const std::optional<CompactionNativeDetail>& native)
    {
        if (!native.has_value()) return;
        if (report.writeCauses.size() >= kMaxCompactionDiagnostics) {
            report.diagnosticsIncomplete = true;
            return;
        }
        CompactionIntentRecoveryWriteCause entry {cause, *native};
        if (entry.cause.message.size() > kMaxCompactionDiagnosticMessageBytes) {
            const std::string clean = toValidUtf8Diagnostic(entry.cause.message);
            entry.cause.message = clean.substr(0, utf8SafePrefixLength(clean, kMaxCompactionDiagnosticMessageBytes));
            report.diagnosticsIncomplete = true;
        }
        if (entry.native.message.size() > kMaxCompactionDiagnosticMessageBytes) {
            const std::string clean = toValidUtf8Diagnostic(entry.native.message);
            entry.native.message = clean.substr(0, utf8SafePrefixLength(clean, kMaxCompactionDiagnosticMessageBytes));
            report.diagnosticsIncomplete = true;
        }
        report.writeCauses.push_back(std::move(entry));
    }

    common::Error writeError(const CompactionIntentMutationOutcome& write, const char* fallbackMessage) const
    {
        return write.persist.result.success() ? common::Error {kIntentRecoveryUnconfirmedCode, fallbackMessage} : *write.persist.result.error();
    }

    void onIntentOnly(const CompactionIntentIdentity& expected, const CompactionIntentObservationOutcome& seen)
    {
        if (!seen.intent.has_value()) {
            unconfirmed(common::Error {kIntentRecoveryUnconfirmedCode, "The intent was reported present but no row was returned"});
            return;
        }
        if (seen.intent->state == CompactionIntentState::Unresolved) {
            ++report.alreadyUnresolved;   // markedAt is never touched
            return;
        }
        const CompactionIntentMutationOutcome mark = intents_.markUnresolvedObserved(expected, instant_);
        if (isConfirmedChange(mark)) {
            ++report.markedUnresolved;
            noteAll(&mark.persist.diagnostics, mark.persist.diagnosticsIncomplete);
            return;
        }
        switch (mark.mutation) {
        case CompactionIntentMutation::AlreadyApplied:
            ++report.alreadyUnresolved;
            return;
        case CompactionIntentMutation::NotFound:
            unconfirmed(common::Error {kIntentRecoveryUnconfirmedCode, "The intent disappeared before it could be marked"},
                mark.persist.primaryNative, &mark.persist.diagnostics, mark.persist.diagnosticsIncomplete);
            return;
        case CompactionIntentMutation::IdentityConflict:
            conflict(common::Error {kIntentIdentityConflictCode, "The stored intent no longer carries the expected identity; nothing was changed"});
            noteAll(&mark.persist.diagnostics, mark.persist.diagnosticsIncomplete);
            return;
        case CompactionIntentMutation::Applied:       // reported applied without a confirmed Committed write: not a confirmation
        case CompactionIntentMutation::Unconfirmed:
            break;
        }

        // The write did not confirm. ONE more pair read may still show exactly the transition this pass asked for.
        noteWriteCause(mark);
        const CompactionIntentObservationOutcome proof = intents_.observeOperation(expected);
        if (proof.observation == CompactionIntentObservation::IntentPresent && proof.intent.has_value()
            && proof.intent->state == CompactionIntentState::Unresolved && proof.intent->unresolvedMarkedAt.has_value()
            && sameMillisecond(*proof.intent->unresolvedMarkedAt, instant_)) {
            ++report.markedUnresolved;   // confirmed by the read; the write's own error stays a cause
            return;
        }
        ++report.unconfirmed;
        remember(writeError(mark, "The change of the intent could not be confirmed"), mark.persist.primaryNative);
        if (proof.error.has_value()) note(*proof.error);
        noteAll(&proof.diagnostics, proof.diagnosticsIncomplete);
    }

    void onInconsistent(const CompactionIntentIdentity& expected, const CompactionIntentObservationOutcome& seen)
    {
        const auto intentDifference = seen.intent.has_value()
            ? compactionIntentIdentityDifference(expected, compactionIntentIdentityOf(*seen.intent))
            : std::optional<CompactionIntentIdentityField> {};
        const auto recordDifference = seen.recordIdentity.has_value()
            ? compactionIntentIdentityDifference(expected, *seen.recordIdentity)
            : std::optional<CompactionIntentIdentityField> {};

        // The one pair this pass may clean up: BOTH rows exist and BOTH carry exactly the expected identity.
        if (seen.intent.has_value() && seen.recordIdentity.has_value() && !intentDifference.has_value() && !recordDifference.has_value()) {
            // The observation itself is kept BEFORE the cleanup is tried, so it survives whatever the close turns out to be.
            note(common::Error {kIntentObservationInconsistentCode,
                "The intent and a final record with the expected identity were observed together; the closing of the intent is reported separately"});
            closeMatchingPair(expected);
            return;
        }
        if (intentDifference.has_value()) {
            conflict(common::Error {kIntentIdentityConflictCode,
                std::string("A stored intent differs from the expected operation (first difference: ") + toString(*intentDifference)
                    + "); nothing was changed"});
        } else if (recordDifference.has_value()) {
            conflict(common::Error {kIntentRecordConflictCode,
                std::string("The final record under this id differs from the intent (first difference: ") + toString(*recordDifference)
                    + "); nothing was changed"});
        } else {
            conflict(seen.error.value_or(common::Error {kIntentObservationInconsistentCode,
                "The intent and the final record of the operation contradict each other; nothing was changed"}));
        }
        noteAll(&seen.diagnostics, seen.diagnosticsIncomplete);
    }

    void closeMatchingPair(const CompactionIntentIdentity& expected)
    {
        const CompactionIntentMutationOutcome close = intents_.closeIntentObserved(expected);
        if (isConfirmedChange(close)) {
            ++report.closedConfirmed;
            noteAll(&close.persist.diagnostics, close.persist.diagnosticsIncomplete);
            return;
        }
        switch (close.mutation) {
        case CompactionIntentMutation::IdentityConflict:
            conflict(common::Error {kIntentIdentityConflictCode, "The stored intent no longer carries the expected identity; nothing was closed"});
            noteAll(&close.persist.diagnostics, close.persist.diagnosticsIncomplete);
            return;
        case CompactionIntentMutation::NotFound:
            unconfirmed(common::Error {kIntentRecoveryUnconfirmedCode, "The intent disappeared before it could be closed"},
                close.persist.primaryNative, &close.persist.diagnostics, close.persist.diagnosticsIncomplete);
            return;
        case CompactionIntentMutation::Applied:
        case CompactionIntentMutation::AlreadyApplied:
        case CompactionIntentMutation::Unconfirmed:
            break;
        }
        noteWriteCause(close);
        const CompactionIntentObservationOutcome proof = intents_.observeOperation(expected);
        if (proof.observation == CompactionIntentObservation::FinalRecordPresent) {
            ++report.closedConfirmed;   // the matching record and no intent: exactly the closing that was asked for
            return;
        }
        ++report.unconfirmed;
        remember(writeError(close, "The closing of the intent could not be confirmed"), close.persist.primaryNative);
        if (proof.error.has_value()) note(*proof.error);
        noteAll(&proof.diagnostics, proof.diagnosticsIncomplete);
    }

    IProjectMemoryCompactionIntentRepository& intents_;
    std::chrono::system_clock::time_point instant_;
    bool sourceUnavailable_ {false};
};

} // namespace

CompactionIntentRecovery::CompactionIntentRecovery(
    IProjectMemoryCompactionIntentRepository& intents, CompactionIntentRecoveryPermit permit, Clock clock)
    : intents_(intents), permit_(std::move(permit)), clock_(std::move(clock))
{
}

CompactionIntentRecoveryReport CompactionIntentRecovery::run()
{
    if (!permit_.permits()) {
        // Before any port is touched.
        Pass refused(intents_, std::chrono::system_clock::time_point {});
        refused.unavailable(common::Error {kIntentRecoveryForbiddenCode, kIntentRecoveryForbiddenMessage});
        return std::move(refused.report);
    }

    Pass pass(intents_, clock_ ? clock_() : std::chrono::system_clock::now());

    // Both lists first, strictly; a list that cannot be read is not replaced by a partial one, and nothing is changed before both are known.
    const auto open = intents_.listIntentsByState(CompactionIntentState::Open);
    if (!open.success()) {
        pass.unavailable(*open.error());
        return std::move(pass.report);
    }
    const auto unresolved = intents_.listIntentsByState(CompactionIntentState::Unresolved);
    if (!unresolved.success()) {
        pass.unavailable(*unresolved.error());
        return std::move(pass.report);
    }

    for (const auto& intent : open.value()) pass.handle(intent);
    for (const auto& intent : unresolved.value()) pass.handle(intent);
    pass.finish();
    return std::move(pass.report);
}

} // namespace ai::application
