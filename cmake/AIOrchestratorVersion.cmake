# The ONE source of the product version. Every project of this repository includes this file BEFORE its project()
# call (the workspace root, First_AI_Agents and UI_AI_Orchestrator, each also when configured standalone), so there
# is no second place to edit. It only sets variables and checks them; it can be included any number of times.
#
#   AIO_VERSION_CORE     1.0.0        numeric part: what project(VERSION ...) takes (CMake accepts no suffix there)
#   AIO_VERSION_FULL     1.0.0        what the user sees: Settings, QCoreApplication::applicationVersion(), the
#                                     Windows FileVersion/ProductVersion strings, and (with a "v") the Git tag
#   AIO_VERSION_RC       1,0,0,0      the numeric quadruple of the Windows FILEVERSION/PRODUCTVERSION
#   AIO_RC_FILEFLAGS     VS_FF_PRERELEASE, or 0 for a final release
#
# To release 1.0.0: empty AIO_VERSION_PRERELEASE. To start the next release: change the three numbers.
set(AIO_VERSION_MAJOR 1)
set(AIO_VERSION_MINOR 0)
set(AIO_VERSION_PATCH 0)
set(AIO_VERSION_PRERELEASE "")

set(AIO_VERSION_CORE "${AIO_VERSION_MAJOR}.${AIO_VERSION_MINOR}.${AIO_VERSION_PATCH}")
set(AIO_VERSION_RC "${AIO_VERSION_MAJOR},${AIO_VERSION_MINOR},${AIO_VERSION_PATCH},0")

if(AIO_VERSION_PRERELEASE STREQUAL "")
    set(AIO_VERSION_FULL "${AIO_VERSION_CORE}")
    set(AIO_RC_FILEFLAGS "0")
else()
    # The text lands in C++ string literals and in a Windows resource: keep it to what a semver pre-release allows.
    if(NOT AIO_VERSION_PRERELEASE MATCHES "^[0-9A-Za-z]+([.-][0-9A-Za-z]+)*$")
        message(FATAL_ERROR "AIO_VERSION_PRERELEASE '${AIO_VERSION_PRERELEASE}' must be dot/dash separated "
            "alphanumerics, e.g. rc.1")
    endif()
    set(AIO_VERSION_FULL "${AIO_VERSION_CORE}-${AIO_VERSION_PRERELEASE}")
    set(AIO_RC_FILEFLAGS "VS_FF_PRERELEASE")
endif()
