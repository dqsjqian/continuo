# continuo::core — event loop, buffers, coroutine plumbing, error model.
#
# A compiled library, not header-only. Header-only was considered and rejected:
# the reference point for this project ships 22k lines in one header, and the
# compile-time cost of that lands on every translation unit of every consumer.
# A compiled target also gives Continuo somewhere to put a real ABI policy.

add_library(continuo_core
    src/error.cpp
)

add_library(continuo::core ALIAS continuo_core)

# ── Platform backends ───────────────────────────────────────────────────────
#
# Each backend file also guards itself with the macros from platform.hpp, so
# compiling the wrong one yields an empty object rather than a broken build.
# Selecting here as well keeps the build output honest about what is compiled.

if(WIN32)
    target_sources(continuo_core PRIVATE src/event_loop_iocp.cpp)
    target_link_libraries(continuo_core PUBLIC ws2_32 mswsock)
    # Keep <windows.h> from defining min/max macros and the kitchen sink.
    target_compile_definitions(continuo_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    set(CONTINUO_IO_BACKEND "iocp")
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
    target_sources(continuo_core PRIVATE
        src/event_loop_posix.cpp
        src/poller_kqueue.cpp
    )
    set(CONTINUO_IO_BACKEND "kqueue")
elseif(UNIX)
    target_sources(continuo_core PRIVATE
        src/event_loop_posix.cpp
        src/poller_epoll.cpp
    )
    set(CONTINUO_IO_BACKEND "epoll")
else()
    message(FATAL_ERROR
        "continuo: no I/O backend for ${CMAKE_SYSTEM_NAME}. "
        "Add a readiness or completion backend before building here.")
endif()

message(STATUS "continuo: I/O backend = ${CONTINUO_IO_BACKEND} (${CMAKE_SYSTEM_NAME})")

target_include_directories(continuo_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Internal headers (poller.hpp, loop_common.hpp) are visible to this target
# only — implementation detail, not part of the installed surface.
target_include_directories(continuo_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(continuo_core PUBLIC cxx_std_20)
target_link_libraries(continuo_core PRIVATE continuo::warnings)

find_package(Threads REQUIRED)
target_link_libraries(continuo_core PUBLIC Threads::Threads)

set_target_properties(continuo_core PROPERTIES
    OUTPUT_NAME continuo-core
    VERSION ${PROJECT_VERSION}
)
