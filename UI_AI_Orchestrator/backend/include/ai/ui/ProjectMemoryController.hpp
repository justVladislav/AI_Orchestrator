#pragma once

#include <ai/application/catalog/ExecutionProfileTypes.hpp>
#include <ai/application/ProjectMemoryCompactionService.hpp>
#include <ai/application/ProjectMemoryService.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>

#include <QObject>
#include <QVariantList>

#include <optional>
#include <stop_token>
#include <thread>

// AG-11 remaining product slice: a focused, selected-project projection over
// ProjectMemoryService (deterministic segments) and ProjectMemoryCompactionService
// (provider-driven consolidation). QML never touches SQLite or the Application
// Layer directly - every list/action goes through this controller, mirroring
// ChatController's async/cancellation shape for compact() specifically because
// it is the one operation here that makes a real provider call.
class ProjectMemoryController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool compacting READ compacting NOTIFY compactingChanged)
    Q_PROPERTY(QString compactingProjectId READ compactingProjectId NOTIFY compactingChanged)

public:
    ProjectMemoryController(
        ai::application::ProjectMemoryService&,
        ai::application::ProjectMemoryCompactionService&,
        ai::application::ProviderCatalogService&,
        QObject* parent = nullptr);
    ~ProjectMemoryController() override;

    [[nodiscard]] bool compacting() const noexcept;
    [[nodiscard]] QString compactingProjectId() const;

    // Bounded segment metadata only - id, source Run id (or empty for a
    // consolidated segment), creation time, and token counts. Never the raw
    // summary text is withheld here; the summary itself is the safe,
    // already-bounded provenance this surface is allowed to show (see
    // ARCHITECTURE_GAPS.md AG-11).
    Q_INVOKABLE QVariantList segments(const QString& projectId);

    // Compaction history: status, provider/model, immutable source ids/hash,
    // and actual token/cost usage only when the record has it (Failed/
    // Cancelled records carry zero usage by construction, not an estimate).
    Q_INVOKABLE QVariantList compactionHistory(const QString& projectId);

    // {providerId, providerName, modelId, modelName, available, statusText}
    // rows, mirroring ChatController::providerChoices() exactly so the same
    // "Ready/Credential required/Disabled" convention applies here too.
    Q_INVOKABLE QVariantList providerChoices();

    // Starts one asynchronous, cancellable compact() call. Returns false
    // without starting anything if a compaction is already running (any
    // project - the underlying service makes one real provider call at a
    // time from this controller) or if providerId/modelId is empty.
    Q_INVOKABLE bool compact(const QString& projectId, const QString& providerId, const QString& modelId);
    Q_INVOKABLE void stopCompaction();

    // Destructive: removes every memory segment for projectId. The
    // confirmation step is a QML-owned dialog; this call executes
    // immediately once invoked and only ever affects projectId.
    Q_INVOKABLE bool clearMemory(const QString& projectId);

    // AG-11's New Run indication: how much of this project's memory would be
    // included, and against what token budget, for a given set of model ids
    // (the New Run dialog's roster/profile selection - fallback model ids may
    // be mixed in with primaries, exactly like ExecutionProfileWorkflowRunner
    // measures both). Uses the same projectMemoryBudget()/assembleRunContext()
    // selection semantics the real Run uses - never a separate estimate.
    // {available, totalSegmentCount, includedSegmentCount, estimatedTokens,
    // budgetTokens}; available is false only on a storage/backend failure
    // (reported via errorOccurred too), never merely because there is no
    // memory yet - zero segments is a real, known zero, not "unknown".
    Q_INVOKABLE QVariantMap memoryBudgetEstimate(const QString& projectId, const QVariantList& modelIds);

signals:
    void segmentsChanged(const QString& projectId);
    void compactionHistoryChanged(const QString& projectId);
    void compactingChanged();
    void errorOccurred(const QString& code, const QString& message);

private:
    void startCompaction(QString projectId, QString providerId, QString modelId);
    void report(const ai::common::Error&);

    ai::application::ProjectMemoryService& memoryService_;
    ai::application::ProjectMemoryCompactionService& compactionService_;
    ai::application::ProviderCatalogService& catalog_;
    std::optional<std::jthread> worker_;
    QString compactingProjectId_;
};
