cmake_minimum_required(VERSION 3.20)

# The one product version for every project of this repository (see the file for what it defines).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/AIOrchestratorVersion.cmake")
# The tests check with assert(): keep it alive in Release test executables (and prove it with a canary test).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/AIOrchestratorTestAsserts.cmake")

project(AIOrchestratorWorkspace VERSION ${AIO_VERSION_CORE} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# The unified workspace graph builds the real desktop app, so it needs the
# same real OpenAI/Anthropic adapters UI_AI_Orchestrator/CMakeLists.txt
# forces ON for a standalone UI configure (see its `if(NOT TARGET
# AIOrchestrator_Application)` block). That guard is false here, since
# First_AI_Agents below already defines the target by the time it's checked
# - so without this, First_AI_Agents's own OFF-by-default option() locks in
# before UI_AI_Orchestrator/CMakeLists.txt is even reached, and the desktop
# app silently falls back to provider_runtime.unavailable/
# agent_session.unavailable for every real OpenAI/Anthropic call. Not
# platform-specific: affects any unified-workspace build, Windows included.
set(AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS ON CACHE BOOL "" FORCE)
set(AI_ORCHESTRATOR_BUILD_REAL_AGENT_SESSIONS ON CACHE BOOL "" FORCE)

# Keep both existing projects independently buildable while also providing one
# authoritative workspace graph for backend/UI integration.
add_subdirectory(First_AI_Agents)
add_subdirectory(UI_AI_Orchestrator)

# AIOrchestrator_Backend links the Application target in its own CMake file so
# the same boundary is preserved in both unified and standalone UI builds.
