#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/verification/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/ManifestWorkLedgerTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

// QML-P4R.4/P4R.5: the ONE shared Backend resolver for "which single
// attempt (if any) currently backs Apply-authority for a manual handoff" -
// used by BOTH ManifestApplyVerificationGate (AG-16, the real Apply
// decision - see its own evaluateManualHandoffEligibility) and
// ManifestVerificationManualHandoffReadModelService (the read-only UI
// projection). Never reimplemented a second time - the exact rule lives
// only in resolveManualHandoffApplyAuthority() below.
struct ManifestVerificationManualHandoffApplyAuthority {
    ManifestVerificationManualHandoffAttempt attempt;
    Run run;
    ManifestWorkLedgerEntry ledgerEntry;
};

// QML-P4R.4: a handoff has one Apply authority, never one independently
// selected attempt per proposal. Iterates attempts NEWEST ordinal first;
// the first one whose Work Ledger entry is terminalStatus == Completed
// wins and is returned immediately - older attempts (even other Completed
// ones) are never consulted once a newer authority is found. The Work
// Ledger is the ultimate authority, never the typed
// ManifestVerificationManualHandoffAttemptResult projection alone: an
// attempt with NO ledger entry (a crash window, or a genuinely
// content-failed/incomplete/rejected attempt) is simply skipped in favor
// of the next older one - never itself an error - UNLESS a typed result
// already claims a verified outcome (VerifiedSuccess/VerifiedContentFailure)
// for it despite the missing ledger, which is a genuine durable-storage
// integrity conflict and fails closed. Symmetrically, when a ledger entry
// DOES exist, any PRESENT typed result must agree with what that ledger
// implies (attemptId/attemptRunId/kind) - disagreement also fails closed.
// A present typed result is never REQUIRED, only validated when present.
// Returns std::nullopt (never an error) when no attempt is authoritative
// yet.
[[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>
resolveManualHandoffApplyAuthority(
    IManifestVerificationManualHandoffRepository& manualHandoffs,
    IManifestWorkLedgerRepository& ledger,
    IRunRepository& runs,
    const ManifestVerificationManualHandoff& handoff);

} // namespace ai::application
