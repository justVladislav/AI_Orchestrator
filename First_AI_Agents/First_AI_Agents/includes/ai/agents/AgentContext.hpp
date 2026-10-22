#pragma once

#include "ai/models/Message.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace ai::agents {

// Per-run input passed to an agent. This is separate from AgentConfig so the same
// role can execute many tasks with different conversation history or shared data.
struct AgentContext {
    std::string task;
    std::vector<models::Message> previousMessages;
    std::unordered_map<std::string, std::string> sharedData;
};

} // namespace ai::agents
