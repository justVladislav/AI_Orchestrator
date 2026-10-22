#pragma once

#include "ai/common/Result.hpp"

#include <optional>
#include <string>

namespace ai::application {

/**
 * Provider-neutral secret storage boundary. Only Provider::credentialReference
 * (an opaque string produced by/passed to this interface) ever appears on a
 * Provider DTO, in SQLite, in logs, or in a Result<...>::error(). The secret
 * value itself never does.
 *
 * No implementation of this interface may be an insecure plaintext
 * filesystem store. Production adapters use Windows Credential Manager or
 * macOS Keychain; unsupported/configured-out builds use a controlled-
 * unavailable adapter. reference is never a secret itself - it is a stable
 * lookup key (e.g. a provider id or a caller-chosen name).
 */
class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;

    // Implementations must never include `secret` in a returned Error, in a
    // thrown exception message, or in any log line - only `reference` (and a
    // fixed, secret-free reason string) may ever describe a failure here.
    virtual common::Result<void> store(
        const std::string& reference,
        const std::string& secret) = 0;

    [[nodiscard]] virtual common::Result<std::optional<std::string>> load(
        const std::string& reference) const = 0;

    virtual common::Result<void> remove(const std::string& reference) = 0;
};

} // namespace ai::application
