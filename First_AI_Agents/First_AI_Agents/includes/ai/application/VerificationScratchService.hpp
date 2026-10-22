#pragma once

#include "ai/application/IVerificationScratchRootProvider.hpp"
#include "ai/application/IVerificationScratchSettingRepository.hpp"

#include <filesystem>
#include <optional>

namespace ai::application {

/**
 * The user's choice of the folder for the temporary verification copies, validated by the Backend, and the one place that decides
 * which root a verification really uses:
 *  - a root the user set is used when it passes the policy, otherwise the verification is REFUSED (never silently moved to another
 *    folder: the user chose this one);
 *  - without a setting the system's temporary folder ("<TEMP>/aio-v") is used when it passes the same policy, otherwise the
 *    verification is refused and the refusal says to choose a short folder;
 *  - nothing is created outside the chosen root, and no shared folder is created on the user's behalf.
 * The setting affects only the temporary execution copies. Each operation still gets its own unique sub-folder (the overlay builder
 * allocates it) and the content-identity checks of the verification are untouched.
 */
class VerificationScratchService final : public IVerificationScratchRootProvider {
public:
    struct Status {
        // The folder the user set (as stored), if any.
        std::optional<std::filesystem::path> configured;
        // The folder a verification would use now, and where it comes from.
        std::filesystem::path effective;
        VerificationScratchSource source {VerificationScratchSource::Default};
        // False when a verification would be refused right now; problem says why.
        bool usable {false};
        std::optional<common::Error> problem;
    };

    // defaultRoot is "<TEMP>/aio-v" when empty.
    explicit VerificationScratchService(
        IVerificationScratchSettingRepository& repository, std::filesystem::path defaultRoot = {});

    [[nodiscard]] common::Result<std::optional<std::filesystem::path>> configuredRoot() const;

    // Validates the folder (creating it private when missing) and stores its canonical path; nullopt clears the setting. A folder
    // that fails the policy is refused with its named error and the previous setting stays.
    common::Result<VerificationScratchRoot> set(const std::optional<std::filesystem::path>& root);

    [[nodiscard]] common::Result<VerificationScratchRoot> resolve(
        const std::optional<std::filesystem::path>& workspaceRoot) const override;

    // What the Settings page shows. Creates no folder.
    [[nodiscard]] Status status() const;

private:
    IVerificationScratchSettingRepository& repository_;
    std::filesystem::path defaultRoot_;
};

} // namespace ai::application
