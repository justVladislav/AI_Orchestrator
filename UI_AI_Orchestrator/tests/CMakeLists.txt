# Headless QML front-end smoke test: loads the real Main.qml with a real
# BackendBridge and drives ProjectsPage/RunsPage exactly like the UI does.
qt_add_executable(AIOrchestrator_QmlFrontend_tests
    QmlFrontendSmokeTest.cpp
)

# Own output directory, isolated from appUI_AI_Orchestrator's deployment root,
# for the same reason AIOrchestrator_QtAdapter_tests keeps its own directory:
# a partial/foreign Qt deployment beside another app can break plugin loading.
set_target_properties(AIOrchestrator_QmlFrontend_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml-frontend-tests"
)

target_link_libraries(AIOrchestrator_QmlFrontend_tests
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        UI_AI_Orchestrator_Content
        UI_AI_Orchestrator_Contentplugin
        AIOrchestrator_Backend
)

if(WIN32)
    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    find_program(AI_ORCHESTRATOR_WINDEPLOYQT
        NAMES windeployqt
        HINTS "${_qt_bin_dir}"
        REQUIRED
    )

    add_custom_command(TARGET AIOrchestrator_QmlFrontend_tests POST_BUILD
        COMMAND "${AI_ORCHESTRATOR_WINDEPLOYQT}"
            --$<IF:$<CONFIG:Debug>,debug,release>
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/../qml"
            --no-compiler-runtime
            --no-translations
            "$<TARGET_FILE:AIOrchestrator_QmlFrontend_tests>"
        COMMENT "Deploying Qt runtime and QML imports for the frontend smoke test"
        VERBATIM
    )
endif()

add_test(
    NAME AIOrchestrator_QmlFrontend_tests
    COMMAND AIOrchestrator_QmlFrontend_tests
)

# PC-5D: real-QML coverage of the follow-up planner (the panel, its quote dialog and the plan review dialog) over a real BackendBridge
# and a disposable SQLite database. It never authorizes a paid call; the Backend fixtures of the follow-up tests (request builders) are
# used from the core test directory.
qt_add_executable(AIOrchestrator_FollowUpQml_tests
    FollowUpQmlTest.cpp
)

set_target_properties(AIOrchestrator_FollowUpQml_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/follow-up-qml-tests"
)

target_include_directories(AIOrchestrator_FollowUpQml_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../First_AI_Agents/First_AI_Agents/tests/Unit"
)
target_compile_features(AIOrchestrator_FollowUpQml_tests PRIVATE cxx_std_20)

target_link_libraries(AIOrchestrator_FollowUpQml_tests
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        UI_AI_Orchestrator_Content
        UI_AI_Orchestrator_Contentplugin
        AIOrchestrator_Backend
        # Test-only: the Runs and the stored plan are seeded through a second connection (no provider can produce them here).
        AIOrchestrator::Persistence
)

if(WIN32)
    add_custom_command(TARGET AIOrchestrator_FollowUpQml_tests POST_BUILD
        COMMAND "${AI_ORCHESTRATOR_WINDEPLOYQT}"
            --$<IF:$<CONFIG:Debug>,debug,release>
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/../qml"
            --no-compiler-runtime
            --no-translations
            "$<TARGET_FILE:AIOrchestrator_FollowUpQml_tests>"
        COMMENT "Deploying Qt runtime and QML imports for the follow-up QML test"
        VERBATIM
    )
endif()

add_test(
    NAME AIOrchestrator_FollowUpQml_tests
    COMMAND AIOrchestrator_FollowUpQml_tests
)
