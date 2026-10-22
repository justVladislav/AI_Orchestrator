#include "ai/application/ManifestWorkLedgerTypes.hpp"

namespace ai::application {

const char* toString(ManifestWorkLedgerDisposition value) noexcept
{
    switch (value) {
    case ManifestWorkLedgerDisposition::ManifestReady: return "manifest_ready";
    case ManifestWorkLedgerDisposition::NoChangesNeeded: return "no_changes_needed";
    case ManifestWorkLedgerDisposition::NotProduced: return "not_produced";
    }
    return "not_produced";
}

std::optional<ManifestWorkLedgerDisposition>
manifestWorkLedgerDispositionFromString(std::string_view value) noexcept
{
    if (value == "manifest_ready") return ManifestWorkLedgerDisposition::ManifestReady;
    if (value == "no_changes_needed") return ManifestWorkLedgerDisposition::NoChangesNeeded;
    if (value == "not_produced") return ManifestWorkLedgerDisposition::NotProduced;
    return std::nullopt;
}

const char* toString(ManifestWorkLedgerTransitionKind value) noexcept
{
    switch (value) {
    case ManifestWorkLedgerTransitionKind::Approved: return "approved";
    case ManifestWorkLedgerTransitionKind::Rejected: return "rejected";
    case ManifestWorkLedgerTransitionKind::Applied: return "applied";
    }
    return "approved";
}

std::optional<ManifestWorkLedgerTransitionKind>
manifestWorkLedgerTransitionKindFromString(std::string_view value) noexcept
{
    if (value == "approved") return ManifestWorkLedgerTransitionKind::Approved;
    if (value == "rejected") return ManifestWorkLedgerTransitionKind::Rejected;
    if (value == "applied") return ManifestWorkLedgerTransitionKind::Applied;
    return std::nullopt;
}

} // namespace ai::application
