#include "ai/ui/CompactionIntentRecoveryController.hpp"

#include "ai/ui/CompactionIntentRecoveryProjection.hpp"

CompactionIntentRecoveryController::CompactionIntentRecoveryController(
    std::optional<ai::application::CompactionIntentRecoveryReport> report,
    std::optional<ai::common::Error> channelError,
    QObject* parent)
    : QObject(parent), report_(ai::ui::compaction_recovery::reportMap(report, channelError))
{
}
