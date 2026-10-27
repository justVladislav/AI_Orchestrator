#pragma once

#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/verification/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/verification/ManifestVerificationContinuationTypes.hpp"

namespace ai::application {

// MW-P5.1: durable, fail-closed admission of a terminal Manifest Run's
// combined-verification failure for a later (MW-P5.3+) bounded continuation.
// Reads only already-durable repositories - never calls a provider, never
// mutates the source Run's Manifest/scope/proposals/Work Ledger entry, and
// never writes anything beyond its own checkpoint row.
class ManifestVerificationContinuationService final {
public:
    ManifestVerificationContinuationService(
        IRunRepository& runs,
        IManifestWorkLedgerRepository& workLedger,
        IChangeProposalRepository& changeProposals,
        IManifestVerificationContinuationRepository& continuations);

    // Read-only form used by MW-P5.5's UI projection. It applies the exact
    // same Backend admission policy as admitOrEvaluate(), but never creates a
    // checkpoint. An already durable checkpoint remains authoritative.
    [[nodiscard]] common::Result<ManifestVerificationContinuationCheckpoint>
    evaluate(const RunId& sourceRunId) const;

    // Idempotent: a checkpoint already persisted for sourceRunId (Admitted
    // or Denied) is returned as-is, never re-evaluated or duplicated.
    // Otherwise evaluates eligibility fresh against already-durable data and
    // persists exactly one new checkpoint - Admitted with denialCode/
    // denialMessage empty, or Denied with them set to one of this class's
    // stable "verification_continuation.*" codes (see the .cpp for the
    // full list and the exact clause each corresponds to).
    [[nodiscard]] common::Result<ManifestVerificationContinuationCheckpoint>
    admitOrEvaluate(const RunId& sourceRunId);

private:
    IRunRepository& runs_;
    IManifestWorkLedgerRepository& workLedger_;
    IChangeProposalRepository& changeProposals_;
    IManifestVerificationContinuationRepository& continuations_;
};

} // namespace ai::application
