#include "ai/application/verification/VerificationScratchService.hpp"

#include "ai/application/verification/VerificationScratchPolicy.hpp"

#include <system_error>
#include <utility>

namespace ai::application {

namespace fs = std::filesystem;

namespace {

fs::path systemDefaultRoot()
{
    std::error_code error;
    auto base = fs::temp_directory_path(error);
    if (error) return {};
    return base / "aio-v";
}

} // namespace

VerificationScratchService::VerificationScratchService(
    IVerificationScratchSettingRepository& repository, fs::path defaultRoot)
    : repository_(repository)
    , defaultRoot_(defaultRoot.empty() ? systemDefaultRoot() : std::move(defaultRoot))
{
}

common::Result<std::optional<fs::path>> VerificationScratchService::configuredRoot() const
{
    return repository_.load();
}

common::Result<VerificationScratchRoot> VerificationScratchService::set(const std::optional<fs::path>& root)
{
    if (!root.has_value()) {
        const auto cleared = repository_.store(std::nullopt);
        if (!cleared.success()) return common::Result<VerificationScratchRoot>::failure(*cleared.error());
        return common::Result<VerificationScratchRoot>::ok({defaultRoot_, VerificationScratchSource::Default});
    }
    // No workspace is known here: the overlap with a project's own folder is checked every time a verification resolves the root.
    const auto validated = VerificationScratchPolicy::validate(*root, std::nullopt, /*createIfMissing=*/true);
    if (!validated.success()) return common::Result<VerificationScratchRoot>::failure(*validated.error());
    const auto stored = repository_.store(validated.value());
    if (!stored.success()) return common::Result<VerificationScratchRoot>::failure(*stored.error());
    return common::Result<VerificationScratchRoot>::ok({validated.value(), VerificationScratchSource::Setting});
}

common::Result<VerificationScratchRoot> VerificationScratchService::resolve(
    const std::optional<fs::path>& workspaceRoot) const
{
    const auto configured = repository_.load();
    if (!configured.success()) return common::Result<VerificationScratchRoot>::failure(*configured.error());
    if (configured.value().has_value()) {
        // The user chose this folder: if it no longer passes, the verification is refused, never moved somewhere else.
        const auto validated = VerificationScratchPolicy::validate(
            *configured.value(), workspaceRoot, /*createIfMissing=*/true);
        if (!validated.success()) {
            return common::Result<VerificationScratchRoot>::failure({validated.error()->code,
                "The verification folder chosen in Settings cannot be used. " + validated.error()->message});
        }
        return common::Result<VerificationScratchRoot>::ok({validated.value(), VerificationScratchSource::Setting});
    }
    if (defaultRoot_.empty()) {
        return common::Result<VerificationScratchRoot>::failure({kVerificationScratchUnavailableCode,
            std::string("The system's temporary folder is unknown. ") + kVerificationScratchChoosePrompt});
    }
    const auto validated = VerificationScratchPolicy::validate(
        defaultRoot_, workspaceRoot, /*createIfMissing=*/true, /*adoptExistingDefault=*/true);
    if (!validated.success()) {
        return common::Result<VerificationScratchRoot>::failure({validated.error()->code,
            "The system's temporary folder cannot be used for verification. " + validated.error()->message});
    }
    return common::Result<VerificationScratchRoot>::ok({validated.value(), VerificationScratchSource::Default});
}

VerificationScratchService::Status VerificationScratchService::status() const
{
    Status status;
    const auto configured = repository_.load();
    if (!configured.success()) {
        status.effective = defaultRoot_;
        status.problem = *configured.error();
        return status;
    }
    status.configured = configured.value();
    status.source = configured.value().has_value() ? VerificationScratchSource::Setting : VerificationScratchSource::Default;
    status.effective = configured.value().has_value() ? *configured.value() : defaultRoot_;
    if (status.effective.empty()) {
        status.problem = common::Error {kVerificationScratchUnavailableCode,
            std::string("The system's temporary folder is unknown. ") + kVerificationScratchChoosePrompt};
        return status;
    }
    // Creates nothing: an absent folder is reported as usable and is created private the first time a verification needs it (only the
    // application's own default folder may be tightened, exactly as resolve() does it, so that this answer matches what a run does).
    const auto checked = VerificationScratchPolicy::validate(
        status.effective, std::nullopt, /*createIfMissing=*/false,
        /*adoptExistingDefault=*/!configured.value().has_value(), /*probeWritable=*/false);
    if (checked.success()) {
        status.usable = true;
        return status;
    }
    // A missing folder is not a problem by itself: it is created private on first use, if its path passes the policy.
    std::error_code existsError;
    if (checked.error()->code == kVerificationScratchUnavailableCode
        && !fs::exists(status.effective, existsError) && !existsError) {
        status.usable = true;
        return status;
    }
    status.problem = *checked.error();
    return status;
}

} // namespace ai::application
