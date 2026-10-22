qt_add_library(AIOrchestrator_Backend STATIC
    src/BackendBridge.cpp
    src/ProjectController.cpp
    src/ProjectVerificationSetupController.cpp
    src/ProjectListModel.cpp
    src/TaskController.cpp
    src/TaskListModel.cpp
    src/RunController.cpp
    src/RunListModel.cpp
    src/RunStageModel.cpp
    src/ArtifactListModel.cpp
    src/RunWarningListModel.cpp
    src/WorkspaceContextController.cpp
    src/ProviderCatalogController.cpp
    src/ProviderListModel.cpp
    src/ModelListModel.cpp
    src/ExecutionProfileController.cpp
    src/ExecutionProfileListModel.cpp
    src/RunDraftController.cpp
    src/RunDraftListModel.cpp
    src/CredentialController.cpp
    src/ChatController.cpp
    src/ExecutionAnalyticsController.cpp
    src/ProviderExecutionAnalyticsModel.cpp
    src/ComparisonController.cpp
    src/ComparisonSessionListModel.cpp
    src/ComparisonResultListModel.cpp
    src/AgentSessionController.cpp
    src/AgentTurnListModel.cpp
    src/AgentToolCallListModel.cpp
    src/AgentProposalListModel.cpp
    src/ManifestController.cpp
    src/ManifestVerificationManualHandoffController.cpp
    src/OutcomeBriefController.cpp
    src/SettingsController.cpp
    src/ProjectMemoryController.cpp
    src/ProjectContextController.cpp
    src/PromptWorkshopController.cpp
    src/FollowUpPlanController.cpp
    include/ai/ui/BackendBridge.hpp
    include/ai/ui/ProjectController.hpp
    include/ai/ui/ProjectVerificationSetupController.hpp
    include/ai/ui/ProjectListModel.hpp
    include/ai/ui/TaskController.hpp
    include/ai/ui/TaskListModel.hpp
    include/ai/ui/RunController.hpp
    include/ai/ui/RunListModel.hpp
    include/ai/ui/RunStageModel.hpp
    include/ai/ui/ArtifactListModel.hpp
    include/ai/ui/RunWarningListModel.hpp
    include/ai/ui/WorkspaceContextController.hpp
    include/ai/ui/ProviderCatalogController.hpp
    include/ai/ui/ProviderListModel.hpp
    include/ai/ui/ModelListModel.hpp
    include/ai/ui/ExecutionProfileController.hpp
    include/ai/ui/ExecutionProfileListModel.hpp
    include/ai/ui/RunDraftController.hpp
    include/ai/ui/RunDraftListModel.hpp
    include/ai/ui/CredentialController.hpp
    include/ai/ui/ChatController.hpp
    include/ai/ui/ExecutionAnalyticsController.hpp
    include/ai/ui/ProviderExecutionAnalyticsModel.hpp
    include/ai/ui/ComparisonController.hpp
    include/ai/ui/ComparisonSessionListModel.hpp
    include/ai/ui/ComparisonResultListModel.hpp
    include/ai/ui/AgentSessionController.hpp
    include/ai/ui/AgentTurnListModel.hpp
    include/ai/ui/AgentToolCallListModel.hpp
    include/ai/ui/AgentProposalListModel.hpp
    include/ai/ui/ManifestController.hpp
    include/ai/ui/OutcomeBriefController.hpp
    include/ai/ui/ManifestVerificationManualHandoffController.hpp
    include/ai/ui/SettingsController.hpp
    include/ai/ui/ProjectMemoryController.hpp
    include/ai/ui/ProjectContextController.hpp
    include/ai/ui/PromptWorkshopController.hpp
    include/ai/ui/FollowUpPlanController.hpp
    include/ai/ui/NextRunPlanVariant.hpp
)

