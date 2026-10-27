#pragma once

#include <ai/application/planning/OutcomeBriefReadModelService.hpp>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

/**
 * PC-4D: the Qt boundary of the read-only `Outcome` block of Run Details.
 *
 * It copies ONE Backend projection - OutcomeBriefReadModelService::viewForRun() of the selected Run - into a QVariantMap and does
 * nothing else: it never writes a brief, never asks the reconciler to run, never starts, retries, applies or composes anything,
 * and calls no provider. Classification (the state, its pending / resolved flags, the typed reasons, the counts) is the Backend's;
 * QML only renders it.
 *
 * `brief` is always a complete map with `availability`:
 *   - "not_applicable": no block (not an outcome Run, a Run that is still going, no Run selected);
 *   - "not_recorded":   a terminal outcome Run whose brief has not been written yet;
 *   - "present":        the current revision - state, flags, reasons, counts, members, relations, revision, time, both
 *                       fingerprints and the rendered text (provider- and human-authored prose labelled untrusted: shown as
 *                       PLAIN text only, never as instructions);
 *   - "unreadable":     the Run or its brief could not be read: errorCode / errorMessage, and nothing of a stale brief.
 *
 * Pull-based, like ManifestController: the brief changes when the Run finishes, a proposal is reviewed or applied, or a handoff
 * is created. The reconciler's own hook runs a moment AFTER those events (best effort, on the worker), so the controller reads
 * now and once more shortly after. It reads ONLY when one of them can matter, because every read is a synchronous SQLite lookup on
 * the UI thread that contends with the worker for the database: it reads when a Run is selected, when the SELECTED Run finishes
 * (onRunFinished), and - only while a block is already shown - when a review / apply / handoff event is announced
 * (scheduleRefresh). Progress events of a running Run never read.
 */
class OutcomeBriefController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY briefChanged)
    Q_PROPERTY(bool applicable READ applicable NOTIFY briefChanged)
    Q_PROPERTY(QVariantMap brief READ brief NOTIFY briefChanged)

public:
    // The trailing read of scheduleRefresh(): after the producer's own event, the reconciler's nudge needs a moment.
    static constexpr int kTrailingRefreshMs = 1200;
    // The delay of selectRunSoon()'s read.
    static constexpr int kSelectionReadDelayMs = 200;

    explicit OutcomeBriefController(
        const ai::application::OutcomeBriefReadModelService& readModel, QObject* parent = nullptr);

    [[nodiscard]] QString selectedRunId() const { return selectedRunId_; }
    // False for "not_applicable": the block is not shown.
    [[nodiscard]] bool applicable() const;
    [[nodiscard]] QVariantMap brief() const { return brief_; }

    // Selects a Run (empty = none) and reads it now.
    Q_INVOKABLE void selectRun(const QString& runId);
    // Selects a Run WITHOUT touching the database now: the previous Run's brief is dropped at once and the read follows after
    // kSelectionReadDelayMs, if that Run is still the selected one. This is what the bridge uses for the application's own selection:
    // a Run that has just been started is selected while its worker is writing, and a synchronous SQLite read on the UI thread would
    // wait for the worker. A selection that changes again within the delay never reads at all.
    Q_INVOKABLE void selectRunSoon(const QString& runId);
    // Reads the selected Run now. briefChanged() is emitted only when what is shown actually changed.
    Q_INVOKABLE void refresh();

public slots:
    // For a review / apply / handoff event: reads now and once more after kTrailingRefreshMs (one pending trailing read at most,
    // however often it is called) - but only while a block is shown. Nothing is shown for a Run that is not an outcome Run or is still
    // going, and such a Run cannot gain a brief from these events (its finish is onRunFinished).
    void scheduleRefresh();
    // The Run `runId` finished. For the SELECTED Run only: reads now (the Run has just become an outcome Run, so the block appears) and
    // once more after kTrailingRefreshMs (the reconciler's hook writes the brief a moment after the finish). Another Run: nothing.
    void onRunFinished(const QString& runId);

    // The pure copy of the Backend's view into the map QML receives (every field under its own key, nothing computed). Public so that
    // a test can pin the whole shape with a hand-made view.
    [[nodiscard]] static QVariantMap project(const ai::application::OutcomeBriefView& view);

signals:
    void briefChanged();

private:
    const ai::application::OutcomeBriefReadModelService& readModel_;
    QString selectedRunId_;
    QVariantMap brief_;
    QTimer trailingRefresh_;
    QTimer selectionRead_;
};
