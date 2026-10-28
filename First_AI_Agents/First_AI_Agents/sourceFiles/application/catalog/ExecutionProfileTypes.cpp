#include "ai/application/catalog/ExecutionProfileTypes.hpp"

namespace ai::application {

bool isKnownExecutionStageType(std::string_view stageType) noexcept
{
    return stageType == "architect"
        || stageType == "implementation"
        || stageType == "documentation";
}

} // namespace ai::application
