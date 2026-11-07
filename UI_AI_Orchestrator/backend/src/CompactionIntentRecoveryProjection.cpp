#include "ai/ui/CompactionIntentRecoveryProjection.hpp"

#include "ai/ui/ProjectMemoryOutcomeProjection.hpp"

#include <QVariantList>

namespace ai::ui::compaction_recovery {

QVariantMap reportMap(const std::optional<ai::application::CompactionIntentRecoveryReport>& report,
    const std::optional<ai::common::Error>& channelError)
{
    using ai::application::CompactionIntentRecoveryStatus;
    QVariantMap map {
        {"scope", QStringLiteral("database")},
        {"applicable", report.has_value()},
        {"status", QStringLiteral("none")},
        {"inspected", QVariant {}},
        {"markedUnresolved", QVariant {}},
        {"alreadyUnresolved", QVariant {}},
        {"closedConfirmed", QVariant {}},
        {"conflicts", QVariant {}},
        {"unconfirmed", QVariant {}},
        {"hasFirstError", false},
        {"firstErrorCode", QString {}},
        {"firstErrorMessage", QString {}},
        {"firstNative", compactionNativeDetailMap(std::nullopt)},
        {"diagnostics", QVariantList {}},
        {"writeCauses", QVariantList {}},
        {"diagnosticsIncomplete", false},
        {"channelErrorCode", channelError.has_value() ? QString::fromStdString(channelError->code) : QString {}},
        {"channelErrorMessage", channelError.has_value() ? QString::fromStdString(channelError->message) : QString {}},
        {"hasNotes", false},
        {"needsAttention", false},
        {"hasActivity", false}};
    if (!report.has_value()) return map;

    map["status"] = QString::fromLatin1(ai::application::toString(report->status));
    map["inspected"] = QVariant::fromValue<qulonglong>(report->inspected);
    map["markedUnresolved"] = QVariant::fromValue<qulonglong>(report->markedUnresolved);
    map["alreadyUnresolved"] = QVariant::fromValue<qulonglong>(report->alreadyUnresolved);
    map["closedConfirmed"] = QVariant::fromValue<qulonglong>(report->closedConfirmed);
    map["conflicts"] = QVariant::fromValue<qulonglong>(report->conflicts);
    map["unconfirmed"] = QVariant::fromValue<qulonglong>(report->unconfirmed);
    if (report->firstError.has_value()) {
        map["hasFirstError"] = true;
        map["firstErrorCode"] = QString::fromStdString(report->firstError->code);
        map["firstErrorMessage"] = QString::fromStdString(report->firstError->message);
    }
    map["firstNative"] = compactionNativeDetailMap(report->firstNative);

    QVariantList diagnostics;
    for (const auto& entry : report->diagnostics) {
        diagnostics.push_back(QVariantMap {
            {"stage", compactionDiagnosticStageSlug(entry.stage)},
            {"stageText", compactionDiagnosticStageText(entry.stage)},
            {"code", QString::fromStdString(entry.error.code)},
            {"message", QString::fromStdString(entry.error.message)}});
    }
    map["diagnostics"] = diagnostics;

    QVariantList writeCauses;
    for (const auto& entry : report->writeCauses) {
        writeCauses.push_back(QVariantMap {
            {"code", QString::fromStdString(entry.cause.code)},
            {"message", QString::fromStdString(entry.cause.message)},
            {"native", compactionNativeDetailMap(entry.native)}});
    }
    map["writeCauses"] = writeCauses;
    map["diagnosticsIncomplete"] = report->diagnosticsIncomplete;
    map["hasNotes"] = !report->diagnostics.empty() || !report->writeCauses.empty() || report->diagnosticsIncomplete;
    map["needsAttention"] = report->status != CompactionIntentRecoveryStatus::Complete;
    map["hasActivity"] = report->inspected > 0;
    return map;
}

} // namespace ai::ui::compaction_recovery
