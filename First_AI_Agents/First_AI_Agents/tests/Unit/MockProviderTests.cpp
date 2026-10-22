#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
#include "ai/providers/MockProvider.hpp"

#include <cassert>
#include <exception>
#include <iostream>
#include <string>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

int runAgentTests();
int runOrchestratorTests();
int runOpenAIProviderTests();
int runHttpTimeoutTests();
int runHttpRetryPolicyTests();
int runProjectContextBuilderTests();
int runApplicationPipelineTests();
int runComparisonServiceTests();
int runArtifactServiceTests();
int runWorkspaceAndCatalogTests();
int runWorkflowRoutingAndProductionTests();
int runChatContextAssemblerTests();
int runRunContextAssemblerTests();
int runProjectMemoryServiceTests();
int runChangeProposalTests();
int runRunPathClaimRegistryTests();
int runAgentToolTurnLoopRunnerTests();
int runExecutionProfileWorkflowRunnerWorkspaceTests();
int runAgentToolReadModelServiceTests();
int runChangeSetApplyServiceTests();
int runManifestApplyVerificationGateTests();
int runManifestWorkLedgerServiceTests();
int runManifestVerificationContinuationServiceTests();
int runManifestVerificationManualHandoffRepositoryTests();
int runManifestVerificationManualHandoffReadModelServiceTests();
int runRunWorkspaceScopeServiceTests();
int runVerificationEnvironmentDiscoveryServiceTests();
int runVerificationEnvironmentPresetSetupServiceTests();
int runExecutionManifestReadModelServiceTests();
int runManifestMandatoryVerificationPolicyTests();
int runManifestRoutingTests();
int runTaskRouteResolverTests();
int runLegacyRosterAdapterTests();
int runLegacyConversionRecordTests();
int runLegacyConversionCommandTests();
int runManifestMemberUsageTests();
int runManifestInterruptedTaskReconcilerTests();
int runManifestVerificationProfileBinderTests();
int runManifestBootstrapValidatorTests();
int runVerificationBootstrapRetryServiceTests();
int runProviderExecutionBudgetTests();
int runCostCalculationTests();
int runExecutionAnalyticsServiceTests();

namespace {

int runMockProviderTests()
{
    ai::providers::MockProvider provider;

    ai::models::ModelRequest request;
    request.options.model = "mock-direct";
    request.messages.push_back({
        ai::models::MessageRole::User,
        "Mock provider direct test"
    });

    const ai::models::ModelResponse response = provider.generate(request);
    assert(response.success);
    assert(response.provider == "mock");
    assert(response.model == "mock-direct");
    assert(response.text.find("MockProvider") != std::string::npos);
    assert(response.text.find("Mock provider direct test") != std::string::npos);

    return 0;
}

} // namespace

int runTests(int argc, char** argv)
{
    // Keep one deterministic, scoped entry point for the preset-setup
    // contract. The aggregate executable is intentionally broad and can be
    // dominated by unrelated long-running/flaky suites during hardening.
    if (argc == 2 && std::string(argv[1]) == "--verification-preset-setup-only") {
        return runVerificationEnvironmentPresetSetupServiceTests();
    }
    if (argc == 2 && std::string(argv[1]) == "--manual-handoff-read-model-only") {
        return runManifestVerificationManualHandoffReadModelServiceTests();
    }
    if (argc == 2 && std::string(argv[1]) == "--provider-budget-only") {
        return runProviderExecutionBudgetTests();
    }
    if (argc == 2 && std::string(argv[1]) == "--cost-calculation-only") {
        return runCostCalculationTests();
    }
    if (argc == 2 && std::string(argv[1]) == "--execution-analytics-only") {
        return runExecutionAnalyticsServiceTests();
    }
    // This project currently uses one small executable as the test runner.
    // Each subsystem exposes a run*Tests function and the runner aggregates them.
    runMockProviderTests();
    runAgentTests();
    runOrchestratorTests();
    runOpenAIProviderTests();
    runHttpTimeoutTests();
    runHttpRetryPolicyTests();
    runProjectContextBuilderTests();
    runApplicationPipelineTests();
    runComparisonServiceTests();
    runArtifactServiceTests();
    runWorkspaceAndCatalogTests();
    runWorkflowRoutingAndProductionTests();
    runChatContextAssemblerTests();
    runRunContextAssemblerTests();
    runProjectMemoryServiceTests();
    runChangeProposalTests();
    runRunPathClaimRegistryTests();
    runAgentToolTurnLoopRunnerTests();
    runExecutionProfileWorkflowRunnerWorkspaceTests();
    runAgentToolReadModelServiceTests();
    runChangeSetApplyServiceTests();
    if (runManifestApplyVerificationGateTests() != 0) return 1;
    runManifestWorkLedgerServiceTests();
    runManifestVerificationContinuationServiceTests();
    if (runManifestVerificationManualHandoffRepositoryTests() != 0) return 1;
    if (runManifestVerificationManualHandoffReadModelServiceTests() != 0) return 1;
    runRunWorkspaceScopeServiceTests();
    if (runVerificationEnvironmentDiscoveryServiceTests() != 0) return 1;
    if (runVerificationEnvironmentPresetSetupServiceTests() != 0) return 1;
    if (runExecutionManifestReadModelServiceTests() != 0) return 1;
    if (runManifestMandatoryVerificationPolicyTests() != 0) return 1;
    if (runManifestRoutingTests() != 0) return 1;
    if (runTaskRouteResolverTests() != 0) return 1;
    if (runLegacyRosterAdapterTests() != 0) return 1;
    if (runLegacyConversionRecordTests() != 0) return 1;
    if (runLegacyConversionCommandTests() != 0) return 1;
    if (runManifestMemberUsageTests() != 0) return 1;
    if (runManifestInterruptedTaskReconcilerTests() != 0) return 1;
    if (runManifestVerificationProfileBinderTests() != 0) return 1;
    if (runManifestBootstrapValidatorTests() != 0) return 1;
    if (runVerificationBootstrapRetryServiceTests() != 0) return 1;
    if (runProviderExecutionBudgetTests() != 0) return 1;
    if (runCostCalculationTests() != 0) return 1;
    if (runExecutionAnalyticsServiceTests() != 0) return 1;

    std::cout << "All tests passed\n";
    return 0;
}

int main(int argc, char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    // A failed assert() in this (assert-based) runner must END the process with its message on stderr,
    // never open a modal assertion window: a developer or a mutation run that breaks a test would
    // otherwise be blocked on a dialog. ctest records the non-zero exit either way.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try {
        return runTests(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Test failure: unknown non-standard exception\n";
        return 1;
    }
}
