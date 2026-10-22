#include "ai/application/ManifestVerificationManualHandoffApplyAuthority.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/ManifestVerificationManualHandoffTypes.hpp"

#include <algorithm>
#include <unordered_set>

namespace ai::application {

common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>
resolveManualHandoffApplyAuthority(
    IManifestVerificationManualHandoffRepository& manualHandoffs,
    IManifestWorkLedgerRepository& ledger,
    IRunRepository& runs,
    const ManifestVerificationManualHandoff& handoff)
{
    const auto attemptsResult = manualHandoffs.listAttemptsForHandoff(handoff.id);
    if (!attemptsResult.success()) {
        return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure(
            *attemptsResult.error());
    }
    auto attempts = attemptsResult.value();
    std::sort(attempts.begin(), attempts.end(), [](const auto& left, const auto& right) {
        if (left.attemptOrdinal != right.attemptOrdinal) {
            return left.attemptOrdinal > right.attemptOrdinal;
        }
        return left.id > right.id;
    });

    std::unordered_set<std::uint64_t> seenOrdinals;
    for (const auto& attempt : attempts) {
        const auto validAttempt = validateManifestVerificationManualHandoffAttempt(attempt);
        if (!validAttempt.success() || attempt.handoffId != handoff.id
                || attempt.verificationAnchorRunId != handoff.verificationAnchorRunId
                || !seenOrdinals.insert(attempt.attemptOrdinal).second) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                "manifest_apply.manual_handoff_authority_corrupt",
                "The manual-handoff attempt history is structurally inconsistent"});
        }

        const auto typedResult = manualHandoffs.findAttemptResultByAttemptId(attempt.id);
        if (!typedResult.success()) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure(
                *typedResult.error());
        }
        const auto entryResult = ledger.findEntry(attempt.attemptRunId);
        if (!entryResult.success()) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure(
                *entryResult.error());
        }
        if (!entryResult.value().has_value()) {
            if (typedResult.value().has_value()) {
                const auto& typed = *typedResult.value();
                if (typed.attemptId != attempt.id
                        || typed.attemptRunId != attempt.attemptRunId
                        || typed.kind
                            == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess
                        || typed.kind
                            == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure) {
                    return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                        "manifest_apply.manual_handoff_authority_corrupt",
                        "A manual-handoff typed result conflicts with Work Ledger authority"});
                }
            }
            continue;
        }

        const auto& entry = *entryResult.value();
        if (entry.runId != attempt.attemptRunId || entry.projectId != handoff.sourceProjectId) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                "manifest_apply.manual_handoff_authority_corrupt",
                "A manual-handoff Work Ledger identity does not match its attempt"});
        }
        const auto expectedKind = entry.terminalStatus == RunStatus::Completed
            ? ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess
            : ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure;
        if (typedResult.value().has_value()) {
            const auto& typed = *typedResult.value();
            if (typed.attemptId != attempt.id || typed.attemptRunId != attempt.attemptRunId
                    || typed.kind != expectedKind) {
                return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                    "manifest_apply.manual_handoff_authority_corrupt",
                    "A manual-handoff typed result conflicts with Work Ledger authority"});
            }
        }
        if (entry.terminalStatus != RunStatus::Completed) continue;
        if (entry.disposition != ManifestWorkLedgerDisposition::ManifestReady
                || !entry.verification.has_value()) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                "manifest_apply.manual_handoff_authority_corrupt",
                "A completed manual-handoff attempt lacks authoritative verification"});
        }
        const auto validVerification = validateManifestProposalSetVerification(*entry.verification);
        if (!validVerification.success()
                || !requiredAcceptancePassed(*entry.verification)
                || entry.proposalIds != entry.verification->orderedProposalIds) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                "manifest_apply.manual_handoff_authority_corrupt",
                "A completed manual-handoff attempt has an inconsistent verified proposal set"});
        }

        const auto runResult = runs.findRun(attempt.attemptRunId);
        if (!runResult.success()) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure(
                *runResult.error());
        }
        if (!runResult.value().has_value()) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                "manifest_apply.manual_handoff_authority_corrupt",
                "The authoritative manual-handoff attempt Run no longer exists"});
        }
        const auto& run = *runResult.value();
        if (run.id != attempt.attemptRunId || run.projectId != handoff.sourceProjectId
                || run.workflowId != kManifestVerificationManualHandoffWorkflowId
                || run.parentRunId != std::optional<RunId>(handoff.verificationAnchorRunId)
                || run.status != RunStatus::Completed) {
            return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::failure({
                "manifest_apply.manual_handoff_authority_corrupt",
                "The authoritative manual-handoff attempt Run identity is inconsistent"});
        }
        return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::ok(
            ManifestVerificationManualHandoffApplyAuthority {attempt, run, entry});
    }
    return common::Result<std::optional<ManifestVerificationManualHandoffApplyAuthority>>::ok(std::nullopt);
}

} // namespace ai::application
