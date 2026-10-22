#pragma once

#include "ai/application/ICredentialStore.hpp"

namespace ai::infrastructure {

/**
 * Windows Credential Manager (wincred.h)-backed ICredentialStore. Secrets
 * are stored as CRED_TYPE_GENERIC credentials under a fixed target-name
 * prefix derived only from the caller's `reference` string, never from the
 * secret itself. Every mutable buffer this implementation copies a secret
 * into (its own UTF-8 staging buffer on store(), and the OS-owned buffer
 * CredFree() is about to release on load()) is explicitly zeroed
 * (SecureZeroMemory) before it goes out of scope - the strongest cleanup
 * the Win32 Credential Manager API surface allows a caller to perform.
 *
 * This type only exists in the AIOrchestrator_WindowsCredentials target,
 * built only on Windows and only when
 * AI_ORCHESTRATOR_BUILD_WINDOWS_CREDENTIALS is enabled (see CMakeLists.txt).
 * BackendRuntime falls back to its own controlled-unavailable adapter on
 * every other platform/configuration - see BackendRuntime.cpp.
 */
class WindowsCredentialStore final : public application::ICredentialStore {
public:
    WindowsCredentialStore() = default;

    common::Result<void> store(const std::string& reference, const std::string& secret) override;
    [[nodiscard]] common::Result<std::optional<std::string>> load(
        const std::string& reference) const override;
    common::Result<void> remove(const std::string& reference) override;
};

} // namespace ai::infrastructure