target_include_directories(AIOrchestrator_Backend
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(AIOrchestrator_Backend
    PUBLIC
        Qt6::Core
        AIOrchestrator::BackendRuntime
    PRIVATE
        # QDesktopServices opens only Backend-validated artifact targets; QML
        # never receives or constructs a filesystem path.
        Qt6::Concurrent
        Qt6::Gui
)

# Headless integration test for the Qt/controller boundary. It uses QCoreApplication
# only and therefore does not require a display server or QML scene.
add_executable(AIOrchestrator_QtAdapter_tests
    tests/QtAdapterTests.cpp
)

# Keep the Core-only test runtime away from the GUI executable. A partial Qt
# deployment beside the app changes Qt's plugin root and breaks qwindows loading.
set_target_properties(AIOrchestrator_QtAdapter_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/adapter-tests"
)
target_link_libraries(AIOrchestrator_QtAdapter_tests
    PRIVATE
        AIOrchestrator_Backend
        Qt6::Core
        # Test-only dependency: one recovery fixture deliberately writes a
        # raw Running row to simulate a crashed prior process. Production UI
        # code depends only on BackendRuntime.
        AIOrchestrator::Persistence
)
# AG-18.4A hardening: lets a test statically prove a specific string (e.g.
# "Create setup Run") never appears in the real deployed QML source, rather
# than only asserting the renamed Qt property/action names.
target_compile_definitions(AIOrchestrator_QtAdapter_tests PRIVATE
    AI_ORCHESTRATOR_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../qml"
)

# Make the test directly runnable outside Qt Creator on Windows.
add_custom_command(TARGET AIOrchestrator_QtAdapter_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE_DIR:AIOrchestrator_QtAdapter_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Gui>"
        "$<TARGET_FILE_DIR:AIOrchestrator_QtAdapter_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Concurrent>"
        "$<TARGET_FILE_DIR:AIOrchestrator_QtAdapter_tests>"
)

add_test(
    NAME AIOrchestrator_QtAdapter_tests
    COMMAND AIOrchestrator_QtAdapter_tests
)

# PC-1D: the asynchronous Project Context preview controller, on a real BackendBridge / BackendRuntime over a
# disposable SQLite database. Own executable (require()-based) so it stays small and fast.
add_executable(AIOrchestrator_ProjectContextController_tests
    tests/ProjectContextControllerTests.cpp
)
set_target_properties(AIOrchestrator_ProjectContextController_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/project-context-tests"
)
target_link_libraries(AIOrchestrator_ProjectContextController_tests
    PRIVATE
        AIOrchestrator_Backend
        Qt6::Core
)
target_compile_definitions(AIOrchestrator_ProjectContextController_tests PRIVATE
    AI_ORCHESTRATOR_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../qml"
)
add_custom_command(TARGET AIOrchestrator_ProjectContextController_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE_DIR:AIOrchestrator_ProjectContextController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Gui>"
        "$<TARGET_FILE_DIR:AIOrchestrator_ProjectContextController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Concurrent>"
        "$<TARGET_FILE_DIR:AIOrchestrator_ProjectContextController_tests>"
)
add_test(
    NAME AIOrchestrator_ProjectContextController_tests
    COMMAND AIOrchestrator_ProjectContextController_tests
)

# PC-3C: the Prompt Workshop controller and the per-message plan state, on a real BackendBridge / BackendRuntime over
# a disposable SQLite database. Own executable (require()-based) so it stays small and fast.
add_executable(AIOrchestrator_PromptWorkshopController_tests
    tests/PromptWorkshopControllerTests.cpp
)
set_target_properties(AIOrchestrator_PromptWorkshopController_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/prompt-workshop-tests"
)
target_link_libraries(AIOrchestrator_PromptWorkshopController_tests
    PRIVATE
        AIOrchestrator_Backend
        Qt6::Core
        # Test-only: chat messages are seeded through a second connection (no provider can write a model reply).
        AIOrchestrator::Persistence
)
add_custom_command(TARGET AIOrchestrator_PromptWorkshopController_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE_DIR:AIOrchestrator_PromptWorkshopController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Gui>"
        "$<TARGET_FILE_DIR:AIOrchestrator_PromptWorkshopController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Concurrent>"
        "$<TARGET_FILE_DIR:AIOrchestrator_PromptWorkshopController_tests>"
)
add_test(
    NAME AIOrchestrator_PromptWorkshopController_tests
    COMMAND AIOrchestrator_PromptWorkshopController_tests
)

# PC-4D: the Qt boundary of the read-only Outcome block (OutcomeBriefController): the shape of its map for every availability, when
# briefChanged() is announced, the selection and the trailing re-read, and the bridge wiring on a real BackendBridge / BackendRuntime over
# a disposable SQLite database. Own executable (require()-based) so it stays small and fast.
add_executable(AIOrchestrator_OutcomeBriefController_tests
    tests/OutcomeBriefControllerTests.cpp
)
set_target_properties(AIOrchestrator_OutcomeBriefController_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/outcome-brief-tests"
)
target_link_libraries(AIOrchestrator_OutcomeBriefController_tests
    PRIVATE
        AIOrchestrator_Backend
        Qt6::Core
        # Test-only: the finished Run is seeded through a second connection (no provider can produce one).
        AIOrchestrator::Persistence
)
add_custom_command(TARGET AIOrchestrator_OutcomeBriefController_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE_DIR:AIOrchestrator_OutcomeBriefController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Gui>"
        "$<TARGET_FILE_DIR:AIOrchestrator_OutcomeBriefController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Concurrent>"
        "$<TARGET_FILE_DIR:AIOrchestrator_OutcomeBriefController_tests>"
)
add_test(
    NAME AIOrchestrator_OutcomeBriefController_tests
    COMMAND AIOrchestrator_OutcomeBriefController_tests
)

# PC-5D: the Qt boundary of the opt-in paid next-Run composer (FollowUpPlanController): every map it returns, the asynchronous contract
# (one at a time, results through operationFinished, the polled status line), that it can neither start a Run nor call a provider, the
# guest refusal, and the wiring on a real BackendBridge. It is driven over the Backend fixtures of the follow-up service tests (a scripted
# source reader and provider over the real in-memory repositories), so those headers are used from the core test directory.
add_executable(AIOrchestrator_FollowUpPlanController_tests
    tests/FollowUpPlanControllerTests.cpp
)
set_target_properties(AIOrchestrator_FollowUpPlanController_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/follow-up-plan-tests"
)
target_include_directories(AIOrchestrator_FollowUpPlanController_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../First_AI_Agents/First_AI_Agents/tests/Unit"
)
target_compile_features(AIOrchestrator_FollowUpPlanController_tests PRIVATE cxx_std_20)
target_link_libraries(AIOrchestrator_FollowUpPlanController_tests
    PRIVATE
        AIOrchestrator_Backend
        Qt6::Core
        # Test-only: the real-bridge scenario seeds a finished Run through a second connection (no provider can produce one).
        AIOrchestrator::Persistence
)
add_custom_command(TARGET AIOrchestrator_FollowUpPlanController_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>"
        "$<TARGET_FILE_DIR:AIOrchestrator_FollowUpPlanController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Gui>"
        "$<TARGET_FILE_DIR:AIOrchestrator_FollowUpPlanController_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Concurrent>"
        "$<TARGET_FILE_DIR:AIOrchestrator_FollowUpPlanController_tests>"
)
add_test(
    NAME AIOrchestrator_FollowUpPlanController_tests
    COMMAND AIOrchestrator_FollowUpPlanController_tests
)
