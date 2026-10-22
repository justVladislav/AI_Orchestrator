#pragma once

#include "ai/application/IApplicationSettingsRepository.hpp"
#include "ai/application/IExecutionProfileRepository.hpp"
#include "ai/application/IModelRepository.hpp"
#include "ai/application/IProviderRepository.hpp"

namespace ai::application {

class ApplicationSettingsService final {
public:
    ApplicationSettingsService(
        IApplicationSettingsRepository& repository,
        IExecutionProfileRepository& profiles,
        IProviderRepository& providers,
        IModelRepository& models);

    [[nodiscard]] common::Result<ApplicationSettings> get() const;
    common::Result<ApplicationSettings> update(
        const UpdateApplicationSettingsRequest& request);

private:
    IApplicationSettingsRepository& repository_;
    IExecutionProfileRepository& profiles_;
    IProviderRepository& providers_;
    IModelRepository& models_;
};

} // namespace ai::application
