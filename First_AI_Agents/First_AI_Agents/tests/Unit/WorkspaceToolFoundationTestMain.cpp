#include "ai/application/workspace/ToolRegistry.hpp"
#include "ai/application/agents/IAgentToolAuditRepository.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <cassert>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;

WorkspaceReadPolicy policy()
{
    auto value = WorkspaceReadPolicy::secureDefaults(
        fs::temp_directory_path() / "ai-workspace-tool-policy");
    value.allowedRoots = {"src", "include/public"};
    value.deniedPaths = {"src/private"};
    return value;
}

void testDefaultRegistry()
{
    const auto result = ToolRegistry::createDefaultReadOnly();
    assert(result.success());
    assert(result.value().definitions().size() == 6);
    assert(result.value().find("workspace_read_file").has_value());
    assert(!result.value().find("workspace_write_file").has_value());
}

void testRegistryRejectsWriteAndDuplicates()
{
    ToolRegistry registry;
    AgentToolDefinition writeTool {
        "workspace_write_file", "1", "write", "{}", false, 1024};
    const auto writeResult = registry.registerTool(writeTool);
    assert(!writeResult.success());
    assert(writeResult.error()->code == "tool_registry.write_tool_denied");

    AgentToolDefinition readTool {
        "workspace_read_file", "1", "read", "{}", true, 1024};
    assert(registry.registerTool(readTool).success());
    const auto duplicate = registry.registerTool(readTool);
    assert(!duplicate.success());
    assert(duplicate.error()->code == "tool_registry.duplicate_name");
}

void testCallValidation()
{
    const auto registryResult = ToolRegistry::createDefaultReadOnly();
    assert(registryResult.success());
    const ToolRegistry& registry = registryResult.value();
    AgentToolExecutionLimits limits;

    AgentToolCall call {
        "provider-call-1",
        "workspace_read_file",
        R"({"path":"src/main.cpp"})",
        0,
        0};
    assert(registry.validateCall(call, limits).success());

    call.toolName = "workspace_delete_file";
    const auto unknown = registry.validateCall(call, limits);
    assert(!unknown.success());
    assert(unknown.error()->code == "tool_call.unknown_tool");

    call.toolName = "workspace_read_file";
    call.argumentsJson.assign(limits.maxArgumentsBytes + 1, 'x');
    const auto oversized = registry.validateCall(call, limits);
    assert(!oversized.success());
    assert(oversized.error()->code == "tool_call.arguments_too_large");
}

void testSafeNestedAllowedPath()
{
    const auto result = WorkspaceReadPolicyValidator::validateRelativePath(
        "include/public/api/Service.hpp", policy());
    assert(result.success());
    assert(result.value() == fs::path("include/public/api/Service.hpp"));
}

void testTraversalAbsoluteAdsAndDevicesDenied()
{
    for (const fs::path& candidate : {
             fs::path("../outside.txt"),
             fs::path("C:/Windows/system.ini"),
             fs::path("//server/share/file.txt"),
             fs::path("src/file.txt:secret"),
             fs::path("src/CON.txt")}) {
        const auto result =
            WorkspaceReadPolicyValidator::validateRelativePath(candidate, policy());
        assert(!result.success());
        assert(result.error()->message.find(candidate.generic_string())
            == std::string::npos);
    }
}

void testSensitiveAndExplicitDenials()
{
    for (const fs::path& candidate : {
             fs::path("src/.git/config"),
             fs::path("src/.env"),
             fs::path("src/certificate.PEM"),
             fs::path("src/private/secret.cpp")}) {
        const auto result =
            WorkspaceReadPolicyValidator::validateRelativePath(candidate, policy());
        assert(!result.success());
        assert(result.error()->code == "workspace_path.denied");
    }
}

void testAllowedRootBoundary()
{
    const auto sibling = WorkspaceReadPolicyValidator::validateRelativePath(
        "include/publicity/file.hpp", policy());
    assert(!sibling.success());
    assert(sibling.error()->code == "workspace_path.outside_allowed_roots");

    const auto outside = WorkspaceReadPolicyValidator::validateRelativePath(
        "tests/file.cpp", policy());
    assert(!outside.success());
}

void testExactFileScopeDoesNotAuthorizeDescendants()
{
    auto exact = policy();
    exact.allowedRoots = {"src/config.json"};
    exact.allowedRootsAreExactPaths = true;

    assert(WorkspaceReadPolicyValidator::validateRelativePath(
        "src/config.json", exact).success());
    const auto child = WorkspaceReadPolicyValidator::validateRelativePath(
        "src/config.json/hidden.txt", exact);
    assert(!child.success());
    assert(child.error()->code == "workspace_path.outside_allowed_roots");
    assert(!WorkspaceReadPolicyValidator::validateRelativePath(
        "src/other.json", exact).success());
}

void testInvalidTrustedPolicy()
{
    WorkspaceReadPolicy invalid = policy();
    invalid.workspaceRoot = "relative/root";
    const auto result =
        WorkspaceReadPolicyValidator::validateTrustedPolicy(invalid);
    assert(!result.success());
    assert(result.error()->code == "workspace_policy.invalid_root");
}

void testAuditRecordContainsHashesInsteadOfPayloads()
{
    AgentToolAuditRecord record;
    record.auditId = "audit-1";
    record.argumentsSha256.assign(64, 'a');
    record.resultSha256.assign(64, 'b');
    record.resultArtifactId = "artifact-1";
    assert(record.argumentsSha256.size() == 64);
    assert(record.resultSha256.size() == 64);
    assert(record.resultArtifactId.has_value());
}

} // namespace

int main()
{
    testDefaultRegistry();
    testRegistryRejectsWriteAndDuplicates();
    testCallValidation();
    testSafeNestedAllowedPath();
    testTraversalAbsoluteAdsAndDevicesDenied();
    testSensitiveAndExplicitDenials();
    testAllowedRootBoundary();
    testExactFileScopeDoesNotAuthorizeDescendants();
    testInvalidTrustedPolicy();
    testAuditRecordContainsHashesInsteadOfPayloads();
    return 0;
}
