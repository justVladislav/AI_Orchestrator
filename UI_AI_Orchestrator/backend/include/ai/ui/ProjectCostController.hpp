#pragma once

#include "ai/ui/ProjectCostProviderModel.hpp"

#include <ai/application/analytics/ProjectCostService.hpp>
#include <ai/application/catalog/ProviderCatalogService.hpp>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <optional>

/**
 * A1-min: the project cost estimate for the Overview, over ProjectCostService (ledger + stored memory compactions).
 *
 * State, in one property: loadState is "idle" (no project), "available" (the numbers are those of the LAST successful read) or
 * "unavailable" (the last read failed: errorCode/errorMessage say why). On a failure EVERYTHING of the previous result is gone - the
 * sums, the counters, the provider rows - and the scope is the one that was requested, so an old total is never shown for a project it
 * does not belong to, and never as a zero. QML reads the state from these properties; errorOccurred is the same fact as a signal.
 *
 * Order of a failure, which every observer can rely on: the state is replaced completely FIRST (loadState unavailable, the error
 * set, the numbers cleared), then summaryChanged, then errorOccurred - exactly once for the failed refresh. A success and clear() emit
 * no error; they clear the previous error.
 *
 * One refresh = one call of ProjectCostService. Provider/model names come from the catalog; a missing name falls back to the id and
 * is never a failure.
 */
class ProjectCostController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString loadState READ loadState NOTIFY summaryChanged)
    Q_PROPERTY(QString scopeId READ scopeId NOTIFY summaryChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY summaryChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY summaryChanged)
    Q_PROPERTY(QString priceState READ priceState NOTIFY summaryChanged)
    Q_PROPERTY(QString costText READ costText NOTIFY summaryChanged)
    Q_PROPERTY(bool priceIncomplete READ priceIncomplete NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong inputTokens READ inputTokens NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong outputTokens READ outputTokens NOTIFY summaryChanged)
    // Ledger rows only: not the number of calls, and compactions are not in it.
    Q_PROPERTY(qulonglong ledgerExecutionCount READ ledgerExecutionCount NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong ledgerAverageLatencyMs READ ledgerAverageLatencyMs NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong compactionRecordCount READ compactionRecordCount NOTIFY summaryChanged)
    // Compaction records without any price data: counted here, never as incomplete and never as zero cost.
    Q_PROPERTY(qulonglong compactionNoneCount READ compactionNoneCount NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong pricingEvidenceCount READ pricingEvidenceCount NOTIFY summaryChanged)
    Q_PROPERTY(qulonglong incompletePriceCount READ incompletePriceCount NOTIFY summaryChanged)
    // Why the provider chart does or does not draw bars: "cost" | "tokens" | "incomplete" | "unavailable" | "empty".
    Q_PROPERTY(QString shareBasis READ shareBasis NOTIFY summaryChanged)
    // B3-III: the recording coverage of the compaction intents of the SAME scope and of the SAME read as the money (see
    // ai::ui::compaction_coverage::coverageMap). A SEPARATE statement: it is in no amount, price state or count above. "none" while idle
    // or after a failed money read (cleared together with the summary), "unavailable" when only the intent read failed (the money stays).
    Q_PROPERTY(QVariantMap coverage READ coverage NOTIFY summaryChanged)
    Q_PROPERTY(QAbstractItemModel* providers READ providers CONSTANT)

public:
    ProjectCostController(ai::application::ProjectCostService& costs,
                          ai::application::ProviderCatalogService& catalog,
                          QObject* parent = nullptr);

    [[nodiscard]] QString loadState() const { return loadState_; }
    [[nodiscard]] QString scopeId() const { return scopeId_; }
    [[nodiscard]] QString errorCode() const { return errorCode_; }
    [[nodiscard]] QString errorMessage() const { return errorMessage_; }
    [[nodiscard]] QString priceState() const;
    [[nodiscard]] QString costText() const;
    [[nodiscard]] bool priceIncomplete() const noexcept;
    [[nodiscard]] qulonglong inputTokens() const noexcept;
    [[nodiscard]] qulonglong outputTokens() const noexcept;
    [[nodiscard]] qulonglong ledgerExecutionCount() const noexcept;
    [[nodiscard]] qulonglong ledgerAverageLatencyMs() const noexcept;
    [[nodiscard]] qulonglong compactionRecordCount() const noexcept;
    [[nodiscard]] qulonglong compactionNoneCount() const noexcept;
    [[nodiscard]] qulonglong pricingEvidenceCount() const noexcept;
    [[nodiscard]] qulonglong incompletePriceCount() const noexcept;
    [[nodiscard]] QString shareBasis() const { return providers_.sharesBasis(); }
    [[nodiscard]] QAbstractItemModel* providers() noexcept { return &providers_; }
    [[nodiscard]] QVariantMap coverage() const;

    Q_INVOKABLE bool refreshProject(const QString& projectId);
    Q_INVOKABLE bool refreshProjectRange(const QString& projectId,
                                         const QDateTime& fromInclusive,
                                         const QDateTime& toExclusive);
    Q_INVOKABLE void clear();

signals:
    void summaryChanged();
    void errorOccurred(const QString& code, const QString& message);

private:
    bool apply(const ai::common::Result<ai::application::ProjectCostSnapshot>& result, const QString& scopeId);
    bool fail(const QString& scopeId, const QString& code, const QString& message);

    ai::application::ProjectCostService& costs_;
    ai::application::ProviderCatalogService& catalog_;
    ProjectCostProviderModel providers_;
    ai::application::ProjectCostAggregate totals_;
    std::optional<ai::application::CompactionCoverageObservation> coverage_;
    QString loadState_ {QStringLiteral("idle")};
    QString scopeId_;
    QString errorCode_;
    QString errorMessage_;
};
