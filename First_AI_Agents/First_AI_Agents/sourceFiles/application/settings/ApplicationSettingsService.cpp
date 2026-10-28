#include "ai/application/settings/ApplicationSettingsService.hpp"

#include <chrono>
#include <utility>

namespace ai::application {
namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

ApplicationSettingsService::ApplicationSettingsService(
    IApplicationSettingsRepository& repository,
    IExecutionProfileRepository& profiles,
    IProviderRepository& providers,
    IModelRepository& models)
    : repository_(repository)
    , profiles_(profiles)
    , providers_(providers)
    , models_(models)
{
}

common::Result<ApplicationSettings> ApplicationSettingsService::get() const
{
    return repository_.load();
}

common::Result<ApplicationSettings> ApplicationSettingsService::update(
    const UpdateApplicationSettingsRequest& request)
{
    if (request.interfaceScalePercent != 0
        && (request.interfaceScalePercent < 75 || request.interfaceScalePercent > 200)) {
        return common::Result<ApplicationSettings>::failure(error(
            "settings.invalid_scale", "Interface scale must be automatic or between 75 and 200 percent"));
    }
    if (request.defaultTimeoutMs < 1'000 || request.defaultTimeoutMs > 86'400'000) {
        return common::Result<ApplicationSettings>::failure(error(
            "settings.invalid_timeout", "Default timeout must be between one second and 24 hours"));
    }
    if (request.defaultMaxParallelAgents == 0 || request.defaultMaxParallelAgents > 32) {
        return common::Result<ApplicationSettings>::failure(error(
            "settings.invalid_parallelism", "Default parallel agents must be between 1 and 32"));
    }
    if (request.chatHistoryLimit == 0 || request.chatHistoryLimit > 10'000) {
        return common::Result<ApplicationSettings>::failure(error(
            "settings.invalid_history_limit", "Chat history limit must be between 1 and 10000"));
    }
    if (request.defaultChatProviderId.has_value()
        != request.defaultChatModelId.has_value()) {
        return common::Result<ApplicationSettings>::failure(error(
            "settings.incomplete_chat_model", "Default chat provider and model must be configured together"));
    }

    if (request.defaultExecutionProfileId.has_value()) {
        const auto profile = profiles_.findById(*request.defaultExecutionProfileId);
        if (!profile.success()) return common::Result<ApplicationSettings>::failure(*profile.error());
        if (!profile.value().has_value() || !profile.value()->enabled) {
            return common::Result<ApplicationSettings>::failure(error(
                "settings.profile_unavailable", "Default execution profile is missing or disabled"));
        }
    }

    if (request.defaultChatProviderId.has_value()) {
        const auto provider = providers_.findById(*request.defaultChatProviderId);
        if (!provider.success()) return common::Result<ApplicationSettings>::failure(*provider.error());
        const auto model = models_.findById(*request.defaultChatModelId);
        if (!model.success()) return common::Result<ApplicationSettings>::failure(*model.error());
        if (!provider.value().has_value() || !provider.value()->enabled
            || !model.value().has_value() || !model.value()->enabled
            || model.value()->providerId != *request.defaultChatProviderId) {
            return common::Result<ApplicationSettings>::failure(error(
                "settings.chat_model_unavailable", "Default chat provider or model is unavailable"));
        }
    }

    auto current = repository_.load();
    if (!current.success()) return common::Result<ApplicationSettings>::failure(*current.error());

    ApplicationSettings value;
    value.theme = request.theme;
    value.interfaceScalePercent = request.interfaceScalePercent;
    value.compactMode = request.compactMode;
    value.reduceAnimations = request.reduceAnimations;
    value.runContextPolicy = request.runContextPolicy;
    value.defaultExecutionProfileId = request.defaultExecutionProfileId;
    value.defaultTimeoutMs = request.defaultTimeoutMs;
    value.defaultMaxParallelAgents = request.defaultMaxParallelAgents;
    value.confirmManyModels = request.confirmManyModels;
    value.chatContextPolicy = request.chatContextPolicy;
    value.defaultChatProviderId = request.defaultChatProviderId;
    value.defaultChatModelId = request.defaultChatModelId;
    value.chatHistoryLimit = request.chatHistoryLimit;
    value.revision = current.value().revision + 1;
    value.updatedAt = std::chrono::system_clock::now();
    return repository_.save(std::move(value));
}

} // namespace ai::application
