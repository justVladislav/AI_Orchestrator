#pragma once

#include <string>
#include <unordered_map>

namespace ai::orchestration {

struct Task {
    std::string id;
    std::string description;
    std::unordered_map<std::string, std::string> metadata;
};

} // namespace ai::orchestration
