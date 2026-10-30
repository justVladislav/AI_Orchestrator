#pragma once

#include "ai/common/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace ai::infrastructure {

/**
 * Optional behaviour of SqliteDatabase::open(). The defaults keep every existing call exactly as it was.
 */
struct SqliteOpenOptions {
    /**
     * Directory for the verified copy that is taken BEFORE an existing database is upgraded to a newer schema version. Empty (the
     * default) takes no copy. It is set only by the composition root that owns the database file (BackendRuntime) from a trusted
     * location next to the database - never from QML, a preset or an agent. A copy is taken only when an existing, versioned database
     * is older than the supported schema; a new or empty database, a database that is already current, a newer one (refused as
     * before) and ":memory:" get none.
     *
     * With a directory set, a database that holds user objects but reports user_version 0 is refused before any change
     * ("migration.unversioned_database"): it is not a new database, so no migration may run on it unprotected.
     */
    std::filesystem::path preMigrationBackupDirectory;
};

/** What the pre-migration copy is (diagnostics only; no database content). */
struct PreMigrationBackupInfo {
    std::filesystem::path path;
    int fromVersion {0};
    int toVersion {0};
    std::uintmax_t sizeBytes {0};
};

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
        const std::filesystem::path& databasePath, const SqliteOpenOptions& options = {});

    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    /** The verified copy taken by this open(), if it upgraded an existing database. */
    [[nodiscard]] const std::optional<PreMigrationBackupInfo>& preMigrationBackup() const noexcept;

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
    friend class SqliteProjectOutputApprovalRepository;
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
