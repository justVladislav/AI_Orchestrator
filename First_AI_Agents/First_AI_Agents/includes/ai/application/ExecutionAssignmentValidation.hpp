#pragma once

#include "ai/application/catalog/ExecutionProfileTypes.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/common/Result.hpp"

#include <string_view>
#include <vector>

namespace ai::application {

/**
 * The single source of truth for "is this list of stage assignments safe to
 * save or execute". Used by both ExecutionProfileService (write-time
 * validation, when a profile is created/updated) and WorkflowRouter
 * (resolve-time re-validation, because a provider/model can be disabled
 * after a profile was saved - a stale-but-once-valid profile must not
 * silently execute against a now-disabled provider/model).
 *
 * Checks per assignment: known stageType, no stageType repeated within the
 * list, positive timeout, provider exists and is enabled, model exists, is
 * enabled, and belongs to the assignment's provider.
 */
[[nodiscard]] common::Result<void> validateExecutionAssignments(
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models);

/**
 * Validates constraints imposed by a SAVED profile's selected workflow. A saved
 * Manifest profile deliberately holds exactly one `architect` assignment: the
 * model plans and then executes every frozen task. Saved profiles persist only
 * stage/provider/model/timeout (the stage id is limited to three fixed names), so
 * they cannot carry a roster's lead/workspace/position/role/instructions - a
 * multi-member Manifest roster is configured per Run (validateManifestRoster).
 * Keep this at both profile-write and plan-resolution boundaries so an invalid
 * persisted profile cannot defer a predictable configuration error until worker
 * start.
 */
[[nodiscard]] common::Result<void> validateExecutionProfileWorkflow(
    std::string_view workflowId,
    const std::vector<StageModelAssignment>& assignments);

/**
 * AG-17.6: shape rules of a per-Run Manifest roster (execution-manifest-v1 with
 * several assignments): exactly one explicit lead Architect, a self-consistent
 * manifest-routing.v1 record (unique ids and positions, bounded identity fields)
 * and at least one non-lead worker with workspace access - a roster nobody can
 * serve could never plan, so it is refused before a Run, a planning-retry
 * authorization or a Planner accounting row exists. The roster is a pool of
 * available workers: a worker may end up with no task. Provider/model/policy
 * checks are validateRunRoster's.
 */
[[nodiscard]] common::Result<void> validateManifestRoster(
    const std::vector<StageModelAssignment>& assignments);

/** Validates an arbitrary per-run agent roster (one lead architect). */
[[nodiscard]] common::Result<void> validateRunRoster(
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models);

} // namespace ai::application
