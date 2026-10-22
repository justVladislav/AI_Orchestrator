#pragma once

#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace ai::application {

class InMemoryManifestVerificationManualHandoffRepository final
    : public IManifestVerificationManualHandoffRepository {
public:
    common::Result<void> insertHandoff(ManifestVerificationManualHandoff handoff) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoff>>
    findHandoffByAnchorRun(const RunId& verificationAnchorRunId) const override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoff>>
    findHandoffById(const std::string& handoffId) const override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoff>>
    findHandoffByMemberProposalId(const ChangeSetId& proposalId) const override;
    [[nodiscard]] common::Result<std::vector<ManifestVerificationManualHandoff>>
    listAllHandoffs() const override;
    common::Result<void> discardHandoff(const std::string& handoffId) override;

    common::Result<void> insertAttempt(
        ManifestVerificationManualHandoffAttempt attempt) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>
    findAttemptByRunId(const RunId& attemptRunId) const override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>
    findAttemptByFingerprint(
        const std::string& handoffId,
        const std::string& editedSnapshotFingerprint) const override;
    [[nodiscard]] common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>
    listAttemptsForHandoff(const std::string& handoffId) const override;
    [[nodiscard]] common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>
    listAllAttempts() const override;

    common::Result<void> insertAttemptResult(
        ManifestVerificationManualHandoffAttemptResult result) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoffAttemptResult>>
    findAttemptResultByAttemptId(const std::string& attemptId) const override;

private:
    mutable std::mutex mutex_;
    std::map<RunId, ManifestVerificationManualHandoff> handoffsByAnchorRun_;
    std::map<std::string, RunId> handoffAnchorRunById_;
    std::map<ChangeSetId, std::string> handoffIdByMemberProposalId_;
    std::map<RunId, ManifestVerificationManualHandoffAttempt> attemptsByRunId_;
    std::vector<ManifestVerificationManualHandoffAttempt> attemptOrder_;
    std::map<std::string, ManifestVerificationManualHandoffAttemptResult> attemptResultsByAttemptId_;
};

} // namespace ai::application
