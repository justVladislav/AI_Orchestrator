#include "ai/application/VerificationBootstrapTypes.hpp"

namespace ai::application {

const char* toString(VerificationBootstrapDisposition value) noexcept
{
    switch (value) {
    case VerificationBootstrapDisposition::BootstrapPending: return "bootstrap_pending";
    case VerificationBootstrapDisposition::ActionRequired: return "action_required";
    case VerificationBootstrapDisposition::BootstrapVerified: return "bootstrap_verified";
    case VerificationBootstrapDisposition::BootstrapFailed: return "bootstrap_failed";
    }
    return "action_required";
}

std::optional<VerificationBootstrapDisposition>
verificationBootstrapDispositionFromString(std::string_view value) noexcept
{
    if (value == "bootstrap_pending") return VerificationBootstrapDisposition::BootstrapPending;
    if (value == "action_required") return VerificationBootstrapDisposition::ActionRequired;
    if (value == "bootstrap_verified") return VerificationBootstrapDisposition::BootstrapVerified;
    if (value == "bootstrap_failed") return VerificationBootstrapDisposition::BootstrapFailed;
    return std::nullopt;
}

} // namespace ai::application
