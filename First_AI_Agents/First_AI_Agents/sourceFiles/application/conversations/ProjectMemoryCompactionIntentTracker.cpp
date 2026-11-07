#include "ai/application/conversations/ProjectMemoryCompactionIntentTracker.hpp"

#include "ai/application/conversations/ProjectMemoryCompactionAccounting.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"

#include <utility>

namespace ai::application {

namespace {

namespace accounting = compaction_accounting;

// An observation whose source is the pair read: the four findings of a read that succeeded. NotObserved and Unreadable observed
// nothing, whatever the layer below says about its source.
bool isFindingOfARead(CompactionIntentObservation observation) noexcept
{
    switch (observation) {
    case CompactionIntentObservation::IntentPresent:
    case CompactionIntentObservation::FinalRecordPresent:
    case CompactionIntentObservation::NeitherPresent:
    case CompactionIntentObservation::Inconsistent:
        return true;
    case CompactionIntentObservation::NotObserved:
    case CompactionIntentObservation::Unreadable:
        break;
    }
    return false;
}

} // namespace

CompactionIntentTracker::CompactionIntentTracker(
    IProjectMemoryCompactionIntentRepository& intents, const ProjectMemoryCompactionRecord& prepared)
    : intents_(intents), expected_(compactionIntentIdentityOf(prepared)), intent_(makeCompactionIntent(prepared))
{
}

CompactionIntentTracker::Admission CompactionIntentTracker::admit()
{
    Admission admission;
    if (attempted_) {
        // One tracker is one admission: a repeated request writes nothing and never permits a second request.
        ProjectMemoryCompactionOutcome refused;
        refused.result = common::Result<ProjectMemoryCompactionRecord>::failure(compactionIntentRefusalError({}));
        refused.intent = trace_;
        admission.refusal = std::move(refused);
        return admission;
    }
    attempted_ = true;

    const CompactionIntentWriteOutcome write = intents_.recordIntentObserved(intent_);
    trace_.admission = compactionIntentAdmissionOf(write);
    trace_.writeState = write.persist.state;
    trace_.writeNative = write.persist.primaryNative;

    // The storage's own error stays as its own cause; the call's result (below) is the typed refusal. Then the write's secondary causes.
    if (!write.persist.result.success()) {
        accounting::appendDiagnostic(admissionCauses_, admissionCausesIncomplete_,
            CompactionDiagnostic {CompactionDiagnosticStage::IntentWrite, *write.persist.result.error()});
    }
    accounting::appendDiagnostics(admissionCauses_, admissionCausesIncomplete_, inIntentWritePhase(write.persist.diagnostics));
    if (write.persist.diagnosticsIncomplete) admissionCausesIncomplete_ = true;

    if (mayDispatch(write)) {
        admission.dispatch = true;
        return admission;
    }

    if (write.persist.result.success()) {
        // No error, yet no confirmed new row either: say so, instead of leaving a refusal without a cause.
        accounting::appendDiagnostic(admissionCauses_, admissionCausesIncomplete_,
            CompactionDiagnostic {CompactionDiagnosticStage::IntentWrite,
                common::Error {"project_memory.intent_admission_unconfirmed",
                    "The intent write reported no error but did not confirm a new committed row"}});
    }

    // The refused call: no request, no final record, no accounting fallback, no usage. Its result is the typed refusal.
    ProjectMemoryCompactionOutcome refused;
    refused.result = common::Result<ProjectMemoryCompactionRecord>::failure(compactionIntentRefusalError(write));
    refused.connection = write.persist.connection;
    conclude(refused, /*finalWriteConfirmed=*/false);
    admission.refusal = std::move(refused);
    return admission;
}

void CompactionIntentTracker::conclude(ProjectMemoryCompactionOutcome& outcome, bool finalWriteConfirmed)
{
    if (concluded_) return;
    concluded_ = true;
    if (trace_.admission == CompactionIntentAdmission::NotApplicable) {
        outcome.intent = trace_;
        return;
    }

    // In the order the causes happened: the admission write first, then what the final writes reported, then the proof.
    std::vector<CompactionDiagnostic> merged;
    bool incomplete = admissionCausesIncomplete_ || outcome.diagnosticsIncomplete;
    accounting::appendDiagnostics(merged, incomplete, admissionCauses_);
    accounting::appendDiagnostics(merged, incomplete, outcome.diagnostics);

    if (finalWriteConfirmed && trace_.admission == CompactionIntentAdmission::Admitted) {
        // The matching final record was committed and its trigger closed the intent in the same transaction: derived from the
        // confirmed write, not read.
        trace_.observation = CompactionIntentObservation::FinalRecordPresent;
        trace_.source = CompactionIntentObservationSource::ConfirmedFinalWrite;
    } else if (compactionIntentNeedsProof(trace_.admission, finalWriteConfirmed)) {
        const CompactionIntentObservationOutcome proof = intents_.observeOperation(expected_);
        trace_.observation = proof.observation;
        trace_.source = isFindingOfARead(proof.observation) ? CompactionIntentObservationSource::AtomicPairRead
                                                            : CompactionIntentObservationSource::None;
        trace_.proofNative = proof.primaryNative;
        if (proof.connection.has_value()) outcome.connection = proof.connection;   // the last health actually observed

        if (proof.error.has_value()
            && (proof.observation == CompactionIntentObservation::Unreadable
                || proof.observation == CompactionIntentObservation::Inconsistent)) {
            accounting::appendDiagnostic(merged, incomplete,
                CompactionDiagnostic {CompactionDiagnosticStage::IntentProofRead, *proof.error});
        } else if (proof.observation == CompactionIntentObservation::Inconsistent) {
            accounting::appendDiagnostic(merged, incomplete,
                CompactionDiagnostic {CompactionDiagnosticStage::IntentProofRead,
                    common::Error {kIntentObservationInconsistentCode, "The intent and the final record of the operation contradict each other"}});
        }
        accounting::appendDiagnostics(merged, incomplete, inIntentProofPhase(proof.diagnostics));
        if (proof.diagnosticsIncomplete) incomplete = true;
    }

    outcome.diagnostics = std::move(merged);
    outcome.diagnosticsIncomplete = incomplete;
    outcome.intent = trace_;
}

} // namespace ai::application
