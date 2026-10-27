set(TARGET_NAME First_AI_Agents)

option(
    AI_ORCHESTRATOR_BUILD_RUNTIME
    "Build CLI, provider runtime, smoke tests, and backend tests"
    ON
)

# Persistence is independently switchable so the Qt UI can keep its existing
# standalone Core/Application build until the desktop composition step opts in.
option(
    AI_ORCHESTRATOR_BUILD_PERSISTENCE
    "Build SQLite repository adapters and persistence tests"
    ${AI_ORCHESTRATOR_BUILD_RUNTIME}
)

# Backend persistence/runtime tests belong to the Backend verification graph.
# A desktop source-level composition may consume the production libraries
# without rebuilding these Backend-only test executables inside the Qt build.
option(
    AI_ORCHESTRATOR_BUILD_PERSISTENCE_TESTS
    "Build Backend persistence and BackendRuntime test executables"
    ON
)

# The first integration step establishes reusable library boundaries without
# moving source files. AIOrchestrator_Core contains provider-neutral execution
# primitives. AIOrchestrator_Runtime temporarily collects the existing workflow,
# provider, HTTP, and storage implementations; later phases split it into the
# approved application and infrastructure targets.
set(CORE_SOURCES
    sourceFiles/agents/Agent.cpp
    sourceFiles/orchestration/Orchestrator.cpp
    sourceFiles/orchestration/ExecutionGraph.cpp
)

set(RUNTIME_SOURCES
    sourceFiles/providers/OpenAIProvider.cpp
    sourceFiles/providers/OpenAIResponseParser.cpp
    sourceFiles/providers/AnthropicProvider.cpp
    sourceFiles/providers/AnthropicResponseParser.cpp
    sourceFiles/providers/MockProvider.cpp
    sourceFiles/orchestration/Workflow.cpp
    sourceFiles/orchestration/ProjectContextBuilder.cpp
    sourceFiles/http/HttpClient.cpp
    sourceFiles/http/HttpRetryPolicy.cpp
    sourceFiles/http/HttpTimeout.cpp
    sourceFiles/storage/FileStorage.cpp
    sourceFiles/storage/Workspace.cpp
)

set(APPLICATION_SOURCES
    sourceFiles/application/settings/ApplicationSettingsTypes.cpp
    sourceFiles/application/settings/ApplicationSettingsService.cpp
    sourceFiles/application/ApplicationTypes.cpp
    sourceFiles/application/artifacts/ArtifactTypes.cpp
    sourceFiles/application/artifacts/ArtifactPathPolicy.cpp
    sourceFiles/application/artifacts/ArtifactService.cpp
    sourceFiles/application/RunWarning.cpp
    sourceFiles/application/VerificationScratchPolicy.cpp
    sourceFiles/application/VerificationScratchService.cpp
    sourceFiles/application/VerificationToolchain.cpp
    sourceFiles/application/ProjectSetupReadiness.cpp
    sourceFiles/application/ProjectReadinessService.cpp
    sourceFiles/application/ExecutionAcceptanceContract.cpp
    sourceFiles/application/ManifestJsonParsingSupport.cpp
    sourceFiles/application/ExecutionManifestTypes.cpp
    sourceFiles/application/ExecutionManifestParser.cpp
    sourceFiles/application/ExecutionManifestDraftTypes.cpp
    sourceFiles/application/ExecutionManifestDraftParser.cpp
    sourceFiles/application/ExecutionManifestDraftCompiler.cpp
    sourceFiles/application/ExecutionManifestDependencyGraph.cpp
    sourceFiles/application/ExecutionManifestValidator.cpp
    sourceFiles/application/ExecutionManifestService.cpp
    sourceFiles/application/ExecutionManifestPlanner.cpp
    sourceFiles/application/ExecutionManifestPlannerTurnBudget.cpp
    sourceFiles/application/ExecutionManifestRepairControls.cpp
    sourceFiles/application/ExecutionManifestExecutionCapacity.cpp
    sourceFiles/application/ExecutionManifestScheduler.cpp
    sourceFiles/application/ExecutionManifestExecutor.cpp
    sourceFiles/application/ManifestVerificationRepairability.cpp
    sourceFiles/application/ManifestWorkflowRunner.cpp
    sourceFiles/application/ManifestWorkLedgerTypes.cpp
    sourceFiles/application/ManifestWorkLedgerService.cpp
    sourceFiles/application/ManifestInterruptedTaskReconciler.cpp
    sourceFiles/application/ManifestVerificationContinuationTypes.cpp
    sourceFiles/application/InMemoryManifestVerificationContinuationRepository.cpp
    sourceFiles/application/ManifestVerificationContinuationService.cpp
    sourceFiles/application/ManifestVerificationContinuationPreparationService.cpp
    sourceFiles/application/ManifestVerificationContinuationReadModelService.cpp
    sourceFiles/application/ManifestVerificationAttributionService.cpp
    sourceFiles/application/ManifestVerificationLegacyBaselineNormalizationService.cpp
    sourceFiles/application/InMemoryManifestVerificationBaselineNormalizationRepository.cpp
    sourceFiles/application/ManifestVerificationManualHandoffTypes.cpp
    sourceFiles/application/InMemoryManifestVerificationManualHandoffRepository.cpp
    sourceFiles/application/ManifestVerificationManualHandoffService.cpp
    sourceFiles/application/ManifestVerificationManualHandoffReadModelService.cpp
    sourceFiles/application/ManifestVerificationContinuationRepairService.cpp
    sourceFiles/application/RunWorkspaceScopeTypes.cpp
    sourceFiles/application/RunWorkspaceScopeService.cpp
    sourceFiles/application/VerificationEnvironmentSnapshotTypes.cpp
    sourceFiles/application/VerificationEnvironmentDiscoveryService.cpp
    sourceFiles/application/VerificationEnvironmentPresetSetupService.cpp
    sourceFiles/application/WorkspaceBaselineProvenance.cpp
    sourceFiles/application/InMemoryExecutionManifestRepository.cpp
    sourceFiles/application/InMemoryManifestTaskExecutionRepository.cpp
    sourceFiles/application/InMemoryManifestWorkLedgerRepository.cpp
    sourceFiles/application/InMemoryRunWorkspaceScopeRepository.cpp
    sourceFiles/application/InMemoryVerificationEnvironmentRepository.cpp
    sourceFiles/application/ExecutionManifestReadModelService.cpp
    sourceFiles/application/ContextSnapshot.cpp
    sourceFiles/application/Sha256.cpp
    sourceFiles/application/IdGenerator.cpp
    sourceFiles/application/InMemoryRepositories.cpp
    sourceFiles/application/projects/ProjectService.cpp
    sourceFiles/application/projects/TaskService.cpp
    sourceFiles/application/MockWorkflowRunner.cpp
    sourceFiles/application/RunStageSettlement.cpp
    sourceFiles/application/RunManager.cpp
    sourceFiles/application/OrchestrationService.cpp
    sourceFiles/application/WorkspaceContextService.cpp
    sourceFiles/application/catalog/ProviderCatalogTypes.cpp
    sourceFiles/application/catalog/ProviderCatalogService.cpp
    sourceFiles/application/ProviderExecutionBudget.cpp
    sourceFiles/application/CostCalculation.cpp
    sourceFiles/application/CacheDurationAccounting.cpp
    sourceFiles/application/ProviderRequestQuote.cpp
    sourceFiles/application/catalog/ExecutionProfileTypes.cpp
    sourceFiles/application/catalog/ExecutionProfileService.cpp
    sourceFiles/application/RunDraftService.cpp
    sourceFiles/application/ExecutionAssignmentValidation.cpp
    sourceFiles/application/WorkflowRouter.cpp
    sourceFiles/application/ExecutionProfileWorkflowRunner.cpp
    sourceFiles/application/AgentToolTurnLoopRunner.cpp
    sourceFiles/application/ManifestWorkerNoChanges.cpp
    sourceFiles/application/ManifestProposalOverlay.cpp
    sourceFiles/application/ManifestProposalSetVerifier.cpp
    sourceFiles/application/QmlLintDiagnosticPolicy.cpp
    sourceFiles/application/ManifestVerificationEnvironmentPreflight.cpp
    sourceFiles/application/CredentialManagementService.cpp
    sourceFiles/application/conversations/ChatTypes.cpp
    sourceFiles/application/conversations/ChatContextAssembler.cpp
    sourceFiles/application/conversations/ChatProjectContext.cpp
    sourceFiles/application/RunContextAssembler.cpp
    sourceFiles/application/ProjectMemoryService.cpp
    sourceFiles/application/ProjectMemoryCompactionService.cpp
    sourceFiles/application/ProjectContextSnapshot.cpp
    sourceFiles/application/ProjectContextSnapshotService.cpp
    sourceFiles/application/RunInterfaceContract.cpp
    sourceFiles/application/NextRunPlan.cpp
    sourceFiles/application/FollowUpPlan.cpp
    sourceFiles/application/FollowUpComposerContext.cpp
    sourceFiles/application/FollowUpComposerQuote.cpp
    sourceFiles/application/FollowUpPlanAdvisories.cpp
    sourceFiles/application/FollowUpPlanRequest.cpp
    sourceFiles/application/InMemoryFollowUpPlanRepository.cpp
    sourceFiles/application/FollowUpAdmission.cpp
    sourceFiles/application/FollowUpPlanService.cpp
    sourceFiles/application/FollowUpComposerProcessor.cpp
    sourceFiles/application/FilesystemFollowUpFileLoader.cpp
    sourceFiles/application/FollowUpSourceReader.cpp
    sourceFiles/application/FollowUpPlanReadModel.cpp
    sourceFiles/application/OutcomeBrief.cpp
    sourceFiles/application/OutcomeBriefRepository.cpp
    sourceFiles/application/InMemoryOutcomeBriefRepository.cpp
    sourceFiles/application/OutcomeBriefSources.cpp
    sourceFiles/application/OutcomeBriefReconciler.cpp
    sourceFiles/application/OutcomeBriefReadModelService.cpp
    sourceFiles/application/RunDraftGroup.cpp
    sourceFiles/application/RunDraftPlanService.cpp
    sourceFiles/application/ChangeProposalTypes.cpp
    sourceFiles/application/ProposalReviewService.cpp
    sourceFiles/application/ManifestProposalContentHash.cpp
    sourceFiles/application/ManifestVerificationManualHandoffApplyAuthority.cpp
    sourceFiles/application/ManifestApplyVerificationGate.cpp
    sourceFiles/application/ManifestMandatoryVerificationPolicy.cpp
    sourceFiles/application/ManifestRouting.cpp
    sourceFiles/application/TaskRouteResolver.cpp
    sourceFiles/application/LegacyRosterAdapter.cpp
    sourceFiles/application/LegacyConversionRecord.cpp
    sourceFiles/application/ManifestMemberUsage.cpp
    sourceFiles/application/ManifestVerificationProfileBinder.cpp
    sourceFiles/application/VerificationBootstrapTypes.cpp
    sourceFiles/application/InMemoryVerificationBootstrapRepository.cpp
    sourceFiles/application/ManifestBootstrapValidator.cpp
    sourceFiles/application/VerificationBootstrapRetryService.cpp
    sourceFiles/application/ChangeSetApplyService.cpp
    sourceFiles/application/ProposedChangeSetParser.cpp
    sourceFiles/application/ChangeProposalValidator.cpp
    sourceFiles/application/RunPathClaimRegistry.cpp
    sourceFiles/application/ChangeProposalService.cpp
    sourceFiles/application/WorkspaceReadPolicy.cpp
    sourceFiles/application/WorkspacePathGuard.cpp
    sourceFiles/application/ToolRegistry.cpp
    sourceFiles/application/FilesystemWorkspaceToolExecutor.cpp
    sourceFiles/application/AgentToolExecutionService.cpp
    sourceFiles/application/AgentToolReadModelService.cpp
    sourceFiles/application/conversations/ChatService.cpp
    sourceFiles/application/ModelExecutionTypes.cpp
    sourceFiles/application/InMemoryModelExecutionRepository.cpp
    sourceFiles/application/ModelExecutionService.cpp
    sourceFiles/application/ExecutionAnalyticsService.cpp
    sourceFiles/application/comparison/ComparisonTypes.cpp
    sourceFiles/application/InMemoryComparisonSessionRepository.cpp
    sourceFiles/application/comparison/ComparisonService.cpp
    sourceFiles/application/comparison/ComparisonReadModelService.cpp
    sourceFiles/application/comparison/ComparisonQualityEvaluator.cpp
    sourceFiles/application/comparison/ComparisonExecutionManager.cpp
    sourceFiles/application/ReleaseEvaluationService.cpp
)

