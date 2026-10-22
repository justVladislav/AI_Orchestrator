# Keeps assert() alive in the TEST executables of every configuration (Release defines NDEBUG, which would silently turn the
# checks of every test into no-ops). Included before project() by the workspace root, First_AI_Agents and UI_AI_Orchestrator:
# it schedules aio_keep_asserts_in_tests() to run when the top-level directory has been fully processed, so it sees the tests of
# all sub-projects whether this repository is configured as the workspace or as First_AI_Agents / UI_AI_Orchestrator alone.
#
# What it does, once per configure:
#  - finds every test registered with add_test() (all sub-directories) that is named like an executable TARGET of this build
#    (the convention of this repository; any other test is listed in the configure output);
#  - force-includes cmake/KeepAsserts.h into that target (it #undefs NDEBUG before any other header);
#  - adds the test AIOrchestrator_AssertCanary_tests, built the same way, which fails when assert() is compiled out;
#  - (Windows) runs every test with a SHORT TEMP and TMP (<SystemDrive>\aio-t), see the comment at shortTemp below.
# Product targets are untouched: they keep NDEBUG in Release.

if(DEFINED AIO_TEST_ASSERTS_INCLUDED)
    return()
endif()
set(AIO_TEST_ASSERTS_INCLUDED TRUE)
set(AIO_KEEP_ASSERTS_HEADER "${CMAKE_CURRENT_LIST_DIR}/KeepAsserts.h")
set(AIO_ASSERT_CANARY_SOURCE "${CMAKE_CURRENT_LIST_DIR}/AssertCanary.cpp")

function(aio_force_include_keep_asserts target)
    if(MSVC)
        target_compile_options(${target} PRIVATE "/FI${AIO_KEEP_ASSERTS_HEADER}")
    else()
        target_compile_options(${target} PRIVATE "SHELL:-include ${AIO_KEEP_ASSERTS_HEADER}")
    endif()
endfunction()

function(aio_keep_asserts_in_tests)
    if(NOT TARGET AIOrchestrator_AssertCanary_tests)
        add_executable(AIOrchestrator_AssertCanary_tests "${AIO_ASSERT_CANARY_SOURCE}")
        target_compile_features(AIOrchestrator_AssertCanary_tests PRIVATE cxx_std_20)
        add_test(NAME AIOrchestrator_AssertCanary_tests COMMAND AIOrchestrator_AssertCanary_tests)
    endif()

    # Every directory below the top-level one.
    set(directories "${CMAKE_SOURCE_DIR}")
    set(index 0)
    while(index LESS_EQUAL 100000)
        list(LENGTH directories count)
        if(index GREATER_EQUAL count)
            break()
        endif()
        list(GET directories ${index} directory)
        get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        list(APPEND directories ${children})
        math(EXPR index "${index} + 1")
    endwhile()

    # A SHORT TEMP/TMP for every test (Windows). The fixtures build their trees under temp_directory_path(), and the proposal
    # store's files sit 164 characters below its root (\runs\<uuid>\proposals\<uuid>\originals\<sha256>): a TEMP of the length of a
    # hosted runner's (C:\Users\runneradmin\AppData\Local\Temp, 8.3 form RUNNER~1) plus the fixture's own folder pushes them past the
    # 260-character MAX_PATH limit, which the executables do not opt out of, and the tests fail there only (found by the first hosted
    # CI run). If the folder cannot be created the tests keep the TEMP they are started with.
    set(shortTemp "")
    if(WIN32)
        if(DEFINED ENV{SystemDrive} AND NOT "$ENV{SystemDrive}" STREQUAL "")
            set(shortTempDir "$ENV{SystemDrive}/aio-t")
        else()
            set(shortTempDir "C:/aio-t")
        endif()
        file(MAKE_DIRECTORY "${shortTempDir}")
        if(IS_DIRECTORY "${shortTempDir}")
            file(TO_NATIVE_PATH "${shortTempDir}" shortTempNative)
            set(shortTemp "TEMP=${shortTempNative}" "TMP=${shortTempNative}")
        endif()
    endif()

    set(patched "")
    set(foreign "")
    foreach(directory IN LISTS directories)
        get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
        foreach(test IN LISTS tests)
            if(shortTemp)
                set_property(TEST ${test} DIRECTORY "${directory}" APPEND PROPERTY ENVIRONMENT ${shortTemp})
            endif()
            # CMake cannot read a test's COMMAND back, so the convention of this repository is used: a test is named
            # like the executable target it runs. A test that follows no such convention is listed below, never skipped silently.
            set(executable "${test}")
            if(TARGET ${executable})
                get_target_property(type ${executable} TYPE)
                get_target_property(imported ${executable} IMPORTED)
                if(type STREQUAL "EXECUTABLE" AND NOT imported)
                    list(FIND patched ${executable} known)
                    if(known EQUAL -1)
                        aio_force_include_keep_asserts(${executable})
                        list(APPEND patched ${executable})
                    endif()
                    continue()
                endif()
            endif()
            list(APPEND foreign "${test}")
        endforeach()
    endforeach()

    list(LENGTH patched patchedCount)
    message(STATUS "Tests keep assert() in ${CMAKE_BUILD_TYPE} builds: ${patchedCount} test executable(s) get cmake/KeepAsserts.h")
    if(shortTemp)
        message(STATUS "Tests run with a short TEMP/TMP: ${shortTemp}")
    endif()
    if(foreign)
        message(STATUS "Tests not named like an executable target of this build (NOT patched, check them): ${foreign}")
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL aio_keep_asserts_in_tests)
