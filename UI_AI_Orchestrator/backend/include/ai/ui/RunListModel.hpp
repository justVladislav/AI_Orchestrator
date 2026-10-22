#pragma once

#include <ai/application/ApplicationTypes.hpp>

#include <QAbstractListModel>
#include <vector>

/** Read-only Qt projection of persisted runs for RunsPage. */
class RunListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        RunIdRole = Qt::UserRole + 1,
        ProjectIdRole,
        ParentRunIdRole,
        TitleRole,
        TaskRole,
        WorkflowIdRole,
        StatusRole,
        ProgressRole,
        CurrentStageIdRole,
        ErrorMessageRole,
        InputTokensRole,
        OutputTokensRole,
        CacheCreationInputTokensRole,
        CacheReadInputTokensRole,
        MaxParallelismRole,
        CreatedAtRole,
        StartedAtRole,
        FinishedAtRole,
        PrimaryProviderIdRole,
        PrimaryModelIdRole,
        AssignmentsRole,
        WorkspacePathRole,
        ProviderBudgetMaxInputTokensRole,
        ProviderBudgetMaxOutputTokensRole,
        ProviderBudgetMaxTurnsRole,
        ProviderBudgetMaxToolCallsRole,
        ProviderBudgetMaxToolResultBytesRole,
        ProviderBudgetMaxWallTimeMsRole,
        ProviderBudgetMaxRequestTimeMsRole,
        ProviderTurnsStartedRole,
        ProviderTurnsCompletedRole,
        ProviderToolCallsRole,
        ProviderToolResultBytesRole,
        ProviderUsageAuthoritativeRole,
        ProviderBudgetStopCodeRole,
        // Distinct from InputTokensRole/OutputTokensRole above: those mirror
        // run.usage (the honest total, cache-read included - see
        // models::TokenUsage's class comment). These mirror what actually
        // gates the Run (ProviderExecutionBudgetUsage, cache-read excluded -
        // see its class comment), so the Run Details budget line reads
        // consistently with what stopped or didn't stop the Run.
        ProviderBudgetInputTokensRole,
        ProviderBudgetOutputTokensRole,
        ManualHandoffResolutionRole,
        // Package 4 (2026-09-28 planner-terminal-budget-hardening): the
        // Backend-owned ai::application::computeUsageTokenBreakdown()
        // projection of run.usage - the genuinely fresh, never-cached
        // portion of InputTokensRole's raw total (InputTokensRole minus both
        // CacheCreationInputTokensRole and CacheReadInputTokensRole). QML
        // must read this role rather than subtracting the cache roles
        // itself, so the Backend stays the sole classifier of usage
        // categories (see RunDetailsPanel.qml's providerUsageBreakdownText).
        NonCacheInputTokensRole,
        // AG-17.7C-3: how this Run came to exist, when it is the product of a legacy roster ->
        // Manifest conversion: "" (an ordinary Run), "explicit_user" (the user confirmed the
        // conversion summary), "not_required_safe_retry" (an automatic Retry that dropped nothing)
        // or "unreadable" (a conversion row whose mode is not recognised - shown as a fault, never
        // hidden). A marker only: the record itself is read, with its integrity check, by
        // RunController::legacyConversionRecord for the Run the user opens.
        LegacyConversionRole,
        // The Run's frozen turn limit when the Backend raised it automatically to what the
        // frozen Manifest needs, else 0. ProviderBudgetMaxTurnsRole is then the raised limit
        // the Run is actually held to, so Run Details can show "turns 25 / 25 (raised from 24)".
        ProviderBudgetTurnsRaisedFromRole,
        // PC-2: the Run's frozen interface contracts, as the list of maps RunContractVariant.hpp
        // defines, and the sha256 fingerprint of that list ("" when there are none). Immutable after
        // enqueue, so a status update never republishes them.
        InterfaceContractsRole,
        InterfaceContractsFingerprintRole
    };
    Q_ENUM(Role)

    explicit RunListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(
        std::vector<ai::application::Run> runs,
        std::vector<std::string> manualHandoffResolutions = {},
        std::vector<std::string> legacyConversions = {});

    // Applies one row from a completed background projection and emits only
    // its changed roles. A run ID absent from the current filtered snapshot
    // is a no-op; a later explicit full refresh handles shape changes.
    void updateOne(const ai::application::Run& run);

    // Looks up terminal status from the already-applied snapshot without a
    // second backend/database round trip.
    [[nodiscard]] QString statusFor(const QString& runId) const;

private:
    std::vector<ai::application::Run> runs_;
    std::vector<std::string> manualHandoffResolutions_;
    std::vector<std::string> legacyConversions_;
};
