#include "ai/providers/MockProvider.hpp"

#include "ai/models/Message.hpp"

#include <cctype>
#include <sstream>
#include <string>

namespace ai::providers {

namespace {

std::string lastUserMessage(const models::ModelRequest& request)
{
    // Providers usually care most about the newest user instruction.
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
        if (it->role == models::MessageRole::User) {
            return it->content;
        }
    }

    return {};
}

std::uint64_t estimateTokens(const std::string& text)
{
    // Cheap approximation for tests; real providers should return actual usage.
    std::uint64_t count = 0;
    bool inToken = false;

    for (unsigned char ch : text) {
        if (std::isspace(ch) != 0) {
            inToken = false;
        } else if (!inToken) {
            inToken = true;
            ++count;
        }
    }

    return count;
}

std::uint64_t estimateInputTokens(const models::ModelRequest& request)
{
    std::uint64_t total = 0;
    for (const auto& message : request.messages) {
        total += estimateTokens(message.content);
    }
    return total;
}

} // namespace

std::string MockProvider::name() const
{
    return "mock";
}

models::ModelResponse MockProvider::generate(const models::ModelRequest& request)
{
    const std::string userTask = lastUserMessage(request);

    // MockProvider is deterministic so tests can verify Agent and Orchestrator
    // behavior without network calls, API keys, or model costs.
    std::ostringstream text;
    text << "MockProvider processed the request";
    if (!request.options.model.empty()) {
        text << " with model '" << request.options.model << "'";
    }
    if (!userTask.empty()) {
        text << ". Last user task: " << userTask;
    }

    const std::string output = text.str();

    return models::ModelResponse {
        output,
        "mock",
        request.options.model,
        models::TokenUsage {
            estimateInputTokens(request),
            estimateTokens(output)
        },
        true,
        {}
    };
}

} // namespace ai::providers
