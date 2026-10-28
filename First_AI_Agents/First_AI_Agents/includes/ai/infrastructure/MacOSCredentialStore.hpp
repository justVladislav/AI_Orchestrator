#pragma once

#include "ai/application/providers/ICredentialStore.hpp"

namespace ai::infrastructure {

/**
 * macOS Keychain-backed credential store.
 *
 * Each secret is stored as a generic-password item under the application's
 * fixed service name. The caller's opaque reference is used as the Keychain
 * account name; secret bytes are never included in errors or metadata.
 */
class MacOSCredentialStore final : public application::ICredentialStore {
public:
    MacOSCredentialStore() = default;

    common::Result<void> store(const std::string& reference, const std::string& secret) override;
    [[nodiscard]] common::Result<std::optional<std::string>> load(
        const std::string& reference) const override;
    common::Result<void> remove(const std::string& reference) override;
};

} // namespace ai::infrastructure
