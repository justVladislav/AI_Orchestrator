#pragma once

#include "ai/common/Result.hpp"

#include <filesystem>
#include <memory>

namespace ai::infrastructure {

/**
 * Shared SQLite connection and migration boundary.
 *
 * Opening is explicit and fallible so desktop/CLI composition roots can show a
 * controlled persistence error instead of silently replacing durable storage
 * with an in-memory repository. Repository instances share this object and its
 * lock, which also keeps multi-statement stage replacement atomic.
 */
class SqliteDatabase final {
public:
    [[nodiscard]] static common::Result<std::shared_ptr<SqliteDatabase>> open(
        const std::filesystem::path& databasePath);

    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    struct Impl;

    explicit SqliteDatabase(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class SqliteProjectRepository;
    friend class SqliteTaskRepository;
    friend class SqliteRunRepository;
    friend class SqliteArtifactRepository;
    friend class SqliteRunWarningRepository;
    friend class SqliteVerificationScratchSettingRepository;
    friend class SqliteProjectReadinessRepository;
    friend class SqliteWorkspaceStateRepository;
    friend class SqliteProviderRepository;
    friend class SqliteModelRepository;
    friend class SqliteExecutionProfileRepository;
    friend class SqliteExecutionManifestRepository;
    friend class SqliteManifestTaskExecutionRepository;
    friend class SqliteManifestWorkLedgerRepository;
    friend class SqliteManifestVerificationContinuationRepository;
    friend class SqliteManifestVerificationManualHandoffRepository;
    friend class SqliteManifestVerificationBaselineNormalizationRepository;
    friend class SqliteRunWorkspaceScopeRepository;
    friend class SqliteVerificationEnvironmentRepository;
    friend class SqliteVerificationBootstrapRepository;
    friend class SqliteRunDraftRepository;
    friend class SqliteOutcomeBriefRepository;
    friend class SqliteFollowUpPlanRepository;
    friend class SqliteChatRepository;
    friend class SqliteChatMemoryRepository;
    friend class SqliteProjectMemoryRepository;
    friend class SqliteProjectMemoryCompactionRepository;
    friend class SqliteModelExecutionRepository;
    friend class SqliteComparisonSessionRepository;
    friend class SqliteApplicationSettingsRepository;
    friend class SqliteChangeProposalRepository;
    friend class SqliteChangeApprovalRepository;
    friend class SqliteProposalReviewRepository;
    friend class SqliteAgentToolSessionRepository;
    friend class SqliteAgentToolAuditRepository;
};

} // namespace ai::infrastructure
