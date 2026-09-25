cmake_minimum_required(VERSION 3.20)

project(continuo
    VERSION 0.1.5
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

# Mobile/cross builds should not produce a host-style server unless requested.
set(CONTINUO_BUILD_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    set(CONTINUO_BUILD_EXAMPLES_DEFAULT ON)
endif()
option(CONTINUO_BUILD_EXAMPLES "Build Continuo examples" ${CONTINUO_BUILD_EXAMPLES_DEFAULT})

option(CONTINUO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(CONTINUO_ENABLE_TLS "Build the optional OpenSSL 3 TLS stream module" OFF)
option(CONTINUO_ENABLE_HTTP2 "Build the optional nghttp2 HTTP/2 module" OFF)
option(CONTINUO_ENABLE_HTTP3 "Build the optional QUIC and HTTP/3 modules (experimental)" OFF)

# Off by default when embedded: a project that pulls Continuo in via
# add_subdirectory() has not asked to install our headers alongside its own.
option(CONTINUO_INSTALL "Generate install and package-config rules"
       ${PROJECT_IS_TOP_LEVEL})

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

# ── Installation ────────────────────────────────────────────────────────────
#
# One export set for every module, so that a consumer gets a single
# `find_package(continuo)` rather than one per layer.
#
# Each module calls this rather than repeating four near-identical blocks,
# which is also what keeps a new module from being installed slightly
# differently to the others.

include(GNUInstallDirs)

function(continuo_install_module target)
    if(NOT CONTINUO_INSTALL)
        return()
    endif()

    set(export_set continuoTargets)
    if(target STREQUAL "continuo_tls")
        set(export_set continuoTlsTargets)
    elseif(target STREQUAL "continuo_http2")
        set(export_set continuoHttp2Targets)
    elseif(target STREQUAL "continuo_quic" OR target STREQUAL "continuo_http3")
        set(export_set continuoHttp3Targets)
    endif()
    install(TARGETS ${target}
        EXPORT ${export_set}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endfunction()

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

# Each module registers its own tests and install rules.
add_subdirectory(modules/core)
add_subdirectory(modules/transport)
add_subdirectory(modules/http)

if(CONTINUO_ENABLE_TLS)
    add_subdirectory(modules/tls)
endif()
if(CONTINUO_ENABLE_HTTP2)
    add_subdirectory(modules/http2)
endif()
if(CONTINUO_ENABLE_HTTP3)
    add_subdirectory(modules/quic)
    add_subdirectory(modules/http3)
endif()

# ── Examples ────────────────────────────────────────────────────────────────

if(CONTINUO_BUILD_EXAMPLES)
    add_executable(continuo_echo_server examples/echo_server.cpp)
    target_link_libraries(continuo_echo_server PRIVATE continuo::transport continuo::warnings)

    if(CONTINUO_BUILD_TESTS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME continuo_echo_example
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_echo_example.py
                    $<TARGET_FILE:continuo_echo_server>
            )
            set_tests_properties(continuo_echo_example PROPERTIES TIMEOUT 90)
        endif()
    endif()
endif()

# ── Package config ──────────────────────────────────────────────────────────
#
# Written after the modules so that the export set is complete.

if(CONTINUO_INSTALL)
    include(CMakePackageConfigHelpers)

    set(CONTINUO_CMAKE_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/continuo)

    install(EXPORT continuoTargets
        FILE continuoTargets.cmake
        NAMESPACE continuo::
        DESTINATION ${CONTINUO_CMAKE_DESTINATION}
    )

    foreach(optional_module IN ITEMS Tls Http2 Http3)
        if((optional_module STREQUAL "Tls" AND CONTINUO_ENABLE_TLS)
           OR (optional_module STREQUAL "Http2" AND CONTINUO_ENABLE_HTTP2)
           OR (optional_module STREQUAL "Http3" AND CONTINUO_ENABLE_HTTP3))
            install(EXPORT continuo${optional_module}Targets
                FILE continuo${optional_module}Targets.cmake
                NAMESPACE continuo::
                DESTINATION ${CONTINUO_CMAKE_DESTINATION}
            )
        endif()
    endforeach()

    if(CONTINUO_ENABLE_HTTP3)
        install(FILES cmake/continuo-http3-dependencies.cmake
            DESTINATION ${CONTINUO_CMAKE_DESTINATION})
    endif()

    configure_package_config_file(
        cmake/continuo-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/continuo-config.cmake
        INSTALL_DESTINATION ${CONTINUO_CMAKE_DESTINATION}
    )

    # SameMinorVersion, not SameMajorVersion: while the major version is 0 the
    # minor one is where breaking changes land, so 0.2 must not satisfy a
    # request for 0.3. See CHANGELOG.md for the policy this encodes.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/continuo-config-version.cmake
        COMPATIBILITY SameMinorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/continuo-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/continuo-config-version.cmake
        DESTINATION ${CONTINUO_CMAKE_DESTINATION}
    )
endif()
