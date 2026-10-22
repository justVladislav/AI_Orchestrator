#pragma once

#include "ai/application/ApplicationSettingsTypes.hpp"
#include "ai/common/Result.hpp"

namespace ai::application {

class IApplicationSettingsRepository {
public:
    virtual ~IApplicationSettingsRepository() = default;
    [[nodiscard]] virtual common::Result<ApplicationSettings> load() const = 0;
    virtual common::Result<ApplicationSettings> save(ApplicationSettings value) = 0;
};

} // namespace ai::application
