#include "ai/backend/StartupRecovery.hpp"

#include "ai/application/comparison/ComparisonService.hpp"
#include "ai/application/planning/FollowUpComposerProcessor.hpp"
#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/application/ManifestInterruptedTaskReconciler.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationManualHandoffService.hpp"
#include "ai/application/ManifestWorkLedgerService.hpp"
#include "ai/application/ModelExecutionService.hpp"
#include "ai/application/OrchestrationService.hpp"

#include <set>
#include <string>
#include <utility>

namespace ai::backend {

StartupRecoveryOutcome runStartupRecovery(const StartupRecoveryServices& services)
{
    StartupRecoveryOutcome outcome;

    const auto surface = [&outcome](common::Error error) {
        if (!outcome.recoveryError.has_value()) {
            outcome.recoveryError = std::move(error);
            return;
        }
        outcome.recoveryError->message += " | [" + error.code + "] " + error.message;
    };

    // Turns any run row a prior process left Queued/Running/Cancelling into a truthful
    // Interrupted state before the composition root returns, so the first read a caller performs
    // never observes a run as falsely still active. Its stages are settled first, then the Run.
    {
        const auto result = services.orchestration.recoverInterruptedRuns();
        if (!result.success()) outcome.recoveryError = result.error();
    }

    {
        if (services.followUpComposer != nullptr) {
            const auto result = services.followUpComposer->recoverAtStartup();
            if (!result.success()) {
                outcome.followUpRecoveryError = *result.error();
            } else {
                outcome.followUpToProcess = result.value().toProcess;
                const auto remember = [&outcome](const std::optional<common::Error>& error) {
                    if (!error.has_value()) return;
                    if (!outcome.followUpRecoveryError.has_value()) {
                        outcome.followUpRecoveryError = *error;
                    } else {
                        outcome.followUpRecoveryError->message += " | [" + error->code + "] " + error->message;
                    }
                };
                remember(result.value().accountingError);
                remember(result.value().executionError);
            }
        }
    }

    {
        const auto result = services.modelExecutions.recoverInterruptedExecutions();
        if (!result.success()) outcome.modelExecutionRecoveryError = result.error();
    }

    // AG-17.7D-1: a task a crash interrupted gets its honest `manifest.task_interrupted` row. It
    // runs AFTER the executions are settled (it proves the task from its
    // `model_execution.interrupted` row) and BEFORE the Work Ledger pass below (an entry is
    // immutable, so it must be written from the complete account). The pass finds its Runs from
    // persisted state - an Interrupted Manifest Run with no ledger entry yet - never from a list
    // handed over by the steps above, so a crash between any two steps is finished by the next
    // startup. A Run it could not complete is held back from the ledger pass.
    std::set<application::RunId> deferredLedgerRuns;
    {
        const auto interrupted = services.interruptedTasks.reconcile();
        if (!interrupted.success()) {
            surface(*interrupted.error());
        } else {
            deferredLedgerRuns = interrupted.value().unreconciled;
        }
    }

    if (services.comparison != nullptr) {
        const auto result = services.comparison->recoverInterruptedSessions();
        if (!result.success()) outcome.comparisonRecoveryError = result.error();
    }

    // Audit recovery: mark all Active sessions and all Requested/Running calls as Interrupted
    // before the composition root returns to its caller. This prevents any crash-survivor
    // session from appearing Active to a future tool executor, satisfying the fail-closed audit
    // requirement.
    {
        const auto sessions = services.agentToolSessions.interruptActiveSessions();
        if (!sessions.success()) {
            outcome.agentAuditRecoveryError = sessions.error();
        } else {
            const auto calls = services.agentToolAudits.interruptAllRunning();
            if (!calls.success()) outcome.agentAuditRecoveryError = calls.error();
        }
    }

    // AG-14.1: closes the gap left by a process crash between a Run's terminal status commit and
    // its Work Ledger write, or a best-effort ledger write that failed and was discarded at its
    // call site. Runs after the Run recovery above has turned every still-active row into a
    // truthful terminal state, so this pass sees every terminal Manifest Run, including ones this
    // very process just recovered. Non-fatal, but an incomplete pass is visible through
    // recoveryError instead of being silently discarded.
    {
        const auto entries = services.workLedger.reconcileMissingEntries(deferredLedgerRuns);
        if (!entries.success()) {
            surface(*entries.error());
        } else if (!entries.value().stillMissing.empty()) {
            surface(common::Error {
                "manifest_work_ledger.recovery_incomplete",
                "One or more terminal Manifest Runs are still missing Work Ledger entries"});
        }
    }

    // AG-14.2: the companion pass for review/apply transitions. Same non-fatal-but-visible
    // contract as the entries pass.
    {
        const auto transitions = services.workLedger.reconcileMissingTransitions();
        if (!transitions.success()) {
            surface(*transitions.error());
        } else if (!transitions.value().stillMissing.empty()) {
            surface(common::Error {
                "manifest_work_ledger.transition_recovery_incomplete",
                "One or more Manifest Runs are still missing Work Ledger review transitions"});
        }
    }

    if (services.continuationPreparation != nullptr) {
        const auto children = services.continuationPreparation->reconcileMissingChildren();
        if (!children.success()) surface(*children.error());
    }

    // QML-P4R.3: the cross-process counterpart to importAndReverify()'s own in-process crash
    // cleanup, run here so the very first caller read already reflects a fully reconciled
    // manual-handoff attempt matrix. A hard failure surfaces like every other pass; a non-empty
    // unmarkedMissingEvidenceProposalIds is ALSO surfaced - an orphan successor proposal left
    // without its missing-evidence annotation must never let startup report a clean recovery, or
    // it could later be mistaken for genuinely verified evidence.
    if (services.manualHandoff != nullptr) {
        const auto handoff = services.manualHandoff->reconcileInterruptedAttempts();
        if (!handoff.success()) {
            surface(*handoff.error());
        } else if (!handoff.value().unmarkedMissingEvidenceProposalIds.empty()) {
            surface(common::Error {
                "manual_handoff.recovery_incomplete",
                "One or more interrupted manual-handoff successor proposals could not be marked "
                "missing-evidence during startup reconciliation"});
        }

        // QML-P4R.3.5: bounded retention, run AFTER the reconciliation above - its own
        // eligibility checks (active attempt, Apply-authority) must see already-reconciled
        // Run/typed-result state, never a mid-crash projection. A hard failure surfaces through
        // the same recoveryError channel; running out of eligible candidates to evict is not one.
        const auto retention = services.manualHandoff->enforceRetention();
        if (!retention.success()) surface(*retention.error());
    }

    return outcome;
}

} // namespace ai::backend
