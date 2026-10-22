#pragma once

#include "ai/application/IProjectMemoryRepository.hpp"
#include "ai/common/Result.hpp"

#include <string_view>
#include <vector>

namespace ai::application {

/**
 * Deterministic, LLM-free writer for the project memory layer (Task B of
 * PROJECT_MEMORY_PLAN.md). recordRunOutcome() builds exactly one
 * ProjectMemorySegment per completed run: project/run identification, the
 * full run task clipped to an explicit ~1000 token budget, and the run's
 * final output (production synthesis stage, or the deterministic Mock
 * runner's final report) clipped to an explicit ~2000 token budget. No
 * provider/LLM call is ever made here - token accounting reuses the existing
 * estimateChatTokens() estimator (see ChatContextAssembler.hpp) so writer and
 * reader measure text the same way.
 *
 * This writer never mutates Project::description. The independent reader
 * path is implemented by RunContextAssembler and the production workflow
 * runner (Tasks C1/C2).
 */
class ProjectMemoryService {
public:
    explicit ProjectMemoryService(IProjectMemoryRepository& repository);

    // Accepts only a run whose Run::status is RunStatus::Completed; any other
    // status fails with "project_memory.run_not_completed" and never calls
    // the repository. finalOutput is the caller-supplied synthesis/last-stage
    // output text (WorkflowResult::finalOutput); it is not read from run/
    // project themselves so callers stay free to source it however their
    // runner produced it.
    common::Result<ProjectMemorySegment> recordRunOutcome(
        const Project& project,
        const Run& run,
        std::string_view finalOutput);

    // Thin pass-throughs kept only for BackendRuntime wiring and tests; this
    // is not a UI-facing read/management API (see PROJECT_MEMORY_PLAN.md
    // Task E for that surface).
    [[nodiscard]] common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(
        const ProjectId& projectId) const;
    common::Result<void> removeMemorySegments(const ProjectId& projectId);

private:
    IProjectMemoryRepository& repository_;
};

} // namespace ai::application
