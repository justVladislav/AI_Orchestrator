#include <ai/ui/OutcomeBriefController.hpp>

#include <QDateTime>
#include <QStringList>
#include <QVariantList>

namespace {

using ai::application::OutcomeBriefView;

QString text(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString optionalText(const std::optional<std::string>& value)
{
    return value.has_value() ? text(*value) : QString();
}

const char* availabilityName(OutcomeBriefView::Availability availability)
{
    switch (availability) {
    case OutcomeBriefView::Availability::NotApplicable: return "not_applicable";
    case OutcomeBriefView::Availability::NotRecorded: return "not_recorded";
    case OutcomeBriefView::Availability::Present: return "present";
    case OutcomeBriefView::Availability::Unreadable: return "unreadable";
    }
    return "not_applicable";
}

} // namespace

OutcomeBriefController::OutcomeBriefController(
    const ai::application::OutcomeBriefReadModelService& readModel, QObject* parent)
    : QObject(parent)
    , readModel_(readModel)
    , brief_(project(OutcomeBriefView {}))
{
    trailingRefresh_.setSingleShot(true);
    trailingRefresh_.setInterval(kTrailingRefreshMs);
    connect(&trailingRefresh_, &QTimer::timeout, this, &OutcomeBriefController::refresh);
    selectionRead_.setSingleShot(true);
    selectionRead_.setInterval(kSelectionReadDelayMs);
    connect(&selectionRead_, &QTimer::timeout, this, &OutcomeBriefController::refresh);
}

bool OutcomeBriefController::applicable() const
{
    return brief_.value(QStringLiteral("availability")).toString() != QLatin1String("not_applicable");
}

void OutcomeBriefController::selectRun(const QString& runId)
{
    if (runId != selectedRunId_) {
        selectedRunId_ = runId;
        // What was armed belongs to the Run that was selected before.
        trailingRefresh_.stop();
    }
    selectionRead_.stop();
    refresh();
}

void OutcomeBriefController::selectRunSoon(const QString& runId)
{
    if (runId == selectedRunId_) return;
    selectedRunId_ = runId;
    trailingRefresh_.stop();
    // Nothing of the previous Run stays on screen, and nothing is read yet.
    brief_ = project(ai::application::OutcomeBriefView {});
    if (runId.isEmpty()) {
        selectionRead_.stop();
    } else {
        selectionRead_.start();
    }
    emit briefChanged();   // selectedRunId changed, whatever the map did
}

void OutcomeBriefController::refresh()
{
    const QVariantMap next = project(readModel_.viewForRun(selectedRunId_.toStdString()));
    const bool changed = next != brief_;
    brief_ = next;
    if (changed) emit briefChanged();
}

void OutcomeBriefController::scheduleRefresh()
{
    if (selectedRunId_.isEmpty() || !applicable()) return;
    refresh();
    if (!trailingRefresh_.isActive()) trailingRefresh_.start();
}

void OutcomeBriefController::onRunFinished(const QString& runId)
{
    if (selectedRunId_.isEmpty() || runId != selectedRunId_) return;
    refresh();
    if (!trailingRefresh_.isActive()) trailingRefresh_.start();
}

QVariantMap OutcomeBriefController::project(const OutcomeBriefView& view)
{
    QVariantMap brief;
    brief.insert(QStringLiteral("availability"), QString::fromLatin1(availabilityName(view.availability)));
    brief.insert(QStringLiteral("runId"), text(view.runId));
    brief.insert(QStringLiteral("errorCode"), text(view.errorCode));
    brief.insert(QStringLiteral("errorMessage"), text(view.errorMessage));
    brief.insert(QStringLiteral("state"), text(view.state));
    brief.insert(QStringLiteral("resolved"), view.resolved);
    brief.insert(QStringLiteral("pending"), view.pending);
    brief.insert(QStringLiteral("terminalStatus"), text(view.terminalStatus));

    QStringList reasons;
    for (const std::string& reason : view.reasons) reasons.append(text(reason));
    brief.insert(QStringLiteral("reasons"), reasons);

    QVariantMap counts;
    counts.insert(QStringLiteral("members"), view.counts.members);
    counts.insert(QStringLiteral("reviewable"), view.counts.reviewable);
    counts.insert(QStringLiteral("applied"), view.counts.applied);
    counts.insert(QStringLiteral("humanRejected"), view.counts.humanRejected);
    counts.insert(QStringLiteral("approved"), view.counts.approved);
    counts.insert(QStringLiteral("undecided"), view.counts.undecided);
    counts.insert(QStringLiteral("applyFailed"), view.counts.applyFailed);
    counts.insert(QStringLiteral("noChanges"), view.counts.noChanges);
    counts.insert(QStringLiteral("validatorRejected"), view.counts.validatorRejected);
    counts.insert(QStringLiteral("superseded"), view.counts.superseded);
    counts.insert(QStringLiteral("unsettled"), view.counts.unsettled);
    counts.insert(QStringLiteral("inconsistent"), view.counts.inconsistent);
    brief.insert(QStringLiteral("counts"), counts);

    QVariantList members;
    for (const auto& member : view.members) {
        QVariantMap row;
        row.insert(QStringLiteral("changeSetId"), text(member.changeSetId));
        row.insert(QStringLiteral("decision"), text(member.decision));
        members.append(row);
    }
    brief.insert(QStringLiteral("members"), members);

    brief.insert(QStringLiteral("recoveryEligible"), view.recoveryEligible);
    brief.insert(QStringLiteral("requiresInspection"), view.requiresInspection);
    brief.insert(QStringLiteral("continuationSourceRunId"), optionalText(view.continuationSourceRunId));
    brief.insert(QStringLiteral("continuationChildRunId"), optionalText(view.continuationChildRunId));
    brief.insert(QStringLiteral("manualHandoffId"), optionalText(view.manualHandoffId));
    brief.insert(QStringLiteral("revision"), static_cast<qulonglong>(view.revision));
    brief.insert(QStringLiteral("recordedAt"),
        view.revision == 0 ? QVariant()
                           : QVariant(QDateTime::fromMSecsSinceEpoch(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(view.recordedAt.time_since_epoch()).count())));
    brief.insert(QStringLiteral("sourceStateFingerprint"), text(view.sourceStateFingerprint));
    brief.insert(QStringLiteral("contentFingerprint"), text(view.contentFingerprint));
    brief.insert(QStringLiteral("renderedText"), text(view.renderedText));
    return brief;
}
