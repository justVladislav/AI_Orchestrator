#pragma once

#include <ai/application/catalog/ExecutionProfileTypes.hpp>
#include <ai/application/conversations/ProjectMemoryCompactionService.hpp>
#include <ai/application/conversations/ProjectMemoryService.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

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

    // Compaction history, ONE read per call: {projectId, state, rows, errorCode, errorMessage}. state is "available" (rows may
    // be empty: a real "no compactions yet") or "unavailable" (the records could not be read: rows are empty, the original
    // error is in errorCode/errorMessage and was reported through errorOccurred; nothing is kept from an earlier success).
    // A project id that is empty is "available" with no rows and reads nothing.
    // Every row: status, provider/model, immutable source ids/hash, token usage and the STORED cost projection of that
    // record - priceState (none|known|partial|unknown), costText (the stored amount, "" unless known or partial),
    // costMicros, priceIncomplete. The cost is the record's own (never recomputed from the catalog) and is shown whatever the
    // status: a Failed or Cancelled record keeps the usage it was paid for.
    Q_INVOKABLE QVariantMap compactionHistorySnapshot(const QString& projectId);

    // The rows of compactionHistorySnapshot() - a compatibility view; it reads (and reports a failed read) on its own.
    Q_INVOKABLE QVariantList compactionHistory(const QString& projectId);

    // {providerId, providerName, modelId, modelName, available, statusText}
    // rows, mirroring ChatController::providerChoices() exactly so the same
    // "Ready/Credential required/Disabled" convention applies here too.
    Q_INVOKABLE QVariantList providerChoices();

    // Starts one asynchronous, cancellable compaction (ONE compactObserved() call of the service). Returns false
    // without starting anything if a compaction is already running (any
    // project - the underlying service makes one real provider call at a
    // time from this controller) or if projectId/providerId/modelId is empty.
    // Every ACCEPTED call ends with exactly one compactionOutcomeReported() carrying the projectId it was accepted for; a
    // failure of that operation - also one found inside the worker (project, provider, model, segments) - is part of that
    // outcome and is NOT sent through errorOccurred. errorOccurred keeps the synchronous refusals (busy) and the reads.
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
    // The terminal result of one accepted compact() call, projected by projectMemoryOutcomeMap() (see its header for the map) with
    // the project the call was accepted for - never the project that is selected when the signal arrives. It precedes
    // segmentsChanged/compactionHistoryChanged/compactingChanged of the same call. Ephemeral: it is not stored and not re-sent.
    void compactionOutcomeReported(const QString& projectId, const QVariantMap& outcome);
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
