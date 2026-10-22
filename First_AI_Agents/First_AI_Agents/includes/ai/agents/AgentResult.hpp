
#pragma once

#include "ai/models/TokenUsage.hpp"

#include <string>

namespace ai::agents {

// Normalized execution result returned by every agent, regardless of provider.
struct AgentResult {
    std::string agentId;
    std::string output;
    models::TokenUsage usage;
    bool success {false};
    std::string errorMessage;
};

} // namespace ai::agents