function(ai_orchestrator_configure_target target_name)
    set_target_properties(${target_name}
        PROPERTIES
            CXX_EXTENSIONS OFF
            # A parent Qt project enables AUTOMOC globally. Explicitly keep
            # every Backend target free of Qt code generation when it is
            # consumed through add_subdirectory by the desktop composition.
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(AIOrchestrator_Core STATIC
    ${CORE_SOURCES}
)
add_library(AIOrchestrator::Core ALIAS AIOrchestrator_Core)

target_include_directories(AIOrchestrator_Core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/includes
)
target_compile_features(AIOrchestrator_Core PUBLIC cxx_std_20)
ai_orchestrator_configure_target(AIOrchestrator_Core)

add_library(AIOrchestrator_Application STATIC
    ${APPLICATION_SOURCES}
)
add_library(AIOrchestrator::Application ALIAS AIOrchestrator_Application)

target_include_directories(AIOrchestrator_Application
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/includes
)
target_compile_features(AIOrchestrator_Application PUBLIC cxx_std_20)
target_link_libraries(AIOrchestrator_Application PUBLIC AIOrchestrator::Core)
ai_orchestrator_configure_target(AIOrchestrator_Application)

add_executable(ExecutionManifest_tests
    tests/Unit/ExecutionManifestTests.cpp
)
target_compile_features(ExecutionManifest_tests PRIVATE cxx_std_20)
target_link_libraries(ExecutionManifest_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ExecutionManifest_tests)
add_test(NAME ExecutionManifest_tests COMMAND ExecutionManifest_tests)

add_executable(ExecutionManifestPlanner_tests
    tests/Unit/ExecutionManifestPlannerTests.cpp
)
target_compile_features(ExecutionManifestPlanner_tests PRIVATE cxx_std_20)
target_link_libraries(ExecutionManifestPlanner_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ExecutionManifestPlanner_tests)
add_test(NAME ExecutionManifestPlanner_tests COMMAND ExecutionManifestPlanner_tests)

add_executable(ProjectContextSnapshot_tests
    tests/Unit/ProjectContextSnapshotTests.cpp
)
target_compile_features(ProjectContextSnapshot_tests PRIVATE cxx_std_20)
target_link_libraries(ProjectContextSnapshot_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ProjectContextSnapshot_tests)
add_test(NAME ProjectContextSnapshot_tests COMMAND ProjectContextSnapshot_tests)

add_executable(ChatProjectContext_tests
    tests/Unit/ChatProjectContextTests.cpp
)
target_compile_features(ChatProjectContext_tests PRIVATE cxx_std_20)
target_link_libraries(ChatProjectContext_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ChatProjectContext_tests)
add_test(NAME ChatProjectContext_tests COMMAND ChatProjectContext_tests)

add_executable(ProjectContextSnapshotService_tests
    tests/Unit/ProjectContextSnapshotServiceTests.cpp
)
target_compile_features(ProjectContextSnapshotService_tests PRIVATE cxx_std_20)
target_link_libraries(ProjectContextSnapshotService_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ProjectContextSnapshotService_tests)
add_test(NAME ProjectContextSnapshotService_tests COMMAND ProjectContextSnapshotService_tests)

add_executable(RunInterfaceContract_tests
    tests/Unit/RunInterfaceContractTests.cpp
)
target_compile_features(RunInterfaceContract_tests PRIVATE cxx_std_20)
target_link_libraries(RunInterfaceContract_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(RunInterfaceContract_tests)
add_test(NAME RunInterfaceContract_tests COMMAND RunInterfaceContract_tests)

add_executable(NextRunPlan_tests
    tests/Unit/NextRunPlanTests.cpp
)
target_compile_features(NextRunPlan_tests PRIVATE cxx_std_20)
target_link_libraries(NextRunPlan_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(NextRunPlan_tests)
add_test(NAME NextRunPlan_tests COMMAND NextRunPlan_tests)

add_executable(FollowUpPlan_tests
    tests/Unit/FollowUpPlanTests.cpp
)
target_compile_features(FollowUpPlan_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpPlan_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpPlan_tests)
add_test(NAME FollowUpPlan_tests COMMAND FollowUpPlan_tests)

add_executable(FollowUpComposerContext_tests
    tests/Unit/FollowUpComposerContextTests.cpp
)
target_compile_features(FollowUpComposerContext_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpComposerContext_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpComposerContext_tests)
add_test(NAME FollowUpComposerContext_tests COMMAND FollowUpComposerContext_tests)

add_executable(FollowUpComposerQuote_tests
    tests/Unit/FollowUpComposerQuoteTests.cpp
)
target_compile_features(FollowUpComposerQuote_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpComposerQuote_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpComposerQuote_tests)
add_test(NAME FollowUpComposerQuote_tests COMMAND FollowUpComposerQuote_tests)

add_executable(FollowUpPlanRequest_tests
    tests/Unit/FollowUpPlanRequestTests.cpp
)
target_compile_features(FollowUpPlanRequest_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpPlanRequest_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpPlanRequest_tests)
add_test(NAME FollowUpPlanRequest_tests COMMAND FollowUpPlanRequest_tests)

add_executable(FollowUpPlanRequestRepository_tests
    tests/Unit/FollowUpPlanRequestRepositoryInMemoryTests.cpp
)
target_compile_features(FollowUpPlanRequestRepository_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpPlanRequestRepository_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpPlanRequestRepository_tests)
add_test(NAME FollowUpPlanRequestRepository_tests COMMAND FollowUpPlanRequestRepository_tests)

add_executable(FollowUpPlanService_tests
    tests/Unit/FollowUpPlanServiceTests.cpp
)
target_compile_features(FollowUpPlanService_tests PRIVATE cxx_std_20)
target_compile_definitions(FollowUpPlanService_tests PRIVATE FOLLOW_UP_SERVICE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(FollowUpPlanService_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpPlanService_tests)
add_test(NAME FollowUpPlanService_tests COMMAND FollowUpPlanService_tests)

add_executable(FollowUpSourceReader_tests
    tests/Unit/FollowUpSourceReaderTests.cpp
)
target_compile_features(FollowUpSourceReader_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpSourceReader_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpSourceReader_tests)
add_test(NAME FollowUpSourceReader_tests COMMAND FollowUpSourceReader_tests)

add_executable(FollowUpPlanReadModel_tests
    tests/Unit/FollowUpPlanReadModelTests.cpp
)
target_compile_features(FollowUpPlanReadModel_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpPlanReadModel_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpPlanReadModel_tests)
add_test(NAME FollowUpPlanReadModel_tests COMMAND FollowUpPlanReadModel_tests)

add_executable(FollowUpPlanAdvisories_tests
    tests/Unit/FollowUpPlanAdvisoriesTests.cpp
)
target_compile_features(FollowUpPlanAdvisories_tests PRIVATE cxx_std_20)
target_link_libraries(FollowUpPlanAdvisories_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(FollowUpPlanAdvisories_tests)
add_test(NAME FollowUpPlanAdvisories_tests COMMAND FollowUpPlanAdvisories_tests)

add_executable(OutcomeBrief_tests
    tests/Unit/OutcomeBriefTests.cpp
)
target_compile_features(OutcomeBrief_tests PRIVATE cxx_std_20)
target_link_libraries(OutcomeBrief_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(OutcomeBrief_tests)
add_test(NAME OutcomeBrief_tests COMMAND OutcomeBrief_tests)

add_executable(CacheDurationAccounting_tests
    tests/Unit/CacheDurationAccountingTests.cpp
)
target_compile_features(CacheDurationAccounting_tests PRIVATE cxx_std_20)
target_link_libraries(CacheDurationAccounting_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(CacheDurationAccounting_tests)
add_test(NAME CacheDurationAccounting_tests COMMAND CacheDurationAccounting_tests)

add_executable(ProviderRequestQuote_tests
    tests/Unit/ProviderRequestQuoteTests.cpp
)
target_compile_features(ProviderRequestQuote_tests PRIVATE cxx_std_20)
target_link_libraries(ProviderRequestQuote_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ProviderRequestQuote_tests)
add_test(NAME ProviderRequestQuote_tests COMMAND ProviderRequestQuote_tests)

add_executable(OutcomeBriefRepository_tests
    tests/Unit/OutcomeBriefRepositoryInMemoryTests.cpp
)
target_compile_features(OutcomeBriefRepository_tests PRIVATE cxx_std_20)
target_link_libraries(OutcomeBriefRepository_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(OutcomeBriefRepository_tests)
add_test(NAME OutcomeBriefRepository_tests COMMAND OutcomeBriefRepository_tests)

add_executable(OutcomeBriefReconciler_tests
    tests/Unit/OutcomeBriefReconcilerTests.cpp
)
target_compile_features(OutcomeBriefReconciler_tests PRIVATE cxx_std_20)
target_link_libraries(OutcomeBriefReconciler_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(OutcomeBriefReconciler_tests)
add_test(NAME OutcomeBriefReconciler_tests COMMAND OutcomeBriefReconciler_tests)

add_executable(OutcomeBriefReadModel_tests
    tests/Unit/OutcomeBriefReadModelTests.cpp
)
target_compile_features(OutcomeBriefReadModel_tests PRIVATE cxx_std_20)
target_link_libraries(OutcomeBriefReadModel_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(OutcomeBriefReadModel_tests)
add_test(NAME OutcomeBriefReadModel_tests COMMAND OutcomeBriefReadModel_tests)

add_executable(MockWorkflowRunner_tests
    tests/Unit/MockWorkflowRunnerTests.cpp
)
target_compile_features(MockWorkflowRunner_tests PRIVATE cxx_std_20)
target_link_libraries(MockWorkflowRunner_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(MockWorkflowRunner_tests)
add_test(NAME MockWorkflowRunner_tests COMMAND MockWorkflowRunner_tests)

add_executable(RunDraftGroup_tests
    tests/Unit/RunDraftGroupTests.cpp
)
target_compile_features(RunDraftGroup_tests PRIVATE cxx_std_20)
target_link_libraries(RunDraftGroup_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(RunDraftGroup_tests)
add_test(NAME RunDraftGroup_tests COMMAND RunDraftGroup_tests)

add_executable(RunDraftPlanService_tests
    tests/Unit/RunDraftPlanServiceTests.cpp
)
target_compile_features(RunDraftPlanService_tests PRIVATE cxx_std_20)
target_link_libraries(RunDraftPlanService_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(RunDraftPlanService_tests)
add_test(NAME RunDraftPlanService_tests COMMAND RunDraftPlanService_tests)

add_executable(ExecutionManifestDraft_tests
    tests/Unit/ExecutionManifestDraftParserTests.cpp
)
target_compile_features(ExecutionManifestDraft_tests PRIVATE cxx_std_20)
target_link_libraries(ExecutionManifestDraft_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ExecutionManifestDraft_tests)
add_test(NAME ExecutionManifestDraft_tests COMMAND ExecutionManifestDraft_tests)

add_executable(ExecutionManifestDraftCompiler_tests
    tests/Unit/ExecutionManifestDraftCompilerTests.cpp
)
target_compile_features(ExecutionManifestDraftCompiler_tests PRIVATE cxx_std_20)
target_link_libraries(ExecutionManifestDraftCompiler_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ExecutionManifestDraftCompiler_tests)
add_test(NAME ExecutionManifestDraftCompiler_tests COMMAND ExecutionManifestDraftCompiler_tests)

add_executable(ExecutionManifestExecutor_tests
    tests/Unit/ExecutionManifestExecutorTests.cpp
)
target_compile_features(ExecutionManifestExecutor_tests PRIVATE cxx_std_20)
target_link_libraries(ExecutionManifestExecutor_tests PRIVATE AIOrchestrator::Application AIOrchestrator::Persistence)
ai_orchestrator_configure_target(ExecutionManifestExecutor_tests)
add_test(NAME ExecutionManifestExecutor_tests COMMAND ExecutionManifestExecutor_tests)

add_executable(ManifestProposalVerification_tests
    tests/Unit/ManifestProposalVerificationTests.cpp
)
target_compile_features(ManifestProposalVerification_tests PRIVATE cxx_std_20)
target_link_libraries(ManifestProposalVerification_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ManifestProposalVerification_tests)
add_test(NAME ManifestProposalVerification_tests COMMAND ManifestProposalVerification_tests)

# AG-18.3 hardening (blocker 3): a real, temporary, minimal CMake project
# fixture - no scripted/fake command runner - proving candidate-side target
# discovery against actual `cmake --preset`/`--target help` output. Kept as
# its own executable/CTest target (like ManifestProposalVerification_tests
# above) so it can be run and reported separately per the verification
# protocol.
add_executable(VerificationEnvironmentCandidateDiscovery_tests
    tests/Unit/VerificationEnvironmentCandidateDiscoveryTests.cpp
)
target_compile_features(VerificationEnvironmentCandidateDiscovery_tests PRIVATE cxx_std_20)
target_link_libraries(VerificationEnvironmentCandidateDiscovery_tests PRIVATE AIOrchestrator::Application)
target_compile_definitions(VerificationEnvironmentCandidateDiscovery_tests PRIVATE
    AI_TEST_CMAKE_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
)
ai_orchestrator_configure_target(VerificationEnvironmentCandidateDiscovery_tests)
add_test(NAME VerificationEnvironmentCandidateDiscovery_tests
    COMMAND VerificationEnvironmentCandidateDiscovery_tests)

add_executable(WorkspaceBaselineProvenance_tests
    tests/Unit/WorkspaceBaselineProvenanceTests.cpp
)
target_compile_features(WorkspaceBaselineProvenance_tests PRIVATE cxx_std_20)
target_link_libraries(WorkspaceBaselineProvenance_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(WorkspaceBaselineProvenance_tests)
add_test(NAME WorkspaceBaselineProvenance_tests COMMAND WorkspaceBaselineProvenance_tests)

add_executable(ManifestWorkflowRunner_tests
    tests/Unit/ManifestWorkflowRunnerTests.cpp
)
target_compile_features(ManifestWorkflowRunner_tests PRIVATE cxx_std_20)
target_link_libraries(ManifestWorkflowRunner_tests PRIVATE AIOrchestrator::Application AIOrchestrator::Persistence)
ai_orchestrator_configure_target(ManifestWorkflowRunner_tests)
add_test(NAME ManifestWorkflowRunner_tests COMMAND ManifestWorkflowRunner_tests)

add_executable(ReleaseEvaluationService_tests
    tests/Unit/ReleaseEvaluationServiceTests.cpp
)
target_compile_features(ReleaseEvaluationService_tests PRIVATE cxx_std_20)
target_link_libraries(ReleaseEvaluationService_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ReleaseEvaluationService_tests)
add_test(NAME ReleaseEvaluationService_tests COMMAND ReleaseEvaluationService_tests)

add_executable(WorkspaceToolFoundation_tests
    tests/Unit/WorkspaceToolFoundationTestMain.cpp
)
target_compile_features(WorkspaceToolFoundation_tests PRIVATE cxx_std_20)
target_link_libraries(WorkspaceToolFoundation_tests
    PRIVATE
        AIOrchestrator::Application
)
ai_orchestrator_configure_target(WorkspaceToolFoundation_tests)
add_test(NAME WorkspaceToolFoundation_tests COMMAND WorkspaceToolFoundation_tests)

# Per-tool + adversarial coverage for FilesystemWorkspaceToolExecutor
# (path safety, content policies, secret-scan precision). This suite existed
# in tests/Unit/ but was never wired into the build - added 2026-07-24 so it
# actually runs under CTest.
add_executable(FilesystemWorkspaceExecutor_tests
    tests/Unit/FilesystemWorkspaceExecutorTestMain.cpp
    tests/Unit/FilesystemWorkspaceExecutorTests.cpp
)
target_compile_features(FilesystemWorkspaceExecutor_tests PRIVATE cxx_std_20)
target_link_libraries(FilesystemWorkspaceExecutor_tests
    PRIVATE
        AIOrchestrator::Application
)
ai_orchestrator_configure_target(FilesystemWorkspaceExecutor_tests)
add_test(NAME FilesystemWorkspaceExecutor_tests COMMAND FilesystemWorkspaceExecutor_tests)

# Provider-neutral agent-session contract tests.
# Links only AIOrchestrator::Application: no CURL, no SQLite, no Qt.
# The ScriptedProviderAgentSession fake and all test logic live in the
# test source files; no new production .cpp is required.
add_executable(ProviderAgentSession_contract_tests
    tests/Unit/ProviderAgentSessionTestMain.cpp
    tests/Unit/ProviderAgentSessionContractTests.cpp
)
target_compile_features(ProviderAgentSession_contract_tests PRIVATE cxx_std_20)
target_link_libraries(ProviderAgentSession_contract_tests
    PRIVATE
        AIOrchestrator::Application
)
ai_orchestrator_configure_target(ProviderAgentSession_contract_tests)
add_test(NAME ProviderAgentSession_contract_tests COMMAND ProviderAgentSession_contract_tests)

# Read-only workspace path guard tests.
# Covers lexical + OS-level canonical containment + Windows reparse-point checks.
# Links only AIOrchestrator::Application: no CURL, no SQLite, no Qt.
add_executable(WorkspacePathGuard_tests
    tests/Unit/WorkspacePathGuardTestMain.cpp
    tests/Unit/WorkspacePathGuardTests.cpp
)
target_compile_features(WorkspacePathGuard_tests PRIVATE cxx_std_20)
target_link_libraries(WorkspacePathGuard_tests
    PRIVATE
        AIOrchestrator::Application
)
ai_orchestrator_configure_target(WorkspacePathGuard_tests)
add_test(NAME WorkspacePathGuard_tests COMMAND WorkspacePathGuard_tests)

if(AI_ORCHESTRATOR_BUILD_PERSISTENCE)

# Native SQLite only. This backend must build and link without the Qt SDK; an
# earlier revision of this target linked Qt6::Sql to work around a UI build
# problem, which made First_AI_Agents depend on Qt to compile at all. That was
# reverted: Persistence talks to sqlite3 directly (see
# sourceFiles/infrastructure/sqlite/), and only the desktop UI project (a
# separate, Qt-based CMake project) is responsible for deploying a native sqlite3 runtime beside itself if needed.
# This lookup is owned by the Backend subproject. It is required both when the
# Backend is configured alone and when it is embedded into the Qt desktop.
#
# Windows resolves it through the vcpkg unofficial-sqlite3 CONFIG package (see
# the manifest in this directory). Every other platform (macOS/Linux) falls
# back to CMake's built-in FindSQLite3 module against a system/Homebrew
# sqlite3 development package. AIOrchestrator_Sqlite3_Target abstracts the two
# different imported-target names for the rest of this file.
if(WIN32)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)

    # Fail next to dependency discovery with an actionable diagnostic instead of
    # deferring an undefined imported-target error to target_link_libraries below.
    if(NOT TARGET unofficial::sqlite3::sqlite3)
        message(FATAL_ERROR
            "The sqlite3 package was found without the expected "
            "unofficial::sqlite3::sqlite3 imported target. Configure with the "
            "Backend vcpkg toolchain and manifest."
        )
    endif()

    set(AIOrchestrator_Sqlite3_Target unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)

    # CMake's FindSQLite3 has been transitioning its imported-target name from
    # the deprecated SQLite::SQLite3 to SQLite3::SQLite3; prefer the new name
    # when the running CMake defines it so configure stays warning-free, and
    # fall back for older CMake versions that only define the old one.
    if(TARGET SQLite3::SQLite3)
        set(AIOrchestrator_Sqlite3_Target SQLite3::SQLite3)
    elseif(TARGET SQLite::SQLite3)
        set(AIOrchestrator_Sqlite3_Target SQLite::SQLite3)
    else()
        message(FATAL_ERROR
            "The SQLite3 development package was not found. On macOS install "
            "it with Homebrew: brew install sqlite3"
        )
    endif()
endif()

add_library(AIOrchestrator_Persistence STATIC
    sourceFiles/infrastructure/sqlite/SqliteDatabase.cpp
    sourceFiles/infrastructure/sqlite/SqliteSchema.cpp
    sourceFiles/infrastructure/sqlite/SqlitePrimitives.cpp
    sourceFiles/infrastructure/sqlite/catalog/SqliteExecutionProfileRepository.cpp
    sourceFiles/infrastructure/sqlite/catalog/SqliteModelRepository.cpp
    sourceFiles/infrastructure/sqlite/catalog/SqliteProfileMapping.cpp
    sourceFiles/infrastructure/sqlite/catalog/SqliteProviderRepository.cpp
    sourceFiles/infrastructure/sqlite/conversations/SqliteChatMemoryRepository.cpp
    sourceFiles/infrastructure/sqlite/conversations/SqliteChatRepository.cpp
    sourceFiles/infrastructure/sqlite/conversations/SqliteProjectMemoryRepository.cpp
    sourceFiles/infrastructure/sqlite/conversations/SqliteProjectMemoryCompactionRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteArtifactRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteRunWarningRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteManifestTaskExecutionRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteManifestWorkLedgerRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteRunWorkspaceScopeRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteExecutionManifestRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteRunRepository.cpp
    sourceFiles/infrastructure/sqlite/verification/SqliteManifestVerificationBaselineNormalizationRepository.cpp
    sourceFiles/infrastructure/sqlite/verification/SqliteManifestVerificationManualHandoffRepository.cpp
    sourceFiles/infrastructure/sqlite/verification/SqliteManifestVerificationContinuationRepository.cpp
    sourceFiles/infrastructure/sqlite/verification/SqliteVerificationEnvironmentRepository.cpp
    sourceFiles/infrastructure/sqlite/verification/SqliteVerificationBootstrapRepository.cpp
    sourceFiles/infrastructure/sqlite/agents/SqliteModelExecutionRepository.cpp
    sourceFiles/infrastructure/sqlite/proposals/SqliteChangeProposalRepository.cpp
    sourceFiles/infrastructure/sqlite/proposals/SqliteChangeApprovalRepository.cpp
    sourceFiles/infrastructure/sqlite/proposals/SqliteProposalReviewRepository.cpp
    sourceFiles/infrastructure/sqlite/agents/SqliteAgentToolSessionRepository.cpp
    sourceFiles/infrastructure/sqlite/agents/SqliteAgentToolAuditRepository.cpp
    sourceFiles/infrastructure/sqlite/comparison/SqliteComparisonSessionRepository.cpp
    sourceFiles/infrastructure/sqlite/planning/SqliteFollowUpPlanRepository.cpp
    sourceFiles/infrastructure/sqlite/planning/SqliteOutcomeBriefRepository.cpp
    sourceFiles/infrastructure/sqlite/planning/SqliteRunDraftRepository.cpp
    sourceFiles/infrastructure/sqlite/execution/SqliteRunStatusMapping.cpp
    sourceFiles/infrastructure/sqlite/projects/SqliteProjectReadinessRepository.cpp
    sourceFiles/infrastructure/sqlite/roster/SqliteRosterMapping.cpp
    sourceFiles/infrastructure/sqlite/projects/SqliteProjectRepository.cpp
    sourceFiles/infrastructure/sqlite/projects/SqliteTaskRepository.cpp
    sourceFiles/infrastructure/sqlite/projects/SqliteWorkspaceStateRepository.cpp
    sourceFiles/infrastructure/sqlite/settings/SqliteApplicationSettingsRepository.cpp
    sourceFiles/infrastructure/sqlite/settings/SqliteVerificationScratchSettingRepository.cpp
    sourceFiles/infrastructure/DatabaseOwnerLock.cpp
    sourceFiles/infrastructure/FileArtifactStore.cpp
    sourceFiles/infrastructure/ChangeProposalStore.cpp
)
add_library(AIOrchestrator::Persistence ALIAS AIOrchestrator_Persistence)

target_include_directories(AIOrchestrator_Persistence
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/includes
)
target_compile_features(AIOrchestrator_Persistence PUBLIC cxx_std_20)
target_link_libraries(AIOrchestrator_Persistence
    PUBLIC
        AIOrchestrator::Application
    PRIVATE
        ${AIOrchestrator_Sqlite3_Target}
)
ai_orchestrator_configure_target(AIOrchestrator_Persistence)

if(AI_ORCHESTRATOR_BUILD_PERSISTENCE_TESTS)
    # Persistence has a dedicated executable so migration/reopen failures are
    # reported independently from provider/runtime tests in CTest and CI. It
    # remains plain C++ and uses the same native sqlite3 dependency as the
    # production adapter.
    add_executable(AIOrchestrator_Persistence_tests
        tests/Unit/SqliteRepositoryTestMain.cpp
          tests/Unit/SqliteRepositoryTests.cpp
          tests/Unit/ModelExecutionRepositoryTests.cpp
          tests/Unit/ChatServicePricingTests.cpp
          tests/Unit/AgentToolAuditTests.cpp
          tests/Unit/RunDraftRepositoryTests.cpp
          tests/Unit/OutcomeBriefRepositoryTests.cpp
          tests/Unit/FollowUpPlanRequestRepositoryTests.cpp
          tests/Unit/ExecutionManifestRepositoryTests.cpp
          tests/Unit/ManifestTaskExecutionRepositoryTests.cpp
          tests/Unit/ManifestWorkLedgerRepositoryTests.cpp
          tests/Unit/ManifestVerificationContinuationRepositoryTests.cpp
          tests/Unit/RunWorkspaceScopeRepositoryTests.cpp
          tests/Unit/VerificationEnvironmentSnapshotRepositoryTests.cpp
          tests/Unit/VerificationBootstrapRepositoryTests.cpp
      )
    target_compile_features(AIOrchestrator_Persistence_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_Persistence_tests
        PRIVATE
            AIOrchestrator::Persistence
            ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_Persistence_tests)
    add_test(NAME AIOrchestrator_Persistence_tests COMMAND AIOrchestrator_Persistence_tests)

    # Feature-owned persistence contracts also run against the monolithic
    # adapter before extraction; they must not depend on private SQLite helpers.
    add_executable(AIOrchestrator_SqliteComparison_tests
        tests/Unit/SqliteComparisonRepositoryTests.cpp
    )
    target_compile_features(AIOrchestrator_SqliteComparison_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_SqliteComparison_tests
        PRIVATE AIOrchestrator::Persistence ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_SqliteComparison_tests)
    add_test(NAME AIOrchestrator_SqliteComparison_tests COMMAND AIOrchestrator_SqliteComparison_tests)

    add_executable(AIOrchestrator_SqliteCatalog_tests
        tests/Unit/SqliteCatalogRepositoryTests.cpp
    )
    target_compile_features(AIOrchestrator_SqliteCatalog_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_SqliteCatalog_tests
        PRIVATE AIOrchestrator::Persistence ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_SqliteCatalog_tests)
    add_test(NAME AIOrchestrator_SqliteCatalog_tests COMMAND AIOrchestrator_SqliteCatalog_tests)

    add_executable(AIOrchestrator_SqliteProjects_tests
        tests/Unit/SqliteProjectsRepositoryTests.cpp
    )
    target_compile_features(AIOrchestrator_SqliteProjects_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_SqliteProjects_tests
        PRIVATE AIOrchestrator::Persistence ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_SqliteProjects_tests)
    add_test(NAME AIOrchestrator_SqliteProjects_tests COMMAND AIOrchestrator_SqliteProjects_tests)

    add_executable(AIOrchestrator_SqliteSettings_tests
        tests/Unit/SqliteSettingsRepositoryTests.cpp
    )
    target_compile_features(AIOrchestrator_SqliteSettings_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_SqliteSettings_tests
        PRIVATE AIOrchestrator::Persistence ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_SqliteSettings_tests)
    add_test(NAME AIOrchestrator_SqliteSettings_tests COMMAND AIOrchestrator_SqliteSettings_tests)

    add_executable(AIOrchestrator_SqliteConversations_tests
        tests/Unit/SqliteConversationsRepositoryTests.cpp
    )
    target_compile_features(AIOrchestrator_SqliteConversations_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_SqliteConversations_tests
        PRIVATE AIOrchestrator::Persistence ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_SqliteConversations_tests)
    add_test(NAME AIOrchestrator_SqliteConversations_tests COMMAND AIOrchestrator_SqliteConversations_tests)

    # The planning repositories reuse the shared repository contracts that also run inside
    # AIOrchestrator_Persistence_tests; this scoped executable runs only those suites.
    add_executable(AIOrchestrator_SqlitePlanning_tests
        tests/Unit/SqlitePlanningTestMain.cpp
        tests/Unit/RunDraftRepositoryTests.cpp
        tests/Unit/OutcomeBriefRepositoryTests.cpp
        tests/Unit/FollowUpPlanRequestRepositoryTests.cpp
        tests/Unit/SqlitePlanningRepositoryTests.cpp
    )
    target_compile_features(AIOrchestrator_SqlitePlanning_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_SqlitePlanning_tests
        PRIVATE AIOrchestrator::Persistence ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_SqlitePlanning_tests)
    add_test(NAME AIOrchestrator_SqlitePlanning_tests COMMAND AIOrchestrator_SqlitePlanning_tests)
endif()

# Backend-owned composition root: owns persistence, repositories, the Mock
# workflow runner, RunManager, and the Application Layer services built on
# top of them, entirely without Qt. Depends on Persistence only as an
# implementation detail (PRIVATE): BackendRuntime's public header exposes no
# SQLite types. Deliberately placed in this AI_ORCHESTRATOR_BUILD_PERSISTENCE
# block, not the AI_ORCHESTRATOR_BUILD_RUNTIME one below, so the desktop
# composition (which needs Application + Persistence + BackendRuntime) does
# not have to also build the CURL-dependent legacy provider runtime.
add_library(AIOrchestrator_BackendRuntime STATIC
    sourceFiles/backend/BackendRuntime.cpp
    sourceFiles/backend/StartupRecovery.cpp
)
add_library(AIOrchestrator::BackendRuntime ALIAS AIOrchestrator_BackendRuntime)

target_include_directories(AIOrchestrator_BackendRuntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/includes
)
target_compile_features(AIOrchestrator_BackendRuntime PUBLIC cxx_std_20)
target_link_libraries(AIOrchestrator_BackendRuntime
    PUBLIC
        AIOrchestrator::Application
    PRIVATE
        AIOrchestrator::Persistence
)
ai_orchestrator_configure_target(AIOrchestrator_BackendRuntime)

if(AI_ORCHESTRATOR_BUILD_PERSISTENCE_TESTS)
    # BackendRuntime tests link Persistence directly so recovery fixtures can
    # seed a crashed run without going through RunManager.
    add_executable(AIOrchestrator_BackendRuntime_tests
        tests/Unit/BackendRuntimeTestMain.cpp
        tests/Unit/BackendRuntimeTests.cpp
        tests/Unit/AgentToolExecutionServiceTests.cpp
    )
    target_compile_features(AIOrchestrator_BackendRuntime_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_BackendRuntime_tests
        PRIVATE
            AIOrchestrator::BackendRuntime
            AIOrchestrator::Persistence
            ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_BackendRuntime_tests)
    add_test(NAME AIOrchestrator_BackendRuntime_tests COMMAND AIOrchestrator_BackendRuntime_tests)

    # The folder of the temporary verification copies: its policy (length, links, overlap, privacy), its service and its SQLite
    # setting (schema step 69), on real folders and a real database. Provider-free.
    add_executable(VerificationScratch_tests
        tests/Unit/VerificationScratchTests.cpp
    )
    target_compile_features(VerificationScratch_tests PRIVATE cxx_std_20)
    target_link_libraries(VerificationScratch_tests
        PRIVATE
            AIOrchestrator::Application
            AIOrchestrator::Persistence
            ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(VerificationScratch_tests)
    add_test(NAME VerificationScratch_tests COMMAND VerificationScratch_tests)

    # AG-18.4B-R1: the MSVC / Windows SDK environment of the verification's child processes (a real configure + link from a
    # process without a Visual Studio environment). Provider-free.
    add_executable(VerificationToolchain_tests
        tests/Unit/VerificationToolchainTests.cpp
    )
    target_compile_features(VerificationToolchain_tests PRIVATE cxx_std_20)
    target_link_libraries(VerificationToolchain_tests
        PRIVATE
            AIOrchestrator::Application
    )
    ai_orchestrator_configure_target(VerificationToolchain_tests)
    add_test(NAME VerificationToolchain_tests COMMAND VerificationToolchain_tests)

    # AG-18.4B-R1: the honest project-readiness check (selection, MSVC-x64 rule, configure, targets) on a scripted cmake.
    add_executable(ProjectSetupReadiness_tests
        tests/Unit/ProjectSetupReadinessTests.cpp
    )
    target_compile_features(ProjectSetupReadiness_tests PRIVATE cxx_std_20)
    target_link_libraries(ProjectSetupReadiness_tests
        PRIVATE
            AIOrchestrator::Application
    )
    ai_orchestrator_configure_target(ProjectSetupReadiness_tests)
    add_test(NAME ProjectSetupReadiness_tests COMMAND ProjectSetupReadiness_tests)

    # AG-18.4B-R1: durable project readiness - fingerprint, freshness, reuse across Runs and restarts, invalidation, and the
    # SQLite record (schema step 70) on a real database. Provider-free.
    add_executable(ProjectReadiness_tests
        tests/Unit/ProjectReadinessTests.cpp
    )
    target_compile_features(ProjectReadiness_tests PRIVATE cxx_std_20)
    target_link_libraries(ProjectReadiness_tests
        PRIVATE
            AIOrchestrator::Application
            AIOrchestrator::Persistence
            ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(ProjectReadiness_tests)
    add_test(NAME ProjectReadiness_tests COMMAND ProjectReadiness_tests)

    # AG-17.7D-1: a multi-member Manifest roster Run through a restart and an exact Retry on a
    # real SQLite file, with the real runner, scripted provider sessions and the production
    # startup recovery sequence (ai::backend::runStartupRecovery). Provider-free.
    add_executable(AIOrchestrator_ManifestRosterRestart_tests
        tests/Unit/ManifestRosterRestartTests.cpp
    )
    target_compile_features(AIOrchestrator_ManifestRosterRestart_tests PRIVATE cxx_std_20)
    target_link_libraries(AIOrchestrator_ManifestRosterRestart_tests
        PRIVATE
            AIOrchestrator::BackendRuntime
            AIOrchestrator::Persistence
            ${AIOrchestrator_Sqlite3_Target}
    )
    ai_orchestrator_configure_target(AIOrchestrator_ManifestRosterRestart_tests)
    add_test(NAME AIOrchestrator_ManifestRosterRestart_tests COMMAND AIOrchestrator_ManifestRosterRestart_tests)
endif()
endif()

if(AI_ORCHESTRATOR_BUILD_RUNTIME)
# Windows resolves CURL through the vcpkg CONFIG package. Every other
# platform falls back to CMake's module-mode FindCURL (Homebrew/system
# curl), which has defined the same CURL::libcurl imported target since
# CMake 3.12, so no downstream target-name abstraction is needed here.
if(WIN32)
    find_package(CURL CONFIG REQUIRED)
else()
    find_package(CURL REQUIRED)
endif()

add_library(AIOrchestrator_Runtime STATIC
    ${RUNTIME_SOURCES}
)
add_library(AIOrchestrator::Runtime ALIAS AIOrchestrator_Runtime)

target_include_directories(AIOrchestrator_Runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/includes
)
target_compile_features(AIOrchestrator_Runtime PUBLIC cxx_std_20)
target_link_libraries(AIOrchestrator_Runtime
    PUBLIC
        AIOrchestrator::Core
        CURL::libcurl
)
ai_orchestrator_configure_target(AIOrchestrator_Runtime)

add_executable(${TARGET_NAME}
    sourceFiles/main.cpp
)
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
target_link_libraries(${TARGET_NAME} PRIVATE AIOrchestrator::Runtime)
ai_orchestrator_configure_target(${TARGET_NAME})

if(MINGW)
    target_link_options(${TARGET_NAME} PRIVATE -static)
endif()

# Lightweight assert-based unit tests link the reusable runtime instead of
# compiling every backend implementation a second time.
add_executable(${TARGET_NAME}_tests
    tests/Unit/AgentTests.cpp
    tests/Unit/OrchestratorTests.cpp
    tests/Unit/MockProviderTests.cpp
    tests/Unit/OpenAIProviderTests.cpp
    tests/Unit/HttpTimeoutTests.cpp
    tests/Unit/HttpRetryPolicyTests.cpp
    tests/Unit/ProjectContextBuilderTests.cpp
    tests/Unit/ApplicationPipelineTests.cpp
    tests/Unit/ArtifactServiceTests.cpp
    tests/Unit/WorkspaceAndCatalogTests.cpp
    tests/Unit/WorkflowRoutingAndProductionTests.cpp
    tests/Unit/ChatContextAssemblerTests.cpp
    tests/Unit/RunContextAssemblerTests.cpp
    tests/Unit/ComparisonServiceTests.cpp
    tests/Unit/ProjectMemoryServiceTests.cpp
    tests/Unit/ChangeProposalTests.cpp
    tests/Unit/RunPathClaimRegistryTests.cpp
    tests/Unit/AgentToolTurnLoopRunnerTests.cpp
    tests/Unit/ExecutionProfileWorkflowRunnerWorkspaceTests.cpp
    tests/Unit/AgentToolReadModelServiceTests.cpp
    tests/Unit/ChangeSetApplyServiceTests.cpp
    tests/Unit/ManifestApplyVerificationGateTests.cpp
    tests/Unit/ManifestWorkLedgerServiceTests.cpp
    tests/Unit/ManifestVerificationContinuationServiceTests.cpp
    tests/Unit/ManifestVerificationManualHandoffRepositoryTests.cpp
    tests/Unit/ManifestVerificationManualHandoffReadModelServiceTests.cpp
    tests/Unit/RunWorkspaceScopeServiceTests.cpp
    tests/Unit/VerificationEnvironmentDiscoveryServiceTests.cpp
    tests/Unit/VerificationEnvironmentPresetSetupServiceTests.cpp
    tests/Unit/ExecutionManifestReadModelServiceTests.cpp
    tests/Unit/ManifestMandatoryVerificationPolicyTests.cpp
    tests/Unit/ManifestRoutingTests.cpp
    tests/Unit/TaskRouteResolverTests.cpp
    tests/Unit/LegacyRosterAdapterTests.cpp
    tests/Unit/LegacyConversionRecordTests.cpp
    tests/Unit/LegacyConversionCommandTests.cpp
    tests/Unit/ManifestMemberUsageTests.cpp
    tests/Unit/ManifestInterruptedTaskReconcilerTests.cpp
    tests/Unit/ManifestVerificationProfileBinderTests.cpp
    tests/Unit/ManifestBootstrapValidatorTests.cpp
    tests/Unit/VerificationBootstrapRetryServiceTests.cpp
    tests/Unit/ProviderExecutionBudgetTests.cpp
    tests/Unit/CostCalculationTests.cpp
    tests/Unit/ExecutionAnalyticsServiceTests.cpp
)
target_compile_features(${TARGET_NAME}_tests PRIVATE cxx_std_20)
target_link_libraries(${TARGET_NAME}_tests
    PRIVATE
        AIOrchestrator::Runtime
        AIOrchestrator::Application
        AIOrchestrator::Persistence
)
ai_orchestrator_configure_target(${TARGET_NAME}_tests)

if(MINGW)
    target_link_options(${TARGET_NAME}_tests PRIVATE -static)
endif()

# Focused, network-free regression test for runtime Anthropic model routing.
# It links only the provider runtime so unrelated application-test failures do
# not hide a regression that would silently route every model to Haiku.
add_executable(AnthropicProviderRouting_tests
    tests/Unit/AnthropicProviderTestMain.cpp
    tests/Unit/AnthropicProviderTests.cpp
)
target_compile_features(AnthropicProviderRouting_tests PRIVATE cxx_std_20)
target_link_libraries(AnthropicProviderRouting_tests PRIVATE AIOrchestrator::Runtime)
ai_orchestrator_configure_target(AnthropicProviderRouting_tests)

if(MINGW)
    target_link_options(AnthropicProviderRouting_tests PRIVATE -static)
endif()

# Manual integration smoke test. It performs real OpenAI requests when
# OPENAI_API_KEY is present and therefore is intentionally not part of ctest.
add_executable(OpenAIProviderSmokeTest
    tests/Manual/OpenAIProviderSmokeTest.cpp
)
target_compile_features(OpenAIProviderSmokeTest PRIVATE cxx_std_20)
target_link_libraries(OpenAIProviderSmokeTest PRIVATE AIOrchestrator::Runtime)
ai_orchestrator_configure_target(OpenAIProviderSmokeTest)

if(MINGW)
    target_link_options(OpenAIProviderSmokeTest PRIVATE -static)
endif()

enable_testing()
add_test(NAME ${TARGET_NAME}_tests COMMAND ${TARGET_NAME}_tests)
add_test(NAME AnthropicProviderRouting_tests COMMAND AnthropicProviderRouting_tests)

endif()

# --- Windows Credential Manager ICredentialStore adapter ------------------
# Isolated, Windows-only target. Depends only on AIOrchestrator::Application
# (for ICredentialStore) plus the Win32 Advapi32 import library (wincred.h) -
# never Qt, never CURL. Independent of AI_ORCHESTRATOR_BUILD_RUNTIME: the
# standalone UI dependency scenario (AI_ORCHESTRATOR_BUILD_RUNTIME=OFF,
# AI_ORCHESTRATOR_BUILD_PERSISTENCE=ON) still gets a real secret store.
option(
    AI_ORCHESTRATOR_BUILD_WINDOWS_CREDENTIALS
    "Build the Windows Credential Manager-backed ICredentialStore adapter (Windows only)"
    ${WIN32}
)

if(AI_ORCHESTRATOR_BUILD_WINDOWS_CREDENTIALS AND TARGET AIOrchestrator_BackendRuntime)
    if(NOT WIN32)
        message(FATAL_ERROR
            "AI_ORCHESTRATOR_BUILD_WINDOWS_CREDENTIALS requires a Windows target; "
            "set it OFF on this platform."
        )
    endif()

    add_library(AIOrchestrator_WindowsCredentials STATIC
        sourceFiles/infrastructure/WindowsCredentialStore.cpp
    )
    add_library(AIOrchestrator::WindowsCredentials ALIAS AIOrchestrator_WindowsCredentials)

    target_include_directories(AIOrchestrator_WindowsCredentials
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/includes
    )
    target_compile_features(AIOrchestrator_WindowsCredentials PUBLIC cxx_std_20)
    target_link_libraries(AIOrchestrator_WindowsCredentials
        PUBLIC
            AIOrchestrator::Application
        PRIVATE
            Advapi32
    )
    ai_orchestrator_configure_target(AIOrchestrator_WindowsCredentials)

    # BackendRuntime.cpp selects this adapter only when it sees this compile
    # definition; PRIVATE so it is a BackendRuntime implementation detail,
    # never part of its public link interface.
    target_link_libraries(AIOrchestrator_BackendRuntime PRIVATE AIOrchestrator::WindowsCredentials)
    target_compile_definitions(AIOrchestrator_BackendRuntime PRIVATE AI_ORCHESTRATOR_HAVE_WINDOWS_CREDENTIALS=1)
endif()

# --- macOS Keychain ICredentialStore adapter ------------------------------
option(
    AI_ORCHESTRATOR_BUILD_MACOS_CREDENTIALS
    "Build the macOS Keychain-backed ICredentialStore adapter (macOS only)"
    ${APPLE}
)

if(AI_ORCHESTRATOR_BUILD_MACOS_CREDENTIALS AND TARGET AIOrchestrator_BackendRuntime)
    if(NOT APPLE)
        message(FATAL_ERROR
            "AI_ORCHESTRATOR_BUILD_MACOS_CREDENTIALS requires an Apple target; "
            "set it OFF on this platform."
        )
    endif()

    find_library(AI_ORCHESTRATOR_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(AI_ORCHESTRATOR_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

    add_library(AIOrchestrator_MacOSCredentials STATIC
        sourceFiles/infrastructure/MacOSCredentialStore.cpp
    )
    add_library(AIOrchestrator::MacOSCredentials ALIAS AIOrchestrator_MacOSCredentials)

    target_include_directories(AIOrchestrator_MacOSCredentials
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/includes
    )
    target_compile_features(AIOrchestrator_MacOSCredentials PUBLIC cxx_std_20)
    target_link_libraries(AIOrchestrator_MacOSCredentials
        PUBLIC
            AIOrchestrator::Application
        PRIVATE
            ${AI_ORCHESTRATOR_SECURITY_FRAMEWORK}
            ${AI_ORCHESTRATOR_COREFOUNDATION_FRAMEWORK}
    )
    ai_orchestrator_configure_target(AIOrchestrator_MacOSCredentials)

    target_link_libraries(AIOrchestrator_BackendRuntime PRIVATE AIOrchestrator::MacOSCredentials)
    target_compile_definitions(AIOrchestrator_BackendRuntime PRIVATE AI_ORCHESTRATOR_HAVE_MACOS_CREDENTIALS=1)

    if(AI_ORCHESTRATOR_BUILD_PERSISTENCE_TESTS)
        add_executable(MacOSCredentialStore_tests
            tests/Unit/MacOSCredentialStoreTests.cpp
        )
        target_compile_features(MacOSCredentialStore_tests PRIVATE cxx_std_20)
        target_link_libraries(MacOSCredentialStore_tests PRIVATE AIOrchestrator::MacOSCredentials)
        ai_orchestrator_configure_target(MacOSCredentialStore_tests)
        add_test(NAME MacOSCredentialStore_tests COMMAND MacOSCredentialStore_tests)
    endif()
endif()

# --- Real OpenAI/Anthropic IProviderClient adapter -------------------------
# Off by default: linking it pulls in AIOrchestrator::Runtime (and therefore
# CURL) as a BackendRuntime implementation detail. Requires both
# AI_ORCHESTRATOR_BUILD_PERSISTENCE (for AIOrchestrator_BackendRuntime) and
# AI_ORCHESTRATOR_BUILD_RUNTIME (for AIOrchestrator_Runtime/OpenAIProvider/
# AnthropicProvider) to already be ON; the standalone UI dependency scenario
# (RUNTIME=OFF) is unaffected by this option existing.
option(
    AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS
    "Build the OpenAI/Anthropic-backed IProviderClientFactory adapter (requires AI_ORCHESTRATOR_BUILD_RUNTIME and AI_ORCHESTRATOR_BUILD_PERSISTENCE)"
    OFF
)

if(AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS)
    if(NOT TARGET AIOrchestrator_BackendRuntime)
        message(FATAL_ERROR
            "AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS requires AI_ORCHESTRATOR_BUILD_PERSISTENCE=ON."
        )
    endif()
    if(NOT TARGET AIOrchestrator_Runtime)
        message(FATAL_ERROR
            "AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS requires AI_ORCHESTRATOR_BUILD_RUNTIME=ON."
        )
    endif()

    target_sources(AIOrchestrator_BackendRuntime PRIVATE sourceFiles/backend/RealProviderClientFactory.cpp)
    # PRIVATE: CURL must never become part of BackendRuntime's public link
    # interface or leak into a caller that only wants Application+Persistence.
    target_link_libraries(AIOrchestrator_BackendRuntime PRIVATE AIOrchestrator::Runtime)
    target_compile_definitions(AIOrchestrator_BackendRuntime PRIVATE AI_ORCHESTRATOR_HAVE_REAL_PROVIDERS=1)

    # Provider-free proof that the private production adapters call the
    # typed policy gate before reaching either network implementation.
    add_executable(RealProviderClientFactory_tests
        tests/Unit/RealProviderClientFactoryTests.cpp
    )
    target_compile_features(RealProviderClientFactory_tests PRIVATE cxx_std_20)
    target_link_libraries(RealProviderClientFactory_tests
        PRIVATE AIOrchestrator::BackendRuntime AIOrchestrator::Application)
    ai_orchestrator_configure_target(RealProviderClientFactory_tests)
    add_test(NAME RealProviderClientFactory_tests COMMAND RealProviderClientFactory_tests)

endif()

# Pure contract of the real-provider translation seam. It has no CURL or
# credential dependency and therefore remains testable when the production
# adapter itself is disabled in this build configuration.
add_executable(ProviderClientTranslation_tests
    tests/Unit/ProviderClientTranslationTests.cpp
)
target_compile_features(ProviderClientTranslation_tests PRIVATE cxx_std_20)
target_link_libraries(ProviderClientTranslation_tests PRIVATE AIOrchestrator::Application)
ai_orchestrator_configure_target(ProviderClientTranslation_tests)
add_test(NAME ProviderClientTranslation_tests COMMAND ProviderClientTranslation_tests)

# --- Real OpenAI/Anthropic IProviderAgentSession adapters ------------------
# Off by default: linking it pulls in AIOrchestrator::Runtime (and therefore
# CURL) as a BackendRuntime implementation detail. Requires both
# AI_ORCHESTRATOR_BUILD_PERSISTENCE (for AIOrchestrator_BackendRuntime) and
# AI_ORCHESTRATOR_BUILD_RUNTIME (for AIOrchestrator_Runtime/HttpClient/
# nlohmann::json) to already be ON.
option(
    AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS
    "Build the OpenAI/Anthropic-backed IProviderAgentSessionFactory adapter (requires AI_ORCHESTRATOR_BUILD_RUNTIME and AI_ORCHESTRATOR_BUILD_PERSISTENCE)"
    OFF
)

if(AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS)
    if(NOT TARGET AIOrchestrator_BackendRuntime)
        message(FATAL_ERROR
            "AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS requires AI_ORCHESTRATOR_BUILD_PERSISTENCE=ON."
        )
    endif()
    if(NOT TARGET AIOrchestrator_Runtime)
        message(FATAL_ERROR
            "AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS requires AI_ORCHESTRATOR_BUILD_RUNTIME=ON."
        )
    endif()

    target_sources(AIOrchestrator_BackendRuntime PRIVATE
        sourceFiles/providers/OpenAIAgentSession.cpp
        sourceFiles/providers/AnthropicAgentSession.cpp
        sourceFiles/providers/RealProviderAgentSessionFactory.cpp
    )
    # PRIVATE: CURL must never become part of BackendRuntime's public link interface.
    target_link_libraries(AIOrchestrator_BackendRuntime PRIVATE AIOrchestrator::Runtime)
    target_compile_definitions(AIOrchestrator_BackendRuntime PRIVATE AI_ORCHESTRATOR_HAVE_REAL_AGENT_SESSIONS=1)
endif()

if(AI_ORCHESTRATOR_BUILD_RUNTIME)
# Network-free fixture tests for the OpenAI and Anthropic agent session adapters.
# Compiles the session .cpp files directly so no BackendRuntime linkage is needed.
# Added inside the AI_ORCHESTRATOR_BUILD_RUNTIME guard because the session files
# depend on nlohmann::json which arrives transitively through AIOrchestrator::Runtime.
add_executable(AIOrchestrator_AgentSession_tests
    sourceFiles/providers/OpenAIAgentSession.cpp
    sourceFiles/providers/AnthropicAgentSession.cpp
    tests/Unit/AgentSessionTestMain.cpp
    tests/Unit/OpenAIAgentSessionTests.cpp
    tests/Unit/AnthropicAgentSessionTests.cpp
    tests/Unit/StrictSchemaContractTests.cpp
)
target_compile_features(AIOrchestrator_AgentSession_tests PRIVATE cxx_std_20)
target_link_libraries(AIOrchestrator_AgentSession_tests
    PRIVATE
        AIOrchestrator::Runtime
        AIOrchestrator::Application
)
ai_orchestrator_configure_target(AIOrchestrator_AgentSession_tests)
add_test(NAME AIOrchestrator_AgentSession_tests COMMAND AIOrchestrator_AgentSession_tests)

# Manual smoke test: real network calls. Not part of CTest.
# Only builds when AI_ORCHESTRATOR_BUILD_RUNTIME is ON.
add_executable(AgentSessionSmokeTest
    sourceFiles/providers/OpenAIAgentSession.cpp
    sourceFiles/providers/AnthropicAgentSession.cpp
    tests/Manual/AgentSessionSmokeTest.cpp
)
target_compile_features(AgentSessionSmokeTest PRIVATE cxx_std_20)
target_link_libraries(AgentSessionSmokeTest
    PRIVATE
        AIOrchestrator::Runtime
        AIOrchestrator::Application
)
ai_orchestrator_configure_target(AgentSessionSmokeTest)

if(MINGW)
    target_link_options(AIOrchestrator_AgentSession_tests PRIVATE -static)
    target_link_options(AgentSessionSmokeTest PRIVATE -static)
endif()
endif() # AI_ORCHESTRATOR_BUILD_RUNTIME (agent session tests block)
