#pragma once

#include "ai/providers/IModelProvider.hpp"

namespace ai::providers {

class MockProvider final : public IModelProvider {
public:
    [[nodiscard]] std::string name() const override;
    models::ModelResponse generate(const models::ModelRequest& request) override;
};

} // namespace ai::providers
