#pragma once

#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/execution/ManifestWorkLedgerTypes.hpp"

#include <string>

namespace ai::application {

class ChangeProposalService;

// The outcome of determineContentRepairableFailureDenial: an empty code
// means eligible. Deliberately NOT a common::Result<void> - a fail-closed
// "why not" reason is the ordinary, expected outcome here (most Runs are
// not repairable), never an exceptional condition.
struct ManifestVerificationRepairableFailureDenial {
    std::string code;
    std::string message;
    [[nodiscard]] bool eligible() const noexcept { return code.empty(); }
};

// QML-P4R.2: the workflow-agnostic core of MW-P5.1's continuation
// eligibility (ManifestVerificationContinuationService.cpp's own
// determineEligibility) - given a Run's own already-durable Work Ledger
// entry, is its failure the kind a content repair (paid continuation OR
// free manual handoff) could plausibly fix? Checks disposition, presence of
// a combined verification identity, passed proposal safety, at least one
// failed required criterion, and every diagnostic's own content-repairable
// allowlist membership - never a workflowId or Run-status gate, which stay
// the CALLER's own responsibility (a source Manifest Run disallows
// continuation-of-continuation; a manual-handoff anchor may legitimately be
// either the origin Manifest Run or one of its own paid continuation
// children - see ManifestVerificationManualHandoffService.cpp's own
// evaluateManualHandoffAdmission for that second case). Never reads a Run,
// a repository, or a proposal - the caller already has the entry and owns
// any further proposal-membership recheck.
[[nodiscard]] ManifestVerificationRepairableFailureDenial
determineContentRepairableFailureDenial(const ManifestWorkLedgerEntry& entry);

// Manifest verification reliability hardening follow-up: the ONLY
// diagnostic codes a worker's proposal CONTENT could plausibly fix. Every
// other code ManifestProposalSetVerifier ever produces describes an
// infrastructure, environment, or Backend-binding condition (a too-long
// candidate path, an unavailable/denied tool, a timeout, an output-size cap,
// cancellation, or invalid Backend-resolved profile arguments) that would
// recur identically on a repeat verification no matter what the worker
// proposes - spending a bounded repair session on one of those would both
// waste it and mislabel an infrastructure failure as a content one.
// Deliberately an allowlist, not a blocklist: an unrecognized future
// diagnostic code fails closed (never repairable) rather than silently
// becoming repairable by omission.
//
// Shared by ExecutionManifestExecutor.cpp's within-Run bounded repair scope
// and MW-P5's cross-Run verification continuation eligibility - one
// authoritative classification, never two copies that could drift.
[[nodiscard]] bool isContentRepairableDiagnosticCode(const std::string& code);

// These diagnostics are neither content-repairable nor reasons to block a
// repair/continuation decision. verification.prerequisite_failed is the
// expected informational side-effect of a NotRun dependent check.
// verification.qml_warning_outside_task_scope belongs to a Passed criterion
// and retains bounded finding provenance only so baseline/candidate
// attribution can prove pre-existing debt. verification.qml_warning_baseline_debt
// is proven pre-existing debt on an otherwise-Failed check. QML-lint
// advisory-severity policy (2026-09-28): verification.qml_warning_advisory
// is a durable, non-blocking finding that can appear on a genuinely Failed
// mixed result (alongside a real blocking diagnostic) - it never has content
// of its own for a repair worker to fix. None of these may independently
// veto eligibility; unrecognized future codes still fail closed.
[[nodiscard]] bool isIgnorableNonRepairDiagnosticCode(const std::string& code);

// Renders the fresh repair session's prompt prefix for exactly one task:
// this task's own verified Backend findings (never another task's), plus
// its own previous proposal's content as untrusted historical reference -
// "the repair worker must see the result of its own previous proposal", not
// just be told to redo the live workspace from scratch. Never includes a
// raw command payload, an absolute path, a secret, or another task's data.
//
// Shared by ExecutionManifestExecutor.cpp's within-Run bounded repair wave
// and MW-P5.4's cross-Run continuation repair (ManifestVerificationContinuationRepairService)
// - one authoritative rendering, never two copies that could drift.
[[nodiscard]] std::string buildVerificationRepairContext(
    const ExecutionManifestTask& task,
    const ManifestProposalSetVerification& verification,
    const ChangeSetId& previousChangeSetId,
    ChangeProposalService& changeProposals);

} // namespace ai::application
