#pragma once

#include "ai/application/planning/FollowUpPlanRequest.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {
class ComparisonService;
class FollowUpComposerProcessor;
class IAgentToolAuditRepository;
class IAgentToolSessionRepository;
class ManifestInterruptedTaskReconciler;
class ManifestVerificationContinuationPreparationService;
class ManifestVerificationManualHandoffService;
class ManifestWorkLedgerService;
class ModelExecutionService;
class OrchestrationService;
} // namespace ai::application

namespace ai::backend {

// The services startup recovery works through. The first six are required: they carry the
// recovery of Runs, of unfinished model executions, of the tasks a crash interrupted, of agent
// tool sessions and calls and of the Work Ledger - everything a restart must settle before the
// first read. The last four are the composition root's wider reconciliation passes; a caller that
// does not own them (a narrower composition such as a restart test) passes null and that pass is
// not run. BackendRuntime passes every one of them.
struct StartupRecoveryServices {
    application::OrchestrationService& orchestration;
    application::ModelExecutionService& modelExecutions;
    application::ManifestInterruptedTaskReconciler& interruptedTasks;
    application::IAgentToolSessionRepository& agentToolSessions;
    application::IAgentToolAuditRepository& agentToolAudits;
    application::ManifestWorkLedgerService& workLedger;
    application::ComparisonService* comparison {nullptr};
    application::ManifestVerificationContinuationPreparationService* continuationPreparation {nullptr};
    application::ManifestVerificationManualHandoffService* manualHandoff {nullptr};
    // PC-5C-2b: when present, this owner settles follow-up requests and their
    // execution rows before generic execution recovery. Narrow legacy test
    // compositions may leave it null.
    application::FollowUpComposerProcessor* followUpComposer {nullptr};
};

// The error channels BackendRuntime exposes. Each pass is non-fatal to the others: a failed
// pass is recorded and the next one still runs.
struct StartupRecoveryOutcome {
    // Run recovery, then (appended as " | [code] message") every later reconciliation pass.
    std::optional<common::Error> recoveryError;
    std::optional<common::Error> modelExecutionRecoveryError;
    std::optional<common::Error> followUpRecoveryError;
    std::optional<common::Error> comparisonRecoveryError;
    std::optional<common::Error> agentAuditRecoveryError;
    // Recovery itself never calls a provider. These explicitly authorised,
    // unexpired requests are queued by the owner only after the shared pass
    // returns and the runner's hooks are installed.
    std::vector<application::FollowUpPlanRequestId> followUpToProcess;
};

// THE startup recovery sequence - one function, shared by BackendRuntime and by the tests that
// prove what a restart does, so the proof is of the production order and not of a copy of it:
//   1. OrchestrationService::recoverInterruptedRuns   - Queued/Running/Cancelling Runs ->
//      Interrupted; each Run's stages are settled FIRST (Running -> Failed, Pending -> Skipped),
//      one instant and message for the stages and the Run. It writes NO Work Ledger entry.
//   2. FollowUpComposerProcessor::recoverAtStartup - restores paid follow-up accounting,
//      interrupts no-response follow-up rows, and returns live authorised requests WITHOUT
//      calling a provider
//   3. ModelExecutionService::recoverInterruptedExecutions - other Pending/Running executions ->
//      Failed `model_execution.interrupted`; it deliberately skips source `follow_up`
//   4. ManifestInterruptedTaskReconciler::reconcile - the tasks a crash interrupted get a Failed
//      `manifest.task_interrupted` row, only where the Backend can prove which task it was. Found
//      from persisted state (an Interrupted Manifest Run with no ledger entry), never from step 1's
//      list; a Run it cannot complete is deferred from step 6
//   5. ComparisonService::recoverInterruptedSessions
//   6. agent tool sessions Active -> Interrupted, then calls Requested/Running -> Interrupted
//   7. ManifestWorkLedgerService::reconcileMissingEntries (skipping the deferred Runs) - the
//      immutable entry is written from the complete account of steps 1-3
//   8. ManifestWorkLedgerService::reconcileMissingTransitions
//   9. ManifestVerificationContinuationPreparationService::reconcileMissingChildren
//  10. ManifestVerificationManualHandoffService::reconcileInterruptedAttempts
//  11. ManifestVerificationManualHandoffService::enforceRetention
// Steps 7-11 run AFTER the Runs are terminal, so they see every terminal Manifest Run, including
// the ones step 1 just recovered. Recovery never resumes anything: no provider session is created.
[[nodiscard]] StartupRecoveryOutcome runStartupRecovery(const StartupRecoveryServices& services);

} // namespace ai::backend
