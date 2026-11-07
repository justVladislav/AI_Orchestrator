#include "ai/ui/CompactionCoverageProjection.hpp"

#include <QDateTime>

#include <chrono>

namespace ai::ui::compaction_coverage {

namespace {

qint64 millisecondsOf(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

QString formatTime(std::chrono::system_clock::time_point value)
{
    return QDateTime::fromMSecsSinceEpoch(millisecondsOf(value)).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

} // namespace

QVariantMap coverageMap(const std::optional<ai::application::CompactionCoverageObservation>& coverage)
{
    QVariantMap map {
        {"state", QStringLiteral("none")},
        {"openOperations", QVariant {}},
        {"unresolvedOperations", QVariant {}},
        {"hasCounts", false},
        {"windowed", false},
        {"scope", QStringLiteral("all_time")},
        {"hasFrom", false},
        {"hasTo", false},
        {"fromText", QString {}},
        {"toText", QString {}},
        {"errorCode", QString {}},
        {"errorMessage", QString {}}};
    if (!coverage.has_value()) return map;

    const bool available = coverage->state == ai::application::CompactionCoverageState::Available;
    map["state"] = available ? QStringLiteral("available") : QStringLiteral("unavailable");
    // Counts exist only for an available observation, and only if the Application gave them; an unavailable one carries none.
    if (available && coverage->openOperations.has_value()) map["openOperations"] = QVariant::fromValue<qulonglong>(*coverage->openOperations);
    if (available && coverage->unresolvedOperations.has_value()) {
        map["unresolvedOperations"] = QVariant::fromValue<qulonglong>(*coverage->unresolvedOperations);
    }
    map["hasCounts"] = available && coverage->openOperations.has_value() && coverage->unresolvedOperations.has_value();
    map["windowed"] = coverage->windowed;
    map["scope"] = coverage->windowed ? QStringLiteral("period") : QStringLiteral("all_time");
    if (coverage->fromInclusive.has_value()) {
        map["hasFrom"] = true;
        map["fromText"] = formatTime(*coverage->fromInclusive);
    }
    if (coverage->toExclusive.has_value()) {
        map["hasTo"] = true;
        map["toText"] = formatTime(*coverage->toExclusive);
    }
    if (coverage->error.has_value()) {
        map["errorCode"] = QString::fromStdString(coverage->error->code);
        map["errorMessage"] = QString::fromStdString(coverage->error->message);
    }
    return map;
}

QVariantMap intentRowMap(const ai::application::ProjectMemoryCompactionIntent& intent)
{
    const bool unresolved = intent.state == ai::application::CompactionIntentState::Unresolved;
    return QVariantMap {
        {"id", QString::fromStdString(intent.id)},
        {"projectId", QString::fromStdString(intent.projectId)},
        {"providerId", QString::fromStdString(intent.providerId)},
        {"modelId", QString::fromStdString(intent.modelId)},
        {"createdAt", formatTime(intent.createdAt)},
        {"createdAtMs", QVariant::fromValue<qlonglong>(millisecondsOf(intent.createdAt))},
        {"state", QString::fromLatin1(ai::application::toString(intent.state))},
        {"sourceCount", QVariant::fromValue<qulonglong>(intent.sourceSegmentIds.size())},
        {"markedAt", unresolved && intent.unresolvedMarkedAt.has_value() ? formatTime(*intent.unresolvedMarkedAt) : QString {}}};
}

QVariantMap intentSectionMap(const QString& projectId, const ai::application::CompactionIntentReadSnapshot& snapshot)
{
    QVariantMap coverage = coverageMap(snapshot.coverage);
    QVariantList rows;
    for (const auto& intent : snapshot.rows) rows.push_back(intentRowMap(intent));
    QVariantMap section {
        {"projectId", projectId},
        {"state", coverage.value(QStringLiteral("state"))},
        {"coverage", coverage},
        {"rows", rows},
        {"errorCode", coverage.value(QStringLiteral("errorCode"))},
        {"errorMessage", coverage.value(QStringLiteral("errorMessage"))}};
    return section;
}

} // namespace ai::ui::compaction_coverage
