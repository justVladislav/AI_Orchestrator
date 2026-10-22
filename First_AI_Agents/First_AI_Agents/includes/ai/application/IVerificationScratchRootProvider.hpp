#pragma once

#include "ai/common/Result.hpp"

#include <filesystem>
#include <optional>

namespace ai::application {

enum class VerificationScratchSource {
    // "<TEMP>/aio-v": nothing was chosen, the system's temporary folder is used if it passes the policy.
    Default,
    // The folder the user chose in Settings.
    Setting
};

struct VerificationScratchRoot {
    std::filesystem::path path;
    VerificationScratchSource source {VerificationScratchSource::Default};
};

/**
 * Hands out the validated root for the temporary execution copies of a workspace. A refusal is a typed error that names the problem
 * and says what to do about it (see VerificationScratchPolicy); it is returned BEFORE a copy is made or a command is started.
 */
class IVerificationScratchRootProvider {
public:
    virtual ~IVerificationScratchRootProvider() = default;

    [[nodiscard]] virtual common::Result<VerificationScratchRoot> resolve(
        const std::optional<std::filesystem::path>& workspaceRoot) const = 0;
};

// The root a service uses for its temporary copies: the provider's validated root when one is wired (production), otherwise the
// service's own fallback unchanged (tests and older compositions). A refusal is returned as it is - before any copy is made.
[[nodiscard]] inline common::Result<std::filesystem::path> resolveVerificationScratchRoot(
    const IVerificationScratchRootProvider* provider,
    const std::filesystem::path& fallback,
    const std::optional<std::filesystem::path>& workspaceRoot)
{
    if (provider == nullptr) return common::Result<std::filesystem::path>::ok(fallback);
    const auto resolved = provider->resolve(workspaceRoot);
    if (!resolved.success()) return common::Result<std::filesystem::path>::failure(*resolved.error());
    return common::Result<std::filesystem::path>::ok(resolved.value().path);
}

} // namespace ai::application
