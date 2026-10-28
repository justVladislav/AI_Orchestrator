#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/execution/IRunRepository.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// One Run as the Prompt Workshop's source-Run picker shows it: just enough to recognise it.
struct RunSourceChoice {
    RunId id;
    std::string title;
    RunStatus status {RunStatus::Queued};
    std::chrono::system_clock::time_point createdAt;
};

// PC-3C/3D: the narrowest read-only view of Runs the Prompt Workshop needs - "which project does this Run belong to?"
// and "which Runs could the user name as the source?" - so a group's `sourceRunIds` can be offered and checked
// against the conversation's project. It cannot read stages or usage, cannot write, and is not a RunManager: handing
// it to RunDraftPlanService does not let the service touch a Run.
class IRunSourceLookup {
public:
    virtual ~IRunSourceLookup() = default;

    // Ok(nullopt) = no such Run. A failure is a storage failure, never "not found".
    [[nodiscard]] virtual common::Result<std::optional<ProjectId>> findRunProject(const RunId& runId) const = 0;

    // The Runs of one project, in no particular order (the caller sorts and bounds them).
    [[nodiscard]] virtual common::Result<std::vector<RunSourceChoice>> listProjectRuns(
        const ProjectId& projectId) const = 0;
};

// The production adapter: reads only the identity columns of a Run through the const methods of the Run repository.
class RunRepositorySourceLookup final : public IRunSourceLookup {
public:
    explicit RunRepositorySourceLookup(const IRunRepository& runs) : runs_(runs) {}

    [[nodiscard]] common::Result<std::optional<ProjectId>> findRunProject(const RunId& runId) const override
    {
        const auto found = runs_.findRun(runId);
        if (!found.success()) return common::Result<std::optional<ProjectId>>::failure(*found.error());
        if (!found.value().has_value()) return common::Result<std::optional<ProjectId>>::ok(std::nullopt);
        return common::Result<std::optional<ProjectId>>::ok(found.value()->projectId);
    }

    [[nodiscard]] common::Result<std::vector<RunSourceChoice>> listProjectRuns(const ProjectId& projectId) const override
    {
        const auto listed = runs_.listRuns(projectId);
        if (!listed.success()) return common::Result<std::vector<RunSourceChoice>>::failure(*listed.error());
        std::vector<RunSourceChoice> choices;
        choices.reserve(listed.value().size());
        for (const Run& run : listed.value()) {
            // Defensive: the repository was asked for one project; never offer a Run of another.
            if (run.projectId != projectId) continue;
            choices.push_back(RunSourceChoice {run.id, run.title, run.status, run.createdAt});
        }
        return common::Result<std::vector<RunSourceChoice>>::ok(std::move(choices));
    }

private:
    const IRunRepository& runs_;
};

} // namespace ai::application
