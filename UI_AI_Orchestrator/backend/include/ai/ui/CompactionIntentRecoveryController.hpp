#pragma once

#include <ai/application/conversations/CompactionIntentRecoveryReport.hpp>

#include <QObject>
#include <QVariantMap>

#include <optional>

/**
 * B3-III: the READ-ONLY view of the owner-startup recovery of the compaction intents.
 *
 * It holds a COPY of the report and of the startup error channel that the runtime produced once, inside its constructor. It has no
 * recovery service, no intent port, no SQL and no way to run, repeat or refresh anything: reading `report` any number of times returns
 * the same startup snapshot. The scope is the whole database (all projects) at that startup, never the selected project or period
 * (see ai::ui::compaction_recovery::reportMap for the map).
 */
class CompactionIntentRecoveryController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap report READ report CONSTANT)

public:
    CompactionIntentRecoveryController(std::optional<ai::application::CompactionIntentRecoveryReport> report,
                                       std::optional<ai::common::Error> channelError,
                                       QObject* parent = nullptr);

    [[nodiscard]] QVariantMap report() const { return report_; }

private:
    QVariantMap report_;
};
