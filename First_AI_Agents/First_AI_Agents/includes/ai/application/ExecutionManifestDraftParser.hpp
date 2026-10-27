#pragma once

#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/ExecutionManifestDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <string>
#include <string_view>
#include <vector>

// MW-P3A package 1: the untrusted-input boundary for a fresh
// ExecutionManifestDraft submission, and the tool contract that shape is
// advertised through. Not wired into ExecutionManifestPlanner yet - package
// 2 ("Backend Compiler + Read Semantics") switches the Planner's
// submit_execution_manifest tool/parse call site over to this draft plus a
// new ExecutionManifestDraftCompiler; until then this is dead code exercised
// only by its own dedicated tests, so its introduction carries zero
// behavioral risk to the live Manifest workflow.
namespace ai::application {

inline constexpr const char* kSubmitExecutionManifestDraftToolName =
    "submit_execution_manifest_draft";

// Strict-compatible (OpenAI Responses / Anthropic Messages strict subset)
// tool schema for ExecutionManifestDraft. Unlike the legacy
// submit_execution_manifest schema, a coordination task and a work task are
// two structurally distinct array item shapes (coordinationTasks vs
// workTasks) - a coordination task's object type has no path/acceptance
// properties at all (additionalProperties:false), and a work task's
// acceptanceContract is `"type":"object"` (never `["object","null"]`) with
// no wire-representable null. Both defects named in this header's .hpp are
// therefore structurally unrepresentable, not merely rejected after parse.
//
// AG-17.4: with a non-empty `assignableMemberIds` (a roster Run: the Backend's
// ManifestRouting::assignableMemberIds) every work task additionally REQUIRES
// `assigneeMemberId`, an enum of exactly those ids. With the default empty list
// (single-assignment) the schema is the unchanged execution-manifest-draft.v1
// text - the provider is never offered a field it must not use.
[[nodiscard]] AgentToolDefinition submitExecutionManifestDraftToolDefinition(
    const std::vector<std::string>& assignableMemberIds = {});

class ExecutionManifestDraftParser {
public:
    [[nodiscard]] static common::Result<ExecutionManifestDraft> parse(
        std::string_view rawSubmission);
};

} // namespace ai::application
