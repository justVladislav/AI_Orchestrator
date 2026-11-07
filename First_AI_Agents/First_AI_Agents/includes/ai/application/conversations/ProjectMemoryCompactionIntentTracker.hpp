#pragma once

#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntent.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentOutcome.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionOutcome.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// B3-II-A: the intent of ONE compaction call, from admission to observation. The service owns the sequence of the operation; this
// component owns what the intent port is asked and what its answers mean:
//   admit()    exactly ONE recordIntentObserved, right before the provider request would be issued. The request is allowed only by
//              mayDispatch (a confirmed Committed NEW row of this call). Nothing is retried, nothing is compensated, no final or
//              accounting record is written for a refusal, and no usage is invented.
//   conclude() after every final write of the call: at most ONE observeOperation, and only when an admission was attempted and no
//              final write was confirmed Committed. A confirmed final write already closed the intent in the same transaction, so it is
//              recorded as such (source ConfirmedFinalWrite) without a read.
// It never writes a record, never calls the provider, has no SQL and never changes the result, the accounting or the observed record
// of the call: what it adds is the `intent` trace, its own causes (stages IntentWrite / IntentProofRead) and the last observed health.
//
// One tracker per call, used from one thread. It is built from the PREPARED record, so the intent and the final record share one
// identity (no second id, no second clock reading).
class CompactionIntentTracker final {
public:
    CompactionIntentTracker(IProjectMemoryCompactionIntentRepository& intents, const ProjectMemoryCompactionRecord& prepared);

    struct Admission {
        // true: the request of this call may be issued.
        bool dispatch {false};
        // Set exactly when `dispatch` is false: the COMPLETE outcome of the refused call (its result is the typed refusal, the
        // storage's own error is a separate IntentWrite cause, the trace and the single proof are already in it).
        std::optional<ProjectMemoryCompactionOutcome> refusal;
    };
    // Writes the intent. A second call writes nothing and refuses: one tracker is one admission.
    [[nodiscard]] Admission admit();

    // Completes the trace of an outcome whose final writes are done. `finalWriteConfirmed` is true only when the LAST final insert of
    // the call (the primary commit, the terminal insert or the accounting fallback) returned success AND Committed - never derived
    // from the call's result or from accounting == Saved (a Saved can also be an identical row found after a failed insert).
    // Idempotent: a second call changes nothing.
    void conclude(ProjectMemoryCompactionOutcome& outcome, bool finalWriteConfirmed);

private:
    IProjectMemoryCompactionIntentRepository& intents_;
    CompactionIntentIdentity expected_;
    ProjectMemoryCompactionIntent intent_;
    CompactionIntentTrace trace_;
    std::vector<CompactionDiagnostic> admissionCauses_;   // the admission write's own causes, already in the intent phase
    bool admissionCausesIncomplete_ {false};
    bool attempted_ {false};
    bool concluded_ {false};
};

} // namespace ai::application
