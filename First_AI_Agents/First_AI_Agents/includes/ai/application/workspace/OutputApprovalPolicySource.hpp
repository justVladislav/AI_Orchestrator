#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/workspace/ProjectOutputApproval.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <string>

namespace ai::application {

// P3c: the narrow port through which a consumer of a project's workspace asks for the read policy of ONE (project, workspace root)
// pair - today's secureDefaults plus the output folders the user explicitly approved for that pair (P3a). It is a trusted BACKEND
// source: the project id and the root come from the loaded Project / Run, never from a model, a tool argument or the UI.
//
// A failure is an error of its own. It is never "no approvals": a consumer that cannot learn the policy does not read the workspace.
class IOutputApprovalPolicySource {
public:
    virtual ~IOutputApprovalPolicySource() = default;

    [[nodiscard]] virtual common::Result<WorkspaceReadPolicy> policyFor(
        const ProjectId& projectId, const std::filesystem::path& workspaceRoot) const = 0;
};

// What the composition root hands to the consumers when the durable approvals are not there (no database): every question is a typed
// storage failure. A production consumer is therefore never left with a null source that would read as "defaults".
class UnavailableOutputApprovalPolicySource final : public IOutputApprovalPolicySource {
public:
    explicit UnavailableOutputApprovalPolicySource(std::string reason) : reason_(std::move(reason)) {}

    [[nodiscard]] common::Result<WorkspaceReadPolicy> policyFor(
        const ProjectId&, const std::filesystem::path&) const override
    {
        return common::Result<WorkspaceReadPolicy>::failure(
            {kOutputApprovalStorageFailedCode, "Output approvals are unavailable: " + reason_});
    }

private:
    std::string reason_;
};

// The comparison key of a root: lexically normal, without a trailing separator, in the project's own path-key form.
[[nodiscard]] inline std::string workspaceRootKey(const std::filesystem::path& root)
{
    std::filesystem::path normal = root.lexically_normal();
    if (!normal.has_filename() && normal.has_parent_path() && normal != normal.root_path()) normal = normal.parent_path();
    return detail::normalizedPathKey(normal);
}

// A policy is usable for a read of `expectedRoot` only when it is a valid trusted policy FOR THAT ROOT. The roots are compared
// component-wise with the project's own path-key rule (ASCII case-insensitive on Windows), never as a string prefix, so the policy of
// one folder cannot be used to read another - whatever produced it.
[[nodiscard]] inline common::Result<void> checkReadPolicyBoundToRoot(
    const WorkspaceReadPolicy& policy, const std::filesystem::path& expectedRoot)
{
    const auto valid = WorkspaceReadPolicyValidator::validateTrustedPolicy(policy);
    if (!valid.success()) return common::Result<void>::failure(*valid.error());
    if (expectedRoot.empty() || workspaceRootKey(policy.workspaceRoot) != workspaceRootKey(expectedRoot)) {
        return common::Result<void>::failure(
            {"workspace_policy.root_mismatch", "The read policy belongs to another workspace root than the one being read"});
    }
    return common::Result<void>::ok();
}

// The one entry the consumers use. With a source: its answer, checked against the root it was asked for. Without one (a standalone
// construction, a test fixture): exactly today's secureDefaults(root), as before P3c.
[[nodiscard]] inline common::Result<WorkspaceReadPolicy> resolveReadPolicy(
    const IOutputApprovalPolicySource* source, const ProjectId& projectId, const std::filesystem::path& workspaceRoot)
{
    using Result = common::Result<WorkspaceReadPolicy>;
    if (source == nullptr) return Result::ok(WorkspaceReadPolicy::secureDefaults(workspaceRoot));
    auto policy = source->policyFor(projectId, workspaceRoot);
    if (!policy.success()) return policy;
    const auto bound = checkReadPolicyBoundToRoot(policy.value(), workspaceRoot);
    if (!bound.success()) return Result::failure(*bound.error());
    return policy;
}

} // namespace ai::application
