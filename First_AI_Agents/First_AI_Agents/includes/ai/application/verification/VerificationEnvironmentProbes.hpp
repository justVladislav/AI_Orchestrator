#pragma once

#include "ai/application/ManifestProposalSetVerifier.hpp"
#include "ai/application/verification/VerificationEnvironmentSnapshotTypes.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application::detail {

// The two probes of VerificationEnvironmentDiscoveryService that the project-readiness check (AG-18.4B-R1) needs on its own,
// exported unchanged so there is ONE implementation of "which presets exist" and "which targets does this configured build
// offer" - the readiness check never grows a second parser.

// `cmake --list-presets=configure|build` plus the preset files' own enrichment, against `root` as it is. A configured build tree
// that exists is probed for its targets; nothing is configured here.
[[nodiscard]] VerificationEnvironmentSnapshot probePresetCatalog(
    const std::filesystem::path& root, IManifestVerificationCommandRunner& commandRunner, std::stop_token stopToken);

struct ConfiguredTargetsProbe {
    bool confirmed {false};
    // Confirmed and not cut off by the per-preset target cap.
    bool complete {false};
    std::vector<std::string> targets;
};

// `cmake --build --preset <buildPreset> --target help` in `root`; the targets are only trusted when the command ran to a clean
// exit 0.
[[nodiscard]] ConfiguredTargetsProbe probeConfiguredTargets(
    IManifestVerificationCommandRunner& commandRunner,
    const std::filesystem::path& root,
    std::string_view buildPreset,
    std::stop_token stopToken);

} // namespace ai::application::detail
