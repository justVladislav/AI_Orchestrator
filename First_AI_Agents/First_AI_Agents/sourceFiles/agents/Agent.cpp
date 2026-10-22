#include "ai/agents/Agent.hpp"

#include "ai/models/ModelRequest.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ai::agents {

Agent::Agent(AgentConfig config, std::shared_ptr<providers::IModelProvider> provider)
    : config_(std::move(config))
    , provider_(std::move(provider))
{
    if (!provider_) {
        throw std::invalid_argument("Agent requires a model provider");
    }
}

const AgentConfig& Agent::config() const noexcept
{
    return config_;
}

AgentResult Agent::execute(const AgentContext& context)
{
    try {
        models::ModelRequest request;
        request.options = config_.generationOptions;

        // The system prompt defines the role; the task stays user input.
        if (!config_.systemPrompt.empty()) {
            request.messages.push_back(models::Message {
                models::MessageRole::System,
                config_.systemPrompt
            });
        }

        // Previous messages let a future workflow continue a conversation.
        request.messages.insert(
            request.messages.end(),
            context.previousMessages.begin(),
            context.previousMessages.end());

        request.messages.push_back(models::Message {
            models::MessageRole::User,
            context.task
        });

        // From this point on, Agent does not care whether the backend is mock,
        // OpenAI, Anthropic, or something else.
        models::ModelResponse response = provider_->generate(request);

        return AgentResult {
            config_.id,
            response.text,
            response.usage,
            response.success,
            response.errorMessage
        };
    } catch (const std::exception& ex) {
        return AgentResult {
            config_.id,
            {},
            {},
            false,
            ex.what()
        };
    } catch (...) {
        return AgentResult {
            config_.id,
            {},
            {},
            false,
            "Unknown agent execution error"
        };
    }
}

std::future<AgentResult> Agent::executeAsync(AgentContext context)
{
    return std::async(std::launch::async, [this, context = std::move(context)]() mutable {
        return execute(context);
    });
}

} // namespace ai::agents
