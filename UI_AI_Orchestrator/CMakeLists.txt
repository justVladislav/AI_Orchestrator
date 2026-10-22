cmake_minimum_required(VERSION 3.16)

# Qt Creator kits configure without CMakePresets.json, losing the vcpkg
# toolchain and manifest that the sibling Backend needs (unofficial-sqlite3,
# curl). Default them here — before project() — whenever the caller did not
# provide a toolchain; preset/CLI configures that already pass one win.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE
        AND EXISTS "C:/c++_libs_tools/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE
        "C:/c++_libs_tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain (defaulted for IDE kit configures)")
endif()
if(NOT DEFINED VCPKG_MANIFEST_DIR)
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../First_AI_Agents"
        CACHE PATH "Backend vcpkg manifest")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "")
endif()

# The one product version for every project of this repository, also when this project is configured standalone.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AIOrchestratorVersion.cmake")
# The tests check with assert(): keep it alive in Release test executables (and prove it with a canary test).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AIOrchestratorTestAsserts.cmake")

project(UI_AI_Orchestrator VERSION ${AIO_VERSION_CORE} LANGUAGES CXX)

# Register adapter tests when the UI is configured outside the unified workspace.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# UI depends only on Qt presentation modules. Native SQLite belongs to the
# sibling Backend and reaches the final executable through BackendRuntime's
# transitive CMake dependency graph.
find_package(Qt6 REQUIRED COMPONENTS Concurrent Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.10)

# A standalone UI configure imports the provider-neutral Core/Application
# targets plus BackendRuntime from the sibling backend, including the
# CURL-backed OpenAI/Anthropic provider runtime. The unified workspace already defines
# these targets before entering this project, so it does not take this branch.
# BackendRuntime owns persistence, so persistence must stay enabled even
# and real provider adapters must stay enabled for Chats and production Runs.
if(NOT TARGET AIOrchestrator_Application)
    set(AI_ORCHESTRATOR_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
    set(AI_ORCHESTRATOR_BUILD_PERSISTENCE ON CACHE BOOL "" FORCE)
    set(AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS ON CACHE BOOL "" FORCE)
    # Same reasoning as AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS above, for the
    # agent-tools read-only-analysis workflow's OpenAI/Anthropic session
    # adapters (RealProviderAgentSessionFactory). Without this, BackendRuntime
    # falls back to UnavailableProviderAgentSessionFactory and every
    # read-only-analysis run fails with "agent_session.unavailable" even
    # when BackendRuntimeConfig::enableAgentToolExecution is true.
    set(AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS ON CACHE BOOL "" FORCE)
    # The desktop build consumes production BackendRuntime/Persistence only.
    # Backend-only repository/runtime tests stay in the standalone Backend
    # build; this Qt graph keeps its own adapter and real-QML integration tests.
    set(AI_ORCHESTRATOR_BUILD_PERSISTENCE_TESTS OFF CACHE BOOL "" FORCE)
    # Short binary-dir name on purpose: the deepest backend object path plus
    # Qt Creator's default build-folder name used to land exactly on the
    # Windows MAX_PATH (260) limit, failing MSVC with an empty-filename C1083.
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../First_AI_Agents"
        "${CMAKE_BINARY_DIR}/fa"
    )
endif()

# Content downloaded from Figma to Qt Design Studio, if present.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/importedcontent/CMakeLists.txt")
    add_subdirectory(importedcontent)
endif()

# Qt adapter over the provider-neutral Application Layer.
add_subdirectory(backend)

qt_add_executable(appUI_AI_Orchestrator
    main.cpp
)

# The application header with the version strings (main.cpp sets the application version from it).
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AppVersion.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/AppVersion.hpp"
    @ONLY
)
target_include_directories(appUI_AI_Orchestrator PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Windows resource of the executable file: the Explorer/taskbar icon and the version information
# (FILEVERSION/PRODUCTVERSION numeric, FileVersion/ProductVersion with the pre-release suffix, VS_FF_PRERELEASE).
if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc"
        @ONLY
    )
    target_sources(appUI_AI_Orchestrator PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")
endif()

# QML file lists stay isolated in the second CMake file.
add_subdirectory(qml)

set_target_properties(appUI_AI_Orchestrator PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.appUI_AI_Orchestrator
    MACOSX_BUNDLE_BUNDLE_VERSION ${AIO_VERSION_CORE}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${AIO_VERSION_FULL}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appUI_AI_Orchestrator
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        UI_AI_Orchestrator_Content
        UI_AI_Orchestrator_Contentplugin
        AIOrchestrator_Backend
)

# Deploy the complete matching Qt/QML runtime next to the Windows executable so
# it can be launched directly from the build tree, not only through Qt Creator.
if(WIN32)
    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    find_program(AI_ORCHESTRATOR_WINDEPLOYQT
        NAMES windeployqt
        HINTS "${_qt_bin_dir}"
        REQUIRED
    )

    add_custom_command(TARGET appUI_AI_Orchestrator POST_BUILD
        COMMAND "${AI_ORCHESTRATOR_WINDEPLOYQT}"
            --$<IF:$<CONFIG:Debug>,debug,release>
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
            --no-compiler-runtime
            --no-translations
            "$<TARGET_FILE:appUI_AI_Orchestrator>"
        COMMENT "Deploying Qt runtime and QML imports for AI Orchestrator"
        VERBATIM
    )
endif()
install(TARGETS appUI_AI_Orchestrator
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Headless QML front-end integration smoke test (ProjectsPage/RunsPage <-> the
# Qt Adapter). Depends on the same QML module target defined above.
add_subdirectory(tests)
