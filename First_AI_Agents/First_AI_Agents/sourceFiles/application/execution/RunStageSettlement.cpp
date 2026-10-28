#include "ai/application/execution/RunStageSettlement.hpp"

#include <utility>

namespace ai::application {

common::Result<void> settleLiveStages(
    IRunRepository& runs,
    const RunId& runId,
    std::optional<StageStatus> runningBecomes,
    std::chrono::system_clock::time_point finishedAt,
    const std::string& message)
{
    if (runningBecomes.has_value()
        && *runningBecomes != StageStatus::Failed
        && *runningBecomes != StageStatus::Cancelled
        && *runningBecomes != StageStatus::Skipped) {
        return common::Result<void>::failure({
            "run.stage_settlement_invalid",
            "A running stage can only be settled as Failed, Cancelled or Skipped"});
    }
    const auto stages = runs.listStages(runId);
    if (!stages.success()) return common::Result<void>::failure(*stages.error());
    for (RunStage stage : stages.value()) {
        if (stage.status == StageStatus::Pending) {
            stage.status = StageStatus::Skipped;
        } else if (stage.status == StageStatus::Running && runningBecomes.has_value()) {
            stage.status = *runningBecomes;
        } else {
            continue;
        }
        stage.finishedAt = finishedAt;
        stage.errorMessage = message;
        const auto settled = runs.updateStage(std::move(stage));
        if (!settled.success()) return common::Result<void>::failure(*settled.error());
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
