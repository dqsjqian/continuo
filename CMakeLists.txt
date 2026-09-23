cmake_minimum_required(VERSION 3.20)

project(continuo
    VERSION 0.1.0
    DESCRIPTION "Coroutine-native C++23 networking library — transport, protocols, and nothing else"
    LANGUAGES CXX
)

# ── Standard ────────────────────────────────────────────────────────────────
#
# C++23 is the baseline. Use std::expected directly rather than maintaining a
# second, subtly different error model for an older standard.
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard used to build Continuo")
if(CMAKE_CXX_STANDARD LESS 23)
    message(FATAL_ERROR "Continuo requires C++23 or newer; use -DCMAKE_CXX_STANDARD=23")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(CONTINUO_BUILD_TESTS "Build Continuo tests" ${PROJECT_IS_TOP_LEVEL})
option(CONTINUO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(CONTINUO_ENABLE_TLS "Build the optional OpenSSL 3 TLS stream module" OFF)

# ── Warning policy ──────────────────────────────────────────────────────────
#
# Applied through an interface target rather than global flags so that a
# consumer pulling Continuo in via add_subdirectory() does not inherit our
# warning policy for their own sources.

add_library(continuo_warnings INTERFACE)
add_library(continuo::warnings ALIAS continuo_warnings)

if(MSVC)
    target_compile_options(continuo_warnings INTERFACE /W4 /permissive-)
    if(CONTINUO_WARNINGS_AS_ERRORS)
        target_compile_options(continuo_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(continuo_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    )
    if(CONTINUO_WARNINGS_AS_ERRORS)
        target_compile_options(continuo_warnings INTERFACE -Werror)
    endif()
endif()

# ── Modules ─────────────────────────────────────────────────────────────────
#
# Dependency direction is one-way and enforced by tools/ci/check_layering.py:
#
#   http and tls consume core's AsyncStream seam; transport implements it.
#   Applications compose the modules, rather than http depending on sockets.
#
# TLS is optional: the core/plain TCP/HTTP build has no OpenSSL dependency.

if(CONTINUO_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(modules/core)
add_subdirectory(modules/transport)
add_subdirectory(modules/http)

if(CONTINUO_ENABLE_TLS)
    add_subdirectory(modules/tls)
endif()

if(CONTINUO_BUILD_TESTS)
    add_subdirectory(modules/core/tests)
endif()
