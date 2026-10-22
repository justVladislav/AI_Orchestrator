#pragma once

#include "ai/application/IExecutionManifestRepository.hpp"
#include "ai/application/IManifestTaskExecutionRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IModelExecutionRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <set>
#include <vector>

namespace ai::application {

// The code of a task row startup recovery writes for a task whose provider session was running
// when the application died. It is NOT cancellation (the user did nothing), NOT an ordinary
// provider failure (the provider did not fail) and NOT `model_execution.orphaned` (which is the
// in-process safety net of a Run that ended while an execution was left open): it means exactly
// "this task was interrupted by the process ending".
inline constexpr const char* kManifestTaskInterruptedErrorCode = "manifest.task_interrupted";
inline constexpr const char* kManifestTaskInterruptedMessage =
    "The application stopped while this task was running; its work was not completed";

// AG-17.7D-1: gives a task a crash interrupted an honest, durable terminal row.
//
// A task's row is written by the runner when the task finishes, so a crash leaves the task that
// was running with NO row at all - indistinguishable, in the task rows and in the Work Ledger
// built from them, from a task that never started. After startup recovery has turned the Run
// Interrupted and its unfinished model executions `model_execution.interrupted`, this pass writes
// ONE Failed `manifest.task_interrupted` row for such a task - and only when the Backend can
// PROVE which task it was:
//   - the Run is Interrupted, a Manifest workflow Run, and has NO Work Ledger entry yet (an entry
//     is immutable history of an earlier policy and is never second-guessed or rewritten; its
//     absence is also the durable marker "this Run's reconciliation is unfinished", so a crash
//     between any two recovery steps is found again by the next startup - the pass depends on
//     persisted state only, never on a list handed over by an earlier step);
//   - the frozen Manifest exists and carries a valid routing record;
//   - the writer's own position allocation (allocateModelExecutionPositionsFromRecord, the same
//     code the member usage projection inverts) maps the execution's position to exactly ONE task;
//   - the execution is a Failed `model_execution.interrupted` (never orphaned, cancelled, or an
//     ordinary failure) carrying the provider and model of that task's frozen routing member;
//   - the task has no terminal row yet (Completed / Failed / Skipped).
// The lead's own Planner and integration rows are told apart from a worker's by POSITION (the
// lead's position; one past the last task slot), never by role: a worker's role is user-chosen text
// and may be "planner" or "integration".
// A task that never started has no execution, so it gets no row. A guess is never written.
//
// What the Backend cannot prove is NOT silently dropped. "Nothing to reconcile" (no Manifest, no
// interrupted execution, a Manifest frozen before routing records existed) is told apart from an
// INCOMPLETE account: an interrupted execution that cannot be attributed with certainty (a damaged
// routing record, no lead, a position allocation that cannot be computed, a position that is not
// exactly one task's slot, provider/model identities that disagree with the frozen member) means
// the Run's account is incomplete, and since the Work Ledger entry written next is immutable the
// Run is reported in `unreconciled` and held back from that pass, never frozen on a guess.
//
// Idempotent: a second pass finds every row already terminal and writes nothing.
class ManifestInterruptedTaskReconciler {
public:
    struct Outcome {
        // Task rows written by this pass.
        std::size_t materialized {0};
        // Interrupted Manifest Runs without a ledger entry whose account this pass could NOT
        // complete (a repository read or write failed, an execution is still unfinished, or an
        // interrupted execution cannot be attributed with certainty). Their
        // ledger entry must wait: it is immutable, so writing it now would freeze an incomplete
        // account. The next startup finds them again.
        std::set<RunId> unreconciled;
    };

    ManifestInterruptedTaskReconciler(
        IRunRepository& runs,
        IExecutionManifestRepository& manifests,
        IManifestTaskExecutionRepository& taskExecutions,
        IModelExecutionRepository& executions,
        IManifestWorkLedgerRepository& ledger);

    // Fails only when the Runs themselves cannot be listed.
    [[nodiscard]] common::Result<Outcome> reconcile();

private:
    IRunRepository& runs_;
    IExecutionManifestRepository& manifests_;
    IManifestTaskExecutionRepository& taskExecutions_;
    IModelExecutionRepository& executions_;
    IManifestWorkLedgerRepository& ledger_;
};

} // namespace ai::application
