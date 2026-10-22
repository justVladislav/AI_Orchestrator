# CMakeList.txt: файл проекта CMake верхнего уровня; проведите глобальную конфигурацию
# и включите сюда подпроекты.
#

cmake_minimum_required(VERSION 3.20)

# The one product version for every project of this repository, also when this project is configured standalone.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AIOrchestratorVersion.cmake")
# The tests check with assert(): keep it alive in Release test executables (and prove it with a canary test).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AIOrchestratorTestAsserts.cmake")

project(AiOrchestrator VERSION ${AIO_VERSION_CORE} LANGUAGES CXX)

enable_testing()

# Включение горячей перезагрузки для компиляторов MSVC, если поддерживается.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# Включите подпроекты.
add_subdirectory ("First_AI_Agents")
