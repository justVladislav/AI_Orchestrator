#include "ai/application/ModelExecutionTypes.hpp"

namespace ai::application {

std::string_view toString(ModelExecutionSource value) noexcept { switch (value) { case ModelExecutionSource::Run: return "run"; case ModelExecutionSource::Comparison: return "comparison"; case ModelExecutionSource::Chat: return "chat"; case ModelExecutionSource::FollowUp: return "follow_up"; } return "run"; }
std::string_view toString(ModelExecutionStatus value) noexcept { switch (value) { case ModelExecutionStatus::Pending: return "pending"; case ModelExecutionStatus::Running: return "running"; case ModelExecutionStatus::Completed: return "completed"; case ModelExecutionStatus::Partial: return "partial"; case ModelExecutionStatus::Failed: return "failed"; case ModelExecutionStatus::Cancelled: return "cancelled"; } return "pending"; }
std::string_view toString(QualityEvaluatorKind value) noexcept { switch (value) { case QualityEvaluatorKind::Automated: return "automated"; case QualityEvaluatorKind::BlindedModel: return "blinded_model"; case QualityEvaluatorKind::Human: return "human"; } return "automated"; }
std::optional<ModelExecutionSource> modelExecutionSourceFromString(std::string_view value) noexcept { if (value == "run") return ModelExecutionSource::Run; if (value == "comparison") return ModelExecutionSource::Comparison; if (value == "chat") return ModelExecutionSource::Chat; if (value == "follow_up") return ModelExecutionSource::FollowUp; return std::nullopt; }
std::optional<ModelExecutionStatus> modelExecutionStatusFromString(std::string_view value) noexcept { if (value == "pending") return ModelExecutionStatus::Pending; if (value == "running") return ModelExecutionStatus::Running; if (value == "completed") return ModelExecutionStatus::Completed; if (value == "partial") return ModelExecutionStatus::Partial; if (value == "failed") return ModelExecutionStatus::Failed; if (value == "cancelled") return ModelExecutionStatus::Cancelled; return std::nullopt; }
std::optional<QualityEvaluatorKind> qualityEvaluatorKindFromString(std::string_view value) noexcept { if (value == "automated") return QualityEvaluatorKind::Automated; if (value == "blinded_model") return QualityEvaluatorKind::BlindedModel; if (value == "human") return QualityEvaluatorKind::Human; return std::nullopt; }
bool isTerminal(ModelExecutionStatus value) noexcept
{
    return value == ModelExecutionStatus::Completed
        || value == ModelExecutionStatus::Partial
        || value == ModelExecutionStatus::Failed
        || value == ModelExecutionStatus::Cancelled;
}

} // namespace ai::application
