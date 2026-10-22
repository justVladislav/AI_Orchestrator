#pragma once

#include "ai/models/GenerationOptions.hpp"

#include <string>

namespace ai::agents {

// Static role definition: who the agent is and which generation settings it uses.
struct AgentConfig {
    std::string id;
    std::string name;
    std::string role;
    std::string systemPrompt;
    models::GenerationOptions generationOptions;
};

} // namespace ai::agents
