#pragma once

#include "ai/application/verification/IManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/verification/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IVerificationBootstrapRepository.hpp"
#include "ai/application/verification/IVerificationEnvironmentRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ai::infrastructure {

/** SQLite implementation of MW-P5.1's durable verification-continuation checkpoint. */
class SqliteManifestVerificationContinuationRepository final
    : public application::IManifestVerificationContinuationRepository {
public:
    explicit SqliteManifestVerificationContinuationRepository(
        std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insert(
        application::ManifestVerificationContinuationCheckpoint checkpoint) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationContinuationCheckpoint>>
    findBySourceRun(const application::RunId& sourceRunId) const override;
    common::Result<void> insertAttribution(
        application::ManifestVerificationAttributionCheckpoint checkpoint) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationAttributionCheckpoint>>
    findAttributionBySourceRun(const application::RunId& sourceRunId) const override;
    common::Result<void> insertInput(
        application::ManifestVerificationContinuationInput input) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationContinuationInput>>
    findInputBySourceRun(const application::RunId& sourceRunId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationContinuationInput>>
    findInputByChildRun(const application::RunId& childRunId) const override;
    [[nodiscard]] common::Result<std::vector<application::ManifestVerificationContinuationInput>>
    listInputs() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of QML-P4's immutable manual-handoff contract + attempts. */
class SqliteManifestVerificationManualHandoffRepository final
    : public application::IManifestVerificationManualHandoffRepository {
public:
    explicit SqliteManifestVerificationManualHandoffRepository(
        std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertHandoff(
        application::ManifestVerificationManualHandoff handoff) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoff>>
    findHandoffByAnchorRun(const application::RunId& verificationAnchorRunId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoff>>
    findHandoffById(const std::string& handoffId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoff>>
    findHandoffByMemberProposalId(const application::ChangeSetId& proposalId) const override;
    [[nodiscard]] common::Result<std::vector<application::ManifestVerificationManualHandoff>>
    listAllHandoffs() const override;
    common::Result<void> discardHandoff(const std::string& handoffId) override;

    common::Result<void> insertAttempt(
        application::ManifestVerificationManualHandoffAttempt attempt) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoffAttempt>>
    findAttemptByRunId(const application::RunId& attemptRunId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoffAttempt>>
    findAttemptByFingerprint(
        const std::string& handoffId,
        const std::string& editedSnapshotFingerprint) const override;
    [[nodiscard]] common::Result<
        std::vector<application::ManifestVerificationManualHandoffAttempt>>
    listAttemptsForHandoff(const std::string& handoffId) const override;
    [[nodiscard]] common::Result<
        std::vector<application::ManifestVerificationManualHandoffAttempt>>
    listAllAttempts() const override;

    common::Result<void> insertAttemptResult(
        application::ManifestVerificationManualHandoffAttemptResult result) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoffAttemptResult>>
    findAttemptResultByAttemptId(const std::string& attemptId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of MW-P5.6D's immutable legacy-baseline-drift normalization proof. */
class SqliteManifestVerificationBaselineNormalizationRepository final
    : public application::IManifestVerificationBaselineNormalizationRepository {
public:
    explicit SqliteManifestVerificationBaselineNormalizationRepository(
        std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insert(
        application::ManifestVerificationBaselineNormalization normalization) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationBaselineNormalization>>
    findBySourceRun(const application::RunId& sourceRunId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the AG-18.1 verification environment snapshot. */
class SqliteVerificationEnvironmentRepository final
    : public application::IVerificationEnvironmentRepository {
public:
    explicit SqliteVerificationEnvironmentRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordSnapshot(
        application::VerificationEnvironmentSnapshot snapshot) override;
    [[nodiscard]] common::Result<std::optional<application::VerificationEnvironmentSnapshot>>
    findForRun(const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the AG-18.3 bootstrap/action-required lifecycle. */
class SqliteVerificationBootstrapRepository final
    : public application::IVerificationBootstrapRepository {
public:
    explicit SqliteVerificationBootstrapRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordDecision(
        application::VerificationBootstrapDecision decision) override;
    [[nodiscard]] common::Result<std::optional<application::VerificationBootstrapDecision>>
    findDecisionForRun(const application::RunId& runId) const override;
    common::Result<void> appendTransition(
        application::VerificationBootstrapTransition transition) override;
    [[nodiscard]] common::Result<std::vector<application::VerificationBootstrapTransition>>
    listTransitionsForRun(const application::RunId& runId) const override;
    common::Result<void> recordRetryLink(
        application::VerificationBootstrapRetryLink link) override;
    [[nodiscard]] common::Result<std::optional<application::VerificationBootstrapRetryLink>>
    findRetryLinkForPredecessor(const application::RunId& predecessorRunId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
