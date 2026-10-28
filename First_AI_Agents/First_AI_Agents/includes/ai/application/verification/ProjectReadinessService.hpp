#pragma once

#include "ai/application/verification/IProjectReadinessRepository.hpp"
#include "ai/application/verification/ManifestProposalSetVerifier.hpp"
#include "ai/application/verification/ProjectSetupReadiness.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace ai::application {

// AG-18.4B-R1: the one owner of a project's durable readiness.
//
//   * A `ready` record is REUSED while every recorded input still matches - no configure, no build, no cmake process at all
//     (assess() only reads files and the cached toolchain identity). A model failure, an exhausted budget, a cancelled or a
//     rejected Run never reach this service, so they cannot reset a working setup.
//   * It is REPLACED, never inherited, when the project root, the selected presets, the configuration inputs, the toolchain or the
//     configured build tree changed: the old success is marked Stale and the next ensureReady() proves the project again.
//   * It never stands in for the verification of a Run's exact candidate before Apply.
//
// The check itself is ProjectSetupReadiness::check (the existing preset discovery + a real configure + the target probe); this
// service adds persistence, freshness and the rule that a result is only recorded when nothing changed while it was gathered.
inline constexpr const char* kProjectSetupRecordNotSaved = "project_setup.record_not_saved";

struct ProjectReadinessAssessment {
    std::optional<ProjectReadinessRecord> record;
    // The record is Ready AND every input it was gathered with still matches.
    bool fresh {false};
    // Why it is not fresh: no_record, unverified, failed, stale, project_root_changed, toolchain_unavailable,
    // toolchain_changed, configuration_changed, build_tree_missing. Empty when fresh.
    std::string reason;
};

class ProjectReadinessService final {
public:
    using ToolchainProvider = ProjectSetupReadiness::ToolchainProvider;

    explicit ProjectReadinessService(IProjectReadinessRepository& repository, ToolchainProvider toolchain = {});

    // Cheap and cmake-free. A Ready record that no longer matches is recorded as Stale (so the UI can say so) before returning.
    [[nodiscard]] common::Result<ProjectReadinessAssessment> assess(
        const ProjectId& projectId, const std::filesystem::path& projectRoot);

    // Fresh record -> returned as is. Otherwise the check runs (provider-free) and its outcome is recorded: Ready only when the
    // check succeeded, was not cancelled, and the configuration inputs did not change while it ran; Failed with the reason
    // otherwise. A cancelled check records nothing. `forceRecheck` ignores a fresh record (the explicit "check again"; it also
    // forgets a remembered toolchain failure).
    [[nodiscard]] ProjectSetupCheck ensureReady(
        const ProjectId& projectId,
        const std::filesystem::path& projectRoot,
        IManifestVerificationCommandRunner& commandRunner,
        std::stop_token stopToken,
        bool forceRecheck = false);

    // The user chose a configure/build pair. Replaces the record with an Unverified one for that pair (a different pair is a
    // different setup; the old proof does not carry over).
    [[nodiscard]] common::Result<void> selectPair(
        const ProjectId& projectId, const std::filesystem::path& projectRoot, const ProjectSetupSelection& selection);

    [[nodiscard]] common::Result<std::optional<ProjectReadinessRecord>> current(const ProjectId& projectId) const;

    // sha256 over the configuration inputs of `projectRoot` (selection-independent): preset files and their includes, toolchain
    // files, and every CMakeLists.txt / *.cmake / dependency manifest outside build trees (a directory holding a CMakeCache.txt
    // is a build tree and is skipped, so the configure itself never changes the fingerprint). Source files are not inputs.
    [[nodiscard]] static common::Result<std::string> computeInputsFingerprint(const std::filesystem::path& projectRoot);

    // The fingerprint that is stored: the inputs plus the selected pair.
    [[nodiscard]] static std::string fingerprintFor(const std::string& inputsFingerprint, const ProjectSetupSelection& selection);

private:
    [[nodiscard]] std::shared_ptr<std::mutex> lockFor(const ProjectId& projectId);
    [[nodiscard]] VerificationToolchainEnvironment toolchain() const;

    IProjectReadinessRepository& repository_;
    ToolchainProvider toolchain_;
    std::mutex locksMutex_;
    std::map<ProjectId, std::shared_ptr<std::mutex>> locks_;
};

} // namespace ai::application
