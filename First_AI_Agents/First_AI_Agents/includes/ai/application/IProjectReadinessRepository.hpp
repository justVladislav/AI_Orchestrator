#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// AG-18.4B-R1: the durable, machine-local answer to "can this project be configured and built here?".
//
// One record per project, but a `ready` record only ever speaks for the exact (project root, configure preset, build preset,
// configuration inputs, toolchain) it was gathered with - a change of any of them makes it stale, never inherits the old
// success. It is NOT a portable success flag (a copied database or workspace must prove itself again), it is NOT the
// per-Run immutable VerificationEnvironmentSnapshot (which stays Run evidence), and it never replaces the verification of a
// Run's exact candidate before Apply.
enum class ProjectReadinessState {
    Unverified, // a pair may be chosen, but nothing was proven yet
    Ready,
    Stale,      // was ready; a recorded input no longer matches
    Failed,     // the last check failed (the reason is recorded)
};

[[nodiscard]] const char* toString(ProjectReadinessState value) noexcept;
[[nodiscard]] std::optional<ProjectReadinessState> projectReadinessStateFromString(std::string_view value) noexcept;

struct ProjectReadinessRecord {
    ProjectId projectId;
    // The project root the evidence belongs to (UTF-8, generic separators).
    std::string projectRoot;
    // The selected pair; empty while the user has not chosen and several exist.
    std::string configurePreset;
    std::string buildPreset;
    // The selected configure preset's build directory, relative to the root; the evidence needs its CMakeCache.txt to exist.
    std::string binaryDir;
    ProjectReadinessState state {ProjectReadinessState::Unverified};
    // sha256 over the configuration inputs (preset files and what they include, toolchain files, every CMakeLists.txt / *.cmake
    // outside build trees); empty unless the state has ever been Ready.
    std::string inputsFingerprint;
    // The MSVC / SDK identity the evidence was gathered with.
    std::string toolchainIdentity;
    std::vector<std::string> targets;
    bool targetsComplete {false};
    std::chrono::system_clock::time_point checkedAt {};
    std::optional<std::chrono::system_clock::time_point> lastSuccessAt;
    // project_setup.* / empty. Bounded.
    std::string failureCode;
    std::string failureMessage;
};

class IProjectReadinessRepository {
public:
    virtual ~IProjectReadinessRepository() = default;

    [[nodiscard]] virtual common::Result<std::optional<ProjectReadinessRecord>> find(const ProjectId& projectId) const = 0;
    // Insert or replace the project's one record.
    virtual common::Result<void> upsert(const ProjectReadinessRecord& record) = 0;
};

/** Thread-safe development/test adapter. */
class InMemoryProjectReadinessRepository final : public IProjectReadinessRepository {
public:
    [[nodiscard]] common::Result<std::optional<ProjectReadinessRecord>> find(const ProjectId& projectId) const override
    {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(projectId);
        if (found == records_.end()) return common::Result<std::optional<ProjectReadinessRecord>>::ok(std::nullopt);
        return common::Result<std::optional<ProjectReadinessRecord>>::ok(found->second);
    }
    common::Result<void> upsert(const ProjectReadinessRecord& record) override
    {
        std::lock_guard lock(mutex_);
        records_[record.projectId] = record;
        return common::Result<void>::ok();
    }

private:
    mutable std::mutex mutex_;
    std::map<ProjectId, ProjectReadinessRecord> records_;
};

} // namespace ai::application
