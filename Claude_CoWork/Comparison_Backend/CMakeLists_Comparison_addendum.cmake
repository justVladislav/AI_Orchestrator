# =============================================================================
# CMakeLists_Comparison_addendum.cmake
#
# This file documents the CMake additions required to build and test the
# Comparison backend integration. Apply these additions to the existing
# CMakeLists.txt files in the main project.
# =============================================================================

# =============================================================================
# SECTION A: First_AI_Agents/First_AI_Agents/CMakeLists.txt additions
# =============================================================================
#
# A1. Add new application-layer sources to the AIOrchestrator_Application target:
#
#     target_sources(AIOrchestrator_Application PRIVATE
#         sourceFiles/application/ComparisonTypes.cpp
#         sourceFiles/application/InMemoryComparisonRepository.cpp
#         sourceFiles/application/ComparisonService.cpp
#     )
#
# A2. Add new infrastructure source to the AIOrchestrator_Persistence target:
#
#     target_sources(AIOrchestrator_Persistence PRIVATE
#         sourceFiles/infrastructure/SqliteComparisonRepository.cpp
#     )
#
#     # SqliteDatabase_Impl.hpp is an internal header included by both
#     # SqliteRepositories.cpp and SqliteComparisonRepository.cpp.
#     # No extra target_include_directories needed — it lives under includes/
#     # which is already on the include path.
#
# A3. Merge instruction for SqliteRepositories.cpp:
#     Replace the inline struct SqliteDatabase::Impl definition block with:
#
#         #include "ai/infrastructure/SqliteDatabase_Impl.hpp"
#
#     and add the v5 migration call inside SqliteDatabase::open():
#
#         // v5: Comparison tables
#         {
#             const std::string ddl = infrastructure::comparisonSchemaDDL();
#             // execute ddl string the same way v1–v4 DDL is applied
#         }
#
# A4. Add new test executable for ComparisonService (application unit tests):
#
#     add_executable(AIOrchestrator_ComparisonServiceTests
#         tests/Unit/ComparisonServiceTests.cpp
#     )
#     target_link_libraries(AIOrchestrator_ComparisonServiceTests PRIVATE
#         AIOrchestrator_Application
#     )
#     add_test(NAME ComparisonServiceTests
#         COMMAND AIOrchestrator_ComparisonServiceTests)
#
# A5. Add new test executable for SqliteComparisonRepository (repository contract):
#
#     add_executable(AIOrchestrator_SqliteComparisonRepositoryTests
#         tests/Unit/SqliteComparisonRepositoryTests.cpp
#     )
#     target_link_libraries(AIOrchestrator_SqliteComparisonRepositoryTests PRIVATE
#         AIOrchestrator_Application
#         AIOrchestrator_Persistence
#     )
#     add_test(NAME SqliteComparisonRepositoryTests
#         COMMAND AIOrchestrator_SqliteComparisonRepositoryTests)
#
# A6. BackendRuntime additions (see BackendRuntime_delta.cpp):
#     - No new source file: all changes are inside BackendRuntime.cpp.
#     - Add #include for ComparisonService.hpp and SqliteComparisonRepository.hpp
#       as described in BackendRuntime_delta.cpp.
#
# =============================================================================
# SECTION B: UI_AI_Orchestrator/CMakeLists.txt additions
# =============================================================================
#
# B1. Add new Qt adapter sources to the AIOrchestrator_BackendQt target
#     (or equivalent UI adapter library):
#
#     target_sources(AIOrchestrator_BackendQt PRIVATE
#         backend/src/ComparisonProjectsModel.cpp
#         backend/src/ComparisonListModel.cpp
#         backend/src/ComparisonResultListModel.cpp
#         backend/src/ComparisonController.cpp
#         backend/src/BackendBridge.cpp          # updated file (adds comparison)
#     )
#
# B2. Add new Qt adapter test executable:
#
#     add_executable(AIOrchestrator_ComparisonQtAdapterTests
#         backend/tests/ComparisonQtAdapterTests.cpp
#     )
#     target_link_libraries(AIOrchestrator_ComparisonQtAdapterTests PRIVATE
#         AIOrchestrator_BackendQt
#         AIOrchestrator_Application
#         Qt6::Core
#         Qt6::Test
#     )
#     add_test(NAME ComparisonQtAdapterTests
#         COMMAND AIOrchestrator_ComparisonQtAdapterTests)
#
# B3. Add QML smoke test executable:
#
#     add_executable(AIOrchestrator_ComparisonQmlSmokeTest
#         tests/ComparisonQmlSmokeTest.cpp
#     )
#     target_link_libraries(AIOrchestrator_ComparisonQmlSmokeTest PRIVATE
#         AIOrchestrator_BackendQt
#         AIOrchestrator_Application
#         AIOrchestrator_Persistence
#         Qt6::Core
#         Qt6::Qml
#         Qt6::Quick
#     )
#     add_test(NAME ComparisonQmlSmokeTest
#         COMMAND AIOrchestrator_ComparisonQmlSmokeTest)
#
# B4. QML file — no CMake change needed if qml/ is already a qmldir/qt_add_qml_module
#     source. If ComparisonPage.qml is listed explicitly in qt_add_qml_module(SOURCES),
#     the updated ComparisonPage.qml replaces the existing entry in-place.

# =============================================================================
# END
# =============================================================================
